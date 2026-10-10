# Pulse · P2 路径正规化实机回归

日期：2026-10-06。项目：`<repo>`。

## 结论

**M03-004 已完成定向修复与 Windows 实机对照回归。** 普通盘符相对路径、DOS／UNC 点段路径先按 Windows 语义解析，再添加 extended 前缀；正规化失败不再制造貌似完整的 extended 路径。相关导航和缓存入口区分错误返回与合法的空“此电脑”路径。

| 阶段 | PASS | FAIL | SKIP | 退出码 |
|---|---:|---:|---:|---:|
| 首轮原逻辑基线 | 16 | 12 | 0 | 1 |
| 扩展原逻辑基线 | 16 | 19 | 0 | 1 |
| 修复后，同一组 | 35 | 0 | 0 | 0 |
| 最终路径／缓存测试 | 43 | 0 | 0 | 0 |
| 直接依赖：回收站元数据 | 44 | 0 | 0 | 0 |
| 直接依赖：reparse／缓存兼容性 | 15 | 0 | 2 | 0 |
| **最终三组检查点** | **102** | **0** | **2** | **均为 0** |

主程序与三个相关测试目标构建 **exit 0**；本批范围 `git diff --check` **exit 0**；增量补丁在独立修复前基线通过 `git apply --check`。

102 是本轮最终断言总数，不与 M03-003 的 97 项、此前 M04 的 506 项或本轮重复运行累加。

## 基线证明了什么

测试在独有目录中创建 `one` 和 `target`，`target` 中放置唯一 marker 文件；只修改测试进程自己的工作目录。

旧代码不仅字符串输出错误，以下两个真实目录枚举也无法得到预期 marker：

- `<当前盘符>:target`：旧代码直接加前缀，产生仍是盘符相对的 `\\?\C:target`。
- `<自有目录>\one\..\target`：旧代码在解析点段前添加 extended 前缀。

扩展基线还证明：同一目标的不同普通输入无法共享 SnapshotStore 条目、代际和 dirty 标志。修复后原 35 项全部通过，没有删除或放宽失败断言。

## 实现变化

### 1. 普通路径先解析，extended 路径保留语义

`src/fs/fs_enum.cpp`：

