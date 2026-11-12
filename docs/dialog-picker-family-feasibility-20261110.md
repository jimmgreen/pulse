# WinRT / Windows App SDK picker 家族可行性与本机证据

日期：2026-11-10。本文将 API 契约、开发依赖、原生实验与 Pulse 接管状态分开；COM/OFN 的成功不外推为本页家族的成功。

## 1. 两个命名空间与版本边界

| 家族 | 入口与宿主关联 | 返回值 | 本次决定 |
|---|---|---|---|
| Windows SDK `Windows.Storage.Pickers` | WinRT 激活；桌面宿主先经 `IInitializeWithWindow::Initialize(HWND)` 关联 owner，再调用异步 `Pick*Async` | `StorageFile` / `StorageFolder`；取消为无项目或异步取消 | 保留平台 picker 与 storage 授权语义；Pulse 尚无等价授权适配 |
| Windows App SDK `Microsoft.Windows.Storage.Pickers` 1.8+ | 独立 App SDK runtime class；构造参数是 `WindowId`，不是双 FileDialog CLSID | `PickFileResult` / `PickFolderResult`，主要承载路径 | 单独版本化适配；本机开发 projection 未接入，不声称已验收 |
| 同命名空间 2.0+ 扩展 | 延续 WindowId；新增标题、状态标识、初始过滤器索引等能力 | 同类路径结果，但保存创建行为也有版本变化 | 固定目标 SDK/运行时版本并逐项协商，不用 1.8 的测试替代 2.x |

