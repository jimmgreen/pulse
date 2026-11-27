#include "picker_jump_context.h"
#include "../ui/folder_picker_dialog.h"
#include "../common/picker_places_snapshot.h"
#include "../app/jump_history.h"
#include "../app/jump_match.h"
#include "../index/index_client.h"
#include "../index/index_executable.h"
#include "../common/dialog_bundle.h"
#include "../common/menu_icon.h"
#include "../common/localization.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "../common/path_utils.h"
#include "../common/display_path.h"
#include <filesystem>
#include <condition_variable>
#include <exdisp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <vector>

namespace pulse::picker_inprocess {
namespace {
using Microsoft::WRL::ComPtr;
std::atomic<unsigned> active_workers{0};
std::atomic<unsigned> active_shell_workers{0};
std::wstring Executable(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    if (!ok) return {};
    path.resize(length); return path;
}
std::wstring Description(const std::wstring& path) {
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size && size <= 4 * 1024 * 1024) {
        std::vector<BYTE> bytes(size);
        if (GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) {
            struct Translation { WORD language, codepage; };
            Translation* translations = nullptr; UINT count = 0;
            if (VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &count)) {
                for (UINT i = 0; i < count / sizeof(Translation); ++i) {
                    wchar_t query[80]{};
                    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[i].language, translations[i].codepage);
                    wchar_t* text = nullptr; UINT length = 0;
                    if (VerQueryValueW(bytes.data(), query, reinterpret_cast<void**>(&text), &length) && text && length > 1)
                        return std::wstring(text, wcsnlen_s(text, length));
                }
            }
        }
    }
    const auto slash = path.find_last_of(L"\\/");
    return path.substr(slash == path.npos ? 0 : slash + 1);
}
bool SameName(const std::wstring& path, const wchar_t* name) {
    const auto slash = path.find_last_of(L"\\/");
    return CompareStringOrdinal(path.c_str() + (slash == path.npos ? 0 : slash + 1), -1, name, -1, TRUE) == CSTR_EQUAL;
}
// PickerHost.exe opens the brokered pickers of packaged apps (Settings' 浏览照片),
// so the program is the window the user was in when the dialog started: the
// foreground window, or for a UWP frame (ApplicationFrameHost) its CoreWindow.
DWORD BrokeredApp(DWORD process_id) {
    if (!SameName(Executable(process_id), L"PickerHost.exe")) return process_id;
    HWND root = GetAncestor(GetForegroundWindow(), GA_ROOTOWNER);
    DWORD owner = 0;
    if (!root || !GetWindowThreadProcessId(root, &owner) || !owner) return process_id;
    if (SameName(Executable(owner), L"ApplicationFrameHost.exe")) {
        struct Find { DWORD frame; DWORD found = 0; } find{owner};
        EnumChildWindows(root, [](HWND child, LPARAM value) -> BOOL {
            auto& f = *reinterpret_cast<Find*>(value);
            DWORD pid = 0;
            if (GetWindowThreadProcessId(child, &pid) && pid && pid != f.frame) { f.found = pid; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&find));
        owner = find.found;
    }
    return owner && owner != process_id && owner != GetCurrentProcessId() ? owner : process_id;
}
// A packaged program's AppUserModelID, empty for ordinary programs.
std::wstring AppUserModelId(DWORD process_id) {
    using Query = LONG(WINAPI*)(HANDLE, UINT32*, PWSTR);
    static const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetApplicationUserModelId"));
    if (!query) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return {};
    wchar_t id[256]{}; UINT32 length = static_cast<UINT32>(std::size(id));
    const LONG status = query(process, &length, id);
    CloseHandle(process);
    return status == ERROR_SUCCESS ? std::wstring(id) : std::wstring{};
}
// The program's Start menu entry (shell:AppsFolder\<id>): the name and icon
// Explorer shows, e.g. 设置 / 记事本 rather than SystemSettings.exe / Notepad.exe.
ComPtr<IShellItem> AppsFolderItem(const std::wstring& id) {
    ComPtr<IShellItem> item;
    if (!id.empty()) SHCreateItemFromParsingName((L"shell:AppsFolder\\" + id).c_str(), nullptr, IID_PPV_ARGS(&item));
    return item;
}
std::wstring PackagedName(const std::wstring& id) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring name;
    if (auto item = AppsFolderItem(id)) {
        PWSTR text = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &text)) && text) name = text;
        CoTaskMemFree(text);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return name;
}
// Caller's STA. The shell hands back a 32bpp DIB; rows are read top-down and
// the colour premultiplied when the source is straight alpha.
common::MenuIconRef PackagedIcon(const std::wstring& id, int edge) {
    auto item = AppsFolderItem(id);
    ComPtr<IShellItemImageFactory> factory;
    HBITMAP bitmap = nullptr;
    if (!item || FAILED(item.As(&factory)) || FAILED(factory->GetImage({edge, edge}, SIIGBF_ICONONLY, &bitmap)) || !bitmap) return {};
    BITMAP info{};
    common::MenuIconRef result;
    if (GetObjectW(bitmap, sizeof(info), &info) && info.bmWidth == edge && std::abs(info.bmHeight) == edge && info.bmBitsPixel == 32) {
        auto pixels = std::make_shared<common::MenuIconPixels>();
        pixels->width = pixels->height = static_cast<uint32_t>(edge);
        pixels->bgra.resize(static_cast<size_t>(edge) * edge * 4u);
        BITMAPINFO header{};
        header.bmiHeader.biSize = sizeof(header.bmiHeader);
        header.bmiHeader.biWidth = edge; header.bmiHeader.biHeight = -edge;
        header.bmiHeader.biPlanes = 1; header.bmiHeader.biBitCount = 32; header.bmiHeader.biCompression = BI_RGB;
        if (HDC screen = GetDC(nullptr)) {
            const int rows = GetDIBits(screen, bitmap, 0, static_cast<UINT>(edge), pixels->bgra.data(), &header, DIB_RGB_COLORS);
            ReleaseDC(nullptr, screen);
            if (rows == edge) {
                auto& px = pixels->bgra;
                bool straight = false;
                for (size_t i = 0; i < px.size() && !straight; i += 4) straight = px[i] > px[i + 3] || px[i + 1] > px[i + 3] || px[i + 2] > px[i + 3];
                if (straight) for (size_t i = 0; i < px.size(); i += 4)
                    for (int c = 0; c < 3; ++c) px[i + c] = static_cast<uint8_t>((px[i + c] * px[i + 3] + 127) / 255);
                if (pixels->Valid()) result = std::move(pixels);
            }
        }
    }
    DeleteObject(bitmap);
    return result;
}
void Add(std::vector<ui::PickerOpenFolder>& folders, ui::PickerOpenFolder folder) {
    if (!picker_places::detail::DiskPath(folder.path)) return;
    if (std::any_of(folders.begin(), folders.end(), [&](const auto& entry) { return _wcsicmp(entry.path.c_str(), folder.path.c_str()) == 0; })) return;
    folders.push_back(std::move(folder));
}
std::vector<ui::PickerOpenFolder> ExplorerFolders(const std::atomic<bool>& cancelled) {
    std::vector<ui::PickerOpenFolder> result;
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return result;
    long count = 0; if (FAILED(windows->get_Count(&count))) return result;
    for (long i = 0; i < std::min(count, 96L) && !cancelled.load(); ++i) {
        VARIANT index{}; index.vt = VT_I4; index.lVal = i;
        ComPtr<IDispatch> dispatch; ComPtr<IWebBrowser2> browser;
        if (FAILED(windows->Item(index, &dispatch)) || !dispatch || FAILED(dispatch.As(&browser))) continue;
        SHANDLE_PTR handle = 0; if (FAILED(browser->get_HWND(&handle))) continue;
        wchar_t name[64]{}; GetClassNameW(reinterpret_cast<HWND>(handle), name, 64);
        if (wcscmp(name, L"CabinetWClass") && wcscmp(name, L"ExploreWClass")) continue;
        ComPtr<IServiceProvider> provider; ComPtr<IShellBrowser> shell;
        ComPtr<IShellView> view; ComPtr<IFolderView2> folder_view; ComPtr<IPersistFolder2> folder;
        if (FAILED(browser.As(&provider)) || FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell))) ||
            FAILED(shell->QueryActiveShellView(&view)) || !view || FAILED(view.As(&folder_view)) ||
            FAILED(folder_view->GetFolder(IID_PPV_ARGS(&folder)))) continue;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(folder->GetCurFolder(&pidl)) || !pidl) continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &path)) && path) {
            Add(result, {path, {}, true}); CoTaskMemFree(path);
        }
        CoTaskMemFree(pidl);
    }
    return result;
}
}
// A module-bound resolver, not a process/PATH search. The caller keeps the
// verified version lease alive for the lifetime of the index client.
std::wstring ResolveJumpIndexHelper(const std::wstring& module_path, dialog_bundle::BundleLease& lease) {
    namespace fs = std::filesystem;
    const fs::path image(module_path);
    if (!image.is_absolute() || (_wcsicmp(image.filename().c_str(), L"PulsePickerUiProof.dll") &&
        _wcsicmp(image.filename().c_str(), L"pulse.exe"))) return {};
    auto root = image.parent_path();
    const auto hash = root.filename().wstring();
    if (_wcsicmp(root.parent_path().filename().c_str(), L"DialogBridge") == 0) {
        if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
        })) return {};
        std::string digest;
        digest.reserve(hash.size());
        for (const wchar_t c : hash) digest.push_back(static_cast<char>(c));
        if (dialog_bundle::VerifyBundle(root.wstring(), digest, lease) != ERROR_SUCCESS) return {};
        root = root.parent_path().parent_path();
    }
    const auto plain_file = [](const std::wstring& path) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    };
    const auto sibling = (root / L"Pulse.Index.exe").wstring();
    const auto resolved = index::ResolveIndexExecutable(sibling, {}, plain_file);
    return plain_file(resolved) ? resolved : std::wstring{};
}
namespace {
std::wstring OwnModulePath() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&OwnModulePath), &module)) return {};
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size); return path;
}
}
struct JumpContext::State {
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> cancelled{false};
    unsigned revision = 0, query_generation = 0;
    std::wstring name, query, completed_query, accepted, last_location;
    std::vector<std::wstring> forget;
    common::MenuIconRef icon;
    std::vector<ui::PickerOpenFolder> folders;
    std::vector<ui::PickerRecentPlace> recent;
    int index_status = 0;
    bool history_loaded = false;
    bool search_complete = false;
    uint64_t search_elapsed_ms = 0;
    std::vector<ui::PickerJumpResult> results;
    ui::PickerOptions filter;
    ui::PickerMode mode = ui::PickerMode::Folder;
    picker_places::Snapshot shared;
};
namespace {
std::wstring HistoryFile(const std::wstring& executable, int mode, const std::wstring& override_root) {
    if (executable.empty()) return {};
    std::wstring root = override_root;
    if (root.empty()) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local))) return {};
    root = std::wstring(local) + L"\\Pulse"; CoTaskMemFree(local);
    CreateDirectoryW(root.c_str(), nullptr); root += L"\\PickerJumpHistory"; CreateDirectoryW(root.c_str(), nullptr);
    }
    // Full exe identity, rather than display name, separates identically named apps.
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : executable) { hash ^= static_cast<uint64_t>(towlower(ch)); hash *= 1099511628211ull; }
    return root + L"\\" + std::to_wstring(hash) + L"-" + std::to_wstring(mode) + L".tsv";
}
// The folder's real location, so one folder reached two ways is one history entry:
// a mapped drive becomes its share path, a junction or subst drive its target,
// a short 8.3 name the long one. Only for a folder just accepted (it is
// reachable); any failure keeps the path as given. Called on the worker thread.
std::wstring CanonicalFolder(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return path;
    std::wstring out(MAX_PATH, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, out.data(), static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= out.size() && length < 32768) {
        out.assign(length + 1, L'\0');
        length = GetFinalPathNameByHandleW(handle, out.data(), static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(handle);
    if (!length || length >= out.size()) return path;
    out.resize(length);
    if (out.rfind(L"\\\\?\\UNC\\", 0) == 0) out = L"\\\\" + out.substr(8);
    else if (out.rfind(L"\\\\?\\", 0) == 0) out = out.substr(4);
    return out.empty() ? path : out;
}
std::wstring Parent(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == path.npos) return {};
    return path.substr(0, slash == 2 && path[1] == L':' ? 3 : slash);
}
std::wstring CountText(l10n::StringId id, uint64_t count) {
    std::wstring text = l10n::Get(id);
    const auto at = text.find(L"{count}");
    if (at != text.npos) text.replace(at, 7, std::to_wstring(count));
    return text;
}
std::wstring VisitDate(int64_t seconds, int64_t now = app::JumpHistory::Now()) {
    if (seconds <= 0 || seconds > 253402300799LL || now <= 0 || now > 253402300799LL) return {};
    const auto age = std::max<int64_t>(0, now - seconds);
    if (age < 60) return l10n::Get(l10n::StringId::PickerJustNow);
    if (age < 3600) return CountText(l10n::StringId::PickerMinutesAgo, age / 60);
    const uint64_t ticks = (static_cast<uint64_t>(seconds) + 11644473600ULL) * 10000000ULL;
    const FILETIME utc{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    FILETIME local{}; SYSTEMTIME date{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &date)) return {};
    const uint64_t now_ticks = (static_cast<uint64_t>(now) + 11644473600ULL) * 10000000ULL;
    const FILETIME now_utc{static_cast<DWORD>(now_ticks), static_cast<DWORD>(now_ticks >> 32)};
    FILETIME now_local{}; SYSTEMTIME today{};
    if (!FileTimeToLocalFileTime(&now_utc, &now_local) || !FileTimeToSystemTime(&now_local, &today)) return {};
    today.wHour = today.wMinute = today.wSecond = today.wMilliseconds = 0;
    FILETIME midnight{}; SystemTimeToFileTime(&today, &midnight);
    const uint64_t midnight_ticks = (static_cast<uint64_t>(midnight.dwHighDateTime) << 32) | midnight.dwLowDateTime;
    const uint64_t local_ticks = (static_cast<uint64_t>(local.dwHighDateTime) << 32) | local.dwLowDateTime;
    if (local_ticks < midnight_ticks && midnight_ticks - local_ticks <= 864000000000ULL)
        return l10n::Get(l10n::StringId::PickerYesterday);
    if (age < 86400) return CountText(l10n::StringId::PickerHoursAgo, age / 3600);
    constexpr uint64_t day_ticks = 864000000000ULL;
    const uint64_t days = local_ticks < midnight_ticks ? (midnight_ticks - local_ticks + day_ticks - 1) / day_ticks : 0;
    if (days >= 2 && days <= 6) {
        std::wstring text = l10n::Get(l10n::StringId::ChangeDaysAgoFormat);
        const auto at = text.find(L"%d");
        if (at != text.npos) text.replace(at, 2, std::to_wstring(days));
        return text;
    }
    wchar_t first_day_text[8]{};
    const bool have_first_day = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, first_day_text, 8) != 0;
    const unsigned first_day = have_first_day ? (static_cast<unsigned>(_wtoi(first_day_text)) + 1) % 7 : 1;
    const unsigned weekday_offset = (today.wDayOfWeek + 7 - first_day) % 7;
    const uint64_t week_start = midnight_ticks - weekday_offset * day_ticks;
    if (local_ticks < week_start && local_ticks >= week_start - 7 * day_ticks)
        return l10n::Get(l10n::StringId::GroupDateLastWeek);
    wchar_t text[96]{};
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &date, nullptr, text, 96, nullptr)) return {};
    return text;
}
std::wstring AppCaption(l10n::StringId id, const std::wstring& name) {
    std::wstring text = l10n::Get(id);
    const auto at = text.find(L"{name}");
    if (at != text.npos) text.replace(at, 6, name);
    return text;
}
}
std::wstring FormatJumpVisitDate(int64_t seconds, int64_t now) {
    return VisitDate(seconds, now);
}
JumpContext::JumpContext(DWORD process_id, int mode, JumpContextOptions options) : state_(std::make_shared<State>()) {
    state_->mode = static_cast<ui::PickerMode>(mode);
    // Before the dialog takes the foreground: a broker's caller is still in front.
    process_id = BrokeredApp(process_id);
    if (active_workers.fetch_add(1) >= 2) { active_workers.fetch_sub(1); return; }
    try {
        std::thread([state = state_, process_id, mode, options = std::move(options)] {
            struct WorkerSlot { ~WorkerSlot() { active_workers.fetch_sub(1); } } slot;
            try {
                const auto executable = Executable(process_id);
                const auto aumid = AppUserModelId(process_id);
                auto name = PackagedName(aumid);
                if (name.empty()) name = Description(executable);
                // Branding does not depend on history or potentially offline recent paths.
                { std::lock_guard lock(state->mutex); state->name = name; }
                const auto shared = options.read_places ? options.read_places() : picker_places::Read();
                std::vector<ui::PickerOpenFolder> folders;
                for (const auto& entry : shared.folders) if (entry.open) Add(folders, {entry.path, entry.label, false});
                const bool remember = shared.jump_policy_available && shared.remember;
                const auto history_file = remember ? HistoryFile(executable, mode, options.history_root) : std::wstring{};
                app::JumpHistory history;
                if (!history_file.empty()) history.Open(history_file);
                std::vector<ui::PickerRecentPlace> recent;
                std::wstring last_location;
                int64_t last_visit = 0;
                if (!history_file.empty()) for (const auto& path : history.Top({}, app::JumpHistory::kMaxEntries)) {
                    const auto visit = history.LastUsed(path);
                    if (visit > last_visit) { last_visit = visit; last_location = path; }
                    if (recent.size() < 16) {
                        const auto count = history.VisitCount(path);
                        recent.push_back({path, {}, count ? CountText(l10n::StringId::PickerVisitCount, count) : VisitDate(visit)});
                    }
                }
                {
                    std::lock_guard lock(state->mutex);
                    state->name = name; state->folders = folders; state->shared = shared; state->recent = recent;
                    state->last_location = std::move(last_location); state->history_loaded = true; ++state->revision;
                }
                // A separate STA isolates hung Explorer providers from search/history.
                if (options.enumerate_explorer) {
                if (active_shell_workers.fetch_add(1) < 2) {
                try { std::thread([state, executable, aumid, enumerate_explorer = options.enumerate_explorer] {
                    struct ShellSlot { ~ShellSlot() { active_shell_workers.fetch_sub(1); } } shell_slot;
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    struct ComScope { HRESULT hr; ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); } } com_scope{com};
                    try {
                        // The title icon (20 DIP) at the system DPI, extracted at that exact size:
                        // a 16 px small icon stretched up looked blurred. ",0" keeps a comma in
                        // the path from being read as the icon index.
                        common::MenuIconRef icon;
                        if (!executable.empty() && !state->cancelled.load()) {
                            int dpi = 96;
                            if (HDC screen = GetDC(nullptr)) { dpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(nullptr, screen); }
                            const int edge = std::clamp(MulDiv(20, dpi > 0 ? dpi : 96, 96), 16, static_cast<int>(common::kMaxMenuIconEdge));
                            // Every dialog this program opens asks again: extract once per
                            // process and size. Never destroyed (detached workers at exit).
                            static std::mutex& icon_mutex = *new std::mutex;
                            static auto& icon_cache = *new std::unordered_map<std::wstring, common::MenuIconRef>;
                            const std::wstring key = (aumid.empty() ? executable : aumid) + L"|" + std::to_wstring(edge);
                            bool cached = false;
                            { std::lock_guard lock(icon_mutex); if (const auto it = icon_cache.find(key); it != icon_cache.end()) { icon = it->second; cached = true; } }
                            if (!cached) {
                                if (!aumid.empty() && SUCCEEDED(com)) icon = PackagedIcon(aumid, edge);
                                if (!icon) icon = common::LoadIconPixels(executable + L",0", static_cast<uint32_t>(edge));
                                std::lock_guard lock(icon_mutex);
                                if (icon_cache.size() >= 16) icon_cache.clear();
                                icon_cache.emplace(key, icon);
                            }
                        }
                        { std::lock_guard lock(state->mutex); state->icon = std::move(icon); ++state->revision; }
                        if (SUCCEEDED(com) && !state->cancelled.load()) {
                            auto explorer = enumerate_explorer ? ExplorerFolders(state->cancelled) : std::vector<ui::PickerOpenFolder>{};
                            std::lock_guard lock(state->mutex);
                            for (auto& entry : explorer) Add(state->folders, std::move(entry));
                            ++state->revision;
                        }
                    } catch (...) {}
                }).detach(); } catch (...) { active_shell_workers.fetch_sub(1); }
                } else active_shell_workers.fetch_sub(1);
                }
                dialog_bundle::BundleLease index_bundle;
                std::wstring helper_executable;
                index::IndexClient client;
                bool index_started = false;
                unsigned generation = 0;
                uint64_t query_started = 0;
                uint32_t request = 0;
                std::wstring active_query;
                std::vector<ui::PickerJumpResult> known;
                picker_places::Snapshot tray_snapshot;
                picker_places::RefreshTraySnapshot(tray_snapshot, shared);
                for (;;) {
                    std::wstring query, accepted;
                    std::vector<std::wstring> forget;
                    unsigned wanted = 0;
                    {
                        std::unique_lock lock(state->mutex);
                        state->wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return state->cancelled.load() || !state->accepted.empty() || !state->forget.empty() || generation != state->query_generation; });
                        query = state->query; wanted = state->query_generation; accepted.swap(state->accepted); forget.swap(state->forget);
                    }
                    if (!forget.empty() && !history_file.empty()) {
                        const auto mutex_name = picker_places::detail::Name() + L".JumpHistory";
                        HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
                        const DWORD wait = mutex ? WaitForSingleObject(mutex, 1000) : WAIT_FAILED;
                        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
                            app::JumpHistory updated; updated.Open(history_file);
                            for (const auto& path : forget) updated.Remove(path);
                            ReleaseMutex(mutex);
                        }
                        if (mutex) CloseHandle(mutex);
                        std::lock_guard lock(state->mutex);
                        const auto before = state->recent.size();
                        std::erase_if(state->recent, [&](const auto& place) {
                            return std::any_of(forget.begin(), forget.end(), [&](const auto& path) { return ui::SamePickerPath(place.path, path); });
                        });
                        if (std::any_of(forget.begin(), forget.end(), [&](const auto& path) { return ui::SamePickerPath(state->last_location, path); }))
                            state->last_location.clear();
                        if (state->recent.size() != before) ++state->revision;
                    }
                    if (!accepted.empty() && !history_file.empty()) {
                        // Re-read while holding a cross-process lock to avoid lost visits.
                        const auto mutex_name = picker_places::detail::Name() + L".JumpHistory";
                        HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
                        const DWORD wait = mutex ? WaitForSingleObject(mutex, 1000) : WAIT_FAILED;
                        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
                            const auto latest = options.read_places ? options.read_places() : picker_places::Read();
                            if (latest.jump_policy_available && latest.remember) {
                                app::JumpHistory updated; updated.Open(history_file);
                                const auto directory = CanonicalFolder(mode == static_cast<int>(ui::PickerMode::Folder) ? accepted : Parent(accepted));
                                if (picker_places::detail::DiskPath(directory)) updated.Record(directory, {});
                            }
                            ReleaseMutex(mutex);
                        }
                        if (mutex) CloseHandle(mutex);
                    }
                    if (state->cancelled.load()) break;
                    // Refresh only memory-backed selection IPC on this worker; Poll never does IPC.
                    auto selection = options.read_places ? options.read_places() : picker_places::Snapshot{};
                    if (!options.read_places) { picker_places::ReadSelection(selection); picker_places::ReadTray(tray_snapshot); }
                    {
                        std::lock_guard lock(state->mutex);
                        if (picker_places::RefreshTraySnapshot(state->shared, options.read_places ? selection : tray_snapshot)) ++state->revision;
                        if (state->shared.selection_available != selection.selection_available ||
                            state->shared.selected_items != selection.selected_items ||
                            state->shared.selection_source != selection.selection_source) {
                            state->shared.selection_available = selection.selection_available;
                            state->shared.selection_source = selection.selection_source;
                            state->shared.selected_items = std::move(selection.selected_items);
                            ++state->revision;
                        }
                    }
                    if (wanted != generation) {
                        generation = wanted; active_query = query; known.clear(); query_started = GetTickCount64();
                        if (index_started) client.CancelSession(1);
                        ++request;
                        std::vector<app::JumpCandidate> candidates;
                        std::vector<int> groups;
                        for (const auto& entry : recent) { candidates.push_back({entry.path, entry.label}); groups.push_back(1); }
                        for (const auto& entry : shared.folders) if (entry.pinned) { candidates.push_back({entry.path, entry.label}); groups.push_back(2); }
                        for (size_t at : app::RankJumpFolders(candidates, query, shared.jump_policy_available && shared.pinyin, 24))
                            known.push_back({candidates[at].path, candidates[at].label, {}, groups[at], false});
                        {
                            std::lock_guard lock(state->mutex);
                            if (state->query_generation == generation) { state->results = known; state->completed_query = query; state->search_elapsed_ms = GetTickCount64() - query_started; state->search_complete = query.empty() || !shared.jump_policy_available || !shared.index; ++state->revision; }
                        }
                        if (!query.empty() && shared.jump_policy_available && shared.index) {
                            if (!index_started) {
                                helper_executable = ResolveJumpIndexHelper(OwnModulePath(), index_bundle);
                                client.Start(nullptr, 0, 0, index::kPipeName, helper_executable); index_started = true;
                            }
                            index::Query request_query; request_query.session_id = 1;
                            request_query.needle = shared.pinyin ? query : L"nopinyin: " + query;
                            request_query.folders_only = mode == static_cast<int>(ui::PickerMode::Folder);
                            request_query.limit = 24; client.SearchAsync(request_query, request);
                        }
                    }
                    const int status = index_started && client.Connected() && !client.ServiceMode() ? 1 : 0;
                    { std::lock_guard lock(state->mutex); if (state->index_status != status) { state->index_status = status; ++state->revision; } }
                    index::SearchResult indexed;
                    if (index_started && client.TakeResult(request, indexed)) {
                        auto results = known;
                        for (const auto& hit : indexed.hits) {
                            if (results.size() >= 48) break;
                            if (!picker_places::detail::DiskPath(hit.path)) continue;
                            results.push_back({hit.path, hit.name, {}, 3, !hit.is_dir});
                        }
                        std::lock_guard lock(state->mutex);
                        if (state->query_generation == generation) { state->results = std::move(results); state->completed_query = active_query; state->search_elapsed_ms = GetTickCount64() - query_started; state->search_complete = true; ++state->revision; }
                    }
                }
            } catch (...) { /* Optional context must never fail the caller's dialog. */ }
        }).detach();
    } catch (...) { active_workers.fetch_sub(1); }
}
JumpContext::~JumpContext() { state_->cancelled.store(true); state_->wake.notify_one(); }
bool JumpContext::Poll(ui::FolderPickerSpec& spec) {
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock) return false;
    state_->filter.filters = spec.filters; state_->filter.filter_index = spec.filter_index;
    if (!state_->history_loaded) {
        if (state_->name.empty() || spec.host_app_name == state_->name) return false;
        spec.host_app_name = state_->name;
        // Do not consume the first history revision: it owns the auto-jump decision.
        return true;
    }
    if (!state_->history_loaded || !state_->revision || (revision_ == state_->revision && filter_index_ == spec.filter_index)) return false;
    const bool first = revision_ == 0;
    revision_ = state_->revision; filter_index_ = spec.filter_index;
    spec.host_app_name = state_->name; spec.host_app_icon_pixels = state_->icon; spec.open_folders = state_->folders;
    spec.pulse_tray_available = state_->shared.tray_available;
    spec.pulse_tray_items = spec.pulse_tray_available ? state_->shared.tray_items : std::vector<picker_places::TrayItem>{};
    spec.pulse_tray_shelf_id = state_->shared.tray_shelf_id;
    spec.pulse_tray_shelf_name = state_->shared.tray_shelf_name;
    spec.pulse_tray_revision = state_->shared.tray_revision;
    spec.pulse_tray_total_count = state_->shared.tray_total_count;
    spec.pulse_tray_publisher_pid = state_->shared.tray_publisher_pid;
    spec.pulse_tray_source = state_->shared.tray_source;
    spec.pulse_tray_endpoint = state_->shared.endpoint;
    spec.pulse_tray_endpoint_pid = state_->shared.endpoint_pid;
    spec.pulse_selection_available = state_->shared.selection_available;
    spec.pulse_selected_items = state_->shared.selection_available ? state_->shared.selected_items : std::vector<picker_places::SelectedItem>{};
    spec.app_recent_places = state_->recent; spec.jump_index_status = state_->index_status;
    spec.jump_search_query = state_->query;
    spec.jump_search_elapsed_ms = state_->completed_query == state_->query ? state_->search_elapsed_ms : 0;
    spec.jump_search_complete = state_->completed_query == state_->query && state_->search_complete;
    if (!state_->last_location.empty()) {
        spec.last_location = {state_->last_location, {}, false};
        spec.last_location_source = AppCaption(state_->mode == ui::PickerMode::Save ?
            l10n::StringId::PickerAppLastSaveLocation : l10n::StringId::PickerAppLastLocation, state_->name);
    }
    if (first && spec.allow_auto_jump && state_->shared.jump_policy_available && state_->shared.auto_jump &&
        picker_places::detail::DiskPath(state_->shared.current) && _wcsicmp(spec.initial_path.c_str(), state_->shared.current.c_str())) {
        spec.auto_jump_from = spec.initial_path; spec.initial_path = state_->shared.current;
    }
    return true;
}
void JumpContext::Accepted(const std::wstring& path) {
    std::lock_guard lock(state_->mutex); state_->accepted = path; state_->wake.notify_one();
}
void JumpContext::Forget(const std::wstring& path) {
    if (path.empty()) return;
    std::lock_guard lock(state_->mutex); state_->forget.push_back(path); state_->wake.notify_one();
}
std::vector<ui::PickerJumpResult> JumpContext::Search(const std::wstring& query) {
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock || query.size() > 512) return {};
    if (state_->query != query) { state_->query = query; state_->completed_query.clear(); state_->results.clear(); state_->search_complete = false; state_->search_elapsed_ms = 0; ++state_->revision; ++state_->query_generation; state_->wake.notify_one(); }
    if (state_->completed_query != query) return {};
    return FilterJumpResults(state_->results, state_->mode, state_->filter);
}
std::vector<ui::PickerJumpResult> FilterJumpResults(std::vector<ui::PickerJumpResult> results,
    ui::PickerMode mode, const ui::PickerOptions& filter) {
    std::erase_if(results, [&](const auto& result) {
        return result.file && (mode == ui::PickerMode::Folder ||
            !ui::PickerMatchesFilter(result.path.substr(result.path.find_last_of(L"\\/") + 1), mode, filter));
    });
    return results;
}
std::shared_ptr<JumpContext> AttachJumpContext(ui::FolderPickerSpec& spec, DWORD process_id) {
    try {
        JumpContextOptions options;
#if defined(PULSE_PICKER_JUMP_UI_TEST_HOOKS)
        wchar_t test_root[32768]{};
        const auto length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", test_root, static_cast<DWORD>(std::size(test_root)));
        if (length && length < std::size(test_root)) {
            options.history_root.assign(test_root, length);
            options.enumerate_explorer = false;
        }
#endif
        auto context = std::make_shared<JumpContext>(process_id, static_cast<int>(spec.mode), std::move(options));
        spec.poll_jump_context = [context](ui::FolderPickerSpec& target) { return context->Poll(target); };
        spec.jump_search = [context](const std::wstring& query) { return context->Search(query); };
        spec.forget_recent_place = [context](const std::wstring& path) { context->Forget(path); };
        return context;
    } catch (...) { return {}; }
}
}
