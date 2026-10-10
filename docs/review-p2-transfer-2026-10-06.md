# Pulse · P2 文件操作安全第一批实机回归

日期：2026-10-06  
项目：`<repo>`

## 结论

本批对 **5 项 P2** 完成代码修复及当前用户权限下的 Windows 定向回归：**M04-002、M04-008、M04-012、M04-013、M04-014**。

最终运行 **121 条检查通过、0 条失败**，三组测试退出码均为 **0**。这里的 121 是测试检查数，**不是审查报告中的 121 项问题全部修复**。其余问题继续按批处理。

保留了用户原有代码改动；没有替换已安装程序，没有关闭原有 Pulse 实例，没有更改真实服务配置，没有发起管理员授权。ACL 与文件修改均发生在测试独有临时目录；本轮新增测试的 ACL 恢复、helper 退出和目录清理断言均通过。

## 修复与验证

| 问题 | 修改 | 本机验证 |
|---|---|---|
| M04-012 冲突取消重复控制消息 | 发出 Cancel 后不再发送 ConflictReply 或 Resume，避免污染后续请求 | 使用真实 OpsManager 冲突回调取消；确认同一个 helper PID 保留，下一次真实传输成功 |
| M04-013 部分完成通知丢失 | 保守刷新与已确认的完成路径分别发布；不安全操作仍不进入 Undo | 部分 Copy、Move、合并目录 Move、锁定第二目标导致失败；断言恰好一条刷新与一条精确子文件映射，未完成来源不得混入；零完成取消不伪造映射 |
| M04-002 扫描与目录清理错误被吞 | 不跳过权限拒绝；每次迭代递增后立即检查错误，包括已到 end 的情况；报告非预期源目录删除失败 | Copy / 合并 Move 遇到不可枚举子目录必须失败且保留源数据；传输后新出现的源文件导致目录无法删除时报告失败；显式 Skip 不误报 |
| M04-008 扫描阶段取消失效 | 在扫描、快速移动、执行与清理边界观察取消/停止，保留已经完成的路径 | 扫描开始取消阻止单根快速移动；第一根完成后取消不再移动第二根；递归枚举在指定边界停止，无目标创建；首项完成/Undo 记录仍保留 |
| M04-014 目标权限探针过度申请 | 普通文件按 FILE_ADD_FILE、新目录按 FILE_ADD_SUBDIRECTORY；混合来源合并必要位；缺失祖先按建目录权限判断；已有真实目录合并不要求在父目录新建入口 | 四种独立创建权限组合，采用“未授予权限”而非宽泛 deny；普通文件 Copy / Replace、空目录 Copy 实际成功；缺失祖先及已有目录合并的路由检查；Move 的 DELETE / 父目录 FILE_DELETE_CHILD 四组合与三种可行组合的实际移动 |

## 最终实机结果

| 测试入口 | PASS | FAIL | 退出码 |
|---|---:|---:|---:|
| `pulse_elevated_session_test.exe --review-transfer` | 90 | 0 | 0 |
| `pulse_elevated_session_test.exe`（已有会话兼容性回归） | 19 | 0 | 0 |
| `pulse_elevated_transfer_test.exe`（已有当前 token Shell 传输回归） | 12 | 0 | 0 |
| **合计** | **121** | **0** | |

只构建了 `pulse`、`pulse_elevated_session_test`、`pulse_elevated_transfer_test` 及其依赖，未默认运行全量 selftest / 全量 release。主程序及生产授权 helper 均构建成功。`git diff --check` 退出码 0；存在 LF/CRLF 提示，不是差异检查失败。

本轮新增回归使用现有白名单测试客户端和**非提权测试 helper**，保留身份、nonce、会话校验，不削弱生产授权边界。旧兼容性回归中的永久删除也只针对该测试自行创建的 fixture。

## 失败记录及处理

没有删除失败用例，也没有将第一次失败隐藏为成功。

