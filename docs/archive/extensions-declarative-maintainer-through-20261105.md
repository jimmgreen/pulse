> 历史维护与验收记录，非当前原生 SDK 状态表。

# Pulse 增强包维护指南

本文面向宿主、SDK 和工具维护者。作者流程见[作者指南](extensions-sdk.md)，规划及未完成工作见[实施方案](Pulse-增强包开放能力与AI协同实施方案-2026-11-03.md)。历史证据在文末按原交付保存；它们不是当前所有目标已完成的声明。

## 当前维护基线

当前 catalog 为 20 模板：navigation、navigation-panel、selection-panel、view-panel、workspace-panel、dock-panel、grouped-panel、workspace-layout、transfer-panel、ui-tree、ui-catalog、settings-form、collection-browser、task-monitor、property-sheet、workflow-navigation、transfer-workflow、resource-client、selection-sort-panel、workspace-control-panel。以 examples/extensions/catalog.json 与实际宿主发现为准；后文九/十/十一模板等数字属于对应历史归档，不回写旧交付。

公开命令为 19 项，独立资源请求为 8 项。有界声明式工作流已走真实宿主命令链并支持本主体取消，文件步骤逐次原生确认；资源服务按宿主持有主体执行，提供当前目录元数据、所属任务查询/取消、自有设置与审计。resource-client 只声明资源权限，不是可执行脚本。可选固定发布者签名验证与本地信任策略已经实现；第三方脚本、通用 DAG、市场、自动更新与完整隔离平台仍未开放。逐项归属、版本和验收入口见[API 维护契约](extensions-api-maintenance.md)，不以此处支持说明代替本次实际运行报告。

安装前的 `--json verify ARCHIVE` 结果含 `verification.authorizationReview`：包名、命令能力、资源权限、workflowCount、grantsAuthority:false 和保守的 nativeFileConfirmationRequired。维护者须验证它与实际清单一致，并审阅独立 signature 的 signed/trusted 结果；这不是执行授权。新增资源权限与新增命令能力一样触发升级审阅。verify 的外部 policy 不自动写入 store；install/enable/rollback 受目标 store 的 signature-policy.json 约束。

## 新增选择与排序的验收状态

新增 `selection.selectAll`、`selection.invert`、`selection.clear` 均无参数；`view.setSort` 接受 column=name/modified/created/accessed/type/size/path 与 direction=ascending/descending。仅作用于聚焦窗格已加载的普通目录，加载/待处理、虚拟页、搜索结果或失效快照拒绝；全选/反选使用当前过滤后可见集合，清空只影响该窗格。排序保留宿主文件夹优先规则，并可能保存目录偏好，停用不回滚。

AI/MCP、助手和CLI共用发现契约，作者应检测 `selectionAndSorting`、`sortColumns`、`sortDirections`；发现、模板返回、test和preview本身不执行命令或授予权限。实际执行仍要求包声明与启用、有效caller/owner代际及宿主当前状态校验；新增能力升级仍需显式接受，内部action ID不是授权入口。

新增四项能力已完成冻结源码构建与真实宿主验收：`bench_data/selection-sort-host-20261104-02` 的62项检查、6张应用内截图通过并逐张查看；过滤主列表显示两项已选文本文件，排序工作流显示四项全选且另一窗格不变。`selection-sort-workflow-regression-20261104-02` 导航13项/3图、传输63项/15图通过。原生7个定向目标、CLI 66项、生成器23项及19模板/73类型负例通过。首次59项报告中的旧帧截图保留为失败证据，不作为最终视觉验收。SDK交付以对应归档旁的 `acceptance/report.json` 为准，其中记录独立解压验收、实际宿主摘要、类型检查及截图；历史归档计数仍只对应各自固定版本。

## 实现来源与同步责任

当前由发布 Schema 与结构化 API 元数据集中生成嵌入 Schema、完整发现描述和 TypeScript 类型，但解析器、运行时及真实行为仍需手工实现和验证。生成器保证声明对齐，不会自动实现权限、执行逻辑或副作用。

| 层 | 当前来源与职责 |
| --- | --- |
| 解析与限制 | `src/app/extensions/manifest.h/.cpp`，真实 `ParseManifest` 和加载限制 |
| 资源请求/结果 | `resource_contract.*`、`resource_service.*` 及 resource-request.schema.json / resource-response.schema.json；逐 op 结果类型、范围与权限保持实际接线一致 |
| 发布 Schema | `schemas/extensions/manifest.schema.json`，嵌入 Schema 和类型的生成输入 |
| 结构化元数据 | [api_metadata.json](../schemas/extensions/api_metadata.json)，能力/绑定摘要、作用目标、副作用与开发特性等声明；生成器要求与Schema标识一一对应 |
| 发现契约 | `src/app/extensions/developer_contract.cpp` 使用生成的 `api_description.generated.h`；参数Schema由清单Schema取得，完整描述供MCP、内置助手与CLI共用 |
| SDK 类型 | `sdk/typescript/index.d.ts` 重新导出 `manifest.generated.d.ts` 与 `api.generated.d.ts`，不另写同名发现接口；新增详情可选以兼容旧宿主 |
| 工具适配 | `developer_tools.*` 共用发现和内存校验；MCP 与内置助手保留已有身份、权限和参数检查 |
| 原生 CLI | `developer_cli.*`，复用公共契约和解析器；Python CLI 调用真实宿主，不复制一套校验器 |
| UI/执行 | `runtime.*`提供主体，`capability_service.*`集中十种宿主能力的执行入口；`palette.*`、`panel_bridge.*`只路由贡献，`context.*`提供只读快照，`src/ui/extension_panel.*` 与 `extension_tree.*` 负责传统面板和组件树呈现 |
| 静态预览 | `preview_model.*`、独立 `Pulse.ExtensionPreview.exe`，固定模拟值、禁用操作，报告明确 mockData |
| 开发重载 | `development_reload.*` 后台有界读取，`development_ui.*` 在窗口线程应用同代最新修订；无效输入保留旧包、关闭旧 UI 后换代、禁用状态保留 |
| 本地包管理 | `tools/extensions/package_store.py` 管理显式开发存储及版本；原生启动重新验证启用包，不把 Python 管理结果当成执行授权 |
| 文字 | `tools/l10n/catalog.tsv`，按项目生成流程更新生成资源，不能手改生成输出 |

## 增加或修改公开接口

1. 明确用户需求、公开标识、作用范围、参数、结果、副作用和不可用状态。内部动作 inventory 仅供盘点，不是授权 API。只有稳定可解释且可验证的部分进入白名单。
2. 修改解析器及边界，同步发布 Schema 和 `api_metadata.json`，运行 `python tools/extensions/generate_sdk.py` 生成嵌入 Schema、完整发现描述与TypeScript，再用 `python tools/extensions/generate_sdk.py --check` 检查一致性。元数据必须与Schema中的能力/绑定一一对齐；不能通过缺项或重复项生成“看似支持”的描述。手工同步运行逻辑、示例和作者文档，生成器不实现运行行为。字符串字节、重复 ID/绑定、引用与跨字段约束由宿主最终校验。
3. 复用普通宿主入口，不绕过会话停用、窗口归属和既有动作约束；禁止在 UI 线程新增阻塞文件、Shell 或网络工作。只读绑定不能顺便读取文件内容或外发。
4. MCP、内置助手与原生 CLI 必须复用同一 `DescribeExtensionApi` 和 `ParseManifest`。工具参数错误是 `invalid_args`，有效参数中的无效清单是普通 `valid:false`；发现不授予执行权限。
5. 增加与改动直接相关的成功、拒绝、边界及生命周期检查。同步契约一致性、真实宿主调用和相关 UI 检查，不以 mock 或编译成功代替功能证据，也不默认重跑所有历史套件。
6. 记录实际执行的目标、报告路径、可见 UI 检查及未覆盖项。验证数字必须来自本次运行，不累加历史次数制造覆盖率。

结构化详情需与真实副作用核对：无参能力应为 `argsRequired:false`、`argsSchema:null`，view.setMode与workspace.setLayout使用各自实际参数Schema，分别声明文件夹视图偏好和布局/会话保存副作用。`modifiesFiles:false` 只说明不修改用户文件内容；`persistsPreferences:false` 只说明不直接修改可配置偏好，导航仍可记录普通历史/最近位置，不能解释为宿主状态、缓存或磁盘零写入。`supportsCancellation:false` 与 `displayOnly:true` 不得被客户端解释成另有执行/取消权限。纯发现契约改动按生成、解析、适配、真实MCP/CLI范围验证；未修改UI时不为此重跑截图套件。

### UI 贡献点与预设的维护

`toolbar.commands` 是工具栏“更多”菜单的本包命令引用，不是直接固定按钮或侧栏 API。应保持引用顺序、包身份与代际路由，停用后不能通过旧菜单执行。`panel.layout` 的 auto/stack/grid 与 `panel.density` 的 comfortable/compact 是有限预设；省略选项保留旧包默认布局，grid 在窄窗口回退一列。

真实面板和静态预览必须共用布局逻辑，不能各自实现一份视觉近似。修改时同步发布 Schema、生成器输出、`DescribeExtensionApi` 的 panelLayouts/panelDensities、toolbarCommands 限制和特性标识，更新 workspace-panel 示例。验收分别覆盖菜单顺序/归属/执行门禁和布局、密度、窄窗、DPI、键盘与滚动；静态预览只证明模拟绘制，不代表“更多”菜单功能已通过。

