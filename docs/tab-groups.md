# 标签组（组 chip）

浏览器把标签分组，Edge 用一个可折叠的**组名按钮**代表整组标签。Pulse 的标签带照这个做法：属于同一个组的标签在标签带上连成一段，段首画一个 chip（组名 + 组色），chips 是控件，不是标签——单击折叠、按住整组拖动、悬停出一张卡片。

## 行为

- **折叠 / 展开**：单击 chip 折叠该组，成员从标签带上消失（只剩 chip），再单击恢复；`Ctrl+Tab` 也跳过被折叠的成员。组是**连续段**，所以 chip 就是这一段的代表。
- **整组拖动**：按住 chip 拖动就是拖这整段（Chrome / Edge 的手感），落点仍在标签带内；按下后没移动才算"单击折叠"。
- **chip 悬停卡片**（停留 150 ms）：折叠组的卡片列出组内标签 + 分隔线 + 「在组中新建标签页」「编辑组」两行动作；展开组的卡片只有那两行动作。点成员行**只切换到该标签，组保持折叠**（卡片就是读折叠组的窗口），点动作行分别新建标签、打开组菜单。卡片与 tooltip 互斥。
- **卡片高度**：卡片永远挂在 chip 下沿（顶边等于 chip 底边，不压住 chip），行数按"chip 底部到窗口底边"能放下的数量截断——放不下时**先砍成员行**，尾部两行动作行始终保留；被截断时，最后一个仍然画出来的成员行照样补一条分隔线，让成员与动作分开。可见行数由同一份几何算出，绘制与命中都用它，所以不会出现"看得见却点不到"的行。
- **折叠组里含激活标签**：chip 走"选中标签"的抬起外观（填充 + 顶部 2 DIP 强调线，用**组自己的颜色**），卡片里那一行在图标列画对勾。
- **卡片里的两种对勾**：组里含当前激活标签 → 该成员行画对勾（`active`）；组已经不拥有激活标签 → 画**同一个对勾但明显更淡**（`was_active`：`theme.text_secondary` 的不透明度再乘 0.7），代表"上次在组里用的那个标签"。两者互斥（active 优先，记忆此时不参与），所以同一张卡片不会看起来有两个选中项。记忆的维护口径是"窗口离开某组时，把离开的那个标签记到它所属的组"；标签被关闭或移出组后，指向它的记忆会被丢弃（只比较指针，不解引用）。这份记忆只活在内存里，**不写 session**，关掉程序就没了；它也**不算选中**——chip 的抬起外观只看这一组是否真的含激活标签。
- **组内新建标签会先展开该组**：折叠组的「在组中新建标签页」先把组展开再建标签（新标签会成为激活标签，藏在折叠组里等于标签带上零视觉变化）。
- **拖动入组**：拖动中不改归属，**只在松手时按最终左右邻居判定**——左右邻居同组就入该组；未分组的标签相邻组尾也入组，相邻组头入组。pinned 标签永不进组；组永远连续，所以拖动**跨过整组**时整段跳一格（"拖到组中间"结构上不可能存在）。
- **拖到展开组前的门槛**：拖动标签遇到一个展开组时，前缘只要越过**它遇到的那个成员的中心**即可整段跨越（不是整段中心——按整段中心算，3 个成员要拖 ~250 px，手感像"拖进组没反应"）；遇到折叠组则用 chip 自己的中心。两种情况都由 `RunCrossCenter` 这一个纯函数给出。
- **组被关光就消失**：组里最后一个标签被关掉时，这个组会一起消失（空组不留 chip——内容都没了，组就失去了价值）；无论从标签带的 ×、`Ctrl+W`、标签右键「关闭标签页」还是跨窗口移交标签哪条路径关闭都一样。组菜单「关闭分组标签页」如果关掉的正是窗口的唯一标签，则与 × 一致地关掉窗口。
- **持久化**：session 存 `tabGroups[]`（id / 名字 / 颜色 / `collapsed`），重启后折叠状态与组色都还在。
- **已知边界**：① 从 chip **斜向**移出时，中途落到标签区会立刻关卡片（Edge 有"安全三角"宽限，这里没有）；② chip 的 150 ms 滑动动画期间，卡片锚点与命中框用的是静止位置，可能与画面上的 chip 有短暂不一致。

## 实现入口

