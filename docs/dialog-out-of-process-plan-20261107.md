# 对话框改为在 pulse.exe 中显示：实施计划（2026-11-07）

## 决定

- 架构：对话框界面只在 Pulse 自己的进程里显示（`pulse.exe --picker-host`），调用程序里只留 COM 包装层（`PulseDialogAuto64.dll` / runtime）。不再把整套 UI（`PulsePickerUiProof.dll`，约 7.2 MB）加载进 Word、QQ 等进程。
- 第一个里程碑：基础的打开、保存、选择文件夹、多选、过滤器和取消走进程外；用到事件订阅（Advise）或自定义控件的对话框暂时继续走进程内，直到双向协议（见 `dialog-runtime-split-plan-20261105.md`）覆盖它们。
- 理由：真正的 Pulse 界面、没有重复的 UI 代码（安装包预计少约 2.7 MB）、对话框崩溃不会拖垮调用程序。代价是 Pulse 未运行时首次弹出更慢，以及以后要做的跨进程回调协议。

## 现状（读代码得出）

- 全局接管：当前用户 COM 覆盖 `CLSID_FileOpenDialog` / `CLSID_FileSaveDialog`（仅 x64）→ `file_dialog_auto_server` → `file_dialog_wrapper`。
- `FileDialog::Show`：有进程内 provider 时优先进程内；进程外（`run_bridge` / `RunPickerClient`，协议 v1）只在没有 provider 时使用，而且 Save（`LocalSaveV1`）、自定义标题/按钮/文件名标签、Open 默认扩展名都会退回原生。
- 宿主（`picker_host_mode.cpp`）用的就是 `ui::ShowFilePickerDetailed`，与进程内是同一套界面；但窗口没有 owner，每次对话框都新起一个 pulse.exe。

## 分阶段

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 协议加 `LocalSaveAccessV1`（及 Save* 选项）和 `DialogLabels`；宿主按进程内 provider 相同的规则映射到 `FolderPickerSpec`；本地保存在显示前预解析目标，失败回 Unsupported | **完成**：`picker_protocol.*`、`picker_bridge_model.cpp`、`picker_host_mode.cpp`；协议测试新增 21 项、bridge model 新增 3 项全部通过 |
| 2 | wrapper 对基础对话框（无订阅、无自定义控件、无属性、无 places）先按宿主能力准备 contract，`BridgeRequestFor` 生成带本地保存与标签的请求后走 pulse.exe；宿主带不了的（扩展原生选项、FOS_SHAREAWARE、Open 默认扩展名、允许不存在的 Open）回到原来的进程内路线。开关：`HKCU\Software\Pulse\DialogAuto` 的 DWORD `Host`，缺省或非 0 走 pulse.exe，0 全部进程内 | **完成**：`file_dialog_contract.*`、`file_dialog_wrapper.*`、`dialog_auto_registration.*`、`file_dialog_auto_server.cpp`；wrapper 测试新增 4 项，contract 116/0、wrapper 119/0 |
| 3 | `file_dialog_auto_server` 在开启 pulse.exe 路线时不再于 Show 前加载 `PulsePickerUiProof.dll`，改为向 wrapper 提供 `load_inprocess_provider`；只有离开 pulse.exe 路线（事件、自定义控件、属性、places 或宿主带不了的设置）的 Show 才加载，加载失败则该次 Show 走原生；已标记原生的对话框不加载；对话框对象复用时仍可捕获设置。`Host`=0 时行为与之前完全相同 | **完成**：`file_dialog_wrapper.*`、`file_dialog_auto_server.cpp`；wrapper 测试新增 3 项（125/0），进程内 wrapper 测试 1172/0 |
| 4a | 窗口归属与前台：客户端把调用窗口句柄作为 `--picker-host` 的可选第 3 个参数传给宿主，并 `AllowSetForegroundWindow(宿主 pid)`；宿主核对该窗口属于父进程后，作为新的 `FolderPickerSpec::placement_owner` 使用（只决定 owner、显示器/DPI 和居中，不启用/禁用、不观察；不能再建第二个 `PickerOwnerGuard`，因为调用方的 guard 已占用该窗口）。模态和 owner 消失时取消仍由调用方的 guard 负责 | **完成**：`picker_client.cpp`、`picker_host_mode.*`、`app_main.cpp`、`folder_picker_dialog.*`；真实宿主测试新增“归调用窗口所有”“在调用窗口所在显示器”断言，140/0、wrapper 模式 32/0；客户端测试 38/1（失败项走的是 Exchange 之前的 guard 路径，本次改动够不到，见下） |
| 4b | 常驻宿主（standby）：`pulse.exe --picker-standby` 预加载后在“用户+会话+桌面”专属的会合管道上等一个请求；客户端先试它，没有则照旧冷启动。会合管道只用来递交一个一次性私有管道名（与冷启动同一种、名字里带创建者 pid），客户端核对该 pid 的用户、会话和映像必须是自己会启动的 pulse.exe，再用现有 `Connect` 校验；会合管道立即关闭，后继用 FIRST_PIPE_INSTANCE 接手名字。每个 bundle 每个桌面只有一个空闲 standby；注册不再指向该 bundle（升级/关闭）时一分钟内自行退出。调用窗口改为协议内 `CallerWindow` 字段传递 | **已实现并测试，但默认不启用**：见下方“4b 实测结论” |
| 5 | 双向协议：事件、veto、自定义控件、属性（按 11-05 计划），完成后移除 `PulsePickerUiProof.dll` | **进行中**：5a 显示前会话能力协商已实现，验证见下；事件、重入和控件尚未迁移 |
| 6 | 覆盖面：x86 包装层、提权进程（HKCU 覆盖对其无效）、传统 `GetOpenFileName` 实测 | 待做 |

