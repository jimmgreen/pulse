# 扩展 UI 与 Pulse 现有组件的对应关系

当前实现与实测结果见 [声明式 UI 交付记录](extensions-ui-delivery.md)。下文保留初次审计历史，旧的缺口描述不代表最终状态；末尾源码 Spec 映射是当前公开契约对应表。

本文前半部分是首轮源码核对的历史记录，不能作为当前支持清单；新增契约以末尾扩展记录及宿主发现 Schema 为准。`fluent::Painter` 是即时绘制接口，`ControlState` 明确由调用者持有输入及动画状态；复用绘制函数不等于获得完整控件。

## 可直接复用的绘制部分

| 当前节点 | 宿主接口及实现 | 已有实际调用 | 当前模型遗漏与边界 |
| --- | --- | --- | --- |
| button | `ButtonSpec` → `Painter::DrawButton`；`fluent_components.cpp:894` | `ui_settings_core.cpp:122`、`:326` | `kind` 的 Standard/Primary/Danger/Transparent/Toggle/TransparentToggle、glyph、icon_only、drop_down、bordered、keycap 均有现成语义，当前仅 text/command 无法表达。hover/press/focus/check 动画应由宿主生成，不能由清单伪造。 |
| checkbox | bounds、text、`ControlState` → `DrawCheckBox`；`fluent_components.cpp:1218` | `ui_settings_core.cpp:556` | 可复用复选框绘制；不能以它冒充开关。`DrawSwitch` 是另一项现有组件，实际调用在 `ui_settings_core.cpp:117`、`:554`，当前没有对应节点。 |
| text | `DrawText`、`DrawWrappedBody/Caption/Section` | `ui_settings_core.cpp:113-114`，`ui_settings_view.cpp:85` | 当前 text 无法指定宿主已有的正文/说明/节标题、换行、对齐语义。应映射现有文字角色与测量函数，不能全部固定为同一字体高度。 |
| progress | `ProgressSpec` → `DrawProgressBar` / `DrawProgressRing`；`fluent_components.cpp:1955`、`:2001` | `ui_settings_packs.cpp:311`、`ui_settings_view.cpp:1320` | 现有 spec 有 indeterminate、paused、error、animation_progress；当前仅 0–100 数值无法表达。bar 与 ring 也不是同一外观。百分比转换到宿主归一化数值应集中处理。 |
| list 的行底板 | `ListRowSpec` → `DrawListRowBackground`；`fluent_components.cpp:1771` | 同文件 `:3064` 的文件行绘制组合 | 只复用了行背景，没有完整通用列表控件。现有 spec 有 cut/drop_target，文字、图标与行内容由上层组合；当前 options 字符串列表不能声称等价于文件列表。 |

## 必须通过现有机制适配的组件

| 当前节点 | 现有组合与真实入口 | 所需适配，不能替换成系统外观 |
| --- | --- | --- |
| textInput | `TextFieldSpec` → `DrawTextField`；设置搜索在 `ui_settings_view.cpp:51-70`。`edit_host.h` 提供 `CreatePulsePaintedChildEdit`、`DrawPulsePaintedChildEdit`、`SynchronizeChildEditBackend`；真实创建在 `app_hosted_edit.cpp:522`、`folder_picker_dialog.cpp:1769` | EDIT 保留输入、IME、选区语义，Pulse 负责画面。不能直接显示默认系统 EDIT 作为成品。现有字段包含 placeholder、leading/trailing glyph、trailing badge/keycap、hosted_edit、suppress_text；当前 text/state 只表达标签和值，覆盖不足。 |
| select | `ButtonSpec` 下拉按钮与 `FluentMenuItem`/`FluentMenu` 组合；`settings_ui.cpp:45-64` 的 Dropdown 调用 `TrackDropdown`，并按真实按钮矩形定位 | 应复用已有浮层、主题、选中标记、键盘选择及滚动。不应另造系统 COMBOBOX。当前 options 缺少已有 item 的 command、disabled、shortcut、glyph、分隔、子菜单等语义；不能宣称覆盖完整菜单。 |
| tabs / tab | `TabSpec` → `DrawTab`，另有 `SegmentedItemSpec` → `DrawSegmentedTrack`/`DrawSegmentedItem` | 本次搜索确认 DrawTab 的公开声明和实现，未确认业务调用，不能写成已接入现有业务标签控件。实际设置分段选择调用在 `ui_settings_core.cpp:139-149`。标签页和分段按钮须按各自真实语义选择；当前 tab 缺 glyph/show_close，关闭命令和焦点管理也不在模型中。 |
| scrollView | `ScrollbarSpec`、`ScrollbarThumbRect`、滚动辅助函数、`ScrollbarFade` 与 `Painter::DrawScrollbar` | 设置页 `ui_settings_view.cpp` 的 `MakeSettingsScrollbar` 用 viewport/content extent、展开进度和 opacity；内容滚动与裁剪另由页面管理。根部统一滚动可适配，嵌套独立视口并无可直接套用的通用组件，当前禁止嵌套不能被表述成已支持任意滚动树。 |
| group | 设置页的卡片背景、标题、说明、图标及操作组合；`ui_settings_core.cpp:31-117` | 没有发现名为 GroupSpec 的通用设置组模型。应抽取现有卡片组合的公共表示与几何，而不是用系统 GROUPBOX 近似。当前 text/children 无法表达标题说明、图标、辅助提示与尾部操作。 |
| divider | 设置页使用 theme.stroke_divider 与实际几何绘制分隔线，例如 `ui_settings_view.cpp:78-80` | 本次未发现独立 DividerSpec。可复用宿主颜色与画笔，但方向、厚度、内缩必须有明确布局来源，不能凭空宣称有现成完整控件。 |

