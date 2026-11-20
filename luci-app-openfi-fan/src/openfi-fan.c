/*
 * openfi-fan —— OpenFi 6C 风扇温控守护进程
 *
 * 设计:决策全在 libopenfi 的 fan_state_* 里(那部分有 43 项单元测试覆盖),
 *       这个程序只做 I/O 粘合:读配置 → 读温度 → 问状态机 → 写 PWM。
 *
 * 用法:
 *   openfi-fan              守护进程(由 procd 拉起)
 *   openfi-fan --once       跑一轮就退出(调试 / CI 冒烟)
 *   openfi-fan --json       输出当前状态 JSON 给 LuCI
 *   openfi-fan --speed N    手动把转速设成 N(0..100),写完就退
 *   openfi-fan --selftest   上机自检:检查 PWM / 温度源 / 配置是否可用
 *
 * 关于 PWM(和原厂 op_fan.sh 一致):
 *   本板 25 kHz,反极性 —— duty_cycle 越小风扇越快,duty=0 是全速。
 *   两路都要写(pwm0 + pwm1),写同一个 duty。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "openfi_fanctl.h"
#include "openfi_json.h"
#include "openfi_log.h"
#include "openfi_switch.h"
#include "openfi_sysfs.h"
#include "openfi_temp.h"
#include "openfi_uci.h"

#define PWM_CHIP_DEFAULT "/sys/class/pwm/pwmchip0"
#define PERIOD_NS_DEFAULT 40000		/* 25 kHz */
#define STATE_FILE "/var/run/openfi-fan.json"
#define RUN_DIR "/var/run"

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_reload;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- 配置 ---------------- */

struct fan_run_cfg {
	struct fan_cfg curve;
	int period;
	int silent_max_temp;
	int silent_when_hi;
	char pwm_chip[96];
	int period_ns;
	int channels[4];
	int nchan;
};

static void cfg_load(struct fan_run_cfg *rc)
{
	int min_speed;

	/* UCI 里没有就用默认 —— 不能因为配置缺项就不转风扇 */
	min_speed = (int)ouci_get_int("openfi_fan", "fan", "min_speed", 10);
	fan_cfg_default(&rc->curve, min_speed);

	rc->curve.t1 = (int)ouci_get_int("openfi_fan", "fan", "temp1", rc->curve.t1);
	rc->curve.t2 = (int)ouci_get_int("openfi_fan", "fan", "temp2", rc->curve.t2);
	rc->curve.t3 = (int)ouci_get_int("openfi_fan", "fan", "temp3", rc->curve.t3);
	rc->curve.t4 = (int)ouci_get_int("openfi_fan", "fan", "temp4", rc->curve.t4);
	rc->curve.s1 = (int)ouci_get_int("openfi_fan", "fan", "speed1", rc->curve.s1);
	rc->curve.s2 = (int)ouci_get_int("openfi_fan", "fan", "speed2", rc->curve.s2);
	rc->curve.s3 = (int)ouci_get_int("openfi_fan", "fan", "speed3", rc->curve.s3);
	rc->curve.s4 = (int)ouci_get_int("openfi_fan", "fan", "speed4", rc->curve.s4);
	rc->curve.emergency_temp =
		(int)ouci_get_int("openfi_fan", "fan", "emergency_temp",
				  rc->curve.emergency_temp);

	rc->period = (int)ouci_get_int("openfi_fan", "fan", "period", 5);
	if (rc->period < 1)
		rc->period = 1;
	if (rc->period > 60)
		rc->period = 60;

	rc->silent_max_temp =
		(int)ouci_get_int("openfi_fan", "fan", "silent_max_temp", 75);
	/* 开关哪一档算静默:默认高电平档(与原厂 "high: led off" 的注释一致) */
	rc->silent_when_hi = ouci_get_bool("openfi_fan", "fan", "silent_when_hi", 1);

	const char *chip = ouci_get_def("openfi_fan", "fan", "pwm_chip", PWM_CHIP_DEFAULT);

	snprintf(rc->pwm_chip, sizeof(rc->pwm_chip), "%s", chip);
	rc->period_ns = (int)ouci_get_int("openfi_fan", "fan", "period_ns",
					  PERIOD_NS_DEFAULT);
	/* 两路是板级事实,不做成配置项 —— 配错了风扇就不转 */
	rc->channels[0] = 0;
	rc->channels[1] = 1;
	rc->nchan = 2;

	if (!fan_cfg_valid(&rc->curve)) {
		olog_warn("配置的温度曲线不合法,退回默认曲线");
		fan_cfg_default(&rc->curve, min_speed);
	}
}

