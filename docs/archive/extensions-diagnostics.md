# 增强包宿主持久诊断

此出口供本机宿主使用者排查跨会话问题，不是增强包 API，也不授予增强包读取其他包历史的权限。`audit.list` 仍只读取有效 caller、owner 的当前会话内存记录；MCP 不可通过传入 packageId 借用此出口。

## 记录与隐私

每条记录只有五个字符串字段：公开 `packageId`、白名单 `operation`、白名单 `outcome`、UTC 毫秒 `utcMs`、随机宿主会话 `sessionId`。不保存路径、选择项、请求或响应 payload、设置值、密钥、错误详情。未知操作或结果被丢弃并计数，而非原样写入。当前操作白名单包含 19 项声明式命令及 10 项资源操作，另有内部事件 `package.trust` 和仅兼容旧日志的 `runtime.invoke`。后者没有可调用执行入口，不表示 JavaScript 运行层仍存在。

日志是诊断信息，不是防篡改证据、签名证明或权限授权。具有相同本机文件访问权的使用者仍能修改日志；读取时严格校验格式、字段及白名单，损坏内容不原样回显。

另有宿主内部事件 `package.trust` / `revoked`：UI 消费失信结果、撤销授权时直接记录公开 packageId，不使用已失效 caller，不记录 policy、manifest 或发布者密钥。它不属于可调用命令能力或资源操作，不能据此扩大权限。

## 异步写入、预算与失败

UI 线程仅将已校验记录加入内存队列，不执行日志文件 I/O。队列最多 256 条，工作线程一次最多取走 256 条；两批合计有界。`Enqueue=true` 只表示接纳，不能称为已持久化。

专用目录内只有 `audit.lock`、`audit-0.jsonl` 至 `audit-3.jsonl`、`audit-status.json` 和提交中的 `audit.pending`。四段各最多 65,536 字节，满后循环替换旧段；状态最多 4,096 字节，锁文件零字节。包含一个原子提交临时文件的峰值预算为 331,776 字节。不是永久保存或按天保留；流量决定可保留时间。记录可按 utcMs/sessionId 分析，文件槽位顺序不代表严格时间顺序。

写入使用立即失败的跨进程文件锁，无无限重试。`store_busy`、磁盘错误、损坏或异常会计入失败和丢失数量；后续批次可以再次尝试，但不会重放已丢弃批次。原子提交先刷新临时文件，再以文件句柄重命名。崩溃留下的 `audit.pending` 报 `interrupted_commit`，不自动删除或重放。未知文件、损坏记录、超预算文件均拒绝继续写入；请保留现场后由本机使用者处理，工具不自动清空证据。

宿主可调用 `PersistentExtensionAuditStatus()` 获取 accepted、persisted、dropped、failures、queued、last_error。启动缺根目录和线程启动失败也有明确状态。CLI 的 `lastWriterStatus` 是上次成功保存的状态快照，不是另一进程的实时健康状态；写盘本身失败时快照可能陈旧，实时状态及固定错误码调试输出才可显示该次失败。

正常停止会关闭入队并由工作线程排空队列，再保存状态。外部锁不会让 join 无限等待，但本地系统 I/O 没有硬性墙钟完成保证。现有退出 watchdog 可在两秒后强制终止进程；强退、断电或崩溃可能丢失尚未提交事件，不能承诺完整落盘。

`PrepareShutdown()` 是幂等的宿主收尾入口：停止信任观察器、撤销后续包调用，排空已接纳审计记录并 join 工作线程。主程序应在最后窗口关闭事件记录完成后、`PostQuitMessage` 及 CRT/静态析构前调用；新启动也先调用它收尾旧实例。不能依赖全局 `unique_ptr` 的静态析构才执行最后一次 JSON 读取/写入。此生命周期修正不构成既有退出访问冲突根因的证明；实际崩溃归因另依符号化证据。最后窗口的既有退出 watchdog 仍决定强退上限。

