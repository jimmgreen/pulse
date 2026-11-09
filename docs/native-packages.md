# Native DLL packages

Native packages load a trusted Windows x64 DLL inside the Pulse process. They have the process user's privileges; resource declarations are displayed at installation, not checked as per-call permissions. Review the source and binary provenance before explicitly consenting. `describe_extension_api`, manifest validation, SDK discovery and manifest generation never grant execution authority.

## Manifest contract

The manifest accepts apiVersion, id, name, resources and native. Legacy commands, toolbar, palette, workflows, panel and runtime fields are rejected. A template may omit native until native-manifest binds its DLL. Native package declarations use:

```json
"native": {
  "version": 1,
  "architecture": "windows-x64",
  "library": "plugin.dll",
  "sha256": "64 lowercase hexadecimal digits generated from the DLL"
}
```

The `native` object is closed: the four fields above are required; an optional `files` array declares 1–63 additional DLLs or resources. Each item is exactly `{ "path": "7z.dll", "sha256": "<64 lowercase hex>" }`. Paths must be flat ASCII filenames (letters, digits, dot, underscore, hyphen), start with a letter or digit, and contain at most 128 bytes. Trailing dots, Windows device names, manifest.json/package.json and case-insensitive duplicates (including the entry DLL) are rejected. Omit files for a single-DLL package; an empty array is rejected. `library` is one ASCII filename matching `[A-Za-z0-9][A-Za-z0-9._-]{0,123}\.dll`; paths, alternate streams, uppercase extensions and traversal are rejected. Windows device stems `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9` and `LPT1`–`LPT9` are rejected case-insensitively, including names such as `CON.extra.dll`. `sha256` is exactly 64 lowercase hexadecimal digits. JavaScript `runtime` and `runtime.invoke` have been removed and are rejected; executable packages use `native`. The DLL must be a regular non-reparse file, at most 64 MiB, with an x64 PE32+ DLL header. These structural checks do not prove that the DLL implements the ABI or that its code is trustworthy; activation additionally requires the public `PulsePluginEntryV1` ABI.

Multi-file packages support private DLL dependencies and resource files. Every declared file is hash-verified at installation, enable/rollback, host discovery and again before loading; DLL files also require an x64 PE header. The loader holds file and ancestor-directory handles until unload. Fully verified multi-file packages use `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`; no process-wide search path is changed. Native code remains trusted in-process code, not isolated.

## Author and install

Build a native SDK example or a plugin using the published `sdk/native/include/pulse` headers. Put the DLL in the desired package directory. The generation command reads an ordinary manifest template, computes the DLL hash, validates the result with the target host and atomically creates a new manifest beside the DLL. It never loads the DLL and refuses to overwrite an existing output:

```powershell
python tools/extensions/pulse_ext.py --pulse build/pulse.exe --json native-manifest examples/native/rename-panel/manifest.json --library C:/packages/rename/plugin.dll --output C:/packages/rename/manifest.json
python tools/extensions/pulse_ext.py --pulse build/pulse.exe --json install C:/packages/rename/manifest.json --allow-native
python tools/extensions/pulse_ext.py --pulse build/pulse.exe --json enable PACKAGE_ID --allow-native
```

Use the manifest's `id` for `PACKAGE_ID`. `--json` returns structured success/error objects, including generated manifest and binary hashes. The default store is `%LOCALAPPDATA%/Pulse/extensions`; add `--store C:/absolute/local/store` to every lifecycle command for an isolated store. Installation starts a new package disabled. Enabling is a separate explicit action. None of these authoring commands launches Pulse's normal UI or immediately invokes a plugin. Normal Pulse startup discovers this store; opening an enabled native package from the command palette activates its DLL. For an explicit store, `pulse_ext.py run --store STORE` uses the existing `--extension-store` host option and verifies enabled binaries before launching. It does not require an additional native startup flag or bypass prior installation/enable consent. Test instances use their isolated `PULSE_TEST_DATA_DIR/extensions` store and do not fall back to the user store.

Old archives containing declarative execution fields are rejected. Native packages use format v2 for a distributable archive containing exactly `manifest.json`, `package.json`, the entry DLL and all declared additional files. Native installation accepts either the archive or a manifest beside its DLL:

