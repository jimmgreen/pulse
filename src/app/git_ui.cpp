#include "git_ui.h"

#include "app_internal.h"
#include "git_status.h"
#include "../common/localization.h"
#include "../ui/fluent_menu.h"
#include <algorithm>
#include <memory>
#include <thread>
#include <unordered_map>

namespace pulse {
namespace {

using ui::GitMark;

enum class GitJobKind { Status, Pull, Push };

struct GitJob {
    GitJobKind kind = GitJobKind::Status;
    std::wstring root;
    std::shared_ptr<app::GitRepoStatus> status;
    app::GitRunResult run;
    ULONGLONG ms = 0;
};

// Per repository, UI thread only.
struct GitRootState {
    bool status_running = false;
    bool status_again = false;          // asked while running: run once more after
    ULONGLONG last_done = 0, last_cost = 0;
    const void* seen_listing = nullptr;
    size_t seen_count = 0;
    GitJobKind op = GitJobKind::Status; // Status = no pull / push running
};

std::unordered_map<std::wstring, GitRootState>& Roots() {
    static std::unordered_map<std::wstring, GitRootState> roots;
    return roots;
}

GitRootState& RootState(const std::wstring& root) { return Roots()[app::GitLowerPath(root)]; }

void Post(HWND hwnd, std::unique_ptr<GitJob> job) {
    if (hwnd && PostMessageW(hwnd, WM_GIT_RESULT, 0, reinterpret_cast<LPARAM>(job.get()))) job.release();
}

void RequestStatus(AppState& s, const std::wstring& root) {
    GitRootState& state = RootState(root);
    if (state.status_running) {
        state.status_again = true;
        return;
    }
    state.status_running = true;
    std::thread([hwnd = s.hwnd, root] {
        auto job = std::make_unique<GitJob>();
        job->kind = GitJobKind::Status;
        job->root = root;
        const ULONGLONG start = GetTickCount64();
        // --no-optional-locks: never rewrite .git/index, which would wake the
        // folder watcher and ask again forever.
        job->run = app::RunGit(root,
            L"--no-optional-locks status --porcelain=v2 --branch -z --untracked-files=normal", 60000, false);
        if (job->run.started && !job->run.timed_out && job->run.exit_code == 0) {
            auto status = std::make_shared<app::GitRepoStatus>();
            status->root = root;
            if (app::ParseGitStatusV2(job->run.out, *status)) job->status = std::move(status);
        }
        job->run.out.clear();
        job->ms = GetTickCount64() - start;
        Post(hwnd, std::move(job));
    }).detach();
}

std::vector<std::wstring> Lines(const std::string& out) {
    const std::wstring text = app::GitUtf8ToWide(out);
    std::vector<std::wstring> kept;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        const size_t lead = line.find_first_not_of(L" \t");
        line = lead == std::wstring::npos ? std::wstring() : line.substr(lead);
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        if (!line.empty() && !line.starts_with(L"hint:")) kept.push_back(std::move(line));
        at = end + 1;
    }
    return kept;
}

std::wstring Clip(std::wstring text) {
    if (text.size() > 320) text = text.substr(0, 319) + L"\u2026";
    return text;
}

bool Has(const std::string& out, const char* needle) { return out.find(needle) != std::string::npos; }

// Why a pull / push failed: a plain explanation for the usual cases, else
// the lines git marked as the problem (rejected, fatal, error, conflict).
std::wstring FailureText(const std::string& out) {
    if (Has(out, "[rejected]") && (Has(out, "fetch first") || Has(out, "non-fast-forward")))
        return l10n::Get(l10n::StringId::GitPushRejected);
    if (Has(out, "divergent branches")) return l10n::Get(l10n::StringId::GitPullDivergent);
    const auto lines = Lines(out);
    std::vector<std::wstring> picked;
    for (const auto& line : lines) {
        if (line.starts_with(L"! ") || line.starts_with(L"fatal:") || line.starts_with(L"error:") ||
            line.starts_with(L"CONFLICT"))
            picked.push_back(line);
    }
    if (picked.empty()) picked = lines;
    if (picked.size() > 2) picked.erase(picked.begin(), picked.end() - 2);
    std::wstring joined;
    for (const auto& line : picked) {
        if (!joined.empty()) joined += L"\n";
        joined += line;
    }
    return Clip(std::move(joined));
}

// What a pull / push did, in one line: the "N files changed" summary or
// "Already up to date" rather than the last file git listed.
std::wstring SuccessText(const std::string& out) {
    const auto lines = Lines(out);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (it->find(L"changed") != std::wstring::npos || it->find(L"up to date") != std::wstring::npos ||
            it->find(L"->") != std::wstring::npos)
            return Clip(*it);
    }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (!it->starts_with(L"create mode") && !it->starts_with(L"delete mode") && !it->starts_with(L"mode change") &&
            !it->starts_with(L"rename ") && !it->starts_with(L"rewrite "))
            return Clip(*it);
    }
    return {};
}

