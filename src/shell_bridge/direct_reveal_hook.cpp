#include "direct_reveal_hook.h"
#include "../common/direct_reveal_client.h"
#include <detours.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <string_view>
namespace pulse::shell_bridge::runtime {
namespace {
using Microsoft::WRL::ComPtr;

bool IsLocalDrivePath(std::wstring_view value) noexcept {
    return value.size() >= 3 &&
        ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) &&
        value[1] == L':' && value[2] == L'\\' &&
        value.find(L'\0') == value.npos && value.find(L'\r') == value.npos && value.find(L'\n') == value.npos;
}

std::wstring_view TrimWhitespace(std::wstring_view text) noexcept {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.remove_suffix(1);
    return text;
}

bool StripMatchingQuotes(std::wstring_view& text) noexcept {
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
        text.remove_prefix(1);
        text.remove_suffix(1);
        return true;
    }
    return false;
}

bool EndsWithInsensitive(std::wstring_view text, std::wstring_view suffix) noexcept {
    return text.size() >= suffix.size() &&
        CompareStringOrdinal(text.data() + (text.size() - suffix.size()), static_cast<int>(suffix.size()),
            suffix.data(), static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
}

bool StartsWithInsensitive(std::wstring_view text, std::wstring_view prefix) noexcept {
    return text.size() >= prefix.size() &&
        CompareStringOrdinal(text.data(), static_cast<int>(prefix.size()),
            prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

std::wstring SystemExplorerPath() {
    wchar_t windows[32768]{};
    const UINT length = GetWindowsDirectoryW(windows, 32768);
    if (!length || length >= 32768) return {};
    return std::wstring(windows, length) + L"\\explorer.exe";
}

bool IsSystemExplorerFile(const wchar_t* file, const wchar_t* directory) noexcept {
    try {
        if (!file || !*file) return false;
        const auto executable = SystemExplorerPath();
        if (executable.empty()) return false;
        std::wstring_view raw = TrimWhitespace(file);
        StripMatchingQuotes(raw);
        if (raw.empty() || raw.find(L'"') != raw.npos) return false;
        std::wstring candidate(raw);
        if (_wcsicmp(candidate.c_str(), L"explorer.exe") == 0) {
            if (directory && *directory) return false;
            // ShellExecute additionally consults App Paths, unlike SearchPath.
            // Any override (including unreadable keys) keeps the call native.
            for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
                for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
                    HKEY key = nullptr;
                    const LSTATUS status = RegOpenKeyExW(root,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\explorer.exe",
                        0, KEY_READ | view, &key);
                    if (key) RegCloseKey(key);
                    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) return false;
                }
            }
            wchar_t resolved[32768]{};
            const DWORD found = SearchPathW(nullptr, candidate.c_str(), nullptr, 32768, resolved, nullptr);
            return found && found < 32768 &&
                CompareStringOrdinal(resolved, -1, executable.c_str(), -1, TRUE) == CSTR_EQUAL;
        }
        return CompareStringOrdinal(candidate.c_str(), -1, executable.c_str(), -1, TRUE) == CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

bool ExpandEnvIfNeeded(std::wstring_view raw, std::wstring& expanded) {
    expanded.assign(raw);
    if (expanded.find(L'%') == std::wstring::npos) return true;
    wchar_t buffer[32768]{};
    const DWORD count = ExpandEnvironmentStringsW(expanded.c_str(), buffer, 32768);
    if (!count || count >= 32768) return false;
    expanded.assign(buffer, count - 1);
    return true;
}

bool NormalizeLocalPath(std::wstring& path) {
    for (auto& ch : path) if (ch == L'/') ch = L'\\';
    if (path.starts_with(L"\\\\?\\") && path.size() >= 7 && path[5] == L':') path.erase(0, 4);
    return IsLocalDrivePath(path);
}

bool ResolveExplorerParameters(const wchar_t* parameters, DirectShellAction& action, std::wstring& path, unsigned depth) noexcept;

// Offline-only .lnk inspection (never calls IShellLink::Resolve, so it never blocks
// on network shares, distributed link tracking, or shell search dialogs).
bool ResolveShortcutFile(const std::wstring& lnk_path, DirectShellAction& action, std::wstring& out_path, unsigned depth) noexcept {
    action = DirectShellAction::None;
    out_path.clear();
    if (depth > 1) return false;
    try {
        struct ComScope {
            bool active = false;
            ~ComScope() { if (active) CoUninitialize(); }
        } com_scope{};
        ComPtr<IShellLinkW> link;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        if (hr == CO_E_NOTINITIALIZED) {
            hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(hr)) {
                com_scope.active = true;
                hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
            }
        }
        if (FAILED(hr) || !link) return false;

        ComPtr<IPersistFile> persist;
        if (FAILED(link.As(&persist)) || !persist) return false;
        if (FAILED(persist->Load(lnk_path.c_str(), STGM_READ))) return false;
        persist.Reset();

        wchar_t args_buf[2048]{};
        if (FAILED(link->GetArguments(args_buf, ARRAYSIZE(args_buf)))) args_buf[0] = L'\0';
        const std::wstring_view args = TrimWhitespace(args_buf);

        std::wstring target;
        wchar_t target_buf[32768]{};
        if (link->GetPath(target_buf, ARRAYSIZE(target_buf), nullptr, SLGP_RAWPATH) == S_OK && target_buf[0]) {
            if (!ExpandEnvIfNeeded(target_buf, target)) return false;
        }
        if (target.empty()) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(link->GetIDList(&pidl)) && pidl) {
                std::wstring pidl_path(32768, L'\0');
                const BOOL ok = SHGetPathFromIDListEx(pidl, pidl_path.data(), static_cast<DWORD>(pidl_path.size()), GPFIDL_DEFAULT);
                CoTaskMemFree(pidl);
                if (ok) {
                    pidl_path.resize(wcslen(pidl_path.c_str()));
                    target = std::move(pidl_path);
                }
            }
        }
        if (!NormalizeLocalPath(target)) return false;

        if (!args.empty()) {
            // Only handle shortcuts explicitly targeting explorer.exe with folder/select arguments.
            const auto explorer = SystemExplorerPath();
            if (explorer.empty() || CompareStringOrdinal(target.c_str(), -1, explorer.c_str(), -1, TRUE) != CSTR_EQUAL)
                return false;
            return ResolveExplorerParameters(std::wstring(args).c_str(), action, out_path, depth + 1);
        }

        const DWORD attrs = GetFileAttributesW(target.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;
        action = DirectShellAction::OpenFolder;
        out_path = std::move(target);
        return true;
    } catch (...) {
        action = DirectShellAction::None;
        out_path.clear();
        return false;
    }
}

