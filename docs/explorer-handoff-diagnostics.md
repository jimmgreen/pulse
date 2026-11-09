# Explorer 接管诊断事件

生产日志位于 `%LOCALAPPDATA%\Pulse\Diagnostics\Runtime`，设置中的诊断导出会包含 Runtime JSONL；不需要开启 selftest tracing。所有新增事件仅记录数字，不记录路径、文件名、选中文件内容或 HWND。进程的 `process_start` 已记录 Windows 版本/build；各事件自带 UTC、tick、pid、tid、session。必须用 pid/session 联合请求 ID 关联，不能跨进程仅匹配 request_id。

## 请求链

`explorer_candidate` 在观察到的候选退出准入/准备阶段时记录一次（known HWND 集合避免每次扫描重复记录），candidate_id 只在本进程/session 内关联。reason：

| reason | 含义 |
| --- | --- |
| 0 | 准备提交，不代表成功接管 |
| 1 | Shift 保留 Explorer（包括准备阶段按下 Shift） |
| 2 | 不支持的命名空间/位置，或其它策略保留 |
| 3 | 进程无法确认为系统 Explorer |
| 4 | 进程创建时间或线程身份无法读取 |
| 5 | 隔离 scope 的初始视图证据不符合 |
| 6 | 窗口生命周期标记无法安装或已被占用 |
| 7 | 提交前窗口消失或隔离 scope 失效 |
| 8 | 设置保留原生「此电脑」 |
| 9 | 等待视图就绪超时 |
| 10 | 文件系统路径不能确认为目录（可能不可访问、已删除或是文件） |
| 11 | 选择状态读取失败，未提交 |
| 12 | 准入观察达到 4 秒上限，未提交 |

view_ready、supported、selection_read、age_ms、view_age_ms、selected 提供状态。tabs 是决策当时观察到的 ShellTabWindowClass 数量；0 不证明没有标签，Windows 11 必须明确验证只有一个标签才能关闭。多标签/无法证明单标签会在关闭安全复核的 Rejected code=2/detail 位 2 中体现；最后复核也会拒绝新增标签。记录字段并非每种 reason 都存在。


`explorer_candidate_link` 连接 candidate_id 和 request_id。`explorer_handoff` 的每个 phase 在同一请求中最多写一次（UI/STA 原子去重），elapsed_ms 从该 handoff 创建开始计算：

| phase | 含义 |
| --- | --- |
| 0 | Created |
| 1 | Submitted（detail 为选择数量） |
| 2 | Received |
| 3 | Prepared（后台目录/选择准备完成） |
| 4 | CloseRequested（取得关闭权） |
| 5 | QuitResult（code 为无符号 HRESULT） |
| 6 | Closed（已观察原生窗口消失） |
| 7 | Cancelled（code：2 Cancelled、3 Expired） |
| 8 | CloseFailed（code：2 Cancelled、3 Expired） |
| 9 | Rejected（见下表） |
| 10 | Deferred（UI 忙，仍保留固定截止时间） |
| 11 | Rollback（code 为终态，detail=1 删除未被用户改动的临时页，0 保留用户页） |
| 12 | Committed（源窗口消失后才公开 Pulse 页） |
| 13 | Presented（最终页已完成有效绘制） |

Rejected code：1 队列提交失败；2 关闭前安全复核失败（detail 位 1=身份/位置无效，2=无法证明只有一标签，4=选择变化）；3 最后身份、停止或截止时间检查失败（detail=1 正在停止）；4 后台枚举失败（detail 位 1=加载失败，2=路径变化，4=无快照，8=只读网络结果）；5 请求的选中项不在当前可见列表；6 提交后出现晚到选择变化，保留源窗口（SelectAndKeep，不重试、不扩大关闭授权）。未带 Rejected 的取消可能由导航、用户操作、停止或 lease 释放引起；不能仅凭 Cancelled 推断具体原因。

`QuitResult` 成功不代表关闭成功，必须继续看到 Closed。Closed 后缺少 Committed/Presented 说明 UI 完成链未确认。查看最近的 phase、elapsed_ms、Rejected 原因和进程健康事件定位；缺日志也可能是 dropped_events/write_errors，不能当作阶段未发生的唯一证据。

## 外部打开与配置

