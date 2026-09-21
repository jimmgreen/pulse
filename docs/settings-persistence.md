# 设置落盘（app.json 与 context_menu.json）

`%LOCALAPPDATA%\Pulse\app.json` 存的是全部应用设置（通用、搜索与索引、外观、重复文件扫描）。每个窗口是一个独立进程，各持一份 `AppPrefs`。原先是"每次保存都把内存里的整份配置写回文件"，于是有两个坑：

- 一个窗口（尤其是 `--new-window` 开的次级窗口）退出时会把整份配置写回去，顺手回滚别的窗口刚改的设置；
- 文件读不出来时（被别的进程独占、被截断、不是合法 UTF-8）`Load()` 静默保留默认值，之后任何一次保存（改任一设置、拖侧栏、退出）就把这一整批键固化成默认。

现在 `app.json` 的读写有四条保证。

## 四条保证

1. **原子写**：先写 `<文件>.tmp.<pid>`（临时名带进程号，两个窗口同时保存不再互相抢同一个名字而让后到者失败），`FlushFileBuffers` 后用 `MoveFileExW` 替换目标。任何一步失败都先删掉自己的临时文件。
2. **备份**：写盘前把现有 `app.json` 复制成 `app.json.bak`（失败忽略）。唯一例外是这次要写的值来自备份（见第 4 条）——那时主文件已经被判定不可信，不能让它覆盖救过命的备份。
3. **读改写合并**：`Load()`/`Save()` 之后都记下"与文件一致的状态"，保存时逐字段比较——本进程自上次同步后改过的字段保留本进程值，没动过的字段取文件里现在的值。次级窗口退出因此不会再回滚主窗口刚写的设置。新增设置要同步加进 `MergedWithDisk()`；漏掉的字段退回"本进程值优先"（会丢别的窗口的改动，不会丢自己的）。
4. **读不到就不覆盖**：`app.json` 存在却读不出来（被独占、内容损坏）时不写默认值——先尽力把它改名成 `app.json.bad` 留证据，改名成功才写新文件；改名也失败（文件被别人独占）就本次保存失败返回 `false`，宁可不写。
   - 主文件能解析但已知键出现不到一半（截断）→ 视为损坏，改读 `app.json.bak`，两份取更完整的。这同时让"坏主文件 + 好备份"能自愈：下一次保存把合并后的完整内容写回主文件。

`Load()` 的返回值语义没变（`false` 只表示数据目录不可用），想知道有没有真的从文件里读到值，用 `AppPrefs::loaded_from_file()`。

## 注册表对账

`launch_on_startup`（开机自启）和 `open_folders_in_pulse`（用 Pulse 打开文件夹）现在由 **app.json 存意图**，注册表降级为"可修复的投影"。原先是"以注册表为准"：安装器的卸载步骤会删这两处，"缺失"被读成"关"，下一次保存就把"关"固化进文件——这正是用户看到的"重装后设置回默认"。`AppPrefs::ReconcileRegistryWithFile()` 改成三条规则：

1. **以文件为准**：`app.json` 读得到，就按文件里的值逐项对账。
   - 文件说开、注册表缺失 → 写回（开机自启写当前 exe；文件夹关联走 `WriteFolderOpenClass`，每类写 `\shell\<verb>\command`，外加空 `DelegateExecute` 遮蔽 HKCR 继承来的委派处理器）；
   - 文件说开、注册表指向的是 `pulse.exe` 但路径不是当前 exe（换过安装目录） → 重写成当前 exe；
   - 文件说开、注册表指向别的程序 → 不抢：注册表和文件都不动（指向 `explorer.exe` 的不算"别的程序"，见下）；
   - 文件说关 → 清掉"我们写的"残留（`ClearFolderOpenClass`：我们自己写过的命令，或只剩 shell 默认动词的半截状态）。
2. **反向保护**：文件说关、但注册表明确是"我们的"且正在工作 → 判定为"文件丢过值"，**采用注册表的值并把文件写成开**，不静默关掉用户本来好用的设置。开机自启那一项没有"存在但关"的状态（设置页关掉时直接删值），所以"文件说关 + Run 值是我们的"只可能是文件丢过值，一律采用为开。
3. **首次迁移**：`app.json` 整体不可用（没有文件或读不出来）→ 保持旧行为，以注册表为准填这两项，继承老用户的现状。

这条对账与安装器侧是**两半**，互相不依赖、同时存在也不冲突：安装器（`installer/PulseSetup.iss`，上游已合 `fb122c0`）在升级前捕获 Run 键与 `Directory`/`Drive` 的 open 命令、装完写回，覆盖"旧卸载器先清再装"的那段空窗；应用侧（本文件描述的对账）以 `app.json` 为准，能补回安装器没有捕获的 `Folder` 类、换过安装目录的旧路径残留，以及两次升级之间用户自己改过的意图。

