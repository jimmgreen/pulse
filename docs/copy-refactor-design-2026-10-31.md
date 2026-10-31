# Pulse v1.0.58 参考 TeraCopy 的复制提速重构方案

版本基线：Pulse v1.0.58  
源码基线：be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26  
整理日期：2026 年 10 月 31 日  
用途：复制功能重构设计、任务拆分与验收参考

## 结论和实施顺序

**建议借鉴 TeraCopy 的分阶段流水线和设备感知调度，但不要第一步就替换 CopyFile2。** Pulse v1.0.58 已经有小文件线程池、部分直写路径和大文件无缓冲策略。更值得先做的是：把扫描、冲突处理、复制、验证、提交和 UI 的成本分别量出来，消除已有控制路径上的重复工作，再逐步扩大调度能力。

推荐按下面的顺序推进：

1. **保持行为不变，补齐阶段计时、错误分类和正确性回归。**
2. **优化冲突与目录探测热路径**，避免重复全量统计；保持目标竞争和同文件保护。
3. **统一大小文件调度**，按源目标设备组合限流，不再只让严格小于 1 MiB 的文件享受线程池。
4. **引入有界枚举流水线**，缩短从开始操作到第一个文件真正写入的时间；不伪造扫描未结束时的总进度。
5. **拆出有界验证阶段**，避免开启校验就整体失去并发；明确不同哈希和耐久性语义。
6. **只有测量证明有必要时，才试验自定义块流水线**，实现边读源数据边计算哈希；CopyFile2 继续作为默认路径和兼容性基线。

用户指出 TeraCopy 是其他用户推荐的参考方向，并认为推荐来自实测体验。这是重要的产品线索，足以支持认真研究它的设计，而不是把反馈当作错觉。不过，目前没有同一机器、同一文件集、同一验证策略的原始对比数据，因此本方案**不宣称 TeraCopy 在所有场景必然更快，也不承诺重构后提升某个百分比**。

本次产出是设计文档：未修改仓库，未编译，未运行复制基准，未执行磁盘清理。下文参数均为待测试的设计起点，不是测量结论。

---

## 1 先明确我们要优化什么

复制功能至少有四个不同目标，不能只用一个 MB/s 掩盖问题。

| 目标 | 主要指标 | 用户感受 | 常见误判 |
| --- | --- | --- | --- |
| 尽快开始 | 请求到第一笔目标写入的时间 | 点击后立即有动作 | 把后台扫描等待叫作 UI 卡死 |
| 尽快完成 | 端到端耗时、有效吞吐、文件每秒 | 整个任务更早完成 | 只看中间瞬时速度，排除扫描和验证 |
| 持续可响应 | 暂停和取消响应、UI 更新延迟 | 不假死、操作可控 | 只增加进度刷新频率，反而抢锁 |
| 数据与行为正确 | 内容、元数据、冲突、失败和撤销结果 | 没有误覆盖、没有假成功 | 关掉验证或弱化覆盖保护后宣称提速 |

建议正式定义两类时间：

- **用户等待时间**：发起复制至所有文件到达终态、必要验证与提交结束。公平比较优先看这一项。
- **数据搬运时间**：实际数据复制阶段的耗时。用于定位瓶颈，不能替代端到端耗时。

首轮目标不是“手写一个比 Windows 更快的复制 API”，而是让 Pulse 的调度和控制路径不成为额外瓶颈，同时保持现有正确性。

## 2 证据分级与当前实现事实

### 2.1 三类结论必须分开

- **源码已确认**：针对上述固定 commit 的静态审查结论。证明存在该路径，不证明它在用户机器上占用了多少时间。
- **TeraCopy 官方设计**：Code Sector 文档描述的能力和架构。可借鉴思路，不代表拿到了其源码，也不能据此推断内部每一处实现。
- **待验证假设**：对 Pulse 瓶颈、线程数、缓冲参数或收益的推断。只有分阶段指标和同条件测试才能确认。

### 2.2 Pulse v1.0.58 已有能力与值得检查的地方

| 项目 | 源码已确认的现状 | 对重构的含义 |
| --- | --- | --- |
| 文件复制核心 | 使用 CopyFile2 | 不要假定当前是低效的逐字节用户态循环 |
| 默认校验 | verify 默认 false | 默认复制慢，不能先归因于 SHA256 |
| 开启校验 | 会关闭当前小文件线程池；复制后完整读取源和目标做 SHA256 | 校验模式存在明确额外读量，并影响调度；值得独立重构 |
| 扫描 | 先建立完整 TransferEntry 向量，再进入 Running | 到首字节存在全量扫描屏障；扫描在后台线程，不等于 UI 主线程阻塞 |
| 枚举 API | 已用 FindFirstFileExW 和 LARGE_FETCH，缓存大小和时间 | “换更快枚举 API”不是未经验证的首要方案 |
| 目标目录缓存 | TargetListing 当前主要用于根层 | 已存在的深层目录仍有逐文件探测机会 |
| 冲突统计 | remaining_conflicts 每次遍历剩余项 | 多次冲突提示时有潜在平方级控制成本，应先消除重复统计 |
| 小文件池入口 | 至少 16 个合格文件，且每个严格小于 1 MiB | 正好 1 MiB 的文件不入池；不要把 600 × 1 MiB 当成已覆盖场景 |
| 默认并发 | SSD 8、远程 4、可移动 1、其他 2；源目标取较小值 | 已有设备感知；但设备分类、多个源与混合任务仍需改善 |
| 非小文件 | 先延后处理，待小文件提交后再处理 | 大文件可能与尚未完成的小文件重叠；不是“所有大小文件绝对串行” |
| 无缓冲策略 | 大于等于 64 MiB 时按设备或内存条件决定 | 不应无差别对所有文件加 COPY_FILE_NO_BUFFERING |
| 参数失败回退 | ERROR_INVALID_PARAMETER 分支同时去掉 COPY_FILE_COPY_SYMLINK 与 COPY_FILE_NO_BUFFERING | 需要按能力拆开诊断，不能让性能回退悄悄改变链接语义 |
| 临时文件 | 无冲突且小于 64 MiB 已有直接写最终路径的路径 | “删掉所有临时文件”既重复已做优化，又可能破坏覆盖保护 |
| 耐久性 | Copy 路径没有 Move 路径同样的耐久性刷新步骤 | 不要把 Move 的 flush 成本算给 Copy |
| 进度和通知 | 回调约 100 ms；通知约 50 ms 合并，再 PostMessage 到 UI | 已有节流，不能笼统断言每个字节都刷新 UI |
| 进度窗口 | 延迟约 2 秒出现，且大于等于 90% 时可能抑制出现 | “看不到进度窗口”不等于磁盘没有在工作 |
| 目录元数据 | 完成时有目录元数据应用，但调用处未检查返回值 | 重构时要保留逻辑并明确报告失败 |
| Shell namespace | 有独立路径 | 本文原生文件系统重构不能顺手替换它 |