bool ResolveLocalDirectoryOrShortcut(std::wstring candidate, DirectShellAction& action, std::wstring& out_path, unsigned depth) noexcept {
    action = DirectShellAction::None;
    out_path.clear();
    try {
        if (!NormalizeLocalPath(candidate)) return false;
        const DWORD attrs = GetFileAttributesW(candidate.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return false;
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            action = DirectShellAction::OpenFolder;
            out_path = std::move(candidate);
            return true;
        }
        if (EndsWithInsensitive(candidate, L".lnk")) {
            return ResolveShortcutFile(candidate, action, out_path, depth);
        }
        return false;
    } catch (...) {
        action = DirectShellAction::None;
        out_path.clear();
        return false;
    }
}

bool ResolveExplorerParameters(const wchar_t* parameters, DirectShellAction& action, std::wstring& path, unsigned depth) noexcept {
    action = DirectShellAction::None;
    path.clear();
    try {
        if (!parameters) return false;
        std::wstring_view args = TrimWhitespace(parameters);
        if (args.empty()) return false;
        if (StartsWithInsensitive(args, L"/select,")) {
            args.remove_prefix(8);
            args = TrimWhitespace(args);
            const bool quoted = StripMatchingQuotes(args);
            if (!IsLocalDrivePath(args) || args.find(L'"') != args.npos || (!quoted && args.find(L',') != args.npos))
                return false;
            action = DirectShellAction::Reveal;
            path.assign(args);
            return true;
        }
        // Optional single /e, or /n, folder-open switch; any other Explorer switch stays native.
        if (StartsWithInsensitive(args, L"/e,") || StartsWithInsensitive(args, L"/n,")) {
            args.remove_prefix(3);
            args = TrimWhitespace(args);
        }
        if (args.empty() || args.front() == L'/' || args.front() == L'-') return false;
        const bool quoted = StripMatchingQuotes(args);
        if (!IsLocalDrivePath(args) || args.find(L'"') != args.npos || (!quoted && args.find(L',') != args.npos))
            return false;
        return ResolveLocalDirectoryOrShortcut(std::wstring(args), action, path, depth);
    } catch (...) {
        action = DirectShellAction::None;
        path.clear();
        return false;
    }
}