```powershell
python tools/extensions/pulse_ext.py --pulse build/pulse.exe --json pack C:/packages/rename/manifest.json --output C:/packages/rename.pulse-ext
python tools/extensions/pulse_ext.py --pulse build/pulse.exe --json verify C:/packages/rename.pulse-ext
python tools/extensions/pulse_ext.py --pulse build/pulse.exe --json install C:/packages/rename.pulse-ext --allow-native
```

The v2 `package.json` contains `format: "pulse-extension-local"`, `formatVersion: 2`, the manifest `apiVersion` and `id`, `manifestSha256` for the exact UTF-8 manifest bytes, and `native: {library, sha256, files?}` matching the native declaration. Unknown metadata or ZIP members are rejected. The manifest hash binds all native fields; every declared file must match its recorded hash. Signatures, when configured, cover the exact manifest bytes and therefore bind the declared file hashes. Existing signature policy enforcement still applies. `verify` returns `authorizationReview.nativeCode` and `nativeExecutionConsentRequired` without granting consent or loading the DLL.

Archive reads are bounded to 256 MiB + 256 KiB before ZIP parsing. A preflight bounds the directory to 2–66 members before allocating ZIP entry objects; ZIP64 and multidisk packages are rejected. The decompressed manifest, metadata and DLL limits are 64 KiB, 16 KiB and 64 MiB per file, with a 256 MiB total file budget. Only stored/deflated, unencrypted regular single-filename members are accepted. Duplicate names, directories, symlinks, traversal, alternate streams and extra files are rejected. Installation consumes the same immutable manifest and file snapshots validated from one archive read; it never extracts arbitrary ZIP paths or reopens the source archive after validation. Packing likewise hashes and packages the same captured file bytes.

## Signing and distributing a preview release

Sign the exact generated manifest after the DLL and dependencies are final. The detached signature binds all declared file hashes. It is distributed beside the `.pulse-ext` archive, not inserted as an extra ZIP member. Changing manifest whitespace or rebuilding a DLL requires regenerating the manifest, signature and package. This signature is separate from Windows Authenticode.

`package_signing.py` uses Windows CNG RSA-PKCS1-SHA256. Supply a 2048–4096-bit `RSAFULLPRIVATEBLOB` and matching `RSAPUBLICBLOB` exported by your signing system; PEM and certificate files are not accepted. The SDK does not provide a production key provisioning service. Keep the private key outside the SDK, plugin package and published artifacts. The publisher ID is the lowercase SHA-256 of the exact public blob. Verify that fingerprint through a trusted channel before adding it to an installation store.

From the extracted SDK root, replace the host, package, key paths and fingerprint below:

```powershell
python tools/extensions/package_signing.py sign C:/packages/rename/manifest.json --private-key C:/Signing/private.blob --output C:/packages/rename.sig.json
python tools/extensions/package_signing.py trust --store C:/PulseAuthor/signed-store --public-key C:/Signing/public.blob --fingerprint PUBLISHER_SHA256 --require-signed
python tools/extensions/pulse_ext.py --pulse C:/PulsePreview/pulse.exe --json verify C:/packages/rename.pulse-ext --signature C:/packages/rename.sig.json --policy C:/PulseAuthor/signed-store/signature-policy.json
python tools/extensions/pulse_ext.py --pulse C:/PulsePreview/pulse.exe --json install C:/packages/rename.pulse-ext --store C:/PulseAuthor/signed-store --signature C:/packages/rename.sig.json --allow-native
python tools/extensions/pulse_ext.py --pulse C:/PulsePreview/pulse.exe --json enable PACKAGE_ID --store C:/PulseAuthor/signed-store --allow-native
```

`verify` must report both integrity verification and a trusted signature. Trust does not activate a DLL. `--require-signed` applies to the whole chosen store; do not use a production store for this walkthrough. Rotate keys by trusting the new fingerprint, distributing and verifying a newly signed release, then using `untrust --store STORE --fingerprint OLD_SHA256`. Removing trust does not unload an already running DLL; restart the host.

