#include "entry_command_probe.h"
#include "entry_command_probe_contract.h"
#include "app_state.h"
#include "app_runtime.h"
#include "startup_location.h"
#include "app_messages.h"
#include "../common/path_utils.h"
#include "../fs/fs_enum.h"
#include <objbase.h>
#include <filesystem>
#include <algorithm>
#include <vector>

namespace pulse {
namespace {
using app::entry_probe::Contract;
using app::entry_probe::Kind;
struct Probe {
    bool attempted = false, enabled = false, finished = false;
    Contract contract;
    std::wstring directory, folder, win_e_target;
    bool win_e_intent = false;
    HWND window = nullptr;
    uint64_t since = 0;
    unsigned observed_frames = 0;
    std::string last = "no-present-hook";
};
Probe& Current() { static Probe probe; return probe; }
std::wstring Environment(const wchar_t* name) {
    wchar_t value[32768]{};
    const DWORD count = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    return count && count < ARRAYSIZE(value) ? value : L"";
}
std::wstring Canonical(const std::filesystem::path& path, bool require_directory) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    std::wstring result(32768, 0);
    const bool kind_ok = GetFileInformationByHandle(file, &info) &&
        (((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == require_directory);
    const DWORD count = kind_ok ? GetFinalPathNameByHandleW(file, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED) : 0;
    CloseHandle(file);
    if (!count || count >= result.size()) return {};
    result.resize(count); return result;
}
bool Same(const std::wstring& first, const std::wstring& second) {
    return !first.empty() && !second.empty() && _wcsicmp(first.c_str(), second.c_str()) == 0;
}
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (!count) return "<invalid-utf16>";
    std::string result(static_cast<size_t>(count), 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}
void Finish(AppState& state, Probe& probe, bool presented) {
    if (probe.finished) return;
    probe.finished = true;
    const auto* tab = ActiveTab(state);
    std::string report = std::string(presented ? "PASS\tpresented\t" : "FAIL\tpresentation\t") +
        Utf8(probe.contract.nonce) + "\t" + std::to_string(static_cast<uint32_t>(probe.contract.kind)) + "\n";
    report += "STATE\tframes=" + std::to_string(probe.observed_frames) + "\treason=" + probe.last +
        "\tpath=" + (tab ? Utf8(tab->current_path) : "<no-tab>") +
        "\tpending=" + std::to_string(tab ? tab->pending_generation : 0) +
        "\tapplied=" + std::to_string(tab ? tab->applied_generation : 0) +
        "\tentries=" + std::to_string(tab ? tab->EntryCount() : 0) + "\n";
    report += "LIMIT\tGenerated command chain and cold actual presentation only; no desktop association selection or hot-instance claim.\n";
    HANDLE file = CreateFileW((std::filesystem::path(probe.directory) / app::entry_probe::kPresentationReport).c_str(),
        GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        CloseHandle(file);
    }
    PostMessageW(state.hwnd, WM_EXIT_PULSE, 0, 0);
}
}

void InitializeEntryCommandProbe(AppState& state) {
    if (!state.isolatedTest || state.shot.active || state.menushot) return;
    const auto nonce = Environment(app::entry_probe::kEnvironment);
    if (nonce.empty()) return;
    auto& probe = Current();
    if (probe.attempted) return;
    probe.attempted = true;
    GUID guid{};
    if (nonce.size() != 38 || FAILED(CLSIDFromString(nonce.c_str(), &guid))) return;
    wchar_t executable[32768]{}, desktop[256]{}; DWORD bytes = 0;
    const DWORD count = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!count || count >= ARRAYSIZE(executable) ||
        !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &bytes) ||
        wcscmp(desktop, L"Default") != 0) return;
    const auto bench = Canonical(std::filesystem::path(executable).parent_path().parent_path() / L"bench_data", true);
    const auto directory = Canonical(Environment(L"PULSE_TEST_DATA_DIR"), true);
    // Exact immediate child + nonce filename, not a prefix-only containment test.
    if (bench.empty() || directory.empty() ||
        !Same(Canonical(std::filesystem::path(directory).parent_path(), true), bench) ||
        std::filesystem::path(directory).filename() != L"entry-command-" + nonce) return;
    HANDLE config = CreateFileW((std::filesystem::path(directory) / app::entry_probe::kContractFile).c_str(),
        GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (config == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{}; DWORD read = 0;
    const bool read_ok = GetFileSizeEx(config, &size) && size.QuadPart == static_cast<LONGLONG>(sizeof(Contract)) &&
        ReadFile(config, &probe.contract, sizeof(Contract), &read, nullptr) && read == sizeof(Contract);
    CloseHandle(config);
    const auto& contract = probe.contract;
    if (!read_ok || contract.magic != app::entry_probe::kMagic || contract.size != sizeof(Contract) || contract.reserved ||
        static_cast<uint32_t>(contract.kind) > static_cast<uint32_t>(Kind::WinE) || contract.nonce[39] ||
        contract.pulse_path[4095] || contract.target_path[4095] || nonce != contract.nonce ||
        !Same(Canonical(contract.pulse_path, false), Canonical(executable, false))) return;
    const auto folder = Canonical(contract.target_path, true);
    if (!Same(folder, Canonical(std::filesystem::path(directory) / L"folder space 中文 & parentheses()", true))) return;
    probe.enabled = true; probe.directory = directory; probe.folder = folder;
    probe.win_e_intent = state.open_path == L"pulse:win-e";
    probe.win_e_target = state.appPrefs.new_window_shows == 1 ? std::wstring() :
        state.appPrefs.new_window_shows == 0 ? app::DefaultLocation(state.appPrefs) : state.session_path;
    probe.since = GetTickCount64(); probe.window = state.hwnd;
    // Only this fully authenticated isolated fixture gets runtime changes.
    // Do not alter startup_open/home_folder or synthesize a navigation target.
    state.appPrefs.persist = false;
    state.isolatedTestPersist = false;
    state.appPrefs.auto_check_updates = false;
    state.appPrefs.integration_enabled = false;
    state.appPrefs.open_folders_in_pulse = false;
    state.appPrefs.take_over_explorer_windows = false;
    SetEnvironmentVariableW(L"PULSE_TEST_EXPLORER_TAKEOVER", nullptr);
    SetEnvironmentVariableW(L"PULSE_TEST_SHELL_WINDOWS", nullptr);
}

void EntryCommandProbePresented(AppState& state) {
    auto& probe = Current();
    if (!probe.enabled || probe.finished || probe.window != state.hwnd) return;
    ++probe.observed_frames;
    if (!state.startupComplete || !IsWindowVisible(state.hwnd) || IsIconic(state.hwnd)) {
        probe.last = "window-not-presentable"; return;
    }
    const auto* tab = ActiveTab(state);
    const auto* layout = state.window_tabs.Active();
    const auto panes = layout ? layout->VisiblePanes() : std::vector<app::Pane*>{};
    if (!tab || !std::any_of(panes.begin(), panes.end(), [&](const app::Pane* pane) { return pane && pane->ActiveTab() == tab; })) {
        probe.last = "no-visible-active-tab"; return;
    }
    if (tab->loading || tab->pending_generation || !tab->snapshot || !tab->applied_generation || tab->net_readonly ||
        tab->snapshot_path != tab->current_path) { probe.last = "enumeration-not-complete"; return; }
    bool target = false;
    switch (probe.contract.kind) {
    case Kind::Folder:
        target = path::EqualInsensitive(path::StripExtendedPathPrefix(tab->current_path), path::StripExtendedPathPrefix(probe.folder));
        break;
    case Kind::ThisPc: target = tab->current_path.empty(); break;
    case Kind::RecycleBin: target = tab->current_path == L"pulse:recycle"; break;
    case Kind::WinE:
        // The relay verifies --win-e; here verify its distinct parsed intent
        // and the actual cold target selected by the current launch preferences.
        target = probe.win_e_intent && (probe.win_e_target.empty() ? tab->current_path.empty() :
            path::EqualInsensitive(path::StripExtendedPathPrefix(tab->current_path),
                                   path::StripExtendedPathPrefix(probe.win_e_target)));
        break;
    }
    if (!target) { probe.last = "wrong-presented-target"; return; }
    probe.last = "actual-present-after-loaded-target";
    Finish(state, probe, true);
}

void TickEntryCommandProbe(AppState& state) {
    auto& probe = Current();
    if (probe.enabled && !probe.finished && probe.window == state.hwnd && GetTickCount64() - probe.since >= 25000)
        Finish(state, probe, false);
}
}