## 4b 实测结论

- 功能：真实宿主测试 `--standby` 模式 53/0（首个请求由 standby 处理，第二个由它启动的后继处理，再由下一个接手）；传输测试新增 8 项 77/0；协议测试新增 4 项。
- 第一版用“多实例共享一个管道名”，结果 `GetNamedPipeServerProcessId` 对后继的实例报告了正在退出的前任 pid，客户端把连接绑到了将死的进程上，偶发失败。改为“会合管道 + 私有管道”后稳定。
- **速度没有改善**：同一测试里 standby 与冷启动到“输入框可用”都在约 300–650 ms。分阶段计时显示，无论冷热，对话框 `CreateWindowExW`（即 `WM_CREATE` 里的初始化）都要 220–400 ms，占到首帧的大头；进程启动本身不是瓶颈。standby 只预加载 DLL，省不掉这部分；后继与当前对话框同时启动时还会争 CPU（热路径有两次 312/406 ms）。
- 因此主程序启动时**不再**拉起 standby（`SeedPickerStandbyAsync` 保留但不调用），避免常驻一个没有收益的进程。客户端找不到 standby 时立即（实测 <150 ms 内，通常 <1 ms）走冷启动，无额外代价。
- 下一步若要真正提速：先拆解 `WM_CREATE` 的 220–400 ms（D3D/DComp 设备、字体、侧栏位置枚举、图标等），这对进程内、进程外和 Pulse 自己的选择器都有效；之后若 standby 能预先建好设备/窗口，再决定是否启用。
- 测试钩子：`PULSE_PICKER_STANDBY_TEST`（仅 `PULSE_WITH_TEST_HOOKS` 构建）让 standby 不依赖用户 HKCU 注册即可运行，只影响存活判断，客户端仍做全部校验。

## 阶段 2 的一个取舍

现有测试 “late host Unsupported cannot reopen native dialog” 规定：请求一旦发给宿主，宿主的任何回答都是终态，不能再弹原生窗口（调用方无法证明宿主没有显示过界面）。阶段 2 保留这条规则，改为在调用方选路前先跑一遍宿主相同的本地保存目标检查（`BridgeSaveTargetResolves`，同一个 `ResolveSaveTarget`），宿主会拒绝的目标根本不走 pulse.exe。

尚未在真实程序里验证；需要新构建的安装包（或 build 目录）并开启自动接管后，用记事本、Word、Excel、QQ、微信做打开/保存/取消。窗口位置和前台交接属于阶段 4，验证时可能看到 Pulse 窗口不在调用程序正上方。

## 验证纪律

- 每个阶段先用协议/模型/contract 定向测试覆盖，再做真实程序（记事本、Word、Excel、QQ、微信）的用户验收；构建成功不等于兼容。
- 已显示 Pulse 界面后的任何失败都不能再弹出原生窗口；只有宿主在显示前明确拒绝时才可以退回原生。
- `pulse_picker_client_test` 的 “failed guard cannot remove another owner's property or enable it” 失败：该用例在 `RunPickerClient` 构造调用方 guard 时就返回 ERROR_BUSY，不会进入阶段 4a 改动的 `Exchange`；`picker_owner_guard.cpp` 另有他人未提交的焦点恢复改动。未证明是既有失败，但与本次改动无代码路径关联，待查。
- 客户端测试的假宿主原先只接受 `--picker-host <pipe> <pid>`；多出 owner 参数后它会把自己当成测试主程序递归运行，已改为也接受第 3 个参数。
- 已知既有失败：`pulse_picker_protocol_test` 的 “invalid model preserves encoded output” 在本次改动前的二进制上就已失败（某个 `Reject` 用例现在能编码），与阶段 1 无关，待查。

## 阶段 5a：显示前会话协商（2026-11-08）

