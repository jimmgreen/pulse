# 文件操作模块：性能优化方案与代码审查问题汇总（2026-11-20）

> **状态说明（必读）**
>
> - 第 1–9 节是只读审查后写的方案。**第 2.1 节是 2026-11-20 补做的实机验证**，结果以 2.1 节为准；第 4–6 节中与之冲突的地方，已经按验证结果改过或加了标注。
> - **2026-11-20 更新：已在独立分支 `perf/file-ops-opt-20261120`（从 `feat/dialog-takeover-compat` @ `06463d0` 创建）上实施了 P0-1、P0-2、P0-3、P1-1、P1-2、N1、B2 和 B3，并做了实机验证（第三批见第 11 节），见第 10 节。** 改动未提交，工作树中原有的未提交改动全部保留。下面原来的这句话描述的是方案阶段的状态。
> - （方案阶段）**产品代码至今没有任何修改。** 实机验证用的是独立的测试程序，放在 `bench_data/opt-verify-20261120/`（这个目录被 git 忽略）。验证方式有两种：
>   - 直接调用产品里 header-only 的函数，并通过它已有的开关做 A/B，例如 `RecycleItemNative` 的 `emit_shell_notify`、`DeleteNative` 的 `workers`；
>   - 逐字复制产品里的算法片段，与拟议实现做对照。
> - 因此 2.1 节的数字说明的是“这个瓶颈确实存在、量级有多大”，**不等于产品端到端已经提速**。真正改代码之后，还要按第 7 节做产品级新旧对照。
> - 代码基线是 HEAD `06463d0` 加上当前未提交的工作树，读取时间为 2026-11-20。行号是读取时的近似位置，后续改动会让它们偏移。
> - 历史数字只引用已有文档，本轮不重新声明。

## 1. 审查范围

| 已读 | 范围 |
|---|---|
| `src/ops/ops_manager.cpp`（7223 行） | 189–252、704–945、1000–1046、1178–1262、2236–2580、3333–3518、4343–6700、7026–7090 |
| `src/ops/ops_manager_interactive.inl` | 1–120、180–301 |
| `src/ops/ops_manager_authorized_delete.cpp` | 120–420（`RunAuthorizedDelete` 主循环） |
| `src/ops/transfer_engine.h/.cpp` | 全文 |
| `src/ops/transfer_device_budget.h/.cpp` | 头文件全文，`.cpp` 106–263 |
| `src/ops/transfer_enumeration_cursor.h`、`source_listing.h`、`verification_worker.cpp`、`transfer_features.h` | 全文 |
| `src/ops/native_recycle.h` | 1–260、575–914 |
| `src/ops/native_delete_stream.h` | 1–140 |
| `src/ops/elevated_delete.cpp` | 90–145、470–647 |
| `src/common/utf8_file.h`、`runtime_log.cpp`、`delete_diagnostics.h` | 相关函数 |
| 历史文档 | `copy-refactor-task-list-2026-10-31`、`copy-refactor-progress-2026-10-31`、`batch-delete-performance-2026-10-29`、`delete-latency-2026-11-02`、`copy-delete-review-delivery-2026-10-31`、`review-p2-*-2026-10-06`、`module-audit-v2-status.json` |

**未读或只读了一部分**（结论不覆盖这些文件）：

- `native_recycle.h` 260–575（`DeleteBatchParallel`、配额压缩器主体）
- `recycle_empty.h`
- `ops_manager_shell_transfer.cpp`
- `src/fs/` 下的枚举、快照和监视实现
- `elevated_transfer*.cpp`

## 2. 结论摘要

按“可能的收益 ÷ 风险”排序，最值得先做的是下面五项。它们都**有代码证据，但收益尚未测量**。

1. **操作日志每次都全量重写，而且是 write-through 写盘**（P0-1）。
   - 每个队列项至少触发 2 次 `PersistJournal`，交互通道再加 2 次。
   - 每次都在 `mutex_` 内把整个队列序列化成 JSON，再用 `FILE_FLAG_WRITE_THROUGH` 原子替换文件。
   - 队列里有 Q 个操作时，总代价约为 O(Q²) 字节加 O(Q) 次同步写盘。
2. **每个操作结束后都全量重建撤销 JSON**（P0-2）。
   - `UndoJsonSnapshot` 在 `mutex_` 内深拷贝整个 `undo_`（包括所有路径），锁外再整体重建 JSON。
   - 持锁的深拷贝会拖慢 UI 线程的 `Status`/`CaptureUiSnapshot`。
3. **批量回收站删除的逐项固定开销**（P0-3）。
   - 每项都有一组固定开销：多次 attributes 探测、目录全量测量、2 次 `SHChangeNotify`、2 次不节流的 `SetStatus`。
   - 每批还要做一次 O(回收站大小) 的 `$I` 扫描。
   - 历史上 1000 项回收产生过 993 次通知，耗时波动在 0.945–9.240s。
4. **`RunTransfer` 主循环和收尾阶段的多处 O(N×S)/O(N²) 结构**（P1-1、P1-2）。
   - 问题点包括 `skipped_prefixes` 线性扫描、KeepBoth 目录重写后续条目、回滚时在 vector 中间 erase。
   - 平时 S 很小，但大量跳过或冲突时会退化成平方级。
5. **`NtFlushBuffersFileEx(NO_SYNC)` 一次失败就全局永久降级**（P1-3，同时也是 bug）。
   - 只要有一次复制落在不支持 no-sync 的文件系统上（例如 FAT 优盘），进程此后所有复制都会改走完整的 `FlushFileBuffers`。

### 2.1 实机验证结果（2026-11-20）

**测试环境**

- CPU：i5-13600KF，48GB 内存。
- 磁盘：C: 是 SATA SSD（Fanxiang S100Pro 512GB，NTFS）。
- 安全软件：火绒 HIPS 处于运行状态，没有改动它的任何设置。
- 编译：MSVC 14.51，`/O2`，C++20。
- 运行方式：用户交互会话中的计划任务。`run_command` 子进程初始化 user32 失败（`ERROR_DLL_INIT_FAILED`），所以改走计划任务。进程优先级设为 Normal，并关闭了 EcoQoS 节流。
- 对照方式：新旧两种做法在同一个进程里交替运行（ABAB 或轮换顺序）。报告的是中位数，日志里另有最小值和最大值。

**机械盘的结果全部作废。** 火绒拦截了测试程序在 D:、E:、F:、G: 上创建文件和目录（`ERROR_ACCESS_DENIED`；用 PowerShell 写同一个位置是成功的）。机械盘上的日志已改名为 `*.INVALID-hips-blocked.log`，不采用。下面所有结论**只适用于 C: SSD，而且是在火绒运行的条件下**。

