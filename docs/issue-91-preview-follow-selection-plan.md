# Issue #91 实施计划：浮动预览窗口跟随列表选中项

- 目标 issue：<https://github.com/jimmgreen/pulse/issues/91>（`功能增强：打开预览窗口后，点击不同的文件预览内容跟着变化`）
- 基线 commit：`f2c3c95`（v1.0.52，本地 `main` = `origin/main` = `fork/main`，无落后）
- 本文档只做设计与验证计划，**不含任何代码改动**。

---

## 1. 问题确认（已读代码核实，不是猜测）

`Space` 打开的是独立浮动窗口 `pulse::ui::QuickPreviewWindow`（`src/ui/quick_preview_window.h:45`），
不是右侧详情面板。issue 截图描述的正是这个窗口。

### 1.1 现象的代码成因

浮动窗口显示的内容由 `AppState::quickPreview.item_` 决定，只有两个写入点：

| 写入点 | 位置 | 触发条件 |
| --- | --- | --- |
| `Show()` | `src/app/app_commands.cpp:2227` | `ToggleQuickPreview()` 首次打开 |
| `Update()` | `src/app/app_commands.cpp:2244`、`2310`、`2331` | 见下 |

`Update()` 的三个调用点全都以 **`quickPreview.item()` 里那个已显示的条目** 为锚点：

- `NavigateQuickPreview()`（`app_commands.cpp:2231`）：只在**预览窗口自己**吃掉 `WM_QUICK_PREVIEW_NAVIGATE`
  （预览窗内按 ↑/↓，`app_main.cpp:1761`）时调用。此时它先把焦点行 `MoveFocus` 到相邻行，
  再用**新的 `selected_index`** 造 item —— 所以「预览窗内翻页」是好的。
- `SyncQuickPreview()`（`app_commands.cpp:2295`）：只在**目录内容变化**时被调用
  （`app_navigation.cpp:1256` 刷新完成后、`app_navigation.cpp:1451` 文件通知后）。
  它先用 `QuickPreviewEntryIndex()` 按**路径**找回已显示的行；找到就只在 `mtime/size/attrs`
  变了时 `Update()`，找不到就按 `quickPreviewAnchorView` 邻接或关闭。
  **它读的是 `shown.path`，从不读 `tab->selected_index`。**

**结论：列表里点击/方向键换选中项时，没有任何代码路径会调用 `Update()`。**
换句话说 `SyncQuickPreview` 顾的是「同一个文件在磁盘上变了」，不是「用户换了目标」。
这与 issue 描述 100% 吻合。

### 1.2 为什么不能靠「再点一次空格」兜底

`ToggleQuickPreview()` 第一次是 `Show()`（`:2193` 只在 `visible()` 时 `Close()`），
所以复现步骤里用户必须先关再开才能看到新文件——issue 说的正是这个痛点。

### 1.3 顺带确认的相关事实（影响方案取舍）

- 预览窗**失焦不关闭**：`WM_ACTIVATE`（`quick_preview_window.cpp:2710`）只通知 handler 前后台，
  `WM_KILLFOCUS`（`:1167`）只处理查找框。所以「点回列表」后窗口还在，符合 issue 期望。
- `Update()`（`quick_preview_window.cpp:316`）已经做了完整的换文件清理：
  `++generation_` → `ResetView()`（含 `ResetPlayback()` 停旧视频、`waveform_.Reset()`）
  → `ResetAnimation()` → `handler_.Reset()` → `BeginVideo()`。
  **换文件的复用与取消机制已经存在，不需要新写。**
- 唯一的浪费点：`Update()` 里没有先比对路径，重复传入同一 item 也会整段重置
  （重置缩放/滚动/停止播放）。跟随逻辑必须自己挡住同路径，否则按住方向键会
  「每格都停一次视频」。`SyncQuickPreview` 现有代码是显式比对了 `modified/size/attrs` 的，
  跟随分支要照同样的严谨度写。
- 多选：`SelectedQuickPreviewItem()`（`app_commands.cpp:2187`）只取 `tab->selected_index`，
  与选区大小无关。跟随语义与它保持一致即可，不引入新概念。

---

## 2. 设计方案

### 2.1 语义定义

新增「浮动预览跟随选中项」：当浮动预览可见时，**活动标签页焦点行变化**即换预览内容。

