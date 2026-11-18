# 复制重构持续交付核对（2026-10-31）

本报告补充 copy-recovery 之后的改动。整份原方案仍未全部验收；未验证、未采用和已发布分开记录。

## 本轮产品改动

- 进程内设备预算加入按任务轮转的等待队列。一个任务的多个 worker 不能连续绕过已登记的其他任务；非冲突设备仅在全局条数/字节预算仍给旧等待者留有余量时前进。
- 复制池与串行执行使用同一任务标识。暂停、取消、停止时注销等待票据，避免暂停任务阻塞后续工作。已运行的 I/O 不被强行抢占；公平性依赖等待线程能继续获调度，不承诺固定时限。
- `PULSE_FAIR_TRANSFER_BUDGET=0` 可单独回退排队策略，不撤销设备容量、UNC 策略或大文件校验重叠预算。
- 异类覆盖备份创建和清理支持进入操作前取消；保留身份绑定的文件/空目录处理，非空备份继续保留并报告。

## 已取得的功能证据

- `build/copy_completion_fair_final.log`：任务轮转、多 worker、旧等待者、未知设备、超预算文件、全局预算、票据生命周期及回退开关通过。
- `build/copy_completion_pool_final.log`：池内内容校验、暂停恢复、取消、停止，以及等待预算时注销票据通过。
- `build/copy_completion_backup_final.log`：身份绑定备份、恢复、清理及入口取消检查通过。
- `build/copy_completion_security_final.log`：实际 ACL 继承和拒绝读写与原生 CopyFile2 对照；实际 EFS 加密文件的普通池/校验提交与原生路径对照，加密状态及完整明文 SHA256 一致。测试前后既有证书集合和默认选择不变，未生成或导入证书。云占位因无提供程序未测。
- `bench_data/setup_isolated_6fe3bab8202e41d1a5609d745e702b4a/evidence.json`：copy-followup 安装、升级到 copy-recovery、卸载实际通过，两次 33 个包载荷核验，隔离安装目录和测试注册清理通过；真实用户偏好、服务、启动项及安装注册不变。此为隔离安装生命周期，不代表机器级提权服务验收。

## 拒绝上线的实验及剩余边界

1. 非空备份树自动清理实验：默认数据流的共享锁不能阻止新增 ADS；delete-pending 门控虽能阻止新增流，但撤销 pending 失败及进程退出路径存在未验证数据丢失风险。实验已撤回，归档于 `bench_data/copy_completion_tree_experiment/`，产品继续保留非空备份，不宣称自动清理完成。
2. 普通覆盖的原子授权与完整撤销竞争未完成。`build/copy_completion_contract_matrix.log` 的真实句柄矩阵发现：能阻止外部写入/替换的访问共享组合，也会阻止原生 ReplaceFileW；只读属性句柄不能提供该保护。不得把提交前路径检查称为原子保证。
3. `undo_file_version` 仅为独立实验：对象身份、版本和可选限额 SHA 收据经测试，但尚未接入生产回收站撤销。默认不全文件读取；目录身份不授权递归删除新子项。
4. 流式扫描保持默认关闭；旧平铺实测出现退化，尚无稳定启用证据。块引擎与零分配预算实验也未进入默认路径。
5. 外部 NAS/USB、真实断线/拔盘/磁盘满/配额、云占位、机器级 UAC/服务生命周期没有本机充分条件。错误注入与 SMB 回环不能冒充这些实际场景。
6. 未安装 TeraCopy，也未完成完全冷缓存 Explorer/TeraCopy 同机对照；不宣称全面超过竞品。

后续测量、最终构建、安装包及当前版本隔离安装结果在本报告末尾追加。上述未完成项目不得因打包成功改为通过。

## 16 GiB 实际边界

`build/copy_completion_16gib.log`：单文件 17,179,869,184 字节，开启校验；复制含内部校验 342,896.673 ms，额外独立复核 258,853.207 ms，首次复制进度 43.924 ms；测试进程峰值工作集 10,887,168 字节（不是完整 GUI 常驻内存）。内容/源不变/属性/时间/ADS 检查通过。