代码位置见第 17 节。以上热点优先级来自结构分析，**不是已经测到 CPU、磁盘或锁的排名**。

### 2.3 不要复用过时的性能结论

历史 issue 60 针对 v1.0.46 的临时文件成本问题，只能作为查找历史设计的线索。v1.0.58 已经包含部分直写优化，不能把旧版本的倍数结论贴到当前版本。

issue 454 中关于 USB 和无进度的反馈没有足够版本与复现实验信息，不能据此断定当前版本一定存在相同根因。应把它转为复现用例，而不是当成已定位 bug。

---

## 3 从 TeraCopy 借鉴什么

### 3.1 官方设计能支持的方向

TeraCopy 的较新官方选项文档描述了读线程、写线程、缓冲以及独立的替换和数据库工作，并会对 HDD 自动采用单线程策略。官方已完成的多线程功能条目也描述了“读取数据块并计算哈希，再交给写入线程”的思路。这支持以下方向：

- 把数据处理和协调工作分开，而不是让一个大循环包办所有阶段。
- 通过有界缓冲连接读、哈希、写，而不是无限扩线程。
- 区分 HDD 与 SSD，避免随机寻道抵消并发收益。
- 将覆盖替换和任务记录集中协调，避免工作线程直接修改全局状态。
- 允许用户明确选择验证算法与元数据策略。

官方提速文章提到复制时计算源数据哈希，以及 xxHash64、BLAKE3 等选择。该文章较早的“多线程即将推出”描述，不能覆盖较新的选项文档和已完成的功能条目。[T1][T2][T3]

**接下来提出的模块、接口、队列、数值和回退策略都是 Pulse 的建议设计，不是声称复刻了 TeraCopy 私有实现。** TeraCopy 支持的多目标线程结构也不意味着 Pulse 必须在这次重构里增加多目标复制产品功能。

### 3.2 三种并发不要混为一谈

| 机制 | 并发对象 | 主要改善对象 | 主要风险 |
| --- | --- | --- | --- |
| 文件级并发 | 多个完整文件同时执行 CopyFile2 | SSD 上的小中型文件、部分远程场景 | HDD 寻道、句柄和队列过多 |
| 文件内块流水线 | 同一文件的多个数据块处于读、哈希、写不同阶段 | 大文件、需要边读边哈希的自定义引擎 | 对齐、顺序、短写、取消、元数据复杂度 |
| 枚举与执行流水线 | 扫到一批文件就开始复制，同时继续扫描 | 大目录首字节延迟、全量预扫内存 | 总量未知、冲突数量变化、目录依赖 |

先把文件级并发与枚举流水线做好，通常比直接重写全部 I/O 路径更容易控制风险。是否值得增加文件内流水线，必须由剩余瓶颈决定。

---

## 4 建议架构

### 4.1 数据路径与控制路径分离

```text
用户请求
  │
  ▼
JobController ─── PauseCancelToken ─── ProgressAggregator ─── UI
  │                       │
  ▼                       │
Enumerator ── 有界条目队列 ── Planner / ConflictResolver
                                  │
                                  ▼
                         DeviceAwareScheduler
                                  │
                 ┌────────────────┴────────────────┐
                 ▼                                 ▼
           CopyFile2Backend               BlockPipelineBackend
             默认与兼容路径                   实验开关路径
                 └────────────────┬────────────────┘
                                  ▼
                           VerificationStage
                                  ▼
                          MetadataAndCommit
                                  ▼
                       ResultJournal / UndoRecord

目录依赖
  父目录先创建 → 子项可执行 → 全部子项结束 → 目录最终元数据
```

模块边界：

- **Enumerator**：枚举源项及已有元数据，只产出条目，不弹窗、不复制、不写 UI。
- **Planner**：确定源目标对应关系、目录依赖、对象类型、提交方式和候选后端。
- **ConflictResolver**：合并冲突请求、维护“全部应用”决策，保留最终执行前的竞态检查。
- **Scheduler**：对设备组合、文件大小、内存和验证读流量共同限流。
- **CopyBackend**：只负责执行一个已经批准的复制计划，报告结构化进度和结果。
- **VerificationStage**：使用显式策略验证，不把“校验关闭”或“算法更换”当隐藏优化。
- **MetadataAndCommit**：控制替换、元数据完成判定和最终发布。
- **ResultJournal**：记录每个文件真实结果，产生正确的部分成功、撤销和重试信息。
- **ProgressAggregator**：聚合计数；UI 读取快照，不在回调中遍历文件列表或拼接大量字符串。

### 4.2 核心接口草图

下面是设计伪代码，不是可直接编译的 Win32 封装；具体类型和错误模型应沿用仓库约定。

```cpp
enum class VerifyMode {
    None,
    Sha256Compatibility,    // 保留现有 SHA256 契约
    XxHash64Accidental,      // 显式选择，只针对意外损坏
    Blake3Integrity         // 显式选择的密码学哈希选项
};

enum class BackendKind { CopyFile2, ExperimentalBlocks };
enum class CommitMode { DirectCreateNew, StagedCreateNew, StagedReplace };
enum class ItemOutcome {
    Success, SuccessWithWarnings, Skipped,
    Failed, CancelledBeforeCommit, CommittedBeforeCancel
};

struct CopyPolicy {
    VerifyMode verify;
    MetadataPolicy metadata;
    SourceMutationPolicy sourceMutation;
    DurabilityPolicy durability;
    bool allowExperimentalBlocks;
    bool allowStreamingEnumeration;
    uint64_t globalBufferBudgetBytes;
    uint32_t maxOpenFiles;
};

struct TransferEntry {
    ItemId id;
    ItemId parentDirectoryId;
    Path source;
    Path requestedTarget;
    EntryKind kind;
    EnumeratedMetadata metadata;  // 枚举所得是快照线索，不是最终真相
};

struct CopyPlan {
    TransferEntry entry;
    DevicePair devicePair;
    BackendKind backend;
    CommitMode commit;
    ResolvedConflictPolicy conflict;
    SourceIdentityHint expectedSource;
    Optional<TargetIdentity> targetApprovedForReplace;
};

struct CopyResult {
    ItemOutcome outcome;
    uint64_t logicalBytesCopied;
    Optional<Digest> sourceDigest;
    Optional<CommittedTargetIdentity> committedTarget;
    vector<StageError> errors;
    vector<MetadataWarning> warnings;
};

interface ICopyBackend {
    CapabilityResult supports(const CopyPlan&, const CopyPolicy&);
    CopyResult execute(const CopyPlan&, ExecutionContext&);
};

interface IConflictResolver {
    Resolution resolve(const ConflictSnapshot&, DecisionScope);
};

interface IProgressSink {
    void addBytes(ItemId, Stage, uint64_t delta); // 轻量，不操作 UI
    void markTransition(ItemId, ItemState);
};
```

