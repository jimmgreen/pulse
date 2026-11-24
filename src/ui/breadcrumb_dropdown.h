#pragma once

#include "fluent_menu.h"
#include "ui_view_model.h"
#include "../app/breadcrumb_cache.h"
#include "../app/breadcrumb_revision.h"
#include "../common/localization.h"
#include "../fs/bounded_enumeration.h"
#include "../fs/drive_kind.h"
#include "../fs/fs_enum.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse::ui {

struct BreadcrumbAncestorMenu {
    std::vector<FluentMenuItem> items;
    std::vector<std::wstring> paths;
};

inline BreadcrumbAncestorMenu BuildBreadcrumbAncestors(
    const std::wstring& path,
    const std::function<bool(const std::wstring&)>& allow = nullptr) {
    BreadcrumbAncestorMenu result;
    const auto ancestors = SplitBreadcrumb(path);
    for (size_t i = 0; i + 1 < ancestors.size(); ++i) {
        if (allow && !allow(ancestors[i].path)) continue;
        FluentMenuItem item;
        item.command = static_cast<int>(result.paths.size()) + 1;
        item.text = ancestors[i].text;
        item.glyph = L"\xE8B7";
        result.items.push_back(std::move(item));
        result.paths.push_back(ancestors[i].path);
    }
    return result;
}