std::wstring Format(l10n::StringId id, const std::wstring& arg) {
    wchar_t buffer[512]{};
    swprintf_s(buffer, l10n::Get(id).c_str(), arg.c_str());
    return buffer;
}

void StartRemoteOp(AppState& s, GitJobKind kind, const std::wstring& root) {
    GitRootState& state = RootState(root);
    if (state.op != GitJobKind::Status) {
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge, l10n::Get(l10n::StringId::GitBusy), L"", {});
        return;
    }
    state.op = kind;
    const auto status = app::GitStatusFor(root);
    const std::wstring branch = status ? status->branch : std::wstring();
    // The first push of a new branch sets its upstream, as most people mean it to.
    const bool set_upstream = kind == GitJobKind::Push && status && status->upstream.empty() && !branch.empty();
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge,
        l10n::Get(kind == GitJobKind::Pull ? l10n::StringId::GitPulling : l10n::StringId::GitPushing),
        status ? app::GitBranchLabel(*status) : std::wstring(), {}, {}, 120000);
    std::thread([hwnd = s.hwnd, root, kind, set_upstream, branch] {
        auto job = std::make_unique<GitJob>();
        job->kind = kind;
        job->root = root;
        const ULONGLONG start = GetTickCount64();
        std::wstring args = kind == GitJobKind::Pull ? L"pull" : L"push";
        if (set_upstream) {
            const auto remotes = app::RunGit(root, L"remote", 15000, false);
            if (remotes.started && remotes.exit_code == 0 &&
                (remotes.out == "origin\n" || remotes.out.starts_with("origin\n") ||
                 remotes.out.find("\norigin\n") != std::string::npos))
                args = L"push -u origin \"" + branch + L"\"";
        }
        job->run = app::RunGit(root, args, 5 * 60 * 1000, true);
        job->ms = GetTickCount64() - start;
        Post(hwnd, std::move(job));
    }).detach();
}

ui::FluentMenuItem Row(int command, l10n::StringId text, const wchar_t* glyph, bool enabled) {
    ui::FluentMenuItem item;
    item.command = command;
    item.text = l10n::Get(text);
    item.glyph = glyph;
    item.enabled = enabled;
    return item;
}

// The branch and how far it is from its upstream, as a quiet header.
ui::FluentMenuItem BranchRow(const std::wstring& root) {
    ui::FluentMenuItem head;
    const auto status = app::GitStatusFor(root);
    head.text = status ? app::GitBranchLabel(*status) : std::wstring(L"Git");
    head.glyph = L"\xE8B7";
    head.enabled = false;
    head.separator_after = true;
    return head;
}

// 拉取 / 推送 / 刷新, with how far the branch is from its upstream.
std::vector<ui::FluentMenuItem> GitRows(const std::wstring& root) {
    const auto status = app::GitStatusFor(root);
    const bool idle = RootState(root).op == GitJobKind::Status;
    std::vector<ui::FluentMenuItem> rows;
    rows.push_back(Row(kCmdGitPull, l10n::StringId::GitPull, L"\xE896", idle));
    if (status && status->behind > 0) rows.back().badge_text = L"\u2193" + std::to_wstring(status->behind);
    rows.push_back(Row(kCmdGitPush, l10n::StringId::GitPush, L"\xE898", idle));
    if (status && status->ahead > 0) rows.back().badge_text = L"\u2191" + std::to_wstring(status->ahead);
    rows.back().separator_after = true;
    rows.push_back(Row(kCmdGitRefresh, l10n::StringId::GitRefresh, L"\xE72C", true));
    return rows;
}

