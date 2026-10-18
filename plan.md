# Issue #303 实现计划：右键菜单新增「复制名称」

> 关联：jimmgreen/pulse#303（OPEN，2026-10-18）
> 标题：能不能文件菜单栏右键新增一个“复制文件名称”，现在只有路径，复制整个文件夹的文件名太不方便
> 附图：现有右键菜单只有「复制路径」一项。
> 分支：feat/#303-copy-file-name-in-context-menu｜基线：c4fe01e（release: 1.0.56）
> 状态：**仅分析与计划，未改动产品代码**（本文件除外）。

## 1. 需求理解

- 现状：列表项右键菜单只有「复制路径」(CmdCopyPath，Ctrl+Shift+C)，多选时逐行复制完整路径。
  用户要批量拿文件名（把文件夹里几十个名字贴进 Excel），得先复制路径再手工截掉目录部分。
- 期望：右键选中项 →「复制名称」，直接得到文件名；多选逐行。
- issue 评论（同一贡献者）已给出方向，本计划采纳：作为「复制路径」的兄弟项放在其上方；
  选中项复制所选文件名，空白处复制当前文件夹名；纳入「设置 → 右键菜单 → Pulse 菜单」的开关与拖动排序；
  实现只需 CmdCopyName（id 199）+ 一条 IDS_COPY_NAME + 一个 BuiltinMenuItem + 复用现有选择逻辑。

结论：**改动小、路径清晰，约 10 个文件、100 行以内**；主要成本在既有测试断言的同步更新。

## 2. 现有实现（已核实的调用链）

| 位置 | 作用 |
| --- | --- |
| `src/app/context_menu.h:28` `CmdCopyPath` | 内置命令 id；`CmdNewMarkdownFile = 198`、`CmdRecentBase = 200`，**199 空闲** |
| `src/app/context_menu.cpp:75-108` `BuildItemMenu` | 列表项菜单：打开 / 图标条 / 在新标签打开 / **复制路径(L97)** / 终端 / 属性 / 固定 / 标签 / 撤销 |
| `src/app/context_menu.cpp:321-341` `BuildBackgroundMenu` | 空白处菜单：新建 / 粘贴 / **复制路径(L328)** / 全选组 / 终端 / 固定 / 撤销 |
| `src/app/context_menu.cpp:289-297` `BuildRecycleItemMenu` | 回收站项菜单也有「复制路径」(L293) |
| `src/app/context_menu.cpp:110-133` `BuiltinItemForCommand` | 命令 → `BuiltinMenuItem`，决定该行能否隐藏/拖动排序 |
| `src/app/context_menu.cpp:219-249` `AppendWindowTransferCommands` | 「复制到窗口/移动到窗口」以 `CmdCopyPath` 为锚点插入 |
| `src/app/builtin_menu_items.{h,cpp}` | Pulse 自有行：稳定 key、文案、分面行表、`BuiltinMenuShared`、预设掩码、拖动排序 |
| `src/app/app_commands.cpp:108-120` `CopySelectedPath` | 逐行 `ClipboardPath(full)` + `ops::WriteClipboardText`，带 `DeferContentSelection` |
| `src/app/app_commands.cpp:1307-1317` `case CmdCopyPath` | 有选中→复制所选；否则复制当前文件夹路径（空白处语义） |
| `src/app/app_commands.cpp:1955-2010` `ShowItemContextMenu` | 应用 `ApplyBuiltinMenuPrefs(.., Item)`；回收站走 `BuildRecycleItemMenu` |
| `src/app/app_commands.cpp:2021-2104` `ShowBackgroundContextMenu` | 应用 `ApplyBuiltinMenuPrefs(.., Background)`，虚拟视图做 `EraseCommand` |
| `src/app/app_commands.cpp:2715-2808` `SelectionChipActions` | 工具栏多选胶囊由 `BuildFinderItemMenu` 生成，按 `kOrder` 排序后截断到 6 项 |
| `src/app/app_runtime.cpp:2958-2980` `EntryFullPath`/`SelectedFullPaths` | 选择→路径；空路径（历史变更记录等）被跳过 |
| `src/app/app_model.cpp:859-876` `TabTitle` | 路径→末级名称（虚拟视图给本地化名：回收站/搜索/此电脑…） |
| `src/common/string_ids.h`+`strings.rcinc`+`localization.h` | 文案三件套；当前最大 id 2732，**2733 空闲** |
| `src/common/strings_zh_tw.rcinc` | zh-TW 由 `tools/zh_tw/gen_zh_tw.py` 生成，仓库里**已 stale**（见 R4） |

补充：列表行刻意保留磁盘原名（含扩展名、`.lnk` 后缀），见 `src/ui/ui_renderer_internal.h:340-342` 注释。

