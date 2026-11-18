# Main integration — 2026-11-01

## Included

- Latest shell integration, native file-dialog replacement, installer and workspace UI from `codex/snapshot-shell-20261101`.
- Image-grid/thumbnail, animation, volume release, copy/delete and background preflight work from `codex/snapshot-integrated-20261101`.
- Main's AI chat/history implementation plus the uncommitted “Jump to latest” change from `build/fix48_wt`.
- Desktop worktree logo design material, the independent details-tree refresh regression, and archived start-pin investigation.

Existing main string IDs remain unchanged. New conflicting IDs were allocated without overwriting existing entries. Drive-eject completion uses a separate window message from dialog management. The assistant enable switch now works before a model service is connected.

## Preservation and exclusions

The source worktrees remain intact. Desktop, integrated and shell source snapshots are retained on `codex/snapshot-*-20261101` branches. Older Shell routing implementations superseded by the current request coordinator were not restored. Obsolete portable-release scripts, temporary root scripts, archives and performance instrumentation remain in their original worktrees rather than replacing current product code. Animation work and the inspected historical preview fixes are already represented by the newer integrated sources.

## Verification

Clean Release build: `pulse`, `pulse_setup`; no compiler warnings reported. Focused executables passed:

- `pulse_setup_options_test`
- `pulse_ai_broker_test --settings-only`
- `pulse_external_open_test`
- `pulse_file_dialog_contract_test`
- `pulse_delete_preflight_test`
- `pulse_native_delete_stream_test`
- `pulse_item_check_motion_test`
- `pulse_wall_layout_job_test`
- `pulse_details_tree_refresh_test`
- `pulse_delete_delivery_test`
- `pulse_ai_chat_store_test`
- `pulse_window_arrange_test`

The native-delete symbolic-link case was skipped because creation privilege was unavailable (1314); other cases passed. Isolated live workspace checks reported zero failures. Assistant switch hit tests passed in Pick, Setup and Connected stages. Generated localization and no-literal-Pick checks passed.

Evidence is retained in the local `bench_data/merge-main-20261101/tests` directory of the original desktop checkout. Full historical suites and system installation were not run. No remote push was performed.
