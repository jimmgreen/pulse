# 打开 / 保存对话框跳转条：实现与验收记录

设计稿：`picker_jump_mockup.html`（顶部可切换状态，也可用 `?s=save|jump|newfolder|notepad|multi|folder|open|clash|idx`）。
原交接提供了界面外壳和数据接口。下方新增续做状态；后面的原始接口说明保留作设计依据，不代表仍全部待接。

## 继续完成设计稿（2026-11-12）

本节覆盖下方上一轮列出的缺口，旧表仅保留为变更前审计记录。

- **open**：后台合并 Pulse 持久化最近文件与当前程序历史，路径去重、按实际时间排序、按当前类型筛选、删除项剔除；遵守记忆开关。新记录保存独立精确次数，旧记录未知次数继续显示日期，不伪造累计值。缩略图缺失接入既有隔离预览宿主缓存，UI 线程不读文件属性、不调用第三方缩略图提供者。
- **save**：自动跳转使用独立 32 DIP 说明条和撤回入口；宿主字段标签/值分离；宿主真实提供的菜单在宽窗口放到底部工具位置，窄窗口保留原选项/更多入口，不虚构空菜单。
- **jump / idx**：显示当前结果分组数量、当前查询完成耗时；新查询不复用旧统计。保存模式索引包括符合当前格式的文件，Folder 模式仍仅目录。用户索引有说明及了解入口，标题不冒充全盘。地址栏有余量时显示“打字跳转 Ctrl L”，长路径优先完整使用空间。
- **newfolder**：复用列表行内编辑，编辑时只是内存草稿；确认后后台创建并进入，Esc/失焦撤销草稿，不提前创建默认名称目录。
- **notepad**：宿主组合框显示独立标签和值下拉框，仍复用原控件和 ItemSelected 通知。
- **multi / folder**：实际多选数量/大小、继续选择提示及类型图标；文件夹模式显示“只看文件夹”。主按钮仍尊重宿主真实标签。
- **clash**：具体编号新名字与独立“替换”放到冲突横幅；底部保存禁用等待明确决定，替换仍走再次验证及原 FileOk 流程。现有文件大小/时间取现有后台列表快照，没有数据时不猜。

### 定向验收
- 历史/查询模型：56 项 PASS、exit 0；含精确计数兼容、合并/去重/隐私、保存类型筛选、固定本地时钟相对日期，以及提前 Search 不得发布尚未加载的历史。日志 `bench_data/picker-jump-integration/completion-history-verified.log`。
- 真实窗口：context（新建确认/取消/同名冲突/进入、短路径提示）、hostmenu（UTF-8 实际切换 UTF-16，ItemSelected 精确 id 20；底部菜单原回调）、multi、recentthumb 均 FAILURES=0、exit 0；jump 的替换/FileOk 与文件结果选择诊断及连续三次复跑通过。
- 未缓存 PNG 实测 `RECENT_BITMAP_DRAWS=2`；独立 ThumbnailCache 复用隔离预览宿主，并使用已有 DrawWallThumbnail 按设计 cover 填满框。已重开真实绿色图截图，确认不是类型占位且无 contain 留白。
- 视图矩阵：420/640/1000 DIP × 100%/150%/200% × 浅/深/高对比，最新 options 定向检查 failures=0；包括独立说明条、选项 label/value、底部真实菜单及窄布局后备入口。重开真实新建、冲突、编码下拉、短路径提示和缩略图截图确认布局。
- 正式构建：pulse、pulse_picker_ui_proof exit 0，无本轮未修复编译警告；再次构建 `ninja: no work to do.`。两项测试 hooks 均 OFF，导出检查确认私有布局测试入口不存在。l10n 生成检查通过。

### 边界与保留的失败证据
- 一次 jump 文件选择瞬态失败未再复现；诊断版及三次连续复跑通过，保留最初日志，不宣称已确定原因。测试中的空目录 ready 判定、合法 Save 选项、受真实 Poll 覆盖的注入 fixture 已分别修正，仍精确检查最终状态/路径/回调。
- 一次 chronology fixture 的多次 Record 静默落盘未完整（磁盘缺 last-visited）；原失败文件保留。排序测试改为内存构造后一次原子写入并检查返回值、重开核对，不再把未写全的测试输入当排序错误；具体 OS 写失败原因未确定。生产另修复已定向复现的初始加载/提前搜索 revision 竞态。
- Tools 仅在宿主实际提供菜单时出现；未知旧历史次数不伪造；没有快照的旧文件信息不猜。不同第三方程序的自定义字段数量/内容仍由宿主决定，不拿固定 Photoshop/Word 假数据填界面。
- 当前正式产物在 `C:\Users\SS\Desktop\pulse\build-dialog-observability`。本轮未替换本机安装版或登记，未重新打安装包；真实第三方 Photoshop/Word/Notepad 全链不等同于自有 ABI 窗口用例。