关键约束：

1. 后端不能自行降低 verify、metadata、symlink 或 overwrite 语义。
2. 所有用户决策和回退理由必须体现在计划与结果中。
3. 失败结果要携带阶段、系统错误、是否已提交、是否有本任务临时文件，不只返回 false。
4. 后端选择在执行前完成。执行中回退必须满足第 11 节的安全前提。
5. 路径只用于定位；涉及替换、清理、同文件判断时，尽可能使用已打开句柄及稳定身份信息。

---

## 5 先改冲突和目标探测热路径

### 5.1 避免重复遍历所有剩余项

当前 remaining_conflicts 的调用结构存在重复扫描剩余项的风险。把它替换为一次计算或增量维护，而不是每次弹窗都重新统计。

完整预扫模式可采用：

```text
首次建立条目和冲突分类
  pendingKnownConflicts = 已发现且尚未解决的冲突数

每次决策、跳过、提交、失败
  原子或协调线程内更新该条目的状态
  对统计做一次增减
  UI 读取统计快照
```

流式枚举模式不能提前知道最终冲突数，应显示“已发现 N 个冲突”，不要显示假的“剩余全部冲突数”。若用户必须在执行前看到完整冲突清单，保留完整规划模式。

“全部替换”“全部跳过”要有明确作用域：当前任务、文件类型、目录冲突类型等。不能让目录合并决策误用于文件替换，也不能因分批枚举丢失已经作出的任务级决策。

### 5.2 按目标目录维护短生命周期缓存

建议为本任务正在处理的每个目标目录维护：

- 名称及类型的枚举结果；
- 已由本任务创建、提交、删除的条目；
- 缓存代次和失效状态；
- 尚未确定的项，而不是把“没查到”当成永久不存在。

缓存的价值是减少**不必要的重复查询**，不是替代安全检查：

- “目标不存在”最后必须通过 fail-if-exists/create-new 等排他语义确认。
- 已批准覆盖的目标若在期间被替换，应重新判断身份与策略，不能覆盖新出现的无关文件。
- 同源同目标、硬链接别名、大小写和重解析点相关情形，不能只靠字符串路径不同就判定安全。
- SMB 断线、权限变化、重解析或其他进程改动时，缓存必须可失效。

建议把 PathExists、SameFileObject、目录枚举、冲突判定计数暴露出来。验收是降低重复工作并保持竞态测试正确，而不是只看是否少调用了 API。

---

## 6 调度器重构

### 6.1 不再把小于 1 MiB 当唯一并行入口

现有严格小于 1 MiB 的条件会让正好 1 MiB 和数 MiB 文件走不同路径。建议用同一个调度器管理所有普通文件，以文件大小和设备预算决定并发，而不是两个相互脱节的队列。

可作为测试起点的分类：

- tiny：小于 64 KiB；
- small：64 KiB 至小于 1 MiB；
- medium：1 MiB 至小于 64 MiB；
- large：大于等于 64 MiB。

这些边界只是便于实验与日志切片的起点。不要在未测试前把它们固化为产品性能承诺。

调度应同时关注：

- 源设备、目标设备及其是否实际共享物理介质；
- 单文件大小与剩余字节；
- 读写方向和正在进行的验证读；
- 队列延迟、每文件尾延迟、吞吐、错误和取消延迟；
- 句柄、内存、CPU 和后台任务竞争。

### 6.2 起始并发应保守且有上限

| 场景 | 初始测试建议 | 为什么 |
| --- | --- | --- |
| 同一机械盘读写 | 1 个活动复制，验证尽量不争抢 | 防止读写及多文件随机寻道 |
| 跨机械盘 | 先 1，再对比 2 | 多盘可重叠，但不能假定控制器与磁盘都能获益 |
| SSD 到 SSD | 从 2、4、8 的对比开始 | 找到吞吐收益与队列、CPU、取消延迟的平衡 |
| USB 闪存 | 从 1 开始，测稳定速度及尾延迟 | “可移动”并不代表高并发能力 |
| USB 外置 SSD | 独立于廉价 U 盘建档 | 连接总线相同不代表介质性能相同 |
| SMB | 从 1、2、4 对比，记录网络和服务器限制 | 客户端线程数无法突破链路和服务器瓶颈 |
| 未知或混合多个源 | 保守预算，逐源分类 | 不能仅使用首个源文件代表整个任务 |

这是建议实验矩阵，不是新的固定默认值。现有 SSD 8 等策略应作为对照保留。

设备识别不能只看盘符。不同分区可能在同一物理盘上；网络共享可能落在同一服务器磁盘上；底层拓扑无法可靠识别时应回到保守预算，而不是假装知道。

### 6.3 加入公平性和背压

- 维护大小文件队列，但统一消耗设备 token，避免小文件无限插队使大文件饥饿。
- 大文件不必等全部小文件完成才获调度，也不要一次让多个大文件压满同一 HDD。
- 对复制和验证共用设备预算，避免复制 8 路再额外验证 8 路。
- 枚举队列达到容量或元数据内存预算时暂停生产；不要只依赖现有队列容量作为整个任务的内存上限。
- 执行队列要同时受最大条目数、最大活动文件数与字节预算约束。
- 调度策略调整应发生在安全边界，使用平滑指标与滞后区间，不在每个采样点升降线程数。
- 自适应策略先作为实验功能；静态可解释策略更容易定位回归。

---

## 7 枚举与复制流水线

### 7.1 推荐保留两种规划模式

| 模式 | 行为 | 适用条件 |
| --- | --- | --- |
| 完整规划 | 扫描结束后执行 | 用户需要完整确认、精确预估或特殊操作 |
| 流式规划 | 扫描和执行重叠，总量逐步增长 | 普通复制，希望降低首字节等待 |

不要简单删除预扫，然后继续显示一个看似精确的百分比。流式模式的 UI 应展示：

- “正在扫描并复制”；
- 已发现文件数和已完成文件数；
- 已发现字节与已复制字节；
- 扫描结束前总量仍在变化；
- 扫描结束后才转为基于完整总量的百分比和更可信 ETA。

