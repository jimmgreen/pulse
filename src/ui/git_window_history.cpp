#include "git_window.h"

#include "../app/git_terms.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "confirm_dialog.h"
#include "window_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pulse::ui {
namespace {

using Sid = pulse::l10n::StringId;
using app::GitFmt;
using app::GitFmtCount;
using app::GitStr;

std::wstring Str(Sid id) { return std::wstring(pulse::l10n::Get(id)); }

bool Inside(const D2D1_RECT_F& rc, float x, float y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

bool HighContrastOn() {
    HIGHCONTRASTW value{sizeof(value)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
           (value.dwFlags & HCF_HIGHCONTRASTON);
}

std::wstring FileNameOf(const std::wstring& rel) {
    const size_t slash = rel.find_last_of(L"\\/");
    return slash == std::wstring::npos ? rel : rel.substr(slash + 1);
}

std::wstring FormatBytesCompact(uint64_t bytes) {
    wchar_t buf[64]{};
    if (bytes >= 1024ull * 1024ull) {
        swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        swprintf_s(buf, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

bool ReadWorktreeRaw(const std::wstring& full_path, std::string& out, uint64_t max_bytes) {
    out.clear();
    const HANDLE f = CreateFileW(full_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 0 || static_cast<uint64_t>(sz.QuadPart) > max_bytes) {
        CloseHandle(f);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(out.size() - done, 1u << 20));
        DWORD got = 0;
        if (!ReadFile(f, out.data() + done, chunk, &got, nullptr) || got == 0) break;
        done += got;
    }
    CloseHandle(f);
    out.resize(done);
    return true;
}

std::wstring AuthorInitials(const std::wstring& name) {
    std::wstring out;
    bool next_word = true;
    for (wchar_t c : name) {
        if (iswspace(c)) {
            next_word = true;
            continue;
        }
        if (next_word) {
            out.push_back(static_cast<wchar_t>(towupper(c)));
            next_word = false;
            if (out.size() >= 2) break;
        }
    }
    if (out.empty() && !name.empty()) out.push_back(name.front());
    return out;
}

struct HistColors {
    D2D1_COLOR_F panel;
    D2D1_COLOR_F body;
    D2D1_COLOR_F card;
    D2D1_COLOR_F branch_purple;
    D2D1_COLOR_F remote_green;
    D2D1_COLOR_F warn_amber;
    D2D1_COLOR_F del_fg;
    D2D1_COLOR_F add_bg;
    D2D1_COLOR_F del_bg;
    D2D1_COLOR_F hunk_bg;
};

HistColors MakeHistColors(bool dark) {
    HistColors h{};
    if (dark) {
        h.panel = D2D1::ColorF(0x202227);
        h.body = D2D1::ColorF(0x1B1D22);
        h.card = D2D1::ColorF(0x24272E);
        h.branch_purple = D2D1::ColorF(0xA78BFA);
        h.remote_green = D2D1::ColorF(0x3DD68C);
        h.warn_amber = D2D1::ColorF(0xF5B941);
        h.del_fg = D2D1::ColorF(0xFF6369);
        h.add_bg = D2D1::ColorF(0x3DD68C, 0.13f);
        h.del_bg = D2D1::ColorF(0xFF6369, 0.13f);
        h.hunk_bg = D2D1::ColorF(0x3B6CF6, 0.12f);
    } else {
        h.panel = D2D1::ColorF(0xF5F6F9);
        h.body = D2D1::ColorF(0xFFFFFF);
        h.card = D2D1::ColorF(0xF8F9FB);
        h.branch_purple = D2D1::ColorF(0x8B5CF6);
        h.remote_green = D2D1::ColorF(0x1F9D55);
        h.warn_amber = D2D1::ColorF(0xC77D02);
        h.del_fg = D2D1::ColorF(0xE5484D);
        h.add_bg = D2D1::ColorF(0x1F9D55, 0.10f);
        h.del_bg = D2D1::ColorF(0xE5484D, 0.09f);
        h.hunk_bg = D2D1::ColorF(0x3B6CF6, 0.07f);
    }
    return h;
}

D2D1_COLOR_F LaneColor(int lane, bool is_remote, const Theme& theme, const HistColors& hc) {
    if (is_remote) return hc.remote_green;
    if (lane <= 0) return theme.accent;
    if (lane == 1) return hc.branch_purple;
    if (lane == 2) return hc.warn_amber;
    return hc.remote_green;
}

std::wstring FormatHunkHdr(int old_start, int old_count, int new_start, int new_count, bool plain) {
    if (plain) {
        const int line = new_start > 0 ? new_start : old_start;
        return GitFmt(Str(Sid::GitHunkNearLine), L"{line}", std::to_wstring(line));
    }
    return L"@@ \u2212" + std::to_wstring(old_start) + L"," + std::to_wstring(old_count) + L" +" +
           std::to_wstring(new_start) + L"," + std::to_wstring(new_count) + L" @@";
}

}  // namespace

std::wstring GitWindow::HistQueryText() const {
    if (!hist_search_edit_) return {};
    const int n = GetWindowTextLengthW(hist_search_edit_);
    std::wstring text(static_cast<size_t>(n), L'\0');
    if (n > 0) GetWindowTextW(hist_search_edit_, text.data(), n + 1);
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return text;
}

std::wstring GitWindow::HistBranchInputText() const {
    if (!hist_branch_edit_) return {};
    const int n = GetWindowTextLengthW(hist_branch_edit_);
    std::wstring text(static_cast<size_t>(n), L'\0');
    if (n > 0) GetWindowTextW(hist_branch_edit_, text.data(), n + 1);
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return text;
}

void GitWindow::StopHistoryLoad() {
    ++hist_generation_;
    ++hist_detail_gen_;
    ++hist_diff_gen_;
    if (hist_thread_.joinable()) hist_thread_.join();
    if (hist_detail_thread_.joinable()) hist_detail_thread_.join();
    if (hist_diff_thread_.joinable()) hist_diff_thread_.join();
    std::lock_guard<std::mutex> lock(hist_mutex_);
    hist_pending_.reset();
    hist_detail_pending_.reset();
    hist_diff_pending_.reset();
}

void GitWindow::StartHistoryLoad() {
    if (!hwnd_ || root_.empty()) return;
    if (hist_thread_.joinable()) hist_thread_.join();
    const uint64_t gen = ++hist_generation_;
    hist_loading_ = true;

    app::GitHistoryFilter filter;
    filter.scope = hist_scope_;
    filter.rel_path = !hist_scope_rel_path_.empty() ? hist_scope_rel_path_ : sel_path_;
    if (filter.scope == app::GitHistoryFilter::Scope::CurrentPath && filter.rel_path.empty()) {
        filter.scope = app::GitHistoryFilter::Scope::All;
    }
    filter.query = HistQueryText();
    filter.mine_author = !identity_.email.empty() ? identity_.email : identity_.name;

    const HWND target = hwnd_;
    const std::wstring root = root_;

    hist_thread_ = std::thread([this, target, root, filter, gen]() {
        auto job = std::make_unique<HistJob>();
        job->generation = gen;
        const app::GitOpResult res = app::GitReadHistory(root, filter, 150, job->commits, job->behind, 20000);
        job->ok = res.ok;
        {
            std::lock_guard<std::mutex> lock(hist_mutex_);
            hist_pending_ = std::move(job);
        }
        PostMessageW(target, kMsgHistDone, 0, 0);
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::AdoptHistoryLoad() {
    std::unique_ptr<HistJob> job;
    {
        std::lock_guard<std::mutex> lock(hist_mutex_);
        job = std::move(hist_pending_);
    }
    if (!job || job->generation != hist_generation_) return;

    hist_loading_ = false;
    hist_behind_ = job->behind;

    std::wstring prev_hash;
    if (hist_sel_ >= 0 && hist_sel_ < static_cast<int>(hist_commits_.size())) {
        prev_hash = hist_commits_[static_cast<size_t>(hist_sel_)].full_hash;
    }

    hist_commits_ = std::move(job->commits);
    hist_list_scroll_ = 0.0f;
    hist_detail_scroll_ = 0.0f;
    hist_creating_branch_ = false;

    int found = -1;
    if (!prev_hash.empty()) {
        for (size_t i = 0; i < hist_commits_.size(); ++i) {
            if (hist_commits_[i].full_hash == prev_hash) {
                found = static_cast<int>(i);
                break;
            }
        }
    }
    if (found >= 0) {
        hist_sel_ = found;
        hist_wip_sel_ = false;
    } else if (!hist_commits_.empty()) {
        hist_sel_ = 0;
        hist_wip_sel_ = false;
    } else {
        hist_sel_ = -1;
        hist_wip_sel_ = set_.Total() > 0;
    }

    if (hist_sel_ >= 0) {
        StartHistoryDetailLoad(hist_detail_mode_ == HistDetailMode::TreeBrowser);
    } else {
        hist_files_.clear();
        hist_tree_.clear();
    }

    Layout();
    SyncChildEditsVisibility();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::SelectHistoryCommit(int index) {
    if (index == -1) {
        if (set_.Total() > 0) SetActiveTab(GitViewTab::Changes);
        return;
    }
    if (index < 0 || index >= static_cast<int>(hist_commits_.size())) return;
    hist_wip_sel_ = false;
    hist_sel_ = index;
    hist_detail_mode_ = HistDetailMode::Detail;
    hist_creating_branch_ = false;
    hist_detail_scroll_ = 0.0f;
    StartHistoryDetailLoad(false);
    Layout();
    LayoutEdits();
    SyncChildEditsVisibility();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::StartHistoryDetailLoad(bool load_tree) {
    if (hist_detail_thread_.joinable()) hist_detail_thread_.join();
    const uint64_t gen = ++hist_detail_gen_;
    if (!hwnd_ || root_.empty() || hist_sel_ < 0 || hist_sel_ >= static_cast<int>(hist_commits_.size())) {
        hist_detail_loading_ = false;
        hist_files_.clear();
        hist_tree_.clear();
        return;
    }

    hist_detail_loading_ = true;
    const HWND target = hwnd_;
    const std::wstring root = root_;
    const std::wstring hash = hist_commits_[static_cast<size_t>(hist_sel_)].full_hash;

    hist_detail_thread_ = std::thread([this, target, root, hash, load_tree, gen]() {
        auto job = std::make_unique<HistDetailJob>();
        job->generation = gen;
        job->hash = hash;
        job->load_tree = load_tree;
        app::GitReadCommitFiles(root, hash, job->files, 15000);
        if (load_tree) {
            app::GitReadCommitTree(root, hash, job->tree, 20000);
        }
        {
            std::lock_guard<std::mutex> lock(hist_mutex_);
            hist_detail_pending_ = std::move(job);
        }
        PostMessageW(target, kMsgHistDetailDone, 0, 0);
    });
}

void GitWindow::AdoptHistoryDetailLoad() {
    std::unique_ptr<HistDetailJob> job;
    {
        std::lock_guard<std::mutex> lock(hist_mutex_);
        job = std::move(hist_detail_pending_);
    }
    if (!job || job->generation != hist_detail_gen_) return;
    hist_detail_loading_ = false;
    hist_files_ = std::move(job->files);
    if (job->load_tree) hist_tree_ = std::move(job->tree);
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::StartHistoryFileDiff(const std::wstring& hash, const std::wstring& rel_path) {
    if (hist_diff_thread_.joinable()) hist_diff_thread_.join();
    const uint64_t gen = ++hist_diff_gen_;

    hist_diff_path_ = rel_path;
    hist_detail_mode_ = HistDetailMode::InlineDiff;
    hist_detail_scroll_ = 0.0f;
    hist_diff_urows_.clear();
    hist_diff_left_.reset();
    hist_diff_right_.reset();
    hist_old_img_bmp_.reset();
    hist_new_img_bmp_.reset();
    hist_has_old_img_ = false;
    hist_has_new_img_ = false;
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (!hwnd_ || root_.empty() || hash.empty() || rel_path.empty()) return;
    const HWND target = hwnd_;
    const std::wstring root = root_;
    const bool plain = plain_terms_;

    hist_diff_thread_ = std::thread([this, target, root, hash, rel_path, plain, gen]() {
        auto job = std::make_unique<HistDiffJob>();
        job->generation = gen;
        job->is_image = app::GitIsImagePath(rel_path);

        if (job->is_image) {
            constexpr uint64_t kMaxImg = 20ull * 1024 * 1024;
            std::string old_bytes, new_bytes;
            if (app::GitReadCommitFileBlob(root, hash, rel_path, old_bytes, kMaxImg)) {
                job->old_image = DecodeGitImageBytes(old_bytes, 1024);
                job->has_old_image = job->old_image.ok();
            }
            if (ReadWorktreeRaw(app::GitJoinPath(root, rel_path), new_bytes, kMaxImg)) {
                job->new_image = DecodeGitImageBytes(new_bytes, 1024);
                job->has_new_image = job->new_image.ok();
            }
        } else {
            auto empty_side = [](const std::wstring& p) {
                auto s = std::make_shared<diff::DiffSide>();
                s->path = p;
                s->status = diff::LoadStatus::Ok;
                return s;
            };
            std::string blob;
            bool too_large = false;
            std::shared_ptr<diff::DiffSide> left = empty_side(rel_path);
            if (app::GitReadCommitFileBlob(root, hash, rel_path, blob, diff::kMaxDiffFileBytes, &too_large)) {
                const std::wstring tmp = app::GitWriteTempFile(blob, FileNameOf(rel_path));
                if (!tmp.empty()) {
                    diff::LoadDiffSide(tmp, *left);
                    DeleteFileW(tmp.c_str());
                } else {
                    left->status = diff::LoadStatus::ReadFailed;
                }
                left->path = rel_path;
            } else if (too_large) {
                left->status = diff::LoadStatus::TooLarge;
            }

            auto right = std::make_shared<diff::DiffSide>();
            diff::LoadDiffSide(app::GitJoinPath(root, rel_path), *right);
            right->path = rel_path;
            if (right->status == diff::LoadStatus::ReadFailed) {
                right = empty_side(rel_path);
            }

            if (left->status == diff::LoadStatus::Ok && right->status == diff::LoadStatus::Ok) {
                diff::ComputeLineDiff(left->lines, right->lines, diff::DiffOptions{}, job->result, nullptr);
                const auto& rows = job->result.rows;
                const int n = static_cast<int>(rows.size());
                std::vector<int> left_before(static_cast<size_t>(n) + 1, 0);
                std::vector<int> right_before(static_cast<size_t>(n) + 1, 0);
                for (int i = 0; i < n; ++i) {
                    left_before[static_cast<size_t>(i) + 1] =
                        left_before[static_cast<size_t>(i)] + (rows[static_cast<size_t>(i)].left >= 0 ? 1 : 0);
                    right_before[static_cast<size_t>(i) + 1] =
                        right_before[static_cast<size_t>(i)] + (rows[static_cast<size_t>(i)].right >= 0 ? 1 : 0);
                }

                const auto& hunks = job->result.hunks;
                size_t h = 0;
                constexpr int kCtx = 3;
                while (h < hunks.size()) {
                    int start = (std::max)(0, hunks[h].first - kCtx);
                    int end = (std::min)(n - 1, hunks[h].last + kCtx);
                    while (h + 1 < hunks.size() && hunks[h + 1].first - kCtx <= end + 1) {
                        ++h;
                        end = (std::min)(n - 1, hunks[h].last + kCtx);
                    }
                    int old_c = 0, new_c = 0;
                    for (int r = start; r <= end; ++r) {
                        if (rows[static_cast<size_t>(r)].left >= 0) ++old_c;
                        if (rows[static_cast<size_t>(r)].right >= 0) ++new_c;
                    }
                    const int old_s = old_c ? left_before[static_cast<size_t>(start)] + 1
                                            : left_before[static_cast<size_t>(start)];
                    const int new_s = new_c ? right_before[static_cast<size_t>(start)] + 1
                                            : right_before[static_cast<size_t>(start)];
                    job->urows.push_back({URowKind::Hunk, -1, -1, start, static_cast<int>(job->urows.size()),
                                          FormatHunkHdr(old_s, old_c, new_s, new_c, plain)});
                    for (int r = start; r <= end; ++r) {
                        const auto& row = rows[static_cast<size_t>(r)];
                        if (row.kind == diff::RowKind::Same) {
                            job->urows.push_back({URowKind::Same, row.left, row.right, r, -1,
                                                  row.left >= 0 ? left->lines[static_cast<size_t>(row.left)] : std::wstring()});
                        } else {
                            if (row.left >= 0) {
                                job->urows.push_back({URowKind::Del, row.left, -1, r, -1,
                                                      left->lines[static_cast<size_t>(row.left)]});
                            }
                            if (row.right >= 0) {
                                job->urows.push_back({URowKind::Add, -1, row.right, r, -1,
                                                      right->lines[static_cast<size_t>(row.right)]});
                            }
                        }
                    }
                    ++h;
                }
            }
            job->left = std::move(left);
            job->right = std::move(right);
        }

        {
            std::lock_guard<std::mutex> lock(hist_mutex_);
            hist_diff_pending_ = std::move(job);
        }
        PostMessageW(target, kMsgHistDiffDone, 0, 0);
    });
}

void GitWindow::AdoptHistoryFileDiff() {
    std::unique_ptr<HistDiffJob> job;
    {
        std::lock_guard<std::mutex> lock(hist_mutex_);
        job = std::move(hist_diff_pending_);
    }
    if (!job || job->generation != hist_diff_gen_) return;

    hist_diff_left_ = std::move(job->left);
    hist_diff_right_ = std::move(job->right);
    hist_diff_urows_ = std::move(job->urows);
    hist_old_img_meta_ = std::move(job->old_image);
    hist_new_img_meta_ = std::move(job->new_image);
    hist_has_old_img_ = job->has_old_image;
    hist_has_new_img_ = job->has_new_image;
    hist_old_img_bmp_.reset();
    hist_new_img_bmp_.reset();
    if (auto* dc = compositor_.Dc()) {
        if (hist_has_old_img_) hist_old_img_bmp_ = CreateBitmapFromDecodedImage(dc, hist_old_img_meta_);
        if (hist_has_new_img_) hist_new_img_bmp_ = CreateBitmapFromDecodedImage(dc, hist_new_img_meta_);
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::PromptRestoreHistoryFile(const std::wstring& hash, const std::wstring& rel_path) {
    if (op_busy_ || hash.empty() || rel_path.empty()) return;
    ConfirmDialogSpec spec;
    spec.title = GitFmt(GitFmt(Str(Sid::GitHistRestoreConfirmTitle), L"{name}", FileNameOf(rel_path)),
                        L"{hash}", hash.substr(0, 7));
    spec.message = Str(Sid::GitHistRestoreConfirmMsg);
    spec.confirm_text = GitStr(plain_terms_, Sid::GitHistRestoreFilePlain, Sid::GitHistRestoreFileGit);
    spec.cancel_text = Str(Sid::Cancel);
    spec.tone = ConfirmTone::Info;
    spec.note = GitFmt(Str(Sid::GitCmdDetailsSummary), L"{cmd}",
                       L"git checkout " + hash.substr(0, 7) + L" -- " + rel_path);
    if (ShowConfirmDialogEx(hwnd_, spec, dark_, accent_) != ConfirmChoice::Confirm) return;

    OpRequest req;
    req.kind = OpKind::RestoreCommitFile;
    req.arg1 = hash;
    req.arg2 = rel_path;
    req.flag1 = set_.Total() > 0;
    StartOp(std::move(req));
}

void GitWindow::PromptResetToCommit(const app::GitCommitItem& commit) {
    if (op_busy_ || commit.full_hash.empty()) return;
    ConfirmDialogSpec spec;
    spec.title = GitFmt(Str(Sid::GitHistResetTitle), L"{subject}",
                        commit.subject.empty() ? commit.short_hash : commit.subject);
    spec.message = Str(Sid::GitHistResetMsg);
    spec.confirm_text = Str(Sid::GitHistResetKeepBtn);
    spec.secondary_text = Str(Sid::GitHistResetDropBtn);
    spec.cancel_text = Str(Sid::Cancel);
    spec.tone = ConfirmTone::Warning;
    spec.default_choice = ConfirmChoice::Confirm;
    spec.note = GitFmt(Str(Sid::GitCmdDetailsSummary), L"{cmd}",
                       L"git reset --soft / --hard " + commit.short_hash);

    const ConfirmChoice choice = ShowConfirmDialogEx(hwnd_, spec, dark_, accent_);
    if (choice == ConfirmChoice::Cancel) return;

    const bool hard = (choice == ConfirmChoice::Secondary);
    if (hard) {
        ConfirmDialogSpec danger;
        danger.title = GitFmt(Str(Sid::GitHistResetHardConfirmTitle), L"{hash}", commit.short_hash);
        danger.message = Str(Sid::GitHistResetHardConfirmMsg);
        danger.confirm_text = Str(Sid::GitHistResetDropBtn);
        danger.cancel_text = Str(Sid::Cancel);
        danger.danger = true;
        danger.tone = ConfirmTone::Danger;
        danger.default_choice = ConfirmChoice::Cancel;
        danger.note = GitFmt(Str(Sid::GitCmdDetailsSummary), L"{cmd}",
                             L"git reset --hard " + commit.short_hash);
        if (ShowConfirmDialogEx(hwnd_, danger, dark_, accent_) != ConfirmChoice::Confirm) return;
    }

    OpRequest req;
    req.kind = OpKind::ResetToCommit;
    req.arg1 = commit.full_hash;
    req.flag1 = hard;
    StartOp(std::move(req));
}

void GitWindow::LayoutHistory(float w, float h) {
    (void)h;
    const float s = scale_;
    const float bar_h = 44.0f * s;
    hist_bar_ = D2D1::RectF(0, toolbar_.bottom, w, toolbar_.bottom + bar_h);

    const std::wstring t_all = GitStr(plain_terms_, Sid::GitHistAllPlain, Sid::GitHistAllGit);
    const std::wstring t_path = GitStr(plain_terms_, Sid::GitHistPathPlain, Sid::GitHistPathGit);
    const std::wstring t_mine = GitStr(plain_terms_, Sid::GitHistMinePlain, Sid::GitHistMineGit);
    const bool has_path = !hist_scope_rel_path_.empty() || !sel_path_.empty();

    float sx = 12.0f * s;
    const float seg_t = hist_bar_.top + 6.0f * s;
    const float seg_b = hist_bar_.bottom - 6.0f * s;
    const float w_all = Measure(t_all, ui_.get()) + 24.0f * s;
    const float w_path = has_path ? (Measure(t_path, ui_.get()) + 24.0f * s) : 0.0f;
    const float w_mine = Measure(t_mine, ui_.get()) + 24.0f * s;

    hist_scope_all_ = D2D1::RectF(sx, seg_t, sx + w_all, seg_b);
    sx = hist_scope_all_.right;
    if (has_path) {
        hist_scope_path_ = D2D1::RectF(sx, seg_t, sx + w_path, seg_b);
        sx = hist_scope_path_.right;
    } else {
        hist_scope_path_ = D2D1::RectF(0, 0, 0, 0);
    }
    hist_scope_mine_ = D2D1::RectF(sx, seg_t, sx + w_mine, seg_b);

    const float search_left = hist_scope_mine_.right + 12.0f * s;
    const float search_right = (std::min)(search_left + 280.0f * s, (std::max)(search_left + 120.0f * s, w - 260.0f * s));
    hist_search_rc_ = search_right > search_left + 60.0f * s
        ? D2D1::RectF(search_left, seg_t, search_right, seg_b)
        : D2D1::RectF(0, 0, 0, 0);

    const float lw = std::clamp(w * 0.56f, 400.0f * s, (std::max)(400.0f * s, w - 340.0f * s));
    hist_left_ = D2D1::RectF(0, hist_bar_.bottom, lw, status_.top);
    hist_right_ = D2D1::RectF(lw, hist_bar_.bottom, w, status_.top);

    float tl_top = hist_left_.top;
    if (hist_behind_ > 0) {
        hist_banner_rc_ = D2D1::RectF(hist_left_.left + 10.0f * s, tl_top + 8.0f * s,
                                      hist_left_.right - 10.0f * s, tl_top + 48.0f * s);
        const std::wstring pt = GitStr(plain_terms_, Sid::GitFetchUpdatesPlain, Sid::GitPull);
        const float pw = painter_.MeasureButtonWidth(pt, L"\xE896");
        hist_pull_rc_ = D2D1::RectF(hist_banner_rc_.right - 8.0f * s - pw, hist_banner_rc_.top + 5.0f * s,
                                    hist_banner_rc_.right - 8.0f * s, hist_banner_rc_.bottom - 5.0f * s);
        tl_top = hist_banner_rc_.bottom + 6.0f * s;
    } else {
        hist_banner_rc_ = D2D1::RectF(0, 0, 0, 0);
        hist_pull_rc_ = D2D1::RectF(0, 0, 0, 0);
    }
    hist_tl_rc_ = D2D1::RectF(hist_left_.left, tl_top, hist_left_.right, hist_left_.bottom);

    // Clamp timeline scroll
    const float row_h = 54.0f * s;
    const size_t total_rows = hist_commits_.size() + (set_.Total() > 0 ? 1u : 0u);
    const float tl_content = static_cast<float>(total_rows) * row_h;
    const float tl_max = (std::max)(0.0f, tl_content - (hist_tl_rc_.bottom - hist_tl_rc_.top));
    hist_list_scroll_ = std::clamp(hist_list_scroll_, 0.0f, tl_max);

    // Right panel interactive rects
    hist_meta_rc_ = D2D1::RectF(0, 0, 0, 0);
    hist_files_rc_ = D2D1::RectF(0, 0, 0, 0);
    hist_acts_rc_ = D2D1::RectF(0, 0, 0, 0);
    hist_btn_files_ = D2D1::RectF(0, 0, 0, 0);
    hist_btn_compare_ = D2D1::RectF(0, 0, 0, 0);
    hist_btn_branch_ = D2D1::RectF(0, 0, 0, 0);
    hist_btn_reset_ = D2D1::RectF(0, 0, 0, 0);
    hist_branch_box_ = D2D1::RectF(0, 0, 0, 0);
    hist_branch_ok_ = D2D1::RectF(0, 0, 0, 0);
    hist_branch_cancel_ = D2D1::RectF(0, 0, 0, 0);
    hist_back_rc_ = D2D1::RectF(0, 0, 0, 0);
    hist_file_rects_.clear();
    hist_file_diff_rects_.clear();
    hist_file_restore_rects_.clear();
    hist_tree_rects_.clear();
    hist_tree_restore_rects_.clear();

    if (hist_sel_ < 0 || hist_sel_ >= static_cast<int>(hist_commits_.size())) {
        hist_detail_scroll_ = 0.0f;
        return;
    }

    const float rx = hist_right_.left + 16.0f * s;
    const float rw = (std::max)(200.0f * s, hist_right_.right - hist_right_.left - 32.0f * s);

    if (hist_detail_mode_ == HistDetailMode::TreeBrowser || hist_detail_mode_ == HistDetailMode::InlineDiff) {
        const std::wstring back_t = Str(Sid::GitHistBackToDetail);
        const float bw = painter_.MeasureButtonWidth(back_t, {});
        hist_back_rc_ = D2D1::RectF(rx, hist_right_.top + 12.0f * s, rx + bw, hist_right_.top + 42.0f * s);
        const float body_top = hist_back_rc_.bottom + 42.0f * s;
        if (hist_detail_mode_ == HistDetailMode::TreeBrowser) {
            hist_tree_rects_.resize(hist_tree_.size());
            hist_tree_restore_rects_.resize(hist_tree_.size());
            const std::wstring rst_t = GitStr(plain_terms_, Sid::GitHistRestoreFilePlain, Sid::GitHistRestoreFileGit);
            const float rw_btn = Measure(rst_t, small_.get()) + 16.0f * s;
            const float tr_h = 34.0f * s;
            for (size_t i = 0; i < hist_tree_.size(); ++i) {
                const float ry = body_top + static_cast<float>(i) * tr_h - hist_detail_scroll_;
                hist_tree_rects_[i] = D2D1::RectF(rx, ry, rx + rw - 10.0f * s, ry + tr_h);
                hist_tree_restore_rects_[i] = D2D1::RectF(rx + rw - 16.0f * s - rw_btn, ry + 5.0f * s,
                                                          rx + rw - 16.0f * s, ry + tr_h - 5.0f * s);
            }
            const float max_s = (std::max)(0.0f, static_cast<float>(hist_tree_.size()) * tr_h - (hist_right_.bottom - body_top - 12.0f * s));
            hist_detail_scroll_ = std::clamp(hist_detail_scroll_, 0.0f, max_s);
        } else {
            // Single restore button top-right of InlineDiff view
            const std::wstring rst_t = GitStr(plain_terms_, Sid::GitHistRestoreFilePlain, Sid::GitHistRestoreFileGit);
            const float rw_btn = painter_.MeasureButtonWidth(rst_t, L"\xE7A7");
            hist_file_restore_rects_.push_back(D2D1::RectF(rx + rw - rw_btn, hist_right_.top + 12.0f * s,
                                                           rx + rw, hist_right_.top + 42.0f * s));
            const float content_h = hist_old_img_meta_.ok() || hist_new_img_meta_.ok()
                ? 360.0f * s
                : static_cast<float>(hist_diff_urows_.size()) * 20.0f * s;
            const float max_s = (std::max)(0.0f, content_h - (hist_right_.bottom - body_top - 12.0f * s));
            hist_detail_scroll_ = std::clamp(hist_detail_scroll_, 0.0f, max_s);
        }
        return;
    }

    // Detail mode
    const auto& c = hist_commits_[static_cast<size_t>(hist_sel_)];
    const float subj_h = (std::max)(26.0f * s, MeasureWrappedHeight(c.subject.empty() ? L" " : c.subject, rw, ui_bold_.get()));
    const float body_h = c.body.empty() ? 0.0f : (MeasureWrappedHeight(c.body, rw, small_.get()) + 8.0f * s);

    float ry = hist_right_.top + 14.0f * s + subj_h + body_h + 10.0f * s;
    const float meta_h = (c.is_remote_only || c.is_unpushed || !c.refs.empty()) ? 104.0f * s : 80.0f * s;
    hist_meta_rc_ = D2D1::RectF(rx, ry, rx + rw, ry + meta_h);
    ry = hist_meta_rc_.bottom + 12.0f * s;

    // Bottom actions anchored at bottom of right pane
    const float act_h = hist_creating_branch_ ? 128.0f * s : 84.0f * s;
    hist_acts_rc_ = D2D1::RectF(rx, (std::max)(ry + 80.0f * s, hist_right_.bottom - act_h - 12.0f * s),
                                rx + rw, hist_right_.bottom - 12.0f * s);
    const float btn_w = (rw - 8.0f * s) * 0.5f;
    const float r1_t = hist_acts_rc_.top;
    const float r2_t = r1_t + 38.0f * s;
    hist_btn_files_ = D2D1::RectF(rx, r1_t, rx + btn_w, r1_t + 32.0f * s);
    hist_btn_compare_ = D2D1::RectF(rx + btn_w + 8.0f * s, r1_t, rx + rw, r1_t + 32.0f * s);
    hist_btn_branch_ = D2D1::RectF(rx, r2_t, rx + btn_w, r2_t + 32.0f * s);
    hist_btn_reset_ = D2D1::RectF(rx + btn_w + 8.0f * s, r2_t, rx + rw, r2_t + 32.0f * s);

    if (hist_creating_branch_) {
        const float r3_t = r2_t + 40.0f * s;
        const std::wstring ok_t = Str(Sid::ConfirmDefault);
        const std::wstring cancel_t = Str(Sid::Cancel);
        const float ok_w = painter_.MeasureButtonWidth(ok_t, {});
        const float cancel_w = painter_.MeasureButtonWidth(cancel_t, {});
        hist_branch_cancel_ = D2D1::RectF(rx + rw - cancel_w, r3_t, rx + rw, r3_t + 30.0f * s);
        hist_branch_ok_ = D2D1::RectF(hist_branch_cancel_.left - 6.0f * s - ok_w, r3_t,
                                     hist_branch_cancel_.left - 6.0f * s, r3_t + 30.0f * s);
        hist_branch_box_ = D2D1::RectF(rx, r3_t, hist_branch_ok_.left - 8.0f * s, r3_t + 30.0f * s);
    }

    hist_files_rc_ = D2D1::RectF(rx, ry, rx + rw, (std::max)(ry, hist_acts_rc_.top - 10.0f * s));
    const float fl_top = hist_files_rc_.top + 26.0f * s;
    const float fr_h = 30.0f * s;
    const float files_content = static_cast<float>(hist_files_.size()) * fr_h;
    const float files_max = (std::max)(0.0f, files_content - (hist_files_rc_.bottom - fl_top));
    hist_detail_scroll_ = std::clamp(hist_detail_scroll_, 0.0f, files_max);

    hist_file_rects_.resize(hist_files_.size());
    hist_file_diff_rects_.resize(hist_files_.size());
    hist_file_restore_rects_.resize(hist_files_.size());

    const std::wstring d_t = GitStr(plain_terms_, Sid::GitViewChangesPlain, Sid::GitViewChangesGit);
    const std::wstring r_t = GitStr(plain_terms_, Sid::GitHistRestoreFilePlain, Sid::GitHistRestoreFileGit);
    const float dw = Measure(d_t, small_.get()) + 14.0f * s;
    const float rbw = Measure(r_t, small_.get()) + 14.0f * s;
    for (size_t i = 0; i < hist_files_.size(); ++i) {
        const float f_y = fl_top + static_cast<float>(i) * fr_h - hist_detail_scroll_;
        hist_file_rects_[i] = D2D1::RectF(hist_files_rc_.left, f_y, hist_files_rc_.right - 6.0f * s, f_y + fr_h);
        hist_file_restore_rects_[i] = D2D1::RectF(hist_files_rc_.right - 8.0f * s - rbw, f_y + 4.0f * s,
                                                  hist_files_rc_.right - 8.0f * s, f_y + fr_h - 4.0f * s);
        hist_file_diff_rects_[i] = D2D1::RectF(hist_file_restore_rects_[i].left - 6.0f * s - dw, f_y + 4.0f * s,
                                               hist_file_restore_rects_[i].left - 6.0f * s, f_y + fr_h - 4.0f * s);
    }
}

void GitWindow::RenderHistory(ID2D1DeviceContext* dc, const Theme& theme, bool hc, bool painter_ok) {
    // Re-sync layout with scroll offsets without side effects
    LayoutHistory(static_cast<float>(compositor_.Width()), static_cast<float>(compositor_.Height()));

    const float s = scale_;
    const float line_w = (std::max)(1.0f, std::round(s));
    HistColors hc_pal = MakeHistColors(dark_);
    // Pulse's surfaces, as the Changes view: window colour, content card, sheet.
    hc_pal.panel = theme.bg;
    hc_pal.body = theme.surface_card;
    hc_pal.card = theme.surface_sheet;
    const D2D1_COLOR_F link_ink = hc ? theme.text : theme.accent;

    auto fill = [&](const D2D1_RECT_F& rc, const D2D1_COLOR_F& color) {
        if (rc.right <= rc.left || rc.bottom <= rc.top) return;
        brush_->SetColor(color);
        dc->FillRectangle(rc, brush_.get());
    };
    auto text_at = [&](std::wstring_view text, const D2D1_RECT_F& rc, IDWriteTextFormat* format,
                       const D2D1_COLOR_F& color) {
        if (!format || text.empty() || rc.right <= rc.left || rc.bottom <= rc.top) return;
        brush_->SetColor(color);
        dc->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, rc, brush_.get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    };
    auto fit = [&](const std::wstring& text, float width, IDWriteTextFormat* format) {
        if (Measure(text, format) <= width) return text;
        return FitTextEnd(text, width, [&](std::wstring_view t) { return Measure(t, format); });
    };
    auto button = [&](const D2D1_RECT_F& rc, const std::wstring& text, std::wstring_view glyph,
                      fluent::ButtonKind kind, bool enabled, HitKind id, int row = -1) {
        if (!painter_ok || rc.right <= rc.left) return;
        fluent::ButtonSpec spec{};
        spec.bounds = rc;
        spec.text = text;
        spec.glyph = glyph;
        spec.kind = kind;
        spec.state.enabled = enabled;
        spec.state.hovered = enabled && hover_.kind == id && hover_.row == row;
        spec.state.pressed = enabled && pressed_.kind == id && pressed_.row == row && hover_.kind == id;
        painter_.DrawButton(spec);
    };

    // ---- History filter & legend bar ----
    fill(hist_bar_, hc_pal.panel);
    fill(D2D1::RectF(hist_bar_.left, hist_bar_.bottom - line_w, hist_bar_.right, hist_bar_.bottom), theme.stroke_divider);

    if (painter_ok) {
        const float track_r = hist_scope_mine_.right;
        const D2D1_RECT_F track = D2D1::RectF(hist_scope_all_.left, hist_scope_all_.top, track_r, hist_scope_all_.bottom);
        painter_.DrawSegmentedTrack(track);

        auto seg = [&](const D2D1_RECT_F& rc, Sid pid, Sid gid, bool sel, fluent::SegmentPosition pos, HitKind id) {
            if (rc.right <= rc.left) return;
            fluent::SegmentedItemSpec sp{};
            sp.bounds = rc;
            sp.text = GitStr(plain_terms_, pid, gid);
            sp.position = pos;
            sp.shared_track = true;
            sp.state.enabled = true;
            sp.state.checked = sel;
            sp.state.hovered = hover_.kind == id;
            sp.state.pressed = pressed_.kind == id && sp.state.hovered;
            painter_.DrawTrackSegment(sp);
        };
        const bool has_path = hist_scope_path_.right > hist_scope_path_.left;
        seg(hist_scope_all_, Sid::GitHistAllPlain, Sid::GitHistAllGit,
            hist_scope_ == app::GitHistoryFilter::Scope::All, fluent::SegmentPosition::First, HitKind::HistScopeAll);
        if (has_path) {
            seg(hist_scope_path_, Sid::GitHistPathPlain, Sid::GitHistPathGit,
                hist_scope_ == app::GitHistoryFilter::Scope::CurrentPath, fluent::SegmentPosition::Middle, HitKind::HistScopePath);
        }
        seg(hist_scope_mine_, Sid::GitHistMinePlain, Sid::GitHistMineGit,
            hist_scope_ == app::GitHistoryFilter::Scope::Mine, fluent::SegmentPosition::Last, HitKind::HistScopeMine);

        if (hist_search_rc_.right > hist_search_rc_.left) {
            painter_.DrawTextFieldFrame(hist_search_rc_,
                                        {true, hover_.kind == HitKind::HistSearchField, false,
                                         hist_search_edit_ && GetFocus() == hist_search_edit_},
                                        true);
            painter_.DrawGlyph(L"\xE721",
                               D2D1::RectF(hist_search_rc_.left + 8.0f * s, hist_search_rc_.top,
                                           hist_search_rc_.left + 26.0f * s, hist_search_rc_.bottom),
                               theme.text_secondary);
        }
    }

    // Legend on right side of filter bar
    {
        float lx = (hist_search_rc_.right > 0.0f ? hist_search_rc_.right : hist_scope_mine_.right) + 14.0f * s;
        const float max_x = hist_bar_.right - 12.0f * s;
        auto draw_legend_item = [&](const std::wstring& label, const D2D1_COLOR_F& col, bool hollow) {
            const float w = Measure(label, small_.get()) + 20.0f * s;
            if (lx + w > max_x) return;
            const float cy = (hist_bar_.top + hist_bar_.bottom) * 0.5f;
            const D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F(lx + 5.0f * s, cy), 4.0f * s, 4.0f * s);
            brush_->SetColor(col);
            if (hollow) dc->DrawEllipse(el, brush_.get(), 1.5f * s);
            else dc->FillEllipse(el, brush_.get());
            text_at(label, D2D1::RectF(lx + 13.0f * s, hist_bar_.top, lx + w, hist_bar_.bottom),
                    small_.get(), theme.text_secondary);
            lx += w + 8.0f * s;
        };
        const std::wstring cur_br = set_.branch.empty() ? L"HEAD" : set_.branch;
        draw_legend_item(cur_br, theme.accent, false);
        draw_legend_item(Str(Sid::GitHistOtherBranches), hc_pal.branch_purple, false);
        draw_legend_item(Str(Sid::GitHistRemoteLegend), hc_pal.remote_green, true);

        const std::wstring tot_t = GitFmtCount(Str(Sid::GitHistTotalCommits), static_cast<int>(hist_commits_.size()));
        const float tw = Measure(tot_t, small_.get()) + 4.0f * s;
        if (lx + tw <= max_x) {
            text_at(tot_t, D2D1::RectF(max_x - tw, hist_bar_.top, max_x, hist_bar_.bottom),
                    small_.get(), theme.text_secondary);
        }
    }

    // ---- Left timeline + D2D graph ----
    fill(hist_left_, hc_pal.body);
    fill(D2D1::RectF(hist_left_.right - line_w, hist_left_.top, hist_left_.right, hist_left_.bottom), theme.stroke_divider);

    if (hist_banner_rc_.right > hist_banner_rc_.left) {
        if (painter_ok) {
            painter_.FillRoundedRect(hist_banner_rc_, 6.0f * s, WithAlpha(hc_pal.remote_green, dark_ ? 0.14f : 0.10f));
            painter_.StrokeRoundedRect(hist_banner_rc_, 6.0f * s, WithAlpha(hc_pal.remote_green, 0.35f), line_w);
        }
        const std::wstring bt = GitFmtCount(
            GitStr(plain_terms_, Sid::GitHistRemoteBannerPlain, Sid::GitHistRemoteBannerGit), hist_behind_);
        text_at(fit(bt, hist_pull_rc_.left - hist_banner_rc_.left - 18.0f * s, small_.get()),
                D2D1::RectF(hist_banner_rc_.left + 12.0f * s, hist_banner_rc_.top, hist_pull_rc_.left - 8.0f * s, hist_banner_rc_.bottom),
                small_.get(), theme.text);
        button(hist_pull_rc_, GitStr(plain_terms_, Sid::GitFetchUpdatesPlain, Sid::GitPull),
               L"\xE896", fluent::ButtonKind::Primary, !op_busy_, HitKind::HistPullBanner);
    }

    const bool has_wip = set_.Total() > 0;
    const int offset = has_wip ? 1 : 0;
    const int total_rows = static_cast<int>(hist_commits_.size()) + offset;
    const float row_h = 54.0f * s;

    if (total_rows == 0) {
        if (painter_ok) {
            fluent::EmptyStateSpec empty{};
            empty.bounds = hist_tl_rc_;
            empty.glyph = L"\xE81C";
            empty.title = hist_loading_ ? Str(Sid::GitScLoading) : Str(Sid::GitHistNoCommits);
            painter_.DrawEmptyState(empty);
        }
    } else {
        int max_lane = 0;
        for (const auto& c : hist_commits_) {
            if (c.lane > max_lane) max_lane = c.lane;
            for (const auto& e : c.edges) {
                if (e.to_lane > max_lane) max_lane = e.to_lane;
            }
        }
        max_lane = (std::min)(max_lane, 5);
        const float lane_dx = 16.0f * s;
        const float graph_x0 = hist_tl_rc_.left + 20.0f * s;
        const float text_x0 = graph_x0 + static_cast<float>(max_lane + 1) * lane_dx + 10.0f * s;

        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1StrokeStyle> dash_style;
        if (factory.get()) {
            D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
            factory->CreateStrokeStyle(sp, nullptr, 0, &dash_style);
        }

        dc->PushAxisAlignedClip(hist_tl_rc_, D2D1_ANTIALIAS_MODE_ALIASED);

        // Pass 1: Row selection / hover backgrounds
        for (int vr = 0; vr < total_rows; ++vr) {
            const float y = hist_tl_rc_.top + static_cast<float>(vr) * row_h - hist_list_scroll_;
            if (y + row_h < hist_tl_rc_.top || y > hist_tl_rc_.bottom) continue;
            const int model_idx = vr - offset;
            const bool sel = (vr == 0 && has_wip && hist_wip_sel_) || (model_idx >= 0 && model_idx == hist_sel_);
            const bool hot = hover_.kind == HitKind::HistRow && hover_.row == model_idx;
            if ((sel || hot) && painter_ok) {
                const D2D1_RECT_F row_rc = D2D1::RectF(hist_tl_rc_.left + 6.0f * s, y + 2.0f * s,
                                                       hist_tl_rc_.right - 12.0f * s, y + row_h - 2.0f * s);
                painter_.FillRoundedRect(row_rc, 6.0f * s, sel ? theme.fill_selected : theme.fill_hover);
                if (sel) {
                    painter_.FillRoundedRect(D2D1::RectF(row_rc.left, y + 12.0f * s, row_rc.left + 3.0f * s, y + row_h - 12.0f * s),
                                             1.5f * s, link_ink);
                }
            }
        }

        // Pass 2: Topology Bezier edges
        auto lane_x = [&](int lane) {
            return graph_x0 + static_cast<float>((std::min)(lane, 5)) * lane_dx;
        };
        auto row_cy = [&](int vr) {
            return hist_tl_rc_.top + (static_cast<float>(vr) + 0.5f) * row_h - hist_list_scroll_;
        };

        if (has_wip && !hist_commits_.empty()) {
            const float wy = row_cy(0);
            const float c0x = lane_x(hist_commits_[0].lane);
            const float wx = lane_x(0);
            const float c0y = row_cy(1);
            brush_->SetColor(hc_pal.warn_amber);
            dc->DrawLine(D2D1::Point2F(wx, wy), D2D1::Point2F(c0x, c0y), brush_.get(), 1.8f * s, dash_style.get());
        }

        for (size_t i = 0; i < hist_commits_.size(); ++i) {
            const auto& c = hist_commits_[i];
            const int vr1 = static_cast<int>(i) + offset;
            const float y1 = row_cy(vr1);
            const float x1 = lane_x(c.lane);

            for (const auto& edge : c.edges) {
                const int vr2 = edge.to_row + offset;
                const float y2 = row_cy(vr2);
                if (y2 < hist_tl_rc_.top - row_h && y1 < hist_tl_rc_.top - row_h) continue;
                if (y1 > hist_tl_rc_.bottom + row_h && y2 > hist_tl_rc_.bottom + row_h) continue;

                const float x2 = lane_x(edge.to_lane);
                const D2D1_COLOR_F col = LaneColor((std::max)(c.lane, edge.to_lane), edge.is_remote, theme, hc_pal);
                brush_->SetColor(col);
                ID2D1StrokeStyle* st = edge.is_remote ? dash_style.get() : nullptr;

                if (std::abs(x1 - x2) < 0.5f) {
                    dc->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.get(), 2.0f * s, st);
                } else if (factory.get()) {
                    ComPtr<ID2D1PathGeometry> geo;
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
                        const float ctrl = (std::min)(24.0f * s, (y2 - y1) * 0.48f);
                        sink->BeginFigure(D2D1::Point2F(x1, y1), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddBezier(D2D1::BezierSegment(
                            D2D1::Point2F(x1, y1 + ctrl),
                            D2D1::Point2F(x2, y2 - ctrl),
                            D2D1::Point2F(x2, y2)));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        dc->DrawGeometry(geo.get(), brush_.get(), 2.0f * s, st);
                    }
                }
            }
        }

        // Pass 3: Nodes + Text + Ref pills
        for (int vr = 0; vr < total_rows; ++vr) {
            const float y = hist_tl_rc_.top + static_cast<float>(vr) * row_h - hist_list_scroll_;
            if (y + row_h < hist_tl_rc_.top || y > hist_tl_rc_.bottom) continue;
            const float cy = y + row_h * 0.5f;

            if (vr == 0 && has_wip) {
                const float nx = lane_x(0);
                brush_->SetColor(hc_pal.body);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(nx, cy), 6.0f * s, 6.0f * s), brush_.get());
                brush_->SetColor(hc_pal.warn_amber);
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(nx, cy), 5.0f * s, 5.0f * s),
                                brush_.get(), 1.8f * s, dash_style.get());
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(nx, cy), 2.5f * s, 2.5f * s), brush_.get());

                const std::wstring wtitle = Str(Sid::GitWipRowPlain);
                const std::wstring wsub = GitFmtCount(Str(Sid::GitWipRowSubPlain), static_cast<int>(set_.Total()));
                text_at(wtitle, D2D1::RectF(text_x0, y + 7.0f * s, hist_tl_rc_.right - 16.0f * s, y + 27.0f * s),
                        ui_bold_.get(), hc_pal.warn_amber);
                text_at(wsub, D2D1::RectF(text_x0, y + 27.0f * s, hist_tl_rc_.right - 16.0f * s, y + 46.0f * s),
                        small_.get(), theme.text_secondary);
                continue;
            }

            const int ci = vr - offset;
            const auto& c = hist_commits_[static_cast<size_t>(ci)];
            const float nx = lane_x(c.lane);
            const D2D1_COLOR_F ncol = LaneColor(c.lane, c.is_remote_only, theme, hc_pal);
            const bool sel = (ci == hist_sel_);

            if (sel) {
                brush_->SetColor(WithAlpha(ncol, 0.28f));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(nx, cy), 9.0f * s, 9.0f * s), brush_.get());
            }
            brush_->SetColor(hc_pal.body);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(nx, cy), 6.0f * s, 6.0f * s), brush_.get());
            if (c.is_remote_only) {
                brush_->SetColor(ncol);
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(nx, cy), 4.5f * s, 4.5f * s),
                                brush_.get(), 1.6f * s, dash_style.get());
            } else if (c.is_merge) {
                brush_->SetColor(ncol);
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(nx, cy), 5.0f * s, 5.0f * s), brush_.get(), 1.6f * s);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(nx, cy), 2.2f * s, 2.2f * s), brush_.get());
            } else {
                brush_->SetColor(ncol);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(nx, cy), 4.5f * s, 4.5f * s), brush_.get());
            }

            // Ref tags drawn right-to-left on top row
            float subj_right = hist_tl_rc_.right - 16.0f * s;
            if (painter_ok) {
                auto draw_ref_tag = [&](const std::wstring& txt, const D2D1_COLOR_F& col) {
                    const float tw = (std::min)(110.0f * s, painter_.MeasureTagWidth(txt));
                    if (subj_right - tw < text_x0 + 80.0f * s) return;
                    fluent::TagSpec tag{};
                    tag.bounds = D2D1::RectF(subj_right - tw, y + 6.0f * s, subj_right, y + 24.0f * s);
                    tag.text = fit(txt, tw - 8.0f * s, small_.get());
                    tag.color = col;
                    tag.bordered = false;
                    painter_.DrawTag(tag);
                    subj_right = tag.bounds.left - 4.0f * s;
                };
                if (c.is_unpushed) {
                    draw_ref_tag(GitStr(plain_terms_, Sid::GitHistRefUnpushedPlain, Sid::GitHistRefUnpushedGit),
                                 hc_pal.warn_amber);
                }
                for (size_t ri = 0; ri < c.refs.size() && ri < 2; ++ri) {
                    const auto& rf = c.refs[ri];
                    const D2D1_COLOR_F rcol = rf.kind == app::GitRefTag::Kind::Local ? theme.accent
                        : (rf.kind == app::GitRefTag::Kind::Tag ? hc_pal.warn_amber : hc_pal.remote_green);
                    draw_ref_tag(rf.name, rcol);
                }
                if (c.is_remote_only && c.refs.empty()) {
                    draw_ref_tag(GitStr(plain_terms_, Sid::GitHistRefRemoteNewPlain, Sid::GitHistRefRemoteNewGit),
                                 hc_pal.remote_green);
                }
            }

            const std::wstring subj = c.subject.empty() ? c.short_hash : c.subject;
            text_at(fit(subj, (std::max)(40.0f * s, subj_right - text_x0 - 6.0f * s), sel ? ui_bold_.get() : ui_.get()),
                    D2D1::RectF(text_x0, y + 6.0f * s, subj_right, y + 26.0f * s),
                    sel ? ui_bold_.get() : ui_.get(), c.is_remote_only ? theme.text_secondary : theme.text);

            const std::wstring sub = c.author + L"  ·  " + c.time_text +
                                     (plain_terms_ ? L"" : L"  ·  " + c.short_hash);
            text_at(fit(sub, (std::max)(40.0f * s, hist_tl_rc_.right - text_x0 - 16.0f * s), small_.get()),
                    D2D1::RectF(text_x0, y + 28.0f * s, hist_tl_rc_.right - 16.0f * s, y + 48.0f * s),
                    small_.get(), theme.text_secondary);
        }

        const float tl_content = static_cast<float>(total_rows) * row_h;
        if (painter_ok && tl_content > hist_tl_rc_.bottom - hist_tl_rc_.top) {
            fluent::ScrollbarSpec bar{hist_tl_rc_, hist_list_scroll_, hist_tl_rc_.bottom - hist_tl_rc_.top, tl_content};
            bar.expand_progress = (hover_.kind == HitKind::HistListBar || drag_bar_ == 3) ? 1.0f : 0.0f;
            painter_.DrawScrollbar(bar);
        }
        dc->PopAxisAlignedClip();
    }

    // ---- Right detail / tree / inline diff pane ----
    fill(hist_right_, hc_pal.panel);
    if (hist_sel_ < 0 || hist_sel_ >= static_cast<int>(hist_commits_.size())) return;

    const auto& sel_c = hist_commits_[static_cast<size_t>(hist_sel_)];
    const float rx = hist_right_.left + 16.0f * s;
    const float rw = (std::max)(200.0f * s, hist_right_.right - hist_right_.left - 32.0f * s);

    if (hist_detail_mode_ == HistDetailMode::TreeBrowser) {
        button(hist_back_rc_, Str(Sid::GitHistBackToDetail), {}, fluent::ButtonKind::Transparent, true, HitKind::HistBackToDetail);
        const std::wstring ttl = GitFmt(Str(Sid::GitHistTreeTitle), L"{hash}", sel_c.short_hash);
        text_at(ttl, D2D1::RectF(rx, hist_back_rc_.bottom + 6.0f * s, rx + rw, hist_back_rc_.bottom + 28.0f * s),
                ui_bold_.get(), theme.text);
        text_at(Str(Sid::GitHistTreeHint),
                D2D1::RectF(rx, hist_back_rc_.bottom + 26.0f * s, rx + rw, hist_back_rc_.bottom + 44.0f * s),
                small_.get(), theme.text_secondary);

        const D2D1_RECT_F list_clip = D2D1::RectF(hist_right_.left, hist_back_rc_.bottom + 46.0f * s,
                                                  hist_right_.right, hist_right_.bottom);
        dc->PushAxisAlignedClip(list_clip, D2D1_ANTIALIAS_MODE_ALIASED);
        const std::wstring rst_t = GitStr(plain_terms_, Sid::GitHistRestoreFilePlain, Sid::GitHistRestoreFileGit);
        for (size_t i = 0; i < hist_tree_.size() && i < hist_tree_rects_.size(); ++i) {
            const D2D1_RECT_F& rc = hist_tree_rects_[i];
            if (rc.bottom < list_clip.top || rc.top > list_clip.bottom) continue;
            const bool hot = hover_.kind == HitKind::HistTreeRow && hover_.row == static_cast<int>(i);
            const bool hot_r = hover_.kind == HitKind::HistTreeRestore && hover_.row == static_cast<int>(i);
            if ((hot || hot_r) && painter_ok) painter_.FillRoundedRect(rc, 4.0f * s, theme.fill_hover);
            const auto& te = hist_tree_[i];
            const std::wstring sz_t = FormatBytesCompact(te.size);
            const D2D1_RECT_F& rrc = hist_tree_restore_rects_[i];
            text_at(fit(te.path + L"  (" + sz_t + L")", rrc.left - rc.left - 12.0f * s, small_.get()),
                    D2D1::RectF(rc.left + 8.0f * s, rc.top, rrc.left - 6.0f * s, rc.bottom),
                    small_.get(), theme.text);
            if ((hot || hot_r) && painter_ok) {
                painter_.FillRoundedRect(rrc, 4.0f * s, hot_r ? theme.accent : theme.surface_card);
                painter_.DrawText(rst_t, rrc, small_.get(), hot_r ? theme.accent_text : link_ink,
                                  fluent::HorizontalAlignment::Center);
            }
        }
        dc->PopAxisAlignedClip();
        return;
    }

    if (hist_detail_mode_ == HistDetailMode::InlineDiff) {
        button(hist_back_rc_, Str(Sid::GitHistBackToDetail), {}, fluent::ButtonKind::Transparent, true, HitKind::HistBackToDetail);
        if (!hist_file_restore_rects_.empty()) {
            button(hist_file_restore_rects_[0],
                   GitStr(plain_terms_, Sid::GitHistRestoreFilePlain, Sid::GitHistRestoreFileGit),
                   L"\xE7A7", fluent::ButtonKind::Standard, !op_busy_, HitKind::HistFileRestore, -1);
        }
        text_at(fit(hist_diff_path_ + L"  (" + sel_c.short_hash + L" → " +
                    GitStr(plain_terms_, Sid::GitDiffNowSidePlain, Sid::GitDiffWorkSideGit) + L")",
                    rw, small_bold_.get()),
                D2D1::RectF(rx, hist_back_rc_.bottom + 8.0f * s, rx + rw, hist_back_rc_.bottom + 30.0f * s),
                small_bold_.get(), theme.text);

        const D2D1_RECT_F diff_rc = D2D1::RectF(rx, hist_back_rc_.bottom + 38.0f * s, rx + rw, hist_right_.bottom - 10.0f * s);
        fill(diff_rc, hc_pal.body);
        dc->PushAxisAlignedClip(diff_rc, D2D1_ANTIALIAS_MODE_ALIASED);
        if (hist_has_old_img_ || hist_has_new_img_) {
            DrawGitImageCompare(dc, painter_, small_.get(), brush_.get(), theme, s, diff_rc,
                                hist_old_img_bmp_.get(), hist_has_old_img_ ? &hist_old_img_meta_ : nullptr,
                                hist_new_img_bmp_.get(), hist_has_new_img_ ? &hist_new_img_meta_ : nullptr,
                                0.5f, plain_terms_, false);
        } else {
            const float dr_h = 20.0f * s;
            for (size_t i = 0; i < hist_diff_urows_.size(); ++i) {
                const float y = diff_rc.top + static_cast<float>(i) * dr_h - hist_detail_scroll_;
                if (y + dr_h < diff_rc.top || y > diff_rc.bottom) continue;
                const auto& ur = hist_diff_urows_[i];
                const D2D1_RECT_F row_rc = D2D1::RectF(diff_rc.left, y, diff_rc.right, y + dr_h);
                if (ur.kind == URowKind::Hunk) fill(row_rc, hc_pal.hunk_bg);
                else if (ur.kind == URowKind::Add) fill(row_rc, hc_pal.add_bg);
                else if (ur.kind == URowKind::Del) fill(row_rc, hc_pal.del_bg);
                const wchar_t pfx = ur.kind == URowKind::Add ? L'+' : ur.kind == URowKind::Del ? L'-' : L' ';
                const std::wstring line = ur.kind == URowKind::Hunk ? ur.text : (std::wstring(1, pfx) + L" " + ur.text);
                text_at(line, D2D1::RectF(row_rc.left + 8.0f * s, y, row_rc.right - 8.0f * s, y + dr_h),
                        ur.kind == URowKind::Hunk ? small_.get() : mono_.get(),
                        ur.kind == URowKind::Hunk ? link_ink : theme.text);
            }
        }
        dc->PopAxisAlignedClip();
        return;
    }

    // Detail mode
    const float subj_h = (std::max)(26.0f * s, MeasureWrappedHeight(sel_c.subject.empty() ? L" " : sel_c.subject, rw, ui_bold_.get()));
    text_at(sel_c.subject, D2D1::RectF(rx, hist_right_.top + 14.0f * s, rx + rw, hist_right_.top + 14.0f * s + subj_h),
            ui_bold_.get(), theme.text);
    if (!sel_c.body.empty()) {
        const float by = hist_right_.top + 16.0f * s + subj_h;
        text_at(sel_c.body, D2D1::RectF(rx, by, rx + rw, hist_meta_rc_.top - 6.0f * s),
                small_.get(), theme.text_secondary);
    }

    if (painter_ok) {
        painter_.FillRoundedRect(hist_meta_rc_, 8.0f * s, hc_pal.card);
        painter_.StrokeRoundedRect(hist_meta_rc_, 8.0f * s, theme.stroke_card, line_w);
    }
    auto meta_row = [&](float my, const std::wstring& key, const std::wstring& val, const D2D1_COLOR_F& vcol) {
        text_at(key, D2D1::RectF(hist_meta_rc_.left + 12.0f * s, my, hist_meta_rc_.left + 88.0f * s, my + 20.0f * s),
                small_.get(), theme.text_secondary);
        text_at(fit(val, hist_meta_rc_.right - hist_meta_rc_.left - 104.0f * s, small_.get()),
                D2D1::RectF(hist_meta_rc_.left + 92.0f * s, my, hist_meta_rc_.right - 12.0f * s, my + 20.0f * s),
                small_.get(), vcol);
    };
    float my = hist_meta_rc_.top + 10.0f * s;
    const std::wstring author_v = AuthorInitials(sel_c.author) + L"  " + sel_c.author +
                                  (sel_c.email.empty() ? L"" : L" <" + sel_c.email + L">");
    meta_row(my, Str(Sid::GitHistAuthor), author_v, theme.text);
    my += 22.0f * s;
    meta_row(my, Str(Sid::GitHistDate), sel_c.time_text, theme.text);
    my += 22.0f * s;
    const std::wstring id_v = sel_c.short_hash + (plain_terms_ ? Str(Sid::GitHistIdNotePlain) : L"");
    meta_row(my, GitStr(plain_terms_, Sid::GitHistIdPlain, Sid::GitHistIdGit), id_v, theme.text_secondary);
    my += 22.0f * s;
    if (sel_c.is_remote_only) {
        meta_row(my, Str(Sid::GitHistLocation),
                 GitStr(plain_terms_, Sid::GitHistRemoteOnlyPlain, Sid::GitHistRemoteOnlyGit), hc_pal.remote_green);
    } else if (sel_c.is_unpushed) {
        meta_row(my, Str(Sid::GitHistLocation),
                 GitStr(plain_terms_, Sid::GitHistRefUnpushedPlain, Sid::GitHistRefUnpushedGit), hc_pal.warn_amber);
    } else if (!sel_c.refs.empty()) {
        std::wstring refs_v;
        for (size_t i = 0; i < sel_c.refs.size(); ++i) {
            if (i > 0) refs_v += L", ";
            refs_v += sel_c.refs[i].name;
        }
        meta_row(my, Str(Sid::GitHistLocation), refs_v, link_ink);
    }

    // Changed files list
    const std::wstring cf_hdr = GitFmtCount(
        GitStr(plain_terms_, Sid::GitHistChangedFilesPlain, Sid::GitHistChangedFilesGit),
        static_cast<int>(hist_files_.size()));
    text_at(cf_hdr, D2D1::RectF(hist_files_rc_.left, hist_files_rc_.top, hist_files_rc_.right, hist_files_rc_.top + 24.0f * s),
            small_bold_.get(), theme.text);

    const D2D1_RECT_F fl_clip = D2D1::RectF(hist_files_rc_.left, hist_files_rc_.top + 26.0f * s,
                                            hist_files_rc_.right, hist_files_rc_.bottom);
    dc->PushAxisAlignedClip(fl_clip, D2D1_ANTIALIAS_MODE_ALIASED);
    const std::wstring d_t = GitStr(plain_terms_, Sid::GitViewChangesPlain, Sid::GitViewChangesGit);
    const std::wstring r_t = GitStr(plain_terms_, Sid::GitHistRestoreFilePlain, Sid::GitHistRestoreFileGit);
    for (size_t i = 0; i < hist_files_.size() && i < hist_file_rects_.size(); ++i) {
        const D2D1_RECT_F& frc = hist_file_rects_[i];
        if (frc.bottom < fl_clip.top || frc.top > fl_clip.bottom) continue;
        const auto& cf = hist_files_[i];
        const int idx = static_cast<int>(i);
        const bool hot_row = (hover_.kind == HitKind::HistFileRow || hover_.kind == HitKind::HistFileDiff ||
                              hover_.kind == HitKind::HistFileRestore) && hover_.row == idx;
        if (hot_row && painter_ok) painter_.FillRoundedRect(frc, 4.0f * s, theme.fill_hover);

        const D2D1_COLOR_F scol = cf.status_char == L'A' ? hc_pal.remote_green
            : cf.status_char == L'D' ? hc_pal.del_fg : hc_pal.warn_amber;
        text_at(std::wstring(1, cf.status_char), D2D1::RectF(frc.left + 4.0f * s, frc.top, frc.left + 22.0f * s, frc.bottom),
                small_bold_.get(), scol);

        const D2D1_RECT_F& drc = hist_file_diff_rects_[i];
        const D2D1_RECT_F& rrc = hist_file_restore_rects_[i];
        const float text_max = (hot_row ? drc.left : frc.right - 68.0f * s) - (frc.left + 24.0f * s);
        text_at(fit(cf.path, (std::max)(40.0f * s, text_max), small_.get()),
                D2D1::RectF(frc.left + 24.0f * s, frc.top, frc.left + 24.0f * s + (std::max)(40.0f * s, text_max), frc.bottom),
                small_.get(), theme.text);

        if (hot_row && painter_ok) {
            const bool hd = hover_.kind == HitKind::HistFileDiff && hover_.row == idx;
            const bool hr = hover_.kind == HitKind::HistFileRestore && hover_.row == idx;
            painter_.FillRoundedRect(drc, 4.0f * s, hd ? theme.fill_pressed : theme.surface_card);
            painter_.DrawText(d_t, drc, small_.get(), link_ink, fluent::HorizontalAlignment::Center);
            painter_.FillRoundedRect(rrc, 4.0f * s, hr ? theme.accent : theme.surface_card);
            painter_.DrawText(r_t, rrc, small_.get(), hr ? theme.accent_text : link_ink, fluent::HorizontalAlignment::Center);
        } else if (!cf.is_binary) {
            const std::wstring stat = L"+" + std::to_wstring(cf.added) + L" \u2212" + std::to_wstring(cf.deleted);
            text_at(stat, D2D1::RectF(frc.right - 74.0f * s, frc.top, frc.right - 6.0f * s, frc.bottom),
                    small_.get(), theme.text_secondary);
        }
    }
    dc->PopAxisAlignedClip();

    // Bottom 4 actions
    button(hist_btn_files_, GitStr(plain_terms_, Sid::GitHistViewFilesPlain, Sid::GitHistViewFilesGit),
           L"\xE8B7", fluent::ButtonKind::Standard, !hist_detail_loading_, HitKind::HistBtnFiles);
    button(hist_btn_compare_, GitStr(plain_terms_, Sid::GitHistCompareNowPlain, Sid::GitHistCompareNowGit),
           L"\xE8AB", fluent::ButtonKind::Standard, !hist_files_.empty(), HitKind::HistBtnCompare);
    button(hist_btn_branch_, GitStr(plain_terms_, Sid::GitHistBranchHerePlain, Sid::GitHistBranchHereGit),
           L"\xF003", fluent::ButtonKind::Standard, !op_busy_, HitKind::HistBtnBranch);
    button(hist_btn_reset_, GitStr(plain_terms_, Sid::GitHistResetPlain, Sid::GitHistResetGit),
           L"\xE7A7", fluent::ButtonKind::Danger, !op_busy_ && !sel_c.is_remote_only, HitKind::HistBtnReset);

    if (hist_creating_branch_ && painter_ok) {
        painter_.DrawTextFieldFrame(hist_branch_box_, {true, false, false, true}, true);
        button(hist_branch_ok_, Str(Sid::ConfirmDefault), {}, fluent::ButtonKind::Primary,
               !op_busy_ && !HistBranchInputText().empty(), HitKind::HistBranchCreateOk);
        button(hist_branch_cancel_, Str(Sid::Cancel), {}, fluent::ButtonKind::Standard,
               true, HitKind::HistBranchCreateCancel);
    }
}

