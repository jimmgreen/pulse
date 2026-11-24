# 对话框接管：为什么每台电脑不一样，以及最好的兼容方案（2026-11-20）

本文只读分析，未改代码或登记。依据当前工作区源码（含今天仍在修改的 `user_auto_attach.cpp` 等）和本机真实状态。

## 1. 结论

接口契约层（`file_dialog_wrapper` / `runtime` / 远程 UI）已经做得很扎实，几百项契约与真实窗口验收都在。**每个人、每台电脑结果不一样，主因不在契约层，而在“入口层”**：DLL 能不能、什么时候进入目标进程，以及进入后能拦到哪些调用。这些都依赖机器上的状态：

- 时序：每 500 ms 轮询一次可见窗口；
- 本机学习记录：先出现过一次原生对话框，之后才接管；
- 指纹：包含文件大小和修改时间，软件一更新就失效；
- 熔断记录：只存在本机，用户看不到；
- 钩子时机：导入表钩子只在接入那一刻打一次；
- 杀软：远程线程注入，加上程序未签名。

**最好的兼容做法**有三条：

1. **入口改成系统保证加载，并由事件驱动**：把“轮询 + 远程线程注入”换成“窗口创建事件 + 针对目标线程的消息钩子”。
2. **拦截改成函数级，覆盖后加载的模块**。
3. **策略改成静态、与机器历史无关**：不再有学习门槛，指纹按版本号。

同时：

- 把“未知”原因拆到可判定，让本机记录直接说清是哪一类问题；
- 拒绝接管时退到“原生对话框 + Pulse 增强”，而不是纯原生；
- 兼容性改用**环境矩阵**（系统版本 × 杀软 × 位数 × 权限 × 加载时机）自动测试。

用户日志不再用来猜原因，只用来发现矩阵里没覆盖的新类别。

## 2. 现有入口（源码核对）

| 入口 | 机制 | 覆盖 | 源码 |
|---|---|---|---|
| 普通 COM | HKCU 覆盖 FileOpenDialog / FileSaveDialog 两个 CLSID（64 位和 32 位两份视图） | 未提升进程中通过 CoCreateInstance 创建的 IFileDialog；激活时生效，没有时序问题 | `dialog_auto_registration.cpp`、`file_dialog_auto_server.cpp` |
| 托管启动 | launcher + Detours，启动时注入 | 只限从托管入口启动的进程 | `dialog_launcher/main.cpp`、`managed_launch_profile.cpp` |
| 用户自适应（今天在改） | 每 500 ms `EnumWindows` 扫可见窗口。导入表或学习记录命中后，用 `OpenProcess(VM_WRITE\|CREATE_THREAD)` + `WriteProcessMemory` + `CreateRemoteThread` 注入，再在目标进程里改导入表 | 旧式 A/W、SHBrowse、CoCreateInstance(Ex)、CoGetClassObject 的静态导入和延迟导入 | `user_auto_attach.cpp`、`dialog_adaptive_attach.*`、`runtime_auto_imports.cpp` |
| 管理员自动 | 与上一行相同，`EnumWindows` 加 `CreateRemoteThread`。11-11 原型曾用针对线程的 `WH_GETMESSAGE` 验证过加载 | 同用户、同会话的提升进程 | `admin_auto_attach.cpp` |

## 3. 本机证据（只读）

- **登记**：HKCU 两份视图都指向 `C:\Program Files\Pulse\DialogBridge\<hash>\PulseDialogAuto64.dll` 和 `PulseDialogAuto32.dll`，状态正常。
- **自适应进程**：`PulseDialogAdminAuto.exe` 在运行，共 2 个进程（x64 主进程和 x86 伴随进程）。
- **学习记录** `dialog_adaptive_targets.json`：已学会 4 个程序（QQ、acad、AppGstr、ChatGPT），熔断 0 个。也就是说，这 4 个程序在本机都是先原生弹过一次，之后才接管。
- **签名**：已安装的 `pulse.exe` 和 `PulseDialogAuto64.dll` **都没有 Authenticode 签名**。今天 B1 测试时，火绒拒绝了未签名程序写回收站（`fallback_error=5`）。
- **接管记录** `takeover-history.json`：84 条，约 37 个程序。

| 原因 | 记录数 | 事件数 |
|---|---|---|
| 显示后关闭 / 成功 | 50 | 193 |
| 不支持的请求 | 12 | 35 |
| **未知** | 7 | **46**（其中 chrome Failure:Unknown ×34、YjkLauncher ×5、Cursor ×3） |
| **处理失败** | 8 | **22**（Excel、Word、Edge、PickerHost、powershell、python、winforms 测试宿主） |
| 原生回退 | 5 | 15 |
| 组件不一致 | 1 | 2（QQ 加载了旧组件） |
| 初始文件夹不支持 | 1 | 2（yjks） |

按事件算，约 30% 的非成功事件属于“未知”或“处理失败”这种不细分的原因。这正是“只能要用户日志”的根源。

## 4. 不稳定的根因（按影响排序，均有源码依据）

**R1 学习门槛导致首次原生，而且每台机器的学习历史不同**

> 11-21 更正：门槛只作用于"导入预检什么都没扫到"的进程。预检把 `CoCreateInstance`、`ShellExecute*`、comdlg32、WinForms、Qt、CEF 等都算作信号，大多数 GUI 程序第一次就会命中。所以 R1 的实际影响比原先估计的小。去掉门槛等于给所有无信号进程注入，风险大于收益，暂不做（见 §9）。

- 没有导入信号的进程，要等 `ObserveNativeFileDialog` 先看到一次原生 #32770，再 `RecordLearned`，之后才会接入。
- 所以同一个软件：A 机器已经学过，接管；B 机器第一次用，原生。

**R2 指纹包含文件大小和修改时间**

- `ComputeBinaryFingerprint` 的组成是：路径哈希、文件大小、`ftLastWriteTime`。
- QQ、微信、Cursor、ChatGPT 这类自动更新的软件，每更新一次，学习记录和熔断记录都会失效，于是又回到原生。用户的感受就是“昨天还行，今天不行”。

**R3 时序与退避**

- 每 500 ms 扫描一次，只看有可见顶层窗口的进程，最多 4 个并发。
- 导入预检没命中时，按 5 s、20 s、60 s、120 s 退避。
- 启动后很快弹框，或者后来才加载插件的进程，会错过。机器快慢不同，结果就不同。

**R4 导入表快照**

> 11-21 更正（以另一会话未提交的最新源码为准）：
> - 用户自适应路径已经改用 Detours 内联钩子（`AttachUserAutoHooks`），后加载的 DLL 和 GetProcAddress 都能拦到。
> - 管理员（导入表）路径已经有 `AutoModuleEpoch`（`LdrRegisterDllNotification`）加 `MonitorAutoImports`，每 250 ms 给新模块补钩子。
> - 剩下的缺口只有一处：管理员路径没有拦截 GetProcAddress。下面原文保留作记录。
> - 11-21 补：这个缺口已由批次 (B) 补上，x64 和 x86 测试层面验证通过，见 9.6。

- 只在接入那一刻改一次导入表。`runtime_auto_imports.cpp` 里没有监听 DLL 加载（`LdrRegisterDllNotification`），也没有拦截 `GetProcAddress`。
- 接入之后才加载的模块不会被覆盖，例如插件、Qt / CEF 组件，以及通过 GetProcAddress 取函数地址的调用方。
- 同一台机器上，用户先做什么后做什么，结果都可能不同。

**R5 注入手段和杀软**