源目录枚举并不是文件系统快照。复制期间新增、删除、修改的条目如何处理，必须定义为 best-effort、检测后报错或使用受支持快照，不能默认声称得到某一时刻的完整镜像。

### 7.2 目录依赖不可丢

正确顺序：

1. 创建或确认父目标目录。
2. 子文件与子目录才进入可执行状态。
3. 等待该目录下所有已枚举子项到达终态，并确认枚举已结束。
4. 最后应用目录时间戳、属性及所需安全信息。
5. 记录目录元数据错误，不能静默成功。

不应过早设置只读属性或限制访问的 ACL，导致后续子项无法创建。目录最终元数据不能在最后一个“当前已知子项”完成时就应用，因为枚举器可能还会发现新项。

### 7.3 协调器伪代码

```text
start job
start enumerator with bounded queue

while enumeration not finished or active work exists:
    check cancellation and pause

    drain a bounded number of discovered entries
    register parent-child dependencies
    resolve or enqueue conflicts without holding global copy locks

    for each schedulable entry:
        require parent directory ready
        require device, memory and handle budgets available
        dispatch one item

    consume completed item results
    update result journal and budgets

    for each directory whose enumeration ended and children settled:
        apply final directory metadata
        record success, warning or failure explicitly

    publish a coalesced progress snapshot

settle all in-flight I/O
finalize partial-success summary
mark job terminal only after children and cleanup settle
```

枚举器、UI 线程与工作线程都不能在持有全局锁时等待冲突弹窗或队列空间。为取消预留唤醒机制，否则有界队列很容易在生产者和消费者互等时死锁。

---

## 8 验证模式重构

### 8.1 先保留算法契约，再优化执行方式

现有 verify 默认关闭，所以需要分别测两条路径：

- 不验证的兼容路径；
- 开启 SHA256 的现有验证路径。

复制后完整读一遍源和目标，逻辑上多出了两遍读取。实际设备 I/O 是否真的发生、是否命中缓存，需要另测，不能把逻辑读量直接等同于物理磁盘读量。

第一步可做：

1. 将验证从“大循环中的串行尾部”拆为独立阶段。
2. 保留 SHA256 结果语义。
3. 不再因为开启验证就一刀切关闭所有文件复制并发。
4. 验证与复制共用设备预算，特别保护 HDD 和 USB。
5. 在 API 允许的前提下复用线程级 provider 和缓冲资源；每个文件的哈希状态独立初始化和释放，不能跨文件或跨线程串用。
6. 单独统计源校验、目标校验、等待和 CPU 时间。

### 8.2 只有自定义数据路径才能直接做源哈希融合

CopyFile2 的进度回调提供进度信息，不提供可供调用方哈希的原始文件数据块。因此：

- 不能在 CopyFile2 回调里“顺手对这次读出的块算哈希”。
- 同时另开一个线程读源并计算哈希，依然是额外源读取，不是免费融合。
- 真正的源哈希融合需要自己拥有读缓冲：读源块 → 更新哈希 → 交给写线程。

这正是实验性块引擎的主要理由之一，但它也把 Win32 复制语义的维护成本带回应用层。

### 8.3 算法选择必须透明

| 算法策略 | 适用用途 | 需要明确的边界 |
| --- | --- | --- |
| SHA256 | 兼容已有校验格式和用户期望 | 不能悄悄替换掉 |
| xxHash64 | 检测常见意外损坏、追求低计算开销 | 非密码学哈希，不提供抗恶意碰撞保证 |
| BLAKE3 | 需要密码学哈希且希望评估速度 | 算法、输出长度、库版本和日志格式要显式定义 |
| 不验证 | 只执行复制 API 的成功检查 | 不能展示成“内容已校验” |

即使使用密码学哈希，“源与目标摘要相同”也不自动证明内容来自可信发布者；真实性还需要可信的参考摘要或签名。

### 8.4 目标重读与耐久性是不同保证

- 源哈希融合最多消除额外的源数据校验读取，不能自动消除目标重读。
- 不重读目标而只哈希提交给写入 API 的缓冲，证明的是“准备写什么”，不是“从目标重新读到什么”。
- 目标重读可能命中系统或设备缓存，不等于断电后仍能读回。
- 是否调用刷新、何时刷新、是否要求设备兑现，属于独立 DurabilityPolicy。
- Copy、Move、覆盖替换的安全要求不能混为一谈；本文不建议为追求跑分移除已有 Move 保护。

### 8.5 源文件在复制期间变化的政策

至少提供并测试一种明确政策：

- **兼容的尽力复制**：记录风险，前后比较身份、大小和时间；发现变化则不报告“稳定源校验成功”。这些属性未变化也不能严格证明源没变。
- **需要稳定源**：在平台允许时以受约束共享方式保持读取句柄，阻止新写入者；若已有写入者或特殊对象导致无法建立保证，失败或要求用户选择。
- **快照来源**：只有实际使用并验证受支持的快照机制时才宣称快照一致性。

严格模式要确保复制和哈希面对同一个已建立身份的源对象，不能“先检查路径 A，再让后端重新打开已被替换的路径 A”后仍声称强一致。

---

## 9 实验性块引擎设计

### 9.1 开发前置条件

只有满足下列条件才值得做：

- 基线指标显示大文件或验证阶段存在当前方案无法消除的额外开销；
- CopyFile2 默认路径已建立稳定的正确性回归；
- 可以列出自定义后端支持和不支持的文件类型；
- 有独立实验开关，可按任务回退；
- 不会为了跑通普通文件而悄悄丢弃 ADS、ACL、稀疏属性、重解析点等语义。

第一版建议只支持明确列出的普通文件场景。其他场景执行前走 CopyFile2 或原有路径。不要在没有等价性测试时支持所有 Windows 文件类型。

### 9.2 缓冲池而不是无限分配

```text
FreeBufferQueue
      │
      ▼
读取源块
      │  offset、validBytes、sequence
      ▼
按文件顺序更新源哈希
      │
      ▼
ReadyToWriteQueue
      │
      ▼
写入对应偏移并确认完成
      │
      ▼
归还缓冲 → FreeBufferQueue
```

建议实验参数：

- 缓冲块先对比 256 KiB、1 MiB、4 MiB；
- 每个活动文件先对比 2、4 个缓冲；
- 明确全局池预算，而不是“线程数 × 块数”无限增长；
- 未用满的尾块保留实际长度，不能把补齐字节算入文件内容和摘要。

这些值仅是扫参起点。内存估算至少包括：

