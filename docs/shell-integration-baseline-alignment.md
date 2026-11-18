# 系统入口重构：最新版基线对齐

2026-10-29。当前实施目录：`C:/Users/SS/.codex/worktrees/shell-integration-latest/pulse`。

## 基线与纠正

旧任务在 Desktop/pulse（c4f51bb）实施，用户所指最新版为 pulse58-integrated-fixes/pulse（9b48b683be449149c9a7a0ea150210818d6ff3ad 加未提交修改）。已建立独立工作树，复制稳定快照的 97 个未提交变更文件并逐文件 SHA-256 校验。关键入口、监听与导航文件又与最新来源复核一致。来源目录未被修改。

快照清单保存在 `bench_data/latest-baseline/manifest.json`。这是采集时点的一致基线；来源仍由另一个任务继续开发，集成前须复查相关文件是否变化。尚未迁入旧任务的产品代码，也未对这个新基线编译或运行功能测试。

两份历史方案已保留，但其中“通过”仅属于旧工作区。旧预算也不能作为新版剩余报价。

## 必须保留的新版行为

- Win+E 由 `--win-e` → `pulse:win-e` 标识，使用独立 `open_win_e`，不能当无参数启动。
- Pulse 图标、此电脑分别使用 `open_pulse_icon`、`open_this_pc`；`DecideLaunchTarget` 与 `new_window_shows` 保留。
- Ctrl+N、显式新窗口/新标签、窗口排列与撤销等近期改动保留。独立新窗口请求不能作为重复请求吞掉。
- 新版目录监视已有 `armed_at`、`WatchClock`、`WaitForStartingWatch`、`ListedSince/NoteArm`。旧版初始 overflow 缺陷是否仍存在，需要在此基线证明，不能覆盖新版 watch/navigation/worker 文件。

## 增量迁移顺序

1. 请求生命周期、拥有请求的队列、独立传输及终态依赖组；保留 v2 兼容，处理重试/拒收/退出。
2. 手工融合实际路由与呈现确认，保留新版三类启动策略、明确新窗口意图和普通文件夹规则。
3. Explorer 不提前隐藏、等待期间不消耗关闭许可、最后身份/路径/选择复核；将呈现与目录监听按新版机制接通。
4. 移植已完成的精确夹具观察与 success/busy/crash 检查；重写 cold Win+E 参数预期。热启动探针仍未完成，不冒充可用测试迁入。
5. 对话框协议/桥接/保存复核另按新版 picker 接口迁移，维持不支持契约回退及生产门禁；旧跨位数结果不直接认证新版。

## 定向验收

每组仅构建受影响目标，检查同 ID 重试、队列失败与终态；三类入口各自规则、普通目录重复及不同目录批次；真实单实例转发与实际呈现；Explorer 成功/忙碌/崩溃、初始 arm/真实 overflow、取消后不得关闭。不要通过减少窗口策略功能、忽略失败或复制旧整文件来得到通过。

完整任务尚未完成；版本对齐不是产品功能验收。


## 2026-10-30 再核对与系统队列迁移

R7/R8真实Edge打开/保存/取消已由用户确认，当前新版桥接证据见third-party-dialog-bridge.md。开始补原始系统入口可靠性缺口前，再次只读核对源树pulse58-integrated-fixes：HEAD仍为9b48b683，Win+E、open_routing、explorer_shortcuts、注册、偏好与相关控制器符合快照；single_instance_coordinator及open_handoff_test与源一致。

源app_main.cpp在快照后有四处驱动器弹出接线增量（include、Volumes失效缓存、WM_CREATE继承暂停、WM_DRIVE_EJECT_READY处理）。内存排除这四块后精确匹配基线hash；未修改源树。这些不是本任务的接收/投递/消费/退出接点，合并时须保留，不可整份覆盖app_main。当前树另有本任务picker_host入口。

第一包限定为拥有载荷的有界系统打开队列：PostMessage仅唤醒；接受记录与入队共同提交；投递失败撤销接受、允许相同ID重试；退出回收未执行请求。保留v2路径、当前Win+E和三类启动偏好。此包只证明排队与派发，不把派发当作实际呈现。后续独立传输、实际路由回执及Explorer关闭保护按上述顺序实施。


### 第一包本机验证通过