bool AnsiToWide(const char* input, std::wstring& output) {
    output.clear();
    if (!input || !*input) return true;
    const int chars = MultiByteToWideChar(CP_ACP, 0, input, -1, nullptr, 0);
    if (chars <= 1) return false;
    output.resize(static_cast<size_t>(chars - 1));
    return MultiByteToWideChar(CP_ACP, 0, input, -1, output.data(), chars) == chars;
}

bool ShiftHeld() noexcept {
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

constexpr ULONG kSafeExecuteExMask =
    SEE_MASK_IDLIST |
    SEE_MASK_INVOKEIDLIST |
    SEE_MASK_NOASYNC |
    SEE_MASK_DOENVSUBST |
    SEE_MASK_FLAG_NO_UI |
    SEE_MASK_UNICODE |
    SEE_MASK_NO_CONSOLE |
    SEE_MASK_ASYNCOK |
    SEE_MASK_NOQUERYCLASSSTORE |
    SEE_MASK_WAITFORINPUTIDLE |
    SEE_MASK_FLAG_LOG_USAGE |
    SEE_MASK_NOZONECHECKS;

thread_local bool g_entering_direct_shell = false;
struct ReentrancyGuard {
    bool entered = false;
    ReentrancyGuard() noexcept {
        if (!g_entering_direct_shell) {
            g_entering_direct_shell = true;
            entered = true;
        }
    }
    ~ReentrancyGuard() {
        if (entered) g_entering_direct_shell = false;
    }
};

pulse::DirectRevealResult SendAction(DirectShellAction action, const std::wstring& path,
    DirectRevealSender reveal_sender, DirectRevealSender open_sender) noexcept {
    if (action == DirectShellAction::Reveal) {
        return reveal_sender ? reveal_sender(path, 250) : pulse::DirectRevealResult::Rejected;
    }
    if (action == DirectShellAction::OpenFolder) {
        return open_sender ? open_sender(path, 250) : pulse::DirectRevealResult::Rejected;
    }
    return pulse::DirectRevealResult::Rejected;
}

decltype(&SHOpenFolderAndSelectItems) original = &SHOpenFolderAndSelectItems;
decltype(&ShellExecuteExW) original_execute_ex = &ShellExecuteExW;
decltype(&ShellExecuteW) original_execute = &ShellExecuteW;
decltype(&ShellExecuteExA) original_execute_ex_a = &ShellExecuteExA;
decltype(&ShellExecuteA) original_execute_a = &ShellExecuteA;
std::atomic<DirectRevealGate> enabled{nullptr};
std::atomic<DirectRevealObserver> observe{nullptr};
std::atomic<DirectRevealSender> send{&DefaultDirectRevealSend};
std::atomic<DirectRevealSender> open_send{&DefaultDirectOpenFolderSend};
std::atomic<bool> installed_flag{false};
void Observed(const char* entry, pulse::DirectRevealResult result) noexcept {
    if (auto callback = observe.load(std::memory_order_acquire)) callback(entry, static_cast<unsigned>(result));
}
std::mutex install_mutex;

HRESULT WINAPI Reveal(PCIDLIST_ABSOLUTE folder, UINT count, PCUITEMID_CHILD_ARRAY children, DWORD flags) {
    auto gate = enabled.load(std::memory_order_acquire);
    if (!g_entering_direct_shell && gate && gate()) {
        const auto result = DispatchDirectRevealPidl(folder, count, children, flags, send.load(std::memory_order_acquire));
        Observed("SHOpenFolderAndSelectItems", result);
        // Pulse first; anything short of an acknowledged reveal stays native.
        if (result == pulse::DirectRevealResult::Accepted) return S_OK;
    }
    return original(folder, count, children, flags);
}

BOOL WINAPI ExecuteEx(SHELLEXECUTEINFOW* info) {
    auto gate = enabled.load(std::memory_order_acquire);
    if (!g_entering_direct_shell && gate && gate()) {
        const auto result = DispatchDirectShellExecuteExW(info, send.load(std::memory_order_acquire),
            open_send.load(std::memory_order_acquire));
        if (result != pulse::DirectRevealResult::Rejected) Observed("ShellExecuteExW", result);
        if (result == pulse::DirectRevealResult::Accepted) { info->hInstApp = reinterpret_cast<HINSTANCE>(33); return TRUE; }
    }
    ReentrancyGuard guard;
    return original_execute_ex(info);
}

HINSTANCE WINAPI Execute(HWND window, LPCWSTR verb, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory, INT show) {
    auto gate = enabled.load(std::memory_order_acquire);
    if (!g_entering_direct_shell && gate && gate()) {
        const auto result = DispatchDirectShellExecuteW(verb, file, parameters, directory, show,
            send.load(std::memory_order_acquire), open_send.load(std::memory_order_acquire));
        if (result != pulse::DirectRevealResult::Rejected) Observed("ShellExecuteW", result);
        if (result == pulse::DirectRevealResult::Accepted) return reinterpret_cast<HINSTANCE>(33);
    }
    // Hold re-entrancy across the native fallback because ShellExecuteW calls ShellExecuteExW internally.
    ReentrancyGuard guard;
    return original_execute(window, verb, file, parameters, directory, show);
}

BOOL WINAPI ExecuteExA(SHELLEXECUTEINFOA* info) {
    auto gate = enabled.load(std::memory_order_acquire);
    if (!g_entering_direct_shell && gate && gate()) {
        const auto result = DispatchDirectShellExecuteExA(info, send.load(std::memory_order_acquire),
            open_send.load(std::memory_order_acquire));
        if (result != pulse::DirectRevealResult::Rejected) Observed("ShellExecuteExA", result);
        if (result == pulse::DirectRevealResult::Accepted) { info->hInstApp = reinterpret_cast<HINSTANCE>(33); return TRUE; }
    }
    ReentrancyGuard guard;
    return original_execute_ex_a(info);
}

HINSTANCE WINAPI ExecuteA(HWND window, LPCSTR verb, LPCSTR file, LPCSTR parameters, LPCSTR directory, INT show) {
    auto gate = enabled.load(std::memory_order_acquire);
    if (!g_entering_direct_shell && gate && gate()) {
        const auto result = DispatchDirectShellExecuteA(verb, file, parameters, directory, show,
            send.load(std::memory_order_acquire), open_send.load(std::memory_order_acquire));
        if (result != pulse::DirectRevealResult::Rejected) Observed("ShellExecuteA", result);
        if (result == pulse::DirectRevealResult::Accepted) return reinterpret_cast<HINSTANCE>(33);
    }
    ReentrancyGuard guard;
    return original_execute_a(window, verb, file, parameters, directory, show);
}

struct Threads {
    std::vector<HANDLE> values;
    ~Threads() { for (HANDLE value : values) CloseHandle(value); }
};
}

