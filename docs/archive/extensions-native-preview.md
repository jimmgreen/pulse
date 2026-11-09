# Pulse C++ 原生插件开发预览

这是可独立编译插件并运行应用内验收的开发包，包含匹配的测试宿主、原生渲染依赖、公开源码 SDK 和九个示例工程（basic、rename-panel、inspector、reactive-panel、live-data、directory-browser、file-operations、directory-transfer、directory-workbench，共十个 DLL 输出）。无需 JavaScript。Windows x64、MSVC C++20、CMake、Ninja 和 Python 3 是作者环境。

## 编译与验收

在 x64 Visual Studio 开发终端，从解压根目录运行：

```powershell
python tools/extensions/test_native_sdk.py --sdk-root "$PWD" --work "$PWD/verification"
```

该命令检查文件哈希、编译九个示例工程的十个 DLL，并验证 ABI 对齐、V2/V1 与 V3/V2 函数表前缀、C++ 包装、动态数据提供者和目录示例假宿主测试。它不自动宣称宿主交互通过。DLL 位置见 `verification` 下的构建目录；显式目标示例为 `build-directory-transfer/pulse_directory_transfer.dll`。

应用内验收使用以下脚本；路径参数指定刚编译的 DLL，输出目录必须不存在。每次启动使用独立测试配置和脚本创建的样本文件，不修改日常 Pulse 配置：

```powershell
python tools/extensions/test_native_runtime_ui.py --pulse bin/Pulse.NativePreview.exe --lower <pulse_rename_lower.dll路径> --upper <pulse_rename_upper.dll路径> --output acceptance/rename
python tools/extensions/test_native_inspector_ui.py --pulse bin/Pulse.NativePreview.exe --plugin <pulse_inspector.dll路径> --output acceptance/inspector
python tools/extensions/test_native_reactive_ui.py --pulse bin/Pulse.NativePreview.exe --plugin <pulse_reactive.dll路径> --output acceptance/reactive
python tools/extensions/test_native_data_ui.py --pulse bin/Pulse.NativePreview.exe --plugin <pulse_live_data.dll路径> --output acceptance/native-data
python tools/extensions/test_native_directory_ui.py --pulse bin/Pulse.NativePreview.exe --plugin <pulse_directory_browser.dll路径> --output acceptance/native-directory
python tools/extensions/test_native_fileops_ui.py --pulse bin/Pulse.NativePreview.exe --plugin <pulse_file_operations.dll路径> --output acceptance/native-fileops
python tools/extensions/test_native_target_ui.py --pulse bin/Pulse.NativePreview.exe --plugin <pulse_directory_transfer.dll路径> --output acceptance/native-target
python tools/extensions/test_native_workbench_ui.py --pulse bin/Pulse.NativePreview.exe --plugin <pulse_directory_workbench.dll路径> --output acceptance/native-workbench
```

测试宿主实际加载 DLL、操作 Pulse 原生控件、执行隔离样本的重命名，并从应用内部保存截图及报告。重命名示例由 DLL 自行计算名字；目录分析示例自行汇总文件；后台示例自行管理工作线程。业务算法不在宿主预置命令中。

开发时直接启动的入口为 `--extension-manifest <manifest.json绝对路径> --extension-native <DLL绝对路径>`。只加载你信任的原生代码；DLL 与 Pulse 同进程，其错误会影响宿主。常规启动不会启用测试脚本的自动操作。

## 交付边界

本包是原生接口测试宿主，不是完整 Pulse 安装程序；包含普通文件操作及 Undo 所需的 `pulse_shell.exe`，未包含索引、文档预览和管理员操作等完整应用辅助进程，也不用于验证那些功能。原生包现在支持单 DLL 的 `.pulse-ext` v2 分发、安装、持久启停、默认目录发现、同窗口多个插件、升级和回退；每个插件按包和窗口拥有独立实例，正常命令面板打开时按需激活。

包作者工作流见 `docs/native-packages.md`：使用 `native-manifest` 生成声明，`pack` 打包，`verify` 校验，`install --allow-native` 安装，再 `enable --allow-native` 启用。安装默认禁用；正常启动不需要开发 DLL 参数。持久安装/启停/升级/回退在下次启动生效，窗口中的启停只改变本进程状态。默认包库损坏时记录诊断并保留主程序可用，显式指定损坏包库仍严格报错。