- 在原 v1 envelope 尾部追加 `SessionHello` / `SessionWelcome`，不改变原 Request/Response/Cancel 的字节布局。协商选择 session version 2，目前只公布 `kSessionBasic`；这不是完整 V2 UI API，事件、veto、自定义控件和属性仍按原规则进程内处理。
- 新客户端在冷启动和 standby 路径先交换版本范围、所需 features 和 Request capability mask，再发送 Request。身份和进程存活仍由原有认证 Channel 绑定；同一个非零 request ID 标识本次 Show，协商后的 Request capability mask 必须与 Hello 完全一致。
- 宿主共享 `ReceivePickerSessionRequest`，只有协商成功且请求校验通过才准备 UI；未知所需版本/能力在 UI 前明确拒绝。新宿主继续接受旧客户端直接发送的 v1 Request；新客户端遇到不识别 Hello 的旧宿主返回失败，不盲目重试。安装包中的客户端和宿主应成对发布。
- capability mask 表示协议字段支持，具体 FOS/路径/过滤器组合仍由 `PreparePickerBridge` 检查，不把协商成功当成所有配置都可显示。
- standby 已连接后的发送失败也作为终态，避免失败后冷启动重复提交可能已经送达的请求。standby 默认仍关闭。
- 下一步：增加带 generation 的事件快照与回调回复，以及调用 STA 上的分发和 Query/Close 重入；完成这些验证前不公布事件或控件能力，也不移除进程内 UI DLL。
- 定向构建：`pulse`、`pulse_dialog_auto`、`pulse_picker_protocol_test`、`pulse_picker_session_test`、`pulse_picker_client_test`、`pulse_picker_host_test` 成功。
- 实测：`pulse_picker_protocol_test --session` 676/0；`pulse_picker_session_test` 83/0；`pulse_picker_client_test --session` 17/0；真实 `pulse_picker_host_test <pulse.exe> --session-cold` 39/0、`--session-standby` 24/0；旧协议 `--local-open` 修改前后及回滚副本均 135/0。
- 首轮真实管道测试发现帧头校验仍只接受三种消息，已改为与 Message variant 数量一致，重跑通过。原有综合 `--wrapper` / `--standby` 各有 21 项失败：包装层桥接入口未触达（gate=0），返回 `0x8007007e`。原因尚未确证，原测试与失败日志保留；新增 session 定向入口不调用 COM 创建，因此这些通过结果不代表 COM 包装层或 Word/QQ 等真实程序兼容性通过。
- 本轮增量差异、原始哈希、回滚脚本和完整命令见 `bench_data/session-negotiation-20261108/VERIFICATION.txt`。工作区保留修改；回滚仅在独立副本执行。

## 阶段 5b：事件协议与调用 STA 回调基础（2026-11-08）

- 新增 `SessionEvent/EventReply/SessionCommand/CommandReply/SessionTerminal`（wire kinds 6–10）。快照含 generation、host HWND、folder/filename、多选与候选路径；回调与命令分别绑定递增 ID，精确保留 HRESULT。严格检查字段组合、UTF-16、路径数量和帧大小。
- `PickerEventState` 建模同步等待期间的 Query/GetWindow/Close、FileOk/FolderChanging veto、generation 失效和 first-Close-wins。相同 generation 必须携带相同完整快照；过期候选不接受，Close(S_OK) 不产生 Accepted 结果。
- `RunPickerEventClient` 在调用 STA 分发回调，管道 IO 在短生命周期 worker 上等待；Query/GetWindow/Close 可在回调内同步往返，等待期间泵窗口消息，不递归分发事件。验证事件 HWND 属于已认证宿主 PID；错误 ID、过期终态和替换候选被拒绝。保留 WM_QUIT，连续窗口消息也逐次检查取消。
- 真实子进程 fixture 使用认证 Channel 和真实隐藏 HWND，在隔离桌面验证回调、veto、代际变化、Close、异常、断管、错 HWND、嵌套事件、取消和 WM_QUIT。它不是 Pulse UI 或 COM sink 的端到端验证。
- **尚未生产接入**：能力仍只公布 Basic，未把 Advise 路由到进程外。下一步将真实 picker 事件/快照接到宿主收发与命令调度，并把 wrapper 的 sink 分发、Query/GetWindow/Close 适配到此调用 STA 层；之后继续 Overwrite/ShareViolation、自定义控件和属性。不要把本轮基础组件完成当作阶段 5 完成。
- 实测：事件 codec 133/0、状态机 58/0；原 session codec 676/0、认证管道协商 83/0。跨进程 STA 回调客户端 112/0（含持续消息取消）；新 codec/客户端对上一轮 host 的冷启动兼容检查 39/0。独立测试、COM 代理和宿主测试目标构建成功。
- 主程序增量构建遇到运行中 `pulse_shell.exe` 占用，链接报 LNK1104；未终止用户进程，未宣称主程序构建成功。独立回调测试和其他目标结果详见 `bench_data/session-events-20261108/VERIFICATION.txt`。旧 COM 综合测试失败未在本轮处理。

## 阶段 5c 前置：真实事件适配边界（2026-11-08）

