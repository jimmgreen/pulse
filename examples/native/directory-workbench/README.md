# Directory workbench — public native SDK business example

This is a usable directory workbench, implemented in one independently built C++20 DLL. It composes the existing native Grid, Stack, Text, Button, TextInput, Select, SidebarSection and DataTable controls. It contains no JavaScript, private Pulse header, custom renderer, filesystem enumerator, directory watcher or background thread.

The DLL owns classification, filtering, sorting, bounded card layout, snapshot-to-table conversion and settings orchestration. Pulse owns directory I/O, immutable snapshots, watch workers, native controls, normal entry-open routing and package-owned settings.

## Build and package

From an x64 Visual Studio developer prompt:

```bat
cmake -S examples/native/directory-workbench -B build-extensions/native-workbench-author-01/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-extensions/native-workbench-author-01/build --target pulse_directory_workbench
```

Output: `build-extensions/native-workbench-author-01/build/pulse_directory_workbench.dll`.
Set `-DPULSE_SDK_INCLUDE=<public-sdk>/include` to build against a separately delivered SDK. The target uses only public headers and static CRT, with C++20 and MSVC `/W4 /permissive- /utf-8`. The package entry is the DLL and the supplied `manifest.json`; use the public native package tooling, without adding a host command for this business example.

The host must provide v1 `pulse.ui`, `pulse.ui.model`, `pulse.ui.data`, `pulse.ui.events`, `pulse.ui.mount`, `pulse.context`, `pulse.directories` and `pulse.resources`. Requested resource permissions are in the manifest: context observation/events, directory read/watch, normal open dispatch and own settings.

## Real directory workflow

Activation starts as a window and immediately opens the current `pulse.context` directory through `pulse.directories.open`. Context events update a summary; **Pulse context** explicitly follows a later Pulse navigation without overwriting an author-edited path. The path input accepts the host's absolute drive/UNC format, within the shared UI's 1024-byte state value limit. **Up** computes only a lexical parent; all directory resolution and enumeration stay in the host service.

**Cards** classifies folders, images, documents and other files inside this DLL. Each kind shows the first two matching entries in the chosen name order, so at most eight cards appear. The outer and inner native Grids collapse columns in narrow layouts. Card names/details wrap using the existing native Text control, and each card has a normal native **Open** button. **Details table** shows the same filtered and sorted entries in the real shared DataTable; **Next 256** appends another page. Table selection survives view switching because the tree and data binding are retained.

The service snapshot can contain up to 65536 entries; this example loads at most its first 4096 into DLL memory. The count explicitly reports matching / loaded / snapshot totals and the eight-card limit. Filter and sort cover the loaded subset. The table sends at most 256 rows and 58 KiB of row strings per page, shrinking a page when names require it, without skipping the next row. This is not 65536 rendered cards. Names use ASCII case-insensitive substring filtering and bytewise UTF-8 ordering after ASCII folding; this does not claim locale-natural or Unicode case-folded sorting.

**Refresh** makes a new immutable snapshot. Watch READY always schedules a refresh to close the watch-arming gap. READY/INVALIDATED events during an operation coalesce through one dirty flag. Rapid A→B opening cancels pending A requests, removes A's watch, closes A's handle, clears selection and rejects late replies by request/directory/snapshot/watch identity. A canceled UI data request retires its publish token; a filter change does not cancel useful host enumeration. **Close / cancel** cancels enumeration and entry-open requests, removes the watch, closes the handle and clears the real table by unbind/rebind, leaving `status=cancelled` until the next open.

Cards and **Open selected** resolve the current row ID back to the service's directory/snapshot/entry tuple, then call `open_entry`. A folder uses Pulse's normal navigation. File success is reported as **route accepted**, explicitly not application launch, execution or document completion. Refresh invalidates old row IDs.

## Settings and mounts

The DLL reads package-owned settings at startup and restores `workbench.view` (`cards` / `table`) and `workbench.sort` (`nameAsc` / `nameDesc`). `workbench.path` stores the last explicitly opened path. Startup still follows the current Pulse context; it preserves an existing saved path until **Restore saved path** or another explicit directory action. The first run can save its initial context path. After an explicit open, a view change or a sort change, the DLL saves the latest choices automatically; **Save choices** is also available. Wait for the `settings` state to start with `saved` before testing close/reopen persistence.