system_owned_queue_build.log：主程序、pulse_open_handoff_test、pulse_single_instance_endpoint_test构建成功。system_owned_queue_test.log116项PASS/0FAIL，system_owned_endpoint_test.log29项PASS/0FAIL，均退出0。实际自有message-only窗口验证无载荷wake、真实Post失败回滚、同ID重试、FIFO、64容量、过期时无需UI取队即可释放容量、退出竞态回收；独立进程验证原有端点身份、路径/虚拟标记和清理。

新增状态仅Pending/Queued/Dispatched/Cancelled，非wire，不代表Presented。当前收到ACK只证明队列接受；本阶段没有改注册关联、打开路由或Explorer来源隐藏。下一步后台pipe解决UI未泵消息时的接收，之后仍须实际路由/呈现回执及真实入口闭环。编译期glob发现了尚未接线的独立pipe源；该源将在第二包冻结后重新构建并单独验收，不能由本包构建推导它通过。


### 第二包：后台 RawV2 接收（定向验证与主程序构建通过）

新增open_request_pipe使用当前EncodeOpenRequest的v2 ID/期限/原始路径字节，不引入旧OpenOperation。v1端点布局不变，以独立128字节nonce绑定sidecar声明RawV2；后台服务Start成功后才发布。显式无sidecar的旧端点才走WM_COPYDATA；存在但损坏、身份不符或管道名无法生成均拒绝，不能降级。新通道一旦选定固定目标身份/请求ID/期限；回复丢失也不换通道。Release禁收撤发布后在队列锁外停止服务，再清队列。

独立模块system_raw_pipe_test.log49/0，覆盖有界帧、server实际PID/创建时间、同用户/会话检查、取消drain和停止。审查未见阻塞级问题，但handler必须执行有界且不等待UI；独立模块测试为同进程多线程，不代表其他SID/session实测。server对client核对实际PID、存活及SID/session，不称双向创建时间绑定；单个pending I/O最多约1秒。

集成首轮system_pipe_endpoint_test.log保留两失败。新增精确诊断后，system_pipe_endpoint_retry_test.log42/0，实测直连第一次error231(PIPE_BUSY)、stageNone、may_sent=false；仅这种确定未发送忙或明确Retry按原ID/原5秒期限重试，随后Accepted/Duplicate，恢复UI仅派发1次。没有放宽期望结果。system_pipe_queue_test.log116/0。独立请求者在接收UI不泵消息时仍得到接受，WinE原标记恢复后派发一次，损坏sidecar拒绝、停止不死锁/取消队列均通过。

主程序system_pipe_main_build.log构建成功，无新增编译警告；相关改动diff检查通过。实际默认系统关联、完整Presented、Explorer来源关闭仍未由这些检查证明。

### 第三包：确定性路由与忙碌等待（验证进行中）

普通打开不再根据近期输入或毫秒批次猜测新窗口；保留 Win+E、图标、此电脑的独立偏好及显式新窗口意图。可用目标都忙时排队，首次遇忙固定等待 30 秒，后续重试不延长期限。请求仍计入有界容量，退出取消；失败提示只保存数量，不保存路径或请求身份。

system_deterministic_route_test.log：73 项通过；system_busy_wait_test.log：26 项通过、0 失败；system_busy_wait_queue_test.log：退出 0，未见失败。已覆盖原始接收期限届满后仍保留合法 UI 等待、固定等待到期、重复请求、退出中的占用释放及提示聚合。主程序构建和真实窗口测试尚在进行，不能由模型通过推导实际界面已经通过。本包仍只报告 Dispatched，不代表完整 Presented。

第三包补充验收：system_busy_wait_main_build.log 主程序构建成功；system_busy_wait_windows.log 为实际隔离 Pulse 进程的 external-open-only 测试，18 项 PASS、0 FAIL、退出 0。真实 HWND 验证隐藏/最小化恢复、全部目标被自有模态窗占用时 Deferred 且不打断弹窗、释放后仅派发一个标签、相同 ID 与额外 wake 不重复派发、两个显式新窗口保持独立、清理所有测试窗口。此用例没有验证默认系统关联或 Explorer 的实际关闭，通知卡片视觉尚未截图验收。队列日志确认 116 项 PASS。

阶段四开始处理 Explorer 来源保护：取消提前隐藏；等待不提前提交关闭；真正 Quit 前复核；枚举应用与实际成功呈现分开。阶段三通过不代表此阶段已完成。

第四包实施范围（尚待构建/验收）：Explorer 不再提前修改透明度、窗口样式或任务栏；Wait 不提交关闭；迟到选择保留来源；Quit 前重新核对进程创建时间、窗口标记、目录、选择和单标签。枚举完成只记录列表事实，实际完整帧 EndDraw/Present 均 S_OK 且同代、同路径、同选择的可见 pane 才允许 Ready；失败/遮挡/隐藏截图不得作为成功。