- 跨进程写内存加远程线程，是杀软启发式规则最常盯的行为组合，再加上程序未签名。
- 火绒、360、电脑管家的版本和规则在每台机器上都不同：有的直接拒绝，有的弹窗询问，有的放行。
- 当前记录里这类失败多半显示为“未知”。

**R6 熔断是看不见的本机状态**

- 连续失败 2 次就会熔断。
- 用户不知道某个软件已经被熔断，而换一台机器又没有熔断。

**R7 原因不细分**

- “处理失败”和“未知”没有子码，无法区分 IPC 超时、UI 进程未运行、UI 崩溃、被拦截、被另一个工具改了登记等情况。

**R8 已运行进程和登记被覆盖**

- 升级后，已运行的进程还挂着旧组件（QQ 的“组件不一致”）。
- 其他对话框工具可能覆盖 HKCU 里的同一个 CLSID。
- 提升进程会忽略 HKCU 里的 COM 登记（已知限制）。

## 5. 最佳方案

### 5.1 入口：事件驱动加针对线程的钩子（取代轮询和远程线程注入）

- **发现**：用 `SetWinEventHook(EVENT_OBJECT_CREATE / EVENT_OBJECT_SHOW, WINEVENT_OUTOFCONTEXT)` 在 helper 线程里接收窗口创建事件。
  - 进程外回调不向任何进程加载 DLL，窗口一创建 helper 就能知道，不必再每 500 ms 轮询。
  - `EnumWindows` 只保留为低频补扫。
- **加载**：策略允许后，对目标进程的 UI 线程调用 `SetWindowsHookEx(WH_GETMESSAGE, …, dwThreadId)`，由系统把一个很小的入口 DLL 加载进**这一个进程**。
  - 这个机制 11-11 已经在普通和管理员 token 下验证过（`dialog_message_entry_probe.cpp`）。
  - 加载后入口 DLL 再从版本化 bundle 加载真正的 runtime，并安装拦截。
- **好处**：
  - 不再需要 `PROCESS_VM_WRITE` / `CREATE_THREAD` / `WriteProcessMemory`，这类行为通常比远程线程更少触发杀软（需要在火绒、360 虚拟机上实测确认）；
  - 已经在运行的进程也能接入；
  - 只加载进选中的进程，排除名单继续有效；
  - 延迟从“最长 500 ms 加退避”降到“窗口创建后几毫秒”。
- **边界（如实）**：
  - 提升进程仍需要提升权限的 helper（UIPI 限制）；
  - AppContainer 和受保护进程不接入；
  - 线程第一个窗口就是对话框本身的情况，接入时已经太晚，这一次交给 5.4 的第二级。
- **不建议用全局 `WH_CBT`**：它会把 DLL 加载进所有 GUI 进程，包括带反作弊的游戏和浏览器，风险不可控。

### 5.2 拦截：覆盖后加载的模块

两种做法，建议先做 A：

- **A（贴合现有实现）**：保留改导入表的方式，再补三处：
  - 用 `LdrRegisterDllNotification` 给新加载的模块补钩子；
  - 拦截 `GetProcAddress`，对 7 个目标函数返回包装后的入口；
  - 已打过钩子的模块做幂等处理。

  这样能保留“不修改指令”的安全性质。
- **B（覆盖最全）**：用 Detours 对 comdlg32、shell32、combase 的导出函数做 inline 钩子，任何调用路径都能拦到。代价是要挂起线程、改指令，CFG / 杀软的风险更高。只有 A 落地后数据仍显示覆盖不足时再考虑。

### 5.3 策略：静态、可解释、与机器历史无关

- **去掉学习门槛**：
  - 不在排除名单内、用户没有关掉的进程，一律接入入口 DLL。入口 DLL 很小，不拦截就没有成本。
  - 是否真的接管，由每次调用时已有的契约检查决定。
  - 导入信号只用来排优先级，不再作为门槛。
- **指纹改用版本号**：用 `VS_FIXEDFILEINFO` 的版本号加签名者（或产品名），不用大小和修改时间。小版本更新不重置；熔断只在大版本变化时重置。
- **熔断可见**：在“软件接管记录”里显示“已暂停接管（原因、次数）”，并提供“重新尝试”。
- **排除和已知问题版本做成规则文件**：签名或校验哈希，可以单独更新，不必等发版。

### 5.4 兜底分两级

- **第一级**：完整的 Pulse 窗口（契约支持时）。
- **第二级**：原生对话框加 Pulse 增强（跳转栏、最近位置）。Edge 已经是这种形态。

契约不支持、接入太晚、提供方失败时，走第二级而不是纯原生。这样用户感受到的一致性会明显提高，而且不会冒险。

### 5.5 原因必须可判定（消除“未知”）

- **入口层**新增原因：
  - 已排除；
  - 接入太晚（对话框先于接入）；
  - 钩子安装失败（带 Win32 错误码）；
  - 入口 DLL 被拦截，即 `LoadLibrary` 返回拒绝访问，或模块被意外卸载，在界面上提示为“安全软件拦截”；
  - 对应位数的 helper 未运行；
  - 提升进程但没有管理员 helper；
  - AppContainer。
- **处理失败**拆出子码：IPC 超时、UI 宿主未运行、UI 进程退出、握手版本不匹配、登记被其他工具覆盖。
- **一键兼容性报告**：
  - 环境信息：系统版本号、`root\SecurityCenter2` 里的杀软产品、两种位数 helper 的状态、登记状态、管理员 helper 的状态；
  - 再加每个软件一行接管记录；
  - 不包含文件名和内容。

### 5.6 签名与杀软（非代码，但可能是国内最大的差异来源）

- 所有发布的二进制做代码签名，EV 证书最好，也有利于 SmartScreen 信誉。
- 向火绒、360、腾讯电脑管家、Microsoft 提交白名单或误报申报。
- 在没有签名的情况下，任何注入方案在不同机器上的结果都不可控。

### 5.7 测试：环境矩阵，而不是应用清单

- **宿主集**：约 20 个最小宿主，一部分已经存在，如 `legacy_win32_app`、`live_e2e_x64/x86`、`winforms_folder_app`（更正：仓库里没有 `dynamic_gpa_app`，`rg` 只在本文档中找到它；GetProcAddress 场景改用 `pulse_runtime_auto_imports_test` 加探针 DLL 验收，见 9.6）。覆盖维度：
  - 调用方式：旧式 A/W 带或不带 hook、IFileDialog 带或不带 events / customize、SHBrowse；
  - 框架：.NET WinForms / WPF、Qt、Electron、Java、Delphi / MFC；
  - 位数：x86 / x64；
  - 权限：普通 / 提升；
  - 加载时机：启动即弹框、后加载 DLL、GetProcAddress、延迟导入、在 Pulse 之前已运行。
- **环境**：
  - 系统：Win10 22H2、Win11 23H2 / 24H2（以后加 ARM64）；
  - 杀软：无、火绒、360、电脑管家；
  - DPI：100% / 150%。

  用 Hyper-V 快照每晚跑一轮，输出矩阵，并与上一轮对比。
- **指标**：
  - 第一次对话框接管率：每个宿主新启动后，第一次弹框是否是 Pulse；
  - 各环境的通过率；
  - “未知”原因占比，目标为 0；
  - 从窗口创建到接入完成的 p95 延迟。

## 6. 实施顺序（每步都有可测验收）

