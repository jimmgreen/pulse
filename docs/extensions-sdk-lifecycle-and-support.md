# Pulse Native Extension SDK — Lifecycle, Compatibility, Deprecation & Migration Policy

This specification defines the canonical binary/semantic compatibility contract, stability tiering (`frozen` vs `experimental`), support and deprecation windows, migration playbooks, and upgrade/rollback/crash-recovery guarantees for the Pulse C/C++20 Native Extension SDK (`windows-x64`, `PULSE_ABI_VERSION_1 = 1`).

Related references:
- [Native SDK Reference (`sdk/native/README.md`)](../sdk/native/README.md)
- [Native DLL Packages & Store Lifecycle (`docs/native-packages.md`)](native-packages.md)
- [C++ Extension Author Quickstart (`docs/extensions-sdk-quickstart.md`)](extensions-sdk-quickstart.md)
- [Machine-Readable Interface Catalog (`sdk/native/api-catalog.json`)](../sdk/native/api-catalog.json)
- [Extension Host Metadata Contract (`schemas/extensions/api_metadata.json`)](../schemas/extensions/api_metadata.json)
- [Extension Authoring & Verification CLI (`tools/extensions/README.md`)](../tools/extensions/README.md)

---

## 1. Stability Tiers & 1.0 Freeze Matrix

Pulse partitions its public Native SDK surface (`sdk/native/include/pulse/*.h`) into two machine-enforced stability tiers recorded in `sdk/native/api-catalog.json` (`interfaces[*].stabilityTier` and top-level `stability`) and `schemas/extensions/api_metadata.json`:

- **`frozen` (Core Entry ABI + 12 host service interfaces)**: Binary layout, struct field offsets, calling convention (`PULSE_CALL` / `__cdecl` under `#pragma pack(push, 8)`), function pointer signatures, status codes, and ownership/cancellation semantics are locked for the `1.x` ABI major line (`PULSE_ABI_MAJOR = 1` / `PULSE_ABI_VERSION_1 = 1u`). Evolution is strictly append-only via versioned struct prefixes or new interface names.
- **`experimental` (8 declarative/retained UI, vector drawing, diagnostics, picker & workbench interfaces)**: Available for opt-in use in Preview 3 (`PULSE_SDK_PREVIEW = 3`), but still undergoing layout, control-set, vector drawing, and data-binding refinement prior to binary freeze. Extensions querying `experimental` interfaces **must** handle `PULSE_ERROR_UNAVAILABLE` or `PULSE_ERROR_ABI` (`std::nullopt` in C++20 `pulse::sdk::Host`) gracefully and degrade to `frozen` command/task/dialog flows when unavailable.

### 1.1 Core Entry ABI & 20-Interface Stability Matrix

