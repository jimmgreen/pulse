# 原生对话框接管：2026-11-10 本机增量验收

本批依据 `pulse-1.0.60-dialog-takeover-plan.md`，复用已有包装层与远程 UI；不是整份路线图已经完成。

## 已实现

- `IFileDialog2::SetCancelButtonLabel`：从 setter trace、能力协商到进程内/远程 UI 的完整传递。保留既有 V2 ABI 布局，以可选扩展结构和 feature bit 协商；显式空字符串、重复 Show、原生 HRESULT 和旧 provider 显示前回退均有回归。
- 传统 A/W API：映射 `OFN_NOTESTFILECREATE`；Explorer 模式下接纳原生忽略的 `OFN_LONGNAMES` / `OFN_NOLONGNAMES`，不再因没有 hook 的 `lCustData` 拒绝。hook/template 仍保留原生契约。
- 三种受管 COM 激活入口统一接纳 `INPROC_SERVER` 加 `DISABLE_AAA`、`NO_CODE_DOWNLOAD`、`NO_FAILURE_LOG` 的白名单组合。原始调用保留全部 context 位；其他位置/权限/接口仍不被推定支持。原生拒绝的 `NO_CUSTOM_MARSHAL` 保留为负用例。
- 双架构部署工具独立校验两个完整 bundle，提供逐架构状态、严格启用、按目标禁用、失败反向恢复。新 native JSON helper 与注册检查共用 mutation lock，不覆盖其他安装的恢复日志。
- 实机发现并修复传统多选的单文件返回缺少第二个 NUL：旧尾部字节可能被宿主当成额外文件。原生对照先确认双 NUL，新 A/W 哨兵缓冲区与容量用例先失败 4 项，修复后全部通过；没有删减失败用例。

## 本机真实结果

| 宿主/路线 | 实际检查 | 结果 |
| --- | --- | --- |
| Bandizip / 普通启动 | 打开窗口 | 仍为原生，不冒称全局 legacy 覆盖 |
| Bandizip / 显式受管启动 | 真实 `GetOpenFileNameW`，Pulse 选择 ZIP，宿主列出包内 `Pulse-takeover.txt`（81 bytes），再次打开并取消 | 通过；修复后全程最多 1 个进程，测试结束剩余 0 个 |
| Excel / 普通启动 | `Ctrl+F12` 打开 Pulse 远程 UI，取消返回 | 显示与取消通过；文件读取/保存未计为通过 |
| x64 独立真实 API 宿主 / launcher + Detours | 真实 COM 与传统 API 显示 Pulse 并取消 | 通过；不是直接调用 Hook 函数的证明 |
| x64 普通 COM 宿主 | Open/Save CLSID 激活，确认请求的 Auto64 实际载入 | 通过 |
| x86 / 普通启动 COM | 实际 32-bit 登记后 CoCreateInstance；Open/Save 接受和取消，真实同包远程 pulse.exe、LumaText；宿主读/写后重开 | 4 次通过，pointer_bytes=4；不是私有 hive 工厂测试 |

本机原自动注册指向已不存在的旧工作树 DLL；已先导出原登记，再按用户选择保留新的用户级 x64 bundle。失败切换实际执行过导回旧登记；成功切换后重新检查真实 COM 激活与启用状态。普通 COM 模式与按应用受管模式明确分开，Bandizip 提供单独的受管快捷方式。

## 定向验证

- x64/x86 activation / activation-ex / class-factory：分别为 115/0、85/0、56/0。
- 新 legacy flags：基线 8 项均未到 factory，修改后 8 项均进入 factory；真实 UI 的 A/W 打开/保存/取消 8 组通过，接受结果由宿主实际读写并重开。
- Cancel label：model 10/0，协议 3369/0，wrapper 49/0，真实 validator 61/0，真实 UI 20/0，真实 remote 16/0；remote 的取消验收不冒充远程截图验收。
- 打包既有 31 项、新双架构 17 项、定向事务回滚 3 项通过。x64 实包完整校验 36 个文件。
- 原始字节、SHA-256、同输入基线/修改/回滚命令、失败诊断及实际结果保存在 `bench_data/dialog-takeover-20261110/VERIFICATION.txt`；源码工作副本回滚不改真实登记。

## 保留的缺口

本次继续已补齐 x86 完整 LumaText/UI 闭包、两种真实登记视图、双架构只读状态页和独立 SHBrowse 家族。仍未完成：正式管理员路径双架构升级/卸载验收、普通启动 legacy 的全局入口、hook/template、NavigationRoot/非文件系统 Shell places、WinRT/App SDK picker 适配、管理员完整验收和 ARM64。具体状态以下表为准，不将直接工厂或普通 COM 成功外推到其他家族。

