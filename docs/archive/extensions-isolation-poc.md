> 历史/独立实验参考：这里的隔离探针不是原生增强包执行层。当前同进程 C++ SDK 不承诺脚本沙箱，路线见 [平台契约](extensions-platform-contract.md)。

# ExtensionHost 隔离 PoC（P4，尚未开放脚本）

本目录是独立原生攻击探针，不是第三方脚本宿主。源码存在不表示隔离通过；必须记录目标 Windows 版本上的真实运行结果。当前未接入 JavaScript 引擎、manifest 脚本字段、host ABI 或扩展执行入口，也未下载依赖。源码检索仅发现 PDFium 公开头中的 V8 参数说明，不能据此认定 Pulse 已拥有可用于扩展的 JS 运行时。现有 AI、预览、Shell 子进程及 Job 管理不能替代 AppContainer 安全边界。

## 最终本机证据（2026-11-04，SDK 冻结时）

主任务统一构建并运行的普通无参数探针日志为 `build-extensions/platform-isolation-06.log`，退出码 **1**。以下 10 项 PASS：授权文件读回、秘密文件读取拒绝、文件写入拒绝、目录创建文件拒绝、私有 event 隔离、私有 pipe 拒绝、子进程禁止、外部地址连接明确拒绝、128 MiB 分配受 64 MiB 上限拒绝、关闭 Job 后忙循环进程被回收。结果只覆盖本机和受控 fixture，不是 Windows 支持矩阵或全系统访问审计。

回环网络 **INCONCLUSIVE**：端口 11566 的父进程 connect/accept 控制在子进程前建立、在子进程后仍 healthy；子进程连接超时。官方 NetworkIsolation 辅助诊断返回 `ERROR_SUCCESS`、`NETISO_ERROR_TYPE_NONE`（category 0），没有报告缺能力类别；该返回值既不证明连接被允许，也不证明本次 TCP 被隔离拒绝。日志没有明确 WSAEACCES 或可关联的 WFP 阻断事件，不能把“未连通”改写为“隔离通过”。该项继续计入失败，第三方脚本保持关闭。

当前实际门槛是取得回环连接的确定隔离证据并通过该项，然后完成下述 Windows/策略环境矩阵、CPU 行为和清理压力检查。即使原生探针全部通过，也仍需单独实现并验收受维护脚本引擎、受限 host ABI、调用方身份/撤权与消息边界，才可能考虑开放脚本；当前没有这些执行能力。

## 实现与构建集成

独立控制台目标只编译 `src/extension_host/isolation.cpp`、`src/extension_host/probe_main.cpp`，使用 C++20、Unicode、Windows 10 SDK（`_WIN32_WINNT=0x0A00`）、`/W4 /utf-8`，链接 `userenv.lib advapi32.lib ws2_32.lib`。该独立目标使用静态 MSVC runtime `/MT`，避免复制到隔离 fixture 的探针依赖工程目录的 CRT DLL；profile API 与可选网络诊断 API 按需从 System32 加载。主任务已完成构建集成与上述本机运行。不得为解决启动失败放宽整个用户目录 ACL。

运行无参数。退出 0 表示本次自动探针均通过；非零表示至少一个失败、环境不可用或清理失败，保持第三方脚本不可用。启动故障记录 API 阶段与 Win32/HRESULT。子进程诊断 `0x10000000 | (错误码 & 0x00ffffff)`；`ERROR_INVALID_ACCESS` 表示本应禁止的操作实际成功。网络超时、拒绝连接和无路由都不能当作访问控制通过。

每个用例创建独立临时 AppContainer profile 和零 capability token，通过 `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES` 启动。父进程核验 `TokenIsAppContainer` 及 profile SID；子进程再次核验 AppContainer 与空 capability 列表。启动带子进程禁止策略、无继承句柄、仅 LOCALAPPDATA / SystemRoot 的启动环境；先挂 Job，后恢复执行。失败不回退普通进程、低完整性进程或仅 Job。

Job 限制：单进程、64 MiB committed process memory、CPU hard cap 20%、关闭最后句柄终止。CPU cap 目前验证设置 API 成功，不把它称为吞吐量测量；内存和关 Job 回收另有真实行为用例。进程退出后删除 profile。临时文件夹使用新建唯一名称；只删除明确创建的文件，不递归清理路径。profile/文件清理失败列为失败。

## 自动探针与判定