| Surface / Interface Name | Header | Published Version(s) | Primary Table / Symbols | Thread Affinity | Stability Tier |
|---|---|---|---|---|---|
| **Core Entry ABI** | `include/pulse/abi.h` | `PULSE_ABI_VERSION_1` (`1`) | `PulseHostV1`, `PulsePluginV1`, `PulsePluginEntryV1`, `PulsePluginSdkPreview`, `PulseStringView`, `PulseStatus` | Owner UI thread | `frozen` |
| `pulse.files` | `include/pulse/files.h` | `1` | `PulseFilesV1` | Owner UI thread (async callbacks on UI thread) | `frozen` |
| `pulse.resources` | `include/pulse/resources.h` | `1` | `PulseResourcesV1` | Owner UI thread (async callbacks on UI thread) | `frozen` |
| `pulse.dispatch` | `include/pulse/dispatch.h` | `1` | `PulseDispatchV1` | `post`/`cancel` any thread; `Query` & callbacks on owner UI thread | `frozen` |
| `pulse.directories` | `include/pulse/directories.h` | `1` | `PulseDirectoriesV1` | Owner UI thread | `frozen` |
| `pulse.context` | `include/pulse/context.h` | `1` | `PulseContextV1` | Owner UI thread | `frozen` |
| `pulse.fileops` | `include/pulse/fileops.h` | `1`, `2`, `3`, `4` | `PulseFileOpsV1` ⊂ `PulseFileOpsV2` ⊂ `PulseFileOpsV3` ⊂ `PulseFileOpsV4` | Owner UI thread (engine executes on host workers) | `frozen` |
| `pulse.commands` | `include/pulse/commands.h` | `1`, `2` | `PulseCommandsV1` ⊂ `PulseCommandsV2` | Owner UI thread | `frozen` |
| `pulse.preview` | `include/pulse/preview.h` | `1` | `PulsePreviewV1` | Register/unregister on UI thread; `provide`/`publish` on host workers | `frozen` |
| `pulse.tasks` | `include/pulse/tasks.h` | `1` | `PulseTasksV1` | `create`/`cancel`/`get`/`release` & `on_cancel` on UI thread; `work`/`report`/`cancelled` on worker | `frozen` |
| `pulse.staging` | `include/pulse/staging.h` | `1` | `PulseStagingV1` | `choose_destination` UI thread; `create`/`commit`/`get`/`cancel`/`release` worker-safe | `frozen` |
| `pulse.settings.pages` | `include/pulse/settings_pages.h` | `1` | `PulseSettingsPagesV1` | Owner UI thread | `frozen` |
| `pulse.dialogs` | `include/pulse/dialogs.h` | `1`, `2` | `PulseDialogsV1` ⊂ `PulseDialogsV2` | Owner UI thread | `frozen` |
| `pulse.ui.model` | `include/pulse/ui_model.h` | `1` | `PulseUiModelV1` | Owner UI thread (`PULSE_PREVIEW_UI_MODEL` copied on preview worker) | `experimental` |
| `pulse.ui.events` | `include/pulse/ui_events.h` | `1` | `PulseUiEventsV1` | Owner UI thread | `experimental` |
| `pulse.ui.data` | `include/pulse/ui_data.h` | `1` | `PulseUiDataV1` | `publish` any thread; registration/binding/requests on owner UI thread | `experimental` |
| `pulse.ui.mount` | `include/pulse/ui_mount.h` | `1` | `PulseUiMountV1` | Owner UI thread | `experimental` |
| `pulse.icons` | `include/pulse/icons.h` | `1` | `PulseIconsV1` | Owner UI thread | `experimental` |
| `pulse.diagnostics` | `include/pulse/diagnostics.h` | `1` | `PulseDiagnosticsV1` | Owner UI thread | `experimental` |
| `pulse.pickers` | `include/pulse/pickers.h` | `1` | `PulsePickersV1` | Owner UI thread | `experimental` |
| `pulse.workbench` | `include/pulse/workbench.h` | `1` | `PulseWorkbenchV1` | Owner UI thread | `experimental` |

### 1.2 Criteria for Promoting `PULSE_SDK_PREVIEW` (`3` → `0` GA `1.0`)

Currently `abi.h` defines `#define PULSE_SDK_PREVIEW 3u` (`sdkPreview: 3`). Every plugin exports `PulsePluginSdkPreview` via `PULSE_PLUGIN_SDK_PREVIEW()`, and the host rejects mismatched preview exports with `PULSE_ERROR_SDK_PREVIEW` before calling `PulsePluginEntryV1`. Promoting `PULSE_SDK_PREVIEW` from `3u` to `0u` (signaling General Availability `1.0` ABI freeze) requires all of the following gates to pass simultaneously:

1. **Zero ABI Layout Drift**: `python tools/extensions/check_native_abi.py check` and `test_native_abi.py` pass with zero layout, alignment, `#pragma pack(1)` caller-isolation, or prefix-invariant violations, and a reviewed `windows-x64-msvc-1.0.json` baseline is committed.
2. **Cross-Version & Ownership Matrix**: `pulse_extension_abi_compat_test` passes 100% across `PulseHostV1`/`PulsePluginV1` size clamping, `PulseFileOpsV1..V4` canary-protected prefix negotiation, headless degradation, cancellation/terminal-state precedence, synchronous borrowed-buffer copying, and >16 `TaskGroup` burst retention.
3. **End-to-End Store & Isolation Readiness**: Detached CNG RSA signature enforcement, publisher continuity & `rotate` proofs, catalog emergency revocation (`revokedPackages` / `revokedRevisions`), and Job-governed out-of-process worker brokering (`pulse_7zip_worker` / `extension_host::CapabilityBroker`) pass acceptance checks.
4. **UI Surface Disposition**: Either the four `pulse.ui.*` interfaces complete full Light/Dark theme, 100%–200% DPI, narrow-layout, and keyboard/accessibility sign-off to join `frozen`, or remain explicitly gated behind `experimental` opt-in negotiation while the 12 `frozen` interfaces ship under the `1.0` binary guarantee.