实机验证使用从旧工具手工适配的精确 fixture scope（当前请求协议，不沿用旧 owned-queue 状态），仅 Default 桌面、bench_data 直属新建目录、唯一窗口身份和指定来源允许接管。当前 Explorer 忙碌分支仍是取消交接并保留来源，不是第三包普通请求的 30 秒排队；两条路径不可混称已经统一。默认系统关联和全局第三方启用仍未验收。

第四包定向模型验证：system_source_guard_model_build.log 构建 handoff、控制器定向测试及精确来源 driver 成功；system_source_guard_handoff_test.log 135 PASS/0 FAIL；system_source_guard_policy_test.log 仅 --explorer-takeover 子集 12 PASS/0 FAIL。主程序实机链路尚待构建和运行，不将这些模型断言计作实际 GPU 呈现或 Explorer 关闭通过。

提示卡片视觉检查完成：system_notice_visual_test.log 退出 0，真实 TickExternalOpen 显示混合失败计数，150% DPI、700×630 surface 的卡片标题/正文/关闭图标无截断或重叠，测试窗口自动清理。截图 bench_data/system-notice-bf60be491c2341df8ae7effa1cca11aa/external-open-notice-20320-175139531.png 来自实际 compositor surface；仅认证该卡片本机尺寸/缩放，不扩称全部主题/语言/桌面合成截图通过。

第四包真实成功链路首轮未通过：system_source_guard_real_success.log 退出 1。driver 精确识别新建来源并验证 selection；Pulse 子进程正常退出，但 ui_received=0、presented=0、handoff=-1，Watcher线程创建/停止均记录，未发出关闭授权。187次授权前采样均来源可见可用、样式不变；停止确认后driver重新复核身份并仅清理该来源。此结果证明保护未提前关闭来源，不能算成功接管；正在补 Watcher Start/Scan 的有界拒绝诊断。

第二轮诊断仍失败（system_source_guard_real_diagnostic.log），已定位到 IShellWindows::Item 返回 S_FALSE/null 后被当作不完整枚举，Watcher Start 中止。官方接口说明将 S_FALSE 定义为指定项未找到，Folder 为 NULL；本轮修复将仅该确定缺项结果跳过，S_OK/null、其他异常与不完整身份读取仍拒绝接管。官方依据：https://learn.microsoft.com/en-us/windows/win32/api/exdisp/nf-exdisp-ishellwindows-item 。本轮190次关闭授权前采样来源保持可用，观察器停止后精确清理；不是成功接管通过。

缺项修复后：system_source_guard_absent_policy.log 定向15项通过。system_source_guard_real_absent_fixed.log 仍退出1，但已实际 handoff-submitted、ui_received=1；后续 Cancelled/presented=0，正在诊断UI导航/呈现取消点，不将原因先归给初始监听。185次采样来源保持可用，最终精确清理。

独立保护分支实机通过：system_source_guard_real_busy.log 退出0，busy-cancel、13次采样和终点来源可见可用，未发出关闭授权；system_source_guard_real_crash.log 退出0，实际UI接收后仅终止自建Pulse job（退出码77），19次采样和退出后来源可见可用、无关闭授权。两轮观察器/测试子进程结束确认后仅清理精确来源。这些通过不替代尚失败的成功接管链路。

UI诊断轮 system_source_guard_real_ui_diagnostic.log 明确取消点：watch-overflow detail=1（armed通知）时 pending=4/applied=0/lease=4/listing_complete=0，紧接 监听触发的 Background refresh-reset，最终applied=5而handoff已Cancelled。由此确认取消发生在初次监听通知后的补读衔接，但后续诊断仍需区分纯 arm 与同时到达的真实文件变更，不能仅凭该时序认定全部由初次 arm 导致。修复限定初次armed补读可继承同路径/尚未完成listing/原期限的lease；用户刷新、真实overflow、路径或选择改变和到期不得继承。仍需新代枚举和实际Present，不能直接标Ready。184次来源采样保持可用，来源已精确清理。

初次移交补丁首轮模型147项通过、主程序构建成功，但system_initial_arm_real_success.log仍退出1。新时序是armed通知后旧listing-record-before detail=3，随后listing_complete=1，再refresh-reset；没有initial-arm-transfer/rebound记录。来源184次采样保持可用并精确清理。当前补充资格位与selection revision诊断区分原因，不能把此轮失败改写为“初次补读已修复”。