| 情况 | 期望行为 | 依据 |
| --- | --- | --- |
| 单击另一行 | 预览换成新行 | issue 诉求 |
| 方向键上下移动 | 预览逐行跟随 | 与预览窗内 ↑/↓ 一致 |
| 焦点行不可预览（回收站/此电脑下的目录、虚拟位置） | **保持当前预览不变**，不闪黑、不关闭 | `QuickPreviewItemAt()` `:2151` 已返回 false 表示不可预览 |
| 目录刷新后原文件被删 | 沿用现有 `quickPreviewAnchorView` 邻接逻辑 | 已有行为，不改 |
| 预览窗口内翻页 | 沿用现有 `NavigateQuickPreview`，不重复跟随 | 焦点行本来就同步变了|
| 焦点行与已显示项同路径 | **跳过**，不重置视图 | 防止按住方向键反复停视频 |

「不可预览时保持不变」是刻意选择：`QuickPreviewItemAt()` 已经在 `:2156` 把
「此电脑 / 回收站 / 虚拟位置下的目录」判为不可预览，此时清空或关闭都是无谓的视觉抖动。

### 2.2 落点选择：单点集中 vs 分散挂钩

**采用：在 `Render()` 里集中同步一次，不在每个输入分支挂钩。**

理由：
- 选中项变更入口极多（`app_input.cpp` 里 `SelectOnly` 有 6 处、`MoveFocus` 4 处，
  加上 `SelectRange` / `ToggleSelect` / 快照 `RemapSelection` / 通知重排 /
  标签切换 / 搜索结果回填 / 内容搜索异步 resolve），逐个挂钩必然漏一个，
  且以后新增入口还会继续漏。
- 渲染循环本来就是「当前焦点行是什么」的唯一权威读取点；
  `BuildVm(s, probe_details)`（`app_runtime.cpp:2179-2196`）已经在里面做
  `detailsSelPath` 的同型判断——详情面板就是靠 paint 跟随选中项的。
  预览跟随复用同一模式，与既有架构一致。
- `BuildVm` 的 `probe_details=false` 路径（命中测试/输入）不得产生副作用，
  跟随逻辑必须挂在 `probe_details==true` 或 `Render()` 显式调用上——
  详情面板的注释（`app_runtime.cpp:1694-1695`）已明确这条规矩。

具体做法：
- 在 `app_commands.cpp` 新增 `void SyncQuickPreviewSelection(AppState& s)`，
  与现有 `SyncQuickPreview()` 并列（`app_commands.h` 加声明）。
- 它只做一件事：预览可见 → 取 `SelectedQuickPreviewItem()` → 路径/属性与已显示项相同则
  返回，否则 `Update()`。**不碰** anchor、不关闭、不管磁盘变化（那些仍归 `SyncQuickPreview`）。
- 在 `Render()`（`app_main.cpp:167`）里 `BuildVm(s)` 之后调用一次。
  必须在 `BuildVm` 之后：`ActiveTab(s)`/`selected_index` 要先被快照回填路径稳定下来，
  且 `BuildVm` 内部可能触发 `RemapSelection`（例如空目录回落到第 0 行）。

### 2.3 与 `SyncQuickPreview` 的职责切分

两个函数名字相近、都在同一个文件里，**必须写清边界**，否则以后必然互相覆盖：

| | `SyncQuickPreview`（现有，不改语义） | `SyncQuickPreviewSelection`（新增） |
| --- | --- | --- |
| 触发 | 目录枚举完成、文件通知 | 每帧 paint |
| 锚点 | `quickPreview.item().path`（已显示项） | `tab->selected_index`（当前焦点行） |
| 职责 | 同文件内容变了要重载；条目离开列表要邻接或关闭 | 焦点行换了要换内容 |
| 能否关闭窗口 | 能 | **不能** |

冲突点：两者可能在同一帧先后跑（刷新完成 → paint）。若刷新后 anchor 逻辑已把预览
切到邻接行，`SyncQuickPreviewSelection` 读到的 `selected_index` 也是同一行 → 路径相同 →
跳过，**天然不打架**。反序也安全：`SyncQuickPreview` 只在路径找不到时才动，
而跟随已把 `item_` 换成新路径、列表里就有这行，`QuickPreviewEntryIndex` 找得到。
实现时要用 `generation_` 递增顺序写一条注释说明这个互不覆盖的依据。