/* ---------------- PWM ---------------- */

static int pwm_path(char *out, size_t n, const struct fan_run_cfg *rc,
		    int ch, const char *leaf)
{
	if (ch < 0)
		return snprintf(out, n, "%s/%s", rc->pwm_chip, leaf);
	return snprintf(out, n, "%s/pwm%d/%s", rc->pwm_chip, ch, leaf);
}

static int pwm_export(const struct fan_run_cfg *rc, int ch)
{
	char p[256], v[16];

	pwm_path(p, sizeof(p), rc, ch, "");
	/* 目录已经在 = 已经 export 过 */
	if (osys_exists(p))
		return 0;
	pwm_path(p, sizeof(p), rc, -1, "export");
	snprintf(v, sizeof(v), "%d", ch);
	return osys_write(p, v);
}

static int pwm_apply(const struct fan_run_cfg *rc, int speed)
{
	long duty = fan_duty_for(speed, rc->period_ns);
	char p[256], v[32];
	int failed = 0;

	for (int i = 0; i < rc->nchan; i++) {
		int ch = rc->channels[i];

		if (pwm_export(rc, ch) != 0) {
			olog_err("pwm%d export 失败", ch);
			failed = 1;
			continue;
		}
		/* 新 export 的通道 period=0,Linux 会拒绝一切 duty 写入 ——
		   所以必须先写 period */
		pwm_path(p, sizeof(p), rc, ch, "period");
		snprintf(v, sizeof(v), "%d", rc->period_ns);
		if (osys_write(p, v) != 0) {
			olog_err("pwm%d period 写入失败", ch);
			failed = 1;
			continue;
		}
		pwm_path(p, sizeof(p), rc, ch, "duty_cycle");
		snprintf(v, sizeof(v), "%ld", duty);
		if (osys_write(p, v) != 0) {
			olog_err("pwm%d duty 写入失败", ch);
			failed = 1;
			continue;
		}
		pwm_path(p, sizeof(p), rc, ch, "enable");
		if (osys_write(p, "1") != 0) {
			olog_err("pwm%d enable 失败", ch);
			failed = 1;
		}
	}
	return failed ? -1 : 0;
}

/* ---------------- 状态输出(给 LuCI) ---------------- */

static void write_state(const struct fan_run_cfg *rc, const struct fan_state *st,
			const struct temp_cfg *tc, const int temps[3], int unified,
			int applied)
{
	struct ojson *o = oj_new();
	char buf[32];

	oj_bool(o, "ok", 1);
	oj_int(o, "output", applied);		/* 实际下发成功的转速 */
	oj_int(o, "target", st->last_applied);	/* 状态机算出来的目标转速 */
	oj_str(o, "reason", fan_reason_str(st->last_reason));
	oj_bool(o, "silent", st->silent);
	oj_str(o, "switch", sw_level_str(sw_read_level(NULL, "func")));
	if (unified == TEMP_INVALID)
		oj_null(o, "temp");
	else
		oj_int(o, "temp", unified);

#define SRC(k, i) do {						\
	if (temps[i] == TEMP_INVALID) oj_null(o, k);		\
	else oj_int(o, k, temps[i]);				\
} while (0)
	SRC("cpu", TEMP_CPU);
	SRC("wifi", TEMP_WIFI);
	SRC("modem", TEMP_MODEM);
