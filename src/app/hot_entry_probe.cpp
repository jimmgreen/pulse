#include "hot_entry_probe.h"
#include "hot_entry_data_dir.h"
#include "hot_entry_probe_contract.h"
#include "entry_command_probe_contract.h"
#include "app_runtime.h"
#include "app_messages.h"
#include "window_registry.h"
#include "../common/path_utils.h"
#include <filesystem>
#include <algorithm>
#include <objbase.h>

namespace pulse {
namespace {
namespace hp = app::hot_entry_probe;
struct Probe {
    HANDLE mapping = nullptr;
    hp::Shared* shared = nullptr;
    std::wstring mutex, target, other;
    int slot = -1;
    uint64_t started = 0;
    ~Probe() { if (shared) UnmapViewOfFile(shared); if (mapping) CloseHandle(mapping); }
};
Probe& Current() { static Probe probe; return probe; }
std::wstring Env(const wchar_t* name) {
    wchar_t value[32768]{};
    const auto count = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    return count && count < ARRAYSIZE(value) ? value : L"";
}
std::wstring Canonical(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::wstring result(32768, 0);
    const auto count = GetFinalPathNameByHandleW(file, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
    CloseHandle(file);
    if (!count || count >= result.size()) return {};
    result.resize(count); return result;
}
bool Same(const std::wstring& a, const std::wstring& b) { return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool Target(const std::wstring& path) {
    return path::EqualInsensitive(path::StripExtendedPathPrefix(path), path::StripExtendedPathPrefix(Current().target));
}
void Sample(AppState& state) {
    auto& probe = Current();
    LONG windows = 0, tabs = 0;
    app::ForEachWindowSharingCore(state, [&](AppState& window) {
        if (IsWindow(window.hwnd)) { ++windows; tabs += static_cast<LONG>(window.window_tabs.items.size()); }
    });
    InterlockedExchange(&probe.shared->windows, windows);
    InterlockedExchange(&probe.shared->tabs, tabs);
    if (windows > hp::Read(probe.shared->max_windows)) InterlockedExchange(&probe.shared->max_windows, windows);
    if (tabs > hp::Read(probe.shared->max_tabs)) InterlockedExchange(&probe.shared->max_tabs, tabs);
    if (windows > 1 || tabs > 3) InterlockedExchange(&probe.shared->failed, 1);
}
}
bool PrepareHotEntryProbe(AppState& state) {
    if (Env(L"PULSE_TEST_LIVE_SHELL_ENTRY") == L"1") {
        wchar_t executable[32768]{};
        if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return false;
        const auto fixture = Canonical(Env(L"PULSE_TEST_DATA_DIR"));
        const auto bench = Canonical(std::filesystem::path(executable).parent_path().parent_path() / L"bench_data");
        if (fixture.empty() || bench.empty() ||
            !Same(Canonical(std::filesystem::path(fixture).parent_path()), bench) ||
            !std::filesystem::path(fixture).filename().wstring().starts_with(L"shell-entry-live-")) return false;
        // Keep the real singleton/forwarding route, but resolve every data read
        // and write to the validated test profile even in non-selftest builds.
        detail::HotEntryDataDirectory::directory_ = fixture;
        return true;
    }
    const auto nonce = Env(L"PULSE_TEST_HOT_ENTRY");
    if (nonce.empty()) return true;
    if (!Env(L"PULSE_TEST_STARTUP_FAIL").empty()) return false;
    GUID guid{};
    if (nonce.size() != 38 || FAILED(CLSIDFromString(nonce.c_str(), &guid))) return false;
    const auto role = Env(L"PULSE_TEST_HOT_SLOT");
    int slot = -1;
    if (role != L"primary") {
        if (role.size() != 1 || role[0] < L'0' || role[0] >= L'0' + static_cast<int>(hp::kSenders)) return false;
        slot = role[0] - L'0';
    }
    // No singleton-bypass switch is accepted. Only the last sender uses the
    // real --win-e CLI; all other operands are exact private fixture folders.
    if (__argc != 2 || !__wargv[1] ||
        (slot == 5 ? wcscmp(__wargv[1], L"--win-e") != 0 : __wargv[1][0] == L'-')) return false;
    wchar_t executable[32768]{};
    const auto count = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!count || count >= ARRAYSIZE(executable)) return false;
    const auto bench = Canonical(std::filesystem::path(executable).parent_path().parent_path() / L"bench_data");
    const auto fixture = Canonical(Env(L"PULSE_TEST_DATA_DIR"));
    if (bench.empty() || !Same(fixture, bench + L"\\hot-entry-" + nonce)) return false;
    app::entry_probe::Contract contract;
    HANDLE file = CreateFileW((std::filesystem::path(fixture) / app::entry_probe::kContractFile).c_str(),
        GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; DWORD read = 0;
    const bool read_ok = GetFileSizeEx(file, &size) && size.QuadPart == static_cast<LONGLONG>(sizeof(contract)) &&
        ReadFile(file, &contract, sizeof(contract), &read, nullptr) && read == sizeof(contract);
    CloseHandle(file);
    if (!read_ok || contract.magic != app::entry_probe::kMagic || contract.size != sizeof(contract) ||
        contract.kind != app::entry_probe::Kind::Folder || contract.reserved || contract.nonce[39] ||
        contract.pulse_path[4095] || contract.target_path[4095] || nonce != contract.nonce ||
        !Same(Canonical(contract.pulse_path), Canonical(executable)) ||
        !Same(Canonical(contract.target_path), fixture + L"\\folder") ||
        !Same(Canonical(std::filesystem::path(fixture) / L"folder"), fixture + L"\\folder")) return false;
    const auto other = Canonical(std::filesystem::path(fixture) / L"folder-other");
    if (!Same(other, fixture + L"\\folder-other") || (slot != 5 && !Same(Canonical(__wargv[1]),
        slot == 4 ? other : Canonical(contract.target_path)))) return false;
    auto& probe = Current();
    probe.mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, hp::MappingName(nonce.c_str()).c_str());
    if (!probe.mapping) return false;
    probe.shared = static_cast<hp::Shared*>(MapViewOfFile(probe.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(hp::Shared)));
    if (!probe.shared || probe.shared->magic != hp::kMagic || probe.shared->size != sizeof(hp::Shared) ||
        probe.shared->nonce[39] || nonce != probe.shared->nonce ||
        (slot < 0 ? probe.shared->primary_pid : probe.shared->senders[slot].pid) != GetCurrentProcessId()) return false;
    probe.slot = slot; probe.mutex = hp::MutexName(nonce.c_str()); probe.target = Canonical(contract.target_path); probe.other = other;
    probe.started = GetTickCount64();
    detail::HotEntryDataDirectory::directory_ = fixture;
    state.isolatedTest = true;
    state.isolatedTestPersist = false;
    SetEnvironmentVariableW(L"PULSE_TEST_PERSIST", nullptr);
    SetEnvironmentVariableW(L"PULSE_TEST_EXPLORER_TAKEOVER", nullptr);
    SetEnvironmentVariableW(L"PULSE_TEST_SHELL_WINDOWS", nullptr);
    return true;
}
std::wstring_view HotEntryProbeMutexName() { return Current().mutex; }
bool HotEntryProbeAcquired(app::SingleInstanceCoordinator::AcquireResult result) {
    auto& probe = Current();
    if (probe.mutex.empty()) return true;
    const bool expected = result == (probe.slot < 0 ? app::SingleInstanceCoordinator::AcquireResult::Primary :
        app::SingleInstanceCoordinator::AcquireResult::Existing);
    if (!expected) InterlockedExchange(&probe.shared->failed, 1);
    return expected;
}
void HotEntryProbeForwarded(bool success, const std::array<unsigned char, 16>& attempted_id) {
    auto& probe = Current();
    if (probe.mutex.empty() || probe.slot < 0) return;
    auto& sender = probe.shared->senders[probe.slot];
    sender.attempted_id = attempted_id;
    sender.forwarded = success ? 1 : 0;
    InterlockedExchange(&sender.published, 1);
    if (!success) InterlockedExchange(&probe.shared->failed, 1);
}
void InitializeHotEntryProbe(AppState& state) {
    if (Current().mutex.empty()) return;
    state.appPrefs.persist = false;
    state.appPrefs.auto_check_updates = false;
    state.appPrefs.integration_enabled = false;
    state.appPrefs.open_folders_in_pulse = false;
    state.appPrefs.take_over_explorer_windows = false;
    // These values must come from the driver's private app.json, not a
    // synthetic navigation or a change to the user's launch policy.
    if (state.appPrefs.external_open != 1 || state.appPrefs.open_win_e != 1 ||
        state.appPrefs.open_pulse_icon != 0 || state.appPrefs.open_this_pc != 0 || state.appPrefs.new_window_shows != 1)
        InterlockedExchange(&Current().shared->failed, 1);
}
void HotEntryProbePresented(AppState& state) {
    auto& probe = Current();
    if (probe.mutex.empty() || probe.slot >= 0) return;
    Sample(state);
    auto* tab = ActiveTab(state);
    const auto* layout = state.window_tabs.Active();
    const auto panes = layout ? layout->VisiblePanes() : std::vector<app::Pane*>{};
    if (!state.startupComplete || !IsWindowVisible(state.hwnd) || IsIconic(state.hwnd) || !tab ||
        !std::any_of(panes.begin(), panes.end(), [&](const auto* pane) { return pane && pane->ActiveTab() == tab; }) ||
        tab->loading || tab->pending_generation || !tab->snapshot || !tab->applied_generation || tab->net_readonly ||
        tab->snapshot_path != tab->current_path) return;
    if (!hp::Read(probe.shared->ready) && Target(tab->current_path) &&
        hp::Read(probe.shared->windows) == 1 && hp::Read(probe.shared->tabs) == 1)
        InterlockedExchange(&probe.shared->ready, 1);
    for (unsigned index = 0; index < hp::kSenders; ++index) {
        auto& sender = probe.shared->senders[index];
        if (!hp::Read(sender.published) || hp::Read(sender.presented)) continue;
        if (!sender.forwarded || std::all_of(sender.attempted_id.begin(), sender.attempted_id.end(), [](auto c) { return !c; })) {
            InterlockedExchange(&probe.shared->failed, 1); continue;
        }
        const auto status = state.single_instance.ObserveOpenDelivery(sender.attempted_id);
        if (status == app::SingleInstanceCoordinator::OpenDeliveryState::Cancelled) {
            InterlockedExchange(&probe.shared->failed, 1); continue;
        }
        if (status != app::SingleInstanceCoordinator::OpenDeliveryState::Dispatched) continue;
        const auto& expected = index < 4 ? probe.target : probe.other;
        const bool correct_path = index == 5 ? tab->current_path.empty() : path::EqualInsensitive(
            path::StripExtendedPathPrefix(tab->current_path), path::StripExtendedPathPrefix(expected));
        if (!correct_path) continue;
        const LONG windows = hp::Read(probe.shared->windows), tabs = hp::Read(probe.shared->tabs);
        if (windows != 1 || tabs != (index < 4 ? 1 : index == 4 ? 2 : 3)) {
            InterlockedExchange(&probe.shared->failed, 1); continue;
        }
        sender.presented_windows = windows; sender.presented_tabs = tabs;
        InterlockedExchange(&sender.presented, 1);
    }
}
void TickHotEntryProbe(AppState& state) {
    auto& probe = Current();
    if (probe.mutex.empty() || probe.slot >= 0) return;
    Sample(state);
    if (GetTickCount64() - probe.started >= 90000) InterlockedExchange(&probe.shared->failed, 1);
    if (hp::Read(probe.shared->stop) || hp::Read(probe.shared->failed)) PostMessageW(state.hwnd, WM_EXIT_PULSE, 0, 0);
    else InvalidateRect(state.hwnd, nullptr, FALSE); // Observe an actual Present; never synthesize completion/navigation.
}
}