---

## 3. 实施步骤

每步都可独立编译/验证，按序推进。

### 步骤 1：抽出可单测的纯判定函数

跟随逻辑真正的难点是「哪些情况要跟随、哪些要跳过」，这部分应当从
`AppState` 里剥出来做成无副作用纯函数，便于用 `[PASS]/[FAIL]` 覆盖。

- 新建 `src/app/quick_preview_follow.h`（命名对齐既有 `src/ui/type_ahead.h` 的纯逻辑头范式）：
  ```cpp
  namespace pulse::app {
  // 浮动预览是否应改显示followed_item。
  // shown/followed 都传完整 QuickPreviewItem；不可预览时 followed.path 为空。
  enum class QuickPreviewFollow { Stay, Switch };
  QuickPreviewFollow DecideQuickPreviewFollow(
      const ui::QuickPreviewItem& shown,
      const std::wstring& followed_path,
      uint64_t followed_modified, uint64_t followed_size, DWORD followed_attrs);
  }
  ```
  判定表（全部要有对应用例）：
  | 输入 | 结果 |
  | --- | --- |
  | `followed_path` 为空（不可预览/无选中） | `Stay` |
  | 路径 == `shown.path` 且 mtime/size/attrs 都相同 | `Stay` |
  | 路径 == `shown.path` 但 mtime/size/attrs 有变 | `Switch`（同路径但内容变了要重载） |
  | 路径不同 | `Switch` |
- 该头**不引入 windows.h 之外的重依赖**，`QuickPreviewItem` 已在
  `src/ui/quick_preview_window.h` 内聚定义，可直接 include。

### 步骤 2：实现 `SyncQuickPreviewSelection`

- `src/app/app_commands.h`：加 `void SyncQuickPreviewSelection(AppState& s);`，
  放在现有 `SyncQuickPreview`（`:110`）声明旁边，注释写明上表的职责边界。
- `src/app/app_commands.cpp`：紧跟 `SyncQuickPreview`（`:2340` 之后）实现：
  1. `if (!s.quickPreview.visible()) return;` —— 不可见时零成本返回（绝大多数帧走这条）。
  2. `ui::QuickPreviewItem item; if (!SelectedQuickPreviewItem(s, item)) return;`
     —— 不可预览则保持不变（§2.1 表格第 3 行）。
  3. `if (DecideQuickPreviewFollow(s.quickPreview.item(), item.path,
     item.modified, item.size, item.attrs) == Stay) return;`
  4. `s.quickPreview.Update(item);`
  5. `s.quickPreviewAnchorView = -1;` —— 已显式换目标，清掉删除锚点，
     否则下一次删除后的邻接跳转会从过期行号起跳（现有 anchor 语义见
     `app_state.h:176-179`）。
- **`DeferContentSelection` 怎么办**：不能像 `ToggleQuickPreview`（`:2197`）那样把整个动作塞进
  `DeferContentSelection`——那会为每一帧排一个 job。但也**不能**拿
  `tab->content_action_ready` 当就绪信号（初稿如此，已在 PR #100 审查回合改正）：
  该字段只服务于「批量操作期间临时钉住跨页选中的行」，在 `CompleteContentSelection()` 内
  **置 true 后又在同一个函数内清回 false**（`content_results_ui.cpp:226` / `:231`），
  正常浏览时恒为 false。用它做守卫 → 内容搜索下无条件早退，预览整场都不跟随。
  行是否已分页由行自己回答：`EntryAt()` 在页未落地时返回 `change_record_only` 占位，
  `EntryFullPath()` 随即给空串，`SelectedQuickPreviewItem()` 判为不可预览 → 本帧保持，
  页落地后的下一帧自然跟上。**实现里写的是这层语义，不再引用 `content_action_ready`。**

### 步骤 3：挂到渲染循环

- `src/app/app_main.cpp` `Render()`（`:167`）：在 `ui::WindowViewModel vm = BuildVm(s);`
  （`:196`）之后、`s.renderer.Render(vm, ...)` 之前插入
  `SyncQuickPreviewSelection(s);`。
- 不放在 `BuildVm` 内部：`BuildVm` 会被 `probe_details=false` 的命中测试路径调用，
  在那里做 `Update()` 等于让鼠标移动触发预览重载。