- 新增 PickerEventAdapter，将真实 PickerDialogSnapshot 的 HWND、generation、UTF-16 字段和完整多选/候选数组映射到事件状态机。支持 Ready 后的 Snapshot、FolderChanging/Changed、SelectionChanged、TypeChanged、FileOk；拒绝字段不一致与未实现控件事件。
- 定向目标 pulse_picker_event_adapter_test 构建成功，37 checks / 0 failures。
- 接线前发现两个明确差距：真实 UI 在 Ready 前有可 veto 的初始化事件，当前首事件 Ready 协议不能覆盖；协议 Retry/veto 自增 generation 与真实 UI generation 尚需协调。适配层显式返回 UnsupportedBeforeReady 或 Invalid，不静默吞事件。本轮没有把它挂到真实 picker，不启用事件生产能力，阶段 5 仍未完成。
- 本轮证据与回滚位于 bench_data/session-adapter-20261108；上一轮缺失的 session-events-20261108/VERIFICATION.txt 已补齐。

## 阶段 5c：V2 远程宿主与 provider 验证（2026-11-08）

- `file_dialog_remote_provider` 与 `picker_remote_host` 已接入 `pulse.exe`：已实现认证父进程、会话 Hello、STA 消息泵、双向事件/命令、Query/GetWindow/Close、Overwrite/ShareViolation、custom control/property、断管和 owner/PID 校验。真实 UI 验证仅覆盖下列明确列出的用例；Overwrite/ShareViolation、异常断管与生产 COM 接线不据此宣称通过。
- 修复了远程 Show 终态 HRESULT、Terminal 绑定、Query 快照回传、重入期间 Shutdown/Show 忙状态，以及跨进程 property 测试中错误传递调用方窗口属性指针的问题。属性事件现在经过远程安全读取和协议深拷贝。
- 定向远程 UI 验证：ready-close 41/0、query 46/0、reentrant-owner 42/0、custom-update 52/0、initial-empty 48/0、startup-folder-close 33/0、startup-folder-cancel 28/0、startup-folder-error 32/0、property-edit 46/0、property-mutate 49/0、close-ok 41/0、cross-thread-owner 42/0。
- 主程序与 `pulse_dialog_auto` 均完成隔离构建；常规 `pulse` 目标另有运行中的 `pulse_shell.exe` 文件锁，使用不覆盖该文件的 `remotecheck.ninja` 完成了主程序链接。
- 本阶段仍不把事件 capability 对外公布：生产 wrapper 的事件/Advise 路由和 `PulsePickerUiProof.dll` 移除仍待完成。阶段 6 的提权、跨架构安装包和真实第三方应用验收仍待完成。

## 阶段 5d：生产接线（开关默认关闭）与远程宿主就绪问题（2026-11-08）

- **接线**：`pulse_dialog_auto` 现在编入 `file_dialog_remote_provider` 与 `picker_remote_protocol`。`file_dialog_auto_server` 的按需加载在 `HKCU\Software\Pulse\DialogAuto` 的 DWORD `RemoteHost`=1 时先用远程 V2 provider（同一调用 STA 线程上，只要还有对话框持有就复用 provider 及其已认证宿主），远程宿主在显示前启动失败才退回进程内 `PulsePickerUiProof.dll`。`RemoteHost` 缺省为 0：基础对话框仍走已验证的 pulse.exe 路线，带事件/自定义控件/属性/places 的对话框仍走进程内，行为与之前相同。
- **远程 Close 语义修正**：进程内 API 允许 Show 期间任何时刻 Close，远程 provider 原先只允许在回调内（Show 等待宿主时返回 RPC_E_CALL_REJECTED / ERROR_INVALID_STATE）。现在 Show 等待宿主时，Close 先做与进程内相同的本地检查（show id、已关闭返回 S_FALSE），再直接写帧（传输允许一读一写并发），宿主的回复由 Show 自己的接收循环取走；宿主拒绝时撤销关闭标记。回调内仍走原同步路径。
- **未解决：远程宿主的对话框在显示后始终 DXGI 遮挡**。当前 `pulse_picker_remote_v2_test`（ready-close、close-ok、custom-update、property-edit、query、reentrant-owner、initial-empty、cross-thread-owner）全部在同一处失败：宿主 Show 返回 `ERROR_FUNCTION_FAILED`、状态 FAILED、已显示。临时跟踪确认：三个启动事件（类型、FolderChanging、FolderChanged）调用方都回 S_OK；隐藏首帧 Present=DXGI_STATUS_OCCLUDED（允许）；ShowWindow 后 `FlushPaintedEdits` 1 秒内没有任何非遮挡帧，在 `flush-no-present` 失败。这条“显示后必须真正呈现一帧”的规则在已提交代码里就有，不是新改动。用 GPT 留下的 04:42 未改动二进制首次运行就是同样结果，与本轮改动无关。
- 排除项：同一私有桌面上冷启动宿主测试 `pulse_picker_host_test` 当前 140/0（它不走 `pulse_painted_plain_child`，不检查遮挡）；会话未锁定、非远程会话、无屏保、显示器全功率、前台窗口正常。GPT 记录的远程用例通过数（如 ready-close 41/0）与当前测试的检查数（46）不同，测试在记录之后又改过。
- ~~下一步：查清遮挡原因~~ → 已查清，见下方“阶段 5e”。
- 另：`PulsePickerUiProof.dll` 当前无法链接（`quick_preview_window` 依赖其他会话新增的 `NativePreviewView`，它又依赖应用扩展运行时），失败的链接删除了 build 目录里的旧 DLL；开发构建里进程内回退暂不可用。
- 实测：wrapper 125/0、进程内 wrapper 1172/0；`pulse_dialog_auto`、auto server 测试、远程 V2 测试、`pulse.exe`（经 `remotecheck.ninja`，因 `pulse_shell.exe` 被占用）构建成功，无警告。