---

## 2. Binary & Semantic Compatibility Contract

### 2.1 `size` & `version` Struct Negotiation and Clamping

All versioned entry and service structs begin with an identical 8-byte header under `#pragma pack(push, 8)`:

```c
uint32_t size;    /* offset 0, sizeof == 4 */
uint32_t version; /* offset 4, sizeof == 4 */
```

1. **Host-to-Plugin (`PulseHostV1`)**:
   - `Module::Load` rejects `host.size < sizeof(PulseHostV1)` (`24` bytes), `host.version != PULSE_ABI_VERSION_1` (`1`), or `host.query_interface == nullptr` with `PULSE_ERROR_ABI` before invoking `PulsePluginEntryV1`.
   - When an extended host caller passes `host.size > sizeof(PulseHostV1)`, `Module::Load` copies the `PulseHostV1` prefix into internal storage and **clamps `host_.size = sizeof(PulseHostV1)`** so the pointer passed to `PulsePluginEntryV1` never advertises unreadable bytes beyond `Module`'s `24`-byte `PulseHostV1` member.
2. **Plugin-to-Host (`PulsePluginV1`)**:
   - Plugins must check `host->size >= sizeof(PulseHostV1)` and `host->version == PULSE_ABI_VERSION_1`.
   - A plugin returning `out->size >= sizeof(PulsePluginV1)` (`48` bytes, including extended `sizeof(PulsePluginV1) + N` tail fields from a newer minor SDK) and `out->version == PULSE_ABI_VERSION_1` loads cleanly on a V1 host; the V1 host reads only the known 48-byte prefix.
   - **Fail-Closed Leak Prevention**: If `PulsePluginEntryV1` returns `PULSE_OK` with `out->size < sizeof(PulsePluginV1)` or `out->version != PULSE_ABI_VERSION_1`, `Module::Load` rejects the load with `PULSE_ERROR_ABI`. Whenever `out->size >= sizeof(PulsePluginV1)` and both `out->instance != nullptr` and `out->destroy != nullptr` were populated on a rejected `out->version`, `Module::Load` invokes `out->destroy(out->instance)` **exactly once** before unloading the DLL so plugin allocations never leak.

### 2.2 Append-Only Struct-Prefix Table Evolution (`PulseFileOpsV1..V4`)

Frozen interfaces evolve across minor releases exclusively by **append-only struct-prefix extension** (`V1` ⊂ `V2` ⊂ `V3` ⊂ `V4`), enforced automatically by `verify_freeze_invariants()` in `tools/extensions/check_native_abi.py`:

- For every consecutive table pair `V(k)` and `V(k+1)` in a `Pulse*V<N>` family:
  - `sizeof(V(k)) < sizeof(V(k+1))`;
  - Every field `[0 .. len(V(k)) - 1]` of `V(k)` appears in `V(k+1)` at the **exact same field index, name, normalized C type, byte size, and byte offset**;
  - Existing fields are never reordered, narrowed, widened, or repurposed.
- When a plugin calls `host->query_interface(host->context, name, version, out_table, out_size)`:
  - Requesting `version = k` with `out_size >= sizeof(TableV<k>)` writes **at most `sizeof(TableV<k>)` bytes**, stamps `out.size = sizeof(TableV<k>)` and `out.version = k`, and leaves any caller memory past `sizeof(TableV<k>)` completely untouched.
  - Requesting an unknown interface returns `PULSE_ERROR_UNAVAILABLE`.
  - Requesting `version = 0`, an unsupported future `version` (e.g. `version = 5` on `PulseFileOpsV4` or `version = 99` on `PulseCommandsV1`), or an undersized `out_size < sizeof(TableV<k>)` fails closed (`PULSE_ERROR_ABI` / non-`PULSE_OK`) without writing past the buffer or touching trailing bytes.
- Incompatible signature or semantic changes are prohibited in-place and must use a new interface name (e.g., `pulse.<domain>.v2`) while keeping the prior frozen table operational throughout the deprecation window.

### 2.3 Borrowed `PulseStringView` Lifetime & Cross-CRT Memory Ownership

