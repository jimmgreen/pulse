# Native directory browser（公开服务组合参考包）

这个独立 C++20 DLL 只使用公开 `pulse.directories`、`pulse.context`、`pulse.ui`、`pulse.ui.model` 和 `pulse.ui.data` ABI。它将目录服务的不可变 snapshot 页转换为 UI data provider 的 rows，用共享原生 DataTable 展示，不自行枚举文件系统、不使用 Win32 watch、不在宿主增加参考包专属业务。运行时没有 JavaScript。

## 构建

在 x64 Visual Studio 开发者命令行，从仓库根目录执行：

```bat
cmake -S examples/native/directory-browser -B build-directory-browser -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-directory-browser
build-directory-browser\pulse_directory_browser_test.exe
```

可设置 `-DPULSE_SDK_INCLUDE=...` 指向单独复制的公开 SDK include。产物为 `pulse_directory_browser.dll`，使用静态 CRT、C++20、`/W4 /permissive- /utf-8`，不链接主程序或私有 `src/` 头。

清单申请 `context.read`、`context.events`、`files.readDirectory`、`files.watchDirectory`、`files.open`。调用成功只是接受异步工作；`open_entry` 成功只是正常打开路由接受分发，不表示应用已经启动或文件执行完成。

## 页面和行为

- 路径输入 `directory-input` 的初值来自当前 Pulse context；启动时自动打开该路径。后续 context 导航只更新摘要，不覆盖作者正在编辑的路径。输入使用共享 UI 状态的 1024 UTF-8 字节上限；超长当前路径不自动填入。
- **Open directory** 取消前一个目录的请求、watch 和句柄，再异步打开输入的绝对 drive / UNC 路径。目录 A 快速切换 B 时不使用 A 的晚到结果。
- **Refresh** 重新建立 snapshot；失败保留目录服务原 snapshot，可再次点击恢复。**Next 256** 在同一 snapshot revision 上追加下一页。
- **Open selected** 先把 `selected` UI row ID 映射回当前 `directory / snapshot / entry_id`，再调用 `open_entry`。刷新后的旧 row ID 不会被当成新条目的 token。
- **Close directory** 取消刷新、分页、打开条目和目录打开请求，停止 watch，关闭句柄，再通过 unbind / rebind 清空表格，状态为 `cancelled`。这避免把尚在 inbox 的空页提交后立即取消而丢失清行。
- `status-label` / `error-label` 显示数据控制器状态与错误；`path-label` 显示参考包打开的目录；`context-label` / `selection-label` 显示 Pulse 自身的导航与选择摘要，包括 revision。

真实 DataTable 节点为 `items`，列 Name / Size / Type 宽度为 210 / 80 / 80 DIP，总宽 370。目录 snapshot 完整保留在服务中；共享 UI 每个节点上限 4096 行，本参考包达到此上限后停止分页并明确提示。单页通常为 256 行；长 Unicode 名称使 UI 64 KiB 页预算不足时会缩小该页，再从精确 offset 继续，没有跳过条目。参考包不声明自行排序，保持目录服务 snapshot 的顺序。

## 生命周期和有界请求

所有组合操作在公开接口规定的 UI 线程上发起；实际枚举和监听由宿主工作线程完成，DLL 不创建后台线程，也不等待文件系统 I/O。每个实例只有一条 `refresh -> list` 流水线、一条可取消的 open-entry 请求、一个目录和一个 watch，远低于 32 个 pending 请求上限。

watch 的 READY 到达后必定再刷新一次，消除打开 snapshot 与 arm watch 之间的启动窗口。工作进行中的 INVALIDATED / overflow 只设置一个 dirty 标记；完成后最多补一次刷新，事件爆发不创建无限请求。watch 失败会显示错误，仍可手动 Refresh。通知是失效提示，不被宣传为精确逐文件变更日志。

新 UI 数据请求会取消旧目录页工作。回调同时检查当前 request / directory / watch token，条目映射包含 snapshot 代际。deactivate / destroy 取消全部请求并取消 context 订阅、watch、data provider 和 UI 命令订阅；服务保证未来回调同步失效，DLL 返回后没有借用字符串或回调 user 指针继续存活。

`pulse_directory_browser_test` 使用公开 ABI 假宿主提供真正延后的回调，覆盖 watch READY 补刷、分页、entry token、失效合并、快速换目录、关闭、失败恢复、context 变化、UI 行预算和销毁。它不替代真实宿主的目录 I/O、页面布局和交互验证。