Publish the immutable `.pulse-ext`, detached signature, public key/fingerprint, license notices, source/build instructions, matching SDK preview number and release notes through the author's chosen release channel. Include the tested host version and actual interaction/cleanup results. There is no SDK upload command, plugin marketplace or automatic external publication. Installation and rollback select immutable manifest revisions; changing an installed package requires a host restart. For local unsigned development use an isolated store without a required-signature policy, then perform signed-package verification before distribution.

## Upgrade, rollback and removal

Generate a new manifest for the rebuilt DLL, then call `install` again with the same package ID and `--allow-native`. Every native installation, enable and rollback requires this explicit consent, including installation of a new DLL hash. `--accept-capabilities` remains a separate acknowledgement when a manifest upgrade adds declared capabilities; it does not substitute for native execution consent.

```powershell
python tools/extensions/pulse_ext.py --pulse build/pulse.exe --json install C:/packages/rename-v2/manifest.json --allow-native
python tools/extensions/pulse_ext.py --pulse build/pulse.exe --json rollback PACKAGE_ID --allow-native
python tools/extensions/pulse_ext.py --json disable PACKAGE_ID
python tools/extensions/pulse_ext.py --json uninstall PACKAGE_ID
```

Upgrades preserve the package's enabled state and retain one `previousRevision` pointer. Rollback swaps active and previous manifest revisions after validating the target signature, identity and every file hash; a missing or damaged target file leaves the registry unchanged. Restart Pulse after changing an enabled native package: a loaded DLL is not hot-replaced. Deactivation/shutdown must cancel and join plugin workers and remove callbacks before unloading, as required by the native ABI.

Disable and uninstall do not require an intact DLL, so a damaged package can be removed from activation. Disabled missing or corrupt packages do not block management of other packages. Uninstall removes the registry entry but retains immutable revisions and binaries; it does not attempt to delete a loaded DLL.

CLI enable/disable changes persistent registry state and takes effect on the next startup. Palette enable/disable changes the current process only; it does not persist to the registry. Each `(owner window, package)` has a separate native session. Two active packages in one window can be opened, closed and disabled independently. Loaded modules retain their startup revision until restart, including after an external upgrade or rollback.

If automatic default-store discovery fails, Pulse starts with no packages from that transaction and keeps its core UI available. It records `extension-startup-status.json` beside the `extensions` directory, with `status` (`disabled` or `loaded`), `error`, `packageCount` and `timestampFileTime`. Repairing the store and restarting clears the old error on successful loading. An explicitly supplied invalid `--extension-store` remains a strict startup error (exit 2). The diagnostic file is a startup result, not a live list of DLL activation states.

## Store layout and transaction

The existing `formatVersion: 1` registry remains unchanged. A package points to the SHA-256 of the exact stored manifest bytes:

```text
extensions/
  registry.json
  .registry.lock
  versions/<manifest-sha256>.json
  versions/<manifest-sha256>.sig.json  (optional)
  binaries/<binary-sha256>.dll       (single DLL)
  packages/<manifest-sha256>/<name> (multi-file package)
```

Multi-file installation writes and flushes every file into a new staging directory, verifies the whole set, then publishes the complete revision directory before committing the registry. Upgrade and rollback select an entire immutable package revision, including licenses and resources.

Mutation takes the existing exclusive store lock. Installation reads and verifies a bounded DLL snapshot from the manifest directory, flushes a temporary binary, then atomically publishes it under its content hash without replacing an existing file. It publishes the immutable manifest before atomically replacing the registry. A failed registry commit leaves the previous active revision intact; unreferenced immutable files may remain. Existing incorrect content at a hash path is rejected, never overwritten. A concurrent mutation reports `store_busy`; retry after the other operation finishes.

## AI authoring prompt

```text
Create a Pulse native package using only the public SDK headers and published interfaces.
Target Windows x64 and export PulsePluginEntryV1. Declare every private DLL and resource with repeated native-manifest --file arguments.
Use flat filenames and include dependencies and required licenses in the same package.
Use only native executable packages; no JavaScript runtime field. Generate native.library and
native.sha256 with pulse_ext.py native-manifest, then validate against the target host.
Return the DLL path, manifest path, binary hash and explicit install/enable commands.
Do not run those lifecycle commands without the user's explicit native-code consent.
Discovery, validation and a successful build are not execution or UI verification.
```

## Directed verification