Windows SDK 的 FileOpenPicker 属于 UniversalApiContract v1，桌面 HWND 关联是显示前的独立要求；它不是 `IFileDialog` ABI 的别名。[FileOpenPicker API](https://learn.microsoft.com/en-us/uwp/api/windows.storage.pickers.fileopenpicker?view=winrt-26100)、[HWND interop](https://learn.microsoft.com/en-us/windows/apps/develop/ui/display-ui-objects)。

App SDK picker 在 1.8 引入，官方指南用 `WindowId` 构造并读取路径结果。2.0 release notes 另列 `FileTypeChoices`、`InitialFileTypeIndex`、`SettingsIdentifier`、`Title`、多文件夹、保存覆盖提示等增量，并记载 2.0 对新保存目标不再预建空文件。以 release notes 和逐成员版本为准；当前 API 索引即使带 1.8 view，也可能展示更新成员列表。[1.8+ 指南](https://learn.microsoft.com/en-us/windows/apps/develop/files/using-file-folder-pickers)、[2.0 变更](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0)。

## 2. Storage 对象、broker 授权与 elevation

- 旧 WinRT picker 返回可操作的 Storage 对象，包含从其他 provider 应用选取项目的模型；本地文件路径不是所有 storage item 的完整契约。[WinRT picker 工作方式](https://learn.microsoft.com/en-us/windows/uwp/files/quickstart-using-file-and-folder-pickers)。
- 用户通过 picker 选择可为应用授予原本没有的项目访问权；FutureAccessList 可保留该访问。本文“broker 授权”指平台为调用应用履行的 storage 访问契约，不通过某个窗口类、进程名称或路径存在来认定。[文件访问权限](https://learn.microsoft.com/en-us/windows/apps/develop/files/file-access-permissions)、[FutureAccessList](https://learn.microsoft.com/en-us/windows/apps/develop/files/track-recently-used-files-folders)。
- App SDK `PickFileResult.Path` 可供调用方尝试 `StorageFile.GetFileFromPathAsync`，但这个转换不是向任意 AppContainer 签发相同授权的证明。本机普通桌面进程已有文件访问权时读取成功，只能证明该进程的那次读取。[PickFileResult API](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.windows.storage.pickers.pickfileresult?view=windows-app-sdk-1.8)。
- 两个 namespace 的 API 备注明确区分 elevation：旧 `Windows.Storage.Pickers` 不支持管理员提升模型，新 App SDK picker 增加提升支持。本次宿主实测 `elevated=0, appcontainer=0`，并未执行管理员或 AppContainer 选取授权验收。快速入门当前有一处相反的 elevation 描述，本文按 namespace API 备注处理，保留独立实测要求。[Windows SDK namespace](https://learn.microsoft.com/en-us/uwp/api/windows.storage.pickers?view=winrt-28000)、[App SDK namespace](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.windows.storage.pickers?view=windows-app-sdk-1.8)。

**实现决策：** 不把 Pulse 路径结果伪装成平台 picker 已授予的 StorageFile，不将主进程路径可读视为受限调用者授权。WinRT 接入须在原调用者 apartment/异步生命周期保留 owner、取消、provider item 与授权；App SDK 可另做显式路径型适配，但仍需履行版本、WindowId、取消、过滤器和保存副作用契约。当前两个家族继续使用平台流程，尚未列入 Pulse 替换验收范围。

## 3. 本机开发与运行时清点

证据：`C:\Users\SS\Desktop\pulse\bench_data\dialog-next-20261110\picker-sdk-inventory.json`。

- 已安装 Windows SDK `10.0.26100.0`，含 C++/WinRT `Windows.Storage.Pickers.h`、ABI header、HWND interop header 和 x64 `windowsapp.lib`。
- 已查询当前用户注册的 App Runtime 包 24 项；包含 1.8 x86/x64，以及 `Microsoft.WindowsAppRuntime.2` 2.6.5 x86/x64。**运行时不是缺失状态。**
- 本机默认 NuGet 缓存无 `Microsoft.WindowsAppSDK` 开发包；该 SDK include 下无 `winrt/Microsoft.Windows.Storage.Pickers.h`。独立 include 编译探针给出 C1083、exit 2，见 `logs\appsdk-development-probe.txt`。这证明本次构建依赖未配置，不证明系统没有该 runtime class，也不证明任一任意应用已经具备正确 package graph。
- 未安装/升级系统包，未改产品入口，未为新家族登记第三方 COM。

## 4. 可复现的真实 Windows SDK 宿主

源码：`C:\Users\SS\Desktop\pulse\src\bench\winrt_picker_family_test.cpp`。该文件独立编译，不加入主程序或 runtime 的入口。

工作目录为 `C:\Users\SS\Desktop\pulse`，x64 MSVC 命令：

```powershell
cmd /c 'call scripts\vcvars.bat >nul && cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /I"C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\cppwinrt" src\bench\winrt_picker_family_test.cpp /Fo:bench_data\dialog-next-20261110\winrt_picker_family_test.obj /Fe:bench_data\dialog-next-20261110\winrt_picker_family_test.exe /link windowsapp.lib user32.lib advapi32.lib'
```

本次编译 exit 0、无新增 warning。首次编译缺少 `Windows.Foundation.Collections.h` 定义产生 C3779，补显式 include 后重新编译成功；原错误日志保留，未计为通过。

部署/运行复现（普通桌面 token，独立目录，不改变用户登记）：

```powershell
$deploy = Join-Path $env:LOCALAPPDATA 'Pulse\DialogBridge\dialog-next-winrt-family-20261110'
New-Item -ItemType Directory -Force $deploy | Out-Null
Copy-Item -LiteralPath 'C:\Users\SS\Desktop\pulse\bench_data\dialog-next-20261110\winrt_picker_family_test.exe' -Destination (Join-Path $deploy 'winrt_picker_family_test.exe')
& (Join-Path $deploy 'winrt_picker_family_test.exe') --storage-fixture 'C:\Users\SS\Desktop\pulse\bench_data\dialog-next-20261110'
```

`--storage-fixture` 只创建进程专属目录与固定内容测试文件，真实调用 `StorageFile.GetFileFromPathAsync` 和 `FileIO.ReadTextAsync`，随后删除文件并验证再次请求得到 `0x80070002`。实际结果：

```text
[PASS] StorageFile owned_fixture_read=1 picker_grant_verified=0 appcontainer_grant_verified=0
INFO missing_fixture_hr=0x80070002
[PASS] deleted StorageFile cannot become stale success
RESULT family=WINRT pulse_adapter=not_implemented reason=WINRT_STORAGE_GRANT_NOT_IMPLEMENTED test_passed=1
EXIT_STATUS=0
```

这不是 UI picker 返回项目的读取验收，更不是受限应用授权验收；宿主明确将两个 grant_verified 都置为 0。fixture 在结束时清理。

### 真实取消调用与原生对照

`--cancel` 创建独立 owner，HWND 初始化后发起**预先计数为 1** 的 `PickSingleFileAsync`；STA 消息泵在 750ms 请求 Cancel，内部 15 秒超时，外部进程 watchdog 30 秒。无显式 provider 标志时始终输出 `ui_shown=unknown`。

```powershell
$out = 'C:\Users\SS\Desktop\pulse\bench_data\dialog-next-20261110\logs\winrt-picker-cancel.txt'
$p = Start-Process -FilePath (Join-Path $deploy 'winrt_picker_family_test.exe') -ArgumentList '--cancel' -WindowStyle Hidden -PassThru -RedirectStandardOutput $out
if (-not $p.WaitForExit(30000)) { Stop-Process -Id $p.Id -Force; throw 'picker watchdog timeout' }
Get-Content -LiteralPath $out
$p.ExitCode
```

首次 automatic 开启状态的实测：异步调用返回 `0x80004002`，exit 1；未到达 Cancel 请求，**取消尚未通过**。本测试宿主没有匹配的 auto JSONL；同时段平台 `PickerHost.exe` 的 `dialog_auto-13688.jsonl` 有一次 `query-interface-not-supported`，IID `9EA5491C-89C8-4BEF-93D3-7F665FB82A33`。这是原生 broker 兼容候选，不凭时间邻近就判定根因。已将脱敏查询结果保存到 `logs\winrt-broker-auto-query.json`，并要求同机原生登记对照后再归因。此实验未自行改变全局登记，也未加进程名称名单。

主执行器随后临时关闭了当前本地 x64 helper，并在 finally 恢复当前登记；再次请求仍返回同一 HRESULT，但该次 broker 日志仍记录**另一旧安装**的 revision47 auto DLL 被激活。因此该实验尚未形成无接管的 native 对照，不据此判定当前新版造成失败；也未据此推断另一个注册上下文的来源。

为核对桌面与命令宿主的环境，另编译 `src/bench/desktop_context_observer.cpp`，只读 token、HKCU/HKCR 双视图与 `NtQueryKey` 原生 key name，并只写自身目录的 `desktop-context.txt`。经主执行器从真实桌面启动后报告显示相同用户 SID；当前 x64 指向当前本地包，x86 为原生状态。该观察**没有给出桌面与命令宿主注册上下文不同的证据**，不据此解释旧 broker 的来源。观察器没有 COM 激活或注册修改；部署文件为 `C:\Users\SS\AppData\Local\Pulse\DialogBridge\desktop-context-next20261110\observer.exe`，编译日志在 `logs\desktop-context-build.txt`。

## 5. 诊断报告与下一验收点

```powershell
python tools/dialog_diagnostics/report.py --logs bench_data/dialog-takeover-20261110 --family-status bench_data/dialog-next-20261110/picker-family-status.json --output bench_data/dialog-next-20261110/diagnostics-picker-family-report.json
```

`family_readiness` 独立给出 `WINRT_STORAGE_GRANT_NOT_IMPLEMENTED`、`APP_SDK_PICKER_ADAPTER_NOT_IMPLEMENTED` 和 `APP_SDK_DEVELOPMENT_DEPENDENCY_MISSING`。这些是实现/开发依赖证据，不计入实际请求、不生成入口覆盖分母，也不把原生失败说成自绘。原历史证据仍是 711 条未关联观察、0 个可关联 Show、覆盖率 null。

后续验收按独立工作包推进：

1. 先完成同机 WinRT native/automatic 对照，识别新 broker QI 兼容边界；未知 IID 的 native 透传须审查 controlling-IUnknown identity，而不是直接返回另一对象接口。
2. 用带明确 package identity 的受限测试宿主完成 picker 选取、StorageFile 实际读取、取消无旧项目、FutureAccessList 重开，以及跨 provider item 的授权验收。
3. 显式接入选定 App SDK 开发包及 package graph/bootstrap，固定 1.8 或 2.x 版本；分别测 x86/x64、普通/提升、取消、多选、WindowId 生命周期和保存后重开。
4. 只有对应家族的异步结果、宿主消费与授权全部有实测证据后，才把该单元格改为已验收。当前 x86/提升/AppContainer/App SDK picker 均保留未测，不从 x64 普通 StorageFile 读取外推。

## 6. 通用 COM 来电上下文的独立探针

源码 `C:\Users\SS\Desktop\pulse\src\bench\com_call_context_test.cpp` 不注册 COM 类：它在本进程创建 IClassFactory，经标准 marshaling 让 MTA 调入 STA，并在原 STA 上记录被动上下文。复现：

```powershell
cmd /c 'call scripts\vcvars.bat >nul && cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 src\bench\com_call_context_test.cpp /Fo:bench_data\dialog-next-20261110\com_call_context_test.obj /Fe:bench_data\dialog-next-20261110\com_call_context_test.exe /link ole32.lib uuid.lib user32.lib rpcrt4.lib'
.\bench_data\dialog-next-20261110\com_call_context_test.exe
```

实际构建和运行均 exit 0，见 `logs\com-call-context-build.txt`、`logs\com-call-context-test.txt`：

| 场景 | CoGetCallContext / CoQueryClientBlanket | context_present | RPC call attributes |
|---|---|---|---|
| 普通 STA 主动代码 | `0x80010117` / `0x80010117` | 0 | status 1725，无 active RPC call |
| **同一进程**跨 apartment 调用 | `S_OK` / `S_OK`，authn service 10、level 6、capabilities 0 | 1，且未 impersonate | status 1725，无 active RPC call |
| 本次 WinRT broker 中的同类调用 | 等待新 revision50 被动日志实测 | 尚未测量 | 尚未测量 |

`CoGetCallContext(IID_IServerSecurity)` 只描述当前线程当前 COM call；`CoQueryClientBlanket` 是对应 QueryBlanket 的 helper，二者都不是 broker 身份判定。`IServerSecurity` 不跨调用/不跨 apartment 保存，`pImpLevel` 必须传 null；本探针不请求 principal 或 privileges，不进行 impersonation。[CoGetCallContext](https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cogetcallcontext)、[CoQueryClientBlanket](https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coqueryclientblanket)、[QueryBlanket 生命周期](https://learn.microsoft.com/en-us/windows/win32/api/objidl/nf-objidl-iserversecurity-queryblanket)。

这次反例证明：“有 IServerSecurity”也包含正常同进程跨 STA 应用代码。如果将所有 incoming context 统一保留原生，这应是明确的通用能力边界，不应描述成识别到了某一个系统进程。

可继续被动观测 `RpcServerInqCallAttributesW` 的 V2 `ClientPID`。该字段只为 ncalrpc、显式 `RPC_QUERY_CLIENT_PID` 请求提供，RPC 返回失败时结构内容未定义，必须忽略；PID 不作为访问授权或永久进程身份。COM 是否在目标调用点提供 active RPC context 仍要实测，1725 不证明当前代码不在 broker 中。[RPC query API](https://learn.microsoft.com/en-us/windows/win32/api/rpcasync/nf-rpcasync-rpcserverinqcallattributesw)、[RPC V2 字段](https://learn.microsoft.com/en-us/windows/win32/api/rpcasync/ns-rpcasync-rpc_call_attributes_v2_w)。

auto server 当前只新增 revision50 的 `com_call_context`、`rpc_call_context` 数值诊断字段，未依据它们改变接管判断；字段不含客户端名称、令牌内容或输入文件名。RPC 查询动态取已加载 Rpcrt4 导出，不新增运行时加载或依赖配置；失败时只记录错误并忽略属性内容。普通 COM / 跨 STA / WinRT broker 三列证据齐全前，保持“测量”与“改变准入规则”分离。
