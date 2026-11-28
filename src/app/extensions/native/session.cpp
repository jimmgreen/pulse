#include "session.h"
#include "module.h"
#include "crash_guard.h"
#include "ui_adapter.h"
#include "ui_model.h"
#include "files_adapter.h"
#include "resources_adapter.h"
#include "dispatch_adapter.h"
#include "context_adapter.h"
#include "commands_adapter.h"
#include "previews_adapter.h"
#include "tasks_adapter_host.h"
#include "staging_adapter_host.h"
#include "settings_pages_adapter.h"
#include "settings_pages_host.h"
#include "dialogs_adapter.h"
#include "diagnostics_adapter.h"
#include "icons_adapter.h"
#include "pickers_adapter.h"
#include "workbench_adapter.h"
#include "tray_adapter.h"
#include "search_adapter.h"
#include "shell_adapter.h"
#include "associations_adapter.h"
#include "associations_adapter_host.h"
#include "vfs_adapter.h"
#include "columns_adapter.h"
#include "statusbar_adapter.h"
#include "extcapsules_adapter.h"
#include "fileevents_adapter.h"
#include "vfs_adapter_host.h"
#include "directories_adapter.h"
#include "fileops_adapter.h"
#include "../../../../sdk/native/include/pulse/ui_events.h"
#include "../../../../sdk/native/include/pulse/ui_data.h"
#include "../../../../sdk/native/include/pulse/ui_mount.h"
#include "../runtime.h"
#include "../command_projection_cache.h"
#include "../../app_runtime.h"
#include "../../app_input.h"
#include "../../app_messages.h"
#include "../../session.h"
#include "../../../common/localization.h"
#include "../../../common/runtime_log.h"
#include "../../../ui/ui_renderer.h"
#if __has_include("pulse_version.h")
#include "pulse_version.h"
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <psapi.h>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions::native {
namespace {
constexpr UINT kWakeMessage = WM_APP + 1;
constexpr UINT_PTR kSessionTimerId = 1;
constexpr UINT kSessionTimerPeriodMs = 30;
constexpr double kSlowStartThresholdMs = 100.0;
constexpr double kSlowUiCallbackThresholdMs = 50.0;
constexpr uint64_t kStalledDrainRestartMs = 5000;

#if defined(PULSE_VERSION_MAJOR) && defined(PULSE_VERSION_MINOR) && defined(PULSE_VERSION_PATCH)
constexpr uint64_t kHostVersionCode =
    static_cast<uint64_t>(PULSE_VERSION_MAJOR) * 1000000ULL +
    static_cast<uint64_t>(PULSE_VERSION_MINOR) * 1000ULL +
    static_cast<uint64_t>(PULSE_VERSION_PATCH);
#else
constexpr uint64_t kHostVersionCode = 0;
#endif

#ifdef PULSE_SDK_PREVIEW_NUMBER
constexpr uint64_t kSdkPreviewNumber = static_cast<uint64_t>(PULSE_SDK_PREVIEW_NUMBER);
#else
constexpr uint64_t kSdkPreviewNumber = static_cast<uint64_t>(PULSE_SDK_PREVIEW);
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
constexpr uint64_t kHostArchitectureCode = 0xAA64u; // arm64 (IMAGE_FILE_MACHINE_ARM64)
#elif defined(_M_X64) || defined(__x86_64__)
constexpr uint64_t kHostArchitectureCode = 0x8664u; // x64 (IMAGE_FILE_MACHINE_AMD64)
#elif defined(_M_IX86) || defined(__i386__)
constexpr uint64_t kHostArchitectureCode = 0x014Cu; // x86 (IMAGE_FILE_MACHINE_I386)
#else
constexpr uint64_t kHostArchitectureCode = 0u;
#endif

std::atomic<uint64_t> next_session_id{0};

uint64_t QpcFrequency() noexcept {
    static const uint64_t frequency = []() noexcept -> uint64_t {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0
            ? static_cast<uint64_t>(value.QuadPart)
            : 0;
    }();
    return frequency;
}

double ElapsedMs(LONGLONG before, LONGLONG after) noexcept {
    const uint64_t frequency = QpcFrequency();
    if (!frequency || after <= before) return 0.0;
    return 1000.0 * static_cast<double>(after - before) / static_cast<double>(frequency);
}

// Sanitizes fixed non-sensitive ASCII identifiers (package id, digest prefix,
// publisher fingerprint) into wide text; never accepts user paths or settings.
std::wstring AsciiIdentifierWide(std::string_view value, std::wstring_view fallback) {
    if (value.empty()) return std::wstring(fallback);
    std::wstring out;
    out.reserve(value.size());
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-') {
            out.push_back(static_cast<wchar_t>(ch));
        } else {
            out.push_back(L'_');
        }
    }
    return out;
}

std::wstring ResolvePublisherFingerprint(size_t package) {
    const std::string publisher = PackagePublisherId(package);
    if (publisher.empty()) return L"unsigned";
    return AsciiIdentifierWide(publisher, L"unsigned");
}

std::wstring ResolvePackageRevision(const Manifest* manifest, std::string_view binding_revision) {
    if (!binding_revision.empty()) {
        const auto prefix = binding_revision.substr(0, (std::min)(binding_revision.size(), size_t{12}));
        return AsciiIdentifierWide(prefix, L"unversioned");
    }
    if (manifest && manifest->native && !manifest->native->sha256.empty()) {
        const std::string_view sha(manifest->native->sha256);
        const auto prefix = sha.substr(0, (std::min)(sha.size(), size_t{12}));
        return AsciiIdentifierWide(prefix, L"unversioned");
    }
    return L"unversioned";
}

std::string EffectiveLanguageTag() {
    switch (l10n::effective_language()) {
    case l10n::Language::ZhTW: return "zh-TW";
    case l10n::Language::EnUS: return "en-US";
    case l10n::Language::ZhCN:
    case l10n::Language::System:
    default: return "zh-CN";
    }
}