**接管范围**：`Directory`、`Drive`（写 `shell\open`，并把类的默认动词从 `none` 改成 `open`）以及 `Folder`（写 `shell\open` + `shell\explore`）。接管 `Folder` 也意味着**系统命名空间**会交过来（任务栏的"文件资源管理器"按钮、桌面上的"此电脑""回收站"图标走的就是这个类），它们由应用侧翻译或忽略，见 `shell-namespace-forward.md`。Folder 的两个动词在 HKCR 里都由委派处理器 `{11dbb47c-...}` 承载，所以还要在 verb 的 `\command` 下写一个空 `DelegateExecute` 把它遮蔽掉，否则执行权仍在 Explorer 的处理器手里。Directory/Drive 自身不定义这两个动词，双击本来就 fall through 到 `Folder` 类，所以补上 Folder 才覆盖到所有文件夹项。**Explorer 的 Win+E 入口（它自己的 CLSID）刻意不接管**：写坏它会让 Win+E/任务栏报错，收益只是把一条快捷键换个程序。同样接不到的还有第三方程序**硬编码**的 `explorer.exe /select,"%1"` 与 `SHOpenFolderAndSelectItems`：它们绕过类动词直接调 Explorer，注册表里没有任何可拦截的钩子（FDM 的"在文件夹中显示"两条路都用，实测仍会弹出资源管理器；Files 也受同一限制）。

"是不是我们的"分三档，**用途不同**：

- **宽松档**（`CommandIsPulse()`：只比首个 token 的**文件名**是否 `pulse.exe`）用于**写回、重写和清理**。旧安装目录留下的命令也算我们的：文件说开时被重写成当前 exe，文件说关（或用户在设置里关掉）时被清掉。不这样放宽，"关"就永远关不干净，而且那条残留会一直劫持"双击文件夹"，指向一个已经不存在（或已不在原路径）的 exe。
- **严格档**（`FolderOpenCommandIsOurs()`：全路径比较）只用于判断"**当前是否已配置**"（`ReadFolderOpen()` 与 `FolderOpenClassIsConfigured()`）——它问的是"当前这个 exe 是否已经生效"，所以旧路径不算已配置，会被重写而不是被当作已完成。
- **系统默认档**（`CommandIsShellDefault()`：文件名是 `explorer.exe`）：Windows 给 `Folder` 类在 HKCU 留了一份指向 `Explorer.exe` + 委派处理器的副本，所以"别家占用"的判定必须放过它，否则接管在真实机器上直接失效。文件说开时可以替换它；文件说关时也**不**把它当我们的残留删掉——`FolderVerbNeedsClear()` 只认命令名为 `pulse.exe` 的残留，或自己写的空 `DelegateExecute`。

前两档都不动指向别的程序的命令（宽松档的代价是"文件名恰好叫 pulse.exe 的别的程序会被当成我们的"，换来的是"关得掉"）。**一个例外要记清**：上面说的是**对账路径**；用户在设置里**手动打开**这个开关时走的是 `ApplyFolderOpen(true)`，它只查"当前 exe 是否已配置"，不查"是不是别家占着"——所以手动打开会**接管**别的程序占着的 `open`/`explore` 动词（存量行为；对账路径始终不抢，因为那里静默接管没有反馈）。所有写注册表的动作仍然只在 `persist` 为真、且这次运行没有被重定向到测试 profile 时执行。

顺带一条边界：`needs clear` 里"只剩 shell 默认动词"的半截状态只在 `\shell\open\command` 为空时才成立——别家的动词要留着自己的默认动词，不能被我们清掉。

数据目录被重定向（`PULSE_TEST_DATA_DIR`，只有自检构建认这个变量）时不碰真机注册表。`PULSE_TEST_REGISTRY_BASE`（同样只在自检构建里生效）会把上面这些路径统一前缀到沙箱键下，于是对账本身也能被测试覆盖：`prefs-registry` 用例就是这么跑的，并且每次结束都会再读一遍真实键确认没被动过。

## 覆盖范围

- `app.json`：四条保证 + 注册表对账。
- `context_menu.json`：右键菜单开关也是设置文件，**四条保证整套适用**（读改写合并、备份 `context_menu.json.bak`、读不到就改名为 `context_menu.json.bad` 再写、以及同一套**截断完整性闸门**——已知 6 个键里出现不到 3 个即判损坏、改读备份）。
- **未覆盖，按风险排序**（都是"整份覆盖 + 读失败当默认"的老模式，值得下一批处理）：
  1. `places.json`（快速访问/星标/最近）与 `tags.json`（自定义标签色）——**风险最高**：用户数据、多窗口常驻，任一窗口一次保存就会回滚别人；`tags.json` 在文件不可读时会被默认颜色表覆盖。
  2. `saved_searches.json`：整份覆盖，而且**不经过 `GetPulseDataDir()`**（自己拼 `%LOCALAPPDATA%\Pulse`，所以测试的数据目录重定向管不到它）。
  3. `session.json`：读失败回默认、退出时整份写；只有主窗口写一次，被强杀会丢但不会被别的窗口覆盖。
  4. `search_history.json`：每个窗口都用同一个路径、各自整份写 → **最后写者胜**（历史可再生，多窗口会互相清掉条目）。
  5. `index-config.json`：机器级（`%ProgramData%`，管理员写），`index_host.cpp` 的安装路径在 `LoadMachineConfig` 失败后按默认回写，可能抹掉排除卷/排除路径。

