# Live generated data（独立 C++ DLL）

此示例仅依赖 `sdk/native/include` 的公开 C/C++ 头。DLL 自行生成 4096 个带稳定 ID 的条目、自行在后台执行名称筛选和排序，再通过 `pulse.ui.data` 向 `pulse.ui.model` 的真实 DataTable 提交 128 条分页和持续 delta。页面仍使用共享的 `pulse.ui` 状态与原生控件，不创建另一套列表渲染器。

这是生成数据的 provider 示例。它不读取文件系统，也没有开放或模拟目录订阅 / directory watch 服务。

## 构建与运行

在 x64 Visual Studio 开发者命令行，从仓库根目录执行：

```bat
cmake -S examples/native/live-data -B build-live-data -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-live-data
build-live-data\pulse_live_data_test.exe
```

可用 `-DPULSE_SDK_INCLUDE=...` 指向一份单独复制的公开 SDK include 目录。DLL 不链接主程序或使用 `src/` 私有头。正常包安装使用 `manifest.json`，显式 DLL 开发启动参数见仓库原生扩展文档；DLL 调试入口不代表目录 watch 已开放。

## 页面操作

- **Filter**：按生成名称进行 ASCII 大小写不敏感子串筛选。输入状态变更由绑定控制器取消旧请求并重新发起筛选，筛选计算在 DLL 工作线程上进行。
- **Refresh**：退出 Empty / Error 演示、从当前数据集首屏重新筛选排序，并启动持续流。初始共 4096 条；添加和删除成对发生，数据集数量保持不变。
- **Next 128**：在有效 cursor 上追加下一页。该流不会产生 delta。可连续翻页；初始名称为 `Item 00001.txt` 到 `Item 04096.txt`。
- **Cancel**：取消当前流。取消后可继续滚动已显示条目；Refresh 开始新请求。
- **Mutate now**：在首屏持续流中立即触发一次 delta；后台也每 30 秒触发一次。每次更改第一条可见条目的分数、删除末条可见稳定 ID，并向数据集加入一个新稳定 ID；新条目只有匹配当前筛选时才加入显示。筛选为空时可直接观察 add / change / remove。
- **Empty / Error**：分别提交有效的空 snapshot 和故意的 provider 错误。Refresh 恢复正常数据。
- **A to Z / Z to A**：更新绑定排序状态，由宿主取消旧流并请求按名称升序 / 降序的新 snapshot。

数据接口规定 delta 会清除分页 cursor。因此要演示分页，可在首个 delta 到达前点击 Next，随后页流保持静态；Refresh 返回持续流。delta 本身按稳定 ID upsert / remove，没有位置字段；Refresh 再执行完整排序。状态、错误、进度和操作说明直接显示在表格上方。

## 线程寿命与取消

每个插件实例只有一个自有 `std::jthread`。请求回调立即复制借用字符串，再把最新请求写入有界 mailbox；反复 Refresh 只覆盖待处理请求，不创建新线程，也不在 UI 回调 join。后台线程拥有生成数据、修订号和算法，且只调用允许跨线程的 `publish`。传入页的字符串和数组在同步复制返回前保持有效。

cancel 回调只使对应请求失效并唤醒线程。计算与等待会检查 request / stop；即使取消和 publish 竞态，宿主返回的 `PULSE_ERROR_STATE` 也使该流退出。deactivate 停止请求、取消绑定和订阅并请求线程停止；destroy 同步 join，返回后没有插件线程或借用内存继续使用宿主。错误不会跨 ABI 抛出。

`pulse_live_data_test` 使用公开接口的最小假宿主验证分页、筛选排序、delta、取消、快速刷新、空/错误和销毁线程收尾。它不是宿主 DataTable 的视觉或集成验证；真实页面仍需在 Pulse 内检查。