bool DirectRevealPath(PCIDLIST_ABSOLUTE folder, UINT count, PCUITEMID_CHILD_ARRAY children,
    DWORD flags, std::wstring& path) noexcept {
    path.clear();
    if (!folder || flags || count > 1 || (count && (!children || !children[0]))) return false;
    PIDLIST_ABSOLUTE combined = nullptr;
    PCIDLIST_ABSOLUTE target = folder;
    if (count) {
        const auto child = children[0];
        if (ILIsEmpty(child)) return false;
        if (ILFindLastID(child) == child) {
            combined = ILCombine(folder, child);
            if (!combined) return false;
            target = combined;
        } else {
            // Some callers pass a desktop-relative PIDL in the child array.
            // Only an immediate child of this folder represents this request.
            const auto full = reinterpret_cast<PCIDLIST_ABSOLUTE>(child);
            if (!ILIsParent(folder, full, TRUE)) return false;
            target = full;
        }
    }
    try {
        std::wstring value(32768, L'\0');
        const BOOL ok = SHGetPathFromIDListEx(target, value.data(),
            static_cast<DWORD>(value.size()), GPFIDL_DEFAULT);
        CoTaskMemFree(combined); combined = nullptr;
        if (!ok) return false;
        value.resize(wcslen(value.c_str()));
        // Receiver supports local drive paths only. Network/virtual paths stay native.
        if (!IsLocalDrivePath(value)) return false;
        path = std::move(value); return true;
    } catch (...) { CoTaskMemFree(combined); return false; }
}

bool DirectExplorerSelection(const wchar_t* file, const wchar_t* parameters, std::wstring& path) noexcept {
    path.clear();
    if (!file || !parameters || !IsSystemExplorerFile(file, nullptr)) return false;
    DirectShellAction action = DirectShellAction::None;
    return ResolveExplorerParameters(parameters, action, path, 0) && action == DirectShellAction::Reveal;
}

