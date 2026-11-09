# 对话框框选与关闭焦点修复

## 原因和改动

- 对话框复用了主列表渲染，但此前空白按下只清空选择，没有接入拖动框选。现在复用 `MainRenderer::PointInItemGrip`、`ItemsInRectInPane` 和 `MarqueeSelection`，通过小型 `PickerMarquee` 状态适配对话框的显式选择集合。普通框选替换、Ctrl 切换、Shift 追加；拖到边缘滚动，Esc/捕获丢失恢复拖动前选择，导航和关闭释放捕获与计时器。
- 主界面的框选仍默认使用原有全选压缩表示；对话框关闭压缩，文件名标签和 COM 返回共享同一个显式集合。
- 关闭时激活宿主会同步调用宿主自身的焦点恢复处理。guard 随后的 `SetFocus(owner)` 覆盖了宿主刚选定的编辑控件；删除这次多余调用，保留恢复启用、窗口身份检查和“不抢其他前台窗口”的判断。

## 实际验证

- 受影响主程序、UI provider、runtime、测试目标构建成功。既有 `app_input.cpp` 的 `pointer_moved` 未使用警告仍存在，不属于本次新增。
- `pulse_marquee_selection_test` 通过：新对话框状态边界，以及原主界面 57,600 帧随机选择等价检查。
- 真实 provider/wrapper 对话框六组回归通过：普通框选返回 4 项、Ctrl 框选返回剩余 6 项、Shift 追加返回 4 项、Esc 与捕获丢失均恢复原 10 项，长列表边缘滚动返回全部 40 项。逐项校验实际路径；截图同时验证选框、列表高亮和文件名标签。
- 焦点实机检查 20/20 通过：恢复宿主编辑控件，以及关闭前、启用通知和激活通知重入时切换其他窗口，不抢回前台。测试中重放旧尾调用，准确触发 6 项编辑控件焦点失败；修复路径再次全部通过。
- 最初拖动测试因驱动线程 DPI 坐标被二次缩放失败，修正测试线程 DPI 上下文后重测通过。最初焦点测试没有取得前台、非输入桌面又没有 foreground，均未计作通过；最终在默认桌面、明确验证前台前提后运行。

主要日志：`bench_data/picker-marquee-*-live-r2.log`、`bench_data/picker-marquee-scroll-live.log`、`bench_data/picker-marquee-model.log`、`bench_data/picker-focus-live-fixed.log`、`bench_data/picker-focus-legacy-tail.log`。

交付包：`dist/PulseSetup-1.0.58-test-picker-interaction-20261102.exe`。保留当前工作区此前的多排标签、设置搜索、分段控件与共享运行目录改动。未覆盖安装用户正在使用的副本，也未宣称已在用户反馈的 Photoshop 上验证。