- `PulseStringView` is a 16-byte POD slice (`const char* data` at offset `0`, `uint32_t length` at offset `8`, 4 bytes tail padding under `pack(8)`). Strings are valid UTF-8 byte spans; empty views may pass `data == nullptr` only when `length == 0`.
- **Caller → Host Synchronous Copy Guarantee**: Every host ingestion function (`PulseCommandsV1::register_command`, `PulseTasksV1::create`, `PulseTaskContext::report`, `PulsePreviewRequest::publish`, `PulseUiModelV1::create`/`update`/`set_states`, `PulseUiDataV1::publish`, `PulseDialogsV1::show`/`set_states`, `PulseSettingsPagesV1::create`) validates and **synchronously copies** all input `PulseStringView` slices and descriptor arrays into host-owned memory **before returning**. Plugins may safely pass views backed by stack buffers or temporary `std::string` instances and overwrite/destroy them immediately after the call returns.
- **Host → Plugin Callback Borrow Rule**: Snapshots and strings passed into plugin callbacks (`PulseContextSelectionSnapshot`, `PulseTaskSnapshot`, `PulseDirectoryEntry`, `PulseUiEvent`, staging directory paths, dialog state arrays) are **borrowed strictly until that callback returns**. Plugins must never cache raw pointers from callback arguments for background threads or later ticks; copy needed strings into plugin-owned storage inside the callback.
- **Cross-CRT Heap Isolation**: Host and plugin DLLs may link distinct static CRTs (`/MT`). Neither side ever calls `free` / `delete` on memory allocated by the other. Optional C++20 wrappers in [`sdk/native/include/pulse/client.hpp`](../sdk/native/include/pulse/client.hpp) (`ModelTree`, `CommandRegistration`, `Subscription`, `DispatchQueue`, `TaskGroup`) keep all STL containers, lambdas, and heap captures strictly inside the plugin DLL.

### 2.4 Thread Affinity, Cooperative Cancellation & Terminal-State Precedence

- **UI Thread Serialization**: Unless explicitly documented as cross-thread safe (`pulse.dispatch` `post`/`cancel`, `pulse.ui.data` `publish`, `pulse.preview` worker callbacks, `pulse.tasks` worker context, and `pulse.staging` worker operations), all host queries, calls, and callbacks execute on the **owner window UI thread**. Plugins must never perform blocking disk/network I/O or long CPU work on the UI thread, and background threads must never invoke UI-thread-only tables directly.
- **Cooperative Task Cancellation & Idempotency (`PulseTasksV1`)**:
  - Calling `cancel(task_id)` on a queued or `Running` task transitions the worker's `ctx->cancelled(ctx->context)` flag from `0` to `1`, transitions non-terminal state to `Cancelling`, and schedules `on_cancel` at most once on the owner UI thread.
  - Duplicate `cancel(task_id)` calls while `Cancelling` are idempotent (`PULSE_OK`) and **never** invoke `on_cancel` a second time (`cancel_count == 1`).
- **Terminal-State Immutability**:
  - Terminal states (`PULSE_TASK_COMPLETED`, `PULSE_TASK_PARTIAL`, `PULSE_TASK_FAILED`, `PULSE_TASK_CANCELLED`) are final once recorded by the worker.
  - If a worker finishes with `PULSE_TASK_COMPLETED` (or `PULSE_TASK_FAILED` during cleanup) **before** the host invokes `Cancel`, the late `Cancel` returns `PULSE_ERROR_STATE`, preserves `PULSE_TASK_COMPLETED` unchanged, and **never** fires `on_cancel` after completion.
- **Owned Task Retention (`TaskGroup` >16 Burst Guarantee)**:
  - Unowned fire-and-forget tasks prune oldest terminal entries beyond the 16-record history cap.
  - Tasks created with explicit caller ownership (`pulse::sdk::TaskGroup`, which binds `on_cancel` and releases via `PulseTasksV1::release`) remain retained in `TasksAdapter` across bursts exceeding 16 concurrent/rapid tasks (verified at 24+ burst completions) until `TaskGroup::Collect()` calls `release` on the UI thread, ensuring zero dropped terminal snapshots and deterministic lambda capture destruction after worker threads join.

### 2.5 C ABI Exception Prohibition & `LoadLibraryExW` Trust Boundary

