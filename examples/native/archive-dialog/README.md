# C++ 压缩对话框示例

该 DLL 仅包含公开 SDK 头，演示 7-Zip 扩展“添加到压缩包”那种宿主绘制的对话框：右键菜单命令打开 `pulse.dialogs` 模态框，用大标题、图标、对象卡片（所选文件或文件夹）、底部提示和主/次按钮，表单由 `pulse.ui.model` 节点组成。

- 名称输入框后的扩展名徽标、密码区的显隐和“两次密码不一致”提示都是派生状态：`state_changed` 收到编辑后，插件计算并用 `set_states` 写回，表单不需要重建。
- “浏览…”按钮声明模型命令 `browse`，由 `DialogSession::BindFolderPicker` 交给 `pulse.pickers` 选文件夹并写回 `output` 状态（需要 V2 宿主；V1 宿主上该字段仍可手动编辑）。
- 点击“创建”后，第二个对话框经 `pulse.dispatch` 投递打开，汇总所选设置。每个包同一时刻只能有一个对话框，因此不在第一个对话框的 `closed` 回调里直接打开。
- 示例只演示界面，不读写所选文件，也不在 UI 线程做文件 I/O；真正的压缩应交给 `pulse.tasks`（进度和完成提示）和进程外工作程序（见 `sdk/native/samples/sandboxed-worker`）。

在 x64 Visual Studio 开发终端：

```powershell
cmake -S examples/native/archive-dialog -B build-native-archive-dialog -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-archive-dialog
```

外部源码包使用 `-DPULSE_SDK_INCLUDE=C:/absolute/sdk/native/include`。输出 `pulse_archive_dialog.dll`，通过宿主开发入口显式加载：

```powershell
.\pulse.exe --extension-manifest C:\absolute\archive-dialog\manifest.json --extension-native C:\absolute\build-native-archive-dialog\pulse_archive_dialog.dll
```

在文件列表选中文件或文件夹，右键选择“Add to archive (example)...”。这是可信进程内 DLL 的开发示例，不是沙箱；原生代码拥有宿主 OS 权限。