## 阶段 5e：遮挡根因与远程用例修复（2026-11-08）

- **遮挡根因：测试环境，不是宿主缺陷**。临时诊断（已删除）在每次遮挡 present 时记录：窗口可见、未 cloak、未最小化，但输入桌面是 `Default`，窗口所在线程的桌面是测试创建且不切换的私有桌面 `PulseV2Proof-…`。DWM 只合成输入桌面，私有桌面上的交换链必然一直返回 `DXGI_STATUS_OCCLUDED`，“显示后必须真正呈现一帧”的就绪规则因此必然失败。同一批用例加 `--interactive`（在当前桌面运行）立即通过，且没有任何遮挡帧。就绪规则保持不变。
- **截图测试钩子跨进程崩溃宿主（已修）**。`Pulse.PickerTestSnapshot` 窗口属性原先是测试进程里的字符串指针，对话框直接解引用；远程时这是野指针，宿主在 WIC `CreateFileW` 里访问冲突（`Diagnostics\Crashes` 转储栈确认），调用方看到 `ERROR_BROKEN_PIPE`。这个钩子原先在正式版里也无宏保护，进程外化后任何同桌面进程都能让 pulse.exe 读任意地址或写任意路径的 PNG。现在：钩子只在 `PULSE_WITH_TEST_HOOKS` / `PULSE_PICKER_INPROCESS_PROOF` 构建中编译（正式版 pulse.exe 两者皆无）；值 ≤ 0xFFFF 视为全局原子（跨进程安全，读名后删除原子），更大的值仍按同进程指针。远程测试（`picker_inprocess_v2_test`、`file_dialog_multiselect_live_test` 在 `PULSE_REMOTE_PROVIDER_TEST` 下）改用原子。
- **测试菜单驱动只在本进程找弹出菜单（已修，测试侧）**。menu-open / open-mask-sequence 的驱动用 `EnumThreadWindows(state.thread)` 且要求菜单属于本进程；改为以对话框 HWND 的所属线程/进程为准（进程内时与原条件相同）。
- **宿主在回调未返回时处理排队输入（已修，产品侧）**。`Host::Event` 等待 EventReply 时用 `GetMessageW` 泵全部消息，测试或用户的 Enter 会在回调期间被处理并嵌套触发 FileOk，而嵌套事件按设计返回 `E_UNEXPECTED`，对话框挂住（multi-close 约一半概率看门狗超时）。现在等待期间只分发宿主窗口自己的消息（分发/唤醒）和 sent message，对话框的排队输入留到回调返回后，与进程内“同步回调阻塞 UI 线程”的语义一致。修复后 multi-close 连续 6/6 通过。
- **实测（交互桌面，`--interactive`）**：28 个远程用例中 25 个通过。剩余：
  - `startup-visibility`、`startup-folder-visibility`：探针用测试线程自己的 `WH_CALLWNDPROC` 钩子观察窗口，跨进程看不到宿主窗口，需要另做跨进程探针（WinEvent 是异步的，会降低“首个可见位置”的时序精度，未做）。
  - `open-mask-sequence`：“`*.xl*;` 必须被拒绝”的期望与当前 Open 掩码契约冲突（`picker_open_filter.h` 有意忽略空分段，`file_picker_model_test --open-filter-separators` 要求接受 `*.zip;`），进程内同样会失败；未改测试期望。另有约 1/5 偶发：驱动在 wildcard 步骤后 5 秒内等不到 FileOk 结果，在本轮宿主泵改动之前已出现，根因未查。
- `pulse_file_dialog_remote_multiselect_live_test`（需空的隔离目录并设 `PULSE_TEST_DATA_DIR`、`LOCALAPPDATA`）普通与 `--typed` 均 failures=0。
- **决定：远程 V2 测试在当前桌面运行**。`pulse_picker_remote_v2_test`（`PULSE_REMOTE_PROVIDER_TEST`）现在总是使用当前交互桌面，不再创建私有桌面，运行时会短暂弹出对话框；进程内 V2 测试不受影响。不加参数实测 25/28，与上面相同的 3 个用例未过。`RemoteHost` 仍默认关闭。

