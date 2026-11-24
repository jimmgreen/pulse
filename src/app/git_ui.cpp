#include "git_ui.h"

#include "app_internal.h"
#include "git_branches.h"
#include "git_changes.h"
#include "git_conflict.h"
#include "git_history.h"
#include "git_row_lookup.h"
#include "git_status.h"
#include "git_terms.h"
#include "../common/localization.h"
#include "../ui/confirm_dialog.h"
#include "../ui/fluent_menu.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace pulse {
namespace {

using Sid = l10n::StringId;
using ui::GitMark;

enum class GitJobKind {
    Status, Pull, Push, Sync, Fetch,
    SwitchBranch, CheckoutRemote, CreateBranch, MergeBranch,
    StashApply, StashDrop,
    FileSummary, QuickStage, QuickUnstage, QuickDiscard, QuickRestore
};

struct GitJob {
    GitJobKind kind = GitJobKind::Status;
    std::wstring root;
    std::wstring arg1;
    std::wstring arg2;
    std::shared_ptr<app::GitRepoStatus> status;
    app::GitRunResult run;
    app::GitRemoteOpResult remote_op;
    app::GitOpResult op;
    app::GitFileSummary summary;
    bool has_head = true;
    uint64_t summary_rev = 0;
    ULONGLONG ms = 0;
};

// Per repository, UI thread only.
struct GitRootState {
    bool status_running = false;
    bool status_again = false;          // asked while running: run once more after
    ULONGLONG last_done = 0, last_cost = 0;
    const void* seen_listing = nullptr;
    size_t seen_count = 0;
    GitJobKind op = GitJobKind::Status; // Status = no mutating op running
};

struct SummaryCacheEntry {
    uint64_t rev = 0;
    app::GitFileSummary summary;
    bool has_head = true;
};

std::unordered_map<std::wstring, GitRootState>& Roots() {
    static std::unordered_map<std::wstring, GitRootState> roots;
    return roots;
}

GitRootState& RootState(const std::wstring& root) { return Roots()[app::GitLowerPath(root)]; }

std::unordered_map<std::wstring, SummaryCacheEntry>& SummaryCache() {
    static std::unordered_map<std::wstring, SummaryCacheEntry> cache;
    return cache;
}

std::unordered_set<std::wstring>& InFlightSummaries() {
    static std::unordered_set<std::wstring> set;
    return set;
}

std::wstring SummaryKey(const std::wstring& root, const std::wstring& rel_slash) {
    return app::GitLowerPath(app::GitRowPlainPath(root)) + L"|" + app::GitLowerPath(rel_slash);
}

void ClearSummaryCacheForRepo(const std::wstring& root) {
    const std::wstring prefix = app::GitLowerPath(app::GitRowPlainPath(root)) + L"|";
    auto& cache = SummaryCache();
    for (auto it = cache.begin(); it != cache.end();) {
        if (it->first.starts_with(prefix)) it = cache.erase(it);
        else ++it;
    }
    auto& inf = InFlightSummaries();
    for (auto it = inf.begin(); it != inf.end();) {
        if (it->starts_with(prefix)) it = inf.erase(it);
        else ++it;
    }
}

bool ExtractRelativeGitPath(const std::wstring& repo_root, const std::wstring& item_path,
                            std::wstring& out_rel_lower_back, std::wstring& out_rel_slash) {
    out_rel_lower_back.clear();
    out_rel_slash.clear();
    if (repo_root.empty() || item_path.empty()) return false;
    const std::wstring root_plain = app::GitRowPlainPath(repo_root);
    const std::wstring full_plain = app::GitRowPlainPath(item_path);
    const std::wstring root_low = app::GitLowerPath(root_plain);
    const std::wstring full_low = app::GitLowerPath(full_plain);
    if (full_low.size() <= root_low.size() || full_low.compare(0, root_low.size(), root_low) != 0 ||
        full_low[root_low.size()] != L'\\') {
        return false;
    }
    out_rel_lower_back = full_low.substr(root_low.size() + 1);
    if (out_rel_lower_back == L".git" || out_rel_lower_back.starts_with(L".git\\")) return false;

    if (full_plain.size() > root_plain.size()) {
        out_rel_slash = full_plain.substr(root_plain.size());
        while (!out_rel_slash.empty() && (out_rel_slash.front() == L'\\' || out_rel_slash.front() == L'/')) {
            out_rel_slash.erase(out_rel_slash.begin());
        }
    } else {
        out_rel_slash = out_rel_lower_back;
    }
    std::replace(out_rel_slash.begin(), out_rel_slash.end(), L'\\', L'/');
    return !out_rel_slash.empty();
}

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

std::wstring Format(l10n::StringId id, const std::wstring& arg) {
    wchar_t buffer[512]{};
    swprintf_s(buffer, l10n::Get(id).c_str(), arg.c_str());
    return buffer;
}

bool BeginBusyOp(AppState& s, GitRootState& state, GitJobKind kind) {
    if (state.op != GitJobKind::Status) {
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge, l10n::Get(Sid::GitBusy), L"", {});
        return false;
    }
    state.op = kind;
    return true;
}