## 3. 方案

### 3.1 先定四个语义（建议值）

1. **保留扩展名**：与列表显示一致（列表刻意不剥 .lnk/扩展名）；用户要“文件名”，扩展名通常有用。
   以后若要“只要主名”，再加一项而不是改这一项的行为。
2. **多选分隔符**：`\r\n`，与「复制路径」完全一致，可直接粘进 Excel/记事本。
3. **空白处语义**：复制当前文件夹名（`app::TabTitle(current_path)`）。真实文件夹得到末级目录名；
   虚拟视图分别得到本地化视图名，与「复制路径」复制 `pulse:recycle:` 属同一级别，不做特例。
   This PC（空路径）不写剪贴板，与现有「复制路径」一致。
4. **不做二级菜单（复制 ▸ 名称/路径）**：菜单还容得下一行，且开关+拖动排序已能精简；
   二级菜单会改动「复制路径」的既有依赖（快捷键、胶囊、窗口 flyout 锚点），收益不抵成本。

> 文案：中文 **复制名称**（标题写的是“复制文件名称”，但同菜单已有“复制路径”，对齐成“复制名称”更短；
> 若维护者坚持原字面，只改 strings.rcinc 一行）。英文 **Copy name**；zh-TW **複製名稱**
> （已用 opencc s2twp + glossary 验证：复制文件名称→複製檔名稱，复制名称→複製名稱）。

### 3.2 关键实现点

**A. 命令 id（`src/app/context_menu.h`）**

```cpp
CmdCopyName = 199,  // item / background: copy the selected names (folder name when none)
```

199 在 198 与 200 之间，不落进 `CmdTabJoinGroupBase(130)+32`、`CmdViewBase`、`CmdIndexBase(1000)`
等任何区间；`static_assert(CmdTabJoinGroupBase + 32 <= CmdRecentBase)` 与 `app_commands.cpp:1403`
的 `cmd >= CmdRecentBase` 分支都不受影响。`BuiltinMenuMnemonic` 不用改（返回 0 = 无访问键）。

**B. 文案（4 个文件）**

- `src/common/string_ids.h`：`#define IDS_COPY_NAME 2733`。
- `src/common/localization.h`：`CopyPath` 旁加 `CopyName = IDS_COPY_NAME,`。
- `src/common/strings.rcinc`：简体块 L423 `IDS_COPY_PATH` 旁、英文块 L1062 旁各加一行。
- `src/common/strings_zh_tw.rcinc`：L286 `IDS_COPY_PATH` 后**手工**补 `IDS_COPY_NAME "複製名稱"`。

**C. Pulse 菜单项（`src/app/builtin_menu_items.{h,cpp}`）**

- 枚举：`BuiltinMenuItem::CopyName` 紧跟在 `CopyPath` 之后。
- `kItems` 同位置加 `{ BuiltinMenuItem::CopyName, L"copy_name", l10n::StringId::CopyName }`；
  `static_assert` 与 `ItemsInEnumOrder()` 会强制两处顺序一致。
- `kItemRows`、`kBackgroundRows`：都在 `CopyPath` 行**上方**插入（图标 + 无快捷键）。
- `kSlimHidden`（“精简”保留列表）：加入 `B::CopyName`，与 CopyPath 同级。
- `BuiltinMenuShared` 自动为 true（两处都有）→ 设置页显示“两处共用”。

**D. 图标**：`kGlyphCopyName = L"\xE8C8"`（复制）。同菜单重复图标已有先例
（`PinNetwork` 复用 `kGlyphLink`，`context_menu.cpp:69`）。改一个常量即可换成别的隐喻。

**E. 菜单构建（`src/app/context_menu.cpp`）**

- `BuildItemMenu`：在 `CmdCopyPath` 之前（`OpenInNewTab` 之后）push `CmdCopyName`。
- `BuildBackgroundMenu`：在 `CmdCopyPath` 之前（`Paste` 之后）push。
- `BuildRecycleItemMenu`：同样加一行（回收站条目也有“原文件名”）；回收站空白菜单不加。
- `BuiltinItemForCommand`：`case CmdCopyName: return BuiltinMenuItem::CopyName;`
  （不加这行则该行无法被隐藏/拖动）。
- `AppendWindowTransferCommands` 锚点：**先找 `CmdCopyName`，找不到再退回 `CmdCopyPath`**，
  让「复制名称/复制路径」保持相邻。代价：`src/app/tab_transfer_test.cpp:609-624` 的 3 行断言要改。
  若要求零测试改动，可保留 `CmdCopyPath` 锚点，代价是两项被 flyout 隔开。
- 可选一行：命令面板 `context_menu.cpp:641` 旁 `add_cmd(CmdCopyName, …)`，与「复制路径」对称。