- 普通相对、盘符相对、根相对、盘符绝对及 UNC 输入统一先调用真实 `GetFullPathNameW`，再转换为 long-path 前缀形式。
- `C:folder` 按 Windows 的该盘符当前目录解析，不擅自改为当前标签页目录。
- 保留产品既有约定：裸 `C:` 表示 `C:\`。
- 显式 `\\?\`／`\\?\UNC\` 输入仍走既有字面路径分支，不重新解释其中的点段或尾部点／空格。
- 保留大小写，不把不同拼写强行折叠成小写。
- 普通路径解析失败、缓冲不足或包含嵌入 NUL 时拒绝；非空输入的失败结果为空，并保留／设置 Win32 错误。
- 目录枚举在非空输入正规化失败时抛出错误，不把空结果拼成根目录通配符。

这不是完整的 Windows 路径合法性验证器，也没有改变显式 extended 输入的全部既有校验规则。

### 2. 错误输入不能冒充“此电脑”或缓存空键

- `app_navigation.cpp`：NavigateTo、StartLoadingPath 在改变当前目录、历史或 worker 状态前拒绝失败转换；使用既有 CannotOpen／DirectoryUnavailable banner。RefreshPath 忽略失败转换。
- `fs_snapshot.cpp`：所有按路径取键的入口拒绝“原输入非空、结果为空”的错误，不能新增错误条目或读写合法的空“此电脑”键；目录身份查询也明确拒绝失败结果。
- `fs_net_cache.cpp`：先正规化、再取得缓存目录；错误 UNC 输入不创建缓存目录、不生成空键对应的缓存文件。
- `fs_recycle.cpp`：回收站根转换失败时停止，不能继续拼接 SID 成根相对路径。

内存键、磁盘键、父路径和目录身份查询继续复用同一正规化约定，没有引入另一套字符串折叠算法。

## 最终 43 项路径测试覆盖

- 普通相对、盘符相对、根相对、纯盘符及盘符根。
- 普通 DOS／UNC 的 `.`、`..`、正斜杠和尾分隔符。
- UNC server-only、share 表示保持兼容。
- 显式 extended DOS／UNC 保持原值，含字面尾部点／空格。
- 大小写不同的名称保持原大小写；虚拟路径和合法空路径不变。
- 父路径、超过 MAX_PATH 的普通路径字符串、超限输入及嵌入 NUL。
- 实际 owned 目录枚举及同一目录的真实 identity 比较。
- 实际 SnapshotStore 的键、代际、dirty 状态与无效键保护。
- 真实磁盘缓存文件的键一致性、保存／读取及错误输入不改变原有字节。

磁盘缓存测试只在测试翻译单元中将 `SHGetFolderPathW` 的返回根重定向到私有 fixture；没有 mock 文件读写。UNC 仅作为路径字符串或缓存键，**没有连接任何测试服务器或网络共享**，也没有写入真实用户 profile 缓存。

## 关联回归和权限限制

正规化、枚举和网络缓存都被现有 reparse 测试直接包含，因此本轮执行该相关目标；同时重跑受影响的回收站读取测试。不扩展到全部历史套件或完整 selftest。

reparse 兼容性结果为 **15 PASS / 0 FAIL / 2 SKIP**：

```text
[SKIP] real directory symlink creation unavailable: 1314
[SKIP] real file symlink creation unavailable: 1314
```

1314 表示当前上下文缺少所需特权。没有请求提权、修改开发者模式或放宽断言。这两个创建分支及其依赖的真实 symlink 枚举检查不算通过；静态链接类型分类检查与真实 symlink 创建验收不是同一件事。

通过的实际项目包括自有目录中的真实 junction、NT 枚举、Win32 fallback、v2 缓存标签、v1 兼容读取和截断缓存拒绝。

旧 reparse 测试内部使用 PID 命名 fixture，因此本轮在外层再原子新建 GUID 独有工作目录，避免复用旧文件夹；执行完已确认内外层清理。路径测试也确认恢复本测试进程 CWD 并删除自有 fixture。

## 仍未覆盖的边界

- 未操作用户正在使用的 Pulse 窗口；导航错误入口已修改并构建，但未对地址栏交互、banner 布局或全部 GUI 路由做桌面验收。
- 大小写检查验证“不折叠字符串”，未创建／配置真实大小写敏感目录。
- 超过 MAX_PATH 的用例验证真实 Windows 路径解析，没有据此声称完成长目录树的实际打开／移动验收。
- UNC 用例不是网络可用性或网络 I/O 超时测试。
- 本轮没有修复 M03-005 的磁盘缓存跨请求提交代际问题，也没有修复其他尚未处理的审查项。
- 未改变真实服务配置、真实回收站、用户原有进程或仓库提交；保留用户现有未提交修改。

## 文件与证据

生产改动：

```text
src/fs/fs_enum.cpp
src/fs/fs_snapshot.cpp
src/fs/fs_net_cache.cpp
src/fs/fs_recycle.cpp
src/app/app_navigation.cpp
```

测试／构建：`src/bench/path_normalize_test.cpp`、`CMakeLists.txt`。

- Windows 报告：`docs/review-p2-normalize-2026-10-06.md`。
- 增量补丁：`pulse-review/p2-normalize.patch`，以 M03-003 已修复状态为基线，包含七个文件。**不要重复应用到已修改的 Windows 工作树。**
- 首轮基线：`bench_data/p2-normalize-baseline-{build,test}.log`、`p2-normalize-baseline-build.exit`、`p2-normalize-baseline.exit`。
- 扩展基线：`p2-normalize-expanded-baseline-*`。
- 首轮修复成功：`p2-normalize-fixed-*`。
- 最终证据：`bench_data/p2-normalize-confirm-{build,path,recycle,reparse,diff}.log/.exit`，均已下载并逐项检查。

最终检查点 SHA-256：

- `build_st/pulse.exe`：`EEBC2602B6856AED4E824B3078E6B52582055C523460A31B4CF43767E0EA9A15`
- `build_st/pulse_path_normalize_test.exe`：`45FD9A38DDE8789182C5AFB317F8581142BEDBFCCBB50E9BD0EB1D3937BE4146`

本轮此前交付的 M03-003 报告为《Pulse-P2回收站元数据安全实机回归.md》，Windows 副本为 `docs/review-p2-recycle-2026-10-06.md`。两项各有独立前后对照，不代表整份 121 项审查全部完成。
