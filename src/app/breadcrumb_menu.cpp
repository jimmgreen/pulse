#include "app_internal.h"
#include "context_menu.h"
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <cstdio>
#include <unordered_map>
#include "../fs/fs_enum.h"
#include "../fs/device_path.h"
#include "../ops/clipboard.h"
#include "../common/localization.h"

namespace pulse {

namespace {

constexpr int kHideCloud = 32400;

// Runs a BuildBreadcrumbMenu command on `path` (a breadcrumb or a sidebar row).
void RunPathCommand(AppState& s, const std::wstring& path, bool filesystem, int command) {
    const std::wstring shell_path = ClipboardPath(path);
    switch (command) {
    case app::CmdOpenInNewTab:
        OpenFolderTab(s, path);
        break;
    case app::CmdOpen:
        NavigateTo(s, path);
        break;
    case app::CmdCopyPath:
        // A phone folder: the path Explorer's address bar shows, not Pulse's.
        ops::WriteClipboardText(fs::IsDevicePath(path) ? DeviceFriendlyPath(path) : shell_path);
        break;
    case app::CmdCopy:
        if (filesystem) ops::WriteClipboard({shell_path}, false);
        break;
    case app::CmdOpenTerminal:
        if (filesystem) s.ops.OpenTerminal(shell_path);
        break;
    case app::CmdProperties:
        if (filesystem) s.ops.ShowProperties(shell_path);
        break;
    default:
        break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace

void ShowBreadcrumbMenu(AppState& s, std::wstring path, POINT screen_pt) {
    if (path.empty() || !EnsureMenu(s)) return;
    s.context_menu.Close();
    const bool filesystem = !fs::IsVirtualPath(path);
    // Keep the clicked path across TrackPopup's nested message loop. Selection
    // commands target file rows and must not be used for breadcrumb ancestors.
    const int command = s.menu->TrackPopup(screen_pt, app::BuildBreadcrumbMenu(filesystem));
    RunPathCommand(s, path, filesystem, command);
}

namespace {
struct BreadcrumbJob {
    std::atomic_bool cancelled{false};
    std::mutex mutex;
    bool ready = false;
    bool failed = false;
    std::vector<std::pair<std::wstring, std::wstring>> folders;
};
struct BreadcrumbTimer { ui::FluentMenu* menu; std::shared_ptr<BreadcrumbJob> job; bool refreshed = false; };
thread_local std::unordered_map<UINT_PTR, BreadcrumbTimer> breadcrumb_timers;
void CALLBACK RefreshBreadcrumb(HWND, UINT, UINT_PTR id, DWORD) {
    const auto it = breadcrumb_timers.find(id);
    if (it == breadcrumb_timers.end() || it->second.refreshed) return;
    std::lock_guard lock(it->second.job->mutex);
    if (it->second.job->ready) {
        it->second.refreshed = true;
        it->second.menu->RequestFilterRefresh();
    }
}
}

void ShowBreadcrumbChildren(AppState& s, std::wstring path, POINT screen_pt) {
    const auto started = GetTickCount64();
    if (!EnsureMenu(s)) return;
    if (s.isolatedTest) std::printf("ensure menu ms=%llu\n", GetTickCount64() - started);
    s.context_menu.Close();
    auto job = std::make_shared<BreadcrumbJob>();
    std::thread([job, path, timing = s.isolatedTest] {
        const auto started = GetTickCount64();
        struct ThreadErrorMode {
            DWORD previous = 0;
            bool changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous) != FALSE;
            ~ThreadErrorMode() { if (changed) SetThreadErrorMode(previous, nullptr); }
        } error_mode;
        std::vector<std::pair<std::wstring, std::wstring>> folders;
        bool failed = false;
        try {
            if (path.empty()) {
                wchar_t drives[512]{};
                const DWORD length = GetLogicalDriveStringsW(512, drives);
                if (!length || length >= 512) throw std::runtime_error("drives");
                for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1)
                    folders.emplace_back(drive, drive);
            } else {
                auto prefix = fs::NormalizePath(path);
                if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
                WIN32_FIND_DATAW data{};
                const HANDLE handle = FindFirstFileExW((prefix + L"*").c_str(),
                    FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (handle == INVALID_HANDLE_VALUE) {
                    if (GetLastError() != ERROR_FILE_NOT_FOUND) throw std::runtime_error("directory");
                } else {
                    struct FindGuard { HANDLE handle; ~FindGuard() { FindClose(handle); } } guard{handle};
                    do {
                        if (job->cancelled) return;
                        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                            wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
                        auto child = path;
                        if (!child.empty() && child.back() != L'\\') child += L'\\';
                        child += data.cFileName;
                        folders.emplace_back(data.cFileName, std::move(child));
                    } while (FindNextFileW(handle, &data));
                    if (GetLastError() != ERROR_NO_MORE_FILES) throw std::runtime_error("directory");
                }
                std::sort(folders.begin(), folders.end(), [](const auto& a, const auto& b) {
                    return CompareStringOrdinal(a.first.c_str(), -1, b.first.c_str(), -1, TRUE) == CSTR_LESS_THAN;
                });
            }
        } catch (...) { failed = true; }
        if (job->cancelled) return;
        std::lock_guard lock(job->mutex);
        job->folders = std::move(folders);
        job->failed = failed;
        job->ready = true;
        if (timing) std::printf("enumeration ms=%llu\n", GetTickCount64() - started);
    }).detach();
    auto filter = [job](const std::wstring& query) {
        std::lock_guard lock(job->mutex);
        std::vector<ui::FluentMenuItem> items;
        if (!job->ready || job->failed || job->folders.empty()) {
            ui::FluentMenuItem item;
            item.enabled = false;
            item.text = l10n::Get(!job->ready ? l10n::StringId::LoadingEllipsis :
                job->failed ? l10n::StringId::DirectoryUnavailable : l10n::StringId::EmptyFolder);
            items.push_back(std::move(item));
        } else {
            for (size_t i = 0; i < job->folders.size(); ++i) {
                const auto& name = job->folders[i].first;
                if (!query.empty() && std::search(name.begin(), name.end(), query.begin(), query.end(),
                    [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); }) == name.end()) continue;
                ui::FluentMenuItem item;
                item.command = static_cast<int>(i + 1);
                item.text = job->folders[i].first;
                items.push_back(std::move(item));
            }
        }
        return items;
    };
    const UINT_PTR timer = SetTimer(nullptr, 0, 30, RefreshBreadcrumb);
    if (!timer) { job->cancelled = true; return; }
    breadcrumb_timers.emplace(timer, BreadcrumbTimer{s.menu.get(), job});
    s.menu->SetFilterMinWidth(180.0f);
    s.menu->SetMaxVisibleRows(10);
    const int command = s.menu->TrackPopup(ui::FluentMenu::CardOrigin(screen_pt), filter(L""), filter);
    KillTimer(nullptr, timer);
    breadcrumb_timers.erase(timer);
    job->cancelled = true;
    std::wstring selected;
    {
        std::lock_guard lock(job->mutex);
        if (command > 0 && static_cast<size_t>(command) <= job->folders.size())
            selected = job->folders[command - 1].second;
    }
    if (!selected.empty()) NavigateTo(s, selected);
}

// #80: cloud rows (OneDrive, WPS网盘, ...) form a header-less section, so the
// row carries what a header menu would: the usual folder commands, then hiding
// the section. The toast
// says where to bring it back.
void ShowCloudPlaceMenu(AppState& s, const std::wstring& path, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    s.context_menu.Close();
    const bool filesystem = !path.empty() && !fs::IsVirtualPath(path);
    std::vector<ui::FluentMenuItem> items;
    if (!path.empty()) items = app::BuildBreadcrumbMenu(filesystem);
    if (!items.empty()) items.back().separator_after = true;
    ui::FluentMenuItem hide;
    hide.command = kHideCloud;
    hide.text = l10n::Get(l10n::StringId::SidebarHideCloud);
    items.push_back(std::move(hide));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == kHideCloud) {
        s.sidebarHiddenMask |= 1u << static_cast<int>(app::SidebarSectionId::Cloud);
        s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::SidebarCloudDrives),
            l10n::Get(l10n::StringId::SidebarCloudHiddenHint), false);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (!path.empty()) RunPathCommand(s, path, filesystem, command);
}

} // namespace pulse
