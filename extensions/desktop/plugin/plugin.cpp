// plugin.cpp - Host-facing Pulse extension DLL (pulse_desktop.dll) bridging
// PulseWorkbenchV2, PulseTrayV1, PulseSearchV1, PulseShellV1, PulseSettingsPagesV1,
// and supervising pulse_desktop_host.exe inside a KILL_ON_JOB_CLOSE Job Object.
// Uses public C ABI headers directly (zero client.hpp dependency).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <pulse/abi.h>
#include <pulse/commands.h>
#include <pulse/context.h>
#include <pulse/dispatch.h>
#include <pulse/icons.h>
#include <pulse/resources.h>
#include <pulse/search.h>
#include <pulse/settings_pages.h>
#include <pulse/shell.h>
#include <pulse/tray.h>
#include <pulse/workbench.h>

#include "desktop_takeover_record.h"
#include "ipc/assistant_availability.h"
#include "ipc/desktop_ipc.h"
#include "plugin/settings_page.h"
#include "plugin/host_job.h"
#include "plugin/preview_dispatch.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

PULSE_PLUGIN_SDK_PREVIEW()

namespace pulse::desktop::plugin {
namespace {

PulseStringView Sv(std::string_view s) noexcept {
    return PulseStringView{s.empty() ? "" : s.data(), static_cast<uint32_t>(s.size())};
}

std::string CopySv(PulseStringView v) {
    return (v.data && v.length > 0) ? std::string(v.data, v.length) : std::string{};
}

bool IsHeadlessTestMode() noexcept {
    wchar_t val[8]{};
    const DWORD len = GetEnvironmentVariableW(L"PULSE_DESKTOP_HEADLESS_TEST", val, ARRAYSIZE(val));
    return len > 0 && val[0] != L'0';
}

std::wstring ResolveSiblingExecutablePath(const wchar_t* exe_name) {
    HMODULE self_mod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ResolveSiblingExecutablePath),
            &self_mod) || !self_mod) {
        return {};
    }
    wchar_t module_path[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(self_mod, module_path, ARRAYSIZE(module_path));
    if (len == 0 || len >= ARRAYSIZE(module_path)) return {};
    std::wstring dir(module_path, len);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    dir.resize(slash + 1);
    dir += exe_name;
    return dir;
}

// Turns Explorer's desktop icons back on after a host that hid them is gone. The plugin keeps zero static
// USER32.dll imports, so the host executable does the window work: `pulse_desktop_host.exe --restore-icons`.
bool RecoverOrphanedTakeoverDynamic(bool extension_enabled) noexcept {
    const desktop_takeover::TakeoverState st = desktop_takeover::ReadState();
    if (!st.active) return false;
    if (extension_enabled && desktop_takeover::detail::ProcessAlive(st.host_pid)) return false;

    bool spawned = false;
    const std::wstring host_exe = ResolveSiblingExecutablePath(L"pulse_desktop_host.exe");
    if (!host_exe.empty()) {
        std::wstring cmdline = L"\"" + host_exe + L"\" --restore-icons";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(host_exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            spawned = true;
            // Cap Pulse UI thread blocking; pulse_desktop_host.exe --restore-icons clears
            // the Active record itself upon completion.
            const bool finished = WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0;
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return finished;
        }
    }
    if (!spawned) desktop_takeover::ClearActive(desktop_takeover::kDefaultRecordKey, false);
    return false;
}

std::vector<std::string> ParseJsonStringArray(std::string_view json, std::string_view key) {
    std::vector<std::string> out;
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return out;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string_view::npos) return out;
    ++pos;
    while (pos < json.size()) {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',' || json[pos] == '\r' || json[pos] == '\n')) {
            ++pos;
        }
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] != '"') break;
        ++pos;
        std::string item;
        while (pos < json.size()) {
            const char ch = json[pos++];
            if (ch == '"') break;
            if (ch == '\\' && pos < json.size()) {
                const char esc = json[pos++];
                if (esc == '"' || esc == '\\' || esc == '/') item.push_back(esc);
                else if (esc == 'n') item.push_back('\n');
                else if (esc == 'r') item.push_back('\r');
                else if (esc == 't') item.push_back('\t');
            } else {
                item.push_back(ch);
            }
        }
        out.push_back(std::move(item));
    }
    return out;
}

class DesktopPluginInstance {
public:
    explicit DesktopPluginInstance(const PulseHostV1* host) : host_(*host) {}
    ~DesktopPluginInstance() { Destroy(); }

    PulseStatus RegisterEntryHooks() {
        // Recover any orphaned desktop icon takeover left by a previous killed host session.
        if (!IsHeadlessTestMode()) {
            RecoverOrphanedTakeoverDynamic(false);
        }

        if (host_.query_interface(host_.context, Sv(PULSE_DISPATCH_INTERFACE),
                                  PULSE_DISPATCH_VERSION, &dispatch_, sizeof(dispatch_)) == PULSE_OK &&
            dispatch_.post) {
            has_dispatch_ = true;
        }

        if (host_.query_interface(host_.context, Sv(PULSE_COMMANDS_INTERFACE_NAME),
                                  PULSE_COMMANDS_VERSION_1, &commands_, sizeof(commands_)) == PULSE_OK &&
            commands_.register_command) {
            has_commands_ = true;
            RegisterStaticCommands();
        }
        return PULSE_OK;
    }

    PulseStatus Activate() {
        if (active_) return PULSE_OK;
        active_ = true;
        shutting_down_.store(false, std::memory_order_release);

        // Query Workbench (prefer V2, fallback V1).
        if (host_.query_interface(host_.context, Sv(PULSE_WORKBENCH_INTERFACE_NAME),
                                  PULSE_WORKBENCH_VERSION_2, &workbench_, sizeof(workbench_)) == PULSE_OK) {
            has_workbench_v2_ = true;
            has_workbench_v1_ = true;
        } else if (host_.query_interface(host_.context, Sv(PULSE_WORKBENCH_INTERFACE_NAME),
                                         PULSE_WORKBENCH_VERSION_1, &workbench_, sizeof(PulseWorkbenchV1)) == PULSE_OK) {
            has_workbench_v1_ = true;
        }

        if (host_.query_interface(host_.context, Sv(PULSE_TRAY_INTERFACE_NAME),
                                  PULSE_TRAY_VERSION_1, &tray_, sizeof(tray_)) == PULSE_OK &&
            tray_.snapshot) {
            has_tray_ = true;
        }

        if (host_.query_interface(host_.context, Sv(PULSE_SEARCH_INTERFACE_NAME),
                                  PULSE_SEARCH_VERSION_1, &search_, sizeof(search_)) == PULSE_OK &&
            search_.query) {
            has_search_ = true;
        }

        if (host_.query_interface(host_.context, Sv(PULSE_SHELL_INTERFACE_NAME),
                                  PULSE_SHELL_VERSION_1, &shell_, sizeof(shell_)) == PULSE_OK &&
            shell_.get_tag_catalog) {
            has_shell_ = true;
        }

        if (has_workbench_v2_ && workbench_.subscribe_appearance) {
            workbench_.subscribe_appearance(workbench_.context, &OnAppearanceChanged, this, &appearance_sub_);
        }
        if (has_tray_ && tray_.subscribe) {
            tray_.subscribe(tray_.context, &OnTraySnapshot, this, &tray_sub_);
        }

        const bool assistant_enabled = ReadAiOrganizerAvailability();
        const PulseStatus page_st = settings_page_.Open(
            &host_,
            [this](const DesktopPrefs& prefs, bool enabled_toggled) {
                OnPrefsChanged(prefs, enabled_toggled);
            },
            [this](std::string_view cmd) {
                OnSettingsCommand(cmd);
            },
            assistant_enabled);
        if (page_st != PULSE_OK) {
            return page_st;
        }
        assistant_visible_.store(assistant_enabled, std::memory_order_release);
        if (has_dispatch_) {
            CreateTimerQueueTimer(&assistant_refresh_timer_, nullptr, &OnAssistantRefreshTimer, this,
                                  750, 750, WT_EXECUTEDEFAULT);
        }

        if (settings_page_.Prefs().enabled) {
            EnsureHostProcessRunning();
        }
        return PULSE_OK;
    }