void StartRemoteOp(AppState& s, GitJobKind kind, const std::wstring& root) {
    GitRootState& state = RootState(root);
    if (!BeginBusyOp(s, state, kind)) return;
    const auto status = app::GitStatusFor(root);
    const std::wstring branch = status ? status->branch : std::wstring();
    const bool set_upstream = status && status->upstream.empty() && !branch.empty();
    const Sid snack_title = kind == GitJobKind::Pull ? Sid::GitPulling
                          : kind == GitJobKind::Push ? Sid::GitPushing
                          : Sid::GitSyncRunning;
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge,
        l10n::Get(snack_title),
        status ? app::GitBranchLabel(*status) : std::wstring(), {}, {}, 120000);
    std::thread([hwnd = s.hwnd, root, kind, set_upstream, branch] {
        auto job = std::make_unique<GitJob>();
        job->kind = kind;
        job->root = root;
        const ULONGLONG start = GetTickCount64();
        switch (kind) {
        case GitJobKind::Pull:
            job->remote_op = app::GitPull(root);
            break;
        case GitJobKind::Push:
            job->remote_op = app::GitPush(root, set_upstream, branch);
            break;
        case GitJobKind::Sync:
            job->remote_op = app::GitSync(root, branch, set_upstream);
            break;
        case GitJobKind::Fetch:
            job->remote_op = app::GitFetch(root);
            break;
        default:
            break;
        }
        job->ms = GetTickCount64() - start;
        Post(hwnd, std::move(job));
    }).detach();
}