bool ResolveDirectShellTarget(const wchar_t* verb, const wchar_t* file, const wchar_t* parameters,
    const wchar_t* directory, DirectShellAction& action, std::wstring& path) noexcept {
    action = DirectShellAction::None;
    path.clear();
    try {
        if (!file || !*file) return false;
        const bool empty_or_open = !verb || !*verb || _wcsicmp(verb, L"open") == 0;
        const bool explore = verb && _wcsicmp(verb, L"explore") == 0;
        if (!empty_or_open && !explore) return false;

        if (empty_or_open && parameters && *parameters && IsSystemExplorerFile(file, directory)) {
            return ResolveExplorerParameters(parameters, action, path, 0);
        }

        // Direct folder / folder-shortcut open requires no extra parameters.
        if (parameters && !TrimWhitespace(parameters).empty()) return false;

        std::wstring_view raw = TrimWhitespace(file);
        StripMatchingQuotes(raw);
        if (raw.empty() || raw.find(L'"') != raw.npos) return false;

        std::wstring candidate(raw);
        if (!IsLocalDrivePath(candidate) && !candidate.starts_with(L"\\\\?\\") && directory && *directory) {
            // Resolve bare relative filename (e.g. "MyFolder.lnk") against local lpDirectory.
            if (candidate.find_first_of(L":\\/") == std::wstring::npos && candidate != L"." && candidate != L"..") {
                std::wstring_view dir_view = TrimWhitespace(directory);
                StripMatchingQuotes(dir_view);
                std::wstring dir(dir_view);
                if (NormalizeLocalPath(dir)) {
                    if (!dir.empty() && dir.back() != L'\\') dir.push_back(L'\\');
                    candidate = dir + candidate;
                }
            }
        }
        return ResolveLocalDirectoryOrShortcut(std::move(candidate), action, path, 0);
    } catch (...) {
        action = DirectShellAction::None;
        path.clear();
        return false;
    }
}

bool ResolveDirectShellExecuteExTarget(const SHELLEXECUTEINFOW& info,
    DirectShellAction& action, std::wstring& path) noexcept {
    action = DirectShellAction::None;
    path.clear();
    try {
        if (info.cbSize != sizeof(info) || info.nShow == SW_HIDE) return false;
        if ((info.fMask & ~kSafeExecuteExMask) != 0) return false;

        if ((info.fMask & SEE_MASK_IDLIST) && info.lpIDList) {
            std::wstring pidl_path(32768, L'\0');
            const BOOL ok = SHGetPathFromIDListEx(reinterpret_cast<PCIDLIST_ABSOLUTE>(info.lpIDList),
                pidl_path.data(), static_cast<DWORD>(pidl_path.size()), GPFIDL_DEFAULT);
            if (!ok) return false;
            pidl_path.resize(wcslen(pidl_path.c_str()));
            return ResolveDirectShellTarget(info.lpVerb, pidl_path.c_str(), info.lpParameters, info.lpDirectory, action, path);
        }

        if (!info.lpFile || !*info.lpFile) return false;
        std::wstring expanded;
        const wchar_t* file = info.lpFile;
        if ((info.fMask & SEE_MASK_DOENVSUBST) && wcschr(file, L'%')) {
            if (!ExpandEnvIfNeeded(file, expanded)) return false;
            file = expanded.c_str();
        }
        return ResolveDirectShellTarget(info.lpVerb, file, info.lpParameters, info.lpDirectory, action, path);
    } catch (...) {
        action = DirectShellAction::None;
        path.clear();
        return false;
    }
}

bool ResolveDirectShellExecuteATarget(const char* verb, const char* file, const char* parameters,
    const char* directory, DirectShellAction& action, std::wstring& path) noexcept {
    action = DirectShellAction::None;
    path.clear();
    try {
        if (!file || !*file) return false;
        std::wstring wverb, wfile, wparams, wdir;
        if (!AnsiToWide(verb, wverb) || !AnsiToWide(file, wfile) ||
            !AnsiToWide(parameters, wparams) || !AnsiToWide(directory, wdir)) return false;
        return ResolveDirectShellTarget(
            verb ? wverb.c_str() : nullptr,
            wfile.c_str(),
            parameters ? wparams.c_str() : nullptr,
            directory ? wdir.c_str() : nullptr,
            action, path);
    } catch (...) {
        action = DirectShellAction::None;
        path.clear();
        return false;
    }
}