#undef SRC

	oj_int(o, "period", rc->period);
	oj_int(o, "t1", rc->curve.t1); oj_int(o, "t2", rc->curve.t2);
	oj_int(o, "t3", rc->curve.t3); oj_int(o, "t4", rc->curve.t4);
	oj_int(o, "s1", rc->curve.s1); oj_int(o, "s2", rc->curve.s2);
	oj_int(o, "s3", rc->curve.s3); oj_int(o, "s4", rc->curve.s4);
	oj_int(o, "min_speed", rc->curve.min_speed);
	oj_int(o, "emergency_temp", rc->curve.emergency_temp);
	oj_int(o, "silent_max_temp", rc->silent_max_temp);
	snprintf(buf, sizeof(buf), "%s", tc->wifi_iface);
	oj_str(o, "wifi_iface", buf);

	/*
	 * 写文件 + 同时打到 stdout(前者给 LuCI 轮询,后者给 --json / 调试)。
	 * 【注意】这里的路径必须走 osys_path —— 否则 root 注入只对"读"生效、
	 * "写"还是落在写死的绝对路径上,本地冒烟测试就会出现"写得进去读不出来"。
	 */
	/*
	 * 先渲染成字符串,自己写文件,再打到 stdout。
	 * 这样一份数据两处用,不用为了写文件再构造一次对象。
	 */
	char *js = oj_render(o);

	if (js) {
		char tmp[256], fin[256];
		FILE *f;

		osys_path(tmp, sizeof(tmp), STATE_FILE ".tmp");
		osys_path(fin, sizeof(fin), STATE_FILE);
		f = fopen(tmp, "w");
		if (f) {
			fprintf(f, "%s\n", js);
			if (fclose(f) == 0)
				rename(tmp, fin);  /* 原子替换,LuCI 不会读到半截 JSON */
		}
		puts(js);
		free(js);
	}
	oj_free(o);
}

/*
 * 读整个文件(状态 JSON 有几百字节,不能用 osys_read ——
 * 那个是按"读 sysfs 的一个值"设计的,fgets 到 256 字节就截断了)。
 * 返回读到的字节数,失败返回 -1。
 */
static int read_whole_file(const char *path, char *out, size_t outlen)
{
	char full[256];
	FILE *f;
	size_t n;

	if (!path || !out || outlen < 2)
		return -1;
	osys_path(full, sizeof(full), path);
	f = fopen(full, "r");
	if (!f)
		return -1;
	n = fread(out, 1, outlen - 1, f);
	fclose(f);
	out[n] = '\0';
	return (int)n;
}

/* ---------------- 自检 ---------------- */

static int selftest(const struct fan_run_cfg *rc, const struct temp_cfg *tc)
{
	int bad = 0;

	printf("openfi-fan 自检\n");

	printf("  配置: 曲线 %d/%d/%d/%d → %d/%d/%d/%d  下限%d  紧急%d  周期%ds\n",
	       rc->curve.t1, rc->curve.t2, rc->curve.t3, rc->curve.t4,
	       rc->curve.s1, rc->curve.s2, rc->curve.s3, rc->curve.s4,
	       rc->curve.min_speed, rc->curve.emergency_temp, rc->period);
	printf("  配置曲线合法: %s\n", fan_cfg_valid(&rc->curve) ? "是" : "否(会退回默认)");

	printf("  PWM: %s  %d 路 (", rc->pwm_chip, rc->nchan);
	for (int i = 0; i < rc->nchan; i++)
		printf("%s%d", i ? "," : "", rc->channels[i]);
	printf(")  周期 %d ns\n", rc->period_ns);
	if (!osys_exists(rc->pwm_chip)) {
		printf("    ❌ %s 不存在 —— PWM 控制器没起来(pwm-fan 有没有抢?)\n",
		       rc->pwm_chip);
		bad++;
	}

	printf("  温度源:\n");
	for (int i = 0; i < TEMP_SRC_COUNT; i++) {
		static const char *nm[] = { "cpu", "wifi", "modem" };
		int t = temp_read((enum temp_src)i, tc);

		if (t == TEMP_INVALID)
			printf("    %-6s ❌ 读不到\n", nm[i]);
		else
			printf("    %-6s ✅ %d ℃\n", nm[i], t);
	}
	{
		int t3[3], u;

		if (temp_read_all(tc, t3, &u) == 0) {
			printf("  ❌ 三路温度全读不到 —— 曲线会一直用兜底值\n");
			bad++;
		} else {
			printf("  统一温度(取最高): %d ℃\n", u);
		}
	}
	printf("\n  %s\n", bad ? "自检发现问题(见上)" : "自检通过");
	return bad ? 1 : 0;
}

