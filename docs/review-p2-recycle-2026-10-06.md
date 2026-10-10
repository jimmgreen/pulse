# Pulse · P2 回收站元数据安全实机回归

日期：2026-10-06。项目：`<repo>`。

## 结果

**M03-003 已完成定向修复和 Windows 实机前后对照验证。** 损坏的回收站索引不再通过缩短原路径“修复”成另一个还原目标。

| 阶段 | PASS | FAIL | 测试退出码 |
|---|---:|---:|---:|
| 初始缺陷基线 | 13 | 5 | 1 |
| 扩展缺陷基线／观察版 | 21 | 23 | 1 |
| 修复后，同一组断言 | 44 | 0 | 0 |
| 最终确认：回收站元数据 | 44 | 0 | 0 |
| 关联回归：worker 获取与 Join | 19 | 0 | 0 |
| 关联回归：查询中 Close 及相邻路径 | 34 | 0 | 0 |
| **本检查点三组最终测试** | **97** | **0** | **均为 0** |

主程序、Shell host、元数据测试和 worker 生命周期测试关联构建 **exit 0**；本批范围 `git diff --check` **exit 0**，仅有 Git 的 LF/CRLF 转换提示。增量补丁在独立修复前基线中通过 `git apply --check`。

97 是本轮直接受影响范围的断言数，不是问题数，不与此前 M04 的 506 项或本轮重复运行累加。没有为本次修改重跑全部历史套件。

## 原缺陷不是只在模拟解析器中出现

在测试私有目录中，索引声明原目标为 `dest\photo.jpg`，实际字节截断到同一目录的 `dest\photo`。旧代码两处读取器都接受了这个短路径；调用生产代码中提取的最终还原步骤后，实际发生了错误改名和索引删除。

观察版缺陷日志：

```text
malformed restore accepted=1 parsed_short_target=1
wrong_target_exists=1 wrong_target_body=1 original_target_exists=0
index_left=0 payload_left=0
```

修复后日志：

```text
malformed restore accepted=0 parsed_short_target=0
wrong_target_exists=0 wrong_target_body=0 original_target_exists=0
index_left=1 payload_left=1
```

拒绝后既没有创建错误名称，也没有创建完整原目标；`$I` 与原 `$R` 内容均保留。没有改弱失败断言。

## 生产修改

新增 `src/common/recycle_index.h`，让目录展示端与 Shell 还原端共用同一个有界读取／解析实现：

- 实读字节必须等于本次观察到的文件大小，不再用未读到的零填充字节补齐记录。
- 逐字节读取小端整数，避免对外部记录直接进行未对齐整数／字符串指针读取。
- v1 必须恰好 544 字节，并在固定的 260 个 UTF-16 单元中找到终止 NUL；终止之后的固定字段仍作为兼容性填充。
- v2 必须恰好等于 `28 + 声明单元数 × 2`；声明长度包括最终 NUL，拒绝提前 NUL、缺少最终 NUL、声明不符、尾随数据和未知版本。
- 路径单元数上限 32768，记录上限 65564 字节，分配前检查范围。
- 按还原端相同的扩展前缀转换检查完全限定路径，拒绝本次测试中的相对、盘符相对、根相对、设备及未知扩展命名空间；保留支持路径的原始字符串。
- 读取句柄采用 RAII。列表仍要求对应 `$R` 存在。

保留生产的当前用户 SID／回收站根校验、`MoveFileExW(..., 0)` 不覆盖已有目标，以及移动成功后才删除索引的顺序。

涉及文件：

```text
CMakeLists.txt
src/common/recycle_index.h
src/fs/fs_recycle.cpp
src/shell_host/main.cpp
src/bench/recycle_metadata_test.cpp
```

## 验证覆盖与安全边界

44 项断言包括：v1/v2 健康 Unicode 路径与大小、真实隔离还原、目标冲突不覆盖、失败保留索引与内容、孤立索引，以及声明长度、终止字符、奇数字节、版本、路径类型和短读异常。

逐字节截断矩阵：

- v1 的 544 个短前缀：旧实现有 518 个被至少一个读取器接受；修复后接受数为 0。
- 本次 v2 样本的 196 个短前缀：旧实现有 166 个被接受；修复后接受数为 0。v2 样本长度随唯一 fixture 路径长度而变化。

所有任意前缀和异常矩阵**仅调用读取器，不执行还原**。只有刻意限制在自有 `dest` 目录中的 `photo.jpg` → `photo` 样本执行真实最终还原步骤，避免任意截断绝对路径落到测试目录之外。

测试以原子新建的唯一 `bench_data/review-recycle-PID-tick` 目录作为所有实际读写、移动和清理范围。UNC／扩展 UNC 样本只作为索引中的字符串解析，不连接网络。

测试编译包含真实 host 实现，但不调用 `wWinMain`，也**不调用会进入真实回收站 legacy 搜索的 `RestoreOneFromRecycle`**。测试调用的是生产完成 SID／根验证后使用的最终单条还原步骤。短读注入宏只属于测试 target。

**没有访问或修改真实回收站，没有执行真实管理员／双账户验收，没有修改服务配置、关闭用户原有 Pulse 实例、创建提交或推送仓库。**

限制：这不是校验和或防篡改格式，也不是读写并发下的原子快照。语法完整但内容本来就错误的索引不能据此识别；v1 固定字段 NUL 后允许填充，不能声称检测所有提前终止的 v1 路径。未以本轮测试证明所有 GUI、第三方 COM、网络 I/O 或长路径实际还原情况。

## 证据与补丁

Windows 报告：`docs/review-p2-recycle-2026-10-06.md`。

本地增量补丁：`pulse-review/p2-recycle.patch`。这是在此前已修复 M04 的工作树基础上的本批差异；**不要重复应用到已经修改好的 Windows 工作树**。

Windows 证据（均已下载）：

- `bench_data/p2-recycle-baseline-{build,test}.log` 与 `.exit`。
- `bench_data/p2-recycle-expanded-baseline-{build,test}.log` 与 `.exit`。
- `bench_data/p2-recycle-observed-baseline-{build,test}.log` 与 `.exit`。
- `bench_data/p2-recycle-fixed-{build,test}.log` 与 `.exit`。
- 最终：`bench_data/p2-recycle-confirm-{build,recycle,worker,close,diff}.log/.exit`。

检查点 SHA-256：

- `build_st/pulse.exe`：`9818DCB3D856FBE751815C70D7E01318068CCE5862E260B4229C05638AD0A101`
- `build_st/pulse_shell.exe`：`E85DCB33C1907720C1B2DCD0A0CE7920A4F3C246B2CE9FA1FAD63C7FE012BE38`
- `build_st/pulse_recycle_metadata_test.exe`：`37C60E22EF76E5036A7C7C0FA04B3E4E5C2AC6496962E1467634344873B5AD5B`

M03 的其他问题和其他模块尚未全部处理。本报告只关闭 M03-003 的本次定向修复验收，不代表整份 121 项审查全部完成。
