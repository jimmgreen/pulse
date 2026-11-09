# 有界声明式工作流维护与验收

工作流按同一增强包、窗口 owner 和授权 generation 顺序执行已有命令；每次文件操作仍进入原生确认。`WorkflowSnapshot.error` 保留机器错误码，`context.workflow` 面向界面的错误字段使用已有本地化文案。确认拒绝、取消、失败或撤销均不得启动后续步骤，完成步骤保留结果。

## 已验证基线

2026-11-04 的 `bench_data/platform-workflow-host-20261104-09` 实际宿主验收：导航 13 项、3 张 PNG；传输 40 项、9 张 PNG；`noSearchHelpers: true`。纯模型 33 项通过，日志为 `build-extensions/platform-workflow-controller-final.log`。这是基线，不包含下述新增生命周期用例。

前期失败与修复分别是：导航完成代次被后台刷新覆盖，现按 owner、tab、原请求 generation 保存不可覆盖的完成证据；探针普通路径与 `\\?\` 扩展路径比较错误，现使用宿主规范化路径；未激活窗口的按钮消息可能空转，现激活真实窗口并必须观察到新的 run ID；删除 `fail.bin` 后目录刷新改变选择，08 的原生确认 4、5 实际复制了 `revoke.bin`。最后一项属于故障注入竞态，不是允许失败后继续的产品行为。

准备失败用例现在通过仅隔离窗口启用的一次性 hook，在不可变传输请求已捕获、后台准备尚未入队时确认精确源路径，再删除隔离的 `fail.bin`。它不修改执行权限、不跳过确认、不伪造任务结果；原无额外确认、后续步骤保持 idle 的断言保留。

## 2026-11-04 生命周期定向验收

`workflow_lifecycle_probe.cpp` 与现有工作流探针分开，继续使用真实 Pulse 窗口、面板、原生确认及受控传输首写 gate：

- 原生确认选择取消：没有任务提交、没有目标文件、后续步骤保持 idle。
- 独立真实窗口的传输到达首写点后发送 `WM_CLOSE`：走真实 owner 清理；旧 binding 被撤销，请求跟踪清空，宿主内部按 task ID 观察终态，目标未提交，后续确认不出现。
- 新窗口不继承旧 run、请求或权限；其他真实 owner 无权查询和取消所属工作流。
- 在原生确认嵌套消息循环内关闭 owner：退出确认后不得复活请求或执行后续步骤。

这些场景使用新增 `refuse.bin`、`close.bin`、`close-confirm.bin` 隔离文件。实际宿主结果保存在 `bench_data/platform-workflow-lifecycle-20261104-01`：传输 63 项通过、15 张 PNG，`noSearchHelpers: true`。该计数包含保留的传输基线与新增生命周期检查，不与 09 的 40 项相加。`live_workflow_check.py` 核验进程退出、报告、PNG 清单、搜索辅助进程隔离及输出文件。本轮定向运行传输页；导航 13 项、3 张 PNG 的证据仍为上述 09 记录。

纯模型 35 项通过，日志为 `build-extensions/platform-next-workflow-model-01.log`，包含确认拒绝和 poll 回调内 owner 关闭的新增确定性用例。主代理已查看 `native-modal-owner-close.png`、`native-refusal.png`、`workflow-refused.png`、`workflow-new-owner.png`，文字和控件清晰，无空白帧。本轮记录只证明列出的隔离工作流生命周期场景，不声称未执行的任意窗口组合或全面平台回归通过。

主代理定向入口：构建 `pulse` 与 `pulse_extension_workflow_test`；运行对应纯模型目标，再运行 `tools/extensions/live_workflow_check.py --pulse build-extensions/pulse.exe --output <全新隔离目录> --page transfer-workflow`。主程序需包含新增 `workflow_lifecycle_probe.cpp`；不需要全量回归。

## 当前窗口标签与窗格步骤

`tabs.new/duplicate/closeActive/activate` 和 `panes.focus` 使用调用者owner的真实layout tabs与可见pane。步骤执行后重新捕获上下文，不沿用已关闭标签的指针。无加载可同步完成；有加载时等待各可见pane的真实结果。同路径成功后台刷新可继续等待，但最新applied代次必须有成功观测，任何初始或后续失败保持不可覆盖；更换layout、聚焦pane或路径仍终止，不执行下一步骤。原请求观察器与后续替换观察器有界，在完成、失败、取消和owner释放时清理。具体参数、最后/pinned标签保护见[工作区能力](extensions-workspace.md)。
