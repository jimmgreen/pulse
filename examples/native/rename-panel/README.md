# C++ 原生重命名示例

这是可信、进程内 DLL 的开发示例，不是沙箱插件，也不是已经支持的原生包安装格式。DLL 仅依赖 `sdk/native/include` 的公开 C ABI 和 C++ 薄封装，不包含 Pulse 私有头、Win32 控件实现或 JSON 请求协议。

在 x64 Visual Studio 开发命令行构建：

```powershell
cmake -S examples/native/rename-panel -B build-native-rename -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-rename
```

外部项目可使用 `-DPULSE_SDK_INCLUDE=C:/path/to/sdk/native/include` 指定公开 SDK。两个输出 DLL 使用相同宿主接口：`pulse_rename_lower.dll` 将 ASCII 文件名转为小写，`pulse_rename_upper.dll` 通过 `PULSE_SAMPLE_UPPER_CASE=1` 转为大写。UTF-8 非 ASCII 字节保持不变，不宣称实现 Unicode 大小写折叠。

宿主开发入口接入后，显式选择一个可信 DLL 和绝对清单路径：

```powershell
.pulse.exe --extension-manifest C:\absolute\rename-panel\manifest.json --extension-native C:\absolute\build-native-rename\pulse_rename_lower.dll
```

清单只提供包身份和资源授权；`view.refresh` 命令用于现有声明式清单兼容，页面由 DLL 使用 `pulse.ui` 创建。此命令示例不是已完成应用内验收的声明；本目录的独立构建只验证 DLL 能以公开 SDK 编译。

操作过程：

1. 在已加载的普通目录输入前缀，例如 `demo-`，点击 Preview。
2. `pulse.files` 异步返回目录条目。DLL 自己生成“前缀 + 从 1 开始的编号 + 连字符 + 转换后的原文件名”，包括原扩展名。编号依宿主返回顺序，跳过目录和重解析项。
3. DLL 将条目的不透明句柄和新叶名称交给 `prepare_rename`；宿主检查名称、冲突与目录快照，返回短期计划。列表展示全部计划，独立输出文字展示第一行。过长展示文字会截断，提交仍使用完整计划。
4. 点击 Review and rename 打开宿主真实确认，确认后提交受控任务。Refresh task status 查询实际结束状态，Cancel own task 仅取消本包拥有的任务。

示例一次处理至多 64 个目录条目；存在下一页时拒绝提交部分页面，提示改用更小目录。计划 60 秒到期，预览后编辑前缀不会修改已有计划，需重新 Preview。关闭页面或停用 DLL 会取消待完成请求并同步撤销事件，不自动取消已提交的文件任务。所有回调在宿主 UI 线程；借用的回复在返回前复制，异常在 DLL 内截获，销毁不跨 CRT。宿主必须等正在执行的回调返回后再卸载 DLL。

固定验收控件 ID：`prefix-input`、`preview-button`、`commit-button`、`status-button`、`output-text`、`message-text`；完整预览列表为 `native-plan`。
