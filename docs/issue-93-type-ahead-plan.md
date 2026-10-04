# issue #93：按字母键快速定位（type-ahead）实现计划

- Issue：https://github.com/jimmgreen/pulse/issues/93
- 标题：按键盘上的字母键会自动跳到当前文件夹中以此字母开头的文件
- 状态：`OPEN`、无 label、无维护者回复、无在途 PR（截至 2026-10-05，上游开放 PR 只有 #95 / #90 / #11）
- 基线：本地 `main` = `f2c3c95`（v1.0.52）

## 1. 需求还原

用户要的是 Windows 资源管理器的经典行为：

1. 焦点在文件列表时，按一个字母键 → 选中（并滚动到）**当前视图顺序**下第一个以该字母开头的条目；
2. 若同名前缀条目很多，**每按一次同一个键就按排列顺序往下跳一个**，到底后回卷到第一个。

原文：「若此字母开头的文件或文件夹较多，每按一次按键按排列顺序从上往下依次向下跳」。

关键点是**「按排列顺序」**——必须遵循当前视图的显示顺序（排序 / 分组 / 过滤后的顺序），而不是快照的原始顺序。

## 2. 现状核查

### 2.1 主窗口确实没有该能力

- `src/app/app_main.cpp:1390-1398`：`WM_KEYDOWN` 一路交给 `HandleKeyDown`；**没有 `WM_CHAR` 分支**。
- `src/app/app_input.cpp:3675` `HandleKeyDown(...)` 是一长串 `else if`：Tab 快捷键、Ctrl+B/T/K/P/F/…、F2/F5/F6/F7、方向键、PageUp/PageDown、Home/End 等。
- `src/app/app_input.cpp:3854-3856`：以上都不匹配时 `handled = false` → `DefWindowProcW`。**无修饰键的可打印字符直接落到这里，没有任何 type-ahead 分支**。
- 全仓检索 `typeahead|type-ahead|首字母|快速定位|键盘定位`：主列表侧零命中。

### 2.2 但仓库里已有一份可直接借鉴的实现（文件夹选择对话框）

`src/ui/folder_picker_model.h:55` + `src/ui/folder_picker_model.cpp:134-144`：

```cpp
int PickerTypeAhead(const std::vector<PickerEntry>& entries, int from, wchar_t ch) {
    const int count = static_cast<int>(entries.size());
    if (count == 0) return -1;
    const wchar_t wanted = static_cast<wchar_t>(std::towlower(ch));
    for (int step = 1; step <= count; ++step) {
        const int index = ((from < 0 ? -1 : from) + step + count) % count;
        const std::wstring& name = entries[static_cast<size_t>(index)].name;
        if (!name.empty() && std::towlower(name.front()) == wanted) return index;
    }
    return -1;
}
```

接入方式 `src/ui/folder_picker_dialog.cpp:775-781`：

```cpp
case WM_CHAR:
    if (wparam >= 0x20 && wparam != 0x7F && visual_.focus == kPickList) {
        const int next = PickerTypeAhead(visual_.entries, visual_.selected,
                                         static_cast<wchar_t>(wparam));
        if (next >= 0) Select(next);
        return 0;
    }
    break;
```

已有回归用例 `src/bench/dialogs_test.cpp:174-177`（循环 + 回卷 + 大小写 + 空表）。

**结论**：语义（`from` 之后第一个匹配、循环、回卷）与 #93 的描述完全一致，可以直接复用这套算法，主列表只需换掉「条目集合」和「选中」两步。

### 2.3 主列表侧要对接的既有 API

| 目的 | API | 位置 |
|------|-----|------|
| 当前窗格视图模型 | `ui::WindowViewModel vm = BuildVm(*s); vm.pane` | `src/app/app_input.cpp:3681` |
| 视图行数（含过滤） | `vm.pane.EntryCount()` | `src/ui/ui_renderer.h:255` |
| 视图行 → 源索引 | `vm.pane.SourceIndex(view_row)` | `src/ui/ui_renderer.h:262` |
| 源索引 → 视图行 | `vm.pane.ViewIndex(source)` | `src/ui/ui_renderer.h:281` |
| 取条目名 | `tab->EntryAt(index).name`（`fs::DirEntry`，**按值返回**） | `src/app/app_model.h:143`、`src/app/app_model.cpp:106-114` |
| 移动焦点 | `tab->MoveFocus(int index, bool extend)` | `src/app/app_model.h:222` |
| 滚动到可见 | `EnsureRowVisible(AppState&, app::Tab&, int index)` | `src/app/app_input.h:34` |