```text
数据缓冲 ≈ 活动文件数 × 每文件缓冲数 × 块大小
总内存 = 数据缓冲 + 队列元数据 + 验证缓冲 + 路径与条目 + 后端额外开销
```

### 9.3 顺序与完成语义

- 写请求发出不等于写请求完成。
- 缓冲在底层 I/O 完成之前不能复用。
- 处理短读、短写、EOF、0 字节文件、取消和中途设备断开。
- 普通 SHA256/xxHash 的文件摘要必须按正确的文件字节顺序更新，不能把并行块摘要直接拼接当文件摘要。
- 使用 BLAKE3 官方树形实现时也必须遵守其接口和格式，不能自创“差不多相同”的组合方式。
- 异步乱序完成时，以偏移记录写入结果；最终文件长度精确截断。
- 暂停应停止新调度并让已发出的 I/O 到安全点；取消必须等待已发出的 I/O 结束或完成取消，再释放内存、关闭句柄和清理。
- I/O 的完成、内容验证、元数据完成、最终提交分别记录。

### 9.4 无缓冲不是默认答案

COPY_FILE_NO_BUFFERING 涉及地址、长度与偏移对齐，还要处理扇区要求和尾块。设备、文件系统和远程实现的支持并不一致。

建议：

1. 先提供缓存 I/O 的实验性块路径。
2. 之后独立增加无缓冲能力探测与对齐测试。
3. 仅在测到收益且语义完整时启用。
4. 区分“此设备不支持此 flag”与“磁盘满、权限失败、目标竞争”等错误。
5. 不把 COPY_FILE_NO_BUFFERING 与 COPY_FILE_COPY_SYMLINK 等语义选项一起盲目清掉。

---

## 10 状态机和提交语义

### 10.1 任务状态

```text
Created
  → Planning
  → Running
  → Settling
  → Completed | CompletedWithErrors | Cancelled

Running / Planning
  → PauseRequested
  → Paused
  → 原来的可恢复执行状态

任何非终态
  → CancelRequested
  → Settling
  → Cancelled
```

这里的暂停是协议，不是“强行挂起线程”。处于冲突等待、队列等待或 I/O 等待的组件都必须能响应控制信号。实际进度保持“扫描中、复制中、验证中、提交中”等子阶段，不必为每种组合创建爆炸式全局状态。

### 10.2 文件状态

```text
Discovered
  → WaitingForParent
  → ResolvingConflict
  → Ready
  → Copying
  → Verifying（可选）
  → ApplyingMetadata
  → ReadyToCommit
  → Committing
  → Committed

任一未提交状态
  → Skipped | Failed | CancelledBeforeCommit

已提交但后续非致命元数据或记录操作失败
  → CommittedWithWarning（不能伪称完全未复制）
```

直接创建最终目标的路径有一个重要区别：文件在 Copying 阶段就可能已经可见。对它而言，Committed 是应用层成功终态，不是“此前文件一直不可见”。若需求是验证通过前不可见，应选择 staging。

### 10.3 三种提交方式

| 方式 | 使用场景 | 安全约束 |
| --- | --- | --- |
| DirectCreateNew | 明确不覆盖、允许失败时清理本任务新建目标 | 最终创建必须排他；清理前核对身份 |
| StagedCreateNew | 希望验证前不暴露半成品 | staging 与目标同目录或同卷策略明确；提交仍不能覆盖竞争者 |
| StagedReplace | 用户批准覆盖现有目标 | 写入与验证完成前保留旧目标；提交前重检授权目标身份 |

不要为了统一代码强制所有小文件都 staging，也不要为了减少 rename 删除所有 staging。现有 v1.0.58 已有小文件无冲突直写路径，应保留为对照。

ACL 验收应对照产品既定的目标继承与覆盖语义，不是无条件要求把源 ACL 原样复制。

对于覆盖：

1. 记录用户实际批准覆盖的目标身份。
2. 创建本任务拥有的唯一 staging 文件，禁止复用不明临时文件。
3. 完成数据、所需验证与可提前应用的元数据。
4. 检查目标是否仍是被批准的对象。
5. 使用平台及目标文件系统适用的提交方式。
6. 记录最终结果和可撤销能力。
7. 清理仅限本任务仍拥有的 staging。

目标身份检查与最终替换之间仍可能存在 TOCTOU 窗口；仅“检查一次再按路径替换”不等于实现了条件替换。实现需验证目标句柄、共享约束和平台提交机制能否共同满足要求；无法建立相应保证时，收窄支持范围、拒绝自动覆盖或重新请求决策，不能宣称已消除竞争。

不要把任何 rename 都描述为跨文件系统、跨 SMB 实现且断电安全的事务。目标存储不提供所需保证时，应降低“原子”承诺或采用更保守策略，并如实展示。

### 10.4 部分成功和撤销

- 用户取消不应把之前已经提交的文件全部标成失败。
- 汇总必须分别展示成功、警告、跳过、失败、未执行和已提交后取消。
- 重试仅针对可安全重试的条目，并重新核对源目标身份。
- undo 记录必须与实际提交对应。无旧目标备份就不能宣称可恢复被覆盖内容。
- 对新建文件执行 undo 时，也要防止删除之后被其他程序替换或修改的文件。
- 如计划增加崩溃恢复，日志的持久化顺序与恢复范围另行设计；不要把普通内存结果表称为事务日志。

---

## 11 回退策略

**禁止“任何错误都再调用一次 CopyFile2”。** 这会掩盖原始错误，并可能重复覆盖、误删临时文件或把部分结果当成完整文件。

| 发生条件 | 允许动作 | 不允许动作 |
| --- | --- | --- |
| 执行前识别为实验后端不支持 | 直接选择兼容后端 | 先写一部分再随意切换 |
| 明确不支持某个性能 flag，尚无最终提交 | 关闭当前尝试，核对并清理本任务输出，使用精确降级计划重试 | 同时移除无关的链接或安全语义 |
| staging 中途失败 | 记录原始错误；清理本任务 staging 后按用户和错误策略决定是否重试 | 覆盖旧目标或将 staging 当成功 |
| 直写中途失败 | 只有确认当前目标仍是本任务创建对象时才清理；再决定是否重试 | 仅凭路径名删除文件 |
| 磁盘满、访问拒绝、源改变、哈希不匹配 | 保留明确失败与用户可理解的恢复选项 | 自动弱化校验或换后端假装成功 |
| 最终提交结果不确定 | 核对实际源目标身份与状态，进入需协调状态 | 盲目重试覆盖 |
| 用户取消 | 收敛在途操作，报告真实部分成功 | 自动重启复制“修复”取消 |