## 阶段 5f：恢复进程内回退并消除远程偶发（2026-11-08）

- **方向调整**：目标是更可靠地接管系统对话框（崩溃不连累调用方、不受调用方 DPI/COM/钩子干扰），体积其次。实测删除 `PulsePickerUiProof.dll` 只让安装包小约 0.66 MB（5.4%，与 pulse.exe 共享代码被整体压缩），因此远程模式的价值在可靠性，不在瘦身；pdfium 保持随包附带。
- **进程内 DLL 链接修复**：`quick_preview_window` 依赖新增的 `NativePreviewView`（扩展预览），其实现依赖扩展运行时与 `app_runtime`，不能进入第三方进程。DLL 改用 `src/picker_inprocess/native_preview_view_unavailable.cpp`（扩展预览不可用，快速预览走内置路径）；`NativePreviewView::model_` 由 `unique_ptr` 改为 `shared_ptr`（删除器在创建处绑定），替身无需 `ExtensionPanel` 定义。直接把 `extension_panel.cpp` 编入会连带整个扩展界面树，已放弃。DLL 与 pulse.exe 均构建通过，无警告。
- **进程内 V2 测试在私有桌面同样遮挡**：默认模式下 close-ok / ready-close 也在 `flush-no-present occluded=1` 失败，进一步确认遮挡是私有桌面造成的。进程内用例需加 `--interactive`：27/28 通过，仅 `*.xl*;` 期望与掩码契约冲突一项。
- **远程 open-mask-sequence 偶发（已修，测试侧）**：只在远程出现（远程 2/8，进程内 0/8）。临时打印确认每次失败都是“下一步打不开类型菜单”。原因：进程内时，未返回的回调（READY、FileOk）会挡住驱动线程用 `SendMessageTimeout` 发的 Alt+N/Tab；远程宿主在回调期间必须处理跨线程 sent message（否则调用方在回调里给对话框发消息会死锁），这些按键便插进 FileOk 处理中间。真实用户的输入是排队消息，已由 5e 的宿主泵修复推迟到回调之后。测试的 `Callback` 现在统计未返回的回调数，掩码驱动在没有未返回回调后再等 50 ms 才操作。首轮测得远程 0/20、进程内 0/5，但后续复测远程仍约 1/10～1/4 偶发（见 5g）。
- **当前结果**：远程 25/28（startup-visibility ×2 需跨进程探针；掩码期望冲突 1 项），进程内 27/28（掩码期望冲突）。`RemoteHost` 仍默认关闭。
- **后续**：跨进程启动可见性探针；掩码测试期望改为与契约一致（需确认）；实测三种方式的打开耗时；真实软件矩阵（记事本、Office、浏览器、管理员、32 位）；全部通过后默认开启远程并保留退回。

## 阶段 5g：掩码期望修正与远程偶发的进一步定位（2026-11-08）

- **掩码期望已按契约修正（经用户同意）**：`open-mask-sequence` 不再要求拒绝 `*.xl*;`，改为断言接受（与 `picker_open_filter.h` 忽略空分段的规则一致），其余 4 个非法模式仍要求拒绝。进程内该用例 70/0。
- **远程偶发未彻底解决**：等待未返回回调后，远程失败率仍约 1/10～1/4（不同批次波动大，曾连续 80 次无失败）。临时诊断（已删除）确认：失败时对话框已被自身 `Cancel()` 关闭（窗口隐藏且禁用，前台变为 Claude 应用或桌面），没有崩溃，也没有多余的 FileOk。`Cancel()` 的来源只有 Esc、WM_CLOSE 和焦点在“取消”按钮时的 Enter；具体是哪一条尚未抓到（加上焦点记录后的 80 次运行都没有复现）。
- **待验证的推测**：测试运行在用户正在使用的桌面上，几次失败时前台都是用户正在操作的 Claude 应用，用户操作可能抢走对话框激活，使驱动的 Alt+N 未能聚焦文件名框，随后 Tab/Enter 落到“取消”按钮。未验证前不按此改代码。
- 另：插件商店会话新增了 `author_probe.cpp`，旧的 `build/remotecheck.ninja` 因此链接失败并删除了 pulse.exe；已从新生成的 `build.ninja` 重新生成 `remotecheck.ninja`（去掉被占用的 `pulse_shell.exe` 依赖），pulse.exe 正常构建。
- **推测已被否定（2026-11-08）**：在用户不操作电脑时连跑 120 次，远程失败 5 次（约 4%）。最后一批记录了系统空闲时间（GetLastInputInfo），失败时用户已约 230～250 秒没有任何键盘鼠标输入，因此失败与用户操作无关。根因仍未知；下一步需要在宿主里区分 `Cancel()` 的三条来源（Esc / WM_CLOSE / “取消”按钮上的 Enter）并记录当时焦点，限时排查。

## 阶段 5h：远程偶发根因与修复（2026-11-08）