分组与语义样式是固定组合子集：`groups`最多4组，精确分区原面板最多8个命令；按组视觉和Tab顺序排列，执行仍使用原panel.commands索引。每组独立应用布局预设，整个面板统一滚动。`primaryCommand`仅映射原生Primary视觉，不改变Enter当前焦点语义；`titleTone:accent`复用主题强调色，高对比或强调色对比度不足时回退主题文字色。浮窗、侧栏、静态预览共用同一Spec与绘制，不能通过预览近似实现。

维护者需定向验证缺组项、重复/面板外引用、非法样式、长标题说明、主按钮焦点/禁用、高对比、窄窗/侧栏与滚动。对反向分组的不同能力检查实际执行归属，不能只看按钮标题；不得把分组子集记成任意组件树或公共设计令牌全面开放。高对比若只有纯函数分支断言，应如实记录，不冒称实际系统高对比截图；不为检查改变用户全局主题。

### 侧栏放置的验收边界

`panel.placement: window | sidebar` 默认 `window`；`panelPlacements` 与 `sidebarPanels` 供作者探测。侧栏必须使用真实子窗口和合成绘制，并验证主工作区占位和恢复，不能以清单解析或静态 PNG 代替。

宿主在所属窗口右侧为侧栏保留 320 DIP 和 8 DIP 间距，并至少保留 360 DIP 文件区。它改变工作区布局而不是覆盖文件内容，不改写原有左侧导航或详情面板偏好；空间不足回退独立窗口。缩窄回退后不自动重新停靠。后续变更仍须检查关闭、会话停用、有效热重载、换包和 owner 退出后恢复占用区域，以及快捷键、焦点、滚动、DPI 和窄窗行为。多包仍只有一个打开的包面板。

固定右侧插槽与浮窗回退不等于通用停靠框架。自由拖放停靠、左右位置选择与任意工作区分区仍需独立设计与验收；已实现的有界组件树是面板内容契约，不等于通用停靠框架。维护者应同时记录正常侧栏与回退状态的真实界面，不能只截到看似停靠的覆盖层。

## 双栏文件计划与后续边界

新增两个无参数双栏复制／移动能力，完整行为约束见作者指南。下列内部模块与执行器的独立验证不能代替最终主窗口验收；实际结果按文末记录。该子集不等于通用文件计划、完整权限系统或操作系统沙箱。

### 内部主体模型与运行时接入边界

[authorization.h](../src/app/extensions/authorization.h) / [authorization.cpp](../src/app/extensions/authorization.cpp) 的会话级Registry已将长期UI绑定与短期请求租约接入runtime。初版独立34项是历史基线，最新模型与运行时结果另列；该接入仍不代表文件操作获得授权保护。

PackageIdentity含slot、包ID、revision和generation；OwnerHandle含宿主owner键和incarnation，注册/注销及包状态同步由宿主负责。调用者不能自行指定可信主体。接入区分两种内部对象：

- `PrincipalBinding` 用于长期UI，不设时间期限，但绑定包版本/代际、owner incarnation和grantEpoch；停用再启用、重载或owner退出都会使旧绑定失效。
- `CallerLease` 用于一次请求，5分钟到期；旧请求过期不能自动续期。新的明确用户点击只有在原Binding仍当前时，才可申请新的Lease；字段轮询只验证Binding，不能不断签发Lease，也不应让面板仅因打开5分钟就静默失效。

二者都是host-only内部主体对象，不是公开清单字段、文件操作许可或确认摘要，也不建立发布者信任。现已接入停用/重新启用、重载和owner incarnation撤销，旧Binding不能续签；后续变更仍需验证长时间面板与过期请求。文件计划仍需把独立确认、资源身份复核与执行授权接成闭环，没有新增MCP执行入口。

### 已验证的内部基础模块

- [transfer_plan.h](../src/app/extensions/transfer_plan.h) 的 `Store` 保存不可变 `FrozenPlan`，绑定宿主 `PrincipalBinding`；确认与一次消费产生的 `ExecutionPermit` 是内部对象，不接受包声明的 `userConfirmed`。到期、撤销、摘要一致性和一次消费由模型检查；它不自行读取文件或提交操作。
- [transfer_snapshot.h](../src/app/extensions/transfer_snapshot.h) 的 `CaptureSource/CaptureTarget` 与 `ReopenSource/ReopenTarget` 面向worker，持有只读句柄链并核对对象身份。只处理受限本地路径；拒绝UNC/网络、重解析点和ADS（备用数据流，路径分量含冒号），不把路径字符串当作访问许可。大小/修改时间只是版本线索，不能证明内容不可变；取消在打开/查询之间检查，不能中断文件系统驱动的同步调用。
- [transfer_confirmation.h](../src/app/extensions/transfer_confirmation.h) 的 `BuildTransferConfirmation` 仅构造 `ConfirmDialogSpec`。宿主canonical ASCII包ID完整独立显示，包名只作辅助；列出复制/移动、冻结源与目标、源数量/总字节，以及不覆盖现有文件的首阶段策略。空源、越限或总字节溢出拒绝生成；初始动作是取消。控制/双向字符使用可见转义，长行与超过可见行数的清单沿用原控件省略，Ctrl+C保留全部安全显示列表；复制内容不宣称是原始可执行路径。安装或启用包不是对该文件操作的确认。

展示helper不调用 `Store::Confirm/Consume` 或ops。[transfer_ui.cpp](../src/app/extensions/transfer_ui.cpp) 的controller冻结两个窗格的输入，经worker准备后显示真实原生确认；返回后重新核验主体并消费一次许可，再提交OpsManager。Windows 8.1尚无实机验证。

### 受控执行接线与扩展约束

- **主体与撤销**：controller、不可变计划、执行许可和受控任务都持有宿主PrincipalBinding；停用、重载、owner退出撤销原代际，重新启用不能复活原绑定。执行中在分块与提交／删除前检查撤销和取消；有效期只限制启动，不因大文件运行超过五分钟而中断。
- **不可变输入与规划**：[transfer_preparation.cpp](../src/app/extensions/transfer_preparation.cpp) 只接收UI冻结的值，不捕获AppState；后台核验源与目标对象后发布未确认计划。既有 `action_files.cpp` 的路径型FilePlan仍不是这条受控路径，不能直接拿来替代。
- **执行前复核**：[controlled_transfer.cpp](../src/ops/controlled_transfer.cpp) 使用经核验的源句柄和目标目录相对创建／提交，禁止覆盖；不进入普通MoveFileEx或CopyFile2分支。拒绝未支持属性及所有命名流。源与祖先限制共享，目标执行目录允许write sharing以支持相对rename，但禁止delete sharing；实际根junction改写测试确认相对创建拒绝，不将属性检查本身当作竞争安全保证。
- **确认独立于AI**：只在真实 `ShowConfirmDialogEx` 返回确认后消费许可，返回后再次检查当前主体；没有包提供的确认布尔、可序列化许可或新增MCP执行入口。原生清单使用的总量检查与后台操作数量限制都必须保留。
- **任务归属与结果**：`SubmitControlled/QueryControlled/CancelControlled` 复用OpsManager队列和进度，查询与取消同时核验原绑定及预期绑定。保留最近16条受控终态；淘汰、无权或未知均返回NotFound，不能推断成功。全局宿主取消及Stop兼容，包的取消只作用于所属任务。不进入普通恢复日志、自动重试或Undo路径。移动目标已提交而源保留时明确报告双份，取消不回滚已完成项目。

崩溃可能遗留随机暂存文件，或留下已提交目标与原文件；目前不扫描路径自动删除，也不承诺断电事务。通用ItemRef/ScopeRef、声明式DAG、文件夹递归、覆盖策略、可信分发、完整权限与统一AI执行仍未交付。

最小本地文件闭环也必须先实现这些边界，再决定普通文件/已有本地目标、目录递归、重解析点、网络路径等范围。当前UI贡献、分组、存储安装和结构化发现均不代替这项工作。

## 宿主分栏操作的边界

`workspace.setLayout` 调用宿主五种既有分栏，不是任意工作区模板。其目标为调用窗口，参数仅 `args.layout`；不能与文件视图的 `view.setMode` 或面板内部的 `panel.layout` 混用。复用已有窗格，保留隐藏窗格、标签页及目录，新建窗格从焦点目录初始化；设置页拒绝多栏。布局/会话可能保存，停用不还原。

维护时同步workspaceLayouts、完整参数Schema、能力副作用和workspace-layout模板。实际验收需验证旧窗格状态保留、焦点、新窗格来源、设置页门禁及会话保存边界。五种分栏并未开放任意比例、面板位置或文件操作计划；内部会话主体不等于稳定第三方权限系统。

## 版本与弃用

API v1 使用能力、绑定、模式与 Schema 做功能发现。不能仅根据 `apiVersion:1` 推断新增字段可用；新宿主接受旧包，旧宿主拒绝未知字段/能力是明确的兼容边界。

当前没有已交付的通用 major/minor 握手、自动迁移或弃用调度器。拟弃用接口时，先提出替代接口与迁移说明、兼容窗口及失败诊断，更新发现和文档并保留旧包回归；破坏语义或移除前需显式版本决策，不能悄悄更改已有能力默认行为。未确定的未来版本不得出现在已发布支持表中。

## 本地包存储与多包维护边界

本地存储允许开发用未签名包，也支持可选分离签名与固定发布者信任策略；是否要求签名由 store/signature-policy.json 决定，不能把未签名或仅摘要匹配解释为可信。内容哈希用于标识和检测版本完整性，不能作为信任证明；升级新增能力的确认是工具交互检查，不是操作系统权限沙箱，也不授予任意代码、网络或文件操作权限。