每次回退日志至少包含：原后端、原始错误、发生阶段、已写字节、提交状态、采取的清理、降级了哪个性能能力，以及最终后端。

发布回滚开关应按层拆分：

- 新冲突统计；
- 目录缓存；
- 统一调度；
- 流式枚举；
- 有界验证；
- 实验块引擎；
- 实验无缓冲策略。

这样出现回归时可以只关闭相关层，而不是撤销全部重构。

---

## 12 元数据和特殊对象验收

性能比较必须对齐功能语义。不能 Pulse 保留 ACL、另一工具不保留，再直接比较速度。

至少列出并决定：

- 创建时间、修改时间、属性及目录最终时间；
- ADS；
- ACL、安全描述符、权限不足时的行为；
- 符号链接、junction、其他 reparse point；
- 硬链接的复制结果是否要求保留关系；
- 稀疏文件、压缩与加密文件；
- 长路径、Unicode、大小写差异；
- 只读目标、目录文件同名冲突；
- 云占位符、离线文件、网络对象；
- 文件占用、共享冲突、源在复制期间变化；
- 不支持的 Shell namespace 对象。

“保留”和“跳过”都需要是明确策略。对实验后端未完成等价性的项目，执行前选择原路径，不应默默退化。

目录元数据应用返回值必须检查。失败应被分类为内容成功但元数据警告，或按严格策略判定操作失败，而不是悄悄显示全绿。

---

## 13 可观测性设计

### 13.1 每任务指标

建议输出一份本地结构化结果，默认不收集敏感完整路径：

```json
{
  "schema_version": 1,
  "job_id": "local-job-id",
  "build_commit": "tested-commit",
  "policy": {
    "verify": "sha256",
    "backend": "copyfile2",
    "streaming_enumeration": false
  },
  "counts": {
    "discovered": 0,
    "committed": 0,
    "failed": 0,
    "skipped": 0,
    "warnings": 0
  },
  "timings_ms": {
    "request_to_first_write": 0,
    "enumeration_wall": 0,
    "conflict_wait": 0,
    "copy_span": 0,
    "source_hash_span": 0,
    "target_hash_span": 0,
    "metadata_span": 0,
    "commit_span": 0,
    "settle_span": 0,
    "end_to_end": 0
  },
  "resources": {
    "peak_buffer_bytes": 0,
    "peak_queued_entries": 0,
    "peak_active_files": 0,
    "peak_open_handles": 0
  }
}
```

并行阶段的 span 可以重叠，**不能把各阶段 wall time 简单相加当作端到端耗时**。还应分别记录线程 CPU 时间、累计工作时间与关键路径时间。

### 13.2 每文件和热路径指标

推荐记录或采样：

- 文件大小分桶、源目标设备类别；
- 排队、复制、验证、提交耗时；
- 后端、flags、并发数和回退原因；
- PathExists、SameFileObject、目录枚举调用数；
- 冲突统计遍历量和提示等待时间；
- 进度回调次数、合并通知次数、UI 消费次数；
- 锁等待、分配量、字符串构造热点；
- 逻辑读写字节与实际存储 I/O 观测值；
- 取消请求到停止新增 I/O、到全部收敛的两个时间。

性能日志不得在每个数据块上同步写盘。使用采样或内存环形缓冲，任务末尾批量写出；测量本身的开销也要校准。

### 13.3 UI 优化的判定方式

当前已有回调和通知节流，应先测：

- 是否仍有逐文件锁竞争；
- 大批小文件完成时是否反复格式化路径；
- 合并通知后 UI 是否仍大面积重绘；
- 进度窗口延迟策略是否被误认为没有开始。

建议 UI 只读取小型快照，活动文件列表按需刷新。扫描阶段可更早提供轻量状态，但不要为追求“看上去忙”每个文件都 PostMessage。

---

## 14 基准与验收矩阵

### 14.1 比较对象

每轮至少记录：

1. Pulse v1.0.58 固定基线；
2. 本次优化构建及 commit；
3. Windows Explorer 的实际系统版本；
4. TeraCopy 的实际版本和选项。

分成两条公平赛道：

- **普通复制赛道**：都不额外验证，并对齐冲突、元数据与目标状态。
- **带验证赛道**：使用可比算法和完成定义。若某工具不支持相同策略，则明确标为不可直接比较，不能强行放在同一排名。

杀毒、安全设置、后台负载、写入缓存策略保持一致。不要把关闭防护或添加杀毒排除项作为产品提速建议。

### 14.2 数据集

| 数据集 | 建议规模 | 主要观察 |
| --- | --- | --- |
| 大量小文件 | 10,000 个，大小分布固定并记录 | 每文件控制开销、目录元数据、冲突探测 |
| 阈值边界 | 600 × 1 MiB；补充略小于与略大于 1 MiB | 现有小文件池边界、统一调度收益 |
| 中型文件 | 4–32 MiB，数量与总量固定 | 不在原小文件池中的并行空间 |
| 大文件 | 单文件及少量 4–16 GiB 文件 | 顺序吞吐、缓存、无缓冲与验证 |
| 混合集 | 小、中、大文件与多层目录 | 公平性、首字节、尾部拖延、内存 |
| 冲突集 | 不冲突、部分重名、全部重名、深层目录 | 重复探测和 remaining_conflicts 成本 |
| 特殊语义集 | 按第 12 节分别构造 | 不能为了提速牺牲兼容性 |

每个数据集使用固定 manifest：相对路径、内容生成方式、大小、摘要和所需元数据。不能把高度可压缩数据、稀疏数据和普通随机数据混作同类结论。

### 14.3 存储矩阵

- 同一 SSD 不同目录；
- 跨 SSD；
- 同一 HDD；
- 跨 HDD 或 HDD 与 SSD；
- USB 闪存；
- USB 外置 SSD；
- SMB，记录网络链路、服务器存储与共享配置。

不是每个提交都跑全矩阵。开发冒烟测试选代表场景，发布候选跑完整矩阵；新增设备策略必须覆盖它声称优化的设备类别。

### 14.4 运行方法

- 冷缓存与热缓存分开报告，不混算中位数。
- “冷缓存”需要可复现操作和记录；不能仅因为换了输出目录就声称冷缓存。
- 每种条件随机化工具运行顺序，至少重复 5 次。
- 报告中位数和范围；必要时补充 P95、标准差或置信区间。
- 每次使用干净但等价的目标状态；覆盖与非覆盖分开。
- 将扫描、首字节、复制、哈希、提交、UI 和端到端同时记录。
- 有条件时用 ETW 或性能分析工具定位文件 I/O、CPU、锁和磁盘队列。
- 记录系统内存、CPU、设备型号、文件系统、连接方式、剩余空间及温度影响。
- 所有破坏性数据准备与清理只在明确标记的专用测试目录内执行。

