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
    common::MenuIconRef icon;
    std::vector<ui::PickerOpenFolder> folders;
    std::vector<ui::PickerRecentPlace> recent;
    std::vector<ui::PickerRecentFile> files;
    int index_status = 0;
    bool history_loaded = false;
    bool pulse_files = false, search_complete = false;
    uint64_t search_elapsed_ms = 0;
    std::vector<ui::PickerJumpResult> results;
    ui::PickerOptions filter;
    ui::PickerMode mode = ui::PickerMode::Folder;
    picker_places::Snapshot shared;
    ~State() { for (const auto& file : files) if (file.thumbnail) DeleteObject(file.thumbnail); }
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
struct RecentSource { std::wstring path; int64_t used = 0; bool pulse = false; };
std::vector<RecentSource> PulseRecentFiles(const JumpContextOptions& options) {
    std::wstring path = options.pulse_places_file;
    if (path.empty()) {
        // Isolated history tests never read the user's real recent files.
        if (!options.history_root.empty()) return {};
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local))) return {};
        path = std::wstring(local) + L"\\Pulse\\places.json"; CoTaskMemFree(local);
    }
    std::wstring text;
    if (!ReadUtf8File(path, text) || !json::ValidConfigObject(text, true)) return {};
    std::vector<RecentSource> files;
    json::ForEachElement(json::ExtractArray(text, L"recent_items"), [&](const std::wstring& raw) {
        if (files.size() >= 100 || json::ExtractString(raw, L"kind") != L"file") return;
        const auto file = json::ExtractString(raw, L"path");
        if (!picker_places::detail::DiskPath(file)) return;
        const auto value = json::ExtractString(raw, L"opened_at");
        const auto ticks = std::wcstoull(value.c_str(), nullptr, 10);
        const auto seconds = ticks >= 116444736000000000ULL ? (ticks - 116444736000000000ULL) / 10000000ULL : 0;
        if (seconds > 253402300799ULL) return;
        files.push_back({file, static_cast<int64_t>(seconds), true});
    });
    return files;
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
    if (active_workers.fetch_add(1) >= 2) { active_workers.fetch_sub(1); return; }
    try {
        std::thread([state = state_, process_id, mode, options = std::move(options)] {
            struct WorkerSlot { ~WorkerSlot() { active_workers.fetch_sub(1); } } slot;
            try {
                const auto executable = Executable(process_id);
                const auto name = Description(executable);
                const auto shared = options.read_places ? options.read_places() : picker_places::Read();
                std::vector<ui::PickerOpenFolder> folders;
                for (const auto& entry : shared.folders) if (entry.open) Add(folders, {entry.path, entry.label, false});
                const bool remember = shared.jump_policy_available && shared.remember;
                const auto history_file = remember ? HistoryFile(executable, mode, options.history_root) : std::wstring{};
                app::JumpHistory history;
                if (!history_file.empty()) history.Open(history_file);
                std::vector<ui::PickerRecentFile> files;
                const bool file_mode = mode == static_cast<int>(ui::PickerMode::File) || mode == static_cast<int>(ui::PickerMode::Image);
                bool has_pulse_files = false;
                if (!history_file.empty() && file_mode) {
                    app::JumpHistory file_history; file_history.Open(history_file + L".files");
                    auto ordered = PulseRecentFiles(options);
                    for (const auto& file : file_history.Top({}, app::JumpHistory::kMaxEntries))
                        ordered.push_back({file, file_history.LastUsed(file), false});
                    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.used > b.used; });
                    for (const auto& entry : ordered) {
                        if (state->cancelled.load()) break;
                        const auto& file = entry.path;
                        if (std::any_of(files.begin(), files.end(), [&](const auto& old) { return _wcsicmp(old.path.c_str(), file.c_str()) == 0; })) continue;
                        WIN32_FILE_ATTRIBUTE_DATA info{};
                        if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info) ||
                            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                        const auto date = VisitDate(entry.used);
                        ui::PickerRecentFile card{file, Parent(file) + (date.empty() ? L"" : L" · " + date), nullptr};
                        card.attributes = info.dwFileAttributes;
                        card.modified = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
                        card.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
                        files.push_back(std::move(card));
                        has_pulse_files = has_pulse_files || entry.pulse;
                    }
                }
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
                    state->name = name; state->folders = folders; state->shared = shared; state->recent = recent; state->files = files;
                    state->pulse_files = has_pulse_files;
                    state->last_location = std::move(last_location); state->history_loaded = true; ++state->revision;
                }
                // A separate STA isolates hung Explorer providers from search/history.
                if (options.enumerate_explorer || !files.empty()) {
                if (active_shell_workers.fetch_add(1) < 2) {
                try { std::thread([state, executable, files, enumerate_explorer = options.enumerate_explorer] {
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
                            const std::wstring key = executable + L"|" + std::to_wstring(edge);
                            bool cached = false;
                            { std::lock_guard lock(icon_mutex); if (const auto it = icon_cache.find(key); it != icon_cache.end()) { icon = it->second; cached = true; } }
                            if (!cached) {
                                icon = common::LoadIconPixels(executable + L",0", static_cast<uint32_t>(edge));
                                std::lock_guard lock(icon_mutex);
                                if (icon_cache.size() >= 16) icon_cache.clear();
                                icon_cache.emplace(key, icon);
                            }
                        }
                        { std::lock_guard lock(state->mutex); state->icon = std::move(icon); ++state->revision; }
                        if (SUCCEEDED(com) && !state->cancelled.load()) {
                            for (size_t i = 0; i < std::min<size_t>(files.size(), 32) && !state->cancelled.load(); ++i) {
                                ComPtr<IShellItemImageFactory> factory;
                                if (FAILED(SHCreateItemFromParsingName(files[i].path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) continue;
                                HBITMAP bitmap = nullptr;
                                HRESULT hr = factory->GetImage({96, 72}, SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY, &bitmap);
                                if (bitmap) {
                                    std::lock_guard lock(state->mutex);
                                    if (SUCCEEDED(hr) && !state->cancelled.load()) { state->files[i].thumbnail = bitmap; ++state->revision; }
                                    else DeleteObject(bitmap);
                                }
                            }
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
                for (;;) {
                    std::wstring query, accepted;
                    unsigned wanted = 0;
                    {
                        std::unique_lock lock(state->mutex);
                        state->wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return state->cancelled.load() || !state->accepted.empty() || generation != state->query_generation; });
                        query = state->query; wanted = state->query_generation; accepted.swap(state->accepted);
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
                                const auto directory = mode == static_cast<int>(ui::PickerMode::Folder) ? accepted : Parent(accepted);
                                if (picker_places::detail::DiskPath(directory)) updated.Record(directory, {});
                                if (file_mode && picker_places::detail::DiskPath(accepted)) {
                                    const DWORD attributes = GetFileAttributesW(accepted.c_str());
                                    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                                        app::JumpHistory file_history; file_history.Open(history_file + L".files"); file_history.Record(accepted, {});
                                    }
                                }
                            }
                            ReleaseMutex(mutex);
                        }
                        if (mutex) CloseHandle(mutex);
                    }
                    if (state->cancelled.load()) break;
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
    if (!state_->history_loaded || !state_->revision || (revision_ == state_->revision && filter_index_ == spec.filter_index)) return false;
    const bool first = revision_ == 0;
    revision_ = state_->revision; filter_index_ = spec.filter_index;
    spec.host_app_name = state_->name; spec.host_app_icon_pixels = state_->icon; spec.open_folders = state_->folders;
    spec.app_recent_places = state_->recent; spec.jump_index_status = state_->index_status;
    spec.jump_search_query = state_->query;
    spec.jump_search_elapsed_ms = state_->completed_query == state_->query ? state_->search_elapsed_ms : 0;
    spec.jump_search_complete = state_->completed_query == state_->query && state_->search_complete;
    ui::PickerOptions filter; filter.filters = spec.filters; filter.filter_index = spec.filter_index;
    spec.recent_files.clear();
    for (const auto& file : state_->files) {
        if (spec.recent_files.size() < 8 && ui::PickerMatchesFilter(file.path.substr(file.path.find_last_of(L"\\/") + 1), spec.mode, filter)) spec.recent_files.push_back(file);
    }
    if (!state_->last_location.empty()) {
        spec.last_location = {state_->last_location, {}, false};
        spec.last_location_source = AppCaption(l10n::StringId::PickerAppLastLocation, state_->name);
    }
    if (!spec.recent_files.empty())
        spec.recent_files_caption = AppCaption(state_->pulse_files ? l10n::StringId::PickerRecentFilesFromAppAndPulse : l10n::StringId::PickerRecentFilesFromApp, state_->name);
    if (first && spec.allow_auto_jump && state_->shared.jump_policy_available && state_->shared.auto_jump &&
        picker_places::detail::DiskPath(state_->shared.current) && _wcsicmp(spec.initial_path.c_str(), state_->shared.current.c_str())) {
        spec.auto_jump_from = spec.initial_path; spec.initial_path = state_->shared.current;
    }
    return true;
}
void JumpContext::Accepted(const std::wstring& path) {
    std::lock_guard lock(state_->mutex); state_->accepted = path; state_->wake.notify_one();
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
        auto context = std::make_shared<JumpContext>(process_id, static_cast<int>(spec.mode));
        spec.poll_jump_context = [context](ui::FolderPickerSpec& target) { return context->Poll(target); };
        spec.jump_search = [context](const std::wstring& query) { return context->Search(query); };
        return context;
    } catch (...) { return {}; }
}
}