    void Deactivate() noexcept {
        if (!active_) return;
        if (preview_dispatch_.Deactivate(has_workbench_v2_ ? &workbench_ : nullptr) != PULSE_OK)
            OutputDebugStringW(L"pulse.desktop: preview cancellation rejected\n");
        active_ = false;
        if (assistant_refresh_timer_) {
            DeleteTimerQueueTimer(nullptr, assistant_refresh_timer_, INVALID_HANDLE_VALUE);
            assistant_refresh_timer_ = nullptr;
        }
        CancelRestartTimer();
        StopHostProcess(true);

        if (active_search_id_ != 0 && has_search_ && search_.cancel) {
            search_.cancel(search_.context, active_search_id_);
            active_search_id_ = 0;
        }
        if (active_preview_id_ != 0 && has_shell_ && shell_.cancel_preview) {
            shell_.cancel_preview(shell_.context, active_preview_id_);
            active_preview_id_ = 0;
        }
        if (appearance_sub_ != 0 && has_workbench_v2_ && workbench_.unsubscribe_appearance) {
            workbench_.unsubscribe_appearance(workbench_.context, appearance_sub_);
            appearance_sub_ = 0;
        }
        if (tray_sub_ != 0 && has_tray_ && tray_.unsubscribe) {
            tray_.unsubscribe(tray_.context, tray_sub_);
            tray_sub_ = 0;
        }

        settings_page_.Close();
        if (!IsHeadlessTestMode() && settings_page_.Prefs().restore_on_exit) {
            RecoverOrphanedTakeoverDynamic(false);
        }
    }

    void Destroy() noexcept {
        if (destroyed_) return;
        destroyed_ = true;
        shutting_down_.store(true, std::memory_order_release);

        Deactivate();
        UnregisterStaticCommands();

        uint64_t pending_tok = 0;
        {
            std::lock_guard<std::mutex> lk(ui_queue_mutex_);
            pending_tok = dispatch_token_;
            dispatch_token_ = 0;
            ui_tasks_.clear();
        }
        if (pending_tok != 0 && has_dispatch_ && dispatch_.cancel) {
            dispatch_.cancel(dispatch_.context, pending_tok);
        }
        if (job_handle_) {
            CloseHandle(job_handle_);
            job_handle_ = nullptr;
        }
    }

private:
    // -----------------------------------------------------------------------
    // Thread-Safe UI Dispatch Queue
    // -----------------------------------------------------------------------
    void PostToUiThread(std::function<void()> fn) {
        if (shutting_down_.load(std::memory_order_acquire) || !has_dispatch_ || !dispatch_.post) return;
        bool need_schedule = false;
        {
            std::lock_guard<std::mutex> lk(ui_queue_mutex_);
            ui_tasks_.push_back(std::move(fn));
            if (!dispatch_scheduled_) {
                dispatch_scheduled_ = true;
                need_schedule = true;
            }
        }
        if (need_schedule) {
            uint64_t tok = 0;
            if (dispatch_.post(dispatch_.context, &OnDispatchDrain, this, &tok) == PULSE_OK) {
                std::lock_guard<std::mutex> lk(ui_queue_mutex_);
                dispatch_token_ = tok;
            } else {
                std::lock_guard<std::mutex> lk(ui_queue_mutex_);
                dispatch_scheduled_ = false;
            }
        }
    }