仓库已有 bench 可复用其部分生成与测量框架，但它是合成数据、没有 Explorer 对照，而且新生成的数据可能仍在缓存里。现有 RemoveTree(root) 具备破坏性，只能对专用测试根使用；本方案没有运行它。

### 14.5 验收门槛

不要先承诺一个未经测量的提速百分比。先建立基线和噪声区间，再共同设定量化发布门槛。

| 维度 | 必须通过的判断 |
| --- | --- |
| 内容正确 | 所有声称成功的普通文件内容与预期一致 |
| 冲突安全 | 竞争者新建或替换目标时，不发生未授权覆盖 |
| 元数据 | 所声明的保留策略逐项通过；失败不会静默 |
| 部分成功 | 取消、断线、空间不足后，汇总与实际磁盘状态一致 |
| 首字节 | 流式枚举在大目录场景确实降低等待，而不是只提前显示窗口 |
| 端到端 | 目标场景收益超过测量噪声；常用非目标场景没有不可接受回归 |
| 资源 | 内存、句柄和队列在设定预算内，无随文件数无界增长 |
| 可响应 | 暂停、取消和冲突操作达到事先设定的目标延迟 |
| 可回退 | 关闭单个实验开关即可恢复已验证路径，不改变文件语义 |
| 可解释 | 每次性能差异能对应阶段、设备、策略和构建版本 |

性能回归阈值、取消延迟 SLO、内存预算等是发布前待填写项，不是本次已达成结果。

---

## 15 建议的开发阶段和任务拆分

### 阶段 0 建立护栏

- 固定 v1.0.58 基线与测试 manifest。
- 把 verify 默认值、覆盖策略、元数据、同文件保护写成行为测试。
- 增加分阶段计时与后端选择日志。
- 为所有新能力加入独立实验开关。

完成标准：不改变外部行为，能区分时间花在扫描、复制、验证还是提交。

### 阶段 1 优化控制路径

- 将 remaining_conflicts 改为一次或增量统计。
- 为深层目标目录建立有界缓存，保持最终竞态检查。
- 检查并传播目录元数据错误。
- 只有 profiling 指向它时，再优化逐文件锁、字符串和通知分配。

完成标准：重复统计和不必要探测减少，冲突竞态及元数据失败测试通过。

### 阶段 2 统一文件调度

- 以统一调度器承接现有 CopyFile2 调用。
- 添加每设备或设备组合预算。
- 让 1 MiB 及中型文件参与受控并发。
- 保持大小文件公平性和验证资源预算。

完成标准：目标 SSD 或远程场景有可重复收益，HDD 和 USB 不出现不可接受退化。

### 阶段 3 引入流式枚举

- 使用有界队列、目录依赖和可取消等待。
- 保留完整规划模式。
- UI 明确总量未知和扫描仍在进行。
- 覆盖源目录变化、深树、空目录和冲突提示场景。

完成标准：首字节时间降低；目录元数据、取消、内存和最终统计正确。

### 阶段 4 重构验证

- 验证独立成有界阶段。
- 保留 SHA256 模式。
- 按明确产品选项增加其他算法，不静默切换。
- 复用合法资源并记录源变化风险。
- 明确验证发生在提交前还是提交后；覆盖保护优先。

完成标准：带验证模式不再无条件牺牲全部复制并发，内容和状态机测试通过。

### 阶段 5 试验块引擎

- 先仅支持受控普通文件与缓存 I/O。
- 实现有界缓冲、顺序哈希、异步完成和安全取消。
- 以源哈希融合为明确收益假设。
- 特殊对象执行前选用兼容路径。
- 无缓冲作为独立实验，而非与主引擎一起发布。

完成标准：相同语义下确有收益，所有适用正确性测试通过；否则保持实验或删除，不影响默认功能。

### 阶段 6 发布与回滚

- 发布说明列明变化的策略和不变的保证。
- 保存对照结果、参数、构建标识与回退案例。
- 灰度范围与默认开关由维护者明确批准。
- 出现内容、覆盖或撤销错误立即停用相关新路径；单纯性能波动按矩阵定位。

不建议一口气把阶段 1–5 合并成一个大 PR，否则无法归因性能变化，也难以回滚。

---

## 16 推荐先写的回归用例

### 控制与并发

- [ ] 0、1、15、16、17 个小文件。
- [ ] 严格小于、等于、略大于 1 MiB。
- [ ] 63 MiB、64 MiB、65 MiB 边界。
- [ ] 小文件持续到来时，大文件不会饥饿。
- [ ] 多个源来自不同设备时，不沿用首个源的分类。
- [ ] 枚举队列满、冲突等待、验证进行时均能取消。
- [ ] 暂停后不会无限新增 I/O，恢复后不重复提交。
- [ ] 缓冲池耗尽时不会死锁或超预算分配。

### 内容与元数据

- [ ] 空文件、空目录、深层目录、超长路径、Unicode。
- [ ] 普通文件 SHA256 对照。
- [ ] 源在打开前、复制中、校验前发生变化。
- [ ] ADS、ACL、目录时间和只读属性按策略正确。
- [ ] 实验后端不支持的对象在写入前选择兼容路径。
- [ ] 哈希不一致明确失败，绝不自动关闭验证重试。

### 冲突与提交

- [ ] 同一路径和指向同一对象的别名。
- [ ] 目标在缓存读取后被其他进程创建。
- [ ] 已批准覆盖的目标在提交前被替换。
- [ ] 文件对目录、目录对文件的冲突。
- [ ] staging 写失败时旧目标仍保留。
- [ ] 提交成功但后续记录失败时，不误报为未复制。
- [ ] 取消后仅清理可证明属于本任务的未提交输出。
- [ ] undo 不删除后来由其他程序替换的对象。

### 故障注入

- [ ] 磁盘满、权限拒绝、共享冲突。
- [ ] SMB 断线与重连。
- [ ] USB 拔出。
- [ ] 短读、短写、无缓冲参数不支持。
- [ ] 元数据写入失败。
- [ ] 多次失败与重试后没有临时文件、句柄或内存泄漏。
- [ ] 所有终态汇总与实际文件状态相符。

---

## 17 参考资料与源码定位

### 17.1 TeraCopy 官方资料

以下资料用于确认设计线索，不用于承诺 Pulse 的性能结果。