`EntryAt` 已覆盖两种数据源：内容搜索走 `content_results`，普通目录走 `snapshot`（`src/app/app_model.cpp:107-113`）。

## 3. 设计决策

### 决策 1：用 `WM_CHAR`，不用 `WM_KEYDOWN`

- `WM_KEYDOWN` 给的是虚拟键码（字母恒为大写 `A`-`Z`，标点符号是 OEM 码，随键盘布局变），且对中文 IME 组合过程无能为力。
- `WM_CHAR` 给的是翻译后的实际字符：数字/符号正确、IME 提交后的汉字也能拿到（对中文文件名定位是刚需，Pulse 本身就有 zh-CN / zh-TW 本地化）。
- 现有 picker 也是走 `WM_CHAR`。保持一致。

字符过滤沿用 `wparam >= 0x20 && wparam != 0x7F`（排除控制字符与 DEL），并额外排除 Ctrl/Alt 按下时的组合键产物。

### 决策 2：Phase 1 做成**无状态**单字符循环

只匹配**首字符**，不累积输入缓冲。理由：

- 与 #93 的原始诉求完全一致（「每按一次按键往下跳」）；
- 与已验证的 `PickerTypeAhead` 语义一致，可直接复用；
- 无状态 ⇒ 不需要超时计时器、不需要「何时清空缓冲」的一堆边界处理，改动面小、回归风险低。

多字符前缀累积（输入 `ab` 定位 `abc`）列为 Phase 2 可选增强。

### 决策 3：扫描视图行而非源索引，且避免逐行拷贝 `DirEntry`

- 必须按 `view_row` 遍历（0..`EntryCount()`），经 `SourceIndex` 拿源索引 —— 这样才符合「按排列顺序」，且天然兼容过滤、分组、任意排序列。
- **性能坑**：`EntryAt` 按值返回 `fs::DirEntry`（内含多个 `std::wstring`）。搜索结果视图可达 10 万行，每次按键逐行拷贝不可接受。扫描时直接读 `(*tab->snapshot)[src].name`（`snapshot` 是 `shared_ptr<const std::vector<DirEntry>>`，见 `src/fs/fs_snapshot.h:14`），仅在 `content_results` 分支才退回 `EntryAt`。

### 决策 4：无匹配时的行为

沿用 picker：**不动选中项**，但**仍然吞掉该字符**（`return 0`），避免 `DefWindowProcW` 发出系统蜂鸣声。不做任何弹窗或状态栏污染。

## 4. 落地步骤

### Phase 1（核心能力，建议先只做这个）

**Step 1 — 抽出通用匹配助手**

新增 `src/ui/type_ahead.h`（仅头文件，无 cpp）：

```cpp
#pragma once
#include <cwctype>
#include <string_view>
#include <cstddef>

namespace pulse::ui {

// 下一个名称以 prefix 开头的视图行；循环并回卷；无匹配返回 -1。
// name_at 接受视图行下标，返回该行用于匹配的名称。
template <class NameAt>
int NextPrefixMatch(int count, int from, NameAt&& name_at, std::wstring_view prefix) {
    if (count <= 0 || prefix.empty()) return -1;
    for (int step = 1; step <= count; ++step) {
        const int index = ((from < 0 ? -1 : from) + step + count) % count;
        const std::wstring_view name = name_at(index);
        if (name.size() < prefix.size()) continue;
        bool match = true;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::towlower(name[i]) != std::towlower(prefix[i])) { match = false; break; }
        }
        if (match) return index;
    }
    return -1;
}

} // namespace pulse::ui
```

> 用 `std::wstring_view` 传名，零拷贝；前缀长度 >1 的重载直接为 Phase 2 留好口子。

**Step 2 — 让 `PickerTypeAhead` 复用它（可选但推荐）**

`src/ui/folder_picker_model.cpp:134` 改为委托，行为逐字符等价（现有 `dialogs_test` 用例可原样保留作护栏）。
若不想动 picker（AGENTS.md 要求「不顺手重构无关代码」），可跳过本步，代价是两份相似逻辑。

**Step 3 — 新增主列表的 type-ahead 处理**

新增 `src/app/type_ahead.h` / `src/app/type_ahead.cpp`，导出：

```cpp
namespace pulse::app {
// 处理一次 WM_CHAR。返回 true 表示已消费该字符。
bool HandleTypeAheadChar(AppState& s, wchar_t ch);
}
```