/* ---------------- ubus 服务 ---------------- */

static struct fan_run_cfg g_rc;
static struct temp_cfg g_tc;
static struct fan_state g_st;
static int g_applied = -1;
static struct ubus_context *g_ubus;

struct fan_snap {
	int output;
	int target;
	int unified;
	int temps[TEMP_SRC_COUNT];
	int silent;
	int ok;
	char reason[32];
};
static struct fan_snap g_snap = { .output = -1, .target = -1, .ok = 1, .unified = TEMP_INVALID };

static const char *temp_name(int i)
{
	static const char *nm[TEMP_SRC_COUNT] = { "cpu", "wifi", "modem" };

	return (i >= 0 && i < TEMP_SRC_COUNT) ? nm[i] : "?";
}

static void snap_refresh(void)
{
	g_snap.output = g_applied;
	g_snap.target = g_st.last_applied;
	g_snap.silent = g_st.silent;
	snprintf(g_snap.reason, sizeof(g_snap.reason), "%s",
		 fan_reason_str(g_st.last_reason));
}

static void snap_fill(struct blob_buf *b, int with_config)
{
	blobmsg_add_u32(b, "output", g_snap.output < 0 ? 0 : g_snap.output);
	blobmsg_add_u32(b, "target", g_snap.target < 0 ? 0 : g_snap.target);
	blobmsg_add_string(b, "reason", g_snap.reason);
	blobmsg_add_u8(b, "silent", g_snap.silent ? 1 : 0);
	blobmsg_add_u8(b, "ok", g_snap.ok ? 1 : 0);
	blobmsg_add_u8(b, "manual", !strcmp(ouci_get_def("openfi_fan", "fan", "mode", "auto"), "manual"));
	if (g_snap.unified != TEMP_INVALID)
		blobmsg_add_u32(b, "temp", g_snap.unified);
	for (int i = 0; i < TEMP_SRC_COUNT; i++)
		if (g_snap.temps[i] != TEMP_INVALID)
			blobmsg_add_u32(b, temp_name(i), g_snap.temps[i]);

	if (!with_config)
		return;
	blobmsg_add_u32(b, "period", g_rc.period);
	blobmsg_add_u32(b, "min_speed", g_rc.curve.min_speed);
	blobmsg_add_u32(b, "emergency_temp", g_rc.curve.emergency_temp);
	blobmsg_add_u32(b, "silent_max_temp", g_rc.silent_max_temp);
	blobmsg_add_u32(b, "temp1", g_rc.curve.t1);
	blobmsg_add_u32(b, "temp2", g_rc.curve.t2);
	blobmsg_add_u32(b, "temp3", g_rc.curve.t3);
	blobmsg_add_u32(b, "temp4", g_rc.curve.t4);
	blobmsg_add_u32(b, "speed1", g_rc.curve.s1);
	blobmsg_add_u32(b, "speed2", g_rc.curve.s2);
	blobmsg_add_u32(b, "speed3", g_rc.curve.s3);
	blobmsg_add_u32(b, "speed4", g_rc.curve.s4);
	blobmsg_add_u32(b, "manual_speed",
			(int)ouci_get_int("openfi_fan", "fan", "manual_speed", 50));
}