- 顺带在 `Render()` 里加一句注释，指明跟随的输入是 `selected_index`，
  并说明它与 `BuildVm` 的 `detailsSelPath` 是同型机制。

### 步骤 4：确认不破坏现有删除锚点流程

`SyncQuickPreview` 的删除邻接（`:2316-2338`）是唯一会「主动换目标」的现有路径。
步骤 2.5 已让它在跟随分支里失效，因此需要复核：
- 预览内 Delete → `HandleQuickPreviewCommand`（`:2280`）先记
  `quickPreviewAnchorView` 再 `DeleteSelected` → 快照变化 → `SyncQuickPreview`
  邻接 → 同一帧 paint 的跟随读到相同路径 → 跳过。**行为不变。**
- 主列表 Delete（预览窗开着、焦点行被删）→ 快照把焦点移到邻接行 →
  跟随直接把预览切到新焦点行（比现有 anchor 逻辑更直接）→ 同样正确。

这一条要写成`步骤 5 的定向验证`里的一条实测项，不能只靠推理。

### 步骤 5：补测试

**5.1纯逻辑回归（新增 CI 门禁）**

- 新建 `src/bench/quick_preview_follow_test.cpp`，照 `src/bench/type_ahead_test.cpp:1`
  的写法（`main()` + `check(ok, "…")` 打印 `[PASS]/[FAIL]`、失败计数、非零退出码）。
- 覆盖 §2.1 判定表全部 4 行 + 边界：空路径、仅 attrs 变（只读/隐藏位翻转）、
  仅 mtime 变、同路径全同。
- `CMakeLists.txt`：在 `pulse_type_ahead_test`（`:900`）旁新增
  ```cmake
  add_executable(pulse_quick_preview_follow_test src/bench/quick_preview_follow_test.cpp)
  ```
  只include 头文件，无需链接额外库（对照 `:901` 的 `pulse_link_pill_test` 只链 `user32`）。
- `scripts/build_release_ci.ps1`：把 `pulse_quick_preview_follow_test` 加进
  `$testNames`（`:45-52`）。**必须加，否则本地绿、CI 不跑**——
  该数组是显式枚举，不在里面的目标不会被执行（现有 18 个 selftest 同理）。

**5.2 行为验证（headless selftest，可选但建议）**

- 在 `src/app/selftest_1b2.cpp` 加一个 `quick-preview-follow` 用例：
  走 `SetTimer` 驱动（范式见 `src/app/search_flow_test.cpp:24`），
  步骤为「选中 A → `ToggleQuickPreview` → 断言 `quickPreview.item().path == A`
  → `tab->SelectOnly(B)` → `Render(s)` → 断言 path 已变为 B」，
  再补一条「选中不可预览项（回收站下的目录）→ 断言仍为 B」。
- 同时要加进 `build_release_ci.ps1` 的 `$selftestCases`（`:94-97`），
  否则不会被执行。注意该数组里 `-native` 后缀是 LumaText 通道，本用例不需要。
- **限制说明**：该用例需要真实 `AppState` + 消息循环，属交互级验证；
  若实现阶段发现挂接点难以在 selftest 里稳定驱动，则退化为「纯逻辑用例 + 手动验证清单」，
  并在PR 里写明这一验证缺口（`AGENTS.md` 要求：不得把未运行的检查描述为通过）。

**5.3 手动验证清单**（写入 `docs/issue-91-manual-test-checklist.md`，格式对齐
既有的 `docs/issue-93-manual-test-checklist.md`）

| 场景 | 期望 |
| --- | --- |
| 单击另一文件 | 预览立即换内容，窗口不闪黑 |
| 方向键连续按住扫过一列图片 | 逐行跟随，无卡顿；松手后播放稳定 |
| 从视频文件切到下一个文件 | 旧视频立即停止，不残留声音 |
| 切到回收站 / 此电脑下的目录 | 预览保持上一项，不关闭、不清空 |
| 预览窗开着用方向键翻页 | 行为与改动前一致（不应双重切换） |
| 预览窗内 Delete | 仍邻接下一项，不跳错行 |
| 缩放/滚动到某图后切走再切回 | 缩放回到默认 fit（`ResetView` 语义） |
| 内容搜索结果里切换选中 | 预览**与普通列表一致地跟随**；短暂未分页的行本帧保持，不出现错位内容 |
| 深色/浅色主题下切换 | 无残留旧画面 |
| 快速连续点击 10 个不同扩展名 | 无崩溃、无窗口句柄泄漏 |