注册表维护契约见 [store.schema.json](../schemas/extensions/store.schema.json)：文件最多 64 KiB、包最多 32 个、版本为 64 位小写十六进制 SHA-256；包 ID 唯一性等额外约束仍由宿主检查。它用于维护与一致性核对，不是让作者或 AI 手写注册表绕过 CLI 的入口。包的公开清单 Schema 与存储注册表 Schema 不能混用。

维护安装与加载时，分别核对清单语义、归档/版本完整性、注册表一致性、包 ID 归属及执行时启用状态。命令归属必须包含包身份，不能因为两个包有相同命令 ID 或标题就执行另一个包。持久启用状态与应用内会话开关必须保持区分，升级、回退和卸载也不得悄悄改变已打开窗口的事实。

扩展这一机制时，定向覆盖首次安装、原版本重复安装、升级新增能力拒绝/显式接受、回退、拒绝覆盖、坏版本诊断与恢复、包间同名命令、会话停用和包数上限。可选签名验证和命令/资源权限升级差异审阅已有实现；多包加载或回退通过仍不代表 P5 自动升级、市场与完整可信分发已完成。

## 证据使用方法

优先看与当前 diff 对应的定向报告，再按需要读取下面历史记录。报告中的静态 PNG、模拟测试、真实命令执行分别证明不同事情。未检查的主题、缩放、窗口生命周期或外部编译器均不得记为通过。原有基准产物可能是本地文件，发布文档时保留路径作为追踪线索，不保证它们存在于所有检出副本。

## 历史验收与可复现检查

以下按原交付时间记录；其中“当前”“本轮”、能力/模板数量及未完成项均指该条记录时点，不替代文首当前维护基线。原归档、摘要、失败与通过数量保持原样。

### 双栏受控文件传输（2026-11-03）

随后将八种能力从面板实现集中到host-only `CapabilityService::Invoke`。输入必须是有效CallerLease、实际owner和已加载包的命令索引，不接受任意Command或调用者自报路径。结果区分Rejected、Dispatched、PendingConfirmation；Dispatched不声称状态一定变化，PendingConfirmation不等于文件任务成功。它仍是有限宿主适配器，不是可直接给脚本或MCP调用的通用执行协议。

这次路由重构定向复验：文件真实窗口60项、工作区46项、分组视图25项通过，进程均退出0。新增断言覆盖无身份、有效但不同owner、越界命令及待确认/拒绝语义。证据为 `bench_data/extension-service-transfer-20261103-01/capture/report.txt`、`bench_data/extension-service-routing-20261103-01/results.json` 和 `build-extensions/capability-service-*.log`。首次编译遗漏面板仍使用的ClampScroll声明，恢复所属头文件后构建成功；未改普通导航和文件传输实现。

本轮增加两个无参数文件能力与 `transfer-panel` 模板。[威胁边界](extensions-threat-model.md)记录当前声明式子集能防止什么、不能保证什么；并非脚本隔离或完整第三方授权的验收。

| 检查 | 实际结果 | 证据 |
| --- | --- | --- |
| 冻结计划与后台准备 | 一次许可模型39项；真实文件准备18项通过 | `build-extensions/transfer-plan-check/`、`transfer-preparation-final-test.log` |
| 同句柄执行器 | 23项通过；源/目标替换、命名流拒绝、取消清理、已完成前项、只读移动双份、异常回调与零字节 | `build-extensions/controlled-transfer-final-test.log`；实际目录根junction攻击另见 `controlled-transfer-root-junction-test.log` |
| 共享任务队列 | 21项通过；按归属查询/取消、Stop、终态淘汰、恢复/重试/撤销隔离、溢出拒绝及部分结果文字 | `build-extensions/controlled-ops-verified-test.log` |
| 原生契约与MCP | manifest159、contract119、runtime51、MCP14项通过，MCP仍为发现/校验而非新增文件执行工具 | `build-extensions/transfer-*-test.log` |
| 主程序真实交互 | 57项通过；真实按键确认/取消、复制/移动字节校验、同名不覆盖、选择/目标变化后冻结输入、停用撤销、60源长清单、一栏拒绝 | `bench_data/extension-transfer-live-20261103-03/capture/report.txt` |
| 作者与独立结果复核 | 12项通过，真实发现/生成/test/拒绝伪确认参数，独立检查文件结果，进程退出0 | 同目录 `checks.json`、`process.json`；`build-extensions/transfer-live-ui-03.log` |
| 应用内截图 | 已查看亮色复制、暗色移动、60项清单；默认取消清晰、无重叠，长路径/列表明确省略 | 同目录 `capture/confirm-copy.txt-light.png`、`confirm-move.txt-dark.png`、`confirm-selection-60.png` |

第一轮实际流程已通过，但主窗口截图读到未绘制的交换链帧，确认截图也缺少系统底色。第二轮修复主窗口截帧；第三轮通过实际绘制、在Present前读取并使用不透明宿主主题底色修复确认截图。01/02目录保留，但不能作为最终外观合格证据；03截图不代表DWM材质或桌面合成的完整捕获。功能没有通过截图逻辑绕过确认。

执行器首轮遇到相对提交sharing violation，目标执行目录改用允许write sharing但禁止delete sharing，并增加实际根junction攻击检查。队列部分结果文字测试曾失败，因为测试程序没有初始化/链接本地化资源；补齐真实资源后21项通过，未弱化断言。初次集成configure遭Ninja重整日志Permission denied，后续配置和目标构建成功。新增失败均按实际证据记录，不推断无关根因。

本轮没有执行完整历史回归，也没有实机Windows 8.1或系统高对比验收。一般文件操作、AI旧工具、脚本、递归、覆盖和断电恢复不因这些结果被宣称已验收。

以下按改动范围记录证据，不累加此前工具链或历史阶段数字。组件绘制与实际菜单执行分别验收。

文件计划基础模块按以下独立范围记录，不能将这些数字记为真实复制/移动成功：

| 范围 | 实际结果 | 证据与限制 |
| --- | --- | --- |
| 不可变计划与执行许可模型 | 39项通过，0失败、0编译警告 | `build-extensions/transfer-plan-check/result.json`、`test.log`；没有文件操作 |
| 句柄快照与身份复核 | 31项通过 | `build-extensions/transfer-snapshot-test.log`、`transfer-snapshot-build.log`；包括ADS/重解析点拒绝、长本地路径及取消；不是执行器验证 |
| 确认展示builder | 22项通过 | `build-extensions/transfer-confirmation-test.log`；Unicode/超长文本、canonical ID、计数与总size溢出、复制/移动、默认取消 |
| 同生产确认绘制与完整列表 | 12张图生成成功；完整clipboard逐项断言通过 | `bench_data/transfer-confirmation-final-20261103-170158/gallery.txt`；复制/移动×明暗/模拟高对比×100/150%，已查看明暗代表图，未更改用户主题/偏好；模拟HC不算系统HC实机验收 |
| 真实模态默认Enter | 复制、移动均返回cancel，进程退出0 | 同目录 `live-copy.txt`、`live-move.txt`；不是截图或操作成功回执，未执行复制移动 |

最终gallery构建日志为 `build-extensions/transfer-confirmation-gallery-final-build.log`；首次builder/gallery构建为 `transfer-confirmation-build.log`。这些定向目标成功不替代完整宿主执行闭环或Windows 8.1实机验收。


workspace.setLayout按以下本轮证据记录，既有grouped或sidebar数字不替代该能力的实际工作区验收。

| 工作区分栏检查 | 结果与限制 | 位置 |
| --- | --- | --- |
| 作者真实CLI | 24项通过，覆盖五种layout、错配args拒绝、升级新增能力未接受时注册表字节不变、接受升级后保持enabled | `bench_data/extension-workspace-authoring-20261103/checks.json`、`build-extensions/workspace-live-cli.log` |
| 六能力结构化契约 | 真实CLI23项通过；完整发现输出单独保存 | `bench_data/extension-workspace-contract-20261103/checks.json`、`api.json`，日志 `build-extensions/workspace-live-contract.log` |
| 真实MCP | 14项通过，断言更新为六能力/五layout并保留与共享适配完整JSON等值检查 | 本轮MCP定向输出 |
| 实际工作区 | 最初43/43后又修复窄窗首帧透字问题；最终46项通过，包含新首帧和caller断言。五种布局、实际选择/背景标签、设置页拒多栏、停用门禁及640 DIP回浮窗保留四栏均覆盖 | 最终 `bench_data/extension-workspace-20261103-164143/capture`；原43项记录保留在 `extension-workspace-20261103-163258/capture` |
| 原生直接依赖 | manifest149、contract110、runtime42、model26、MCP14全部通过；MCP14与上行同范围不重复累加。最终构建通过 | `build-extensions/workspace-*-test.log`、`build-extensions/workspace-selection-build.log` |

首轮失败来自选择表示的断言：宿主使用all_selected优化，内部集合可能转换。已改为生产IsSelected/EntryAt查询实际选中文件集合并验证，最终通过；不声称内部集合原样不变。最终工作区与侧栏截图已查看，host帧不含child，dock另帧捕获，不将它们描述为一张合成整窗截图。

随后发现旧窄窗截图首帧有收藏提示透到窗格标题；已实修BuildVm先稳定标签换行高度再FillPaneSlots，使绘制与命中使用一致布局。最新 `extension-workspace-20261103-164143/capture/narrow-four-host.png` 已查看无该透字问题，补充首帧断言后46项通过。旧43项通过不掩盖随后发现的可见缺陷。