内部流程：

1. **前置守卫**（任一命中则 `return false`，交回默认处理）：
   - `ch < 0x20 || ch == 0x7F`
   - `GetKeyState(VK_CONTROL) & 0x8000` 或 `GetKeyState(VK_MENU) & 0x8000`
   - `s.filterEditing`（筛选框编辑中，`src/app/app_state.h:478`）
   - `s.renameIndex >= 0`（内联重命名中，`src/app/app_state.h:499`）
   - `!s.tagRenameId.empty()`
   - `IsSettingsTab(tab)`（`src/app/app_navigation.h:84`）
   - `s.menu` 处于打开状态（Fluent 菜单自带筛选输入，别抢）
2. 取 `app::Tab* tab = ActiveTab(s)`；`ui::WindowViewModel vm = BuildVm(s)`。
3. `const int count = static_cast<int>(vm.pane.EntryCount()); if (count == 0) return true;`
4. `const int from = vm.pane.ViewIndex(tab->selected_index);`
5. 调用 `ui::NextPrefixMatch(count, from, name_at, std::wstring_view(&ch, 1))`，其中：

   ```cpp
   auto name_at = [tab](int view_row) -> std::wstring_view {
       const int src = /* vm.pane.SourceIndex(view_row) */;
       if (src < 0) return {};
       if (tab->content_results) return tab->EntryAt(static_cast<size_t>(src)).name; // 注意：返回的是临时对象
       if (tab->snapshot && static_cast<size_t>(src) < tab->snapshot->size())
           return (*tab->snapshot)[static_cast<size_t>(src)].name;
       return {};
   };
   ```

   > `content_results` 分支要小心：`EntryAt` 按值返回，不能直接返回其成员的 `wstring_view`（悬垂）。该分支先用局部 `fs::DirEntry` 接住再比较，或改用 `content_results->Get(src, row)` 拿引用。
6. 命中 `next_view`：

   ```cpp
   tab->MoveFocus(vm.pane.SourceIndex(next_view), false);
   EnsureRowVisible(s, *tab, tab->selected_index);
   ClampScroll(s);
   InvalidateRect(s.hwnd, nullptr, FALSE);
   return true;   // 未命中也返回 true：吞字符、不响铃、不动选中
   ```

   内容搜索进行中（`tab->search_content_active` / `search_live_generation`）时，参考 `src/app/app_input.cpp:3712` 的既有写法，经 `DeferContentSelection(s, apply)` 走延迟路径，避免与翻页/流式结果打架。

**Step 4 — 接入消息循环**

`src/app/app_main.cpp` 的 `WndProc`，紧邻 `WM_KEYDOWN`（约 1390 行）之前加：

```cpp
case WM_CHAR:
    if (s && app::HandleTypeAheadChar(*s, static_cast<wchar_t>(wParam))) return 0;
    break;
```

注意 `WM_SYSCHAR`（Alt+组合）已在 1386 行单独处理，不要牵连。

### Phase 2（可选增强，需与维护者确认后再做）

- **多字符前缀累积**：`AppState` 增 `std::wstring typeahead_text` + `ULONGLONG typeahead_at`；1.2s 内连续输入则拼接前缀，超时或按 Esc / 方向键 / 切换目录 / 快照变更则清空；Backspace 退格。语义需对齐资源管理器（前缀延长时停在原命中项，无匹配则保留旧前缀），**建议先与维护者确认期望行为**再动手。
- **输入反馈 UI**：Phase 1 无任何视觉提示，连按同一键时用户只能靠选中项移动感知。可选在窗格标题栏复用筛选行的视觉（`vm.pane.filter_text` / `filter_expand`，见 `src/ui/toolbar_layout.h:18`、`src/ui/ui_hit_test.cpp:645`）做只读提示条，或走状态栏 hint。**这属于产品设计决策，提交前必须先问维护者。**

## 5. 涉及文件

| 文件 | 改动 |
|------|------|
| `src/ui/type_ahead.h` | 新增：通用 `NextPrefixMatch` |
| `src/app/type_ahead.h` / `.cpp` | 新增：`HandleTypeAheadChar` |
| `src/app/app_main.cpp` | 新增 `WM_CHAR` 分支（约 1 处） |
| `src/ui/folder_picker_model.cpp` | 可选：委托到通用助手 |
| `src/bench/type_ahead_test.cpp` | 新增：纯函数回归用例 |
| `CMakeLists.txt` | 新增 `pulse_type_ahead_test` 目标（参照 `CMakeLists.txt:892-899` `pulse_link_pill_test` 的写法） |
| `scripts/build_release_ci.ps1` | 把 `pulse_type_ahead_test` 加进 `$testNames`（**文件第 39-46 行，不加则 CI 不会跑**） |
| `docs/issues-93.md` | 可选：与 `docs/issues-86-88.md` 对齐，写清范围与验证 |

