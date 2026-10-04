# 多窗口标签（一个窗口一组标签）

Pulse 默认是单实例：第二次启动把文件夹转发给已经运行的窗口就退出，标签因此只能堆在一条标签带里。`--new-window [路径]` 让新启动的进程跳过单实例互斥，成为**独立窗口**，每个窗口有自己的标签。

多窗口能力由设置里的**「多窗口模式」**（`app.json` 的 `multi_instance_mode`，**默认关闭**）统一控制：关掉时 Pulse 回到"原始单实例"的形态——跳转列表的固定项在运行中的窗口里新建标签页、不提供「新建窗口」任务、标签菜单没有「在新窗口中打开」、标签拖过窗口边缘不撕出新窗口、连旧的 `--new-window` 快捷方式也退化成普通启动。打开后才是下面这一整套行为。默认关闭是为了让"没打算用多窗口"的用户完全感觉不到它的存在。

## 行为

- **每个窗口一组标签**：次级窗口读 session 只取起始目录，**不写 session**，也不建托盘图标、不注册全局热键、关掉窗口就直接退出——托盘、全局热键、撤销栈和持久化都留给主窗口。
- **开关关闭时（默认）**：跳转列表固定项 → 现有窗口新建标签页（走既有的单实例转发路径）；「新建窗口」任务不出现；标签右键菜单不含「在新窗口中打开」；拖动标签越过窗口边缘按普通重排处理（`AppState::tabDragExternal` 恒 false，浮动卡片与落点解析都不参与）；`--new-window` 被忽略——启动早期先读一次 `multi_instance_mode`，因为 `secondaryInstance` 必须在取单例互斥之前定下来（否则进程已经跳过了互斥）。
- **关掉开关 = 把多余的窗口收回来**：主窗口向每个其它 Pulse 窗口发一条 `'PUDR'`（drain）请求，对方**按顺序**把每个标签用既有的转移消息交给主窗口，全部交付成功才 `WM_CLOSE`（并置 `mergedAway`，不写 session）；有一个标签发不出去就保留那个窗口，宁可留着也不丢标签。只有主窗口执行合并——托盘、全局热键与 session 都在它手里；次级窗口改这个设置只让行为当场生效，不做合并。
- **入口**：标签右键的「在新窗口中打开」、任务栏按钮右键的「任务 → 新建窗口」，以及把标签拖出/拖到别的窗口。托盘菜单只剩「打开 / 退出」——托盘是留给这个会话本身的地方。
- **起始位置**：`--new-window <路径>` 的唯一标签就是该路径；不带路径时是**最近使用**（`pulse:recent`）。任务栏 jump list 的「新建窗口」任务不带路径，所以新窗口从最近使用起步；标签右键入口带的是该标签当前聚焦窗格的路径（虚拟视图也照样传，它本身就是一个 `pulse:` 路径）。窗口按"已存在的 Pulse 窗口数 × 32 px"级联。
- **跨窗口拖标签**：标签拖出标签带后，光标下的 Pulse 窗口接收它，且**总是新开一个标签**——即使对方已经开着同一个文件夹（这一点与双击文件夹的"已打开就激活"不同：前者是"把这个标签搬过来"，后者是"打开它"）。拖动期间有一张跟随光标的浮动卡片。
- **合并**：被拖走的是源窗口的**最后一个**标签时，源窗口关闭（不写 session），接收方**接管**托盘图标、全局热键与 session。
- **拆窗口**：在没有任何 Pulse 窗口的地方松手，标签变成一个**新窗口**。
- **关闭窗口里最后一个标签 = 关闭这个窗口**（Explorer 的语义）：标签带上的 ×、标签右键「关闭标签页」、Ctrl+W 三条路径一致。是否真的退出交给窗口自己的关闭逻辑——主窗口按托盘/开机自启设置隐藏到托盘或退出，次级窗口直接退出。唯一例外是**固定（pinned）标签**：它不画 ×、菜单项不可用、Ctrl+W 无效，因此不会关掉窗口。
- **边界**：一次只带走**一个路径**（被拖标签当前聚焦窗格的 `current_path`）——分屏布局、标签组、自定义标签名都不跟随；虚拟视图（最近使用、标签、保存的搜索）照传。拖动过程中在标签带内仍是普通重排，只有离开本窗口（或落到别的窗口）才触发交接。

## 实现入口