## 本轮复核：Edge、扩展名联动与打开截图（2026-11-12）

### 本轮修改
- Edge：远程宿主不再把“owner 必须属于调用进程”作为唯一条件；改为持有已认证调用进程句柄，核对双方用户、会话、完整性、进程创建时间及窗口身份。外部 owner 必须已经由调用方禁用，Pulse 只观察，不擅自启用或抢焦点。补充 `remote_owner_preflight` 可读失败阶段及双方 PID 日志。
- 手输扩展名：保存模式中，只有调用方提供唯一、具体且匹配的类型时才联动下拉框、类型编号与 TypeChanged；当前类型已匹配则保留，歧义/通配符/不完整后缀不猜。保留用户原文件名，宿主回调改名不递归反切。
- 截图细节：逐芯片完整 Alt 1 键帽；74 DIP 缩略图和独立文字槽；更多选项数字徽章；当前目录搜索占位；收藏组排在快速访问之前并去重。
- 真实历史详情：最近目录显示实际使用日期，文件卡显示真实目录与日期，来源标签明确为本程序；“上次”按最后使用时间选取，不把访问频率第一名冒充上次位置。最近文件按时间排序。现有历史格式保持不变，未从衰减分数伪造“12 次”。

### 本轮实际验证
- 扩展名模型 `pulse_file_save_model_test --infer-filter`：23 项 PASS，exit 0。
- 真实对话框 `jump_ui.exe --case=extension`：FAILURES=0，exit 0；覆盖 TypeChanged 回调改名、快速输入、相对路径、歧义后缀及最终 FileOk 路径/类型编号。已重开 `extension-sync.png`，确认 `child\final.PNG` 原文保留，顶部 Images 与底部 Images 同步，当前目录搜索提示正确。
- 历史与上下文 `pulse_picker_jump_context_test`：36 项 PASS，exit 0；新增最后使用时间与高频排名分离、真实历史元数据验证。
- 外部 owner：当前 CMake 测试 44 项通过；既有 foreign-only 测试 28 项通过。真实 Pulse 远程宿主的自有三进程对照：旧版 Validate 返回 0x80070005、Ready=0；新版 Validate 成功、Ready=1、显式 Close 返回 CLOSED，9 项通过，owner 禁用状态/焦点保持。该测试复现 Edge 的跨进程 owner 条件，不等同于已安装版 Edge 端到端验收。
- 视图定向测试：420/640/1000 DIP、100%/150%/200%、浅/深/高对比组合及原 31 张效果图通过；重开卡片、选项徽标和真实窗口截图核对。Painter 单独渲染图不包含完整嵌入列表，未冒充完整产品截图。
- 失败记录保留：工作区直接运行 owner fixture 的子进程/连接失败；复制相同二进制及依赖到独立 TEMP 后上述对照通过。初始 UI 断言忽略宿主初始化 TypeChanged，已改为 Ready 后精确事件增量；显式 Close 按 ABI 验证 CLOSED，而非用户取消的 CANCELLED。未放宽产品认证或删除失败断言。

- 最终关闭 `PULSE_PICKER_JUMP_UI_TEST_HOOKS` / `PULSE_WITH_TEST_HOOKS` 后，`pulse`、`pulse_picker_ui_proof` 与 owner 测试目标构建 exit 0；再次构建 `ninja: no work to do.`。DLL 无 `PulsePickerJumpLayoutTest` 导出。关闭钩子的最终真实宿主再跑三进程回归：9 项通过，exit 0。l10n 生成检查和本轮差异空白检查通过。
- 当前构建产物：`C:\Users\SS\Desktop\pulse\build-dialog-observability\pulse.exe` 与同目录 `PulsePickerUiProof.dll`。本轮未替换安装版、未改登记、未关闭用户 Edge/Photoshop，也未重新打安装包。

