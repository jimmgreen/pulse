# Pulse · P2 启动控制与精确撤销实机回归

日期：2026-10-06。项目：`<repo>`。

## 本批结果

继续完成 **M04-011、M04-003** 两项代码修复及 Windows 当前权限定向回归。结合上一批，已有 7 项 P2 文件操作问题完成对应的代码与隔离验证；不是全部审查问题验收完成。

| 最终测试入口 | PASS | FAIL | 退出码 |
|---|---:|---:|---:|
| `pulse_elevated_session_test.exe --review-transfer` | 316 | 0 | 0 |
| `pulse_elevated_session_test.exe` | 19 | 0 | 0 |
| `pulse_elevated_transfer_test.exe` | 12 | 0 | 0 |
| `pulse_elevated_transfer_protocol_test.exe` | 17 | 0 | 0 |
| **合计** | **364** | **0** | |

316 条主回归包含上一批 90 条，不与上一批结果重复相加。第一阶段启动控制回归 166/0；加入撤销场景后 301/0；再增加非法名称和多叶文件恢复测试后 316/0，各阶段退出码均为 0。主程序、生产授权 helper 和测试目标均构建成功；`git diff --check` 退出码 0。

## M04-011：接受请求后不再丢失取消／暂停

- 控制状态随 `QueueItem` 保存，任务还在队列时也可接收控制命令。
- 请求控制与引擎启动初始化共用锁，取消／暂停不会被随后的 `store(false)` 清掉。
- 在权限探针、扫描、快速移动等文件操作边界之前处理暂停；Resume 后才继续。
- 取消不能被后续 Pause/Resume 复活；下一请求使用自己的控制状态。

**真实 worker 屏障测试**：Copy 和 Move 各覆盖 8 种时序，包括启动 worker 前、进入引擎但尚未初始化、初始化后尚未进行文件操作，取消、暂停/恢复、暂停中取消，以及提前恢复。验证暂停期间源与目标不变；取消无完成映射/Undo；恢复后产生精确映射；每任务终态计数为 1；随后请求不继承旧控制状态。

屏障在测试客户端编译条件下启用，不进入生产构建。这是对生产 OpsManager 队列与 worker 的确定性测试，**不是在真实管理员 helper 的管道上注入启动屏障**。已有会话测试另行验证普通控制消息兼容性。

## M04-003：KeepBoth 撤销恢复准确的原名

- 复用已有 `OpRequest.new_name` 字段表示单源 Move 的明确目标叶名；Undo 设置原始文件名，而不再沿用 KeepBoth 生成名。
- 快速移动、扫描后的目标构造、权限探针及完成映射使用一致的明确目标名。
- 继续通过原冲突处理流程执行，不增加无确认的覆盖或补救式裸 rename。
- 会话 Undo JSON 保存的原路径／实际目标映射保持不变；已有 journal 的 `name` 字段保留逆操作目标名。

**实际文件验证**：普通文件、目录与同名目标文件冲突、目录合并内叶文件、部分完成；分别从当前 Undo 栈和经过 session JSON／journal 写入、读取、RetryRecovery 的状态执行。再测试两条叶文件逆请求分别恢复原名，保留原目标冲突数据。

**撤销时原位置已被重新创建**：分别选择 Cancel、Skip、KeepBoth、Replace。前三者不得无确认覆盖重新创建的内容；Replace 只在明确选择后执行；完成事件使用实际发生的路径。授权链测试使用真实 Undo 生成的请求，经客户端和非提权测试 helper 恢复原名。

**名称边界**：本地引擎、客户端、host 验证单源 Move 的目标叶名；实际拒绝路径穿越、绝对路径、设备名、流名和路径分隔符，不修改文件、不更换现有 helper。协议测试还检查多源／Copy 不接受明确 Move 叶名。

## 协议与部署注意

授权传输协议从 **v2 升至 v3**：Transfer 尾部新增目标叶名字串，普通传输发送空串。客户端和 host 双端校验；旧版本在帧校验时拒绝，不把不同布局误读成合法请求。

已一并重建当前工作树中的主程序、生产 helper、测试 helper。未来发布时必须一起部署匹配版本；本轮没有替换已安装程序，也没有关闭原有 Pulse 实例。未进行真实管理员／跨账户授权或混合版本程序实际部署测试。

## 证据

项目 `bench_data/`：
- `p2-control-build-1.log`、`p2-control-test-1.log`、`p2-control-1.exit`
- `p2-undo-build-1.log`、`p2-undo-test-1.log`、`p2-undo-1.exit`
- `p2-undo-build-2.log`、`p2-undo-test-2.log`、`p2-undo-2.exit`
- `p2-second-session.log/.exit`、`p2-second-shell.log/.exit`、`p2-second-protocol.log/.exit`

对应日志已同步保存至本地工作区 `pulse-review/files/bench_data/`。本批基线为上一批五项修复后的源码，保存在 `pulse-review/p2-control-baseline/`。

本批检查点 SHA-256（之后的构建可能改变）：
- `build_st/pulse.exe`：`9549E189314B913800C87A4FCCFA36D0CA7408B7A371FF87C66D33FD5FA4D5D4`
- `build_st/pulse_elevated.exe`：`4E13FD592D1E49648DBEFDE264E31B42C05666219807F6D760B3D09B1791BBC0`

## 范围限制与后续

- 本批没有证明管理员、跨账户、任意阻塞系统调用取消、标签／托盘／剪贴板 UI 全链路或全部 Undo 历史策略安全。
- 不把“不覆盖重建文件”的冲突验证等同于“用户取消 Undo 后仍保留可再次撤销的历史”；后者不是本批修改。
- 测试数据、ACL、恢复日志均限于独有 fixture；主回归中的 ACL 恢复、helper 退出及 fixture 清理断言均通过。
- 接下来继续 Shell 客户端生命周期：M04-005 首次连通前退出后无法重启、M04-004 旧连接失败误伤新连接。当前分析见 `pulse-review/next-shell-lifecycle-plan.md`，不冒称已修复。
- 用户要求已持久化：继续推进；任何需要用户指令的问题，必须使用交互式选项，不以普通文字提问替代。