进一步诊断system_arm_direct_real.log：初次arm记录时revision一致、资格255；随后结果阶段arm_missed=false而dirty=true（资格254），选择仍未改变。因此未放宽真实变更保护。代码核对发现旧测试把takeover-ready.tsv等控制记录写在同一个被监听fixture目录，测试自身可能制造真实事件。现正在分离control_root与source子目录：所有状态/偏好/报告在root，真实Explorer只打开预先创建的source；成功断言、身份与关闭条件保持。没有事件文件名证据前不把该假设写成已确认唯一根因。

### 第四包真实链路通过（控制目录与来源分离后）

system_source_isolation_build.log 主程序及driver构建成功。system_source_isolation_real_success.log退出0：ui_received=1、presented=1、handoff=Closed4、production_finish_close=1且source_alive=0；实际generation3列表/选择经过成功帧确认，15次授权前采样来源保持可用，4次授权后采样，正常STOPPED=1。该轮listing已覆盖arm，因此没有触发arm lease移交；移交仍有147项模型中的专项证据，不冒称此轮实机覆盖了该分支。

因夹具结构变化，定向重测保护分支：system_source_isolation_real_busy.log退出0，busy-cancel，25次采样与终点来源可见可用且无关闭授权；system_source_isolation_real_crash.log退出0，UI收到后仅终止自有Pulse job，10次采样与退出后来源保持、无关闭授权。所有测试来源已经关闭或精确清理。

分离后同样arm路径的dirty保持false并完成成功链路，支持旧测试向来源目录写控制记录导致干扰的判断；此前失败全部保留，不称已捕获旧事件具体文件名。后续阶段将Explorer也迁入拥有载荷的统一队列，固定10秒源期限，忙碌恢复一次执行；目前busy仍取消保留，不能把本阶段写成该统一队列已完成。

### 第五包开始：统一 Explorer 请求所有权

在第四包实机通过的基础上，将 Explorer 内部上下文接入现有有界队列，而非另建队列。原始 RawV2 协议不携带内部来源上下文；来源仍固定10秒期限，普通打开仍30秒遇忙等待。后台使用可撤销提交端口，停止/退出后不得凭裸 coordinator 引用提交；消息只唤醒、不携带堆指针。UI首次领取才Receive，忙碌归队后不重复Receive，成功导航一次后由lease接管、真正Present才允许关闭来源。

验收重点包括晚提交/退出、容量/投递失败、来源类型隔离、同期限不续、忙碌到期和busy→恢复只导航一次。此段是正在实施的范围，不是通过记录。

第五包构建成功，统一队列专项system_unified_explorer_queue_test.log46/0；普通等待26/0、handoff147/0、端点42/0均退出0；普通实窗18项通过。审查修复了Dispatched Pending/Ready lease被Release或重复/撤销提交误取消的边界，独立复审通过。

system_unified_real_success.log实际统一队列成功：receive_attempts=1/received=1/dispatched=1/presented=1/Closed、tabs1→2，152次授权前采样来源保持，退出0。

system_unified_real_busy_resume.log首轮失败：receive1/deferred4/dispatched1/resumed1/tabs1→2，但未Presented、applied_generation0、handoffExpired；parent watchdog也失败，随后子进程正常退出0不能抹掉watchdog失败。252次采样来源保持，已经精确清理。初始COM准备约17s，Pending期间scope测试反复完整枚举又增加开销；另probe总预算从WM_CREATE计，可能与晚收到的请求冲突。正在仅减少Pending测试枚举自扰、补阶段时间并分离测试准备预算，生产10秒期限和准入/Ready/最终关闭门禁不变。未据单轮认定唯一根因。

### 第五包最终定向验收（2026-10-30）

system_unified_timing_build.log 主程序构建成功。修正测试准备预算并减少 Pending 阶段测试 scope 的重复 COM 枚举后，生产原始 10 秒期限、准入/Ready/最终关闭核对保持不变。

- system_unified_busy_resume_timing.log：退出0，received=1、deferred=9、dispatched=1、presented=1、Closed；tabs1→2，原期限内完成，20次授权前和3次授权后采样。
- system_unified_real_busy_expiry.log：退出0，received=1、deferred=64、dispatched=0、Expired；tabs1→1，102次采样来源保持可用，未授权关闭。
- system_unified_real_crash.log：RESULT0；请求接收后只终止测试自有 Pulse job，退出77；12次采样及终点来源保持可用，未授权关闭。
- system_unified_final_success.log：退出0，received=1、presented=1、Closed，production_finish_close=1/source_alive=0；77次授权前和28次授权后采样。实际呈现后才关闭来源。

