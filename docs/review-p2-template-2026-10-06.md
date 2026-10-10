# Pulse · P2 命令模板实机回归

日期：2026-10-06。项目：`<repo>`。

## 结论

已修复 **M04-010：Shell 命令模板再次解释插入文件名中的占位符**，并在你的 Windows 电脑上完成修复前后对照。

- 原代码：**10 PASS / 9 FAIL，exit 1**。
- 修复后，同一组测试：**19 PASS / 0 FAIL，exit 0**。
- 主程序及对应依赖构建：**exit 0**。
- 最终九组实机回归：**425 PASS / 0 FAIL，九组均 exit 0**。
- `git diff --check`：exit 0；已有 LF/CRLF 提示未通过批量格式化处理。

425 是此次检查点的断言数，包含此前的 389 项，不能再与历史总数相加，也不是已修复问题数。

## 修改

1. `ExpandShellCommand` 只扫描原始模板一次。识别 `%1/%L/%l/%V/%v/%*`，插入的文件名始终作为数据，不再经过后续占位符替换。
2. 对完整带引号的占位符一并消费外层引号，避免额外双重引用。
3. 提取 `QuoteWindowsArgument(std::wstring_view)`，沿用 Terminal 已有的 Windows CRT 引用规则：正确处理引号前、参数末尾的反斜杠。Terminal 复用该函数，保持原有 `-d` 参数行为。
4. 未知占位文本及无占位符模板保持不变；保留既有扩展路径前缀转换。

生产改动仅在 `src/ops/ops_manager.cpp`、`shell_command.cpp/.h`。测试改动在 `src/bench/elevated_session_test.cpp`、`review_transfer_test.cpp`。

## 实机验证方法与安全边界

新增 `--review-template` 通过 test-only friend 调用真实 `OpsManager::ExecuteCommand` 和入队路径，但不启动 OpenThread。

测试将自己的可执行文件复制到独有 `bench_data/review-template-PID-tick` 目录，以显式 `lpApplicationName` 启动 `argv recorder.exe --template-child`。子程序仅将 UTF-16 argv 写回同一测试目录；测试逐项比较真实 Windows 参数解析结果，然后清理 fixture。

覆盖八组路径字符串，包括占位符样式文本、Unicode、空格、盘符根、尾反斜杠、UNC 根和扩展路径前缀；每组涵盖全部支持的占位符和未知 `%Q`。

**没有执行用户配置的命令，没有打开这些选定路径，没有运行真实 Shell 动词或申请管理员权限。** 含 `%*` 的值是纯参数数据，并没有创建非法文件名。兼容性测试的提权分支使用 stub，不代表已执行真实 UAC。

## 最终回归

| 测试 | PASS | FAIL | 退出码 |
|---|---:|---:|---:|
| 命令模板生产入队及真实 argv | 19 | 0 | 0 |
| 文件操作 `--review-transfer` | 316 | 0 | 0 |
| 已有授权会话 | 19 | 0 | 0 |
| 首次启动失败恢复 | 6 | 0 | 0 |
| 旧读取／新连接交错 | 9 | 0 | 0 |
| 半帧／新连接交错 | 10 | 0 | 0 |
| 当前 token Shell 传输 | 12 | 0 | 0 |
| 授权协议 v3 | 17 | 0 | 0 |
| 命令启动与 Terminal 兼容性 | 17 | 0 | 0 |
| **合计** | **425** | **0** | |

最终构建包装器产生了成功构建记录，但未产生后续测试文件。因此在确认 build exit 0 后，已单独启动全部九组回归，并下载、逐项统计每份日志和退出码。未将 launcher 的完成状态当作测试成功。

## 交付与证据

- 本批补丁：`pulse-review/p2-template.patch`，已在本地修复前基线通过 `git apply --check`，未重复应用到已修改的 Windows 工作树。
- Windows 报告：`docs/review-p2-template-2026-10-06.md`。
- 对照日志：`bench_data/p2-template-baseline-test.log`、`p2-template-fixed-test.log`，及对应退出码。
- 最终证据：`bench_data/p2-template-final-{build,template,transfer,session,startup,generation,partial,shell,protocol,command}.log/.exit`。

检查点 SHA-256（后续构建可能更新）：
- `build_st/pulse.exe`：`64F6BAF14072F05E1167BBFB7FF689AFE095756AAC3E95E1DC54E1D938098717`
- `build_st/pulse_elevated_session_test.exe`：`112E3DE883CD3BBDF2A9721DCB077041CC43095395104915CE2B6DF80D4A2002`

## 尚未证明／继续处理

本项证明普通路径参数被重复展开的问题已修复，不将其夸大为已复现任意代码执行，也不声称覆盖任意 `cmd /c`、PowerShell 或复杂混合引号模板语义。

M04 已完成定向修复并实机回归的 P2：002、003、004、005、008、010、011、012、013、014，共 10 项。001、006、007 尚未关闭；009 为 P3。继续处理菜单请求归属及 worker 生命周期。整个审查报告的 121 项并未全部关闭。
