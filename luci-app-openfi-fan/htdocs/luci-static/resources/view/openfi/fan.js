'use strict';
'require view';
'require form';
'require poll';
'require uci';
'require rpc';
'require fs';

var BIN = '/usr/sbin/openfi-fan';

var callStatus = rpc.declare({
	object: 'openfi.fan',
	method: 'status'
});

var CSS = [
	'.openfi-fan{max-width:1100px}',
	'.openfi-fan-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:.7rem;margin-bottom:1rem}',
	'@media (max-width:992px){.openfi-fan-grid{grid-template-columns:repeat(2,minmax(0,1fr))}}',
	'@media (max-width:600px){.openfi-fan-grid{grid-template-columns:1fr}}',
	'.openfi-fan-card{border:1px solid var(--border-color,#d4d4d4);border-radius:.4rem;padding:.65rem .85rem;background:var(--background-color,#fff)}',
	'.openfi-fan-card-label{font-size:.8rem;opacity:.65;margin-bottom:.15rem}',
	'.openfi-fan-card-value{font-size:1.45rem;font-weight:600;line-height:1.2}',
	'.openfi-fan-card-sub{font-size:.75rem;opacity:.6}',
	'.openfi-fan-main{display:grid;grid-template-columns:1.15fr 1fr;gap:1rem;align-items:start}',
	'@media (max-width:992px){.openfi-fan-main{grid-template-columns:1fr}}',
	'.openfi-fan-chart{border:1px solid var(--border-color,#d4d4d4);border-radius:.4rem;padding:.6rem;background:var(--background-color,#fff)}',
	'.openfi-fan-chart svg{display:block;width:100%;height:auto}',
	'.openfi-fan-chart text{fill:var(--text-color,#333);font-size:11px}',
	'.openfi-fan-note{font-size:.78rem;opacity:.65;margin-top:.4rem}'
].join('');

function fmtTemp(v) { return (v === null || v === undefined) ? '—' : v + ' °C'; }
function fmtPct(v) { return (v === null || v === undefined) ? '—' : v + '%'; }

function readStatus() {
	return callStatus().catch(function () {
		/* ubus 不可用时退回 --status（只读状态文件，零副作用） */
		return fs.exec(BIN, [ '--status' ]).then(function (r) {
			try { return JSON.parse(r.stdout); } catch (e) { return null; }
		});
	}).catch(function () { return null; });
}

function card(label, sub) {
	var value = E('div', { 'class': 'openfi-fan-card-value' }, '—');

	return {
		node: E('div', { 'class': 'openfi-fan-card' }, [
			E('div', { 'class': 'openfi-fan-card-label' }, label),
			value,
			E('div', { 'class': 'openfi-fan-card-sub' }, sub || '')
		]),
		set: function (text) { value.textContent = text; }
	};
}

function curveSvg(temps, speeds, current) {
	var W = 340, H = 150, PAD = 28;
	var t0 = 40, t1 = 90;
	var x = function (t) {
		return PAD + (Math.min(t1, Math.max(t0, t)) - t0) * (W - 2 * PAD) / (t1 - t0);
	};
	var y = function (s) { return H - PAD - s * (H - 2 * PAD) / 100; };
	var pts = [];

	for (var i = 0; i < 4; i++)
		pts.push(x(temps[i]) + ',' + y(speeds[i]));

	var kids = [
		E('line', { x1: PAD, y1: y(0), x2: W - PAD, y2: y(0), stroke: 'currentColor', 'stroke-opacity': '.25' }),
		E('line', { x1: PAD, y1: y(100), x2: W - PAD, y2: y(100), stroke: 'currentColor', 'stroke-opacity': '.25' }),
		E('polyline', { points: pts.join(' '), fill: 'none', stroke: 'var(--primary,#2f7fd8)', 'stroke-width': 2 })
	];

	for (var j = 0; j < 4; j++) {
		kids.push(E('circle', { cx: x(temps[j]), cy: y(speeds[j]), r: 3.5, fill: 'var(--primary,#2f7fd8)' }));
		kids.push(E('text', { x: x(temps[j]), y: H - PAD + 15, 'text-anchor': 'middle' }, temps[j] + '°'));
	}

	if (current !== null && current !== undefined) {
		kids.push(E('line', {
			x1: x(current), y1: PAD, x2: x(current), y2: H - PAD,
			stroke: 'var(--danger-color,#d33)', 'stroke-width': 1.5, 'stroke-dasharray': '4 3'
		}));
		kids.push(E('text', { x: x(current), y: PAD - 8, 'text-anchor': 'middle' }, current + '°'));
	}

	return E('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': _('温度-转速曲线') }, kids);
}