static int fan_ubus_status(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_buf b = {};

	(void)obj; (void)method; (void)msg;
	blob_buf_init(&b, 0);
	snap_fill(&b, 0);
	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	return 0;
}

static int fan_ubus_get_config(struct ubus_context *ctx, struct ubus_object *obj,
			       struct ubus_request_data *req, const char *method,
			       struct blob_attr *msg)
{
	struct blob_buf b = {};

	(void)obj; (void)method; (void)msg;
	blob_buf_init(&b, 0);
	snap_fill(&b, 1);
	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	return 0;
}

enum {
	CF_T1, CF_T2, CF_T3, CF_T4,
	CF_S1, CF_S2, CF_S3, CF_S4,
	CF_PERIOD, CF_MIN_SPEED, CF_EMERGENCY, CF_SILENT_MAX,
	CF_MODE, CF_MANUAL_SPEED,
	__CF_MAX
};

static const struct blobmsg_policy cfg_policy[__CF_MAX] = {
	[CF_T1] = { .name = "temp1", .type = BLOBMSG_TYPE_INT32 },
	[CF_T2] = { .name = "temp2", .type = BLOBMSG_TYPE_INT32 },
	[CF_T3] = { .name = "temp3", .type = BLOBMSG_TYPE_INT32 },
	[CF_T4] = { .name = "temp4", .type = BLOBMSG_TYPE_INT32 },
	[CF_S1] = { .name = "speed1", .type = BLOBMSG_TYPE_INT32 },
	[CF_S2] = { .name = "speed2", .type = BLOBMSG_TYPE_INT32 },
	[CF_S3] = { .name = "speed3", .type = BLOBMSG_TYPE_INT32 },
	[CF_S4] = { .name = "speed4", .type = BLOBMSG_TYPE_INT32 },
	[CF_PERIOD] = { .name = "period", .type = BLOBMSG_TYPE_INT32 },
	[CF_MIN_SPEED] = { .name = "min_speed", .type = BLOBMSG_TYPE_INT32 },
	[CF_EMERGENCY] = { .name = "emergency_temp", .type = BLOBMSG_TYPE_INT32 },
	[CF_SILENT_MAX] = { .name = "silent_max_temp", .type = BLOBMSG_TYPE_INT32 },
	[CF_MODE] = { .name = "mode", .type = BLOBMSG_TYPE_STRING },
	[CF_MANUAL_SPEED] = { .name = "manual_speed", .type = BLOBMSG_TYPE_INT32 },
};

static int fan_ubus_set_config(struct ubus_context *ctx, struct ubus_object *obj,
			       struct ubus_request_data *req, const char *method,
			       struct blob_attr *msg)
{
	struct blob_attr *tb[__CF_MAX];
	char v[32];
	int err = 0;

	(void)obj; (void)method;
	if (!msg)
		return UBUS_STATUS_INVALID_ARGUMENT;
	blobmsg_parse(cfg_policy, __CF_MAX, tb, blob_data(msg), blob_len(msg));

	if (tb[CF_MODE])
		err |= ouci_set("openfi_fan", "fan", "mode",
				blobmsg_get_string(tb[CF_MODE]));

#define SET_I(id, key) do {						\
	if (tb[id]) {							\
		snprintf(v, sizeof(v), "%d", (int)blobmsg_get_u32(tb[id])); \
		err |= ouci_set("openfi_fan", "fan", key, v);		\
	}								\
} while (0)
	SET_I(CF_T1, "temp1");
	SET_I(CF_T2, "temp2");
	SET_I(CF_T3, "temp3");
	SET_I(CF_T4, "temp4");
	SET_I(CF_S1, "speed1");
	SET_I(CF_S2, "speed2");
	SET_I(CF_S3, "speed3");
	SET_I(CF_S4, "speed4");
	SET_I(CF_PERIOD, "period");
	SET_I(CF_MIN_SPEED, "min_speed");
	SET_I(CF_EMERGENCY, "emergency_temp");
	SET_I(CF_SILENT_MAX, "silent_max_temp");
	SET_I(CF_MANUAL_SPEED, "manual_speed");