`python -m unittest discover -s tools/extensions -p test_native_package_store.py` exercises store and CLI behavior in temporary directories using synthetic PE headers and a mocked manifest validator. `test_native_archive.py` additionally verifies v2 pack/verify/install roundtrips, archive replacement after validation, DLL replacement during packing, metadata/hash binding, consent, rollback and malformed ZIP rejection. These tests do not load a real DLL or assert native UI/lifecycle correctness; those require separate tests against a built host and plugin.

## Archive reference sample

The source-only `sdk/native/samples/archive` loads the real 7-Zip engine and registers extract/add-to-archive command entries using the selected file types. The extraction commands now use host-owned same-volume staging and commit; P1.5 real-host acceptance has passed. The add-to-archive command remains a labelled demonstration and does not create archives. The verify-archive task now performs real extraction to a private temporary directory, reports progress and removes temporary output; its installed-package host acceptance has passed. The staged publication path passed P1.5 acceptance independently of that verification evidence. Supply an x64 7-Zip distribution with `7z.dll`, `License.txt` and `NOTICE.txt`; SDK bundles do not redistribute that binary. Build with `cmake -S sdk/native/samples/archive -B archive-build -G Ninja -DPULSE_7ZIP_DIR=C:/7zip`, then `cmake --build archive-build` in an x64 MSVC environment. Generate its manifest beside the DLL:

```powershell
python tools/extensions/pulse_ext.py --pulse build/pulse.exe --json native-manifest sdk/native/samples/archive/manifest.json --library archive-build/pulse_archive.dll --file archive-build/7z.dll --file archive-build/License.txt --file archive-build/NOTICE.txt --output archive-build/manifest.json
```

`--file` is repeatable and never copies or changes author files. MCP `native_package_stage` accepts a `files` array of workspace paths and copies them into its new staging directory before generating the manifest. `test_native_multifile.py` covers transaction, exact archive members, hashes and budgets; `test_native_multifile_ui.py --pulse HOST --sample-build ARCHIVE_BUILD --output NEW_DIRECTORY` exercises the real sample through install, upgrade, rollback and application capture.

### Preview provider source packaging

The source SDK includes `pulse/preview.h` and the archive sample's `archive_reader.h`, `archive_reader.cpp`, and `archive_reader_test.cpp`, alongside its CMake file. `-DPULSE_ARCHIVE_READER_TEST=ON` enables the standalone reader test; supply real archive fixtures as required by that test. The 7-Zip DLL and license notices remain explicit external inputs and are included in the generated package file/hash inventory, not redistributed in the source SDK.

`pulse.preview` is available in the current SDK preview 3. Registration/unregistration runs on the owner UI thread; provider callbacks run on host workers (at most two per instance), poll cancellation, and synchronously publish borrowed archive, premultiplied-BGRA bitmap, or UI-model data before returning. The host retains the DLL until callbacks finish. Do not retain a request or call UI APIs on those workers. `PULSE_PREVIEW_UI_MODEL` borrows `PulseUiModelTree` through `model`, which the host converts and copies on the worker. Quick Preview and Details render existing docked controls, with local state/actions and limits of 128 nodes, depth 8, and 64 KiB. Native command callbacks are unavailable (`command_count > 0` returns UNAVAILABLE); data sources, context bindings and data-operation actions remain unsupported. The P1.3 host acceptance covers both preview surfaces, light/dark rendering, rapid selection, local model actions and bitmap replacement. See the implementation roadmap for evidence. The verify-archive source now extracts to a private temporary directory and cleans it; P1.4 installed-package host acceptance has passed, and publishing output through staged commit passed P1.5 acceptance.

Provider unregistration is two-phase: `unregister_provider` immediately retires the provider and cancels its work, preventing new requests. It returns `PULSE_ERROR_STATE` while callbacks drain. Keep userdata alive and retry on the UI thread until `PULSE_OK`, or retain it through `destroy`; STATE is not permission to free callback state.

### Task source packaging (P1.4 host acceptance passed)

The source SDK includes `pulse/tasks.h` and the archive sample's `archive_tasks.h` / `archive_tasks.cpp`. Task descriptors carry copied title/type (1–128 UTF-8 bytes), a worker callback and optional UI cancellation callback. The host runs at most two workers with 16 retained tasks; terminal records can be released or evicted at capacity. Progress carries percent (0–100 or UINT32_MAX indeterminate), completed/total items and bytes, current_item (32768 UTF-8 bytes) and error (4096 bytes), copied during report.