| 内部主体接入检查 | 结果与边界 | 证据 |
| --- | --- | --- |
| 模型与运行时 | authorization模型42、runtime51、dev25、store40通过；长期UI绑定与短调用租约分离，停用/重新启用、重载、owner代际撤销 | 本轮原生定向报告 |
| 实际调用 | 最新workspace46含caller检查；无TTL Binding不会使面板仅因打开5分钟失效，旧Binding不能续签 | `bench_data/extension-workspace-20261103-164143/capture` |

该表只证明会话主体与调用门禁，不证明文件操作计划、持久权限或发布者身份已实现。

分组面板与宿主视觉语义按本轮证据分别记录；以下结构化契约、侧栏及此前布局数字不自动证明新增分组行为。完整UI框架与权限/文件计划仍未交付。

| 分组与语义样式检查 | 结果与限制 | 位置 |
| --- | --- | --- |
| 作者真实CLI | 22项通过、退出0；已查看亮色100%分组与主按钮静态预览正常，静态按钮不执行 | `bench_data/extension-grouped-authoring-20261103/checks.json`、`preview/`，日志 `build-extensions/grouped-live-cli.log` |
| 原生定向检查 | manifest134、contract100、runtime35、preview_model26、MCP14、panel111全部通过，分别记录不累计成旧回归覆盖 | `build-extensions/grouped-*.log` |
| 真实窗口与侧栏 | 鼠标/键盘各25项通过，重排组顺序从相反起始模式执行，停用撤销回调；最终亮色100%浮窗、暗色150%侧栏图已检查正常 | `bench_data/extension-grouped-window-20261103-161958/capture`、`bench_data/extension-grouped-sidebar-20261103-162000/capture` 的报告与截图 |
| 长内容与颜色回退 | 四组八个长按钮、滚动、白强调色配浅背景/黑强调色配深背景的可读性回退截图已检查；不据此声称实际系统高对比截图通过 | `bench_data/extension-grouped-components-20261103` |

高对比仅通过生产使用的 `ExtensionPanelTitleColor` 纯函数分支断言，确认返回宿主前景文字色；未改变用户系统高对比偏好，也未拍实机高对比截图。白/黑极端强调色则有真实组件渲染截图，二者证明范围不同。

结构化能力详情与集中生成是本轮契约变更；下面既有UI与侧栏结果不作为本轮契约通过证明。本轮不改变UI，不要求无关截图重测。

| 结构化契约检查 | 当前证据与边界 | 位置 |
| --- | --- | --- |
| 生成器 | 13项通过及生成一致性检查；Schema与metadata生成四个产物，新增详情字段保持可选。manifestSchema保持对象类型，避免旧客户端访问Schema属性受布尔联合类型影响 | 生成器本轮定向测试输出 |
| Python CLI响应边界 | 52项通过，覆盖缺详情、ID不匹配/重复、布尔类型错误及argsSchema矛盾；旧宿主无特性仍兼容，不是通用Schema解释器 | `build-extensions/contract-details-cli-test.log` |
| 真实CLI | 19项通过、退出0，覆盖每种能力有效声明、禁参/缺参拒绝、偏好副作用描述、配置未变与无命令执行 | `bench_data/extension-structured-contract-20261103/api.json`、`checks.json`，日志 `build-extensions/structured-live-cli.log` |
| 原生 / 真实MCP | 标准独立构建成功；contract95/95、manifest111/111、真实MCP14/14全部通过。MCP完整发现内容与内置助手共享adapter一致，无新增执行工具 | `build-extensions/structured-contract-build.log`、`structured-developer_contract-test.log`、`structured-manifest-test.log`、`structured-mcp-test.log`（均在同目录） |

| 右侧面板放置检查 | 结果与限制 | 证据 |
| --- | --- | --- |
| 原生子窗口组件 | 83 项通过、退出0，WS_CHILD 与 Compositor 实际绘制非空；已看亮色100%无裁切 | `build-extensions/sidebar-child-build.log`、`build-extensions/sidebar-child-test.log`、`bench_data/extension-sidebar-component-20261103/dock-child-*.png` |
| 宿主占位/恢复与回退 | 最终30/30 PASS、退出0：实际视图切换、选择、滚轮、真实源拖放文件区COPY/侧栏NONE、详情共存/偏好保留、关闭/停用/重开恢复，以及800 DIP保留/640 DIP回退不抢焦点 | `bench_data/extension-sidebar-capture-20261103-154220/verification-summary.json`、同目录 `capture/report.txt`、`build-extensions/sidebar-final-ui-probe.log` |
| 侧栏开发重载 | 22 PASS / 0 FAIL，与主宿主检查范围分别记录 | `bench_data/extension-sidebar-dev-capture-20261103-151910/capture/report.txt` |
| 侧栏多包存储 | 23 PASS / 0 FAIL；不替代主宿主探针 | `bench_data/extension-sidebar-store-capture-20261103-151914/capture/report.txt` |
| CLI 侧栏契约 | 真实宿主17项通过、退出0：发现、dock模板、test不冒充执行、pack/verify/install默认停用/enable、3种非法placement、旧默认/显式window兼容及无偏好副作用 | `bench_data/extension-dock-cli-20261103/checks.json`、`build-extensions/sidebar-live-cli.log` |
| 构建与契约 | 初次标准构建成功；最终独立构建目录的标准 target `pulse` 成功、退出0。原生parser111、contract62、panel83通过，组件83不重复累加 | `build-extensions/sidebar-isolated-build.log` 与本轮定向运行记录 |

侧栏 CLI 检查可用 `python tools/extensions/live_dock_check.py --output NEWDIR` 复现；它调用真实宿主，输出使用新目录，不能把脚本语法检查等同于该闭环通过。主窗口占位与恢复另由宿主探针和截图证明。

主宿主探针首轮为24通过、2失败：800 DIP 时原左侧栏进入窄栏状态，文件区仍足够，不能假定一定回退。测试改用640 DIP验证回退，并补充800 DIP保留检查；同时从不同视图模式开始验证按钮切换，加入真实拖放源对照。失败记录保留，最终强化检查30/30通过。

最终复测构建曾受共享 build 目录的 PCH/对象文件及辅助程序锁定影响，转到独立 `build-extensions` 完成标准 `pulse` 目标构建，退出0。临时构建图尝试不作为标准目标成功证据；最终报告来自该独立构建的程序，初次标准构建和CLI17项事实不变。

主窗口交换链截图不包含独立子窗口内容，主工作区和侧栏必须分别捕获，不能把单张主窗口截图描述成完整合成后的画面。已查看最终 `capture/host-with-dock-space.png`、`dock-light.png` 和 `dock-dark.png`，确认工作区预留及真实子面板显示，亮色侧栏无裁切；这些是分开捕获的证据。整体工作区恢复由对应功能断言与截图共同证明。

| UI 贡献与布局检查 | 结果与限制 | 证据 |
| --- | --- | --- |
| 原生面板组件 | 76 项通过，保留生命周期、只读字段及预览禁止执行检查 | `build-extensions/ui-authoring-panel-test.log` |
| 静态预览模型 | 24 项通过，不代表实际工具栏菜单执行通过 | 同轮定向检查输出 |
| 布局与密度外观 | grid/stack/compact/narrow 的真实渲染，明暗与 100%/150%/200% 截图检查正常 | `bench_data/extension-ui-authoring-component-20261103` |
| 跨包工具栏路由 | 25 项通过，两个包经实际 Dispatch 切换各自模式，注册表未改变 | `bench_data/extension-toolbar-multipack-20261103-02/capture` |
| 作者 CLI 与新模板 | 41 项通过，覆盖 workspace 模板/发现/test、三布局的 18 张原生预览、pack/verify/install/enable 及拒绝非法样式 | `bench_data/extension-authoring-cli-20261103/checks.json`、`build-extensions/ui-authoring-live-test.log` |
| Python 与生成边界 | CLI 46 项、store 14 项、生成器 6 项通过；含未初始化 store run 拒绝、发现与 UI 预设一致性 | 同轮定向检查输出，不替代真实窗口检查 |
| workspace 工具栏交互 | 最终 28/28 通过、退出 0，覆盖实际“更多”菜单键盘操作、共享分发、停用阻断/重新启用 | `bench_data/extension-ui-authoring-20261103-04/capture/report.txt`、`build-extensions/ui-authoring-label-build.log` |
| 最终可读性与外观 | 已看“更多”菜单亮/暗图：动作可读，长身份明确省略且完整提示保留，无重叠；stack compact 暗色150%正常，CLI grid compact 亮色100%静态禁用正确 | 最终 workspace 目录与 `bench_data/extension-authoring-cli-20261103` 截图 |

本次工具栏验收曾出现初次超时及第二次报告 3 项失败，相关源码和测试两处问题修复后，第三次功能检查 28 项通过。随后截图发现包身份前缀挤占动作标题，已将显示顺序改为“动作 · 包名 [id]”，保留完整提示；最终增量构建后仅复跑受影响的 workspace 检查，28/28 通过。前面的失败记录保留，不将失败或被修正的测试描述为首次通过，也不把长身份的明确省略称为“完全无截断”。