// Without the \\?\ (or \\?\UNC\) long-path prefix.
std::wstring PlainPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

}  // namespace

void FillGitPane(AppState& s, const app::Tab& tab, ui::PaneViewModel& pane) {
    pane.git_marks.reset();
    if (tab.git_root.empty() || tab.current_path.empty() || fs::IsVirtualPath(tab.current_path)) return;
    const std::wstring& root = tab.git_root;
    GitRootState& state = RootState(root);
    // Ask again when the listing changed (a file was saved, created, ...) or
    // the answer is old (a commit made in a terminal); slow repositories wait
    // longer between runs.
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG stale_after = (std::max<ULONGLONG>)(5000, state.last_cost * 10);
    const bool listing_changed = state.seen_listing != tab.snapshot.get() || state.seen_count != tab.EntryCount();
    if (!state.status_running && (state.last_done == 0 || listing_changed || now - state.last_done > stale_after)) {
        state.seen_listing = tab.snapshot.get();
        state.seen_count = tab.EntryCount();
        RequestStatus(s, root);
    }
    const auto status = app::GitStatusFor(root);
    if (!status || status->changed == 0 || !tab.snapshot) return;

    // Rows only change with the listing or the answer: keep the last few maps.
    struct Cached {
        std::weak_ptr<const std::vector<fs::DirEntry>> listing;
        size_t count = 0;
        const app::GitRepoStatus* status = nullptr;
        std::shared_ptr<const std::unordered_map<int, GitMark>> marks;
    };
    static std::vector<Cached> cache;
    for (const Cached& c : cache) {
        if (c.status == status.get() && c.count == tab.EntryCount() && !c.listing.expired() &&
            c.listing.lock() == tab.snapshot) {
            pane.git_marks = c.marks;
            return;
        }
    }
    // The folder relative to the repository; either side may carry \\?\.
    const std::wstring folder = app::GitLowerPath(PlainPath(tab.current_path));
    const std::wstring top = app::GitLowerPath(PlainPath(root));
    if (folder.size() < top.size() || folder.compare(0, top.size(), top) != 0 ||
        (folder.size() > top.size() && folder[top.size()] != L'\\'))
        return;
    std::wstring prefix = folder.substr(top.size());
    while (!prefix.empty() && prefix.front() == L'\\') prefix.erase(prefix.begin());
    if (!prefix.empty()) prefix += L'\\';
    auto marks = std::make_shared<std::unordered_map<int, GitMark>>();
    const size_t count = tab.EntryCount();
    std::wstring rel;
    for (size_t i = 0; i < count; ++i) {
        const fs::DirEntry entry = tab.EntryAt(i);
        if (entry.name == L".git") continue;
        rel = prefix;
        rel += app::GitLowerPath(entry.name);
        const GitMark mark = app::GitMarkFor(*status, rel, entry.is_dir);
        if (mark != GitMark::None) (*marks)[static_cast<int>(i)] = mark;
    }
    Cached fresh;
    fresh.listing = tab.snapshot;
    fresh.count = count;
    fresh.status = status.get();
    fresh.marks = std::move(marks);
    pane.git_marks = fresh.marks;
    // Holding `status` keeps the pointer it is keyed on from being reused.
    static std::vector<std::shared_ptr<const app::GitRepoStatus>> pinned;
    pinned.push_back(status);
    if (cache.size() >= 6) {
        cache.erase(cache.begin());
        pinned.erase(pinned.begin());
    }
    cache.push_back(std::move(fresh));
}