| 建议 / 问题 | 实测场景 | 现状做法 | 拟议做法 | 结论 |
|---|---|---|---|---|
| **P0-3** 回收逐项 Shell 通知 | 产品 `RecycleItemNative` 处理 1000 个 4KiB 文件，回收到测试专用的 bin 目录，3 轮 | 每项通知（产品现状）：**6246 ms** | 批末通知（每个父目录一次 `UPDATEDIR`，加一次回收站更新）：**1459 ms**；完全不通知：1587 ms | **成立，约快 4.3 倍**。产品自带的 `notify_ms` 指标显示每项 2 次 `SHChangeNotify` 合计约 **3.4 ms**，是这条路径的主要开销（`elevated_delete.cpp:534` 固定传 `true`）。另一组 4 轮测试：6017 vs 1409 ms |
| P0-3（目录） | 200 个目录，每个含 10 个文件，4 轮 | 1036 ms | 不通知：322 ms | 成立。目录本身的测量 `scan_ms` 总计只占约 200 ms |
| **P0-1** 日志写盘合并 | 产品 `WriteUtf8FileAtomic` 写到 C:，按现有调用序列重放（每次入队写一次，每项开始和结束各写一次） | Q=50、每项 1 个源：443 ms，150 次写；Q=200：2053 ms，600 次写；Q=200、每项 100 个源：4933 ms | 173 / 631 / 1761 ms，写盘次数降到约 1/3 | **成立，快 2.6–3.3 倍**。每次 write-through 写约 2.5–3.4 ms。崩溃一致性的语义还没验证 |
| P0-2 撤销 JSON | 撤销历史 100 步 × 每步 10 / 100 / 1000 个路径 | 持锁深拷贝：0.23 / 3.6 / **27 ms** | 改成拷贝 `shared_ptr`：0.007 / 0.013 / 0.019 ms | 成立，但**量级比预想小**，只有超大撤销历史才明显。锁外 JSON 重建另需 0.5 / 4.4 / 41 ms。**降级为 P1** |
| **P1-1** `skipped_prefixes` | 101000 个条目，跳过 S 个目录 | S=10：25 ms；S=100：219 ms；S=500：1079 ms；S=1000：2167 ms | 0.7 / 1.2 / 3.0 / 4.5 ms | 成立，平方级退化确认存在，跳过结果完全一致 |
| P1-1 的 `dir_key` 和环境变量读取 | 单次调用耗时 | `dir_key` 76 ns；`TransferFeatureEnabled` 63 ns | — | **不成立，不值得改**，从方案中删除 |
| **P1-2** KeepBoth 重写后续条目 | 101000 个条目，K 个目录冲突 | K=10：19 ms；K=100：162 ms；K=1000：1620 ms | 只扫描连续子树：0.18 / 1.7 / 16 ms | 成立，结果一致。前提是条目按先序排列，有交叉的多根选择需另行处理 |
| P1-2 回滚 | N 个完成项，R 个已恢复 | N=2000、R=200：8.6 ms；N=10000、R=10000：**1097 ms** | 0.8 / 10.3 ms | 成立，结果一致 |
| **P1-3 / B4** 刷盘降级 | 1000 个 64KiB 文件，4 轮 | 降级后的完整 `FlushFileBuffers`：0.83 ms/文件 | no-sync：0.36 ms/文件 | **降级的代价已经量化：每个文件刷盘慢约 2.3 倍**。但**降级的触发条件没能复现**：本机没有 FAT/exFAT 卷，机械盘写入又被拦截 |
| **P1-5** 本地永久删除并发 | 产品 `DeleteNative` 删除一个目录内 10000 个零字节文件，3 轮 | workers=1（本地默认）：2490 ms | workers=4：1927 ms；workers=8：**1714 ms** | 在这块 SATA SSD 上快约 31%。机械盘上的效果未知，测试被拦截，**不能直接改默认值** |
| **新发现 N1** 每个新目录都解析一次卷 | 1000 个不同目录，路径深度 10 | 每个目录调一次 `GetVolumePathNameW`（与 `TransferDeviceIdentityCache::Resolve` 相同）：1149 ms，即每个目录 1.15 ms | 传输根只解析一次，子目录沿用，遇到 reparse 点再重新解析：1.1 ms | 一致性：0 处不一致（夹具里没有挂载点）。单次调用：C: 深路径 0.5–0.7 ms，而 `GetFileAttributesW` 只要 0.02 ms。**32768 wchar 缓冲区本身只花 0.9 µs，P2-1 的成本在系统调用，不在内存分配** |
| **B2** `MeasureFolderFast` 测量遗漏 | 测试目录共 35MiB；对一个子目录拒绝 `RD` 权限 | 测出 7 项、5MiB；在 16MiB 限额下 `RecycleItemNative` 照样移入，`$I` 记录的大小为 5242880 | — | **bug 复现**：配额检查被绕过，`$I` 中的大小错误。测试后 ACL 已恢复，夹具已删除 |
| P2-2 / E14 | 单次调用耗时 | 额外的 `GetFileAttributesExW` 25 µs；`SameFileObject` 52 µs | — | 开销很小，维持 P2 |

**验证后的优先级调整**

- 最先做 **P0-3**（收益最大，改动集中在 `elevated_delete.cpp` 和 `native_recycle.h`）和 **B2**。
- 接着做 **P0-1**、**P1-1（只保留 `skipped_prefixes`）**、**P1-2**、**N1**。
- **P0-2 降为 P1**；`dir_key` 和环境变量缓存**删除**。
- **P1-5** 要等机械盘数据。**P1-3** 要等能复现触发条件的环境。

**这次没有验证的部分**

- 产品端到端的新旧对照，需要先改代码。
- 批末通知之后，打开着的 Explorer 窗口和桌面能否正确刷新。
- B1（回收撤销缺失）、B3、P1-4、P2-3 到 P2-7。
- 所有机械盘、SMB、优盘场景。
- 测量 Explorer CPU 占用的代码在本环境下始终返回 0，该指标已弃用。

另外有一个异常现象：一次连续调用 2 万次 `GetVolumePathNameW` 的测试，跑了 4 分钟以上还没结束，CPU 时间也很少；同样的调用在 `vol_probe` 和 `vol_tree` 里是正常的。原因不明，所以**没有采用**这次的数据。

日志：`bench_data/opt-verify-20261120/*.log`。源码、构建脚本和运行脚本（`build.bat`、`runtask.ps1`、`suite*.cmd`）在同一目录。

## 3. 已有基线（引自历史文档，本轮不重新声明）

| 项 | 历史数据 | 来源 |
|---|---|---|
| 1000 文件回收 | 4.466s，993 次通知 | `batch-delete-performance-2026-10-29` |
| 回收计时稳定性 | 0.945 / 1.696 / 9.240s，波动太大，不能据此下吞吐结论 | 同上 |
| 回收撤销 | 基线只恢复 785/1000；新版第 3 轮恢复 199/200；其余由人工恢复 | 同上 |
| 删除预检延迟 | 改用 `PostMessage` 后从 1187–6407ms 降到 250–453ms | `delete-latency-2026-11-02` |
| D2 流式永久删除 | 10k 个零字节文件，中位数 2.410s → 2.481s，慢 2.9% | `copy-delete-review-delivery-2026-10-31` |
| 复制方面已交付的项 | 冲突计数快照、SSD 四路中等文件复制、SHA256 校验重叠、UNC 小文件预算、设备预算唤醒、公平轮转（各自的限定条件见原文） | `PERFORMANCE_ROADMAP.md`、`copy-refactor-progress-2026-10-31` |
| 已撤回的项 | 预算零分配：SMB 场景回退 16.5% | 同上 |
| 尚未完成的项 | 覆盖暂存并行、按作业设置的设备权重、提权常驻 STA、元数据快照复用、**日志压缩** | `copy-delete-review-delivery-2026-10-31` |

## 4. 热点证据（只记录代码事实，不含推测）

### 4.1 队列、日志与撤销

| # | 位置 | 事实 |
|---|---|---|
| E1 | `ops_manager.cpp:1208` `PersistJournal` | 先持有 `journal_write_mutex_`，再在 `mutex_` 内调用 `JournalJsonLocked()`（1178）生成**全量** JSON，然后调用 `WriteUtf8FileAtomic` |
| E2 | `common/utf8_file.h:81` | 临时文件以 `FILE_FLAG_WRITE_THROUGH` 打开，写完后再替换；每次调用都是一次同步落盘 |
| E3 | `ops_manager.cpp:3370`、`3505`，`ops_manager_interactive.inl:259`、`300` | 调用点：工作线程在项开始和结束时各调用 1 次，交互通道再调用 2 次；`1359` 也有一次 |
| E4 | `ops_manager.cpp:3502` → `7030` `UndoJsonSnapshot` | 注释说明这是在 worker 上为持久化预热缓存。每个操作结束都会调用；修订号变化后，**在 `mutex_` 内深拷贝整个 `undo_` deque**（包括所有路径字符串），然后在锁外整体重建 JSON |
| E5 | `ops_manager.cpp:2509` `PushUndo` | `req.sources` 同时被复制进 `CompletedOperation` 和 `UndoEntry` |
| E6 | `ops_manager.cpp:2274` `CaptureUiSnapshot` | 每次调用都在 `mutex_` 内对 `queue_` 做 `count_if`，并汇总撤销信息；`StatusLocked` 还要遍历 `external_tasks_` |

### 4.2 状态与通知

| # | 位置 | 事实 |
|---|---|---|
| E7 | `ops_manager.cpp:2352` `SetStatus` | 每次调用都要加 `mutex_`、调用 `std::function`；阶段变化时还会写一条 diagnostics 事件；枚举未完成时调用 `l10n::Get` |
| E8 | `ops_manager.cpp:2332` | 非紧急通知最多合并 250ms（`kNotifyPendingMs`），等待 `AcknowledgeNotify` |
| E9 | `ops_manager.cpp:2435–2463` `SetProgressStatus` | 80ms 节流；总数 ≤64（`kEveryItemMaxTotal`）时，每完成一项就发一次紧急通知 |
| E10 | `ops_manager_authorized_delete.cpp:199`、`381` | 删除循环中每项至少调用 2 次**不节流**的 `SetStatus`（开始时设置 token，结束时清零），外加一次 `SetProgressStatus` |
| E11 | `ops_manager.cpp` 约 5990 行（校验路径） | 每个已校验文件调用 2 次 `SetStatus`：先设为 Verifying（附带 `l10n::Get` 和 `FileName`），再设回 Running |