### 上一轮审计：当时尚未完成的内容（本轮续做状态见文首）
| 状态 | 仍有差距（不能标记为 1:1 完成） |
|---|---|
| open / 截图 | 尚未合并 Pulse 最近文件与当前程序历史；没有真实累计次数；日期目前使用系统日期格式而非“昨天 / 10 分钟前”；缩略图仅读取缓存，未命中显示类型卡。尚无真实 Photoshop 全链实测。 |
| save | 自动跳转仍主要为撤回按钮，缺完整说明条；作者/标签使用现有属性编辑面板，未实现设计稿标签/值分区外观及底部工具布局。 |
| jump | 缺分组结果数量、耗时/总数；保存模式索引只搜索目录，未展示设计稿示例中的同类文件结果。地址栏“打字跳转 Ctrl L”静态提示尚未接入。 |
| newfolder | 普通新建按钮走既有“先创建默认名称，再行内重命名”；不是设计稿“输入名字后 Enter 才创建并进入”。按查询名称创建并进入是另一条已实现路径。 |
| notepad | 编码等宿主控件功能已接，但外观仍是通用选项按钮/原控件，不是完整独立标签加组合框布局。 |
| multi | 缺总大小、Ctrl/Shift 提示、托盘文件类型图标；主按钮尊重宿主标签，未强制加数量。 |
| folder | 仍缺内容区“只看文件夹”胶囊。 |
| clash | 当前横幅为返回修改/改名保存，主按钮替换；HTML 则把具体编号新名字与替换放在横幅，并显示旧文件时间/大小。交互层级尚不同。 |
| idx | 目前仅连接成功的用户索引文字提示，缺独立说明条、了解链接、用户文件夹组及结果统计。 |

控件均复用既有 Painter、Badge、菜单、列表和编辑面板；不使用设计稿假文件、假次数填充真实数据。验证结果以本轮日志为准，下面保留上一轮记录。
## 续做状态（2026-11-11）

### 已接通
- A1–A3：后台获取真实发起程序名称/图标，合并 Pulse 打开的标签与 Explorer 窗口；按完整程序路径和打开/保存模式隔离历史。只在最终接受成功后记录，取消不记录。
- A4–A5：共享偏好控制自动跳转；调用方 `SetFolder` 的明确目录不被覆盖，`SetDefaultFolder` 经新 ABI 能力协商才允许自动跳转；用户已开始操作时不再异步跳走。芯片显示完整路径提示。
- B1/B5：内存快照返回搜索结果，后台复用拼音和 IndexClient；查询代次隔离旧结果。只有真实连接到用户索引时才显示对应提示。索引 helper 从实际 Pulse 模块定位，版本目录先验证 bundle，不从第三方软件目录或 PATH 猜测。
- B2–B4：Ctrl+Shift+Enter 按输入名称创建并进入；Ctrl+Enter 等目录加载提交后走原保存/覆盖确认/FileOk 流程；文件结果进入父目录并选中精确文件。
- C1：底部最多四个常用选项，其余进入“更多选项”；复用现有 Painter 复选框、按钮、FluentMenu 和复杂属性编辑面板，不另造编辑控件。复杂字段展开后 Escape 收起，取消按钮仍取消整个对话框。
- C2：复用列表行勾选框，支持不按 Ctrl 连续多选；底部托盘逐项删除，不清掉其他选择。
- C3：最近文件来自本程序实际成功接受的文件，随文件类型过滤；已删除文件不展示。后台读取缓存缩略图，缓存缺失时使用扩展名卡片，不在 UI 线程调用缩略图提供者。
- C4：相对文件名包含缺失子目录时，先显示明确创建清单；取消不创建，确认后后台创建，再走原保存验证。遵守 PATHMUSTEXIST 等调用方限制，不预先创建目标文件。单独确认创建的目录在后续取消保存时保留。
- 布局：按设计稿接入标题 40、导航 46、跳转条 36 DIP 的全宽层级，侧栏默认 208 DIP；新建文件夹与类型筛选位于内容区工具行，类型顺序不随点击跳动；最近文件条位于工具行下、列头上。保留用户已保存侧栏宽度；主程序及旧嵌入模式布局不被这个开关改变。