| 本地存储与多包检查 | 结果与限制 | 证据 |
| --- | --- | --- |
| 真实 CLI 生命周期与启动 | 生命周期 35 项、run 17 项通过；不以启动 PID 代替加载成功 | `build-extensions/store-*.log` |
| 原生存储与直接依赖 | store 37 项通过、1 项因符号链接权限限制跳过；runtime 35、CLI 29、dev 24、contract 48、MCP 13 项通过，跳过不计通过 | 对应 `build-extensions/store-*.log` |
| 多包应用内检查 | 21 项通过，进程退出 0，注册表未被会话操作修改；截图已检查 | `bench_data/extension-store-capture-20261103-142100/verification-summary.json` 及该目录 `capture/` |
| 开发重载相关复测 | 21 项应用内检查与截图通过，不替代新增布局/菜单验收 | `bench_data/extension-dev-capture-20261103-141831` |

### 此前工具链基线

下表保留此前实际运行证据，不并入上述本地存储检查次数。

| 检查 | 实际结果与边界 | 证据 |
| --- | --- | --- |
| 真实宿主工具链 | 66 项通过，覆盖四模板 init/test/pack/verify、拒绝覆盖、doctor 脱敏、六张原生预览、坏哈希与非法语义拒绝、无效输入无产物及隔离 LOCALAPPDATA 未改 | `build-extensions/toolchain-complete-test.log`、`bench_data/extension-toolchain-complete-20261103/checks.json` |
| 静态外观 | 已查看亮色 100% 与前一轮暗色 150% 产物，无裁切或重叠，按钮正确禁用；未声称全部状态均逐张检查 | `extension-toolchain-complete-20261103/screenshots`、`extension-toolchain-live-20261103/screenshots`（位于 `bench_data`） |
| Python 工具边界 | 40 项 mock 回归通过，包括有界清单/归档、原子不覆盖、开发功能探测与全新配置门槛；不计为真实窗口验收 | `tools/extensions/test_pulse_ext.py` |
| 生成与一致性 | 生成器 5 项 unittest 通过，`generate_sdk.py --check`、`catalog.py --check` 通过 | 对应命令的实际运行结果 |
| CI 配置 | 增加 `.github/workflows/extensions.yml`，Windows/Python 3.8 静态与 mock 检查 | 工作流尚未实际运行，不记为 CI 通过 |
| 原生构建与定向检查 | 主程序构建通过，无新增警告；重载 24、运行时 35、原生 CLI 29、契约 47、真实 MCP 13 项通过 | `bench_data/extension-dev-capture-20261103-135154/capture/verification-summary.json`；原始控制台输出保留在本轮工具记录，未另存 raw 日志 |
| 隔离开发 CLI | 真实启动开发窗口，合法修改、非法保留、修复恢复、关闭与最终 stopped 诊断，共 7 项通过，退出 0 | `build-extensions/dev-cli-live-test.log`、`bench_data/extension-dev-cli-live-20261103/checks.json` |
| 应用内重载与截图 | 21 项通过，包含新版按钮实际刷新、旧菜单 ID 失效、非法更新保留、停用执行阻断、重载不自动启用、停止后不提交；退出 0，三张截图已检查，无重叠、裁切或黑图 | `bench_data/extension-dev-capture-20261103-135154/capture/report.txt` 及 `before.png` / `after.png` / `reenabled.png` |
| 本地化一致性 | `gen_strings.py --check` 与 `check_no_pick.py` 通过 | 本轮命令输出；未重新生成无关资源 |

隔离开发 CLI 检查通过 `python tools/extensions/live_dev_check.py --output NEWDIR` 复现，仅向自身启动的 Pulse 子进程主窗口请求关闭；不会关闭其他 Pulse 窗口。原生截图探针使用 `PULSE_TEST_EXTENSION_DEV_CAPTURE`，清单副本与截图目录必须位于 `PULSE_TEST_DATA_DIR` 内，同时指定 `--test-instance --extension-dev`。探针会改写清单，必须复制样例到独立测试目录，不能直接传用户包。当前未验证跨物理显示器或跨进程文件系统竞争；最初开发窗口关闭后不迁移观察任务。

真实宿主检查通过 `python tools/extensions/live_check.py --output NEWDIR` 复现，输出必须使用新目录；它不加入默认 unittest 自动执行。生成器检查用 `python tools/extensions/generate_sdk.py --check`，目录一致性用 `python tools/extensions/catalog.py --check`。按修改范围选择检查，不默认重跑历史应用内套件。

## 第一阶段交付验证（命令接口）

以下为既有交付时的验证记录，保留当时范围、数字、限制和产物路径，不表示每次后续修改都重跑了这些检查。

在 `codex/extension-platform` 分支完成主程序及两个定向测试目标构建。清单测试 21 项、运行时测试 16 项通过；示例实际主程序检查退出 0，未支持能力清单退出 2。应用内测试 18 项通过，覆盖贡献搜索、明暗渲染、停用拒绝执行、重新启用、真实上级/后退/前进/刷新与加载完成。截图仅验证增强包贡献区域，未覆盖完整平台的未来 UI。

应用内截图测试使用隔离配置，运行后显式等待退出：

```powershell
$repoRoot = (Get-Location).Path
$env:PULSE_TEST_DATA_DIR = Join-Path $repoRoot 'bench_data\extension-ui-profile'
$env:PULSE_TEST_EXTENSION_CAPTURE = Join-Path $repoRoot 'bench_data\extension-ui-capture'
$fixture = Join-Path $repoRoot 'bench_data\extension-ui-folder\child'
New-Item -ItemType Directory -Force $env:PULSE_TEST_DATA_DIR, $fixture | Out-Null
$manifest = (Resolve-Path examples/extensions/navigation/manifest.json).Path
$test = Start-Process -FilePath .\build\pulse.exe -ArgumentList @('--test-instance', '--extension-manifest', ('"'+$manifest+'"'), ('"'+$fixture+'"')) -PassThru -WindowStyle Hidden
if (!$test.WaitForExit(30000)) { Stop-Process -Id $test.Id; throw 'Screenshot test timeout' }
if ($test.ExitCode -ne 0) { throw "Screenshot test failed: $($test.ExitCode)" }
Get-Content (Join-Path $env:PULSE_TEST_EXTENSION_CAPTURE 'report.txt')
Remove-Item Env:PULSE_TEST_DATA_DIR, Env:PULSE_TEST_EXTENSION_CAPTURE
```

输出 `enabled-light.png`、`enabled-dark.png`、`disabled.png` 与逐项 `report.txt`。截图使用实际 FluentMenu 渲染，导航经正式 `HandlePaletteCommand` 和运行时门禁执行；不是鼠标自动点击测试。

手动体验可双击 `examples/extensions/navigation/run.cmd`，或运行 `examples/extensions/navigation/run.ps1`，启动后按 Ctrl+Shift+K。脚本先验证清单再打开正常交互窗口，不自动修改增强包或安装设置。

面板字段属于本轮开发预览，需要包含面板支持的新构建；旧宿主会按未知字段拒绝带 `panel` 的清单。向后兼容指新宿主继续接受原有纯命令包，不代表旧宿主能够识别新增字段。


## 第二阶段验证：声明式工具面板

主程序构建通过。协议/运行时共 52 项检查通过；独立原生组件 27 项检查通过；实际主程序的增强包面板 36 项检查通过。应用内截图已逐张检查示例的明暗与 100%/150%/200% 缩放，以及 8 按钮长文案：无重叠和意外裁切，超长内容显式省略。DPI 验证包含实际窗口消息位置检查，未声称完成物理双屏拖动验证。

功能检查包含面板入口、按钮真实导航、Tab/Enter/Esc、关闭后禁止调用、停用关闭、重新启用、主窗口销毁和旧回调失效。修复了 DPI 变化错误回到 owner 屏幕的位置处理；截图读取改用交换缓冲前保留的实际绘制帧，并拒绝空白/单色结果。

本轮应用内测试沿用上节脚本，将清单改为 `examples/extensions/navigation-panel/manifest.json`，输出目录改为 `bench_data/extension-panel-capture`。主程序截图报告位于该目录的 `report.txt`；组件测试 `pulse_extension_panel_test` 默认输出到 `bench_data/extension-panel-component`。为避开其他任务使用同一个构建目录，本轮组件目标使用 `build-extensions` 构建。

交付的是原生工具窗口这一种 UI 贡献点。标题、说明、按钮名称和排序由清单控制，底层绘制、键盘交互和生命周期由 Pulse 维护；自定义停靠布局、任意组件树和脚本仍未开放。

`context.directory` 是用于展示的路径文字，复用 Pulse 的友好路径格式，隐藏 Windows 内部长路径前缀；它不是可用于授权或文件操作的路径引用。


## 第三阶段验证：只读上下文绑定

主程序构建通过。清单 57 项、运行时 16 项、原生组件 42 项检查通过；最终应用内 48 项检查通过。覆盖实际计时器更新、单选名称、多选空值、快速清空、加载状态隔离、导航目录更新、停用释放和截图不读取。明暗主题、100%/150%/200% 缩放与长路径截图已检查；输出位于 `bench_data/extension-context-capture`。

本轮曾出现一次“检查完成后退出异常”。调试器三次运行未重现，未捕获原始异常栈。代码审查另行确认了全局面板对象依赖线程绘图状态的退出顺序隐患，现改为在所属主窗口销毁时释放整个对象，停用也立即释放对象。修复后连续三次测试正常退出，友好路径显示调整后的最终复测也正常退出；不能据此断言已证明原异常的根因。

复现本轮应用内测试时，用 `examples/extensions/selection-panel/manifest.json` 清单和独立目录 `bench_data/extension-context-folder/child`，在该目录准备 `alpha.txt`、`beta.txt` 两个文本夹具，然后沿用前述环境变量与显式等待退出的方式。两个夹具仅为测试断言服务，正常使用增强包不要求这些文件。

