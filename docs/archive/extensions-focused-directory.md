# 聚焦目录的选择与排序能力

本页描述声明式命令的行为；不等于原生 C++ SDK 已开放同名服务。文末验收记录保留对应旧版本状态，当前开发阶段和后续项见 [主路线图](../Pulse-增强包开放能力与AI协同实施方案-2026-11-02.md)。JavaScript 执行层已经移除，不再推进脚本隔离阶段。

`selection.selectAll`、`selection.invert`、`selection.clear` 无参数；`view.setSort` 接受严格的 `column` 与 `direction`，列为 name/modified/created/accessed/type/size/path，方向为 ascending/descending。这些能力使用增强包既有 owner 与 generation 绑定，不接受客户端传入的 AppState、路径或主体。

执行前重新检查聚焦 pane：普通文件系统目录、快照与当前路径一致、没有 loading 或 pending generation。文件名搜索、内容搜索、虚拟页、设备页与回收站视图拒绝。选择使用宿主 `CollectFilterMatches`、`SelectIndices`、`InvertIndices`、`ClearSelection`，因此全选与反选包括宿主已展开的过滤后可见行，反选不会改变该集合外已选项；清空移除本 pane 全部选择。

排序直接调用宿主 `SetSort`，保留既有文件夹排序模式和选中项恢复行为；可能写入该目录的视图偏好，不修改全部 pane 或全局默认排序。工作流中的选择同步完成，排序等待同一 tab、同一路径的真实请求结果及加载结束，完成前再次核对列和方向；期间用户替换排序时工作流失败，不能继续执行后续选择。

## 定向验收入口与当前状态

纯映射与状态边界测试：`src/bench/extension_focused_directory_test.cpp`，覆盖七列、双方向、未知值、加载/待刷新/过期快照/搜索拒绝以及排序替换。它不依赖宿主 UI。

真实入口：`tools/extensions/live_focused_directory_check.py --pulse build-extensions/pulse.exe --output <全新目录>`。使用独立 profile 与四个测试文件，实际 `selection-sort-panel` 按钮覆盖过滤选择、零匹配、空目录、七列双方向真实列表次序、选中保持、另一 pane 不变、错误 owner、撤销和排序后选择工作流；另检查工作流排序被用户替换后不执行后续选择。六张应用内截图（四张面板、两张主窗口文件列表及另一 pane）、报告、退出码、文件摘要和搜索辅助进程检查保存在输出目录。

新增四项能力已完成冻结源码构建与真实宿主验收：`bench_data/selection-sort-host-20261104-02` 的62项检查、6张应用内截图通过并逐张查看；过滤主列表显示两项已选文本文件，排序工作流显示四项全选且另一窗格不变。`selection-sort-workflow-regression-20261104-02` 导航13项/3图、传输63项/15图通过。原生7个定向目标、CLI 66项、生成器23项及19模板/73类型负例通过。首次59项报告中的旧帧截图保留为失败证据，不作为最终视觉验收。SDK交付以对应归档旁的 `acceptance/report.json` 为准，其中记录独立解压验收、实际宿主摘要、类型检查及截图；历史归档计数仍只对应各自固定版本。

首次真实运行的59项断言与6图未作为视觉完成依据：过滤主窗图显示旧的四行未选中状态。探针现通过ShowFilterEditor/原生EDIT/SyncFilterEditor/HideFilterEditor以及ClearPaneFilter驱动过滤，截图前核对BuildVm可见行数和选中数，再渲染双缓冲的稳定状态。第二轮已重跑62项并亲看全部6图，两张主窗图均与真实选择和过滤状态一致，详见该轮 visual-review.json。

冻结构建位于 `build-extensions/frozen-selection-sort/build`，源码2789文件摘要与受控修复记录位于上级 `snapshot.json`。构建保留共享 action_registry.cpp 的既有 C4100 警告；并发探针的字符串类型编译错误仅在验证快照采纳其已有修复，没有修改共享文件。最终构建退出0。

## 2026-11-04 选择与排序固定交付

最终开发预览归档：`dist/extension-sdk-capabilities-20261104-02/Pulse-Extension-SDK-v1-preview.zip`，SHA-256 `2fd718f96a70ea872015a677e6d5d58e58c5135ecf57aa308be719e91c8c0ba9`。63文件、19模板、14命令能力、8资源操作。独立解压作者链路145项、安装窗口25项、启动17项通过；19模板、73项类型负例和实际host-api严格编译通过。五模板30张原生预览图；新增选择排序模板的明暗主题及100/150/200%六图均已查看，预览不执行命令。

真实目录62项/6图已通过并逐图复核；导航13项/3图和文件工作流63项/15图回归通过。`acceptance/report.json` 保存实际宿主摘要、固定源码2789文件摘要、受控快照修复、原生定向结果、原始失败记录和所有证据文件摘要。归档及解压文件逐项一致，首轮旧帧截图不计最终通过。最终宿主位于 `build-extensions/frozen-selection-sort/build/pulse.exe`；共享导航改动仍由其他任务维护，不以本记录声称整个共享工作区已验证。

本条为归档验收后的记录，不回写ZIP。UI框架/原生控件适配与本次四项内部能力已交付；完整人工输入法/辅助技术矩阵、其余内部能力、持久诊断、完整授权产品界面、脚本隔离及生产分发仍按实施方案的剩余项推进，不能把本次增量写成全部计划完成。
