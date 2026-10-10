# 继续逐模块审查：本机补验

本轮遇到窗口工具激活错误后，继续推进可独立验证的项目。重新枚举时没有 Pulse 窗口；重新启动本轮便携版后，刷新窗口绑定和激活仍返回 `failed to activate captured window`。这不是用户前台遮挡的证据，不要求用户为此保持其他软件状态。

## M10-004 工作区持久化

增加 `pulse_places_persistence_audit_test --workspace-only` 定向入口。生产 PlacesCatalog 把三栏工作区写到真实隔离磁盘目录，销毁写入对象，再由全新的对象 Load。分别将“此电脑”的空路径放在首栏、中栏、末栏，检查三栏路径、视图模式及活动工作区索引。

结果：6 PASS、0 FAIL、exit 0，日志 `bench_data/workspace-disk-roundtrip-audit.log`。夹具 `bench_data/places-audit-26268` 保留。此前 OpenWorkspace 生产导航验证仍适用；本次补的是原记录明确欠缺的磁盘边界，不声称手动点击工作区已经完成。

## M19-002 增强包真实远端资产及安装

只读查询 GitHub 上 v1.0.53，状态为草稿。用已授权的 GitHub CLI 下载其中 17 个 LZMS 文件和 4 份包清单，与当前编译清单比对压缩大小、SHA-256 和完整清单内容：21 项通过，exit 0。日志 `bench_data/release-packs-live-audit.log`；下载文件保留在 `bench_data/release-packs-live-20261006`。

给既有安装测试增加 `--catalog-assets <downloads> <new-isolated-root>`，将下载文件分块送入生产 InstallPack，实际执行校验、LZMS 解压、暂存提交和 installed.json 写入。四个包共 37 项通过、0 FAIL、exit 0，涵盖解压后 17 个文件的大小与 SHA-256、主工具存在、版本记录、暂存目录清理及进度总字节。

日志 `bench_data/release-packs-real-install-audit.log`。安装结果保留在 `bench_data/release-packs-installed-20261006`，没有改动用户的增强包。该检查使用真实远端资产，但不经过客户端 WinHTTP 下载，不能替代设置页下载交互、镜像或取消测试。草稿尚未公开，不将 GitHub CLI 的授权下载等同普通用户可下载；没有发布或修改远端资产。

## M18-002 更新暂存目录与真实进程生命周期

增加 `pulse_update_stage_audit_test --process-only`。把测试程序自身复制为隔离暂存目录中的 PulseSetup.exe 并实际启动；关闭下载租约后执行生产 SweepUpdateStages，检查执行中的程序仍存在且仍运行；通过事件让自有进程正常退出，再次清理，检查安装包及目录消失。

结果：4 PASS、0 FAIL、exit 0，日志 `bench_data/update-stage-real-process-audit.log`。没有操作用户安装目录或执行真实安装升级，也不声称覆盖 UAC；本次把原有文件锁模拟补到真实 Windows 进程占用边界。

## 构建与交付边界

只构建上述三个受影响测试目标，均通过；差异空白检查通过（Git 提示的 LF/CRLF 转换不是代码检查失败）。本轮没有改动生产代码，因此沿用 `dist/audit-20261006-search-layout` 测试包，不重复打包。XML/Excel 的桌面交互仍待工具恢复，M13-007 继续按用户要求暂放。