| 用例 | 建立的对照 | AppContainer 预期 |
|---|---|---|
| read | 新建固定 marker；只给该 profile 文件读取/执行 ACL | 精确读回 marker |
| secret | 同一受控目录的 secret 使用独立受保护 ACL，只有用户/System | 读取明确 ACCESS_DENIED |
| write / create | 目录和 allowed 文件只给 profile 读取/执行 | 打开写句柄及创建新文件明确 ACCESS_DENIED |
| ipc | 父进程创建并可打开私有命名 event | 拒绝打开，或被 AppContainer 对象命名空间隔离 |
| pipe | 父进程创建私有本地 named pipe 并用正常客户端成功打开 | 禁止读写连接，明确 ACCESS_DENIED |
| child | 核验 NoChildProcessCreation 策略；尝试启动相同可执行文件 | ACCESS_DENIED 或 CHILD_PROCESS_BLOCKED；意外成功立即终止并失败 |
| network | 父进程本机 TCP listener，正常进程实际连接成功 | AppContainer connect 明确 WSAEACCES |
| external | 尝试连接文档保留地址 192.0.2.1:443，不携带应用数据 | 必须 WSAEACCES；超时/无路由记未验证失败 |
| memory | 父进程先成功 commit 128 MiB 再释放 | 子进程同请求被 64 MiB Job 上限拒绝 |
| spin | 子进程真实忙循环，先确认 400 ms 仍未退出 | 关闭 Job 后 5 秒内进程被回收 |

AppContainer 不是“所有文件都不可读取/写入”：Windows 可读系统资源及 AppContainer 自身 profile 可写区是平台行为。本探针证明受控用户 fixture 的授权读、未授权读写和 IPC 对象边界，不宣称全系统零访问。零网络 capability 依赖 Windows 网络隔离；存在 loopback exemption 或特殊系统策略时，探针可能失败，必须保留该诊断，不能添加 exemption 使测试通过。

## 后续验收计划与仍未满足条件