void StartBranchOrStashOp(AppState& s, GitJobKind kind, const std::wstring& root,
                          const std::wstring& arg1, const std::wstring& arg2, bool flag1) {
    GitRootState& state = RootState(root);
    if (!BeginBusyOp(s, state, kind)) return;
    std::thread([hwnd = s.hwnd, root, kind, arg1, arg2, flag1] {
        auto job = std::make_unique<GitJob>();
        job->kind = kind;
        job->root = root;
        job->arg1 = arg1;
        job->arg2 = arg2;
        const ULONGLONG start = GetTickCount64();
        switch (kind) {
        case GitJobKind::SwitchBranch:
            job->op = app::GitSwitchBranch(root, arg1, flag1);
            break;
        case GitJobKind::CheckoutRemote:
            job->op = app::GitCheckoutRemoteBranch(root, arg1);
            break;
        case GitJobKind::CreateBranch:
            job->op = app::GitCreateBranch(root, arg1, arg2);
            break;
        case GitJobKind::MergeBranch:
            job->remote_op = app::GitMergeBranch(root, arg1);
            job->op.ok = job->remote_op.ok;
            job->op.git_missing = job->remote_op.git_missing;
            job->op.message = job->remote_op.message;
            break;
        case GitJobKind::StashApply:
            job->remote_op = app::GitStashApply(root, arg1);
            job->op.ok = job->remote_op.ok;
            job->op.git_missing = job->remote_op.git_missing;
            job->op.message = job->remote_op.message;
            break;
        case GitJobKind::StashDrop:
            job->op = app::GitStashDrop(root, arg1);
            break;
        case GitJobKind::QuickStage:
            job->op = app::GitStagePaths(root, {arg1});
            break;
        case GitJobKind::QuickUnstage:
            job->op = app::GitUnstagePaths(root, {arg1}, flag1);
            break;
        case GitJobKind::QuickDiscard:
            if (flag1) job->op = app::GitRecycleUntracked(root, {arg1});
            else job->op = app::GitDiscardPaths(root, {arg1});
            break;
        case GitJobKind::QuickRestore:
            job->op = app::GitRestoreFileFromCommit(root, arg1, arg2, true);
            break;
        default:
            break;
        }
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
    head.glyph = L"\xF003";
    head.enabled = false;
    head.separator_after = true;
    return head;
}

std::vector<ui::FluentMenuItem> GitRows(const AppState& s, const std::wstring& root) {
    const auto status = app::GitStatusFor(root);
    const bool idle = RootState(root).op == GitJobKind::Status;
    const bool plain = s.appPrefs.git_plain_terms;
    std::vector<ui::FluentMenuItem> rows;
    rows.push_back(Row(kCmdGitSourceControl, plain ? Sid::GitCenterPlain : Sid::GitScMenu, L"\xE7A2", true));
    if (status && status->changed > 0) rows.back().badge_text = std::to_wstring(status->changed);
    rows.push_back(Row(kCmdGitHistory, plain ? Sid::GitHistoryPlain : Sid::GitHistoryGit, L"\xE81C", true));
    rows.push_back(Row(kCmdGitBranches, Sid::GitScBranch, L"\xF003", true));
    rows.back().separator_after = true;
    rows.push_back(Row(kCmdGitSync, Sid::GitSyncLabel, L"\xE895", idle));
    rows.push_back(Row(kCmdGitPull, plain ? Sid::GitFetchUpdatesPlain : Sid::GitPull, L"\xE896", idle));
    if (status && status->behind > 0) rows.back().badge_text = L"\u2193" + std::to_wstring(status->behind);
    rows.push_back(Row(kCmdGitPush, plain ? Sid::GitUploadPlain : Sid::GitPush, L"\xE898", idle));
    if (status && status->ahead > 0) rows.back().badge_text = L"\u2191" + std::to_wstring(status->ahead);
    rows.back().separator_after = true;
    rows.push_back(Row(kCmdGitRefresh, Sid::GitRefresh, L"\xE72C", true));
    return rows;
}

void OpenBranchPopup(AppState& s, const std::wstring& root, POINT screen_pt, bool open_above) {
    if (root.empty()) return;
    if (!s.gitBranchPopup) {
        s.gitBranchPopup = std::make_unique<ui::GitBranchPopup>();
    }
    ui::GitBranchPopupCallbacks cb;
    cb.on_sync = [&s, root] { StartRemoteOp(s, GitJobKind::Sync, root); };
    cb.on_pull = [&s, root] { StartRemoteOp(s, GitJobKind::Pull, root); };
    cb.on_push = [&s, root] { StartRemoteOp(s, GitJobKind::Push, root); };
    cb.on_fetch = [&s, root] { StartRemoteOp(s, GitJobKind::Fetch, root); };
    cb.on_switch_branch = [&s, root](const std::wstring& branch, int dirty) {
        bool auto_stash = false;
        if (dirty > 0) {
            ui::ConfirmDialogSpec spec;
            spec.title = app::GitFmt(l10n::Get(Sid::GitSwitchDirtyTitle), L"{branch}", branch);
            spec.message = app::GitFmtCount(l10n::Get(Sid::GitSwitchDirtyMsg), dirty);
            spec.confirm_text = l10n::Get(Sid::GitSwitchStashOpt);
            spec.secondary_text = l10n::Get(Sid::GitSwitchCarryOpt);
            spec.cancel_text = l10n::Get(Sid::Cancel);
            spec.tone = ui::ConfirmTone::Question;
            spec.default_choice = ui::ConfirmChoice::Confirm;
            spec.note = L"git stash push -u && git switch " + branch;
            const ui::ConfirmChoice ch = ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor);
            if (ch == ui::ConfirmChoice::Cancel) return;
            auto_stash = (ch == ui::ConfirmChoice::Confirm);
        }
        StartBranchOrStashOp(s, GitJobKind::SwitchBranch, root, branch, {}, auto_stash);
    };
    cb.on_checkout_remote = [&s, root](const std::wstring& remote_ref) {
        StartBranchOrStashOp(s, GitJobKind::CheckoutRemote, root, remote_ref, {}, false);
    };
    cb.on_create_branch = [&s, root](const std::wstring& new_name) {
        StartBranchOrStashOp(s, GitJobKind::CreateBranch, root, new_name, {}, false);
    };
    cb.on_merge_branch = [&s, root](const std::wstring& branch) {
        ui::ConfirmDialogSpec spec;
        spec.title = app::GitFmt(l10n::Get(Sid::GitMergeConfirmTitle), L"{branch}", branch);
        spec.message = l10n::Get(Sid::GitMergeConfirmMsg);
        spec.confirm_text = l10n::Get(Sid::GitMergeBtn);
        spec.cancel_text = l10n::Get(Sid::Cancel);
        spec.tone = ui::ConfirmTone::Question;
        spec.note = L"git merge " + branch;
        if (ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) != ui::ConfirmChoice::Confirm) return;
        StartBranchOrStashOp(s, GitJobKind::MergeBranch, root, branch, {}, false);
    };
    cb.on_stash_apply = [&s, root](const std::wstring& ref) {
        StartBranchOrStashOp(s, GitJobKind::StashApply, root, ref, {}, false);
    };
    cb.on_stash_drop = [&s, root](const std::wstring& ref, const std::wstring& msg) {
        ui::ConfirmDialogSpec spec;
        spec.title = app::GitFmt(l10n::Get(Sid::GitStashDropTitle), L"{name}", msg.empty() ? ref : msg);
        spec.message = l10n::Get(Sid::GitStashDropMsg);
        spec.confirm_text = l10n::Get(Sid::GitStashDropBtn);
        spec.cancel_text = l10n::Get(Sid::Cancel);
        spec.danger = true;
        spec.tone = ui::ConfirmTone::Danger;
        spec.default_choice = ui::ConfirmChoice::Cancel;
        spec.note = L"git stash drop " + ref;
        if (ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) != ui::ConfirmChoice::Confirm) return;
        StartBranchOrStashOp(s, GitJobKind::StashDrop, root, ref, {}, false);
    };
    cb.on_open_history = [&s, root] {
        OpenGitSourceControlTab(s, root, 1, {});
    };
    s.gitBranchPopup->Show(s.hwnd, root, screen_pt, open_above, s.darkMode, s.accentColor,
                          s.appPrefs.git_plain_terms, std::move(cb));
}

}  // namespace

