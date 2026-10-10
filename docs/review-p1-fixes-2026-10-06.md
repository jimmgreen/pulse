# Pulse：P1 首批修复与 Windows 实机回归

日期：2026-10-06  
项目：`<repo>`  
依据：用户提供的《Pulse逐模块代码审查-2026-10-05 (1).md》  
范围：用户选择优先处理五项 P1；权限项采用当前权限下的隔离测试。

## 结论

五项 P1 均已写入针对性代码修改，并完成下列 Windows 本机自动化检查。**最终记录为 105 条检查通过、0 条失败**；这是检查断言数量，不是修复了 105 个问题，也不是全仓回归通过。

M05、M06 的权限边界尚有管理员及双账户验收缺口，不能据此宣布全部安全验收完成。报告其余 **116 项**尚未在本轮逐项核验或修复；现有未提交修改是否已覆盖其中一些问题，需后续对照。

没有提交 Git、重置工作区、发布安装包、替换正在运行的程序、迁移真实索引或改变服务配置。开始时已有 107 个受跟踪文件修改，本轮在读取版本后增量修改，保留原有内容。结束时原有三个 Pulse 实例仍在运行，因此**当前已安装/已运行的旧程序不会自动获得这些修复**。

## 五项修改

| 审查项 | 实施内容 | 本轮验证及限制 |
|---|---|---|
| M18-001 启动失败覆盖配置 | 增加启动完成状态；会话自动保存、更新握手、关机及销毁保存检查初始化状态 | 在图形初始化前、初始化后但加载配置前注入 WM_CREATE 失败；确认普通实例进入 WM_DESTROY，四份配置 SHA-256 不变。正常初始化和更新握手隔离保存通过。未模拟真实 GPU 驱动故障 |
| M10-001 筛选后 Shift 选择越界 | 鼠标与方向键/Page/Home/End 共享显示行映射；范围选择跳过折叠组；不可见旧锚点从当前目标重新起选 | 文本、标签、差异及组合筛选、正反向、升降序、隐藏属性、折叠组和六种 Shift 按键检查通过；比较生产 SelectedFullPaths 导出的路径，没有派发真实删除或移动 |
| M14-002 子菜单旧 ID 被重新解释 | 展开的 flyout 拒绝替换快照；静态动词也保存显示快照；完整实时结果中已消失的动词不再按旧整数 ID 回退执行 | 创建真实 Windows popup/flyout，验证交换 ID、重排、关闭重开及鼠标调用；生产控制器通过记录器核对 A 的重映射及消失时拒绝。没有调用真实 Shell 扩展的有副作用命令，也未覆盖所有实际扩展组合 |
| M05-001 自定义索引迁移泄漏元数据 | 服务迁移先保护/验证目标再复制；临时文件显式使用私有 ACL；验证根目录、后代与所有者；配置发布及服务启动不再忽略权限失败 | 普通可读非空目标、已有相同副本均在复制前被拒绝，保留源及无关文件。私有 ACL 契约检查通过。当前权限下私有迁移走安全拒绝分支，未验证管理员成功迁移、服务重启及受限用户全过程读拒绝 |
| M06-001 网络代理跨用户/会话访问 | 管道及 singleton 按用户 SID、会话号、登录 SID 命名；登录 SID 专用 DACL；拒绝远端；所有请求在派发前校验客户端身份；两条客户端入口校验服务器进程身份 | 真实命名管道验证同身份连接、受限令牌访问拒绝、客户端/服务器身份核验。不同用户/会话/登录的区分有身份契约测试，但不是两个真实账户或登录会话的端到端测试。超时与线程回收回归通过 |

### 权限兼容性注意

- 不安全的旧自定义索引目录将被拒绝启用，不会为了让检查通过而静默修改其中无关文件的 ACL。应在管理员授权下迁移到独立、可验证的私有目录。
- 网络代理管道名称发生变化，UI 和 Index helper 需配套使用新构建。旧进程仍保留旧行为，本轮没有擅自终止它们。
- “当前权限下拒绝且没有复制负载”不等于“管理员成功迁移已通过”。