`read_fields` 是宿主内部的 UI 线程回调，只能读取缓存状态，不能执行阻塞文件或网络操作；它不作为脚本入口对包开放。现有绑定也不是通用文件读取 API。

## 第四阶段验证：AI 开发契约与 MCP

公开契约检查 35 项、生产适配器检查 38 项、真实 MCP 管道检查 11 项通过。MCP 测试启动实际 `Pulse.AI.exe`，连接隔离测试 broker，并注册与生产相同的两个开发工具处理函数；不需要开启或修改用户的 AI 设置。它覆盖发现、返回清单 Schema、有效/无效 JSON、错误参数、64 KiB 和中文多字节边界以及正常退出。这不等于测试了真实账号、外部模型或已安装版本的升级流程。

定向测试命令（先在开发环境构建同名目标）：

```powershell
.\build-extensions\pulse_extension_developer_contract_test.exe
.\build-extensions\pulse_extension_developer_tools_test.exe
.\build-extensions\pulse_extension_mcp_test.exe
```

契约测试会核对嵌入 Schema 与 `schemas/extensions/manifest.schema.json`，逐项验证公开能力与绑定可被实际解析器接受，并校对发现列表与 Schema 枚举。修改协议时应同步维护这些契约；不要删掉或弱化一致性测试来绕过漂移。

当前源码的原生面板检查 42 项通过，采用组件内部绘图捕获生成截图；已检查明暗主题与长字段排版，证据在 `bench_data/extension-developer-panel-capture`。该组件检查与完整主程序的应用内验收分别记录。

本阶段最终主程序与 AI 宿主构建通过。新主程序隔离实例的 48 项应用内检查通过，进程退出码为 0；截图位于 `bench_data/extension-developer-capture`，已检查亮色单选字段与暗色 150% 面板。构建期间曾因共享目录内并行修改造成文字生成文件缺失和新增源文件未纳入的问题，分别通过既定文字生成流程和重新生成构建清单解决，未修改文件选择器功能逻辑。文字生成一致性与禁止字面量 Pick 检查通过。

## 第五阶段验证：视图切换与视图状态

交付 `view.setMode` 八种模式、`context.viewMode` 本地化显示绑定，以及 `examples/extensions/view-panel` 参考包。当前公开范围为五种命令能力和四种绑定，单个面板仍最多选择三种绑定。MCP/内置助手继续通过同一开发契约发现与校验；无需增加另一个执行工具。视图切换作用于调用窗口活动标签页的焦点栏，沿用普通 Pulse 操作的文件夹偏好保存行为；停用包不还原已选择的视图。

本轮主程序构建通过；清单 83 项、运行时 35 项、开发契约 46 项、工具适配器 42 项、真实 MCP 通信 13 项，共 219 项定向检查通过。新应用内用例 47 项通过，最终进程退出码为 0。覆盖八种实际视图、按钮/命令入口、选择与目录保持、偏好更新、普通宿主操作反向更新绑定、加载状态、跨标签页、快速切换、停用/启用及键盘操作。没有重跑完整历史回归。

应用内截图位于 `bench_data/extension-view-capture`：`host-large-icons.png`、`host-details.png` 和 `view-panel-light-100.png`、`view-panel-dark-150.png`、`view-panel-light-200.png`。上述图片已检查，无本次新增的重叠或裁切。主窗口捕获只在隔离配置中采用不透明背景，以避免应用绘图缓冲不包含系统 Mica 背景的问题；正常启动示例不改变材质设置。

复现应用内测试可沿用前述隔离脚本，将清单改为 `examples/extensions/view-panel/manifest.json`，测试目录改为 `bench_data/extension-view-folder/child`，并在其中准备 `alpha.txt`。输出目录使用 `bench_data/extension-view-capture`；测试配置使用独立的 `PULSE_TEST_DATA_DIR`。普通体验直接运行该示例的 `run.cmd`，再按 Ctrl+Shift+K 打开“视图切换工具”。

新增 Schema 已检查 JSON 结构、条件定义及与嵌入契约的一致性，并由宿主解析器执行有效/无效参数测试。环境没有独立 JSON Schema 验证器或 TypeScript 编译器，未将这些外部工具验证标记为通过。文字生成一致性与禁止字面量 Pick 检查通过，本轮未增加新的界面文字资源。

## 第六阶段验证：开发命令行工具

交付 `tools/extensions/pulse_ext.py` 和 Windows 入口 `pulse-ext.cmd`，实现 `init`、`inspect-sdk`、`validate`。新原生入口在窗口创建、单实例转发和用户配置加载之前处理请求；查询复用 MCP 的同一公开契约，校验复用宿主解析器。初始化支持四种现有模板，宿主校验通过后才创建目标目录，附带可复制的校验/加载说明；已有目录（包括空目录）一律拒绝覆盖。

本轮主程序构建通过；原生 CLI 29 项、既有运行时兼容 35 项、Python mock 边界 16 项、真实宿主端到端 16 项，共 96 项检查通过。真实端到端覆盖四种模板、中文及空格路径、拒绝覆盖、错误诊断、64 KiB 限额和上一阶段宿主的保护性退出；Windows cmd 入口也实测返回有效契约。未把 mock 检查当作真实宿主验证。

由 CLI 实际生成的视图包又通过 47 项应用内检查，进程退出码为 0。截图位于 `bench_data/extension-cli-capture`，已检查亮色 100% 和暗色 150% 面板，未发现新增裁切或重叠。使用独立测试配置，未安装包或修改用户日常设置。主程序首次链接受另一个任务同步加入的测试入口影响，增量重建后通过，本轮未修改那部分功能逻辑。

开发者可构建并运行 `pulse_extension_developer_cli_test`、`pulse_extension_runtime_test`，并运行 `python -m unittest discover -s tools/extensions -p test_pulse_ext.py` 复现定向检查。完整宿主体验按本节前面的 CLI 指令创建包后显式加载；包仍须实际操作与截图验收。`preview`、热加载、安装打包和诊断命令仍待后续实现。

## 第七阶段验证：静态组件预览

交付独立 `Pulse.ExtensionPreview.exe` 和 `pulse-ext preview`。预览程序只链接清单解析、模拟模型及原生面板绘制，不链接 AppState、增强包命令分发、文件操作或索引宿主。窗口保持隐藏，绘制入口强制清除命令与字段读取回调；因此按钮呈禁用状态，不会执行清单中的能力。普通面板的显示与执行路径保持原行为。

本轮只构建受影响的预览程序与定向测试目标，未重建主程序或运行完整历史回归。Python CLI mock 检查 23 项、预览模型 20 项、原生面板组件 45 项、真实 CLI/预览程序端到端 40 项，共 128 项通过。组件检查包含传入操作回调仍不执行、没有新增可见窗口、不抢焦点。端到端检查包含六种图片组合、中文输出路径、三种模拟状态、视图标签、报告、拒绝已有目录（含空目录）、无面板/非法清单，以及测试用户配置目录保持不变。

本轮实际产物位于 `bench_data/extension-preview-live-94b60cb7`，也可通过 `bench_data/extension-preview-artifacts.txt` 定位。已查看选中亮色 100%、空值暗色 200%、加载暗色 100% 和视图面板亮色 200% 图片，未发现新增重叠或裁切；全部生成图片通过原生非空帧检查和 PNG 尺寸检查。`report.json` 包含 `mockData: true`、`commandsExecuted: 0`、模拟状态、图片路径及 `mockValues`，用于区分模拟输出与真实宿主状态。

`empty` 模拟无选择；`loading` 模拟加载期间选择信息不可用，不模拟异步加载动画或实际目录变化。静态预览通过后，命令执行、取消、停用及实时状态仍按原应用内验收流程检查。本轮没有新增或手改生成文字资源，文字生成一致性及禁止字面量 Pick 检查通过。

复现相关测试可构建运行 `pulse_extension_preview_model_test`、`pulse_extension_panel_test`，并运行 `python -m unittest discover -s tools/extensions -p test_pulse_ext.py`。真实图片按前述 `preview` 命令生成，每次使用新的输出目录；父目录需要已经存在。输出失败可能保留本次部分产物，工具不会清空已有目录。


## AI 模板发现与校验（2026-11-03）

`examples/extensions/catalog.json` 是九个模板 ID 的唯一目录；CLI 初始化、宿主发现、内置助手和 MCP 共用该目录。修改目录或示例清单后运行 `python tools/extensions/generate_sdk.py`，提交其五份生成输出（包含 `src/app/extensions/templates.generated.h`），再运行 `--check`。模板新增或删除也必须更新相应用例；不要手改嵌入内容。

宿主通过 `declarativeTemplates` 特性和 `templates` 列表声明支持。`get_extension_template` 只接受目录中的 ID，返回 `manifest` JSON 文本、`validated: true` 和 `executed: false`。返回前由当前宿主解析器验证；不会读取调用者提供的路径、安装或执行包。内置助手和 MCP 复用同一个生产处理函数；MCP 参数枚举由生成目录提供。旧宿主未声明此特性时，作者工具仍可使用本地模板并通过旧校验接口验证。

本轮主程序、AI 宿主及受影响测试目标构建通过。模板目录 6、生成器 17、作者 CLI 59、生产工具适配器 73、开发契约 133、真实 MCP 通信 18 项通过。MCP 覆盖取模板后校验返回清单，以及路径形式 ID 和多余参数拒绝；没有调用外部模型。真实 CLI 的 57 项检查通过，覆盖九模板初始化及真实宿主校验、能力参数和不修改用户偏好。证据为 `build-extensions/template-*-test.log`、`build-extensions/extension-template-build.log` 和 `bench_data/extension-template-discovery-20261103/checks.json`。这次只增加只读开发工具，不以已有 UI 截图作为其验证证据。


