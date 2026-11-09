# 声明式 UI 树

先通过 `describe_extension_api` 检查 `developerFeatures` 中的 `declarativeUiTree`。`uiComponents` 提供版本、节点数与深度限制以及组件完整 Schema。发现接口只描述能力，不执行事件。

`panel.ui` 包含 `version: 1`、可选的 `state`（key/value 字符串数组）和 `root`。每个节点必须有唯一 `id`。最多 128 个节点，深度最多 8；状态最多 32 项。未知属性、无效引用由宿主拒绝。具体字段及范围以返回的 Schema 为准。

布局组件为 stack、grid、group、scrollView、tabs、tab；tabs 的直接子节点必须是 tab。显示与交互组件为 text、button、textInput、checkbox、switch、select、list、progress、divider，名称区分大小写。list 是有界选项选择控件，不支持目录枚举或自定义子节点。

text/progress 的 binding 可引用允许的只读 context 或 `state.<key>`。textInput、checkbox、select、list、tabs 用 state 指定已声明的本地状态键。checkbox 使用字符串 true/false；select、list、tabs 使用从零开始的规范十进制索引字符串；progress 的状态绑定使用 0–100 的整数。原生输入只更新本地展示状态，不改变文件或宿主设置。button 的 command 必须引用清单中的命令，复用宿主能力授权和确认流程。没有脚本回调或任意代码执行。

示例 `examples/extensions/ui-tree/manifest.json` 将设置组、文本输入、选项列表和双列视图按钮放在滚动容器中。两颗按钮分别切换详细信息和平铺视图；输入和选项不会自动执行这些命令。

在仓库根目录执行（先构建对应宿主）：

```powershell
python tools/extensions/pulse_ext.py --pulse build-extensions/pulse.exe validate examples/extensions/ui-tree/manifest.json
python tools/extensions/pulse_ext.py --pulse build-extensions/pulse.exe test examples/extensions/ui-tree/manifest.json
python tools/extensions/pulse_ext.py --pulse build-extensions/pulse.exe --preview-host build-extensions/Pulse.ExtensionPreview.exe preview examples/extensions/ui-tree/manifest.json --output bench_data/ui-tree-preview
python tools/extensions/pulse_ext.py --pulse build-extensions/pulse.exe dev examples/extensions/ui-tree/manifest.json --profile bench_data/ui-tree-profile
```

validate 使用真实宿主验证清单，test 检查作者契约（不执行界面交互），preview 向新目录导出六张模拟预览，dev 使用隔离配置运行交互宿主。预览输出目录必须不存在；截图不能代替输入、切换、键盘焦点、滚动及窄窗口行为验证。安装和打包步骤见 [SDK 快速入门](../extensions-sdk-quickstart.md)，在明确接受清单能力后安装。

当前范围：不提供脚本事件或自定义渲染。scrollView 支持独立嵌套视口；textInput 必须提供非空 text 作为无障碍名称。UI 树与旧面板 commands、fields、groups、layout、primaryCommand 互斥，不能混用。本文描述受支持的契约；实际交互仍应按上面的预览步骤验证。

控件映射见 [宿主共享控件映射](extensions-ui-component-mapping.md)。按钮使用宿主 standard/primary/danger/transparent 样式；文本框使用宿主编辑控件，可设置 placeholder；switch 与 checkbox 共用字符串 true/false 状态约定。进度支持 bar/ring 与 normal/paused/error/indeterminate 状态。这些是宿主已有控件语义，不开放自定义颜色或绘制。示例保留复选框并增加 review-switch、主操作按钮和环形进度；它们的实际视觉及交互需由当前宿主验证，本文不代表视觉验收结果。

## 完整组件目录与安全动作

`ui-catalog` 模板覆盖发现 Schema 中的每一种节点，目录覆盖测试按 Schema 动态核对，不依赖固定数量。原 `ui-tree` 示例保留作为较小的交互入门。完整目录位于 `examples/extensions/ui-catalog/manifest.json`，可将前面的 validate/test/preview/dev 命令路径替换为此文件。

`uiComponents.components` 逐项提供 type、完整 schema、stateFields、commandFields。嵌套菜单、数据行等引用在 definitions 中；副作用、是否修改文件、是否保存偏好、是否支持取消仍以 capabilityDetails 为准。AI 应先发现组件再生成清单，不能把显示能力推断成新的执行权限。