#undef SET_I

	if (err) {
		olog_warn("ubus set_config 部分写入失败");
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	/* 下一轮立刻重读配置并生效，不用重启服务 */
	g_reload = 1;
	if (g_sample.cb)
		uloop_timeout_set(&g_sample, 0);
	ubus_send_reply(ctx, req, NULL);
	return 0;
}

enum { SL_ENABLE };
static const struct blobmsg_policy silent_policy[1] = {
	[SL_ENABLE] = { .name = "enable", .type = BLOBMSG_TYPE_INT32 },
};

static int fan_ubus_silent(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_attr *tb[1];
	struct blob_buf b = {};

	(void)obj; (void)method;
	blobmsg_parse(silent_policy, 1, tb, msg ? blob_data(msg) : NULL,
		      msg ? blob_len(msg) : 0);
	if (tb[SL_ENABLE]) {
		int on = (int)blobmsg_get_u32(tb[SL_ENABLE]);

		fan_state_set_silent(&g_st, on ? 1 : 0);
		g_snap.silent = on ? 1 : 0;
		g_applied = -1;		/* 强制下一轮重新下发 PWM */
		if (g_sample.cb)
			uloop_timeout_set(&g_sample, 0);
	}
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "silent", g_snap.silent ? 1 : 0);
	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	return 0;
}

static const struct ubus_method g_fan_methods[] = {
	UBUS_METHOD_NOARG("status", fan_ubus_status),
	UBUS_METHOD_NOARG("get_config", fan_ubus_get_config),
	UBUS_METHOD("set_config", fan_ubus_set_config, cfg_policy),
	UBUS_METHOD("silent", fan_ubus_silent, silent_policy),
};

static struct ubus_object_type g_fan_obj_type =
	UBUS_OBJECT_TYPE("openfi-fan", g_fan_methods);

static struct ubus_object g_fan_obj = {
	.name = "openfi.fan",
	.type = &g_fan_obj_type,
	.methods = g_fan_methods,
	.n_methods = ARRAY_SIZE(g_fan_methods),
};

/* ---------------- 采样 / 主循环 ---------------- */

static void fan_iteration(void)
{
	int got = temp_read_all(&g_tc, g_snap.temps, &g_snap.unified);
	int sl = sw_is_silent(sw_read_level(NULL, "func"), g_rc.silent_when_hi);
	const char *mode = ouci_get_def("openfi_fan", "fan", "mode", "auto");
	int want = -1;

	/* 物理开关每轮都看：拨动后最多一个周期生效 */
	if (sl >= 0)
		fan_state_set_silent(&g_st, sl);

	if (!strcmp(mode, "manual")) {
		int ms = (int)ouci_get_int("openfi_fan", "fan", "manual_speed", 50);

		if (ms < 0) ms = 0;
		if (ms > 100) ms = 100;
		want = ms;
		/* 安全优先：静默档停机；过热强制满速 */
		if (g_st.silent)
			want = 0;
		if (g_snap.unified != TEMP_INVALID &&
		    g_snap.unified >= g_rc.curve.emergency_temp)
			want = 100;
	} else {
		want = fan_state_step(&g_st, g_snap.unified, now_ms());
	}

	if (want >= 0 && want != g_applied) {
		if (pwm_apply(&g_rc, want) != 0) {
			olog_err("PWM 下发失败(转速 %d)", want);
			g_snap.ok = 0;
		} else {
			g_applied = want;
			g_snap.ok = 1;
			olog_info("转速 → %d%%（%s，温度 %d℃，读到 %d 路）",
				  want, fan_reason_str(g_st.last_reason),
				  g_snap.unified == TEMP_INVALID ? -1 : g_snap.unified,
				  got);
		}
	}

	snap_refresh();
	write_state(&g_rc, &g_st, &g_tc, g_snap.temps, g_snap.unified, g_applied);
}