void FillGitPane(AppState& s, const app::Tab& tab, ui::PaneViewModel& pane) {
    pane.git_marks.reset();
    pane.git_mark_lookup = {};
    pane.git_dir_count_lookup = {};
    pane.git_plain_terms = s.appPrefs.git_plain_terms;
    if (tab.git_root.empty() || tab.current_path.empty() || fs::IsVirtualPath(tab.current_path)) return;
    const std::wstring& root = tab.git_root;
    GitRootState& state = RootState(root);
    // Ask again when the listing changed or the answer is old; slow repos wait longer.
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

    pane.git_mark_lookup = app::MakeGitRowLookup(pane.snapshot, status, tab.current_path, root);
    pane.git_dir_count_lookup = app::MakeGitDirCountLookup(pane.snapshot, status, tab.current_path, root);
}

void AppendGitStatusText(const AppState& s, const app::Tab& tab, std::wstring& text) {
    if (tab.git_root.empty()) return;
    const auto status = app::GitStatusFor(tab.git_root);
    if (!status) return;
    if (!text.empty()) text += L"  \u00B7  ";
    text += L"Git " + app::GitBadgeText(*status);
    if (status->changed > 0) {
        wchar_t changes[128]{};
        const Sid fmt_id = s.appPrefs.git_plain_terms ? Sid::GitChangesShortPlain : Sid::GitChanges;
        swprintf_s(changes, l10n::Get(fmt_id).c_str(), status->changed);
        text += L"  \u00B7  ";
        text += changes;
    }
}