- **[T1] Code Sector，Options Tab**，页面更新日期 2024-12-16。描述线程、缓冲、HDD 自动策略及元数据选项。  
  https://support.codesector.com/en/articles/9905589-options-tab
- **[T2] Code Sector，Multi-threading**，官方已完成功能条目，2024-01-06。描述读块、计算哈希和写入线程的协作。  
  https://ideas.codesector.com/p/multi-threading
- **[T3] Code Sector，How to make TeraCopy faster**，页面更新日期 2024-09-24。说明复制中计算源摘要和算法选择；其中旧的多线程预告应结合较新资料阅读。  
  https://support.codesector.com/en/articles/8789951-how-to-make-teracopy-faster

### 17.2 Pulse 固定版本源码

仓库：[jimmgreen/pulse](https://github.com/jimmgreen/pulse)。所有源码链接固定到 [be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26](https://github.com/jimmgreen/pulse/tree/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26)，不跟随主分支漂移。该 commit 来自 v1.0.58 annotated tag 解引用。

| 编号 | 文件和位置 | 对应结论 |
| --- | --- | --- |
| P1 | [src/ops/ops_manager.h:567](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.h#L567) | verify 默认 false |
| P2 | [src/ops/ops_manager.cpp:3493–3575](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L3493-L3575)；[src/ops/ops_manager.cpp:3598–3619](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L3598-L3619) | 完整 TransferEntry 预扫，再进入 Running |
| P3 | [src/ops/ops_manager.cpp:1133–1150](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L1133-L1150) | 操作在后台线程运行 |
| P4 | [src/ops/ops_manager.cpp:3419–3455](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L3419-L3455)；[src/ops/ops_manager.cpp:3910–3929](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L3910-L3929) | 根层 TargetListing 与已有目录逐文件检查 |
| P5 | [src/ops/ops_manager.cpp:3667–3677](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L3667-L3677)；[src/ops/ops_manager.cpp:3935–3952](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L3935-L3952) | remaining_conflicts 重复遍历与调用 |
| P6 | [src/ops/ops_manager.cpp:3699–3713](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L3699-L3713)；[src/ops/ops_manager.cpp:1694–1700](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L1694-L1700) | verify 关闭池、至少 16 个、严格小于 1 MiB |
| P7 | [src/ops/transfer_engine.cpp:53–103](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/transfer_engine.cpp#L53-L103) | 设备并发分类、源目标取较小值、队列策略 |
| P8 | [src/ops/ops_manager.cpp:4109–4186](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L4109-L4186) | 延后文件、直写和大文件无缓冲条件 |
| P9 | [src/ops/ops_manager.cpp:4225–4267](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L4225-L4267)；[src/ops/ops_manager.cpp:798–848](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L798-L848) | 源目标 SHA256 重读及每次哈希资源准备 |
| P10 | [src/ops/transfer_engine.cpp:234–265](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/transfer_engine.cpp#L234-L265)；[src/ops/ops_manager.cpp:4299–4311](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L4299-L4311) | Move 耐久性与 Copy 路径差异 |
| P11 | [src/ops/ops_manager.cpp:585–623](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L585-L623)；[src/ops/ops_manager.cpp:1632–1715](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L1632-L1715) | 进度回调与通知节流 |
| P12 | [src/app/app_main.cpp:1557–1560](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/app/app_main.cpp#L1557-L1560)；[src/app/app_main.cpp:2908–2924](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/app/app_main.cpp#L2908-L2924) | PostMessage 与 UI invalidation |
| P13 | [src/app/app_ops_ui.cpp:1235–1254](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/app/app_ops_ui.cpp#L1235-L1254) | 进度窗口延迟和大于等于 90% 抑制 |
| P14 | [src/ops/ops_manager.cpp:720–730](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L720-L730)；[src/ops/ops_manager.cpp:4335–4356](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L4335-L4356) | 目录元数据 helper 与未检查返回值的调用 |
| P15 | [src/ops/ops_manager.cpp:3228–3233](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/ops/ops_manager.cpp#L3228-L3233) | Shell namespace 独立路径 |
| P16 | [src/bench/ops_bench_main.cpp:153–195](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/bench/ops_bench_main.cpp#L153-L195) | 现有合成基准及使用边界 |

### 17.3 历史反馈

- [issue 60](https://github.com/jimmgreen/pulse/issues/60)：旧版 v1.0.46 的临时文件性能反馈。只作历史线索，不能引用旧倍数代表 v1.0.58。
- [issue 454](https://github.com/jimmgreen/pulse/issues/454)：USB 与无进度反馈；缺少足够版本与复现条件，仍需验证。

### 17.4 待实现时需要进一步核对的接口契约

本次没有把“列出接口名称”当成完成 API 验证。真正编码时应按目标 Windows SDK 与文件系统核对以下官方契约，并将结论写进测试：

- CopyFile2、进度回调、取消暂停及 COPY_FILE_* flags；
- CreateFile 的共享模式、create disposition 和文件身份查询；
- 异步 I/O 生命周期、取消完成及无缓冲对齐；
- rename/replace 在本地与网络文件系统上的行为；
- CNG 哈希 provider 与 hash object 的线程和生命周期约束；
- 选用 xxHash/BLAKE3 库的官方接口、版本、许可证和测试向量。

**汇编不是当前优先项。** 若 profiling 证明瓶颈确实在哈希或数据变换 CPU 内核，先采用经过验证的成熟库、编译器优化或受支持 SIMD 实现，再考虑手写汇编。若瓶颈是扫描、冲突探测、设备寻道或额外重读，手写汇编不会解决它们。

---

## 18 开工前的决策清单

- [ ] 首轮优化重点是首字节、总耗时，还是开启校验后的体验？
- [ ] 覆盖失败时必须保留旧目标吗？验证通过前允许新文件可见吗？
- [ ] 默认 SHA256 契约是否保持？新算法如何显式选择与记录？
- [ ] ACL、ADS、链接、稀疏等语义中哪些是强保证？
- [ ] 源文件变化时采用哪种政策？
- [ ] 普通复制与耐久性模式的完成定义分别是什么？
- [ ] HDD、USB、SMB 的回归门槛和取消响应目标是什么？
- [ ] 哪个开关能单独回退每个新阶段？
- [ ] 使用哪个固定测试目录，如何防止基准清理误删用户文件？
- [ ] 谁批准把实验引擎切为默认路径？

建议第一项实际开发任务是：**不改变复制行为，先补阶段计时和正确性测试，再移除重复冲突统计。** 这样很快就能知道“慢在哪里”，后续每一次架构改动都有可解释的依据。