各轮在观察器/测试进程停止后精确清理自有来源。采样不能证明每毫秒状态；不扩称所有桌面、负载、主题或默认注册入口均已验证。首轮 busy-resume 失败日志保留，不以新通过覆盖失败记录。第五包统一拥有载荷的接收、忙碌等待和退出保护在本次定向范围完成；默认关联及第三方日常启用继续推进。

### 第六包：最新版入口命令验证开始

system_latest_entry_registry_build.log 定向构建通过；system_latest_entry_registry_test.log 94 PASS/0 FAIL，system_latest_entry_registry_recovery_test.log 679 PASS/0 FAIL，两进程退出0。测试均先将 HKCU 重定向至专属测试根，恢复/删除也限定该根；未改日常默认关联。确认新版 WinE 注册命令保留 --win-e，图标/此电脑/WinE 三个偏好仍独立。

尚待真实命令启动到呈现的 cold/hot 端到端验证；旧工作区的四类 cold 和 CLASSKEY 证据不能沿用。当前用户层上述五个 command 键只读查询均不存在，这不排除机器级/继承关联，也不等于已确认实际桌面路由。没有据此自动写入全局关联。

文档定向 diff 检查无问题。全工作区 diff --check 报 catalog.tsv 的末尾空 tw 列（含已有与本次条目）；该列是六列 TSV 格式，不为消除空白提示而删除字段。未宣称全工作区 diff --check 通过。

第六包 cold 命令闭环通过：system_latest_cold_entry_build.log 构建成功；system_latest_cold_entry_test.log 四个 kind 均 PASS、各自 ran=1/exit=0/owned_job_drained=1，总退出0。包含含空格/中文/&/括号的文件夹路径、此电脑、回收站、WinE。WinE relay 实际核对 --win-e，Pulse记录初始 pulse:win-e 并核对当前偏好选择的目标；该轮目标为此电脑，实际呈现2帧、pending0/applied1。未用无参数启动替代 WinE。

测试先在nonce绑定独立HKCU映射根生成/恢复命令，成功解除映射并清理后才启动专属job里的relay与Pulse；成功报告须nonce/kind、argv、真实非hidden EndDraw/Present S_OK及加载目标共同匹配。未执行真实桌面双击/Win+E按键，不认证默认关联选择，也不认证hot单实例。下一包补真实第二进程投递的隔离hot验证。

### 第七包：真实 hot 单实例闭环通过

新增只读 attempted-ID/Dispatched 观察，不改变 wire、接受或路由策略；Dispatched 不冒充 Presented。首轮 system_latest_hot_entry_build.log 构建成功但出现hot probe在SELFTEST=OFF下的C4100，当前构建也无法使用原SELFTEST环境数据目录；未在该状态运行hot测试。已改为完整nonce/直属canonical fixture/同exe/真实argv/共享mapping PID验证成功后才发布进程内专用目录，GetPulseDataDir优先无副作用读取；没有将任意环境路径开放到普通运行。

system_hot_private_data_build.log 定向重建成功，无新增warning。system_latest_hot_endpoint_test.log 42 PASS/0 FAIL、退出0。system_latest_hot_entry_test.log 17项/0失败、退出0：真实Acquire私有命名空间，不用--test-instance；A两次顺序及两次并发第二进程请求逐ID确认Dispatched+实际loaded frame，均1窗1tab；B真实呈现后1窗2tab；--win-e在私有偏好open_win_e=1/open_pulse_icon=0/new_window_shows=1下呈现此电脑，1窗3tab。各sender退出0，primary仍存活；最后nonce stop后primary退出0，owned job无剩余进程。

fixture为bench_data/hot-entry-{487C63AB-F672-435F-8D85-66A1AEB7F447}，包含逐sender ID/PID/frame/count/exit报告。观察范围是该fixture和策略组合，不认证实际桌面默认关联、物理Win+E、任意负载或所有偏好组合。生产策略保持最新版独立图标/此电脑/WinE行为。

数据目录接点直接影响启动读取分支，故仅复查四cold：system_cold_after_private_data_test.log四类全PASS、各exit0/drained1，总退出0。未扩展全套历史测试。