## 路径与本地读取

成功加载增强包后启用。普通运行目录是 `%LOCALAPPDATA%\Pulse\extension-audit`；`--test-instance` 使用 `%PULSE_TEST_DATA_DIR%\extension-audit`，缺少隔离根时不回退到用户目录。单纯 manifest 检查不启用记录。

ROOT 必须是固定本地磁盘上的绝对路径。逐级固定祖先目录句柄并拒绝重解析点；拒绝 UNC、设备路径、ADS、`.`/`..`、文件硬链接和意外目录项。读取不创建存储。导出目标必须在 ROOT 外、父目录已存在且安全，文件必须尚不存在，不能覆盖源或既有文件。

先读取 `pulse.exe --extension-api` 或 MCP/助手的共同发现，确认 developerFeatures 包含 `persistentHostDiagnostics`；developmentNotes 提供以下两个宿主命令及边界。它们不是新的 MCP 执行工具，也不增加包资源权限。

在支持该标志的宿主中运行：

```powershell
.\pulse.exe --extension-audit-read 'C:\isolated-profile\extension-audit'
.\pulse.exe --extension-audit-export 'C:\isolated-profile\extension-audit' 'C:\isolated-profile\audit-export.json'
```

只接受上述参数组合，不接受包身份、任意查询过滤或额外启动选项。标准输出为 JSON：`ok`、`partial`、固定码 `error`、已验证 `records`、`lastWriterStatus`、`retention`、`grantsAuthority:false`。部分文件损坏时可能返回此前已校验记录并标记 partial；导出要求完整读取成功。退出码 0 表示成功，2 表示读取/导出或参数失败，3 表示输出失败。

## 验证与接线

独立测试源码为 `src/bench/extension_audit_persistence_test.cpp`，仅创建唯一临时目录，覆盖后台写入、跨会话、预算轮转、锁冲突、损坏与秘密不回显、导出覆盖拒绝、硬链接和真实目录重解析点。优先创建真实目录符号链接；失败时使用 FSCTL_SET_REPARSE_POINT 创建真实 junction，并记录实际夹具类型及符号链接失败码。两种都无法创建时记为未验证并返回失败，不冒充通过。2026-11-04实际原生38项通过（build-extensions/workspace-audit_persistence-native-01.log）；目录符号链接返回1314后实际junction夹具通过，不计为跳过通过。真实宿主workspace-audit-host-20261104-03完成21项检查、读取19条记录，覆盖共同发现、五项工作区能力、导出一致性/覆盖拒绝和损坏载荷隔离。资源场景workspace-audit-resource-cli-20261104-01保留20项检查/45条记录证据。

宿主需链接 audit_persistence.cpp、audit_diagnostics_cli.cpp 及 bcrypt；在进入 GUI 前调用 `HandleAuditDiagnosticsCli`，有返回值即退出。独立测试需 audit_persistence.cpp、common/json_value.cpp 和 bcrypt。runtime 初始化与 audit_log 镜像已接线；CLI 与构建目标已接入并实际执行上述验证。

定向复验补充：workspace-audit_persistence-native-03 有一次“rotation preserves admitted batches”失败，当时缺少状态诊断，根因未确定。保留失败日志及原严格断言；加入批次/accepted/persisted/dropped/failures/queued/last_error诊断后，第05–29轮均38项通过，不能把重跑通过描述为该失败根因已修复。

宿主最后窗口退出以及主入口提前返回均调用幂等 PrepareShutdown：先停止观察器、撤销来源授权，再排空已接纳审计并等待后台线程退出，确保正常收尾在CRT静态析构前完成。现有宿主退出看门狗仍可在故障I/O持续阻塞时强退，未提交记录仍可能丢失。独立真实落盘/重复收尾/关闭后不能复权三项已通过，含于workspace-shutdown-native-01.log的22项信任与退出检查；这项收尾不被用来解释已定位到Shell命令预热线程的退出崩溃。