## 面板命令可用状态

`CapabilityService::Available` 绑定当前包主体和所属窗口，复制／移动复用 `transfer_ui.cpp` 的内存预检与请求捕获。轮询不申请调用凭据、不读取磁盘、不显示提示，也不创建计划；执行入口仍重新捕获并验证。导航、视图和布局保持原有行为；此接口为内部 UI 适配，尚未公开通用条件表达式或任务 API。

`ExtensionPanelSpec::read_enabled` 返回完整按钮状态快照。窗口每 250 毫秒刷新，鼠标和键盘共用的 Invoke 门禁立即再读；异常或错误数量令全部按钮禁用，关闭／重开期间的过期结果按窗口代次丢弃。绘制仅消费缓存，内部截图冻结快照，不执行实时读取回调；静态作者预览继续清除全部回调。

本轮组件 122 项、实际应用内 75 项、外部夹具复核 12 项通过。覆盖空选择、加载中的来源／目标、过期目录快照、超过 64 项、准备中的重复调用、恢复可用及单栏禁用，同时重新验证复制／移动／取消／冲突／停用的真实文件结果。应用内截图在 `bench_data/extension-availability-live-20261103-01/capture`，已核对空选择亮色及单栏暗色 150% 图片；组件最终证据在 `bench_data/extension-availability-component-20261103-02`（同一渲染输出的首次目视检查在 `-01`）。没有使用桌面截图或修改日常偏好。

构建证据为 `build-extensions/extension-availability-verified-build.log`，应用结果为 `build-extensions/extension-availability-live-test.log` 及捕获目录的 `report.txt`。首次构建遇到并行任务新加测试源文件未纳入及该文件缺少类型定义；重新生成构建清单、待原任务修正后构建通过，本任务未改其测试逻辑。最初组件命令使用相对输出路径被拒绝，改用新建夹具的绝对路径后通过，未将该次参数错误计为功能通过。


## AI 开发工具的单一接口目录

三个只读开发工具的名称、模型说明、参数名、长度限制和模板枚举统一在 `src/app/extensions/developer_tool_catalog.h`。内置助手的函数工具数组由 `DeveloperFunctionTools()` 构造；MCP 的工具目录和输入 Schema 从同一结构适配，保持原有工具顺序、broker 名称及只读属性。新增接口先修改该目录，再实现生产处理函数和注册；不要在助手或 MCP 中复制一份 JSON 参数描述。清单 Schema、能力元数据和模板内容仍按各自既有生成入口维护。

共享目录只消除描述漂移，不代替执行端检查：MCP 参数预检和宿主处理函数仍独立拒绝不合法数据，字节长度限制在运行时执行；其他已有 AI 执行工具仍走原有协议，尚未归并为增强包授权能力。契约检查直接比较内置助手实际使用的函数工具数组与 MCP 输出，覆盖三项名称、说明、完整 Schema 和只读边界。

共享工具目录阶段：主程序和 AI 宿主构建通过，144 项契约检查、18 项真实 MCP 检查通过；证据为 `build-extensions/extension-shared-tools-verified-build.log`、`extension-shared-tools-contract-test.log`、`extension-shared-tools-mcp-test.log`。构建曾报告测试局部变量遮蔽，已改名并复建；不存在新增编译警告。未将工具目录一致性检查称为外部模型调用验收。

## 按包归属的传输状态字段

`context.transferStatus` 是第五种只读绑定，面板仍最多三个字段。`panel_bridge` 捕获宿主上下文后，使用当前长效主体读取窗口最近传输；`transfer_ui` 在准备／确认阶段直接返回自身状态，提交后通过 `OpsManager::QueryControlled` 按归属读取真实结果。取消确认和准备失败也有明确显示，但不伪造任务 ID。新的准备请求取代该窗口上一条显示记录；窗口释放时清理，包版本／启用代次失效后不可重用。

`transfer_status.h` 将逐项结果归类为全部完成、部分完成、取消或失败。只要已有提交而不能确认全部完成，就显示部分完成；目标提交但源保留不会被当作完整移动。空结果或错误与成功标记并存不能显示全部成功。结果历史已淘汰时返回空状态，不使用缓存的“排队”或“完成”冒充当前事实。

绑定文字供展示，包含三语资源；它不是稳定机器状态枚举、工作流条件或公开任务查询接口。字段不含任务路径／ID／其他包历史。静态预览明确使用空值与作者 fallback，模板可用性通过 `bindings` 检测；只支持旧传输命令的宿主不能因此推断支持新状态字段。

状态绑定阶段最终验证：主程序与 AI 宿主构建通过，无新增编译警告。清单 160、预览模型 27、开发契约 146、生产工具处理 73、受控任务／结果分类 28 项通过；生成器 17、模板目录 6、作者 CLI 59 项通过。真实应用内 92 项及夹具独立复核 12 项通过，含准备／等待确认／取消／完成／失败的真实显示及重新启用后的旧主体隔离。亮色完成和暗色 150% 未完成截图已检查，无新增裁切或重叠，位于 `bench_data/extension-transfer-status-live-20261103-01/capture/status-completed-light.png`、`status-failed-dark.png`；这两张来自实际任务结果和应用内渲染。

构建日志：`build-extensions/extension-transfer-status-final-build.log`。定向日志：`build-extensions/transfer-status-{manifest,preview,contract,tools,ops}-test.log`、`transfer-status-{generator,template,cli}-test.log`；应用内过程为 `transfer-status-live-test.log`，详细断言在对应捕获目录的 `report.txt`。生成契约一致性、文字生成一致性及禁止字面量 Pick 检查通过。TypeScript 类型仍通过生成一致性检查，未冒称运行外部 TypeScript 编译器；Windows 8.1 未实机验证。

本轮 MCP 首次执行遗漏 `--extensions-only`，意外运行了完整 MCP 程序，56 项通过、9 项失败；原日志 `transfer-status-mcp-test.log` 保留。定位发现旧验证的工具数量仍写死为 9／11，阻止了后续协议／拒绝访问断言。将预期准确更新为 12，保留工具顺序、完整名称、严格 Schema、身份拒绝及原有结果断言。针对这些已知失败复验后，完整 MCP 65 项、增强包定向 18 项通过；最终证据为 `extension-mcp-count-build.log`、`extension-mcp-count-full-test.log`、`extension-mcp-count-focused-test.log`。没有删除或放宽失败测试，也没有扩大到应用全量回归。以后增强包工具验证显式使用 `pulse_ai_mcp_test.exe --extensions-only`。

本次工作继续保留共享工作区其他任务的修改。包含共享 TSV 的差异检查在其他已有修改的空繁体字段上报告尾部制表符；该 TSV 格式允许空列，本次新增九行均含明确繁体值，未为清理报告改动别人的条目。


## 可独立使用的 SDK 交付物（2026-11-03）

新增 `tools/extensions/bundle_sdk.py`，先检查生成契约、真实宿主发现以及九份模板的字节快照，再发布 SDK ZIP。输出目录必须不存在；完成的 ZIP 以不覆盖的方式发布，附带 SHA-256 和逐文件清单。归档固定时间戳与成员顺序，包含作者 CLI、Schema、TypeScript、九模板、离线文档、x64 原生预览器、LumaText DLL 及许可。Pulse 主程序另行提供，包记录验收宿主摘要；开发预览不称为签名或稳定平台。

打包输入采用明确清单，拒绝链接／重解析输入、超限文件、非 x64 预览二进制和缺失许可。归档验证在构造 ZIP 条目对象前限制目录数量和大小，并拒绝路径穿越、Windows 设备别名、大小写碰撞、未列出文件及摘要不符。离线公开文档链接必须完整；需要源码仓库的引用转成明确的文字参考，不伪装为 SDK 内可点击文件。作者入口为根目录 `pulse-ext.cmd`，从包内定位预览器和依赖，不需要源码检出。

发布验收脚本 `live_sdk_check.py` 在新的中文／空格路径中解压，清除 Python 模块搜索路径干扰，并仅调用包内作者 CLI／预览器。对 `dist/extension-sdk-20261103-02` 的实测得到 72 项交付检查通过：九模板创建及真实校验、六张原生模拟预览、默认停用、重复安装、能力升级拒绝、显式升级、回退、卸载及 SDK 文件不变。实际安装的两个同名包另通过 25 项应用内检查，包含命令和工具栏按包路由、信息绑定、停用隔离和恢复；不是只验证窗口出现。CLI `run` 的独立会话另有 17 项通过，涵盖正常退出及损坏版本在 UI 前拒绝。主程序使用隔离夹具，不修改日常配置；已检查原生静态预览暗色 200% 和实际已安装面板暗色 150% 截图，未发现新增裁切或重叠。

证据：`bench_data/extension-sdk-delivery-20261103-02/checks.json`、`native-profile/capture/report.txt`、`run-check/checks.json`，原始输出在 `build-extensions/sdk-delivery-live-02-test.log`。第一次验收脚本错误读取 `doctor.previewHost`，实际契约为 `doctor.checks.previewHost`；原失败日志保留为 `sdk-delivery-live-test.log`，修正测试取值后使用新目录完成验收，未改变产品返回值或弱化断言。预览目标构建日志 `extension-sdk-preview-build.log` 无新增警告。