### 4.3 复制主循环 `RunTransfer`（约 4343–6280 行）

| # | 位置 | 事实 |
|---|---|---|
| E12 | 4828、5137、5417、5441、5538 | `skipped_prefixes` 是一个 vector；处理每个条目时都对它线性调用 `StartsWithPath`，复杂度 O(N×S) |
| E13 | 4944 `dir_key` | 每次调用都分配字符串并调用 `CharLowerBuffW`；每个条目至少调用 1 次（`parent_key`），某些分支调用 2 次 |
| E14 | 5434–5440 | 父目录不是本次新建的条目，每个都要调用 `PathExists`；目标存在时还要再调 `SameFileObject`，它内部使用 `std::filesystem::equivalent`，会打开两个句柄 |
| E15 | 5542–5552 | 目录选择 KeepBoth 时，从 `index+1` 一直扫描到末尾，改写匹配的后续条目；每次冲突 O(N) |
| E16 | 5430、5454、5599、5734、6073 | 多个分支会先 `drain_pool()`：同名目标、有冲突且启用了中等文件池、串行或大文件等 |
| E17 | 5003、5694 | 候选和条目循环内部直接调用 `TransferFeatureEnabled`，每次都是一次 `GetEnvironmentVariableW`（`transfer_features.h:4`） |
| E18 | 6116 | 删除已移动源目录**失败时**（`RemoveMovedSourceItem` 返回目录非空），才对 `skipped_sources` 做 `any_of`，O(失败目录数×S)。核对后影响比最初判断的小 |
| E19 | 6148 | 判断 generated_backups 是否已发布：对 `completed_destinations` 做 `any_of`，O(B×N) |
| E20 | 6180–6191 | 回滚时：每项对 `restored_originals` 做 `any_of`（O(N×R)），在 vector **中间** `erase` 两次（最坏 O(N²)），每次还对 `moved_sources_` 做一次 `erase_if` |
| E21 | 6251、6266–6271 | 合并部分撤销的根时，再次对 `completed_destinations` 做 `any_of` 和嵌套扫描 |

### 4.4 传输引擎 `transfer_engine.cpp`

| # | 位置 | 事实 |
|---|---|---|
| E22 | 19–24、27 | `VolumeRoot` 每次调用都分配 32768 个 wchar，并调用 `GetVolumePathNameW`；`StorageOf` 是在**查 30s 缓存之前**就调用它 |
| E23 | 146–147 | `DescribeTransferTarget` 对源路径和目标路径各调用一次 `VolumeRoot` |
| E24 | 209–219 `Submit` | `queue_.push_back(std::move(job))` 之后，又读取了 `job.index` 和 `job.AdmissionBytes()` |
| E25 | 193–197 `HasRoom` | 预算固定为 256MiB，与设备类型无关 |
| E26 | 401–414 `Progress` | 拿到 `CaptureOwnedOutputStamp` 之前，每个 chunk 回调都会重试 |
| E27 | 509–513 | 大小 ≥1MiB 的文件复制完成后，都要再按路径调用一次 `GetFileAttributesExW` |
| E28 | 约 364、381–383 `FlushCopyToDisk` | `g_no_sync_unsupported` 是进程级全局变量，`NtFlushBuffersFileEx` 只要返回过一次失败，就永久置为 true |
| E29 | 370、392 | 只读文件刷盘时先清除只读属性，之后恢复属性的 `SetFileAttributesW` 不检查返回值 |

### 4.5 删除与回收站

| # | 位置 | 事实 |
|---|---|---|
| E30 | `native_recycle.h:710` `RecycleItemNative` | 每项依次执行：`GetFileAttributesExW`；回收站未缓存时调用 `GetVolumePathNameW`、`GetDriveTypeW`、查 SID；`GetFileAttributesW` 探测 `$R`；目录做 `MeasureFolderFast` 全量遍历；写 `$I`；`MoveFileExW`；2 次 `SHChangeNotify` |
| E31 | `native_recycle.h:615–617` | `MeasureFolderFast` 遇到 `FindFirstFileExW` 失败时直接 `continue`，**静默跳过**该子目录 |
| E32 | `native_recycle.h:640` | `FindNextFileW` 返回 false 时没有区分 `ERROR_NO_MORE_FILES` 和真正的错误 |
| E33 | `native_recycle.h` 约 860–870 | `$I` 写完后直接 `CloseHandle`，没有 `FlushFileBuffers`，紧接着执行 `MoveFileExW` |
| E34 | `native_recycle.h:650` `PlanQuotaCompaction` | 枚举回收站里的所有 `$I`，每个都调用 `ReadIndex` 和一次 `GetFileAttributesW($R)` |
| E35 | `elevated_delete.cpp:475` `DeleteWithCurrentToken` | 回收策略缓存只保存**上一个父目录**，`ParentKey` 会分配内存并转小写 |
| E36 | `ops_manager_authorized_delete.cpp:291–295` | 永久删除先走 `DeleteNative`；**本地盘 `workers=1`**，远程盘 8 |
| E37 | `ops_manager_authorized_delete.cpp:386–389` | `diagnostics::deletion::Item` 的参数里，`any_of` 和 `Key()` 在调用前就求值，而函数内部的 `Enabled()` 检查在这之后，所以诊断关闭时也会做规范化 |
| E38 | `common/delete_diagnostics.h:64` | 诊断开启时，每项都要额外调用一次 `GetFileAttributesW` |

## 5. 优化项

每一项都写明：改什么、预期收益（假设）、怎么做同条件对照测量、风险。

### P0：先测量，收益最可能显著

#### P0-1 日志增量化与写盘合并（对应 E1–E3）

- **改法**
  1. 合并写盘：脏标记加上最多 N ms（例如 50–100ms）的延迟落盘。开始项和结束项的落盘合并成一次；进程退出和崩溃前的关键点（项开始执行真实变更之前）仍保证落盘。
  2. 序列化移出锁：`mutex_` 内只复制必要字段的轻量快照，或只复制版本号和指针，在锁外生成 JSON。
  3. 长期方案：改成追加式日志（每个事件一行），定期压缩。这就是 review 文档里尚未完成的“日志压缩”。
- **预期收益（假设）**
  - 大量排队操作（例如连续拖放 200 个独立操作）时，worker 每项可少掉 1–3 次同步写盘。
  - UI 线程等待 `mutex_` 的尾延迟会下降。
  - 单个大批次几乎没有收益。
- **测量方法**
  - 同一台机器、同一个盘，Release 构建，诊断开关状态一致。
  - 场景：Q = 1 / 50 / 200 个小复制或重命名操作连续入队，各跑 ≥5 次，取中位数和 P95。
  - 指标：
    - 总耗时
    - 每项的 `PersistJournal` 耗时，通过临时计时事件统计
    - UI 消息循环的最长阻塞时间
    - 写盘次数（ETW/ProcMon 计数）
- **风险**
  - 合并写盘会扩大崩溃恢复窗口。必须保证“开始真实变更之前，日志已经落盘”这一不变量。
  - 恢复测试（`ReconcileTemporaryFiles` 和相关测试）要补充延迟写盘场景。

#### P0-2 撤销 JSON 增量化，持锁时间最小化（对应 E4–E5）

- **改法**
  1. `3502` 处 worker 调用 `UndoJsonSnapshot`，代码注释写明是为持久化预热缓存，目的是不让 UI 线程自己构建。因此**保留预热**，不要改成 UI 侧懒构建，只降低它的成本（见第 2、3 点）。
  2. 每个 `UndoEntry` 缓存自己的 JSON 片段，生成时只做拼接。
  3. 锁内只拷贝 `shared_ptr<const UndoEntry>`，不深拷贝条目。
  4. `PushUndo` 中让 `CompletedOperation` 和 `UndoEntry` 共享 sources（`shared_ptr<const vector>`）。
- **预期收益（假设）**：撤销历史接近上限（100 步）且单步包含大量 sources 时，每个操作结束可以少掉一次 O(撤销总量) 的深拷贝和序列化。
- **测量方法**
  - 准备 100 步撤销历史，每步 1000 个路径，再执行 50 个小操作。
  - 对比每项结束阶段的耗时，以及 `CaptureUiSnapshot` 的最大等待时间。
  - 新旧版本各跑 5 次。
