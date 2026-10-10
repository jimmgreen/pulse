# memory4 名字池整理修复版：安装与部署

日期：2026-09-18。用户在获知升级会关闭 Pulse 及相关进程、短暂中断索引后要求继续；本轮执行了原有安装器的就地升级流程。

## 产物

- 安装包：dist/PulseSetup-1.0.32-memory4.exe。
- 完整路径：<repo>/dist/PulseSetup-1.0.32-memory4.exe。
- 大小：7,881,762 B。
- SHA-256：B05A45F16C9F4F79BC012836F7F104DDF7311C6D25578F78E75326935E0098AA。
- 清单：dist/PulseSetup-1.0.32-memory4.manifest.json。
- 产品版本仍是 1.0.32，memory4 仅为安装包变体名称；没有改 AppId 或升级路径。构建标识仍为 20260916T002441Z-62B651EE，因此版本核验以文件 SHA-256 为准。

## 打包与安装核验

仅构建 pulse 与 pulse_index 两个生产目标，退出 0；主程序因索引头文件依赖进行了增量重编译。本轮未修改生产源码或重复运行上一轮已经通过的 79 个相关检查。

Release / PULSE_WITH_SELFTEST=OFF。主程序通过 scripts/check_release_payload.ps1 检查，索引程序包含 name_pool_compact、deleted_ratio、reclaimed_chars 诊断标记。直接使用既有 installer/PulseSetup.iss 与 Inno Setup 6.7.3，采用独立 memory4 输出文件名。打包前后七个主要输入哈希一致，原版、memory、memory2、memory3 四个 1.0.32 安装包哈希均未改变。

安装前校验新包哈希，再以 Windows RunAs 请求本地 UAC 提权，未绕过 UAC。安装参数为 /VERYSILENT /SUPPRESSMSGBOXES /NORESTART，并保存安装日志。原安装器会强制关闭 Pulse、停止服务、静默卸载旧程序，再安装新程序；静默升级路径不清理用户数据。

安装器日志时间：20:21:56.792–20:26:10.062 +08:00，安装退出 0，无需重启 Windows。文件安装耗时较长，本轮未对耗时原因作无证据归因。

安装后七个主要文件 SHA-256 全部匹配清单：

| 文件 | SHA-256 |
|---|---|
| pulse.exe | 4B90B5B254A6495BACEA1BF959172A3B4745619EC1D1282F2C7B37FD5150D46B |
| Pulse.Index.exe | 027ED55E5ABDF381934153D460D33FFA46C7E1261CD467D5DE684F48348762E9 |

其余五个文件的哈希见清单，均与升级前相同。安装目录仍为 C:/Program Files/Pulse，索引目录仍为 C:/ProgramData/Pulse/Index；安装任务仍是 indexservice,startup,desktopicon。

- PulseIndex：Running，PID 12900，启动时间 20:26:06.390 +08:00。
- 主程序：PID 28080，启动时间 20:26:10.534，以 Hidden 窗口启动方式恢复；未修改用户偏好。
- 网络代理：PID 26304。
- Shell：PID 36760。

索引、历史没有被本轮脚本清空；没有裁剪工作集。目录和注册选项已核对，不将此表述为对所有历史记录做了逐条完整性校验。

## 初始查询检查与观察

安装后现有索引返回 2,910,355 项。运行已有 build/pulse_index_perf.exe，对五种查询各做五次读取：

| 查询 | p50 ms | 探针 p95 ms | 命中数 | 探针预算 |
|---|---:|---:|---:|---|
| a | 318.50 | 320.71 | 1,431,338 | 超过 150 ms |
| ab | 29.91 | 49.42 | 90,038 | 满足 50 ms |
| abc | 2.18 | 3.23 | 4,728 | 满足 50 ms |
| report | 0.79 | 0.80 | 2,202 | 满足 100 ms |
| pdf | 1.69 | 1.87 | 8,002 | 满足 100 ms |

该工具未使用 --enforce，退出 0 仅表示查询流程完成，不能据此称全部性能预算通过。a 查询超预算已保留；没有升级前同负载查询对照，不能归因于本次修复或直接称为回归。将在本轮观察结束后复查一次，不因此扩展为全面搜索优化。五次样本的探针 p95 不是大样本延迟分位数。

30 分钟观察于 20:27:25.388 +08:00 开始，15 秒间隔采样，不制造改名风暴或主动强制合并。首帧服务仍 Running，采样错误 0，新版本诊断可读取。最终结果另记 docs/mcp-index-memory4-postinstall.md；本文不预先声称合并次数或内存峰值已经下降。

## 原始资料与过程限制

资料均位于 bench_data/name-pool-deploy-20260918/：preflight.json、postinstall.json、build.log、package.log、install.log、install-driver.log、install-exit.txt、search-before.log，以及安装前后保存的配置和诊断副本。观察脚本、采样及分析也保存在该目录，不放在 docs/。

本轮最初新建 PTY 两次未能在 8 秒内到达提示符，改为复用原有受管理终端后正常。首次创建预检脚本因父目录尚未存在被拒绝，随后的执行报脚本不存在；创建目录并重新写入后运行成功。一次观察数据读取早于首帧落盘而返回文件不存在，随后确认正常采样。这些是工具准备/时序问题，不是生产程序故障。

实现及隔离验证见 docs/mcp-index-name-pool-compaction.md。安装前运行的是 memory3，本次确实已升级，后续观察应按新 PID 和哈希过滤，不沿用旧进程累计计数。