void AppendGitMenu(const AppState& s, const app::Tab& tab, std::vector<ui::FluentMenuItem>& items) {
    if (tab.git_root.empty()) return;
    ui::FluentMenuItem git;
    git.text = l10n::Get(Sid::Git);
    git.glyph = L"\xF003";
    git.children.push_back(BranchRow(tab.git_root));
    for (auto& row : GitRows(s, tab.git_root)) git.children.push_back(std::move(row));
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
    if (command != kCmdGitPull && command != kCmdGitPush && command != kCmdGitRefresh &&
        command != kCmdGitSourceControl && command != kCmdGitSync &&
        command != kCmdGitBranches && command != kCmdGitHistory) {
        return false;
    }
    if (root.empty()) return true;
    if (command == kCmdGitSourceControl) {
        OpenGitSourceControl(s, root);
        return true;
    }
    if (command == kCmdGitHistory) {
        OpenGitSourceControlTab(s, root, 1, s.detailsSelPath);
        return true;
    }
    if (command == kCmdGitBranches) {
        POINT pt{};
        GetCursorPos(&pt);
        OpenBranchPopup(s, root, pt, false);
        return true;
    }
    if (command == kCmdGitRefresh) {
        GitRootState& state = RootState(root);
        state.last_done = 0;
        ClearSummaryCacheForRepo(root);
        RequestStatus(s, root);
        return true;
    }
    if (command == kCmdGitSync) {
        StartRemoteOp(s, GitJobKind::Sync, root);
        return true;
    }
    StartRemoteOp(s, command == kCmdGitPull ? GitJobKind::Pull : GitJobKind::Push, root);
    return true;
}

void OpenGitSourceControl(AppState& s, const std::wstring& root) {
    OpenGitSourceControlTab(s, root, 0, {});
}

void OpenGitSourceControlTab(AppState& s, const std::wstring& root, int tab_index,
                             const std::wstring& focus_full_or_rel_path) {
    if (root.empty()) return;
    if (!s.gitWindow) {
        s.gitWindow = std::make_unique<ui::GitWindow>();
        s.gitWindow->SetOnChanged([&s](const std::wstring& changed) {
            HandleGitCommand(s, kCmdGitRefresh, changed);
        });
        s.gitWindow->SetOnTermsChanged([&s](bool plain) {
            s.appPrefs.git_plain_terms = plain;
            s.appPrefs.SaveAsync();
            InvalidateRect(s.hwnd, nullptr, FALSE);
        });
        s.gitWindow->SetOnRevealFile([&s](const std::wstring& full_path) {
            const std::wstring normalized = fs::NormalizePath(full_path);
            if (normalized.empty()) return;
            std::filesystem::path p(normalized);
            const std::wstring parent = p.parent_path().wstring();
            const std::wstring name = p.filename().wstring();
            if (!parent.empty()) NavigateTo(s, parent);
            if (auto* tab = ActiveTab(s); tab && !name.empty()) SelectNameInTab(s, *tab, name);
            if (s.hwnd && IsWindow(s.hwnd)) SetForegroundWindow(s.hwnd);
        });
    }

    std::wstring rel_focus = focus_full_or_rel_path;
    std::wstring unused_lower, rel_slash;
    if (ExtractRelativeGitPath(root, focus_full_or_rel_path, unused_lower, rel_slash)) {
        rel_focus = rel_slash;
    }

    const ui::GitViewTab initial_tab = tab_index == 1 ? ui::GitViewTab::History
                                     : tab_index == 2 ? ui::GitViewTab::Conflict
                                     : ui::GitViewTab::Changes;
    s.gitWindow->Show(s.hwnd, root, s.darkMode, s.accentColor,
                      s.appPrefs.git_plain_terms, initial_tab, rel_focus);
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
    for (auto& row : GitRows(s, root)) items.push_back(std::move(row));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    HandleGitCommand(s, command, root);
    return true;
}