- **Zero Exceptions Across ABI**: Every `PULSE_CALL` entrypoint and callback is `noexcept` at the binary boundary. C++ exceptions must be caught inside the side that threw them and mapped to an explicit `PulseStatus` code (`PULSE_ERROR_ARGUMENT`, `PULSE_ERROR_ABI`, `PULSE_ERROR_STATE`, `PULSE_ERROR_LIMIT`, `PULSE_ERROR_UNAVAILABLE`, `PULSE_ERROR_INITIALIZATION`, `PULSE_ERROR_INTERNAL`, etc.). Allowing a C++ exception to unwind across `PULSE_CALL` is undefined behavior.
- **`LoadLibraryExW` / `DllMain` Trust Boundary**: When Pulse loads an enabled native package, Windows `LoadLibraryExW` maps the PE32+ image and may execute `DllMain` and TLS initializers **before** Pulse inspects `PulsePluginSdkPreview` or calls `PulsePluginEntryV1`. Consequently, `PULSE_SDK_PREVIEW`, `PulseHostV1` negotiation, and manifest `resources` disclosures are **ABI compatibility and user-consent disclosures—not a security sandbox**. Native packages execute with the user's process privileges unless risky parsing is delegated to a Job-governed out-of-process worker (Section 4.2).

---

## 3. Support Window & Deprecation Policy

### 3.1 Supported `apiVersion` and `PULSE_ABI_MAJOR` Line

- **Manifest Contract**: `apiVersion: 1` in `schemas/extensions/manifest.schema.json`.
- **Native Binary Contract**: `PULSE_ABI_VERSION_1 = 1u` (`abiMajor: 1` in `sdk/native/api-catalog.json` and `schemas/extensions/api_metadata.json`), targeting `windows-x64` (`PE32+`, `minWindowsBuild: 10240`).
- Within `abiMajor == 1` post-1.0 freeze, compiled extensions targeting only `"frozen"` interfaces continue to load and operate across minor Pulse updates without recompilation.

### 3.2 Minimum Deprecation Window (`>= 2` Minor Releases AND `>= 180` Days)

No `"frozen"` interface, operation, manifest `resources` declaration, or published struct-prefix table version (such as `PulseFileOpsV1`..`V3` once frozen) may be removed until **both** conditions have been satisfied following formal deprecation:

1. **Release Count Floor**: At least **2 minor Pulse releases** (`minMinorReleases: 2`) have shipped with the deprecation notice active; **AND**
2. **Calendar Floor**: At least **180 calendar days** (`minDays: 180`) have elapsed since the first release publishing the deprecation notice and replacement migration path.
3. **Major-Version Boundary**: Actual removal of a frozen binary symbol or struct version occurs only in a new major ABI revision (`abiMajor >= 2`), never silently in a `1.x` patch or minor update.

### 3.3 Catalog & Tooling Warning Stages

| Stage | Trigger | `api-catalog.json` / Schema State | CLI / Host Behavior |
|---|---|---|---|
| **Stage 0 — Active (`frozen`)** | General support | `"stabilityTier": "frozen"` | Normal compilation, verification, and execution. |
| **Stage 1 — Soft Deprecation** | Replacement interface shipped | Adds `"deprecated": {"since": "<ver>", "replacement": "<iface>", "removeNoEarlierThan": "<date>"}` | `pulse_ext.py verify` and `native_host_diagnose` emit structured migration warnings; runtime execution remains unchanged. |
| **Stage 2 — Template & Authoring Sunset** | `>= 1` minor release after Stage 1 | Templates & samples updated to replacement API | `pulse_ext.py new` scaffolds only replacement APIs; existing installed/packed extensions continue to run unmodified. |
| **Stage 3 — Major-ABI Retirement** | `>= 2` minor releases **and** `>= 180` days, at `abiMajor + 1` | Retired from new major ABI catalog | Legacy `abiMajor` queries fail closed with `PULSE_ERROR_ABI` / `PULSE_ERROR_UNAVAILABLE` rather than crashing. |

*Note on `experimental` interfaces*: Interfaces in the `"experimental"` tier (`pulse.ui.model`, `pulse.ui.events`, `pulse.ui.data`, `pulse.ui.mount`, `pulse.icons`, `pulse.diagnostics`, `pulse.pickers`, `pulse.workbench`) may evolve across `PULSE_SDK_PREVIEW` increments prior to promotion into `"frozen"`. Once an interface is promoted to `"frozen"`, it immediately inherits the full 2-minor-release + 180-day protection above.