**F. 复制执行（`app_runtime.{h,cpp}` + `app_commands.{h,cpp}`）**

新增两个可单测的小函数：

```cpp
// 与 SelectedFullPaths 同一套过滤（EntryFullPath 为空的行跳过），保证“名称/路径”行集一致。
std::vector<std::wstring> SelectedEntryNames(const app::Tab& tab);
// 逐行 CRLF 拼接（复制路径/名称/托盘复制共用）。
std::wstring JoinClipboardLines(const std::vector<std::wstring>& lines);
```

`CopySelectedName(AppState& s)`：

```cpp
if (DeferContentSelection(s,[=](AppState& v){CopySelectedName(v);})) return;
app::Tab* tab = ActiveTab(s);
if (!tab) return;
auto names = SelectedEntryNames(*tab);
if (names.empty()) {
    if (tab->current_path.empty()) return;      // This PC：不写剪贴板
    names.push_back(app::TabTitle(tab->current_path));
}
ops::WriteClipboardText(JoinClipboardLines(names));
```

`DispatchMenuCommand` 在 `case CmdCopyPath` 旁加 `case CmdCopyName:`（选中/空白处都走它，
内部已回退到文件夹名）。**不需要**加进 `needs_files`：与 `CmdCopyPath` 现状一致，
`CopySelectedName` 自己做了 `DeferContentSelection`。

**G. 设置页/开关/拖动排序**：`app_runtime.cpp:834` 按 `kBuiltinMenuItemCount` 生成行，
`ui_renderer_internal.h:2457` 按 `BuiltinMenuOrderedRows` 排列，`settings_controller.cpp:1232` 按
`item - seen.size()` 反查枚举——**新增枚举后全部自动生效，不用改 UI 代码**。需目视确认列表与
预览高度自洽（两处都按行数累加，预计无碍）。

**H. 胶囊**：`SelectionChipActions` 直接读 `BuildFinderItemMenu`，新行自动成为候选；
在 `app_commands.cpp:2789` 的 `kOrder` 里把 `CmdCopyName` 放 `CmdCopyPath` 前。

## 4. 涉及文件清单

| 文件 | 改动 |
| --- | --- |
| `src/app/context_menu.h` | `CmdCopyName = 199` |
| `src/common/string_ids.h` | `IDS_COPY_NAME 2733` |
| `src/common/localization.h` | `CopyName = IDS_COPY_NAME,` |
| `src/common/strings.rcinc` | zh-CN + en 各一行 |
| `src/common/strings_zh_tw.rcinc` | 手工补一行 |
| `src/app/builtin_menu_items.h` | 枚举加 `CopyName` |
| `src/app/builtin_menu_items.cpp` | `kItems`/`kItemRows`/`kBackgroundRows`/`kSlimHidden` |
| `src/app/context_menu.cpp` | 3 个菜单构建 + `BuiltinItemForCommand` + 转移锚点（可选命令面板） |
| `src/app/app_runtime.h/.cpp` | `SelectedEntryNames`、`JoinClipboardLines` |
| `src/app/app_commands.h/.cpp` | `CopySelectedName`、`case CmdCopyName`、`kOrder` |
| `src/app/selftest_1b2.cpp` | 行数/顺序断言 + 新用例 |
| `src/bench/context_menu_identity_test.cpp` | key / 共用 / 预设 / 排序断言 |
| `src/bench/menu_mnemonic_test.cpp` | 无访问键断言 |
| `src/bench/localization_test.cpp` | 可选：三语可用 |
| `src/app/tab_transfer_test.cpp` | 仅改锚点时动 2-3 行 |

## 5. 测试计划

**必须同步的既有断言**

- `selftest_1b2.cpp:2059` `items.size() == 9` → 10。
- `:2065` `want` 在 `CmdOpenInNewTab` 之后、`CmdCopyPath` 之前插入 `CmdCopyName`。
- `:2081` `folder_items.size() == 10` → 11；`:2085` `[3]`→`[4]`（`[3]` 为 `CmdCopyName`）。
- `:2692` `kept_ids`、`:2756` `moved_ids` 加入 `CmdCopyName`。
- `:2708-2714` 隐藏 Select 组后 `copy_at + 1 == terminal_at` 仍成立（CopyName 在 CopyPath 之前）。
- `tab_transfer_test.cpp:613-624`：仅在改锚点时更新。

**新增用例**

1. `context_menu_identity_test`（纯逻辑）：`BuiltinMenuKey(CopyName)==L"copy_name"`；
   `BuiltinMenuShared(CopyName)`；精简预设保留；`BuiltinMenuOrderedRows` 中位于 CopyPath 之前且可拖首。