回环超时后的辅助诊断使用官方 [NetworkIsolationDiagnoseConnectFailureAndGetInfo](https://learn.microsoft.com/en-us/windows/win32/api/netfw/nf-netfw-networkisolationdiagnoseconnectfailureandgetinfo)（Windows 8+，Firewallapi.dll，SDK 26100 声明已核对）。在同一个零 capability AppContainer 子进程中，仅当真实 loopback connect 超时后按需从 System32 加载并调用目标 `127.0.0.1`；返回 ERROR_SUCCESS 时记录 NETISO_ERROR_TYPE 0/1/2/3，分别为无错误/缺 privateNetworkClientServer/internetClient/internetClientServer。API 加载/调用失败另行记录，仍不通过。该接口接受目标 IP，没有 socket/端口参数，说明的是调用方对目标的能力诊断，**不能替代本次 TCP 连接被拒绝的运行时事件**。即使返回缺能力类别也保留 INCONCLUSIVE/非零，不自动升级 loopback 为 PASS；不修改系统审计、防火墙或豁免。

`platform-isolation-05.log` 在 DETACHED_PROCESS 修复后有 10 项 PASS：授权读、秘密读拒绝、写/创建拒绝、event/pipe、子进程、外网、内存上限和 Job 回收。回环网络仍为 `0x1000274C`（WSAETIMEDOUT），**不是 PASS**，整体退出非零且脚本门禁保持关闭。端口链路为 getsockname→ntohs→十进制参数→htons，未发现字节序错误；原控制组有真实 connect，监听 backlog=4，单个未 accept 控制连接不足以证明队列耗尽。现补控制组 accept/关闭及子进程结束后的再次 connect/accept，日志记录端口和容器 SID；超时显式标为 INCONCLUSIVE，并继续计入失败。只读查询运行附近 Security 5152/5157 未获得对应阻断事件；回环豁免列表的存在也不能证明本次 SID 被豁免或被拒绝。没有据此推定 Windows 静默丢包或安全软件根因；只有明确拒绝/可关联运行时 WFP 证据才能升级结论，不启用豁免或弱化断言换取通过。

已取得直接系统证据：`platform-isolation-conhost-events.xml` 保存 Security-Mitigations/KernelMode 的事件 4，精确匹配两档预算和 profile 对照的 fixture/PID。其中 profile 对照 PID 18280 在 2026-11-04 01:10:07 UTC 尝试启动 `conhost.exe 0xffffffff -ForceV1`，被禁止子进程策略阻止；另两档 PID 35460/50324 亦相同。这解释了为何仅加载启动系统 DLL 后退出；没有证据指向缺少其他环境变量或安全软件。PE 检查为 AMD64/PE32+、控制台子系统 3、DLL characteristics 0x8160（含 ASLR/NX），栈保留 1 MiB/提交 4 KiB。现将 `CREATE_NO_WINDOW` 改为 `DETACHED_PROCESS`，避免无控制台需求的 worker 触发 conhost 初始化；**保留** AppContainer、禁止子进程及所有 Job/ACL 限制。微软 [Creation of a Console](https://learn.microsoft.com/en-us/windows/console/creation-of-a-console) 明确 DETACHED_PROCESS 创建的控制台程序不连接控制台。修复仍须同环境正常无参数模式的全部断言复跑，事件证据本身不是安全验收通过。

64/128 MiB 加载跟踪均只记录 probe、ntdll、kernel32、KernelBase，随后退出 `0xC0000142`，无异常事件或调试字符串；两档一致，暂无启动内存不足证据。新增 `--diagnose-loader-profile` 保持 64 MiB，仅对照固定的 Windows profile 环境项 APPDATA、HOMEDRIVE、HOMEPATH、ProgramData、SystemDrive、TEMP、TMP、USERPROFILE，以及由 Windows 目录生成的 windir 和仅含 System32 的 PATH；保留原 LOCALAPPDATA/SystemRoot。只输出变量是否存在，不记录值，不继承其他环境或凭据。该模式同样固定返回 2，不能算隔离 PASS，也不修改正常启动环境。如果成功，应继续逐项缩减以确定必要项；若仍失败，不能继续无证据扩大环境或权限，应转向更详细的系统 loader 诊断。

第四轮移除 USERENV 静态依赖后仍返回 `0xC0000142`，因此不能把 USERENV 记为根因或已修复。新增独立诊断入口 `pulse_extension_isolation_probe.exe --diagnose-loader`（64 MiB）和 `--diagnose-loader-128`（仅启动预算对照）。两者只运行授权读 fixture，记录 Windows debug events 的模块路径、异常代码/参数、调试输出和退出码，最多 10 秒/4096 个事件；保留 AppContainer、零 capability、ACL、禁止继承句柄和子进程限制。两种诊断模式固定返回 2，不输出安全验收 PASS；debug port 和 128 MiB 预算均不属于正常验收配置。正常无参数模式仍执行全部边界断言并保留 64 MiB 上限及 128 MiB 分配失败测试。模块加载日志本身不证明某个 DLL 的初始化失败，需结合异常或 loader 输出定位；若信息不足，继续收集系统 loader 诊断，不放宽 ACL 或切换普通进程。

第三轮 `platform-isolation-03.log` 已越过进程创建阶段，但所有子进程均以 `0xC0000142`（`STATUS_DLL_INIT_FAILED`）退出，仍未通过任何隔离断言。该状态只说明 DLL 初始化失败，不能单凭退出码指定失败 DLL、缺少的环境变量或桌面权限。实际 PE 静态依赖包含仅父进程使用的 `USERENV.dll`；当前将 profile API 改为父进程按需从 System32 加载，子进程不再承担该依赖的初始化，并输出明确启动失败诊断。待重编后核对 import table 不含 USERENV，再在原正常用户环境复跑。此处未改变环境白名单、桌面、ACL、能力和 Job 限制；若仍失败，需要 loader/系统诊断证据定位具体 DLL，不能把初始化失败计作拒绝访问成功。

启动环境回归：`platform-isolation-02.log` 中 profile 创建成功，但全部模式在 `CreateProcess-AppContainer` 返回 `0xCB`（`ERROR_ENVVAR_NOT_FOUND`），因此该次运行没有验证子进程隔离。原实现仅传 `SystemRoot`，现补入启动 profile 解析所需的 `LOCALAPPDATA`，仍使用仅这两项、排序且双空字符结尾的显式 Unicode 环境块；不继承 PATH、凭据或其他调用方变量。微软说明 AppContainer 启动会将 `LOCALAPPDATA` 重定向到自身 profile（见下方官方启动文档）。缺少调用方 `LOCALAPPDATA` 时在 `environment-LOCALAPPDATA` 阶段明确失败，不回退普通进程。第三轮确认补充此项后进程创建成功，但出现上述 DLL 初始化失败；仍须所有边界断言通过才能确认隔离结果。环境路径本身不授予文件权限，原 token、ACL、零 capability、禁止句柄继承和 Job 限制保持不变。

1. 在 Windows 10/11 支持矩阵、标准用户、已有父 Job、受保护安装目录和不同安全软件策略下运行，记录版本、架构、全部输出和退出码。Windows 8.1 的原产品兼容性不等于本 PoC 可用：子进程策略等 API 未满足时明确不可用，需单独决定最低版本。
2. 复测 AppContainer 私有 profile 写区、系统可读资源白名单、其他用户资源、命名管道客户端身份与多 profile 互访；当前私有 event/pipe 不是完整 broker IPC 协议审计。
3. 后续若新增 host ABI，必须另测包身份绑定、消息长度/类型/版本、超时、撤销、重放、句柄传递和 confused-deputy 防护。本 PoC 没有授权 broker 通道，只有进程退出状态回报，不能作为 ABI 已安全的证据。
4. 再选择受维护 JS runtime 并明确更新策略，关闭本地模块/动态库/任意系统 API，验证引擎内存耗尽、异常和长循环；这些设置也不能替代 OS 安全边界。
5. CPU cap 压测、并发 profile、异常退出和 profile 清理需专项记录。任何自动用例失败或环境不可用，继续只开放声明式增强包。全部探针通过也不等于可发布第三方脚本。

## 微软官方依据（2026-11-04 核对）

本轮只读 WFP 实测记录：`build-extensions/workspace-loopback_evidence_test-01.log` 的 15 项匹配边界检查通过，
仅证明生产匹配函数的过滤规则。`workspace-isolation_probe-01.log` 在受限运行环境创建 AppContainer profile 时返回
`0x80070002`，退出 1，未到隔离断言。经自动审批允许使用正常用户配置中的临时容器后，
`workspace-isolation_probe-02.log` 有 10 项 PASS，但回环仍为 INCONCLUSIVE、整体退出 1：
父进程连接前后对照健康，WFP 输出 `subscribed=0 error=5 capabilityDrop=0 settingsChanged=0`，
连接超时且 NetworkIsolation 返回 `ERROR_SUCCESS/category=0`。
该结果表明本次观察器未能获得可用事件证据，不证明回环已被明确阻止；不能把匹配器通过或其他十项通过当成整组通过。
本轮不继续请求管理员权限、不修改系统防火墙/事件收集/审计设置。P4 证据缺口保留，第三方脚本门禁继续关闭。

新增只读回环证据观察器 `loopback_evidence.cpp`：父进程动态加载 Windows 8 起提供的
`FwpmNetEventSubscribe1`，先查询既有事件收集状态，不开启收集、不改防火墙或审计设置。
订阅条件限定探针应用 ID、TCP、127.0.0.1、本次监听端口及 `CAPABILITY_DROP`；回调还要求字段标志完整、
本次 AppContainer SID、应用 ID、端口和时间匹配，且事件明确 `isLoopback=true`。
仅保留匹配事件的 filter ID 和能力类型；不保存无关事件、包或网络载荷。
父进程前后连接健康对照仍必须通过。普通分类丢弃、订阅拒绝、收集关闭或没有匹配事件均不能将超时升级为通过。
观察器兼容性不改变完整 AppContainer/Job 探针对系统版本的要求，也不开放脚本门禁。

`extension_loopback_evidence_test.cpp` 使用生产匹配函数检查 SID、程序、端口、地址、时间、协议、字段标志和事件类型边界；
这些合成事件测试不是本机网络拒绝证据。真实结论须另存实际探针日志及退出码。
订阅需要事件容器的 `FWPM_ACTRL_SUBSCRIBE` 权限；引擎打开成功不代表订阅获准，不自动请求提升。
取消订阅先等待回调退出；若取消和关闭会话均失败，保留这唯一订阅的私有状态至探针进程退出，避免回调访问已释放内存。

- [FwpmNetEventSubscribe1](https://learn.microsoft.com/en-us/windows/win32/api/fwpmu/nf-fwpmu-fwpmneteventsubscribe1)
- [WFP 事件过滤条件](https://learn.microsoft.com/en-us/windows/win32/api/fwpmtypes/ns-fwpmtypes-fwpm_net_event_enum_template0)
- [AppContainer 能力拒绝事件](https://learn.microsoft.com/en-us/windows/win32/api/fwpmtypes/ns-fwpmtypes-fwpm_net_event_capability_drop0)

- [AppContainer isolation](https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-isolation)：文件、对象、网络安全边界。
- [Launch an AppContainer](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer)：profile/SID、能力与进程启动属性；不授予网络 capability。
- [CreateAppContainerProfile](https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-createappcontainerprofile)：每用户 profile、失败 HRESULT、SID 生命周期。
- [UpdateProcThreadAttribute](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute)：SECURITY_CAPABILITIES 与 CHILD_PROCESS_POLICY；只在实际支持的系统采用。
- [Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)：资源管理与进程组终止；本实现不把 Job 本身视作安全沙箱。
