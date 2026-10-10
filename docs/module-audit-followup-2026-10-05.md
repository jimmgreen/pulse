# 逐模块审查修复与实测记录（2026-10-05）

依据：`%USERPROFILE%\Downloads\Pulse逐模块代码审查-2026-10-05.md`。
实施目录：`build/quicklook_wt`；构建目录：`build/quicklook_build`。
保留此前已经恢复的主界面复用版打开对话框，以及共用输入控件修复。

## 范围与结论

原报告只完成 M01–M03，包含 13 个确认问题、1 个容量候选；M02-003 已撤销。
本轮针对这部分修复，不将尚未审查的 M04 以后模块标记为通过。
M02-004 在当前代码中已修复，本轮实际运行验证，没有重复修改。

| 项目 | 处理 | 定向证据 |
| --- | --- | --- |
| M01-001 | JSON 字段查找仅匹配当前对象成员，跳过字符串值和嵌套值 | 标签关联、嵌套键、保存搜索及偏好回归 |
| M01-002 | 补齐控制字符转义、Unicode 转义解码；保存前验证；异步失败记日志 | 实际 NTFS ADS 导入、配置写入及新 Catalog 载入 |
| C01-001 | 实测确认候选；读写统一为 64 MiB UTF-8 字节上限，超额写入保留旧文件 | 修复前生产保存器写出的 18,649,017 字节配置无法重载；修复后 2 万条 metadata 重载及容量边界通过 |
| M02-001 | 正常后台线程执行保留策略，跨进程租约保护完整报告；失败删除不虚减占用并重试 | 实际可恢复 SEH、两个报告进程、锁定超额文件与解锁重试 |
| M02-002 | 区分不存在与属性/枚举/删除失败，返回 Win32 错误 | 故障注入及真实共享锁文件删除失败 |
| M02-004 | 保留已有繁体语言测试修复 | 运行 localization 测试 |
| M03-001 | 导航枚举接入取消与 15 秒截止；退役 I/O 保留自身状态，远程最多 16／总计 32 项 | 真实 WorkerPool 生命周期，注入打开、NT pending、Win32 回退阻塞；Stop 31/32/32 ms；新导航及迟到回收 |
| M03-002 | 通知错误退避，不支持时每 5 秒重试，按路径合并 overflow | 永久错误、退避期间停止、恢复后的实际文件通知 |
| M03-003 | 展示与还原共享严格回收站元数据读取，拒绝截断/短读 | 隔离 I/R 文件实际还原、损坏不操作、已有目标不覆盖 |
| M03-004 | 普通 DOS/UNC 先解析绝对路径和点段，保留显式 extended literal | 真实文件路径、盘符相对路径、UNC 点段及尾部字面字符 |
| M03-005 | 唯一临时文件、按路径提交锁和请求代际，过期写入丢弃 | 两代逆序提交、8 个并发大快照、真实替换失败保留旧缓存 |
| M03-006 | 探测按路径验证代际；容量耗尽时保留队列并定时重试 | A 超时→B 完成→A 晚到；容量释放后重新调度 |
| M03-007 | 超时消息使用监控线程计时，成功同步后才读取工作线程结果 | 真实线程、事件与消息窗口；超时、晚到、关闭后清理 |
| M03-008 | 枚举与 benchmark 共享正确 NT ABI，比较完整名称和元数据 | Unicode 实际文件、非零 EA/tag 注入、同数量错名及中途错误拒绝 |

## 已执行的定向测试

| 测试 | 结果 |
| --- | --- |
| pulse_common_persistence_test | 37 PASS，退出 0 |
| pulse_saved_search_test | 15 PASS，退出 0 |
| pulse_audit_persistence_test | 16 PASS，退出 0 |
| pulse_diagnostics_retention_test | 16 PASS，退出 0 |
| pulse_localization_test | 77 PASS，退出 0 |
| pulse_fs_audit_test | 55 PASS，退出 0 |
| pulse_enum_benchmark_test | 5 PASS，退出 0 |
| pulse_net_cache_audit_test | 30 PASS，退出 0 |
| pulse_app_controllers_test --context-menu-prefs | 7 PASS，退出 0 |
| pulse_file_picker_model_test | 60 PASS，退出 0 |
| pulse.exe --picker-dialog-test --input-surfaces | 9 PASS，退出 0 |
| pulse.exe --picker-operations-test | 19 PASS，退出 0 |

合计 346 个通过断言，0 个失败。未运行完整历史套件。
本轮构建了主程序、Shell/Preview/Index/Document/Elevated/Integration 辅助程序，
以及所需定向测试目标；构建退出 0，无编译警告。`git diff --check` 通过。
主程序内置 picker 实测在进程自有隔离桌面执行，不操作用户当前桌面。
已查看 `bench_data/quicklook-b/module-picker.png`：仍为主渲染器复用版，
没有标签栏和重复路径标题；列标题、侧栏、底部输入区没有可见裁切或重叠。

日志：工作树 `bench_data/m01_probe/*-after.log`；仓库根目录
`bench_data/m03-audit/fs-audit.log`、`enum-benchmark.log`、
`bench_data/quicklook-b/module-net-cache.log`。
M02 两项测试的原始 stdout 保存在会话工具记录中，执行时未保存为磁盘日志；
不将事后转录称为原始日志。
主程序构建、偏好、picker 模型、输入及操作日志均位于仓库根目录
`bench_data/quicklook-b/module-*.log`。

## 验证边界

- 故障网络使用底层 API 注入；没有连接真实故障 SMB 服务器。线程、Windows 事件、消息窗口和磁盘操作实际运行。
- WorkerPool 测试保留生产枚举和池生命周期，替换外围 Git、链接、identity、排序、缓存及日志依赖。上述 Stop 时间仅证明目录枚举路径，不代表所有后台 I/O 都有相同退出上限。
- 2 万条为配置 metadata，不是创建 2 万个用户文件；重载使用新 Catalog，未把它称为整应用重启实测。
- 诊断保留为软上限：最新单组超过容量、活跃报告或无法删除的文件可能暂时超过上限；不会为了满足计数删除正在写的组。
- 未在真实繁体系统、真实拒绝 ACL／reparse 诊断目录上复测；对应分支采用定向注入，实际锁定文件另行验证。
- 不将编译成功视作功能通过；不发布正式版本，也未恢复之前暂停的发布流水线。

## 测试包

`<repo>\dist\PulseSetup-1.0.53-module-audit-test.exe`

大小 9,187,992 字节；SHA-256：
`D504CC50325034A03BEC0A2E0693440FAF45D574296423DD6F5A10ABA7351320`。

Inno Setup 编译成功。本轮未实际安装覆盖用户环境。
打包器仍提示项目既有的 admin/HKCU 用途警告；它不属于本轮编译器警告。