## 实现入口

- `src/app/app_prefs.{h,cpp}`：`AppPrefsValues`（落盘字段）/ `AppPrefs`（`persist`、`disk_state_`、`loaded_from_file()`）、`ReadDiskState`（主文件 → 备份 + 完整性判断）、`MergedWithDisk`（逐字段三方合并）、`ReconcileRegistryWithFile`（注册表对账）、`Load`、`Save`；文件内 `RegPath()`（沙箱前缀）、`FolderOpenClass`（类 / 是否带 explore / 是否写默认动词 / 是否遮蔽委派处理器的四元组）、`CommandIsPulse()` 与 `CommandIsShellDefault()`（两档归属判断）。
- `src/app/context_menu_prefs.{h,cpp}`：`ContextMenuPrefsValues` / `ContextMenuPrefs`（`disk_state_`、`loaded_from_file()`）、`ReadDiskState`（同样带完整性闸门）、`MergedWithDisk`、`Load`、`Save`。
- `src/common/utf8_file.h`：`ReadUtf8File` / `WriteUtf8FileAtomic`（临时名带 PID），以及两个**共用写盘策略叶子** `QuarantineUnreadableFile()`（改名 `.bad`，失败即"别写"）与 `KeepPreviousFileCopy()`（留 `.bak`）——两个设置文件的策略只有这一份，不会各自漂移。
- 测试：`src/app/selftest_1b2.cpp` 的 `TestPrefsPersistence()`（`prefs-persist`）、`TestPrefsRegistryReconcile()`（`prefs-registry`）、`TestContextMenuPersistence()`（`ctxmenu-persist`），三个都挂在全量 `--selftest` 清单里；各自在临时 profile 里跑，用 `PULSE_TEST_DATA_DIR` 重定向数据目录、`PULSE_TEST_REGISTRY_BASE` 重定向注册表，结束后还原并删掉沙箱键。

## 定向验证

只构建 `pulse` 与 `pulse_app_controllers_test`（Release），只跑与本次改动直接相关的用例。换基到 `upstream/main`（`187240e`，含 #7/#8/#10）后重跑的结论：`pulse_app_controllers_test`（全量 + `--layout-search-prefs`）全绿、全量 `--selftest` **1244 通过 / 0 失败 / 0 跳过**。

- `PULSE_SELFTEST_CASE=prefs-persist`（21 条）：往返、过期写入不回滚（两个 `AppPrefs` 交叉写，且连存两次也不会复活旧值）、独占文件时拒绝覆盖且逐字节不变、坏主文件 + 好备份自愈并修复主文件，最后再比对一遍 `HKCU\Software\Classes\Directory|Drive\Folder\shell` 与对应 `\shell\open|explore\command`（含 `DelegateExecute`）的默认值确认整个用例没动过注册表。
- `PULSE_SELFTEST_CASE=prefs-registry`（53 条）：对账十例——缺失写回（含 Folder 的 open/explore 与空委派值）、explorer.exe 系统默认镜像可替换且委派处理器被遮蔽成空、说关时该镜像不被当残留清理、别人（另一个程序）的不动（Folder 类被占时连 explore 都不建，但不影响 Directory）、旧路径重写、说关时清掉旧路径与半截残留、说关且命令属于别的程序时一个字节都不动（默认动词也留下）、混合所有者时只清我们那一半、说关但注册表在工作则采用并写回文件；最后比对真实 Run 值与 Directory/Drive/Folder 关联键未被沙箱之外的任何动作改动。
- `PULSE_SELFTEST_CASE=ctxmenu-persist`（21 条）：`context_menu.json` 的往返、过期写入不回滚、独占时拒绝覆盖且逐字节不变、截断主文件 + 完整备份时由完整性闸门改读备份并修回主文件。
- `pulse_app_controllers_test.exe`（全量 + `--layout-search-prefs` + `--global-search`）：prefs 的 JSON 往返、范围回退、以及"保存失败要回滚"的既有断言全过。

验证边界：没有做两个真实进程同时改设置的手工验证（那属于下一批）；`places.json`、`session.json` 未纳入本次改动；`selftest_1b2.cpp` 里 `DumpContextVerbs()` 的 `C4456`（局部变量 `line` 遮蔽）是既有警告，本次未处理。文件夹接管的注册表写入/清理由沙箱用例覆盖，**Explorer 里真实双击文件夹/盘符落到 Pulse 已由用户实机确认**；`Folder` 的 explore 动词没有自然的触发入口（只有程序化调用会走它），它与 open 共用同一套写入与遮蔽逻辑、未单独端到端验证；Win+E 刻意不接管，因此不在验证范围内。
