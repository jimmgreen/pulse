// Read-only field probe: records every native open/save/browse dialog that
// appears on this desktop and why Pulse did not replace it. It never sends
// input, moves windows or touches the observed processes beyond queries.
//   pulse_dialog_coverage_probe [--minutes N] [--out file.jsonl]
#include "../common/dialog_adaptive_attach.h"
#include <windows.h>
#include <psapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
struct Handle {
    HANDLE h = nullptr;
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::string Json(const std::wstring& text) {
    std::string out;
    for (const char c : Utf8(text)) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out;
}
std::wstring Basename(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
std::wstring ClassOf(HWND hwnd) {
    wchar_t name[128]{};
    return GetClassNameW(hwnd, name, ARRAYSIZE(name)) ? name : L"";
}
std::string UtcNow() {
    SYSTEMTIME t{};
    GetSystemTime(&t);
    char text[40]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond, t.wMilliseconds);
    return text;
}

struct ProcessFacts {
    std::wstring image, basename;
    bool opened = false, token = false, elevated = false, app_container = false, modules_read = false;
    DWORD integrity = 0;
    unsigned bits = 0;
    std::vector<std::wstring> pulse_modules;
};
ProcessFacts Inspect(DWORD pid) {
    ProcessFacts facts;
    Handle limited{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!limited.h) return facts;
    facts.opened = true;
    wchar_t image[32768]{};
    DWORD size = ARRAYSIZE(image);
    if (QueryFullProcessImageNameW(limited.h, 0, image, &size)) { facts.image = image; facts.basename = Basename(image); }
    BOOL wow64 = FALSE;
    if (IsWow64Process(limited.h, &wow64)) facts.bits = wow64 ? 32 : (sizeof(void*) == 8 ? 64 : 32);
    HANDLE raw = nullptr;
    if (OpenProcessToken(limited.h, TOKEN_QUERY, &raw)) {
        Handle token{raw};
        facts.token = true;
        TOKEN_ELEVATION elevation{};
        DWORD returned = 0;
        if (GetTokenInformation(token.h, TokenElevation, &elevation, sizeof(elevation), &returned)) facts.elevated = elevation.TokenIsElevated != 0;
        DWORD container = 0;
        if (GetTokenInformation(token.h, TokenIsAppContainer, &container, sizeof(container), &returned)) facts.app_container = container != 0;
        BYTE buffer[128]{};
        if (GetTokenInformation(token.h, TokenIntegrityLevel, buffer, sizeof(buffer), &returned)) {
            const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
            facts.integrity = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
        }
    }
    Handle full{OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid)};
    if (full.h) {
        std::vector<HMODULE> modules(1024);
        DWORD needed = 0;
        if (EnumProcessModulesEx(full.h, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) {
            facts.modules_read = true;
            modules.resize((std::min)(modules.size(), static_cast<size_t>(needed / sizeof(HMODULE))));
            for (HMODULE module : modules) {
                wchar_t name[MAX_PATH]{};
                if (!GetModuleBaseNameW(full.h, module, name, MAX_PATH)) continue;
                if (_wcsnicmp(name, L"Pulse", 5) == 0) facts.pulse_modules.emplace_back(name);
            }
        }
    }
    return facts;
}

const char* Kind(HWND hwnd) {
    // IFileDialog hosts DirectUI; the old Explorer-style GetOpenFileName also
    // embeds SHELLDLL_DefView, so only DirectUI marks the modern dialog.
    struct Probe { bool modern = false, shell_view = false, tree = false; unsigned visited = 0; } probe;
    EnumChildWindows(hwnd, [](HWND child, LPARAM raw) -> BOOL {
        auto& p = *reinterpret_cast<Probe*>(raw);
        if (++p.visited > 200) return FALSE;
        const auto name = ClassOf(child);
        if (name == L"DUIViewWndClassName") p.modern = true;
        if (name == L"SHELLDLL_DefView") p.shell_view = true;
        if (name == L"SysTreeView32") p.tree = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&probe));
    return probe.modern ? "modern" : probe.shell_view ? "classic_explorer" : probe.tree ? "browse_folder" : "classic";
}

// The injected runtime logs native_reason (hashed) when it deliberately keeps
// the system dialog; no such line near the show means the call never reached it.
std::string RecentNativeReason(DWORD pid, const fs::path& runtime_logs) {
    const auto path = runtime_logs / (L"dialog_auto-" + std::to_wstring(pid) + L".jsonl");
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto end = static_cast<std::streamoff>(in.tellg());
    in.seekg((std::max)(std::streamoff{0}, end - 65536));
    std::string line, last;
    while (std::getline(in, line)) if (line.find("\"event\":\"native_reason\"") != std::string::npos) last = line;
    if (last.empty()) return {};
    // Accept only a reason logged within the last 30 seconds.
    const auto utc = last.find("\"utc\":\"");
    if (utc == std::string::npos) return {};
    SYSTEMTIME logged{};
    if (sscanf_s(last.c_str() + utc + 7, "%4hu-%2hu-%2huT%2hu:%2hu:%2hu", &logged.wYear, &logged.wMonth, &logged.wDay,
                    &logged.wHour, &logged.wMinute, &logged.wSecond) != 6) return {};
    FILETIME a{}, b{};
    SYSTEMTIME now{};
    GetSystemTime(&now);
    if (!SystemTimeToFileTime(&logged, &a) || !SystemTimeToFileTime(&now, &b)) return {};
    const auto diff = (static_cast<long long>(ULARGE_INTEGER{{b.dwLowDateTime, b.dwHighDateTime}}.QuadPart) -
                       static_cast<long long>(ULARGE_INTEGER{{a.dwLowDateTime, a.dwHighDateTime}}.QuadPart)) / 10000000;
    if (diff < -5 || diff > 30) return {};
    const auto hash = last.find("\"reason_hash\":");
    return hash == std::string::npos ? "logged" : last.substr(hash + 14, last.find_first_of(",}", hash + 14) - hash - 14);
}

struct Probe {
    std::ofstream out;
    fs::path runtime_logs;
    std::set<HWND> seen;
    std::map<std::string, unsigned> totals;
    std::wstring pulse_image_name = L"pulse.exe";