1. **先补诊断（风险最低，1–2 天）**：做 5.5 的原因子码、杀软拦截识别和一键报告。先用真实数据验证第 4 节的排序，再动入口。
2. **策略**：指纹改按版本号；在开关后面去掉学习门槛。验收：用宿主集对比新旧的第一次对话框接管率。
3. **入口**：用事件驱动加针对线程的钩子取代 `CreateRemoteThread`，用户和管理员两条路径共用。验收：接入延迟 p95，以及火绒、360 虚拟机上的拦截和弹窗次数，新旧对比。
4. **拦截**：做 5.2 A。验收：后加载 DLL、GetProcAddress、延迟导入这三个宿主从原生变为 Pulse。
5. **第二级兜底**：所有拒绝路径统一走增强。
6. **签名和白名单申报**：与以上并行。
7. **环境矩阵 CI**：上面每一步都用它来验收。

## 7. 明确不做

- 按软件名写特例。
- 全局 `WH_CBT` / AppInit_DLLs。
- 覆盖 HKLM 里的 CLSID，或改 UAC 策略。
- 在契约不支持时冒险接管。

## 8. 协作注意

`user_auto_attach.cpp`、`runtime_auto_imports.cpp`、`legacy_com_provider.cpp` 等今天 05:37–05:46 仍有另一会话在改，并且都没有提交。落地第 3、4 步之前需要先和那条线对齐，避免互相覆盖。

## 9. 实施记录

### 9.1 批次 D1（2026-11-21）：诊断加策略

分支 `perf/file-ops-opt-20261120`，未提交。改动叠加在另一会话未提交的改动之上，改前的文件备份在 `bench_data/dialog-compat-20261121/orig/`。

**先查证据，再改代码。** 用本机运行日志（只读复制到 bench 目录）定位"未知"的来源：

- 历史里最大的一块"未知"是 chrome.exe 的 Failure:Unknown，34 次。原始事件是 `terminal=failed`、`ui_shown=1`、`hr=0x8007065B`（`ERROR_FUNCTION_FAILED`）。
- 前序日志为 `inprocess-api-show-return status=3 flags=1 win32=1627`：Pulse 对话框已经显示，之后由 picker UI 自己报失败。
- 这不是入口层问题。旧分类没有对应原因，所以落进了"未知"。
- 发行版里，picker UI 的失败阶段（`FailPaintedEdit(stage)`）只在测试钩子构建中输出，正式版会丢掉，所以目前还定位不到具体是哪一处失败。

**改动 1：新原因"已显示后失败"（`ShownThenFailed=15`）**

- `dialog_takeover_history.{h,cpp}`：
  - 条件是 `failed` 加 `ui_shown_known && ui_shown`，且没有其他已知原因。满足时不再归为"未知"。
  - 建议动作为"重试，再出现就导出诊断"（RetryDialog）。
  - Decode 允许读取 15。
- `dialog_management_ui.cpp` 的原因表加了一项。
- `catalog.tsv` 新增 `DialogHistoryShownThenFailed`（7194），然后运行 `gen_strings.py`。核对过，生成文件只多了这一条。

**改动 2：自适应指纹 v2，熔断可过期**

- `ComputeBinaryFingerprintV2`：
  - 由小写路径哈希、`VS_FIXEDFILEINFO` 文件版本号、产品版本号组成。
  - 文件没有版本资源时，退回 v1（大小加修改时间），行为与原来一致。
  - 链接新增 `version` 库。
- `user_auto_attach.cpp`：
  - 学习和查询都用 v2。
  - 旧的 v1 键仍然有效：已学习的条目第一次使用时迁移到 v2（`AdoptLegacyLearned`）；被熔断的 v1 条目仍按熔断处理。
- 熔断 7 天后自动过期并重试（`kBlockedRetryAfter`、`IsBlockedAt`）。重试时失败计数重新开始。
- 顺带修了一个已有缺陷：`updated_ft` 原来按 53 位截断后写入，重新读出来不是真实时间。现在改为十进制字符串，按精确值保存。
  - 旧文件里的数字读不出真实时间，熔断窗口从读入那一刻重新算，不会一读就过期。
  - store 的 version 改为 2。

**测试（runtask，交互会话，TMP 隔离）**

| 测试 | 改前 | 改后 |
|---|---|---|
| `pulse_dialog_adaptive_attach_test` | 40 项，2 项失败 | 57 项，同样 2 项失败 |
| `pulse_dialog_takeover_history_test` | 改前未单独跑 | 44/44 |

- 自适应测试的 2 项失败在改前就存在：`user binding accepts non-elevated same-user process` 和 `user binding passes format validation`，与本次改动无关，没有处理。新增的 17 项全部通过。
- 历史测试新增 3 项：已显示后失败有独立原因、该原因能持久化、未显示 UI 的失败仍是"未知"。
- 编译（/W4）：只有 3 处已有的警告（epub_document、global_search_window、app_runtime），没有新增警告。pulse 也完成了重建（BUILD_EXIT=0）。

**实机数据（同一份日志副本，新旧对比）**

| | chrome.exe 这条记录 | 其余记录 |
|---|---|---|
| 旧 | Failure / 未知(0) / 导出诊断，3 次 | probe.exe 两条"不支持"，不变 |
| 新 | Failure / 已显示后失败(15) / 重试，3 次 | 不变 |

指纹（`fpbench`，只读）：

- 本机 600 个程序 exe（Program Files、x86、LocalAppData\Programs）里，有版本资源的 273 个。
- 不在排除名单内的 526 个里，能用 v2 的 222 个，约 42%；其余退回 v1，与原来相同，不会更差。PotPlayer、Photoshop 附带的工具、豆包输入法等确实没有版本号（PowerShell `FileVersionInfo` 核对为 0.0.0.0）。
- 单次耗时（冷缓存）：v1 约 0.25 ms，v2 约 2.0 ms。每个新进程只算一次，可以接受，但确实变慢了。
- 本机 store 已学习 4 个软件（QQ、acad、AppGstr、ChatGPT）。QQ 当前二进制的 v1 仍匹配，会被迁移到 v2；另外三个找不到可读的安装路径，没法验证是否已经因为更新而失效。

**结论**

- 改动 1 直接消除了本机最大的一块"未知"，但它只是把原因分出来，没有修好 chrome 的失败本身。
- 改动 2 能让约四成的软件在同版本重新复制或打补丁后不必重新学习，并且熔断不再是永久的。它不会提高第一次对话框的接管率。

### 9.2 下一步建议（按收益排序）

1. ~~**把 picker UI 的失败阶段带进正式版日志。**~~ 已在 D2 完成（见 9.3），没有改 ABI。
2. **管理员路径补 GetProcAddress 拦截。**
   - 做法是比对真实 `GetProcAddress` 的返回值是否等于钩子原函数，相等就返回包装入口。不解析名称，也覆盖按序号取地址。
   - 只改 `runtime_auto_imports` 的钩子表。（更正：仓库里没有 `dynamic_gpa_app` 宿主，实际用 `pulse_runtime_auto_imports_test` 验收。）已完成，见 9.6。
3. **熔断在"软件接管记录"里可见，并提供"重新尝试"。** 需要 UI 和新字符串。已完成，见 9.7。
4. **入口层（WinEvent 加 WH_GETMESSAGE）。** 收益最大，风险也最大，放在诊断数据更完整之后再做。第一步“事件驱动发现”已完成，见 9.8；`WH_GETMESSAGE` 加载仍是待办。
5. **不做：** 去掉学习门槛（理由见 R1 的更正）。

### 9.3 批次 D2（2026-11-21）：正式版记录 picker 失败阶段

**目标。** chrome 的"已显示后失败"（hr `0x8007065B`，win32 1627）在正式版日志里只有一个笼统的 1627。picker 里至少有 40 处会以 1627 收尾，没法判断是哪一处。

