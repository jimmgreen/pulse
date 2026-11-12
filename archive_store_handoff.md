# 扩展商店 / 7-Zip 重设计：交接文档

目标：把 `archive_store_mockup.html` 的效果完整做进 Pulse。设计稿是验收标准，用浏览器打开对照（`?view=store|detail|menu|installed&theme=dark&w=narrow` 等参数可切状态）。

另一个已完成的独立任务：活动标签方案 C（`tab_active_mockup.html`），代码在 `src/ui/ui_renderer.cpp` drawTab、`ui_compositor.*`（`TabActiveFormat`），已实机验证，无需再动。

---

## 0. 必读约定（踩过的坑）

- **图标必须用矢量路径绘制，不要用字体字符**（用户明确要求）。现有代码里商店和卡片用的 Segoe Fluent 字形（`L"\xE7B8"`、`L"\xE91B"`、`L"\xEA18"`、`L"\xE76B"` 等）都要换成绘制的图标，见第 3 节 C。
- 界面文字只改 `tools/l10n/catalog.tsv`，然后运行 `python tools/l10n/gen_strings.py`；禁止 `l10n::Pick`。本轮新增的 112 条（id 6454–6565，`Store*` / `Sz*`）已经生成好，直接用 `l10n::Get(l10n::StringId::Xxx)`。
- 本机 Bash 工具的 heredoc 会把 `\\` 吞成 `\`。写含反斜杠的 C++ 字符串一律用编辑器 / Edit 工具，不要用 shell 管道写代码。
- 工作区有大量用户自己的未提交修改，不要 revert，不要整理无关文件。
- 构建需要 VS x64 环境：
  ```
  cmd /c ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake --build build --target pulse"
  ```
  动手前先确认没有别人的 ninja 正在用 `build` 目录（`Get-Process ninja`）。
- 7-Zip 插件单独构建：`cmake --build build-extensions\7zip`。
- 实机测试脚本：`build-extensions\claude-live7z-run\relaunch.ps1 -Profile profile-N`（每次用新的 profile 名）。它会重编 pulse 和插件、打包、装进隔离的扩展目录，再以隔离配置启动 `build\pulse.exe`。测试文件在 `build-extensions\claude-live7z`。
- **测试目录不能放在 `%TEMP%` 下**：在那里，从当前环境启动的 Pulse 会遇到 staging 失败和 `store_lock_open:5`（拒绝访问）。
- 改了 `sdk/native/include/pulse/*.h` 就要重跑：
  ```
  python tools\extensions\check_native_abi.py generate
  python tools\extensions\check_native_abi.py check
  ```
  这两条需要在 vcvars 环境里跑。

---

## 1. 已完成并实机验证

| 内容 | 位置 |
|---|---|
| SDK：`PulseCommandDescriptor` 末尾追加 `detail` / `group` / `flags`（`PULSE_COMMAND_GROUP_SUBMENU`），定义 `PULSE_COMMAND_DESCRIPTOR_SIZE_V1`；ABI 快照已重新生成 | `sdk/native/include/pulse/commands.h`、`sdk/native/abi/*.json` |
| 宿主接受旧尺寸的描述符；detail 中 `{stem}` / `{base}` 按选中项解析 | `src/app/extensions/native/commands_adapter.*` |
| 右键菜单：插件分组标题、两行菜单项（detail）、可选子菜单；命令面板保持单行 | `command_contributions.*`（`ContextMenuCommandRows`）、`app_commands.cpp`（`AppendNativeCommandRows`） |
| 菜单两行样式（高度 +16 DIP；detail 参与宽度计算，上限 320 DIP） | `fluent_menu.{h,cpp}`、`fluent_components.{h,cpp}`（`MenuItemSpec::detail`） |
| 7-Zip 插件：detail 提示、分组、子菜单选项、每个命令可单独开关；设置格式 v4（兼容 v3）；设置页改用单选项；状态里提供 `formats_rw` / `formats_ro` | `extensions/7zip/{plugin,settings,text}.*` |
| 商店（旧版第一轮）：下划线式标签、通栏 feature 卡、底部信任说明和链接、卡片"设置"按钮跳到插件设置页（`RequestSettingsPagesPackage`）、页面 15 的"‹ 扩展商店"返回、商店图标乱码修复 | `settings_layout_sections.h`、`ui_settings_packs.cpp`、`extension_store_ui.cpp`、`settings_pages_host.*`、`ui_settings_view.cpp`、`ui_settings_layout.cpp`、`ui_hit_test.cpp` |
| 测试：商店布局测试新增 feature 卡用例并通过；`pulse_native_commands_test` 14/14 通过 | `src/bench/extension_store_layout_test.cpp` |

已知测试情况：
- `extension_store_layout_test` 中 "card fields stay contained and vertically separated" 在 HEAD 上就失败（测试过期），不要为了通过去改它。
- `pulse_menu_mnemonic_test` 有一项 "menu tests preserve the foreground window" 失败，当时我在操作窗口焦点，可能是干扰导致，需要在不动鼠标的情况下重跑确认。

---

## 2. 本轮写了一半、尚未编译的内容

本轮修改后**还没有编译过**，下一步先编译。

1. **宿主代用户读写插件设置页状态**（已写完）：
   - `ExtensionTreeView::CommitUserState`（`extension_tree.*`）
   - `ExtensionPanel::CommitTreeUserState`（`extension_panel.*`）
   - `UiAdapter::EmbeddedState / CommitEmbeddedState / ClickEmbedded`（`ui_adapter.*`）
   - `SettingsPagesAdapter::State / CommitState / Click`（`settings_pages_adapter.*`）

   宿主写入的值会以 USER 来源到达插件，插件自己校验、保存、重新注册命令。宿主不了解插件的存储格式。
2. **7-Zip 桥接**（已写完）：`src/app/extension_store_seven_zip.{h,cpp}`。
   - `FillSevenZipView`：读取插件页状态和菜单预览，填进 `view.seven_zip`。
   - `SetSevenZipState(key, value)`：key 为 `mode` / `location` / `format` / `level`（数字）、`remember`、`folder`、`placement0|1`、`cmd0..5`。
   - `BrowseSevenZipFolder`：调用插件自带的文件夹选择。
   - 菜单预览用 `CommandsAdapter::Preview`（新增），用样例选中项模拟。
3. **视图模型**（已写完）：
   - `extension_store_view.h`：`SevenZipView`、`SevenZipMenuRow`；`ExtensionStoreView` 增加 `category / search / detail / detail_tab / preview_kind / saved / seven_zip`；`ExtensionStoreItem` 增加 `id / category / license / source / released / history`。
   - `app_state.h`：`store_category / store_search / store_detail / store_detail_tab / store_preview_kind / store_saved_at`。
   - `FillExtensionStoreView` 已填充这些字段，并调用 `FillSevenZipView`。
4. **动作 id**（已写完）：`ui_view_model.h` 的 `PackAction` 70000 起，包括 `StoreChipBase`、`StoreQuick*`、`StoreDetail*`、`Sz*` 等。
5. **布局字段**（已写完）：`ui_renderer_internal.h` 的 `SettingsLayout` 增加 `store_search / store_chips / store_skeleton / store_failed* / store_packs / store_quick / sz`。
6. **新布局**（已写完，**未接入**）：`src/ui/settings_layout_store.h`，包含：
   - `store_view` 命名空间：分类、搜索匹配、官方增强包卡片模型 `Pack()`（复用 `pack_text` 和现有的 PackAction）、7-Zip 文案函数；
   - 新的 `LayoutSettingsStore`（商店首页）；
   - `LayoutSevenZipDetail`（详情页，四个标签加右侧预览）。

---

## 3. 剩余任务（按顺序）

### A. 接入布局

- 在 `settings_layout_sections.h` 删除旧的 `LayoutSettingsStore` 函数体（从 `// Shared geometry for drawing and hit tests.` 到 `float LayoutSettingsInstalled(` 之前），改成 `#include "settings_layout_store.h"`。
- `LayoutSettingsPacks` 的页面 8 分支（`y += caption_h(...StoreIntro...)` 之前）：如果 `vm.settings_store_view.detail && seven_zip.item >= 0`，直接 `return LayoutSevenZipDetail(l, vm, scale, y - 44*scale, painter);`，不排标签和简介。
- 标签文字从 `store_text::Tab` 改用 `store_view::Tab`（发现 / 已安装 / 管理增强包）。"已安装"后面加计数徽标：`view.installed.size()`，有可更新项时用强调色。

### B. 绘制（新文件 `src/ui/ui_settings_store.cpp`）

在 `ui_renderer.h` 声明 `DrawSettingsStore` 和 `DrawSevenZipDetail`。

- `ui_settings_packs.cpp` 页面 8 浏览分支整体改为调用 `DrawSettingsStore`；detail 模式调用 `DrawSevenZipDetail`。
- `ui_settings_view.cpp` 在页面 8 的 detail 模式下不画大标题和简介。

逐项对照设计稿（颜色用 Theme tokens，强调色用 `theme.accent`）：

- **搜索框**：圆角 7，`input` 底色，下边线 `input_line`；放大镜用绘制图标；文字由 F 节的 EDIT 负责。
- **分类标签**：高 30 的胶囊。选中项用强调色 9.5% 底色、40% 强调色描边、强调色文字加粗；悬停用 `fill_hover`。
- **feature 卡**（`store_quick.on`）：
  - 卡片底色叠加左上强调色渐变（已有写法，在 `ui_settings_packs.cpp` 的 feature wash 里）；
  - 圆角 14 的 60×60 图标块；名称用 section 字体，后面跟"已启用"绿色胶囊；
  - 第二行：认证图标 + "Pulse 官方" + `SzEngineFormats`（`{n}` = formats_rw + formats_ro 的数量）；
  - 右上角：全部设置（`c.settings`，标准按钮）和更新（`c.button`）。
  - quick 区域上方画一条分隔线。标题"常用设置"，右侧显示"改动会自动保存"，`saved` 为真时变绿色"已保存"。
  - 三行设置，行之间画分隔线：
    1. 右键"解压"的方式：组合框显示 `ModeTitle(z.mode)`，副标题 `StoreMenuShows` 填当前第一项菜单名；
    2. 快速压缩：格式和等级两个组合框（tar 时等级禁用，显示 `SzLevelTar`）；
    3. 记住本次输入的密码：开关。
  - 右栏（宽屏）或下方（窄屏）：浅底色区，标题 `StorePreviewArchiveCaption`，下面是迷你菜单（见下方"菜单预览"）。
  - `z.available == false` 时，quick 区域只显示 `SzLoading` 或 `SzUnavailable`。
- **官方增强包卡**（`store_packs`）：
  - 图标块用 `Pack().color` 加绘制图标；
  - 摘要最多两行，超出部分裁掉（`PushAxisAlignedClip`）；
  - 底部左侧：`meta`，或状态文字 `Pack().state`（已安装时显示绿色 ✓）；
  - 右侧：`button`（主按钮）和 `secondary`（"管理"，标准按钮）；
  - 下载中把 `progress` 画成 3px 进度条。
- **加载中**：`store_skeleton` 四块，`skel` 底色加横向流光（用定时重绘或静态都可以）；`store_state` 前面放一个转圈图标，后面"取消"。
- **加载失败**：`store_failed` 卡片：
  - 左侧红色底的断网图标；
  - 标题 `StoreFailed`，描述 `StoreFailedDesc`，`store_notice` 用等宽字体显示诊断信息；
  - 右侧"重试"（`store_refresh`，主按钮）和"下载源设置"（`store_failed_sources`）。
- **离线**：`store_notice` 用 warning 色横幅，文字 `StoreOfflineDesc`。
- **待重启**：`store_restart` 横幅。
- **社区为空**：`store_community_card` 用虚线描边，左侧 44×44 的拼图图标块，标题加 `StoreCommunityEmptyDesc`。
- **底部说明**：保持现状，盾牌改为绘制图标。
- **详情页 `sz`**：
  - 面包屑：‹ 扩展商店 › 名称。
  - 头部：72×72 图标块、26 号名称、副行、三个胶囊（第一个已启用为绿色）、开关加"已启用/已停用"、⋯ 按钮。
  - 四个标签用下划线式。
  - 每组是圆角 10 的卡片（`groups[i]`），`section_label` 是组标题。
  - 选项卡片（`mode_opts` / `location_opts` / `format_opts` / `placement_opts`）：圆角 8，左上单选圆点；选中项强调色描边加 9.5% 底色；标题加粗，下方说明文字。格式卡的标题用等宽字体显示格式名。
  - 等级：轨道加 6 个刻度点击区（`level_stops`），选中刻度文字用强调色，右上 `level_value` 显示当前等级名。
  - 文件夹：`folder` 是输入框（F 节的 EDIT），右侧"浏览…"。
  - 命令行：左侧 32×32 图标块、`CommandName(i)`、`CommandTag(i)`，右侧开关。
  - 格式页：格式名胶囊，可写的用强调色。
  - 关于页：键值行用 `item.license / source / history / version`。权限一行用 `SzAboutPermissionsValue`。
  - 右侧：预览卡（标题 `SzPreviewTitle`，压缩包/文件两段切换，`preview_menu` 迷你菜单）和结果卡（`SzResultSmart / Folder / Here`）。
- **迷你菜单**（feature 卡和详情页共用）：
  - flyout 底色、圆角 9、阴影；
  - 先画 "7-Zip" 小标题行，再按 `rows` 逐行画：图标、标题、detail 第二行；
  - `z.submenu` 时先画一行 "7-Zip ›"，下方缩进列出子项，左侧画 2px 强调色竖线。

### C. 矢量图标（用户要求，必须做）

新建 `src/ui/store_icons.{h,cpp}`：

```cpp
enum class StoreIcon {
    Box, Extract, ExtractTo, Compress, AddArchive, Test, Hash, Gear, Folder, File,
    ChevronRight, ChevronLeft, Check, Badge, Shield, Lock, Image, Camera, Film,
    TextSearch, Sparkle, Puzzle, Cube, Warn, Info, CloudOff, Refresh, Search, More, Download
};
void DrawStoreIcon(ID2D1DeviceContext*, ID2D1Brush*, D2D1_RECT_F bounds, StoreIcon, float stroke_dip);
```

- 路径数据直接取自 `archive_store_mockup.html` 顶部 `<svg><defs><symbol>` 的 path（16×16 viewBox，stroke 1.5，圆头圆角）。
- 实现一个只支持 M/L/H/V/C/A/Z（含相对坐标）的小型 SVG path 解析器，生成 `ID2D1PathGeometry` 后 `DrawGeometry`；用 stroke style 设置 round cap 和 round join。`circle` / `rect` 元素直接用 ellipse / rounded rect 画。
- 按 (icon, size) 缓存 geometry。
- 替换范围：
  - 商店卡片图标块：`extension_store_ui.cpp` 的 `IconGlyph` 改为返回 `StoreIcon`，`ExtensionStoreItem::icon_glyph` 改成枚举；
  - 官方增强包卡（`Pack().glyph` 换成 `StoreIcon`）；
  - 认证勾、盾牌、面包屑箭头、迷你菜单行图标（按 `SevenZipMenuRow::icon` 映射：EXTRACT→Extract，ARCHIVE→Compress，DEFAULT→File）、详情页行图标、搜索放大镜、失败卡断网图标。
- 真实右键菜单（`FluentMenuItem`）目前只支持字形。如果也要换成绘制图标，需要给 `MenuItemSpec` 增加一个可选的矢量图标 id，并在 `DrawMenuItem` 的图标列里调用 `DrawStoreIcon`。这一点和用户确认后再做。

### D. 命中测试

扩展 `settings_layout_sections.h` 的 `HitTestSettingsStore`：

- 浏览页：
  - `store_chips[i]` → `StoreChipBase+i`
  - `store_quick.mode/format/level/remember` → `StoreQuick*`
  - `store_failed_sources` → `StoreSources`
  - pack 卡 `button` → `Pack().action`（需 `action_enabled`），`secondary` → `secondary_action`
  - 保留原有的 item / settings / refresh / cancel / manage。
- 详情页（detail 时只测这些）：
  - `crumb` → `StoreDetailBack`
  - `tabs[i]` → `StoreDetailTabBase+i`
  - `enable` → `StoreDetailEnable`，`more` → `StoreDetailMore`
  - `mode_opts` / `location_opts` / `format_opts` / `level_stops` / `placement_opts` / `command_switch` / `preview_kinds` → 对应的 `Sz*Base+i`
  - `remember` → `SzRemember`，`browse` → `SzBrowse`
  - `history_rollback[1]` → `SzHistoryRollback`

### E. 动作（`extension_store_ui.cpp` 的 `ExtensionStoreAction`，在已有代码之前处理 ≥70000 的动作）

- **分类**：`s.store_category = i`。
- **快速设置组合框**：用 `ui::FluentMenuItem` 列出选项，`s.menu->TrackPopup(屏幕坐标, items)` 弹出（坐标由 `store_quick` 矩形加窗口客户区换算），选中后 `SetSevenZipState("mode" | "format" | "level", 数字)`。
- **记住密码**：`SetSevenZipState("remember", z.remember ? "false" : "true")`。z 的当前值用 `FillSevenZipView` 重新读，或者缓存上一帧的视图。
- **Sz***：对应 `SetSevenZipState`：
  - 模式 → `"mode"`，位置 → `"location"`，格式 → `"format"`，等级 → `"level"`；
  - 位置 → `"placement0"` 或 `"placement1"` = `"true"`；
  - 命令开关 → `"cmd<i>"` 取反。
- **每次写入后**：`s.store_saved_at = GetTickCount64()`。`TickExtensionStoreUi` 在"已保存"显示 1.6 秒到期时 `InvalidateRect` 一次。
- **SzBrowse**：`BrowseSevenZipFolder`。
- **SzPreviewBase**：`s.store_preview_kind`。
- **StoreDetailTabBase**：`s.store_detail_tab`。
- **StoreDetailBack**：`s.store_detail = false`。
- **StoreDetailEnable**：通过 `controller->management->Action(s, InstalledEnableBase|InstalledDisableBase + installed_index, installing)` 走现有的审阅和重启流程。
- **StoreDetailMore**：弹出菜单"卸载 / 回滚"，对应 `InstalledUninstallBase` / `InstalledRollbackBase + installed_index`。
- **SzHistoryRollback**：同回滚。
- **StoreSources**：等同 `StoreManage`（切到管理增强包标签）。
- **StoreRetry**：等同 `StoreRefresh`。
- **卡片"设置" / "全部设置"**：如果卡片是 7-Zip（`items[i].id == kSevenZipExtensionId`），改为 `s.store_detail = true; s.store_detail_tab = 0;`，不再跳到页面 15。其他插件保持跳页面 15。
- `settings_pages_host.cpp` 的 `SyncSettingsPagesHost` 只在页面 15 显示插件面板，不受影响。确认页面 8 的 detail 模式不会显示插件面板。
- pack 卡的动作是原有 PackAction 值，`ExtensionStoreAction` 返回 false 后由 `settings_ui.cpp` 的 switch 处理，不需要新代码。

### F. 输入框（搜索和固定文件夹）

- 参照 `src/app/ai_assistant_settings.cpp` 的 EDIT 用法（`CreateChildEdit`、`SetWindowSubclass`、`DefPresentedChildEditProc`、`EM_SETCUEBANNER`、`Place`）。
- 新建 `src/app/extension_store_edits.cpp`，提供 `SyncStoreEdits(AppState&, const ui::WindowViewModel&)`，在 `app_runtime.cpp` 调用 `SyncSettingsPagesHost` 的旁边调用。
- 渲染器在绘制时记录 `store_search` 和 `sz.folder` 的屏幕矩形，供 app 摆放 EDIT（参照 `SettingsAssistantField`）。
- 搜索：文字变化 → `s.store_search` → 重绘。
- 文件夹：回车或失去焦点 → `SetSevenZipState("folder", 文本)`。插件会校验，非法路径时插件的 `status` 状态是错误文案，显示在 `sz.saved` 的位置。
- 页面不可见时隐藏 EDIT，滚动时跟随位置。

### G. 测试与验证（只跑相关的）

- 更新 `extension_store_layout_test.cpp`：
  - 新布局下 feature 卡的 settings 是"全部设置"；
  - 新增断言：quick 区域矩形都在卡片内；不过滤时 pack 卡有 5 张；分类过滤生效；搜索不匹配时显示 `store_no_match` 或空卡；详情页各控件在 360 / 680 / 1080 三种宽度下互不重叠且在内容区内；命中测试返回正确的 `Sz*` 动作。
- 运行：`pulse_extension_store_layout_test`、`pulse_native_commands_test`、`pulse_menu_mnemonic_test`（复核那一项失败）。
- 实机（`relaunch.ps1`）逐项对照设计稿：浅色/深色；商店首页（加载中、在线、离线、失败）；分类和搜索；feature 卡三项设置改动后右键菜单立即变化；详情页四个标签；固定文件夹浏览和输入；子菜单和命令开关；启用/停用后的重启提示。
- 交付时附截图，说明哪些地方和设计稿不一致及原因。

---

## 4. 设计稿到代码的对应

- 颜色：`src/ui/FluentTokens.h` 的 `MakeTheme`。浅色 sheet `#F7F8F4`、title `#F1F3EE`、card 白；深色 `#202020` / `#1A1A1A` / `#272727`。强调色取用户设置（截图里是灰绿色 `#527D70`）。
- 圆角：卡片 10、feature 卡 12、图标块 11/14/17、选项卡 8、胶囊为高度的一半、迷你菜单 9。
- 字号：正文 13、说明 12–12.5、卡片名 14 半粗、页面标题 26–28 半粗。
- 设计稿里的示例数据不要做进去：社区"示例数据"卡片、格式数 57、FFmpeg 38 MB、校验值都是示例。