void FillGitDetails(AppState& s, const app::Tab& tab, ui::DetailsPanelView& dv) {
    if (tab.git_root.empty() || dv.path.empty()) return;
    const auto status = app::GitStatusFor(tab.git_root);
    if (!status) return;
    std::wstring rel_lower, rel_slash;
    if (!ExtractRelativeGitPath(tab.git_root, dv.path, rel_lower, rel_slash)) return;

    const bool plain = s.appPrefs.git_plain_terms;
    dv.git_plain_terms = plain;
    dv.git_root_path = tab.git_root;
    dv.git_rel_path = rel_slash;
    dv.git_branch_text = app::GitBranchLabel(*status);
    dv.git_mark = app::GitMarkFor(*status, rel_lower, dv.is_dir);
    if (dv.git_mark == GitMark::None) {
        dv.git_state_text = l10n::Get(Sid::GitScUnchanged);
    } else if (dv.is_dir) {
        dv.git_state_text = ui::GitDirBadgeText(dv.git_mark, app::GitDirChangeCount(*status, rel_lower), plain);
    } else {
        dv.git_state_text = ui::GitMarkLabel(dv.git_mark, plain);
    }

    if (dv.is_dir || fs::IsVirtualPath(dv.path)) return;

    const std::wstring key = SummaryKey(tab.git_root, rel_slash);
    const uint64_t rev = app::GitStatusRevision();
    auto& cache = SummaryCache();
    const auto it = cache.find(key);
    if (it != cache.end() && it->second.rev == rev) {
        const auto& sum = it->second.summary;
        dv.git_summary_ready = sum.valid;
        dv.git_has_head = it->second.has_head;
        dv.git_staged = sum.staged;
        dv.git_unstaged = sum.unstaged;
        dv.git_untracked = sum.untracked;
        dv.git_added = sum.added;
        dv.git_deleted = sum.deleted;
        dv.git_preview_lines.clear();
        for (size_t i = 0; i < sum.preview_lines.size() && i < 6; ++i) {
            const auto& ln = sum.preview_lines[i];
            dv.git_preview_lines.push_back({ln.sign, ln.line_no, ln.text});
        }
        dv.git_history.clear();
        for (size_t i = 0; i < sum.history.size() && i < 3; ++i) {
            const auto& h = sum.history[i];
            dv.git_history.push_back({h.hash, h.subject, h.author, h.time_text});
        }
        return;
    }

    auto& in_flight = InFlightSummaries();
    if (in_flight.contains(key)) return;
    if (cache.size() > 64) cache.clear();
    in_flight.insert(key);

    const std::wstring root = tab.git_root;
    const HWND hwnd = s.hwnd;
    std::thread([hwnd, root, rel_slash, rev] {
        auto job = std::make_unique<GitJob>();
        job->kind = GitJobKind::FileSummary;
        job->root = root;
        job->arg1 = rel_slash;
        job->summary_rev = rev;
        job->summary = app::GitReadFileSummary(root, rel_slash);
        job->has_head = !app::GitHeadId(root).empty();
        Post(hwnd, std::move(job));
    }).detach();
}

void GitDetailsToggleStage(AppState& s, const std::wstring& root, const std::wstring& item_full_path) {
    std::wstring rel_lower, rel_slash;
    if (!ExtractRelativeGitPath(root, item_full_path, rel_lower, rel_slash)) return;
    const std::wstring key = SummaryKey(root, rel_slash);
    const auto it = SummaryCache().find(key);
    bool currently_staged = false;
    bool has_head = true;
    if (it != SummaryCache().end() && it->second.summary.valid) {
        currently_staged = it->second.summary.staged && !it->second.summary.unstaged;
        has_head = it->second.has_head;
    }
    StartBranchOrStashOp(s, currently_staged ? GitJobKind::QuickUnstage : GitJobKind::QuickStage,
                         root, rel_slash, {}, has_head);
}

void GitDetailsDiscard(AppState& s, const std::wstring& root, const std::wstring& item_full_path) {
    std::wstring rel_lower, rel_slash;
    if (!ExtractRelativeGitPath(root, item_full_path, rel_lower, rel_slash)) return;
    const std::wstring key = SummaryKey(root, rel_slash);
    const auto it = SummaryCache().find(key);
    bool untracked = false;
    if (it != SummaryCache().end() && it->second.summary.valid) {
        untracked = it->second.summary.untracked;
    } else if (const auto st = app::GitStatusFor(root)) {
        const auto fit = st->files.find(rel_lower);
        if (fit != st->files.end() && fit->second == GitMark::Added) untracked = true;
    }
    const bool plain = s.appPrefs.git_plain_terms;
    ui::ConfirmDialogSpec spec;
    const size_t slash = rel_slash.find_last_of(L'/');
    const std::wstring name = slash == std::wstring::npos ? rel_slash : rel_slash.substr(slash + 1);
    spec.title = app::GitFmt(l10n::Get(Sid::GitScDiscardTitleOne), L"{name}", name);
    spec.message = l10n::Get(untracked ? Sid::GitScDiscardUntracked : Sid::GitScDiscardMsg);
    spec.confirm_text = app::GitStr(plain, untracked ? Sid::GitDeleteUntrackedPlain : Sid::GitDiscardPlain,
                                    untracked ? Sid::GitDeleteUntrackedGit : Sid::GitScDiscard);
    spec.cancel_text = l10n::Get(Sid::Cancel);
    spec.danger = true;
    spec.tone = ui::ConfirmTone::Danger;
    spec.default_choice = ui::ConfirmChoice::Cancel;
    spec.note = app::GitFmt(l10n::Get(Sid::GitScDiscardCmd), L"{cmd}",
                            untracked ? l10n::Get(Sid::GitScRecycleCmd) : (L"git checkout -- " + rel_slash));
    if (ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) != ui::ConfirmChoice::Confirm) return;
    StartBranchOrStashOp(s, GitJobKind::QuickDiscard, root, rel_slash, {}, untracked);
}