- **风险**
  - 撤销 JSON 的格式和顺序必须保持字节级一致。建议加一个对比测试：同一份 `undo_`，新旧序列化结果逐字节相同。

#### P0-3 批量回收：逐项开销与通知合并（对应 E10、E30、E34、E35）

- **改法**
  1. `SHChangeNotify`：批内只记录受影响的父目录，批末或每 250ms 对每个父目录发一次 `SHCNE_UPDATEDIR`。单项回收保持现状。
  2. 删除循环中两次不节流的 `SetStatus` 合并进 `SetProgressStatus`。`current_item_token` 的更新可以放在同一个回调里，前提是不破坏“跳过当前项”的语义（要核对 `item_control_`）。
  3. 策略缓存改成按父目录键的小型 map（例如最多 16 项），或者按卷缓存（策略本身就是按卷生效的，需确认）。
  4. `PlanQuotaCompaction`：同一批次内缓存 `$I` 扫描结果，之后只做增量（加上本批新增的部分），不再每次 flush 都全量扫描。
- **预期收益（假设）**
  - 历史上 1000 项回收产生 993 次通知，合并后可降到“父目录数 × 批次数”。
  - 逐项可减少 1–3 次系统调用，以及持锁和通知次数。
  - 墙钟时间的收益**无法预估**，历史计时本身就不稳定（0.9–9.2s），必须先解决测量稳定性。
- **测量方法**
  - 预先清空回收站，并固定回收站大小配置。
  - 关闭杀软实时扫描（或记录其状态），关闭 Explorer 中对该目录的窗口（或固定打开）。
  - 1000 个 4KiB 文件（单父目录 / 10 个父目录两组），交替运行 old/new 共 ≥7 轮，报告中位数、P95 和标准差。
  - 同时记录 `delete_native_recycle` 中的 `scan_ms`、`index_write_ms`、`move_ms`、`notify_ms`，以及 `SHChangeNotify` 次数。
- **风险**
  - Explorer 或桌面视图可能刷新不及时。需要验证打开着的 Explorer 窗口和 Pulse 自身的列表都能在批末正确刷新。
  - 回收撤销本身就有未解决的缺失问题（B1），通知合并不能让它更难排查。

### P1：结构性的复杂度问题，平时不明显，极端场景下显著

#### P1-1 `RunTransfer` 逐条目路径处理（对应 E12–E14、E17）

- **改法**
  1. `skipped_prefixes`：条目是先序枚举（`TransferEnumerationCursor` 采用 pre-order），可以维护一个“当前被跳过子树根”的栈，或记录最近一个跳过前缀。判断变成 O(1) 均摊，退出子树时出栈。如果某些路径会让条目非先序（例如多根、KeepBoth 重写），就退回到按大小写不敏感前缀组织的有序集合。
  2. `dir_key`：复用一个线程局部缓冲区，并缓存最近一次 parent → key 的结果。连续条目大多共享父目录。
  3. `TransferFeatureEnabled`：在 `RunTransfer` 开头一次性读入一个 `TransferFeatures` 结构体，循环内不再读取环境变量。
- **预期收益（假设）**
  - 一般场景收益很小，每条目节省微秒级。
  - 用户跳过大量目录（S 达到数百）且条目数 N 达到 10⁵ 时，可去掉 O(N×S) 的字符串比较。
- **测量方法**
  - fixture：100k 个文件，分布在 1000 个目录中。
  - 对 500 个目录选择“跳过”（用测试钩子自动应答冲突），比较主循环 CPU 时间（`copy_phase_profile`）。
  - 另做一组无跳过的对照，确认没有回退。