- **根因（临时标记已删除）**：给对话框 5 处 `Cancel()` 调用分别打标记后抓到失败来源是“取消”按钮被激活（焦点在取消按钮、对话框为前台）。测试驱动发 Alt+N 后不确认结果就发 Tab；筛选切换后文件夹重载期间文件名框被禁用（`validating_`/`waiting_`），Alt+N 无效，Tab 从类型字段移到“取消”，随后的 Enter 触发取消。远程时序偶尔落进这个窗口期，进程内恰好躲开。属测试驱动问题；真实用户在文件名框禁用时按 Alt+N 只会无效，不会误取消。
- **修复（测试侧）**：掩码驱动在每步先等没有未返回的回调，再等文件名框可用并发 Alt+N，确认 `GetGUIThreadInfo` 焦点确实在文件名框后才发 Tab/Enter（与同文件 combo 驱动的做法一致），去掉了固定 50 ms 等待。
- **验证**：修复前远程约 4%（120 次失败 5 次）；修复后远程 160 次 0 失败，进程内 5/5。完整用例：远程 26/28（仅 startup-visibility ×2，需跨进程探针），进程内 28/28（`--interactive`）。
- 构建提示：插件商店会话持续新增源文件，`build/remotecheck.ninja` 需先 `cmake build` 重新扫描源文件，再从新的 `build.ninja` 去掉被占用的 `pulse_shell.exe` 依赖后生成。

## 阶段 5i：跨进程启动可见性探针，远程用例全部通过（2026-11-08）

- **探针**：`StartupVisibility` 在 `PULSE_REMOTE_PROVIDER_TEST` 下改用独立线程上的进程外 WinEvent（`EVENT_OBJECT_CREATE`…`EVENT_OBJECT_LOCATIONCHANGE`，跳过本进程）：按类名 `PulseFolderPickerWindow` 识别宿主窗口，在显示和每次位置变化时记录矩形，首次可见时读取快照像素。事件按序送达、不丢失，读取矩形相对事件略有延迟；“首次可见后移动”仍能被后续位置事件发现。读取观察结果时加锁，并给异步事件最多 2 秒的到达时间。进程内仍用原 `WH_CALLWNDPROC` 钩子，记录逻辑共用 `Record()`。
- **验证**：远程首个可见矩形 (31,47) 等于宿主要求的位置，快照 1500×960 完整渲染；两个启动用例远程 20 次 0 失败。完整用例远程 28/28、进程内 28/28（进程内需 `--interactive`）。
- **下一步**：实测 Windows 原生 / 进程内 / 远程三种方式的打开耗时；真实软件矩阵（记事本、Office、浏览器、管理员、32 位）；之后再默认开启 `RemoteHost`（保留退回）。

## 阶段 5j：三种方式的打开耗时（2026-11-08）

新增 `pulse_picker_open_bench`（`src/bench/picker_open_bench.cpp`，`native|inprocess|remote PATH COUNT`）。从“开始创建/初始化”计时到窗口可见（进程外 WinEvent `EVENT_OBJECT_SHOW`）和 READY（Pulse 首帧已呈现）；可见/READY 后立即关闭。原生对话框直接从系统 `comdlg32.dll` 取类工厂，不受 Pulse 的 HKCU COM 接管影响。同一进程连开多次：第 1 次为冷启动，其后为热启动。当前桌面运行，夹具 `bench_data/picker-open-bench`（20 个文件）。

| 方式 | 冷启动 | 热启动（中位数） |
|---|---|---|
| Windows 原生（可见） | 540～655 ms | 270～300 ms |
| Pulse 进程内（READY） | 325～505 ms | 165～177 ms |
| Pulse 远程（READY） | 340～400 ms | 345～375 ms |

- 原生“可见”时文件列表可能仍在异步填充，实际可用时间不早于此；Pulse 的 READY 已含首帧。
- 远程每次 Initialize 都启动一个新的 pulse.exe 宿主（约 35～45 ms），宿主内对话框首帧约 280～320 ms，接近进程内冷启动。因此远程**没有热启动收益**，每次都约 350～375 ms；8 次中偶见一次约 1.35 s 的尖峰（未分析）。
- 结论：远程已比原生快，但比进程内热启动慢约 190 ms。差距来自每次在新进程里冷初始化界面（D2D、字体、模块加载），不是 IPC。若要接近进程内，需要让一个已初始化的宿主常驻或在对话框关闭后保温一段时间（此前 v1 的待命宿主没有测出收益，口径不同，需用本基准重测）。

## 阶段 5k：真实软件试用（2026-11-08，经用户同意临时改 HKCU，测后已按备份恢复）

做法：备份 HKCU 两个 CLSID 与 `Software\Pulse\DialogAuto`，把接管指向 `build\PulseDialogAuto64.dll` 并设 `RemoteHost=1`；逐个启动程序打开对话框，按窗口类与所属进程判断路线（`PulseFolderPickerWindow` 属于 pulse.exe = 远程，属于调用方 = 进程内，`#32770` = 原生）。测后删除 `RemoteHost` 并导入备份，已核对恢复。