- 模型：`src/app/app_model.h`（`LayoutTab::tab_group`、`TabGroup{id,name,color_rgb,collapsed}`、`WindowTabs::next_tab_group_id`）+ `src/app/tab_group_model.cpp`——`MoveTabRun` / `FindGroupRun` / `MoveTabGroupAcrossFreeTab` / `NormalizeGroupRuns`（只归一连续性，不改归属）、chip 拖动的 `CollapsedChipBlockW` / `ChipBlockCrossed` / `DisplacedRestDelta`、拖入展开组的门槛 `RunCrossCenter`。`FillWindowTabStrip` 把折叠组成员置 `TabView::hidden`，并给 chip 记 `TabGroupView::has_active`（`app_model.cpp`）；`TabGroupCardRows` 构建卡片行，第三个参数是"上次激活"的记忆（默认空 = 不标记）。
- 控制器：`src/app/tab_controller.{h,cpp}`——`ToggleGroupCollapse`、`ShowGroupMenu`（组菜单：色板 8 色、改名、解组、关闭整组）、`NewTabInGroup`（组内新建，折叠时先展开）、`ShowTabMenu`。组菜单命令 id 在 `src/app/context_menu.h`（`CmdTab*`）。
- 几何：标签带不是 `view_layout.*`，而是 `MainRenderer::TabStripMetrics` / `ComputeTabStrip`（`src/ui/ui_renderer.cpp`）+ `TabItemRect` / `TabGroupChipRect`（`src/ui/ui_hit_test.cpp`）；卡片的矩形、行切分与截断都在 `src/ui/ui_renderer_internal.h`——`TabGroupCardRect` 通过出参给出可见行数，`TabGroupCardRowRect` / `TabGroupCardRowTop` / `TabGroupCardRowAt` / `TabGroupCardRowIndex` 是绘制、悬停与命中共用的同一份几何。
- 交互：`src/app/app_input.cpp`——chip 的按下/松手状态机（单击折叠、按住整组拖动）、卡片行的点击分支、拖动时的落组判定（`RunCrossCenter` + `MoveTabGroupAcrossFreeTab`）；弹卡/收卡是 `src/app/app_main.cpp` 里那套 16 ms `WM_TIMER`（`groupCardSince` 由 `src/app/app_runtime.cpp` 的 `ApplyHoverTarget` 维护，进 chip 才算起点），卡片行数据在同一文件的 `BuildVm` 里每次构建，状态字段在 `src/app/app_state.h`、卡片绘制在 `src/ui/ui_renderer.cpp`。

## 定向验证

只构建 `pulse`、`pulse_app_controllers_test`，未运行全部历史测试。

- `pulse_app_controllers_test.exe`：
  - `group crossing: dragging right crosses at the first member centre`、`... dragging left ... last member centre`、`a folded group crosses at the block centre`（`RunCrossCenter`：展开组按遇到的成员中心、折叠组按块中心）；
  - 标签转移消息用例（与本功能共用标签带，改动后须仍为 PASS）：`tab transfer reaches the window that takes the tab`、`tab transfer message is not the shell's open-path forward`、`shell open-path forward is not a tab transfer`。
- `PULSE_SELFTEST_CASE=tab-groups`（`pulse.exe --selftest`，日志 `bench_data\selftest_1b2_last.log`）：
  - 展开组：成员都在标签带上；卡片只有两行动作（成员行为 0 行）；
  - 折叠后成员 `hidden`、再点恢复；`has_active` 只在组含激活标签时置位（把激活标签移出组后不再置位）；
  - 卡片行构造：折叠组 = 成员行 +（最后一个成员行带分隔线）+ 两行动作、恰好激活那一行 `active`、展开组只有两行动作、未知组返回空、两组并存时只列自己的成员；
  - 折叠组里 `NewTabInGroup` 会展开该组，新标签属于该组；
  - 两种标记：组含激活标签时恰好一行 `active` 且**所有** `was_active` 为 false（即使传入记忆标签）；把激活标签切到组外并传入"上次在组里用的成员"后，只有该行 `was_active == true` 且 `active == false`，其余成员行两者皆 false。
- `PULSE_SELFTEST_CASE=tab-handoff`（同一份标签带几何，改动后须仍为 PASS）：桌面被置顶窗口盖住时它只打印 SKIP，不算回归。
- 手工（合成鼠标不可靠，chip 的命中/悬停/点击没有自动化覆盖）：建两个组 → 单击 chip 折叠/展开、按住 chip 整组拖动；折叠组悬停出卡片 → 点成员行（只切换、组仍折叠）、点「在组中新建标签页」（组展开且新标签可见）、点「编辑组」；把组内标签拖到另一个组里 / 拖整组跨过别的标签；把组做大到 10 个以上成员，确认卡片截断后两行动作行仍可点、成员与动作之间仍有分隔线；切换主题与 DPI 各看一次卡片配色与命中。

验证边界：编译通过与上面的受控用例不能替代真实交互——卡片的观感与截断手感、chip 滑动动画期间的锚点偏移、以及"斜向移出即关卡片"这条已知边界都需要人工确认。**淡对勾的浓淡只能人眼定**（当前是 `theme.text_secondary` 乘 0.7 的不透明度，太淡或太显就改这一个系数）。**chip 的悬停、卡片弹出与卡片点击都没有自动化**：合成鼠标在真实桌面上不可靠，这一层只能手验。

（本页不提交 `bench_data` 下的截图。）