打包边界与离线链接检查 10 项通过，证据为 `build-extensions/sdk-bundle-verified-unit-test.log`。额外安装了锁定版本 TypeScript 5.7.3，安装脚本禁用且依赖留在项目构建目录；首次下载遇到网络权限拒绝，获工具授权后下载成功。使用真实编译器检查已解压 SDK 的九个模板、宿主实际 API 描述及十个错误参数断言，strict 与 exactOptionalPropertyTypes 均开启，退出码为 0。类型证据位于 `bench_data/extension-sdk-types-20261103-01/report.json` 与 `compiler.log`。这补齐了先前只有生成一致性检查的限制，不覆盖未来未实现的脚本运行时。

类型检查使用 `tools/extensions/typecheck/package-lock.json` 锁定依赖，已加入扩展契约 CI；公开文档变化也触发该工作流。本轮证据来自本机实际执行，尚未声称 GitHub 上的工作流已运行。

最终交付归档固定为 `dist/extension-sdk-20261103-03/Pulse-Extension-SDK-v1-preview.zip`，SHA-256 为 `afb08abcd23a74429632c05e2499e7ae33d2ae41870c9852b1562404fba9ed1d`。对这一确切归档重新完成 74 项交付检查（增加验收前后归档不变检查）、25 项应用内检查及 17 项独立会话检查；完成标志为 true。最终解压件再次通过 TypeScript 5.7.3 的九模板、宿主描述和十个非法用法断言。原始记录为 `bench_data/extension-sdk-delivery-20261103-03` 与 `bench_data/extension-sdk-types-20261103-02`。交付目录的 `acceptance/` 保存不含本机命令路径的报告与两张已检查的应用内截图，作为归档之外的验收附件，避免验收后修改被测 ZIP。此交付是开发预览 SDK 的作者闭环，不代表整个平台路线图全部完成。

## 2026-11-03 UI 树共享控件修正交付

此前独立 GDI 外观已撤掉，现复用 Pulse Painter、EditHost、FluentMenu、字体和滚动条辅助函数。当前是 15 种有界节点的开发预览，仍不等于完整 UI 框架。真实组件对应关系及缺口见 extensions-ui-component-mapping.md，作者契约见 extensions-ui-tree.md；MCP/CLI 通过统一发现中的 declarativeUiTree、uiComponents 和模板目录探测能力，不扩大执行权限。

新交付物：dist/extension-sdk-ui-tree-20261103-01/Pulse-Extension-SDK-v1-preview.zip。其 acceptance/README.md 和 report.json 保存最终验收：原生树79、契约148、真实树窗口25、五类控件六组主题/DPI的RGB差异为零；独立解压SDK77、安装包窗口25、启动17；TypeScript严格编译10模板、20负例和真实host-api。包内UI树预览器另已实跑六张截图。最终ZIP摘要及逐文件清单验证通过，归档不再回写。

上述计数是不同定向验证，不应相加成独立功能总数。仍未完成所有宿主控件开放、完整无障碍/IME人工验收、实际高对比度切换、GPU设备丢失注入、虚拟化列表及通用动态更新。历史九模板/旧SDK记录仅代表当时验收；当前模板目录为十项。
## 2026-11-03 声明式 UI 组件树完整适配

此前15节点阶段的控件、虚拟数据、嵌套滚动和本地动态动作缺口已实现并验收。当前41种节点按宿主公开绘制Spec逐项映射，使用实际Painter/EditHost/FluentMenu；MCP/助手/CLI统一发现，11模板和完整组件目录同步。不是以节点数量推断完成，源码Spec覆盖测试、原生交互、MSAA、GPU恢复和应用内截图均有独立证据。

本轮结果：原生解析315、共同契约150、树交互125、独立集成57、虚拟数据与恢复52、MSAA27、预览模型28、旧面板124；完整目录真实宿主79项/48图、原UI树真实宿主25项/8图；原五控件六组主题/DPI像素对照差异为零。详见 extensions-ui-delivery.md、extensions-ui-tree.md 和 extensions-ui-component-mapping.md。不同检查不相加为功能总数。

P2的声明式组件树适配子项据上述证据完成；这不更改P0–P5中宿主业务能力、第三方脚本、签名分发等其他退出条件。接口明确的数据预算、原生资源封装及授权边界继续有效。最终SDK以验收归档及其acceptance旁附报告为准，已测试归档不回写。
最终交付归档已验收：dist/extension-sdk-ui-framework-20261103-01/Pulse-Extension-SDK-v1-preview.zip，SHA-256 29dafc7219c9c99d02bf0788a779339bcef711dfceb8f2af3d78fc65978f09c9。固定ZIP独立解压工作流80项、SDK安装包真实窗口25项、启动17项通过；实际解压版TypeScript严格编译11模板33负例及host-api通过；包内完整目录预览器实跑六图。旁附acceptance/README.md及report.json包含日志和应用内截图。归档及解压文件再次逐项校验未变化，维护文档本条为验收后记录，不回写归档。

## 2026-11-04 资源、工作流与平台边界

本轮公开契约为10命令能力、8资源操作、18模板。资源服务49项/6图，AI/MCP授权9项/2图，签名原生启动16项；各自确切报告及尚未开放的边界见[平台交付记录](extensions-platform-delivery.md)。历史模板计数仅对应各自固定归档。

有界工作流的确定性模型33项通过（`build-extensions/platform-workflow-controller-final.log`）；真实第09轮导航13项/3图、传输40项/9图通过（`bench_data/platform-workflow-host-20261104-09`）。逐次原生确认、真实文件结果、重复启动、取消、失败停止、跨主体访问及撤销后旧运行不可恢复均有定向检查，结束没有遗留搜索子进程。

保留第01–08轮失败记录。导航完成现在保存原始请求结果，不由后台刷新覆盖。探针按规范化路径匹配，激活真实按钮且验证新运行编号。故障注入在不可变请求捕获后删除精确隔离文件，避免提前删除触发目录刷新而误选下一文件；仍要求准备失败不弹确认、不执行后续步骤。错误机器码留在结构化结果，普通错误文案使用已有本地化资源。

脚本隔离仍未获准开放：AppContainer第06轮10项通过但回环拒绝证据不充分，退出1。生产PKI、运行中信任撤销、市场/自动更新和完整人工兼容矩阵不在本轮已交付范围内。

本轮固定归档：`dist/extension-sdk-platform-20261104-01/Pulse-Extension-SDK-v1-preview.zip`，SHA-256 `229c8a83e27016eb3dfae7b19d452d1398c3562ee999df1a8253ce37702c02db`。独立解压129项、安装窗口25项、启动17项通过；包内18模板、65项类型负例及实际host-api通过；四模板24张原生预览图。`acceptance/report.json` 保存文件摘要、检查结果和未完成项。本条为验收后记录，不回写固定ZIP；完整边界见平台交付记录。

后续生命周期补齐：资源双包57项/6图、真实重载及回调释放16项、引用模型21项；工作流真实63项/15图、模型35项通过。契约指纹/compare-sdk与138动作机器开放政策已实现，分别有10项定向检查；共同原生契约169项通过。具体证据、历史边界及新固定归档验收分别见[平台记录](extensions-platform-delivery.md)、[兼容检查](extensions-compatibility.md)和[工作流维护](extensions-workflows.md)。持久诊断、完整产品授权、P4脚本门禁、生产分发及人工矩阵仍未完成。

新增固定归档platform-20261104-02，SHA-256 `a8735b260de946dfac28e5d8c23ff0349853112713b75cd65fc56f5e5cb9cf10`：独立解压133、安装25、启动17项通过；18模板/65类型负例及实际host-api通过。旁附acceptance逐文件摘要和应用内截图，旧归档不变，本条不回写ZIP。

## 2026-11-04 选择与排序固定交付

最终开发预览归档：`dist/extension-sdk-capabilities-20261104-02/Pulse-Extension-SDK-v1-preview.zip`，SHA-256 `2fd718f96a70ea872015a677e6d5d58e58c5135ecf57aa308be719e91c8c0ba9`。63文件、19模板、14命令能力、8资源操作。独立解压作者链路145项、安装窗口25项、启动17项通过；19模板、73项类型负例和实际host-api严格编译通过。五模板30张原生预览图；新增选择排序模板的明暗主题及100/150/200%六图均已查看，预览不执行命令。

真实目录62项/6图已通过并逐图复核；导航13项/3图和文件工作流63项/15图回归通过。`acceptance/report.json` 保存实际宿主摘要、固定源码2789文件摘要、受控快照修复、原生定向结果、原始失败记录和所有证据文件摘要。归档及解压文件逐项一致，首轮旧帧截图不计最终通过。最终宿主位于 `build-extensions/frozen-selection-sort/build/pulse.exe`；共享导航改动仍由其他任务维护，不以本记录声称整个共享工作区已验证。

本条为归档验收后的记录，不回写ZIP。UI框架/原生控件适配与本次四项内部能力已交付；完整人工输入法/辅助技术矩阵、其余内部能力、持久诊断、完整授权产品界面、脚本隔离及生产分发仍按实施方案的剩余项推进，不能把本次增量写成全部计划完成。

## 宿主持久诊断与会话审计

[持久诊断](extensions-diagnostics.md)采用独立有界worker与轮转目录，提供宿主本地read/export；不是新增包资源权限，`audit.list`继续仅返回当前主体/owner会话。故障排查必须同时看accepted/persisted/dropped/failures，不能把入队当落盘；强退可能丢失尚未提交事件。未通过实际归档验收的源码检查不能替代宿主运行证据。
