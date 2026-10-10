# Pulse · P2 菜单请求归属与 M04 阶段收尾

日期：2026-10-06。项目：`<repo>`。

## 已验证结果

**M04-001 已完成定向修复和 Windows 实机对照回归。至此，审查报告中 M04 的 13 个 P2 均已完成本轮定向修复与实机验证。** M04-009 为 P3，尚未处理；其他模块仍有待修项，不能据此宣布整份 121 项审查全部完成。

- M04-001 原逻辑：**3 PASS / 12 FAIL，exit 1**。
- 修复后，同一组：**15 PASS / 0 FAIL，exit 0**。
- 扩展后最终请求归属测试：**28 PASS / 0 FAIL，exit 0**。
- 主程序及实际关联目标构建：**exit 0**。
- 最终十二组回归：**506 PASS / 0 FAIL，十二组均 exit 0**。
- `git diff --check`：**exit 0**。

506 是此次检查点的断言总数，包含此前的 478 项，不与历史总数重复累加，也不是问题数量。

## M04-001 的生产修改

1. **先登记归属，再允许发送或回调。** `ShellClient` 提供同步 `Accepted` 回调，在分配非零 ID 后、任何发送或同步失败完成之前调用。返回零表示未接受，不调用登记回调；分配器跳过保留的零值。
2. **菜单 Query/Invoke 在发送前建立路由。** 同步发送失败和极快响应不再抢在登记之前。Query 的失败 DONE 单独结束对应 UI token、清除映射；未启动客户端返回零时也在本地结束 token，不让界面一直等待。
3. **文件终态按 request-id 保存。** 使用已登记请求集合与终态 map，仅接收有归属的结果。未知／重复完成不能覆盖第一份终态；等待者在锁内移动错误文本、提取结果并移除归属，不再锁外读取／清空共享单槽字符串。
4. **进度按文件请求归属过滤。** 其他请求的进度不能覆盖当前文件操作。超时计算先读取活动时刻再采集当前时间，避免并发采样先后引起无符号相减下溢。
5. **Stop 的 join 后清理。** 清除菜单路由、排队菜单与文件结果归属，晚到的无归属完成不能重新占据状态。

生产改动：`src/ipc/shell_client.cpp/.h`、`src/ops/ops_manager.cpp/.h`。测试改动：`src/bench/review_transfer_test.cpp`、`elevated_session_test.cpp`。

## 对照测试如何执行

新增 `--review-routing`，执行真实的菜单派发、`ShellClient::Submit`、OpsManager 回调、`RunShellOp` 和 `WaitShellDone` 代码。仅在测试编译中替换传输响应；部分 Query 回调由另一条 Windows 线程在 Submit 返回前完成，从而固定“响应早于调用返回”的时序。

覆盖：

- 快速 Query 回调准确投给原 token；partial/final 均不丢失；Close 后晚到回调不再投递。
- Invoke 同步失败只完成一次，不留下阻止更新准备的 in-flight ID。
- Query 失败 DONE、零 ID Query/Invoke 均有明确收尾。
- 先收到文件 DONE，再收到菜单失败 DONE，文件结果仍能被真实 `RunShellOp` 消费，不依赖十分钟超时。
- 多个已登记 ID 的终态各自保存，未知和重复完成不能替换其错误文本／结果。
- Accepted 调用顺序、零 ID 跳过、客户端未启动时不接受／不发送。
- 文件进度归属、join 后状态清理及晚到完成。

**这些新测试不启动 ShellClient reader、命名管道或 helper，不执行真实文件创建、菜单动词或第三方 COM 扩展。** 模拟创建请求得到成功响应，验证的是结果归属，不代表该测试真的创建了文件。Stop 扩展用例验证的是无 OS worker 的 join 后记账清理，不外推为任意阻塞 IO 的退出保证。

修复前的等待失败先被记录，再用停止标志和条件变量收尾；没有等待十分钟，也没有删除失败断言。

## 扩展测试中发现并纠正的前置条件

第一次扩展运行是 **27 PASS / 1 FAIL**，没有计作通过。进度 fixture 只登记请求，没有像生产 `RunShellOp` 一样设置 Running；`OpStatus` 默认 Completed，`PresentOperationStatus` 按既有契约将完成进度呈现为 100%。

