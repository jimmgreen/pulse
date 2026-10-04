# Issue #93 verification: type-ahead jumps to a typed letter

- Issue: https://github.com/jimmgreen/pulse/issues/93
- Branch: `93-type-ahead` (baseline `f2c3c95`, v1.0.52)
- Plan: `docs/issue-93-type-ahead-plan.md`

## What changed

Typing a printable character with the file list focused selects — and scrolls to —
the next entry whose name starts with that character, in the order the entries are
currently displayed. Pressing the same key again advances to the next match and
wraps at the end of the list. This is the File Explorer behavior the issue asks for,
including the follow-up request "每按一次按键按排列顺序从上往下依次向下跳".

Matching follows the **view** order, not the snapshot order: rows are walked by view
index and resolved through `PaneViewModel::SourceIndex`, so sorting, grouping and an
active filter are all respected.

Scope is Phase 1 only: single characters, no accumulated prefix buffer, no visual
indicator. Phase 2 items in the plan (multi-character accumulation, on-screen
feedback) are product decisions and were left for the maintainer.

## Implementation

- `src/ui/type_ahead.h` (new): `NextPrefixMatch(count, from, name_at, prefix)` — the
  shared matcher. It starts strictly after `from`, wraps after a full lap and returns
  -1 when nothing matches. `name_at` hands back a `std::wstring_view`, so no row is
  copied.
- `src/app/type_ahead.h` / `.cpp` (new): `HandleTypeAheadChar(AppState&, wchar_t)`.
- `src/app/app_main.cpp`: one `WM_CHAR` case ahead of `WM_KEYDOWN`. `WM_SYSCHAR`
  (Alt+letter) keeps its own case and is untouched.
- `src/ui/folder_picker_model.cpp`: `PickerTypeAhead` now delegates to the shared
  matcher. Behavior is character-for-character identical; the existing
  `pulse_dialogs_test` case was kept as the guard.
- `src/bench/type_ahead_test.cpp` (new) plus the `pulse_type_ahead_test` CMake
  target; the target was added to `$testNames` in `scripts/build_release_ci.ps1`
  (the CI list, not just CMake).

### Why `WM_CHAR` and not `WM_KEYDOWN`

`WM_KEYDOWN` carries virtual key codes: letters are always uppercase, punctuation is
OEM-coded and layout-dependent, and an IME composition produces nothing useful.
`WM_CHAR` carries the translated character, so a committed Chinese character works
and matches Pulse's own zh-CN / zh-TW localizations. The folder picker already used
`WM_CHAR`.

### Guards (any one of these returns false and lets the default handler run)

- `ch < 0x20` or `ch == 0x7F` — control characters, including the `0x01` that
  Ctrl+A produces.
- Ctrl or Alt held — those are shortcuts.
- Space — `VK_SPACE` is bound to quick preview in `HandleKeyDown`, and
  `TranslateMessage` posts a `WM_CHAR` even after the keydown was handled. Without
  this guard one press would both toggle the preview and move the selection.
- Filter editor, address editor, inline rename, tag rename, or an open Fluent menu.
- Settings tab.

A character with **no** match is still consumed (`return true`) so
`DefWindowProcW` cannot beep; the selection is left alone.

### Performance

The scan reads `(*tab->snapshot)[src].name` directly instead of `Tab::EntryAt`,
which returns `fs::DirEntry` **by value** and would copy several `std::wstring`s per
row on a 100k-row view. Only the content-results branch needs a value source; it
moves the name into a reusable scratch buffer that outlives each comparison, so the
`wstring_view` handed to the matcher never dangles.

Content search rows are read through `ContentResultStore::Get`. Rows whose spool page
is not resident yet simply do not match, so a large in-flight content search degrades
to "no jump" rather than blocking the UI thread.

### Deviation from the plan

The plan suggested routing the content-search case through
`DeferContentSelection`. That helper resolves the **current selection** into rows for
bulk operations; type-ahead only moves focus and needs no row resolution, and it can
trigger a `RefreshContentResults`. Arrow-key navigation moves focus the same direct
way, so type-ahead does too.

## Verification

Run:

- `pulse_type_ahead_test.exe` — 12 checks, all pass: first press, repeat-to-advance,
  wrap, case-insensitivity, single match, no match, empty prefix, Chinese first
  character, empty entry name, empty list, two-character prefix, over-long prefix.
- `pulse_dialogs_test.exe` — 19 checks, all pass (the picker delegation guard).
- `cmake --build build-ci --target pulse` — clean, no new warnings under `/W4`.

Not run: the in-app checks in section 6.2 of the plan (view modes, grouped/sorted
order, filtered view, rename focus, IME). Those need a live window; only the pure
matcher is covered by an executable here, and `pulse.exe --selftest` was deliberately
not run in full per the repository's scoped-verification rule.