std::wstring ResolveProjectionDataRoot(const AppState& owner) {
    if (owner.isolatedTest) {
        wchar_t value[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", value, ARRAYSIZE(value));
        if (!size || size >= ARRAYSIZE(value) || !std::filesystem::path(value).is_absolute()) return {};
        return std::wstring(value, size);
    }
    return app::GetPulseDataDir();
}

class SlowUiCallbackScope final {
public:
    SlowUiCallbackScope(std::string_view package_id, uint64_t session_id, std::wstring_view phase) noexcept
        : package_id_(package_id), session_id_(session_id), phase_(phase) {
        QueryPerformanceCounter(&start_);
    }
    ~SlowUiCallbackScope() noexcept {
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        const double duration_ms = ElapsedMs(start_.QuadPart, end.QuadPart);
        if (duration_ms <= kSlowUiCallbackThresholdMs) return;
        try {
            const std::wstring wide_pkg = AsciiIdentifierWide(package_id_, L"unknown");
            diagnostics::runtime::TextEvent(
                "native_extension_slow_ui_callback",
                {{"package_id", wide_pkg},
                 {"phase", phase_}},
                {{"session_id", session_id_},
                 {"duration_ms", static_cast<uint64_t>(duration_ms + 0.5)}},
                diagnostics::runtime::Level::Warning);
        } catch (...) {}
    }
    SlowUiCallbackScope(const SlowUiCallbackScope&) = delete;
    SlowUiCallbackScope& operator=(const SlowUiCallbackScope&) = delete;

private:
    std::string_view package_id_;
    uint64_t session_id_ = 0;
    std::wstring_view phase_;
    LARGE_INTEGER start_{};
};

void EmitSessionTelemetry(std::string_view package_id,
                          std::wstring_view publisher_fingerprint,
                          std::wstring_view revision,
                          uint64_t session_id,
                          std::wstring_view phase,
                          uint64_t duration_ms,
                          PulseStatus result_code,
                          uint64_t pending_callbacks,
                          diagnostics::runtime::Level level = diagnostics::runtime::Level::Info) noexcept {
    try {
        const std::wstring wide_pkg = AsciiIdentifierWide(package_id, L"unknown");
        // runtime_log TextEvent caps text_fields at 4 and numeric fields at 12
        // (and writes the string host version at top-level "version").
        diagnostics::runtime::TextEvent(
            "native_extension_session",
            {{"package_id", wide_pkg},
             {"publisher_fingerprint", publisher_fingerprint.empty() ? L"unsigned" : publisher_fingerprint},
             {"revision", revision.empty() ? L"unversioned" : revision},
             {"phase", phase}},
            {{"host_version", kHostVersionCode},
             {"sdk_preview", kSdkPreviewNumber},
             {"architecture", kHostArchitectureCode},
             {"session_id", session_id},
             {"duration_ms", duration_ms},
             {"result_code", static_cast<uint64_t>(static_cast<uint32_t>(result_code))},
             {"pending_callbacks", pending_callbacks}},
            level);
    } catch (...) {}
}

struct WakeSignal {
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> queued{false};

    void Request() noexcept {
        const HWND target = window.load(std::memory_order_acquire);
        if (!target) return;
        bool expected = false;
        if (!queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
        if (!PostMessageW(target, kWakeMessage, 0, 0))
            queued.store(false, std::memory_order_release);
    }
};

struct Session : std::enable_shared_from_this<Session> {
    uint64_t session_id = next_session_id.fetch_add(1, std::memory_order_relaxed) + 1;
    HWND owner = nullptr, timer = nullptr;
    size_t package = 0;
    std::string package_id;
    std::wstring publisher_fingerprint = L"unsigned";
    std::wstring revision_tag = L"unversioned";
    std::wstring package_dir;
    std::string projection_revision;
    std::wstring projection_root;
    bool projection_eligible = false;
    bool projection_dirty = false;
    ULONGLONG projection_dirty_since = 0;
    std::optional<CommandProjectionSnapshot> last_persisted_projection;
    DWORD thread = GetCurrentThreadId();
    bool closing = false;
    bool close_failed = false;
    mutable bool drain_stalled_reported = false;
    bool tracked = false; // image registered with crash_guard
    bool timer_armed = false;
    ULONGLONG closing_since_tick = 0;
    double load_ms = 0, activate_ms = 0;
    uint64_t private_before = 0, private_after = 0;
    std::shared_ptr<WakeSignal> wake_signal = std::make_shared<WakeSignal>();
    std::unique_ptr<UiAdapter> ui;
    std::unique_ptr<FilesAdapter> files;
    std::unique_ptr<ResourcesAdapter> resources;
    std::unique_ptr<DispatchAdapter> dispatch;
    std::unique_ptr<ContextAdapter> context;
    std::unique_ptr<CommandsAdapter> commands;
    std::unique_ptr<PreviewsAdapter> previews;
    std::unique_ptr<TasksAdapter> tasks;
    std::unique_ptr<StagingAdapter> staging;
    std::unique_ptr<SettingsPagesAdapter> settings_pages;
    std::unique_ptr<DialogsAdapter> dialogs;
    std::shared_ptr<DiagnosticsSink> diagnostics_sink = std::make_shared<DiagnosticsSink>();
    std::unique_ptr<DiagnosticsAdapter> diagnostics;
    std::unique_ptr<PickersAdapter> pickers;
    std::unique_ptr<WorkbenchAdapter> workbench;
    std::unique_ptr<TrayAdapter> tray;
    std::unique_ptr<SearchAdapter> search;
    std::unique_ptr<ShellAdapter> shell;
    std::unique_ptr<AssociationsAdapter> associations;
    std::unique_ptr<VfsAdapter> vfs;
    std::unique_ptr<ColumnsAdapter> columns;
    std::unique_ptr<StatusBarAdapter> statusbar;
    std::unique_ptr<ExtCapsulesAdapter> extcapsules;
    std::unique_ptr<FileEventsAdapter> fileevents;
    std::unique_ptr<IconsAdapter> icons;
    std::unique_ptr<DirectoriesAdapter> directories;
    std::unique_ptr<FileOpsAdapter> fileops;
    std::unique_ptr<Module> module;
    CallerBinding caller;
    explicit Session(CallerBinding value) : caller(std::move(value)) {}
    // Runs before the members: the image is forgotten before ~Module frees it.
    ~Session() { if (tracked) crash_guard::ForgetModule(package); }
    void RequestWake() const noexcept {
        if (wake_signal) wake_signal->Request();
    }
    void RefreshTrackedModules() const {
        if (!tracked) return;
        const auto* manifest = PackageManifest(package);
        crash_guard::RefreshPackageModules(package, package_dir,
            manifest && manifest->native ? &*manifest->native : nullptr);
    }
    uint64_t PendingCallbackCount() const noexcept {
        uint64_t count = 0;
        if (module && module->Invoking()) ++count;
        if (diagnostics && diagnostics->CallbackActive()) ++count;
        if (pickers && pickers->CallbackActive()) ++count;
        if (workbench && workbench->CallbackActive()) ++count;
        if (tray && tray->CallbackActive()) ++count;
        if (search && search->CallbackActive()) ++count;
        if (shell && shell->CallbackActive()) ++count;
        if (associations && associations->CallbackActive()) ++count;
        if (vfs && vfs->CallbackActive()) ++count;
        if (columns && columns->CallbackActive()) ++count;
        if (statusbar && statusbar->CallbackActive()) ++count;
        if (fileevents && fileevents->CallbackActive()) ++count;
        if (dialogs && dialogs->CallbackActive()) ++count;
        if (settings_pages && settings_pages->InCallback()) ++count;
        if (tasks && tasks->CallbackActive()) ++count;
        if (staging && staging->CallbackActive()) ++count;
        if (previews && previews->CallbackActive()) ++count;
        if (commands && commands->CallbackActive()) ++count;
        if (ui && ui->InCallback()) ++count;
        if (files && files->CallbackActive()) ++count;
        if (resources && resources->CallbackActive()) ++count;
        if (dispatch && dispatch->CallbackActive()) ++count;
        if (context && context->CallbackActive()) ++count;
        if (directories && directories->CallbackActive()) ++count;
        return count;
    }
    bool CallbacksActive() const noexcept {
        return PendingCallbackCount() != 0;
    }
    void EmitLifecycle(std::wstring_view phase,
                       double duration_ms,
                       PulseStatus result_code,
                       diagnostics::runtime::Level level = diagnostics::runtime::Level::Info) const noexcept {
        const uint64_t rounded_ms = duration_ms > 0.0 ? static_cast<uint64_t>(duration_ms + 0.5) : 0;
        EmitSessionTelemetry(package_id,
                             publisher_fingerprint,
                             revision_tag,
                             session_id,
                             phase,
                             rounded_ms,
                             result_code,
                             PendingCallbackCount(),
                             level);
    }
    void CheckDrainStalled() const noexcept {
        if (!closing || drain_stalled_reported) return;
        const ULONGLONG now = GetTickCount64();
        const uint64_t draining_ms = now >= closing_since_tick ? now - closing_since_tick : 0;
        if (!close_failed && draining_ms < kStalledDrainRestartMs) return;
        drain_stalled_reported = true;
        EmitLifecycle(L"drain_stalled",
                      static_cast<double>(draining_ms),
                      close_failed ? PULSE_ERROR_STATE : PULSE_ERROR_UNAVAILABLE,
                      diagnostics::runtime::Level::Warning);
    }
    bool NeedsTimer() const noexcept {
        if (!timer) return false;
        if (closing) return true;
        if (staging && staging->HasPendingWork()) return true;
        if (context && context->HasSubscriptions()) return true;
        if (workbench && workbench->HasSubscriptions()) return true;
        if (tray && tray->HasSubscriptions()) return true;
        if (search && search->HasPendingWork()) return true;
        if (shell && shell->HasPendingWork()) return true;
        if (associations && associations->HasPendingWork()) return true;
        if (tasks && tasks->CallbackActive()) return true;
        if (directories && directories->HasPendingWatchStart()) return true;
        if (ui && (ui->IsSidebar() || ui->IsWorkspace() || IsWindow(ui->Window()))) return true;
        if (projection_eligible && projection_dirty) return true;
        return false;
    }
    void FlushCommandProjection(bool force) noexcept {
        try {
            if (!projection_eligible || !commands || projection_revision.empty()) return;
            if (!force && !projection_dirty) return;
            CommandProjectionSnapshot snap;
            snap.package_id = package_id;
            snap.revision = projection_revision;
            snap.language = EffectiveLanguageTag();
            snap.commands = commands->ExportProjection();
            const auto* manifest = PackageManifest(package);
            const bool declared_commands = manifest && manifest->command_contributions_declared;
            projection_dirty = false;
            if (snap.commands.empty() && !declared_commands && !last_persisted_projection) return;
            if (last_persisted_projection && *last_persisted_projection == snap) return;
            command_projection::PublishLive(snap);
            last_persisted_projection = snap;
            if (!projection_root.empty()) {
                if (auto* app = GetAppState(owner)) {
                    app->worker.EnqueueIo([root = projection_root, snapshot = std::move(snap)] {
                        (void)WriteCommandProjectionFile(root, snapshot);
                    });
                }
            }
        } catch (...) {}
    }
    void UpdateTimer() noexcept {
        if (!timer) {
            timer_armed = false;
            return;
        }
        const bool needed = NeedsTimer();
        if (needed && !timer_armed) {
            timer_armed = SetTimer(timer, kSessionTimerId, kSessionTimerPeriodMs, nullptr) != 0;
        } else if (!needed && timer_armed) {
            KillTimer(timer, kSessionTimerId);
            timer_armed = false;
        }
    }
    void Stop() {
        if (closing) return;
        FlushCommandProjection(true);
        closing = true;
        closing_since_tick = GetTickCount64();
        LARGE_INTEGER before{}, after{};
        QueryPerformanceCounter(&before);
        {
            const SlowUiCallbackScope slow_scope(package_id, session_id, L"deactivate_start");
            if (pickers) pickers->Close();
            if (dialogs) dialogs->Close();
            if (workbench) workbench->Close();
            if (tray) tray->Close();
            if (search) search->Close();
            if (shell) shell->Close();
            if (associations) associations->Close();
            if (vfs) vfs->Close();
            if (columns) columns->Close();
            if (statusbar) statusbar->Close();
            if (extcapsules) extcapsules->Close();
            if (fileevents) fileevents->Close();
            if (diagnostics) diagnostics->Close();
            if (settings_pages) settings_pages->Close();
            if (commands) commands->Close();
            if (icons) icons->Close();
            if (previews) previews->Close();
            if (staging) staging->Close();
            if (tasks) tasks->Close();
            if (ui) ui->Stop();
            if (fileops) fileops->Close();
            if (files) files->Close();
            if (resources) resources->Close();
            if (dispatch) dispatch->Close();
            if (context) context->Close();
            if (directories) directories->Close();
        }
        QueryPerformanceCounter(&after);
        EmitLifecycle(L"deactivate_start", ElapsedMs(before.QuadPart, after.QuadPart), PULSE_OK);
    }
    bool Drain() {
        if (!closing) return false;
        if (CallbacksActive()) {
            CheckDrainStalled();
            return false;
        }
        if (module) {
            PulseStatus close_status = PULSE_ERROR_STATE;
            {
                const SlowUiCallbackScope slow_scope(package_id, session_id, L"deactivate");
                close_status = module->Close();
            }
            if (close_status != PULSE_OK) {
                close_failed = true;
                CheckDrainStalled();
                return false;
            }
        }
        if (staging) {
            staging->CleanupRemaining();
            if (staging->CallbackActive()) {
                CheckDrainStalled();
                return false;
            }
        }
        if (tracked) { crash_guard::ForgetModule(package); tracked = false; }
        const ULONGLONG now = GetTickCount64();
        const uint64_t drain_ms = now >= closing_since_tick ? now - closing_since_tick : 0;
        EmitLifecycle(L"drain_complete", static_cast<double>(drain_ms), PULSE_OK);
        return true;
    }
};
using SessionKey = std::pair<HWND, size_t>;
// Retire normally destroys drained sessions. At process shutdown a cooperative
// provider may still be returning: CRT static destruction must not unload its
// DLL beneath that worker. The OS reclaims any remaining sessions on exit.
auto& sessions = *new std::map<SessionKey, std::shared_ptr<Session>>;
std::map<SessionKey, PulseStatus> last_status;
std::map<SessionKey, uint64_t> command_attempts;
std::map<SessionKey, uint64_t> preview_attempts;
// Grid thumbnails: the decode lanes see which enabled packages could still
// register a thumbnail provider (SDK OnStart runs at activation, packages
// activate lazily). Written on the UI thread, read on any thread.
struct ThumbnailPackages {
    std::mutex mutex;
    HWND owner = nullptr;                  // the window whose sessions serve them
    bool any = false;                      // an inactive candidate takes every extension
    std::set<std::string> extensions;      // ".ext" of inactive filtered candidates
    std::set<std::string> wanted;          // asked for since the last activation
    bool posted = false;
};
ThumbnailPackages& thumbnail_packages() { static ThumbnailPackages state; return state; }
void RecordStatus(HWND owner, size_t package, PulseStatus status) {
    last_status[{owner, package}] = status;
    if (status == PULSE_OK) return;
    const auto diagnostic = "Pulse extension native: package=" + std::to_string(package) +
        " status=" + std::to_string(status) +
        (status == PULSE_ERROR_SDK_PREVIEW ? " (built against another SDK preview; rebuild with the current SDK)" : "") + "\n";
    OutputDebugStringA(diagnostic.c_str());
}
void RecordSlowStart(size_t package, const std::string& id, double load_ms, double activate_ms) {
    if (load_ms <= kSlowStartThresholdMs && activate_ms <= kSlowStartThresholdMs) return;
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
                  "Pulse extension native slow start: package=%zu id=%s load_ms=%.1f activate_ms=%.1f\n",
                  package, id.c_str(), load_ms, activate_ms);
    OutputDebugStringA(buffer);
    diagnostics::runtime::Event("native_extension_slow_start",
        {{"package", static_cast<uint64_t>(package)},
         {"load_ms", static_cast<uint64_t>(load_ms)},
         {"activate_ms", static_cast<uint64_t>(activate_ms)}},
        diagnostics::runtime::Level::Warning);
}
bool Name(PulseStringView name, const char* expected) {
    const auto size = std::strlen(expected);
    return name.data && name.length == size && std::memcmp(name.data, expected, size) == 0;
}
PulseStatus PULSE_CALL Query(void* context, PulseStringView name, uint32_t version, void* out, uint32_t size) {
    auto* session = static_cast<Session*>(context);
    if (!session || GetCurrentThreadId() != session->thread || session->closing || !ValidateCallerBinding(session->caller)) return PULSE_ERROR_STATE;
    if (Name(name, PULSE_SETTINGS_PAGES_INTERFACE_NAME)) return session->settings_pages->Query(version, out, size);
    if (Name(name, PULSE_DIAGNOSTICS_INTERFACE_NAME)) return session->diagnostics->Query(version, out, size);
    if (Name(name, PULSE_DIALOGS_INTERFACE_NAME)) return session->dialogs->Query(version, out, size);
    if (Name(name, PULSE_PICKERS_INTERFACE_NAME)) return session->pickers->Query(version, out, size);
    if (Name(name, PULSE_WORKBENCH_INTERFACE_NAME)) return session->workbench->Query(version, out, size);
    if (Name(name, PULSE_TRAY_INTERFACE_NAME)) return session->tray->Query(version, out, size);
    if (Name(name, PULSE_SEARCH_INTERFACE_NAME)) return session->search->Query(version, out, size);
    if (Name(name, PULSE_SHELL_INTERFACE_NAME)) return session->shell->Query(version, out, size);
    if (Name(name, PULSE_ASSOCIATIONS_INTERFACE_NAME)) return session->associations->Query(version, out, size);
    if (Name(name, PULSE_VFS_INTERFACE_NAME)) return session->vfs->Query(version, out, size);
    if (Name(name, PULSE_COLUMNS_INTERFACE_NAME)) return session->columns->Query(version, out, size);
    if (Name(name, PULSE_STATUSBAR_INTERFACE_NAME)) return session->statusbar->Query(version, out, size);
    if (Name(name, PULSE_EXTCAPSULES_INTERFACE_NAME)) return session->extcapsules->Query(version, out, size);
    if (Name(name, PULSE_FILEEVENTS_INTERFACE_NAME)) return session->fileevents->Query(version, out, size);
    if (Name(name, PULSE_ICONS_INTERFACE_NAME)) return session->icons->Query(version, out, size);
    if (Name(name, PULSE_UI_MODEL_INTERFACE)) return session->ui->QueryModel(version, out, size);
    if (Name(name, PULSE_UI_DATA_INTERFACE)) return session->ui->QueryData(version, out, size);
    if (Name(name, PULSE_UI_MOUNT_INTERFACE)) return session->ui->QueryMount(version, out, size);
    if (Name(name, PULSE_UI_EVENTS_INTERFACE_NAME)) return session->ui->QueryEvents(version, out, size);
    if (Name(name, PULSE_DISPATCH_INTERFACE)) return session->dispatch->Query(version, out, size);
    if (Name(name, PULSE_RESOURCES_INTERFACE_NAME)) return session->resources->Query(version, out, size);
    if (Name(name, PULSE_FILES_INTERFACE_NAME)) return session->files->Query(version, out, size);
    if (Name(name, PULSE_TASKS_INTERFACE_NAME)) return session->tasks->Query(version, out, size);
    if (Name(name, PULSE_STAGING_INTERFACE_NAME)) return session->staging->Query(version, out, size);
    if (Name(name, PULSE_PREVIEW_INTERFACE_NAME)) return session->previews->Query(version, out, size);
    if (Name(name, PULSE_COMMANDS_INTERFACE_NAME)) return session->commands->Query(version, out, size);
    if (Name(name, PULSE_CONTEXT_INTERFACE_NAME)) return session->context->Query(version, out, size);
    if (Name(name, PULSE_DIRECTORIES_INTERFACE_NAME)) return session->directories->Query(version, out, size);
    if (Name(name, PULSE_FILEOPS_INTERFACE_NAME)) return session->fileops->Query(version, out, size);
    return PULSE_ERROR_UNAVAILABLE;
}
void Retire(const std::shared_ptr<Session>& session) {
    if (session->wake_signal) session->wake_signal->window.store(nullptr, std::memory_order_release);
    if (session->timer) {
        if (session->timer_armed) {
            KillTimer(session->timer, kSessionTimerId);
            session->timer_armed = false;
        }
        SetWindowLongPtrW(session->timer, GWLP_USERDATA, 0);
        DestroyWindow(session->timer);
        session->timer = nullptr;
    }
    const auto found = sessions.find({session->owner, session->package});
    if (found != sessions.end() && found->second == session) sessions.erase(found);
}
void PumpSession(const std::shared_ptr<Session>& current) {
    if (!IsWindow(current->owner) || !ValidateCallerBinding(current->caller)) current->Stop();
    {
        const SlowUiCallbackScope slow_scope(current->package_id, current->session_id, L"session_pump");
        if (!current->closing && current->ui) {
            if (auto* state = GetAppState(current->owner)) current->ui->SyncHost(*state, [state] { ClampScroll(*state); });
        }
        if (current->tasks) current->tasks->Poll();
        if (current->staging) current->staging->Poll();
        if (!current->closing && current->workbench) current->workbench->Sync();
        if (!current->closing && current->tray) current->tray->Sync();
        if (!current->closing && current->search) current->search->Poll();
        if (!current->closing && current->shell) current->shell->Poll();
        if (!current->closing && current->associations) current->associations->Poll();
        if (!current->closing && current->previews) current->previews->Poll();
        if (!current->closing && current->context) current->context->Poll();
        if (!current->closing && current->directories) current->directories->Poll();
        if (!current->closing && current->projection_eligible && current->projection_dirty &&
            GetTickCount64() - current->projection_dirty_since >= 150) {
            current->FlushCommandProjection(false);
        }
    }
    current->RefreshTrackedModules();
    if (current->Drain()) {
        Retire(current);
        return;
    }
    current->UpdateTimer();
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create ? create->lpCreateParams : nullptr));
        return TRUE;
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message != WM_TIMER && message != kWakeMessage) return DefWindowProcW(window, message, wparam, lparam);
    auto* raw = reinterpret_cast<Session*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!raw) return 0;
    std::shared_ptr<Session> current;
    try { current = raw->shared_from_this(); } catch (...) { return 0; }
    if (current->wake_signal) current->wake_signal->queued.store(false, std::memory_order_release);
    PumpSession(current);
    return 0;
}
bool Start(AppState& owner, size_t package, const std::wstring& path, bool activate = true) {
    const auto emit_pre_session_failure = [&](PulseStatus status) {
        const auto* manifest = PackageManifest(package);
        const std::string_view pkg_id = manifest ? std::string_view(manifest->id) : std::string_view{};
        EmitSessionTelemetry(pkg_id,
                             ResolvePublisherFingerprint(package),
                             ResolvePackageRevision(manifest, {}),
                             0,
                             L"start_failed",
                             0,
                             status,
                             0,
                             diagnostics::runtime::Level::Warning);
    };
    const auto fail = [&](PulseStatus status, bool emit_failure = true) {
        if (emit_failure) emit_pre_session_failure(status);
        RecordStatus(owner.hwnd, package, status);
        return false;
    };
    if (!PackageEnabled(package) || path.empty() || !IsWindow(owner.hwnd)) return fail(PULSE_ERROR_ARGUMENT);
    const auto key = SessionKey{owner.hwnd, package};
    const auto found = sessions.find(key);
    if (found != sessions.end()) {
        const auto existing = found->second;
        if (!existing->closing && ValidateCallerBinding(existing->caller) && existing->module->Loaded()) {
            if (!activate) return true;
            if (!existing->module->Active()) {
                LARGE_INTEGER before{}, after{};
                QueryPerformanceCounter(&before);
                PulseStatus status = PULSE_ERROR_STATE;
                {
                    const crash_guard::ActivationScope activation(package);
                    const SlowUiCallbackScope slow_scope(existing->package_id, existing->session_id, L"activate");
                    status = existing->module->Activate();
                }
                QueryPerformanceCounter(&after);
                existing->activate_ms = ElapsedMs(before.QuadPart, after.QuadPart);
                RecordSlowStart(package, existing->package_id, 0.0, existing->activate_ms);
                existing->RefreshTrackedModules();
                existing->UpdateTimer();
                if (status == PULSE_OK && !existing->closing && ValidateCallerBinding(existing->caller)) {
                    existing->EmitLifecycle(L"activate", existing->activate_ms, PULSE_OK);
                    RecordStatus(owner.hwnd, package, PULSE_OK);
                    return true;
                }
                const PulseStatus effective = status == PULSE_OK ? PULSE_ERROR_STATE : status;
                existing->EmitLifecycle(L"start_failed", existing->activate_ms, effective, diagnostics::runtime::Level::Warning);
                existing->Stop();
                if (existing->Drain()) Retire(existing);
                else existing->UpdateTimer();
                return fail(effective, false);
            }
        }
        if (!existing->closing && ValidateCallerBinding(existing->caller) &&
            existing->module->Active() && IsWindow(existing->ui->Window())) {
            ShowWindow(existing->ui->Window(), SW_SHOW);
            const HWND window = existing->ui->Window();
            SetForegroundWindow((GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) ? owner.hwnd : window);
            SetFocus(window);
            existing->UpdateTimer();
            RecordStatus(owner.hwnd, package, PULSE_OK);
            return true;
        }
        existing->Stop();
        if (!existing->Drain()) {
            existing->UpdateTimer();
            return fail(PULSE_ERROR_STATE);
        }
        Retire(existing);
    }
    const auto* manifest = PackageManifest(package);
    if (!manifest) return fail(PULSE_ERROR_ARGUMENT);
    const std::string expected_sha256 = manifest->native ? manifest->native->sha256 : std::string{};
    const auto binding = CaptureCallerBinding(package, reinterpret_cast<uintptr_t>(owner.hwnd));
    if (!binding) return fail(PULSE_ERROR_STATE);
    auto session = std::make_shared<Session>(*binding);
    session->owner = owner.hwnd;
    session->package = package;
    session->package_id = manifest->id;
    session->publisher_fingerprint = ResolvePublisherFingerprint(package);
    session->revision_tag = ResolvePackageRevision(manifest, binding->Package().revision);
    session->package_dir = std::filesystem::path(path).parent_path().wstring();
    session->projection_revision = manifest->native ? manifest->native->sha256 : std::string{};
    session->projection_root = ResolveProjectionDataRoot(owner);
    const auto weak = std::weak_ptr<Session>(session);
    const auto wake_signal = session->wake_signal;
    const auto wake = [wake_signal]() noexcept { wake_signal->Request(); };
    const auto is_live = [weak] {
        const auto value = weak.lock();
        return value && !value->closing && ValidateCallerBinding(value->caller);
    };
    session->ui = std::make_unique<UiAdapter>(owner.hwnd, owner.darkMode, owner.accentColor, owner.scale, is_live);
    session->ui->SetDiagnostics(session->diagnostics_sink);
    session->ui->SyncHost(owner, [&owner] { ClampScroll(owner); });
    session->settings_pages = std::make_unique<SettingsPagesAdapter>(owner.hwnd, owner.darkMode, owner.accentColor, owner.scale, is_live);
    session->settings_pages->SetDiagnostics(session->diagnostics_sink);
    session->diagnostics = std::make_unique<DiagnosticsAdapter>(session->diagnostics_sink, is_live);
    session->dialogs = std::make_unique<DialogsAdapter>(owner.hwnd, owner.darkMode, owner.accentColor, is_live, session->diagnostics_sink);
    auto* dialogs_raw = session->dialogs.get();
    session->pickers = std::make_unique<PickersAdapter>(
        owner.hwnd, [dialogs_raw] { return dialogs_raw->ActiveModalWindow(); }, is_live, session->diagnostics_sink);
    session->workbench = std::make_unique<WorkbenchAdapter>(owner.hwnd, is_live, session->diagnostics_sink, wake);
    session->tray = std::make_unique<TrayAdapter>(owner.hwnd, is_live, session->diagnostics_sink, wake);
    session->search = std::make_unique<SearchAdapter>(owner.hwnd, is_live, session->diagnostics_sink, wake);
    session->shell = std::make_unique<ShellAdapter>(owner.hwnd, is_live, session->diagnostics_sink, wake);
    session->associations = std::make_unique<AssociationsAdapter>(
        manifest->id,
        manifest->file_handlers,
        MakeAssociationsBackend(owner.hwnd, manifest->id, session->package_dir),
        is_live,
        session->diagnostics_sink,
        wake);
    session->vfs = std::make_unique<VfsAdapter>(
        manifest->id,
        MakeVfsBackend(owner.hwnd),
        is_live,
        session->diagnostics_sink,
        wake);
    session->columns = std::make_unique<ColumnsAdapter>(manifest->id, is_live, session->diagnostics_sink, wake);
    session->statusbar = std::make_unique<StatusBarAdapter>(manifest->id, is_live, session->diagnostics_sink);
    session->extcapsules = std::make_unique<ExtCapsulesAdapter>(manifest->id, is_live, session->diagnostics_sink);
    session->fileevents = std::make_unique<FileEventsAdapter>(manifest->id, is_live, session->diagnostics_sink);
    session->icons = std::make_unique<IconsAdapter>(manifest->id, is_live);
    session->files = std::make_unique<FilesAdapter>(owner, *binding);
    session->resources = std::make_unique<ResourcesAdapter>(owner, *binding);
    session->context = std::make_unique<ContextAdapter>(owner, *binding);
    session->context->SetWake(wake, manifest->id);
    session->commands = std::make_unique<CommandsAdapter>(owner.hwnd, manifest->id, session->context->Table(), is_live);
    session->commands->SetOnChanged([weak]() noexcept {
        if (const auto self = weak.lock()) {
            if (!self->closing) {
                self->projection_dirty = true;
                self->projection_dirty_since = GetTickCount64();
                self->UpdateTimer();
            }
        }
    });
    session->previews = std::make_unique<PreviewsAdapter>(owner.hwnd, is_live, wake, manifest->id);
    session->tasks = std::make_unique<TasksAdapter>(owner.hwnd, is_live, MakeTasksBackend(owner.hwnd), wake, manifest->id);
    session->staging = std::make_unique<StagingAdapter>(owner.hwnd, is_live, MakeStagingBackend(owner.hwnd, *binding), wake, manifest->id);
    session->directories = std::make_unique<DirectoriesAdapter>(owner, *binding);
    session->directories->SetWake(wake, manifest->id);
    session->fileops = std::make_unique<FileOpsAdapter>(owner, *binding, *session->directories);
    session->dispatch = std::make_unique<DispatchAdapter>(is_live, manifest->id);
    session->module = std::make_unique<Module>();
    session->module->SetPackageId(manifest->id);
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = Procedure;
    cls.lpszClassName = L"Pulse.NativePluginSession";
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        session->EmitLifecycle(L"start_failed", 0.0, PULSE_ERROR_UNAVAILABLE, diagnostics::runtime::Level::Warning);
        return fail(PULSE_ERROR_UNAVAILABLE, false);
    }
    session->timer = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, session.get());
    if (!session->timer) {
        session->EmitLifecycle(L"start_failed", 0.0, PULSE_ERROR_UNAVAILABLE, diagnostics::runtime::Level::Warning);
        return fail(PULSE_ERROR_UNAVAILABLE, false);
    }
    SetWindowLongPtrW(session->timer, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(session.get()));
    session->wake_signal->window.store(session->timer, std::memory_order_release);
    sessions.emplace(key, session);
    const PulseHostV1 host{sizeof(PulseHostV1), PULSE_ABI_VERSION_1, session.get(), Query};
    const auto live = [&] { return !session->closing && IsWindow(session->owner) && ValidateCallerBinding(session->caller); };
    LARGE_INTEGER start{}, loaded{}, activated{};
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        session->private_before = memory.PrivateUsage;
    QueryPerformanceCounter(&start);
    PulseStatus load_status = PULSE_ERROR_UNAVAILABLE;
    PulseStatus status = PULSE_ERROR_UNAVAILABLE;
    bool attempted_activate = false;
    {
        // A crash or hang from here to the end of Activate disables this
        // build of the package at the next start.
        const crash_guard::ActivationScope activation(package);
        {
            const SlowUiCallbackScope slow_load(session->package_id, session->session_id, L"load");
            load_status = expected_sha256.empty() ? session->module->Load(path, host)
                : session->module->LoadVerified(path, *manifest->native, host);
        }
        status = load_status;
        if (status == PULSE_OK) {
            session->tracked = crash_guard::TrackPackageModules(
                package, session->module->Library(), session->package_dir,
                manifest->native ? &*manifest->native : nullptr);
        }
        QueryPerformanceCounter(&loaded);
        if (status == PULSE_OK && live() && activate) {
            attempted_activate = true;
            const SlowUiCallbackScope slow_activate(session->package_id, session->session_id, L"activate");
            status = session->module->Activate();
        }
        QueryPerformanceCounter(&activated);
    }
    session->load_ms = ElapsedMs(start.QuadPart, loaded.QuadPart);
    session->activate_ms = ElapsedMs(loaded.QuadPart, activated.QuadPart);
    RecordSlowStart(package, session->package_id, session->load_ms, session->activate_ms);
    if (load_status == PULSE_OK) {
        session->EmitLifecycle(L"load", session->load_ms, PULSE_OK);
    }
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        session->private_after = memory.PrivateUsage;
    if (status != PULSE_OK || !live()) {
        const PulseStatus effective = status == PULSE_OK ? PULSE_ERROR_STATE : status;
        const double fail_ms = attempted_activate ? session->activate_ms : session->load_ms;
        session->EmitLifecycle(L"start_failed", fail_ms, effective, diagnostics::runtime::Level::Warning);
        session->Stop();
        if (session->Drain()) Retire(session);
        else session->UpdateTimer();
        return fail(effective, false);
    }
    if (attempted_activate) {
        session->EmitLifecycle(L"activate", session->activate_ms, PULSE_OK);
    }
    session->projection_eligible = true;
    session->projection_dirty = true;
    session->projection_dirty_since = GetTickCount64();
    session->RefreshTrackedModules();
    session->UpdateTimer();
    RecordStatus(owner.hwnd, package, PULSE_OK);
    return true;
}
PackageRuntimeSnapshot SnapshotForSession(const Session& session) {
    PackageRuntimeSnapshot snapshot;
    snapshot.callbacks_active = session.CallbacksActive();
    if (session.closing) {
        snapshot.state = PackageRuntimeState::Draining;
        const ULONGLONG now = GetTickCount64();
        snapshot.draining_ms = now >= session.closing_since_tick ? now - session.closing_since_tick : 0;
        snapshot.restart_required = session.close_failed || snapshot.draining_ms >= kStalledDrainRestartMs;
        if (snapshot.restart_required) session.CheckDrainStalled();
        snapshot.last_status = PULSE_OK;
        return snapshot;
    }
    if (ValidateCallerBinding(session.caller) && session.module && session.module->Loaded()) {
        snapshot.state = session.module->Active() ? PackageRuntimeState::Active : PackageRuntimeState::Loaded;
        snapshot.last_status = PULSE_OK;
        return snapshot;
    }
    snapshot.state = PackageRuntimeState::Inactive;
    return snapshot;
}
}
bool StartFromArguments(AppState& owner, int argc, wchar_t** argv) {
    const wchar_t* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--extension-native") != 0) continue;
        if (path || i + 1 >= argc || argv[i + 1][0] == L'-') return false;
        path = argv[++i];
    }
    if (!path) return true;
    if (PackageCount() != 1) return false;
    return Start(owner, 0, path);
}
bool StartPackage(AppState& owner, size_t package) {
    const auto* manifest = PackageManifest(package);
    if (!manifest || !manifest->native) {
        RecordStatus(owner.hwnd, package, PULSE_ERROR_ARGUMENT);
        return false;
    }
    return Start(owner, package, PackageNativePath(package));
}
bool BackgroundPackageEnabled() {
    for (size_t package = 0; package < PackageCount(); ++package) {
        if (!PackageEnabled(package)) continue;
        const auto* manifest = PackageManifest(package);
        if (manifest && manifest->native && manifest->id == "pulse.desktop") {
            return true;
        }
    }
    return false;
}
void StartBackgroundPackages(AppState& owner) {
    if (!IsWindow(owner.hwnd)) return;
    {
        auto& thumbs = thumbnail_packages();
        std::lock_guard lock(thumbs.mutex);
        if (!thumbs.owner || !IsWindow(thumbs.owner)) {
            thumbs.owner = owner.hwnd;
            thumbs.wanted.clear();
            thumbs.posted = false;
        }
    }
    RefreshThumbnailPackages(owner.hwnd);
    for (size_t package = 0; package < PackageCount(); ++package) {
        if (!PackageEnabled(package)) continue;
        const auto* manifest = PackageManifest(package);
        if (!manifest || !manifest->native || manifest->id != "pulse.desktop") continue;
        bool already_active = false;
        for (const auto& [key, session] : sessions) {
            if (key.second != package || !session || session->closing) continue;
            if (IsWindow(key.first) && ValidateCallerBinding(session->caller) &&
                session->module && session->module->Active()) {
                already_active = true;
                break;
            }
        }
        if (!already_active) {
            Start(owner, package, PackageNativePath(package), true);
        }
    }
}
bool PackageSessionLoaded(HWND owner, size_t package) {
    const auto found = sessions.find({owner, package});
    return found != sessions.end() && !found->second->closing &&
        ValidateCallerBinding(found->second->caller) &&
        found->second->module && found->second->module->Loaded();
}
bool EnsureCommandPackageStarted(AppState& owner, size_t package) {
    const auto* manifest = PackageManifest(package);
    const auto key = std::make_pair(owner.hwnd, package);
    if (!manifest || !manifest->native || !PackageEnabled(package) || !IsWindow(owner.hwnd)) {
        command_attempts.erase(key);
        return false;
    }
    // Activate, not just load: SDK App plugins register their commands (and
    // everything else) in OnStart, which runs on activate.
    if (PackageActive(owner.hwnd, package)) return true;
    const auto generation = PackageGeneration(package);
    if (command_attempts.contains(key) && command_attempts[key] == generation &&
        PackageLastStatus(owner.hwnd, package) != PULSE_OK) {
        return false;
    }
    command_attempts[key] = generation;
    return Start(owner, package, PackageNativePath(package), true);
}
void PrepareCommandPackages(AppState& owner) {
    // Packages that declare static `contributes.commands` in manifest.json are
    // discovered statically and never verified/loaded during menu or shortcut scans.
    // Only legacy/development packages without static declarations fall back here.
    auto& attempted = command_attempts;
    for (size_t package = 0; package < PackageCount(); ++package) {
        const auto* manifest = PackageManifest(package);
        const auto key = std::make_pair(owner.hwnd, package);
        if (!manifest || !manifest->native || !PackageEnabled(package)) { attempted.erase(key); continue; }
        if (manifest->command_contributions_declared) continue;
        const auto generation = PackageGeneration(package);
        if (attempted.contains(key) && attempted[key] == generation && sessions.contains(key)) continue;
        if (attempted.contains(key) && attempted[key] == generation && PackageLastStatus(owner.hwnd, package) != PULSE_OK) continue;
        attempted[key] = generation;
        Start(owner, package, PackageNativePath(package), true);
    }
    RefreshThumbnailPackages(owner.hwnd);
}
namespace {
constexpr UINT_PTR kStartupPackagesTimer = 0x50535450;  // 'PSTP'
constexpr UINT kStartupPackagesDelayMs = 400;           // after the first frames
}  // namespace
void ScheduleStartupPackages(HWND owner) {
    if (IsWindow(owner)) SetTimer(owner, kStartupPackagesTimer, kStartupPackagesDelayMs, nullptr);
}
bool HandleStartupPackagesTimer(HWND window, WPARAM timer, AppState& owner) {
    if (timer != kStartupPackagesTimer) return false;
    KillTimer(window, kStartupPackagesTimer);
    ActivateStartupPackages(owner);
    return true;
}
void ActivateStartupPackages(AppState& owner) {
    if (!IsWindow(owner.hwnd)) return;
    for (size_t package = 0; package < PackageCount(); ++package) {
        const auto* manifest = PackageManifest(package);
        if (!manifest || !manifest->native || !PackageEnabled(package) || !manifest->StartsWithWindow() ||
            manifest->id == "pulse.desktop" || PackageActive(owner.hwnd, package)) continue;
        // Same per-build give-up as previews: a package that failed to start is not retried.
        EnsurePreviewPackageActive(owner, package);
    }
    RefreshThumbnailPackages(owner.hwnd);
}
namespace {
// Packages that may answer a preview request for `extension` (".ext", lower
// case; empty: any extension). Packages without per-extension commands may
// preview anything; the rest only the extensions their commands name.
bool PreviewCandidate(const Manifest& manifest, std::string_view extension) {
    if (!manifest.command_contributions_declared || manifest.command_contributions.empty()) return true;
    if (extension.empty()) return false;
    for (const auto& command : manifest.command_contributions)
        if (!command.exclude_extensions &&
            std::find(command.extensions.begin(), command.extensions.end(), extension) != command.extensions.end())
            return true;
    return false;
}
bool PreviewActivationGivenUp(HWND owner, size_t package) {
    const auto found = preview_attempts.find({owner, package});
    return found != preview_attempts.end() && found->second == PackageGeneration(package) &&
        PackageLastStatus(owner, package) != PULSE_OK;
}
}  // namespace
bool EnsurePreviewPackageActive(AppState& owner, size_t package) {
    const auto* manifest = PackageManifest(package);
    const auto key = std::make_pair(owner.hwnd, package);
    if (!manifest || !manifest->native || !PackageEnabled(package) || !IsWindow(owner.hwnd)) {
        preview_attempts.erase(key);
        return false;
    }
    if (PackageActive(owner.hwnd, package)) return true;
    if (PreviewActivationGivenUp(owner.hwnd, package)) return false;
    preview_attempts[key] = PackageGeneration(package);
    return Start(owner, package, PackageNativePath(package), true);
}
void ActivatePreviewPackages(AppState& owner, std::string_view extension) {
    for (size_t package = 0; package < PackageCount(); ++package) {
        const auto* manifest = PackageManifest(package);
        if (!manifest || !manifest->native || !PackageEnabled(package) ||
            PackageActive(owner.hwnd, package) || !PreviewCandidate(*manifest, extension)) continue;
        EnsurePreviewPackageActive(owner, package);
    }
}
void RefreshThumbnailPackages(HWND owner) {
    auto& thumbs = thumbnail_packages();
    {
        std::lock_guard lock(thumbs.mutex);
        if (!owner || thumbs.owner != owner) return;
    }
    bool any = false;
    std::set<std::string> extensions;
    for (size_t package = 0; package < PackageCount(); ++package) {
        const auto* manifest = PackageManifest(package);
        if (!manifest || !manifest->native || !PackageEnabled(package) ||
            PackageActive(owner, package) || PreviewActivationGivenUp(owner, package)) continue;
        if (PreviewCandidate(*manifest, {})) { any = true; continue; }
        for (const auto& command : manifest->command_contributions)
            if (!command.exclude_extensions) extensions.insert(command.extensions.begin(), command.extensions.end());
    }
    std::lock_guard lock(thumbs.mutex);
    if (thumbs.owner != owner) return;
    thumbs.any = any;
    thumbs.extensions = std::move(extensions);
}
bool RequestThumbnailPackages(std::string_view extension) {
    auto& thumbs = thumbnail_packages();
    std::lock_guard lock(thumbs.mutex);
    if (!thumbs.owner || extension.empty()) return false;
    if (!thumbs.any && !thumbs.extensions.contains(std::string(extension))) return false;
    thumbs.wanted.emplace(extension);
    if (!thumbs.posted) thumbs.posted = PostMessageW(thumbs.owner, WM_PLUGIN_THUMBNAILS, 0, 0) != FALSE;
    return thumbs.posted;
}
void ActivateThumbnailPackages(AppState& owner) {
    std::set<std::string> wanted;
    {
        auto& thumbs = thumbnail_packages();
        std::lock_guard lock(thumbs.mutex);
        thumbs.posted = false;
        if (thumbs.owner != owner.hwnd) return;
        wanted.swap(thumbs.wanted);
    }
    for (const auto& extension : wanted) ActivatePreviewPackages(owner, extension);
    RefreshThumbnailPackages(owner.hwnd);
}
PackageRuntimeSnapshot QueryPackageRuntime(HWND owner, size_t package) {
    const auto found = sessions.find({owner, package});
    if (found != sessions.end()) return SnapshotForSession(*found->second);
    PackageRuntimeSnapshot snapshot;
    const auto status_it = last_status.find({owner, package});
    if (status_it != last_status.end() && status_it->second != PULSE_OK) {
        snapshot.state = PackageRuntimeState::Failed;
        snapshot.last_status = status_it->second;
    }
    return snapshot;
}
PackageRuntimeSnapshot QueryPackageRuntime(size_t package) {
    PackageRuntimeSnapshot aggregate;
    auto rank = [](PackageRuntimeState state) -> int {
        switch (state) {
        case PackageRuntimeState::Inactive: return 0;
        case PackageRuntimeState::Failed: return 1;
        case PackageRuntimeState::Loaded: return 2;
        case PackageRuntimeState::Active: return 3;
        case PackageRuntimeState::Draining: return 4;
        }
        return 0;
    };
    for (const auto& [key, session] : sessions) {
        if (key.second != package || !session) continue;
        const auto current = SnapshotForSession(*session);
        if (rank(current.state) > rank(aggregate.state)) aggregate.state = current.state;
        aggregate.draining_ms = (std::max)(aggregate.draining_ms, current.draining_ms);
        aggregate.callbacks_active = aggregate.callbacks_active || current.callbacks_active;
        aggregate.restart_required = aggregate.restart_required || current.restart_required;
    }
    if (aggregate.state == PackageRuntimeState::Inactive) {
        for (const auto& [key, status] : last_status) {
            if (key.second == package && status != PULSE_OK) {
                aggregate.state = PackageRuntimeState::Failed;
                aggregate.last_status = status;
                break;
            }
        }
    }
    return aggregate;
}
void StopPackage(HWND owner, size_t package) {
    command_attempts.erase({owner, package});
    preview_attempts.erase({owner, package});
    const auto found = sessions.find({owner, package});
    if (found == sessions.end()) return;
    const auto session = found->second;
    session->Stop();
    if (session->Drain()) Retire(session);
    else session->UpdateTimer();
    RefreshThumbnailPackages(owner);
}
void StopPackage(size_t package) {
    std::erase_if(command_attempts, [package](const auto& item) { return item.first.second == package; });
    std::erase_if(preview_attempts, [package](const auto& item) { return item.first.second == package; });
    std::vector<HWND> owners;
    for (const auto& [key, session] : sessions) {
        (void)session;
        if (key.second == package) owners.push_back(key.first);
    }
    for (const auto owner : owners) StopPackage(owner, package);
}
void StopOwner(HWND owner) {
    CloseSettingsPagesHost(owner);
    CommandsAdapter::ClearStaticOwner(owner);
    std::vector<size_t> packages;
    for (const auto& [key, session] : sessions) {
        (void)session;
        if (key.first == owner) packages.push_back(key.second);
    }
    for (const auto package : packages) StopPackage(owner, package);
    std::erase_if(command_attempts, [owner](const auto& entry) { return entry.first.first == owner; });
    std::erase_if(preview_attempts, [owner](const auto& entry) { return entry.first.first == owner; });
    {
        auto& thumbs = thumbnail_packages();
        std::lock_guard lock(thumbs.mutex);
        if (thumbs.owner == owner) {
            thumbs.owner = nullptr;
            thumbs.any = false;
            thumbs.extensions.clear();
            thumbs.wanted.clear();
            thumbs.posted = false;
        }
    }
    std::erase_if(last_status, [owner](const auto& entry) { return entry.first.first == owner; });
}
void StopAll() {
    std::vector<SessionKey> keys;
    std::vector<HWND> owners;
    for (const auto& [key, session] : sessions) {
        (void)session;
        keys.push_back(key);
        owners.push_back(key.first);
    }
    for (const auto owner : owners) CommandsAdapter::ClearStaticOwner(owner);
    for (const auto& [owner, package] : keys) StopPackage(owner, package);
    last_status.clear();
    command_attempts.clear();
    preview_attempts.clear();
}
SettingsPagesAdapter* PackageSettings(HWND owner, size_t package) {
    const auto found = sessions.find({owner, package});
    return found != sessions.end() && !found->second->closing && ValidateCallerBinding(found->second->caller)
        ? found->second->settings_pages.get() : nullptr;
}
UiAdapter* PackageUi(HWND owner, size_t package) {
    const auto found = sessions.find({owner, package});
    return found != sessions.end() && !found->second->closing && ValidateCallerBinding(found->second->caller)
        ? found->second->ui.get() : nullptr;
}
bool PackageActive(HWND owner, size_t package) {
    const auto found = sessions.find({owner, package});
    return found != sessions.end() && !found->second->closing && ValidateCallerBinding(found->second->caller) &&
        found->second->module->Active();
}
PulseStatus PackageLastStatus(HWND owner, size_t package) {
    const auto found = last_status.find({owner, package});
    return found != last_status.end() ? found->second : PULSE_ERROR_STATE;
}
UiAdapter* OwnerUi(HWND owner) { return PackageUi(owner, 0); }
bool OwnerActive(HWND owner) { return PackageActive(owner, 0); }
json::Value OwnerMetrics(HWND owner) {
    auto result = json::Value::Object();
    uint64_t loaded_sessions = 0, active_sessions = 0, draining_sessions = 0, timer_armed_sessions = 0;
    for (const auto& [key, session] : sessions) {
        if (key.first != owner || !session) continue;
        if (session->timer_armed) ++timer_armed_sessions;
        if (session->closing) {
            ++draining_sessions;
            continue;
        }
        if (ValidateCallerBinding(session->caller) && session->module && session->module->Loaded()) {
            ++loaded_sessions;
            if (session->module->Active()) ++active_sessions;
        }
    }
    result.set("loadedSessions", loaded_sessions);
    result.set("activeSessions", active_sessions);
    result.set("drainingSessions", draining_sessions);
    result.set("timerArmedSessions", timer_armed_sessions);
    const auto found = sessions.find({owner, 0});
    if (found == sessions.end()) return result;
    const auto& value = *found->second;
    result.set("loadMilliseconds", value.load_ms);
    result.set("activateMilliseconds", value.activate_ms);
    result.set("processPrivateBefore", std::to_string(value.private_before));
    result.set("processPrivateAfter", std::to_string(value.private_after));
    result.set("scope", "one process snapshot; activation creates actual UI; no cold-cache or steady-state claim");
    return result;
}

}