新增 radio 使用 true/false 字符串状态；slider 使用 0 到 stops-1 的索引字符串；segmented 使用选项索引。toggle/transparentToggle 按钮必须绑定状态，可选命令仍引用已声明能力。enabledBinding/visibleBinding 引用 `state.<key>` 布尔状态，不接受任意表达式。

menuButton 和 splitButton 支持有界嵌套菜单：叶项有 command，子菜单有 children，两者互斥；菜单总项数和深度由宿主验证。infoBar actions 最多两项；closeCommand 是显式宿主命令。glyph、keycap、文字角色和标签颜色只按对应组件的公开字段解释，不是 CSS 或脚本入口。

virtualList、tree、dataTable 可使用清单内的本地 rows，或显式声明受支持的 dataSource；两者互斥，不读取任意文件。每个数据节点最多 4096 行，树深度最多 8；dataTable 最多 16 列，列宽 48–800 DIP。选择状态保存行 ID，空字符串表示未选中。行命令与普通按钮复用相同授权。表格单元格数必须与列数一致。节点预算、跨引用和 UTF-8 字节长度仍需要原生验证，TypeScript 无法证明这些约束。

这些文档与类型描述的是公开契约；新增组件的实际绘制、滚动、键盘、菜单和命令路由验收需要当前宿主的独立证据，不能用 SDK 编译通过代替。

## 其余宿主绘制入口

- trayCard：text/detail/itemCount/moveIntent 仅描述卡片；command 是显式声明动作。
- commandSearch：placeholder/keycap/state 提供本地输入，不隐式搜索文件或执行命令。
- splitPane：children 必须恰好两项；orientation 指定方向，value 或 state 使用 0–100 分隔比例。宿主管理拖动与布局。
- sidebarSection、stagingTray：state 为 true/false 展开状态；后者的 helper/countLabel/actionText/itemCount 只是展示，command 才是显式动作。
- driveItem：capacity 为 0–100；freeBytes/totalBytes 为准确整数，已知总量时 free 不得超过 total。清单不获得设备查询权限。
- paneHeader：summary/filterText 为显示内容；filterCommand/sortCommand 引用现有能力，不隐式修改宿主筛选或排序。
- fileRow：modified/fileType/fileSize/status/kind/tagColors/cut 是本地行展示；最多三个 #RRGGBB 标签。没有私有位图或文件句柄输入。
- stagingItem：missing 表示展示状态；statusBar 使用 text/centerText/rightText/ready。
- tooltip：text 使用宿主提示绘制入口；titleBarButton 的 role 选择视觉角色，command 仍为必填，close 等名称不授予关闭窗口的隐式权限。

textInput 可使用 trailingGlyph/keycap/badgeText；tab 的 closeCommand 引用声明命令；badge.dot 表达点状标记。实际焦点、IME、悬停、按下、动画、设备资源与布局几何全部由宿主适配器管理。完整映射按公开 Spec 核对，不能用节点数量宣称全部输入语义或视觉状态均已验收。

## 本地动作与宿主命令

`panel.ui.actions` 声明本地动作：`{id, updates:[{key,value}]}`，最多 32 个动作，每个最多 32 项更新。updates 存在时 key 必须是已声明的状态，value 保持字符串形式并满足对应控件状态约束。actions 也可使用 dataOperation；两者至少提供一项。更新由 UI 适配器原子提交；任一更新无效时整批不改变状态。动作 ID 不得与宿主命令 ID 冲突。

节点、菜单和 closeCommand 的动作引用可指向本地动作或清单宿主命令。本地动作可修改声明状态、校验输入和操作已声明的数据源；任务源的 cancel 仅请求取消本包所属的受控任务，不创建新的文件操作。宿主命令仍使用原有授权、确认与副作用说明。不能把本地动作写成脚本、进程或任意文件操作。validate 当前用于本地动作，不提供“校验后自动执行任意宿主命令”的链式调用。

