# Pulse 开放 PR 只读盘点

日期：2026-09-18。用户选择先列出开放 PR、检查后再决定；本轮未执行 fetch、pull、merge、push、checkout、stash 或提交，也未运行测试。

仓库：jimmgreen/pulse。本地：<repo>，当前分支 main。

| PR | 内容 | 目标 | 改动 | GitHub 状态 |
|---|---|---|---|---|
| https://github.com/jimmgreen/pulse/pull/5 | 侧边面板和窗口尺寸按文件列表调整 | main | 10 文件，+164/-13 | 非草稿，MERGEABLE / CLEAN |
| https://github.com/jimmgreen/pulse/pull/6 | 受保护操作系统文件使用独立显示选项 | main | 21 文件，+132/-15 | 非草稿，MERGEABLE / CLEAN |

两项作者均为 4B-Rubber。查询返回 statusCheckRollup=[]、reviewDecision 为空；不能据此声称 CI 已通过或已经获得审批。MERGEABLE 仅表示查询时相对 GitHub 目标分支可合并，不等于代码审查或本地集成验证通过。

PR #5 头提交：12d94c6d2a0fe5563ca763225065136d53255914。
PR #6 头提交：12969a94b73af36e20b019b57a558959ebacd746。

本地有 11 个受跟踪文件尚未提交，并有未跟踪的修复源码、测试和报告；包含此前性能修复和关闭诊断的工作，不能重置或覆盖。两项 PR 的文件列表与当前已修改的受跟踪文件无直接重叠，但这不是合并无冲突或行为兼容的证明。

两项 PR 共同修改 src/app/app_prefs.cpp、src/app/selftest_1b2.cpp、src/bench/app_controllers_test.cpp、src/ui/ui_renderer.h；若选择两项，应顺序集成，合并第一项后重新核对第二项。

下一步待用户指定 PR 和本地／远端合并范围。建议在保护现有未提交修改的前提下，使用隔离工作树检查选定 PR，再进行定向验证。未自动提交、暂存或移动用户修改。

审计快照位于 bench_data/pr-inventory-20260918/（worktree.txt、pr5.json、pr6.json）；仅新增检查记录，本轮没有修改产品源码、分支或提交。