void GitDetailsRestoreCommit(AppState& s, const std::wstring& root, const std::wstring& item_full_path, int history_index) {
    std::wstring rel_lower, rel_slash;
    if (!ExtractRelativeGitPath(root, item_full_path, rel_lower, rel_slash)) return;
    const std::wstring key = SummaryKey(root, rel_slash);
    const auto it = SummaryCache().find(key);
    if (it == SummaryCache().end() || history_index < 0 ||
        history_index >= static_cast<int>(it->second.summary.history.size())) {
        return;
    }
    const auto& entry = it->second.summary.history[static_cast<size_t>(history_index)];
    const bool plain = s.appPrefs.git_plain_terms;
    const size_t slash = rel_slash.find_last_of(L'/');
    const std::wstring name = slash == std::wstring::npos ? rel_slash : rel_slash.substr(slash + 1);

    ui::ConfirmDialogSpec spec;
    spec.title = app::GitFmt(app::GitFmt(l10n::Get(Sid::GitHistRestoreConfirmTitle), L"{name}", name),
                             L"{hash}", entry.hash.substr(0, 7));
    spec.message = l10n::Get(Sid::GitHistRestoreConfirmMsg);
    spec.confirm_text = app::GitStr(plain, Sid::GitHistRestoreFilePlain, Sid::GitHistRestoreFileGit);
    spec.cancel_text = l10n::Get(Sid::Cancel);
    spec.tone = ui::ConfirmTone::Info;
    spec.note = app::GitFmt(l10n::Get(Sid::GitCmdDetailsSummary), L"{cmd}",
                            L"git checkout " + entry.hash.substr(0, 7) + L" -- " + rel_slash);
    if (ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) != ui::ConfirmChoice::Confirm) return;
    StartBranchOrStashOp(s, GitJobKind::QuickRestore, root, entry.hash, rel_slash, true);
}

void ShowGitStatusMenu(AppState& s, POINT screen_pt) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->git_root.empty()) return;
    OpenBranchPopup(s, tab->git_root, screen_pt, true);
}

void AppendGitToolbarPins(AppState& s, const app::Tab& tab, std::vector<ui::SelectionChipAction>& pins, size_t cap) {
    if (tab.git_root.empty()) return;
    const auto status = app::GitStatusFor(tab.git_root);
    const bool idle = RootState(tab.git_root).op == GitJobKind::Status;
    const bool plain = s.appPrefs.git_plain_terms;
    const auto add = [&](int command, const wchar_t* glyph, std::wstring label, bool enabled, int badge_count) {
        if (pins.size() >= cap) return;
        ui::SelectionChipAction pin;
        pin.command = command;
        pin.glyph = glyph;
        pin.label = std::move(label);
        pin.enabled = enabled;
        pin.situational = true;
        pin.badge_count = badge_count;
        pins.push_back(std::move(pin));
    };

    // 1. Branches popover button
    std::wstring br_label = status ? app::GitBranchLabel(*status) : l10n::Get(Sid::GitScBranch);
    br_label += L"  \u00B7  ";
    br_label += app::GitStr(plain, Sid::GitBranchMenuTipPlain, Sid::GitBranchMenuTipGit);
    add(kCmdGitBranches, L"\xF003", std::move(br_label), true, 0);

    // 2. One-click Sync (with behind+ahead badge) when an upstream exists
    if (status && !status->upstream.empty()) {
        std::wstring sync_label = app::GitStr(plain, Sid::GitSyncTipPlain, Sid::GitSyncTipGit);
        if (status->behind > 0) sync_label += L"  \u2193" + std::to_wstring(status->behind);
        if (status->ahead > 0) sync_label += L"  \u2191" + std::to_wstring(status->ahead);
        add(kCmdGitSync, L"\xE895", std::move(sync_label), idle, status->behind + status->ahead);
    }

    // 3. Version Center / Source Control (with changed-count badge)
    std::wstring sc_label;
    if (status && status->changed > 0) {
        wchar_t buf[128]{};
        swprintf_s(buf, l10n::Get(plain ? Sid::GitSaveVersionDotCount : Sid::GitScMenuDotCount).c_str(),
                   status->changed);
        sc_label = buf;
    } else {
        sc_label = app::GitStr(plain, Sid::GitCenterPlain, Sid::GitScMenu);
    }
    add(kCmdGitSourceControl, L"\xE7A2", std::move(sc_label), true, status ? status->changed : 0);
}

