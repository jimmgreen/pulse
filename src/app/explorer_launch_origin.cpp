// explorer_launch_origin.cpp — see explorer_launch_origin.h.
#include "explorer_launch_origin.h"

#include <winternl.h>

#include <map>
#include <mutex>
#include <vector>

namespace pulse::app::explorer_launch_origin {
namespace {

constexpr ULONGLONG kRecordLifetimeMs = 60000;
constexpr size_t kMaxRecords = 32;
constexpr ULONG kProcessCommandLineInformation = 60;

struct Record {
    std::wstring selected;
    std::wstring parent;
    ULONGLONG tick = 0;
};

struct State {
    std::mutex mutex;
    std::vector<Record> records;
    std::map<DWORD, DWORD> inspected;   // launcher pid -> parent pid already examined
};

State& Shared() {
    static State state;
    return state;
}

bool SameText(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring Normalize(std::wstring_view path) {
    std::wstring value(path);
    for (auto& ch : value) if (ch == L'/') ch = L'\\';
    if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return value;
}

ULONGLONG Created(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

std::wstring CommandLine(HANDLE process) {
    using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query) return {};
    ULONG size = 0;
    query(process, kProcessCommandLineInformation, nullptr, 0, &size);
    if (size < sizeof(UNICODE_STRING) || size > 256 * 1024) return {};
    std::vector<BYTE> buffer(size);
    if (query(process, kProcessCommandLineInformation, buffer.data(), size, &size) < 0) return {};
    const auto* text = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
    if (!text->Buffer || text->Length > size) return {};
    return std::wstring(text->Buffer, text->Length / sizeof(wchar_t));
}

std::wstring Image(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
    path.resize(size);
    return path;
}

void AddLocked(State& state, Record record) {
    const ULONGLONG now = GetTickCount64();
    std::erase_if(state.records, [now](const Record& item) { return item.tick + kRecordLifetimeMs < now; });
    if (state.records.size() >= kMaxRecords) state.records.erase(state.records.begin());
    state.records.push_back(std::move(record));
}

} // namespace

std::wstring SelectTarget(std::wstring_view line) {
    constexpr std::wstring_view key = L"/select,";
    size_t at = std::wstring_view::npos;
    for (size_t i = 0; i + key.size() <= line.size(); ++i) {
        if (SameText(line.substr(i, key.size()), key)) { at = i; break; }
    }
    if (at == std::wstring_view::npos) return {};
    auto rest = line.substr(at + key.size());
    while (!rest.empty() && (rest.front() == L' ' || rest.front() == L'\t')) rest.remove_prefix(1);
    if (!rest.empty() && rest.front() == L'"') {
        rest.remove_prefix(1);
        const size_t quote = rest.find(L'"');
        if (quote == std::wstring_view::npos) return {};
        rest = rest.substr(0, quote);
    } else {
        while (!rest.empty() && (rest.back() == L' ' || rest.back() == L'\t')) rest.remove_suffix(1);
    }
    return Normalize(rest);
}

void Observe(const PROCESSENTRY32W& entry) noexcept {
    try {
        if (!SameText(entry.szExeFile, L"explorer.exe") || entry.th32ProcessID == 0) return;
        auto& state = Shared();
        {
            std::lock_guard lock(state.mutex);
            if (state.inspected.size() > 512) state.inspected.clear();
            const auto [it, inserted] = state.inspected.try_emplace(entry.th32ProcessID, entry.th32ParentProcessID);
            if (!inserted && it->second == entry.th32ParentProcessID) return;
            it->second = entry.th32ParentProcessID;
        }
        Inspect(entry.th32ProcessID, entry.th32ParentProcessID);
    } catch (...) {
    }
}

void Inspect(DWORD pid, DWORD parent_pid) noexcept {
    try {
        HANDLE launcher = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!launcher) return;
        const std::wstring selected = SelectTarget(CommandLine(launcher));
        const ULONGLONG launched = Created(launcher);
        CloseHandle(launcher);
        if (selected.empty() || !launched) return;
        HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid);
        if (!parent) return;
        const ULONGLONG parent_created = Created(parent);
        std::wstring image = Image(parent);
        CloseHandle(parent);
        // A reused parent PID started after the launcher is not its creator.
        if (!parent_created || parent_created > launched || image.empty()) return;
        auto& state = Shared();
        std::lock_guard lock(state.mutex);
        AddLocked(state, {selected, std::move(image), GetTickCount64()});
    } catch (...) {
    }
}

std::wstring FindLauncherParent(std::wstring_view selected_path, ULONGLONG window_ms) noexcept {
    try {
        const std::wstring wanted = Normalize(selected_path);
        const ULONGLONG now = GetTickCount64();
        auto& state = Shared();
        std::lock_guard lock(state.mutex);
        std::wstring found;
        for (const auto& record : state.records) {
            if (record.tick + window_ms < now || !SameText(record.selected, wanted)) continue;
            if (!found.empty() && !SameText(found, record.parent)) return {};   // ambiguous
            found = record.parent;
        }
        return found;
    } catch (...) {
        return {};
    }
}

void RecordForTest(std::wstring selected_path, std::wstring parent_image, ULONGLONG tick) {
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    AddLocked(state, {Normalize(selected_path), std::move(parent_image), tick});
}

} // namespace pulse::app::explorer_launch_origin