static struct uloop_timeout g_sample;

static void fan_tick(struct uloop_timeout *t)
{
	if (g_stop) {
		uloop_end();
		return;
	}
	if (g_reload) {
		g_reload = 0;
		olog_info("重读配置");
		cfg_load(&g_rc);
		fan_state_init(&g_st, &g_rc.curve, g_rc.silent_max_temp);
		g_applied = -1;
	}
	fan_iteration();
	uloop_timeout_set(t, g_rc.period * 1000);
}

static struct uloop_signal g_sig_term = { .signo = SIGTERM };
static struct uloop_signal g_sig_int  = { .signo = SIGINT };
static struct uloop_signal g_sig_hup  = { .signo = SIGHUP };

static void on_uloop_signal(struct uloop_signal *s)
{
	if (s->signo == SIGHUP)
		g_reload = 1;
	else
		g_stop = 1;
	if (g_sample.cb)
		uloop_timeout_set(&g_sample, 0);
}

static int ubus_start(void)
{
	g_ubus = ubus_connect(NULL);
	if (!g_ubus) {
		olog_warn("ubus 连接失败，状态服务不可用（--status 仍可用）");
		return -1;
	}
	ubus_add_uloop(g_ubus);
	if (ubus_add_object(g_ubus, &g_fan_obj) != 0) {
		olog_warn("注册 ubus 对象 openfi.fan 失败");
		ubus_free(g_ubus);
		g_ubus = NULL;
		return -1;
	}
	olog_info("ubus 对象 openfi.fan 已注册");
	return 0;
}

/* ---------------- main ---------------- */

static void usage(void)
{
	fprintf(stderr,
		"用法: openfi-fan [选项]\n"
		"  --status      输出上次的状态(只读,零副作用 —— 给 LuCI 轮询用)\n"
		"  --once        跑一轮就退出\n"
		"  --json        输出状态 JSON\n"
		"  --speed N     手动设转速 0..100\n"
		"  --speed auto  回到温度曲线\n"
		"  --selftest    上机自检\n"
		"  -h, --help    本帮助\n");
}