- **风险**：前缀判断必须保持当前语义，包括大小写不敏感、`\` 与 `/` 都算分隔符、长路径前缀。

#### P1-2 KeepBoth 目录重写与收尾阶段的平方结构（对应 E15、E18–E21）

- **改法**
  - KeepBoth：不再立即改写后续条目，改为记录一张 `old_prefix → new_prefix` 重映射表，按最长前缀在取用条目时惰性应用。
  - 也可以按目录子树区间（先序下子树是连续区间）只扫描区间内的条目，不扫到末尾。
  - 收尾：`completed_destinations` 和 `completed_sources` 用标记加一次 `erase_if` 压缩，替代倒序中间 erase。`restored_originals`、`skipped_sources` 先排序，再用前缀二分或前缀树。
- **预期收益（假设）**：在大量冲突加 KeepBoth，或回滚大批次时，可从平方级降到 N log N。
- **测量方法**
  - 场景一：目标中已存在 1000 个同名目录，每个目录下 100 个文件，选“全部保留两者”。
  - 场景二：10k 个文件移动到第 9k 个时注入失败，触发回滚。
  - 比较总耗时和收尾阶段耗时。
- **风险**
  - KeepBoth 的撤销语义（M04-003 已修复）必须保持，相关回归测试需要全部重跑。
  - 惰性重映射会影响 `retire_known_conflict` 的时机。

#### P1-3 刷盘降级按卷记录（对应 E28，也作为 bug B4）

- **改法**：`g_no_sync_unsupported` 改成按卷（卷 GUID 或卷根）记录的集合。只有返回 `STATUS_INVALID_PARAMETER`、`STATUS_NOT_SUPPORTED`、`STATUS_INVALID_DEVICE_REQUEST` 这类“能力不支持”状态时才降级，其他错误不降级。
- **预期收益（假设）**：进程曾经复制到 FAT/exFAT/网络盘之后，NTFS SSD 上的复制不再退化为完整 `FlushFileBuffers`。小文件批量场景下，flush 可能占主要成本。
- **测量方法**
  - 同一进程中先向 exFAT 优盘复制 1 个文件，再向 NTFS SSD 复制 2000 个 64KiB 文件，比较 old/new。
  - 另做一组“未先触发降级”的对照：new 版本应与 old 版本在未降级时的表现一致。
- **风险**：判断不支持的错误码时要保守，不确定就保持完整 flush。

#### P1-4 校验路径的状态更新（对应 E11、E7）

- **改法**
  - Verifying/Running 的切换改走节流的 `SetProgressStatus`。
  - `l10n::Get(...)` 的结果在任务开始时缓存，不要每个文件取一次。
  - 文件名只在实际发布时才拼接。
- **预期收益（假设）**：开启校验的小文件批次中，每个文件少 2 次加锁、2 次 `std::function` 调用和 1 次字符串拼接。
- **测量方法**：5000 个 4KiB 文件开启 SHA256 校验，比较总耗时和 UI 刷新次数。
- **风险**：快速完成的文件可能根本不显示 Verifying 状态。这是可以接受的，但需要和 UI 约定。

#### P1-5 永久删除的逐项开销（对应 E10、E36、E37）

- 背景：D2 流式删除在 10k 个零字节文件上慢了 2.9%，说明瓶颈很可能**不在删除后端本身**，而在每项固定的调度、状态和诊断开销上。这一点是推测，需要先用 profiler 确认。
- **改法**
  1. 先用 WPR/ETW 或临时计时事件拆分每项的耗时：`item_control_.Begin/Finish`、`SetStatus`、诊断、`DeleteNative`。
  2. 去掉每项 2 次不节流的 `SetStatus`。
  3. `diagnostics::deletion::Item` 的参数改成惰性求值，或调用方先判断 `Enabled()`。
  4. 评估本地 NVMe 上 `workers>1` 的效果（NTFS 对同一目录的元数据更新可能串行化，收益不确定）。
- **测量方法**
  - 10k 个零字节文件，分两组：全部作为根选中，以及放在 1 个目录里选中目录。
  - 每组 7 轮，诊断关闭；另跑一组诊断开启的。
- **风险**：如果为本地盘打开多 worker，删除失败时的部分完成语义要重新验证，包括 `mutated` 和 `request_replay_unsafe`。

### P2：低风险的小改动，收益有限，可以顺手做

| 编号 | 对应 | 改法 | 预期收益（假设） | 测量 |
|---|---|---|---|---|
| P2-1 | E22–E23 | **按 2.1 节修正**：内存分配只占 0.9 µs，可以忽略。真正的成本是 `GetVolumePathNameW` 本身（C: 深路径 0.5–1.15 ms），它在 `StorageOf`、`Resolve` 的缓存查找之前就被调用。合并进 N1 一起处理 | 见 N1 | 统计一次真实树复制中的调用次数 × 1 ms |
| P2-2 | E27 | 字节数改从 `COPYFILE2_CALLBACK_STREAM_FINISHED` 的 `uliTotalFileSize` 取，或用已持有的句柄取，不再按路径重新打开 | 每个 ≥1MiB 的文件少一次路径解析和 open | 2000 个 2MiB 文件，SMB 目标上更明显 |
| P2-3 | E26 | `CaptureOwnedOutputStamp` 失败一次后记下原因，同一文件内不再每个 chunk 都重试 | 大文件逐 chunk 回调更轻 | 单个 4GiB 文件，比较 CPU 时间 |
| P2-4 | E25 | 预算按设备类型调整（HDD/USB 降低，NVMe 提高），**必须单独 A/B**；历史上的“预算零分配”就是因为 SMB 回退而被撤回 | 不确定 | 按 SSD/HDD/USB/SMB 四类设备分别对照 |
| P2-5 | E16 | 只有在同名冲突、或确实依赖前序结果时才 `drain_pool()`；“串行/大文件之前统一 drain”改为只等待同目标的作业 | 混合大小文件批次中并发更持续 | 混合 1000×64KiB + 20×200MiB 的批次 |
| P2-6 | E6 | `CaptureUiSnapshot` 的队列计数改为在入队/出队时维护计数器 | UI 每次检查少一次 O(Q) 遍历 | Q=500 时测 UI 线程耗时 |
| P2-7 | E34 | 配额扫描结果在同一卷、同一 SID 的回收站上缓存，并以目录修改时间作为失效条件 | 回收站很大（数万项）时，每批少一次全量扫描 | 先往回收站放入 20k 项，再测 100 项回收 |

### 新增：N1 每个新目录解析一次卷（依据 2.1 节实测）

- **证据**
  - `ops_manager.cpp:4967–4983`：`source_device` 和 `target_device` 按父目录缓存，最多 256 项，满了就清空；目录条目以自身作为键。
  - 每次未命中缓存都会调用 `TransferDeviceIdentityCache::Resolve`（`transfer_device_budget.cpp:51–56`），它**先调用** `GetVolumePathNameW`，再查 `volumes_`。
  - 其他调用点：`transfer_engine.cpp:19–27`（`VolumeRoot`/`StorageOf`），`elevated_delete.cpp:429`、`440`，`native_recycle.h:763`（没有缓存的回收站时），`ops_manager.cpp:5029`（目录源）。
- **改法**
  - 在一次传输内，卷根随先序枚举向下继承。只有遇到 reparse 条目（挂载点属于 reparse 点），或者跨越源根/目标根时，才重新解析。
  - `Resolve` 先按“已解析前缀 → 卷根”查缓存，查不到再调用系统 API。
- **实测**：1000 个目录，1149 ms → 1.1 ms（只是解析这一步）。
- **待测**：一次真实的多目录复制里到底调用了多少次。走 pool/budget 路径（`devices_for`，5697）时，次数大致等于不同源父目录数加上不同目标父目录数。
- **风险**：卷挂载点、目录联接和 subst 盘要有专门的回归测试，确认跨卷判断不会出错。

## 6. 代码审查发现的 bug

严重度：**高**＝数据丢失或撤销不可靠；**中**＝结果错误或保护失效；**低**＝潜在问题、可维护性或性能 bug。状态全部是“未修复”，本轮没有改代码。

| ID | 严重度 | 位置 | 问题 | 影响 | 建议 |
|---|---|---|---|---|---|
| B1 | **高** | 回收撤销链路：逐项重放，耗时随“项数 × 回收站大小”增长（见第 12 节） | 历史实测中产品撤销只恢复了 785/1000（基线）和 199/200（新版第 3 轮），剩余由人工恢复 | 用户以为可以撤销，实际文件留在回收站 | **已修复（第 12 节）**：分批还原，部分失败后精确续做；压测改为等待整个撤销完成。真实回收站端到端验证受火绒拦截，待放行后补测 |
| B2 | **中（已实机复现，见 2.1 节）** | `native_recycle.h:615–617`、`640` `MeasureFolderFast` | 子目录 `FindFirstFileExW` 失败时静默跳过；`FindNextFileW` 的错误也不检查 | 目录大小被少算，`$I` 中记录的大小错误，**配额检查可能被绕过**（超过回收站容量的目录仍被移入回收站） | 枚举失败时返回错误并回退到 Shell 路径，或者至少把结果标记为“大小未知，不做快速路径” |
| B3 | 中 | `native_recycle.h` 约 860–870 | `$I` 写入后没有 flush，就直接 `MoveFileExW` | 崩溃或断电时可能出现 `$R` 已存在但 `$I` 内容为空或损坏，项目在回收站中不可见或不可还原 | `CloseHandle` 前调用 `FlushFileBuffers`（单项成本需测量，可只在批末统一 flush，但顺序要求要论证清楚） |
| B4 | 中（性能；代价已量化为每文件慢 2.3 倍，触发条件未复现） | `transfer_engine.cpp` 约 364、381–383 | `g_no_sync_unsupported` 是全局变量，任何一次失败都会让之后所有卷永久改用完整 flush；任何负的 NTSTATUS 都会触发降级 | 一次 FAT/网络盘复制之后，NTFS 上的复制持续变慢，用户无感知 | 见 P1-3 |
| B5 | 低 | `transfer_engine.cpp:370`、`392` | 恢复只读属性时不检查 `SetFileAttributesW` 的结果 | 目标文件可能丢失只读属性，元数据与源不一致 | 检查返回值并记入失败列表或诊断 |
| B6 | 低（潜在） | `transfer_engine.cpp:215–219` `Submit` | 移动之后又读取 `job.index` 和 `job.AdmissionBytes()`。这两个字段目前都是标量，移动后仍然有效，所以**暂时无害** | 如果以后 `AdmissionBytes()` 依赖可移动成员，就会读到错误的值，预算记账出错 | 在 move 之前先保存 `index` 和 `bytes` |
| B7 | 低 | `elevated_delete.cpp:475` | 策略缓存只保存最近一个父目录 | 跨多个父目录的选择会反复调用 `QueryRecyclePolicy`（性能问题） | 见 P0-3 |
| B8 | 低 | `ops_manager_authorized_delete.cpp:386–389` | 诊断关闭时仍对每项执行 `Key()` 规范化和 `any_of` | 白做的 CPU 工作 | 调用前先判断 `runtime::Enabled()` |
| B9 | 低 | `ops_manager.cpp` 6180–6191 | 回滚过程在 vector 中间 erase，并且每项都对 `moved_sources_` 做 `erase_if` | 大批次回滚出现平方级耗时，表现为“取消后卡住” | 见 P1-2 |

另外核对了 `module-audit-v2-status.json`：M03/M04 系列（文件操作和枚举）的审查项都标记为“已修复，定向验证已运行”或“上一轮已实测”。本文 B1–B9 是**本轮新发现**或**历史文档中仍未关闭**的问题，不与上述编号重复。

## 7. 统一测量规程（所有 P0/P1 项通用）

1. **同条件**
   - old 和 new 两个构建来自同一编译器、同一配置（Release），只差一个优化项的补丁。
   - 两者在同一台机器、同一块盘、同一个 fixture 上**交替运行**（ABAB…）。
2. **环境记录**
   - 盘的型号和文件系统、剩余空间、杀软状态、诊断开关、是否打开着 Explorer 窗口、回收站大小配置。
3. **次数与统计**
   - 每组至少 5 轮（回收站场景至少 7 轮）。
   - 报告中位数、P95 和最小/最大值；离散度过大时，**不下结论**，只记录数据。
4. **正确性**
   - 每轮结束后校验目标树：文件数、总字节数，抽样或全量 SHA256。
   - 校验撤销结果和日志恢复结果。
   - 性能补丁必须通过现有回归：`copy_*`、`delete_*`、`controlled_*`、`elevated_*`、`enum_*`、`ops_test_main`。
5. **记录**
   - 原始日志放在 `bench_data/`，结论写进新的日期文档。
   - 没有达到收益的项，如实记为“无收益”或“撤回”，参考“预算零分配”被撤回的写法。

## 8. 风险与边界

- **崩溃一致性**：P0-1 和 B3 都涉及“什么时候落盘”。优化不能破坏“开始真实变更之前，日志已持久化”和“`$I` 先于 `$R` 存在”这两个不变量。
- **UI 线程**：本方案中所有改动都在 worker 线程或锁的粒度上进行，不会把任何文件系统或 Shell 调用移到 UI 线程（遵守 `AGENTS.md`）。
- **Explorer 交互**：合并通知（P0-3）可能影响第三方文件管理器和桌面的实时刷新，需要人工验证。
- **并发删除**：本地多 worker 删除（P1-5 第 4 点）对机械盘可能有害，只应在测量确认之后，按设备类型启用。
- **代码结构**：按 `AGENTS.md` 要把大文件拆成专用的 `.h/.cpp`。日志增量化（P0-1）建议放到独立的 `ops_journal.h/.cpp`，撤销序列化（P0-2）放到 `undo_serialization.h/.cpp`，不要继续往 `ops_manager.cpp` 里加内容。
- **文案**：如果新增状态文案，只能加到 `tools/l10n/catalog.tsv`。
- （方案阶段）本文档只新增这一个文件，**没有修改任何代码**，也没有改动工作树中已有的未提交改动。实施阶段的改动见第 10 节。

## 9. 建议执行顺序

1. 先补测量工具：给日志写盘、撤销序列化、删除每项的各阶段加计时诊断事件（默认关闭），并解决回收计时不稳定的问题。
2. 修复 B1（回收撤销缺失）和 B2（测量遗漏导致配额被绕过）。这两项影响正确性，优先级高于性能。
3. 做 P0-1、P0-2，各自单独做 A/B，写测量文档。
4. 做 P0-3，再做 P1-3（改动小、风险低，但需要可复现的降级场景）。
5. 其余 P1、P2 按测量结果决定是否继续。

## 10. 实施记录（分支 `perf/file-ops-opt-20261120`，2026-11-20）

所有数字来自本机（i5-13600KF，48 GB，C: 为 SATA SSD，NTFS，火绒在运行），条件相同、新旧交替运行，取中位数。日志都在 `bench_data/opt-verify-20261120/`。**没有提交**；改动只涉及下表中的文件。

### 10.1 改了什么

| 项 | 文件 | 改动 | 语义 |
|---|---|---|---|
| P0-3 | `native_recycle.h`、`elevated_delete.cpp/.h`、`ops_manager_authorized_delete.cpp` | 批量回收时 Shell 通知按父目录合并、1 秒节流，批末统一发送；增加通知计数指标 | 每个父目录仍会收到更新通知 |
| P0-1 | `ops_manager.cpp`（`WorkerThread`） | 队列中下一项会立刻开始时，推迟本项结束时的日志写盘，与下一项开始时的写盘合并；清理路径和循环退出时必定写盘 | 崩溃时最多丢失“上一项已结束”这一条状态（下一项开始时会一并写入） |
| P1-1 | `ops_manager.cpp`、新增 `path_prefix_set.h` | `skipped_prefixes` 由线性列表改为 `PathPrefixSet`：成员少于 16 个时用原来的线性扫描，否则按路径边界做大小写折叠后的哈希查找 | 与 `StartsWithPath` 等价（随机 60 万次查询，0 处不一致） |
| P1-2 | `ops_manager.cpp` | KeepBoth 目录改名：只扫描本根目录所在的那一段，以及根目标路径与被改名目录互相嵌套的其他段，其余段整段跳过；布局异常时退回全量扫描。回滚：一次压缩遍历，`moved_sources_` 只做一次 `erase_if` | 完全保留原语义，包括同名根目录、大小写不同的同级目录、嵌套的根目标路径这三种“非连续”情况 |
| N1 | `transfer_device_budget.cpp/.h` | `Resolve` 缓存“路径 → 卷根”；一个路径若在已解析的祖先之下（最多 4 层），并且祖先本身及中间每一级都是存在的、非 reparse 的目录，就沿用祖先的卷根（每级一次 `GetFileAttributesW`）；挂载点、junction、缺失路径、长路径都退回 `GetVolumePathNameW` | 与每次调用 `GetVolumePathNameW` 的结果一致 |
| P0-2 | `ops_manager.h`（`UndoEntry` 新增拷贝时清空的 `UndoJsonFragment`）、`ops_manager.cpp`（`UndoJsonSnapshot`） | 每个条目缓存自己的 JSON 片段。锁内只收集已缓存的片段，并拷贝发生变化的条目（通常只有刚记录的那条）的持久化字段；锁外序列化并拼接；发布时修订号未变才回填缓存。片段只在条目的形状（类型、标志、名称、路径数量）与生成时一致时使用；`PushUndo` 合并到已有条目时显式作废 | 输出与原序列化器逐字节一致。方案中的第 4 点（`CompletedOperation` 与 `UndoEntry` 共享 sources）没有做 |
| B3 | `native_recycle.h`（`RecycleItemNative`） | `$I` 写完后、关闭和 `MoveFileExW` 之前调用 `FlushFileBuffers`；落盘失败按写入失败处理（删除 `$I`，回退，源文件不动） | 修复：保证断电后不会出现“`$R` 在、`$I` 空或损坏”。**这是按用户选择、以速度换断电安全的改动**，见 10.2 |
| B2 | `native_recycle.h`（`MeasureFolderFast`）、`elevated_delete.cpp/.h`（`MeasureFolder`、`CheckRecycleFits`） | 有容量上限需要证明时，子目录无法列出或列出中途出错，测量失败并返回 Win32 错误，不再按部分大小放行；回收站与已用空间一样大（无需证明）时行为不变 | 修复：此前 Shell 回退路径会把超过容量的目录交给 Shell，而 Shell 在 `FOF_NO_UI` 下会把放不下的项目永久删除 |

### 10.2 产品级与代码级对照

| 项 | 方法 | 旧 | 新 | 说明 |
|---|---|---|---|---|
| P0-3 | 产品 `DeleteWithCurrentToken`，1000 × 4 KiB，私有回收站，3 轮 × 3 次（`abr_ab.log`） | 1523 ms | 1058 ms | 约 1.44 倍；失败 0、残留 0；Explorer 刷新效果未验证 |
| P0-1 | 真实 `OpsManager`，Q 个双文件 Copy 请求，3 轮 × 5 次（`journal_ab.log`） | Q=50：683/697/638；Q=200：2828/2830/2657 ms | Q=50：513/573/605；Q=200：2465/2195/2189 ms | 日志替换次数减半（Q=200：399 → 200）；约快 5–26% |
| P0-1（无收益） | 单项请求（CreateFolder 等，走交互通道，`journal_ab_run1.log`） | 38 次替换 | 38 次替换 | **没有收益**：交互通道逐项写盘的逻辑未改 |
| P0-2 | 真实 `OpsManager`：载入 100 步 × P 个路径的历史，再记录 50 次（每次新增一条并裁剪最旧一条），每次后取快照；同时一个线程循环调用 `CanUndo()` 记录等锁时间。新旧交替 5 轮（`undo_ab.log`） | P=100：快照 6.4–7.2 ms，等锁最大 2.2–3.6 ms；P=1000：64–69 ms，等锁最大 19–30 ms | P=100：0.96–1.0 ms，等锁最大 0.08–0.16 ms；P=1000：12.9–14.2 ms，等锁最大 0.44–0.67 ms | 新旧每次快照的哈希完全相同。冷启动后的第一次完整构建没有变快（P=1000：约 87 → 78 ms）。P=1000 剩下的约 14 ms 是拼接 13 MB 完整载荷本身的成本 |
| B3（代价） | 产品 `DeleteWithCurrentToken`，1000 × 4 KiB，私有回收站，3 遍 × 3 轮，新旧交替（`b3_product_ab.log`；旧 = 本分支加 B3 之前） | 9 轮中位数 1023 ms | 2506 ms | **慢约 2.45 倍（每项多约 1.48 ms）**，失败 0、残留 0。抵消了 P0-3 的收益，比 P0-3 之前的 1523 ms 还慢。单独的落盘方式对比见 10.4 |
| P1-1 | 产品头文件 `PathPrefixSet` 对照原线性扫描，101000 条目（`prefix_ab.log`） | S=10/100/500/1000：19/155/764/1521 ms | 22/29/30/29 ms | **S=10 时慢约 3 ms（每条目约 27 ns）**；S≥100 时大幅加快 |
| P1-2 KeepBoth | 新旧代码逐字对照，结果逐条比对（`p12_ab.log`） | 1000 根各改名：1140 ms；改名每个目录：2222 ms；10 根：20 ms | 37 / 53 / 9.7 ms | 6 种布局（含 3 种非连续情况）结果全部一致 |
| P1-2 回滚 | 同上，N=100000 | B=1/10/100/1000：94/713/5877/20550 ms | 11/18/25/66 ms | 结果一致 |
| N1 | 链接分支的 `transfer_device_budget.cpp`，对照原 `Resolve`，2021 个目录（`n1_ab.log`） | 1897 ms | 96 ms | 系统调用 2021 → 1 次；身份 0 处不一致 |

### 10.3 测试（分支构建，TMP 指向基准目录）

- `pulse_delete_progress_test`：17/17 PASS（新增 4b：无法列出的子目录会让带上限的原生回收和 Shell 路径预检都失败且不改动文件；无上限时照常放行；恢复权限后按全量测量判定为过大）。`FindNextFileW` 中途出错这一分支难以构造，没有专门的用例。
- `pulse_elevated_delete_test` 16/16，`pulse_recycle_policy_test` 7/7，`pulse_delete_preflight_test` 5/5，`pulse_delete_delivery_test` 13/13，退出码均为 0。
- `pulse_ops_test` 的撤销专项模式（新旧程序对照）：`--undo-history-persistence-only` 新 13/13（含新增“追加、裁剪后的增量快照与原序列化器逐字节一致”）、旧 12/12；`--undo-history-progress-only`、`--undo-history-summary-only`（9/9）新旧都通过；`--undo-redo-history-only` 新旧都是 29 PASS / 7 FAIL，36 行结果逐行相同（失败项都是回收相关）。
- `pulse_generated_output_test`：构建通过；运行时在撤销/重做（会把生成的输出移进回收站）处挂起超过 5 分钟、CPU 接近 0，已强制结束。**这个目标没有旧基线程序，无法证明与本分支无关**；现象与 `pulse_ops_test` 中被火绒拦截的回收步骤相同。
- `pulse_transfer_device_budget_test`：全部 PASS，新增 4 项（普通目录沿用卷根且与全新解析一致、缺失目录退回系统调用、同卷 junction 及其内部目录由系统解析）。跨卷 junction 一项 **SKIP**：在 D: 上执行 `mklink /J` 失败（推测是火绒拦截）。
- `pulse_ops_test`：在本环境下，旧基线（未改动的源码）和分支都会因回收/删除步骤的 `WaitOpDone` 超时而极慢，无法跑完。两者各运行约 12 分钟，共同到达的前 50 项结果**逐项完全相同**（32 PASS / 18 FAIL）。这 18 项 FAIL 在基线中同样存在：中文系统下的英文 UI 文案、“把目录移动/复制到自身”的提示文案、回收与删除（推测为火绒拦截）。**它们不是本分支引入的，但本环境也无法证明这些用例在分支上通过。**
- 构建：`pulse` 及上述测试目标 `BUILD_EXIT=0`；改动文件没有新增警告。剩余 3 条 /W4 警告在 `epub_document.cpp`、`global_search_window.cpp`、`app_runtime.cpp`，属于工作树中原有的改动。

### 10.4 未做 / 受阻

- **B3 落盘方式的选择依据**（已按用户选择采用完整 `FlushFileBuffers`）：按 `RecycleItemNative` 的真实顺序（创建 `$I`、写 544 字节、[落盘]、关闭、`MoveFileExW` 一个 4 KiB 文件），1000 项、4 轮轮换顺序、失败 0（`b3_ab.log`）。每项耗时：不落盘 0.85 ms；`FlushFileBuffers` 2.17 ms；`NtFlushBuffersFileEx(FILE_DATA_SYNC_ONLY)` 2.19 ms；`FILE_FLAG_WRITE_THROUGH` 1.89 ms；`NtFlushBuffersFileEx(NO_SYNC)` 1.69 ms。逐项落盘至少让单项成本翻倍，1000 项批量回收会从约 1.06 s 变为 1.9–2.4 s，比 P0-3 之前的 1.5 s 还慢。用户选择了断电也安全的完整落盘。若以后要找回速度，可评估“按批写好 `$I`、一次性并发落盘、再统一重命名”的方案（未实测）。

- P0-2 第 4 点（sources 共享）、P1-3（需要 FAT/网络盘复现降级）、P1-4、P1-5（需要 HDD 数据）、B1：未做（B1 后来在第 12 节修复）。
- 交互通道的单项请求不会从 P0-1 受益；如果要优化，需要另行设计交互通道的写盘合并，并单独测量。
- P1-1、P1-2 没有做产品级端到端 A/B：需要冲突对话框的 Skip/KeepBoth 应答。现有结论来自逐字复制的新旧代码对照。

## 11. 第三批：B3 分批落盘原型、操作期间界面卡顿排查、粘贴延迟修复（2026-11-20）

### 11.1 B3 分批落盘原型（只测量，产品未改）

`bench_data/opt-verify-20261120/b3batch.cpp`，1000 项 × 4 轮，各方案交替执行，每轮轮换顺序，失败 0。每种方案都保持“每个 `$I` 在自己的 `$R` 重命名之前落盘”。

| 方案 | K=64 每项中位数 | K=256 每项中位数 |
|---|---|---|
| none（B3 之前，不落盘） | 0.780 ms | 0.812 ms |
| flush（逐项 `FlushFileBuffers`，分支现状） | 2.015 ms | 1.967 ms |
| batch_seq（写完 K 个 `$I`，再逐个落盘，最后统一重命名） | 2.025 ms | 1.757 ms |
| batch_par（同上，8 线程并发落盘） | 1.379 ms | 1.191 ms |
| batch_one（每个 `NO_SYNC`，每批只做一次 `FlushFileBuffers`） | 1.464 ms | 1.327 ms |

日志：`b3batch_k64.log`、`b3batch_k256.log`。

结论：并发落盘能省回落盘开销的约 35–40%（1.97 → 1.19 ms），但仍比不落盘慢约 0.4 ms/项。要用上它，`RecycleItemNative` 得从“逐项写、逐项重命名”改成按批处理，而且批内任一项失败时的回滚路径都要重写。这次没有改产品，留作后续可选项。

### 11.2 文件操作期间界面卡顿：排查结论

用户反馈：删除 node_modules 时粘贴没反应；复制过程中点“返回上一级”很久才有反应。

- **已安装的 1.0.61 不包含之前那轮界面响应修复。** 修复包括交互通道、`OpsNotifyGate` 和 `ops_notify_dispatch_slow` 诊断。这些改动目前只在工作区，还没提交：`ops_notify_gate.h` 和 `ops_notify_dispatch.h` 未纳入版本控制，`app_main.cpp` 和 `ops_manager.cpp` 有未提交的修改。已安装的 exe 里找不到 `ops_notify_dispatch_slow` 这个字符串，分支编译出的 exe 里有。本机运行时日志（`app-51556`）里也没有这次操作的记录。
- **当前源码没有复现导航卡顿。** 这次扩展了已有的隔离探针（`PULSE_SMALL_FILES_LIVE_PROBE`，`--test-instance`，私有配置目录），新增 `src/bench/small_files_tree_scenario.inc`，用环境变量 `PULSE_TEST_SMALL_FILES_TREE` 开启。测试数据是 node_modules 形状的目录树：600 个包、21900 个文件、4051 个目录、47 MB，由 `gen_tree.exe` 生成。场景在复制和删除进行中反复进入目录、再用真实的 `GoUp` 命令返回。4 次运行（`tree_cur/old1/new1/new2.log`）结果一致：
  - 复制中的进入和返回：p50 31 ms，最大 47–63 ms。复制完成后的对照组同样是 31 ms。
  - 投递消息延迟：p95 31 ms，最大 47 ms。
  - 复制 21900 个文件耗时 20.1–21.3 s，文件数逐一核对一致。
- **发现的真问题：操作期间 WM_TIMER 被饿死。** 操作进行中窗口不停重绘，每次运行约 3300 次 WM_PAINT、5100 次 `WM_UI_KICK`。Windows 只有在没有待处理的重绘时才会生成 WM_TIMER。一次性 25 ms 定时器从设置到触发的实测：
  - 复制中：3968、9812、15375 ms；
  - 删除中：2016–4203 ms；
  - 空闲对照：79–438 ms。

  之前的修复已经让操作通知（`ResumeOpsNotify`）在被饿死时由投递的帧和 UI 处理兜底。但**粘贴**读完剪贴板后，只靠 25 ms 的 `kFileClipboardPasteTimer` 轮询来提交，所以操作进行中按 Ctrl+V 要等几秒到十几秒才会真正提交，表现为“没反应”。这和用户看到的“删完才弹出复制窗口”一致。

### 11.3 修复：粘贴即时唤醒 + 排队提示

改动：
- `app_messages.h`：新增 `WM_FILE_CLIPBOARD_READY = WM_APP + 147`。
- `app_ops_ui.cpp`：后台读完剪贴板后投递这条消息，UI 线程收到后执行 `PollFileClipboardPaste`。25 ms 定时器保留作兜底（超时和投递失败时用）。
- 排队提示：粘贴或拖放提交的复制、移动，如果 400 ms（`kQueuedTransferNoticeMs`）后仍处于 `Queued`，且有别的任务正在执行，就弹出一次提示：“已加入队列 · 当前文件操作完成后会自动开始，无需再次粘贴”。检查在 `WM_OPS_NOTIFY` 中进行（`NoteQueuedTransfers`），登记点在 `SubmitTreeTransfer`。
- 新增字符串 `TransferQueued` 和 `TransferQueuedDetail`（7192/7193，简中、英文、繁中），用 `gen_strings.py` 重新生成。

对照（同一次运行、同一负载）：

| 机制 | 复制中 | 删除中 |
|---|---|---|
| 旧：粘贴等 25 ms WM_TIMER | 3968–15375 ms | 2016–4203 ms |
| 新：粘贴走投递消息 | p95 31 ms，最大 47 ms | p95 16–31 ms，最大 32–47 ms |

- 排队提示：大复制进行中，用真实的 `SubmitTreeTransfer` 再粘贴一个文件夹。两次运行中，任务都确实在排队，提示分别在 453 ms 和 469 ms 出现，主复制结束后它自动执行完成。
- 测试：`pulse_tree_transfer_destination_test` 23/23 通过；`check_no_pick.py` 和 `check_service_text.py` 通过；`pulse` 构建 BUILD_EXIT=0，只有已知的 3 个旧警告。
- 探针用完后已把 `build` 恢复为 `PULSE_SMALL_FILES_LIVE_PROBE=OFF` 并重新编译。

### 11.4 限制与未验证

- 新粘贴路径的端到端 Ctrl+V（真实剪贴板）没有自动化：那样要往系统剪贴板写文件，会覆盖用户的剪贴板。粘贴延迟的对照是机制级的（25 ms WM_TIMER 对比投递消息，同一次运行中测得）。建议手动验证：删除大目录时 Ctrl+V，应立即看到“已加入队列”。
- 其他依赖主窗口 WM_TIMER 的功能（悬停、提示等）在操作期间同样会被推迟，这次没有逐一排查。
- 排队提示依赖操作通知触发。如果主任务长时间不发进度，提示可能晚于 400 ms 出现。
- `gen_strings.py` 重新生成的头文件和资源文件里，也包含工作区原有的未提交 catalog 修改（约 +266 行）对应的内容。
- 之前构建中断过一次，`.ninja_log` 损坏（ninja 提示 `premature end of file`），导致后续几次 `pulse` 都是全量编译。现在已恢复。

## 12. B1：回收后撤销恢复不全（2026-11-20）

### 12.1 根因

撤销一个回收删除时，撤销条目里只存了原路径（常规回收不记录 `$R` 回执），还原时要按原路径到 `<盘>\$Recycle.Bin\<SID>` 的 `$I` 索引里反查唯一版本。

- **11 月 8 日整合提交（e08e818）之前**：撤销是一个 `RestoreRecycle` 请求，带全部原路径（`inv.sources = e.sources`）。10-29 文档里“旧版 785/1000”测的就是这一版。
- **现在**：为了支持断点续做，`QueueHistoryRequestLocked` 把每一项拆成单独的 `RestoreRecycle` 请求。每个请求都新建一个 `RecycleRestoreIndex`，重新扫描并读取回收站里**全部** `$I` 文件。总耗时约为“撤销项数 × 回收站项数”。
- 另外，任何一项失败（例如同一路径在回收站里有两个版本）都会中止剩下的所有请求。

压测 `pulse_ops_bench --only recycle` 本身也有缺陷：它在 `Undo()` 之后只等到 `completed_ops` 第一次变化（也就是第一个分项请求结束），就开始统计并 `RemoveTree`、`Stop()`。所以它报告的“恢复数”，其实是计数那一刻撤销进行到了哪里。

10-29 记录的 785/1000 和 199/200 没有原始日志，测的又是更早的实现，**无法确认当时的具体成因**。本节只修复当前代码里可以复现、可以测量的问题。

### 12.2 复现与量化（产品代码，组件级）

`restore_index_ab`（基准目录）直接调用产品的 `ReadRecycleOriginal`、`RecycleRestoreIndex`，以及与 `RestoreOneFromRecycle` 相同的加载和校验回调，对模拟的 `$I`/`$R` 目录按原路径解析。这里只计解析耗时，不含移动文件。

| 撤销项数（= 回收站项数） | 每项新建索引（现状） | 一批共享索引 |
|---|---|---|
| 200 | 1824 ms（3 轮中位数） | 26 ms |
| 500 | 12502 ms | 80 ms |
| 1000 | **53340 ms** | **119 ms** |

增长近似平方。用户回收站里还有其他条目时，每项的代价还会更高。

### 12.3 修复

- `QueueHistoryRequestLocked`：撤销回收删除时，每 512 项合成一个 `RestoreRecycle` 请求，仍按最新到最旧的倒序。一个请求只扫描一次回收站索引。其他类型（移动、重命名、复制、重做回收）不变，仍逐项重放。
- 精确续做：这类请求走进程内已有的 `RestoreOneFromRecycle` 路径（Shell host 只回一个 HRESULT，无法知道哪些项成功）。每批实际还原的原路径记入 `history_restored_`；部分失败后重试，只跳过这些项。不按“文件是否存在”来猜。
- 撤销条目本身不删减，所以之后重做仍会回收全部原项。整步完成后清除续做记录；`UndoFromJson` 也一并清空。
- 如果重试时发现所有项都已还原（例如最后一项之后才取消），这一步直接完成。`StartHistoryLocked` 此时返回成功，不再误报不可撤销。
- 批量还原过程中逐项更新进度（当前项、完成数、百分比）。
- 压测：`ops_bench_main.cpp` 的 recycle 模式改为等 `HistoryProgress()` 的完成序号变化且不再忙，再统计，并输出撤销耗时、是否完成、是否成功。

### 12.4 验证

- 新增 `pulse_recycle_undo_batch_test`（不访问回收站，用友元直接检查重放队列，并模拟每批的结果），**18/18 PASS**：
  - 1200 项：3 个请求（512、512、176），倒序；
  - 第 2 批只恢复 100 项后失败：步骤保留，记录 612 项；
  - 重试：只剩 588 项，分 2 个请求，不含任何已恢复项，最后一批以最旧的项结尾；
  - 完成后进入重做栈，续做记录清除；重做仍逐项回收全部 1200 项；
  - 全部已恢复但报失败：重试直接完成；
  - 移动撤销仍逐项重放。
- `pulse_ops_test` 相关模式（当前代码，与 B1 改动前同条件对比）：
  - `--undo-history-persistence-only` 13/0；
  - `--undo-history-summary-only` 9/0；
  - `--undo-history-progress-only` 退出码 0；
  - `--undo-redo-history-only` 29 PASS / 7 FAIL，**FAIL 名单与改动前完全相同**（分支前原版也是 29/7，都是回收站拒绝访问导致的环境性失败）。
- 构建：`pulse_ops_bench`、`pulse_ops_test`、`pulse_recycle_undo_batch_test` 均为 BUILD_EXIT=0，没有新警告。`pulse`（头文件改动后全量重编 817 步）BUILD_EXIT=0，只有已知的 3 个旧警告。

### 12.5 受阻：真实回收站端到端

这台机器上，开发构建（未签名）的 `pulse_ops_bench` / `pulse_shell` / `pulse_ops_test` 写回收站时被拒绝：诊断事件 `delete_native_recycle fallback_error=5`，任务停在 `authorization=ActionRequired`，推测是火绒拦截。因此：

- 1000 项“回收 → 撤销 → 断言全部恢复”的端到端用例，以及新旧产品的端到端耗时对比，**这次都没能执行**；
- `--restore-index-only` 和 `--recycle-audit` 同样卡在授权，已结束进程；
- 每次运行后都确认过：回收站里只有 8 月的 4 个旧条目，测试目录已删除。

需要用户在火绒里临时放行 `C:\Users\SS\Desktop\pulse\build\` 下的这几个程序，或者手动做一次验证：回收 1000 个文件，撤销后应在数秒内全部回来。放行后可以直接用 `b1run.cmd 1000 <log>` 补测。