2. `selftest_1b2.cpp` TestMenuModel：列表项/空白/回收站项三个菜单都含 `CmdCopyName` 且文案非空。
3. 纯逻辑：3 行快照 `SelectRange(0,2)` → `SelectedEntryNames` 与 `SelectedFullPaths` 行数一致；
   把一行设为 `change_record_only` 后两边同时跳过；`JoinClipboardLines({"a","b"}) == L"a\r\nb"`。
4. `menu_mnemonic_test`：`BuiltinMenuMnemonic(CmdCopyName) == 0`。
5. 可选 `localization_test`：三语非空且分别为 复制名称 / Copy name / 複製名稱。

**构建与运行（定向，不跑全量）**

```powershell
call scripts\vcvars.bat
cmake --build build-ci --target pulse pulse_context_menu_identity_test pulse_menu_mnemonic_test pulse_localization_test
.\build-ci\pulse_context_menu_identity_test.exe
.\build-ci\pulse_menu_mnemonic_test.exe --pure-only
.\build-ci\pulse_localization_test.exe
```
```powershell
$env:PULSE_SELFTEST_CASE="pr-shell"; .\build-ci\pulse.exe --selftest   # TestMenuModel + TestShellMenuMerge
$env:PULSE_SELFTEST_CASE="tab-transfer"; .\build-ci\pulse.exe --selftest
```
```powershell
# 视觉核对：真实 Fluent 菜单渲染 PNG（列表项；PULSE_TEST_QUICK_MENU=1 为空白处）
.\build-ci\pulse.exe --menushot bench_data\menu-copyname.png
$env:PULSE_TEST_QUICK_MENU="1"; .\build-ci\pulse.exe --menushot bench_data\menu-bg-copyname.png
```

手动核对：单选/多选粘贴到记事本与 Excel；空白处；搜索/标签/回收站视图；深色浅色、100%/150%/200% DPI；
设置 → 右键菜单页的开关与拖动排序。

## 6. 风险与规避

| # | 风险 | 规避 |
| --- | --- | --- |
| R1 | 枚举中间插入使 `BuiltinMenuBit` 位移 | 持久化按字符串 key；仓库内唯一按数值设掩码的是 `PULSE_TEST_BUILTIN_HIDDEN`（仅测试实例），已 grep 无脚本/文档依赖 |
| R2 | 行数断言散落多处 | §5 已列全；改完先跑 `pr-shell` |
| R3 | 菜单变长 | FluentMenu 已有溢出滚动；高度断言按行数计算，自动一致 |
| R4 | zh-TW 生成文件在 HEAD 上已 stale（`gen_zh_tw.py --check` 现在就失败，整份重生成有 562 行无关 diff） | 只手工补一行；PR 里注明“生成器漂移为既有问题” |
| R5 | 名称与路径行集不一致 | `SelectedEntryNames` 复用 `EntryFullPath` 过滤 + 一致性用例 |
| R6 | `.lnk`/扩展名语义 | 明确“与列表一致、保留原样”，不做特例 |
| R7 | 虚拟视图空白处的“名称”是视图名（回收站/搜索） | 与「复制路径」同级行为；This PC 不写剪贴板 |
| R8 | 胶囊动作被挤出 | 既有降级策略（≤6 从尾部丢），只调 `kOrder` |
| R9 | 回收站名称是“原文件名” | 与“复制路径=原路径”一致，保留 |
| R10 | 新 id 落入错误区间 | 199 不在任何 base 偏移区间；既有 static_assert 不变 |

## 7. 实施顺序

1. 文案三件套 + `CmdCopyName`（先编译通过）。
2. `builtin_menu_items` + `context_menu.cpp` 行与映射（含转移锚点决定）。
3. `SelectedEntryNames`/`JoinClipboardLines`/`CopySelectedName` + dispatch + `kOrder`。
4. 更新既有断言 + 新增用例。
5. 定向构建 + 上述测试 + 两张 `--menushot` 视觉核对。
6. 手工冒烟：单选/多选/空白处/搜索/回收站，粘贴到记事本与 Excel。

## 8. 明确不做

- 不把「复制路径」改名「复制完整路径」。
- 不做二级菜单、不改 `Ctrl+Shift+C`、不顺带重构 `SelectedFullPaths`。
- 不改托盘/面包屑/详情面板已有的「复制路径」。
- 不整份重生成 zh-TW、不跑 `build_release.bat`、不跑完整 `--selftest`。

## 9. 待确认（可一次性回复）

1. 文案用「复制名称」还是与标题一致的「复制文件名称」？（建议前者）
2. 「复制名称」放在「复制路径」正上方（两个窗口 flyout 因此上移一格）？（建议是）
3. 空白处复制当前文件夹名（虚拟视图则复制视图名），可接受吗？
4. 是否同时给命令面板加一行（与「复制路径」对称）？
