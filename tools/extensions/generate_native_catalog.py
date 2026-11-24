"""Refresh native interface discovery from public headers and shared constraints."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


FROZEN_INTERFACES = (
    "pulse.files",
    "pulse.resources",
    "pulse.dispatch",
    "pulse.directories",
    "pulse.context",
    "pulse.fileops",
    "pulse.commands",
    "pulse.preview",
    "pulse.tasks",
    "pulse.staging",
    "pulse.settings.pages",
    "pulse.dialogs",
)

EXPERIMENTAL_INTERFACES = (
    "pulse.ui.model",
    "pulse.ui.events",
    "pulse.ui.data",
    "pulse.ui.mount",
    "pulse.icons",
    "pulse.diagnostics",
    "pulse.pickers",
    "pulse.workbench",
    "pulse.tray",
    "pulse.search",
    "pulse.shell",
    "pulse.associations",
    "pulse.vfs",
    "pulse.columns",
    "pulse.statusbar",
    "pulse.fileevents",
)

STABILITY_TIERS = {
    **{name: "frozen" for name in FROZEN_INTERFACES},
    **{name: "experimental" for name in EXPERIMENTAL_INTERFACES},
}


def header_interface(filename, name, table_name, interface_macro, version_macro, operations, version=1):
    header = (ROOT / "sdk/native/include/pulse" / filename).read_bytes().replace(b"\r\n", b"\n")
    text = header.decode("utf-8")
    table = re.search(r"typedef struct " + table_name + r"\s*\{(.*?)\}\s*" + table_name + ";", text, re.S)
    if (not table or f'#define {interface_macro} "{name}"' not in text or
            f"#define {version_macro} {version}u" not in text or
            "#pragma pack(push, 8)" not in text or "#pragma pack(pop)" not in text):
        raise ValueError("Unexpected public ABI: " + name)
    actual_operations = re.findall(r"\(\s*PULSE_CALL\s*\*\s*(\w+)\s*\)", table.group(1))
    if actual_operations != operations:
        raise ValueError("Unexpected public operations: " + name)
    return {"name": name, "version": version, "header": "include/pulse/" + filename,
            "status": "developer-preview", "operations": actual_operations,
            "headerContract": {"sha256": hashlib.sha256(header).hexdigest(), "text": text}}


def outputs():
    path = ROOT / "sdk/native/api-catalog.json"
    catalog = json.loads(path.read_text(encoding="utf-8"))
    header = (ROOT / "sdk/native/include/pulse/ui_model.h").read_bytes().replace(b"\r\n", b"\n")
    text = header.decode("utf-8")
    node_types = [{"name": name, "value": int(value)} for name, value in
                  re.findall(r"(PULSE_UI_MODEL_[A-Z_]+)\s*=\s*(\d+)", text)
                  if name != "PULSE_UI_MODEL_COUNT"]
    if len(node_types) != 39 or "#pragma pack(push, 8)" not in text or "#pragma pack(pop)" not in text:
        raise ValueError("Unexpected public UI model ABI")
    schema = json.loads((ROOT / "schemas/extensions/ui-model.schema.json").read_text(encoding="utf-8"))
    public_names = {''.join(part.title() if i else part.lower() for i, part in enumerate(item['name'].removeprefix('PULSE_UI_MODEL_').split('_'))) for item in node_types}
    schema['$defs']['uiNode']['oneOf'] = [variant for variant in schema['$defs']['uiNode']['oneOf'] if variant['properties']['type']['const'] in public_names]
    schema['$defs']['uiNode']['description'] = 'General-purpose Pulse controls. Host additionally validates UTF-8 budgets, IDs, state/command references and layout bounds. Shell controls remain internal.'
    constraints = {key: copy.deepcopy(value) for key, value in schema.items() if key not in ("$schema", "$defs")}
    definitions = {}

    def visit(value):
        if isinstance(value, dict):
            reference = value.get("$ref", "")
            if reference.startswith("#/$defs/"):
                key = reference.removeprefix("#/$defs/")
                if key not in definitions:
                    definitions[key] = copy.deepcopy(schema["$defs"][key])
                    visit(definitions[key])
            for item in value.values():
                visit(item)
        elif isinstance(value, list):
            for item in value:
                visit(item)
    visit(constraints)
    constraints["$defs"] = definitions
    constraints["$schema"] = "https://json-schema.org/draft/2020-12/schema"
    constraints["$comment"] = "Shared UiTree semantic constraints, not a JSON/native execution transport. Native model data_source/context bindings/data operations remain UNAVAILABLE. pulse.ui.data separately binds providers to existing empty model data nodes; see api-catalog."
    model = {
        "name": "pulse.ui.model", "version": 1, "header": "include/pulse/ui_model.h",
        "status": "developer-preview", "operations": ["create", "update", "subscribe_command", "set_states", "get_state", "unsubscribe", "close"],
        "sharedOperations": "All UI creation, state and lifetime operations use this single table",
        "continuousSlider": "Optional kind=continuous uses options=[min,max] numeric strings and a real floating-point state, not stops/index. Text is the label, helper is the unit, command fires after ordered state delivery on release/capture loss/keyboard completion; Escape restores drag-start state. Existing kind-absent sliders stay stepped. Requires this host build; C structs and numeric enum values are unchanged. Initial tree states max128; set_states and action update batches max64.",
        "nodeTypes": node_types,
        "headerContract": {"sha256": hashlib.sha256(header).hexdigest(), "text": text},
        "constraintSchema": "ui-model.schema.json",
        "fieldMapping": "C snake_case fields map to existing UiTree camelCase; data_columns maps DataTable columns; optional present=0 means absent. Header declares every field and borrowed array type.",
        "limits": {"utf8Bytes": 262144, "nodes": 384, "depth": 12, "states": 128, "stateBatch": 64, "actions": 64, "stringBytes": 4096, "menuItems": 128, "menuDepth": 4, "dataRows": 4096, "dataDepth": 8, "nativeCommandIds": 256},
        "copying": "All strings/arrays copied synchronously into a temporary in-memory DOM for shared validation; no JSON text serialization/parsing; not zero-copy",
        "events": "Declared native command IDs, including menu/close/filter/sort/row, use subscribe_command; callback string is command ID, not node ID. Local actions update state without native callbacks.",
        "unavailable": ["non-null data_source", "context.* bindings", "non-null action.data_operation"],
        "presentationExtras": "Optional PulseUiModelTree.extras keyed by node ID: padding (stack/grid/group, DIP 0..48), tooltip (non-container controls, <=256 UTF-8 bytes), pointer_state (canvas: declared state receiving \"move X Y\", \"leave\", \"click X Y\" in viewBox units before the command callback). A tree without extras may use size PULSE_UI_MODEL_TREE_SIZE_V1 so earlier hosts accept it; node record layout is unchanged.",
        "typography": "text role: body, caption, section, bodyStrong, subtitle, title, code; text kind (tone): default, secondary, accent, success, warning, danger.",
        "validationStatus": "39 general-purpose types represented; does not assert every interaction or theme/DPI capture passed"
    }
    data_header = (ROOT / "sdk/native/include/pulse/ui_data.h").read_bytes().replace(b"\r\n", b"\n")
    data_text = data_header.decode("utf-8")
    data_table = re.search(r"typedef struct PulseUiDataV1 \{(.*?)\} PulseUiDataV1;", data_text, re.S)
    if (not data_table or '#define PULSE_UI_DATA_INTERFACE "pulse.ui.data"' not in data_text or
            "#define PULSE_UI_DATA_VERSION 1u" not in data_text or
            "#pragma pack(push, 8)" not in data_text or "#pragma pack(pop)" not in data_text):
        raise ValueError("Unexpected public UI data ABI")
    data = {
        "name": "pulse.ui.data", "version": 1, "header": "include/pulse/ui_data.h",
        "status": "developer-preview",
        "operations": re.findall(r"\(PULSE_CALL \*(\w+)\)", data_table.group(1)),
        "requestOperations": {name: int(value) for name, value in re.findall(r"(PULSE_UI_DATA_(?:REFRESH|NEXT_PAGE|CANCEL))\s*=\s*(\d+)", data_text)},
        "pageStatuses": {name: int(value) for name, value in re.findall(r"(PULSE_UI_DATA_(?:LOADING|READY|ERROR))\s*=\s*(\d+)", data_text)},
        "pageModes": {name: int(value) for name, value in re.findall(r"(PULSE_UI_DATA_(?:SNAPSHOT|DELTA))\s*=\s*(\d+)", data_text)},
        "headerContract": {"sha256": hashlib.sha256(data_header).hexdigest(), "text": data_text},
        "targets": ["virtualList", "tree", "dataTable"],
        "binding": "Bind an existing empty pulse.ui.model data node with initially empty selection. Configured state keys must exist, have distinct roles and not overlap selection; initial sort is nameAsc/nameDesc. Uses the shared renderer/data controller. bind does not start a request; successful model update requires rebind.",
        "thread": "publish may run on any thread; all other operations and request/cancel callbacks run on owner UI thread",
        "copying": "Request views expire at callback return. publish synchronously copies all borrowed page strings/arrays and never invokes plugin code. PULSE_OK means copied/queued, not displayed.",
        "requests": "REFRESH starts a live stream. Filter/sort changes cancel/restart it. NEXT_PAGE cancels the previous stream and requests its cursor; revision must match.",
        "snapshot": "SNAPSHOT publishes at most page_size top-level rows. READY requires a nonempty revision; LOADING/ERROR carry no rows/cursor/delta and ERROR requires error text. Row semantics match PulseUiModelRow; commands must be declared native commands.",
        "delta": "READY DELTA requires an established snapshot and base_revision matching displayed revision. Upsert stable row IDs and remove top-level IDs; descendant tree changes replace their top-level ancestor. Not allowed for a next-page stream; clears pagination.",
        "limits": {"providersPerInstance": 16, "bindingsPerInstance": 16, "pageSizeMin": 1, "pageSizeMax": 256, "recursiveRowsPerPage": 4096, "displayedRows": 4096, "rowDepth": 8, "cellsPerRow": 16, "pageUtf8Bytes": 65536, "rowIdUtf8Bytes": 64, "revisionUtf8Bytes": 128, "cursorUtf8Bytes": 256, "errorUtf8Bytes": 1024, "removedIdsPerDelta": 256, "progressMin": 0, "progressMax": 100, "inboxPages": 64},
        "state": "Configured keys expose initial empty/loading/error, refresh, pagination and deltas. Stale revisions and row/inbox overflow become error states; inbox overflow fails the stream until refresh.",
        "cancellation": "Cancel invalidates a request before its callback; later publish returns STATE. Successful model update, close, stop, unregister and unbind cancel requests. Unbind clears rows and selection. Providers remain until unregister or stop; all handles are plugin-instance scoped.",
        "shutdown": "Callbacks return promptly and only signal worker cancellation; plugin owns/joins workers before destroy returns. Host context lives through destroy, not after it. No exceptions cross the ABI.",
        "unavailable": ["pulse.ui.model non-null data_source", "context.* bindings", "non-null action.data_operation"],
        "documentation": "sdk/native/README.md",
        "validationStatus": "Published author contract; host functional/UI verification requires separate evidence and is not implied by this catalog"
    }
    if data["operations"] != ["register_provider", "unregister_provider", "bind", "unbind", "request", "publish"]:
        raise ValueError("Unexpected public UI data operations")
    directories = header_interface("directories.h", "pulse.directories", "PulseDirectoriesV1",
                                   "PULSE_DIRECTORIES_INTERFACE_NAME", "PULSE_DIRECTORIES_VERSION_1",
                                   ["open", "refresh", "list", "open_entry", "watch", "unwatch", "close", "cancel"])
    directories.update({
        "thread": "All calls and callbacks run on owner UI thread; filesystem enumeration and watch I/O run on host workers",
        "resourceDeclarationPolicy": "Installation disclosure only; not a per-call permission gate",
        "resourceDeclarations": {"open": ["files.readDirectory"], "refresh": ["files.readDirectory"], "list": ["files.readDirectory"], "open_entry": ["files.readDirectory", "files.open"], "watch": ["files.readDirectory", "files.watchDirectory"]},
        "requestOperations": {name: int(value) for name, value in re.findall(r"(PULSE_DIRECTORY_(?:OPEN|REFRESH|LIST|OPEN_ENTRY))\s*=\s*(\d+)u?", directories["headerContract"]["text"])},
        "changeKinds": {name: int(value) for name, value in re.findall(r"(PULSE_DIRECTORY_(?:WATCH_READY|INVALIDATED|WATCH_ERROR))\s*=\s*(\d+)u?", directories["headerContract"]["text"])},
        "entryFlags": {name: int(value) for name, value in re.findall(r"(PULSE_DIRECTORY_ENTRY_(?:FOLDER|REPARSE))\s*=\s*(\d+)u?", directories["headerContract"]["text"])},
        "paths": "Absolute drive/UNC paths only; device, virtual and relative paths are rejected",
        "limits": {"directoriesPerAttachment": 4, "pendingRequestsPerAttachment": 32, "watchesPerAttachment": 8, "snapshotRows": 65536, "snapshotNamesUtf8Bytes": 16 * 1024 * 1024, "pageLimitMin": 1, "pageLimitMax": 256},
        "snapshots": "Immutable, never silently truncated. Successful refresh replaces the snapshot; failure preserves it. Snapshot and entry IDs are owner/directory/snapshot bound, not persistent file IDs. list uses explicit offset against the requested snapshot; next_offset UINT32_MAX marks exhaustion.",
        "callbacks": "PULSE_OK accepts work; completion callbacks are deferred. Reply strings and row arrays are borrowed only through callback return; copy them before returning.",
        "errors": "Check immediate PulseStatus and deferred reply.status/system_error/error. Accepted work is not completed enumeration; stale snapshot/entry identities are rejected.",
        "watch": "Coalesced invalidations, not an exact change journal. After WATCH_READY refresh once, then refresh on INVALIDATED or overflow to avoid an arm gap. Sequence is notification ordering, not a lossless filesystem journal.",
        "openEntry": "Resolves a current snapshot entry and dispatches Pulse normal navigation/open routing. Success is route acceptance, not external application launch or document execution completion.",
        "cancellation": "close/cancel/unwatch immediately suppress future callbacks; an on-stack callback may finish. Stop does not wait for filesystem I/O on UI thread.",
        "shutdown": "No exceptions cross ABI. Context remains valid through plugin destroy and expires afterwards.",
        "documentation": "sdk/native/README.md",
        "validationStatus": "Author contract only; functional/UI acceptance requires separate host evidence"
    })
    context = header_interface("context.h", "pulse.context", "PulseContextV1",
                               "PULSE_CONTEXT_INTERFACE_NAME", "PULSE_CONTEXT_VERSION_1",
                               ["get", "subscribe", "unsubscribe", "get_selection"])
    context.update({
        "thread": "All calls and callbacks run on owner UI thread",
        "resourceDeclarationPolicy": "Installation disclosure only; not a per-call permission gate",
        "resourceDeclarations": {"get": ["context.read"], "subscribe": ["context.read", "context.events"]},
        "changeFlags": {name: int(value) for name, value in re.findall(r"(PULSE_CONTEXT_(?:INITIAL|NAVIGATION|SELECTION|VIEW))\s*=\s*(\d+)u?", context["headerContract"]["text"])},
        "limits": {"subscriptionsPerAttachment": 16, "coalescingTickMs": 30, "selectionEntries": 4096, "selectionUtf8Bytes": 4194304},
        "snapshot": "get/subscribe observe owner's already loaded summary without enumerating files or selections. get_selection synchronously copies loaded selection paths/names/directory flags and directory, with selection_revision; no disk access. Empty ordinary-directory selection succeeds. Loading returns STATE; unsupported views or limits return UNAVAILABLE without truncation. Loading clears selection summary. Revisions are monotonic per attachment; selection_revision exposes multi-selection changes even when count/name summary stays identical.",
        "callbacks": "get calls back synchronously and retains neither callback nor user pointer. subscribe has deferred initial delivery and coalesces changes at the 30 ms session tick; not every change is delivered. Borrowed strings and selection arrays expire at callback return; copy before starting worker work. Recursive reads are rejected.",
        "errors": "Check immediate PulseStatus; get does not retain callbacks on return. A successful subscription is observation, not an enumeration or a lossless event stream.",
        "cancellation": "unsubscribe/stop suppress future callbacks; an on-stack callback may finish",
        "shutdown": "Context lives through plugin destroy. No C++ exceptions cross ABI callbacks.",
        "unavailable": ["arbitrary context.* pulse.ui.model resolver bindings"],
        "documentation": "sdk/native/README.md",
        "validationStatus": "Author contract only; functional/UI acceptance requires separate host evidence"
    })
    fileops_v1 = header_interface("fileops.h", "pulse.fileops", "PulseFileOpsV1",
                                  "PULSE_FILEOPS_INTERFACE_NAME", "PULSE_FILEOPS_VERSION_1",
                                  ["submit", "get", "cancel"])
    fileops = header_interface("fileops.h", "pulse.fileops", "PulseFileOpsV2",
                               "PULSE_FILEOPS_INTERFACE_NAME", "PULSE_FILEOPS_VERSION_2",
                               ["submit", "get", "cancel", "submit_to_directory"], version=2)
    fileops_v3 = header_interface("fileops.h", "pulse.fileops", "PulseFileOpsV3",
                                  "PULSE_FILEOPS_INTERFACE_NAME", "PULSE_FILEOPS_VERSION_3",
                                  ["submit", "get", "cancel", "submit_to_directory", "submit_entries"], version=3)
    fileops_v4 = header_interface("fileops.h", "pulse.fileops", "PulseFileOpsV4",
                                  "PULSE_FILEOPS_INTERFACE_NAME", "PULSE_FILEOPS_VERSION_4",
                                  ["submit", "get", "cancel", "submit_to_directory", "submit_entries",
                                   "submit_directory_tree", "get_item"], version=4)
    fileops.update({
        "thread": "All calls use the owner UI thread",
        "resourceDeclarations": {"submit.transfer": ["files.transferCurrentSelection", "operations.own"],
                        "submit.delete": ["files.deleteCurrentSelection", "operations.own"],
                        "submit_to_directory": ["files.transferToDirectory", "files.readDirectory", "operations.own"],
                        "submit_entries": ["files.transferFromDirectory", "files.transferToDirectory", "files.readDirectory", "operations.own"],
                        "submit_directory_tree": ["files.transferDirectoryTree", "files.transferToDirectory", "files.readDirectory", "operations.own"],
                        "get_item": ["operations.own"],
                        "get": ["operations.own"], "cancel": ["operations.own"]},
        "operationContract": {"copy": "Current selection to the other visible pane",
                              "move": "Current selection to the other visible pane",
                              "deleteToRecycleBin": "Current ordinary-file selection; host rejects items that cannot be recycled",
                              "copyToDirectory": "Current selection to a successfully opened directory handle in the same attachment",
                              "moveToDirectory": "Current selection to a successfully opened directory handle in the same attachment",
                              "copyEntriesToDirectory": "Current immutable snapshot entry IDs from an opened directory to an opened directory in the same attachment",
                              "moveEntriesToDirectory": "Current immutable snapshot entry IDs from an opened directory to an opened directory in the same attachment",
                              "copyDirectoryTreeToDirectory": "Copy one ordinary current-snapshot directory entry recursively to a new destination directory; never replace or merge"},
        "supportedVersions": [{"version": 1, "table": "PulseFileOpsV1", "operations": fileops_v1["operations"]},
                              {"version": 2, "table": "PulseFileOpsV2", "operations": fileops["operations"]},
                              {"version": 3, "table": "PulseFileOpsV3", "operations": fileops_v3["operations"]},
                              {"version": 4, "table": "PulseFileOpsV4", "operations": fileops_v4["operations"]}],
        "destinationLifetime": "Closing a directory handle cancels preparation/confirmation before task submission; afterward cancellation uses the request ID. Refresh preserves the handle. Paths resolve at worker preparation; controlled object identity applies after preparation.",
        "confirmation": "Always uses Pulse native confirmation before mutation; plugin cannot bypass or supply confirmation",
        "progress": "get returns a synchronous task-owned snapshot; request and task IDs are attachment-owned",
        "limits": {"sources": 64, "activeRequestsPerAttachment": 32},
        "cancellation": "Cancel signals preparation/confirmation/task cancellation; completed filesystem effects are not rolled back",
        "documentation": "sdk/native/README.md",
        "validationStatus": "Author contract only; real host operation acceptance requires separate behavior evidence"
    })
    fileops["version"] = 4
    fileops["operations"] = fileops_v4["operations"]
    fileops["headerContract"] = fileops_v4["headerContract"]
    fileops["sourceSnapshot"] = "submit_entries accepts only unique ordinary non-reparse file entry IDs from the exact current snapshot; size and modified time are revalidated during worker preparation. Refresh or either handle closing before task submission cancels preparation/confirmation."
    fileops["resultLifetime"] = "Native accepted requests retain their exact task result until terminal get consumes it or the attachment closes; owner/package-bound retention is bounded by the 32-request attachment limit."
    fileops["treeTransfer"] = "submit_directory_tree accepts one ordinary folder from the exact current snapshot and copies it to a new root named after its source leaf. Limits: 128 directories, 512 files, 1 MiB of plan paths. get_item returns each file's relative path and terminal result. Directories remain visible even if a later file fails; no rollback is implied."
    mount = header_interface("ui_mount.h", "pulse.ui.mount", "PulseUiMountV1",
                             "PULSE_UI_MOUNT_INTERFACE", "PULSE_UI_MOUNT_VERSION_1",
                             ["set", "get", "subscribe", "unsubscribe"])
    mount.update({
        "thread": "All calls and callbacks run on owner UI thread",
        "placements": {"window": 0, "sidebar": 1, "workspace": 2},
        "panel": "Uses the existing pulse.ui.model panel; moving preserves the HWND, tree, state, subscriptions and data bindings.",
        "ownership": "One native occupant per owner sidebar/workspace slot, multiple independent owned windows. Busy slots fail without replacing the occupant. Closing, stopping or destroying owner releases the slot.",
        "events": "Deferred initial snapshot; size, DPI, theme, focus and placement changes coalesce per host turn. Callbacks can move, close or unsubscribe without nesting.",
        "limits": {"subscriptionsPerAttachment": 16},
        "narrowLayout": "Unavailable dock geometry falls back to an owned window; get and callbacks report actual placement.",
        "documentation": "sdk/native/README.md",
        "pending": ["native tab contributions", "arbitrary toolbar UI surfaces (command buttons use pulse.commands)", "desktop surfaces"],
    })
    commands_v1 = header_interface("commands.h", "pulse.commands", "PulseCommandsV1",
                                   "PULSE_COMMANDS_INTERFACE_NAME", "PULSE_COMMANDS_VERSION_1",
                                   ["register_command", "unregister_command"])
    commands = header_interface("commands.h", "pulse.commands", "PulseCommandsV2",
                                "PULSE_COMMANDS_INTERFACE_NAME", "PULSE_COMMANDS_VERSION_2",
                                ["register_command", "unregister_command", "update_command", "set_command_flags"],
                                version=2)
    commands.update({
        "thread": "Owner UI thread; register during plugin entry, before panel activation",
        "limits": {"commandsPerAttachment": 32, "selectionEntries": 4096, "extensions": 32, "extensionBytes": 32},
        "supportedVersions": [{"version": 1, "table": "PulseCommandsV1", "operations": commands_v1["operations"]},
                              {"version": 2, "table": "PulseCommandsV2", "operations": commands["operations"]}],
        "icons": "All 68 built-in StoreIcon IDs (0..67), custom PulseIconsV1 / contributes.icons IDs (>= 0x10000), or icon_name resolution",
        "conditions": "Selection count range, allowed file/directory/background kinds, lowercase dotted extension include/exclude list, and runtime show/hide/checked/separator/primary flags",
        "invocation": "Host rechecks conditions and captures current loaded selection before invoking; snapshot strings/arrays borrowed only through callback return",
        "shortcuts": "Owner main UI only, not global hotkeys; built-in shortcuts take precedence; editors do not dispatch plugin shortcuts; duplicate owner shortcuts rejected",
        "toolbar": "Stable native:package:command identity persists in existing toolbar_tools; menu tokens are transient; update_command and set_command_flags preserve command IDs",
        "lifecycle": "Unregister/stop invalidates tokens; IDs never reused; host waits for active callbacks before unloading",
        "documentation": "sdk/native/README.md"
    })
    abi = (ROOT / "sdk/native/include/pulse/abi.h").read_text(encoding="utf-8")
    catalog["sdkPreview"] = int(re.search(r"#define PULSE_SDK_PREVIEW (\d+)u?", abi).group(1))
    preview = header_interface("preview.h", "pulse.preview", "PulsePreviewV1",
                               "PULSE_PREVIEW_INTERFACE_NAME", "PULSE_PREVIEW_VERSION_1",
                               ["register_provider", "unregister_provider"])
    preview.update({
        "thread": "Register/unregister on owner UI thread during entry. Provider callback runs on a host worker, at most two concurrent calls per instance; synchronize shared userdata. Never call UI APIs from provider callbacks.",
        "limits": {"providersPerInstance": 16, "concurrentCallsPerInstance": 2, "extensionMinBytes": 2, "extensionMaxBytes": 32, "archiveEntries": 4096, "archivePathUtf8Bytes": 4194304, "entryPathBytes": 32768, "formatBytes": 128, "bitmapDimension": 4096, "bitmapBytes": 16777216, "modelNodes": 128, "modelDepth": 8, "modelBytes": 65536},
        "results": ["archive", "bitmap", "ui_model"],
        "resultKinds": {name: int(value) for name, value in re.findall(r"(PULSE_PREVIEW_(?:ARCHIVE|BITMAP|UI_MODEL))\s*=\s*(\d+)u?", preview["headerContract"]["text"])},
        "model": "PULSE_PREVIEW_UI_MODEL uses borrowed PulseUiModelTree* model. Host converts and copies on the provider worker; Quick Preview and Details use existing docked controls. Local state/actions are supported; command_count > 0 returns UNAVAILABLE. Limits: 128 nodes, depth 8, 64 KiB. Non-null data_source, context.* bindings and action.data_operation remain unsupported.",
        "publish": "Call request.publish on the callback worker. It synchronously copies borrowed result strings, entries, pixels and UI model. At most one successful publish per callback; returning OK without a result fails. Request and contexts expire when callback returns.",
        "bitmap": "Premultiplied BGRA, positive dimensions, stride >= width*4 and byte_count == stride*height within limits.",
        "thumbnails": "Appended PulsePreviewProvider.flags PULSE_PREVIEW_PROVIDER_THUMBNAIL (1) also routes grid thumbnails of local files for that extension to the same callback, on host thumbnail threads within the same two-call budget, with appended request.purpose PULSE_PREVIEW_PURPOSE_THUMBNAIL (1) and pixels = wanted longest edge. Only a BITMAP result is used; errors, other kinds, cancellation or no worker slot within ~2 s fall back to the system thumbnail. The host scales larger bitmaps down and caches results in memory and on disk by path, size and modified time. Descriptors sized without flags register as preview-only; unknown flag bits return ARGUMENT. C++: pulse::sdk::PreviewClient / PreviewContext, App::AddPreview(ext, fn, flags).",
        "providerFlags": {name: int(value) for name, value in re.findall(r"(PULSE_PREVIEW_PROVIDER_THUMBNAIL)\s*=\s*(\d+)u?", preview["headerContract"]["text"])},
        "purposes": {name: int(value) for name, value in re.findall(r"(PULSE_PREVIEW_PURPOSE_(?:PREVIEW|THUMBNAIL))\s*=\s*(\d+)u?", preview["headerContract"]["text"])},
        "cancellation": "Poll request.cancelled during work and return promptly. Cancellation discards results; stale selection results are not displayed. Unregister retires immediately and returns STATE while callbacks drain; retain userdata and retry on UI thread until OK or retain through destroy. Stop retains the DLL until all provider callbacks return.",
        "errors": "No exceptions cross ABI. Check publish status. Do not retain request/publish/cancel contexts, call UI APIs from workers, or free memory owned by the other side.",
        "unavailable": ["preview native command callbacks (command_count > 0)", "non-null data_source", "context.* bindings", "non-null action.data_operation"],
        "documentation": "sdk/native/README.md",
        "validationStatus": "P1.3 archive/bitmap/UI-model host acceptance recorded in implementation roadmap; extraction tasks remain separate"
    })
    tasks = header_interface("tasks.h", "pulse.tasks", "PulseTasksV1",
                             "PULSE_TASKS_INTERFACE_NAME", "PULSE_TASKS_VERSION_1",
                             ["create", "cancel", "get", "release"])
    tasks.update({
        "thread": "create/cancel/get/release and cancellation callbacks use owner UI thread; work uses host workers; progress report is any-thread and never calls host UI backend",
        "limits": {"retainedTasks": 16, "concurrentWorkers": 2, "titleUtf8Bytes": 128, "typeUtf8Bytes": 128, "errorUtf8Bytes": 4096, "currentItemUtf8Bytes": 32768},
        "progress": "Copied percent (0..100 or UINT32_MAX indeterminate), completed/total item and byte counts, current_item and error; completed counters and determinate percent are monotonic and cannot exceed totals",
        "states": ["Running", "Cancelling", "Completed", "Partial", "Failed", "Cancelled"],
        "completion": "Worker return selects an immutable terminal state; cancellation does not override explicit failure or completion. Report remains accepted during cleanup until worker returns, allowing cleanup failure diagnostics",
        "cancellation": "Exact task ID, cooperative cancellation flag; on_cancel delivered once on UI before terminal publication. Cancellation is not rollback",
        "lifetime": "Keep userdata until get observes terminal, release succeeds, or destroy. Close cancels; DLL remains loaded until workers, cancellation callbacks and UI terminal publication finish. get strings borrowed only through callback",
        "retention": "release terminal records explicitly; create at capacity may evict oldest terminal; IDs never reused",
        "validationStatus": "Task lifecycle and host acceptance are recorded in the implementation roadmap; output staging and commit remain separate",
        "documentation": "sdk/native/README.md"
    })
    staging = header_interface("staging.h", "pulse.staging", "PulseStagingV1",
                               "PULSE_STAGING_INTERFACE_NAME", "PULSE_STAGING_VERSION_1",
                               ["create", "commit", "get", "cancel", "release", "choose_destination"])
    staging.update({
        "thread": "choose_destination synchronous owner UI; create callback and create/commit/release disk work on host workers; get callback synchronous caller thread; commit confirmation/submission on owner UI",
        "limits": {"stages": 16, "destinationUtf8Bytes": 131072, "treeEntries": 2000000, "topLevelItems": 4096, "treePathUtf8Bytes": 1073741824},
        "destination": "Existing ordinary local drive directory; private hidden host-created staging directory beneath destination preserves volume. choose_destination uses the standard host folder picker.",
        "commit": "Join writers before commit. Host validates nonempty ordinary tree, rejects reparse points and repeated commit, then confirms and submits standard file operations. Snapshot task_id identifies submitted operation; acceptance is not completion.",
        "lifetime": "Borrowed callback strings expire on return. Finish writers before release. Close blocks create/commit/choose; get/cancel/release remain valid through destroy. Close never removes writer output; remaining cleanup starts after destroy.",
        "cleanup": "ID-only asynchronous release for unsubmitted or terminal stage; successful cleanup removes the ID. Cancel is cooperative and not rollback.",
        "documentation": "sdk/native/README.md"
    })
    settings_pages_v1 = header_interface("settings_pages.h", "pulse.settings.pages", "PulseSettingsPagesV1",
                                          "PULSE_SETTINGS_PAGES_INTERFACE_NAME", "PULSE_SETTINGS_PAGES_VERSION_1",
                                          ["create", "query", "remove"])
    settings_pages = header_interface("settings_pages.h", "pulse.settings.pages", "PulseSettingsPagesV2",
                                      "PULSE_SETTINGS_PAGES_INTERFACE_NAME", "PULSE_SETTINGS_PAGES_VERSION_2",
                                      ["create", "query", "remove", "create_tab", "set_status", "set_side_preview"],
                                      version=2)
    settings_pages.update({
        "thread": "Owner UI thread for all calls and callbacks",
        "supportedVersions": [{"version": 1, "table": "PulseSettingsPagesV1", "operations": settings_pages_v1["operations"]},
                              {"version": 2, "table": "PulseSettingsPagesV2", "operations": settings_pages["operations"]}],
        "limits": {"pagesPerSession": 1, "tabsPerSession": 6, "titleUtf8Bytes": 128, "tabIdUtf8Bytes": 64, "tabTitleUtf8Bytes": 64, "statusTextUtf8Bytes": 128, "sidePreviewWidthDip": [240, 440]},
        "presentation": "Up to 6 embedded UI-model settings tabs per package session (1 in V1), with optional header status pill and responsive side preview panel.",
        "query": "Dedicated pulse.ui.model, pulse.ui.events, and pulse.ui.data tables per settings page; page IDs are panel IDs. No mount interface.",
        "lifetime": "Title/descriptor/model arrays and strings copied synchronously; borrowed callback values expire at callback return. remove/stop invalidate page IDs and subscriptions. Keep callback userdata until callbacks return; dedicated contexts retained through session destroy",
        "persistence": "Use existing pulse.resources settings_get/settings_set with expected_version. Acceptance is not save completion; version_conflict never overwrites. Reload current revision, retain draft and require explicit retry",
        "sample": "Archive destination and compression format/level preferences with live context menu preview",
        "validationStatus": "V1+V2 settings lifecycle and real-host persistence/conflict/theme/scroll acceptance",
        "documentation": "sdk/native/README.md"
    })
    dialogs_v1 = header_interface("dialogs.h", "pulse.dialogs", "PulseDialogsV1",
                                  "PULSE_DIALOGS_INTERFACE_NAME", "PULSE_DIALOGS_VERSION_1",
                                  ["show", "set_states", "close"])
    dialogs = header_interface("dialogs.h", "pulse.dialogs", "PulseDialogsV2",
                               "PULSE_DIALOGS_INTERFACE_NAME", "PULSE_DIALOGS_VERSION_2",
                               ["show", "set_states", "close", "subscribe_command",
                                "unsubscribe_command", "update_model", "get_state"],
                               version=2)
    dialogs.update({
        "thread": "Owner UI thread for all calls and callbacks; show returns at once and the dialog answers later",
        "limits": {"dialogsPerSession": 1, "buttons": 3, "titleUtf8Bytes": 128, "messageUtf8Bytes": 2048,
                   "widthDip": [360, 720], "formHeightDip": 640, "commandSubscriptions": 32},
        "supportedVersions": [{"version": 1, "table": "PulseDialogsV1", "operations": dialogs_v1["operations"]},
                              {"version": 2, "table": "PulseDialogsV2", "operations": dialogs["operations"]}],
        "presentation": "Host-drawn Fluent modal over the owner window guarded by PickerOwnerGuard: title, optional message, optional UI-model form, 1-3 footer buttons (standard/primary/danger). Text inputs with kind password are masked. V1 rejects declared model commands with UNAVAILABLE; V2 supports non-closing in-dialog command callbacks (subscribe_command/unsubscribe_command), dynamic model updates (update_model), synchronous state queries (get_state), and automatic nested picker parenting.",
        "lifetime": "Model and strings copied by show/update_model. closed runs exactly once with every declared state unless the session stops first; borrowed values expire at callback return.",
        "sample": "extensions/7zip: password prompt, compression options and checksum results",
        "documentation": "sdk/native/include/pulse/dialogs.h"
    })
    icons_v1 = header_interface("icons.h", "pulse.icons", "PulseIconsV1",
                                "PULSE_ICONS_INTERFACE_NAME", "PULSE_ICONS_VERSION_1",
                                ["register_icon", "update_icon", "unregister_icon", "resolve_builtin"])
    icons = header_interface("icons.h", "pulse.icons", "PulseIconsV2",
                             "PULSE_ICONS_INTERFACE_NAME", "PULSE_ICONS_VERSION_2",
                             ["register_icon", "update_icon", "unregister_icon", "resolve_builtin", "resolve_icon"],
                             version=2)
    icons.update({
        "thread": "Owner UI thread for all calls",
        "limits": {"dynamicIconsPerSession": 64, "layersPerIcon": 32, "pathBytesPerLayer": 2048, "totalPathBytes": 8192},
        "supportedVersions": [{"version": 1, "table": "PulseIconsV1", "operations": icons_v1["operations"]},
                              {"version": 2, "table": "PulseIconsV2", "operations": icons["operations"]}],
        "presentation": "68 built-in Fluent StoreIcon vectors (0..67) plus multi-layer SVG path vector icons (M L H V C S A Z) with semantic theme colors (inherit, accent, success, warning, danger, RGB) cached per D2D factory; V2 resolve_icon resolves manifest contributes.icons, dynamic icons, qualified package/name, #id, and built-in names to PulseIconId",
        "lifetime": "Descriptor strings and layer arrays copied synchronously; dynamic icons unregistered automatically when session closes",
        "documentation": "sdk/native/include/pulse/icons.h"
    })
    diagnostics = header_interface("diagnostics.h", "pulse.diagnostics", "PulseDiagnosticsV1",
                                   "PULSE_DIAGNOSTICS_INTERFACE_NAME", "PULSE_DIAGNOSTICS_VERSION_1",
                                   ["get_last_error", "clear_last_error"])
    diagnostics.update({
        "thread": "Owner UI thread for all calls and synchronous callbacks",
        "presentation": "Per-session structured failure record (status, monotonic sequence, interface_name, operation, subject, field, message) populated by pulse.ui.model, pulse.dialogs, pulse.settings.pages, pulse.pickers, pulse.workbench, pulse.tray, pulse.search, pulse.shell, pulse.associations, pulse.vfs, and pulse.columns.",
        "lifetime": "get_last_error invokes callback synchronously; borrowed PulseStringView slices expire when callback returns. Successful host calls preserve the prior failure until clear_last_error.",
        "documentation": "sdk/native/include/pulse/diagnostics.h"
    })
    pickers = header_interface("pickers.h", "pulse.pickers", "PulsePickersV1",
                               "PULSE_PICKERS_INTERFACE_NAME", "PULSE_PICKERS_VERSION_1",
                               ["open_files", "save_file", "pick_folders", "quick_pick", "cancel"])
    pickers.update({
        "thread": "Owner UI thread for all calls and callbacks; launch returns immediately and presents on a subsequent UI turn",
        "limits": {"activePickersPerSession": 1, "filters": 32, "quickPickItems": 512, "pathUtf8Bytes": 32768},
        "presentation": "Standard Win32 file/save/folder pickers and host-drawn Fluent QuickPick popup (with Pinyin + substring filtering, built-in/custom icons, and shortcut hints), parented to the open pulse.dialogs modal if present or the owner window via PickerOwnerGuard.",
        "lifetime": "Request inputs copied synchronously; callback runs at most once with outcome ACCEPTED, CANCELLED, or FAILED unless session stops first (which cancels open pickers and suppresses pending callbacks).",
        "documentation": "sdk/native/include/pulse/pickers.h"
    })
    workbench_v1 = header_interface("workbench.h", "pulse.workbench", "PulseWorkbenchV1",
                                    "PULSE_WORKBENCH_INTERFACE_NAME", "PULSE_WORKBENCH_VERSION_1",
                                    ["show_snack", "show_notice", "reveal_path", "show_text_diff", "set_clipboard_text"])
    workbench_v2 = header_interface("workbench.h", "pulse.workbench", "PulseWorkbenchV2",
                                    "PULSE_WORKBENCH_INTERFACE_NAME", "PULSE_WORKBENCH_VERSION_2",
                                    ["show_snack", "show_notice", "reveal_path", "show_text_diff", "set_clipboard_text",
                                     "open_search", "reveal_path_from_rect", "query_appearance",
                                     "subscribe_appearance", "unsubscribe_appearance"],
                                    version=2)
    workbench = header_interface("workbench.h", "pulse.workbench", "PulseWorkbenchV3",
                                 "PULSE_WORKBENCH_INTERFACE_NAME", "PULSE_WORKBENCH_VERSION_3",
                                 ["show_snack", "show_notice", "reveal_path", "show_text_diff", "set_clipboard_text",
                                  "open_search", "reveal_path_from_rect", "query_appearance",
                                  "subscribe_appearance", "unsubscribe_appearance",
                                  "query_locale", "subscribe_locale", "unsubscribe_locale"],
                                 version=3)
    workbench.update({
        "thread": "Owner UI thread for all calls, optional notification action callbacks, and appearance/locale subscriptions",
        "limits": {"actionsPerNotification": 2, "retainedActionTokens": 64, "snackTextBytes": 256, "noticeMessageBytes": 1024, "appearanceSubscriptionsPerSession": 16, "localeSubscriptionsPerSession": 16, "searchQueryBytes": 512},
        "supportedVersions": [{"version": 1, "table": "PulseWorkbenchV1", "operations": workbench_v1["operations"]},
                              {"version": 2, "table": "PulseWorkbenchV2", "operations": workbench_v2["operations"]},
                              {"version": 3, "table": "PulseWorkbenchV3", "operations": workbench["operations"]}],
        "presentation": "Capsule snacks, corner notice cards with optional action buttons, current-tab/new-tab/selection path reveal (with optional V2 screen-space origin_rect and hidden-to-tray window reveal), search-tab launch, built-in side-by-side text diff viewer, clipboard UTF-8 text write, host appearance/backdrop/accent observation, and V3 host UI locale query/subscription. V2 reveal_path_from_rect also accepts PREVIEW=3, HOVER_PREVIEW=4, and DISMISS_HOVER_PREVIEW=5 without changing the interface layout. Preview modes use the host preview UI rather than navigating a file tab; hover respects host preview preferences and dwell and does not activate the owner. Dismiss must supply the same path as the hover request so stale cancellation cannot close a different target. Older hosts may return PULSE_ERROR_ARGUMENT or PULSE_ERROR_UNAVAILABLE for these modes; callers must handle that result rather than treating a successful V2 query as preview-mode support.",
        "lifetime": "Inputs copied synchronously; optional action callback fires at most once while session remains active; appearance and locale subscriptions deliver deferred initial and coalesced updates on session pump; stopping session invalidates pending action tokens and subscriptions.",
        "documentation": "sdk/native/include/pulse/workbench.h"
    })
    tray = header_interface("tray.h", "pulse.tray", "PulseTrayV1",
                            "PULSE_TRAY_INTERFACE_NAME", "PULSE_TRAY_VERSION_1",
                            ["snapshot", "collect", "remove_item", "clear", "release_to_directory", "subscribe", "unsubscribe"])
    tray.update({
        "thread": "Owner UI thread for all calls and synchronous/deferred snapshot callbacks",
        "limits": {"collectPaths": 256, "subscriptionsPerSession": 16, "pathUtf8Bytes": 32768},
        "presentation": "Bidirectional bridge to Pulse's shared StagingTray (AppCore::tray): synchronous snapshot inspection, batch collection with copy/move intent, item removal/clear, copy/move release to a destination directory, and coalesced revision subscriptions.",
        "lifetime": "snapshot invokes callback synchronously; subscribe delivers deferred initial and coalesced updates on session pump; borrowed strings and PulseTrayEntry slices expire when callback returns.",
        "documentation": "sdk/native/include/pulse/tray.h"
    })
    search = header_interface("search.h", "pulse.search", "PulseSearchV1",
                              "PULSE_SEARCH_INTERFACE_NAME", "PULSE_SEARCH_VERSION_1",
                              ["query", "cancel"])
    search.update({
        "thread": "Owner UI thread for query, cancel, and completion callbacks; search execution runs asynchronously on PulseIndex IPC or a bounded worker fallback",
        "limits": {"activeRequestsPerSession": 16, "queryUtf8Bytes": 512, "kindTokenBytes": 64, "scopePathUtf8Bytes": 32768, "defaultMaxResults": 20, "maxResults": 50},
        "presentation": "Bounded asynchronous search over PulseIndex (with kind_token expansion, Pinyin, folder/file filtering, and optional scope_directory), falling back to a cooperative-cancellable directory scan when the index service is disconnected.",
        "lifetime": "Request inputs copied synchronously; callback runs at most once on owner UI thread unless cancelled or session closes first; borrowed PulseSearchHit slices expire when callback returns.",
        "documentation": "sdk/native/include/pulse/search.h"
    })
    shell = header_interface("shell.h", "pulse.shell", "PulseShellV1",
                             "PULSE_SHELL_INTERFACE_NAME", "PULSE_SHELL_VERSION_1",
                             ["get_tag_catalog", "get_path_tags", "set_path_tag", "query_context_menu",
                              "invoke_context_menu", "close_context_menu", "read_preview", "cancel_preview"])
    shell.update({
        "thread": "Owner UI thread for all calls and callbacks; context menus delegate out-of-process to pulse_shell_host.exe and preview thumbnails/excerpts read asynchronously on background workers",
        "limits": {"menuPaths": 64, "activeMenuSessions": 4, "activePreviewRequests": 8, "textExcerptBytes": 1024, "thumbPxMin": 32, "thumbPxMax": 512, "pathUtf8Bytes": 32768},
        "presentation": "Color tag catalog and per-path tag assignment (AppCore::places), out-of-process isolated Shell context menu query/invoke/close, and non-blocking Quick Look file metadata, UTF-8 text excerpt, and WIC premultiplied BGRA thumbnail summary.",
        "lifetime": "Tag callbacks run synchronously; context menu replies stream partial/final callbacks until closed; preview callbacks run at most once on owner UI thread unless cancelled or session stops; borrowed slices expire when callbacks return.",
        "documentation": "sdk/native/include/pulse/shell.h"
    })
    associations = header_interface("associations.h", "pulse.associations", "PulseAssociationsV1",
                                    "PULSE_ASSOCIATIONS_INTERFACE_NAME", "PULSE_ASSOCIATIONS_VERSION_1",
                                    ["query", "apply", "remove", "cancel", "open_chooser", "open_default_apps"])
    associations.update({
        "thread": "Owner UI thread for all calls and async callbacks; registry and IApplicationAssociationRegistration queries/updates run on a serialized host worker thread while OS chooser/settings dialogs launch on owner UI thread",
        "limits": {"maxExtensionsPerRequest": 64, "activeRequestsPerSession": 16, "handlerIdBytes": 64, "extensionBytes": 32, "displayNameUtf8Bytes": 128, "verbLabelUtf8Bytes": 128, "iconResourceBytes": 128},
        "presentation": "Host-managed reversible HKCU file association service restricted to extensions declared in manifest contributes.fileHandlers (never overwriting Windows UserChoice silently; restoring backed-up HKCU defaults automatically on package disable or uninstall).",
        "lifetime": "Request inputs validated and copied synchronously; async callbacks run at most once on owner UI thread unless cancelled or session stops; borrowed PulseStringView slices expire when callbacks return.",
        "documentation": "sdk/native/include/pulse/associations.h"
    })
    vfs = header_interface("vfs.h", "pulse.vfs", "PulseVfsV1",
                           "PULSE_VFS_INTERFACE_NAME", "PULSE_VFS_VERSION_1",
                           ["register_mount", "unregister_mount", "invalidate_path", "open_path"])
    vfs.update({
        "thread": "Owner UI thread for register_mount, unregister_mount, invalidate_path, and open_path; provider list_dir, extract_file, and stat callbacks execute asynchronously on host worker threads",
        "limits": {"maxMountsPerSession": 32, "maxEntriesPerListing": 50000, "mountIdBytes": 64, "displayNameUtf8Bytes": 128, "iconGlyphUtf8Bytes": 16, "entryNameUtf8Bytes": 255, "relPathUtf8Bytes": 4096, "maxPathSegments": 64},
        "presentation": "Read/extract Virtual File System provider registration under pulse:vfs:<package_id>/<mount_id>[/<rel_path>], integrating asynchronous directory listing, breadcrumb display names, temp extraction on file open, and session drain safety before DLL unload.",
        "lifetime": "Mount descriptors copied synchronously; provider callbacks receive cooperative cancellation token that asserts automatically when host cancels or unregisters mount; Session::Drain waits for active provider calls via CallbackActive() before closing the module.",
        "documentation": "sdk/native/include/pulse/vfs.h"
    })
    columns = header_interface("columns.h", "pulse.columns", "PulseColumnsV1",
                               "PULSE_COLUMNS_INTERFACE_NAME", "PULSE_COLUMNS_VERSION_1",
                               ["register_column", "register_badge", "set_title", "invalidate", "unregister"])
    columns.update({
        "thread": "Owner UI thread for register_column, register_badge, set_title, invalidate, and unregister; provider query callbacks execute asynchronously on a host worker thread in batches and must never wait for the UI thread",
        "limits": {"columnsPerPackage": 4, "badgesPerPackage": 4, "batchItems": 64, "cellTextUtf8Bytes": 256, "idBytes": 64, "titleUtf8Bytes": 64, "extensionsBytes": 512, "cachedValuesPerProvider": 20000, "widthDipMin": 48, "widthDipMax": 480},
        "presentation": "Display-only Details columns appended after the built-in columns and small icon badges at the top-right of item icons (one per item: the first provider in registration order wins), for local folders; the user hides or shows each provider from the column header menu and resizes its column; values are asked lazily for visible rows and cached until invalidate.",
        "lifetime": "Descriptors copied synchronously; unregister returns once a running query for the provider has returned (is_cancelled reads nonzero meanwhile); session close unregisters every provider and waits for running queries before DLL unload.",
        "documentation": "sdk/native/include/pulse/columns.h"
    })
    statusbar = header_interface("statusbar.h", "pulse.statusbar", "PulseStatusBarV1",
                                 "PULSE_STATUSBAR_INTERFACE_NAME", "PULSE_STATUSBAR_VERSION_1",
                                 ["add_item", "update_item", "set_visible", "remove_item"])
    statusbar.update({
        "thread": "Owner UI thread for add_item, update_item, set_visible, and remove_item; on_click callbacks run on the owner UI thread",
        "limits": {"itemsPerPackage": 2, "idBytes": 64, "textUtf8Bytes": 64, "tooltipUtf8Bytes": 256, "iconBytes": 96},
        "presentation": "Small text and/or icon items in the main window status bar after the folder text and the Git chip, in registration order, tinted by tone; a click runs the plugin callback and hovering shows the tooltip; items that no longer fit a narrow window are dropped whole.",
        "lifetime": "Descriptors copied synchronously; remove_item returns after a click callback already running for the item has returned; session close removes every item of the package.",
        "documentation": "sdk/native/include/pulse/statusbar.h"
    })
    fileevents = header_interface("fileevents.h", "pulse.fileevents", "PulseFileEventsV1",
                                  "PULSE_FILEEVENTS_INTERFACE_NAME", "PULSE_FILEEVENTS_VERSION_1",
                                  ["subscribe", "unsubscribe"])
    fileevents.update({
        "thread": "Owner UI thread for subscribe and unsubscribe; event callbacks run on the owner UI thread after the host applied the operation",
        "limits": {"subscriptionsPerSession": 8, "itemsPerEvent": 1024},
        "presentation": "Read-only notifications of file operations Pulse ran (copy, move, rename, recycle, delete, new folder/file, restore, empty Recycle Bin, including undo/redo and plugin requests), one event per operation with the items that really changed; changes made outside Pulse are not reported.",
        "lifetime": "Event strings are borrowed for the callback; unsubscribe returns after a running callback for the subscription returned; session close removes every subscription.",
        "documentation": "sdk/native/include/pulse/fileevents.h"
    })
    refreshed_names = ("pulse.ui", "pulse.ui.model", "pulse.ui.data", "pulse.directories", "pulse.context", "pulse.fileops", "pulse.ui.mount", "pulse.commands", "pulse.preview", "pulse.tasks", "pulse.staging", "pulse.settings.pages", "pulse.dialogs", "pulse.icons", "pulse.diagnostics", "pulse.pickers", "pulse.workbench", "pulse.tray", "pulse.search", "pulse.shell", "pulse.associations", "pulse.vfs", "pulse.columns", "pulse.statusbar", "pulse.fileevents")
    catalog["interfaces"] = [item for item in catalog["interfaces"] if item["name"] not in refreshed_names] + [model, data, directories, context, fileops, mount, commands, preview, tasks, staging, settings_pages, dialogs, icons, diagnostics, pickers, workbench, tray, search, shell, associations, vfs, columns, statusbar, fileevents]
    from native_author_mcp import TOOLS
    catalog["authoring"] = {
        "mcpServer": "tools/extensions/native_author_mcp.py",
        "command": "python tools/extensions/native_author_mcp.py --workspace AUTHOR_WORKSPACE --sdk-root SDK_ROOT --pulse HOST",
        "transport": "stdio JSON-RPC, initialize/tools/list/tools/call and notifications/cancelled",
        "tools": copy.deepcopy(TOOLS),
        "buildEnvironment": "Windows x64 Visual Studio developer environment, CMake/Ninja, Python 3",
        "installStore": "AUTHOR_WORKSPACE/.pulse-author/extensions; daily Pulse store is not modified",
        "execution": "Build runs project CMake code; native install/run require allowNative=true. No automatic native execution from SDK discovery.",
        "documentation": "sdk/native/README.md",
    }
    schema = json.loads((ROOT / "schemas/extensions/manifest.schema.json").read_text(encoding="utf-8"))
    catalog.pop("resourcePermissions", None)
    catalog["resourceDeclarations"] = copy.deepcopy(schema["properties"]["resources"])
    catalog["resourceDeclarationPolicy"] = "Installation disclosure only; native code runs in process without per-call permission gates."
    for interface in catalog["interfaces"]:
        iface_name = interface.get("name")
        if iface_name not in STABILITY_TIERS:
            raise ValueError("Unclassified native interface stability tier: " + str(iface_name))
        interface["stabilityTier"] = STABILITY_TIERS[iface_name]
        for operation in interface.get("operations", []):
            if isinstance(operation, dict) and "permission" in operation:
                operation["resourceDeclaration"] = operation.pop("permission")
        if interface["name"] in ("pulse.files", "pulse.resources"):
            interface["resourceDeclarationPolicy"] = catalog["resourceDeclarationPolicy"]
    if {item["name"] for item in catalog["interfaces"]} != set(STABILITY_TIERS):
        raise ValueError(f"Native interface catalog does not match expected {len(STABILITY_TIERS)}-interface tier partition")
    catalog["stability"] = {
        "sdkPreview": catalog["sdkPreview"],
        "abiMajor": 1,
        "coreEntryAbi": {
            "stabilityTier": "frozen",
            "abiMajor": 1,
            "abiVersionMacro": "PULSE_ABI_VERSION_1",
            "sdkPreviewMacro": "PULSE_SDK_PREVIEW",
            "entrypoint": "PulsePluginEntryV1",
            "previewExport": "PulsePluginSdkPreview",
            "symbols": [
                "PulseHostV1",
                "PulsePluginV1",
                "PulsePluginEntryV1",
                "PulsePluginEntryV1Fn",
                "PulsePluginSdkPreview",
                "PulseStringView",
                "PulseStatus",
            ],
        },
        "prefixTableRule": "Append-only struct-prefix evolution (V1 <= V2 <= V3 <= V4): existing fields preserve identical index, name, type, size, and byte offset; new fields append strictly at the tail with monotonically increasing struct size and version.",
        "experimentalSurfaceRule": "Opt-in UI surface not yet covered by binary freeze; plugins must handle query_interface returning PULSE_ERROR_UNAVAILABLE or PULSE_ERROR_ABI (C++ std::nullopt fallback) gracefully.",
        "tiers": {
            "frozen": {
                "count": len(FROZEN_INTERFACES),
                "interfaces": list(FROZEN_INTERFACES),
                "binaryFreezeCandidate": True,
            },
            "experimental": {
                "count": len(EXPERIMENTAL_INTERFACES),
                "interfaces": list(EXPERIMENTAL_INTERFACES),
                "binaryFreezeCandidate": False,
            },
        },
    }
    catalog["supportPolicy"] = {
        "abiMajor": 1,
        "supportedApiVersions": [1],
        "deprecationWindow": {
            "minMinorReleases": 2,
            "minDays": 180,
        },
        "emergencyRevocation": {
            "channels": ["revokedPackages", "revokedRevisions"],
            "scope": "Immediate revocation reserved exclusively for active security vulnerabilities, memory corruption, or host-crashing defects.",
        },
        "policyDocument": "docs/extensions-sdk-lifecycle-and-support.md",
    }
    catalog["lifecycle"]["thread"] = "owner UI thread, except pulse.dispatch post/cancel and pulse.ui.data publish are any-thread"
    catalog["packageStore"] = {
        "version": 1, "architecture": "windows-x64", "manifestField": "native",
        "manifestSchema": schema["$defs"]["nativePackage"],
        "removedManifestFields": ["runtime", "commands", "toolbar", "palette", "workflows", "panel"], "removedCapabilities": ["runtime.invoke"],
        "defaultStore": "%LOCALAPPDATA%/Pulse/extensions", "registryFormatVersion": 1,
        "binaryPath": "binaries/<sha256>.dll (single DLL); packages/<manifestSha256>/<originalName> (multi-file)", "manifestPath": "versions/<manifestSha256>.json",
        "maxBinaryBytes": 64 * 1024 * 1024,
        "authorCommand": "pulse_ext.py --pulse HOST --json native-manifest TEMPLATE --library DLL [--file DEPENDENCY_OR_RESOURCE ...] --output MANIFEST",
        "packCommand": "pulse_ext.py --pulse HOST --json pack MANIFEST --output PACKAGE.pulse-ext",
        "verifyCommand": "pulse_ext.py --pulse HOST --json verify PACKAGE.pulse-ext",
        "installCommand": "pulse_ext.py --pulse HOST --json install MANIFEST --allow-native",
        "archiveInstallCommand": "pulse_ext.py --pulse HOST --json install PACKAGE.pulse-ext --allow-native",
        "archive": {"format": "pulse-extension-local", "formatVersion": 2,
                    "members": ["manifest.json", "package.json", "<native.library>", "<native.files[*].path>"],
                    "maxBytes": 256 * 1024 * 1024 + 256 * 1024,
                    "manifestMaxBytes": 65536, "metadataMaxBytes": 16384,
                    "binaryMaxBytes": 64 * 1024 * 1024, "snapshotInstall": True,
                    "metadataNativeFields": ["library", "sha256", "files (optional)"], "extractsArchive": False,
                    "declarativeCompatibility": "formatVersion 1 retains manifest.json/package.json only"},
        "enableCommand": "pulse_ext.py --pulse HOST --json enable ID --allow-native",
        "rollbackCommand": "pulse_ext.py --pulse HOST --json rollback ID --allow-native",
        "consent": "Install, enable and rollback require explicit --allow-native, including new binary hashes. Native code executes inside Pulse with the user's process privileges; capability declarations are not a sandbox.",
        "dependencies": "Up to 63 additional flat-name private DLLs/resources; each file is SHA-256 verified and locked until unload. Multi-file loads use DLL_LOAD_DIR and SYSTEM32. Whole-package size <=256 MiB.",
        "updates": "Atomic binary publication precedes registry commit; restart Pulse to activate an update or rollback. Loaded native DLLs are never hot-replaced.",
        "discoveryGrantsExecution": False, "documentation": "docs/native-packages.md"
    }
    catalog["pending"] = ["native model data_source/context resolver/dataOperation integration", "general-purpose host interfaces beyond published native tables", "complete interaction/theme/DPI verification of native model catalog"]
    return {
        path: json.dumps(catalog, ensure_ascii=False, indent=2) + "\n",
        ROOT / "sdk/native/ui-model.schema.json": json.dumps(constraints, ensure_ascii=False, indent=2) + "\n",
    }


def repository_outputs():
    generated = outputs()
    api_metadata_path = ROOT / "schemas/extensions/api_metadata.json"
    if api_metadata_path.is_file():
        catalog = json.loads(generated[ROOT / "sdk/native/api-catalog.json"])
        api_metadata = json.loads(api_metadata_path.read_text(encoding="utf-8"))
        api_metadata["stability"] = copy.deepcopy(catalog["stability"])
        api_metadata["supportPolicy"] = copy.deepcopy(catalog["supportPolicy"])
        generated[api_metadata_path] = json.dumps(api_metadata, ensure_ascii=False, indent=2) + "\n"
    return generated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    for path, text in repository_outputs().items():
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                raise SystemExit("Stale native discovery: " + str(path))
        else:
            path.write_text(text, encoding="utf-8")
    print("Native discovery is current" if args.check else "Generated native discovery")


if __name__ == "__main__":
    main()