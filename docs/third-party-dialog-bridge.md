# 第三方文件对话框通用桥接
## 最新状态（2026-10-30，R7 打开与 R8 保存实际 Edge 验收通过）

R6 的无窗口故障已在本机精确复现：初始文件名为空时，第一份快照 generation 为0，被桥接在初始 TypeChanged 阶段拒绝。R7 修复首次有效快照初始化，不放宽桥接校验或吞掉应用错误。空快照专项44/0，空文件名的真实COM、Pulse UI和键盘按钮回调21/0；用户已确认R7实际Edge中Pulse出现、选择结果回传和再次打开取消正常；日志也确认Accepted/S_OK与Cancelled/800704C7且UI_SHOWN=1。这是第三方打开阶段通过，不代表整体接管完成。

此前已修复自定义分组/按钮缺失、早期对象初始化竞态。本功能仍处于显式启动的新进程试验阶段；默认全局接管、已有进程及完整控件兼容尚未交付；保存现已接入实验组件，用户确认R8实际Edge保存和取消均正常；实验范围之外仍有未完成项。整体重构范围和成本见 `shell-integration-refactor.md`。以下保留分阶段原始证据与失败记录，不把历史检查当作最新实机验收。

> 2026-10-29 基线纠正：当前实施目录为 `C:/Users/SS/.codex/worktrees/shell-integration-latest/pulse`，以 1.0.58 新基线及已核对的未提交改动为基础。下方“最新版实施状态”“实机 Edge 原生窗口回归”是当前证据；后续旧工作区历史测试不能用于认证新版。保留新版 Win+E 与选择器近期改动。

日期：2026-10-28，实施记录持续更新至 2026-10-29。本文是实施设计与阶段记录；协议、专用管道、Save 界面、独立 Picker Host、调用器及自有 EXE 标准调用接入已实施并完成定向验证。已实现受控新进程及新子进程的装载，但已有进程统一启用、全入口覆盖和完整事件UI尚未完成，不能视为任意第三方兼容性验证结果。

## 当前阻塞及修复：Edge 自定义按钮（2026-10-30）

R3 实机日志已确认具体触发，不再是装载猜测。会话 `{2033F5BC-969D-4DCF-BBB9-28E6461120FB}`、子进程记录 `{AD7900E9-286F-47B4-AFE8-253BBB927FD3}`：StartVisualGroup(10000)、AddPushButton(10086)、EndVisualGroup 均返回 S_OK，随后 native=1 回退原生。用户反馈仍原生属实，R3 是诊断版且未修复该能力。

本次修复采用标准接口通用模型，不识别 Edge 进程名或硬编码控件编号：有序分组与按钮、标签及可见/禁用状态、同 STA 的 IFileDialogControlEvents::OnButtonClicked；Pulse UI 显示真实控件并支持状态更新。未知控件类型仍有明确回退；按钮事件开始后不得重弹原生。ABI 增加有界控件描述、事件 ID 和 UpdateControl，并校验组件大小与函数所属模块，旧新实验组件不能混装。

验收要求：精确重现上述三调用，确认走 Pulse Show；按钮实际可见、可点击/键盘操作、回调身份与编号正确；回调内状态修改、退订、关闭无崩溃或第二个窗口；保持普通选择、取消和严格显示检查。构建或组件装载通过不代表 Edge 完成验收。本文后续历史记录保留，实际进度以本节及后续验收补记为准。

## 最新版实施状态（2026-10-29）

当前目录 `C:/Users/SS/.codex/worktrees/shell-integration-latest/pulse`。本节优先于下方旧基线历史。目标仍是未修改的第三方软件实际使用 Pulse 对话框；受控调用器验证不是最终交付。

- 已迁移 IPC、标准接口包装、Pulse 独立选择器宿主及 Save 复核；保留新版选择器云盘识别、文件操作与扩展确认框，未覆盖新版 Win+E 规则。
- `picker_latest_build.log` 构建通过；`latest_wrapper_host.log` 实际宿主 Open/Save/Cancel 26 项通过，`latest_save_dialog.log` 保存确认界面 13 项通过，`latest_save_revalidation.log` 32 项通过，后台 Save 及桥接模型专项通过。
- 正式 runtime/launcher 已构建并完成定向验证，区别于旧测试 Probe DLL。已接入 CoCreateInstance、CoCreateInstanceEx、CoGetClassObject；当前已验证的装载范围包括显式启动的同权限同架构主进程及受控CreateProcessW新子进程；已有进程及统一启用流程尚未接入。握手只证明配置/入口装载，不代表实际显示了 Pulse。
- 普通本地单文件 Open 增加独立 LocalOpenV1 策略、成对必需能力、真实 GetFolder 初始目录解析及后台读取访问检查。它是有明确限制的 Pulse 策略，不称为 Windows 内部校验完全等价。默认 Save、事件、自定义控件及其他未实现契约仍回退；尚未完成真实第三方应用验收。
- 正在检查新的请求字段与契约模块，并验证运行组件；不会以编译、握手或受控程序通过代替真实软件的打开、保存和取消验收。

