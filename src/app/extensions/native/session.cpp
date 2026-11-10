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
#include "directories_adapter.h"
#include "fileops_adapter.h"
#include "../../../../sdk/native/include/pulse/ui_events.h"
#include "../../../../sdk/native/include/pulse/ui_data.h"
#include "../../../../sdk/native/include/pulse/ui_mount.h"
#include "../runtime.h"
#include "../../app_runtime.h"
#include "../../app_input.h"
#include "../../../ui/ui_renderer.h"
#include <cwchar>
#include <algorithm>
#include <psapi.h>
#include <cstring>
#include <memory>
#include <map>
#include <vector>

namespace pulse::extensions::native {
namespace {
struct Session {
    HWND owner = nullptr, timer = nullptr;
    size_t package = 0;
    DWORD thread = GetCurrentThreadId();
    bool closing = false;
    bool tracked = false; // image registered with crash_guard
    double load_ms = 0, activate_ms = 0;
    uint64_t private_before = 0, private_after = 0;
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
    std::unique_ptr<DirectoriesAdapter> directories;
    std::unique_ptr<FileOpsAdapter> fileops;
    std::unique_ptr<Module> module;
    CallerBinding caller;
    explicit Session(CallerBinding value) : caller(std::move(value)) {}
    // Runs before the members: the image is forgotten before ~Module frees it.
    ~Session() { if (tracked) crash_guard::ForgetModule(package); }
    void Stop() {
        if (closing) return;
        closing = true;
        if (dialogs) dialogs->Close();
        if (settings_pages) settings_pages->Close();
        if (commands) commands->Close();
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
    bool Drain() {
        if (!closing || (dialogs && dialogs->CallbackActive()) || (settings_pages && settings_pages->InCallback()) || (tasks && tasks->CallbackActive()) || (staging && staging->CallbackActive()) || (previews && previews->CallbackActive()) || (commands && commands->CallbackActive()) || (ui && ui->InCallback()) || (files && files->CallbackActive()) || (resources && resources->CallbackActive()) || (dispatch && dispatch->CallbackActive()) || (context && context->CallbackActive()) || (directories && directories->CallbackActive())) return false;
        if (!module) return true;
        if (module->Close() != PULSE_OK) return false;
        if (staging) {
            staging->CleanupRemaining();
            if (staging->CallbackActive()) return false;
        }
        if (tracked) { crash_guard::ForgetModule(package); tracked = false; }
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
void RecordStatus(HWND owner, size_t package, PulseStatus status) {
    last_status[{owner, package}] = status;
    if (status == PULSE_OK) return;
    const auto diagnostic = "Pulse extension native: package=" + std::to_string(package) +
        " status=" + std::to_string(status) +
        (status == PULSE_ERROR_SDK_PREVIEW ? " (built against another SDK preview; rebuild with the current SDK)" : "") + "\n";
    OutputDebugStringA(diagnostic.c_str());
}
bool Name(PulseStringView name, const char* expected) {
    const auto size = std::strlen(expected);
    return name.data && name.length == size && std::memcmp(name.data, expected, size) == 0;
}
PulseStatus PULSE_CALL Query(void* context, PulseStringView name, uint32_t version, void* out, uint32_t size) {
    auto* session = static_cast<Session*>(context);
    if (!session || GetCurrentThreadId() != session->thread || session->closing || !ValidateCallerBinding(session->caller)) return PULSE_ERROR_STATE;
    if (Name(name, PULSE_SETTINGS_PAGES_INTERFACE_NAME)) return session->settings_pages->Query(version, out, size);
    if (Name(name, PULSE_DIALOGS_INTERFACE_NAME)) return session->dialogs->Query(version, out, size);
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
    if (session->timer) { KillTimer(session->timer, 1); DestroyWindow(session->timer); session->timer = nullptr; }
    const auto found = sessions.find({session->owner, session->package});
    if (found != sessions.end() && found->second == session) sessions.erase(found);
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message != WM_TIMER) return DefWindowProcW(window, message, wparam, lparam);
    std::shared_ptr<Session> current;
    for (const auto& [owner, session] : sessions) { (void)owner; if (session->timer == window) { current = session; break; } }
    if (!current) return 0;
    if (!IsWindow(current->owner) || !ValidateCallerBinding(current->caller)) current->Stop();
    if (!current->closing && current->ui) {
        if (auto* state = GetAppState(current->owner)) current->ui->SyncHost(*state, [state] { ClampScroll(*state); });
    }
    if (current->tasks) current->tasks->Poll();
    if (current->staging) current->staging->Poll();
    if (!current->closing && current->previews) current->previews->Poll();
    if (!current->closing && current->context) current->context->Poll();
    if (!current->closing && current->directories) current->directories->Poll();
    if (current->Drain()) Retire(current);
    return 0;
}
bool Start(AppState& owner, size_t package, const std::wstring& path, bool activate = true) {
    const auto fail = [&](PulseStatus status) { RecordStatus(owner.hwnd, package, status); return false; };
    if (!PackageEnabled(package) || path.empty() || !IsWindow(owner.hwnd)) return fail(PULSE_ERROR_ARGUMENT);
    const auto key = SessionKey{owner.hwnd, package};
    const auto found = sessions.find(key);
    if (found != sessions.end()) {
        const auto existing = found->second;
        if (!existing->closing && ValidateCallerBinding(existing->caller) && existing->module->Loaded()) {
            if (!activate) return true;
            if (!existing->module->Active()) {
                const crash_guard::ActivationScope activation(package);
                const auto status = existing->module->Activate();
                if (status == PULSE_OK && !existing->closing && ValidateCallerBinding(existing->caller)) return true;
                existing->Stop(); if (existing->Drain()) Retire(existing);
                return fail(status == PULSE_OK ? PULSE_ERROR_STATE : status);
            }
        }
        if (!existing->closing && ValidateCallerBinding(existing->caller) &&
            existing->module->Active() && IsWindow(existing->ui->Window())) {
            ShowWindow(existing->ui->Window(), SW_SHOW);
            const HWND window = existing->ui->Window();
            SetForegroundWindow((GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) ? owner.hwnd : window);
            SetFocus(window);
            RecordStatus(owner.hwnd, package, PULSE_OK);
            return true;
        }
        existing->Stop();
        if (!existing->Drain()) return fail(PULSE_ERROR_STATE);
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
    const auto weak = std::weak_ptr<Session>(session);
    session->ui = std::make_unique<UiAdapter>(owner.hwnd, owner.darkMode, owner.accentColor, owner.scale, [weak] {
        const auto value = weak.lock();
        return value && !value->closing && ValidateCallerBinding(value->caller);
    });
    session->ui->SyncHost(owner, [&owner] { ClampScroll(owner); });
    session->settings_pages = std::make_unique<SettingsPagesAdapter>(owner.hwnd, owner.darkMode, owner.accentColor, owner.scale, [weak] {
        const auto value = weak.lock();
        return value && !value->closing && ValidateCallerBinding(value->caller);
    });
    session->dialogs = std::make_unique<DialogsAdapter>(owner.hwnd, owner.darkMode, owner.accentColor, [weak] {
        const auto value = weak.lock();
        return value && !value->closing && ValidateCallerBinding(value->caller);
    });
    session->files = std::make_unique<FilesAdapter>(owner, *binding);
    session->resources = std::make_unique<ResourcesAdapter>(owner, *binding);
    session->context = std::make_unique<ContextAdapter>(owner, *binding);
    session->commands = std::make_unique<CommandsAdapter>(owner.hwnd, manifest->id, session->context->Table(), [weak] {
        const auto value = weak.lock();
        return value && !value->closing && ValidateCallerBinding(value->caller);
    });
    session->previews = std::make_unique<PreviewsAdapter>(owner.hwnd, [weak] {
        const auto value = weak.lock();
        return value && !value->closing && ValidateCallerBinding(value->caller);
    });
    session->tasks = std::make_unique<TasksAdapter>(owner.hwnd, [weak] {
        const auto value = weak.lock();
        return value && !value->closing && ValidateCallerBinding(value->caller);
    }, MakeTasksBackend(owner.hwnd));
    session->staging = std::make_unique<StagingAdapter>(owner.hwnd, [weak] {
        const auto value = weak.lock();
        return value && !value->closing && ValidateCallerBinding(value->caller);
    }, MakeStagingBackend(owner.hwnd, *binding));
    session->directories = std::make_unique<DirectoriesAdapter>(owner, *binding);
    session->fileops = std::make_unique<FileOpsAdapter>(owner, *binding, *session->directories);
    session->dispatch = std::make_unique<DispatchAdapter>([weak] {
        const auto value = weak.lock();
        return value && !value->closing && ValidateCallerBinding(value->caller);
    });
    session->module = std::make_unique<Module>();
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = Procedure;
    cls.lpszClassName = L"Pulse.NativePluginSession";
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return fail(PULSE_ERROR_UNAVAILABLE);
    session->timer = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (!session->timer) return fail(PULSE_ERROR_UNAVAILABLE);
    sessions.emplace(key, session);
    const PulseHostV1 host{sizeof(PulseHostV1), PULSE_ABI_VERSION_1, session.get(), Query};
    const auto live = [&] { return !session->closing && IsWindow(session->owner) && ValidateCallerBinding(session->caller); };
    LARGE_INTEGER frequency{}, start{}, loaded{}, activated{};
    QueryPerformanceFrequency(&frequency);
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        session->private_before = memory.PrivateUsage;
    QueryPerformanceCounter(&start);
    PulseStatus status = PULSE_ERROR_UNAVAILABLE;
    {
        // A crash or hang from here to the end of Activate disables this
        // build of the package at the next start.
        const crash_guard::ActivationScope activation(package);
        if (SetTimer(session->timer, 1, 30, nullptr))
            status = expected_sha256.empty() ? session->module->Load(path, host)
                : session->module->LoadVerified(path, *manifest->native, host);
        if (status == PULSE_OK) session->tracked = crash_guard::TrackModule(package, session->module->Library());
        QueryPerformanceCounter(&loaded);
        if (status == PULSE_OK && live() && activate) status = session->module->Activate();
        QueryPerformanceCounter(&activated);
    }
    if (frequency.QuadPart) {
        session->load_ms = 1000.0 * static_cast<double>(loaded.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
        session->activate_ms = 1000.0 * static_cast<double>(activated.QuadPart - loaded.QuadPart) / static_cast<double>(frequency.QuadPart);
    }
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        session->private_after = memory.PrivateUsage;
    if (status != PULSE_OK || !live()) {
        session->Stop();
        if (session->Drain()) Retire(session);
        return fail(status == PULSE_OK ? PULSE_ERROR_STATE : status);
    }
    RecordStatus(owner.hwnd, package, PULSE_OK);
    return true;
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
void PrepareCommandPackages(AppState& owner) {
    // Entry-time registration loads lazily when a command surface is requested.
    // Failed generations are not retried every menu rebuild.
    auto& attempted = command_attempts;
    for (size_t package = 0; package < PackageCount(); ++package) {
        const auto* manifest = PackageManifest(package);
        const auto key = std::make_pair(owner.hwnd, package);
        if (!manifest || !manifest->native || !PackageEnabled(package)) { attempted.erase(key); continue; }
        const auto generation = PackageGeneration(package);
        if (attempted.contains(key) && attempted[key] == generation && sessions.contains(key)) continue;
        if (attempted.contains(key) && attempted[key] == generation && PackageLastStatus(owner.hwnd, package) != PULSE_OK) continue;
        attempted[key] = generation;
        Start(owner, package, PackageNativePath(package), false);
    }
}
void StopPackage(HWND owner, size_t package) {
    command_attempts.erase({owner, package});
    const auto found = sessions.find({owner, package});
    if (found == sessions.end()) return;
    const auto session = found->second;
    session->Stop();
    if (session->Drain()) Retire(session);
}
void StopPackage(size_t package) {
    std::erase_if(command_attempts, [package](const auto& item) { return item.first.second == package; });
    std::vector<HWND> owners;
    for (const auto& [key, session] : sessions) {
        (void)session;
        if (key.second == package) owners.push_back(key.first);
    }
    for (const auto owner : owners) StopPackage(owner, package);
}
void StopOwner(HWND owner) {
    CloseSettingsPagesHost(owner);
    std::vector<size_t> packages;
    for (const auto& [key, session] : sessions) {
        (void)session;
        if (key.first == owner) packages.push_back(key.second);
    }
    for (const auto package : packages) StopPackage(owner, package);
    std::erase_if(command_attempts, [owner](const auto& entry) { return entry.first.first == owner; });
    std::erase_if(last_status, [owner](const auto& entry) { return entry.first.first == owner; });
}
void StopAll() {
    std::vector<SessionKey> keys;
    for (const auto& [key, session] : sessions) { (void)session; keys.push_back(key); }
    for (const auto& [owner, package] : keys) StopPackage(owner, package);
    last_status.clear();
    command_attempts.clear();
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