The worker explicitly returns Completed/Partial/Failed/Cancelled. Cancellation is cooperative, never a rollback guarantee; reporting stays valid during cancellation cleanup until work returns, and a cleanup failure remains Failed. Keep userdata until terminal get/release or destroy. The verify-archive source performs real extraction into a private process/task temporary directory and cleans it on completion, cancellation or failure; cleanup failure remains Failed with diagnostic text. It does not publish output files. P1.4 installed-package host acceptance used a real 2 GiB extraction and covered success, failure, cancellation at 18 percent, disable cleanup, and the existing operation window/progress pill. Progress counts successful WriteFile bytes, including cancellation cleanup diagnostics. P1.5 staged extraction is implemented through the public interface and real-host acceptance has passed; compression remains a labelled demonstration. P1.5 installed, upgraded and rolled-back packages each passed the staged publication flow.

### Staged extraction lifecycle (P1.5 accepted)

The archive sample queries public `pulse.staging` during entry. `extract-here` selects the archive parent lexically; `extract-to` uses the host directory chooser on the owner UI thread and copies its callback-borrowed UTF-8 result. Cancelling that chooser starts no task. Extraction commands register only `.7z` and `.rar`, matching the reader.

The task worker requests a stage asynchronously, retains its Job userdata through the creation callback even after cancellation, and writes only into the host-created directory. After the reader closes its output handles it submits commit and polls every 10ms. The host owns final confirmation, ordinary conflict handling, file operations and undo. The sample preserves Completed/Partial/Failed/Cancelled and cancellation waits for commit termination before release. Extraction occupies 0–90 percent; publication remains at 90 until actual successful commit.

`release` accepts asynchronous cleanup; it is not proof that the directory has already disappeared. Host shutdown drains writers and cleanup before unloading. Cancellation does not roll back already published files. The verification command retains its private-temporary-directory behavior. P1.5 real-host evidence covers chooser/confirmation cancellation, publication, Replace conflict, Locate, undo/redo and staging cleanup across installation, upgrade and rollback. Writer shutdown and cleanup ordering are covered separately by staging lifecycle tests.

Publication currently supports local absolute paths, including long paths; UNC and network destinations are unsupported. The host publishes via same-volume Move and retains the old destination before Replace. Generated-output undo/redo is session-only and is not serialized. If a redo destination is occupied, restoration fails conservatively without consuming its recovery item; retry after resolving the collision instead of silently choosing another filename while retaining the old history path.

Stage cleanup and old-version retention are separate. Normal stage release does not delete `.pulse-generated-original-*` backups holding the user's pre-Replace content. These backups may remain beside the target after history eviction or application exit for manual recovery. Automatic backup garbage collection and crash-restored undo history are not promised. A retained old-version backup does not itself indicate failed stage cleanup.

Only `choose_destination` is UI-thread-only and calls back synchronously; `get` invokes its borrowed-snapshot callback synchronously on its calling thread. Other staging calls may originate on workers, and create/commit/release filesystem work runs on host workers. All callback views must be copied before return. P1.5 real-host acceptance has passed.

P1.5 evidence: 40 staging tests and 28 generated-output history tests passed. See `build-extensions/p15-staging-host-final/profile` for the three host reports and application-rendered confirmation/conflict screenshots. Generated redo records the exact recycled generation rather than resolving an ambiguous original path; an unresolved receipt blocks repeat deletion of a subsequently recreated file.

### Embedded settings page (P1.6 accepted)

The SDK includes `pulse/settings_pages.h` and archive_settings.h/.cpp. `pulse.settings.pages` V1 creates at most one existing UI-model page per package session under host extension settings. All calls/callbacks run on the owner UI thread; create copies title/model synchronously. query returns dedicated pulse.ui.model and pulse.ui.events tables for the page's panel ID, independent of the main engine panel; mount/data are unavailable. remove/stop invalidates IDs/subscriptions; retain callback userdata through callback return and copy borrowed values. Contexts remain valid through session destroy.

