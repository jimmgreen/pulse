#include "explorer_takeover_probe.h"
#include "explorer_source_observation.h"
#include "app_state.h"
#include "app_runtime.h"
#include "app_messages.h"
#include "app_navigation.h"
#include "window_registry.h"
#include "../common/path_utils.h"
#include <filesystem>
#include <cstring>
#include <cstdio>

namespace pulse {
namespace {
struct Probe {
    std::shared_ptr<const app::ExplorerSourceScope> scope;
    std::wstring directory;
    uint64_t started = 0, stopping = 0;
    bool initialized = false, finished = false, pass = false;
    bool ready_written = false, disabled_by_probe = false, resumed = false;
    uint64_t received_tick = 0;
    size_t baseline_tabs = 0, baseline_windows = 0;
    const void* baseline_active = nullptr;
    uint64_t closed_observed_tick = 0;
    unsigned receive_attempts = 0, receive_successes = 0, deferred = 0, dispatched = 0;
    std::shared_ptr<app::ExplorerHandoff> delivery_handoff;
    std::shared_ptr<app::ExplorerHandoff> presented_handoff;
    uint64_t presented_generation = 0, presented_tick = 0;
    uint64_t prepared_tick = 0;
    app::ExplorerSourceObservation initial_source;
    std::string report;
    std::array<std::string, 64> navigation;
    size_t navigation_count = 0;
};
Probe& Current() { static Probe value; return value; }
size_t TabCount(const AppState& state) {
    size_t count = 0;
    state.core.windows.ForEach([&](HWND, const AppState& window) { count += window.window_tabs.items.size(); });
    return count;
}
std::string HResult(long result) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(result));
    return text;
}
std::wstring Env(const wchar_t* name) {
    wchar_t text[32768]{};
    const DWORD count = GetEnvironmentVariableW(name, text, ARRAYSIZE(text));
    return count && count < ARRAYSIZE(text) ? text : L"";
}
void Write(AppState& state, Probe& probe) {
    const auto evidence = probe.scope->evidence;
    probe.report += std::string("WATCHER\tstage=") + evidence->stage.load() +
        "\tresult=" + HResult(evidence->result.load()) +
        "\tthread_created=" + std::to_string(evidence->thread_created.load()) +
        "\tthread_id=" + std::to_string(evidence->thread_id.load()) +
        "\tstop_requested=" + std::to_string(evidence->stop_requested.load()) +
        "\tcount=" + std::to_string(evidence->shell_count.load()) +
        "\tindex=" + std::to_string(evidence->shell_index.load()) + "\n";
    {
        std::lock_guard lock(evidence->mutex);
        const size_t begin = evidence->trace_count > evidence->trace.size() ? evidence->trace_count - evidence->trace.size() : 0;
        for (size_t index = begin; index < evidence->trace_count; ++index) {
            const auto& entry = evidence->trace[index % evidence->trace.size()];
            probe.report += "TRACE\t" + std::to_string(entry.tick) + "\t" + entry.stage + "\t" + HResult(entry.result) + "\n";
        }
        const size_t handoff_begin = evidence->handoff_trace_count > evidence->handoff_trace.size() ? evidence->handoff_trace_count - evidence->handoff_trace.size() : 0;
        for (size_t index = handoff_begin; index < evidence->handoff_trace_count; ++index) {
            const auto& entry = evidence->handoff_trace[index % evidence->handoff_trace.size()];
            probe.report += "HANDOFF_TRACE\t" + std::to_string(entry.tick) + "\t" + entry.stage + "\t" + HResult(entry.result) + "\n";
        }
    }
    const size_t begin = probe.navigation_count > probe.navigation.size() ? probe.navigation_count - probe.navigation.size() : 0;
    for (size_t index = begin; index < probe.navigation_count; ++index)
        probe.report += probe.navigation[index % probe.navigation.size()];
    const auto file = probe.directory + L"\\takeover-report.tsv";
    HANDLE output = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(output, probe.report.data(), static_cast<DWORD>(probe.report.size()), &written, nullptr);
        CloseHandle(output);
    }
    probe.finished = true;
    if (probe.disabled_by_probe && IsWindow(state.hwnd)) EnableWindow(state.hwnd, TRUE);
    PostMessageW(state.hwnd, WM_EXIT_PULSE, 0, 0);
}
}
void ExplorerTakeoverProbeDelivery(AppState& state, const std::shared_ptr<app::ExplorerHandoff>& handoff, const char* stage) {
    auto& probe = Current();
    if (!state.isolatedTest || !probe.scope || probe.finished || !handoff) return;
    if (!probe.delivery_handoff) probe.delivery_handoff = handoff;
    if (probe.delivery_handoff != handoff) return;
    if (std::strcmp(stage, "receive-attempt") == 0) {
        if (!probe.receive_attempts) {
            probe.baseline_tabs = TabCount(state);
            probe.baseline_windows = state.core.windows.Count();
            probe.baseline_active = state.window_tabs.Active();
        }
        ++probe.receive_attempts;
    } else if (std::strcmp(stage, "received") == 0) {
        ++probe.receive_successes; probe.received_tick = GetTickCount64();
    } else if (std::strcmp(stage, "deferred") == 0) ++probe.deferred;
    else if (std::strcmp(stage, "dispatched") == 0) ++probe.dispatched;
    // Keep repeated busy retries counted without flooding the bounded phase trace.
    if (std::strcmp(stage, "deferred") != 0 || probe.deferred == 1)
        ExplorerTakeoverProbeNavigation(state, ActiveTab(state), stage);
}
void ExplorerTakeoverProbeNavigation(AppState& state, const app::Tab* tab, const char* reason, unsigned detail) {
    auto& probe = Current();
    if (!state.isolatedTest || !probe.scope || probe.finished) return;
    app::ExplorerTakeoverRequest request;
    { std::lock_guard lock(probe.scope->evidence->mutex); request = probe.scope->evidence->submitted; }
    const auto* lease = tab ? tab->explorer_handoff.get() : nullptr;
    const auto handoff = lease ? lease->handoff : request.handoff;
    probe.navigation[probe.navigation_count++ % probe.navigation.size()] =
        std::string("NAVIGATION\treason=") + reason + "\tdetail=" + std::to_string(detail) +
        "\ttick=" + std::to_string(GetTickCount64()) +
        "\tpending=" + std::to_string(tab ? tab->pending_generation : 0) +
        "\tapplied=" + std::to_string(tab ? tab->applied_generation : 0) +
        "\tlease=" + std::to_string(lease ? lease->generation : 0) +
        "\tlisting_complete=" + std::to_string(lease && lease->listing_complete) +
        "\tloading=" + std::to_string(tab && tab->loading) +
        "\thandoff=" + std::to_string(handoff ? static_cast<int>(handoff->state.load()) : -1) +
        "\tdeadline=" + std::to_string(handoff ? handoff->deadline_tick : 0) + "\n";
}
void ExplorerTakeoverProbePresented(AppState& state, const app::Tab* tab, bool acknowledged) {
    auto& probe = Current();
    if (!acknowledged || !state.isolatedTest || !probe.scope || probe.finished || !tab || !tab->explorer_handoff) return;
    app::ExplorerTakeoverRequest request;
    { std::lock_guard lock(probe.scope->evidence->mutex); request = probe.scope->evidence->submitted; }
    const auto& lease = *tab->explorer_handoff;
    if (!request.handoff || lease.handoff != request.handoff || !lease.listing_complete ||
        !lease.generation || !lease.prepared_tick || request.handoff->state != app::HandoffState::Closed ||
        tab->pending_generation || tab->applied_generation != lease.generation ||
        !request.handoff->received.load()) return;
    probe.presented_handoff = lease.handoff;
    probe.prepared_tick = lease.prepared_tick;
    probe.presented_generation = lease.generation; probe.presented_tick = GetTickCount64();
    ExplorerTakeoverProbeNavigation(state, tab, "successful-presented", 1);
}
bool ExplorerTakeoverProbeRequested(const AppState& state) {
    // Any request suppresses the legacy unrestricted test observer, even if
    // the complete gate/config below is invalid.
    return state.isolatedTest && !Env(L"PULSE_TEST_SCOPED_TAKEOVER").empty();
}
std::shared_ptr<const app::ExplorerSourceScope> ExplorerTakeoverProbeScope(const AppState& state) {
    const auto& probe = Current();
    return ExplorerTakeoverProbeRequested(state) && !probe.stopping && !probe.finished ? probe.scope : nullptr;
}
void InitializeExplorerTakeoverProbe(AppState& state) {
    if (!ExplorerTakeoverProbeRequested(state)) return;
    auto& probe = Current();
    if (probe.initialized) return;
    probe.initialized = true;
    state.appPrefs.persist = false;
    state.appPrefs.auto_check_updates = false;
    if (Env(L"PULSE_TEST_SCOPED_TAKEOVER") != L"1") return;
    wchar_t desktop[256]{}, executable[32768]{}; DWORD bytes = 0;
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &bytes) ||
        wcscmp(desktop, L"Default") != 0) return;
    const DWORD executable_length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!executable_length || executable_length >= ARRAYSIZE(executable)) return;
    const auto bench = app::CanonicalExplorerScopePath((std::filesystem::path(executable).parent_path().parent_path() / L"bench_data").wstring());
    const auto directory = app::CanonicalExplorerScopePath(Env(L"PULSE_TEST_DATA_DIR"));
    const auto parent = app::CanonicalExplorerScopePath(std::filesystem::path(directory).parent_path().wstring());
    const DWORD directory_attributes = directory.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(directory.c_str());
    if (bench.empty() || directory.empty() || parent.empty() || _wcsicmp(parent.c_str(), bench.c_str()) != 0 ||
        directory_attributes == INVALID_FILE_ATTRIBUTES || !(directory_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        std::filesystem::path(directory).filename().wstring().find(L"explorer-source-") != 0) return;
    // Control files never enter the watched source directory.
    auto scope = std::make_shared<app::ExplorerSourceScope>();
    HANDLE config = CreateFileW((directory + L"\\scope-token.bin").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (config == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{}; DWORD read = 0;
    const bool valid = GetFileSizeEx(config, &size) && size.QuadPart == static_cast<LONGLONG>(sizeof(scope->token)) &&
        ReadFile(config, &scope->token, sizeof(scope->token), &read, nullptr) && read == sizeof(scope->token);
    CloseHandle(config);
    if (!valid || scope->token.magic != 0x50585345 || scope->token.version != 1 ||
        scope->token.reserved > app::kExplorerScopeExternalEmptySelection || scope->token.busy > 3) return;
    const bool external = scope->token.reserved == app::kExplorerScopeExternalEmptySelection;
    auto source_path = std::filesystem::path(directory) / L"source";
    if (external) {
        HANDLE path_file = CreateFileW((directory + L"\\external-folder.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (path_file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER path_size{};
        std::wstring supplied;
        bool path_valid = GetFileSizeEx(path_file, &path_size) && path_size.QuadPart > 0 &&
            path_size.QuadPart < 32768 * static_cast<LONGLONG>(sizeof(wchar_t)) && path_size.QuadPart % sizeof(wchar_t) == 0;
        if (path_valid) {
            supplied.resize(static_cast<size_t>(path_size.QuadPart) / sizeof(wchar_t));
            path_valid = ReadFile(path_file, supplied.data(), static_cast<DWORD>(path_size.QuadPart), &read, nullptr) &&
                read == path_size.QuadPart && supplied.find(L'\0') == std::wstring::npos && std::filesystem::path(supplied).is_absolute();
        }
        CloseHandle(path_file);
        if (!path_valid) return;
        source_path = supplied;
    }
    const auto source = app::CanonicalExplorerScopePath(source_path.wstring());
    const auto source_parent = app::CanonicalExplorerScopePath(std::filesystem::path(source).parent_path().wstring());
    const DWORD source_attributes = GetFileAttributesW(source_path.c_str());
    if (source.empty() || source_parent.empty() ||
        _wcsicmp(source_parent.c_str(), external ? bench.c_str() : directory.c_str()) != 0 ||
        (!external && _wcsicmp(std::filesystem::path(source).filename().c_str(), L"source") != 0) ||
        source_attributes == INVALID_FILE_ATTRIBUTES || !(source_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (source_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    scope->folder = source;
    if (!app::ExplorerScopeWindow(*scope, reinterpret_cast<HWND>(scope->token.hwnd))) return;
    scope->evidence->close_authorized_event = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        app::ExplorerScopeCloseEventName(scope->token).c_str());
    if (!scope->evidence->close_authorized_event) return;
    probe.directory = directory;
    probe.scope = std::move(scope);
    probe.initial_source = app::ObserveExplorerSource(reinterpret_cast<HWND>(probe.scope->token.hwnd));
    probe.started = GetTickCount64();
    if (probe.scope->token.busy && IsWindowEnabled(state.hwnd)) {
        probe.disabled_by_probe = true; EnableWindow(state.hwnd, FALSE);
    }
}
void TickExplorerTakeoverProbe(AppState& state) {
    auto& probe = Current();
    if (!ExplorerTakeoverProbeRequested(state) || !probe.scope || probe.finished) return;
    auto now = GetTickCount64();
    auto evidence = probe.scope->evidence;
    app::ExplorerTakeoverRequest request;
    { std::lock_guard lock(evidence->mutex); request = evidence->submitted; }
    if (!probe.stopping) {
        const HWND source = reinterpret_cast<HWND>(probe.scope->token.hwnd);
        const bool resume = probe.scope->token.busy == 3;
        const bool busy = probe.scope->token.busy == 1 || probe.scope->token.busy == 2;
        const bool received = request.handoff && request.handoff->received.load();
        if (received && !probe.ready_written) {
            HANDLE ready = CreateFileW((probe.directory + L"\\takeover-ready.tsv").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (ready != INVALID_HANDLE_VALUE) {
                constexpr char text[] = "READY\tWatcher request received by UI handoff\n";
                DWORD written = 0;
                probe.ready_written = WriteFile(ready, text, sizeof(text) - 1, &written, nullptr) && written == sizeof(text) - 1;
                CloseHandle(ready);
            }
        }
        if (resume && received && !probe.resumed && probe.received_tick && now - probe.received_tick >= 1000 && probe.deferred) {
            if (probe.disabled_by_probe && IsWindow(state.hwnd)) {
                ExplorerTakeoverProbeDelivery(state, request.handoff, "resume-before");
                EnableWindow(state.hwnd, TRUE);
                ExplorerTakeoverProbeDelivery(state, request.handoff, "resume-after");
                probe.resumed = IsWindowEnabled(state.hwnd) != FALSE;
                if (probe.resumed) probe.disabled_by_probe = false;
            }
        }
        now = GetTickCount64();
        const auto handoff_state = request.handoff ? request.handoff->state.load() : app::HandoffState::Pending;
        if (handoff_state == app::HandoffState::Closed && !probe.closed_observed_tick)
            probe.closed_observed_tick = now;
        const bool presented = request.handoff && probe.presented_handoff == request.handoff &&
            probe.presented_generation != 0 && probe.prepared_tick != 0 && probe.presented_tick >= probe.prepared_tick;
        const bool terminal = busy ? received && handoff_state == app::HandoffState::Expired :
            presented && evidence->quit && !IsWindow(source) && handoff_state == app::HandoffState::Closed;
        // Discovery and delivery are separate test budgets. Never truncate the
        // handoff's original production deadline because Shell discovery was slow.
        const auto budget_handoff = request.handoff ? request.handoff : probe.delivery_handoff;
        const bool failed_terminal = handoff_state == app::HandoffState::Cancelled || handoff_state == app::HandoffState::Expired;
        const char* stop_reason = nullptr;
        if (probe.scope->token.busy == 2) {
            // Parent must have an opportunity to observe READY and kill only its
            // owned child. This watchdog does not extend any handoff authorization.
            const uint64_t crash_start = probe.received_tick ? probe.received_tick : probe.started;
            if (now - crash_start < 20000) return;
            stop_reason = "crash-fixture-watchdog";
        } else if (terminal) stop_reason = "expected-terminal";
        else if (failed_terminal) stop_reason = "failed-handoff-terminal";
        else if (handoff_state == app::HandoffState::Closed) {
            // This budget checks the real post-close frame; it never extends
            // the source-close authorization deadline.
            if (now - probe.closed_observed_tick < 3000) return;
            stop_reason = "post-close-presentation-timeout";
        }
        else if (budget_handoff) {
            if (now < budget_handoff->deadline_tick) return;
            stop_reason = "handoff-deadline";
        } else {
            if (now - probe.started < 20000) return;
            stop_reason = "pre-handoff-watchdog";
        }
        const auto* tab = ActiveTab(state);
        const bool target_folder = tab && path::EqualInsensitive(path::StripExtendedPathPrefix(tab->current_path),
            path::StripExtendedPathPrefix(probe.scope->folder));
        bool selected = false;
        if (tab) for (int index : tab->SelectedIndices())
            if (index >= 0 && static_cast<size_t>(index) < tab->EntryCount() && tab->EntryVisible(index) &&
                tab->EntryAt(index).name == L"selected.txt") selected = true;
        const bool external = probe.scope->token.reserved == app::kExplorerScopeExternalEmptySelection;
        const bool expected_selection = external ? tab && tab->SelectedIndices().empty() : selected;
        const auto expected_names = external ? std::vector<std::wstring>{} : std::vector<std::wstring>{L"selected.txt"};
        const bool source_unchanged = app::UnchangedAvailableSource(probe.initial_source, app::ObserveExplorerSource(source));
        probe.pass = terminal && probe.scope->token.busy != 2 && probe.receive_attempts == 1 && probe.receive_successes == 1 &&
            probe.delivery_handoff == request.handoff && (busy ? (source_unchanged && !evidence->quit &&
            tab && !target_folder && probe.dispatched == 0 && probe.deferred &&
                now >= request.handoff->deadline_tick && TabCount(state) == probe.baseline_tabs &&
                state.window_tabs.Active() == probe.baseline_active) :
            (probe.dispatched == 1 && (!resume || (probe.resumed && probe.deferred &&
                TabCount(state) == probe.baseline_tabs + 1 && state.core.windows.Count() == probe.baseline_windows)) &&
            presented && request.selection_read && request.names == expected_names &&
            tab && tab->applied_generation == probe.presented_generation && target_folder && expected_selection && tab->snapshot && !tab->loading && tab->applied_generation != 0 &&
            tab->snapshot_path == tab->current_path && IsWindowVisible(state.hwnd) && !IsIconic(state.hwnd)));
        probe.report = std::string("CASE\t") + (busy ? "busy-expiry" : resume ? "busy-resume" : "success") +
            "\nSTATE\tsource_unchanged_available=" + std::to_string(source_unchanged) +
            "\tproduction_finish_close=" + std::to_string(evidence->quit.load()) + "\tsource_alive=" + std::to_string(IsWindow(source) != FALSE) +
            "\tui_received=" + std::to_string(received) + "\tpresented=" + std::to_string(presented) +
            "\thandoff=" + std::to_string(request.handoff ? static_cast<int>(request.handoff->state.load()) : -1) + "\n";
        probe.report += std::string("TIMING\tstop_reason=") + stop_reason +
            "\tstarted=" + std::to_string(probe.started) + "\treceived_tick=" + std::to_string(probe.received_tick) +
            "\tstop_tick=" + std::to_string(now) + "\thandoff_deadline=" +
            std::to_string(budget_handoff ? budget_handoff->deadline_tick : 0) + "\n";
        probe.report += "DELIVERY\treceive_attempts=" + std::to_string(probe.receive_attempts) +
            "\treceived=" + std::to_string(probe.receive_successes) + "\tdeferred=" + std::to_string(probe.deferred) +
            "\tdispatched=" + std::to_string(probe.dispatched) + "\tresumed=" + std::to_string(probe.resumed) +
            "\tbaseline_tabs=" + std::to_string(probe.baseline_tabs) + "\tfinal_tabs=" + std::to_string(TabCount(state)) + "\n";
        probe.report += "REQUEST\tchannel=Watcher-to-owned-queue-to-UI\tapplied_generation=" +
            std::to_string(tab ? tab->applied_generation : 0) + "\tpresented_generation=" + std::to_string(probe.presented_generation) +
            "\tpresented_tick=" + std::to_string(probe.presented_tick) +
            "\tprepared_tick=" + std::to_string(probe.prepared_tick) +
            "\tnow=" + std::to_string(now) + "\tdeadline=" + std::to_string(request.handoff ? request.handoff->deadline_tick : 0) + "\n";
        probe.stopping = now;
        if (state.explorer_takeover) { state.explorer_takeover->Stop(); state.explorer_takeover.reset(); }
    }
    if (!evidence->stopped && now - probe.stopping < 10000) return;
    probe.report += std::string("STOPPED\t") + (evidence->stopped ? "1\n" : "0\n");
    probe.report += probe.pass && evidence->stopped ? "PASS\tproduction Watcher background-prepare/source-close/actual-present\n" :
        "FAIL\tproduction Watcher background-prepare/source-close/actual-present\n";
    Write(state, probe);
}
}
