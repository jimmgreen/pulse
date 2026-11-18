# 响应式 C++ 示例

仅依赖公开 SDK，以 C++20 编译。输入控件 `query-input` 通过 `pulse.ui.events` 监听用户提交，复制借用文本后交给一个 `std::jthread`。工作线程按 ASCII 空白统计 token、统计 UTF-8 码点并将 ASCII 字母转大写；非 ASCII 内容保持原样。这不是自然语言分词或字形数量统计。

后台通过 `pulse.dispatch` 返回 UI，只有最新代次可以写入 `result-text` 和 `message-text`。后者完成时显示 `Complete: N tokens, M code points.`。连续输入合并为最新任务，旧结果不会覆盖新输入。普通 UI/事件/接口查询仍只能在 UI 线程使用；只有 dispatch 的 post/cancel 支持其他线程。

```powershell
cmake -S examples/native/reactive-panel -B build-native-reactive -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-reactive
.\build-native-reactive\pulse_client_wrapper_test.exe
```

`client.hpp` 的 Panel/Subscription 在 UI 线程销毁。订阅回调先取得自己的共享生存期并复制 functor，允许回调内注销或关闭。`std::function` 仅保留在 DLL，跨 ABI 仍是函数指针与 userdata。批次和字符串仍是借用数据，需在回调内复制；异常在 C trampoline 内截获。

DispatchQueue 在 post 返回前保留槽，即使回调先运行也不会提前释放 userdata。Close 抑制后续 functor，不将 cancel 返回 STATE 当作释放正在运行数据的许可。队列最多保留 128 槽。必须先结束并 join 所有生产线程，再在 UI 线程销毁队列；该封装不替调用者管理线程。

本例停用只请求合作取消、撤销订阅、关闭队列和页面，不在输入回调中 join。destroy 会 join 自己唯一的线程；线程只处理至多 1024 字节输入，每字节检查取消，无磁盘 I/O 或等待 UI 的调用。宿主必须等活动回调结束后卸载。停用后的同一实例不重新激活，请销毁再创建。无文件资源权限，清单 refresh 仅兼容开发入口。

独立 wrapper 测试使用内存假宿主检查生命周期边界，不能替代真实输入、IME、快速变化和停用的应用内验收。