1. 第一轮：**27 PASS / 1 FAIL，exit 1**。合并目录 Move 的精确路径检查失败。
2. 加入扫描测试后的第二轮：测试钩子声明与定义命名空间不匹配，出现 **LNK2019**，未运行测试；修正为命名空间作用域声明。
3. 第三轮：**40 PASS / 1 FAIL，exit 1**。诊断确认真实映射为正确的 `tree\a-first.txt`，而测试预期写成 `tree/a-first.txt`。刷新与映射数量均为 1。
4. 只将 fixture 改为逐个路径组件构造，**保留精确路径比较和数量断言**。第四轮 **41 PASS / 0 FAIL，exit 0**。
5. 增加 M04-014 权限矩阵后，第五轮 **90 PASS / 0 FAIL，exit 0**；再执行两组已有兼容性测试，得到上述最终 121 PASS。

## 证据位置

以下路径均相对项目根目录：

- 最终构建：`bench_data/p2-transfer-build-5.log`
- 最终新增回归：`bench_data/p2-transfer-test-5.log`、`bench_data/p2-transfer-5.exit`
- 会话兼容性：`bench_data/p2-session-compat.log`、`bench_data/p2-session-compat.exit`
- Shell 传输兼容性：`bench_data/p2-shell-compat.log`、`bench_data/p2-shell-compat.exit`
- 历史失败/修正过程：`p2-transfer-test.log`、`p2-transfer-build-2.log`、`p2-transfer-test-3.log`、`p2-transfer-test-4.log`（均在 `bench_data/`）

本地工作区另存对应日志、`pulse-review/p2-only.patch`、`pulse-review/p2-changed-files.txt`。补丁已在本地 P2 基线副本通过 `git apply --check`，**未在当前已修复的 Windows 工作树再次应用**。CMake 的 P2 前版本由当前文件撤去本批新增的测试源与链接库重建，其他源码基线是修改前读取快照，详见 `pulse-review/p2-baseline-note.md`。

### 本批结束时的 SHA-256

| 文件 | SHA-256 |
|---|---|
| `build_st/pulse.exe` | `145D38D84C270F0D90449BB022981B423E69156AE6CE68AFF824C69143FD3E46` |
| `build_st/pulse_elevated.exe` | `4B513806E006766F0F89036F215F5D332B2C3E25E384CC081720C88D951DE061` |
| `build_st/pulse_elevated_session_test.exe` | `B7C399658CBD84E8212CCCE47D090A4CCA4235A64864A50DDDC81B445BB123D4` |

后续构建会改变这些 hash；P1 报告中的主程序 hash 也不再代表当前构建。

## 明确未覆盖的边界

- **M04-011**：请求已接受但引擎尚未启动时的取消/暂停，尚未修复；M04-008 的扫描阶段测试不能替代该验收。
- **M04-003**：KeepBoth 移动的撤销恢复原名，尚未修复。
- 没有验证真实管理员授权、真实两账户会话、Result 丢失后的恢复，以及标签/托盘/剪贴板 UI 全链路；仅证明已确认路径的生产完成队列契约。
- 权限探针是只读的根级判断，返回不需提权不保证所有后代权限；没有在检查和执行之间提供原子权限保证。
- 缺失祖先和已有目录合并的 ACL 矩阵项验证路由判断，不等同所有复杂继承 ACL 下的实际递归复制。
- 取消在可检查的边界生效，不承诺中断任意阻塞的系统调用/网络提供程序。
- P1 的 105 PASS 是上一批历史检查点，本批没有重跑全部 P1。

## 变更文件

`CMakeLists.txt`；`src/ops/ops_manager.cpp`、`ops_manager.h`、`ops_manager_shell_transfer.cpp`、`elevated_transfer_client.cpp`、`elevated_transfer.cpp`、`elevated_transfer.h`；`src/bench/elevated_session_test.cpp`、新增 `review_transfer_test.cpp`。

交互及操作约定继续保存在 `/home/user/localbridge-notes.md` 和 `/home/user/pulse-review/current-task.md`：需要用户决策时使用交互式选项，不以普通文本提问替代。