### 3.4 Emergency Revocation Exception (`revokedPackages` / `revokedRevisions`)

The 180-day / 2-minor-release deprecation window governs **API and contract evolution**, not active threats. Pulse reserves an immediate **emergency revocation exception** enforced through signed store catalog metadata:

- **`revokedPackages`**: Immediately blocks download, installation, and startup activation of all revisions of a package ID (used for malicious publisher keys or Critical/High privilege-escalation/data-loss defects).
- **`revokedRevisions`**: Immediately blocks specific compromised or deterministic host-crashing manifest SHA-256 digests while allowing safe older (`previousRevision` rollback) or patched newer revisions of the same package ID to run.

Emergency revocations take effect as soon as the signed catalog snapshot refreshes (and pair with local `crash-disabled.txt` quarantine if a build already faulted locally).

---

## 4. Migration Playbooks

### 4.1 Playbook A: Legacy Declarative / Preview 2 → Preview 3 → 1.0 GA

Use this playbook when upgrading older declarative packages or Preview 2 C++ plugins to Preview 3 (`PULSE_SDK_PREVIEW == 3u`) and preparing for `1.0` GA (`PULSE_SDK_PREVIEW == 0u`):

1. **Migrate Manifest Capabilities/Permissions to `resources` + `contributes.commands`**:
   - Remove retired declarative execution keys (`runtime`, `commands`, `toolbar`, `palette`, `workflows`, `panel`) and legacy `capabilities` / `permissions` / `runtime.invoke` fields; `manifest.schema.json` rejects them closed.
   - Declare installation disclosures in `resources` (from the 15 supported identifiers such as `"context.read"`, `"files.readDirectory"`, `"operations.own"`, `"settings.own"`).
   - Add optional static `contributes.commands` (`id`, `title`, optional `icon`, `shortcut`) so Pulse surfaces commands in menus and the command palette at **zero DLL load cost**, activating your DLL on first invocation.
   - Bind binary hashes with `pulse_ext.py --pulse HOST --json native-manifest manifest.template.json --library build/my_ext.dll [--file ...] --output build/manifest.json`.
2. **Stamp `PULSE_PLUGIN_SDK_PREVIEW()` & Negotiate Interfaces Safely**:
   - Place `PULSE_PLUGIN_SDK_PREVIEW()` in exactly one `.cpp` file alongside `PulsePluginEntryV1`.
   - Query `frozen` tables at the minimum struct version your feature requires (for example `PulseFileOpsV2` if you only need `submit_to_directory`, or `PulseFileOpsV4` with fallback to `PulseFileOpsV2`/`V1` on older hosts).
   - Wrap optional or `experimental` queries (`pulse.ui.mount`, `pulse.ui.model`, `pulse.ui.data`, `pulse.ui.events`) in a helper that converts `PULSE_ERROR_UNAVAILABLE` / `PULSE_ERROR_ABI` into `std::nullopt`, falling back to `pulse.commands` V1 + `pulse.tasks` V1 + `pulse.dialogs` V1 when running in headless or restricted host contexts:
   ```cpp
   template <class Table>
   std::optional<Table> TryQuery(const pulse::sdk::Host& host, std::string_view name, uint32_t version) noexcept {
       Table table{};
       return host.Query(name, version, table) == PULSE_OK ? std::optional<Table>(table) : std::nullopt;
   }
   ```
3. **Adopt `pulse::sdk::TaskGroup` Ownership & Synchronous Copy Rules**:
   - Replace manual detached `std::thread` task bookkeeping with `pulse::sdk::TaskGroup` (`sdk/native/include/pulse/client.hpp`).
   - Always supply a non-null `PulseTaskId*` to `TaskGroup::Start(...)`, poll `ctx.cancelled(ctx.context)` inside `work`, call `group.Collect()` on the owner UI thread (including during `deactivate` and `destroy`) to release retained host records, and call `group.CancelAll()` on session stop before `destroy` returns.
4. **Preview 3 → 1.0 GA Recompile Checklist**:
   - When Pulse promotes `PULSE_SDK_PREVIEW` from `3u` to `0u`, recompile against the `1.0` SDK headers with `/W4 /WX /permissive- /utf-8`, regenerate `manifest.json` (`native-manifest`), repack (`.pulse-ext` v2), sign (`package_signing.py sign`), and verify (`pulse_ext.py verify`). No source changes are required for extensions restricted to the 12 `frozen` interfaces.