Every `settings_set` uses the version returned by the preceding `settings_get`/`settings_set`. The three keys are separate CAS writes, not an atomic three-key transaction. Unchanged choice sets are not rewritten. A `version_conflict` stops the sequence and automatic retries; it does not overwrite the competing version. **Reload settings** explicitly reloads authoritative settings and enables a later save. A conflict after an earlier key succeeded can leave that earlier key committed; no all-or-nothing claim is made.

**Window / Sidebar / Workspace** move the existing panel using `pulse.ui.mount`. The default is Window; no host dimensions are assumed during activation. At the host's normal sidebar width, constrained horizontal stacks keep the three mount buttons in one row and show the filter, view choice and actual data above secondary controls. Card groups still use responsive Grid flow. The collapsed native **Directory and settings** section contains the path editor, navigation, settings actions and diagnostic states; expand it to use those actions. Actual placement, DIP size, DPI and theme appear in `mount`, so a host fallback is observable. A busy slot reports rejection in `route` while the existing panel remains usable. There is one tree viewport plus the table's own row viewport; the outer panel adds no redundant scrollbar.

Deactivation cancels resource requests, directory operations, pending entry opens, context/state/mount subscriptions, watch, directory handle, data request/provider/binding and every native command subscription, then closes the panel. All service callbacks copy borrowed data before returning. The DLL creates no threads and waits for no filesystem I/O.

## Stable probe contract

Node IDs are stable and also serve as command IDs for buttons unless stated otherwise:

| Node / command | Meaning |
|---|---|
| `advanced` | Native SidebarSection; initially collapsed, expands directory/settings controls |
| `path-input` | TextInput bound to state `directory` |
| `open-directory`, `refresh`, `up`, `use-context` | Open typed path, refresh, lexical parent, follow current context |
| `restore-path`, `cancel-load` | Open persisted path; cancel all directory work and clear rows |
| `view-cards`, `view-table` | Switch visible native business view; state `view` is `cards` or `table` |
| `filter-input` | TextInput bound to `filter`; real control commit automatically requests filtered table data |
| `sort-input` | Native Select bound to **`sort-index`**, canonical index `0` = A→Z, `1` = Z→A; DLL maps it to `sort=nameAsc/nameDesc` |
| `cards` | Outer native Grid; `kind-group-0..3` / `kind-0..3` hold four classified grids |
| `card-tile-0..7`, `card-text-0..7` | Native card stacks and wrapped filename/detail text |
| `card-0..7` | Native card Open buttons/commands: 0–1 folders, 2–3 images, 4–5 documents, 6–7 other files |
| `items` | Real provider-bound DataTable; selection state `selected` holds a current snapshot row ID |
| `open-selected`, `next` | Open the selected current entry; append the next table page |
| `mount-window`, `mount-sidebar`, `mount-workspace` | Move the same panel to the corresponding host placement |
| `save-settings`, `reload-settings` | Save current choices with CAS; explicitly reload settings/version |
| `count` | Matching entry and bounded card counts; snapshots above 4096 disclose the loaded subset |
| `status`, `error` | Data controller states (`loading`, `ready`, `error`, or explicit `cancelled`) and error message |
| `route` | Open acceptance, retries, empty-state guidance and mount errors |
| `path`, `context`, `mount`, `settings`, `view` | Loaded path, latest Pulse context, actual mount info, CAS progress/outcome, selected view |

The UI declares exactly 32 state keys, including `card0..7` (wrapped display strings), `show0..7` (visibility booleans), `cards-visible` and `table-visible`. These remain native states; no scripting runtime or hidden fake filesystem result is involved.

For live verification, drive TextInput/Select/Button notifications through the real shared controls. Programmatically setting `sort-index` is not a USER control commit and does not simulate selecting the native dropdown. Probe screenshot cases should include populated cards and table, a narrow sidebar→Window return, empty and missing directories, typing a filter, selecting a row/opening a folder, rapid A→B, a real watch invalidation, persisted view/sort/path restoration, CAS conflict and deactivation with pending work. Isolated fixtures belong under `bench_data/`; do not alter user documents or preferences for tests.