### 已运行的定向验收
- `pulse_picker_jump_context_test`：29 项 PASS，exit 0；历史隔离、取消不写入、自动跳转保护、拼音/旧查询隔离、最近文件过滤、策略与 helper 定位边界。
- 真实窗口 `jump_ui.exe --case=options|multi|create|save|jump|complex|context`：七组分别 FAILURES=0、exit 0。覆盖真实点击、覆盖确认、一次 FileOk、相对目录创建/取消和文件结果选中；仅操作测试自己的窗口及 fixture。
- 新布局测试导出返回 0；旧布局不变、全宽导航绘制/命中一致、最近文件条不覆盖侧栏。
- `pulse_file_picker_view_test --options`：420/640/1000 DIP、100%/150%/200%、浅色/深色/高对比组合通过；原 31 张效果图检查通过。已重开真实选项、多选、覆盖确认截图查看。
- 保存目录模型 `--create-directories`：17 项通过；`--path-targets` 通过。文件对话框包装器 provenance、cancel-label 定向测试各 97 通过、0 失败。
- 修复真实截图暴露的属性面板黑色空隙、覆盖按钮截断；新建按钮使用现有异步创建与行内重命名，顶部类型筛选与底部过滤器/实际列表同步。最终截图已重开确认。
- `python tools/l10n/gen_strings.py --check`：通过。新增文案只从 catalog 生成。
- 最终 `pulse` 与 `pulse_picker_ui_proof` Release 构建 exit 0，无本轮编译警告；再次构建确认无需重编。`PULSE_PICKER_JUMP_UI_TEST_HOOKS` 与全局测试钩子均为 OFF，DLL 导出检查确认私有测试入口不存在。产物位于 `build-dialog-observability`，尚未替换安装版。

### 验证边界 / 未宣称完成的事项
- 真实窗口验收不是所有第三方软件兼容性保证，也不是所有设计稿状态的像素级截图比对。属性文本/单选组复用原展开面板；代表性文本、radio 和标量属性已验证真实编辑回调、Escape 收起及取消，但尚未对每一种第三方自定义控件组合做实际窗口验收。
- 旧短生命周期 `picker_host_mode` 未增加接受历史的后台落盘，避免进程提前退出造成虚假记录成功；现代进程内 V1/V2 已接入。
- 索引 helper 定位边界已测，未卸载本机服务去强制做无服务全盘索引实测；缓存缺失缩略图以文件类型卡片降级。
- 早期隔离桌面的 DComp occluded/error1627，以及一次组合测试的 save error5 已保留日志；独立和最终对应实测通过，未臆测早期 error5 根因。
- 本轮没有改安装登记或替换正在运行的用户软件。私有 UI 测试钩子只用于验收，正式构建关闭。

证据目录：`bench_data/picker-jump-integration`、`bench_data/picker-jump-actions`、`build-picker-jump-visual`。

## 原始交接说明
## 一、已完成（界面）

| 位置 | 内容 |
|---|---|
| `src/ui/folder_picker_view.h/.cpp` | 标题栏：程序图标 + 程序名 + 竖分隔线 + 动作（另存为/打开）。没有程序名时恢复原样式，图标改成绘制的矢量图标，不再用字体字符 |
| 同上 | 地址栏下的“已打开”条：`已打开 [Alt+] [1 季度报告] [2 …]`，资源管理器窗口用窗口图标；空间不够时先去掉来源文字，再丢掉放不下的项。条右侧依次为“回到 X”（撤回自动跳转）和带快捷键的“新建文件夹 Ctrl+Shift+N” |
| 同上 | 底部目标行拆成三段绘制：`将保存到：` + 文件夹（正文色）+ 文件名（次要色），前面有文件夹图标 |
| 同上 | 同名冲突：原来的“返回修改 / 改名保存”按钮移进红色横幅（警告图标 + “目标名称已存在 …”），按钮行只留“取消 / 替换”。**行为和快捷键都没变** |
| `src/ui/ui_renderer.h` | `SetEmbeddedBand(dip)`：嵌入模式下在地址栏和窗格之间留出一条空带，由宿主（选择器）自己绘制 |
| `src/ui/folder_picker_dialog.h/.cpp` | 新的 `FolderPickerSpec` 字段（见下节）。`SyncJumpStrip()` 把它们转成界面数据；点芯片或按 Alt+1..9 跳转（`JumpToOpenFolder`）；“回到 X”（`UndoAutoJump`，只提供一次）；“新建文件夹”调用现有的 `NewFolder()`（和 Ctrl+Shift+N、F7 相同）；程序图标 HICON 转成 D2D 位图（`AppIconBitmap`，按设备缓存） |
| `tools/l10n/catalog.tsv` | 新增 `PickerWillSave`（将保存到：）、`PickerOpenFolders`（已打开）、`PickerUndoJump`（回到 {name}）、`PickerExplorerSource`（资源管理器），已重新生成 |
| `src/bench/file_picker_view_test.cpp` | 新增离屏效果图 `jump_{light,dark,hc}{,_conflict}.png`，以及“已打开”条的布局和点击检查 |
| `src/bench/file_picker_dialog_test.cpp` | `--save` 场景填了示例数据（程序名、3 个已打开文件夹、自动跳转来源），用于截图检查 |