完整分发验收脚本 `test_native_packages_ui.py` 接受包内宿主、已独立编译示例目录和一份缺少 `PulsePluginEntryV1` 的测试 DLL（`--missing-entry`），通过 `--archives` 测试真实 v2 包。它创建独立配置，运行同窗口多包、失败隔离、卸载再启用、小写算法→大写升级→小写回退，并保存 19 张应用内截图。测试输入必须放在专用目录，输出目录必须不存在。

动态数据通过新增的 `pulse.ui.data` V1 绑定既有 virtualList、tree、dataTable；支持提供者注册/撤销、分页、快照、增量、筛选/排序请求和取消。`live-data` DLL 自己生成数据、管理后台线程和算法；宿主复用实际控件。应用内脚本走打包、安装、启用和正常菜单入口，检查 4096 条数据、快速筛选、增删改、空/错误/取消、关闭恢复并保存五张截图。线程、寿命和限制见 `docs/extensions-native-data.md`。旧模型的 `data_source`、`data_operation`、上下文解析仍未接通，不应与新绑定接口混用。

新增目录示例组合 `pulse.directories` 与 `pulse.context`：读取显式目录快照、分页、watch 合并失效后重刷、跟随已加载上下文，并通过 `pulse.ui.data` 使用已有数据控件。watch READY 后刷新以避免监听建立间隙；context 按 30 ms 合并并非逐次事件；`open_entry` 成功仅表示普通打开路由已接受。权限、线程、借用和 AI 作者提示见 `docs/extensions-native-directories.md`。该脚本保存的真实宿主结果独立于作者编译和假宿主测试。

文件操作示例组合当前选择复制/移动到另一可见窗格及移到回收站，使用宿主原生确认、任务进度与所属请求取消。回收站脚本验证实际回收与 Undo 恢复、默认取消、确认中的 SDK 取消、准备取消及停用清理，并保存五张应用内截图。接口不接收插件路径；回收站要求 1–64 个普通文件，拒绝目录与重解析点。线程、结果与路径身份限制见 `docs/extensions-native-fileops.md`。

显式目标示例组合 `pulse.directories` V1 与 `pulse.fileops` V3：既可传输当前选择，也可从同一 attachment 已打开源目录的当前快照按 entry ID 选择 1–64 个普通文件，复制/移动到已打开目标目录。V3 保留 V1/V2 函数表布局，准备时核对快照元数据并显示原生确认。隔离宿主验收通过 54 项、0 失败，覆盖复制/移动、过期快照、大小变化、源/目标关闭、取消和停用；报告与应用内截图见 `build-extensions/native-target-v3-20261106-01`。

目录工作台是开发者业务组合样例，不是 Pulse 内置场景命令。它使用公开目录/context/UI/data/mount/resources API，实现四类分类、双视图、筛选/排序、分页、真实目录 watch、CAS 设置与三种挂载。专用应用内脚本已验证真实控件操作、侧栏/工作区/窗口复用同一 HWND、停用清理及重开恢复，并保存五张真实面板截图；完整样例限制见其 README 和 [工作台指南](extensions-native-workbench.md)。

独立 stdio AI 作者工具见 [原生作者 MCP](extensions-native-author-mcp.md)。它可创建工程、调用 CMake/Ninja、生成并校验包、安装到独立作者测试库及比较宿主清单。`native_host_run` 只启动隔离配置宿主；包加载和交互仍需应用内验收脚本，不把进程启动当成功能验收。

仍未交付：桌面承载/接管、原生标签页和工具栏贡献、任意源目录文件操作及目录递归、永久删除、额外私有 DLL/资源侧载、完整图形包管理器、跨多个真实 Pulse 窗口的集成验收、搜索/预览/标签等新增 C++ 服务。独立 MCP 已实现作者构建/打包/测试安装/诊断/启动工作流；启动不证明插件交互，外部作者端到端发布治理仍需推进。结构声明和已完成示例不代表这些能力已经开放；目录失效 watch 也不承诺精确文件变化 journal 或任意 UI 模型 context resolver。

已开放接口、线程和寿命约束见 `sdk/native/README.md`、`sdk/native/api-catalog.json` 及公开头。`--extension-api` 的 `nativeSdk` 字段与 SDK 清单一致，供 AI/MCP 读取；读取清单不会授予执行权限。

每份 ZIP 均不可覆盖发布；`native-preview-manifest.json` 记录各文件 SHA256。实际验收报告另存于发布目录，不把作者编译成功当作功能验收成功。