`external_open` 使用原有随机 wire identity 的 id_high/id_low 两半，入口 entry：0 普通外部请求，1 Explorer 接管，2 内部 reveal。handoff_id 非零时连接接管记录。phase=0 是入队结果（code 0 Invalid、1 Accepted、2 Duplicate、3 Retry）；phase=1 是实际交付终态（code 2 Dispatched、3 Cancelled）。交付成功不是画面呈现确认。用同 identity 的入队/交付 tick_ms 差查看排队耗时。

Accepted 和交付终态按请求记录；拒绝、重复和 Retry 在进程中每秒最多采样一次，避免重试风暴刷屏，所以采样日志不能用于精确计数。正常接管阶段不在每次轮询或每帧打印。底层日志采用有界队列和文件轮转。

`explorer_takeover_config` 在首次同步及开关组合变化时记录 enabled/integration/takeover/compat/this_pc/isolated，包含关闭状态。它表示应用当前配置，不证明注册表注册状态；注册表核验使用现有 integration 验证工具。`explorer_watcher_start` phase 0=创建 ShellWindows，1=初始窗口枚举不完整，2=完成事件订阅；hr 为无符号 HRESULT，window_hook 表示窗口创建事件钩子是否存在。phase 3=顶层 baseline 枚举失败，4=低频发现定时器失败，5=COM 初始化失败，6=工作线程创建失败。phase=2 的 baseline_windows 是启动时保留的窗口数（包括尚未注册 Shell COM 的 CabinetWClass 顶层窗口），不是遗漏请求数。`explorer_watcher_scan` 只在完整枚举状态变化时记录 complete，持续失败不会每秒重复刷日志；恢复 complete=1 也不证明中间没有遗漏。

订阅后立即补扫，正常运行每秒补扫一次，弥补注册通知遗漏/短暂枚举失败；事件订阅失败时仍可依靠成功创建的补扫定时器工作（phase=2 失败不单独证明 watcher 已停止）；候选事务仍使用原有较短轮询。钩子收到销毁事件会清除旧 HWND 的已知状态，避免句柄复用一直被视为旧窗口。低频补扫不能恢复已消失的窗口，也不能保证在 Explorer COM 阻塞时及时发现。

已有窗口复用、已有窗口内打开新标签或导航不会成为新候选。公开 Shell 注册事件/活跃视图不能提供可靠的「这是一次新的外部打开意图」以及单独标签关闭授权；主动关闭这些窗口可能丢失用户原有标签，所以保留此边界。启动 baseline 只记录一次数量，不逐次记录已有窗口导航；Shift 等理由只覆盖已观察到的候选。日志没有全系统打开请求总数，不能据这些事件计算全局接管成功率，或把未出现 candidate 的操作认定为接管成功。底层有界队列/轮转仍可能丢日志。

## 定向验证

`pulse_open_handoff_test` 含两个并发线程重复上报同一阶段 2000 次的回归检查，要求只发一个阶段事件且不同请求 ID 不重复。生产落盘和导出验证应在真实接管隔离 probe 运行后检查对应 Runtime 文件及导出中的上述事件；仅通过编译或此单测不代表实机接管成功。

## Win+E 运行时接管

此入口需要 Pulse 正在运行，且 integration_enabled、integration_win_e、take_over_win_e 均开启，当前实例拥有服务 endpoint，并未退出。截图模式不安装钩子；隔离实例仅在 selftest 构建且显式设置 PULSE_TEST_WIN_E_HOTKEY 时允许参与真实桌面。配置满足条件不等于钩子安装成功，必须检查 win_e_hook.enabled。它仅处理无 Ctrl/Alt/Shift 修饰的 Win+E，不代表能够接管所有系统保留快捷键；Pulse 未运行时没有此运行时钩子。

| 事件 | 字段及含义 |
| --- | --- |
| win_e_hook | enabled=1 表示 WH_KEYBOARD_LL 安装成功；enabled=0 且 error 非零表示启动/安装失败。正常结束时 reason=1 表示 Stop（关闭开关或退出），2 表示消息线程自行结束。 |
| win_e_chord | accepted=1 表示固定队列槽接受请求，当前 E 按键交由 Pulse；0 表示未接受，原生按键放行。menu_mask 是 SendInput 返回的事件数量，2 表示防止裸 Win 触发菜单的两个辅助事件均已提交，不能据此证明画面变化。 |
| win_e_delivery | state=2 表示 Dispatched，3 表示 Cancelled。native_fallback=1 表示已尝试启动系统 Explorer；error=0 仅表示 CreateProcess 成功，不能证明 Explorer 窗口已出现。native_fallback=0 且 error 非零也可能表示分配/启动结果监视任务失败，此时 state 可能仍为 Pending(0)/Queued(1)。 |

