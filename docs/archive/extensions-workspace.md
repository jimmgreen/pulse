# 当前窗口标签与窗格能力

`tabs.new`、`tabs.duplicate`、`tabs.closeActive` 无参数。`tabs.activate` 接受整数 `index` 0..63，`panes.focus` 接受整数 `index` 0..3；运行时还检查真实范围。只接受当前授权 caller 所属窗口，不接受外部 HWND、AppState 或内部 action 字符串。

标签是当前窗口顶栏的 layout tabs，每个可包含多个 pane。标签索引按窗口当前标签顺序；窗格索引按当前活动 layout 的 `VisiblePanes()` 可见顺序，不访问隐藏或其他标签中的窗格。new 跟随宿主新标签位置偏好。duplicate 与宿主“复制标签”相同，只复制当前聚焦文件夹位置到新单窗格标签，不深拷贝多窗格布局。新增与复制在已有64标签时拒绝；关闭拒绝最后一个标签和固定标签，不能借此关闭窗口。

适配层直接复用 `OpenNewTab`、`TabController` 的类型化复制入口、`CloseActiveTab`、`SwitchTab`、`FocusPane`，不直接修改标签或窗格容器。能力入口重新校验 owner/generation 和运行时范围。工作流上下文操作执行后捕获新的活动 layout、聚焦 tab 和可见 pane 路径；无加载则同步完成，有加载则等待真实请求结果。等待期间替换活动 layout、聚焦 pane、任一pane身份或路径会终止工作流。同路径后台刷新允许继续，但必须等待所有加载结束，由有界观察器证明初始请求及最新applied代次实际成功；任一次失败保持sticky，后来的成功不能洗掉失败，两次轮询之间完成的请求也不会遗漏，后续步骤不能误用旧 tab。

## 定向验证（2026-11-04）

`pulse_extension_focused_workspace_test` 已通过18项；目标仅编译 `src/bench/extension_focused_workspace_test.cpp`，覆盖真实索引预算、缺少参数、64标签上限、最后标签与固定标签拒绝、无效上下文等18项纯策略边界。

真实入口：`tools/extensions/live_workspace_check.py --pulse build-extensions/pulse.exe --output <全新隔离目录>`。从作者模板复制隔离清单，额外添加两项合法但超过实际数量的索引命令，验证动态范围拒绝；普通操作均使用模板真实按钮。覆盖聚焦两窗格、复制聚焦位置、关闭非末标签、按偏好新建、激活既有标签、固定与末标签关闭拒绝、错误 owner/撤销、激活后复制工作流以及等待期间切换上下文阻止后续复制。

预期六张应用内截图：五张宿主主窗与一张真实面板。主窗使用既有 compositor 快照并刷新双缓冲，禁止桌面截图替代。脚本检查退出码、报告、PNG清单、fixture摘要和搜索辅助进程隔离。第10、11轮真实宿主均退出0、32 PASS、0 FAIL、六张PNG，夹具摘要不变且无搜索辅助进程。第10轮六张应用内截图已逐张检查，窗格展开后布局正常；面板长内容使用原生滚动区域。

首轮 `bench_data/workspace-host-20261104-01` 的31项断言与6张PNG不计作视觉验收通过：复制和新建主窗图抓到了窗格展开动画中的窄布局。探针现等待宿主 `paneLayoutMotion.start` 清零，比较 `BuildVm` 每个pane矩形与既有 `LayoutWindowPanes` 最终几何，并在100ms消息泵周期保持一致后才渲染双缓冲抓图。若超时则截图检查失败并报告实际矩形，不修改生产动画或布局。第10轮重新验图通过，首轮失败证据保留。

重复验收08暴露探针的观察竞态：缓存足够快时首步骤同步完成，轮询无法可靠停在current_step=0。替换用例改用仅isolatedTest可触发的一次性workspace检查点，位置在真实Activate调用及新上下文捕获之后、完成判定之前；回调立即执行真实SwitchTab，下一真实timer tick按原上下文判定拒绝后续duplicate。它不阻塞UI、不伪造加载、不放宽失败断言；此用例是明确的受控交错，不声称模拟真实磁盘延迟。正常工作流用例不安装检查点。第10、11轮均验证该受控交错拒绝后续复制。

第09轮诊断确认此前严格拒绝所有新代次会误停正常宿主：非聚焦pane的初始15与后台16均成功、layout/focus/path未变。等待逻辑改由共享ObserveWorkspaceListing判定，允许有成功证据的同路径后续刷新；保留initial exact失败、任何中间失败、未观察到的最新applied和真实上下文替换拒绝。新增六项模型回归覆盖这些边界。真实布局替换检查点和duplicate禁止断言不变；最终共享模型45项通过（build-extensions/workspace-workflow-native-02.log），真实第10、11轮通过。