    static void PULSE_CALL OnDispatchDrain(void* user) noexcept {
        if (!user) return;
        auto* self = static_cast<DesktopPluginInstance*>(user);
        if (self->shutting_down_.load(std::memory_order_acquire)) return;

        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lk(self->ui_queue_mutex_);
            batch.swap(self->ui_tasks_);
            self->dispatch_scheduled_ = false;
            self->dispatch_token_ = 0;
        }
        for (auto& task : batch) {
            if (self->shutting_down_.load(std::memory_order_acquire)) break;
            try {
                if (task) task();
            } catch (...) {}
        }
    }

    // -----------------------------------------------------------------------
    // Static Context Menu Commands
    // -----------------------------------------------------------------------
    void RegisterStaticCommands() {
        struct CmdSpec {
            const char* id;
            const char* title;
            uint32_t icon;
            const char* icon_name;
        };
        static constexpr CmdSpec kSpecs[] = {
            {"desktop-ai-organize", "AI 桌面收纳", PULSE_ICON_SPARKLE, "sparkle"},
            {"desktop-health", "桌面体检", PULSE_ICON_SHIELD, "shield"},
            {"desktop-beautify", "美化桌面", PULSE_ICON_SPARKLE, "sparkle"},
            {"desktop-toggle-icons", "显示/隐藏桌面图标", PULSE_ICON_EYE, "eye"},
        };
        for (size_t i = 0; i < std::size(kSpecs); ++i) {
            PulseCommandDescriptor d{};
            d.size = sizeof(d);
            d.id = Sv(kSpecs[i].id);
            d.title = Sv(kSpecs[i].title);
            d.icon = kSpecs[i].icon;
            d.kinds = PULSE_COMMAND_BACKGROUND;
            d.min_selection = 0;
            d.max_selection = 4096;
            d.invoke = &OnCommandInvoked;
            d.user = this;
            d.group = Sv("桌面整理");
            d.icon_name = Sv(kSpecs[i].icon_name);

            PulseCommandId cid = 0;
            if (commands_.register_command(commands_.context, &d, &cid) == PULSE_OK && cid != 0) {
                registered_commands_.push_back({cid, kSpecs[i].id});
            }
        }

        // Single-file video context menu entry: right-click a supported video in Pulse -> "设为桌面壁纸"
        static const PulseStringView kVideoExts[] = {
            Sv(".mp4"), Sv(".mov"), Sv(".mkv"), Sv(".m4v"), Sv(".webm"), Sv(".wmv"),
        };
        PulseCommandDescriptor vd{};
        vd.size = sizeof(vd);
        vd.id = Sv("desktop-set-video-wallpaper");
        vd.title = Sv("设为桌面壁纸");
        vd.icon = PULSE_ICON_PC;
        vd.kinds = PULSE_COMMAND_FILES;
        vd.min_selection = 1;
        vd.max_selection = 1;
        vd.extensions = kVideoExts;
        vd.extension_count = static_cast<uint32_t>(std::size(kVideoExts));
        vd.invoke = &OnCommandInvoked;
        vd.user = this;
        vd.group = Sv("桌面整理");
        vd.icon_name = Sv("pc");
        PulseCommandId vcid = 0;
        if (commands_.register_command(commands_.context, &vd, &vcid) == PULSE_OK && vcid != 0) {
            registered_commands_.push_back({vcid, "desktop-set-video-wallpaper"});
        }
    }

    void UnregisterStaticCommands() noexcept {
        if (!has_commands_ || !commands_.unregister_command) return;
        for (const auto& [cid, _] : registered_commands_) {
            commands_.unregister_command(commands_.context, cid);
        }
        registered_commands_.clear();
    }

    static void PULSE_CALL OnCommandInvoked(void* user, PulseCommandId id,
                                            const PulseContextSelectionSnapshot* sel) noexcept {
        if (!user) return;
        auto* self = static_cast<DesktopPluginInstance*>(user);
        try {
            for (const auto& [cid, name] : self->registered_commands_) {
                if (cid == id) {
                    if (name == "desktop-set-video-wallpaper") {
                        if (sel && sel->entry_count >= 1 && sel->entries && sel->entries[0].path.data &&
                            sel->entries[0].path.length > 0) {
                            const std::string path_utf8 = CopySv(sel->entries[0].path);
                            self->DispatchHostActionWithPath("set-video-wallpaper", path_utf8);
                        }
                    } else {
                        self->DispatchHostAction(name);
                    }
                    break;
                }
            }
        } catch (...) {}
    }

    void DispatchHostAction(std::string_view action) {
        if (action == "desktop-ai-organize" && !RefreshAiOrganizerAvailability()) return;
        if (!active_) Activate();
        if (!pipe_.IsRunning() && settings_page_.Prefs().enabled) {
            EnsureHostProcessRunning();
        }
        const std::string json = "{\"action\":" + ipc::JsonQuote(action) + "}";
        pipe_.Send(ipc::MessageType::CommandActionMsg, ++next_req_id_, json);
    }

    void DispatchHostActionWithPath(std::string_view action, std::string_view path_utf8) {
        if (!active_) Activate();
        if (!pipe_.IsRunning() && settings_page_.Prefs().enabled) {
            EnsureHostProcessRunning();
        }
        const std::string json = "{\"action\":" + ipc::JsonQuote(action) +
                                 ",\"path\":" + ipc::JsonQuote(path_utf8) + "}";
        pipe_.Send(ipc::MessageType::CommandActionMsg, ++next_req_id_, json);
    }

    // -----------------------------------------------------------------------
    // Settings & Host Process Lifecycle (Job Object + 3-in-10s Breaker)
    // -----------------------------------------------------------------------
    bool ReadAiOrganizerAvailability() {
        std::lock_guard<std::mutex> lock(assistant_availability_mutex_);
        return assistant_availability_.Enabled();
    }

    bool RefreshAiOrganizerAvailability() {
        const bool enabled = ReadAiOrganizerAvailability();
        if (settings_page_.SetAiOrganizerEnabled(enabled))
            assistant_visible_.store(enabled, std::memory_order_release);
        return enabled;
    }

    static VOID CALLBACK OnAssistantRefreshTimer(PVOID ctx, BOOLEAN /*timed_out*/) noexcept {
        auto* self = static_cast<DesktopPluginInstance*>(ctx);
        try {
            if (self->assistant_visible_.load(std::memory_order_acquire) == self->ReadAiOrganizerAvailability()) return;
            self->PostToUiThread([self]() {
                if (self->active_) self->RefreshAiOrganizerAvailability();
            });
        } catch (...) {}
    }

    void OnPrefsChanged(const DesktopPrefs& prefs, bool enabled_toggled) {
        if (enabled_toggled) {
            if (prefs.enabled) {
                if (!IsHeadlessTestMode()) {
                    desktop_takeover::ClearActive(desktop_takeover::kDefaultRecordKey, false);
                }
                EnsureHostProcessRunning();
            } else {
                CancelRestartTimer();
                StopHostProcess(true);
                if (!IsHeadlessTestMode() && prefs.restore_on_exit) {
                    RecoverOrphanedTakeoverDynamic(false);
                }
            }
            return;
        }
        if (pipe_.IsConnected()) {
            pipe_.Send(ipc::MessageType::SettingsSync, ++next_req_id_, prefs.ToJson());
        }
    }

    void OnSettingsCommand(std::string_view cmd) {
        if (cmd == "cmd.ai_organize" || cmd == "desktop-ai-organize") {
            DispatchHostAction("desktop-ai-organize");
        } else if (cmd == "run-health") {
            DispatchHostAction("desktop-health");
            settings_page_.SetStatusText("已触发桌面体检扫描");
        } else if (cmd == "reorganize-desktop") {
            DispatchHostAction("auto-organize");
            settings_page_.SetStatusText("已触发按规则重新整理桌面");
        } else if (cmd == "scene-work" || cmd == "scene-focus" || cmd == "scene-present") {
            DesktopPrefs updated = settings_page_.Prefs();
            updated.scene = (cmd == "scene-focus") ? 1u : ((cmd == "scene-present") ? 2u : 0u);
            settings_page_.UpdateFromHostProcess(updated);
            if (pipe_.IsConnected()) {
                pipe_.Send(ipc::MessageType::SettingsSync, ++next_req_id_, updated.ToJson());
            }
            DispatchHostAction(cmd);
            settings_page_.SetStatusText(
                (cmd == "scene-focus") ? "已切换至专注场景"
                                       : ((cmd == "scene-present") ? "已切换至演示场景" : "已切换至工作场景"));
        } else if (cmd == "restore-icons") {
            DispatchHostAction("restore-native-icons");
            if (!IsHeadlessTestMode()) {
                RecoverOrphanedTakeoverDynamic(false);
            }
            settings_page_.SetStatusText("已触发原生桌面图标位置恢复");
        } else if (cmd == "pick-photo-folder") {
            DispatchHostAction("pick-photo-folder");
            settings_page_.SetStatusText("已打开照片小组件文件夹选择");
        } else if (cmd == "pick-video-wallpaper") {
            DispatchHostAction("pick-video-wallpaper");
        } else if (cmd == "cancel-video-optimize") {
            DispatchHostAction("cancel-video-optimize");
        }
    }

    bool EnsureJobObject() {
        if (job_handle_) return true;
        job_handle_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_handle_) return false;

        if (!ConfigureHostJob(job_handle_)) {
            CloseHandle(job_handle_);
            job_handle_ = nullptr;
            return false;
        }
        return true;
    }

    void EnsureHostProcessRunning() {
        if (IsHeadlessTestMode() || !active_ || !settings_page_.Prefs().enabled || host_process_ != nullptr) return;

        const std::wstring host_exe = ResolveSiblingExecutablePath(L"pulse_desktop_host.exe");
        if (host_exe.empty() || GetFileAttributesW(host_exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            settings_page_.SetStatusText("未找到 pulse_desktop_host.exe");
            return;
        }
        if (!EnsureJobObject()) return;

        stopping_host_.store(false, std::memory_order_release);
        const std::wstring pipe_name = ipc::MakeUniquePipeName();
        if (pipe_name.empty()) return;

        const uint64_t session_gen = ++host_generation_;
        if (!pipe_.StartServer(
                pipe_name,
                [this, session_gen](ipc::Frame&& f) {
                    const uint16_t raw_type = static_cast<uint16_t>(f.type);
                    const uint64_t req_id = f.request_id;
                    std::vector<uint8_t> bytes = std::move(f.payload);
                    PostToUiThread([this, session_gen, raw_type, req_id, payload = std::move(bytes)]() mutable {
                        if (session_gen != host_generation_) return;
                        HandleIpcFrameOnUiThread(static_cast<ipc::MessageType>(raw_type), req_id, std::move(payload));
                    });
                },
                {},
                10000)) {
            return;
        }

        wchar_t cmdline[1024]{};
        std::swprintf(cmdline, ARRAYSIZE(cmdline),
                      L"\"%ls\" --pipe \"%ls\" --parent-pid %lu",
                      host_exe.c_str(), pipe_name.c_str(), GetCurrentProcessId());

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(host_exe.c_str(), cmdline, nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi)) {
            pipe_.Stop();
            settings_page_.SetStatusText("无法启动桌面宿主进程");
            return;
        }

        // Never run an unsupervised host after job assignment fails.
        if (!AssignProcessToJobObject(job_handle_, pi.hProcess) || ResumeThread(pi.hThread) == DWORD(-1)) {
            TerminateProcess(pi.hProcess, ERROR_PROCESS_ABORTED);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            pipe_.Stop();
            return;
        }
        CloseHandle(pi.hThread);
        host_process_ = pi.hProcess;
        host_pid_ = pi.dwProcessId;

        RegisterWaitForSingleObject(
            &process_wait_handle_,
            host_process_,
            &OnHostProcessExitedWait,
            this,
            INFINITE,
            WT_EXECUTEONLYONCE);

        settings_page_.SetStatusText("正在运行 · 更改会立即应用到桌面");
    }

    void StopHostProcess(bool graceful) noexcept {
        stopping_host_.store(true, std::memory_order_release);
        ++host_generation_;

        if (process_wait_handle_) {
            UnregisterWaitEx(process_wait_handle_, INVALID_HANDLE_VALUE);
            process_wait_handle_ = nullptr;
        }

        if (host_process_) {
            if (graceful && pipe_.IsConnected()) {
                const bool restore = settings_page_.Prefs().restore_on_exit;
                pipe_.Send(ipc::MessageType::ShutdownClean, ++next_req_id_,
                           restore ? "{\"restore\":true}" : "{\"restore\":false}");
                WaitForSingleObject(host_process_, 1200);
            }
            DWORD exit_code = 0;
            if (GetExitCodeProcess(host_process_, &exit_code) && exit_code == STILL_ACTIVE) {
                TerminateProcess(host_process_, 0);
                WaitForSingleObject(host_process_, 500);
            }
            CloseHandle(host_process_);
            host_process_ = nullptr;
            host_pid_ = 0;
        }
        pipe_.Stop();
    }

    static VOID CALLBACK OnHostProcessExitedWait(PVOID ctx, BOOLEAN /*timed_out*/) noexcept {
        if (!ctx) return;
        auto* self = static_cast<DesktopPluginInstance*>(ctx);
        if (self->shutting_down_.load(std::memory_order_acquire) ||
            self->stopping_host_.load(std::memory_order_acquire)) {
            return;
        }
        self->PostToUiThread([self]() { self->HandleUnexpectedHostExitOnUi(); });
    }

    void HandleUnexpectedHostExitOnUi() {
        if (!active_ || stopping_host_.load(std::memory_order_acquire)) return;

        DWORD exit_code = 0;
        if (host_process_) {
            GetExitCodeProcess(host_process_, &exit_code);
        }
        StopHostProcess(false);

        // Immediately ensure native desktop icons are visible if host crashed!
        if (!IsHeadlessTestMode()) {
            RecoverOrphanedTakeoverDynamic(false);
        }

        if (exit_code == 0 || !settings_page_.Prefs().enabled) {
            return;
        }

        bool tripped_breaker = false;
        desktop_takeover::RecordCrashEvent(GetTickCount64(), 10000ull, 3u, tripped_breaker);

        if (tripped_breaker) {
            DesktopPrefs disabled = settings_page_.Prefs();
            disabled.enabled = false;
            settings_page_.UpdateFromHostProcess(disabled);
            settings_page_.SetStatusText("检测到桌面宿主在 10 秒内连续异常退出 3 次，已自动熔断并恢复系统原生图标");

            if (has_workbench_v1_ && workbench_.show_notice) {
                static const PulseWorkbenchAction kRetryAction{
                    sizeof(PulseWorkbenchAction), 0, Sv("retry-desktop"), Sv("重新启用")
                };
                PulseNoticeRequest req{};
                req.size = sizeof(req);
                req.kind = PULSE_NOTICE_WARNING;
                req.title = Sv("桌面整理已安全恢复原生桌面");
                req.message = Sv("桌面渲染宿主在 10 秒内异常退出达 3 次，已自动停止接管并完整恢复 Windows 原生图标。");
                req.actions = &kRetryAction;
                req.action_count = 1;
                PulseNotificationId nid = 0;
                workbench_.show_notice(workbench_.context, &req, &OnNoticeAction, this, &nid);
            }
            return;
        }

        // Schedule non-blocking 1-second restart without stalling Pulse's UI thread.
        settings_page_.SetStatusText("桌面宿主意外退出，1 秒后自动重连…");
        CancelRestartTimer();
        CreateTimerQueueTimer(&restart_timer_, nullptr, &OnRestartTimerFired, this, 1000, 0, WT_EXECUTEONLYONCE);
    }

    static VOID CALLBACK OnRestartTimerFired(PVOID ctx, BOOLEAN /*timed_out*/) noexcept {
        if (!ctx) return;
        auto* self = static_cast<DesktopPluginInstance*>(ctx);
        if (self->shutting_down_.load(std::memory_order_acquire)) return;
        self->PostToUiThread([self]() {
            self->CancelRestartTimer();
            if (self->active_ && self->settings_page_.Prefs().enabled) {
                self->EnsureHostProcessRunning();
            }
        });
    }

    void CancelRestartTimer() noexcept {
        if (restart_timer_) {
            DeleteTimerQueueTimer(nullptr, restart_timer_, INVALID_HANDLE_VALUE);
            restart_timer_ = nullptr;
        }
    }

    static void PULSE_CALL OnNoticeAction(void* user, PulseNotificationId /*id*/, PulseStringView action_id) noexcept {
        if (!user) return;
        auto* self = static_cast<DesktopPluginInstance*>(user);
        if (CopySv(action_id) == "retry-desktop") {
            if (!IsHeadlessTestMode()) {
                desktop_takeover::ClearActive(desktop_takeover::kDefaultRecordKey, false);
            }
            DesktopPrefs reenabled = self->settings_page_.Prefs();
            reenabled.enabled = true;
            self->settings_page_.UpdateFromHostProcess(reenabled);
            self->EnsureHostProcessRunning();
        }
    }

    // -----------------------------------------------------------------------
    // Bidirectional IPC Message Handlers (Pulse UI Thread)
    // -----------------------------------------------------------------------
    void HandleIpcFrameOnUiThread(ipc::MessageType type, uint64_t req_id, std::vector<uint8_t> payload) {
        const std::string_view json(reinterpret_cast<const char*>(payload.data()), payload.size());

        switch (type) {
        case ipc::MessageType::Hello: {
            if (json.find("\"paths\"") != std::string_view::npos) {
                watched_desktop_paths_ = ParseJsonStringArray(json, "paths");
            }
            last_tag_sync_hash_ = 0;
            pipe_.Send(ipc::MessageType::SettingsSync, ++next_req_id_, settings_page_.Prefs().ToJson());
            SendCurrentAppearance();
            SendCurrentTraySnapshot();
            SyncTagsToDesktop(true);
            break;
        }

        case ipc::MessageType::Ping: {
            bool has_paths = false;
            if (json.find("\"paths\"") != std::string_view::npos) {
                watched_desktop_paths_ = ParseJsonStringArray(json, "paths");
                has_paths = true;
            }
            pipe_.Send(ipc::MessageType::Pong, req_id, "{}");
            if (has_paths) {
                ping_tag_poll_ticks_ = 0;
                SyncTagsToDesktop(true);
            } else if ((++ping_tag_poll_ticks_ % 10u) == 0u) {
                SyncTagsToDesktop(false);
            }
            break;
        }

        case ipc::MessageType::SettingsSync: {
            DesktopPrefs updated = settings_page_.Prefs();
            updated.ApplyJson(json);
            updated.enabled = settings_page_.Prefs().enabled;
            settings_page_.UpdateFromHostProcess(updated);
            std::string vname, vstatus;
            const bool has_vname = ipc::JsonFindString(json, "video_name", vname);
            const bool has_vstatus = ipc::JsonFindString(json, "video_status", vstatus);
            if (has_vname || has_vstatus) {
                settings_page_.SetVideoWallpaperStatus(vname, vstatus);
            }
            break;
        }

        case ipc::MessageType::OpenInPulseReq: {
            PulseStatus preview_status = PULSE_OK;
            if (preview_dispatch_.Dispatch(json, has_workbench_v2_ ? &workbench_ : nullptr, preview_status)) {
                if (preview_status != PULSE_OK) OutputDebugStringW(L"pulse.desktop: preview request rejected\n");
                break;
            }
            std::string mode;
            std::string path_utf8;
            std::string query_utf8;
            ipc::JsonFindString(json, "mode", mode);
            ipc::JsonFindString(json, "path", path_utf8);
            ipc::JsonFindString(json, "query", query_utf8);
            if (mode == "settings" && path_utf8.empty()) {
                path_utf8 = "pulse:settings:native-extensions/pulse.desktop";
            }

            if (path_utf8 == "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}") {
                // The public path API rejects This PC's empty path; use the existing single-instance route.
                if (!IsHeadlessTestMode()) {
                    wchar_t exe[MAX_PATH * 4]{};
                    const DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
                    if (n && n < ARRAYSIZE(exe)) {
                        AllowSetForegroundWindow(ASFW_ANY);
                        std::wstring command = L"\"" + std::wstring(exe, n) + L"\" --this-pc";
                        STARTUPINFOW si{};
                        si.cb = sizeof(si);
                        PROCESS_INFORMATION pi{};
                        if (CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                            CloseHandle(pi.hThread);
                            CloseHandle(pi.hProcess);
                        }
                    }
                }
            } else if (mode == "quick_search" && has_workbench_v2_ && workbench_.open_search) {
                // Pulse's quick-search window, seeded with what was typed on the desktop.
                const std::string uri = "pulse:quick-search:" + query_utf8;
                workbench_.open_search(workbench_.context, Sv(uri));
            } else if (mode == "search" && has_workbench_v2_ && workbench_.open_search) {
                workbench_.open_search(workbench_.context, Sv(query_utf8));
            } else if (!path_utf8.empty() && path_utf8.starts_with("pulse:")) {
                // Virtual Pulse URIs (e.g. "pulse:settings:assistant") open directly in a new Pulse tab.
                PulseStatus nav_st = PULSE_ERROR_UNAVAILABLE;
                if (has_workbench_v2_ && workbench_.reveal_path_from_rect) {
                    nav_st = workbench_.reveal_path_from_rect(workbench_.context, Sv(path_utf8),
                                                              PULSE_NAVIGATE_NEW_TAB, nullptr);
                }
                if (nav_st != PULSE_OK && has_workbench_v1_ && workbench_.reveal_path) {
                    nav_st = workbench_.reveal_path(workbench_.context, Sv(path_utf8), PULSE_NAVIGATE_NEW_TAB);
                }
                if (nav_st != PULSE_OK && !IsHeadlessTestMode()) {
                    wchar_t host_exe[MAX_PATH * 4]{};
                    const DWORD n = GetModuleFileNameW(nullptr, host_exe, ARRAYSIZE(host_exe));
                    const std::wstring wuri = ipc::Utf8ToWide(path_utf8);
                    if (n > 0 && n < ARRAYSIZE(host_exe) && !wuri.empty()) {
                        AllowSetForegroundWindow(ASFW_ANY);
                        std::wstring cmdline = L"\"" + std::wstring(host_exe, n) + L"\" \"" + wuri + L"\"";
                        STARTUPINFOW si{};
                        si.cb = sizeof(si);
                        PROCESS_INFORMATION pi{};
                        if (CreateProcessW(host_exe, cmdline.data(), nullptr, nullptr, FALSE,
                                           0, nullptr, nullptr, &si, &pi)) {
                            if (pi.dwProcessId != 0) AllowSetForegroundWindow(pi.dwProcessId);
                            CloseHandle(pi.hThread);
                            CloseHandle(pi.hProcess);
                        }
                    }
                }
            } else if (!path_utf8.empty() && has_workbench_v1_ && workbench_.reveal_path) {
                const uint32_t nav_mode = (mode == "reveal")
                                              ? PULSE_NAVIGATE_REVEAL_ITEM
                                              : ((mode == "new_tab") ? PULSE_NAVIGATE_NEW_TAB : PULSE_NAVIGATE_CURRENT_TAB);
                int64_t rx = 0, ry = 0, rw = 0, rh = 0;
                const bool has_rect =
                    (ipc::JsonFindInt64(json, "rect_x", rx) && ipc::JsonFindInt64(json, "rect_y", ry) &&
                     ipc::JsonFindInt64(json, "rect_w", rw) && ipc::JsonFindInt64(json, "rect_h", rh) &&
                     rw > 0 && rh > 0) ||
                    (ipc::JsonFindInt64(json, "x", rx) && ipc::JsonFindInt64(json, "y", ry) &&
                     ipc::JsonFindInt64(json, "w", rw) && ipc::JsonFindInt64(json, "h", rh) &&
                     rw > 0 && rh > 0);
                if (has_rect && has_workbench_v2_ && workbench_.reveal_path_from_rect) {
                    const PulseScreenRect rect{
                        static_cast<int32_t>(rx), static_cast<int32_t>(ry),
                        static_cast<int32_t>(rw), static_cast<int32_t>(rh)
                    };
                    workbench_.reveal_path_from_rect(workbench_.context, Sv(path_utf8), nav_mode, &rect);
                } else {
                    workbench_.reveal_path(workbench_.context, Sv(path_utf8), nav_mode);
                }
            }
            break;
        }

        case ipc::MessageType::TrayMutateReq: {
            if (!has_tray_) break;
            std::string op;
            ipc::JsonFindString(json, "op", op);
            if (op == "collect" && tray_.collect) {
                const auto paths = ParseJsonStringArray(json, "paths");
                std::vector<PulseStringView> views;
                views.reserve(paths.size());
                for (const auto& p : paths) views.push_back(Sv(p));
                bool move_intent = false;
                ipc::JsonFindBool(json, "move", move_intent);
                if (!views.empty()) {
                    tray_.collect(tray_.context, views.data(), static_cast<uint32_t>(views.size()),
                                  move_intent ? 1u : 0u);
                }
            } else if (op == "remove" && tray_.remove_item) {
                uint64_t id = 0;
                if (ipc::JsonFindUint64(json, "item_id", id)) {
                    tray_.remove_item(tray_.context, id);
                }
            } else if (op == "clear" && tray_.clear) {
                tray_.clear(tray_.context);
            } else if (op == "release" && tray_.release_to_directory) {
                std::string dest;
                if (ipc::JsonFindString(json, "dest", dest) && !dest.empty()) {
                    tray_.release_to_directory(tray_.context, Sv(dest), PULSE_TRAY_RELEASE_BATCH_DEFAULT);
                }
            }
            break;
        }

        case ipc::MessageType::SearchQueryReq: {
            if (!has_search_ || !search_.query) break;
            std::string query_utf8;
            ipc::JsonFindString(json, "query", query_utf8);
            if (active_search_id_ != 0 && search_.cancel) {
                search_.cancel(search_.context, active_search_id_);
                active_search_id_ = 0;
            }
            if (query_utf8.empty()) break;

            active_search_ipc_req_ = req_id;
            PulseSearchRequest sreq{};
            sreq.size = sizeof(sreq);
            sreq.flags = PULSE_SEARCH_FLAG_PINYIN;
            sreq.max_results = 10;
            sreq.query = Sv(query_utf8);
            search_.query(search_.context, &sreq, &OnSearchReply, this, &active_search_id_);
            break;
        }

        case ipc::MessageType::SetTagReq: {
            if (!has_shell_ || !shell_.set_path_tag) break;
            std::string path_utf8;
            std::string tag_id;
            std::string prev_tag_id;
            bool tagged = true;
            bool clear_all = false;
            ipc::JsonFindString(json, "path", path_utf8);
            ipc::JsonFindString(json, "tag_id", tag_id);
            ipc::JsonFindString(json, "prev_tag_id", prev_tag_id);
            ipc::JsonFindBool(json, "tagged", tagged);
            ipc::JsonFindBool(json, "clear_all", clear_all);
            if (!path_utf8.empty()) {
                if (clear_all || (!tagged && (tag_id == "*" || tag_id == "clear"))) {
                    shell_.set_path_tag(shell_.context, Sv(path_utf8), Sv("*"), 0u);
                } else {
                    if (!prev_tag_id.empty() && prev_tag_id != tag_id) {
                        shell_.set_path_tag(shell_.context, Sv(path_utf8), Sv(prev_tag_id), 0u);
                    }
                    if (!tag_id.empty()) {
                        shell_.set_path_tag(shell_.context, Sv(path_utf8), Sv(tag_id), tagged ? 1u : 0u);
                    }
                }
                SyncTagsToDesktop(true);
            }
            break;
        }

        case ipc::MessageType::ShellMenuReq: {
            if (!has_shell_ || !shell_.query_context_menu) break;
            const auto paths = ParseJsonStringArray(json, "paths");
            if (paths.empty()) break;
            std::vector<PulseStringView> views;
            views.reserve(paths.size());
            for (const auto& p : paths) views.push_back(Sv(p));
            bool bg = false;
            ipc::JsonFindBool(json, "background", bg);

            active_menu_ipc_req_ = req_id;
            PulseShellMenuRequest mreq{};
            mreq.size = sizeof(mreq);
            mreq.flags = bg ? PULSE_SHELL_MENU_QUERY_BACKGROUND : 0u;
            mreq.paths = views.data();
            mreq.path_count = static_cast<uint32_t>(views.size());
            PulseShellMenuSessionId sid = 0;
            shell_.query_context_menu(shell_.context, &mreq, &OnShellMenuReply, this, &sid);
            break;
        }

        case ipc::MessageType::ShellMenuInvokeReq: {
            if (!has_shell_) break;
            uint64_t sid = 0;
            uint64_t item_id = 0;
            bool close_only = false;
            std::string verb;
            ipc::JsonFindUint64(json, "session_id", sid);
            ipc::JsonFindUint64(json, "item_id", item_id);
            ipc::JsonFindBool(json, "close", close_only);
            ipc::JsonFindString(json, "verb", verb);
            if (sid != 0) {
                if (close_only && shell_.close_context_menu) {
                    shell_.close_context_menu(shell_.context, sid);
                } else if (shell_.invoke_context_menu) {
                    shell_.invoke_context_menu(shell_.context, sid, static_cast<uint32_t>(item_id), Sv(verb));
                }
            }
            break;
        }

        case ipc::MessageType::PreviewReq: {
            if (!has_shell_ || !shell_.read_preview) break;
            std::string path_utf8;
            ipc::JsonFindString(json, "path", path_utf8);
            if (active_preview_id_ != 0 && shell_.cancel_preview) {
                shell_.cancel_preview(shell_.context, active_preview_id_);
                active_preview_id_ = 0;
            }
            if (!path_utf8.empty()) {
                active_preview_ipc_req_ = req_id;
                shell_.read_preview(shell_.context, Sv(path_utf8), 512u, &OnShellPreviewReply, this, &active_preview_id_);
            }
            break;
        }

        default:
            break;
        }
    }

    // -----------------------------------------------------------------------
    // Host Subscription Callbacks -> IPC Sync
    // -----------------------------------------------------------------------
    void SendCurrentAppearance() {
        if (!has_workbench_v2_ || !workbench_.query_appearance) return;
        PulseHostAppearance app{};
        app.size = sizeof(app);
        if (workbench_.query_appearance(workbench_.context, &app) == PULSE_OK) {
            OnAppearanceChanged(this, &app);
        }
    }

    static void PULSE_CALL OnAppearanceChanged(void* user, const PulseHostAppearance* app) noexcept {
        if (!user || !app) return;
        auto* self = static_cast<DesktopPluginInstance*>(user);
        if (!self->pipe_.IsConnected()) return;
        char buf[256]{};
        std::snprintf(buf, sizeof(buf),
                      "{\"dark\":%s,\"effect\":%u,\"opacity\":%u,\"accent\":%u,\"font_scale\":%u}",
                      (app->flags & PULSE_APPEARANCE_DARK) ? "true" : "false",
                      app->window_effect,
                      app->opacity_percent,
                      app->accent_argb,
                      app->font_scale_milli);
        self->pipe_.Send(ipc::MessageType::AppearanceSync, ++self->next_req_id_, buf);
    }

    void SendCurrentTraySnapshot() {
        if (has_tray_ && tray_.snapshot) {
            tray_.snapshot(tray_.context, &OnTraySnapshot, this);
        }
    }

    static void PULSE_CALL OnTraySnapshot(void* user, const PulseTraySnapshot* snap) noexcept {
        if (!user || !snap) return;
        auto* self = static_cast<DesktopPluginInstance*>(user);
        if (!self->pipe_.IsConnected()) return;
        try {
            std::string json = "{\"total_bytes\":" + std::to_string(snap->total_bytes) + ",\"entries\":[";
            for (uint32_t i = 0; i < snap->entry_count; ++i) {
                if (i > 0) json.push_back(',');
                const auto& e = snap->entries[i];
                json += "{\"id\":" + std::to_string(e.item_id);
                json += ",\"size\":" + std::to_string(e.size_bytes);
                json += ",\"folder\":" + std::string((e.flags & PULSE_TRAY_ITEM_FOLDER) ? "true" : "false");
                json += ",\"move\":" + std::string((e.flags & PULSE_TRAY_ITEM_MOVE_INTENT) ? "true" : "false");
                json += ",\"path\":" + ipc::JsonQuote(CopySv(e.path));
                json += ",\"name\":" + ipc::JsonQuote(CopySv(e.name)) + "}";
            }
            json += "]}";
            self->pipe_.Send(ipc::MessageType::TraySnapshotSync, ++self->next_req_id_, json);
        } catch (...) {}
    }

    struct TagCatalogItemCapture {
        std::string id;
        std::string name;
        uint32_t argb = 0;
        int32_t fav = 0;
    };

    static void PULSE_CALL CaptureTagListCallback(void* user, const PulseColorTagInfo* tags, uint32_t count) noexcept {
        if (!user) return;
        auto* out = static_cast<std::vector<TagCatalogItemCapture>*>(user);
        try {
            out->clear();
            out->reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                TagCatalogItemCapture item;
                item.id = CopySv(tags[i].id);
                item.name = CopySv(tags[i].name);
                item.argb = tags[i].argb;
                item.fav = tags[i].favorite_slot;
                out->push_back(std::move(item));
            }
        } catch (...) {}
    }

    void SyncTagsToDesktop(bool force) {
        if (!has_shell_ || !shell_.get_tag_catalog || !pipe_.IsConnected()) return;
        try {
            std::vector<TagCatalogItemCapture> catalog;
            if (shell_.get_tag_catalog(shell_.context, &CaptureTagListCallback, &catalog) != PULSE_OK) {
                return;
            }

            std::string json = "{\"tags\":[";
            for (size_t i = 0; i < catalog.size(); ++i) {
                if (i > 0) json.push_back(',');
                json += "{\"id\":" + ipc::JsonQuote(catalog[i].id);
                json += ",\"name\":" + ipc::JsonQuote(catalog[i].name);
                json += ",\"fav\":" + std::to_string(catalog[i].fav);
                json += ",\"argb\":" + std::to_string(catalog[i].argb) + "}";
            }
            json += "],\"tagged_items\":[";
            if (!catalog.empty() && shell_.get_path_tags && !watched_desktop_paths_.empty()) {
                bool first = true;
                std::vector<TagCatalogItemCapture> ptags;
                for (const auto& p : watched_desktop_paths_) {
                    if (p.empty()) continue;
                    ptags.clear();
                    if (shell_.get_path_tags(shell_.context, Sv(p), &CaptureTagListCallback, &ptags) == PULSE_OK &&
                        !ptags.empty()) {
                        if (!first) json.push_back(',');
                        first = false;
                        json += "{\"path\":" + ipc::JsonQuote(p);
                        json += ",\"id\":" + ipc::JsonQuote(ptags.front().id);
                        json += ",\"argb\":" + std::to_string(ptags.front().argb) + "}";
                    }
                }
            }
            json += "]}";

            uint64_t h = 14695981039346656037ULL;
            for (unsigned char c : json) {
                h ^= static_cast<uint64_t>(c);
                h *= 1099511628211ULL;
            }
            if (!force && h == last_tag_sync_hash_) return;
            last_tag_sync_hash_ = h;
            pipe_.Send(ipc::MessageType::TagSyncMsg, ++next_req_id_, json);
        } catch (...) {}
    }

    static void PULSE_CALL OnSearchReply(void* user, const PulseSearchReply* reply) noexcept {
        if (!user || !reply) return;
        auto* self = static_cast<DesktopPluginInstance*>(user);
        self->active_search_id_ = 0;
        if (!self->pipe_.IsConnected()) return;
        try {
            std::string json = "{\"total\":" + std::to_string(reply->total_matches) + ",\"hits\":[";
            for (uint32_t i = 0; i < reply->hit_count; ++i) {
                if (i > 0) json.push_back(',');
                const auto& h = reply->hits[i];
                json += "{\"path\":" + ipc::JsonQuote(CopySv(h.path));
                json += ",\"name\":" + ipc::JsonQuote(CopySv(h.name));
                json += ",\"size\":" + std::to_string(h.size_bytes);
                json += ",\"folder\":" + std::string((h.flags & PULSE_SEARCH_HIT_FOLDER) ? "true" : "false") + "}";
            }
            json += "]}";
            self->pipe_.Send(ipc::MessageType::SearchReplyMsg, self->active_search_ipc_req_, json);
        } catch (...) {}
    }

    static void PULSE_CALL OnShellMenuReply(void* user, const PulseShellMenuReply* reply) noexcept {
        if (!user || !reply) return;
        auto* self = static_cast<DesktopPluginInstance*>(user);
        if (!self->pipe_.IsConnected()) return;
        try {
            std::string json = "{\"session_id\":" + std::to_string(reply->session_id) + ",\"items\":[";
            for (uint32_t i = 0; i < reply->item_count; ++i) {
                if (i > 0) json.push_back(',');
                const auto& it = reply->items[i];
                json += "{\"id\":" + std::to_string(it.item_id);
                json += ",\"enabled\":" + std::string((it.flags & PULSE_SHELL_MENU_ITEM_ENABLED) ? "true" : "false");
                json += ",\"sep\":" + std::string((it.flags & PULSE_SHELL_MENU_ITEM_SEPARATOR_AFTER) ? "true" : "false");
                json += ",\"submenu\":" + std::string((it.flags & PULSE_SHELL_MENU_ITEM_SUBMENU) ? "true" : "false");
                json += ",\"depth\":" + std::to_string(it.depth);
                json += ",\"handler\":" + ipc::JsonQuote(CopySv(it.handler_name));
                json += ",\"verb\":" + ipc::JsonQuote(CopySv(it.verb));
                json += ",\"label\":" + ipc::JsonQuote(CopySv(it.label)) + "}";
            }
            json += "]}";
            self->pipe_.Send(ipc::MessageType::ShellMenuReplyMsg, self->active_menu_ipc_req_, json);
        } catch (...) {}
    }

    static void PULSE_CALL OnShellPreviewReply(void* user, const PulseShellPreviewSummary* summary) noexcept {
        if (!user || !summary) return;
        auto* self = static_cast<DesktopPluginInstance*>(user);
        self->active_preview_id_ = 0;
        if (!self->pipe_.IsConnected()) return;
        try {
            ipc::PreviewPayloadData d{};
            d.status = summary->status;
            d.size_bytes = summary->size_bytes;
            d.modified_unix_ms = summary->modified_unix_ms;
            d.image_width = summary->image_width;
            d.image_height = summary->image_height;
            d.thumb_width = summary->thumb_width;
            d.thumb_height = summary->thumb_height;
            d.thumb_stride = summary->thumb_stride;
            d.path = ipc::Utf8ToWide(CopySv(summary->path));
            d.display_type = ipc::Utf8ToWide(CopySv(summary->display_type));
            d.text_excerpt = ipc::Utf8ToWide(CopySv(summary->text_excerpt));
            if (summary->thumb_bgra && summary->thumb_byte_count > 0 &&
                summary->thumb_byte_count <= ipc::kMaxPayloadBytes - 4096u) {
                d.thumb_bgra.assign(summary->thumb_bgra, summary->thumb_bgra + summary->thumb_byte_count);
            }
            self->pipe_.SendBinary(ipc::MessageType::PreviewReplyMsg,
                                   self->active_preview_ipc_req_,
                                   ipc::EncodePreviewPayload(d));
        } catch (...) {}
    }

    PulseHostV1 host_{};
    PulseDispatchV1 dispatch_{};
    PulseCommandsV1 commands_{};
    PulseWorkbenchV2 workbench_{};
    PreviewDispatch preview_dispatch_;
    PulseTrayV1 tray_{};
    PulseSearchV1 search_{};
    PulseShellV1 shell_{};

    bool has_dispatch_ = false;
    bool has_commands_ = false;
    bool has_workbench_v1_ = false;
    bool has_workbench_v2_ = false;
    bool has_tray_ = false;
    bool has_search_ = false;
    bool has_shell_ = false;

    bool active_ = false;
    bool destroyed_ = false;
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> stopping_host_{false};

    std::vector<std::pair<PulseCommandId, std::string>> registered_commands_;
    PulseAppearanceSubscription appearance_sub_ = 0;
    PulseTraySubscription tray_sub_ = 0;
    PulseSearchRequestId active_search_id_ = 0;
    uint64_t active_search_ipc_req_ = 0;
    uint64_t active_menu_ipc_req_ = 0;
    PulseShellPreviewRequestId active_preview_id_ = 0;
    uint64_t active_preview_ipc_req_ = 0;

    std::vector<std::string> watched_desktop_paths_;
    uint64_t last_tag_sync_hash_ = 0;
    uint32_t ping_tag_poll_ticks_ = 0;

    ai::AssistantAvailability assistant_availability_;
    std::mutex assistant_availability_mutex_;
    std::atomic<bool> assistant_visible_{true};
    DesktopSettingsPage settings_page_;
    HANDLE assistant_refresh_timer_ = nullptr;
    ipc::AsyncPipeEndpoint pipe_;
    uint64_t next_req_id_ = 1;
    uint64_t host_generation_ = 0;

    HANDLE job_handle_ = nullptr;
    HANDLE host_process_ = nullptr;
    HANDLE process_wait_handle_ = nullptr;
    HANDLE restart_timer_ = nullptr;
    DWORD host_pid_ = 0;

    std::mutex ui_queue_mutex_;
    std::deque<std::function<void()>> ui_tasks_;
    bool dispatch_scheduled_ = false;
    uint64_t dispatch_token_ = 0;
};