测试文件采用对齐无缓冲写入、write-through，并在生成时计算预期 SHA；复制前不预读源内容。没有清空全机缓存或设备缓存，故只能称“未主动预热”，不是完全冷缓存，也没有本次旧版对照，不能换算提速百分比。成功后仅删除本次 GUID fixture。

## 本轮定向功能及构建

- `build/copy_completion_release_build.log`：正式 pulse（含依赖的提权执行器）与 pulse_ops_test 构建完成。
- `build/copy_completion_alias.log`：硬链接、junction 别名的双向 Copy/Move Replace 保护通过；源 junction 的重解析数据保持，引用目标及子文件身份、SHA 不变。文件 symlink 创建 Win32=1314，明确跳过，未算通过。
- `build/copy_completion_backup-file.log`、`copy_completion_backup-nonempty.log`、`copy_completion_backup-restore.log`：正常异类覆盖完成；非空备份以准确路径报告 ERROR_DIR_NOT_EMPTY，原子项内容/属性保持；源共享冲突导致复制失败时原目录恢复完整且不残留备份。
- 本轮未运行完整历史回归，也未改动用户真实文件、偏好或实际安装。

## 公平调度首轮五对实测

同一归档 exe，每个场景随机交替公平开关，各五对，完整内容核验。开关只控制等待票，不控制原有预算；所有样本保留在 `bench_data/copy_completion_fair_compare/`。

|场景|关闭中位 ms|开启中位 ms|变化|
|---|---:|---:|---:|
|2048 个 4KiB 文件|685.002|787.339|+14.9%|
|1024 个文件 SMB 回环|713.062|691.305|-3.1%|
|4×65MiB，带校验|1461.734|1474.522|+0.9%|

公平性不是本轮单任务提速承诺。小文件结果触发带阶段诊断的定向复核，不删首轮结果。脚本在发现构建进程时等待，但不能隔绝其他会话中途开始的负载；因此保留范围、异常样本，不按单个最优值宣称收益。

## 小文件疑点复核

`bench_data/copy_completion_fair_flat_check/` 保存五对带阶段诊断的独立复核：关闭中位 782.670 ms，开启 723.075 ms（-7.6%）；十次 `copy_phase_profile` 的 `budget_wait_us` 均为 0。未进入预算等待的场景，不能把首轮差值归因于票据 FIFO/2ms 轮询。两轮方向相反，保留全部样本，不主张单任务稳定提速，也不能保证所有环境无回退。

已确认预算竞争时存在有界轮询，公平性测试验证的是轮转及取消语义；未据未发生的等待计时继续增加唤醒机制。当前保留默认开启、独立回退开关，设备容量和校验策略保持原有契约。

## 当前安装包及实际交付

- 安装包：`dist/PulseSetup-1.0.58-copy-completion.exe`，10,186,944 字节，9.72 MiB。保留之前 copy-recovery 等包，未替代用户安装。
- 包 SHA256：`e8c1f50fbd5872b617b625dd6e0b804745639c10dddc7399f5010a39af94fcd6`。
- pulse.exe SHA256：`227e5595c46ff6ec07321bd7f3c553f5dbf0c838012cbe69010db7d161db1984`。
- `build/copy_completion_package_verified.json`：压缩包、清单、安装器 stub 和全部 33 个载荷文件逐字节及 SHA 校验通过。本轮涉及源码哈希在 `build/copy_completion_source_hashes.json`。
- `bench_data/setup_isolated_fb59ab9c9a1941e38b81acd0903e2681/evidence.json`：copy-recovery 安装 1.590 秒，升级当前包 5.772 秒，卸载 0.249 秒；均退出 0。两版各 33 文件核验通过，实际替换 pulse.exe 和 pulse_elevated.exe；隔离目录/测试注册/快捷方式清理通过，真实安装/用户偏好/服务/启动项前后快照一致。
- 包名仅标识本轮，不代表整个原始方案已全部完成；剩余安全、流式性能、外部设备及机器级验收边界以上述清单为准。
