# Pulse v1.0.33：两个 PR、本地修复与正式发布

日期：2026-09-18（UTC+08:00）。用户授权合并 PR #5、#6，包含本地性能修复和关闭临时诊断的改动，推送 GitHub 并发布下一个补丁正式版。

## 最终结果

- 正式版：[Pulse v1.0.33](https://github.com/jimmgreen/pulse/releases/tag/v1.0.33)。
- 2026-09-18 22:12:46 +08:00 发布；最终核对为非草稿、非预发布，latest 为 v1.0.33。
- 远端 main、标签解引用后的提交、CI 构建提交均为 `355ec4b3434e929b89603bae0f844190ed6eac9b`。
- PR #5、#6 均在 GitHub 显示 MERGED，合并时间为 22:02:15 +08:00。
- 两个 Windows 构建及发布任务全部成功，4 个正式附件已下载复核。
- 本轮没有安装或升级本机正在运行的 Pulse，也没有修改用户索引、历史或配置。

## 纳入的改动

1. [PR #5](https://github.com/jimmgreen/pulse/pull/5)：侧栏、详情面板和窗口宽度调整。
2. [PR #6](https://github.com/jimmgreen/pulse/pull/6)：受保护操作系统文件的独立显示选项、偏好及选择行为。
3. 本地 27 个明确选定的源码／测试／CMake 文件：隐藏窗口历史轮询优化、历史汇总与序列化、Feed 存储、USN 队列容量与背压、名字池局部整理、临时性能诊断默认关闭。
4. 整合检查发现并修复 #5 的高 DPI 单位混用：EffectiveSidebarWidth 原本将已缩放像素值与 DIP 上限比较后再次缩放，改用 sidebar_width_dip_。新增 100%、125%、150%、200% 下的实际侧栏宽度、上限和折叠宽度检查。
5. version.txt 更新为 1.0.33；公开更新说明为 docs/releases/1.0.33.md。发布 CI 增加与本次改动相关的定向回归。

没有提交 .workbuddy-ai、bench_data、内部 MCP 排查报告、采样文件或其他无关未跟踪文件；没有读取、导出或提交发布签名私钥。

## 合并与提交

使用隔离工作树 bench_data/release-v1.0.33，分支 release/v1.0.33-integration。起点为原本与远端一致的 main：97da20efd06f1619d5e06ee26433bedcdd67ee98。

- 本地修复提交：cc82849（fix(index): bound background retention and make performance diagnostics opt-in）。
- #5 合并提交：fcd1f4030636c1304f6d069fea119ed76a80d909。
- #6 合并提交：b7b74f64df1c09fe5089320c1e6a026b7b3bfd96。
- 发布准备提交：355ec4b3434e929b89603bae0f844190ed6eac9b。
- 注释标签对象：97d50749b34137fae919aa23c96ea754a0aa1065，解引用为上述发布提交。

两个 PR 均通过普通合并保留其提交历史；共同修改的文件自动合并成功，没有丢弃其中任一 PR 的改动。

首次普通 Git 推送停住，已取消，并经 GitHub API 确认远端 main 未变、版本标签不存在。随后仅在该次 Git 命令中使用已登录的 gh 凭据辅助程序，以 --atomic 一并普通推送 main 和标签，退出 0。没有强推、覆盖标签或永久修改凭据配置。

## 验证与发布流程

本地隔离构建的 14 组定向检查共 **317 项通过、0 项失败**：控制器 74、面板偏好 9、默认关闭诊断 24、名字池 60、维护 19、USN 10、Feed 16、父节点环 14、轮询 9、历史 37、历史内存 8、Feed 存储 13、显式内存探针 1、包队列 23。另通过发布脚本语法和 git diff --check 检查。

正式构建沿用仓库的 GitHub Actions，不将本地开发工具链产物冒充正式包：

[Build and publish Pulse / 35353761256](https://github.com/jimmgreen/pulse/actions/runs/35353761256)

| 任务 | 结果 | 完成时间（+08:00） |
|---|---|---|
| build (windows) | success | 22:12:07 |
| build (win81) | success | 22:12:18 |
| publish | success | 22:12:50 |

正式流程使用 v143、固定且校验过的 LumaText SDK，运行既有发布检查及新增的定向检查，再关闭嵌入式自测、检查发布载荷；Windows 8.1 另做启动导入审计。签署更新清单并上传两个安装包后，才将 Release 公开并设为最新。

## 公开附件与完整性

| 附件 | 字节数 | SHA-256 |
|---|---:|---|
| PulseSetup-1.0.33.exe | 8,000,092 | 3d69620c5b7c01a6b965da6c15d41fbf72ab293d0b677c3c8bdd946ed7f04d4d |
| PulseSetup-1.0.33-win81.exe | 8,000,577 | f74a8762e9b0c93642d6f9326c4aa5f0f247ab043c209ab228470c2d4b0c48aa |
| update-manifest.json | 384 | 260938d101f4cb323d9deaec0b957041aab21feb99fd89dd9012f6af70c2c034 |
| update-manifest-win81.json | 389 | b43f29b575af2265fca79fc78b91b7c62f358b8a709057eaaaee7b11823b2b1b |

已从 Release 实际下载全部 4 个附件，核对大小、GitHub 资产 digest 和本地 SHA-256。使用源码中的公钥独立验证两份清单的 ECDSA P-256 签名，同时核对版本、下载 URL、安装包哈希和最低系统 build：普通版 10240、Windows 8.1 版 9600。此处验证的是更新清单签名，不等同于宣称安装程序具有 Authenticode 代码签名。

下载副本位于 bench_data/release-integration-20260918/published-assets/。验证记录为 asset-verification.json、final-audit.json；CI、PR、推送及本地测试记录也留在同一审计目录。

## 原工作区保留情况

原目录 <repo> 未做 reset、stash 或源码覆盖；所选 27 个原文件在发布后仍与发布前快照 SHA-256 一致，无关未提交内容和旧安装包保持原状。

**原工作区的本地 main 因此仍停留在旧提交，状态显示落后 origin/main 6 个提交，原有未提交修改仍可见。** 这些相关修复已经通过隔离工作树提交并包含在远端正式版中，并非遗漏。完整整合源码位于 bench_data/release-v1.0.33；不要在未妥善处理原工作区修改前直接执行破坏性同步。

本次未继续做短查询性能优化，也不宣称消除历史快照瞬时峰值。既有性能限制不因正式发布而自动视为已解决。
