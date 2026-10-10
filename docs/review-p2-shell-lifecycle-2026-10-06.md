# Pulse · P2 Shell 生命周期实机回归

日期：2026-10-06。项目：`<repo>`。

## 结果

本批继续完成 **M04-005（首次连通前退出无法重启）**、**M04-004（旧连接失败误伤新连接）** 的修复及 Windows 隔离前后对照测试。

最终主程序及相关目标构建成功，构建退出码 **0**；以下七组测试均退出 **0**，合计 **389 PASS、0 FAIL**。其中包含上一检查点的 364 条，不重复累加为另一批全新检查。

| 最终测试 | PASS | FAIL |
|---|---:|---:|
| Shell 首次启动失败恢复 | 6 | 0 |
| 旧 OVERLAPPED 读取／新连接交错 | 9 | 0 |
| 旧连接半帧／新连接交错 | 10 | 0 |
| 文件操作主回归 `--review-transfer` | 316 | 0 |
| 已有授权会话兼容性 | 19 | 0 |
| 已有当前 token Shell 传输 | 12 | 0 |
| 授权协议 v3 | 17 | 0 |
| **合计** | **389** | **0** |

`git diff --check` 退出码 0。本批补丁 `pulse-review/p2-shell-lifecycle.patch` 已在本地基线通过 `git apply --check`，没有再次应用到已修复的 Windows 工作树。

## M04-005：修复前失败，修复后通过

在测试独有目录复制测试客户端和 stub helper。stub 第一次以退出码 23 结束，故意不创建管道；第二次才接受连接。**stub 不执行文件请求或 Shell 动词。**

- 初始基线：3 PASS / 2 FAIL，exit 1。
- 增强基线：3 PASS / 3 FAIL，exit 1。增加“只 Start，不提交任何请求，由 reader 自己恢复”的检查，仍不能启动第二个 helper。
- 修复后：6 PASS / 0 FAIL，exit 0；随后重复三次均为 6/0、exit 0。

修改 `EnsureConnected`：在尚未发布连接时，退休已确认退出的 child，关闭进程／线程句柄并复位 `child_started_`。保留 `last_spawn_try_`，不取消既有 1500ms 重启退避。验证不需要 Stop/Start，也不依赖反复提交操作来驱动恢复。

## M04-004：确定性交错与修复

### 修复前实机复现

让旧 Reader 停在异步 ReadFile 已返回 IO_PENDING、尚未收取完成结果的位置。Abort 旧请求，启动第二个 helper 并提交新请求，再放行旧 Reader。

基线 **4 PASS / 2 FAIL，exit 1**：新 helper 确实已接受请求，但旧断连处理使新请求丢失，并触发错误重启。失败用例保留，没有删掉或放松断言。

### 生产修改

- 连接对象通过共享所有权持有 pipe；旧读者完成 OVERLAPPED 收尾前不释放句柄，不只增加一个代际数字。
- 只有当前连接退休时才能终止它所属的 child；旧 Reader 不能清理新连接。
- Pending 保存连接 generation；旧响应与旧断连只处理对应代际。
- 正在重试的条目保留在 Pending 中，Abort 可以移除；真正重发前再确认请求仍存在。
- 过期／未知 Abort ID 不终止新 helper；同一个请求不因交错重复完成。
- Stop 清理旧 Pending；错误消息在锁内复制，回调在锁外调用。

### 修复后实机覆盖

首次修复回归 6/0、exit 0；扩展后验证：
1. 新请求在旧 Reader 放行前已交给新 helper；放行后仍成功且只有一次成功完成。
2. 旧 Abort 只完成一次，新 helper 没被旧失败终止，也没有第三次重启。
3. 半个响应头已经由旧 helper 写出时退休连接，不从新连接补读旧帧。
4. 再次 Abort 旧 ID 或不存在的 ID 不影响新 helper。
5. 新连接有明确的待完成 idle read 时 Stop，在测试的 3 秒界限内取消并收尾。
6. 测试 child/helper 退出，只有测试独有目录被清理，没有实际执行收到的文件请求。

## 测试隔离

新增 `src/bench/shell_reconnect_test.cpp`、`shell_reconnect_stub.cpp`，CMake 新增独立测试 target。测试驱动只在独有 `bench_data/shell-reconnect-PID-tick` 目录复制自身和 stub，并将副本命名为 `pulse_shell.exe` 以满足现有邻接启动契约。

**没有覆盖原有 `build_st/pulse_shell.exe` 或安装目录的 helper，没有关闭用户原有 Pulse 实例，没有发起管理员授权。** 测试宏只用于测试客户端屏障，不改变生产服务端身份校验。

## 证据

项目 `bench_data/`：

- 前后对照：`p2-shell-startup-baseline-test.log`、`p2-shell-startup-baseline2-test.log`、`p2-shell-startup-fixed-test.log` 及对应 `.exit`
- 重复测试：`p2-shell-repeat-1/2/3.log` 及对应 `.exit`
- 代际基线：`p2-generation-baseline-test.log`、`p2-generation-baseline.exit`
- 中间修复：`p2-generation-fixed-test.log`、`p2-generation-expanded.log`、`p2-generation-partial.log`、`p2-generation-startup.log`
- 最终构建：`p2-lifecycle-final-build.log`、`p2-lifecycle-final-build.exit`
- 最终七组测试：`p2-lifecycle-final-{startup,generation,partial,transfer,session,shell,protocol}.log` 及对应 `.exit`

最终构建后的第一个命令包装器因 `set` 尾随空格使成功判断提前退出，没有启动测试。确认只有构建成功记录、没有测试结果后，已单独执行七组测试并逐一核实退出码；没有把包装器完成当作测试完成。

本批检查点 SHA-256（后续构建可能改变）：
- `build_st/pulse.exe`：`5CD00FC7206D031631219267F0D55F4097950AFE76F32447FDBC6DECC1A535F6`
- `build_st/pulse_shell_reconnect_test.exe`：`4324A6A0EF5CC4681C45196971C8753A8C9D0AE33D632CA4560EC14E56414CDF`

## 不外推的结论

- 未证明所有阻塞写都可有界取消。现有 `PipeWrite` 仍会在 `send_mutex_` 内等待；这里验证的是受控 named-pipe 读侧收尾。
- 未改变既有“重试一次”的总体策略，不证明未知副作用的破坏性 Shell 请求可安全重放；测试 stub 从不执行这些请求。
- 未测试真实管理员、跨账户或第三方 Shell 扩展。未刻意强制 OS 重用相同 HANDLE 数值；已通过保留所有权避免旧读期间过早关闭句柄。
- 389 是检查数，不是审查问题数。目前 M04 已完成定向修复的 P2 为 002、003、004、005、008、011、012、013、014；001、006、007、010 等仍需继续。

用户交互要求持续保存在 `/home/user/localbridge-notes.md` 和 `pulse-review/current-task.md`：继续任务；任何询问必须使用交互式选项。