## 二、数据接口（`FolderPickerSpec`，全部可选，没填就不显示）

```cpp
std::wstring host_app_name;            // 发起对话框的程序名，例如 "Microsoft Word"
HICON host_app_icon = nullptr;         // 程序图标；Show 返回前调用方负责保持有效，之后由调用方销毁
std::vector<PickerOpenFolder> open_folders; // 先列 Pulse 标签，再列资源管理器窗口；顺序就是 Alt+1..9
PickerOpenFolder last_location;        // 这个程序上次保存 / 打开的位置；path 为空表示没有
std::wstring last_location_source;     // 芯片上的小字，例如 "Word 上次"
std::wstring auto_jump_from;           // 宿主把 initial_path 自动换成了 Pulse 当前文件夹时，这里填程序原本给的文件夹（显示“回到 X”）
std::vector<PickerRecentPlace> app_recent_places; // 侧栏最上面的“{程序名} 最近使用”分组：{path, label, detail="9 次"/"今天"}，按次数排序
// open_folders 为空时，“已打开”条、Alt+数字和跳转面板会改用共享 Pulse 位置（pulse_places_）里标记为 open 的文件夹
std::vector<PickerRecentFile> recent_files; // 打开 / 选图片模式：“已打开”条下面的最近文件卡片 {path, detail="设计稿-v3\导出 · 10 分钟前", thumbnail=HBITMAP(可选，32bpp 预乘 alpha)}
std::wstring recent_files_caption;          // 例如 "最近的图片 · 来自 Photoshop 和 Pulse，不限文件夹"；空时用默认文案
// 卡片单击：把完整路径填进文件名框；双击：直接打开。没有缩略图时按扩展名画色块卡片。缩略图请在后台线程用 IShellItemImageFactory 生成
// PickerOpenFolder { path, label(空=取文件夹名), explorer(资源管理器窗口) }
```

## 三、需要你实现的逻辑

### A. 给接口填数据（在构造 spec 的地方）
构造真实对话框 spec 的地方有：`src/app/picker_bridge_model.cpp`、`src/app/picker_host_mode.cpp`、`src/picker_inprocess/picker_ui_bootstrap*.cpp`。先确认哪条路径对应其他程序的进程内对话框。

1. **程序名和图标**：取宿主进程 exe 版本信息里的 `FileDescription`，取不到就用 exe 名；图标用 `SHGetFileInfo(SHGFI_ICON|SHGFI_SMALLICON)` 或 `ExtractIconEx`。不要阻塞 UI 线程。
2. **已打开的文件夹**：直接复用 `src/app/dialog_jump.*` 的现有数据。`DialogJumpContext::folders` 里 `open == true` 的就是 Pulse 标签，Pulse 已有“对话框出现时向 Pulse 要上下文”的消息流程（`DialogJump::Update` / `SetContext`）。资源管理器窗口参考 `src/app/explorer_window_takeover.cpp` 里的 `IShellWindows` 枚举。**Pulse 没运行时只列资源管理器窗口，不能卡住或失败。**
3. **上次位置**：用 `src/app/jump_history.*` 的 `JumpHistory`，它已经按程序记录跳转并按频率排序。需要按“程序 + 打开/保存模式”取第一条，用户确认（FileOk）时也要记录。
4. **自动跳转**：`DialogJumpContext::auto_jump` 打开且 `current` 非空时，把 `initial_path` 设为 `current`，并把原来的 `initial_path` 放进 `auto_jump_from`。程序明确要求某个文件夹（`SetFolder` 而不是 `SetDefaultFolder`）时**不要**自动跳转。
5. 芯片的悬停提示（显示完整路径）还没做，需要在 `UpdateHover` 里加 tooltip。

