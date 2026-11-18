#include "../common/app_runtime_paths.h"
// shell_tag_menu.cpp - File Explorer "Pulse tags >" submenu (see header).
#include "shell_tag_menu.h"
#include "shell_tag_registry.h"
#include "shell_tag_com.h"
#include "shell_tag_batch.h"
#include "shell_tag_actions.h"
#include "app_commands.h"
#include "single_instance_coordinator.h"
#include "app_internal.h"
#include "../common/localization.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace pulse {
bool ToggleTagForSelection(AppState& s, const app::TagId& tag_id,
                           const std::vector<std::wstring>& paths);
}



namespace pulse {
namespace {

constexpr ULONGLONG kHeadlessExitDelayMs = 1500; // let ADS writes drain
constexpr ULONGLONG kRegistrySyncPeriodMs = 2000;
struct ShellTagState {
    std::vector<app::shell_tags::Request> pending;
    app::ShellTagActions actions;
    bool headless = false;
    ULONGLONG headless_exit_at = 0;
    ULONGLONG next_sync = 0;
    size_t synced_signature = 0;
    bool synced_once = false;
};

ShellTagState& State() {
    static ShellTagState state;
    return state;
}

std::wstring ModulePath() {
    return app_runtime_paths::StableEntryPath();
}

std::wstring IconDirectory() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
        dir = std::wstring(base) + L"\\Pulse\\tagicons";
    CoTaskMemFree(base);
    return dir;
}

size_t Signature(const AppState& s) {
    size_t h = std::hash<std::wstring>{}(ModulePath());
    auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(std::hash<std::wstring>{}(l10n::Get(l10n::StringId::ShellTagMenu)));
    for (const auto& tag : s.places.tags) {
        mix(std::hash<std::wstring>{}(tag.id));
        mix(std::hash<std::wstring>{}(tag.name));
        mix(tag.rgb);
    }
    return h;
}

void SyncRegistry(AppState& s, ULONGLONG now) {
    ShellTagState& st = State();
    if (now < st.next_sync) return;
    st.next_sync = now + kRegistrySyncPeriodMs;
    if (!s.appPrefs.persist) return;
    if (!s.appPrefs.shell_tag_menu) {
        app::shell_tags::RevokeCommandServer();
        if ((!st.synced_once || st.synced_signature != 0) && app::RemoveTagMenu(IconDirectory())) {
            st.synced_signature = 0;
            st.synced_once = true;
        }
        return;
    }
    if (!s.isolatedTest && FAILED(app::shell_tags::RegisterCommandServer())) return;
    const size_t signature = Signature(s);
    const auto exe = ModulePath();
    const auto title = l10n::Get(l10n::StringId::ShellTagMenu);
    const auto dir = IconDirectory();
    if (st.synced_once && signature == st.synced_signature && app::TagMenuMatches(s.places.tags, exe, title, dir)) return;
    if (app::InstallTagMenu(s.places.tags, exe, title, dir)) {
        st.synced_signature = signature;
        st.synced_once = true;
    }
}

void ApplyPendingActions(AppState& s, ULONGLONG now) {
    auto& st = State();
    const auto pending = app::GroupShellTagActions(st.actions.Drain());
    if (pending.empty()) return;
    for (const auto& request : pending) {
        std::wstring name;
        const bool add = request.action == app::ShellTagAction::Add;
        std::vector<app::TagAdsUpdate> updates;
        if (!app::ApplyShellTagBatch(s.places, request, updates, name)) continue;
        QueueTagAds(s, std::move(updates));
        InvalidateRect(s.hwnd, nullptr, FALSE);
        wchar_t message[512]{};
        swprintf_s(message, l10n::Get(add ? l10n::StringId::ShellTagAdded :
            l10n::StringId::ShellTagRemoved).c_str(), name.c_str(), static_cast<int>(request.paths.size()));
        s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::ShellTagMenu), message, false);
    }
    if (st.headless) st.headless_exit_at = now + kHeadlessExitDelayMs;
}

void ApplyDueBatches(AppState& s, ULONGLONG now) {
    ShellTagState& st = State();
    for (auto& request : app::shell_tags::TakeCommandBatches()) st.pending.push_back(std::move(request));
    unsigned failed = app::shell_tags::TakeCommandFailures();
    if (st.pending.empty() && !failed) return;
    auto pending = std::move(st.pending);
    st.pending.clear();
    for (const auto& request : pending) {
        const auto& paths = request.paths;
        const app::TagId id = s.places.ResolveTagRef(request.tag);
        const app::ColorTag* tag = id.empty() ? nullptr : s.places.FindTag(id);
        if (!tag || !s.appPrefs.shell_tag_menu) { ++failed; continue; }
        const std::wstring name = tag->name;
        bool add = false, duplicate = false;
        if (app::shell_tags::ProcessBatchHandler().Handle(s.places, request,
            [&s](std::vector<app::TagAdsUpdate> updates) { QueueTagAds(s, std::move(updates)); }, now, &add, &duplicate)
            != app::shell_tags::Reply::Applied) { ++failed; continue; }
        if (duplicate) continue;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        wchar_t message[512]{};
        swprintf_s(message,
                   l10n::Get(add ? l10n::StringId::ShellTagAdded : l10n::StringId::ShellTagRemoved).c_str(),
                   name.c_str(), static_cast<int>(paths.size()));
        s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::ShellTagMenu), message, false);
    }
    if (failed) s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::ShellTagMenu),
        l10n::HantText(l10n::Get(l10n::StringId::TheCompleteSelectionCouldNotBeProcessedCheckTheTagsBeforeTry).c_str()), false);
    if (st.headless) st.headless_exit_at = now + kHeadlessExitDelayMs;
}

} // namespace

void QueueShellTagRequest(AppState& s, app::ShellTagRequest request, bool headless_launch) {
    auto& st = State();
    if (headless_launch) st.headless = true;
    if (st.actions.Push(std::move(request))) st.headless_exit_at = 0;
    (void)s;
}
void BeginShellTagComLaunch() {
    State().headless = true;
    State().headless_exit_at = GetTickCount64() + 30000;
}

bool ShellTagHeadlessLaunch() {
    return State().headless;
}

void TickShellTagMenu(AppState& s, ULONGLONG now) {
    ShellTagState& st = State();
    ApplyDueBatches(s, now);
    ApplyPendingActions(s, now);
    SyncRegistry(s, now);
    if (!st.headless) return;
    if (app::shell_tags::CommandServerBusy()) st.headless_exit_at = now + kHeadlessExitDelayMs;
    // The user opened the window meanwhile (Win+E, tray): stay running.
    if (s.hwnd && IsWindowVisible(s.hwnd)) {
        st.headless = false;
        return;
    }
    if (st.pending.empty() && st.actions.empty() && st.headless_exit_at && now >= st.headless_exit_at && s.hwnd) {
        // headless stays set so WM_DESTROY skips the session save.
        st.headless_exit_at = 0;
        DestroyWindow(s.hwnd);
    }
}

} // namespace pulse
