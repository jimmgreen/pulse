# 快速访问

侧栏的「快速访问」是一段分组：上面四个是 Pulse 内置的入口，下面跟着用户自己固定的文件夹。它就是一个普通的侧栏分区——有组头，可以折叠，也可以在分区菜单里整个隐藏——但它比别的分区多两条规则：内置入口的显隐逐个由这里管，用户固定的行可以在组内拖动重排。

## 行为

- **内置四项**：最近、桌面、Downloads、回收站，顺序固定。它们**不写进 `places.json`**，也不参与组内拖动重排；「最近」指向 `pulse:recent` 虚拟视图，其余三项指向系统已知文件夹。
- **内置项的显隐**：分区右键菜单为每个内置入口给一个勾选项（`sidebarQuickAccessHiddenMask`，session 存 `quickAccessHidden`）。把整个分区隐藏后再放出来时内置链接会一起恢复——否则分区会空到没有地方可以右键。
- **用户固定的路径**：存在 `places.json` 的 `quick_access_paths` 里，排在四个内置项之后。只接受真实路径（空串和 `pulse:` 虚拟路径会被忽略），重复固定是幂等的，大小写与斜杠差异按同一路径去重；被固定的目录改名或移动后跟着 `RemapPaths` 走。
- **固定 / 取消固定**：文件列表里是「固定到快速访问」/「从快速访问取消固定」（多选时按"是否已全部固定"给其中一项）；固定行的右键菜单是「打开 / 从快速访问取消固定」。
- **拖动重排**：按住一个固定行拖动，在固定行之间插入；插入线只按用户固定过的行计算，四个内置项不参与。松手时按新位置写回 `places.json`，任务栏 jump list 的顺序跟着一起刷新。
- **分区本身**：组头是「快速访问」；分区掩码控制折叠与隐藏，窄栏（rail）下同样留一行可点击的图标。行都被关掉的空分区不绘制。
- **已删除的行为**：以前"当前标签所在目录往上 ≤12 层内有 `.git`"时，会在内置项之后、用户固定项之前自动插一行「项目 <仓库根名>」加一个绿色 `Git` 徽标。这一行**已经删掉**——它是内存里的临时项，不落盘、右键也没有菜单，与用户自己固定的项不是一回事。删除只影响这一行：`FindGitRoot`、`Tab::git_root`、`AppState::gitRoots` 都还在，命令面板的项目搜索（`ProjectSearchRoot`）照常用。

## 实现入口

- `src/app/app_model.cpp`：`BuildSidebarModel()` 产出内置四项（`BuiltinQuickAccess::Recent / Desktop / Downloads / RecycleBin`）；`BuildWindowViewModel(...)` 按 `sidebarQuickAccessHiddenMask` 滤掉被关掉的内置项，再把 `places.quick_access_paths` 逐条追加成行。
- `src/app/app_model.h`：`BuiltinQuickAccess`（内置入口的顺序，也是掩码位）、`SidebarEntry::builtin`、`SidebarSectionId::QuickAccess`。
- `src/app/places.cpp`：`IsQuickAccessPinned` / `SetQuickAccessPinned` / `ReorderQuickAccessPinned`，以及 `Load` / `Save` 里的 `quick_access_paths`。
- `src/app/quick_access.{h,cpp}`：`QuickAccessTargets`（从选中项或背景取候选路径）、`AppendQuickAccessCommand`（菜单里的固定、取消固定项）、`HandleQuickAccessCommand`、`ShowQuickAccessMenu`（固定行的右键菜单）。
- `src/app/app_input.cpp`：固定行的拖动（`ResetSidebarPinDrag` / `UpdateSidebarPinDrag`，插入位只对固定行计算）。
- `src/app/sidebar_sections.cpp`：`ShowSidebarSectionMenu`——内置入口逐项勾选，加上分区的折叠/展开/隐藏。
- `src/ui/ui_sidebar.cpp` 与 `src/ui/fluent_components.*`：行的绘制（`fluent::SidebarItemSpec`，含图标与徽标）与组头（`SidebarSectionHeaderSpec`）；布局与命中在 `src/ui/ui_renderer_internal.h`、`src/ui/ui_hit_test.cpp`。

## 定向验证

这个分区没有专门的自动化用例；改动后按这份手工清单验证（只构建受影响的 `pulse`）：

- 打开一个含 `.git` 的目录（例如仓库根下的子目录），确认快速访问里**不再**冒出「项目 …」那一行。
- 选一个文件夹 → 固定到快速访问，行出现在内置四项之后；再固定一次，行数不变。
- 在固定行上右键 → 从快速访问取消固定，行消失；该文件夹若同时是星标，星标不受影响。
- 按住一个固定行拖动，越过别的固定行后松手，顺序改变；重启后顺序还在。
- 分区右键菜单逐个关掉内置入口，只留用户固定项；把分区整个隐藏再显示，内置项回来。
- 折叠/展开分区，窄栏下确认图标行仍在，且点击它同样能折叠。

验证边界：内置的「最近」指向 `pulse:recent` 虚拟视图（不是文件系统里的目录），其余三项是系统已知文件夹；用户配置在 `%LOCALAPPDATA%\Pulse` 的 `places.json`，安装器不写这一处。以上都是手工检查，编译成功或别的分区的受控用例都不能替代它们。