## 布局的真实来源

`SettingsLayout` 位于 `ui_renderer_internal.h:2577`；`MakeSettingsLayout` 实现在 `ui_settings_layout.cpp:11`，同时供绘制和命中测试使用。它是设置页的具体几何结果，不是通用 stack/grid 布局树。页面使用窗口宽度分支、测量按钮和换行文字、内容高度与导航宽度共同计算布局；例如导航在 760/1000 DIP 附近切换宽度。

`settings_layout_sections.h` 使用 `Painter::MeasureButtonWidth`，`ui_settings_core.cpp` 使用正文和说明测量，现有 `Painter` 与 compositor 提供字体、比例和主题。扩展 stack/grid 必须作为这些机制之上的布局适配，不能说它们已经是宿主已有组件。固定列数和 gap 不足以覆盖设置页的响应式换行、文字测量、尾部动作保留空间、不同密度及焦点可见区域。

组件标识方面，`HitTestResult` 是宿主真实交互标识体系，例如 SettingsFind、SettingsSearchClear。扩展节点 id 是扩展实例身份，不应伪装成现有组件 ID；需要明确映射到宿主的命中、焦点与动作表。宿主绘制 spec 不携带清单式组件 ID。

## 当前节点集真正缺少的现有能力

公开接口还包含 `SplitButtonSpec`、`SegmentedItemSpec`、`BadgeSpec`、`TagSpec`、`InfoBarSpec`、`StepSliderSpec`、`SidebarItemSpec`、`BreadcrumbSegmentSpec` 等。设置页实际使用 Badge（`ui_settings_core.cpp:204-206`）、InfoBar（`:488-498`）、分段控件及 Switch。这些不能用 text/button/group 的名称集合就声称已完整表达。

这次核对没有发现可以直接暴露的完整通用虚拟列表、嵌套滚动视口或声明式布局引擎。它们是当前通用机制缺失或尚未证实的能力，不是可以靠更名获得的现有组件。后续契约应逐项列出真实宿主 spec、可安全公开的语义、输入适配及验收证据；不得把系统 GDI 控件外观或新画的近似控件称为复用 Pulse 组件。

初次核对仅记录源码映射；后续视觉与交互证据见下文。尚未完成完整无障碍验收。

## 首轮契约对齐记录

现已在清单模型、严格解析和 schema 中加入与上述真实接口一一对应的有限语义：button.kind 支持 standard/primary/danger/transparent；textInput.placeholder；progress.kind 支持 bar/ring，status 支持 normal/paused/error/indeterminate；新增 switch，与 checkbox 共用布尔状态约束，绘制目标为现有 DrawSwitch。原有节点保持兼容。

这些是契约层已实现项，渲染与交互验收仍由宿主实现单独提供证据，不能据此宣布视觉完成。Toggle/TransparentToggle 尚未开放，因为它们需要明确的状态及动作语义。表格中的其他缺口仍然成立，尤其 glyph、keycap、InfoBar、Badge、分段选择和完整菜单项模型没有因此获得支持。

## 2026-11-03 共享实现验收

`extension_tree.cpp` 已直接使用 Pulse Painter、EditHost、FluentMenu、字体与滚动条命中/拖动辅助函数，撤掉独立 GDI 外观。stack/grid 是扩展布局适配，不能称作原有通用布局框架；横向堆叠支持窄窗口换行。

本次定向验证：原生树测试 79 项，真实宿主交互 25 项及 8 张应用内截图。独立参考绘制对照覆盖按钮、复选框、开关、下拉按钮、进度条，在明暗主题及 100%/150%/200% 缩放下 RGB 差异均为零（只忽略 DIB 传输的 alpha）。该结论仅限上述固定状态，不代表所有组件与状态都经过像素验证。

证据位于 `bench_data/ui-tree-shared-live-20261103-02/capture`、`bench_data/ui-tree-control-identity-20261103-02` 及 `build-extensions/ui-tree-delivery-native-test.log`。未执行真实系统高对比度切换、GPU 设备丢失注入及完整 IME/无障碍人工验收。虚拟列表、完整菜单、所有宿主控件与通用动态更新仍未开放；当前为有界 UI 树开发预览。

## 扩展组件契约与验收边界