原生旗标语义参考：[Microsoft OPENFILENAME](https://learn.microsoft.com/en-us/windows/win32/api/commdlg/ns-commdlg-openfilenamew)；激活位参考：[Microsoft CLSCTX](https://learn.microsoft.com/en-us/windows/win32/api/wtypesbase/ne-wtypesbase-clsctx)。


## 本次继续：新增实现与真实验收

### 双架构交付和普通 COM

- 使用 SDK manifest 所固定的 LumaText 提交 `94e913c13b20b9f7f7dee01c9f0b10f831506379` 创建只读来源的工作副本，适配 Windows x86/x64 guard；未更改桌面原始 LumaText 仓库。真实 x86 PDFium 与闭包每一 PE 的位数均检查。
- 两个 schema-1 子 bundle 各 36 文件，外层 `multiarch.json` 绑定各 manifest SHA256。普通 COM x64/x86 分开核验，启用/禁用预检不覆盖其他安装，失败逐架构反向恢复。
- 最终同源 PMv2 宿主在两种真实 HKCU 视图各进行 4 次：Open/Save 取消均 `800704C7` 且无结果；Open 接受实际读取，Save 接受实际写入后重开；8 次均显示 Pulse、无 native UI、同 bundle 的 LumaText 已载入。完整物理截图已查看，取消/打开/保存按钮可见，无原先 DPI 虚拟化造成的测试截图截断。
- 受控诊断分母来自源码固定 4 个 scenario × 2 架构，不按已收到日志倒推。报告 `requests=8 expected=8 accepted=4 consumed=4`，取消 4。历史未关联事件保留为 unattributed，不伪造全局成功率。
- 隔离 hive 的 native helper 源码入口事务实际演练双启禁、x86 写后失败与逆序 `x86→x64` 回滚；这不等于真实正式安装器已验收。旧式 HKCU/HKCR override 激活实验未载入指定 shim，exit 1，保留为失败而非接管成功。

### SHBrowseForFolder 独立家族

- 支持真实 `SHBrowseForFolderW/A` 的 flags 1、65、81；root、callback、其他旗标、MTA、不合法 owner 继续走原生，且保留明确 reason。返回 owned PIDL、原生等价 display name/icon，宿主消费后 `CoTaskMemFree`。
- Native 对照 `20/0`；实际 Detours 契约 `31/0`；真实 UI `31/0`，共 12 Shows。
- 另用**不含 Detours/Hook 依赖**的真实宿主，通过生产 launcher→v2 profile→最终 runtime 启动：`planned=12 calls=12 failures=0`，6 次接受、6 次取消，6 个 PIDL 均实际读取 `probe.txt`；24 条 begin/terminal 配对。只有该 x64 managed 样本的 12/12 入站有明确分母，不宣称 ordinary SHBrowse 全局覆盖或 x86 SHBrowse 已测。

### 保存语义与真实软件

- 新 feature `SAVE_NO_OVERWRITE` 和显式 `SAVE_NO_OVERWRITE_PROMPT` 在保持 V2 布局的前提下协商；不再强加宿主未设置的 `FOS_OVERWRITEPROMPT`。旧 provider 显示前保留 native；矛盾/缺省 prompt 模式继续拒绝。访问、目标/父 identity、FileOk veto 和复验保留。
- 模型新定向 `--save-no-overwrite` 18/0；相关 no-test-create 15/0、native-save-defaults 14/0。
- 同一最终 UI probe：旧 DLL `Checks=40 failures=12`；新 DLL `Checks=42 failures=3`，exit 1。新 UI 的 5 个语义场景（提示/不提示、接受/取消、首次 FileOk veto、宿主写后重开）无失败；native 最终与回调候选的精确文件身份亦通过。**保留的 3 项失败全部为系统原生 `ClearClientData=80004005`**；未删除清理断言，亦未把整个测试写成全绿。该方法在未 Show 的全新 native 对象上也失败，未推断其 OS 根因。
- WordPad 本机 ordinary COM 已显示 Pulse Open/Save；Open 接受返回。另用生产受管 profile 验收强消费：从 Pulse 选择内容不同的第二份文本，正文实际变化；保存成新 leaf 文件后，WordPad 真正写出 65 bytes，并在新 WordPad 进程重新打开，正文及 SHA256 一致；原 fixture 哈希不变。普通路线的保存仅计显示，不与受管写后重开混记。
- 上述实测复现了普通 Save 文件名框输入完整新路径被拒绝、同目标改 leaf 则成功的缺口。已新增路径模型红绿回归：baseline `failures=24`，modified `failures=0`；一般 absolute/relative 路径使用显式 directory 解析，仍经过完整 metadata/access/overwrite/父 identity 复验，隐式 drive/root-relative、非法设备/名称等不被放宽。修复后的完整正常 x64 包经生产 profile 重新启动 WordPad，重试完整新路径保存成功，真实写出 65 bytes；新 WordPad 进程重新打开正文、SHA256 均一致，原件仍未变。

### 可观测性与支持页

- COM wrapper 与 managed runtime 为每个 Show 记录稳定 dialog/sequence、family/route、provider、明确 UI 是否已观测及终态；不把尝试 Show 当作 UI 已出现。
- Python 报告 27 tests、wrapper 新生命周期 25 checks 通过。历史日志没有独立分母时 coverage 为 null；broker QI 失败发生于 Show 前时不计作一次 Show。
- 设置页新增 x64/x86 两行：组件存在/缺失/未知与登记匹配/未登记/停用但残留/冲突/读取错误分开；登记匹配注明**未测激活**。这里只读视图，不运行 helper、不改登记，不把全局 COM 与 managed legacy 混写。
- 隔离 hive 63/0（activation_attempts=0）、控制器定向 34/0、catalog 生成检查通过；原 5134 项 id 保持。应用原有截图流程先启动异步 VM 再 settle，真实宽/窄、明亮/深色截图的双架构文字均已查看，无裁切、重叠；当前已选安装目录与启用登记不同的提示保留，不掩盖差异。

### WinRT / Windows App SDK 结论

独立报告见 `dialog-picker-family-feasibility-20261110.md`。本机有 App Runtime，但未配置 picker 开发 projection；StorageFile fixture 可读不等于 picker/broker 授权等价。真实 WinRT 取消请求返回 `80004002`，临时关闭当前 local helper 的对照仍观察到另一旧安装 revision47 被激活，因此尚未得到无接管 native 对照，未归因于当前新包。没有添加软件名特例或把未知 native QI 指针转交给错误的 controlling IUnknown。

## 路线图逐项状态

“部分”不代表整项完成；后续验收沿用同一证据目录。

| 任务 | 当前状态 | 实际边界 |
| --- | --- | --- |
| A1/A2 | 已实现并定向验收 | COM/managed Show 串联与分层原因；历史无分母保留 unknown |
| A3 | 已验收主要宿主 | x64/x86 ordinary COM 和传统 W/A；SHBrowse managed 此次仅 x64 |
| A4 | 部分 | 正式格式 payload 的 /PULSETEST 安装/升级/复装/卸载均 exit 0；管理员 Program Files 路径待测 |
| A5 | 已实现并定向验收 | 双视图状态与双架构启禁事务；真视图实测，不把登记存在等同激活 |
| B1 | 交付接入完成，管理员路径待测 | 双完整不可变闭包、两 pointer/两 previous、设置页启禁、安装迁移和卸载事务已接入并定向测试 |
| B2 | 已验收 | 两个真实视图各 4 次 COM；宿主读/写重开 |
| B3/B4 | 部分 | 生产 profiles、Bandizip 与 WordPad 实例；已有单实例进程不被宣称已注入 |
| B5 | 已验收受管矩阵 | 两架构各 56 次生产入口 UI，含动态/delay/plugin/子进程及 ready 后缓存工厂；不外推预先运行的宿主 |
| C1 | 已验收 | Cancel label 进程内与 remote，旧 provider 协商 |
| C2 | 已验收本次范围 | 新 flags、双 NUL 与 A/W buffer；hook/template 未扩展 |
| C3 | 已实现并定向验收 | 不强制覆盖提示与保存路径模型；原生 metadata 清理失败独立保留 |
| C4 | 已验收本次组合 | 正向 context 白名单及原生拒绝负例；未知 QI 不猜 ABI |
| C5 | 部分完成 | SetFilter 与普通文件系统、无显式 metadata 的 SaveAsItem 已完成双架构真实桌面消费验收；NavigationRoot/非文件系统 Shell places 保留原生 |
| C6 | 部分完成 | 未启用标志时的原生忽略字段已接纳；实际启用 hook/template 仍保留原生，替换试点未完成 |
| D1 | 已验收本次范围 | W/A 三种 flags、PIDL 生命周期、生产 managed 12 calls |
| D2 | 报告完成，适配未实现 | WinRT grant/WinAppSDK host/开发依赖明确分开 |
| D3/D4 | 未完成 | 管理员完整验收、ARM64/特殊宿主不列入发布承诺 |
| E1/E2 | 已验收本次范围 | 普通 COM 双架构、真实软件受管消费与取消无旧结果 |
| E3 | 部分 | 提示/veto 功能通过；native ClearClientData 3 项失败保留 |
| E4 | 部分 | 真实双视图启禁、失败恢复和隔离 setup 四阶段已测；管理员正式安装迁移/卸载仍待测 |
| E5/E6 | 未完成完整门槛 | 不由本次短测推断全部故障恢复、并发与长稳 |
| E7 | 已记录 | 仅列已测路线；全路线图未宣称完成 |

本轮源码、逐字输出、退出码、哈希与独立副本回滚见 `bench_data/dialog-next-20261110/VERIFICATION.txt`；上一轮证据保持原样。活动登记按用户选择保留通过实测的新包，源码回滚脚本不暗中改变登记。

### 端到端验证额外捕获的传输缺口

独立 ordinary COM→remote 宿主发现：provider-direct 新功能通过，不代表真实传输已通过。初始新包三种 no-prompt 场景均显示前回退 native，旧 prompt 对照正常；actual hello feature 已含 0x1000000，但 Config codec 仍以 0x3f 拒绝新增 0x40。已改为显式已知 Save flags mask，并在 client Validate/Show 前按 peer feature 协商；旧 peer、未知 0x80 和矛盾 prompt 保持负例。相同定向 codec 测试由 11 checks/5 failures 变为 11/0。最终包真正登记后，x64/x86 各 4 次 ordinary COM→remote 实测均 28 checks/0 failures：no-prompt 接受、取消、首次 veto 后接受与旧 prompt 对照；所有均同包远程 pulse.exe/LumaText、无 native 回退，覆盖回调与宿主写后重开。两轮严格串行并校验 UTC/tick 不重叠，登记前后不变。首次编排意外并发轮次明确标无效，未计为验收。


## 上轮留存状态（历史快照）

- 截至上一轮，用户选择保留通过实测的新接管；当时两视图均 enabled、无冲突，DLL 分别位于 `C:\Users\SS\AppData\Local\Pulse\DialogBridge\1.0.60-final-dual-e71667bc7c00\x64` / `x86`。
- 最终完整包再次进行普通 COM 两架构共 8 次真 UI 验收全部通过；最终 NoOverwrite remote 两架构另 8 次全部通过。既有 12 次 SHBrowse 生产受管样本来自前一相同 Browse runtime 实现；未伪称在每次重链后重跑全部历史测试。
- x86 头依赖前缀初始乱码导致旧 PCH 未失效，已按编译器真实 UTF-8 字节修复并重编受影响目标；Ninja deps 实际列出新 l10n 与 remote protocol 头，不以仅链接成功代替依赖验证。原未改源中的 C4244 警告仍保留于 `save-path-model/x86-warning-audit.json`，不宣称整包零 warning；本次模型、codec/provider 与 fixture 独立 /W4 构建无 warning。
- 最新源码包回滚只作用于明确指定的工作区副本；当前真实双架构登记按用户选择保持新版本。原登记导出、旧包与此前各批证据均保留。


## 本次继续：正式双架构交付接入

### 实现

- 正式 payload 保留原 `dialog-current.json`，可选 `--dialog-build-x86` 增加 `dialog-x86-current.json`。两个独立 `DialogBridge/<manifest SHA256>` 闭包各自验证机器、依赖、文件哈希和完整性，两 manifest 的版本必须一致；本次最终包共 93 文件。不依赖终端用户安装 Python。
- 原生注册 API 显式接受 X64/X86，DLL basename、PE32/PE32+ 和 WOW64 视图贯穿校验；旧同架构接口保留。设置页 worker 验证两个完整 bundle 后执行同一全局锁下的启禁事务，第二项失败时反序恢复，包含失败的那一项，并独立报告 rollback_error。显式单 bundle 操作仅作用于选中的架构。
- 安装器将两个 pointer 和两个 previous 标记纳入同一 root 文件事务，主 pointer 最后发布；清理保护两个 current 和两个 previous，保留忙、未知、损坏和 reparse 树。已安装 dual 的目录收到 single payload 时，在写入标记前拒绝，不静默遗失 x86 覆盖。
- 安装迁移 v4 journal 在写注册前持久化两架构来源/目标和各自阶段，失败恢复采用逆序并在 Enable 后核验状态；旧 v1/v2/v3 marker 兼容。并发外包登记或用户已禁用的完成项保持原样。正式卸载接线仅收集本安装树拥有的登记，并使用同一双架构禁用事务。

复用命令（分别准备完整 x64/x86 Release build）：

```powershell
python tools/pack_installer/pack_installer.py --root . --build build --dialog-build-x86 build-dialog-x86-next --stub build/pulse_setup.exe --version 1.0.60 --out dist/PulseSetup-1.0.60-dual.exe
# 或：scripts\pack_setup.bat build windows build-dialog-x86-next
```

### 实际验证

- 同一架构解析 probe：旧单 pointer 适配输出 `Bundle architecture checks=16 failures=2` / exit 1；新实现 `17 / 0` / exit 0。包含真正有效的不同版本 x86 闭包负例、架构错配、缺失/损坏/逃逸/重复键及失败清空旧 lease。
- 登记事务注入测试 `44 / 0`；迁移原套 `176 / 0`、双架构矩阵 `313 / 0`；显式架构 API 的同源 x64/x86 fake-Win32 fixture 各 `18 / 0`。这些不是实际激活或真实双视图运行的替代。原 no-op Enable probe 为 `2 checks / 2 failures`，新实现 `2 / 0`，独立副本回滚后恢复 `2 / 2`。
- Python installer 旧 3 项、新 9 项通过；native dual-version 定向 `failures=0`，覆盖两 previous、错配/篡改、reuse 前复验、锁定新伴包时 root rollback、四版本保护与未知/忙目录保留。旧定向兼容结果和保留失败见本批日志。
- **真实 /PULSETEST 安装器**：全新安装 A → 升级 B → 同 B 复装 → 卸载，四步均 exit 0。新装恰好 2 个闭包，升级/复装保留 4 个、两个 previous 都指向 A；卸载后 managed files=0、专用卸载登记不存在。实际全局接管登记前后相同，既有 Program Files Pulse 进程仍在。
- **真实用户登记**：跨包严格 Enable 返回 183，前后状态完全相同；经已授权的显式切换后，同一 x64 设置 controller worker 实际禁用两视图、再启用两视图，error/rollback_error 都为 0，无 path mismatch。
- **最终不可变包真 UI**：x64/x86 普通 COM 各 4 次 Open/Save 接受/取消，外加 noPrompt/取消/veto/prompt 各 4 次，共 16 次。同步 subprocess 四个退出码均 0，时间不重叠，宿主消费结果、保存后重开、同包远程 pulse.exe/LumaText、native=0 均通过，登记前后不变。x64 Open / x86 Save 的 1500×960 物理截图已查看，按钮完整可见。
- 最终 x64/x86 受影响目标构建通过，最终增量没有新 warning；首次重编中未改 x86 源的既有 C4244 仍保留，不宣称全树零 warning。

### 证据限定与当前状态

- 首轮打包期间 controller 的单架构边界仍需修正，冻结输入校验明确失败（exit 1）；该批包没有作为最终留存包。修正、重编后在 `candidate-final` 新目录重新构建，两包冻结输入 13/13 相同。
- workspace 目录普通递归复制曾报 Access denied，缺文件 fixture 的 controller 测试为 exit 1；保留诊断。随后用已校验 ZIP 普通提取到全新本机目录，完整 fixture 的 controller 启禁回归为 `18 / 0`，没有修改安全标签。
- 初次 PowerShell 重定向 UI 子进程的 ExitCode 为 null，未计作最终退出码证据；同步 subprocess 已完整重跑 16 次并逐一记录真实 0。未弱化断言或删除失败记录。
- **当前保留根目录**：`C:\Users\SS\AppData\Local\Pulse\DialogBridge\1.0.60-delivery-fbe4e80c5e6646e282b92b1303e67ac2`。x64 manifest 为 `1cf6021fb89bdc29c9230c4dd9ebcf1180deedab06a90bad5cbea4da433f5e83`，x86 为 `de27d3f7f8ceb9092a3a2a49b5be08c0d5cfab991619f99a50cb00496a977075`。两真实视图 enabled、无冲突，按用户选择保留。
- `/PULSETEST` 不等同于实际管理员 Program Files 升级和服务/卸载全路径验收；原正式安装未被升级。普通启动 legacy 全局入口、B5 完整矩阵、C5/C6、WinRT/App SDK、ARM64 和完整长稳门槛仍按表保留，未把本次交付接入外推为所有软件已覆盖。
- 本批四件源码与回滚证据：`C:\Users\SS\Desktop\pulse\bench_data\dialog-delivery-20261110\{MODIFIED_FILE.zip,DIFF_FILE.patch,VERIFICATION.txt,ROLLBACK.sh}`。独立副本回滚不改变真实登记；原始下载 MD 和此前证据保持原样。

## 继续实施：C5、入口矩阵与用户手动验收

### 登记范围更正

前述“当前保留根目录”“两真实视图”的结论只覆盖了当时**工具启动进程所读取的登记上下文**，不是 Explorer 桌面的全局完成证明。后续以同一只读原生 probe 分别直接启动和经项目已有 `LaunchFromDesktop` 启动，观察到同 SID、无 package、非 AppContainer 的进程读到了不同登记；当前证据没有确定这种差异的原因。

本轮再次只读复核，真实 Explorer 桌面仍指向 `C:\Program Files\Pulse\DialogBridge\e8465a107b894ad50b082afa255fc19573c89a1a2b5b223c2d09a42d2dd379e6\PulseDialogAuto64.dll`，x86 配置和 Open CLSID 均不存在。原生 probe 的证据为 `bench_data/dialog-manual-20261110/registry-context-desktop-before.json`。先前工具上下文的双架构功能结果保留，但不再称为真实桌面已切换。用户已同意备份后切换、失败恢复；**实际桌面写入和新功能真 UI 验收还没有执行**。

### 本次新增与实际验收层级

- B5：普通、动态导入、真正 delay-load、晚载入插件、CoCreateInstanceEx、缓存 class factory、新子进程的生产 managed 入口矩阵已经完成。x64/x86 各 56 次 UI，宿主实际读/写后重开，子进程与父进程退出均为 0；工厂在 managed ready 后取得，不外推到已经运行的单实例软件或注入前缓存。证据为 `bench_data/dialog-contracts-20261110/entry-matrix/VERIFICATION.txt`。
- C5 `SetFilter`：caller STA 保留 COM filter，现有 callback 协议传路径，只有原生实测的精确 S_FALSE 隐藏项目，HR2/FAILED 的真实用户返回仍包含。typed path 不再次过滤，GetEnumFlagsForItem 不被调用。契约、模型和受控 wrapper 已定向验证；真实 ordinary COM→remote 新包的验收仍待手动完成。
- C5 `SetSaveAsItem`：实现普通文件系统、无显式 metadata 的原生初始位置/文件名快照与原始项保留，不将 setter 简化为 SetFileName。folder、非文件系统及显式 metadata 混用保留原生。定向原生/模型 60 checks、contract 12、wrapper 30 均通过，show_calls=0 的测试不计作接管成功。独立原生 Show 对照中的 ClearClientData=80004005 保留，不宣称整套全绿。
- 新回归修复一：callback 上下文的 push、DPI、语言、恢复、pop 错误与真正用户 HRESULT 分开，内部失败终止 Show，不再悄悄显示未过滤列表。注入测试旧行为 30 checks/7 failures，修改后 30/0。
- 新回归修复二：IncludeItem 只处理 worker 枚举的当前列表候选，导航侧栏的失效收藏夹/离线快捷入口不在 caller STA 重绑或探测。模拟坏入口旧行为 8 checks/4 failures，修改后 8/0，binds=0、probe_io=0。没有通过更改用户收藏或映射盘制造测试。
- 桌面切换脚本发现旧 fake 与原生严格端点不一致：Disable 后旧 DllPath 会保留，原 `--enable-json NEW` 正确拒绝跨包。新增期望旧路径精确匹配、Enabled/Stage 类型和值均为 DWORD 0、两个 CLSID 根均 absent 的原子 rebind 入口；保留原严格 EnableIfUnclaimed 行为。新的定向原生及事务证据随本轮包记录，不复用原 24 项 green 作为该路径的证明。

### 不占用桌面的后续方式

用户本轮要求需要使用电脑，因此后续后台工作不发送鼠标/键盘事件，不自动启动选择器。新增独立手动 host：三项按钮各由用户决定是否点击，任一项结束后不会自动进入下一项；每次尝试保留独立 fixture 和 JSONL。

1. 过滤列表：用户观察 visible.txt 可见、blocked.txt 隐藏，再手输 blocked.txt 打开；host 验证精确身份和实际读取、caller STA、filter 生命周期。
2. SaveAsItem：先设置 folderB，再设置 sourceA/seed.txt；用户观察初始 sourceA/seed.txt，改为 saved-result.txt 保存；host 只在 owned 精确目标执行 CREATE_NEW、写后重开和源文件未变核验。
3. 1000 项过滤：用户自行取消；日志记录实际枚举数及耗时，枚举已完成就明确记作“枚举后取消”，不冒称枚举中取消。

host 在 Show 前验证 vtable 来自指定 Auto，同一次 Show 的远程 pulse.exe 和 lumatext.dll 必须属于该次激活的同一闭包。旧包、原生窗口、跨同伴包混用或结果不匹配均不计通过。操作和日志说明放在 `bench_data/dialog-manual-20261110`；构建成功和 `--help` 成功均不等于用户已经验收。

### 实际桌面第一轮结果与恢复

- 双架构受影响目标最终构建均 exit 0。本轮 10 个 CMake 产物无界面检查通过：两架构各 Invoke 30/0、roles 8/0、disabled rebind 67/0、filter model 17/0 及手动 host `--help`。初次 host 项目构建暴露 WIN32_LEAN_AND_MEAN 下缺 shellapi.h，已显式补 include 并重编通过；失败日志保留。
- 新生产布局包完整校验 93 文件、冻结 21 个输入后普通解压到全新本机目录。真实桌面 status 两个 helper 均 exit 0，完整原登记快照前后相同，expected_before_matched=true。
- 用户选择现在测试后，首次真正桌面事务完成 `disable-for-json OLD → enable-from-json OLD NEW`（x64）与 `enable-json NEW`（原缺失 x86），所有 helper 返回 0，结果 switched。原六项登记备份 SHA256 为 `17ef0f901f7ac74fad43292072dc67e67adc581455e3dc4e36cabc0353fb6004`；原件保留在本轮 session 的 evidence/desktop-backup.json，并已复制入证据目录。
- 用户首轮用例 1 的宿主读取/回调/生命周期通过，但没有独立列表视觉确认。用例 2 首次返回非预定 target，host 未写入任何错误目标，记录 not_passed_or_inconclusive；未推断这次差异的原因。用户随后将操作交回助手。
- 助手通过 Computer Use 操作独立测试窗重试用例 2，观察到初始 sourceA/seed.txt、输入精确 saved-result.txt 后，实际写入并重开、源文件身份/内容未变、原生空 ApplyProperties=S_OK；Show/result 均 S_OK，且 Auto/远程 Pulse/luma 均来自同一新闭包。
- 助手再做用例 1 的稳定视觉检查，发现 blocked.txt 仍与 visible.txt 一起显示。**虽然 IncludeItem 的 S_FALSE 与宿主读取断言通过，列表视觉失败，因此该包的 SetFilter 不算验收通过。** 已取消本次 Show，保留日志，不把手动取消改记为通过。
- 按失败恢复约定，随后经真实桌面 restore 事务反序恢复 x86 原三键缺失和 x64 原登记；结果 restored、rollback_error=null，恢复后的完整六项 raw typed 快照与初始备份逐项相同。新包暂不留作实际桌面活动登记；独立测试窗已关闭，用户现有工作窗口未被关闭或编辑。

该轮精确命令、逐字 helper 输出和快照见 `bench_data/dialog-manual-20261110/desktop-{status,switch,restore-filter-render}.json`。显示列表与过滤 snapshot 的一致性修复及最终复测结果在后续记录，不覆盖这一轮失败事实。


## 最终候选：过滤渲染修复与真实桌面复测

以下记录更新当前状态；前面各阶段的失败、恢复和范围声明保留为历史，不代表当前登记。

- 修复过滤只改变 interaction entries、却仍发布未过滤 snapshot 的不一致。新 helper 按同一批接受的当前行索引投影 entries 和不可变 snapshot，保留深拷贝元数据、排序对应及 stale 防护。真实 PaneViewModel/渲染数据回归基线 15 checks/8 failures，修改后 x64/x86 均 15/0。
- 新不可变双架构包经真实 Explorer 桌面事务切换成功，五个 helper 均 exit 0。当前活动根目录为 `C:\Users\SS\AppData\Local\Pulse\DialogBridge\1.0.60-c5-manual-3f266978c06848d8b479f3c08b98e1d2`。原登记备份 SHA256 `b9444500e0a5cd4f4993789400d0530b444e5942dce886b323043cecc498816d`；证据为 `bench_data/dialog-manual-20261110/desktop-switch-render.json`。
- 助手通过 Computer Use 亲自操作 x64 和 x86 普通 COM 宿主：两种列表均只显示 visible.txt、隐藏 blocked.txt；手输 blocked.txt 后宿主仍按原生语义读取成功。两架构均确认 caller STA、filter 生命周期、GetEnumFlagsForItem=0、指定 Auto 和同闭包远程 Pulse/Luma、native_seen=false。
- 两架构 SaveAsItem 均观察初始 sourceA/seed.txt，另存 saved-result.txt 后实际创建、写入、重开，源文件身份/内容不变；空 ApplyProperties=S_OK，无显式 metadata 的 GetProperties=E_UNEXPECTED。不是只验证 Show 返回。
- x64 初次 1000 项取消已经返回 800704C7、无结果、filter 引用释放，但旧测试器漏记 visible，因此该次整体仍标记 inconclusive。原因已定位为非 callback 阶段 GetWindow 被 provider 拒绝；不是取消返回失败，也没有把完整枚举后的取消冒称为枚举中取消。后续使用精确窗口身份只读观察器复测，另行记录结果。
- 新 x86 证据：`bench_data/dialog-final-20261110/x86-first-acceptance.json`；x64 证据：`bench_data/dialog-manual-20261110/quick-acceptance.json`。用户已将操作交回助手，本轮没有关闭或编辑用户工作窗口。


### NavigationRoot 的原生对照与交付边界

System32/comdlg32 的真正 Open/Save 对象无 Show 对照均通过：普通目录 root=S_OK，null/文件 root=E_INVALIDARG，可替换 root，虚拟 Computer root=S_OK。尤其 SetNavigationRoot 不会立刻覆盖既有外部 GetFolder，之后 SetFolder(sibling) 也返回 S_OK。该结果不能证明 Show 后允许越界，也不能据此擅自增加字符串前缀限制。当前产品仍保留原生路线；独立手动 Show oracle 已提供，未把无 Show 数据称为界面或最终选择语义的证明。证据见 `bench_data/dialog-final-20261110/navigation-root/VERIFICATION.txt`。

本批安装包可交付的是已经完成验证的双架构能力扩展，不等于路线图全部完成。明确保留：NavigationRoot/非文件系统 Shell places、hook/template 替换、WinRT/App SDK 实际适配、ARM64，以及正式管理员 Program Files 安装/服务/卸载全路径和完整长稳矩阵。对这些情况保持原生或既有受支持路线，不以软件名称特例强行放行。


### 无鼠标键盘的真实 API 取消复测

用户要求后台操作后，新增独立 `--api-close-test` 模式，不创建控制窗、不发送窗口消息或鼠标键盘输入。真正 ordinary COM 在真实 Explorer 桌面登记下激活；1000 项 fixture 的第 40 个 owned IncludeItem 回调内，由 caller STA 调用 IFileDialog::Close(ERROR_CANCELLED)。

x64/x86 各一次实际运行均 exit 0：Close=S_OK，Show=800704C7，GetResult=8000FFFF、result_present=false，include_calls=40/1000，GetEnumFlagsForItem=0，filter 引用释放，Auto/远程 Pulse/Luma 为同闭包，native_seen=false。终态为 `passed_api_close_contract`。该项证明的是枚举回调中的 API 取消；`visible_ui_pass=false`，不冒称按钮操作或窗口可见验收通过。精确命令和原始日志为 `bench_data/dialog-final-20261110/api-{x64,x86}-result.json`、`api-{x64,x86}-events.jsonl`，汇总为 `api-close-acceptance.json`。

NavigationRoot 现有保留原生的两项断言增设精确 `--navigation-root` selector，连同 factory/cleanup 共 4 checks/0 failures；没有实际 Show，不把计数 seam 当成真实原生 UI。


## 最终安装包交付

- 安装包：`C:\Users\SS\Desktop\pulse\dist\PulseSetup-1.0.60-dialog-tested-20261110.exe`，20,885,992 bytes；SHA256 `256815d77163371664f73a53255cec741e11e2ecbda0d571ff91b0eaff11bb6d`。
- 版本 1.0.60，正式安装器格式；x64/x86 各 43 个闭包文件，外层 93 文件，两个 pointer/manifest/PE 位数及每文件 SHA、压缩流 CRC、payload/trailer 均独立验证。
- 首个候选发现 x64 pulse.exe 被重新链接，与已实测版本哈希不同，因此未交付首个候选。22 个对话框相关冻结源仍全部一致；最终使用独立 staging 复制已实测主程序，其余构建产物和当前 build 原文件不改。最终两架构 36 个 EXE/DLL 与真实 UI/API 验收使用的文件逐字节相同；新 setup stub 另行执行以下四阶段。
- 实际 `/PULSETEST` 全新安装 A → 升级最终 B → 同 B 复装 → 卸载，四步均 exit 0。新装双闭包、升级/复装双 previous 保留、卸载受管文件和专用登记/快捷方式清理通过；既有用户 Pulse 保持运行。
- 每阶段当前进程上下文的完整 raw 六键登记未变；另外经真实 Explorer 桌面前后两次只读采样确认 raw 六键完全相同，活动新双架构包仍匹配。没有用工具进程视图替代真实桌面证明。
- `/PULSETEST` 不等于管理员 Program Files 安装、服务和卸载全路径验收。此安装包用于用户本地测试；尚未实现/验收的路线仍按前文清单保留，不把交付包等同于整份路线图完成。
- 证据：`bench_data/dialog-final-20261110/delivery.json`、`installer-final-vs-real-ui-binaries.json`、`setup-isolated-final/stages.txt`、`desktop-final-status-{before,after}-setup.json`。原下载计划和历史失败记录均保留。


## 后续兼容性增量（2026-11-11 UTC）

- 修复无效 NavigationRoot 探测导致误回退：原生 E_INVALIDARG 不再禁用接管；成功设置 root、未知失败及先前有效 root 仍保留原生。基线 26 checks/4 failures；增加 fixture 身份断言后，x64/x86 各 27/0。
- 未启用 hook/template 标志时接纳原生忽略的指针和 lCustData，不解引用、不调用，接受/取消不修改这些字段。实际启用标志（含空 hook 等错误组合）继续走原生；两架构 COM/W/A 定向分别 116/0、52/0、52/0。这不是完整 hook/template 支持。
- 新包真实 Explorer 桌面已备份切换。两架构实际普通 COM 均先返回 root 探测 E_INVALIDARG，然后进入同包 Pulse/Luma，而非原生；第40/1000项调用 Close 后取消、无结果、引用释放，退出0。不计窗口可见性验收。
- 新包隔离安装、升级、复装、卸载四阶段均退出0。仅四个 Auto/UiProofRuntime DLL更新；主程序/UI/Luma和setup沿用上一已验字节。正式管理员路径仍待测。
- x86 SHBrowse 定向 Detours 契约31/0；真实MTA原生调用两次超时，已清理自有进程，未作为生产接管成功。真实STA界面仍待测；没有新增shipping host。
- 新体验包：`C:\Users\SS\Desktop\pulse\dist\PulseSetup-1.0.60-dialog-root-c6-20261111.exe`；SHA256 `9e6c7a2d73e8772dfc3f274467ec53872c4fa2d2a8f6ff347c8f2bd35d9db15e`。上一体验包保留。
- 当前已实测桌面接管根：`C:\Users\SS\AppData\Local\Pulse\DialogBridge\1.0.60-compat-4782fbb4a3f549ddb9421e23fccad91c`。


## B3/B4 增量：已有目标进程诊断（2026-11-11）

- [x] 托管启动前扫描同会话、同完整路径的目标进程；同名不同路径不计入。记录已确认数量、查询失败、消失进程、取消和超时。扫描不完整时，零匹配不代表软件未运行。
- [x] `target_preflight` 同时输出到启动器和会话日志，并接入诊断摘录与报告；旧进程管理状态始终为 unknown，certified=0，不计入对话框 Show、成功率或覆盖率。不会关闭或注入已有进程，也不阻止合法多实例。
- [x] 生产启动器 x64/x86 各验证无旧进程、已有进程、单实例转交，共 6 场景通过。转交后旧进程仍未加载 runtime；启动器返回 3（未连接），没有误判成功。实际扫描存在 132 次查询失败，匹配数量仅为下限。
- [x] 两架构扫描器各 19 项通过；诊断摘录 31 项通过；Python 报告 48 项通过。6 份真实会话日志经实际 collector/report 后均只有 1 条启动预检、0 条对话框请求。
- 主程序已重建以包含新摘录器；本次实际导出验证使用独立 collector，未宣称点击主程序界面导出已经实测。真实启动器测试使用自有宿主，不代表所有第三方单实例实现都已覆盖。
- [ ] B3/B4 尚未整体完成：按应用选择、托管启动及已有进程提示的完整图形入口仍待补齐；本次新增的是启动器输出和诊断链，不是该入口的界面交付。
- 前项 YJK 的 DWG 筛选、0x42 参数兼容及关闭后焦点恢复已完成定向修复；用户已确认关闭后正常回到 YJK。管理员 YJK 仍使用明确的托管入口，不等同于普通启动自动接管。
- 本次保留此前 YJK 桥/UI 和 7-Zip 相关源码修改；新候选包另存，不覆盖前一已验包。证据目录：`bench_data/dialog-launch-preflight-20261111`。

- 候选安装包：`C:\Users\SS\Desktop\pulse\dist\PulseSetup-1.0.60-managed-preflight-20261111.exe`，12,870,408 字节（12.27 MiB），比上一包增加 41,112 字节；SHA256 `5324df871770c8eff63bdd242ce5bac9369179c3d7565edcaa46346e79af4954`。已解包核对 56 文件及双架构闭包，未把本次打包核验描述为管理员安装实测。


## B3/B4 增量：设置中的托管启动入口（2026-11-11）

- [x] 设置 → 启动与集成：选择 64 位应用 `.exe`，显示并保留所选路径，后台验证后启用“托管启动”。复用现有配置与托管 profile，不新增命令行手工步骤。
- [x] 点击启动先后台预检，再弹出默认取消的确认；显示已确认同路径进程数、扫描不完整提示，说明单实例可能转交旧进程。确认后复核 generation，取消或过期结果不启动；不退出用户应用。
- [x] UI 只报告启动请求和错误，不宣称已接管。异常/超时提示应用可能已启动；普通入口拒绝从高权限 Pulse 静默启动。进程扫描和校验留在后台。
- [x] 真实控制器→生产 launcher→自有应用启动链 21 项通过（普通本机进程上下文），包含已有进程、取消、真实握手观察及事件退出。受限执行上下文中的启动被现有 launcher 安全条件拒绝，失败证据保留；没有修改或放宽 launcher 条件。
- [x] 三语、620/1280 DIP、100/150/200% DPI、9 状态的布局/点击判定，40 张亮暗截图生成，共 89 项检查无失败；人工查看中文宽屏、英文暗色窄屏的新入口，未见裁切/重叠。此处验证渲染/命中与后台真实链，不冒称已经完整鼠标点击文件选择器和确认框的端到端验收。
- [x] l10n 生成校验通过；最终测试关闭专用 GUI probe 后另行构建用于候选安装包。
- [ ] 尚未完成：管理员应用的统一 GUI 路由、多个应用的配置管理、已有软件升级后重启清单（32 位路由见后续完成项）；B3/B4 仍非整体完成。旧 QQ 组件驻留问题不因新增按钮自动消除，仍需保存工作后完整退出再启动。
- 本轮没有修改真实接管登记，也没有关闭 QQ/YJK。证据：`bench_data/dialog-managed-ui-20261111` 和 `bench_data/managed-ui`。

- 本轮候选安装包：`C:\Users\SS\Desktop\pulse\dist\PulseSetup-1.0.60-managed-ui-20261111.exe`，12889736 字节；SHA256 `71efe0865deae9608aee7378d9a8fd09a5f5f0e60e5d8e1f74622be757eab8d9`。解包、56 文件与双架构闭包校验通过；没有将此描述成正式管理员安装流程验收。上一包保留。


## B3/B4 当前增量：自动双架构路由与可靠状态（2026-11-11）

- [x] 同一设置入口接受真实 x86/x64 PE，自动选择已校验的对应安装包；用户不用选择启动器位数。所选应用行显示架构；匹配组件缺失/坏指针/校验失败时禁用启动并显示原因，不回退到错误位数。
- [x] 配置捕获以实际已验证 launcher 的 PE 架构为准，目标/runtime/launcher 必须一致；解决 x64 管理器捕获 x86 profile 被错误拒绝的问题。显式实验包仍不猜测兄弟目录。
- [x] 跨架构会话只读观察验证 launcher 真实 PE、PID、创建时间、映像与令牌；旧默认读取/修改接口保持本机架构约束。跨架构 disable 尚未开放。
- [x] 真正默认数据目录模式现在保存/恢复所选应用；启动时忽略旧实验包绑定，重新解析当前安装指针，避免更新后锁死旧版本。实测使用 GetPulseDataDir 的隔离替身，不写真实用户配置。
- [x] 修复实测发现的长路径握手误报：session.bin 256 字符可读而 runtime-ready.bin 262 字符失败；Ready 现在从已存在目录的规范扩展路径读取，字段验证不变。原失败目录保留，未缩短夹具。夹具清理也使用扩展路径并保留目录身份/边界及非重解析验证。
- 验证：好包/缺 x86 指针/坏 x86 指针分别 31/27/27 项通过；25 项会话边界（含长路径及篡改拒绝）、10 项诊断、33 项 profile 模型、x64/x86 capture 各 4 项通过。真实两架构管理链各 21 项通过，包含已有进程、代际取消、有效握手和事件退出；界面 89 项通过（三语、DPI、窄屏、状态及截图）。
- 原 cross-capture 基线 4 项中 2 项失败，修复后 4/0；真实启动曾暴露 handshake=0，修复后两架构 alive=1/handshake=1/status_error=0。测试失败和修复记录均保留。
- 新包解出 56 文件并核对运行副本哈希后，又对包内 x64/x86 启动器各跑 21 项真实链，均通过。此处不等于所有第三方软件或原生文件选择器点击全过程验收，也不是管理员安装全流程验收。
- 本轮是“显式托管启动”的自动分流，不代表已运行的软件被接管，管理员 GUI、多应用列表、升级后重启清单仍待完成。没有关闭 QQ/YJK、修改真实接管登记或发布正式版本。

- 测试安装包：`C:\Users\SS\Desktop\pulse\dist\PulseSetup-1.0.60-managed-dual-ui-20261111.exe`，12920464 字节（约 12.32 MiB）；SHA256 `373e469bf2b14a53ad9e0f4b960c53d87676e3518b2fc319d035a0341272c21f`。证据目录：`bench_data/dialog-managed-arch-20261111`。


## 用户体验方向调整：按软件自动记录（2026-11-11）

用户明确否定“先判断权限、再选择托管按钮”的日常流程。本节替代此前“管理员 GUI 双启动入口”的交付方向；保留后台能力，但不把它暴露为日常必选操作。

- [x] 日常设置改为“软件接管记录”，移除选择 EXE、普通启动、管理员启动按钮；显示软件名、最近发生时间、打开/保存/未确认类型、原因及具体操作建议。技术位数登记细节留在诊断中。
- [x] Pulse 运行时后台每 15 秒采集，不要求先打开设置。磁盘读取、解析、存储均在后台 worker；读取受限/身份不足单独说明，不把空记录判作失败。最多显示最近 12 组，底层记录/文件/读取量均有上限。
- [x] 自动与托管日志加入一次缓存的软件 basename、进程创建时间和不含原路径的路径指纹。托管组件复用有界异步全局日志；原 session 日志保留。默认诊断导出不包含 basename、所选文件名或内容。
- [x] 只按生产者 session、创建时间和路径指纹关联，不用当前 PID 猜历史软件。旧日志缺身份时显示“待确认”，不伪造 QQ/YJK 的失败归属。
- [x] 明确区分配置异常、真实原生回退、处理失败、未确认与有证据的成功；权限 gate 本身不证明发生过对话框失败。取消 Pulse 不计失败；已经回退原生后取消仍记录回退，不计失败。
- [x] 按软件和原因合并重复事件，游标跨重启持久化，重复轮询不重复计数；后续成功保留原问题并注明只确认那次操作。固定 JSON 原子替换、备份校验恢复；无变化不重复写入。
- [x] 操作建议对应事实：组件不一致提示先保存、完整退出含托盘后重新打开；组件暂不可用提示重试；不支持请求保留原生并可导出诊断；不要求用户运行 CMD 或手动找日志。
- 验证：历史模型/存储 29 项、真实全局日志采集 3 项、身份 8 项、runtime contract 33 项、真实异步日志 41 项、controller 后台轮询 10 项、诊断导出 12 项、白名单采集 31 项通过。界面 97 项通过（三语/宽窄/DPI/状态/44 张截图），检查了英窄暗色及中文宽屏部分记录视图，无裁切重叠。
- 真实 global contract fixture 有 6 条 accepted 和 6 条 cancelled，但没有 UI 展示证据；因此归为 1 个软件、6 次“待确认”，不是“已接管成功”。取消不计问题、再次采集新增 0。最初错误 Success 预期的失败记录保留，未弱化产品判据。
- 安装包解出 56 文件；包内两架构更新组件逐个对照构建产物哈希，x64/x86 实际托管启动链各 21 项通过（自有进程、握手、事件退出）。此项不等同于所有第三方软件文件选择器覆盖率认证。
- 本轮没有关闭 QQ/YJK、修改实际接管登记或正式发布。已运行的软件仍需完全退出再打开才能加载更新组件；未进入可观察接管路径的自绘窗口仍不能仅凭日志判断，管理员普通启动路径的无感覆盖也未在本轮宣布完成。
- 测试安装包：`C:\Users\SS\Desktop\pulse\dist\PulseSetup-1.0.60-application-status-20261111.exe`；12976112 字节（12.37 MiB），较上一双架构包增加 55,648 字节；SHA256 `c535a3033c155285c8654546f723c7f27e5e6418f6273553f0019ca1f73e1d96`。证据位于 `bench_data/dialog-application-status-20261111`。


## 安装器增量修复：缺失 x86 版本目录（2026-11-11）

- 用户 06:56 安装日志在 Prepare 阶段失败 error2。只读核对：x64 f828 完整，x86 pointer 指向 e46780，但整个目录缺失；旧安装日志曾保留它，后续删除原因没有证据。
- [x] 仅当旧 x86 pointer 规范、祖先无重解析、版本目录本身确实不存在时，允许用已完整验证的新包修复。坏 hash、目录存在但缺文件、访问/共享错误及 primary 异常仍拒绝。不要求用户卸载或手动删目录。
- [x] 事务失败恢复原始双指针、pulse.exe、文件清单和双 previous marker；修复成功后两个架构和 thin host 均可验证，cleanup 保留当前版本及无主文件。
- 隔离复现原始 failures=2/exit1；修复后19项PASS、failures=0/exit0。包含缺manifest的error2、坏hash、恶意路径、锁定指针和坏新staging拒绝。仅构建安装器及定向事务测试，未重跑无关主程序/UI历史检查。
- 修正版：`C:\Users\SS\Desktop\pulse\dist\PulseSetup-1.0.60-application-status-setupfix-20261111.exe`，12980208字节；SHA256 `ccf74638fe2306be5af91c856ddc44d3306ae8735a4640d55c80d248259187fc`。56个应用负载文件与上一测试包逐字节一致，仅安装器stub更新。尚未宣称已在用户真实Program Files目录覆盖安装成功；真实安装和登记未改。


## B3/B4 增量：升级后运行软件重开清单（2026-11-11）

- [x] 补齐此前“升级后应用重开清单”：在软件接管记录中优先显示仍加载本安装目录旧版组件的软件，并给出“保存工作、完整退出含托盘、正常重开”的处理方法。不自动关闭软件，不要求用户识别管理员权限或 32/64 位，也不要求运行 CMD。
- [x] 清单依据同用户、同会话进程的实际模块路径；只接受已验证当前双架构 bundle 与安装根 `DialogBridge/<64hex>` 下已知组件的比较。读取前后复核 PID/创建时间；同名不同路径软件分开，同软件多进程合并。其他安装目录同名 DLL、访问失败、进程退出和不完整模块枚举不产生“需要重开”的断言。
- [x] 当前观察与历史事件分开；不把历史失败当作当前仍需重开，不持久化实时清单。后台约每 60 秒扫描，“检查最新记录”立即重查；重查替换上次结果，缺失或受限的观察显示检查不完整，不推断所有软件已经恢复。
- [x] 校验与枚举均在已有 worker 上；只读，不提权、不注入、不终止用户进程。每轮最多 512 次模块查询，查询间检查 1500ms 预算；轻量 PID 枚举有独立上限。截断后用游标轮转，避免固定从头扫描导致后方进程持续饥饿。默认导出仅含清单数量和检查状态，不增加应用名/原始路径。
- 验证：识别/调度/权限/隐私及实际自有进程 LoadLibrary 22 项通过；后台轮询 12 项、导出 12 项通过；复制真实已验包到临时目录后，持当前组件 lease 的完整 worker 链实测 5 项通过（实际加载旧组件被识别，卸载后手动刷新移除该观察）。x86 本项为模型验证，实际模块加载测试使用 x64 自有进程。
- 界面 105 项通过，覆盖三语、620/1280 DIP、100/150/200% 和 48 张截图；已检查新增清单的英文窄屏暗色、中文宽屏亮色，无文字裁切/重叠。生成文字一致性检查通过。
- 历史 B3/B4 中“选择程序及管理员按钮”属于已被后续产品方向替代的设计，不重新开放日常技术选择入口。管理员软件正常启动时的无感覆盖、未进入观察路径的非标准对话框仍待独立实现与实测；本清单不作为覆盖率证明。
- 本轮未修改真实登记、关闭 QQ/YJK 或覆盖 Program Files。发布构建完成（测试开关 OFF）；测试包：`C:\Users\SS\Desktop\pulse\dist\PulseSetup-1.0.60-restart-status-20261111.exe`，13010056 字节，SHA256 `f03b9a55ef9055a13d05665e8fcacca279ee12647c6a61b79710d0ca6571a674`。解包及双架构闭包验证通过，保留此前安装器缺失 x86 目录修复。证据：`bench_data/dialog-restart-status-20261111`。


## 管理员普通启动自动入口：第一阶段验证（2026-11-11）

- [x] 确认加载前缺口：`dialog_auto_registration.cpp::Open` 只写 HKCU，而提升进程不读取 per-user COM classes；`file_dialog_auto_server.cpp::TokenSafe` 已允许经过受保护目录校验的管理员组件，因此再次放宽该 gate 不解决加载入口。依据：[Microsoft COM elevation documentation](https://learn.microsoft.com/en-us/windows/win32/com/the-com-elevation-moniker)。
- [x] 新增 `src/bench/dialog_message_entry_probe.cpp` 自有进程验证原型，使用正常 CreateProcess（非 suspended/非 Detours）启动 child，仅对其已核验 PID、创建时间和主消息线程安装 WH_GETMESSAGE。128 位随机 nonce 隔离共享对象，拒绝已存在 mapping；不装全桌面 hook，不改系统 CLSID，不改用户接管登记。
- [x] x64 普通原生 token 实测：父/子 elevated=0、restricted=0、integrity=8192；安装前 callbacks=0，安装后的首条测试消息 callbacks=1 且 PID/TID 与 child 一致；Unhook 后新消息不再增加回调，child exit=0、mapping_remaining=0。/W4 /WX 编译及独立副本回滚重放通过。
- restricted sandbox 的同项测试出现“hook 安装成功但 callbacks=0”，原始失败日志保留，未将未证实原因写成结论。执行环境与原生用户 token 的结果分开记录。
- [x] 真实管理员 token/UAC 实测通过：父 PID 26028、子 PID 34224 均 elevated=1、restricted=0、integrity=12288；安装后的首条测试消息在子线程 25744 内回调。Unhook 后新消息不再增加回调；child exit=0、mapping_remaining=0、探针 exit=0。只证明自有管理员进程消息入口；尚未连接实际文件对话框替换，不代表真实软件覆盖完成。
- [x] 自有管理员进程的显式自动入口已连接经过认证的 runtime/provider；复制请求后在线程中核验受保护 session、PID/创建时间、映像和 bundle。新实验路径使用静态 IAT 原子替换，不暂停业务线程、不修改指令；既有托管路径不变。后台自动发现及会话发布尚未接入，不能据此宣布普通启动全部自动覆盖。
- [ ] 接入独立的启停、升级、卸载事务，再加入安装包；不将对话框后台能力塞入 PulseIndex，不通过覆盖系统对话框 CLSID 或修改 UAC 策略获得结果。
- [ ] 验证 x86、启动即弹框/窗口出现前/工作线程调用、管理员子进程及跨登录会话。原型的 early worker 已在安装前完成，消息线程命中不代表这些早期调用已覆盖。
- 第一阶段消息探针证据：`bench_data/dialog-admin-entry-20261111`。后续 runtime 实验如下；现有用户安装包和真实登记保持不变。


## 管理员自动入口：runtime 连接及并发复测（2026-11-11）

- [x] 最终实验 DLL SHA256：`340C432F32BDB6D27F63303E0D404F6E7D6AE1FFEAD494BA997F9BBF7DC85255`。真实 UAC 自有进程先显示原生，连接后 CoCreateInstance、CoCreateInstanceEx（对话框 options=0x42）、类工厂及保存入口均显示 Pulse。打开/保存均返回 S_OK 和预期夹具路径；保存对话框没有自行创建文件。
- [x] 关闭会话后下一次恢复原生；错误创建时间的会话拒绝初始化且保持原生。两个自有子进程及测试脚本均 exit=0；没有关闭真实软件或修改真实接管登记。
- [x] 四个并发堆分配线程在安装期间继续运行，心跳 172→268；覆盖 mask=7、slots=5、protection_restored=1。指针替换避免覆盖其他 hook；补充页权限被并发收回时的访问异常保护。该故障注入在定向测试通过，不声称实际软件发生过此竞态。
- 定向检查：IAT 36/0、零槽 3/0、自动初始化 13 PASS、既有 managed 17/0、policy 56/0。最终 DLL 已重新执行上述 UAC UI 链，并非沿用上一版结果。证据：`bench_data/dialog-admin-runtime-iat-20261111/cas-live-runlog`、`cas-live-negative-log`、`cas-live-result.json`。
- 尚未覆盖 GetProcAddress、延迟导入、后加载 DLL、既存 COM 对象和 x86 新入口；静态 IAT 覆盖不是所有软件覆盖率证明。
- [ ] 后台自动发现同用户同会话管理员应用，发布受保护身份绑定，自动触发入口；不要求用户日常选择管理员按钮。
- [ ] 主开关撤销、helper 退出/异常的失效策略、升级卸载事务及安装包集成。上述完成前不把当前实验 DLL 当作用户发行包。


## 自动入口增量：后台身份绑定与退出回退（2026-11-11）

- [x] 新增固定大小身份记录，包含目标 PID/创建时间、完整用户 SID、登录会话、后台 PID/创建时间、代次及 nonce。采集真实进程句柄和 token，拒绝跨用户/跨会话、受限/非提升 token、线程模拟及身份读取失败；定位 basename 只由数值身份生成，不接受路径。
- [x] 实验 runtime 在修改 IAT 前验证受保护的 `auto-binding.bin`；持有后台进程对象句柄，每次激活/Show 校验后台是否退出，避免 PID 重用恢复旧授权。记录错误拒绝初始化；后台退出后后续对话框保持原生，不强制关闭活动窗口。
- [x] 真实 UAC 对照已复现缺口：旧 340C DLL 在后台退出且没有 disable 标记后仍显示 Pulse，`native=1 pulse=7 result=1`。新版 `30DBFED95A7D2CF7551E4969DCC695C52C4D3C0472A8BCBDA37A2FDFC8F5A23E` 同一用例恢复原生，`native=2 pulse=6 result=0`。不是仅凭编译或模型判断修复。
- [x] 伪造后台创建时间：初始化 `8007000d`、ready=0、entries=0，后续原生仍可取消；无绑定的既有显式入口兼容链也通过。正常打开/保存返回路径、三种 COM 入口及取消均实测；自有子进程退出0。
- 身份模型31项通过，自动入口13项通过；仅构建受影响实验 runtime/test 及新增模型目标，/W4 编译无新增警告。证据：`bench_data/dialog-auto-lease-20261111`、`bench_data/dialog-auto-binding-20261111`。
- 边界：当前缺文件仅兼容旧显式入口，未来自动发布入口必须强制要求绑定；代次校验目前是模型，尚无后台实时代次撤销协议。主开关仍依赖现有单向 disable 标记，后台存活但挂起尚无心跳超时。自动发现/发布、开关 IPC、升级卸载及安装包集成仍未完成，不据本轮宣布无感全覆盖。真实安装和登记未修改，未关闭用户软件。