### 4.2 Playbook B: High-Risk Native Parser / Codec Migration (In-Process DLL → Job-Governed Out-of-Process Worker)

In-process DLLs share `pulse.exe`'s virtual address space: an out-of-bounds write, stack overflow, or third-party codec crash inside an archive/media/document parser terminates the entire file manager. Extensions handling complex or untrusted binary inputs **should migrate parser execution out of the entry DLL into a Job-governed worker sidecar**, following the production architecture of `extensions/7zip` (`pulse_7zip_worker.exe`) and `extension_host::CapabilityBroker`:

1. **Split Entry Coordinator DLL from Sidecar Worker `.exe`**:
   - Keep `plugin.dll` (`PulsePluginEntryV1`) minimal and memory-safe: register `pulse.commands`, `pulse.preview`, `pulse.dialogs` prompts, `pulse.settings.pages`, `pulse.tasks`, and `pulse.staging`.
   - Move third-party or complex C/C++ parsing/decompression codecs (e.g., `7z.dll`, image/document decoders) into a dedicated ` windows-x64` console worker executable (e.g., `my_codec_worker.exe`, built without GUI/`USER32` dependencies where possible) and declare both the worker `.exe` and any private codec DLLs in `native.files` via repeated `native-manifest --file` flags. Every file is SHA-256 and PE32+ verified before activation.