完整目录的 dismissInfo 将 infoVisible 改为 false，infoBar 使用 visibleBinding=state.infoVisible，关闭按钮因此实际隐藏提示；提示中的 Details 动作仍是显式宿主视图命令。类型检查只能检查结构，重复 ID、状态有效性、引用和原子更新由原生验证及实际交互测试负责。

## 三类参考页面

- `settings-form`：显示名称必填并限制长度，提交动作先 validate 输入节点，再更新本地 submitted 状态。无效输入应显示字段 message，成功提示只在校验通过后出现；不会提交网络或更改宿主偏好。
- `collection-browser`：virtualList 从 `context.directoryEntries` 读取当前目录已加载的内存快照，pageSize 分页，filterState/sortState 控制本地筛选和 nameAsc/nameDesc 排序。刷新、下一页、取消、重试是显式 dataOperation；没有任意路径读取能力。
- `task-monitor`：开始按钮使用既有受控复制能力，保留每次原生确认。`context.transferTask` 仅观察本包 owner/generation 的任务，展示进度、状态和错误；取消只作用于所属任务。刷新/重试只是重新订阅，不再次复制文件；关闭面板取消订阅，不取消实际任务。

动作的 validate 数组引用带 validation 的输入节点，校验失败时不得执行 updates 或 dataOperation。dataOperation.target 引用带 dataSource 的节点，operation 为 refresh/nextPage/cancel/retry。状态、错误、进度通过声明状态键显示，过期请求结果由宿主管理代际隔离。

每个集合最多保留 4096 行。达到预算但仍有更多数据时，状态明确为 error，保留已加载行并停止续页；请缩小筛选后刷新。页游标绑定目录快照版本和筛选/排序条件，快照改变后旧游标不得拼接新数据；retry 从新快照的第一页重新加载。首页订阅随当前目录快照更新，关闭面板释放订阅。

数据表的目录源按列 ID 提供 `name`（名称）、`size`（字节数）、`type`（宿主本地化类型）；任务源提供 `name` 或 `status`（状态文案）。未知列 ID 返回空单元格，不推测其业务含义。原生静态预览明确标记模拟数据，不连接目录或任务，也不会执行确认、复制或取消。

可用前文 CLI validate/test/preview/dev 命令替换三份模板路径。预览和模拟宿主验证只能说明结构和确定状态，不能替代实际目录快照、分页取消及真实受控任务监控的宿主验收。新增三页当前验证进度随交付记录更新，不能沿用旧静态目录的通过证据。

## 组合属性表

`property-sheet` 是 Group 内嵌两列 Grid，每组以 Text 标签与 Text 只读绑定值组成的页面 recipe。它展示当前目录、选择数量、单项名称和视图，沿用既有刷新命令。没有新 PropertySheet 宿主 Spec，也不提供任意属性读取、文件修改或新增权限；空选择等状态遵循既有 context 绑定语义。模板应与其他模板一样执行原生验证、类型检查和实际宿主布局检查。

## 尺寸与伸缩布局

minWidth/minHeight/maxWidth/maxHeight 使用 0–4096 的整数 DIP，max 为 0 表示无上限。非 scrollView 容器只允许 maxHeight=0，避免截断子控件。flex 为 0–16 整数，只能出现在 stack 的直接子节点；根节点、grid/group 等其他父节点下不能使用 flex。stack.orientation 支持 horizontal/vertical，狭窄视口优先保证内容在可视范围内，横向排列可退回纵向。

collection-browser 的排序按钮使用 horizontal stack、flex=1、minWidth=140，作为两按钮均分及窄窗口回退的真实页面示例。尺寸声明由宿主布局适配器处理，不接受 CSS；类型只约束字段类型和结构，数值范围仍由 Schema 与原生解析验证。布局截图和交互结果由对应真实页面验收提供，不以模板能解析代替。

最大高度限制伸缩增长，但不会压缩宿主控件的自然可读高度。输入校验提示在控件高度之外另占空间，避免被限高裁掉；需要严格限高并保持内容可达时使用 scrollView。根滚动区域和分栏中的滚动区域始终以实际可视高度为上限，minHeight 不会将滚动条推到窗口外。横向伸缩从最小宽度（未指定时为 160 DIP）分配剩余空间，达到最大宽度的节点退出剩余分配；纵向伸缩从自然测量高度开始，保留间距及反馈占位。
