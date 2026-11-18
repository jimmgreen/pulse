# C++ 目录分析插件

该 DLL 仅包含公开 SDK 头。`pulse.files` 分页读取当前已加载目录，DLL 自己按扩展名累计文件数与字节，再按选项 `bytes` 或 `count` 降序排列，用 `pulse.ui.model` 的真实 DataTable 展示。扩展名只做 ASCII 大小写归一化；不递归目录，跳过目录和重解析项，最多处理 4096 个目录条目。没有扩展名或以单个点开头的名称归入 `(no extension)`。

在 x64 Visual Studio 开发终端：

```powershell
cmake -S examples/native/inspector -B build-native-inspector -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-inspector
```

外部源码包使用 `-DPULSE_SDK_INCLUDE=C:/absolute/sdk/native/include`。输出 `pulse_inspector.dll`，通过宿主开发入口显式加载：

```powershell
.\pulse.exe --extension-manifest C:\absolute\inspector\manifest.json --extension-native C:\absolute\build-native-inspector\pulse_inspector.dll
```

这是可信进程内 DLL 的开发示例，不是沙箱或生产安装协议。清单声明的四项资源授权仅控制宿主接口；原生代码本身拥有宿主 OS 权限。

页面操作与验收标记：

- `analyze-button`：完成全部分页和计算后，`message-text` 显示 `Analysis complete: N files, M extensions.`，结果位于 `result-table`。宿主拒绝表格更新时明确显示失败，不称为展示成功。
- `stat-button`：用分析时复制的首个普通文件不透明句柄调用 `pulse.resources.stat`，成功显示 `Stat complete: N bytes.`。句柄过期或目录快照变化会显示宿主错误。
- `load-settings-button`：读取本包设置与版本，将 `display.sort` 放入 `option-input`。成功显示 `Settings loaded: version N.`。
- `save-settings-button`：仅接受 `bytes` / `count`，使用上次成功读取的版本执行 CAS 写入。先加载后保存；版本冲突要求重新加载，绝不自动覆盖。成功显示 `Settings saved: version N.`，不把请求接受等同提交成功。
- `audit-button`：读取当前 owner/principal 的最多 128 条自有审计记录，显示 `Audit complete: N own records.`，不读取其他包或历史会话。

所有接口和回调运行在宿主 UI 线程。条目、游标、设置、状态等借用数据在回调返回前复制到 DLL 自有内存，未保存宿主数组指针。新操作会取消旧的未完成请求；取消设置请求不会回滚可能已经提交的设置。停用同步取消请求、撤销命令订阅并关闭页面；宿主须等当前回调返回后再卸载。所有 ABI 回调捕获 C++ 异常。独立编译不能替代真实宿主 UI、分页、设置冲突和资源授权验收。