bool ResolveDirectShellExecuteExATarget(const SHELLEXECUTEINFOA& info,
    DirectShellAction& action, std::wstring& path) noexcept {
    action = DirectShellAction::None;
    path.clear();
    try {
        if (info.cbSize != sizeof(info) || info.nShow == SW_HIDE) return false;
        if ((info.fMask & ~kSafeExecuteExMask) != 0) return false;
        std::wstring wverb, wfile, wparams, wdir;
        if (!AnsiToWide(info.lpVerb, wverb) || !AnsiToWide(info.lpFile, wfile) ||
            !AnsiToWide(info.lpParameters, wparams) || !AnsiToWide(info.lpDirectory, wdir)) return false;
        SHELLEXECUTEINFOW wide{};
        wide.cbSize = sizeof(wide);
        wide.fMask = info.fMask;
        wide.hwnd = info.hwnd;
        wide.lpVerb = info.lpVerb ? wverb.c_str() : nullptr;
        wide.lpFile = info.lpFile ? wfile.c_str() : nullptr;
        wide.lpParameters = info.lpParameters ? wparams.c_str() : nullptr;
        wide.lpDirectory = info.lpDirectory ? wdir.c_str() : nullptr;
        wide.nShow = info.nShow;
        wide.lpIDList = info.lpIDList;
        return ResolveDirectShellExecuteExTarget(wide, action, path);
    } catch (...) {
        action = DirectShellAction::None;
        path.clear();
        return false;
    }
}

pulse::DirectRevealResult DispatchDirectRevealPidl(PCIDLIST_ABSOLUTE folder, UINT count,
    PCUITEMID_CHILD_ARRAY children, DWORD flags, DirectRevealSender reveal_sender) noexcept {
    ReentrancyGuard guard;
    if (!guard.entered || ShiftHeld()) return pulse::DirectRevealResult::Rejected;
    std::wstring path;
    return DirectRevealPath(folder, count, children, flags, path)
        ? SendAction(DirectShellAction::Reveal, path, reveal_sender, nullptr)
        : pulse::DirectRevealResult::Rejected;
}

pulse::DirectRevealResult DispatchDirectShellExecuteW(LPCWSTR verb, LPCWSTR file, LPCWSTR parameters,
    LPCWSTR directory, INT show, DirectRevealSender reveal_sender, DirectRevealSender open_sender) noexcept {
    ReentrancyGuard guard;
    if (!guard.entered || show == SW_HIDE || ShiftHeld()) return pulse::DirectRevealResult::Rejected;
    DirectShellAction action = DirectShellAction::None;
    std::wstring path;
    if (!ResolveDirectShellTarget(verb, file, parameters, directory, action, path))
        return pulse::DirectRevealResult::Rejected;
    return SendAction(action, path, reveal_sender, open_sender);
}

pulse::DirectRevealResult DispatchDirectShellExecuteExW(const SHELLEXECUTEINFOW* info,
    DirectRevealSender reveal_sender, DirectRevealSender open_sender) noexcept {
    ReentrancyGuard guard;
    if (!guard.entered || !info || ShiftHeld()) return pulse::DirectRevealResult::Rejected;
    DirectShellAction action = DirectShellAction::None;
    std::wstring path;
    if (!ResolveDirectShellExecuteExTarget(*info, action, path))
        return pulse::DirectRevealResult::Rejected;
    return SendAction(action, path, reveal_sender, open_sender);
}

pulse::DirectRevealResult DispatchDirectShellExecuteA(LPCSTR verb, LPCSTR file, LPCSTR parameters,
    LPCSTR directory, INT show, DirectRevealSender reveal_sender, DirectRevealSender open_sender) noexcept {
    ReentrancyGuard guard;
    if (!guard.entered || show == SW_HIDE || ShiftHeld()) return pulse::DirectRevealResult::Rejected;
    DirectShellAction action = DirectShellAction::None;
    std::wstring path;
    if (!ResolveDirectShellExecuteATarget(verb, file, parameters, directory, action, path))
        return pulse::DirectRevealResult::Rejected;
    return SendAction(action, path, reveal_sender, open_sender);
}

