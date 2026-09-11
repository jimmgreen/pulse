# Repository Guidelines

## 工作范围与执行方式

- 以用户当前请求和本次改动为边界，保留已有未提交修改，不顺手重构或修复无关问题。
- 简单修改、构建和已有命令验证直接完成，不启动子代理。仅在复杂工作存在可独立并行的明确子任务时使用代理。
- 子代理使用 `fork_turns="none"` 或等效的不继承上下文选项；工作包只包含目标、目录、相关文件或符号、修改边界、约束和验收条件。禁止递归委派，返修复用原代理，主代理不重复执行已委派工作。
- 优先用 `rg` 定位，再读取必要代码段；审查从本次 diff 和验证结果开始，避免重复全面扫描。工具输出保留结论和诊断所需内容。
- 完成后简要汇报改动、实际验证结果和未解决问题；不要把编译成功等同于功能验证通过。

## Project Structure & Module Organization

Pulse is a Windows C++20 file manager using Win32, Direct2D, and DirectComposition:

- `src/app/` owns state, navigation, preferences, and the main window; `src/ui/` owns rendering, layouts, Fluent controls, thumbnails, and drag/drop.
- `src/fs/` and `src/ops/` provide directory snapshots and asynchronous operations.
- `src/index/`, `src/preview_host/`, and `src/shell_host/` isolate search, previews, and Shell COM; protocols live in `src/ipc/`.
- `src/bench/` contains benchmarks/tests, `assets/` contains SVGs, and `bench_data/` contains generated fixtures.

Keep implementation inside the owning module. Do not move blocking filesystem, preview-provider, or Shell work onto the UI thread.

Split features into focused `.h/.cpp` files instead of growing unrelated logic in `app_main.cpp` or another catch-all. Reuse existing models, helpers, controls, layout primitives, and tokens. Keep interaction, spacing, colors, states, and naming consistent.

## Build, Test, and Development Commands

Use an x64 Visual Studio developer prompt with MSVC, CMake 3.25+, and Ninja:

以下为按需选择的命令，不是每次任务必须依次执行的清单：

```powershell
# 默认：仅构建受影响的目标，此处以主程序为例
cmake --build build --target pulse
# 仅涉及对应模块时运行；必要时先构建对应测试目标
.\build\pulse_preview_test.exe
.\build\pulse_ops_test.exe
# 仅确有全量构建需求时使用
.\build_release.bat
```

`build_release.bat` builds all Release targets; build one target during iteration. For the optional headless suite, configure with `-DPULSE_WITH_SELFTEST=ON`, then run `pulse.exe --selftest` and explicitly wait for it to exit.

`build_installer.bat` builds Release and compiles `dist\PulseSetup-<version>.exe` (Inno Setup 6, `winget install JRSoftware.InnoSetup`); the script is `installer/PulseSetup.iss`. The installer requires admin, optionally registers and starts the `PulseIndex` service for full-disk MFT/USN indexing, and removes it on uninstall. Crash logs live in `%LOCALAPPDATA%\Pulse` (`pulse_crash.log`, `pulse_shell_host.log`) so installed copies under Program Files can write them.

## Coding Style & Naming Conventions

Use four-space indentation, same-line braces, and comments only for non-obvious behavior. Use `PascalCase` for types/functions, `snake_case` for data, and lowercase namespaces. Prefer RAII, immutable values, generation cancellation, and existing helpers. `/W4`, `/permissive-`, `/utf-8`, and C++20 are enabled; new warnings are defects.

## Testing Guidelines

Tests are custom `[PASS]/[FAIL]` executables. Put preview cases in `src/bench/preview_test_main.cpp`, operation cases in `src/bench/ops_test_main.cpp`, and model cases in `src/app/selftest_1b2.cpp`. Cover success, cancellation, stale results, UNC/long paths, and cleanup only as relevant to the change. Keep fixtures in `bench_data/`; use isolated fixtures and do not modify the user's files or preferences for testing.

新增回归用例应复现本次 bug 或验证关键边界，不为低风险改动机械增加测试。测试失败时保留错误信息，区分本次回归、既有问题和环境限制；未经验证不推断根因。不为得到通过结果擅自删除或弱化测试；用户明确要求删除时，同时清理仅为该测试服务的代码，避免留下依赖旧数据的虚假通过。

## Production-Ready Verification

默认按本次改动的影响范围验证，禁止每次修复新 bug 都重跑全部历史测试：

- 只构建受影响的目标，只运行与本次 bug 及其直接依赖相关的测试；不默认运行 `build_release.bat`、完整 `pulse.exe --selftest` 或所有测试程序。
- 优先使用测试筛选或独立测试目标。相关用例无法单独运行时，使用最小可行的定向验证并说明限制，不因测试打包在一起就自动运行完整套件。
- 只有公共底层改动有证据影响多个模块、影响范围经定向检查后仍无法可靠界定，或用户明确要求时，才扩大测试范围；运行前说明具体原因和拟增加的检查，优先扩到受影响模块，不直接跳到完整回归。完整回归不是每次修复的完成条件。
- 相关检查通过后停止验证；仅在后续改动、新失败或未解决的相关风险出现时重跑受影响的检查。不因无关历史失败自动扩展修复范围。
- 纯文档或规则修改只检查内容和差异，不构建、不运行功能测试。

Verify both function and design within this scope; do not stop at a successful compile for functional changes. Fix regressions caused by the current change and repeat the affected checks. Exercise loading, empty, error, cancellation, rapid-switching, theme, DPI, and narrow-layout states only when relevant. Do not approve visible UI changes with clipping, overlap, inconsistent controls, or unexplained fallback behavior. Report the checks actually run and any relevant verification limits; do not describe skipped or deleted tests as passed.

## Commit & Pull Request Guidelines

Use imperative, scoped commit subjects such as `preview: decode UTF-16 BE text`. When a pull request is requested, explain behavior and risk, list actual test results, and link issues when available. For visible UI changes, include before/after screenshots when capture is available; otherwise disclose the visual verification limit. Do not expand testing solely to fill a PR template.
