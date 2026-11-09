# Native UI model v1

Query `pulse.ui.model` version 1 for `PulseUiModelV1`. Keep the existing `pulse.ui`
version 1 table for typed state reads/writes, subscription removal and closing.
Both tables address the same owner-bound panel and use the same panel IDs.
This is a trusted, in-process native interface, not an untrusted-code sandbox.

`PulseUiModelNode` maps all 41 existing `UiNodeType` values. It adds no replacement
widgets or independent visual design. Its named optional fields retain the
existing model's semantics: `kind`, `role` and `status` are validated for the
specific node type. `columns` is the Grid integer, while `data_columns` describes
DataTable columns. Set `size = sizeof(record)` on each sized record. Zero initialize
records, then set optional fields' `present` to 1 when supplying them; omitted
fields retain the host defaults. A nonzero payload on an absent field is rejected.

Node children, menu items, rows, columns, validation rules and local state actions
are typed borrowed records. Submission copies every value before returning.
It constructs an internal in-memory DOM solely to reuse the existing `ParseUiTree`
semantic validator; it neither serializes nor parses JSON text. This is not a
zero-copy interface. Frequent `set_states` calls continue to use the direct,
transactional typed state path.

Limits apply before copying recursive structures: 128 UI nodes, depth 8; 128 total
menu items, depth 4; 4096 total data rows, depth 8; 65536 total UTF-8 string bytes.
Repeated structural pointers, cycles, invalid UTF-8/NUL, duplicate IDs and invalid
state/command references are rejected. Individual field, array and state limits
also follow the existing model. Failed conversion leaves the previous panel and
its state intact. `update` preserves current values of retained state keys and
keeps subscriptions whose declared command IDs remain present.

Declare native callback IDs in `commands`; register callbacks with
`subscribe_command`. The callback string is the **command ID**, including commands
used by menus, tab/info-bar close actions, filter/sort controls and data rows. Local
`actions` perform validated state updates and input validation inside the host;
they do not call native callbacks. This interface does not execute manifest host
capabilities implicitly. Unsubscribe and panel close synchronously invalidate
future callbacks. A callback may update or close its own panel; module unloading
must still wait until the callback returns (`UiAdapter::InCallback()` on the host).

Not connected in this version: `data_source`, `data_operation`, and `context.*`
binding resolution. They explicitly return `PULSE_ERROR_UNAVAILABLE`. Static
virtualList/tree/dataTable rows and local `state.*` bindings are supported. This
boundary prevents displaying fake provider results or silently ignoring requests.

Dynamic data is available separately through the additive `pulse.ui.data` V1
table: bind a registered native provider to an existing empty virtualList, tree
or dataTable node. It reuses those same controls without changing this model's
ABI or enabling its reserved fields. See `docs/extensions-native-data.md` for
typed pages, revisions, filtering/sorting requests, cancellation and lifetime.

The model test covers conversion and rejection cases across 41 node types. That
coverage does not certify every control's interactive behavior, theme, DPI,
accessibility or screenshot layout. The initial `pulse.ui` seven-type real-window
test remains separate; broader native model UI acceptance must be recorded from
actual host interactions and captures.

The catalog test accepts `OUTPUT --type titleBarButton` for a focused rerun. A
filtered run creates, updates and captures that same control in light/dark themes.
The title-bar-button fixture explicitly uses `role = "close"` and a declared
native `run` callback: clicking it emits that callback, not a privileged window
close operation. In a full catalog run, `update` replaces the body with the next
control type without changing the panel's original title; the dark PNG filename
identifies the updated body, so its title can intentionally name the preceding case.