## 实际运行结果

| 检查 | 结果 | Windows 证据目录 |
|---|---:|---|
| M10/M14/M18 图形程序定向探针及启动失败脚本 | 49 PASS，0 FAIL；汇总退出码 0 | `bench_data\review-p1-20261006-105229\` |
| `pulse_review_security_test.exe` | 21 PASS，0 FAIL；退出码 0 | `bench_data\review-security-20261006-110731\` |
| `pulse_network_agent_client_test.exe` | 2 PASS，0 FAIL；退出码 0；静默管道约 5016 ms 后终止请求 | 同上 |
| `pulse_index_migration_test.exe` | 33 PASS，0 FAIL；退出码 0 | 同上 |
| 本轮受跟踪文件 `git diff --check` | 退出码 0 | 命令输出已核对 |

构建通过的是 `build_st` 下受影响的 `pulse`、Index helper 及上述测试目标，使用项目 `scripts\vcvars.bat` 检测到的 Visual Studio 2026 工具。`build_st` 为 Release、`PULSE_WITH_SELFTEST=ON`。没有运行完整 `--selftest`、所有历史测试或正式发布流程；也没有 Windows 8.1 实机验证。

调试过程未隐藏失败：首次新增故障注入代码出现作用域编译错误，已修复；首轮权限测试的受限令牌夹具两项失败，调整令牌创建后复测通过；控制台测试进程退出码采集也改为显式持有 .NET Process 后重新核验。早期日志保留，以上表格只引用最终有效结果。

## 复跑入口

在项目根目录运行：

```powershell
# 三项 UI/生命周期 P1；要求已有 SELFTEST=ON 的 build_st 配置
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_review_p1.ps1 -Rebuild

# 两项权限 P1 + 网络客户端与普通迁移直接依赖回归
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_review_security.ps1
```

脚本使用独立 `bench_data` 目录、隔离应用数据目录和带唯一名称的管道。超时时只停止自己启动的测试进程。故障注入与 GUI 探针受 `PULSE_WITH_SELFTEST` 编译条件保护，不作为普通发布入口启用。

## 关键文件

- `src/app/app_main.cpp`、`app_state.h`：启动及退出保存门。
- `src/app/app_model.cpp/.h`、`app_input.cpp`：显示行范围选择。
- `src/ui/fluent_menu.cpp/.h`、`src/app/context_menu_controller.cpp/.h`：菜单快照与命令身份。
- `src/index/network_agent_security.h`、`network_agent_main.cpp`、`network_agent_client.cpp/.h`、`network_agent_protocol.h`、`change_tracking_client.cpp`：代理身份边界。
- `src/index/index_directory_security.h`、`index_config.cpp/.h`、`index_migration.cpp/.h`、`index_path_service.cpp`、`index_host.cpp`：私有索引迁移与启动验证。
- `src/app/p1_regression_probe.cpp`、`src/bench/review_security_test.cpp`、两个复跑脚本及 `CMakeLists.txt`：定向测试接线。

最终构建 SHA-256（开发/测试产物，非发布认证）：

```text
build_st\pulse.exe
7DEB27ED3C5BB40DBB003AFF985A94CB04B703417CDC979B29FD8BAB5C3DF917

build_st\Pulse.Index.exe
A80D4831DBDF1146586DA6866328E85BC7131C6DB5755FB677A6375E4E75BF41
```

## 后续建议

下一批按原报告进入 P2 的文件操作与持久化：先核对 M04 的取消、部分成功、撤销与提权交接，以及 M01/M09 的 JSON 和保存代际。每项继续遵循“核对当前代码—最小修改—对应隔离回归”，不以本次 105 条通过替代其验收。