运行组件采用固定版本的 [Microsoft Detours v4.0.1](https://github.com/microsoft/Detours/releases/tag/v4.0.1)，许可证和归档校验值见 `third_party/detours/`。只用于用户指定新进程的标准接口接入；不提权、不批量附着、停用不结束用户应用，DLL 生命周期保留至进程退出。

## 外观与模糊问题：修复和定向验证（2026-10-29）

用户指出测试窗口模糊、与主程序 Pulse 对话框不一致，此问题是交付缺陷。复用选择器源代码不代表运行环境或视觉一致，不能据此宣布接管完成。
### 当前验证结果

本轮已统一144 DPI下的PMv2绘制、只读外观快照和输入字体比例；第三方宿主中的输入框沿用同一Pulse编辑位图，直接绘入主表面，保留原EDIT编辑/选区/光标/IME语义。主程序默认layered路径不变。字体DLL及Shell/Preview辅助组件均按代码所属模块目录定位，未改变CWD/PATH。

- `picker_failure_final_build.log`：主程序、proof UI DLL、测试程序构建通过；`picker_main_input_surfaces.log` 8项通过，含三类输入框Unicode编辑和输入文件名接受。
- `picker_final_stdout.log` 正常路径25项通过，`picker_isolated_final_stdout.log` 独立EXE目录+不同CWD实际调用25项通过；原8秒准备、20秒驱动、30秒总预算未放宽。
- 第二次回车释放可能在Show返回后留在调用者队列：`CALLER-QUEUE`实际记录1→2，测试保持hook并运行正常消息泵，仍要求两次真实SendInput标记释放，增加第二次接受前第一次释放检查；没有伪造按键或改变产品消息循环。
- `picker_render_failure_stdout.log` 28项通过：第二次FileOk返回S_OK后注入Present失败，必须返回Failed/ERROR_FUNCTION_FAILED、空路径并恢复owner；清故障后下一次取消正常。故障分支仅编入proof，不进入主程序。
- `picker_appearance_final_test.log` 24项通过；`module_location_test.log` 父12/子10项通过，测试不启动真实helper；Luma绑定夹具三个场景及父汇总均通过。
- 实机截图已看到文件名、选区、中文Unicode输入和光标；未宣称真实拼音IME、全主题/全DPI/全部软件视觉验收。
- 近期首帧分别为219/109毫秒（普通目录）及172/218毫秒（独立目录）。先前慢帧与超时仍保留，不能据此声称其历史根因已完全证明或给出性能保证。

以上只完成本轮外观/输入环境修复。生产事件门仍关闭，Edge全面接管、默认Save及统一启用流程尚未完成。后续记录保留排查过程，不覆盖这些当前结果。

- 已确认并修正测试 UI 模块 Show 作用域的线程 DPI：`picker_scoped_dpi_test.log` 15 项通过，调用者/owner 原为 96 DPI，实际选择器为 144 DPI、PMv2；Show 返回恢复原线程上下文，原 owner 不变。此测试仅验证 DPI、窗口及取消，不证明输入/视觉通过；生产事件回调仍须恢复调用者上下文。
- DPI 修复后 `READY children=0` 仍存在。`layered_child_probe_test.log` 记录无兼容 manifest 的自有进程不能直接创建 layered child；自有 class0 后设 layered 位也未保留。没有修改应用 manifest、系统窗口类或兼容设置。采样完成不等于视觉通过。
- 已增加只读外观快照并接入主程序、独立宿主和 proof：统一主题、强调色、字体比例、渲染模式、语言、行高及列表样式；显式零列不再与默认列混淆。不调用 AppPrefs::Load。初轮合成配置/文件测试 `picker_appearance_test.log` 24 项通过；随后补充显式列断言，待重新运行。实际视觉待验收。
- 保留 Pulse 的字体渲染与控件视觉；普通 GDI 输入框替代实验已撤回，未作为最终方案。已增加仅 proof 启用的 Pulse 自绘普通子窗口：保留同一位图、布局、选区、光标和编辑语义，仅调整位图输出目的地；失败明确返回错误。正在构建及补查失败处理，尚未声明输入或视觉通过。后续必须实际检查文字、输入、光标、选中、IME、缩放及取消。

此阶段未完成真实 Edge 打开/保存验收，生产事件能力门保持关闭。
本轮实际结果补充：`picker_painted_full_test.log` 24 项通过，真实创建3个 EDIT、两次 Return 的 KeyUp、FileOk 否决保留同窗、再次接受及取消通过。`luma_binding_test.log` 三个独立进程场景（3/5/6项）及父汇总7项通过，仅证明模块目录与绑定规则。`picker_appearance_final_test.log` 24项通过。

但 Computer Use 实际画面检查仍失败：144 DPI 下列表/工具栏显示 Pulse 样式，文件名字段却仍空白，Tab后可见焦点下划线，输入文字也未可见。不能以 child DC 呈现API成功或输入提交通过替代视觉通过。已记录为显示层缺陷，继续修复。`picker_visual_inspection_retry.log` 记录测试窗口正常取消退出；首次授权超时窗口由自有watchdog退出，未干预用户应用。

独立调用目录验证 `picker_isolated_caller_test.log` 失败：复制自有测试 EXE 到独立目录并使用不同工作目录，DLL加载/ABI/Initialize成功，但READY未到，30秒自有watchdog退出99。根因尚未证明，不能以fake绑定测试通过覆盖该失败。
主表面合成阶段：`picker_surface_build.log` 构建通过。`picker_surface_full_test.log` 输入测试失败：首帧约12.95秒、READY约16秒，超过原8秒准备时限，最后自有30秒watchdog退出99；未放宽自动输入验收时限。新阶段日志将阻塞缩小到首帧路径，但尚不能区分代码耗时、GPU等待与整机负载。

Computer Use实际截图已看到 `selection.txt`、Tab选区、中文“清晰度测试-selection.txt”和光标，证明主表面合成解决了本轮观察到的空白显示；这不是拼音IME验收，也不是全DPI/主题视觉认证。该次视觉窗口最终达到手工检查180秒watchdog（`picker_surface_visual.log`），未认定正常取消通过。仅手工 `--inspect` 窗口寿命延长到10分钟以便工具操作；原自动准备/输入/总超时不变。完整提交HRESULT检查已修入源码，待构建及回归。
## 实机 Edge 原生窗口回归（2026-10-29）

用户截图确认 Ctrl+O 仍显示原生窗口，本轮实际验收失败，不能以已加载组件替代功能通过。

证据：`bench_data/manual-edge-dialog/launch.log` 与会话 `{19533EAE-B7AA-445F-ACC3-00398B12A454}/launcher.log` 记录 PID 39992、绑定成功、handshake=1/status=0/coverage=1；只读进程模块检查确认该 PID 装载当前工作树 DLL，且仍为浏览器主进程。会话没有 runtime-decisions.tsv。旧实现只在目标类被拦截后记录，因此无日志不能单独证明某个具体入口未调用。

[Chromium 主程序工厂](https://raw.githubusercontent.com/chromium/chromium/main/chrome/browser/win/chrome_select_file_dialog_factory.cc) 通过 Mojo 将选择任务交给 [utility STA 服务](https://raw.githubusercontent.com/chromium/chromium/main/chrome/services/util_win/util_win_impl.cc)，最终仍用 CoCreateInstance。当前会话只绑定主 PID，未向这些新子进程提供独立会话，这是明确的架构缺口，与现象吻合；本机 Edge 对话框所属 PID 尚未采样证明，不能将 Chromium 源码直接当成该版本 Edge 的运行追踪。

需连续完成的通用能力：

1. 标准 COM 入口覆盖与可区分的诊断：已增加 CoCreateInstanceEx/CoGetClassObject，沿用跨入口递归保护；初始化即记录入口及 ready 前计数。定向测试 runtime entry 17、activation Ex 36、class factory 49 项通过。增加入口不是此次浏览器子进程问题的充分修复。
2. 受控新进程树：仅对本会话创建的新进程做精确身份绑定；不改变 sandbox/令牌/继承句柄，拒绝不支持的权限或架构，不附着用户其他已有进程。创建仅执行一次；额外挂起必须释放，调用者原挂起必须保留；注入中途失败必须有可验证回滚，不能假称恢复原生。父级停用必须传递到后代。当前实现已完成定向构建：`bench_data/child-tree-20261029-171742/parent-report.log` 22 项通过并确认父测试进程退出，覆盖真实新子进程、A/W/NULL 环境、扩展启动句柄属性、挂起、权限旁路、父级停用和父退出；`runtime_child_rollback_test.log` 5 项通过，真实装载后故障恢复为无 DLL 原生启动。仅覆盖普通 CreateProcessW、同架构 native PE；拒绝签名/动态代码策略限制与查询失败，未覆盖其他令牌启动入口。仍未构成浏览器事件 UI 验收。
3. 跨进程 owner：当前客户端要求调用线程拥有 HWND，不能处理 browser HWND 经 IPC 传入 utility 的模式。已支持调用者已管理模态且窗口已禁用的情形，验证进程身份并保持原禁用状态；不能跨进程无限等待 EnableWindow。`cross_process_owner_test.log` 39 项本机测试通过，覆盖独立子进程窗口、取消、宿主崩溃、销毁、占用标记和原禁用状态。测试使用自有协议宿主，不等于浏览器 Pulse UI 验收。
4. 事件与窗口接口：[AutoClose](https://raw.githubusercontent.com/chromium/chromium/main/ui/shell_dialogs/auto_close_dialog_event_handler_win.cc) 依赖 IOleWindow/GetWindow 与关闭行为；[SafeAccept](https://raw.githubusercontent.com/chromium/chromium/main/ui/shell_dialogs/safe_accept_file_dialog_event_handler_win.cc) 可在 OnFileOk 返回 S_FALSE。必须实现否决后保持同一 Pulse 窗口、取消及重入；不得忽略 Advise 直接强行接管。SafeAccept 还依赖 Show 线程的 GetKeyState/WH_GETMESSAGE，纯回调 IPC 不能转移真实输入队列。因此有事件的通用路径转向原 Show STA 中运行 Pulse UI 的独立 UI 模块；无事件路径继续隔离宿主。正在验证依赖与同线程 UI 可行性，生产事件门仍关闭。

后续复测：扩展路径形式经同目录身份核对后转为普通本地 Shell 路径，`open-normalized-20261029-174614` 的真实窗口取消用例 14 项通过；`child-open-confirmed-20261029-174725` 父→子→宿主串联为父 23 项、子窗口 14 项通过，确认显示 PulseFolderPickerWindow、仅一个宿主、正确取消、owner 恢复和宿主退出。使用独立测试桌面，未宣称用户桌面外观或浏览器事件验收。原失败日志保留。

补充未通过证据：`bench_data/child-open-20261029-172859` 的原始 CoCreateInstance 已进入包装，但自有测试配置阶段失败，未执行 Show；父测试 23 项中 1 项失败，不能标记真实 Pulse 窗口链路通过。保存独立模型 `save_access_policy_test.log` 为 38 项中 1 项失败：父目录在探测阶段实际重命名成功，与原有测试假设不同；模型本次返回 PATH_NOT_FOUND、未创建探测文件，仍需验证目录替换竞争，尚不开放生产 Save 策略。
后续定向结果：原生文件类型索引采样确认 SetFileTypeIndex(0)=S_OK，紧随 GetFileTypeIndex 返回 1。包装器保留原 setter 值/HRESULT，单独记录 getter 证据；修复后 contract 88 项、wrapper 112 项通过（`index_save_pulse_file_dialog_*_test.log`）。保存 guard 增加真实目录读取权限并在创建前复核身份，`save_guard_diagnostic_test.log` 39 项通过：进行中的替换被 ERROR_SHARING_VIOLATION 阻止，释放 guard 后实际替换成功，旧确认不能复用。此策略更严格的权限前提仍需保留，生产 Save 未开放。

进程内 UI 模块已完成链接，实际运行 `picker_inprocess_diagnostic_test.log` 在 LoadLibrary 返回 ERROR_INVALID_ORDINAL (182)，尚未进入导出接口或显示 UI；不能标记原线程事件 proof 通过。
默认 Save、事件能力和第三方进程树最终验收仍未完成。下一次实机验收必须确认 Pulse 外观、正确返回文件、取消不重复弹窗与应用窗口恢复；在这些条件完成前不得标记全面接管完成。

### 事件路径调整及代价

原线程事件不能简单通过“选择完成后再回调”补齐：OnFileOk 的 S_FALSE 必须保留同一个窗口；依赖 Show 线程键盘消息的调用方还要求真实输入在该线程处理。代理 HWND 或 AttachThreadInput 不能直接视为等价替代。

因此验证两条后端：无事件的已支持契约继续使用独立宿主；事件契约拟由匹配架构的独立 Pulse UI 模块在调用方 Show STA 执行。复用同一选择器、后台枚举和视觉组件，使用版本化 C ABI、调用方缓冲区及真实 HWND，避免跨 CRT 分配/释放。模块显式初始化并保持加载至进程退出；不得在 DllMain 初始化图形、COM 或文件系统。

该选择有实际成本：UI 缺陷可能影响调用应用，隔离性低于独立宿主；每个使用 UI 的进程都有自己的资源缓存与模块占用；需要分别验证位数、资源/辅助进程路径、线程语言与激活上下文恢复、多线程并发及可重入回调。具体内存与启动耗时待测，不以主程序指标推算。已有签名或进程安全策略拒绝加载时保留原生，不降低应用安全策略。

当前 `PulsePickerUiProof` 仅为验证目标：单初始化 STA、同线程 owner、单文件、Ready/FileOk 与取消；不能据此声称完整 IFileDialogEvents 支持。目录/选择/过滤器事件、回调查询状态、Close HRESULT、跨进程 owner、辅助程序定位及生产启用仍需逐项接入。此前完整产品化预算仍为基线估算，不是此验证目标完成后的剩余工期承诺。

## 产品边界

目标按 Windows 标准调用接口覆盖应用，不以逐应用插件或源码改造为前提。现代入口为 IFileDialog / IFileOpenDialog / IFileSaveDialog；传统入口为 GetOpenFileNameW/A、GetSaveFileNameW/A。不支持的契约回到同一次调用的原生实现。应用名单只用于紧急禁用和兼容例外，不作为主要接入方式。

标准接口覆盖不等于所有对话框覆盖：自绘界面、其他平台 picker、沙箱代理、受保护进程及未支持架构不在首版承诺内。默认文件管理器注册与 Explorer 接管不能提供这些调用的参数和返回值，不能替代此桥接层。

### 为什么选择按接口接入

| 路线 | 能覆盖什么 | 主要代价与限制 |
| --- | --- | --- |
| 默认管理器注册 | 文件夹和 Shell 打开入口 | 不能接收应用内部 Open/Save 调用 |
| 观察窗口并模拟输入 | 部分已显示的原生对话框 | 参数、返回值、取消和自定义控件难以可靠保持，不作为完整替换主链路 |
| 标准 API 通用桥接（选定） | 使用受支持 Windows 标准接口的多种应用 | 需进程内装载、调用契约、位数、生命周期和兼容性工程 |
| 每应用专用插件 | 单个应用的特殊界面 | 不作为基础架构；仅在确有必要时考虑例外 |

通用实现的维护单位是“接口能力”，例如多选或覆盖回调；实现一次后由使用该能力的应用共享。测试仍需真实应用样本，因为同一标准接口可能被不同顺序、线程和选项组合调用。

## 架构与实施路线

调用应用 → 进程内拦截层 → 通用契约模型 → 本地 IPC → 独立 Picker Host → 协议结果 → 原调用线程返回。

1. 独立启动管理器发现符合策略的桌面进程；先在自有测试调用器验证装载，再逐步扩大标准接口覆盖。装载方式、签名、进程权限和应用安全策略需专项验证。拒绝受保护进程及跨权限绕过；拒绝时保持原生行为。
2. 分别构建 x86/x64 桥接 DLL 与必要的同架构装载器；host 可统一为 x64，线上消息不得包含指针或 ABI 相关布局。ARM64、ARM64EC、Windows 8.1 另行验证，不由 x64 成功推断支持。
3. 现代入口在 COM 创建路径识别文件对话框类，保留真实对象并包装接口，记录设置直到 Show。不能只假定应用调用 CoCreateInstance：需评估 CoCreateInstanceEx、CoGetClassObject/class factory、延迟绑定及创建后 QueryInterface 的覆盖。包装必须维护 COM identity、引用计数和接口能力；未知接口不能随意透传造成一半原生、一半自定义状态。
4. 传统入口拦截 comdlg32 的 W/A 调用，评估静态导入、GetProcAddress、延迟加载及模块加载后装载。IAT 修改和函数入口跳转有不同遗漏与风险，技术验证后选定；不能将 SetWindowsHookEx 本身描述为文件对话框 API 拦截。任何用户态装载方式都不承诺所有进程可用。
5. 进程内 DLL 只负责契约、线程和 IPC，不承载主程序 UI，不在 DllMain 做 COM、管道握手或复杂初始化。host 复用 picker 浏览组件；枚举和 Shell 工作仍放后台。

## 现代 COM 契约

将 SetOptions、目录、文件名、过滤器、默认扩展名、places、标题及结果查询转换为明确状态；不要将所有 FOS 位机械传给现有 picker。Show 前协商必需能力，未知选项或未支持语义原生回退。现代 API 的文件类型索引从 1 开始，现有 Pulse picker 从 0 开始，边界转换必须测试。

IFileDialogEvents、IFileDialogCustomize、IFileDialogControlEvents 需要显式能力：OnFileOk 可否决，目录/过滤器变更及覆盖、共享冲突事件存在行为约束。回调必须在调用方正确 apartment 执行，不能把 COM 指针通过管道传入 host。Show 等待期间需处理消息和回调重入；回调期间 Close、销毁、再次打开以及应用退出必须定义状态转换。

首版可拒绝自定义控件及事件契约并回退，之后按接口能力增加，而不是逐应用写分支。属性存储、虚拟 Shell item、非文件系统结果及流式访问未实现前必须回退。不能只返回文件路径而声称完全实现 IShellItem 契约。

## 传统 OPENFILENAME 契约

在调用线程验证结构版本、flags、缓冲区容量及编码；A 版按调用约定进行代码页转换，转换失败不能静默替换字符。不能跨进程传递调用方地址。回填 lpstrFile、nFileOffset、nFileExtension、过滤器索引等必须遵循原接口，检查 nMaxFile 和整数溢出。

多选是约定的目录及文件名 NUL 分隔表示，结尾双 NUL；单选与旧式格式不能混用。缓冲区不足、取消及真实错误需维护 BOOL 与 CommDlgExtendedError 的对应语义，因此需要覆盖该错误查询入口及线程局部状态。OFN hook、模板、自定义消息、Explorer/旧式模式等不支持时在显示前回退，不忽略 flag。GetLastError 不能代替扩展错误契约。

## Save 契约与现有组件差距

初始 PickerMode 只有 Folder/Image/File；本次已增加 Save 和后台目标验证，可返回尚不存在的文件路径。现有别名导出仍调用原生 IFileSaveDialog；第三方接入必须另外完成契约映射。

Save 增量必须单独覆盖：合法新文件名、父目录存在、过滤器与默认扩展名、覆盖确认、只读返回策略、共享冲突、取消、回调否决及属性收集。保存内容由应用写入；host 不预创建或截断目标文件，选择成功不表示写入一定成功。覆盖确认后仍可能发生文件变化，不能宣称确认消除了竞态。

当前桥接已支持经过验证的单目标 Save 子集，要求覆盖确认、规范扩展和叶文件名。未实现契约在显示前返回 Unsupported；传统 API 的文件创建检查、共享冲突和只读策略仍需单独适配，不能由 Save UI 通过推断完整 IFileSaveDialog/OPENFILENAME 兼容。

## 回退、恢复与关闭

保留真实原生对象及完整设置，尽量立即将设置同步给它，避免回退时丢失状态。能力判断应在 Show 前完成；有副作用的 callbacks 不得因重放执行两次。内部调用使用线程级递归保护，避免原生回退再次被截获。

当前实施采用保守边界：在调用桥接前根据配置与能力选择原生；一旦进入桥接，Cancelled、Unsupported、连接失败、host 崩溃和结果对象构造失败均直接返回终态，不自动重弹原生。断连、晚到结果和取消竞争须用 request ID 和唯一终态约束。将来若要增加连接失败后的回退，必须先证明自定义界面未显示、未产生回调副作用且 owner 已恢复，不能仅根据进程已经退出推断安全。

调用方负责其 owner 的禁用与恢复，并监控 host 死亡；host 不永久承担恢复异进程窗口的责任。保存 owner 原始 enabled 状态，嵌套模态只恢复本次调用改变的状态。核验 HWND 所属进程、会话和存活，不信任请求里的 PID。

提供全局关闭、新请求即时旁路、进程级紧急禁用及版本熔断。升级使用版本化文件和协议协商，不覆盖正在加载的 DLL；先停止新装载，排空活动调用，旧进程可保持旁路直到退出。卸载先停装载/启动项并恢复自身创建的配置，不强制卸载活动 DLL，不删除其他产品设置；被占用文件安排后续清理。日志默认只记录接口、能力、错误与请求 ID，不记录完整文件路径。

## 可以立即独立实施的模型与 codec 工作包

目标目录：src/ipc/picker_protocol.h/.cpp；独立测试 src/bench/picker_protocol_test.cpp 与 CMake 目标 pulse_picker_protocol_test。不依赖 UI、注入、注册表或 COM 初始化，不更改现有 picker 行为。

请求包含 version、request_id、operation（OpenFiles/PickFolder/SaveFile）、required_capabilities、initial_directory、filename、filters、filter_index、selection/options；响应包含同 ID、终态（Accepted/Cancelled/Unsupported/Failed）、错误来源与数值、paths、filter_index。Cancel 单独消息。连接元数据中的调用方身份由传输层获取，不接受载荷自报身份作为授权。

采用手工逐字段编码的固定端序格式：magic、版本、消息类型、payload 长度、16 字节请求 ID；字符串明确 UTF-16LE 与长度单位，拒绝非法代理项、嵌入 NUL、截断、尾随垃圾及超限。所有长度先检验再分配，设置总消息、单字符串、过滤器及结果数量上限，检查加法/乘法溢出。不能直接 memcpy C++ struct、wstring、size_t 或 HWND。

语义检查：未知必需能力返回 Unsupported，未知版本/消息拒绝；Accepted 必须有与模式相符的结果数量，取消/错误不能携带成功路径；过滤器索引范围、Save 单结果和 Folder 首版单选明确校验。错误枚举与系统错误码分开，协议解码错误不得变成用户取消。

验收：确定性黄金字节、往返、所有截断位置、超限和溢出、无效 UTF-16、未知版本/能力、非法状态组合、过滤索引边界、单多选约束；只构建和运行该独立目标。该工作包通过只证明模型/codec，不证明 IPC、装载或 UI。

## 阶段与成本

以下为熟悉 Windows COM/兼容性工程的开发者估算，单位人日，不含持续应用兼容维护与签名采购；阶段门槛未通过应停止扩大部署。

| 阶段 | 交付与退出条件 | 人日 |
| --- | --- | ---: |
| 0 | 模型、严格 codec、受控调用器与原生基线 | 4–7 |
| 1 | x86/x64 装载与标准入口覆盖技术验证；失败安全旁路 | 10–18 |
| 2 | Open/Folder host、IPC、模态恢复及通用能力协商 | 12–20 |
| 3 | Save、legacy 缓冲区/编码、错误语义 | 12–20 |
| 4 | COM callbacks、自定义能力、重入与结果语义 | 15–25 |
| 5 | 升级卸载、熔断、故障注入与接口覆盖矩阵 | 10–18 |
| 合计 | 首版标准接口产品化范围 | 63–108 |

不承诺任意第三方覆盖率；应用样本只用于验证接口实现与发现例外。发现额外框架/安全策略限制应公开列出，不用逐应用适配掩盖架构缺口。

## 验证矩阵

| 维度 | 最小验证 |
| --- | --- |
| 架构与创建 | x86/x64，静态/动态导入，现代创建及 class factory，启动前/后装载 |
| Open/Folder | 单/多选、过滤器、初始目录、隐藏项、取消、空目录、UNC/长路径 |
| Save | 新文件、扩展名、覆盖接受/拒绝、只读/共享冲突、回调否决 |
| Legacy | W/A、编码失败、精确容量/不足、多选双 NUL、扩展错误、hook/template 回退 |
| COM | QueryInterface identity、引用计数、回调线程、重入、Close、未知接口回退 |
| 生命周期 | host 启动失败/崩溃、调用方退出、晚到回复、取消竞争、嵌套 owner 恢复 |
| 展示 | 主题、DPI、窄布局、焦点、加载错误与快速切换 |
| 运维 | 关闭后旁路、版本不匹配、升级活动调用、卸载残留和原生可用 |

现有 pulse_file_picker_model_test 和 pulse_file_picker_view_test 仅验证可复用组件，不替代上述桥接验收。涉及文件操作才运行对应 operations 测试。真实第三方验证须记录应用版本、所走接口和具体限制；编译成功不是功能验证。

## 依据

- [Common Item Dialog](https://learn.microsoft.com/en-us/windows/win32/shell/common-file-dialog)：现代接口的创建、事件、自定义及结果契约。
- [IFileSaveDialog](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifilesavedialog)：保存额外属性方法。
- 本仓库 folder_picker_model.h、folder_picker_dialog.h/.cpp、folder_picker_loader.cpp 及 app_commands.cpp 的 PickAliasFile：现有能力与 Save 缺口。

实施记录：协议 codec 812 项、专用管道 69 项、桥接映射 24 项、真实宿主与调用器组合 103 项、调用器独立 23 项、Save UI 8 项、传统契约模型 45 项、现代设置模型 49 项、真实 Shell 结果 26 项通过。显式 COM 包装层 52 项，以及包装层→真实宿主→Shell 结果的 Open/Save/Cancel 专项 26 项通过；原生回退事件/自定义控件 77 项通过，真实控件点击验证功能断言通过，但原生历史清理仍失败。传统 W 显式适配 25 项及真实宿主专项 29 项通过。创建适配 61 项、自有 x64 EXE 静态导入接入/卸载 37 项通过。包装层生产校验门禁仍关闭，Pulse 侧尚不支持完整事件和自定义控件。宿主首轮超时由测试过早提交引起，等待条件修正后保留原断言通过。未执行第三方注入或真实应用全局接管。最新详细结果见 [主方案实施记录](shell-integration-refactor.md)。

Shell 长路径结果可能由 Windows 表示为 8.3 路径；验收比较真实文件身份，新 Save 则比较已存在父目录的身份与叶名，不强迫系统 Shell 对象返回某种字符串拼写。UNC 结果尚未实测。

调用器使用消息泵保持调用线程响应，并以私有 job 管理自己创建的宿主，参考 [Windows Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)。同线程再次进入当前桥接返回明确忙错误；尚不承诺 COM 回调重入支持。

### 2026-10-29 后续验证

以下覆盖早期实施记录中的旧计数与“尚未实现”描述：自有 EXE IAT 接入及异常状态恢复最新 41 项、静态标准调用到真实 host 38 项通过；传统 A/W 共享线程错误状态及 ACP 936 编码 44 项、UTF-8 ACP 编码 53 项、ANSI 真实 host 32 项、Unicode 真实 host 29 项通过。扩展 `CoCreateInstanceEx` 显式创建适配 36 项、真实 host 32 项通过。所有生产兼容性门禁保持关闭；这些是受控调用方证据，不能据此宣称第三方进程已自动替换。

UTF-8 ACP 验证使用独立测试进程的 `activeCodePage=UTF-8` 清单，不更改本机区域设置。若运行环境未启用 ACP 65001，测试明确返回不支持而不计通过。进程级清单支持条件见 [微软 UTF-8 代码页说明](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page)。此方案不把其他代码页或旧系统支持推定为通过。

类工厂显式适配 47 项及真实 host 32 项通过。工厂代理仅支持 IUnknown/IClassFactory，未知工厂接口不是透明透传；IClassFactory2-capable 原工厂整体旁路。x86 专用桥接构建及创建/Ex/工厂/W/A/UTF-8 61/36/47/25/44/53 项通过；32 位调用方→64 位宿主的五条显式链路 26/32/32/29/32 项通过。尚未由这些结果推定 x86 IAT 或自动装载已完成。

随后自有 EXE 静态 IAT 已补 x86：x86/x64 各 44 项通过，两种位数静态调用到真实宿主各 38 项通过。尚只覆盖当前主 EXE 的 CoCreateInstance 静态导入，不含 DLL/第三方装载、其他创建 API 的自动拦截或动态入口。

受控显式 LoadLibrary 阶段：非发布 Probe DLL 的 C ABI 与生命周期测试在 x86/x64 各 87 项通过，IAT 核心最新各 48 项。包括短头保护页、未 PIN 状态无持久资源分配、查询/拒绝后卸载重载、PIN 后停用恢复及旧对象安全。模块 PIN 后保留至进程退出，停用不是排空或热卸载；首版升级按重启目标进程与版本化 DLL 路径设计。当前 DLL Show 固定取消，仅验证生命周期，不启动 picker；仍未执行第三方进程注入。

后续受控 DLL 已增加真实宿主链路：独立 Fixture API 严格复制、解码并匹配完整请求体，忽略 ID 后才调用真实 `RunPickerClient`。原 v1 函数表不变；旧 wrapper 持有原配置快照。x86/x64 扩展 C ABI/生命周期各 173 项通过，两种位数 DLL→x64 宿主打开、保存、取消及配置替换隔离各 51 项通过。日志分别见 `bench_data/bridge_fixture_load_{x64,x86}_initial.log` 与 `bench_data/picker_dll_host_{x64,x86}_latest.log`。测试填充值类型警告已修正，两架构重新构建无新增警告；该类型修正后未重复功能测试。

精确夹具只是验证手段，不能为真实用户请求提供生产校验背书。未匹配仍固定取消；生产验证策略、第三方自动装载与更广泛标准入口覆盖仍未开放。当前证据不需要逐应用编写桥接实现，但完整兼容性仍需按接口能力及代表性应用验证。

### 生产校验的下一步

代码核查确认，当前 Open 主要验证文件属性、过滤器和快捷方式路径，尚未执行与原生等价的共享锁及访问权限检查；Save 检查父目录、目标属性、只读与覆盖确认，但不能据此证明写入权限或创建可行性。原生对照中使用的 `NOVALIDATE`、`NODEREFERENCELINKS`、`NOTESTFILECREATE` 与当前现代契约白名单并不相同，因此不能拿这些对照直接为默认 Open/Save 放行。

先把原始有效 FOS 选项、操作类别和必须完成的校验传给验证策略，保留生产默认关闭。随后按同一组选项比较原生与 Pulse：普通/缺失目标、独占锁、仅可读属性但不可读内容、自定义扩展名输入与快捷方式。不得凭初始目录批准整个对话框，因为用户随后可导航到其他目录。

默认 Open、显式跳过校验的 Open、Save 应分别验收；事件、自定义控件、虚拟项等仍按能力旁路。Save 的写入/创建校验尤其不能靠创建再删除用户目标文件来冒充无副作用验证。微软对选项含义的定义见 [FILEOPENDIALOGOPTIONS](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_fileopendialogoptions)。这些是通用接口语义工作量，已包含在前述契约与兼容验证预算中，不另按应用数量重复计费。

上述上下文传递已实施：gate 接收完整 ContractDecision，包含原生有效 options、校验类别及运行时义务，模型没有扩大选项白名单。两架构契约各 62 项、wrapper 各 81 项通过。wrapper 同时补齐按原生能力暴露的 IFileDialog2；导航根目录/取消标签交原生展示，三种创建入口均识别接口，创建/Ex/工厂各 71/36/49 项、事件各 77 项、更新后 DLL→真实宿主各 51 项通过。门禁仍默认关闭，不能把类别字段当成原生验证已完成的证明。

后续只扩充现代 OpenFiles 的 `FOS_NODEREFERENCELINKS`，同时发送现有 `NoDereferenceLinks` 选项与必需 `PreserveLinks` 能力，旧宿主不得忽略能力而返回目标文件。Save/Folder/`NOVALIDATE` 仍保持原生。最新两架构契约各 70 项，真实宿主普通 `.lnk` 返回自身及取消专项各 19 项通过；没有据此认证复杂链接或访问校验，生产门禁仍关闭。

### 默认 Open 的同选项本机采样

新增 `--open-validation`，五个场景统一使用 options `0x02001848`、`*.txt`、index 1，实际替换文件名编辑框并读回确认后提交。首轮输入驱动未成功提交，功能失败 16、清理失败 5（清理 HRESULT 为 `0x80070005`），日志 `bench_data/native_open_validation_initial.log`，不用于推断原生行为。

调整设置顺序并加入有界就绪重试后，在已授权的本机非受限测试进程运行，`bench_data/native_open_validation_retry.log` 记录：普通文件和手输 `manual.bin` 均返回精确目标；独占锁、缺失文件和“属性可读但读取内容被拒绝”的文件出现同一对话框所属提示、未接受。后者是提示与非接受的实测证据，不代表知道 Windows 内部使用的访问/共享参数。初次与重试同时改变了驱动及执行环境，不能把首次失败仅归因于其中之一。

重试功能失败 0、历史清理失败 5（各 `0x80004005`），测试总退出码仍为 1，不能标整个测试通过。新建 ACL 夹具的原 DACL 与保护标志已恢复并核对，四个文件内容不变且已删除，仅保留准确的 GUID 和恢复日志。没有批量清理用户历史或修改用户既有文件权限。

该证据确认默认 Open 的文件过滤器不能作为手输文件名的硬性限制。正在增加明确的协议能力来区分展示过滤与接受限制，产品自身 picker 和旧请求继续保留原来的严格模式；共享锁/读取权限的原生等价验证仍待实施。

### 展示过滤能力已实施，验证仍有待办

现代 OpenFiles 的非空过滤器增加配对的 `AdvisoryFileFilters` 选项和 `AdvisoryFilters` 必需能力。列表继续过滤，但接受手输的其他扩展名；Save、legacy 和产品普通 picker 的严格模式不变。x64/x86 调用方到实际宿主各 20 项通过，包含编辑框替换、精确目标、索引保留、取消及 owner 恢复，日志 `bench_data/advisory_host_{x64,x86}.log`。

契约各 72 项、桥接映射 27 项、picker 模型专项 9 项通过。x64 完整协议 970 项通过；x86 完整协议发生访问异常，56 项独立截断检查通过后仍在定位最大字符串阶段，尚未通过。以上不认证共享锁/读取权限的原生等价性，生产门禁仍关闭；Probe DLL 尚未就此次过滤器变化重新验证。

### 默认 Open 的共享与权限区分采样（2026-10-29）

`--open-sharing` 在同一组选项 `0x02001848` 下完成 8 例。原生接受只读属性文件；已有读句柄或写句柄仅允许 SHARE_WRITE 时出现提示、不接受，允许 SHARE_READ 或全部共享位时均接受。每例同时执行 `FILE_READ_DATA + SHARE_READ|WRITE|DELETE + OPEN_EXISTING`，只取得并关闭句柄、不读取内容；前后错误一致。

关键反例：只授权读取数据和属性等必要恢复权限、但不授权 READ_EA 的新建 ACL 夹具，FILE_READ_DATA 探测成功，GENERIC_READ 与 READ_EA 均返回 ERROR_ACCESS_DENIED，而原生出现所属提示、未接受。由此可否定“只探测 FILE_READ_DATA 即等价”的方案；不能反过来宣称已知道原生实际权限掩码。后续不得用该简化探测开放默认 Open 生产门禁。

日志 `bench_data/native_open_sharing_initial.log`：功能失败 0，历史清理失败 8（ClearClientData 均 E_FAIL），总退出码 1。ACL 和保护标志精确恢复，最终独占读取核对内容不变、无遗留探测句柄，测试文件已删除，保留 GUID 与恢复记录。没有改动用户既有文件或系统关联。原生失败证据为提示加未接受，不宣称完全匹配错误文本或内部行为。

受控 x64 DLL 已重新构建，并新增 `--dll-filters`，真实 DLL 包装→Pulse 宿主过滤器接受/取消专项 30 项通过，日志 `bench_data/advisory_dll_host_x64.log`。这补充显式 wrapper 证据，不改变 DLL 非发布与生产门禁关闭的状态；x86 对应链路尚待本轮验证。

x86 协议异常后续已定位并修正：反汇编证实当前工具链在测试嵌套表达式处错误传参，拆开表达式、保留全部断言后 x86/x64 完整协议各 970 项通过。未改生产 codec。详细证据和最初失败记录见主方案“x86 协议异常定位与修正”。

随后 x86 Probe 与调用方重新构建，`--dll-filters` 也通过 30 项，日志 `bench_data/advisory_dll_host_x86.log`。两架构专项均通过，未扩到全局第三方装载。

### 显式 NOVALIDATE 的原生基线（2026-10-29）

新增独立 `--open-novalidate`，复用默认 Open 的五场景，选项唯一差异为 `FOS_NOVALIDATE`，实际回读 `0x02001948`。本机原生接受普通文件、独占锁文件、只有属性可读而读取数据被拒绝的文件、手输过滤器外文件；缺失目标仍出现所属提示、不接受，且未创建该目标。全部接受均验证精确目标，拒绝/取消无结果。

日志 `bench_data/native_open_novalidate_initial.log`：功能失败 0，历史清理失败 5（E_FAIL），总退出码 1。ACL/保护标志已精确恢复，四个夹具内容不变且已删除。这是显式 NOVALIDATE 加 FILEMUSTEXIST/PATHMUSTEXIST 的本机证据，不推广到所有 NOVALIDATE 组合。

后续按该接口配置增加独立协议能力和验证类别，以保留“调用方明确跳过访问检查”的意图，不能把默认 Open 请求误归类。生产门禁仍默认关闭，待实际 Pulse 宿主完成相同夹具对照再评估范围。

### 显式 NOVALIDATE 映射与真实宿主验证

实现 `SkipAccessValidation` 与必需能力 `ExplicitAccessPolicy` 的配对协商，只允许 OpenFiles。契约将 `FOS_NOVALIDATE` 记录为独立 `OpenSkipAccessValidation` 类别，仍要求 FILEMUSTEXIST/PATHMUSTEXIST 等原条件，仍保留类别对应的原生行为与 Shell 结果验证义务。Save/Folder 组合回退；产品 picker 默认值 false；字段只表达调用方意图，不能让默认 Open 冒充完成了访问检查。

本轮构建无新增警告。协议两架构各 1038 项、契约各 76 项通过；映射 30 项、真实后台 loader 专项 12 项通过，后者确认缺失文件仍返回 FILE_NOT_FOUND/空结果，单独跳过访问检查不会放宽严格过滤。

实际 `--wrapper-novalidate` 两种位数调用方各 55 项通过：普通文件、独占锁、属性可读/数据拒绝 ACL、过滤器外手输、显式取消；接受后核对文件身份，owner 恢复，无第二个原生 Show。权限和保护标志精确恢复，内容不变且夹具删除，恢复日志保留。证据为 `bench_data/novalidate_host_{x64,x86}.log` 及 `bench_data/novalidate_*` 模型日志。缺失目标的合成渲染错误提示未在该 UI 专项中读取，不把后台断言冒充界面验收。生产默认门禁仍关闭，第三方自动装载尚未实施。

同一 NOVALIDATE 五场景随后经受控 DLL 全链路验证，两架构 `--dll-novalidate` 各 80 项通过，日志 `bench_data/novalidate_dll_host_{x64,x86}.log`；增加 DLL 对象来源、精确夹具匹配、一次真实 host、终态证据、停用后 IAT 恢复和永久 PIN 状态检查。只在自有进程显式装载非发布 Probe DLL，未改变生产授权策略或自动装载范围。

### 下一切片：默认 Save 对照与覆盖后的复核（尚未验证）

原有 Save 对照选项 `0x0211894A` 包含 NOVALIDATE、NOTESTFILECREATE 和 NODEREFERENCELINKS，不能代替当前契约默认 Save 的证据。新增独立 `--save-validation` 采用精确 `0x0200884A`，记录新目标、已有普通、只读、独占锁及禁止创建目录五例；目前仅完成实现与只读审查，尚未构建运行。提示框可能是覆盖确认，不能仅凭提示推断访问拒绝；测试历史清理错误继续单列。

另外确认产品代码在覆盖确认返回后直接沿用确认前路径，缺少后台状态复核。后续以固定路径及父目录、目标身份快照复核，目标替换需重新确认，只读/目录/查询失败不得沿用旧确认；这只校验当时状态，不承诺调用方随后写入无竞争，也不能据此开启默认 Open/Save 全局接管。

### Save 覆盖后复核：模型通过，真实界面仍有失败

已接入后台身份快照与确认后复核，默认接管门禁保持关闭。独立 `save_revalidation_model_fixed.log` 32 项通过；首次失败是测试误将 `..` 的既有 Unsupported 分类期望为 InvalidName，现分别精确验证 `..`→ERROR_NOT_SUPPORTED 和非法叶名→ERROR_INVALID_NAME，均要求 Failed 且无快照，没有放宽为任意错误。后台 picker `--save` 专项 `save_picker_model_initial.log` 通过，包含替换重确认、只读无输出及旧代际失效。

真实 `pulse.exe --picker-dialog-test --save` 的 `save_picker_dialog_initial.log` 仍有 3 失败：初始文件名、新目标默认扩展名返回、嵌套确认中的取消；普通覆盖、替换再确认、只读拒绝及确认中改名失效通过。正在增加控件值/发送错误和取消阶段诊断，未将整体界面验收计通过。

原生 `--save-validation` 首轮日志 `native_save_validation_initial.log` 在第二例既有普通目标的模态取消中 watchdog 退出 99。新目标曾正常返回，Show 前后均未留下目标文件（未监测中途创建）；ClearClientData 仍 E_FAIL。此轮未执行到 ACL 用例，不能声称完成五例。已窄修仅测试拥有的模态取消，正重跑；隐藏模态被取消不等同可见拒绝证据，12 秒退出保护及原判据保留。

### Save 真实确认框诊断与复核通过

仅增加诊断、没有再修改产品逻辑后，`save_picker_dialog_diagnostic.log` 和针对首轮未复现风险的 `save_picker_dialog_confirm.log` 两次各 13 项通过。记录初始 EDIT=预期 new-file、默认扩展名返回、目标替换二次确认、只读拒绝、确认期间改名使旧结果失效，以及取消事件真实置位后 Cancelled/无路径；无 readiness/close timeout。首轮三项失败原因仍不明，不能声称已根治；原失败日志保留，不扩大到全套历史测试。

原生对照重试 `native_save_validation_cancel_fixed.log` 仍在既有文件模态取消处 watchdog 99，未跑到权限用例。新证据为提示可见、属于测试同一 STA、但四个标准按钮句柄均未找到；进一步记录其控件及负向消息投递，尚不能据此归因于私有桌面或原生拒绝。默认接管资格仍关闭。

保存宿主改动后补跑 x86 调用方→x64 宿主 `--wrapper`（`save_revalidation_bridge_x86.log`）：实际 Open/Save/Cancel 结果、owner 恢复及 Save 目标前后不存在均通过，但 Cancel 的“界面及文件名就绪”断言失败，整体 26 项/1 失败。已拆出同断言 `--wrapper-cancel` 并补文字读取/错误/耗时诊断；不能把整项计通过，原因尚未确定。

### 原生 Save 五例完成与跨位数取消复核

`native_save_negative_button.log` 完成默认选项 `0x0200884A` 的五例，功能失败 0、历史清理失败 5，总退出码 1，不标整套通过。测试只对同一提示框中唯一、文字明确的“否/取消/No/Cancel”按钮执行负向操作；不猜按钮编号、位置，不点 Yes/OK。新目标接受且 Show 前后均不存在；其余四例均有可见所属提示，负向取消后无结果。提示可能是覆盖确认，不能据此认定只读、共享锁或创建权限已被原生拒绝；接受覆盖后的验证顺序仍待取证。

本轮无 watchdog，创建拒绝夹具的原 DACL 及保护标志精确恢复，测试文件及权限目录清理完成，保留 GUID 和日志。各例 ClearClientData 仍为 E_FAIL，没有清理用户全局历史或将失败忽略。产品确认后复核不等于原生访问验证，默认接管门禁保持关闭。

仅重跑此前失败的跨位数取消场景：`wrapper_cancel_diagnostic_x86.log` 10 项通过，界面就绪 578ms，文件名读取 109ms/error=0，实际与期望均为 selection.txt；取消无结果、owner 恢复通过。此前整组的就绪失败未复现、原因未明，原日志保留；未重复已经通过的 Open/Save 场景。

### 默认 Save 同意覆盖首轮未通过

新增单例 `--save-overwrite-accept`，仅本次 CREATE_NEW 的普通文件，选项保持 `0x0200884A`。只允许同 PID/STA、直接所属且唯一可见的提示中唯一明确 Yes/是按钮；实际通过仍要求 Show/GetResult 成功和精确结果，前后文件身份与内容不变。没有扩大既有五例或点击其他应用。

`native_save_accept_build.log` 构建通过；`native_save_overwrite_accept.log` 功能失败 2、历史清理失败 1，整体退出 1。肯定操作未发生（attempted/posted/refused 均 0）；超时后负向取消识别出所属提示的“是(&Y)/否(&N)”。Show 返回取消，不能认定覆盖成功，也不能据此推断权限或产品根因。原超时、实际返回断言均保留，正定位肯定探针未观察到提示的原因。文件身份/内容和句柄释放、owner 恢复及夹具删除通过，GUID journal 保留。

### 2026-10-29 父窗口保护与标准事件接线（进行中）

抽取 `src/common/picker_owner_guard`，IPC 客户端和进程内 Pulse UI 共享身份与占用检查。只恢复本次 guard 实际执行的同线程禁用；调用方原本禁用的窗口和外部窗口不由 Pulse 启用。UI 关闭不再将 owner 交给无条件启用它的公共隐藏函数；监视、回调后和返回前复查窗口身份，失效取消。

定向构建 `picker_owner_events_build.log` 成功；`picker_owner_client.log` 39 项/0 失败。`picker_owner_inprocess.log` 中外部 disabled owner 的接受、Close 取消和子进程正常清理通过，但第三例 fixture ready 后整个测试触发 30 秒 watchdog，退出 99；整组未通过，不将后续场景计入通过。正在拆分独立场景，保持各进程原时限与断言。

`native_events_observation.log` 原生事件对照退出 99（45 秒 watchdog）。已采集 TypeChange、FolderChanging、FolderChange 和 SelectionChange 的实际 HWND/Show STA/getter 结果，尚未完成 FileOk veto/接受及 Close HRESULT 检查。探针计时及重入诊断需补齐；不能以事件间隔推断产品根因。生产标准事件接管资格保持关闭。

v2 接口和实现仍在进行：真实 Pulse HWND、同调用 STA 的事件、可重入状态查询、候选代际校验、精确 Close HRESULT。设计声明不是已交付能力。Edge 实机完整接管、Save 和未实现能力仍未验收完成。

父窗口专项补测：仅测试增加单case筛选及阶段时间记录，未放宽30秒进程watchdog。`picker_owner_destroy.log` 15/0、`picker_owner_reenable.log` 16/0、`picker_owner_fileok-reenable.log` 16/0、`picker_owner_original-disabled.log` 12/0，均退出0，包含原断言、无陈旧HWND、资源/自有子进程/夹具清理。前一整组的接受/取消通过记录与最终99仍保留，不将整组改记通过；旧日志不足以断定超时根因。公共client39/0与这些实际UI用例完成当前owner修复的定向行为验证；后续v2共享UI变更需另验。

原生事件诊断第二轮：`native_events_diagnostic_build.log` 构建通过；`native_events_diagnostic.log` 中 FileOk 首次 S_FALSE、同 HWND 再次 S_OK、精确文件结果及 Show S_OK 已实际通过，回调内 GetResult 成功。ClearClientData 返回 E_FAIL。第二个 Close 场景在实际窗口上调用自定义 HRESULT 与20秒超时取消均返回 S_OK，但 Show 未退出，最终45秒 watchdog 99；记录缓冲溢出数量也明确保留。整组仍未通过，不归因于产品或放宽时限。Pulse v2 的 Close 以微软文档规定的原样 HRESULT 和无结果语义实现并独立测试：https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-close 。

### v2 进程内事件基础实现（待运行验收）

新增独立 `picker_ui_bootstrap_v2.cpp` 与内部环境边界，共享现有 Pulse UI/外观/模块初始化。API 提供非复用会话 token、真实窗口查询、一致状态快照、目录否决/选择/类型/确认通知和精确 Close HRESULT；回调暂时退出 UI 的 DPI/激活上下文并恢复调用者环境。会话在整个 Show（含 owner/字体恢复析构）保持 busy，避免同步 WM_ENABLE 重入销毁；UI 异常退出新增幂等窗口与异步结果清理。

排队事件若已经落后于当前状态代际，明确 ERROR_RETRY 终止，不能把历史通知配上最新快照或倒退查询。该限制意味着复杂重入仍未声称完全兼容。能力广告保持0，生产标准事件门保持关闭。首批独立测试 ready-close/close-ok/query/reentrant-owner 及真实键盘确认测试正在构建、验收；声明和代码落盘均不等于验收通过。

### 实际第三方测试入口（2026-10-29）

新增显式启动器专用 `PulseFileDialogRuntimeUiProof64.dll`，通过 `PULSE_DIALOG_UI_PROOF` 构建开关接入v2 provider；普通runtime不启用该provider，不改全局注册。真实pulse.exe同目录的UI模块由已验证装载器保持文件/模块身份。`test-thirdparty-edge-ui.cmd` 使用实际安装的Edge及独立测试profile，打开本地文件选择页，不要求结束日常Edge，也不关闭浏览器安全机制。

`inprocess_wrapper_confirm.log` 316项/0失败；`thirdparty_ui_provider_test.log` 21项/0失败；原wrapper112/0、events77/0。首次新wrapper测试141/22失败来自WRL ComPtr重载取地址清空sink，已改std::addressof并保留所有原断言。`picker_v2_input_initial_events.log` 22/0，真实两次SendInput/KeyUp、FileOk否决同窗、候选/结果/DPI/清理通过。实际桌面v2 core四例均0失败，初始化事件后query42项通过。隐藏桌面首次失败的原始Present值确认为DXGI_STATUS_OCCLUDED(0x087A0001)，保留失败记录，未降低产品严格绘制判据。

双sink原生对照观察Advise顺序和S_FALSE短路；first-veto/self-unadvise原测试错误预期第二sink也会收到而报功能失败，原断言/日志保留。新增E_NOTIMPL两种顺序均观察到通知继续、Show成功和精确结果，功能0失败；ClearClientData/CloseDesktop各失败仍独立记录。COM映射仅按这些证据处理E_NOTIMPL，不吞任意失败。

当前是真实第三方“打开”测试版本，尚未取得用户实际Edge选择结果；不能声明Edge验收完成或全面接管完成。Save、多选、自定义控件及未支持契约仍回退原生。此前v1第二次KeyUp回归25项/1失败仍保留，不被v2验证覆盖。

### Edge 原生回退实机反馈与 R2

用户反馈首轮仍原生。会话 `{DC190330-7D1D-4546-A5A4-1D5174168B2D}` 子进程日志证明 activation 与UI模块装载S_OK、合同Prepared、options=0x1848、events=3，但native=1，随后原生回退。旧日志没有具体触发IID，不能倒推确切调用。

修正COM纯QueryInterface的路由副作用：不支持的查询仍返回E_NOINTERFACE，不永久强制原生；Customize只查询不强制原生，实际方法仍走原回退。增加原因/IID诊断，observer异常不影响行为。R2使用独立DLL文件名 `PulseFileDialogRuntimeUiProof64R2.dll` 和独立Edge profile，不覆盖首轮已装载模块或结束日常浏览器。

`thirdparty_r2_build.log` 构建通过；`inprocess_wrapper_r2.log` 369/0、`wrapper_r2.log` 112/0、`events_r2.log` 77/0。此证据不等于Edge实机通过，R2仍待用户触发“选择文件”确认；旧回退日志保留。

### R2 实机仍回退：确认 Customize 实际调用，尚未修复

用户再次反馈仍为系统窗口。会话 `{4B8B514F-17F2-4F8B-920A-DE51E46F0316}`，子日志 `{81C2FEA5-2232-400E-AB56-8201D790BC5D}` 与 `{5857DF3C-F6B4-47A6-B918-93FA2E8E8088}` 均记录 UI provider 装载成功，Customize 纯 QI 后连续三次实际方法调用，随后 `native=1`、options `0x1848`、events `3`，最终原生回退。因此 R2 未完成真实 Edge 接管；不能将 wrapper 的 369 项通过作为 Edge 验收。

旧诊断只记录接口、不记录方法名或返回值，无法区分应用添加控件、读取状态或失败探测。R3 增加具体静态方法名、数值参数及 HRESULT 诊断，不记录控件文字或用户文件路径；仍保留未实现自定义控件的回退，不通过忽略控件来声称兼容。R3 页面明确标注诊断用途，独立 DLL 与 profile 避免旧模块混用。待取得调用明细后再确定通用接口实现范围及验收用例，不按 Edge 名称硬编码绕过。

R3 定向构建 thirdparty_r3_build.log 成功；inprocess_wrapper_r3.log 371 项/0 失败，包含真实原生对象的 AddSeparator 成功、GetControlState 只读成功、无效控件 SetSelectedControlItem 返回 E_INVALIDARG 的日志核对。R3 会话 {2033F5BC-969D-4DCF-BBB9-28E6461120FB} 握手成功，尚待实际选择按钮触发，不作为接管成功证据。

2026-10-30 原生控件状态对照：`native_customize_state_probe.cpp` 只创建独立COM对象，不Show、不改用户历史。构建成功；`native_customize_state.log` 退出0记录GetControlState为自身状态：禁用/隐藏组不重写子按钮own flags，恢复组不能覆盖单独禁用的按钮。重复ID、嵌套StartVisualGroup和多余EndVisualGroup返回E_UNEXPECTED，无效控件getter返回E_INVALIDARG。此证据仅说明getter和设置结果，不冒充可见UI或完整兼容认证。
### R4 通用分组按钮接线与本机验证（2026-10-30）

已实现标准 StartVisualGroup/AddPushButton/EndVisualGroup 的有序控件模型与真实 Pulse 自绘区域，不按 Edge 名称或其控件ID特判。支持自身可见/启用状态、组有效状态、文字更新和真实按钮事件；初始实现界限16项、单行标签256 UTF-16单位，超出表达范围在显示前回退。仍不声称覆盖所有 Customize 控件或 Save。

审查发现并修复两个一致性问题：Show内更新必须同步原生镜像，否则Show后getter/第二次Show读旧状态；UI快照发布失败必须回滚并终止，不能留下返回失败而界面已改变的状态。后者异常分配路径尚未故障注入，不能用普通更新通过代替该路径运行证据。

定向构建 custom_button_ui_build.log、custom_button_final_build.log 成功；最终wrapper577/0（custom_button_wrapper_final.log），新API模块校验21/0。修复前阶段旧wrapper112/0、events77/0；本次最终事务修复通过新增wrapper相关断言，不将旧二进制称为最终重跑。custom-update初轮43/0。普通真实输入初轮22/4失败，未获得前台且未发送输入；仅测试加入验证自有窗口后的激活尝试，原时限与输入硬门保留，custom_button_plain_input_activated.log 22/0。

真实COM+UI联合测试首次和激活后两次失败日志保留（custom_button_com_live.log、custom_button_com_live_activated.log）。第二轮证明窗口在前台，但焦点在主窗口而非脚本假定的文件名编辑框；修改测试为真实Tab导航直到观察到文件名焦点，不修改产品与判据。custom_button_com_live_navigation.log 19/0：真实SendInput触发按钮57291（与Edge编号无关）、两个sink身份正确且按序，首sink E_FAIL不吞第二回调；第二sink自退订与Close(0x80040629)精确返回，无文件结果、native=0，所有自有对象/夹具清理。检查数少一项因成功自退订后无需清理第二订阅，非删除断言。

Computer Use自有custom-inspect截图尝试返回“foreground window did not report a process id”，刷新后未找到可控窗口；因此本轮尚无有效截图外观验收，未绕过工具限制截图。真实Edge R4仍需实际触发并核对日志与用户所见，以上联合测试不能冒充Edge已通过。

最终 custom_button_ui_update_final.log 43/0；两处事务返修只读复审未发现新阻断。custom_button_visual_inspection.log 在无法取得有效Computer Use截图后到达独立180秒人工模式watchdog，退出99，保留自有夹具，未记作视觉或点击通过。

R4实际Edge会话：{03E12A88-CD4E-44CB-96B1-7816D749FA67}。专用runtime为PulseFileDialogRuntimeUiProof64R4.dll、独立edge-profile-r4，2026-10-30启动握手status0/coverageF。当前等待用户实际选择/取消；握手不代表接管认证。

### R4 实机再次失败：发现早期激活竞态（2026-10-30）

用户实际反馈R4仍原生。此轮没有进入custom模型或UI provider装载：子会话 `{52E9264F-01CA-4F50-8E91-C63987A4CB33}` 明确记录 CoCreateInstance `startup-counters-calls=5-targets=1-before-ready=1`，无activation-adapter-entered。启动hook已存在，但初始化worker尚未发布经校验的services时，一次目标文件对话框创建直接走了native。不能把上一阶段控件修复或联合测试认作该实机入口已成功。

下一修复为早期对象延迟绑定：提前创建时保持原生对象并追踪设置，首次Show时仅绑定已完成校验且未停用的services；若仍未就绪，保守原生且不在同一对象后续Show混用接管。不得在DllMain/loader lock等待，不关闭浏览器安全机制，不以随机sleep掩盖竞态。该方案覆盖早创建、晚Show；Show本身也抢在验证完成前的场景仍需明确限制及进一步启动方案，不能宣称任意启动时序均已覆盖。
### R5 早期激活延迟绑定实现及验证

三个既有COM入口在初始化尚未发布时保留真实native对象、原生设置结果及受支持控件/订阅状态；首次Show单次解析，已校验未停用才绑定Services。未ready/异常时固定原生，不等待、不后续升级；提前类工厂按值保存resolver。未知QI仍保持现有E_NOINTERFACE边界，不宣称完全原生透明。新增early-wrapper-created、resolve-ready、resolve-not-ready-native及disabled阶段诊断；没有可信目录前只计数，初始化后汇总，不读取未验证路径写日志。

R5定向构建early_dialog_r5_build.log成功：early_dialog_r5_inprocess.log 678/0、旧wrapper112/0、runtime入口17/0、factory49/0。用明确发布状态控制五类顺序，未使用sleep制造通过。early_dialog_r5_live.log 21/0补充真实UI+COM：提前setter/控件/Advise期间resolver调用0次，发布后首次Show调用1次，真实键盘按钮产生两sink回调及精确Close，无native replay、无结果，自有夹具清理。独立窄审未发现本轮阻断；实际Edge仍待本轮触发，不能把这些检查当作实机Edge通过。
R5实际Edge会话 {A9002DB2-8DF6-4B83-ABD7-54E1F2CE77CA}，独立edge-profile-r5及PulseFileDialogRuntimeUiProof64R5.dll，启动握手成功，实际选择/取消结果待验证。

### R5 点击无窗口，进入显示阶段取证

用户确认能看到R5页面，点击选择文件后无窗口。实际子日志45F29BAA-A689-45E0-A3E2-4DF1036CD33D、E92E801B-394C-4929-919D-4C650C47721D等已记录UI装载S_OK、native=0、events=3、LocalOpenV1 admitted；未见native回退。此证据仅说明走入接管路径，不能认作实际UI成功。限定最近Application错误记录及专用profile未找到崩溃记录/转储，不据此断言未崩溃。

R6仅补wrapper/session诊断：Prepare进入及HRESULT、真实api.Show返回hr/status/flags/win32_error/terminal_hr、事件kind及返回hr、owner存在/有效/同PID/同线程/enabled的布尔状态；无标题、路径、HWND输出，不改变路由和owner策略。r6_show_diagnostics_build.log构建成功、r6_show_diagnostics_test.log 680/0。外部enabled owner是代码可解释的候选之一（会返回80070032），但尚无R6实机证据，不当作已确定根因或擅自放宽父窗口保护。
R6诊断会话 {8E5C4915-B4F4-47AB-9D95-C2695AF80A8D}，独立edge-profile-r6，当前尚未收到实际对话框显示阶段日志，等待一次用户点击。

### R6 实机确证：初始类型通知返回 E_UNEXPECTED

用户已点击R6。子会话4909853C-EB6A-44CB-B279-63E506F7778E及79EC2498-3A51-416F-A5B7-19B766ADA3FC一致：外部owner有效且已disabled（enabled=0），Prepare S_OK；进入api.Show后首次TypeChanged(kind=5)返回0x8000FFFF，最终Show同HRESULT、FAILED/status3、UI_SHOWN=0、win32_error1627。已排除本次“外部enabled owner拒绝”假设，不修改父窗口保护。

当前需进一步区分session.Observe内部不变量失败与具体订阅sink错误，并以原生对照确认非否决通知的HRESULT处理；不能直接吞掉所有桥接错误。R6实际Edge仍未通过，后续精确诊断及定向修复继续。
### R7 空初始文件名修复及前后对照

R6之后的本机复现使用旧UI文件哈希记录empty_name_old_ui_hash.json：empty_name_before_fix.log真实COM Show返回8000FFFF，未进入TYPE sink；empty_name_snapshot_before_fix.log 44项/3失败，明确首TypeChanged非零代际、后续推进和旧代拒绝失败。所有失败日志保留。

修复PublishEmbeddedState只让首次generation0也发布为1，后续仍按实际状态变化递增，event/Query共享快照。未放宽Observe/Read、未变更通知HRESULT策略、未调整owner保护。empty_name_snapshot_after_fix.log 44/0、r7_nonempty_ready_close.log 37/0、r7_wrapper_diagnostics.log 680/0。

empty_name_com_after_fix.log已有真实窗口/TYPE回调，但原driver空文件名导航20/2失败。测试原先100ms发下一Tab，并不代表上次输入已处理；随后改为每个真实SendInput唯一标记，通过自身STA WH_GETMESSAGE确认对应Tab KEYUP已出队才继续，保留原8秒导航及30秒总预算。没有生成虚假KEYUP、没有盲改Tab次数或削弱按钮断言。empty_name_input_consumed.log 21/0，真实顺序list→filename→filter→custom，两个sink收到准确按钮ID，精确Close与native0，夹具清理通过。日志显示其中一枚Tab从入队到处理约1.2秒，原100ms假设不足。

proof/env门控焦点诊断不记录文字、路径，不修改焦点行为。原生TypeChanged错误探针只完成新增模式和构建，未运行，也未据此改变产品策略。R7实际Edge仍需核对新增Observe/sink/getter细分日志与用户所见。
### R7 实际 Edge 打开、结果回传、取消验收通过

用户明确反馈“Pulse 出现，选择和取消都正常”。实机会话 `{015E33F1-DA7A-445F-AEB2-7A3B37CD279F}`：子会话 EFFBE7D7-739F-43B8-90D1-D79686394F55 记录 FileOk S_OK及 api.Show hr=0/status=Accepted1/flags=UI_SHOWN1；再次打开的18B86BAD-9F3E-444E-A776-0AEA64ADE08E记录hr=800704C7/status=Cancelled2/flags1。两次合同native=0，符合用户看到的真实Pulse流程。

阶段结论：实际安装Edge、隔离profile下单文件打开/结果回传/取消通过。尚未验证任意应用、现有进程统一接入、Save、多选、全部自定义控件或所有 DPI/主题；不能从本阶段推导全面接管完成。接下来推进标准 IFileSaveDialog 的进程内Pulse事件与现有保存校验/覆盖复核接线，完成本机定向验证后再实际第三方验收，保留当前已通过Open路径。

## Save 阶段：原生事件对照（2026-10-30）

R7 的实际 Edge Open 已由用户确认：Pulse 显示、选择回传、再次取消正常；这不是 Save 或全局接管验收。

新增 `pulse_native_save_events_probe`，只操作自有窗口和唯一测试夹具，不控制浏览器。三个独立运行分别记录于 `bench_data/save_events_new_veto.log`、`save_events_overwrite_accept.log`、`save_events_overwrite_refuse.log`：

- 新目标：第一次 FileOk 返回 S_FALSE 后保留同一窗口，第二次 S_OK 后 GetResult 返回精确路径，目标始终未创建。
- 现有目标 ACCEPT：OnOverwrite 先于 FileOk；成功后内容、文件身份和最后写入时间不变。
- 现有目标 REFUSE：保留可用窗口，没有 FileOk，随后显式取消返回 0x800704C7，目标不变。

三项均 `functional_failures=0 cleanup_failures=2`，进程退出码为 1。清理失败分别是专属 GUID 的 ClearClientData 返回 E_FAIL，以及 CloseDesktop 失败；保留 journal 和失败记录，不能称为全部通过。此证据仅涵盖单订阅者、当前系统，不推断多订阅者响应合并或全部扩展名规则。

实施采用独立 LocalSaveV1、显式 in-process proof 开关；普通 runtime 和 DefaultSave 不获准。复用 Pulse 保存界面，独立承载 DEFAULT/ACCEPT/REFUSE 覆盖响应，并在 FileOk 回调后后台重新检查目标身份和访问。已有访问模型对新目标会短暂创建并删除探测对象，有文件系统副作用，不能称为 Windows 校验完全等价。实际第三方 Save 尚待实施和验收。


### Save 实现与本机验证（R8 准备阶段）

新增独立 LocalSaveV1、in-process API Save 字段和覆盖响应回调。普通 runtime 不调用实验 Save 门禁，未配置 provider 时不发送 Save IPC。Runtime 策略43/0、合同93/0、初版 wrapper805/0；补多订阅者仲裁后 wrapper1114/0。Save worker --save 定向检查通过，主程序/proof UI/runtime/相关测试构建通过。以上不代表真实 Edge Save 已验收。

实际 COM + Pulse 窗口：新目标默认扩展名24/0，覆盖 ACCEPT23/0（SendInput）；自有窗口消息模式 REFUSE→Escape20/0、FileOk veto重试23/0、FileOk Close21/0、Overwrite Close20/0、FileOk替换目标25/0。替换例真实序列 Overwrite1→FileOk1→Overwrite2→FileOk2，最终保留测试创建的替换文件身份/内容及原文件备份。消息模式明确不是物理键盘UX验证，也不控制浏览器。新文件访问探测有短暂创建/删除，断言仅证明回调及返回时目标不存在，不能说从未创建。

原生多订阅者：save_multi_accept_refuse.log、save_multi_refuse_accept.log、save_multi_notimpl_accept.log 三项功能0/清理2；首S_OK响应决定，E_NOTIMPL继续，FileOk按顺序通知。实现使用强引用快照+cookie/order复查，回调中新订阅不参加当前Overwrite。默认响应模态对照三次仍失败（save_multi_default_accept*.log）：WM_CLOSE未关闭；实际按钮ID都是0，未猜测按钮或强点，45秒自身watchdog结束，不能称默认覆盖模态验收通过。

保留键盘失败记录：save_live_refuse.log与save_live_refuse_diagnostics.log暴露测试只识别WM_KEYUP而漏掉真实WM_SYSKEYUP，现已修正严格匹配线程/键/marker。save_live_close.log与save_live_overwrite_close.log记录前台激活被Windows拒绝，不作为Close业务失败；save_live_refuse_syskeyup.log还暴露拒绝后编辑框失去焦点，正在修复，不以消息模式通过掩盖键盘缺陷。

Open 空文件名回归：save_open_initial_empty_interactive.log44/0。非交互桌面尝试save_open_initial_empty.log37/3失败，显示阶段8007065B及CloseDesktop失败；保留该环境限制，不把失败改写为通过。


### Save 焦点修复与 R8 第三方验收准备

本轮确证 Save 验证临时禁用文件名框会导致焦点为空；此前 UpdateChosen 只重新启用。现在提交前记录自有焦点，仅当 picker 仍是前台/活动窗口、没有有效焦点且未结束/取消时恢复原编辑焦点；不激活窗口、不合成输入、不覆盖用户已移走的焦点，Open 路径不变。主程序及 UI proof 重建成功（save_focus_build.log）。

原物理键盘模式现通过：save_live_refuse_focus_fixed.log22/0（Overwrite REFUSE后真实Escape取消）；save_live_veto_focus_fixed.log25/0（真实Return、FileOk S_FALSE、同窗再次Return接受）。初始失败日志保留。未因本次改动重复全部历史测试。

R8 入口 test-thirdparty-edge-save.cmd（原test-thirdparty-edge-ui.cmd同步指向同一R8内容），独立edge-profile-r8及版本化PulseFileDialogRuntimeUiProof64R8.dll。页面仍为本地无上传测试，Ctrl+S触发真实Edge保存，目标限定建议路径bench_data/thirdparty-edge-ui/save-results-r8/pulse-r8.html。准备/启动不等于通过；需要用户实际看到Pulse并核对保存文件和取消，未进行浏览器自动化绕过。


### R8 实际 Edge 保存与取消验收通过

用户明确反馈“都正常”。会话 `{5B6B0CC6-3E5A-4A2E-BF75-68D27B9B1F86}` 内实际Save合同options=0x880A/category6/native0/events2，成功进入LocalSave proof。子会话D6BD2A75-7211-42AF-8A3E-C0BB00ADF9EC与C01E93CF-9DD8-438E-B76E-A72ADF705A9E记录S_OK/statusAccepted1/UI_SHOWN1；AEE74698-32C0-4C9F-86BF-A262412A0D90记录800704C7/statusCancelled2/UI_SHOWN1。日志证明返回状态，实际保存体验由用户确认；未读取用户其他目录的文件内容。

同轮Open也有真实Accepted和Cancelled。阶段结论是隔离profile、显式启动Edge的单文件打开/保存/取消已实际通过；不等于已有进程、所有第三方、自定义控件、多选、其他位数与全局日常启用完成。下一阶段先核对最新版系统入口与原旧工作区可靠性重构的差异，防止遗漏原始重复窗口/无反应问题，保留新版Win+E策略。

### 日常启用切片：当前代码缺口（2026-10-30）

R7/R8 通过并不意味着普通快捷方式已受管。`dialog_launcher/main.cpp::Launch` 可创建并绑定新目标进程，仍需显式 host/runtime/cwd/control-root；已有实例没有 attach 入口。单实例应用把新启动请求交给旧进程时，必须报告未连接，不能把 launcher 返回或 ready 当作对话框已采用 Pulse。

下一可交付切片是通用受管启动、连接状态和会话停用，不做逐应用名称放行：复用标准接口契约，区分启动成功、运行时连接、实际采用 Pulse 三种证据。先提供可重复启动配置/明确的快捷入口，再接设置页。根会话停用已有 CLI 和父链检查，可令后续 Show 回到原生；正在完成的操作继续收尾，PIN 的 DLL 不热卸载。

代价不能只算一个开关：需要把当前 proof provider 整理为有明确能力声明的发布组件、版本化并存以避免覆盖已加载 DLL、PE 位数选择与 x86 真实进程内 UI 验证、单实例转交失败反馈、子进程与停用/更新闭环。当前 x86 caller 到 x64 host 历史证据不等于 x86 进程内 UI 通过。已有进程任意接管、所有创建进程 API、受保护/沙箱进程以及未知控件仍是独立兼容范围；不把它们隐藏在“全局开启”按钮背后。原始总预算见总方案，尚不能据此给出已验证的剩余工期。

### 可重复受管启动配置（实施及首轮验证）

新增 --save-profile/--check-profile/--profile，保存不启动应用；重用标准受控启动，不按应用名称放行。配置绑定 host/runtime/launcher 三个文件的身份/大小/时间，目标应用允许更新后重新验证路径/位数；这不是签名认证，也不绑定另行加载的 UI provider，不代表发布组件包已完成。保持原 proof 门禁。

managed_launch_profile_build.log 定向构建成功，managed_launch_profile_test.log 29项/0失败。新增状态检查区分实际进程存活、历史握手和本会话停用标记；routing_status=not_queried 不声称已查询祖先会话或当前对话框资格。

首次真实 --profile 启动失败，保留 bench_data/managed-launch-live-5b16fd44-6ab9-44e5-9c81-f0da9846d95e：创建和绑定均成功，但自有目标退出2、未握手。正在检查夹具路径/argv校验，尚不能归结为已确认根因，也不能算真实启动通过。实际启动/停用/退出验证只使用自有无UI夹具，不替代R7/R8第三方界面证据。

可重复启动补充验收：managed_launch_fixture_fix_build.log构建通过，managed_launch_profile_fixed_test.log33项/0失败。夹具原先对GetModuleFileName扩展路径与普通report路径作字面比较；现在用目录句柄规范化，仍严格限制bench直属专用目录。第二轮managed_launch_live_fixed.log已收到精确argv/成功握手，但同步PowerShell输出等待使20秒release夹具先退出；该轮未通过，记录保留。

改用Start-Process -PassThru（隐藏窗口），只对启动器自身WaitForExit(12000)，未延长夹具20秒期限。managed_launch_live_handle_test.log退出0：同一配置重复启动两轮，PID24424与5500各自精确argv、nonce/PID/创建时间握手、coverage15；status0→disable0→local_disabled1/status3；写唯一release后自有子进程均退出0；退出后status3拒绝旧握手。完整证据在bench_data/managed-launch-live-80899b13-b123-4cd9-89de-2fa2cd6c482c。

此包完成配置重用和本地会话状态验证；不认证当前路由祖先链、真实对话框显示、全局启用、发布组件完整性或已有应用实例接入。界面接管仍以此前R7/R8实际Edge证据为限。

额外版本变化实测：主程序重新链接后，对此前成功launch.profile执行--check-profile，managed_launch_rebuilt_host_rejected.log返回profile_valid=0/error1306/process_started0，退出2，符合旧配置拒绝组件变化的约定；未启动目标应用。需要重建配置或使用固定版本目录，不能把可变化build目录当长期发布包。

### 固定版本包与 R9 实际 Edge

新增 tools/pack_dialog_experiment，16项纯文件回归通过（代理执行）；root实际pack得到bench_data/dialog-bundles/20261030-r8-x64，34文件静态依赖/SHA256核验成功（dialog_bundle_pack.log）。包含实验runtime/UI及明确的preview/shell/elevated动态helper、LumaText/PDFium和分发许可；不是完整安装器，也不认证所有独立Pulse功能。只创建新目录，保留旧组件。

从不同caller/CWD加载：dialog_bundle_save_veto.log23/0，真实Save COM+UI否决后重试、精确返回结果；dialog_bundle_open.log22检查/3失败，窗口已进入Pulse但Windows前台激活被拒，物理键盘没有派发按钮回调。失败保留。dialog_bundle_open_empty.log44/0验证包内Open空初始文件名、真实UI_SHOWN与取消/清理；不代替物理键盘UX。dialog_bundle_post_ui_verify.log再次确认加载后包文件未改变。

R9通过可重复profile从固定包启动实际Edge，独立edge-profile，入口test-thirdparty-edge-r9.cmd；session {29AD9CC3-6D49-4EE2-8AC7-7001921FAF23}。用户反馈“打开、保存和取消都正常”。直接日志证据：子会话E7749074-2F67-40E7-934D-C8D76E36A86B的Open options0x1848/category5/native0，子会话052DEAB4-77EC-4EC3-9D09-84CD7C1929BF的Save options0x880A/category6/native0；均返回800704C7/status2/flags1，证明两类Pulse界面显示并取消。当前R9日志没有Accepted回执，专用save-results未发现新文件，因此用户整体反馈与可直接核查证据分开记录，不额外宣称本轮已取到保存落盘证据。此前R7/R8选择/保存成功仍有原会话证据。

R9浏览器可继续占用固定包，后续开发不覆盖该目录。下一包补启动器对完整包的自动校验，再接用户可操作的启用/状态/停用入口；当前profile只绑定三文件，包清单尚不是runtime自动授权。

### 完整包自动校验与 profile v2（R10）

新增公共src/common/dialog_bundle验证器，提供持文件句柄的BundleLease；启动前核对完整文件清单/摘要/PE位数/普通与延迟导入/许可项和额外文件，并复查目录身份。不是签名认证或能力放行；租约释放后不保证目录永远不变。launcher新增显式--bundle，profile v2保存manifest摘要，旧CLI/v1兼容。

首轮dialog_bundle_native_test.log59检查/12失败，真实R9只读校验也error161（dialog_bundle_native_r9_first.log）；根因是manifest许可路径使用/而Win32最终路径为反斜杠，严格比较未统一分隔符。仅对已验证相对路径make_preferred，保留全部路径/身份/重解析检查；负测改精确错误码，未知profile版本改为99，旧失败日志保留。

修复后dialog_bundle_native_path_fix_build.log构建通过；dialog_bundle_native_path_fixed_test.log60/0，managed_launch_v1_compatibility_test.log33/0；dialog_bundle_native_r9_fixed.log真实R9 Verify/Recheck均通过。无额外历史全套回归。

新目录bench_data/dialog-bundles/20261030-r10-x64由新launcher打包，dialog_bundle_r10_pack.log34文件验证成功。dialog_bundle_v2_live_test.log退出0：实际format2配置，同一profile两次独立启动，精确argv/握手、status0→disable0→disabled status3，release后自有子进程退出0、旧握手status3。fixture为bench_data/managed-launch-live-ff18a0b8-420b-4824-80ba-eab9a5b13fbf。未修改在用R9，未因后端校验改动重复要求用户测试相同UI。

下一包开始实现设置“启动与集成”内独立管理卡及异步后台：选择包/应用、显式启动、状态刷新、精确停用。历史握手不能显示成当前全面接管，后台不持AppState裸指针、不阻塞UI、不终止已运行应用；尚待实现与本机验收。

### 接入管理卡与真实进程验证（2026-10-30）

设置“启动与集成”新增独立管理卡：选择固定组件包、选择应用、启动、刷新及逐会话停用。后台异步验证完整包，保存绑定摘要的配置；每次启动单独保留 operation/profile/control，取消观察不会关闭已启动的应用。状态区分未知、已停用、已退出与已启动等待对话框，不把历史握手当作当前全面接管。窗口消息丢失时仅在已有操作忙碌期间读取内存结果，不在 UI 线程做文件或包校验。

构建 dialog_management_final_build.log 通过。第一轮 UI 定向检查 failures=0，并查看八张明暗/宽窄/100%-150% 渲染图；发现窄窗口路径硬截断。局部复用 FitEndEllipsis，保留模型完整路径；dialog_management_ellipsis_build.log 通过，management-ui-{a3d61516-ca1e-48d5-bb97-9be5ff2fb576}/management.log failures=0，修复后窄窗口100%与150%截图已复看，省略提示正常。这些是程序渲染图，不冒充实机桌面交互验收。

R11 包 dialog_bundle_r11_pack.log 验证34文件，未覆盖在用R9。dialog_management_live_test.log 首轮26检查/1失败：测试命令传入正斜杠路径，而包验证器要求Windows路径；失败证据保留。改用反斜杠运行 dialog_management_live_windows_path.log，真实启动、取消旧结果、重新发现会话、精确停用、销毁管理器不关闭应用、信号退出及退出后历史握手均通过；39检查中仍有1项自有测试目录清理失败，暂不记该轮整体通过。正在修复测试清理并增加错误码诊断，未弱化产品校验。

管理卡实际点击到后台的联合验证、真实桌面默认关联、已有进程及全应用覆盖仍未完成，不能据本阶段称全面接管完成。

最终定向复验：dialog_management_cleanup_build.log 构建成功，dialog_management_live_final.log 39检查/0失败（退出0），完整包/应用校验均error=0。测试补入Windows路径格式诊断及仅针对共享/锁冲突的5秒非修改删除访问预检，释放后重新核对所有目录身份和重解析点才清理；不终止应用或启动器，不修改旧失败证据。既往清理失败未记录具体错误码，不回溯宣称已实测某个错误码。

实机界面验收发现另一个运行条件：启动build/pulse.exe后，Computer Use返回的窗口属于C:\Program Files\Pulse\pulse.exe，说明请求已转交安装版实例。已请用户正常退出安装版（含托盘），等待后再验收最新管理卡；未关闭用户实例，也未把旧界面算作新版本通过。

### 实机管理卡验收（2026-10-30，继续）

用户确认退出安装版后，Computer Use launch_app仍返回安装版进程；正常关闭该窗口后确认无pulse.exe，改用确切build路径启动，核实PID34828属于shell-integration-latest。隐藏启动未立即提供可操作窗口，显式--new-window后出现两个最新版窗口。本轮不据此宣称冷启动窗口数量正常。

实际设置页可进入第三方管理卡，使用Pulse文件夹选择器选R12组件包，回到UI显示“组件包已验证”；使用Pulse文件选择器选C:\Windows\System32\notepad.exe，启动按钮启用；再次选择应用并Escape取消，配置不变。两窗口情况下，选择器确认及取消后另一列表窗口出现在前景，需重新激活管理窗，正在核查owner焦点恢复。

实际点击“使用Pulse启动”失败，UI显示31，无记事本窗口；专属operation {6C3682FB-7537-421B-B133-C98B0713D30D}，control {AD3BC4DA-4CE9-45DC-9C7E-2A7533ACC64F}的launcher.log：create=1 error=0、pid=42812 binding=0、pre_resume_abort=1 error=0。当前只能确认启动后绑定失败，不能由error=0猜测具体根因；正在补分阶段诊断。管理卡实际启动链路尚未验收通过。

记事本绑定分阶段诊断：R13中created/allowed_process通过，process_identity失败；PID、创建时间及映像查询均正常，仅canonical_match=0。R14补充路径后确认 expected=\\?\C:\Windows\System32\notepad.exe，而 actual=C:\Program Files\WindowsApps\Microsoft.WindowsNotepad_11.2607.14.0_x64__8wekyb3d8bbwe\Notepad\Notepad.exe。证据：dialog_management_notepad_identity.log及management-notepad-{a1544e38-337b-43f1-83e6-ba51d6dfb1dd}内launcher.log。这是系统转发到另一个程序，不是所检查的System32/Windows硬链接别名；未放宽身份校验、未宣称该入口受支持。R13/R14仅增强诊断，不修改既有包。

已实现选择器关闭前恢复owner的修复，适用确认/取消共用关闭路径，仅恢复本guard亲自禁用且身份仍一致的同线程owner；若前景已切走不抢回。正在构建并运行双窗口定向回归及直接依赖的owner验证，尚未以代码审查替代实际通过。

### 焦点修复最终验证与记事本子进程调查

picker_owner_focus_build.log首轮在最终链接因测试进程34828仍占用pulse.exe失败；仅在核对本轮自有进程路径及创建时间后结束该残留实例，picker_owner_focus_relink.log链接成功。picker_owner_component_build.log选择器DLL构建成功。picker_owner_client_test.log 39/0、picker_owner_original_disabled.log 12/0、picker_owner_inprocess_cancel.log 15/0，均退出0。

默认独立桌面的picker_owner_focus_test.log没有真实前台窗口，前景先决条件失败，未记通过。新增显式--owner-focus --interactive后，picker_owner_interactive_build.log构建成功，picker_owner_focus_interactive.log在当前桌面只使用自有窗口，三个场景全部通过：双窗口确认返回精确路径且激活原owner、取消空结果且激活原owner、主动切换另一窗口后取消不抢回；共13个PASS、退出0。默认隔离桌面缺前景现明确退出4/未验证，不弱化断言。

直接选择日志中确认的商店版记事本实际exe，R14启动绑定与握手成功，PID38388/coverage=f，证据dialog_management_direct_notepad.log及management-direct-notepad-{bf00e75f-e129-474f-84e4-23446b655d33}。但实机Ctrl+O仍系统窗口。CIM观察到同映像子进程50280、父38388、参数含/SESSION；runtime-decisions.tsv仅初始化及child-routing-configured，无子会话或对话框判定。当前coverage=f只代表已有三个COM入口和CreateProcessW已装钩，不代表所有创建路径/子进程/对话框受覆盖。现有早期未配置、重入、未启用分支无日志，尚不能据缺日志断言具体漏掉的Windows API。

用户要求继续调查，但打开、保存操作由用户亲自完成。后续不再通过Computer Use驱动这些操作，准备有辨识度的新诊断版本后再给出明确步骤。

### R15：区分CreateProcessW旁路与入口缺失

新增固定64条不覆盖的CreateProcessW诊断缓冲，包含进入/返回时间、分支、准备阶段、即时原生返回错误及成功返回PID；配置前仅记录内存，发布后输出。没有记录命令行、环境、用户名、文档路径。child_diagnostic_build.log构建通过；child_diagnostic_test.log 22/0、child_diagnostic_missing_enabled.log 15/0，均退出0，证明受控seam参数原样、单次调用、错误码保持、重入有界与容量保留；这些测试不创建真实进程。

独立R15包dialog_bundle_r15_pack.log验证34文件，未覆盖旧包。实际直接记事本启动dialog_management_notepad_r15.log成功，fixture management-notepad-r15-{27b36a2c-ef90-48e3-b763-4b8958e8a3ef}，session {DAE5ED0A-13C4-452C-B21D-71CEA13F72E3}。CIM parent50172/child20444同映像；日志child-observed-startup-count=0，后续没有child-call记录或children目录。由此确认该次子进程创建没有被现有CreateProcessW hook观察到，不能再用未ready/disabled等已记录分支解释；尚未确认具体其他API。无需重复请求用户点打开取得同一启动证据。

下一轮只在实验proof runtime透明观察其他公开创建入口，不改token、环境、启动标志或注入策略。用户已关闭R15记事本；实际打开保存由用户完成。

### R16：已确认实际漏接入口

实验runtime透明观察CreateProcessA/CreateProcessAsUserW/CreateProcessWithTokenW/CreateProcessWithLogonW，普通runtime不新增这些hook。child_observer_build.log构建成功；child_observer_test.log 37/0、child_observer_w_regression.log 22/0、child_observer_missing_regression.log 15/0。四入口只调用原API一次，不改变参数、返回及LastError，无身份凭据/命令行/环境内容日志；此次只观察，不新增注入。

R16包34文件验证通过。实际fixture management-notepad-r16-{d5828d13-f858-4d48-97a4-272b876fc323}，session {FC3153F9-3464-4421-8609-E963AB03D265}，父50452/子22444。runtime记录CreateProcessAsUserW child-call:n=1;t0=190194765;t1=190194843;b=7;g=0;s=0;ok=1;ge=0，原生error=0、返回PID22444，与CIM完全一致。由此确认当前记事本子进程通过AsUserW绕开已有W接入，并非推测某个API。

后续实现限定实验AsUserW入口，复用W已有的父链/身份/子token资格/绑定/注入回退和原挂起状态规则，不按应用名特判、不附加已有进程、不调整权限。参数语义依据本机SDK及[Microsoft CreateProcessAsUserW 文档](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessasuserw)，调用者传入token及安全参数保持不变。用户已关闭R16，等待修复后手动打开/保存验证。

### R17 记事本启动时序诊断（2026-10-30）

新版本包 20261030-r17-x64 校验34文件通过。直接启动安装包真实 Notepad.exe，launcher退出0、父进程23484握手成功；窗口子进程48348仍未接入。父日志 CreateProcessAsUserW 的 t0=191415468 早于 provider-published=191415484 与 routing-ready=191415500，分支 b=4/g=2（尚未发布），tr=0，不是已证明的令牌拒绝。证据：bench_data/dialog_management_notepad_r17.log 与 bench_data/management-notepad-r17-{bbb383a0-bdbe-4d29-bd84-c5494aa06836}。用户已关闭本轮窗口；没有让用户重复打开/保存。下一步修复初始化与早期子进程创建竞态，不能将父握手当作全进程接入完成。

### R18 真实记事本窗口子进程已初始化，待手工对话框验收

修正provider/child routing发布顺序，失败撤销与对象寿命检查：child_publication_test.log 14/0，child_publication_entry_regression.log 17/0；受影响目标构建通过。R18版本包34文件校验通过。实际父49272创建子49912，AsUserW b=0/g=0/tr=0，child-injected-awaiting-ready成功，子目录独立产生runtime-ready和初始化日志。证据：dialog_management_notepad_r18.log、management-notepad-r18-{4b7bd895-9dcd-458f-88a2-18445e35c5a7}。首次证明本轮记事本窗口子进程已接入；尚待用户Open/Save/Cancel实际验收。更早发生于全部配置前的NotConfigured仍未据此排除，不能称所有启动情况已覆盖。

### R18 用户验收失败：Open 与 Save 仍为系统窗口

用户反馈仍为系统窗口。子进程日志证明CoCreateInstance已进入adapter，非漏接：Open自定义combo id1，items 0/1/2/3/5/4/6、selected0均原生setter S_OK，但contract reason18（Labels），options0x1A48触发回退；Save先unsupported-setting，combo同上selected5，contract reason21（SaveMetadata），options0x884A。日志尚不能区分具体Save setter，必须补方法级诊断，不能把metadata忽略后宣称兼容。

源代码核对：当前v2只表达groups/buttons，故Labels修复之后仍可能因ComboBox回退。本轮必须覆盖已知Open完整trace的自定义文字、通用下拉框、选项ID与事件回传后再实机，不能仅修第一个阻断便让用户重测。R18不算Open/Save接管通过。

### R18完整Open参数审查后的实现范围

进一步审查确认options0x1A48包含FOS_ALLOWMULTISELECT。当前LocalOpenV1例外仅单选，去掉Labels阻断仍会因RecentItemsPolicy回退；session、runtime capability、协议codec、异步ValidatePaths及v2单字符串结果也均有限制。因此本轮新增的是通用多选接入，不能删除调用者多选标志或只放开某个mask。

实现分界：COM contract/wrapper/session负责文字、Combo状态/事件及同代多结果；v2 ABI/bootstrap/UI负责能力协商与有界完整选择/候选/结果数组；protocol/runtime/loader负责匹配能力与每文件异步访问校验。整批任一项失败不得提交部分结果，取消不返回历史结果，FileOk veto保留窗口和当前选择。旧provider不支持新能力时明确回退，不能静默丢字段。已知SaveMetadata仍独立待方法级证据，不将Open通过等同于Save通过。

### R19准备：已完成的本机定向验证（尚未实际记事本通过）

- notepad_multi_protocol.log：LocalOpen协议135项通过；notepad_multi_worker.log：真实多文件worker检查退出0，失败整批清空及权限恢复通过。
- notepad_contract_test.log 94/0、notepad_runtime_policy_test.log 47/0。
- notepad_open_contract.log：完整0x1A48+文字+7项combo+两文件COM桥接187/0；notepad_inprocess_regression.log 1156/0，notepad_wrapper_regression.log 112/0。
- notepad_ui_build.log真实UI DLL/runtime构建成功；notepad_bridge_model.log退出0。
- 私有桌面labels/旧ready-close/其他新模式显示失败，保留原log；diagnostic第46行明确Present=DXGI_STATUS_OCCLUDED(0x087A0001)。没有放宽必须真实呈现S_OK的产品规则。
- 在当前桌面的自有测试窗口，labels与labels-empty各40/0，multi-accept46/0，multi-close48/0。label实图已检查，长文字截断没有覆盖输入框。layout专项退出0。
- combo-select当前桌面测试超时99，尚未通过；正在修复测试焦点前置并保留原结果，未据此声称控件可用。所有打开/保存均尚待实际记事本R19验收。

### R19已启动待用户Open验收

combo driver焦点前置修复后，notepad_ui_combo_focus_fixed.log 47/0；产品没有为通过测试放宽检查。主程序notepad_main_build.log构建通过。真实保存回归首轮误传DLL路径未进入流程（保留日志），纠正为pulse.exe路径后notepad_save_new_corrected.log22/0、notepad_save_close_corrected.log21/0。仅证明原单文件保存通道未回归，不证明记事本SaveMetadata已支持。

R19不可变版本包34文件校验通过。实际父49108/窗口子50420再次成功AsUser注入并独立初始化。fixture=bench_data/management-notepad-r19-{e6d4f5db-37b6-4d25-a54e-78250d14c02e}；用户待验Ctrl+O接受与取消，尚未记为实机通过。

### R20/R21实际诊断：默认扩展与保存属性

R19用户再次报告Open原生，reason11/call1为DefaultExtension。R20新日志出现seq0/2/3而缺seq1；源码定位runtime固定256字节日志行超长直接丢弃，已将扩展诊断拆为有界短行。R21由agent实机操作Ctrl+O/取消、Ctrl+Shift+S/取消，并关闭空白记事本；不再要求用户重复开关诊断窗口。

R21日志确认Open顺序Title→DefaultExtension=txt→FileTypes→Options0x1A48。Save首项SetCollectedProperties(S_OK,input_present=1,append=0)，之后FileName/Title/DefaultExtension=txt/FileTypes/Options0x884A；尚未确定属性count，非空不得忽略。fixture=management-notepad-r21-{53ac18bb-2a29-4539-8c80-d433b2ca1278}。

原生Open扩展probe观测：both/exact-only返回stem，extension-only返回stem.txt，explicit返回stem.md，multi返回one.txt和two.txt；无exact的type-change/all-type也返回stem.txt（SetFileTypeIndex程序设置，不是用户菜单切换，不能据此推断后者）。全部Show S_OK，但ClearClientData=E_FAIL和私有桌面清理失败导致退出1，保留所有夹具及日志，不称测试全通过。来源native_open_ext_*.log。实现需exact存在优先，仅不存在时补名，访问失败不得换打开另一个文件；列表选中路径永不补名。

### R22用户确认Open显示Pulse，Save仍原生

用户明确反馈“打开的是pulse，保存还是原生”。子进程日志同时确认Pulse UI已显示、回调正常，最后status=Cancelled/hr=0x800704C7；这证明实际记事本Open显示与取消路径，不代表选中文件后的内容读取已验收。

Save新的属性诊断为SetCollectedProperties(append=FALSE)，列表存在且GetCount=S_OK/count=1，不能按空列表绕过。仍reason21 SaveMetadata回退。下一步读取属性描述名称/键/类型（不读值）确定通用支持边界，修复后自行操作实机验收。证据fixture management-notepad-r22-{1b294480-6530-4e19-a672-33fe620c9785}。

### 实际Save属性已确认及原生对照

R23c实际descriptor匹配System.Security.EncryptionOwners，PROPERTYKEY={5F5AFF6A-37E5-4780-97EA-80C7565CF535}/34，vt=4127，typeflags=0x8000001B；appendFALSE/count1。不是空列表，也不是可编辑任意metadata。no-Show oracle只证明schema；GetProperties未Show返回E_UNEXPECTED，不能拿它替代Pulse已Show后的语义。

notepad_encryption_save_oracle.log真实native Save对自有不存在new.txt，options0x0200884A，Show S_OK，FileOk/postShow GetProperties均S_OK且真实IPropertyStore count0；目标始终未创建。functional_failures=0，但ClearClientData=E_FAIL使cleanup_failures=1和总exit1，保留失败事实。实现方向为精确已支持只读收集契约、目标及全部祖先非encrypted、返回标准空store；不开放任意metadata写入或忽略企业/加密语义。

另R23早期NotConfigured实证已修，notepad_early_child_test与regression各22/0；父正向ready校验notepad_parent_ready_test25/0，防止初始化失败且disable标记失败仍授权后代UI。审查确认闭合该失败分支；真实多轮冷启动仍待完成。

### R24记事本用户实测通过，开始扩展应用矩阵

用户明确反馈“实测，已正常。下一步是更多的软件。我来测试。”据此将记事本11.2607.14.0的打开、另存为及取消记录为用户实测通过；本轮未独立读取保存文件校验内容，不把用户验收写成自动文件内容比对。

后续固定20261030-r24-x64，不覆盖不可变包。下一应用Word（Office LTSC 2024，16.0.17932.20790）通过真实WINWORD.EXE测试入口启动，launcher成功、初始化S_OK；fixture=bench_data/compat-word-r24-{80d47c11-f0e6-4a06-b475-befb193e6f7a}。这仅证明接入初始化，打开/保存仍待用户测试。随后计划Excel和当前Edge；不宣称全局常驻接管完成。

## R25：通用创建入口诊断与微信 / QQ 测试准备（2026-10-30）

本轮补充 CoCreateInstanceEx 的通用可观测性，未增加任何应用名称分支。现在记录创建上下文、请求接口数量、Open/Save 类型，以及聚合、服务端设置、线程模型、未知接口、输入/输出状态、原生返回结果、对象身份、包装失败和最终包装成功的独立原因。未知接口记录 IID，不采集聊天内容、文件路径或账户信息。诊断回调抛异常不会改变原生调用次数或返回对象。

定向构建 pulse_file_dialog_activation_ex_test / pulse_file_dialog_ui_proof_runtime 成功；activation_ex_diagnostics_test.log 为 58 项、0 失败，包括观察器异常、原生指针/HRESULT 保留和成功包装身份。未运行无关全量测试。此改动用于查清此前 Word 的断点，不宣称 Word 接管已修好。

新固定包 20261030-r25-x64：34 个文件校验通过，仅代表组件包完整。微信 4.1.15.56、QQ 9.9.19.34606 均为 x64；两份绑定该包的启动配置保存并检查成功。用户先退出微信，由本任务启动新进程；QQ 原有实例保留。选择文件与另存为体验结果待用户实测，不发送消息或文件作为自动测试。

## R26：标准 Open 的全部文件过滤及工作目录选项（2026-10-30）

R25 微信选择文件用户反馈原生。会话 `{971B000E-9D8A-4676-91EC-B21391C65E93}` 主进程日志明确：CoCreateInstance 已包装并捕获 SetTitle、SetOptions(0x10001A00)、SetFileTypes、SetFolder；Show 前 contract reason=12 / call=2 即 UnsupportedFilter，并回退原生。这证明存在过滤契约缺口，不能据旧日志断定具体过滤字符串。该选项还缺少 NOCHANGEDIR，旧 LocalOpenV1 映射及准入会再次拒绝。

修改不包含应用名称或路径分支：Open 精确 `*` 规范化为已有 `*.*` 全文件语义，复杂模式仍不近似替换；LocalOpenV1 允许省略 NOCHANGEDIR，保持绝对路径及不改变进程工作目录的行为，文件和目录存在性检查不变。Save/Folder/其他旧策略仍保留原约束。新增过滤语法分类日志，只记录长度和星号/分号分类，不记录名称或过滤标签。

实际原生对照：cwd-accept、cwd-cancel 均在无 NOCHANGEDIR、过滤为 `*` 时达到预期成功/取消，工作目录不变。测试自有 GUID 历史 ClearClientData 返回 E_FAIL、CloseDesktop 失败，均保留，整组退出码仍为失败。首次取消驱动同步 Close 卡住并 watchdog=99；改为向自有窗口投递 IDCANCEL 后正确取消，首轮日志保留。证据分别为 native_open_cwd_accept.log / native_open_cwd_cancel.log / native_open_cwd_cancel_post.log。这是本机观测，不扩大为所有 Windows 版本认证。

受影响构建成功。自动检查：映射 104/0、准入 48/0、真实 COM 包装＋模拟 UI 的 all-files-open-contract 46/0、此前 Open 多选/扩展名/标签链回归 222/0。包装用例验证两项文件结果、事件、没有原生重放、工作目录不变；不等同于真实微信 UI 验收。

R26 x64 新包 34 文件校验及微信启动配置检查通过。用户正常退出 R25 后启动 R26；微信实际选择文件待测，QQ 尚未切换。若仍原生，按新过滤分类及契约回退证据继续定位，不将准备完成标为接管成功。

## R27：微信显示后崩溃与激活上下文栈修复（2026-10-30）

用户提供截图及本机 Weixin_8786387b-4ed7-43ba-9a26-508eb2a2e063.dmp；未上传转储。R26 微信实测判失败，不能因已弹 Pulse 就记通过。R26 日志现在确认过滤为单字符 `*`，Open 进入 Pulse，类型/目录回调返回后发生异常。

本机直接解析 minidump：异常 0xC015000F（STATUS_SXS_EARLY_DEACTIVATION），线程 2872，异常地址 ntdll+0xcb338，栈内候选返回地址包括 PulsePickerUiProof+0xa317 等。此处是模块范围解析和栈内地址扫描，不冒充有符号展开的完整调用栈。微软 DeactivateActCtx 文档说明：撤销不在栈顶但仍存在的 cookie 会抛出此异常。

代码缺陷：v2 CallbackContext::Invoke 在事件回调前撤销 Show 的外层激活帧；嵌套窗口消息可能已自动压入额外帧，导致提前撤销异常。独立回归通过相同嵌套结构复现旧方法的精确异常码。修复保留并引用调用方激活上下文，回调时在栈顶临时叠加它，返回时只撤销本次回调的帧；Show 外层与窗口消息自动帧保持原顺序。不使用 FORCE_EARLY_DEACTIVATION，不吞掉该系统异常继续运行。

相关构建成功；独立 activation-stack 回归 15/0，覆盖旧异常复现、正常/抛异常/嵌套回调和完整逐层撤销。真实 UI DLL 验证 ready-close 37/0、query 42/0、multi-accept 46/0，检查实际显示/结果/回调/宿主恢复和 DPI 恢复。均是自有测试窗口与文件，不自动操作或发送聊天消息。R27 新版本包 34 文件验证通过；微信进程已不存在，故通过新绑定配置重新启动，实际微信结果仍待用户复验。QQ 暂不推进。

参考：https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-deactivateactctx

## 2026-10-30 用户验收更新与 CADReader 准备

用户确认 R27 微信“正常了”，对应本轮指示的发送文件选择窗口、加载列表和取消后继续运行；记录为该路径实测通过，不延伸为微信另存为或实际发送文件通过。下一应用为 C:\Program Files (x86)\CADReader。PE 检查主程序 CADReader.exe 6.5.3.105 与 CADReader-Editor.exe 1.8.0.30 均为 x86。现有进程 CADReader.exe 26024；未关闭。基于当前工作区新建 build-dialog-x86 定向构建接入运行时、启动器、UI及回归测试，不复用旧 Desktop 工作区二进制。

### CADReader x86 定向验证结果

当前源码的 x86 启动器、运行时、UI DLL 及测试目标构建成功。激活栈回归 cad_x86_activation_test.log 为 15/0。但实际 UI ready-close 返回 0x80070032（ERROR_NOT_SUPPORTED），24 项中 3 项失败；未显示 UI，停止后续 multi-accept 与 CADReader 启动，保留 cad_x86_ready-close.log。

源码证据：Compositor::CustomEditEnabled 依赖 LumaTextAvailable；PULSE_WITH_LUMATEXT=OFF 时渲染实现的 Init 返回 false。Pulse 进程内选择器要求自绘输入框，不允许退化为原生输入框，因此窗口创建拒绝。本机 LumaText 与 LumaTextSystem 源码均在 CMake 顶层明确限制 Windows x64。不能把旧 x86 显式桥接测试当成最新自绘 UI 已支持 x86，也不能将关闭 LumaText 的构建成功当成 UI 可用。

CADReader 导入 QtCore4/QtGui4；QtGui4 同时导入传统 GetOpenFileNameW/GetSaveFileNameW 和 CoCreateInstance，仅能确定可能路径，尚未证明实际选用哪条。现有 CADReader 26024 未关闭，未注入本轮新组件。

下一技术前提：补齐并验证 32 位自绘文字组件，或设计 32 位接入到独立 64 位 Pulse UI 的事件/结果桥。后者必须处理同步 COM 回调、宿主窗口/焦点和取消生命周期，不能直接复用当前要求同进程窗口的 v2 事件契约。上述两条均未实现；本轮不擅自修改独立 LumaText 项目、不删除其架构限制，也不放宽选择器自绘门禁。

## AutoCAD 2027 R27 接入准备（2026-10-30）

用户指定 E:\Program Files\Autodesk\AutoCAD 2027。确认 acad.exe 版本 R26.0.60.0.0、PE x64，启动前未发现 acad.exe 进程。使用现有 20261030-r27-x64 固定包，保存并验证 bench_data/compat-autocad-r27/launch.profile，再通过受控入口启动。没有更改 AutoCAD 设置或图纸。实际打开、另存为与取消待用户验证；启动/组件握手不能替代对话框通过。

### AutoCAD 启动配置纠正

两次无参数受控启动均出现“加载 acadres.dll 资源文件时出错”，截图由用户提供。资源文件实际存在于 AutoCAD 2027/zh-CN/acadres.dll。本机原桌面快捷方式明确为 acad.exe /product ACAD /language "zh-CN"，工作目录为 AutoCAD 2027/UserDataCache。先前测试没有保留语言参数及工作目录，属于测试启动配置遗漏；不能据此断定注入不兼容或安装损坏。另有一次普通启动使用相同不完整参数，不是有效的原快捷方式对照。

已在 compat-autocad-r27-zhcn 创建独立新配置，保留原快捷方式参数和工作目录，绑定同一 R27 包并校验后启动；不修改原快捷方式、不修改资源 DLL、不调整系统或应用语言配置。真实主界面和对话框尚待确认。后续从应用安装路径准备测试时，先检查匹配的现有快捷方式并保留产品/语言/工作目录等启动约定。

### R28：保留进程可见的普通路径语法

同参数 R27 仍显示资源错误；直接启动原桌面快捷方式后普通进程进入 Autodesk AutoCAD 2027 - [Drawing1.dwg] 主界面。发现通用启动器将规范化的扩展路径直接传入 CreateProcess 的应用路径和工作目录，可能改变应用观察到的模块/资源路径。本次将普通短 DOS 路径以普通拼写传给进程；完整规范化路径仍用于安全绑定及进程身份校验，不改变文件/目录或权限。UNC、长路径、尾点/尾空格、保留设备名等扩展语义路径不转换。不添加 AutoCAD 名称判断。

新 process_launch_path_test 13/0，含真实自有子进程观测模块路径和工作目录。启动器构建成功，R28 x64 包 34 文件验证及携带原快捷方式参数的 AutoCAD 配置验证通过。用户确认退出普通 AutoCAD 后启动 R28；资源错误是否消失及打开/另存为接管仍待本机验证，不能只凭这个差异认定根因已修复。

R28 实机启动结果：新 acad.exe PID 53144（父启动器 17064）独立握手成功，并进入 Autodesk AutoCAD 2027 - [Drawing1.dwg] 主界面；本轮不再停在 acadres.dll 资源错误。打开/另存为仍待用户操作。普通启动旧诊断进程 43460 无主窗口，未将其当作本轮成功证据。

## AutoCAD 实际选择窗口：未接管，传统入口诊断补充

用户在 R28 最新会话输入 `~` 后提供“选择文件”截图：AutoCAD 风格的目录浏览、右侧预览及初始视图控件；明确不是 Pulse，记为打开未接管。该会话主进程日志未出现 COM 文件对话框目标激活记录，不能据截图断言它必定调用某个 API。源码检查确认现有安装钩子只覆盖 COM 创建和进程创建，GetOpenFileName/GetSaveFileName 的 W/A 只有显式适配器，未自动接入。

补充 proof runtime 四个传统入口的只观察钩子：有界记录结构大小、Flags、hook/template/custom-data 是否存在；不记录字符串或指针数据，不调用传入 hook，不改返回值、缓冲区或取消语义。原接口仍执行一次；记录异常隔离，原调用前恢复 LastError，返回后不再调用诊断。该观察不计入替换能力 coverage。观察器运行时及独立测试构建通过；legacy_observer_test.log 44/0 覆盖 W/A、成功/失败、空/短结构及观察器异常。尚未部署给当前 AutoCAD，不宣称已证实其传统入口或已经接管。

下一步需要采集实际入口/扩展标记再选择传统标准契约或自定义模板兼容路径；已有适配器明确拒绝 hook/template/custom-data，不能删除这些条件强行使用 Pulse。右侧业务预览与初始视图如果属于应用模板，需保持其通知/校验/结果语义，不能静默丢弃。用户暂不需要反复打开同一 R28 对话框。

R29 传统入口观察部署：用户确认关闭上一轮后，新包 20261030-r29-x64（34 文件完整性检查）及原快捷方式参数配置通过验证。新 AutoCAD PID 21460、父启动器 44364，会话 {D23DA02D-0C81-4149-BDD6-7B5909D74B0A}；主界面 Drawing1.dwg 已出现，日志明确 legacy-observers-installed-native-only=4。仅开启被动记录，传统选择窗口依旧由应用原接口处理；待用户打开/取消采集真实入口与扩展标记，不作为 Pulse 接管验收。

### R29 用户操作后日志核对

已检索本轮主会话及全部有日志的子会话：没有 GetOpenFileName/GetSaveFileName W/A 观察记录，也没有 COM 文件对话框目标激活/契约记录。主日志 7970 字节，未接近 256 条上限；诊断安装记录存在，未发现会话停用标记。不能把“没有记录”升级为已证实某个私有函数被调用；仍需区分私有导航界面、未接入进程或其他未观察入口。此前按截图猜测传统接口，R29 没有证实该猜测。

用户截图有应用自定义预览及初始视图。Autodesk ObjectARX 官方资料另提供 CAcUiNavDialog（AutoCAD 文件导航对话框）及 CAdUi/CAcUiFileDialog；它们说明可能存在应用专有导航路径，但文档本身不是本轮调用栈证据。AutoCAD 打开保持“未接管”，不继续要求用户在同版重复点击。标准接口通用覆盖与应用专有导航扩展应分开列入能力清单。

### Word R30：兼容原接口接受的预置结果状态

R29 用户打开后，日志到达 CoCreateInstanceEx，随后 ex-native-input-state，尚不能区分其输入 IID、指针或状态字段。独立真实 COM 回归发现：输入 MULTI_QI.hr 为 E_NOINTERFACE、E_FAIL、S_FALSE 时，Windows 仍能创建打开/保存对象，而旧包装前置判断跳过这六种情况。修复前 76 项中 6 项失败，修复后 76/0（word_seeded_status_before.log / word_seeded_status_after.log）。

R30 保留输入参数原样传给原接口，仅取消输入 hr 必须为 S_OK 的前置限制；空 IID、非空输入指针、原接口失败/部分成功及结果身份不一致仍不包装。增加有限的预置状态诊断，未增加 Word 名称分支。官方 MULTI_QI 文档仍要求输入 hr=0，此处是兼容原实现实际接受的调用，不能表述为文档规定该字段仅为输出。

受影响目标构建成功。R30 x64 独立包 34 文件完整性及 Word 启动配置检查通过；实际 Word 是否命中该修复以及打开/另存为结果仍待用户验证。单元检查不作为 Word 通过证据。

### R30 Word 实测缺口与下一阶段通用能力

R30 实际记录 ex-seeded-result hr=00000001、ex-wrapper-published，证实预置 HRESULT 的修复已生效。仍回退的首个原因是 SetClientGuid（reason=Persistence）；完整记录另有三次 AddPlace、EnableOpenDropDown、AddMenu、MakeProminent，以及最终 options=0x20001B00。这是标准接口组合尚未支持，并非又一个创建入口漏接。

本轮补充范围：Pulse 自有应用/GUID 目录记忆（不读写 Windows 私有历史）；文件系统侧栏位置；菜单与主按钮下拉；显式传递 FOS_NOVALIDATE 的跳过访问预检要求，仍检查所请求的路径/文件存在与结果有效性；保留已废弃 DEFAULTNOMINIMODE 位但不令其阻断扩展式 Pulse 窗口。文档依据：[ClientGuid](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-setclientguid)、[FDAP](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-fdap)、[Options](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_fileopendialogoptions)。

已通过：契约110/0，调用方校验wrapper47/0，菜单wrapper42/0，目录记忆/快捷位置/接受取消组合80/0，view测试20张布局图无失败。菜单测试第一次使用尚未包含selector的旧测试程序，意外运行原默认1160项（0失败）；不把它作为新菜单通过证据，后续正确selector为42/0。真实UI测试另发现测试驱动时序问题，仍在修复验证中；此段不代表Word已经通过。受限范围保留：虚拟Shell位置、UNC侧栏、运行中结构增删、任意元数据等尚未覆盖。

R31 本机阶段验证补充：实际菜单/主按钮下拉 UI 59/0（word_menu_ui_fixed_test.log），侧栏点击导航 UI 50/0（word_places_dpi_test.log），运行时能力匹配52/0（word_policy_final_test.log），位置标签处理后的wrapper组合80/0（word_client_places_labels_test.log）。菜单失败来自测试在popup隐藏后早于焦点恢复发键；侧栏失败来自测试线程DPI上下文与窗口不一致导致消息坐标虚拟化；修正测试同步/线程上下文后保留原断言通过。临时产品路径诊断已删除。已检查自有测试渲染图，split箭头与附加菜单可见；仍以实际Word操作结果作为应用验收依据。

### R32 定位 / R33 此电脑导航

R31 用户仍反馈原生。R32 精确记录：前两个 AddPlace 为属性0x60000000的文件系统目录；第三个属性0x20000000、GetAttributes=S_FALSE、SIGDN_FILESYSPATH=E_INVALIDARG，Shell命名空间为{20D04FE0-3AEA-1069-A2D8-08002B30309D}（此电脑）。截图另有网络/Linux/Microsoft Word，不能把它们的可见名称当作触发回退的证据；本轮明确原因是ComputerFolder。

R33连接标准ComputerFolder和选择器已有的This PC磁盘视图。以Shell对象身份比较识别，导航边界用明确内部token，UI继续复用空根磁盘枚举/返回/向上逻辑；GetFolder和FolderChanging还原为真正Computer Shell对象。只允许导航，不能把虚拟根冒充文件结果。没有Word进程名分支。新能力需要UI明确协商；缺能力保持显示前回退。普通网络/云/其他虚拟Shell目录仍不因此自动宣称支持。

定向模型Computer枚举→磁盘→真实文件检查通过；实际COM包装的Computer导航与缺能力回退31/0。实际UI导航及Word验收仍待完成。Word测试启动改用官方/w独立实例参数，避免已关闭窗口后残留进程复用旧组件：[Microsoft Office命令行参数](https://support.microsoft.com/en-us/office/lifecycle/command-line-switches-for-microsoft-office-products)。

R33 实际UI Computer导航48/0（word_computer_ui_test.log），截图picker-computer-42944.png确认磁盘视图显示正常、虚拟根不能提交。独立包34文件完整性验证通过。已创建Pulse-Word-R33.rtf测试文档并以/w启动独立Word实例；本段仍不等同Word打开/另存为验收通过。


### R33 用户反馈与 R34 保存补充（2026-10-30，进行中）

用户对独立 `Pulse-Word-R33.rtf` 窗口反馈“打开正常了，另存为还是原生”。打开窗口接入和取消已获用户确认；不将其扩展为实际文件读取或保存通过。

R33 保存日志显示 `SetCollectedProperties(append=FALSE)` 包含8项属性，以及 radio/check/edit 控件；最终 options 为 `0x20000102`。R34 补充通用属性快照/编辑、控件同步和明确的调用方保存校验能力，无 Word 进程名专用分支。未实测前不标记 Word 另存为完成。

属性接口依据：[GetProperties](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifilesavedialog-getproperties)、[ApplyProperties](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifilesavedialog-applyproperties)。展示期间可读取属性，取消后不暴露成功结果；应用主动调用 ApplyProperties 时仍调用系统实现，不由 Pulse 擅自写入文档。


R34 定向验证进展：属性模型通过（含 Word 八项真实描述、只读 EncryptionOwners 拒绝、取消恢复）；COM 属性桥接104/0（含 Word `0x20000102` 保存选项）、值控件桥接57/0、合同116/0、运行策略56/0、保存重验证34/0。真实 UI 属性增删48/0；初版 `property-edit` 和 `value-controls` 在编辑回调关闭时触发看门狗，仍在修复，不能将这些失败算为通过。新属性布局已检查截图，删除按钮与输入同行、另有小型新增按钮。

本轮明确边界：属性 UI 最多16字段、每个向量最多16项，超界在显示前回退；显式完整收集列表（append=FALSE），只支持可编辑文本和文本向量。未实现展示期间重建属性列表。调用方保存校验模式仍保留本地路径语法、父目录存在、元数据可读及非reparse限制，不承诺全部原生NOVALIDATE语义。


R34 后续修复验证：编辑通知期间发起 Close 后，modal loop 在循环顶部及 PeekMessage 返回后直接优先检查取消信号，消除持续消息队列下不结束的问题；property-edit 真实UI45/0。首帧 DXGI_STATUS_OCCLUDED 仅进行最多1秒有界可取消重绘，必须实际Present S_OK才发READY，严重错误/持续遮挡仍失败。最终 value-controls44/0、property-scroll（3向量+5单值，滚动最后一项编辑并取消）45/0、menu-open受影响回归59/0；property-mutate48/0。截图 `bench_data/picker-properties-scroll-52704.png` 已人工视觉检查，无字段与页脚重叠。前述失败作为修复前证据保留，不计入通过。

Word 实际另存为仍需 R34 独立实例人工验收；上述检查证明本机测试程序中的接口和界面行为，不等同于 Word 文件写出成功。


### R34 实测回退与 R35 属性显示转换（2026-10-30）

用户确认 R34 另存为仍为原生。实际日志 `compat-word-r34/{CB2AFAED-1C93-4F94-9812-332752784F0D}/runtime-decisions.tsv`：`properties-capture=S_OK`、8项显式收集列表，随后 `properties-collection-capture=E_NOTIMPL`，在显示 Pulse 前回退。该日志未记录每个原始值类型，不能声称已证实是某一特定字段或 VT_NULL。

旧模型要求每个值类型已与属性描述完全相同，未覆盖 Windows 允许的非规范输入。新增回归先复现 NULL 标题和单字符串作者的失败（`word_r35_properties_before.log`），再通过 Windows `PSCoerceToCanonicalValue` 转换私有显示副本；只接受 S_OK、禁止截断，原始 store 不因显示转换而变化。已经规范的向量仍逐项保持，不重新拆分分号。未编辑类型、显式编辑和取消恢复均已验证；完整属性桥接161/0（含 Word 保存选项、非规范值、成功及取消）。模型测试全部通过，仅构建受影响目标，不重跑无关 UI。

参考：[Windows 属性规范转换](https://learn.microsoft.com/en-us/windows/win32/api/propsys/nf-propsys-ipropertydescription-coercetocanonicalvalue)。R35 若仍回退，将记录各字段 expected/actual 类型和 HRESULT，不记录属性值。Word 实际保存仍待用户验证，不能由模型测试替代。


### R35 实测与 R36 配置校验定位（2026-10-30）

用户确认 R35 另存为仍原生。实测日志显示属性快照与收集均 S_OK，contract Prepared、native_only=0、Save options=0x20000102；随后 inprocess Prepare 返回 E_INVALIDARG。这证明属性转换已越过前一回退点，但不代表整体接入完成。

新增 `--metadata-real-config <UI DLL>` 将 wrapper 生成的完整配置交给真实 UI ValidateConfig（不显示窗口），替代此前仅测试替身接受的盲区。含八属性、17筛选器、Computer及文件夹位置和隐藏radio/check/edit的组合检查225/0。实际 Word配置仍存在未捕获差异。

另修复 EndVisualGroup 后 AddPushButton/AddComboBox 的 parent_id 残留：has_parent=false 时归零；真实UI配置及回调检查65/0。未证明这是 Word 本次回退的原因。

R36 在完整配置校验失败时才进行只读分段探测（基础、标签、筛选器、位置、控件前缀、单属性），仅记录部分名、编号、计数、HRESULT。探测配置绝不用于 Show，原校验失败仍回退。R36 是修复加定位版本，Word另存为尚未通过，不保证用户下一次看到Pulse。


### R36 Word 实际保存通过（2026-10-30）

用户反馈“可以了”。本机日志与文件交叉确认：session `{445EFDEB-C107-4E1D-876F-105E0A7F97EF}` 的属性收集、合同和 UI Prepare 均成功；`inprocess-api-show-return hr=0 status=1 flags=1`，FileOk/GetResult均S_OK，没有这次显示前的原生回退。用户实际执行了保存（不是原计划的仅取消）。

自有 `bench_data/compat-word-r36/Pulse-Word-R36.rtf` 从初始117字节变为41855字节，RTF头有效，`Pulse Word R36 test.` 与 `Save As configuration diagnosis.` 两段正文均保留。此证据确认实际文件写出和正文保留，不声称重新打开/复杂内容完整性已验证。

范围：Word R33打开窗口接入与取消用户确认；R36另存为界面接入、接受和RTF写出通过。R36取消尚无日志记录，已单独询问用户；DOCX/其他格式切换、编辑属性写回、所有第三方应用均不由此自动视为通过。没有再次扩大构建或重跑历史测试。


### R36 Word 取消补测通过（2026-10-30）

用户对重新打开的 R36 测试窗口反馈“正常”。session `{1253D65C-560B-4545-B761-ABC5784FE7B6}` 日志确认合同及UI Prepare成功，`inprocess-api-show-return hr=0x800704C7 status=2 flags=1`：Pulse已显示并正常返回ERROR_CANCELLED，没有原生回退。

本轮验收收口：Word打开窗口与取消（R33用户确认）、另存为Pulse界面与RTF实际写出（R36用户反馈及文件证据）、另存为取消（R36用户反馈及日志）通过。其他文件格式、复杂文档及编辑属性写回仍不在本轮已验收范围。此轮仅补人工取消验收和文档记录，未重新构建或运行无关测试。


### QQ R36 实测准备（2026-10-30）

用户要求继续其他软件，选择此前未验收的 QQ 9.9.19.34606 x64。用户正常退出后确认QQ进程清空，通过固定 `20261030-r36-x64` 包启动，主进程12688、session `{6AFADC96-4266-47D8-937A-DF6BBB45BDC2}`；历史初始化握手成功。控制目录 `bench_data/compat-qq-r36`，独立测试文件 `pulse-qq-sample.txt`。已请用户分别验证选择文件/另存为及取消，不自动发送文件。启动成功不计为接口兼容通过，实际结果待反馈。本阶段没有修改代码或重新构建。


### QQ R36 回退与 R37 默认目录接口（2026-10-30）

用户截图确认QQ选择文件仍原生。R36主进程日志明确进入CoCreateInstance包装，SetDefaultFolder S_OK被旧代码直接标为unsupported；最终options0x1A48，DefaultDirectoryPolicy回退。不是本次对话框漏接子进程。

R37保留原生SetDefaultFolder调用及结果，记录为独立setter，不设置explicit_folder；仅LocalOpenV1/LocalSaveV1且已有Show前native GetFolder解析证据才允许。最终目录仍由原生GetFolder与已有Pulse目录记忆逻辑决定，显式SetFolder优先，不把默认目录强行当显式目录。无QQ进程名分支。

定向验证：真实COM＋真实UI ValidateConfig默认目录/已记忆目录/先显式后默认三种场景40/0，取消不写记忆；合同回归116/0。参考：[SetDefaultFolder](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-setdefaultfolder)、[GetFolder](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-getfolder)。仅构建受影响目标。

R37固定包34文件完整性检查通过。确认QQ旧进程已退出后，创建 `bench_data/compat-qq-r37` 配置并启动新QQ；实际选择文件/另存为及取消待用户测试。完整性与握手不代表实际对话框通过。

### QQ R37 实测：同进程跨线程 owner 显示前拒绝（2026-10-30）

- 用户反馈点击后没有任何窗口。会话 `{BD652D7B-AF7E-4744-8D2A-6A10A7127C6E}` 的准备成功，owner 日志为 samepid=1 / samethread=0 / enabled=1；Show 返回 `0x80070032`、win32=50、flags=0，未显示UI。
- `PickerOwnerGuard` 当前将所有跨线程且启用的owner拒绝；不能把R37默认目录配置通过当作窗口验收通过，也不是再次缺失注入。
- 新增真实UI用例 `cross-thread-owner`：独立消息泵线程创建自有owner，调用线程仍保留Show及COM回调。使用不可变R37 DLL复现25项检查中3项失败，错误与QQ一致。证据：`bench_data/qq_r38_cross_thread_before.log`。
- 修复必须将主窗口禁用/恢复在其所属线程执行，限制等待并处理销毁、超时及组件生命周期；不得简单删除保护后跨线程无限等待。实际QQ验收仍待修复后执行。

### QQ R38：跨线程 owner 模态桥修复与本机验证（2026-10-30）

- 同进程跨线程且enabled的owner，通过仅目标线程的WH_GETMESSAGE/PM_REMOVE钩子处理带代次token的内部消息，在窗口所属线程调用EnableWindow；调用者等待独立event最多2秒。未将COM回调迁移到owner线程。
- 超时保留身份与恢复状态，阻止同一窗口重复接管；owner恢复响应后在原线程回滚，或在线程退出后清理。原disabled的owner不被擅自启用；跨进程enabled限制保留。
- 代价：首次使用此桥时包含回调的模块固定驻留至宿主进程退出；仅超时路径需要额外恢复等待线程。若宿主永久无响应，恢复不能即时完成，相关状态保留至恢复或进程退出。这不是全局接入或所有应用兼容承诺。
- 最终构建：UI组件、runtime、独立owner guard测试成功，无新增编译警告。独立生命周期测试29/0，真实UI跨线程Close 39/0、窗口取消41/0、实际双文件接受48/0，同线程重入保护39/0。日志：`bench_data/qq_r38_owner_guard_final.log`、`bench_data/qq_r38_final_*.log`。最初销毁测试在WM_NCDESTROY返回前发完成信号导致1项失败，修正为销毁返回后的同步，保留Alive失效断言后通过。
- 不可变包 `bench_data/dialog-bundles/20261030-r38-x64`，34个文件完整性校验通过（不是应用兼容认证）。QQ从该包重新启动，control root=`bench_data/compat-qq-r38`；实际选择文件/取消和另存为仍待用户验收。
- Windows约束参考：[GetMsgProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/getmsgproc)、[UnhookWindowsHookEx并发回调](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-unhookwindowshookex)、[SendMessageTimeout同输入队列不受超时约束](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw)。

R38实际QQ反馈更新：用户确认“可以了”。会话 `{0BE6C278-7F3A-4264-9AAD-3E0B6D493D64}` 的同进程跨线程owner进入成功，真实Show返回 `0x800704C7 / status=2 / flags=1`，确认Pulse已显示且取消完成。QQ实际选中文件返回、另存为仍单独待验收；未发送任何文件。取消后的ClearClientData仍返回E_FAIL，此项不计为已支持，不影响本轮显示取消证据。

### R38 QQ 另存为视觉问题（待修复验收）

用户确认可弹出，但先出现透明窗口，再换位显示Pulse；不能据此判定另存为体验通过。当前实现首次ShowWindow早于首帧Render，初始化回调又可获取真实HWND并请求显示/定位。R39正在补隐藏状态下完成布局及首帧提交、限制初始化提前显示，同时保留宿主定位请求。取消日志不代表消除了透明首帧或跳位；需单独画面与实际QQ验收。

### R39 首次显示顺序修复（2026-10-30）

- `folder_picker_dialog.cpp`：初始化期间限制SWP_SHOWWINDOW，但保留宿主位置/尺寸请求；先在隐藏窗口完成布局、自绘输入和完整首帧Present，成功后才允许显示，READY仍需可见帧验证。隐藏绘制按子窗口WS_VISIBLE判断布局可见性，避免父窗口隐藏时漏画文件名等字段。
- 新增真实保存用例`startup-visibility`：初始化TypeChanged中模拟宿主提前ShowWindow及SetWindowPos，线程消息钩子记录第一次实际可见位置，首显时读取先前生成的GPU帧PNG。检查实际画面覆盖、文件主体不透明、多颜色内容，及首显位置等于最终请求且无可见换位。正常Acrylic/Mica侧栏按既有材质透明度验证，不能将其误判空壳。
- 相同最终测试：不可变R38为50项/5失败，修复版50项/0失败。日志`qq_r39_startup_old_final.log`、`qq_r39_startup_final.log`。最终首显前图像`bench_data/picker-v2-50116-215049765-startup.png`；已检查同实现前一张实际图像，完整布局及自绘文件名存在。
- 直接相关功能：跨线程用户取消41/0、双文件接受48/0、保存自绘属性编辑取消46/0。仅构建UI和对应测试；未以全量历史测试替代此次首帧验证。
- R39不可变包34文件完整性校验通过；用户已退出旧QQ，已通过新包启动，control root `bench_data/compat-qq-r39`。实际QQ透明闪现/跳位的视觉验收仍待反馈。

### R39实际反馈失败，补齐首个FolderChanged路径

用户反馈R39仍相同，不能将startup-visibility模拟通过视作QQ已修复。真实QQ日志显示：Ready(kind=1)之后才FolderChanged(kind=3)，QQ在kind=3中请求GetWindow；之前模拟在TypeChanged中移动窗口，没有覆盖此时序。该证据尚不足以直接断言QQ回调修改了矩形，下一版补回调前后RECT/可见性/窗口样式诊断（不记录文件名或内容）。正在新增FolderChanged时才显示/定位的回归，并将初始目录验证与后台完整枚举拆分，使宿主首轮布局回调可在窗口首显前完成。

### R40 初始文件夹通知提前到首显之前（2026-10-30）

- loader为嵌入式对话框新增轻量folder-commit消息：后台真实打开目录并检查属性成功后发布generation/path，完整枚举仍在后台执行；Computer按既有虚拟目录语义处理，不伪造磁盘路径。最终listing不重复发送FolderChanged。
- 隐藏初始化消息泵等待有效commit及宿主重定向回调完成，再布局/绘制/首显。5秒截止，仅作用于嵌入模式；处理中响应取消、关闭、owner失效及WM_QUIT。验证失败保留原默认目录尝试策略，最终失败明确返回错误且不报告UI_SHOWN。慢枚举不阻碍首显；已显示后枚举失败沿用原错误可取消页面。代价是一次额外后台目录打开检查，初始验证超过截止会失败；真实慢设备5秒超时未作可控实测，不能写成已验证。
- 新`startup-folder-visibility`模拟QQ首次FolderChanged内才显示和定位：不可变R39为54项/7失败，R40为54项/0失败。初始回调Close 33/0、窗口取消31/0、真实目录占用失败32/0、Computer导航49/0、跨线程双文件接受48/0。首显前实际GPU截图`bench_data/picker-v2-45520-216343750-startup.png`已查看，包含完整Pulse保存界面与文件名。
- wrapper增加宿主每次回调前后窗口RECT/可见性/扩展样式诊断，用于判断实际QQ是否移动窗口；不记录文件内容或聊天文字。受影响UI、runtime和测试目标构建成功。
- 包`bench_data/dialog-bundles/20261030-r40-x64`34文件完整性校验通过；QQ已通过`bench_data/compat-qq-r40`重新启动。以上仅为本机工程证据，QQ透明闪现/换位视觉验收仍待反馈。

R40 QQ实测更新：用户确认透明闪现/换位“解决了”。会话 `{6D472BE9-ED74-4CAE-8C3D-61F99E01ACF4}` 首个FolderChanged回调前后均visible=0，矩形由(451,868,1951,1828)变为(1170,600,2670,1560)，实证QQ在此回调定位窗口且现在完成于隐藏阶段。Show返回ERROR_CANCELLED / status=2 / flags=1。因此本轮另存为显示、无透明换位及取消通过；实际写出文件尚未单独验收。

下一应用Excel：复用不可变R40通用包，无Excel专属代码改动。创建隔离`bench_data/compat-excel-r40/Pulse-Excel-R40.xlsx`（包完整性及XML解析通过），通过EXCEL.EXE /x启动独立实例。control root=`bench_data/compat-excel-r40`，实际打开、另存为、取消待用户测试。

### Excel R40：保存已显示Pulse，打开筛选缺口

用户反馈“打开有问题，保存正常”，截图为系统打开窗口。会话 `{493225CF-8FC5-4D0F-AECC-171CA0E3E528}` 显示Save进入Pulse，Show返回ERROR_CANCELLED/status2/flags1；Open options=0x20001B00，在SetFileTypes(call6)因UnsupportedFilter(reason12)回退，尚未尝试显示Pulse。截图包含`*.xl*`，当前contract/UI只接受固定扩展名，因此需补通用扩展名通配符匹配；不能删除过滤或把Excel整次请求强制放行。后续追加filter-shape诊断只记录语法数量，不记录文件名/标签。

Excel R41原生筛选对照：前三阶段使用同一原生Open，调用方默认txt，依次实际选择*.md、*.xl*;*.xlsx、*.*，每阶段均由OnTypeChange/GetFileTypeIndex以及FileOk结果确认。最终日志`bench_data/excel_r41_native_sequence3.log`观察到literal.md、wildcard.txt、all.txt。因此本机Common Item Dialog在通配项使用caller默认txt，不保留上个literal md，也不猜后续xlsx。前两轮驱动没有真正切换类型，不能用于语义结论。第三轮整体仍有2项清理失败（ClearClientData=E_FAIL、CloseDesktop失败），保留fixture并明确区分，不能称完整测试通过。产品按实际观察只扩Open扩展名星号语法，Save及未验证问号语义保持原限制。

### Excel R41：通用 Open 扩展名星号过滤

共享解析器接入 contract 与 UI，支持 *.xl* 等扩展名星号以及分号组合，保留完整过滤表达式；仍拒绝路径、文件名前缀及未验证的问号语义。默认扩展名遵循本机原生对照：固定项使用首个固定扩展名，星号项使用调用方原始默认扩展名。Save 规则不变，没有 Excel 专用分支。增加只含语法计数的 filter-shape 与自定义控件拒绝原因日志。

定向验证：contract --extension-masks 17/0；model --extension-masks 退出0；实际 Pulse UI open-mask-sequence 70/0，确认 literal.md → wildcard.txt → all.txt。日志为 bench_data/excel_r41_contract_masks.log、excel_r41_model_masks.log、excel_r41_ui_masks.log。原生对照清理限制见上文，不计为完整通过。R41 包34文件完整性校验通过，不代表应用兼容性认证。

已从无 Excel 进程状态启动独立 Pulse-Excel-R41.xlsx，control root=bench_data/compat-excel-r41。实际 Excel 打开及取消待用户验收；R40 保存显示正常的用户反馈仍保留，未推定实际文件写出成功。

R41 Excel 用户验收更新：用户针对打开及再次打开取消的测试请求回复‘可以了’，本轮打开回退问题确认解决。保存保留此前正常反馈；未额外核验实际文件内容和保存写出。