pulse::DirectRevealResult DispatchDirectShellExecuteExA(const SHELLEXECUTEINFOA* info,
    DirectRevealSender reveal_sender, DirectRevealSender open_sender) noexcept {
    ReentrancyGuard guard;
    if (!guard.entered || !info || ShiftHeld()) return pulse::DirectRevealResult::Rejected;
    DirectShellAction action = DirectShellAction::None;
    std::wstring path;
    if (!ResolveDirectShellExecuteExATarget(*info, action, path))
        return pulse::DirectRevealResult::Rejected;
    return SendAction(action, path, reveal_sender, open_sender);
}

bool IsDirectRevealHookInstalled() noexcept {
    return installed_flag.load(std::memory_order_acquire);
}

LONG InstallDirectRevealHook(HMODULE owner, DirectRevealGate gate, DirectRevealObserver observer, DirectRevealSender sender, pulse::DirectRevealInstallDiagnostic* diagnostic, DirectRevealSender open_sender) noexcept {
    pulse::DirectRevealInstallDiagnostic local;
    if (!diagnostic) diagnostic = &local;
    *diagnostic = {};
    const auto finish = [&](LONG error) { diagnostic->error = static_cast<DWORD>(error); return error; };
    try {
        std::lock_guard lock(install_mutex);
        diagnostic->stage = 1;
        if (!gate || !owner || !sender) return finish(ERROR_INVALID_PARAMETER);
        observe.store(observer, std::memory_order_release);
        send.store(sender, std::memory_order_release);
        const DirectRevealSender effective_open = open_sender ? open_sender :
            (sender == &DefaultDirectRevealSend ? &DefaultDirectOpenFolderSend : nullptr);
        open_send.store(effective_open, std::memory_order_release);
        if (installed_flag.load(std::memory_order_acquire)) { enabled.store(gate, std::memory_order_release); return NO_ERROR; }
        diagnostic->stage = 2;
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Reveal), &pinned) || pinned != owner) return finish(ERROR_INVALID_HANDLE);
        Threads threads;
        diagnostic->stage = 3;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return finish(static_cast<LONG>(GetLastError()));
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
        BOOL more = Thread32First(snapshot, &entry);
        DWORD error = more ? ERROR_SUCCESS : GetLastError();
        while (more) {
            if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId()) {
                diagnostic->stage = 4; diagnostic->thread_id = entry.th32ThreadID;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, entry.th32ThreadID);
                if (!thread) { error = GetLastError(); break; }
                try { threads.values.push_back(thread); } catch (...) { CloseHandle(thread); CloseHandle(snapshot); throw; }
            }
            more = Thread32Next(snapshot, &entry);
            if (!more && GetLastError() != ERROR_NO_MORE_FILES) error = GetLastError();
        }
        CloseHandle(snapshot);
        if (error) return finish(static_cast<LONG>(error));
        diagnostic->stage = 5;
        LONG status = DetourTransactionBegin();
        if (status != NO_ERROR) return finish(status);
        diagnostic->stage = 6;
        status = DetourUpdateThread(GetCurrentThread());
        for (HANDLE thread : threads.values) {
            if (status != NO_ERROR) break;
            diagnostic->thread_id = GetThreadId(thread);
            status = DetourUpdateThread(thread);
        }
        if (status == NO_ERROR) { diagnostic->stage = 7; status = DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(&Reveal)); }
        if (status == NO_ERROR) { diagnostic->stage = 8; status = DetourAttach(reinterpret_cast<PVOID*>(&original_execute_ex), reinterpret_cast<PVOID>(&ExecuteEx)); }
        if (status == NO_ERROR) { diagnostic->stage = 9; status = DetourAttach(reinterpret_cast<PVOID*>(&original_execute), reinterpret_cast<PVOID>(&Execute)); }
        if (status == NO_ERROR) { diagnostic->stage = 10; status = DetourAttach(reinterpret_cast<PVOID*>(&original_execute_ex_a), reinterpret_cast<PVOID>(&ExecuteExA)); }
        if (status == NO_ERROR) { diagnostic->stage = 11; status = DetourAttach(reinterpret_cast<PVOID*>(&original_execute_a), reinterpret_cast<PVOID>(&ExecuteA)); }
        if (status == NO_ERROR) { diagnostic->stage = 12; status = DetourTransactionCommit(); }
        else DetourTransactionAbort();
        if (status == NO_ERROR) { installed_flag.store(true, std::memory_order_release); enabled.store(gate, std::memory_order_release); }
        return finish(status);
    } catch (...) { return finish(ERROR_NOT_ENOUGH_MEMORY); }
}
}