**方案选择：不改 ABI。**
- `Show()` 严格要求 `result->size == sizeof(*result)`。直接在 `PulsePickerUiV2Result` 末尾加字段，旧调用方会被拒（`E_INVALIDARG`）。
- picker 已经在正式版里写运行时事件。远程宿主（Pulse 主进程）的事件进 `app-<pid>.jsonl`，进程内宿主进 `picker_ui-<pid>.jsonl`。
- 所以改为在失败出口写一条正式版事件 `picker_show_failed`。跨模块结构体和导出函数都没动。

**改动（3 个文件；改前副本在 `bench_data\dialog-compat-20261121\orig\`）。**

| 文件 | 内容 |
|---|---|
| `src/ui/folder_picker_dialog.h` | 新增内部结构 `FilePickerFailure`（stage、line、error、hr、两个编辑框错误、frame、occluded/ready/rendering）；`FilePickerOutcome` 增加 `failure`。只在模块内部使用，不是 ABI。 |
| `src/ui/folder_picker_dialog.cpp` | `NoteFailure()` 只记录第一次失败（根因，后续都是连锁反应）。行号用 `std::source_location` 自动取，所以 40 多处 `FailPaintedEdit` 调用点不用逐个改。另外给关键点起了固定名字：notify-shown、notify-ready、on-ready-rejected、file-ok-rejected、flush-busy、edit-check、edit-check-disabled、end-draw、prepare-present、strict-present、create-window、compositor-init、create-unsupported、create-edits、edit-create、edit-subclass、pump-wait、pump-get-message；已有的 flush-no-present 保留。end-draw 和 present 会额外带上 D2D/DXGI 的 HRESULT。 |
| `src/picker_inprocess/picker_ui_bootstrap_v2.cpp` | `ActiveShow.last_event` 记录最后一次交给调用方的事件类型（覆盖确认用 0x1000）。`ShowCore` 的两个失败出口调用 `LogShowFailure`：`exit=callback`（调用方回调失败）和 `exit=outcome`（picker 自己失败）。事件包含 12 个数字字段和 2 个固定文本字段，不含路径或用户文本。日志初始化抽成 `EnsureRuntimeLog()`，与已有的 `LogOwnerContext` 共用，仍然只在出问题时才打开。proof 构建在 `PULSE_PICKER_UI_DIAGNOSTICS=1` 时把同一份记录打印到 stderr。 |

事件格式：`picker_show_failed {exit, stage, hr, win32, flags(1 已显示|2 已回调|4 首帧被遮挡|8 可提交|16 渲染中；D3 起追加 32 自身最小化|64 所有者最小化|128 被系统隐藏或随所有者隐藏|256 不可见|512 被应用隐藏), last_event, outcome_status, outcome_error, ui_line, ui_hr, rename_error, filename_error, frame, show_id}`

**构建。** 目标 `pulse_picker_ui_proof`、`pulse_picker_inprocess_v2_test`、`pulse`：`BUILD_EXIT=0`。只有 3 条已有警告（epub_document:554、global_search_window:1053、app_runtime:2810），没有新警告。

**实机测试（同一台机器，同一套 `tpick.cmd`，通过计划任务在交互会话里跑，私有桌面）。**

| 用例 | 旧 DLL（改前源码单独构建） | 新 DLL | 新增诊断 |
|---|---|---|---|
| startup-folder-error | 34 项 / 0 失败 | 34 / 0 | `exit=callback stage=other hr=80070005 last_event=2 ui_line=702` |
| startup-folder-close | 35 / 0 | 35 / 0 | 无（不是失败） |
| startup-folder-cancel | 33 / 0 | 33 / 0 | 无（不是失败） |
| ready-close | 35 / **3 失败**，`SHOW=8007065B status=3 flags=1` | 35 / **同样 3 项失败**，结果相同 | `exit=outcome stage=flush-no-present hr=8007065B flags=7 last_event=3 ui_line=2403 frame=0` |

- 新旧结果完全一致，说明这次改动没有改变行为。
- ready-close 的 3 项失败改前就有，是环境原因：非交互的私有桌面不会被 DWM 合成，首帧一直是 `DXGI_STATUS_OCCLUDED`（测试源码注释里也写了这一点）。
- **重要发现：** 这个环境失败的签名，和 chrome 的真实失败完全一样（已显示、win32 1627、`status=3 flags=1`）。新诊断把它定位到 `FlushPaintedEdits` 的 **flush-no-present**：首帧被遮挡，1 秒内一帧都没呈现出来（frame=0，flags 含 4）。
- 所以 chrome 的问题**很可能**是同一类：首帧被 DWM 遮挡或呈现超时。但这只是推测，要等装了新版之后 chrome 再失败一次，看 `app-<pid>.jsonl` 里的 stage 才能确认。

**验证中遇到的环境限制。**
- 由计划任务启动的测试进程，写 `%LOCALAPPDATA%` 下的任何目录都会被拒（`ERROR_ACCESS_DENIED`；`rtprobe.exe` 实测：写 bench 目录正常，写 `%LOCALAPPDATA%\Pulse` 和 `%LOCALAPPDATA%\PulseProbeTmp` 都被拒）。疑似安全软件策略。
- 因此进程内测试看不到 `picker_ui-<pid>.jsonl`，改用 proof 的 stderr 输出来验证同一份记录。
- 正式版的远程宿主是 Pulse 主进程，它本来就在往这个目录写 `app-*.jsonl`，不受影响。
- 曾经在 v2 测试里加过一个读日志文件的断言，因为上面的限制会误报，已经撤回。测试文件已恢复成改前状态。

**没做到的。** 真实 chrome 端到端需要安装新版 DialogBridge，按规则不安装。

**下一步。** 用户选择处理首帧被遮挡导致的超时，见 9.4。

### 9.4 批次 D3（2026-11-21）：首帧被遮挡时不再误判失败（行为部分已撤回，见 9.4.2）

**问题。** 窗口显示后，`FlushPaintedEdits` 要求 1 秒内真正呈现出一帧，否则判为失败（flush-no-present，1627）。这 1 秒是为 DWM 显示动画准备的。但如果窗口本来就看不见，DXGI 会一直报告被遮挡，正常的对话框也会在 1 秒后被判失败，调用方随后收到"已显示后失败"。窗口看不见的情况包括：自身或所有者被最小化，或者在另一个虚拟桌面上被系统隐藏。

**改动（`src/ui/folder_picker_dialog.cpp`，只影响首帧等待循环）。**
- 新增 `HiddenFromUser()`，返回以下标志位：1 自身最小化、2 所有者最小化、4 被系统隐藏或随所有者隐藏（`DWM_CLOAKED_SHELL`/`INHERITED`）、8 不可见、16 被应用隐藏（`DWM_CLOAKED_APP`）。
- **只有用户能恢复的情况（1、2、4）**才暂停 1 秒预算：每次循环把截止时间往后推，轮询间隔改为 100 ms，并且不再触发重绘。窗口恢复后会重新开始计算 1 秒，并重新绘制。
- **8（被代码隐藏）和 16（被应用隐藏）仍然 1 秒失败。** 用户没办法把这种窗口找回来，而所有者窗口会一直处于禁用状态。快速失败能让调用方及时恢复。
- 取消、所有者退出、WM_CLOSE 仍然会立即结束等待，和原来一样。
- 提交时的 `FlushPaintedEdits`（previous≠0）不受影响，循环只针对第一帧。
- 诊断：`FilePickerFailure.hidden` 记录失败时和等待期间出现过的隐藏原因，写入事件 flags 的第 5–9 位。

**新测试用例（`picker_inprocess_v2_test`，在私有桌面上隔离运行）。** 在 picker 刚显示、首帧尚未呈现时，由驱动线程对真实窗口执行操作：
- `startup-hidden-wait`：最小化，2.5 秒后发送 WM_CLOSE。期望结果是取消，而不是失败，且耗时 ≥2 s。
- `startup-hidden-restore`：最小化，1.5 秒后恢复。期望两种结果之一：首帧呈现后被关闭（可合成的桌面），或者预算从恢复时重新计算、≥2.3 s 才失败（私有桌面永远被遮挡）。
- `startup-hidden-hide`：`SW_HIDE`。期望仍然快速失败（<2 s）。这条守住安全边界。

**新旧对比。** 同一个测试 exe，同一台机器，同一个 `tpick3.cmd`。旧版是 D2 构建的 DLL（`d2dll\`），新版是 D3。

| 用例 | D2（旧） | D3（新） |
|---|---|---|
| startup-hidden-wait | 1359 ms **失败** flush-no-present（flags=7）；用例不通过 | 2828 ms **取消**（800704C7）；用例通过 |
| startup-hidden-restore | 1344 ms 失败；用例不通过 | 2766 ms 失败，预算从恢复时重新计算（flags=39 = 7 + 32 最小化）；用例通过 |
| startup-hidden-hide | 1297 ms 失败；用例通过 | 1360 ms 失败（flags=263 = 7 + 256 不可见）；用例通过，行为不变 |
| ready-close | 3 项失败（环境原因，见 9.3） | 同样 3 项失败，行为不变 |
| startup-folder-error / close / cancel | 34/0、35/0、33/0 | 34/0、35/0、33/0 |

- 第一次 D3 运行（隐藏判定还没区分可恢复/不可恢复）也做过一次同样的对比，结论相同。
- 凡是 picker 窗口真正在私有桌面上显示过的用例，新旧两版都会多一项"close private desktop"失败。这是改前就存在的环境问题，与本改动无关。
- 构建：`pulse_picker_ui_proof`、`pulse_picker_inprocess_v2_test`、`pulse` 均为 `BUILD_EXIT=0`，只有 3 条已有警告。
- 等待时不重绘，每 100 ms 醒一次，CPU 开销可以忽略（没有单独测量）。

**未验证 / 限制。**
- 在**可合成的真实桌面**上，"最小化 → 恢复 → 首帧呈现 → 就绪"这条成功路径没有实测。私有桌面永远不会被合成；而在真实桌面跑 `--interactive` 会在用户桌面上弹出窗口并抢焦点。需要用户同意才能做。
- chrome 的失败是不是这一类，还要等装了新版、chrome 再出问题后，看 `app-<pid>.jsonl` 里的 `picker_show_failed`。如果 stage=flush-no-present 且 flags 不含 32/64/128，说明窗口可见但一直被遮挡，那是另一类问题，需要另外处理。
- 文件改动：`folder_picker_dialog.{h,cpp}`、`picker_ui_bootstrap_v2.cpp`（flags 掩码）、`picker_inprocess_v2_test.cpp`（3 个用例；这个文件原本就有其他 agent 未提交的改动，已保留）。改前副本在 `orig\`，D2 版本的副本在 `d2new\`。

#### 9.4.1 真实桌面交互验证（2026-11-21，用户同意）

用 `tint.cmd` 跑 `--interactive`，不切桌面，在用户的真实桌面上进行。新版是 D3 构建，旧版是 `d2dll\`，两边用同一个测试 exe。

| 用例 | 结果 |
|---|---|
| startup-hidden-restore（新） | 显示后才最小化，但首帧在最小化之前就已呈现；329 ms 就绪并被关闭。没有走到隐藏等待的分支。 |
| startup-hidden-cloak（新/旧，已删除） | 先隐藏所有者（`DWMWA_CLOAK`，结果 S_OK），但 picker **没有跟着被隐藏**（picker_cloaked=0）。原因：应用设置的隐藏不会传给它拥有的窗口，只有系统的虚拟桌面隐藏才会。两版都在约 0.3 s 就绪，这个模拟方法不成立，用例已删除。 |
| startup-hidden-minimize-sync（新增，第 4 个用例） | 用 WH_CALLWNDPROCRET 钩子，在 picker 自己的 ShowWindow 刚完成、首帧还没呈现时同步最小化（驱动线程确认 IsIconic=1）。**新旧两版都在约 0.3–0.4 s 正常呈现并就绪**（新 344/390 ms，旧 313/407 ms）。 |

**结论（推翻 D3 的部分前提）。**
- 在可合成的真实桌面上，**最小化的窗口照样能呈现**，不会返回 `DXGI_STATUS_OCCLUDED`。
- 私有桌面上的"被遮挡"来自桌面本身从不被合成，跟窗口是否最小化无关。
- 所以 D3 针对"最小化"的放宽，在真实桌面上不会被触发。它不会造成回归（两版的真实桌面结果一致），但也没有证明能修好任何真实失败。
- "在另一个虚拟桌面上被系统隐藏"这一类，无法用应用自己设置隐藏来模拟，仍然没有验证。
- 第 4 个用例的期望值已改为"最小化后仍然就绪并关闭"。改完后新旧两版都通过（30/0）。这个用例留作真实桌面的回归测试，只在 interactive 模式下有意义。
- 对 chrome 的推论：如果它的失败确实是 flush-no-present，原因更可能是窗口**可见但一直没被合成**，例如锁屏、安全桌面（UAC），或者 DWM/显卡异常，而不是最小化。D2 的诊断加上 D3 记录的隐藏标志位（flags 第 5–9 位），可以在下次失败时把这几种情况分开。

#### 9.4.2 撤回行为改动，只保留诊断（用户决定）

用户根据 9.4.1 的结论，选择撤掉行为改动、只保留诊断。

**现在的代码。**
- `FlushPaintedEdits` 的首帧等待循环**和 D2 逐字相同**：取出这个函数，与 `d2new\` 中的副本逐字比较，结果完全一致。首帧仍然是 1 秒内没呈现就失败。
- 保留了 `HiddenFromUser()`，但只用于诊断。失败时写入 `FilePickerFailure.hidden`（1 自身最小化、2 所有者最小化、4 被系统隐藏或随所有者隐藏、8 不可见、16 被应用隐藏），出现在 `picker_show_failed` 的 flags 第 5–9 位（32/64/128/256/512）。
- `kUserRecoverableHidden`、`first_present_hidden_` 以及循环里的推迟逻辑已删除。
- 测试保留 4 个用例，期望值改为当前策略（1 秒预算与是否最小化无关）：
  - wait 和 restore：可合成的桌面上，就绪后被取消或关闭；从不合成的桌面上，在 2 秒内失败。
  - hide：2 秒内失败。
  - minimize-sync：interactive 模式下就绪并关闭；非 interactive 模式下在 2 秒内失败。

**验证（同一个测试 exe，私有桌面，`tpick3.cmd` 共 8 个用例）。**

| 用例 | D2 DLL | 撤回后的 DLL |
|---|---|---|
| hidden-wait / restore / hide / minimize-sync | 1297 / 1312 / 1312 / 1359 ms 失败，flags=7 | 1406 / 1375 / 1328 / 1328 ms 失败，flags=39 / 39 / 263 / 39（多出隐藏标志位） |
| 以上 4 个用例的检查结果 | 每个 30 项 / 1 失败（只有 close private desktop） | 相同 |
| ready-close | 35 / 3（环境原因） | 35 / 3 |
| startup-folder-error / close / cancel | 34/0、35/0、33/0 | 相同 |

- 真实桌面（interactive）跑 minimize-sync：328 ms 就绪，30 项 / 0 失败。
- 构建：`pulse_picker_ui_proof`、`pulse_picker_inprocess_v2_test`、`pulse` 均为 `BUILD_EXIT=0`，只有 3 条已有警告。
- 结论：撤回后行为与 D2 相同，只多了隐藏原因的诊断位。
- 等 chrome 再失败时，用 flags 区分：
  - 含 32/64/128：窗口被最小化或被隐藏。
  - 只含 4（首帧被遮挡）：窗口可见，但一直没被合成，例如锁屏、安全桌面或 DWM 异常。


### 9.5 批次 (A)（2026-11-21）：“准备失败”不再显示为“未知”

**问题。** runtime 在 UI 出现之前准备 provider 失败时，写入的回退码是 `PROVIDER_PREPARE_FAILED_BEFORE_UI`。`Classify()` 只认识 `provider-prepare-failed`、`provider-failed-before-ui` 和 `PROVIDER_START_FAILED_BEFORE_UI`，所以这类记录落到“未知”。

**改动。** `src/common/dialog_takeover_history.cpp` 的 `Classify()` 只加了一个分支：`PROVIDER_PREPARE_FAILED_BEFORE_UI` 归入 `Reason::ProviderFailure`（10）。`Advice(ProviderFailure)` 走默认分支，建议动作是“导出诊断”。另外加了 2 个测试。

**验证（同一份测试源码，同一台机器）。**

| `pulse_dialog_takeover_history_test` | 结果 |
|---|---|
| 旧 `.cpp`（D1 备份） | 46 项 / 1 失败（正是新加的这条） |
| 新 `.cpp` | 46 项 / 0 失败 |

### 9.6 批次 (B)（2026-11-21）：管理员路径补上 GetProcAddress 拦截（R4 的最后一个缺口）

**做法。**
- 在导入表钩子表里加了第 17 项 `GetProcAddress`，掩码是 `kGetProcAddress = 0x40000`（`runtime_config.h`）。
- 包装函数 `HookGetProcAddress` 先调用真正的查找。为了让 kernel32 看到的调用方仍是应用模块，有 `GetProcAddressForCaller` 时就用它并传入 `_ReturnAddress()`。
- 只有当返回地址**正好等于**某个已挂钩函数的原地址时，才换成对应的替换入口；其他地址一律原样返回。
- 因为按地址比对，不解析名称，所以按序号查找和转发导出（比如 combase 转发）都覆盖到了。
- 钩子表在第一次安装时发布，之后不再变化（所属模块被固定在内存里），包装函数读它不加锁。

**验证（非提升，用 `runtask` 运行；x86 与 x64 同一份源码）。**

| | 旧 x64（`bold\`） | 新 x64 | 新 x86 |
|---|---|---|---|
| 检查结果 | 55 项 / 0 失败 | 60 项 / 0 失败 | 60 项 / 0 失败 |
| 安装 | `install_hr=0`，`mask=7`，`slots=3` | `install_hr=0`，`mask=262151`（7 \| 0x40000），`slots=4` | 同 x64 |
| GetProcAddress | 断言“仍是原生，属于已声明的覆盖缺口” | 新增检查全部通过 | 同 x64 |
| zero-slot 模式 | — | 3 项 / 0 失败 | 未跑 |

- 新增检查：
  - 挂钩的查找会转到 helper；
  - 转发导出（combase）转到同一个 helper；
  - 无关的查找保持原生；
  - 不存在的导出仍返回 null，错误码是 `ERROR_PROC_NOT_FOUND`；
  - 回滚后地址恢复原生；
  - 并发 COM 期间导入表回滚依然成立。
- 注意：新旧测试源码不同。缺口被补上，体现为旧断言换成了新断言，而不是同一条断言由失败变成通过。

**x86 测试的两处修正（只改测试）。**
1. 探针 DLL 的 stdcall 导出带修饰名（`_ProbeInstall@0`、`_ProbeRollback@0`、`_ProbeValue@4`），所以 `ProbeExport` 先查原名，查不到再查修饰名。
2. x86 编译器会把 `GetProcAddress` 的导入表指针缓存在寄存器里，跨过安装和回滚继续用，于是“路由”和“回滚”的检查结果时好时坏。现在测试里所有查找都改为经过 `__declspec(noinline) Lookup()`，`LookupNs` 也改成 noinline。

### 9.7 批次 (C)（2026-11-21）：熔断在“软件接管记录”里可见，可以一键重试（R6）

**用户看到什么。**
- 被熔断的软件单独成组，标题是“已暂停自动接管”，排在“需要重新打开”之后、“最近记录”之前。
- 每一行显示程序名和图标（同名软件在历史记录里有图标时复用），以及“已暂停”标签和暂停时间。
- 说明框里写着：“连续 N 次未能接管这个程序的对话框，已暂时改用系统对话框，D 天后自动再试。”以及“如果程序已更新或问题已解决，点右侧的重试按钮，下次启动它时就会重新接管。”
- 行右侧原来放删除按钮的位置，换成“刷新”图标的重试按钮，点一下就恢复。恢复后这一行消失，这本身就是反馈。
- “清空”按钮只清历史记录，不会顺带解除熔断，因为暂停行不算作可删除的记录。

**实现。**
- 存储（`dialog_adaptive_attach.{h,cpp}`）：
  - `PausedTargets(now)`：列出仍然有效的熔断，最新的在前，带自动重试时间；
  - `ResumeBasename(name)`：按可执行文件名解除这个程序**所有版本键**下的熔断。名字按 UTF-8 存储，比较时转成 Unicode，并忽略大小写；
  - `ReloadIfChanged(path)`：按写入时间加大小判断文件是否被别人改过，自己保存的不算。
- 常驻 helper（`user_auto_attach.cpp`）：每轮循环先调用 `ReloadIfChanged`。这样设置页的修改不会被 helper 内存里的旧副本覆盖（之前 helper 只在启动时读一次，之后会把文件整个写回）。
- 控制器：新增 `ResumePaused(basename)` 操作，在工作线程上完成。每次轮询历史时，同时从 `<数据目录>\dialog_adaptive_targets.json` 读出暂停列表放进结果，这个文件和 `DialogManagement` 目录相邻。
- 界面：`DialogTakeoverRow` 加了 `retry` 字段，分组编号 3 表示“已暂停”。行按钮沿用 `100+i` 的命中区域，`retry` 行转为恢复操作；删除操作的序号换算不变，因为暂停行不可删除，计在前面的不可删除行里。
- 字符串：只在 `catalog.tsv` 加了 4 条（7195–7198，简体、英文、繁体），然后运行 `gen_strings.py` 生成。

**验证。**

| 测试 | 结果 |
|---|---|
| `pulse_dialog_adaptive_attach_test` | 70 项 / 2 失败。新增 13 项全部通过；2 个失败是已有的 “user binding …” 两项，D1 时就是 57 项 / 2 失败 |
| `pulse_dialog_management_controller_test --resume-paused`（新增） | 11 项 / 0 失败：列出 3 条暂停记录，恢复 `pausededitor.exe` 后它的两个版本键都被清除、其他程序不受影响，结果已写入文件，下一次轮询保持不变 |
| 同一测试的已有模式，新旧源码对比 | `--history-poll`：新、旧都是 11 项 / 1 失败；默认模式：新、旧都是 43 项 / 3 失败，失败项完全相同，属于已有问题，不是本次引入 |
| `pulse_dialog_admin_auto_attach_test` | 13 项 / 0 失败 |
| 设置页 UI 自测（`build_st\pulse.exe --test-instance`，`PULSE_TEST_DIALOG_MANAGEMENT_UI`） | 0 失败。新增“暂停行有自己的分组标题，重试按钮在行内且命中 `100+i`，重启行没有按钮”，以及背景 VM 测试中的暂停行检查；3 种语言 × 3 种缩放 × 2 种宽度的布局检查全部通过 |

- 截图存放在 `bench_data\dialog-compat-20261121\ui_shots\startup-{light,dark}-{wide,narrow}-11.png`。截图里的说明文字显示 `{count}` 和 `{days}` 原样，是因为布局用例直接放了未替换的字符串；正式代码在 `FillHistoryVm` 里会替换，VM 测试也断言替换后不含 `{`。
- 第一次跑 UI 自测时有 1 项失败，原因是我新加的断言写错了：用例里的历史记录本来就不可删除，我却期望它有按钮。断言已经改正，并拆成单独一条检查。
- 为了能编译带自测的 `build_st`，顺手修了 `navigation_audit_probe.cpp:34`，只在测试中使用。原因是另一会话给 `ops::FinishedTask` 加了 `final_status` 成员，原来的花括号初始化 `{task_id, moved_sources}` 因此编译不过；现在改成按成员名赋值，行为不变。

### 9.8 批次 (D)（2026-11-21）：入口层，第一步——事件驱动发现（R3）

**做了什么。**
- 新文件 `src/dialog_launcher/window_show_watch.{h,cpp}`。
- 用 `SetWinEventHook(EVENT_OBJECT_SHOW, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)` 接收窗口显示事件。这是进程外回调，**不向任何进程加载 DLL**。
- 只接受顶层、非子窗口；同一进程在两次取用之间只记一次；待处理最多 64 个，超出的交给补扫。
- 用户自适应 helper 和管理员 helper 的主循环：等待从 `WaitForMultipleObjects` 换成 `MsgWaitForMultipleObjectsEx` 加有界消息泵，每轮最多处理 256 条消息。
  - 有新显示窗口的进程立即走原来的 `Begin(pid)`。
  - 用户路径上，如果显示的窗口本身就是原生文件对话框，同时调用 `ObserveNativeFileDialog`，即原来的学习逻辑。
- 原来的 `EnumWindows` 补扫（用户 500 ms、管理员 1000 ms）原样保留，不改变任何既有判断，只是多了一条更快的路径。钩子装不上时写 `window_watch_unavailable` 日志，退回纯轮询。

**验证（`pulse_dialog_window_watch_test`，新增，15 项 / 0 失败，共 3 次运行）。**
- 子进程在屏幕外显示 1×1、不抢焦点的工具窗口，桌面上看不到。
- 检查项：
  - 本进程窗口不上报；
  - 过滤器接受顶层弹窗、拒绝子控件；
  - 同一进程一次显示两个窗口只上报一次；
  - 分类器能标出对话框类窗口。
- 同一个子进程、同一组窗口下，从 `ShowWindow` 到被发现的时间（毫秒，每次 30 轮；旧循环按原逻辑模拟：每 500 ms 枚举一次、每次等待 100 ms、显示时刻相对扫描的相位随机）：

| 运行 | 事件驱动 中位数 / p95 / 最大 | 旧 500 ms 补扫 中位数 / p95 / 最大 |
|---|---|---|
| 第 1 次（与编译同时进行，有 CPU 争用） | 3.27 / 7.35 / 18.2 | 216.8 / 935.8 / 1155.8 |
| 第 2 次（空闲） | 2.55 / 3.04 / 3.08 | 219.5 / 436.8 / 436.9 |
| 第 3 次（空闲） | 2.59 / 3.45 / 3.55 | 218.5 / 436.6 / 437.4 |

- 管理员路径原来是 1000 ms 补扫，按同一模型，改动前的等待时间大约是上表的两倍。
- 这里测的只是“发现”这一段。从发现到接入完成，还要加上 `Begin` 本身的耗时（预检、绑定、注入），这部分没有变化。
- 构建：x64 的 `pulse_dialog_admin_auto`、`pulse_dialog_admin_auto_attach_test`、`pulse_dialog_adaptive_attach_test`、`pulse`，以及 x86 的 `pulse_dialog_admin_auto`，全部 `BUILD_EXIT=0`，没有新警告。
- 没有做端到端验证：在这台机器上再起一个 helper，会对用户真实运行的程序做接管，也会和已经在运行的 Pulse helper 冲突，违反“不影响桌面”的约定。

**没做的第二步：用 `WH_GETMESSAGE` 加载，取代远程线程（如实说明）。**
- 需要做的事：
  - 在随包发布的 runtime 里加一个导出的钩子入口；
  - 换掉现在“`WriteProcessMemory` 写参数 + `CreateRemoteThread`”的参数通道，改为按 PID 和 nonce 命名的共享内存；
  - 绑定校验要在新通道上重新做一遍；
  - 还要在火绒、360 上实测，确认误报确实下降。
- 这等于重写接入核心路径，影响 x64、x86、用户和管理员四条路径。本机只有火绒，没有 360 和干净的虚拟机，也就无法做“同条件新旧对比”。
- 所以这一步保持为待办，没有为了“做完”而硬上。

### 9.9 GetProcAddress 包装的开销（9.6 的补充，已更正）

- 之前记下的单次数字是 x64 72.0→215.4 ns、x86 113.8→184.4 ns，**这组数字作废**：“改动前”的采样在并发 COM 工作线程启动之前做，“改动后”的采样在启动之后做，两者条件不同。
- 现在两次采样都放在 4 个 COM 工作线程运行期间，同一进程，各跑 3 次：

| | 改动前 ns/次 | 改动后 ns/次 | 增加 |
|---|---|---|---|
| x64 | 74.9 / 75.0 / 74.7 | 90.7 / 75.8 / 86.1 | +15.8 / +0.8 / +11.4（中位数 +11） |
| x86 | 108.9 / 113.3 / 109.7 | 122.3 / 152.3 / 116.4 | +13.4 / +39.0 / +6.7（中位数 +13） |

- 包装函数只多了一次真实查找的转发，再加上最多 16 次地址比较，没有锁，也没有分配，所以没有再做优化。
- 更早的 72.7→75.4 也不可信：当时 x64 编译器同样缓存了导入表指针，测到的调用根本没有经过钩子。

### 9.10 本轮状态

**已完成：**
- (A) 准备失败归类：46 项 / 0 失败；
- (B) 管理员路径 GetProcAddress：x64、x86 都是 60 项 / 0 失败，zero-slot 3 项 / 0 失败，开销已更正；
- (C) 熔断可见并可一键重试：UI、控制器、存储、helper 重新加载，测试全部通过；
- (D) 第一步：事件驱动发现，发现延迟中位数从约 219 ms 降到约 2.6 ms；
- `tdlg.cmd` 补上探针绝对路径；文档 9.2 和 5.7 中不存在的 `dynamic_gpa_app` 已更正。

**待办：**
- (D) 第二步，`WH_GETMESSAGE` 加载，需要 360 和虚拟机做同条件对比；
- B1 回收站端到端测试（用户已决定暂缓）；
- 提升权限下的端到端运行，需要 UAC。

**受阻：**
- 用真实 chrome 复现确认，本机不允许安装软件。

**未提交：** 按用户要求，所有改动都留在工作区，没有提交。

### 9.11 评估：改用 `WH_GETMESSAGE` 加载是否值得重构（2026-11-21）

结论：**不重构**。按实机数据，消息钩子加载不会带来明显的覆盖提升，反而增加时序盲区和四套（x64/x86 × 普通/管理员）核心路径的改写风险。

实机证据（只读，`%LOCALAPPDATA%\Pulse\Diagnostics\Runtime\` 与在线进程探测）：

| 观察 | 数据 | 消息钩子能否改善 |
|---|---|---|
| 管理员助手远程线程注入 | 提交 24 次 → `ready` 23 次；`attach_thread_failed` / `remote_thread_*` 失败 **0** 次 | 无失败可修 |
| 跳过原因：权限不匹配 | `identity_rejected` err 740，180 次 | 否：UIPI 同样阻止低完整性进程向高完整性进程挂钩；该类目标本就由管理员助手负责 |
| 跳过原因：位数不匹配 | `architecture_rejected` err 193，26 次 | 否：钩子 DLL 也必须与目标位数一致，x86 由对应位数助手处理 |
| 当前桌面会话 17 个有窗口进程的注入权限探测（`OpenProcess` CREATE_THREAD/VM_*，非提权） | 非提权 15/15 可获得；仅 2 个提权进程（任务管理器、yjks）拒绝 err 5 | 否：这 2 个对消息钩子同样不可达 |
| 杀软拦截（火绒） | 本机未见拦截记录 | 无法在本机验证（无 360/虚拟机） |

消息钩子的额外代价：只在目标线程开始取消息后才加载，首个窗口就是对话框的程序会错过；需要常驻的钩子宿主进程（每种位数一个）；全局钩子会把 DLL 带进所有 GUI 进程，扩大影响面。唯一可能的收益是对“远程线程”敏感的安全软件，目前没有任何实机失败样本支撑。

保留触发条件：若后续用户诊断中出现 `remote_thread_timeout_retained` / `remote_thread_wait_failed` / `attach_thread_failed` 显著增多，或特定安全软件拦截远程线程，再以“可选回退路径”方式引入线程级（非全局）`WH_GETMESSAGE`，而非替换现有路径。

### 9.12 设置页文案通俗化（2026-11-21）

只改 `tools/l10n/catalog.tsv` 中 82 条文本（zh/en，以及原本手写的 zh-TW），ID 与占位符不变，测试无改动。原则：去掉“组件/登记/入口/原生/熔断/COM/WinUI/接口名”等术语；统一用“Pulse 对话框 / 系统对话框”；“诊断”改为“问题报告”；每条问题都给出“怎么办：…”。示例：

| 原文 | 新文 |
|---|---|
| 软件接管记录 / 检查最新记录 | 各软件使用情况 / 刷新 |
| 软件使用的接管组件与当前登记不一致。 | 这个软件还在用旧版本的 Pulse 对话框。 |
| 运行中 · 仍加载旧接管组件 | 正在运行 · 使用旧版本 |
| 兼容传统与 WinUI 应用对话框；……异常自动熔断保护 | 让更多软件也用上 Pulse 对话框；如果某个软件多次出问题，Pulse 会自动对它改回系统对话框 |
| 已暂停自动接管 | 暂时改用系统对话框 |
| 诊断报告 / 导出诊断 | 问题报告 / 导出报告 |
| 对话框跳转条 | Pulse 快捷栏 |
| 打开文件（IFileOpenDialog） | 打开文件 |
| 保留其他提供程序 / Preserves other providers | 不和其他软件抢 / Won't override other apps |

zh-TW：OpenCC 自动转换在“只/帐户/内建/系统匣/另存新档/回应/传回”上有误，相关 7 条改为手写。验证：`build_st` 两次构建 `BUILD_EXIT=0`，仅既有 4 条警告；管理界面自测（3 语言 × 3 缩放 × 2 宽度）`failures=0`，截图人工复核。

### 9.13 第 1 项调查：让软件退回系统对话框的原因（2026-11-21）

**目标软件现状。**

| 软件 | 结论 | 依据 |
|---|---|---|
| yjks.exe（`C:\YJKS\YJKS_8_1_0\`，提权，pid 27744） | **不是功能缺口。** 进程里已加载旧版 Pulse 运行时，并停在系统对话框模式。 | 管理员助手：`existing_runtime_native`（attach_stage 10），随后 `candidate_skipped` err 1247（已初始化）。这正是第 2 项（薄加载器：更新后不用重开软件）要解决的问题。 |
| acad.exe（AutoCAD 2024/2027，`E:\Program Files\Autodesk\`） | 原因未知 | 11-17/18 的回退日志已轮转。能解码的 native_reason 都是正常流程。需要重新复现：在 AutoCAD 里打开“打开 / 另存为”。 |
| AppGstr.exe | 原因未知，安装位置也未找到 | 11-20 的日志已轮转。（LzLiangJM 位于 `E:\LiZheng\TBS_7.0.4.0\`。） |

**诊断缺口。** wrapper 内部约 48 个 `MarkNativeOnly` 门，名字只以哈希形式写进 `native_reason`。`dialog_show_terminal` 的 `reason_name` 是合同原因，不包含这个门名。日志一旦轮转，就查不到具体是哪个门导致回退。建议把门名映射成固定枚举，写进 terminal 事件（待做）。

**“chrome ×37 次失败”的真相。**
- 这不是 Google Chrome，而是 LocalBridge 自带的 fingerprint-chromium（`Downloads\LocalBridge-Share\LocalBridge-FP\vendor\fingerprint-chromium\chrome.exe`）。每次另存为都在一个刚启动的 utility 进程里进行，所有者窗口在另一个进程（`samepid=0`）。
- 现存 5 次日志全部相同：保存、选项 `0x880A`、事件订阅 1 个。UI 显示后 1.25–1.44 s，`inprocess-api-show-return status=3 win32=1627`，而且在 READY（事件 1）之前就结束了。
- 同样配置的 Edge 在显示后约 110 ms 收到 READY，并正常完成（2.3 s、9.5 s 接受）。
- 这正是 9.3 / 9.4 中的 flush-no-present 签名：首帧 1 秒内没有被真正呈现。已安装版本（1.0.61）还没有 D2 的 `picker_show_failed`，所以拿不到隐藏标志位。

**试验后已撤回：另一种首帧处理方式。** 与 D3 延长等待预算不同，这次的做法是：首帧被遮挡时不判失败，窗口保持打开，READY 和期间排队的 OK 都等到第一帧真正呈现后再处理（每 200 ms 重试一次呈现）。
- 私有桌面复现，用新增用例 `--picker-dialog-test --occluded-first-present` 对比：

  | 版本 | 结果 |
  |---|---|
  | 改前 | 2359 ms 失败 1627，`stage=flush-no-present occluded=1 frame=0`，3 项全部失败 |
  | 改后 | 保持打开，OK 被推迟，关闭记为取消，3/3 通过 |

- 真实桌面：用 topmost 窗口完全盖住，DComp 交换链也不会报告被遮挡，picker 立即就绪。这与 9.4.1 的结论同类：“恢复可见 → 就绪”这条路径仍然无法在本机验证。
- **按 9.4.2 的用户决定（撤回首帧行为改动，只保留诊断），这次改动已完整撤回：**
  - `folder_picker_dialog.cpp` 和 `file_picker_dialog_test.cpp` 按 `orig\f_*` 恢复，与改前逐字节一致。
  - `build_st` 已用恢复后的源码重新构建。
  - 没有提交。
- 如果要启用，需要用户明确改变 9.4.2 的决定。代码思路记录在本节，可以随时重做。