GitWindow::Hit GitWindow::HitTestHistory(float x, float y) const {
    if (Inside(hist_scope_all_, x, y)) return {HitKind::HistScopeAll, -1};
    if (Inside(hist_scope_path_, x, y)) return {HitKind::HistScopePath, -1};
    if (Inside(hist_scope_mine_, x, y)) return {HitKind::HistScopeMine, -1};
    if (Inside(hist_search_rc_, x, y)) return {HitKind::HistSearchField, -1};
    if (Inside(hist_pull_rc_, x, y)) return {HitKind::HistPullBanner, -1};

    if (Inside(hist_tl_rc_, x, y)) {
        const bool has_wip = set_.Total() > 0;
        const int offset = has_wip ? 1 : 0;
        const int total_rows = static_cast<int>(hist_commits_.size()) + offset;
        const float row_h = 54.0f * scale_;
        const float tl_content = static_cast<float>(total_rows) * row_h;
        fluent::ScrollbarSpec bar{hist_tl_rc_, hist_list_scroll_, hist_tl_rc_.bottom - hist_tl_rc_.top, tl_content};
        if (OnScrollbar(bar, x, y) && tl_content > hist_tl_rc_.bottom - hist_tl_rc_.top)
            return {HitKind::HistListBar, -1};
        const int vr = static_cast<int>((y - hist_tl_rc_.top + hist_list_scroll_) / row_h);
        if (vr >= 0 && vr < total_rows) {
            return {HitKind::HistRow, vr - offset};
        }
    }

    if (Inside(hist_right_, x, y)) {
        if (hist_detail_mode_ == HistDetailMode::TreeBrowser) {
            if (Inside(hist_back_rc_, x, y)) return {HitKind::HistBackToDetail, -1};
            for (size_t i = 0; i < hist_tree_restore_rects_.size(); ++i) {
                if (Inside(hist_tree_restore_rects_[i], x, y)) return {HitKind::HistTreeRestore, static_cast<int>(i)};
                if (i < hist_tree_rects_.size() && Inside(hist_tree_rects_[i], x, y))
                    return {HitKind::HistTreeRow, static_cast<int>(i)};
            }
            return {};
        }
        if (hist_detail_mode_ == HistDetailMode::InlineDiff) {
            if (Inside(hist_back_rc_, x, y)) return {HitKind::HistBackToDetail, -1};
            if (!hist_file_restore_rects_.empty() && Inside(hist_file_restore_rects_[0], x, y))
                return {HitKind::HistFileRestore, -1};
            return {};
        }
        if (Inside(hist_btn_files_, x, y)) return {HitKind::HistBtnFiles, -1};
        if (Inside(hist_btn_compare_, x, y)) return {HitKind::HistBtnCompare, -1};
        if (Inside(hist_btn_branch_, x, y)) return {HitKind::HistBtnBranch, -1};
        if (Inside(hist_btn_reset_, x, y)) return {HitKind::HistBtnReset, -1};
        if (hist_creating_branch_) {
            if (Inside(hist_branch_ok_, x, y)) return {HitKind::HistBranchCreateOk, -1};
            if (Inside(hist_branch_cancel_, x, y)) return {HitKind::HistBranchCreateCancel, -1};
        }
        if (Inside(hist_files_rc_, x, y)) {
            for (size_t i = 0; i < hist_file_rects_.size(); ++i) {
                if (i < hist_file_diff_rects_.size() && Inside(hist_file_diff_rects_[i], x, y))
                    return {HitKind::HistFileDiff, static_cast<int>(i)};
                if (i < hist_file_restore_rects_.size() && Inside(hist_file_restore_rects_[i], x, y))
                    return {HitKind::HistFileRestore, static_cast<int>(i)};
                if (Inside(hist_file_rects_[i], x, y)) return {HitKind::HistFileRow, static_cast<int>(i)};
            }
        }
    }
    return {};
}