PulseStatus PULSE_CALL PluginActivate(void* instance) noexcept {
    if (!instance) return PULSE_ERROR_ARGUMENT;
    try {
        return static_cast<DesktopPluginInstance*>(instance)->Activate();
    } catch (...) {
        return PULSE_ERROR_INITIALIZATION;
    }
}

void PULSE_CALL PluginDeactivate(void* instance) noexcept {
    if (!instance) return;
    try {
        static_cast<DesktopPluginInstance*>(instance)->Deactivate();
    } catch (...) {}
}

void PULSE_CALL PluginDestroy(void* instance) noexcept {
    if (!instance) return;
    auto* self = static_cast<DesktopPluginInstance*>(instance);
    self->Destroy();
    delete self;
}

} // namespace
} // namespace pulse::desktop::plugin

extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(
    const PulseHostV1* host, PulsePluginV1* plugin) {
    if (!host || !plugin) return PULSE_ERROR_ARGUMENT;
    if (host->version != PULSE_ABI_VERSION_1 || host->size < sizeof(PulseHostV1) || !host->query_interface) {
        return PULSE_ERROR_ABI;
    }
    if (plugin->version != PULSE_ABI_VERSION_1 || plugin->size < sizeof(PulsePluginV1)) {
        return PULSE_ERROR_ABI;
    }

    try {
        auto* inst = new pulse::desktop::plugin::DesktopPluginInstance(host);
        plugin->instance = inst;
        plugin->activate = &pulse::desktop::plugin::PluginActivate;
        plugin->deactivate = &pulse::desktop::plugin::PluginDeactivate;
        plugin->destroy = &pulse::desktop::plugin::PluginDestroy;
        return inst->RegisterEntryHooks();
    } catch (...) {
        return PULSE_ERROR_INITIALIZATION;
    }
}