| 程序 | 结果 |
|---|---|
| Word 打开（Ctrl+F12） | 远程；owner 为 Word 窗口，标题“打开”，按键后约 574 ms 可见；关闭后 Word 与宿主正常退出 |
| Word 另存为（F12） | 远程；合约通过，含自定义控件调用；关闭正常 |
| WinForms 打开/保存（x64，设置了 Filter） | 远程；选择文件后调用方收到正确路径（`OK|…\file-3.txt`、`OK|…\new-from-remote.txt`），保存对话框不创建文件 |
| WinForms 打开（x64，未设 Filter） | **退回原生**：合约 `InvalidData`（`file_dialog_contract.cpp:88` 拒绝空 `SetFileTypes`），而原生对该调用返回 S_OK |
| WinForms 打开（x86） | 原生（尚无 x86 接管 DLL，预期） |
| 记事本（Win11 打包应用） | Pulse 进程内，但加载的是**已安装版**的 DLL：新启动的打包进程仍按旧注册路径解析 COM 类 |
| Edge | 用户正在使用（06:54 起运行），已加载旧 DLL，未关闭，未测新构建 |
| 管理员权限程序、Chrome | 未测（UAC 无法自动操作；未安装 Chrome） |

发现与待办：
1. **空文件类型列表**：WinForms 未设 Filter 时调用 `SetFileTypes(0, …)`，原生接受，Pulse 判非法并退回原生，所有此类 WinForms 程序都无法接管。建议：原生已接受时把空列表视为“无筛选”。
2. **打包应用沿用旧注册路径**：修改 HKCU 后新启动的记事本仍加载旧路径 DLL（推测打包应用的注册表视图需重新登录才刷新，未验证）。升级换 DialogBridge 目录后，若删除旧目录，打包应用的对话框可能失效；当前安装目录保留了旧哈希目录，需核实这是否是有意设计。
3. **先启动宿主再做合约检查**：WinForms 未设 Filter 的例子中，远程宿主已启动（约 45 ms 和一个进程），随后合约判定原生，宿主一直挂到原生对话框关闭。可改为合约通过后再加载远程 provider。
4. 未覆盖：管理员权限程序、浏览器（新构建）、打包应用（新构建）、x86。

## 阶段 5l：空文件类型列表不再退回原生（2026-11-08）

- **修改**：`file_dialog_contract.cpp` 中原生已接受（S_OK）的空 `SetFileTypes` 视为“未调用”：不设筛选，也不标记已设置，后续真正的 `SetFileTypes` 仍按原规则检查。文件夹对话框、数量上限、原生拒绝的空列表、只有空列表时的 `SetFileTypeIndex` 等仍走原有路径（原生）。
- **回归用例**（`file_dialog_contract_test`）：Open/Save 空列表可准备且无筛选；空列表后再设置筛选取后者；仅空列表时设置索引仍原生；原生拒绝的空列表仍原生。
- **验证**：合约测试 121/0（`--native-options` 46/0），运行时策略 56/0，包装器 125/0，进程内包装器 1172/0，`PulseDialogAuto64.dll` 构建无警告。真实程序复测（经用户同意再次临时改 HKCU，测后已按备份恢复并核对）：未设 Filter 的 WinForms 打开、保存均走远程，合约 `None`，调用方收到正确路径（`file-7.txt`、`nofilter-save.txt`）；设了 Filter 的打开同样正常，无回归。

## 阶段 5m：远程模式默认开启（2026-11-08，用户决定，用于测试安装包）

- `PreferRemoteHost()` 改为默认 true：`HKCU\Software\Pulse\DialogAuto` 下没有 `RemoteHost`（或类型不对）时走远程，只有 REG_DWORD `RemoteHost=0` 才回到进程内 UI。远程宿主启动失败仍退回进程内。这是代码默认值的改动，若测试后决定不默认开启，需要改回。
- 验证：`PulseDialogAuto64.dll` 构建无警告；auto server 测试无失败、包装器 125/0。
- 用户侧关闭远程：`reg add HKCU\Software\Pulse\DialogAuto /v RemoteHost /t REG_DWORD /d 0 /f`。
- **测试安装包**：`dist\PulseSetup-1.0.58-remote-dialog-20261108.exe`（12,046,160 字节）。全量 `build_release.bat` 因其他会话未完成的测试目标 `pulse_address_edit_stability_test`（`MainRenderer` 未定义）失败，改为只编译安装包所需程序后 `build_installer.bat /skipbuild` 打包；资源检查全部通过，编译无警告。原 `dist\PulseSetup-1.0.58.exe` 保持不变。注意：`build` 目录现为发布配置（`PULSE_WITH_TEST_HOOKS=OFF`），开发测试前需要重新配置。
