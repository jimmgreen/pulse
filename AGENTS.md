# Repository Guidelines

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

```powershell
.\build_release.bat
cmake --build build --target pulse
.\build\pulse_preview_test.exe
.\build\pulse_ops_test.exe
```

`build_release.bat` builds all Release targets; build one target during iteration. For the optional headless suite, configure with `-DPULSE_WITH_SELFTEST=ON`, then run `pulse.exe --selftest` and explicitly wait for it to exit.

## Coding Style & Naming Conventions

Use four-space indentation, same-line braces, and comments only for non-obvious behavior. Use `PascalCase` for types/functions, `snake_case` for data, and lowercase namespaces. Prefer RAII, immutable values, generation cancellation, and existing helpers. `/W4`, `/permissive-`, `/utf-8`, and C++20 are enabled; new warnings are defects.

## Testing Guidelines

Tests are custom `[PASS]/[FAIL]` executables. Put preview cases in `src/bench/preview_test_main.cpp`, operation cases in `ops_test_main.cpp`, and model cases in `src/app/selftest_1b2.cpp`. Cover success, cancellation, stale results, UNC/long paths, and cleanup. Keep fixtures in `bench_data/`.

## Production-Ready Verification

After implementation, verify both function and design; do not stop at a successful compile. Iterate through build, focused tests, full regression tests, and appropriate visual/performance checks. Fix every discovered regression, then repeat the affected checks until the result is production-ready. Exercise loading, empty, error, cancellation, rapid-switching, theme, DPI, and narrow-layout states when relevant. Do not approve visible UI changes with clipping, overlap, inconsistent controls, or unexplained fallback behavior. Record any check that cannot be run and why.

## Commit & Pull Request Guidelines

No Git history is included, so use imperative, scoped subjects such as `preview: decode UTF-16 BE text`. Pull requests should explain behavior and risk, list test results, link issues, and include before/after screenshots for UI changes. Disclose unrun checks or environment failures.