win_e_chord、win_e_delivery 的 id_high/id_low 是预生成随机请求身份，结合 pid/session 关联 external_open 的同身份交付终态。键盘固定槽不走普通 SubmitOpenRequestImpl 入队日志，因此此入口以 win_e_chord.accepted=1 为入队证据，不要求 external_open.phase=0。正常完成应看到 external_open phase=1/code=2 及 win_e_delivery state=2/native_fallback=0；这些都是交付确认，画面仍须单独验证。

已接受但未派发的 Win+E 请求取消后，后台任务直接启动系统 `explorer.exe /n,/e,::{20D04FE0-3AEA-1069-A2D8-08002B30309D}`，回退目标是原生「此电脑」。不重新发送 Win+E，也不调用已接管的默认 verb；仍需以实际 HWND/COM 观察确认原生窗口出现。

已接受请求若最终 Cancelled，结果监视任务在独立线程尝试一次原生 Explorer 回退；Dispatched 不回退。关闭钩子后仍可能出现已接受请求的结果日志。原生回退成功与否必须独立测试，不能以日志 error=0 替代真实窗口证据。

日志只描述 Win+E 请求结果，字段均为数字；不会记录其它按键、扫描码、输入文本、路径或文件名。键盘回调不写日志，结果通过专用消息线程报告；长按重复不重复提交。拒绝原因当前只有 accepted=0 这一结果，无法单靠它区分固定槽占用、队列满、锁竞争、endpoint 无效或预分配失败。


## Shell 文件选择桥接

`shell_registry_bridge` 记录普通 Shell 打开后选择文件的注册桥接，和 watcher 接管是不同入口；pane_key 仅关联本进程中的窗格，不记录路径。phase 1=RegisterPending，2=Register，3=OnNavigate，hr 为调用结果；1/3 的 detail 表示 PIDL 形式（0 主形式、1 别名），2 的 detail=1 表示 pending/bound cookie 一致。每个注册槽的每个阶段只在首次或 HRESULT 改变时记录，导航成功日志不作为逐次导航计数。

phase 4=文档 QueryService 调用结果，service_low/high、iid_low/high 是 GUID 内存的两个 64 位数字；phase 5=IShellView::SelectItem 入口，detail 是 SVSI 标志，仅表明进入调用，hr=0 不代表选择已应用。每个文档/视图最多记录 16 次接口/选择调用；缺后续日志不能证明接口未调用。若 SHOpenFolderAndSelectItems 失败，应先确认 folders 配置生效且存在注册阶段，再查看是否有 QueryService/SelectItem；仅有 external_open entry=0/handoff_id=0 不能归因为 watcher 过早关闭。

## 冷启动和离线汇总

新注册命令使用 `--shell-open <target>`。稳定 launcher 仅在确定尚未创建宿主时回退原生；创建成功后不按超时或退出码重开。旧注册需重新应用或升级安装才获得该入口保护。`launcher-*.jsonl` 的 `shell_entry_launch_failed` 包含 phase（0 自身路径、1 组件解析、2 启动准备、3 创建宿主）、error、native_attempted、native_error，均为数字；它随正常诊断导出。native_error=0 只说明启动调用成功，仍不证明原生窗口出现。

支持人员可以对已解包的 Runtime 目录运行 `python scripts/summarize_explorer_handoff.py <Runtime目录> --out summary.json`。工具按进程会话和请求身份关联，去重重复导出的序号，区分已关闭但缺少呈现证据、回滚、取消及 Win+E 重复/矛盾交付。它不生成全局接管百分比，也不把原生进程启动成功当作窗口成功；原始日志仍是排查依据。字段含义需结合本文读取。


新窗口的事务提交前会观察完整目录和选择集合：非空选择连续两次及以上读到相同内容、跨度至少 150ms；空选择需稳定至少 800ms，避免在原生 API 稍后应用选择之前提交空选择事务。路径/选择变化或不可读会重置稳定观察，枚举顺序变化不重置；注册起 4 秒是统一准入上限，持续变化不会无限等待。stable_age_ms 记录本次稳定时长，原有 view_age_ms 仍指首次视图就绪时间，不能替代稳定性证据。提交后原有 10 秒截止时间和最后完整安全复核不变，不会为晚到选择延长事务或反复重提。