补齐 active/Running/初始 0% 的测试前置状态后，保留了“所属请求必须显示 50%”的断言，并加强了“无关进度仍为 0%”及运行态检查。**没有为迎合测试修改展示逻辑，也没有放松 50% 的预期。** 失败日志保留为 `p2-routing-expanded-initial.log/.exit`。

随后重新构建并重跑全部十二组，最终以 `p2-routing-confirm-*` 为成功证据。

## 最终回归清单

| 测试 | PASS | FAIL | 退出码 |
|---|---:|---:|---:|
| 请求归属与终态保存 | 28 | 0 | 0 |
| worker 获取失败与 Join | 19 | 0 | 0 |
| 查询中 Close 与相邻路径 | 34 | 0 | 0 |
| 命令模板与真实 Windows argv | 19 | 0 | 0 |
| 文件操作主回归 | 316 | 0 | 0 |
| 授权会话兼容性 | 19 | 0 | 0 |
| 首次启动失败恢复 | 6 | 0 | 0 |
| 旧读取／新连接交错 | 9 | 0 | 0 |
| 半帧／新连接交错 | 10 | 0 | 0 |
| 当前 token Shell 传输 | 12 | 0 | 0 |
| 授权协议 v3 | 17 | 0 | 0 |
| 命令启动与 Terminal 兼容性 | 17 | 0 | 0 |
| **合计** | **506** | **0** | |

## M04 P2 收尾索引

| 编号 | 本轮处理主题 |
|---|---|
| 001 | 菜单发送前归属登记、文件终态按 ID 保存 |
| 002 | 不完整扫描不得静默报告成功 |
| 003 | KeepBoth 移动撤销恢复原父目录及原名 |
| 004 | 旧连接读取／断连不能破坏新代际连接 |
| 005 | 首次连接前 helper 退出后的恢复 |
| 006 | 查询阶段 Close 跨阶段保留并及时清理 |
| 007 | 事件失败不启动线程，资源所有权与 Join |
| 008 | 扫描及多 root 快移中的取消检查 |
| 010 | 命令模板不再重扫插入的文件名 |
| 011 | 请求自有取消／暂停状态，保护启动窗口 |
| 012 | 冲突取消协议与状态一致性 |
| 013 | 部分完成路径通知与撤销安全分离 |
| 014 | 权限探测及当前权限分流的隔离矩阵 |

各项的精确复现、对照证据及限制分别保存在此前的文件操作、启动控制与撤销、Shell 生命周期、命令模板、菜单资源生命周期报告中。此表不是对整个操作层无缺陷的保证。

## 证据与交付

- Windows 报告：`docs/review-p2-routing-2026-10-06.md`。
- 本批补丁：`pulse-review/p2-routing.patch`，已包含最终测试前置条件修正，并在修复前基线通过 `git apply --check`；未重复应用到已经修复的 Windows 工作树。
- 基线：`bench_data/p2-routing-baseline-build.log`、`p2-routing-baseline-test.log`、`p2-routing-baseline.exit`。
- 首轮修复：`p2-routing-fixed-*`。
- 扩展首轮失败证据：`p2-routing-expanded-initial.log/.exit`。
- 最终成功证据：`bench_data/p2-routing-confirm-{build,routing,worker,close,template,transfer,session,startup,generation,partial,shell,protocol,command,diff}.log/.exit`。

全部最终日志及退出码已下载、逐项统计。启动器完成不等于测试完成；最终测试是在确认构建 exit 0 后单独启动的。

检查点 SHA-256：
- `build_st/pulse.exe`：`B6F8F3B764F97D9BA6A39B5BAF3FD0DC462C660EC7D85581C983D4A91E0E03A4`
- `build_st/pulse_elevated_session_test.exe`：`74CBA444DBF0990519917F412CDB00AF0C945BF68CD78BF08F7D101409335C7B`

## 后续及边界

继续沿已选的“P2 文件操作安全”方向处理 **M03-003：回收站损坏／截断元数据导致错误还原目标**。这一项目前仅完成源码核对和隔离测试设计，尚未修改或关闭。

保留既有边界：未执行真实管理员／双账户验收；未声称任意阻塞写可有界取消，或未知副作用请求可安全自动重放；未证明全部 GUI／第三方 COM 交错。未改真实服务配置，未关闭用户原有 Pulse 实例，未创建提交或推送仓库。