## 6. 测试计划

### 6.1 单元测试（新增 `src/bench/type_ahead_test.cpp`）

纯函数，不需要窗口，覆盖：

- 从 `-1` 开始匹配到第一个；
- 已选中第 1 个匹配项时再按同键 → 跳到**下一个**匹配项（即 #93 的核心诉求）；
- 到末尾回卷到第一个；
- 大小写不敏感（`a` 命中 `Apple`）；
- 无匹配返回 -1；空表返回 -1；
- 空名称条目不崩溃；
- 中文名称按首字匹配（`文` 命中 `文档.docx`）；
- 前缀长度 2 的行为（为 Phase 2 预留）。

### 6.2 手工验证（隔离实例 `--test-instance`）

- 详细信息视图：按 `a` 跳到首个 `a` 开头项，连按在多个 `a` 项间循环；
- 大图标 / 中图标 / 图库视图：同样生效，且滚动到可见；
- **按「修改日期」排序**（非名称排序）时仍按显示顺序往下跳，而不是按名称顺序；
- 分组开启（按类型/日期分组）时跨组跳转顺序正确；
- 筛选框已激活（Ctrl+F）时不抢字符；内联重命名（F2）时不抢字符；
- 搜索结果视图、内容搜索结果视图可用；
- 10 万行目录（可用 `bench_data/` 生成）下按键无明显卡顿；
- 中文目录下用 IME 提交汉字后可定位；
- 设置页标签内按字母键无副作用。

### 6.3 构建与回归

按 AGENTS.md「只构建受影响的目标」：

```powershell
cmake --build build --target pulse
cmake --build build --target pulse_type_ahead_test
.\build\pulse_type_ahead_test.exe
```

另外跑 `pulse_dialogs_test.exe`（若 Step 2 改了 picker，必须过）与 `pulse_app_controllers_test.exe --layout-search-prefs`（涉及输入/焦点路径）。
**不要**默认跑 `build_release.bat` 或完整 `--selftest`。

## 7. 风险与未决问题

| 风险 | 说明 | 应对 |
|------|------|------|
| 上游抢先实现 | 1.0.52 刚抢先实现了 #88，本项目同样存在被上游先做的可能 | 开工前再查一次 issue 状态与上游近期提交；PR 保持 draft、hold submit |
| `EntryAt` 悬垂引用 | 按值返回 `fs::DirEntry`，取其成员的 `wstring_view` 会悬垂 | 扫描直接读 `snapshot`；`content_results` 分支用局部对象接住再比较 |
| 10 万行性能 | 逐行构造 `DirEntry` 会退化 | 见决策 3，只拷 `wstring_view` |
| 与筛选/重命名/菜单抢字符 | 多处编辑态并存 | 守卫清单见 Step 3.1；宁可漏（不生效）不可错（抢字符） |
| Ctrl/Alt 组合键误触发 | Ctrl+A 会产生 `WM_CHAR 0x01` | `ch >= 0x20` + 显式 Ctrl/Alt 守卫双保险 |
| IME 组合期间 | 组合过程不产生 `WM_CHAR`，提交后才产生 | 天然安全；但「拼音首字母定位」不在本期范围（那是 `docs/releases/1.0.30.md` 的搜索侧能力） |
| Phase 2 前缀语义 | 资源管理器与第三方管理器行为不完全一致 | 先问维护者，别自己拍板 |

## 8. 提交建议

- 分支：`93-type-ahead`
- subject 建议：`list: jump to the next entry matching a typed letter (#93)`
- PR 正文需写明：行为变更（新增键盘能力，无既有行为被改）、仅在无编辑态且无 Ctrl/Alt 时生效、测试清单与实际运行结果、未做视觉提示（Phase 2 待确认）。
- 上游**没有** PR 触发的 CI（`release.yml` 只认 tag `v[0-9]*`），因此 fork 侧需自行构建验证后再开 PR。
- 按既有约定：PR 默认保持 draft，等用户明确指示才转正式。