void ReceiveGitResult(AppState& s, LPARAM lparam) {
    std::unique_ptr<GitJob> job(reinterpret_cast<GitJob*>(lparam));
    if (!job) return;
    GitRootState& state = RootState(job->root);

    if (job->kind == GitJobKind::FileSummary) {
        const std::wstring key = SummaryKey(job->root, job->arg1);
        InFlightSummaries().erase(key);
        SummaryCache()[key] = {job->summary_rev, std::move(job->summary), job->has_head};
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }

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
    ClearSummaryCacheForRepo(job->root);

    const bool is_remote_kind = job->kind == GitJobKind::Pull || job->kind == GitJobKind::Push ||
                                job->kind == GitJobKind::Sync || job->kind == GitJobKind::Fetch ||
                                job->kind == GitJobKind::MergeBranch || job->kind == GitJobKind::StashApply;
    const bool git_missing = is_remote_kind ? job->remote_op.git_missing : job->op.git_missing;
    const bool ok = is_remote_kind ? job->remote_op.ok : job->op.ok;
    const std::wstring& err_msg = is_remote_kind ? job->remote_op.message : job->op.message;

    if (git_missing) {
        s.notification_toast.ShowError(s.hwnd, l10n::Get(Sid::GitNotFound),
                                       l10n::Get(Sid::GitNotFoundDetail));
    } else if (is_remote_kind && job->remote_op.has_conflicts) {
        s.notification_toast.ReplaceNextSnack();
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge,
            l10n::Get(Sid::GitMergeConflictToast), job->remote_op.summary, {}, {}, 6000);
        OpenGitSourceControlTab(s, job->root, 2, {});
    } else if (!ok) {
        Sid fail_title = Sid::GitSyncFailed;
        if (job->kind == GitJobKind::Pull) fail_title = Sid::GitPullFailed;
        else if (job->kind == GitJobKind::Push) fail_title = Sid::GitPushFailed;
        s.notification_toast.ShowError(s.hwnd, l10n::Get(fail_title), err_msg);
    } else {
        const auto status = app::GitStatusFor(job->root);
        const std::wstring branch = status ? (status->branch.empty() ? status->head : status->branch) : std::wstring();
        std::wstring title = l10n::Get(Sid::GitSyncDone);
        std::wstring detail = is_remote_kind ? job->remote_op.summary : job->op.message;
        if (job->kind == GitJobKind::Pull) title = Format(Sid::GitPullDone, branch);
        else if (job->kind == GitJobKind::Push) title = Format(Sid::GitPushDone, branch);
        else if (job->kind == GitJobKind::MergeBranch) title = l10n::Get(Sid::GitMergeDone);
        else if (job->kind == GitJobKind::SwitchBranch || job->kind == GitJobKind::CheckoutRemote ||
                 job->kind == GitJobKind::CreateBranch) {
            title = job->arg1;
        }
        if (job->kind != GitJobKind::QuickStage && job->kind != GitJobKind::QuickUnstage) {
            s.notification_toast.ReplaceNextSnack();
            s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge, title, detail, {}, {}, 4000);
        }
    }

    if (s.gitWindow && s.gitWindow->visible() && _wcsicmp(s.gitWindow->root().c_str(), job->root.c_str()) == 0) {
        s.gitWindow->RefreshAsync();
    }
    state.last_done = 0;
    RequestStatus(s, job->root);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

}  // namespace pulse