int main(int argc, char **argv)
{
	static struct option opts[] = {
		{ "once",     no_argument,       NULL, '1' },
		{ "status",   no_argument,       NULL, 'S' },
		{ "json",     no_argument,       NULL, 'j' },
		{ "speed",    required_argument, NULL, 's' },
		{ "selftest", no_argument,       NULL, 't' },
		{ "help",     no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int opt_once = 0, opt_json = 0, opt_selftest = 0, opt_speed = -1;
	int opt_speed_auto = 0;
	int opt_status = 0;
	int rc_code = 0;

	while (1) {
		int c = getopt_long(argc, argv, "h", opts, NULL);

		if (c == -1)
			break;
		switch (c) {
		case '1': opt_once = 1; break;
		case 'S': opt_status = 1; opt_once = 1; break;
		case 'j': opt_json = 1; opt_once = 1; break;
		case 't': opt_selftest = 1; opt_once = 1; break;
		case 's':
			if (!strcmp(optarg, "auto")) {
				opt_speed_auto = 1;
			} else {
				opt_speed = atoi(optarg);
				if (opt_speed < 0) opt_speed = 0;
				if (opt_speed > 100) opt_speed = 100;
			}
			opt_once = 1;
			break;
		case 'h': default: usage(); return (c == 'h') ? 0 : 1;
		}
	}

	olog_init("openfi-fan");
	{
		const char *r = getenv("OPENFI_SYSFS_ROOT");

		if (r && *r)
			osys_set_root(r);
		r = getenv("OPENFI_UCI_ROOT");
		if (r && *r)
			ouci_set_root(r);
	}
	cfg_load(&g_rc);
	temp_cfg_default(&g_tc);
	{
		char tmp[sizeof(g_tc.wifi_iface)];
		const char *p = ouci_get_def("openfi_fan", "fan", "wifi_iface", "ra0");

		snprintf(tmp, sizeof(tmp), "%s", p);
		memcpy(g_tc.wifi_iface, tmp, sizeof(tmp));
		g_tc.wifi_enabled = ouci_get_bool("openfi_fan", "fan", "wifi_temp", 1);
	}

	/* --status：只读上次的状态文件，不读温度、不碰 PWM */
	if (opt_status) {
		char *buf = malloc(4096);

		if (buf && read_whole_file(STATE_FILE, buf, 4096) > 0) {
			fputs(buf, stdout);
			fputc('\n', stdout);
			free(buf);
			return 0;
		}
		free(buf);
		return oj_fail("还没有状态文件(守护进程没起来?)");
	}

	if (opt_selftest)
		return selftest(&g_rc, &g_tc);

	(void)opt_json;	/* --json 走 write_state() 的 stdout 输出 */

	if (fan_state_init(&g_st, &g_rc.curve, g_rc.silent_max_temp) != 0) {
		olog_warn("状态机初始化失败(配置非法),用默认曲线重来");
		fan_cfg_default(&g_rc.curve, 10);
		if (fan_state_init(&g_st, &g_rc.curve, g_rc.silent_max_temp) != 0) {
			olog_err("还是失败,退出");
			return 1;
		}
	}
	{
		int sl = sw_is_silent(sw_read_level(NULL, "func"), g_rc.silent_when_hi);

		if (sl >= 0)
			g_st.silent = sl;
		olog_info("启动时的开关状态:%s", g_st.silent ? "静默档" : "正常档");
	}

	if (opt_speed_auto) {
		if (ouci_set("openfi_fan", "fan", "mode", "auto") != 0)
			return oj_fail("写配置失败");
		olog_info("已回到温度曲线");
		return 0;
	}

	if (opt_speed >= 0) {
		char buf[16];

		if (pwm_apply(&g_rc, opt_speed) != 0)
			return oj_fail("PWM 写入失败");
		snprintf(buf, sizeof(buf), "%d", opt_speed);
		if (ouci_set("openfi_fan", "fan", "mode", "manual") != 0 ||
		    ouci_set("openfi_fan", "fan", "manual_speed", buf) != 0)
			olog_warn("转速已生效,但写配置失败 —— 重启后会回到温度曲线");
		else
			olog_info("已固定转速 %d%%(改回请用 --speed auto)", opt_speed);
		return 0;
	}

	if (opt_once) {
		fan_iteration();
		return rc_code;
	}

	/* 守护进程：uloop + ubus */
	uloop_init();
	if (ubus_start() != 0)
		olog_warn("继续以无 ubus 模式运行");
	uloop_signal_add(&g_sig_term, on_uloop_signal);
	uloop_signal_add(&g_sig_int, on_uloop_signal);
	uloop_signal_add(&g_sig_hup, on_uloop_signal);

	olog_info("启动:曲线 %d/%d/%d/%d → %d/%d/%d/%d,周期 %ds,静默=%d",
		  g_rc.curve.t1, g_rc.curve.t2, g_rc.curve.t3, g_rc.curve.t4,
		  g_rc.curve.s1, g_rc.curve.s2, g_rc.curve.s3, g_rc.curve.s4,
		  g_rc.period, g_st.silent);

	g_sample.cb = fan_tick;
	uloop_timeout_set(&g_sample, 10);
	uloop_run();

	/* 退出前把风扇开到全速 */
	olog_info("退出:把风扇开到全速以保证散热");
	pwm_apply(&g_rc, 100);
	if (g_ubus) {
		ubus_free(g_ubus);
		g_ubus = NULL;
	}
	if (g_snap.ok == 0 && rc_code == 0)
		rc_code = 0;
	return rc_code;
}