void AppendGitStatusText(const app::Tab& tab, std::wstring& text) {
    if (tab.git_root.empty()) return;
    const auto status = app::GitStatusFor(tab.git_root);
    if (!status) return;
    if (!text.empty()) text += L"  \u00B7  ";
    text += L"Git " + app::GitBadgeText(*status);
    if (status->changed > 0) {
        wchar_t changes[128]{};
        swprintf_s(changes, l10n::Get(l10n::StringId::GitChanges).c_str(), status->changed);
        text += L"  \u00B7  ";
        text += changes;
    }
}

void AppendGitMenu(const app::Tab& tab, std::vector<ui::FluentMenuItem>& items) {
    if (tab.git_root.empty()) return;
    ui::FluentMenuItem git;
    git.text = l10n::Get(l10n::StringId::Git);
    git.glyph = L"\xE8B7";
    git.children.push_back(BranchRow(tab.git_root));
    for (auto& row : GitRows(tab.git_root)) git.children.push_back(std::move(row));
    auto at = std::find_if(items.begin(), items.end(),
                           [](const ui::FluentMenuItem& item) { return item.command == app::CmdOpenTerminal; });
    if (at == items.end()) {
        if (!items.empty()) items.back().separator_after = true;
        items.push_back(std::move(git));
        return;
    }
    git.separator_after = at->separator_after;
    at->separator_after = false;
    items.insert(at + 1, std::move(git));
}

bool HandleGitCommand(AppState& s, int command, const std::wstring& root) {
    if (command != kCmdGitPull && command != kCmdGitPush && command != kCmdGitRefresh) return false;
    if (root.empty()) return true;
    if (command == kCmdGitRefresh) {
        GitRootState& state = RootState(root);
        state.last_done = 0;
        RequestStatus(s, root);
        return true;
    }
    StartRemoteOp(s, command == kCmdGitPull ? GitJobKind::Pull : GitJobKind::Push, root);
    return true;
}

bool ShowGitProjectMenu(AppState& s, const std::wstring& path, POINT screen_pt) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->git_root.empty() || path.empty() || _wcsicmp(path.c_str(), tab->git_root.c_str()) != 0)
        return false;
    if (s.places.IsStarred(path) || s.places.IsQuickAccessPinned(path)) return false;  // their own menus
    if (!EnsureMenu(s)) return true;
    const std::wstring root = tab->git_root;
    std::vector<ui::FluentMenuItem> items;
    items.push_back(BranchRow(root));
    for (auto& row : GitRows(root)) items.push_back(std::move(row));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    HandleGitCommand(s, command, root);
    return true;
}

void ReceiveGitResult(AppState& s, LPARAM lparam) {
    std::unique_ptr<GitJob> job(reinterpret_cast<GitJob*>(lparam));
    if (!job) return;
    GitRootState& state = RootState(job->root);
    if (job->kind == GitJobKind::Status) {
        state.status_running = false;
        state.last_done = GetTickCount64();
        state.last_cost = job->ms;
        if (job->status) app::StoreGitStatus(std::move(job->status));
        if (std::exchange(state.status_again, false)) RequestStatus(s, job->root);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    state.op = GitJobKind::Status;
    const bool pull = job->kind == GitJobKind::Pull;
    if (!job->run.started) {
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::GitNotFound),
                                       l10n::Get(l10n::StringId::GitNotFoundDetail));
    } else if (job->run.timed_out || job->run.exit_code != 0) {
        std::wstring detail = job->run.timed_out ? l10n::Get(l10n::StringId::GitTimedOut) : FailureText(job->run.out);
        s.notification_toast.ShowError(s.hwnd,
            l10n::Get(pull ? l10n::StringId::GitPullFailed : l10n::StringId::GitPushFailed), std::move(detail));
    } else {
        const auto status = app::GitStatusFor(job->root);
        const std::wstring branch = status ? (status->branch.empty() ? status->head : status->branch) : std::wstring();
        s.notification_toast.ReplaceNextSnack();
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge,
            Format(pull ? l10n::StringId::GitPullDone : l10n::StringId::GitPushDone, branch),
            SuccessText(job->run.out), {}, {}, 5000);
    }
    state.last_done = 0;
    RequestStatus(s, job->root);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

}  // namespace pulse