    void Record(const std::string& line, const std::string& key) {
        out << line << '\n';
        out.flush();
        ++totals[key];
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }
    void Consider(HWND hwnd) {
        if (!hwnd || seen.contains(hwnd) || !IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) return;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid || pid == GetCurrentProcessId()) return;
        const auto cls = ClassOf(hwnd);
        if (pulse::dialog_adaptive::IsNativeFileDialogWindow(hwnd)) {
            seen.insert(hwnd);
            const auto facts = Inspect(pid);
            std::string outcome;
            std::string reason;
            if (!facts.opened) outcome = "process_unqueryable";
            else if (facts.app_container) outcome = "app_container";
            else if (facts.elevated || (!facts.token && facts.integrity == 0)) outcome = facts.token ? "elevated" : "elevated_or_protected";
            else if (!facts.modules_read) outcome = "modules_unreadable";
            else if (facts.pulse_modules.empty()) outcome = "not_injected";
            else {
                reason = RecentNativeReason(pid, runtime_logs);
                outcome = reason.empty() ? "injected_unseen" : "injected_fallback";
            }
            std::string modules;
            for (const auto& m : facts.pulse_modules) modules += (modules.empty() ? "\"" : ",\"") + Json(m) + "\"";
            const std::string app = Json(facts.basename.empty() ? L"?" : facts.basename);
            Record("{\"utc\":\"" + UtcNow() + "\",\"type\":\"native\",\"app\":\"" + app + "\",\"pid\":" + std::to_string(pid) +
                   ",\"bits\":" + std::to_string(facts.bits) + ",\"kind\":\"" + Kind(hwnd) + "\",\"outcome\":\"" + outcome +
                   "\",\"elevated\":" + (facts.elevated ? "true" : "false") + ",\"integrity\":" + std::to_string(facts.integrity) +
                   ",\"pulse_modules\":[" + modules + "]" + (reason.empty() ? "" : ",\"native_reason\":\"" + reason + "\"") + "}",
                   "native/" + Utf8(facts.basename) + "/" + outcome);
            return;
        }
        // Pulse's picker: shown by pulse.exe (out of process) or in the caller
        // (Pulse* window class), owned by the requesting application's window.
        const HWND owner = GetWindow(hwnd, GW_OWNER);
        DWORD owner_pid = 0;
        if (owner) GetWindowThreadProcessId(owner, &owner_pid);
        const auto facts = Inspect(pid);
        const bool pulse_process = _wcsicmp(facts.basename.c_str(), pulse_image_name.c_str()) == 0;
        if ((pulse_process && owner_pid && owner_pid != pid) || (!pulse_process && _wcsnicmp(cls.c_str(), L"Pulse", 5) == 0)) {
            seen.insert(hwnd);
            const auto app = pulse_process ? Inspect(owner_pid).basename : facts.basename;
            Record("{\"utc\":\"" + UtcNow() + "\",\"type\":\"pulse\",\"app\":\"" + Json(app) + "\",\"pid\":" +
                   std::to_string(pulse_process ? owner_pid : pid) + ",\"host\":\"" + (pulse_process ? "pulse.exe" : "in_process") +
                   "\",\"class\":\"" + Json(cls) + "\"}", "pulse/" + Utf8(app));
        }
    }
    void Sweep() {
        for (auto it = seen.begin(); it != seen.end();) it = IsWindow(*it) ? std::next(it) : seen.erase(it);
        EnumWindows([](HWND hwnd, LPARAM raw) -> BOOL { reinterpret_cast<Probe*>(raw)->Consider(hwnd); return TRUE; },
                    reinterpret_cast<LPARAM>(this));
    }
};
Probe* g_probe = nullptr;
void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
    if (g_probe && object == OBJID_WINDOW && child == CHILDID_SELF) g_probe->Consider(hwnd);
}
}