return view.extend({
	load: function () {
		return uci.load('openfi_fan');
	},

	render: function () {
		var cpu = card(_('CPU'), _('温度'));
		var wifi = card(_('WiFi'), _('温度'));
		var modem = card(_('5G'), _('温度'));
		var fan = card(_('风扇'), _('占空比'));

		var chartBox = E('div', { 'class': 'openfi-fan-chart' });

		var m = new form.Map('openfi_fan', _('风扇温控'),
			_('按温度曲线自动调节转速；物理开关拨到静默档会停转并关灯。'));

		var secCurve = m.section(form.NamedSection, 'fan', 'fan', _('温度曲线'));
		secCurve.anonymous = true;

		var ids = [];
		var labels = [ '一档', '二档', '三档', '四档' ];

		for (var i = 0; i < 4; i++) {
			var t = secCurve.option(form.Value, 'temp' + (i + 1), labels[i] + ' ' + _('温度'));
			t.datatype = 'range(20,100)';
			t.rmempty = false;

			var s = secCurve.option(form.Value, 'speed' + (i + 1), labels[i] + ' ' + _('转速'));
			s.datatype = 'range(0,100)';
			s.rmempty = false;
		}

		var secAdv = m.section(form.NamedSection, 'fan', 'fan', _('高级'));

		secAdv.anonymous = true;

		var mode = secAdv.option(form.ListValue, 'mode', _('模式'));
		mode.value('auto', _('按曲线'));
		mode.value('manual', _('固定转速'));

		var ms = secAdv.option(form.Value, 'manual_speed', _('固定转速值'));
		ms.datatype = 'range(0,100)';
		ms.depends('mode', 'manual');

		var period = secAdv.option(form.Value, 'period', _('采样周期'), _('秒'));
		period.datatype = 'range(1,60)';

		var minSpeed = secAdv.option(form.Value, 'min_speed', _('转速下限'), _('%'));
		minSpeed.datatype = 'range(0,100)';

		var emergency = secAdv.option(form.Value, 'emergency_temp', _('紧急温度'), _('°C，超过即满速'));
		emergency.datatype = 'range(60,110)';

		var silentMax = secAdv.option(form.Value, 'silent_max_temp', _('静默档过热兜底'), _('°C，超过即满速'));
		silentMax.datatype = 'range(50,100)';

		function formValue(name) {
			var el = document.getElementById('cbid.openfi_fan.fan.' + name);

			return el && el.value !== '' ? parseInt(el.value) : null;
		}

		function draw(current) {
			var temps = [], speeds = [];

			for (var k = 0; k < 4; k++) {
				temps.push(formValue('temp' + (k + 1)) || parseInt(uci.get('openfi_fan', 'fan', 'temp' + (k + 1)) || 60));
				speeds.push(formValue('speed' + (k + 1)) != null
					? formValue('speed' + (k + 1))
					: parseInt(uci.get('openfi_fan', 'fan', 'speed' + (k + 1)) || 50));
			}

			chartBox.replaceChildren(curveSvg(temps, speeds, current));
		}

		function refresh() {
			return readStatus().then(function (st) {
				cpu.set(fmtTemp(st && st.cpu));
				wifi.set(fmtTemp(st && st.wifi));
				modem.set(fmtTemp(st && st.modem));
				fan.set(fmtPct(st && st.output) + (st && st.silent ? ' ' + _('(静默)') : ''));

				return st ? st.temp : null;
			});
		}

		return m.render().then(function (formEl) {
			formEl.addEventListener('input', function () { draw(null); });
			formEl.addEventListener('change', function () { draw(null); });

			draw(null);

			refresh().then(function (cur) { draw(cur); });

			poll.add(function () { return refresh().then(function (cur) { draw(cur); }); }, 2);

			return E('div', { 'class': 'openfi-fan' }, [
				E('style', {}, CSS),
				E('h2', {}, _('风扇温控')),
				E('div', { 'class': 'openfi-fan-grid', 'aria-live': 'polite' },
					[ cpu.node, wifi.node, modem.node, fan.node ]),
				E('div', { 'class': 'openfi-fan-main' }, [
					E('div', {}, [
						chartBox,
						E('div', { 'class': 'openfi-fan-note' },
							_('横轴为温度（40~90 °C），纵轴为转速（0~100%）；红色虚线是当前温度。'))
					]),
					formEl
				])
			]);
		});
	}
});
