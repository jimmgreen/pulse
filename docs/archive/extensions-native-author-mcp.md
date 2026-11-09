# C++ 增强包 AI 作者工具

`tools/extensions/native_author_mcp.py` 是独立的 stdio MCP 作者服务。它使用同一公开 SDK、CMake 工程及包作者 CLI，提供真实创建、编译、打包、测试安装和诊断。Pulse 内置的 `describe_extension_api` 等发现工具继续只读；完整编译环境放在作者服务中，不搬入文件管理器 UI 线程。

在 x64 Visual Studio 开发终端中，将以下命令配置为 MCP 客户端的 stdio server：

```powershell
python tools/extensions/native_author_mcp.py --workspace C:/PulseAuthor --sdk-root C:/PulseSdk --pulse C:/PulseSdk/bin/Pulse.NativePreview.exe
```

`--workspace` 是本次作者工作目录，`--sdk-root` 是解压 SDK 根目录，`--pulse` 是匹配的宿主路径。需要 MSVC x64、CMake、Ninja 和 Python 3。服务继承启动环境；没有编译器时返回 CMake 诊断，不伪造编译成功。

| 工具 | 实际行为 |
|---|---|
| `native_sdk_describe` | 列出接口摘要和可复制示例；传 `interface` 获取单个完整公开头契约，减少 AI 重复读取量 |
| `native_project_create` | 将一个公开示例复制到 `projects/name`；已有目录拒绝覆盖 |
| `native_project_build` | 用公开 include 路径执行 CMake/Ninja；返回构建目录、DLL 和编译诊断 |
| `native_package_stage` | 绑定 DLL SHA256，生成清单、打包并校验 `.pulse-ext` |
| `native_package_install` | 安装到本作者目录的 `.pulse-author/extensions`；`enable=true` 可启用供下次启动加载 |
| `native_host_diagnose` | 比较宿主与源码 SDK 清单，返回明确的版本不匹配结果 |
| `native_host_run` | 启动使用独立配置的测试宿主，返回 PID 与日志路径；启动接受不等于 DLL 或界面验收成功 |

安装和运行要求调用参数 `allowNative=true`。新 CMake 工程在构建期间也会执行作者代码。路径限定于服务启动时配置的作者工作目录，SDK 模板只从配置的 SDK 读取；测试存储与日常 Pulse 存储分开。原生 DLL 的信任模型与 SDK 一致，不把目录约束宣传为进程沙箱。

服务支持 `initialize`、`ping`、`tools/list`、`tools/call` 和 `notifications/cancelled`。构建/CLI 调用串行执行，发现仍可响应；取消构建会终止该命令进程树，单命令最长 300 秒，CLI 校验最长 40 秒。已启动并返回 PID 的独立测试宿主由用户正常关闭，不能用已经完成的 MCP 请求取消它。

每条结果包含 `ok`、结构化业务结果、实际命令、退出码、耗时与最多 32768 字符诊断尾部；`outputTruncated` 明确标记截断。宿主清单解析另有 2 MiB 上限。失败会保留诊断，不自动清理项目或改写源文件；取消不回滚已经生成的构建产物或已提交安装。

推荐 AI 按顺序执行：发现需要的接口 → 复制 `directory-workbench` → 修改 DLL 业务代码 → 编译 → 打包校验 → 测试安装/启用 → 诊断匹配 → 启动观察。应用内验收见目录工作台脚本与报告；不能用编译、安装或启动成功代替交互验收。

独立 stdio 链路验收（从仓库根目录，在 x64 Visual Studio 开发环境运行）：

```powershell
python tools/extensions/test_native_author_mcp_workflow.py --pulse build/pulse.exe --sdk-archive <native-sdk.zip> --output <new-output-directory>
```

该脚本通过 MCP initialize/list/call 请求，从独立解压 SDK 开始，执行样例构建、同目录 DLL staging、打包校验、隔离测试库安装/启用、宿主 SDK 清单比较及隔离宿主启动。脚本结束前持有启动宿主的 Win32 进程句柄，清理后必须等待退出信号；清理失败会让报告失败，不能把进程查询报错当作已退出。它只验证 host launch 和清理；工作台 DLL 的实际加载和交互由 `test_native_workbench_ui.py` 单独验收。输出目录必须不存在。

```text
请使用 Pulse 原生作者 MCP，在给定 workspace 内创建一个 C++ 增强包。
先查询所需接口的完整契约，只使用公开头；业务算法留在 DLL。
运行 native_project_build，依据实际诊断修复后再 stage 和 test install。
明确处理 API 的异步、取消、所有权与部分结果；挂载读取实际位置。
最终报告开发者新增能做什么、真实运行结果和仍未实现的能力。
```