### B. 跳转面板（界面已做，逻辑留空）
已做好的界面：
- `folder_picker_view.*` 里的 `PickerJumpPanel`、`LayoutPickerJump`、`DrawPickerJump`、`HitTestPickerJump`：分组标题、输入部分高亮、右侧标签和快捷键、底部按键提示，最后一行是新建文件夹。
- `folder_picker_dialog.cpp` 里的 `UpdateJump` / `JumpKey` / `ActivateJumpRow` / `CloseJump`：
  - 地址栏里输入名字（不是路径）就弹出面板。
  - ↑↓ 选择，Enter 进入，Tab 补全成路径，Esc 只关面板，点行直接跳转。
- 目前面板只会在 `open_folders` 和 `last_location` 里做子串匹配。

需要你接的逻辑：
1. `FolderPickerSpec::jump_search(query)`：返回 `PickerJumpResult` 列表，group 取值 1 = 本程序最近，2 = 收藏，3 = 全盘。它在每次按键时同步调用，所以只能读内存缓存。索引和拼音搜索复用 `dialog_jump`，放在后台线程算好再缓存。拼音匹配（如 jd → 季度）也在这里做；面板自带的匹配只是简单的子串匹配。
2. 新建文件夹那一行（Ctrl+Shift+Enter）：`ActivateJumpRow` 里标了 `TODO(logic)`。现在只会调用普通的 `NewFolder()`，需要改成用输入的名字直接建好并进入。
3. Ctrl+Enter（直接保存到这里）：`ActivateJumpRow` 的 `save_here` 参数还没用，现在的行为和 Enter 一样，只是进入该文件夹。
4. 结果是文件（`file = true`）时，要选中这个文件；现在只进入它所在的文件夹。
5. 没装 PulseIndex 服务时的“正在使用用户索引”提示行还没做（参考 `AboutIndexUser` 的文案），新文字要先加进 `catalog.tsv`。

### C. 设计稿里还没做的界面
0. 底部已经改成设计稿的样子：文件名一行，下面一行左边是“将保存到”、右边是按钮（底部高 100 DIP，有同名冲突时插入一行横幅，变成 144 DIP）。
1. 选项行：程序加的自定义控件（复选框 / 下拉 / 属性）要改成设计稿的胶囊样式，超过 4 个收进“更多选项”。现在仍是原来的 `DrawCustom` 面板。
2. 多选：行首勾选框和底部的已选托盘（现在是文件名框里的标签）。
3. ~~打开模式顶部的“最近的图片 / 文档”缩略图条~~：界面已做（`recent_files`），数据和缩略图待接。
4. 文件名里写 `子文件夹\名字.ext` 时，提示“将新建文件夹 …”并在保存时创建。

## 四、验证方法
- 离屏效果图：`cmake --build build --target pulse_file_picker_view_test`，运行 `build\pulse_file_picker_view_test.exe`，图片在 `build\file-picker-visual\jump_*.png`。保存到 `%TEMP%` 会失败，请用默认目录。
- 真实窗口截图需要带测试钩子的构建：`build_st`（`PULSE_WITH_TEST_HOOKS=ON`）。运行 `set PULSE_PICKER_TEST_SNAPSHOT=<png 路径>`，再运行 `build_st\pulse.exe --picker-dialog-test --save`。
- 已知的旧失败：`pulse_file_picker_view_test` 里的 “the summary never runs under the buttons” 断言比较的是整行宽的摘要矩形，这次改动之前就会失败，与本次无关。
- 进程内对话框 DLL：`cmake --build build --target pulse_picker_ui_proof`（本轮已确认能链接）。
- 改动会影响文件对话框契约测试（覆盖确认、自定义控件、多选），接逻辑后请运行 `src/bench/file_dialog_*_test.cpp` 中相关的测试。

## 五、约束
- 遵守 `AGENTS.md`：界面文字只改 `catalog.tsv` 后重新生成；图标用矢量绘制；不在 UI 线程上做阻塞的文件系统 / Shell / 索引操作。
- 对话框运行在其他程序的进程里：任何 Pulse 侧的数据都可能拿不到，界面必须能自然地缺省显示。




