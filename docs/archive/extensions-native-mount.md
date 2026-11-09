# 原生 UI 挂载

`pulse.ui.mount` V1 将增强包已经创建的 `pulse.ui` / `pulse.ui.model` 页面挂入 Pulse。它移动同一个原生 HWND 和组件树，不序列化重建页面；状态、点击/输入订阅及 `pulse.ui.data` 绑定继续有效。

## 三种位置

| placement | 位置 | 归属 |
|---|---|---|
| `PULSE_UI_MOUNT_WINDOW` | 独立工具窗口 | 所属 Pulse 窗口拥有；多个包可同时拥有各自窗口 |
| `PULSE_UI_MOUNT_SIDEBAR` | 主窗口右侧栏 | 每个宿主窗口至多一个，占用时返回 `PULSE_ERROR_STATE`，不替换原页面 |
| `PULSE_UI_MOUNT_WORKSPACE` | 主窗口内容承载区 | 每个宿主窗口至多一个；这是内容面，不是新增原生标签页 |

侧栏与工作区槽位分别占用；不同包不共享 panel ID。旧声明式侧栏与原生侧栏互斥。空间不足时回到独立窗口，`get` 和事件报告实际位置；作者应读取结果，不能把请求位置直接写成成功状态。关闭、停用及宿主销毁会释放占用，不保存或改变用户窗格配置。

## 调用与事件

```cpp
#include <pulse/sdk.hpp>
#include <pulse/ui_mount.h>

PulseUiMountV1 mount{};
auto status = pulse::sdk::Host(host).Query(
    PULSE_UI_MOUNT_INTERFACE, PULSE_UI_MOUNT_VERSION_1, mount);
if (status != PULSE_OK) return status;
status = mount.set(mount.context, panel, PULSE_UI_MOUNT_WORKSPACE);
PulseUiMountInfo info{};
info.size = sizeof(info);
if (status == PULSE_OK) status = mount.get(mount.context, panel, &info);
```

所有调用、宿主同步和回调均在所属 UI 线程执行。结构按公开头的 8 字节 ABI 对齐；`PulseUiMountInfo` 为 40 字节，调用者必须初始化 `size`。窗口句柄是宿主实现细节，不跨 C ABI 暴露。无效/外来 panel 和已关闭实例返回状态错误，未知位置返回参数错误；旧接口表布局不变。

`subscribe` 返回订阅 token，初始快照延后到下一个宿主同步 turn。之后将 placement、客户区 DIP 尺寸、DPI、主题及焦点变化合并推送，以单调 `revision` 区分。独立窗口按自身 `WM_DPICHANGED` 缩放，嵌入页面跟随宿主。每 attachment 最多 16 个挂载订阅。

回调可移动页面、关闭页面或取消订阅；宿主不嵌套调用挂载回调。Info 指针仅在当前回调期间借用。`unsubscribe`、UI 关闭及停用抑制后续排队通知，正在栈上的回调可以正常返回。作者应在 `deactivate` 撤销订阅并关闭 UI，异常不得跨 ABI。

挂载协议不包含工具栏贡献、自定义原生标签页、桌面承载或桌面接管。目录工作台使用这三种位置验证公开 SDK；多屏桌面层级与异常恢复仍需独立交付，不能从普通窗口截图推断。

```text
AI 作者提示：先查询 pulse.ui.mount V1，使用既有 panel ID 调用 set。
以 get/subscribe 返回的实际 placement 和 DIP 尺寸调整增强包布局；
不要为每个挂载位置重建另一套控件。保留数据、输入与事件订阅，处理
槽位忙、窄窗口回退、主题/DPI变化和关闭。停用时取消订阅并关闭页面。
```
