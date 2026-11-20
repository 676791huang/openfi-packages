# openfi-packages

OpenFi 6C 的树内包（OpenWrt feed 布局），由固件仓库
[`676791huang/immortalwrt-mt798x-rebase`](https://github.com/676791huang/immortalwrt-mt798x-rebase)
通过 `feeds.conf.default` 拉取：

```
src-git openfi https://github.com/676791huang/openfi-packages.git;main
```

| 包 | 说明 |
|---|---|
| `luci-app-openfi-fan` | 风扇温控：C 守护进程 + LuCI 页面 + 物理开关联动 |
| `openfi-lib` | 上面这个包的公共库（静态库） |

## 改完会怎样

- 固件仓库每天/每次构建时 `feeds update` 拉这个仓库的 `main`；
- 往这里 push 会触发固件仓库的 `build` 工作流（`repository_dispatch`），自动重编固件；
- 固件仓库的 `build.yml` 里仍会做包级断言（`CONFIG_PACKAGE_luci-app-openfi-fan=y`、镜像 manifest 等）。

## 布局

OpenWrt feed 的约定：**仓库根目录下每个子目录就是一个包**。

```
luci-app-openfi-fan/
openfi-lib/
```