int wmain(int argc, wchar_t** argv) {
    unsigned minutes = 60;
    fs::path out_path = fs::absolute(L"dialog-coverage.jsonl");
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--minutes" && i + 1 < argc) minutes = static_cast<unsigned>(std::wcstoul(argv[++i], nullptr, 10));
        else if (arg == L"--out" && i + 1 < argc) out_path = argv[++i];
        else { std::fputs("pulse_dialog_coverage_probe [--minutes N] [--out file.jsonl]\n", stderr); return 2; }
    }
    if (!minutes || minutes > 24 * 60) return 2;
    Probe probe;
    probe.out.open(out_path, std::ios::app | std::ios::binary);
    if (!probe.out) { std::fprintf(stderr, "cannot open output\n"); return 3; }
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        probe.runtime_logs = fs::path(local) / L"Pulse" / L"Diagnostics" / L"Runtime";
        CoTaskMemFree(local);
    }
    g_probe = &probe;
    probe.out << "{\"utc\":\"" << UtcNow() << "\",\"type\":\"start\",\"minutes\":" << minutes << "}\n";
    probe.out.flush();
    const HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, 0, 0,
                                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Children of a file dialog are created after its first show; the sweep catches those.
    const UINT_PTR timer = SetTimer(nullptr, 0, 1000, nullptr);
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(minutes) * 60000;
    MSG message{};
    while (GetTickCount64() < deadline) {
        const DWORD wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, 1000, QS_ALLINPUT);
        if (wait == WAIT_FAILED) break;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_TIMER) probe.Sweep();
            DispatchMessageW(&message);
        }
    }
    KillTimer(nullptr, timer);
    if (hook) UnhookWinEvent(hook);
    std::string summary;
    for (const auto& [key, count] : probe.totals) summary += (summary.empty() ? "\"" : ",\"") + key + "\":" + std::to_string(count);
    probe.out << "{\"utc\":\"" << UtcNow() << "\",\"type\":\"summary\",\"totals\":{" << summary << "}}\n";
    std::printf("summary {%s}\n", summary.c_str());
    return 0;
}