2. **Enforce OS Containment with a Windows Job Object**:
   - Launch the worker suspended or directly bound to a dedicated Windows Job Object configured with:
     - `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (guarantees zero orphan worker processes if `pulse.exe` exits or crashes);
     - Bounded per-job memory (`JobObjectExtendedLimitInformation`), active-process count, and UI restrictions (`JOB_OBJECT_UILIMIT_*`);
     - `SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX` so worker faults fail fast without modal WER dialogs.
3. **Broker Scoped Handles via `extension_host::CapabilityBroker` Pattern (`DuplicateHandle`)**:
   - Instead of passing unconstrained ambient filesystem authority to parser logic, open source inputs in the coordinator with least privilege (`FILE_GENERIC_READ`, `FILE_SHARE_READ`, non-reparse validation where applicable) and pass scoped OS handles to the worker via `DuplicateHandle` (the `extension_host::CapabilityBroker` pattern).
   - For extraction or file generation, request a host-owned same-volume private directory via `PulseStagingV1::create` and restrict worker writes strictly to handles/paths rooted inside that staging directory.
4. **Bridge Cooperative Cancellation, Crash Containment & Staged Commit**:
   - Inside `PulseTasksV1` `work` (or `PulsePreviewV1` `provide`), poll `cancelled(context)` while awaiting framed pipe/IPC progress from the worker. On cancellation, signal the worker over IPC and terminate the Job Object if a grace timeout expires.
   - Validate all framed IPC responses against strict byte/entry budgets before forwarding progress (`ctx->report`) or preview results (`request->publish`).
   - If the worker process crashes (non-zero exit code / broken pipe), catch the failure cleanly in `plugin.dll`, release the staging handle (`PulseStagingV1::release`), report a structured diagnostic string via `ctx->report`, and return `PULSE_TASK_FAILED`—keeping `pulse.exe` completely unaffected. Only after the worker exits `0` and closes all handles does `plugin.dll` invoke `PulseStagingV1::commit` for user confirmation, conflict resolution, and undo/redo registration.

---

## 5. Rollback, Upgrade & Failure Recovery

### 5.1 Atomic Content-Addressed Upgrade, `previousRevision` Rollback & `gc`

- **Transactional Publication**: `pulse_ext.py install` and the in-app `store/install_transaction.h` acquire `.registry.lock`, write and flush immutable payloads (`binaries/<sha256>.dll` or `packages/<manifest-sha256>/<file>`) and `versions/<manifest-sha256>.json`, verify every digest, and commit by atomically replacing `registry.json`. Any interrupted or failed install leaves the active revision untouched.
- **Single-Step `previousRevision` Rollback**: Upgrading an installed package preserves its `enabled` state (when publisher continuity holds) and records the outgoing manifest digest in `previousRevision`. Running `pulse_ext.py --pulse HOST --json rollback <ID> --allow-native [--accept-capabilities]` re-validates the target manifest, detached signature, publisher continuity, capability delta, and all payload hashes before swapping `activeRevision` and `previousRevision`.
- **Garbage Collection (`gc`)**: `pulse_ext.py gc [--store STORE]` computes live reachability across every registered package's `{activeRevision, previousRevision}` under `.registry.lock` and removes unreferenced revisions and binaries, safely skipping any file currently locked by a running Pulse process (`gc.skipped`).

### 5.2 Publisher Signing Continuity & Key Rotation (`signature-policy.json` / `trusted_publishers.json`)

- **Trust Roots**: Store signature verification checks CNG RSA-PKCS1-SHA256 detached manifest signatures against `signature-policy.json` in the target store and curated/enterprise publisher catalogs (`trusted_publishers.json`).
- **Continuity Enforcement**:
  1. **No Silent Downgrade**: Upgrading a signed package with an unsigned package is rejected (`installed_publisher_mismatch`).
  2. **Cryptographic Key Rotation (`rotate`)**: Changing publisher keys for an existing package ID requires a rotation proof generated by `package_signing.py rotate` signing `Pulse.Extension.Publisher.Rotation\0v1\0<id>\0<oldPublisher>\0<newPublisher>` with the outgoing private key while the new public key is trusted in `signature-policy.json` / `trusted_publishers.json`.
  3. **Override Safety**: Using CLI `--allow-publisher-change` without a cryptographic `--rotation` proof forces `enabled: false` on the newly installed revision so a new publisher never silently inherits execution consent.

### 5.3 Hot-Swap Prohibition & `Draining` → `RestartRequired` Lifecycle

- **No In-Process DLL Hot-Replacement**: Windows native DLLs holding active threads, vtables, or UI callbacks cannot be safely hot-swapped in place. Installing an upgrade, rolling back, or changing trust/revocation for a package already loaded in a running Pulse process leaves the mapped startup revision pinned in memory and sets `PackageRuntimeSnapshot::restart_required = true` (`RestartRequired`), activating the new revision cleanly on next host restart.
- **Controlled Session Drain (`Inactive → Loaded → Active → Draining → Inactive`)**:
  - Disabling a package or closing an owner window transitions the session to `PackageRuntimeState::Draining`: the host immediately revokes command/UI tokens, cancels active `pulse.tasks` and `pulse.preview` requests, invokes `deactivate`, and waits for `callbacks_active` (all worker threads, preview callbacks, and in-flight UI dispatches) to drop to zero before calling `destroy` and `FreeLibrary`.
  - **Drain Stall Safety**: If a non-cooperative worker stalls past the drain threshold (e.g., `draining_ms >= 5000`), Pulse reports the stalled drain state (`Draining` / `RestartRequired`) and **refuses to `FreeLibrary` underneath the live thread**, preventing use-after-unload access violations until process exit.

### 5.4 Crash Attribution & Automatic Quarantine (`crash_guard` & Worker Thresholds)

- **In-Process Fault Attribution (`crash_guard`)**:
  - Before invoking `LoadLibraryExW`, `PulsePluginEntryV1`, or `activate`, `crash_guard::ActivationScope` writes `native-activating.txt` (`<id>\t<sha256>`) beside the store.
  - Once loaded, `crash_guard::TrackPackageModules` records the exact virtual-address ranges of the entry DLL and all loaded private sidecar DLLs declared in `native.files` (complemented by `crash_guard::CallScope` thread-local call breadcrumbs).
  - If an unhandled exception terminates Pulse during activation or with an instruction pointer inside a tracked plugin image, the crash handler writes `native-crashed.txt`. On the next normal startup (`crash_guard::ApplyAtStartup`), Pulse records `<id>\t<sha256>` in `crash-disabled.txt`, **automatically disables that exact build before any plugin loads**, and presents a one-time user diagnostic banner. Installing a fixed build (new `sha256`) or explicit user re-enablement clears the quarantine.
- **Out-of-Process Worker Fault Attribution & Auto-Disable Thresholds**:
  - For out-of-process workers (`pulse_7zip_worker` / `extension_host`), process crashes, IPC protocol violations, and Job memory/timeout kills are attributed directly to the owning package/request without crashing `pulse.exe`. Repeated consecutive worker crashes trip per-session/package failure thresholds, failing in-flight requests cleanly and requiring explicit user retry or package update rather than respawning runaway crash loops.