---

## 4. 影响范围与风险

### 4.1 改动文件清单（计划）

| 文件 | 改动性质 |
| --- | --- |
| `src/app/quick_preview_follow.h` | 新增（纯逻辑头） |
| `src/app/app_commands.h` | +1 声明 |
| `src/app/app_commands.cpp` | 新增 1 个函数（约 15 行） |
| `src/app/app_main.cpp` | `Render()` 内+1 行调用 |
| `src/bench/quick_preview_follow_test.cpp` | 新增测试 |
| `CMakeLists.txt` | +1 目标 |
| `scripts/build_release_ci.ps1` | `$testNames` +1 项（+可能的 `$selftestCases` +1 项） |
| `docs/issue-91-*.md` | 计划与验证清单 |

无`src/ui/quick_preview_window.*` 改动：换文件的重置与取消机制已完备
（`Update()` → `ResetView`/`ResetAnimation`/`handler_.Reset`/`BeginVideo`，
`generation_` 递增）。**这是本方案改动面小的关键。**

### 4.2 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 每帧 paint 调用引入开销 | 预览不可见时是 2 次分支判断，可见时多一次路径比较 | `visible()` 首行早退；纯判定是字符串比较，无 IO、无锁 |
| 按住方向键时视频反复启停 | 观感卡顿 | 纯判定挡同路径；跨路径切换是用户主动行为，且已有 `generation_` 取消 |
| 内容搜索异步分页导致显示错位 | 预览显示上一个文件 | 行未落地时 `EntryAt()` 给占位 → `SelectedQuickPreviewItem()` 判不可预览 → 本帧保持；**不早退整场**（初稿误用 `content_action_ready`，已改） |
| 与 `SyncQuickPreview` 抢同一份状态 | 预览显示错行 | §2.3 的职责表；两者都以 `item_.path` 为唯一真相来源，同帧必然收敛 |
| 焦点在别的窗格时误跟随 | 预览跳到非焦点窗格的文件 | 只读 `ActiveTab(s)`，语义与 `SelectedQuickPreviewItem` 一致 |
| 触碰渲染循环引发回归 | 启动/关闭流程异常 | 只加一行调用，不改`Render()` 其余顺序；按 §4.3 定向验证 |

### 4.3 定向验证（按 `AGENTS.md` 的范围要求，不跑全量）

只构建受影响目标、只跑相关检查：

```powershell
cmake --build build --target pulse                       # 主程序
cmake --build build --target pulse_quick_preview_follow_test
.\build\pulse_quick_preview_follow_test.exe               # 纯逻辑回归
# 若实现了 5.2 的 selftest 用例：
$env:PULSE_SELFTEST_CASE='quick-preview-follow'; .\build\pulse.exe --selftest
```

**不跑** `build_release.bat`、**不跑**完整 `pulse.exe --selftest`、**不跑**全部测试程序
（本次只碰预览跟随一条链路，无公共底层改动）。
`pulse_preview_test.exe --vector-only` 与 `pulse_playback_controls_test.exe --timeline-only`
可作为 `Update()` 复用路径的旁证，但它们覆盖的是预览渲染而非跟随行为，
不作为通过条件。

### 4.4 UI 变更声明

本次**不新增可见控件、不改布局、不改文案**，仅改变既有窗口的内容跟随行为，
因此不需要前后截图；但§5.3 的「深浅主题」「缩放状态」两行必须真机看过。

---

## 5. 提交与PR 策略（遵循本仓库既有规则）

- 本仓库对上游 PR默认 **hold submit**：先开 draft，等用户明确指示再转正式/合并。
- 分支建议：`91-preview-follow-selection`，从 `origin/main`（`f2c3c95`）切出。
- 提交信息用imperative + scoped subject，例：
  `preview: float quick preview follows the focused row (#91)`
- `gh pr create --head` 只能写两段 `owner:branch`（如 `yukitakasama:91-preview-follow-selection`）。
- PR 正文必须写：行为变化、风险、**实际跑过的检查**、验证缺口（5.2 若退化要显式说明）、
  并链接 `Closes #91`。