最新支持范围由 `describe_extension_api.uiComponents.components` 从 Schema 逐项生成，包含完整字段约束、状态字段、命令字段；不要将历史表格的“当前遗漏”当作最终清单。命令副作用仍查 `capabilityDetails`，UI 组件不会获得额外执行权限。

| 组件族 | 复用目标 | 需要独立验证的行为 |
| --- | --- | --- |
| radio、slider、segmented | DrawRadioButton、StepSliderSpec、SegmentedItemSpec | 选择、步进、键盘与本地状态同步 |
| badge、tag、infoBar | BadgeSpec、TagSpec、InfoBarSpec | 语义样式、关闭命令、文本测量 |
| splitButton、menuButton | SplitButtonSpec、ButtonSpec、FluentMenu | 主动作和菜单动作分离、禁用项、子菜单 |
| sidebarItem、breadcrumb、emptyState | SidebarItemSpec、BreadcrumbSegmentSpec、EmptyStateSpec | 导航动作、当前项、空状态操作 |
| virtualList、tree、dataTable | 宿主列表行和滚动机制上的有界数据适配 | 可视行、树展开、列几何、选择和命令路由 |

这批契约和作者工具的通过结果不等于上述宿主交互均已通过。旧的像素比较与截图证据只覆盖当时的固定状态，不能外推到新增组件。后续原生测试和真实宿主验收应单独列出范围；无障碍、IME、系统高对比度和设备丢失仍不能由类型编译代替。

## 公开绘制 Spec 完整性清单

本清单以 `src/ui/fluent_components.h` 中公开的 `*Spec` 为检查源，按输入语义和适配责任分类；出现对应节点不代表每个内部绘制参数都应成为清单字段，也不代表视觉验证完成。

| 公开 Spec | 声明式入口或适配责任 |
| --- | --- |
| MotionSpec | 宿主管理动画曲线、计时，不接收清单动画对象 |
| ButtonSpec | button、menuButton；悬停、按下、焦点、菜单旋转由适配器管理 |
| TextFieldSpec | textInput；编辑句柄、IME、选区和文本抑制由 EditHost 管理 |
| TabSpec | tab；关闭动作引用显式命令 |
| MenuItemSpec | menu 项；菜单几何、绘制抑制、动画由 FluentMenu 管理 |
| ScrollbarSpec | scrollView 及数据视口；滚动范围、拇指几何和淡入淡出由宿主管理 |
| StepSliderSpec | slider；步进状态由输入适配器更新 |
| ListRowSpec | list、virtualList、tree、dataTable 行底板；可见行和命中由视口管理 |
| SidebarItemSpec | sidebarItem；动态图标资源不作为清单输入 |
| TrayCardSpec | trayCard；显示清单内摘要，不创建真实文件暂存任务 |
| ProgressSpec | progress；动画相位由宿主管理 |
| BreadcrumbSegmentSpec | breadcrumb；菜单和导航必须通过已声明动作 |
| CommandSearchBoxSpec | commandSearch；本地输入不会自动执行宿主搜索 |
| SplitButtonSpec | splitButton；主动作和菜单命中分别处理 |
| SplitterSpec | splitPane 的分隔拖动；坐标来自布局适配器 |
| SegmentedItemSpec | segmented；位置、共享底板由选项布局计算 |
| BadgeSpec | badge；语义样式与 dot，不开放任意内部画刷 |
| TagSpec | tag；仅公开约定颜色与边框语义 |
| SidebarSectionHeaderSpec | sidebarSection；展开状态由本地状态管理 |
| DriveSidebarItemSpec | driveItem；清单容量数据仅供显示，不查询设备 |
| PaneHeaderSpec | paneHeader；筛选、排序动作引用声明命令 |
| ColumnHeaderSpec | dataTable 列头；几何、命中及排序适配由宿主管理 |
| FileRowContentSpec | fileRow；清单字段仅供显示，不代表真实文件对象 |
| StagingTrayPanelSpec | stagingTray；显示状态不创建文件操作任务 |
| StagingItemSpec | stagingItem；操作仍使用受控命令 |
| StatusBarSpec | statusBar；显示字段不冒充宿主任务状态 |
| InfoBarSpec | infoBar；动作命中、背景和提示几何由宿主管理 |
| EmptyStateSpec | emptyState；宿主字体测量及布局 |

tooltip、titleBarButton 是公开 Painter 方法的适配入口，并非同名 Spec。bitmap、COM 对象、设备资源、原生句柄、ControlState 内部动画进度、私有绘制开关均不作为 JSON 输入。公开 enabled/visible 等语义由适配器转换为实际状态，不能伪造宿主焦点或权限。

## Spec 覆盖与 U1–U5 的区别

上面的公开 Spec 清单只约束源码入口映射，不能证明 U1–U5 框架完成。动态数据源的分页、过滤、排序、取消与代际隔离，表单校验和提交反馈，以及设置表单、集合浏览器、真实任务监控三类参考页面都需要单独实现和验证。旧静态目录、像素对照和节点数量不能代替这些工作流证据；新增实现的状态应以当前原生测试和真实宿主结果为准。
