# Building a directory workbench with public native services

The complete business sample is [`examples/native/directory-workbench`](../../examples/native/directory-workbench/README.md). It is an independent C++20 DLL and manifest. It uses the public SDK without linking the host or including `src/` headers, adding host business commands, embedding JavaScript or implementing another renderer.

The sample combines eight existing public v1 interfaces:

| Interface | Responsibility |
|---|---|
| `pulse.context` | Get current Pulse directory at activation and observe later context summaries |
| `pulse.directories` | Asynchronous open/refresh/list, watch invalidation, snapshot entry routing and cancellation |
| `pulse.ui` / `pulse.ui.model` | Build and retain actual native controls and their declared states/commands |
| `pulse.ui.data` | Publish filtered/sorted immutable rows into the real DataTable and page them |
| `pulse.ui.events` | Observe native Select/filter USER commits without rebuilding the control tree |
| `pulse.ui.mount` | Move the same panel among Window, Sidebar and Workspace; observe actual size/theme/DPI |
| `pulse.resources` | Restore and persist package-owned view/sort/path with explicit expected versions |

All entry names, sizes and opaque entry IDs originate in the directory service. The DLL copies each immutable snapshot page and classifies it into Folders, Images, Documents or Other files. It owns the name filter, ordering, type buckets and Grid layout. This is a business composition example rather than a synthetic control gallery or a host-provided directory-workbench shortcut.

Two distinct views share the same current snapshot. Grouped cards contain native grids and wrapped text with normal Open buttons; the details view is a provider-bound native DataTable. The sample loads at most 4096 entries, displays at most eight cards (two per kind), and pages table rows in batches of at most 256. The count shows matching entries and bounded cards; snapshots above 4096 explicitly disclose the subset and snapshot total. Filtering uses ASCII case folding; sorting compares the resulting UTF-8 bytes. No full-Unicode natural-sort claim is made.

Opening another path invalidates the previous requests, watch, handle and selected row generation. READY watch delivery schedules a refresh; an invalidation burst coalesces while a refresh/list chain is running. Snapshot/entry IDs stay tied to their service directory and snapshot. Both card Open and table Open selected use `open_entry`, preserving Pulse's folder-navigation and normal-file-open routing. File success means route dispatch accepted, not application launch or execution completed.

The panel starts in Window so activation does not depend on a usable host workspace size. Mount buttons move the existing controls. Constrained horizontal stacks keep the mount, view and filter controls compact at normal sidebar width, so the initial viewport includes the real table header and multiple rows. A collapsed native Directory and settings section retains the path, navigation and settings controls below the data. Card groups still use responsive Grid flow. `mount` reports actual placement, DIP geometry, DPI and theme. The initial path follows the current Pulse context, while persisted view/sort restore automatically and Restore saved path reopens the last explicit path. Each settings write uses the latest returned CAS version; a conflict stops retries until explicit reload. Three separate key writes are not advertised as an atomic transaction.

The sample's README includes independent build commands, exact dependencies, stable control/command/state IDs and a live-probe checklist. Public-only fake-host verification can validate model conversion and asynchronous orchestration, but does not substitute for real filesystem/watch, native input, persistence and rendered-layout checks in the actual host. In particular, use native Select commit notifications rather than pretending a programmatic state write is a user dropdown selection.

On deactivate, the sample removes state/context/mount/command subscriptions, cancels directory and settings requests, removes watches, releases directory handles, unbinds/unregisters the data provider and closes the panel. It creates no threads, never enumerates or watches the filesystem itself, and never blocks the UI waiting for filesystem I/O.