## 2026-11-04 全屏直达修正

Win+E 不再使用前台窗口几何信息决定是否放行原生入口。是否启用仍由接管总开关、Win+E 选项和有效运行实例决定。请求成功提交则消费 E 的按下、重复和抬起；提交失败放行原生，已接受但取消的请求沿用原生回退。

`win_e_foreground` 记录 accepted、foreground_pid、geometry_at_log 和 kind_at_log（0=普通窗口，1=覆盖屏幕，2=未知）。几何信息是消息线程稍后采样，仅用于诊断，不代表按键发生时的精确前台状态。旧 `win_e_native_route` 是旧版主动放行的事件，新版不再产生。请结合相邻的 win_e_chord 和 win_e_delivery 判断提交与交付结果。

20:50 回访：安装二进制 SHA256 与交付包一致。修正 win_e_foreground 的 13 字段超过日志 12 字段上限，之前会丢弃整条记录。新增 win_e_gate，仅在 E 首次按下且已观察到 Win 或异步状态显示 Win 时记录修饰键判定，包含前台 PID、提权状态及读取错误；不记录输入文本或其他普通按键。不改变原有快捷键路由。用于区别收到修饰键但异步状态不一致、附加修饰键导致不匹配，以及按键回调没有观察到请求。缺少事件仍不能单独证明键盘钩子已被移除。

## Win+E 输入链路修复与完整关联（2026-11-04，替代前述诊断包）

当前生产实现不再用 `win_e_gate` / `win_e_foreground` 作为主要证据，采用独立后台线程采集：
- `win_e_context_process`：context_id、前台程序 basename、PID、进程创建时间和读取错误；不采集标题、完整路径或输入文本。
- `win_e_context_security/window/monitor`：相同 context_id 下的权限、窗口、显示器范围；即使快捷键没有到达也随前台切换记录。
- `win_e_input`：captured_tick / input_time_u32 为收到事件的时间，日志 utc 为后台写入时间，delay_ms 为队列延迟。phase 1/2=Win 按下/松开，3/4/5=E 首次按下/重复/松开。cycle 对应 E 按键周期；Win 事件中的 cycle 是最近 E 周期，不能单独拿它统计物理快捷键次数。
- observed/asynchronous 位：左/右 Win=1/2，左/右 Ctrl=4/8，左/右 Alt=16/32，左/右 Shift=64/128。flags 为原始 KBDLLHOOKSTRUCT.flags，供区分注入；action 0/1/2=放行/提交/吞掉重复，accepted 表示本次 E 归 Pulse 处理。
- `win_e_chord`：cycle、context_id、随机请求 id_high/id_low、accepted、prepared、menu_mask、wake_error。和已有 external_open/win_e_delivery 通过请求 ID 关联；wake_error 非零时 timer 仍会完成请求监视与预准备，不重复打开。
- `win_e_hook_refresh`：前台切换触发重新安装 hook，generation 每次成功递增；reason 1=前台通知、2=timer兜底；不根据全屏、尺寸或应用名称关闭接管。
- `win_e_trace_health/failed`：固定队列消费数量、丢弃数量、退出/失败，以及累计 E 按下/松开计数（不记录普通 E 的逐键序列）；便于区别完全没收到 E 与收到但没有匹配 Win+E。日志不再静默忽略字段超限；trace/context事件有编译期字段上限检查，测试验证真实落盘。

按键回调只更新状态并写固定容量单生产者队列；进程/权限查询与文本日志均不在键盘线程执行。组合键路由仍使用当前异步快照，事件态只作诊断：其他 hook 也可能吞掉松开事件，不能永久把旧 Win 按下当成仍然按住，否则会误吞普通 E。实际恢复通过前台切换重新安装钩子，避免按键在更早的 hook 被截断。重复/松开继续归已接受的 E 周期，刷新 hook 不清空周期所有权。

本机原生钩子链已复现：下游吞 Win 导致 async=false；后装钩子遮挡前一钩子；重新安装到链头恢复接收。它证明修复针对的机制，并不证明用户游戏一定使用了该机制。
