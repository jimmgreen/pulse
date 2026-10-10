# Pulse · P2 菜单资源生命周期实机回归

日期：2026-10-06。项目：`<repo>`。

## 本批结果

已修复并在 Windows 实机前后对照验证 **M04-007（资源创建失败后的 worker 生命周期）**、**M04-006（查询期间关闭菜单后仍进入会话等待）**。

主程序、Shell host 及定向测试目标构建成功，build exit **0**。最终十一组回归 **478 PASS / 0 FAIL**，所有测试退出码 **0**；`git diff --check` 退出码 **0**。

478 包含上一检查点的 425 项，不与历史总数重复累加。当前 M04 已完成定向修复并实机回归的 P2 共 **12 项**：002、003、004、005、006、007、008、010、011、012、013、014。**001 尚未关闭**；009 为 P3。未宣称整份审查报告已全部完成。

## M04-007：先取得事件，再启动线程

### 修复前对照

将原始获取顺序原样提取为生产 `StartHandlerWorker`，通过函数指针注入第一个事件失败、第二个事件失败、线程创建失败，另外保留成功路径。提取时尚未修复原逻辑。

实机基线：**14 PASS / 5 FAIL，exit 1**。两个事件失败场景都错误地尝试启动线程；三种失败场景都有已取得的句柄未释放。

测试使用真实 Windows 事件和可控线程，但保持参数对象存活直至线程退出，**不故意让线程读取释放后的内存**。因此证据是生产错误获取顺序与句柄泄漏的确定性复现，不是 ASan 或崩溃复现。

### 修改与修复后结果

- `HandlerWorker` 增加 RAII 句柄清理并禁止复制。
- 第一个事件成功后才创建第二个，两个事件都成功后才允许启动线程。
- 线程创建失败时，由唯一所有者释放已取得的事件。
- 启动 worker 前为 vector 预留容量，避免线程启动后的所有权转移再次分配。
- 保持原有 Join 路径；已显式关闭的句柄置空，避免重复关闭。既有挂起 handler 的 `release()` 保活策略未改成危险的强制释放。

同一组修复后 **19 PASS / 0 FAIL，exit 0**，包括成功 Join 和测试自有句柄数回归基线。

## M04-006：关闭状态跨查询阶段保留

测试调用真实 `CtxSessionThreadImpl`、`PostCtxMessage`、Windows 消息队列与 Join 代码。替换的仅是 provider 枚举、可控 worker 和日志出口；不启动 host 主入口，不加载第三方扩展，不写用户的 host 诊断日志。

固定时序：可控 worker 停留在查询阶段 → 发送 Close → 确认查询 pump 已消费并保存关闭状态 → 允许 worker 返回 → 检查会话是否在测试的 1.2 秒窗口内退出。

- 修复前重复三次：**18 PASS / 3 FAIL，exit 1**。三个退出时限检查均失败。
- 先记录失败，再发送第二条 Close 仅用于基线收尾；没有把补发后的退出当成通过，也没有故意等待 120 秒或强杀线程。
- 修复在进入后续等待前检查持久的 `SessionCloseRequested(sid)`，不再依赖已经被取走的那条消息。保留既有 pending invoke 的处理顺序。
- 同一组修复后 **21 PASS / 0 FAIL**。
- 扩展正常查询结束后 Close、线程初始化前 Close、空 handler／失败 fallback 三类相邻路径后，最终 **34 PASS / 0 FAIL，exit 0**。

各场景检查 session 退休、可控 worker 未被遗弃，以及本次取得的事件／线程句柄关闭。

## 最终十一组回归

| 测试 | PASS | FAIL |
|---|---:|---:|
| worker 获取失败与成功 Join | 19 | 0 |
| 查询期间 Close 与相邻路径 | 34 | 0 |
| 命令模板与真实 Windows argv | 19 | 0 |
| 文件操作主回归 | 316 | 0 |
| 授权会话兼容性 | 19 | 0 |
| Shell 首次启动失败恢复 | 6 | 0 |
| 旧读取／新连接交错 | 9 | 0 |
| 半帧／新连接交错 | 10 | 0 |
| 当前 token Shell 传输 | 12 | 0 |
| 授权协议 v3 | 17 | 0 |
| 命令启动与 Terminal 兼容性 | 17 | 0 |
| **合计** | **478** | **0** |

构建与测试分开启动；全部日志及对应退出码已下载、逐一统计，不以 launcher 返回值代替子进程结果。

## 交付及证据

- 生产修改：`src/shell_host/main.cpp`。
- 测试：`src/bench/shell_worker_lifecycle_test.cpp`，及对应 CMake target。
- 补丁：`pulse-review/p2-menu-worker.patch`，已在本地修复前基线通过 `git apply --check`，未重复应用到已修改的 Windows 工作树。
- Windows 报告：`docs/review-p2-menu-worker-2026-10-06.md`。
- 基线：`bench_data/p2-worker-baseline-*`、`p2-close-baseline-*`。
- 修复中间证据：`p2-worker-fixed-*`、`p2-close-fixed-*`、`p2-close-worker-compat.*`。
- 最终证据：`bench_data/p2-menu-worker-final-{build,worker,close,template,transfer,session,startup,generation,partial,shell,protocol,command}.log/.exit`，以及 `p2-menu-worker-final-diff.log/.exit`。

检查点 SHA-256（后续构建可能改变）：
- `build_st/pulse_shell.exe`：`F9983F4F39A2D880C88FEF3AB24BAE9E3FA9341E20DDA8575EC03504ABF89D25`
- `build_st/pulse_shell_worker_lifecycle_test.exe`：`EB3EC50C12DCA38964462019F471E901DC7EC0ACF45840F35B07D8552B3CEA5E`

## 边界

未运行 Application Verifier／ASan，未制造真实系统资源耗尽，未验证任意 C++ 分配异常。1.2 秒是受控测试的验收窗口，不是所有第三方 COM handler 的总退出上限；既有挂起 handler 的等待／遗弃／host 回收策略仍有其边界。

未操作真实右键菜单或执行第三方动词，未改变服务配置或申请管理员权限。未证明 Close 与实际用户 invoke 的所有交错；本次保留原有 pending invoke 行为。

继续处理 M04-001 的菜单请求归属登记和文件操作终态保存问题。