- 不新建 CI 触发（上游 `.github/workflows/` 只有 `release.yml`，PR 不会自动跑检查），
  门禁靠 fork 上已有的 `release.yml` tag 流程与本地定向测试。

---

## 6. 已完成 / 未完成

**已完成（本轮）**
- 核实 fork 与上游一致：`compare`返回 `identical / ahead 0 / behind 0`，无需同步。
- 读代码定位根因，确认 §1.1 的「没有 `Update()` 调用路径」结论。
- 确认 `Update()` 的清理与取消机制已完备，无需改 `src/ui/quick_preview_window.*`。
- 确认测试落点（新增 CI 门禁目标 + selftest 用例需手工登记进 `$testNames` / `$selftestCases`）。

**未完成（下一轮，需用户批准后开始）**
- 全部代码改动（步骤 1–4）。
- 测试代码与 CI 登记（步骤 5）。
- 手动验证与`docs/issue-91-manual-test-checklist.md`。
- draft PR。

---

## 7. 修订记录：PR #100 审查回合（2026-10-05）

上游 PR <https://github.com/jimmgreen/pulse/pull/100> 收到请求修改的审查意见，本轮据此修订。

### 7.1 阻断项：内容搜索下跟随失效（已修）

`SyncQuickPreviewSelection` 的守卫

```cpp
if (tab->content_results && !tab->content_action_ready) return;   // 已删除
```

用错了字段（语义见 §3 步骤 2 的更正说明）。实证：内容搜索里打开预览后，
直接调 `SyncQuickPreviewSelection()` 与经 `Render()` 两条路径都停在原行，
用户观感与 issue #91 描述的 bug 一致。

**改法**：删掉该行，改为依赖 `SelectedQuickPreviewItem()` 的自然兜底
（未分页的行 → `change_record_only` 占位 → `EntryFullPath()` 空 → 判为不可预览 → 保持）。
不引入 `content_results->Ready(selected_index)` 是因为 `Get()` 已先查 `content_action_rows`，
再加一层页级判断会把「行已可用但页判定未就绪」的情况一起压掉，反而更严于实际需要。

### 7.2 新增回归用例（已补）

`src/app/quick_preview_follow_ui_test.cpp` 增内容搜索段（真实 `ContentResultStore` + 真实 tab）：
seed 6 行 → 等页落地 → 在 `content_action_ready=true` 的状态下打开预览、
随后清回 false（复刻 `CompleteContentSelection()` 的时序）→ 断言
①预览开在焦点行，②直接跟随换行，③经 `Render()` 换行。原 8 例 → 14 例。

### 7.3 验证结果（本地 `build-ci`）

| 检查 | 结果 |
| --- | --- |
| `cmake --build build-ci --target pulse pulse_quick_preview_follow_test pulse_app_controllers_test` | 通过，无新增警告 |
| `pulse_quick_preview_follow_test.exe` | 8 passed, 0 failed |
| `PULSE_TEST_QUICK_PREVIEW_FOLLOW` 行为用例 | 14 passed, 0 failed（exit 0） |
| 反向验证：临时加回旧守卫重跑同一用例 | 如期失败（exit 1，仅两条内容搜索用例 FAIL） |
| 相邻：`pulse_app_controllers_test --layout-search-prefs` | 通过 |
| 相邻：selftest `release-panels-hidden` / `pr-shell` / `list-columns` | 全部 exit 0，0 FAIL |

### 7.4 第二项（`quickPreviewAnchorView = -1` 时序）结论

**判定为非问题，代码未改。** `quickPreviewAnchorView` 只在
`HandleQuickPreviewCommand()` 的 Delete 分支设置，而消费它的 `SyncQuickPreview()`
仅由两处刷新回调调用（`app_navigation.cpp:1256`、`:1451`），
两处都在**同一个同步函数内**先 `RemapSelection()` 再调 `SyncQuickPreview()`。
也就是说「selection 已移动」与「anchor 被消费」之间不存在可插入 `Render()` 的窗口：
anchor 设置后到下一次 `SyncQuickPreview()` 之前，快照未更新、`selected_index`
仍指向被删行 → 跟随读到与已显示项相同的路径 → `Stay`，不触碰 anchor。
真机确认项仍保留在 §3.5（预览窗内 Delete 是否邻接）。