namespace detail {

inline std::shared_ptr<app::BreadcrumbCache> SharedBreadcrumbCache() {
    static auto cache = std::make_shared<app::BreadcrumbCache>();
    return cache;
}

struct BreadcrumbMenuTimer {
    FluentMenu* menu = nullptr;
    std::shared_ptr<app::BreadcrumbJob> job;
    bool refreshed = false;
};

inline std::unordered_map<UINT_PTR, BreadcrumbMenuTimer>& BreadcrumbTimers() {
    thread_local std::unordered_map<UINT_PTR, BreadcrumbMenuTimer> timers;
    return timers;
}

inline void CALLBACK RefreshBreadcrumbTimer(HWND, UINT, UINT_PTR id, DWORD) {
    auto& timers = BreadcrumbTimers();
    const auto it = timers.find(id);
    if (it == timers.end() || it->second.refreshed) return;
    std::lock_guard lock(it->second.job->mutex);
    if (it->second.job->ready) {
        it->second.refreshed = true;
        it->second.menu->RequestFilterRefresh();
    }
}

inline std::shared_ptr<app::BreadcrumbJob> StartBreadcrumbChildrenJob(
    const std::wstring& path, bool log_timing = false) {
    auto registry = SharedBreadcrumbCache();
    const auto acquisition = registry->Acquire(path);
    auto job = acquisition.job;
    if (!acquisition.start) return job;

    // Retired synchronous providers retain their slot until the API returns.
    // Reserve before creating the thread so repeatedly closing menus cannot
    // accumulate either providers or threads waiting for admission.
    auto* active = (fs::IsUncPath(path) || fs::IsRemoteLikeDrive(path))
        ? &fs::active_network_enumerations
        : &fs::active_local_enumerations;
    unsigned count = active->load();
    while (count < 4 && !active->compare_exchange_weak(count, count + 1)) {}
    if (count >= 4) {
        {
            std::lock_guard lock(job->mutex);
            job->failed = true;
            job->ready = true;
        }
        registry->Retire(path, job);
        return job;
    }

    try {
        std::thread([job, path, active, registry, log_timing] {
            struct Retirement {
                std::shared_ptr<app::BreadcrumbCache> registry;
                std::shared_ptr<app::BreadcrumbJob> job;
                ~Retirement() { registry->Retire(job->key, job); }
            } retirement{registry, job};
            struct Slot {
                std::atomic<unsigned>* active;
                ~Slot() { --*active; }
            } slot{active};
            const auto started = GetTickCount64();
            struct ThreadErrorMode {
                DWORD previous = 0;
                bool changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous) != FALSE;
                ~ThreadErrorMode() { if (changed) SetThreadErrorMode(previous, nullptr); }
            } error_mode;

            std::vector<std::pair<std::wstring, std::wstring>> folders;
            bool failed = false;
            uint64_t revision = 0;
            bool revision_valid = false;
            bool cache_hit = false;
            try {
                revision_valid = !path.empty() && app::ReadBreadcrumbRevision(fs::NormalizePath(path), revision);
                if (revision_valid && (cache_hit = registry->Load(path, revision, GetTickCount64(), folders))) {
                    // Revision validation stays on the admitted provider thread, including UNC/mapped drives.
                } else if (path.empty()) {
                    wchar_t drives[512]{};
                    const DWORD length = GetLogicalDriveStringsW(512, drives);
                    if (!length || length >= 512) throw std::runtime_error("drives");
                    for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1)
                        folders.emplace_back(drive, drive);
                } else {
                    auto prefix = fs::NormalizePath(path);
                    if (prefix.empty()) throw std::runtime_error("directory");
                    if (prefix.back() != L'\\') prefix += L'\\';
                    WIN32_FIND_DATAW data{};
                    const HANDLE handle = FindFirstFileExW((prefix + L"*").c_str(),
                        FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                    if (handle == INVALID_HANDLE_VALUE) {
                        if (GetLastError() != ERROR_FILE_NOT_FOUND) throw std::runtime_error("directory");
                    } else {
                        struct FindGuard {
                            HANDLE handle;
                            ~FindGuard() { FindClose(handle); }
                        } guard{handle};
                        do {
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
            } catch (...) {
                failed = true;
            }
            try {
                if (failed || !revision_valid) {
                    registry->Invalidate(path);
                } else if (!cache_hit) {
                    uint64_t after = 0;
                    if (app::ReadBreadcrumbRevision(fs::NormalizePath(path), after) && after == revision)
                        registry->Store(path, revision, GetTickCount64(), folders);
                    else
                        registry->Invalidate(path);
                }
            } catch (...) {
                failed = true;
                folders.clear();
            }
            std::lock_guard lock(job->mutex);
            job->folders = std::move(folders);
            job->failed = failed;
            job->ready = true;
            if (log_timing) std::printf("enumeration ms=%llu\n", GetTickCount64() - started);
        }).detach();
    } catch (...) {
        --*active;
        {
            std::lock_guard lock(job->mutex);
            job->failed = true;
            job->ready = true;
        }
        registry->Retire(path, job);
    }
    return job;
}

inline std::vector<FluentMenuItem> BuildBreadcrumbChildrenItems(
    const app::BreadcrumbJob& job,
    const std::wstring& query,
    const std::function<bool(const std::wstring&)>& allow = nullptr) {
    std::vector<FluentMenuItem> items;
    if (!job.ready || job.failed) {
        FluentMenuItem item;
        item.enabled = false;
        item.text = l10n::Get(!job.ready ? l10n::StringId::LoadingEllipsis
                                         : l10n::StringId::DirectoryUnavailable);
        items.push_back(std::move(item));
        return items;
    }

    bool any_allowed = false;
    for (size_t i = 0; i < job.folders.size(); ++i) {
        if (allow && !allow(job.folders[i].second)) continue;
        any_allowed = true;
        const auto& name = job.folders[i].first;
        if (!query.empty() && std::search(name.begin(), name.end(), query.begin(), query.end(),
            [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); }) == name.end()) continue;
        FluentMenuItem item;
        item.command = static_cast<int>(i + 1);
        item.text = name;
        items.push_back(std::move(item));
    }
    if (!any_allowed) {
        FluentMenuItem item;
        item.enabled = false;
        item.text = l10n::Get(l10n::StringId::EmptyFolder);
        items.push_back(std::move(item));
    }
    return items;
}

} // namespace detail

template<class TrackFn>
inline std::wstring TrackBreadcrumbChildrenMenu(
    FluentMenu& menu,
    const std::wstring& path,
    POINT screen_pt,
    TrackFn&& track,
    std::function<bool(const std::wstring&)> allow = nullptr,
    bool log_timing = false) {
    auto job = detail::StartBreadcrumbChildrenJob(path, log_timing);
    FluentMenu::FilterFn filter = [job, allow = std::move(allow)](const std::wstring& query) {
        std::lock_guard lock(job->mutex);
        return detail::BuildBreadcrumbChildrenItems(*job, query, allow);
    };

    const UINT_PTR timer = SetTimer(nullptr, 0, 30, detail::RefreshBreadcrumbTimer);
    if (!timer) return {};
    detail::BreadcrumbTimers().emplace(timer, detail::BreadcrumbMenuTimer{&menu, job, false});
    struct TimerGuard {
        UINT_PTR id;
        ~TimerGuard() {
            KillTimer(nullptr, id);
            detail::BreadcrumbTimers().erase(id);
        }
    } timer_guard{timer};

    menu.SetFilterPlaceholder(L"");
    menu.SetFilterMinWidth(180.0f);
    menu.SetMaxVisibleRows(10);
    const int command = track(FluentMenu::CardOrigin(screen_pt), filter(L""), filter);

    std::wstring selected;
    {
        std::lock_guard lock(job->mutex);
        if (command > 0 && static_cast<size_t>(command) <= job->folders.size())
            selected = job->folders[static_cast<size_t>(command - 1)].second;
    }
    return selected;
}

} // namespace pulse::ui