bool GitWindow::OnClickHistory(const Hit& hit, float, float y) {
    switch (hit.kind) {
    case HitKind::HistScopeAll:
        hist_scope_ = app::GitHistoryFilter::Scope::All;
        StartHistoryLoad();
        return true;
    case HitKind::HistScopePath:
        hist_scope_ = app::GitHistoryFilter::Scope::CurrentPath;
        StartHistoryLoad();
        return true;
    case HitKind::HistScopeMine:
        hist_scope_ = app::GitHistoryFilter::Scope::Mine;
        StartHistoryLoad();
        return true;
    case HitKind::HistSearchField:
        if (hist_search_edit_) SetFocus(hist_search_edit_);
        return true;
    case HitKind::HistPullBanner: {
        OpRequest req;
        req.kind = OpKind::Pull;
        StartOp(std::move(req));
        return true;
    }
    case HitKind::HistListBar: {
        const float row_h = 54.0f * scale_;
        const size_t total_rows = hist_commits_.size() + (set_.Total() > 0 ? 1u : 0u);
        const float tl_content = static_cast<float>(total_rows) * row_h;
        fluent::ScrollbarSpec bar{hist_tl_rc_, hist_list_scroll_, hist_tl_rc_.bottom - hist_tl_rc_.top, tl_content};
        const D2D1_RECT_F thumb = fluent::ScrollbarThumbRect(bar, scale_, HighContrastOn());
        drag_bar_ = 3;
        drag_grab_ = (y >= thumb.top && y < thumb.bottom) ? (y - thumb.top) : (thumb.bottom - thumb.top) * 0.5f;
        SetCapture(hwnd_);
        return true;
    }
    case HitKind::HistRow:
        SelectHistoryCommit(hit.row);
        return true;
    case HitKind::HistFileRow:
    case HitKind::HistFileDiff:
        if (hist_sel_ >= 0 && hist_sel_ < static_cast<int>(hist_commits_.size()) &&
            hit.row >= 0 && hit.row < static_cast<int>(hist_files_.size())) {
            StartHistoryFileDiff(hist_commits_[static_cast<size_t>(hist_sel_)].full_hash,
                                 hist_files_[static_cast<size_t>(hit.row)].path);
        }
        return true;
    case HitKind::HistFileRestore:
        if (hist_sel_ >= 0 && hist_sel_ < static_cast<int>(hist_commits_.size())) {
            const std::wstring& hash = hist_commits_[static_cast<size_t>(hist_sel_)].full_hash;
            if (hit.row == -1 && !hist_diff_path_.empty()) {
                PromptRestoreHistoryFile(hash, hist_diff_path_);
            } else if (hit.row >= 0 && hit.row < static_cast<int>(hist_files_.size())) {
                PromptRestoreHistoryFile(hash, hist_files_[static_cast<size_t>(hit.row)].path);
            }
        }
        return true;
    case HitKind::HistBtnFiles:
        if (hist_sel_ >= 0 && hist_sel_ < static_cast<int>(hist_commits_.size())) {
            hist_detail_mode_ = HistDetailMode::TreeBrowser;
            hist_detail_scroll_ = 0.0f;
            StartHistoryDetailLoad(true);
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return true;
    case HitKind::HistBtnCompare:
        if (hist_sel_ >= 0 && hist_sel_ < static_cast<int>(hist_commits_.size()) && !hist_files_.empty()) {
            StartHistoryFileDiff(hist_commits_[static_cast<size_t>(hist_sel_)].full_hash, hist_files_[0].path);
        }
        return true;
    case HitKind::HistBtnBranch:
        if (hist_sel_ >= 0 && hist_sel_ < static_cast<int>(hist_commits_.size())) {
            hist_creating_branch_ = !hist_creating_branch_;
            if (hist_creating_branch_ && hist_branch_edit_) SetWindowTextW(hist_branch_edit_, L"");
            Layout();
            LayoutEdits();
            SyncChildEditsVisibility();
            if (hist_creating_branch_ && hist_branch_edit_) SetFocus(hist_branch_edit_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return true;
    case HitKind::HistBranchCreateOk: {
        const std::wstring name = HistBranchInputText();
        if (!op_busy_ && !name.empty() && hist_sel_ >= 0 && hist_sel_ < static_cast<int>(hist_commits_.size())) {
            OpRequest req;
            req.kind = OpKind::CreateBranch;
            req.arg1 = name;
            req.arg2 = hist_commits_[static_cast<size_t>(hist_sel_)].full_hash;
            hist_creating_branch_ = false;
            Layout();
            SyncChildEditsVisibility();
            StartOp(std::move(req));
        }
        return true;
    }
    case HitKind::HistBranchCreateCancel:
        hist_creating_branch_ = false;
        Layout();
        SyncChildEditsVisibility();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    case HitKind::HistBtnReset:
        if (hist_sel_ >= 0 && hist_sel_ < static_cast<int>(hist_commits_.size())) {
            const auto& c = hist_commits_[static_cast<size_t>(hist_sel_)];
            if (!c.is_remote_only) PromptResetToCommit(c);
        }
        return true;
    case HitKind::HistBackToDetail:
        hist_detail_mode_ = HistDetailMode::Detail;
        hist_detail_scroll_ = 0.0f;
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    case HitKind::HistTreeRow:
        if (hist_sel_ >= 0 && hist_sel_ < static_cast<int>(hist_commits_.size()) &&
            hit.row >= 0 && hit.row < static_cast<int>(hist_tree_.size())) {
            StartHistoryFileDiff(hist_commits_[static_cast<size_t>(hist_sel_)].full_hash,
                                 hist_tree_[static_cast<size_t>(hit.row)].path);
        }
        return true;
    case HitKind::HistTreeRestore:
        if (hist_sel_ >= 0 && hist_sel_ < static_cast<int>(hist_commits_.size()) &&
            hit.row >= 0 && hit.row < static_cast<int>(hist_tree_.size())) {
            PromptRestoreHistoryFile(hist_commits_[static_cast<size_t>(hist_sel_)].full_hash,
                                     hist_tree_[static_cast<size_t>(hit.row)].path);
        }
        return true;
    default:
        break;
    }
    return false;
}

}  // namespace pulse::ui