The sample loads package resources on entry and atomically saves one versioned `archive.preferences` value through existing settings_get/settings_set (1024 UTF-8 byte maximum). It announces Saved only on completion. Version conflicts reload the latest version while retaining the draft and require explicit Save retry. Destruction cancels pending resource callbacks; committed settings are not rolled back.

Preferences cover extraction destination (empty means archive parent), native Browse, and future 7z/zip compression format and Store/Fast/Normal/Maximum level. extract-to initializes its chooser from the saved destination; extract-here is unchanged. Compression is still unimplemented. P1.6 real-host layout, persistence/restart, conflict and lifecycle acceptance has passed.

```text
Build an embedded page using only settings_pages.h and the existing UI-model controls.
Use dedicated query contexts and keep the engine panel independent.
Persist with resource version CAS; copy callback values, retain userdata, and cancel pending requests on teardown.
Show loading/error/retry states; never report Saved before completion or overwrite a version conflict automatically.
Mark compression choices as future preferences and verify restart persistence and host UI separately from compilation.
```

P1.6 acceptance: 23 settings-page adapter checks and the public-header ABI layout check passed. `build-extensions/p16-settings-host-verified` records installed/upgraded/rolled-back host runs, including persistence across processes, real Save, optimistic-version conflict/retry, draft retention, focus, invalid paths, stop/reload, and narrow scrolling to Save. Application-rendered screenshots cover the entry, light page, dark narrow page and scrolled actions. The archive manifest discloses `settings.own`; compression preferences remain future-only.


## P1.7 C++20 开发便利层（2026-11-08）

包含 `pulse/client.hpp` 即可使用拥有数据的 `ModelNode`、`ModelRow`、`ModelColumn`、`ModelTree`，由构建器填写 present 和数组视图。控件仍是 Pulse 原生模型，宿主保留属性、可访问性标签及状态绑定验证。输入框、下拉框必须提供 text 标签；布局中另有文字标签不能替代它。

```cpp
using namespace pulse::sdk;
using N = PulseUiModelNode;
auto tree = ModelTree(ModelNode(PULSE_UI_MODEL_STACK, "root").Children({
    ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "query")
        .String(&N::text, "Search").String(&N::state, "query")}))
    .State("query", "");
// model 为 Host::Query 得到的 PulseUiModelV1。
uint64_t panel = 0;
auto status = model.create(model.context, View("Example"), &tree.Build(), &panel);
```

构建器拥有字符串及子节点；Build 返回借用视图，只可在所属对象下次修改、Build、移动或销毁之前使用，宿主 create/update 同步复制。复制或移动后重新 Build，禁止缓存旧指针。显式空字符串、false、0、空数组与未设置属性不同。

`Subscription` 管理 UI 订阅，`CommandRegistration::Register` 接受命令描述符与 lambda，移动句柄转移注销责任；回调可自行 Reset，异常不会跨 C ABI。`DispatchQueue` 支持工作线程投递 UI lambda；Close 不负责 join，销毁前必须结束生产线程。

`TaskGroup::Start(title, type, work, on_cancel, &id)` 把 lambda 交给宿主工作线程，out 必须非空；UI 线程调用 Collect 回收已成功 release 的终态任务。CancelAll 只请求取消，不等待完成。未知任务 ID、释放失败仍保留捕获对象；必须等 Collect 清空，或宿主排空工作线程和取消回调后的插件 destroy 才销毁 TaskGroup，禁止回调内销毁组。所有 STL 对象和捕获分配留在 DLL，C ABI 未改变。

压缩包样例的设置树、引擎状态树、UI 订阅、命令和后台任务已迁移；目录工作台的模型与订阅也已迁移。定向 wrapper_test 共 28 项通过。真实宿主证据：`build-extensions/p17-settings-host-final`（安装/升级/回退与设置持久化）、`build-extensions/p17-archive-host`（真实 2 GiB 解压取消、停用排空、发布/冲突/撤销重做）、`build-extensions/p17-workbench-host`（18 项，通过应用内截图核对）。

P1.8 UI API 收敛已完成（SDK 预览3）；P1.9 旧压缩下载包已移除；压缩创建仍未实现，格式和级别是未来功能的持久化偏好。本阶段没有引入 JavaScript 运行时。