- 开关：`src/app/app_prefs.{h,cpp}`（`multi_instance_mode`，默认 false；键同样要登记进 `MergedWithDisk()`，否则多窗口下会互相回滚这个设置）+ `src/app/settings_controller.{h,cpp}`（`ToggleUi(34)`、`SettingsEffect::MultiInstance`）+ `src/app/app_commands.cpp`（效果消费点：重建跳转列表、关闭时合并窗口）+ 设置页五处（`settings_layout_sections.h` / `ui_renderer_internal.h` / `ui_hit_test.cpp` / `ui_settings_core.cpp` / `ui_settings_view.cpp`）。
- `src/app/instance_launcher.{h,cpp}`：`LaunchNewWindow`（`ShellExecuteW --new-window`）、`PulseWindowUnderPoint`（`WindowFromPoint` → 沿 owner 链找类名 `PulseMainWindow`，跳过发起窗口和 `PulseTabDragGhost` 卡片）、`OtherPulseWindows`（`EnumWindows` + 同一套类名比对，"收回窗口"用它枚举目标）。
- `src/app/single_instance_coordinator.{h,cpp}`：`Acquire`（`Local\Pulse.Singleton`，`--new-window` 跳过）、`SendTabTransfer` / `DecodeTabTransfer`（`WM_COPYDATA`，id `'PTBT'`）、合并请求 `SendDrainRequest` / `DecodeDrainRequest`（id `'PUDR'`，payload 是接收窗口的 `HWND`），以及既有的 `SendOpenPathToWindow` / `DecodeOpenPath`（id `'PULS'`）。
- `src/ui/drag_ghost.{h,cpp}`：`TabDragGhost`——layered、topmost、不吃鼠标的浮动卡片，按抓取偏移贴光标。
- `src/app/app_input.cpp`：`HandOffTabUnderCursor`（松手时的落点判定与交接）、拖动期间的 `AppState::tabDragExternal`（关闭模式下恒 false）、`ShowTabMenu` 调用点传的模式参数。
- `src/app/tab_controller.{h,cpp}`：`ShowTabMenu(..., bool multi_instance)`——关闭时不放「在新窗口中打开」这一项。
- `src/app/jump_list.{h,cpp}`：`RefreshJumpList(folders, bool multi_instance)`——固定项的启动参数（`--new-window <路径>` / 裸路径）与「新建窗口」任务的有无都由它决定。
- `src/app/app_main.cpp`：`WM_COPYDATA` 接收（含 `'PUDR'` 的逐标签交接与 `WM_CLOSE`）与 `WM_CLOSE` 的"合并退出"分支。`src/app/app_runtime.cpp`：`OpenFolderInNewTab`、`AdoptSingletonOwnership`。

## 定向验证

只构建 `pulse`、`pulse_app_controllers_test`，未运行全部历史测试。

- `pulse_app_controllers_test.exe`：
  - `tab transfer reaches the window that takes the tab`（转移消息真的送到目标窗口，payload 往返一致）；
  - `tab transfer message is not the shell's open-path forward`、`shell open-path forward is not a tab transfer`（两个消息 id 双向不可混用）；
  - `multi-window mode is off by default`、`multi-window mode turns on and persists`、`multi-window mode switches back and persists`（默认关闭、开关往返落盘，且随 `SettingsEffect::MultiInstance` 生效）。
- `PULSE_SELFTEST_CASE=tab-handoff`（`pulse.exe --selftest`）：
  - `the drag card hangs by the grab offset`、`the drag card is hidden before the hand-off`（浮动卡片就位与交接前隐藏）；
  - `the window under the cursor takes the tab`、`the other window takes it when the cursor moves on`、
    `the window that owns the tab is never its own target`、`a window carrying the drag card's class is never a target`（落点解析）。
  - 日志：`bench_data\selftest_1b2_last.log`。
  - 用例依赖真实桌面：它建三个 TOPMOST 的可见探针窗口，再用 `WindowFromPoint` 做真实命中，所以**命中前先校验探针确实在最上层**。桌面上有通知、输入法候选窗或别的置顶应用盖住探针时，它打印 `[SKIP] tab-handoff: the probe windows are covered` 并跳过上面那 4 条落点断言（不判失败）。**被盖住不是回归**：换个干净的桌面或重跑一次即可，但那一轮里这 4 条断言确实没有执行。
- 全量 `pulse.exe --selftest`：本轮为 `1526–1532 passed / 22–24 failed`，**失败集合与未改动的 `main` 构建完全一致**（`advanced edit` / `advanced search` / `search history` 的焦点类用例需要独占前台窗口；机器上另有 Pulse 实例在跑时会多出 2 条 `tray stack` 拖动偶发）。所以"多窗口模式的改动没有引入自测回归"是相对于 `main` 基线成立的，不是"全绿"。
- 手工：两个实例 → 把一个标签拖到另一个窗口（对方多出一个标签）；把最后一个标签拖过去（源窗口关闭，托盘图标与热键跟过去）；拖到没有 Pulse 窗口的地方（变成新窗口）；任务栏按钮右键 →「任务 → 新建窗口」。**再关掉「多窗口模式」**：确认跳转列表只剩固定项、标签菜单里没有新窗口项、拖标签出界不撕窗口、旧的 `--new-window` 快捷方式只在现有窗口开标签，以及刚才那些额外窗口的标签被并回主窗口后窗口自己关闭。

验证边界：编译通过与上面的受控用例不能替代真实的多窗口交互——卡片观感、DPI 缩放下的命中、把标签拖到"另一个进程正忙"的窗口时的手感都需要人工确认；"关掉开关就收回窗口"这条路径里的**交接顺序与焦点**（主窗口被逐个激活、标签落地顺序）也只能手工看。`tab-handoff` 被置顶窗口盖住时只落一条 SKIP，那 4 条落点断言在这一轮并不成立；把 SKIP 当成"通过"会掩盖真实的落点回归。

（本页不提交 `bench_data` 下的截图。）
