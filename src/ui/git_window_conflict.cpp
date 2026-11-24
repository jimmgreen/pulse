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

std::wstring FolderOf(const std::wstring& rel) {
    const size_t slash = rel.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : rel.substr(0, slash);
}

std::wstring FormatByteCount(uint64_t bytes) {
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

std::wstring ToCrlf(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            out += L"\r\n";
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else if (text[i] == L'\n') {
            out += L"\r\n";
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::wstring FromCrlf(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t c : text) {
        if (c != L'\r') out.push_back(c);
    }
    return out;
}

struct ConflictColors {
    D2D1_COLOR_F panel;
    D2D1_COLOR_F body;
    D2D1_COLOR_F card;
    D2D1_COLOR_F ours_tint;
    D2D1_COLOR_F ours_ink;
    D2D1_COLOR_F theirs_tint;
    D2D1_COLOR_F theirs_ink;
    D2D1_COLOR_F base_tint;
    D2D1_COLOR_F ok_ink;
    D2D1_COLOR_F warn_bg;
    D2D1_COLOR_F warn_ink;
};

ConflictColors MakeConflictColors(bool dark) {
    ConflictColors c{};
    if (dark) {
        c.panel = D2D1::ColorF(0x202227);
        c.body = D2D1::ColorF(0x1B1D22);
        c.card = D2D1::ColorF(0x24272E);
        c.ours_tint = D2D1::ColorF(0x3DD68C, 0.14f);
        c.ours_ink = D2D1::ColorF(0x3DD68C);
        c.theirs_tint = D2D1::ColorF(0x60A5FA, 0.14f);
        c.theirs_ink = D2D1::ColorF(0x60A5FA);
        c.base_tint = D2D1::ColorF(0xFFFFFF, 0.05f);
        c.ok_ink = D2D1::ColorF(0x3DD68C);
        c.warn_bg = D2D1::ColorF(0xF5B941, 0.14f);
        c.warn_ink = D2D1::ColorF(0xF5B941);
    } else {
        c.panel = D2D1::ColorF(0xF5F6F9);
        c.body = D2D1::ColorF(0xFFFFFF);
        c.card = D2D1::ColorF(0xF8F9FB);
        c.ours_tint = D2D1::ColorF(0x1F9D55, 0.10f);
        c.ours_ink = D2D1::ColorF(0x188045);
        c.theirs_tint = D2D1::ColorF(0x2563EB, 0.09f);
        c.theirs_ink = D2D1::ColorF(0x1D4ED8);
        c.base_tint = D2D1::ColorF(0x000000, 0.03f);
        c.ok_ink = D2D1::ColorF(0x188045);
        c.warn_bg = D2D1::ColorF(0xC77D02, 0.10f);
        c.warn_ink = D2D1::ColorF(0x9E6000);
    }
    return c;
}

std::wstring FormatSideHeader(const std::wstring& role_label, const std::wstring& author, const std::wstring& time_text) {
    std::wstring out = role_label;
    if (!author.empty()) {
        out += L" · " + author;
    }
    if (!time_text.empty()) {
        out += L" (" + time_text + L")";
    }
    return out;
}

}  // namespace

int GitWindow::ConflictTotalCount() const noexcept {
    return static_cast<int>(cf_files_.size());
}

int GitWindow::ConflictResolvedCount() const noexcept {
    int count = 0;
    for (const auto& f : cf_files_) {
        if (f.resolved || cf_session_resolved_.contains(f.path)) ++count;
    }
    return count;
}

void GitWindow::StopConflictLoad() {
    ++cf_state_gen_;
    ++cf_file_gen_;
    if (cf_state_thread_.joinable()) cf_state_thread_.join();
    if (cf_file_thread_.joinable()) cf_file_thread_.join();
    std::lock_guard<std::mutex> lock(cf_mutex_);
    cf_state_pending_.reset();
    cf_file_pending_.reset();
}

void GitWindow::StartConflictStateLoad() {
    if (!hwnd_ || root_.empty()) return;
    if (cf_state_thread_.joinable()) cf_state_thread_.join();
    const uint64_t gen = ++cf_state_gen_;
    cf_loading_ = true;
    const HWND target = hwnd_;
    const std::wstring root = root_;

    cf_state_thread_ = std::thread([this, target, root, gen]() {
        auto job = std::make_unique<ConflictStateJob>();
        job->generation = gen;
        job->state = app::GitReadConflictState(root, 15000);
        {
            std::lock_guard<std::mutex> lock(cf_mutex_);
            cf_state_pending_ = std::move(job);
        }
        PostMessageW(target, kMsgCfStateDone, 0, 0);
    });
}

void GitWindow::AdoptConflictStateLoad() {
    std::unique_ptr<ConflictStateJob> job;
    {
        std::lock_guard<std::mutex> lock(cf_mutex_);
        job = std::move(cf_state_pending_);
    }
    if (!job || job->generation != cf_state_gen_) return;
    cf_loading_ = false;
    cf_state_ = std::move(job->state);
    merging_ = cf_state_.merging || !cf_state_.conflict_files.empty();

    std::vector<app::GitConflictFile> merged_list;
    std::unordered_set<std::wstring> active_paths;
    for (const auto& cf : cf_state_.conflict_files) {
        active_paths.insert(cf.path);
        cf_session_resolved_.erase(cf.path);
    }
    // Preserve resolved items during an active merge so progress stays accurate.
    if (merging_) {
        for (const auto& prev : cf_files_) {
            if (!active_paths.contains(prev.path) && (prev.resolved || cf_session_resolved_.contains(prev.path))) {
                app::GitConflictFile copy = prev;
                copy.resolved = true;
                merged_list.push_back(std::move(copy));
            }
        }
    } else {
        cf_session_resolved_.clear();
    }
    for (const auto& cf : cf_state_.conflict_files) {
        merged_list.push_back(cf);
    }
    cf_files_ = std::move(merged_list);

    auto find_sel = [&]() -> int {
        for (size_t i = 0; i < cf_files_.size(); ++i) {
            if (cf_files_[i].path == cf_sel_path_) return static_cast<int>(i);
        }
        return -1;
    };
    if (cf_files_.empty()) {
        cf_sel_path_.clear();
        cf_hunks_.clear();
        cf_segments_.clear();
        cf_show_base_.clear();
        cf_file_is_image_ = false;
        cf_file_is_binary_ = false;
        cf_ours_bmp_.reset();
        cf_theirs_bmp_.reset();
        cf_has_ours_ = false;
        cf_has_theirs_ = false;
        cf_editing_hunk_ = -1;
    } else if (find_sel() < 0) {
        cf_sel_path_ = cf_files_.front().path;
        for (const auto& f : cf_files_) {
            if (!f.resolved && !cf_session_resolved_.contains(f.path)) {
                cf_sel_path_ = f.path;
                break;
            }
        }
        StartConflictFileLoad();
    } else if (cf_hunks_.empty() && !cf_file_is_binary_ && !cf_file_is_image_) {
        StartConflictFileLoad();
    }

    Layout();
    LayoutEdits();
    SyncChildEditsVisibility();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::StartConflictFileLoad() {
    if (cf_file_thread_.joinable()) cf_file_thread_.join();
    const uint64_t gen = ++cf_file_gen_;
    cf_editing_hunk_ = -1;
    cf_ours_bmp_.reset();
    cf_theirs_bmp_.reset();
    cf_has_ours_ = false;
    cf_has_theirs_ = false;
    cf_body_scroll_ = 0.0f;

    if (!hwnd_ || root_.empty() || cf_sel_path_.empty()) {
        cf_hunks_.clear();
        cf_segments_.clear();
        cf_show_base_.clear();
        cf_file_is_image_ = false;
        cf_file_is_binary_ = false;
        SyncChildEditsVisibility();
        return;
    }

    bool is_image = app::GitIsImagePath(cf_sel_path_);
    bool is_binary = is_image;
    for (const auto& f : cf_files_) {
        if (f.path == cf_sel_path_) {
            is_image = f.is_image || is_image;
            is_binary = f.is_binary || is_image;
            break;
        }
    }

    const HWND target = hwnd_;
    const std::wstring root = root_;
    const std::wstring path = cf_sel_path_;

    cf_file_thread_ = std::thread([this, target, root, path, is_image, is_binary, gen]() {
        auto job = std::make_unique<ConflictFileJob>();
        job->generation = gen;
        job->path = path;
        job->is_image = is_image;
        job->is_binary = is_binary;

        constexpr uint64_t kMaxBinBytes = 24ull * 1024 * 1024;
        if (is_image || is_binary) {
            std::string ours_bytes, theirs_bytes;
            job->has_ours = app::GitReadConflictStageBlob(root, 2, path, ours_bytes, kMaxBinBytes);
            job->has_theirs = app::GitReadConflictStageBlob(root, 3, path, theirs_bytes, kMaxBinBytes);
            job->ours_bytes = ours_bytes.size();
            job->theirs_bytes = theirs_bytes.size();
            if (is_image) {
                if (job->has_ours) job->ours_image = DecodeGitImageBytes(ours_bytes, 1024);
                if (job->has_theirs) job->theirs_image = DecodeGitImageBytes(theirs_bytes, 1024);
            }
        } else {
            std::string raw;
            if (ReadWorktreeRaw(app::GitJoinPath(root, path), raw, 8ull * 1024 * 1024)) {
                app::ParseConflictText(raw, job->hunks, job->segments);
                if (job->hunks.size() == 1 && job->hunks[0].base_text.empty()) {
                    std::string base_bytes;
                    if (app::GitReadConflictStageBlob(root, 1, path, base_bytes, 256ull * 1024)) {
                        job->hunks[0].base_utf8 = base_bytes;
                        job->hunks[0].base_text = app::GitUtf8ToWide(base_bytes);
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(cf_mutex_);
            cf_file_pending_ = std::move(job);
        }
        PostMessageW(target, kMsgCfFileDone, 0, 0);
    });
}

void GitWindow::AdoptConflictFileLoad() {
    std::unique_ptr<ConflictFileJob> job;
    {
        std::lock_guard<std::mutex> lock(cf_mutex_);
        job = std::move(cf_file_pending_);
    }
    if (!job || job->generation != cf_file_gen_) return;

    cf_file_is_image_ = job->is_image;
    cf_file_is_binary_ = job->is_binary;
    cf_hunks_ = std::move(job->hunks);
    cf_segments_ = std::move(job->segments);
    cf_show_base_.assign(cf_hunks_.size(), false);
    cf_editing_hunk_ = -1;
    cf_body_scroll_ = 0.0f;

    cf_ours_meta_ = std::move(job->ours_image);
    cf_theirs_meta_ = std::move(job->theirs_image);
    cf_ours_bytes_ = job->ours_bytes;
    cf_theirs_bytes_ = job->theirs_bytes;
    cf_has_ours_ = job->has_ours;
    cf_has_theirs_ = job->has_theirs;

    cf_ours_bmp_.reset();
    cf_theirs_bmp_.reset();
    if (auto* dc = compositor_.Dc()) {
        if (cf_ours_meta_.ok()) cf_ours_bmp_ = CreateBitmapFromDecodedImage(dc, cf_ours_meta_);
        if (cf_theirs_meta_.ok()) cf_theirs_bmp_ = CreateBitmapFromDecodedImage(dc, cf_theirs_meta_);
    }

    for (auto& f : cf_files_) {
        if (f.path == job->path && !cf_hunks_.empty()) {
            f.hunk_count = static_cast<int>(cf_hunks_.size());
        }
    }

    Layout();
    LayoutEdits();
    SyncChildEditsVisibility();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::SyncCfCustomEditBox() {
    if (!cf_custom_edit_ || cf_editing_hunk_ < 0 || cf_editing_hunk_ >= static_cast<int>(cf_hunks_.size())) {
        return;
    }
    const auto& h = cf_hunks_[static_cast<size_t>(cf_editing_hunk_)];
    std::wstring init;
    if (!h.custom_text.empty()) {
        init = h.custom_text;
    } else if (h.choice == app::GitConflictChoice::Ours) {
        init = h.ours_text;
    } else if (h.choice == app::GitConflictChoice::Theirs) {
        init = h.theirs_text;
    } else if (!h.ours_text.empty() && !h.theirs_text.empty()) {
        init = h.ours_text + L"\n" + h.theirs_text;
    } else {
        init = h.ours_text + h.theirs_text;
    }
    const std::wstring crlf = ToCrlf(init);
    SetWindowTextW(cf_custom_edit_, crlf.c_str());
    Layout();
    LayoutEdits();
    SyncChildEditsVisibility();
    SetFocus(cf_custom_edit_);
    const int len = GetWindowTextLengthW(cf_custom_edit_);
    SendMessageW(cf_custom_edit_, EM_SETSEL, len, len);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::ApplyCustomEditBuffer() {
    if (!cf_custom_edit_ || cf_editing_hunk_ < 0 || cf_editing_hunk_ >= static_cast<int>(cf_hunks_.size())) {
        return;
    }
    const int n = GetWindowTextLengthW(cf_custom_edit_);
    std::wstring buf(static_cast<size_t>(n), L'\0');
    if (n > 0) GetWindowTextW(cf_custom_edit_, buf.data(), n + 1);
    auto& h = cf_hunks_[static_cast<size_t>(cf_editing_hunk_)];
    h.custom_text = FromCrlf(buf);
    h.custom_utf8 = app::GitWideToUtf8(h.custom_text);
    h.choice = app::GitConflictChoice::Custom;
    cf_editing_hunk_ = -1;
    Layout();
    LayoutEdits();
    SyncChildEditsVisibility();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void GitWindow::PromptAbortMerge() {
    if (op_busy_) return;
    ConfirmDialogSpec spec;
    spec.title = Str(Sid::GitConflictAbortTitle);
    spec.message = Str(Sid::GitConflictAbortMsg);
    spec.confirm_text = Str(Sid::GitConflictAbortBtn);
    spec.cancel_text = Str(Sid::Cancel);
    spec.danger = true;
    spec.tone = ConfirmTone::Danger;
    spec.default_choice = ConfirmChoice::Cancel;
    spec.note = GitFmt(Str(Sid::GitCmdDetailsSummary), L"{cmd}", L"git merge --abort");
    if (ShowConfirmDialogEx(hwnd_, spec, dark_, accent_) != ConfirmChoice::Confirm) return;
    OpRequest req;
    req.kind = OpKind::AbortMerge;
    StartOp(std::move(req));
}

void GitWindow::LayoutConflict(float w, float h) {
    (void)h;
    const float s = scale_;
    const float bar_h = 46.0f * s;
    cf_bar_ = D2D1::RectF(0, toolbar_.bottom, w, toolbar_.bottom + bar_h);

    const std::wstring abort_t = GitStr(plain_terms_, Sid::GitConflictAbortPlain, Sid::GitConflictAbortGit);
    const std::wstring comp_t = GitStr(plain_terms_, Sid::GitConflictFinishPlain, Sid::GitConflictFinishGit);
    const float abort_w = painter_.MeasureButtonWidth(abort_t, L"\xE711");
    const float comp_w = painter_.MeasureButtonWidth(comp_t, L"\xE73E");
    const float btn_top = cf_bar_.top + 7.0f * s;
    const float btn_bot = btn_top + 32.0f * s;

    float right = w - 14.0f * s;
    cf_complete_rc_ = D2D1::RectF(right - comp_w, btn_top, right, btn_bot);
    right = cf_complete_rc_.left - 8.0f * s;
    cf_abort_rc_ = D2D1::RectF(right - abort_w, btn_top, right, btn_bot);

    const std::wstring banner_t = plain_terms_
        ? Str(Sid::GitConflictBannerPlain)
        : GitFmt(Str(Sid::GitConflictBannerGit), L"{head}",
                 cf_state_.merge_head_short.empty() ? L"MERGE_HEAD" : cf_state_.merge_head_short);
    const std::wstring prog_t = GitFmt(
        GitFmt(GitStr(plain_terms_, Sid::GitConflictProgressPlain, Sid::GitConflictProgressGit),
               L"{done}", std::to_wstring(ConflictResolvedCount())),
        L"{total}", std::to_wstring(ConflictTotalCount()));
    const float prog_left = (std::min)(cf_abort_rc_.left - 180.0f * s,
                                       14.0f * s + Measure(banner_t, ui_bold_.get()) + Measure(prog_t, small_.get()) + 44.0f * s);
    const float prog_right = cf_abort_rc_.left - 16.0f * s;
    if (prog_right - prog_left >= 60.0f * s) {
        const float py = (cf_bar_.top + cf_bar_.bottom) * 0.5f;
        cf_prog_rc_ = D2D1::RectF((std::max)(prog_left, 14.0f * s), py - 3.0f * s, prog_right, py + 3.0f * s);
    } else {
        cf_prog_rc_ = D2D1::RectF(0, 0, 0, 0);
    }

    const float lw = std::clamp(310.0f * s, 250.0f * s, (std::max)(250.0f * s, w * 0.34f));
    cf_left_ = D2D1::RectF(0, cf_bar_.bottom, lw, status_.top);
    cf_right_ = D2D1::RectF(lw, cf_bar_.bottom, w, status_.top);
    cf_body_ = cf_right_;

    cf_file_rects_.resize(cf_files_.size());
    float fy = cf_left_.top + 38.0f * s;
    for (size_t i = 0; i < cf_files_.size(); ++i) {
        cf_file_rects_[i] = D2D1::RectF(cf_left_.left + 8.0f * s, fy, cf_left_.right - 8.0f * s, fy + 44.0f * s);
        fy += 48.0f * s;
    }

    cf_hunk_rects_.clear();
    cf_apply_file_rc_ = D2D1::RectF(0, 0, 0, 0);
    cf_custom_box_ = D2D1::RectF(0, 0, 0, 0);
    cf_custom_apply_ = D2D1::RectF(0, 0, 0, 0);
    cf_bin_ours_rc_ = D2D1::RectF(0, 0, 0, 0);
    cf_bin_theirs_rc_ = D2D1::RectF(0, 0, 0, 0);
    cf_bin_copy_rc_ = D2D1::RectF(0, 0, 0, 0);

    if (cf_sel_path_.empty() || (!merging_ && cf_files_.empty())) {
        cf_body_content_h_ = 0.0f;
        cf_body_scroll_ = 0.0f;
        return;
    }

    const float bx = cf_body_.left + 16.0f * s;
    const float bw = (std::max)(240.0f * s, cf_body_.right - cf_body_.left - 38.0f * s);
    float y = 16.0f * s; // relative to top of cf_body_ content

    // Warning banner height
    y += 42.0f * s + 12.0f * s;

    const float view_top = cf_body_.top;
    auto to_screen_y = [&](float rel_y) { return view_top + rel_y - cf_body_scroll_; };

    if (cf_file_is_image_ || cf_file_is_binary_) {
        const float col_gap = 14.0f * s;
        const float col_w = (std::max)(140.0f * s, (bw - col_gap) * 0.5f);
        const float card_h = cf_file_is_image_ ? 290.0f * s : 190.0f * s;
        const float btn_h = 32.0f * s;
        const float sy_top = to_screen_y(y);
        cf_bin_ours_rc_ = D2D1::RectF(bx, sy_top, bx + col_w, sy_top + card_h + btn_h + 12.0f * s);
        cf_bin_theirs_rc_ = D2D1::RectF(bx + col_w + col_gap, sy_top, bx + 2.0f * col_w + col_gap,
                                        sy_top + card_h + btn_h + 12.0f * s);
        y += card_h + btn_h + 24.0f * s;

        const float sy_chk = to_screen_y(y);
        cf_bin_copy_rc_ = D2D1::RectF(bx, sy_chk, bx + bw, sy_chk + 26.0f * s);
        y += 38.0f * s;
    } else {
        cf_hunk_rects_.resize(cf_hunks_.size());
        const std::wstring mine_t = GitStr(plain_terms_, Sid::GitConflictMinePlain, Sid::GitConflictMineGit);
        const std::wstring theirs_t = GitStr(plain_terms_, Sid::GitConflictTheirsPlain, Sid::GitConflictTheirsGit);
        const std::wstring both_t = GitStr(plain_terms_, Sid::GitConflictBothPlain, Sid::GitConflictBothGit);
        const std::wstring man_t = GitStr(plain_terms_, Sid::GitConflictManualPlain, Sid::GitConflictManualGit);
        const std::wstring base_t = GitStr(plain_terms_, Sid::GitConflictBasePlain, Sid::GitConflictBaseGit);

        const float b_mine_w = Measure(mine_t, small_.get()) + 18.0f * s;
        const float b_theirs_w = Measure(theirs_t, small_.get()) + 18.0f * s;
        const float b_both_w = Measure(both_t, small_.get()) + 18.0f * s;
        const float b_man_w = Measure(man_t, small_.get()) + 18.0f * s;
        const float b_base_w = Measure(base_t, small_.get()) + 18.0f * s;

        for (size_t i = 0; i < cf_hunks_.size(); ++i) {
            const auto& hk = cf_hunks_[i];
            auto& hr = cf_hunk_rects_[i];
            const float card_start_y = y;

            // Header bar buttons right-to-left
            const float h_top = to_screen_y(y + 8.0f * s);
            const float h_bot = h_top + 24.0f * s;
            float rx = bx + bw - 10.0f * s;
            if (!hk.base_text.empty()) {
                hr.base_btn = D2D1::RectF(rx - b_base_w, h_top, rx, h_bot);
                rx = hr.base_btn.left - 6.0f * s;
            }
            hr.custom_btn = D2D1::RectF(rx - b_man_w, h_top, rx, h_bot);
            rx = hr.custom_btn.left - 6.0f * s;
            hr.both_btn = D2D1::RectF(rx - b_both_w, h_top, rx, h_bot);
            rx = hr.both_btn.left - 6.0f * s;
            hr.theirs_btn = D2D1::RectF(rx - b_theirs_w, h_top, rx, h_bot);
            rx = hr.theirs_btn.left - 6.0f * s;
            hr.ours_btn = D2D1::RectF(rx - b_mine_w, h_top, rx, h_bot);

            y += 40.0f * s;
            if (!hk.context_before.empty()) {
                const float ch = (std::max)(22.0f * s, MeasureWrappedHeight(hk.context_before, bw - 24.0f * s, mono_.get()) + 8.0f * s);
                y += ch + 6.0f * s;
            }

            // Two columns: ours & theirs
            const float col_gap = 10.0f * s;
            const float col_w = (std::max)(110.0f * s, (bw - 20.0f * s - col_gap) * 0.5f);
            const float code_w = (std::max)(80.0f * s, col_w - 16.0f * s);
            const float ours_h = MeasureWrappedHeight(hk.ours_text.empty() ? L" " : hk.ours_text, code_w, mono_.get());
            const float theirs_h = MeasureWrappedHeight(hk.theirs_text.empty() ? L" " : hk.theirs_text, code_w, mono_.get());
            const float col_body_h = (std::max)(44.0f * s, (std::max)(ours_h, theirs_h) + 36.0f * s);
            y += col_body_h + 10.0f * s;

            if (i < cf_show_base_.size() && cf_show_base_[i] && !hk.base_text.empty()) {
                const float bh = (std::max)(36.0f * s, MeasureWrappedHeight(hk.base_text, bw - 36.0f * s, mono_.get()) + 30.0f * s);
                y += bh + 8.0f * s;
            }

            if (cf_editing_hunk_ == static_cast<int>(i)) {
                const float sy_box = to_screen_y(y);
                const float edit_h = 110.0f * s;
                cf_custom_box_ = D2D1::RectF(bx + 10.0f * s, sy_box, bx + bw - 10.0f * s, sy_box + edit_h);
                const std::wstring ok_t = Str(Sid::Apply);
                const float ok_w = painter_.MeasureButtonWidth(ok_t, L"\xE73E");
                cf_custom_apply_ = D2D1::RectF(bx + bw - 10.0f * s - ok_w, sy_box + edit_h + 6.0f * s,
                                               bx + bw - 10.0f * s, sy_box + edit_h + 34.0f * s);
                y += edit_h + 42.0f * s;
            }

            if (!hk.context_after.empty()) {
                const float ch = (std::max)(22.0f * s, MeasureWrappedHeight(hk.context_after, bw - 24.0f * s, mono_.get()) + 8.0f * s);
                y += ch + 6.0f * s;
            }

            hr.card_top = D2D1::RectF(bx, to_screen_y(card_start_y), bx + bw, to_screen_y(y + 6.0f * s));
            y += 18.0f * s;
        }

        // Result preview box
        bool all_picked = !cf_hunks_.empty();
        for (const auto& hk : cf_hunks_) {
            if (hk.choice == app::GitConflictChoice::None) {
                all_picked = false;
                break;
            }
        }
        std::wstring preview_text = all_picked
            ? app::GitUtf8ToWide(app::ComposeResolvedText(cf_segments_, cf_hunks_))
            : Str(Sid::GitConflictPickPrompt);
        if (preview_text.size() > 4000) preview_text.resize(4000);
        const float pv_h = (std::max)(56.0f * s, (std::min)(220.0f * s,
            MeasureWrappedHeight(preview_text.empty() ? L" " : preview_text, bw - 24.0f * s, mono_.get()) + 40.0f * s));
        y += pv_h + 12.0f * s;

        const std::wstring apply_t = Str(Sid::GitConflictApplyFile);
        const float apply_w = painter_.MeasureButtonWidth(apply_t, L"\xE73E");
        const float sy_btn = to_screen_y(y);
        cf_apply_file_rc_ = D2D1::RectF(bx + bw - apply_w, sy_btn, bx + bw, sy_btn + 34.0f * s);
        y += 48.0f * s;
    }

    cf_body_content_h_ = y;
    const float max_scroll = (std::max)(0.0f, cf_body_content_h_ - (cf_body_.bottom - cf_body_.top));
    if (cf_body_scroll_ > max_scroll) cf_body_scroll_ = max_scroll;
}

void GitWindow::RenderConflict(ID2D1DeviceContext* dc, const Theme& theme, bool hc, bool painter_ok) {
    // Re-sync layout rectangles with current scroll offset cleanly.
    const D2D1_RECT_F prev_box = cf_custom_box_;
    LayoutConflict(static_cast<float>(compositor_.Width()), static_cast<float>(compositor_.Height()));
    if (cf_editing_hunk_ >= 0 &&
        (std::abs(prev_box.top - cf_custom_box_.top) > 0.5f ||
         std::abs(prev_box.bottom - cf_custom_box_.bottom) > 0.5f)) {
        LayoutEdits();
    }

    const float s = scale_;
    const float line_w = (std::max)(1.0f, std::round(s));
    ConflictColors cc = MakeConflictColors(dark_);
    // Pulse's surfaces, as the Changes view: window colour, content card, sheet.
    cc.panel = theme.bg;
    cc.body = theme.surface_card;
    cc.card = theme.surface_sheet;
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

    // ---- Top conflict progress bar strip ----
    fill(cf_bar_, cc.panel);
    fill(D2D1::RectF(cf_bar_.left, cf_bar_.bottom - line_w, cf_bar_.right, cf_bar_.bottom), theme.stroke_divider);

    const std::wstring banner_t = plain_terms_
        ? Str(Sid::GitConflictBannerPlain)
        : GitFmt(Str(Sid::GitConflictBannerGit), L"{head}",
                 cf_state_.merge_head_short.empty() ? L"MERGE_HEAD" : cf_state_.merge_head_short);
    const float banner_w = Measure(banner_t, ui_bold_.get());
    text_at(banner_t, D2D1::RectF(14.0f * s, cf_bar_.top, 14.0f * s + banner_w + 4.0f, cf_bar_.bottom),
            ui_bold_.get(), theme.text);

    const int done = ConflictResolvedCount();
    const int total = ConflictTotalCount();
    const std::wstring prog_t = GitFmt(
        GitFmt(GitStr(plain_terms_, Sid::GitConflictProgressPlain, Sid::GitConflictProgressGit),
               L"{done}", std::to_wstring(done)),
        L"{total}", std::to_wstring(total));
    const float prog_tx = 28.0f * s + banner_w;
    text_at(prog_t, D2D1::RectF(prog_tx, cf_bar_.top, prog_tx + Measure(prog_t, small_.get()) + 4.0f, cf_bar_.bottom),
            small_.get(), total > 0 && done == total ? cc.ok_ink : theme.text_secondary);

    if (painter_ok && cf_prog_rc_.right > cf_prog_rc_.left) {
        fluent::ProgressSpec ps{};
        ps.bounds = cf_prog_rc_;
        ps.value = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
        painter_.DrawProgressBar(ps);
    }

    const bool can_complete = !op_busy_ && (total > 0 ? done == total : merging_);
    button(cf_abort_rc_, GitStr(plain_terms_, Sid::GitConflictAbortPlain, Sid::GitConflictAbortGit),
           L"\xE711", fluent::ButtonKind::Danger, !op_busy_ && (merging_ || total > 0), HitKind::CfAbort);
    button(cf_complete_rc_, GitStr(plain_terms_, Sid::GitConflictFinishPlain, Sid::GitConflictFinishGit),
           L"\xE73E", fluent::ButtonKind::Primary, can_complete, HitKind::CfComplete);

    if (!merging_ && cf_files_.empty()) {
        fill(D2D1::RectF(0, cf_bar_.bottom, static_cast<float>(compositor_.Width()), status_.top), cc.body);
        if (painter_ok) {
            fluent::EmptyStateSpec empty{};
            empty.bounds = D2D1::RectF(0, cf_bar_.bottom, static_cast<float>(compositor_.Width()), status_.top);
            empty.glyph = L"\xE73E";
            empty.title = Str(Sid::GitConflictNoMerge);
            painter_.DrawEmptyState(empty);
        }
        return;
    }

    // ---- Left conflicted + auto-merged files pane ----
    fill(cf_left_, cc.panel);
    fill(D2D1::RectF(cf_left_.right - line_w, cf_left_.top, cf_left_.right, cf_left_.bottom), theme.stroke_divider);

    const std::wstring list_hdr = GitStr(plain_terms_, Sid::GitConflictNeedPick, Sid::GitConflictFilesGit);
    text_at(list_hdr, D2D1::RectF(cf_left_.left + 14.0f * s, cf_left_.top + 8.0f * s, cf_left_.right - 12.0f * s,
                                  cf_left_.top + 32.0f * s), ui_bold_.get(), theme.text);

    dc->PushAxisAlignedClip(cf_left_, D2D1_ANTIALIAS_MODE_ALIASED);
    float after_y = cf_left_.top + 38.0f * s;
    for (size_t i = 0; i < cf_files_.size(); ++i) {
        const auto& f = cf_files_[i];
        const D2D1_RECT_F& rc = cf_file_rects_[i];
        after_y = rc.bottom + 8.0f * s;
        const bool sel = f.path == cf_sel_path_;
        const bool hot = hover_.kind == HitKind::CfFileRow && hover_.row == static_cast<int>(i);
        const bool res = f.resolved || cf_session_resolved_.contains(f.path);
        if ((sel || hot) && painter_ok) {
            painter_.FillRoundedRect(rc, 6.0f * s, sel ? theme.fill_selected : theme.fill_hover);
            if (sel) {
                painter_.FillRoundedRect(D2D1::RectF(rc.left, rc.top + 8.0f * s, rc.left + 3.0f * s, rc.bottom - 8.0f * s),
                                         1.5f * s, link_ink);
            }
        }

        const std::wstring tag_t = res
            ? Str(Sid::GitConflictResolvedBadge)
            : (f.is_binary || f.is_image)
            ? Str(Sid::GitConflictBinaryCount)
            : GitFmtCount(Str(Sid::GitConflictHunksCount), (std::max)(1, f.hunk_count));
        const float tag_w = painter_ok ? painter_.MeasureTagWidth(tag_t) : 64.0f * s;
        const D2D1_RECT_F tag_rc = D2D1::RectF(rc.right - 8.0f * s - tag_w, rc.top + 11.0f * s,
                                               rc.right - 8.0f * s, rc.bottom - 11.0f * s);
        if (painter_ok) {
            fluent::TagSpec tag{};
            tag.bounds = tag_rc;
            tag.text = tag_t;
            tag.color = res ? cc.ok_ink : cc.warn_ink;
            tag.bordered = false;
            painter_.DrawTag(tag);
        }

        const float text_max = (std::max)(40.0f * s, tag_rc.left - rc.left - 18.0f * s);
        const std::wstring name = fit(FileNameOf(f.path), text_max, ui_bold_.get());
        const std::wstring folder = FolderOf(f.path);
        const float tx = rc.left + 12.0f * s;
        if (folder.empty()) {
            text_at(name, D2D1::RectF(tx, rc.top, tx + text_max, rc.bottom), ui_.get(), theme.text);
        } else {
            text_at(name, D2D1::RectF(tx, rc.top + 4.0f * s, tx + text_max, rc.top + 23.0f * s),
                    ui_.get(), theme.text);
            text_at(fit(folder, text_max, small_.get()),
                    D2D1::RectF(tx, rc.top + 22.0f * s, tx + text_max, rc.bottom - 3.0f * s),
                    small_.get(), theme.text_secondary);
        }
    }

    if (!cf_state_.auto_merged_files.empty() && after_y + 40.0f * s < cf_left_.bottom - 52.0f * s) {
        text_at(Str(Sid::GitConflictAutoMerged),
                D2D1::RectF(cf_left_.left + 14.0f * s, after_y + 4.0f * s, cf_left_.right - 12.0f * s, after_y + 24.0f * s),
                small_bold_.get(), theme.text_secondary);
        after_y += 26.0f * s;
        const size_t max_show = (std::min<size_t>)(cf_state_.auto_merged_files.size(), 8);
        for (size_t i = 0; i < max_show && after_y + 22.0f * s < cf_left_.bottom - 52.0f * s; ++i) {
            const std::wstring line = L"\u2713  " + cf_state_.auto_merged_files[i];
            text_at(fit(line, cf_left_.right - cf_left_.left - 28.0f * s, small_.get()),
                    D2D1::RectF(cf_left_.left + 14.0f * s, after_y, cf_left_.right - 12.0f * s, after_y + 20.0f * s),
                    small_.get(), cc.ok_ink);
            after_y += 22.0f * s;
        }
    }

    // Persistent reassurance note at bottom left
    const std::wstring note_t = GitStr(plain_terms_, Sid::GitConflictNotePlain, Sid::GitConflictNoteGit);
    const D2D1_RECT_F note_rc = D2D1::RectF(cf_left_.left + 12.0f * s, cf_left_.bottom - 52.0f * s,
                                            cf_left_.right - 12.0f * s, cf_left_.bottom - 8.0f * s);
    if (painter_ok) {
        painter_.FillRoundedRect(note_rc, 6.0f * s, theme.surface_card);
        painter_.DrawWrappedCaption(note_t, D2D1::Point2F(note_rc.left + 8.0f * s, note_rc.top + 6.0f * s),
                                    note_rc.right - note_rc.left - 16.0f * s, theme.text_secondary);
    }
    dc->PopAxisAlignedClip();

    // ---- Right conflict resolver body ----
    fill(cf_body_, cc.body);
    if (cf_sel_path_.empty()) return;

    dc->PushAxisAlignedClip(cf_body_, D2D1_ANTIALIAS_MODE_ALIASED);
    const float bx = cf_body_.left + 16.0f * s;
    const float bw = (std::max)(240.0f * s, cf_body_.right - cf_body_.left - 38.0f * s);
    const float view_top = cf_body_.top;
    float y = 16.0f * s;

    // Top explanation banner
    const std::wstring hint_t = (cf_file_is_image_ || cf_file_is_binary_)
        ? Str(Sid::GitConflictBinHint)
        : Str(Sid::GitConflictTextHint);
    const D2D1_RECT_F hint_rc = D2D1::RectF(bx, view_top + y - cf_body_scroll_, bx + bw,
                                            view_top + y - cf_body_scroll_ + 42.0f * s);
    if (painter_ok) {
        painter_.FillRoundedRect(hint_rc, 6.0f * s, cc.warn_bg);
        painter_.StrokeRoundedRect(hint_rc, 6.0f * s, WithAlpha(cc.warn_ink, 0.35f), line_w);
    }
    text_at(fit(cf_sel_path_ + L"  ·  " + hint_t, bw - 24.0f * s, small_.get()),
            D2D1::RectF(hint_rc.left + 12.0f * s, hint_rc.top, hint_rc.right - 12.0f * s, hint_rc.bottom),
            small_.get(), theme.text);
    y += 54.0f * s;

    if (cf_file_is_image_ || cf_file_is_binary_) {
        const float col_gap = 14.0f * s;
        const float col_w = (std::max)(140.0f * s, (bw - col_gap) * 0.5f);
        const float card_h = cf_file_is_image_ ? 290.0f * s : 190.0f * s;

        auto draw_bin_card = [&](const D2D1_RECT_F& outer, bool is_ours, ID2D1Bitmap* bmp,
                                 const DecodedGitImage& meta, uint64_t bytes, bool has_blob, HitKind hit_id) {
            const bool hot = hover_.kind == hit_id;
            const D2D1_COLOR_F ink = is_ours ? cc.ours_ink : cc.theirs_ink;
            const D2D1_COLOR_F tint = is_ours ? cc.ours_tint : cc.theirs_tint;
            const D2D1_RECT_F card = D2D1::RectF(outer.left, outer.top, outer.right, outer.top + card_h);
            if (painter_ok) {
                painter_.FillRoundedRect(card, 8.0f * s, tint);
                painter_.StrokeRoundedRect(card, 8.0f * s, hot ? ink : WithAlpha(ink, 0.45f), hot ? 2.0f * s : line_w);
            }
            const std::wstring role = is_ours
                ? GitStr(plain_terms_, Sid::GitConflictMinePlain, Sid::GitConflictMineGit)
                : GitStr(plain_terms_, Sid::GitConflictTheirsPlain, Sid::GitConflictTheirsGit);
            const std::wstring hdr = FormatSideHeader(
                role, is_ours ? cf_state_.ours_author : cf_state_.theirs_author,
                is_ours ? cf_state_.ours_time : cf_state_.theirs_time);
            text_at(fit(hdr, col_w - 20.0f * s, ui_bold_.get()),
                    D2D1::RectF(card.left + 10.0f * s, card.top + 8.0f * s, card.right - 10.0f * s, card.top + 32.0f * s),
                    ui_bold_.get(), ink);

            if (cf_file_is_image_ && bmp && meta.ok()) {
                const D2D1_RECT_F img_area = D2D1::RectF(card.left + 14.0f * s, card.top + 38.0f * s,
                                                         card.right - 14.0f * s, card.bottom - 34.0f * s);
                const float aw = (std::max)(1.0f, img_area.right - img_area.left);
                const float ah = (std::max)(1.0f, img_area.bottom - img_area.top);
                const float scale_fit = (std::min)(aw / static_cast<float>(meta.orig_w),
                                                   ah / static_cast<float>(meta.orig_h));
                const float dw = static_cast<float>(meta.orig_w) * scale_fit;
                const float dh = static_cast<float>(meta.orig_h) * scale_fit;
                const float dx = img_area.left + (aw - dw) * 0.5f;
                const float dy = img_area.top + (ah - dh) * 0.5f;
                dc->DrawBitmap(bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.0f,
                               D2D1_INTERPOLATION_MODE_LINEAR);
                const std::wstring info = std::to_wstring(meta.orig_w) + L" × " + std::to_wstring(meta.orig_h) +
                                          L" · " + FormatByteCount(bytes);
                text_at(info, D2D1::RectF(card.left + 10.0f * s, card.bottom - 28.0f * s,
                                          card.right - 10.0f * s, card.bottom - 6.0f * s),
                        small_.get(), theme.text_secondary);
            } else {
                if (painter_ok) {
                    painter_.DrawGlyph(L"\xE8A5", D2D1::RectF(card.left, card.top + 48.0f * s, card.right, card.top + 96.0f * s),
                                       ink);
                }
                const std::wstring info = has_blob ? FormatByteCount(bytes) : L"—";
                text_at(info, D2D1::RectF(card.left + 14.0f * s, card.top + 110.0f * s, card.right - 14.0f * s, card.bottom - 14.0f * s),
                        ui_.get(), theme.text_secondary);
            }

            const D2D1_RECT_F pick_btn = D2D1::RectF(outer.left, card.bottom + 8.0f * s, outer.right, outer.bottom);
            button(pick_btn, Str(Sid::GitConflictUseThis), L"\xE73E",
                   is_ours ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard, !op_busy_, hit_id);
        };

        draw_bin_card(cf_bin_ours_rc_, true, cf_ours_bmp_.get(), cf_ours_meta_, cf_ours_bytes_, cf_has_ours_,
                      HitKind::CfBinPickOurs);
        draw_bin_card(cf_bin_theirs_rc_, false, cf_theirs_bmp_.get(), cf_theirs_meta_, cf_theirs_bytes_, cf_has_theirs_,
                      HitKind::CfBinPickTheirs);

        if (painter_ok && cf_bin_copy_rc_.right > cf_bin_copy_rc_.left) {
            fluent::ControlState cs{};
            cs.enabled = true;
            cs.checked = cf_save_other_copy_;
            cs.hovered = hover_.kind == HitKind::CfBinSaveCopyToggle;
            cs.pressed = pressed_.kind == HitKind::CfBinSaveCopyToggle && cs.hovered;
            painter_.DrawCheckBox(cf_bin_copy_rc_, Str(Sid::GitConflictSaveOther), cs);
        }
    } else {
        const std::wstring mine_t = GitStr(plain_terms_, Sid::GitConflictMinePlain, Sid::GitConflictMineGit);
        const std::wstring theirs_t = GitStr(plain_terms_, Sid::GitConflictTheirsPlain, Sid::GitConflictTheirsGit);
        const std::wstring both_t = GitStr(plain_terms_, Sid::GitConflictBothPlain, Sid::GitConflictBothGit);
        const std::wstring man_t = GitStr(plain_terms_, Sid::GitConflictManualPlain, Sid::GitConflictManualGit);
        const std::wstring base_t = GitStr(plain_terms_, Sid::GitConflictBasePlain, Sid::GitConflictBaseGit);

        auto pill_btn = [&](const D2D1_RECT_F& rc, const std::wstring& label, bool active, HitKind id, int idx) {
            if (rc.right <= rc.left || !painter_ok) return;
            const bool hot = hover_.kind == id && hover_.row == idx;
            const D2D1_COLOR_F bg = active ? link_ink : (hot ? theme.fill_hover : theme.surface_card);
            const D2D1_COLOR_F fg = active ? theme.accent_text : theme.text;
            painter_.FillRoundedRect(rc, 5.0f * s, bg);
            if (!active) painter_.StrokeRoundedRect(rc, 5.0f * s, theme.stroke_card, line_w);
            painter_.DrawText(label, rc, small_.get(), fg, fluent::HorizontalAlignment::Center);
        };

        for (size_t i = 0; i < cf_hunks_.size() && i < cf_hunk_rects_.size(); ++i) {
            const auto& hk = cf_hunks_[i];
            const auto& hr = cf_hunk_rects_[i];
            const int idx = static_cast<int>(i);
            const D2D1_RECT_F& card = hr.card_top;

            if (painter_ok) {
                painter_.FillRoundedRect(card, 8.0f * s, cc.card);
                painter_.StrokeRoundedRect(card, 8.0f * s,
                                           hk.choice != app::GitConflictChoice::None ? WithAlpha(cc.ok_ink, 0.55f)
                                                                                     : theme.stroke_card,
                                           line_w);
            }

            const std::wstring title = L"#" + std::to_wstring(i + 1);
            text_at(title, D2D1::RectF(card.left + 12.0f * s, card.top + 8.0f * s, card.left + 120.0f * s, card.top + 32.0f * s),
                    ui_bold_.get(), theme.text);

            pill_btn(hr.ours_btn, mine_t, hk.choice == app::GitConflictChoice::Ours, HitKind::CfPickOurs, idx);
            pill_btn(hr.theirs_btn, theirs_t, hk.choice == app::GitConflictChoice::Theirs, HitKind::CfPickTheirs, idx);
            pill_btn(hr.both_btn, both_t, hk.choice == app::GitConflictChoice::Both, HitKind::CfPickBoth, idx);
            pill_btn(hr.custom_btn, man_t, hk.choice == app::GitConflictChoice::Custom || cf_editing_hunk_ == idx,
                     HitKind::CfToggleCustom, idx);
            if (hr.base_btn.right > hr.base_btn.left) {
                const bool show_b = i < cf_show_base_.size() && cf_show_base_[i];
                pill_btn(hr.base_btn, base_t, show_b, HitKind::CfToggleBase, idx);
            }

            float cy = card.top + 40.0f * s;
            if (!hk.context_before.empty()) {
                const float ch = (std::max)(22.0f * s, MeasureWrappedHeight(hk.context_before, bw - 24.0f * s, mono_.get()) + 8.0f * s);
                text_at(hk.context_before, D2D1::RectF(card.left + 12.0f * s, cy, card.right - 12.0f * s, cy + ch),
                        mono_.get(), theme.text_secondary);
                cy += ch + 6.0f * s;
            }

            const float col_gap = 10.0f * s;
            const float col_w = (std::max)(110.0f * s, (bw - 20.0f * s - col_gap) * 0.5f);
            const float code_w = (std::max)(80.0f * s, col_w - 16.0f * s);
            const float ours_h = MeasureWrappedHeight(hk.ours_text.empty() ? L" " : hk.ours_text, code_w, mono_.get());
            const float theirs_h = MeasureWrappedHeight(hk.theirs_text.empty() ? L" " : hk.theirs_text, code_w, mono_.get());
            const float col_h = (std::max)(44.0f * s, (std::max)(ours_h, theirs_h) + 36.0f * s);

            const D2D1_RECT_F ours_box = D2D1::RectF(card.left + 10.0f * s, cy, card.left + 10.0f * s + col_w, cy + col_h);
            const D2D1_RECT_F theirs_box = D2D1::RectF(ours_box.right + col_gap, cy, ours_box.right + col_gap + col_w, cy + col_h);

            auto draw_side = [&](const D2D1_RECT_F& box, const std::wstring& label, const std::wstring& body,
                                 const D2D1_COLOR_F& tint, const D2D1_COLOR_F& ink, bool picked) {
                if (painter_ok) {
                    painter_.FillRoundedRect(box, 6.0f * s, tint);
                    painter_.StrokeRoundedRect(box, 6.0f * s, picked ? ink : WithAlpha(ink, 0.35f), picked ? 2.0f * s : line_w);
                }
                text_at(fit(label, col_w - 16.0f * s, small_bold_.get()),
                        D2D1::RectF(box.left + 8.0f * s, box.top + 6.0f * s, box.right - 8.0f * s, box.top + 24.0f * s),
                        small_bold_.get(), ink);
                text_at(body.empty() ? L"(empty)" : body,
                        D2D1::RectF(box.left + 8.0f * s, box.top + 26.0f * s, box.right - 8.0f * s, box.bottom - 6.0f * s),
                        mono_.get(), theme.text);
            };

            draw_side(ours_box,
                      FormatSideHeader(mine_t, hk.ours_label.empty() ? cf_state_.ours_author : hk.ours_label, cf_state_.ours_time),
                      hk.ours_text, cc.ours_tint, cc.ours_ink,
                      hk.choice == app::GitConflictChoice::Ours || hk.choice == app::GitConflictChoice::Both);
            draw_side(theirs_box,
                      FormatSideHeader(theirs_t, hk.theirs_label.empty() ? cf_state_.theirs_author : hk.theirs_label, cf_state_.theirs_time),
                      hk.theirs_text, cc.theirs_tint, cc.theirs_ink,
                      hk.choice == app::GitConflictChoice::Theirs || hk.choice == app::GitConflictChoice::Both);
            cy += col_h + 10.0f * s;

            if (i < cf_show_base_.size() && cf_show_base_[i] && !hk.base_text.empty()) {
                const float bh = (std::max)(36.0f * s, MeasureWrappedHeight(hk.base_text, bw - 36.0f * s, mono_.get()) + 30.0f * s);
                const D2D1_RECT_F brc = D2D1::RectF(card.left + 10.0f * s, cy, card.right - 10.0f * s, cy + bh);
                if (painter_ok) {
                    painter_.FillRoundedRect(brc, 6.0f * s, cc.base_tint);
                    painter_.StrokeRoundedRect(brc, 6.0f * s, theme.stroke_divider, line_w);
                }
                text_at(base_t, D2D1::RectF(brc.left + 8.0f * s, brc.top + 5.0f * s, brc.right - 8.0f * s, brc.top + 22.0f * s),
                        small_bold_.get(), theme.text_secondary);
                text_at(hk.base_text, D2D1::RectF(brc.left + 8.0f * s, brc.top + 24.0f * s, brc.right - 8.0f * s, brc.bottom - 5.0f * s),
                        mono_.get(), theme.text);
                cy += bh + 8.0f * s;
            }

            if (cf_editing_hunk_ == idx && painter_ok) {
                painter_.DrawTextFieldFrame(cf_custom_box_, {true, false, false, true}, true);
                button(cf_custom_apply_, Str(Sid::Apply), L"\xE73E", fluent::ButtonKind::Primary, true, HitKind::CfCustomApply);
                cy = cf_custom_apply_.bottom + 8.0f * s;
            }

            if (!hk.context_after.empty()) {
                const float ch = (std::max)(22.0f * s, MeasureWrappedHeight(hk.context_after, bw - 24.0f * s, mono_.get()) + 8.0f * s);
                text_at(hk.context_after, D2D1::RectF(card.left + 12.0f * s, cy, card.right - 12.0f * s, cy + ch),
                        mono_.get(), theme.text_secondary);
            }
        }

        // Result preview panel + Apply file button
        bool all_picked = !cf_hunks_.empty();
        for (const auto& hk : cf_hunks_) {
            if (hk.choice == app::GitConflictChoice::None) {
                all_picked = false;
                break;
            }
        }
        std::wstring pv_text = all_picked
            ? app::GitUtf8ToWide(app::ComposeResolvedText(cf_segments_, cf_hunks_))
            : Str(Sid::GitConflictPickPrompt);
        if (pv_text.size() > 4000) pv_text.resize(4000);
        const float pv_h = (std::max)(56.0f * s, (std::min)(220.0f * s,
            MeasureWrappedHeight(pv_text.empty() ? L" " : pv_text, bw - 24.0f * s, mono_.get()) + 40.0f * s));
        const float pv_top = cf_apply_file_rc_.top - pv_h - 12.0f * s;
        const D2D1_RECT_F pv_rc = D2D1::RectF(bx, pv_top, bx + bw, pv_top + pv_h);
        if (painter_ok) {
            painter_.FillRoundedRect(pv_rc, 8.0f * s, cc.panel);
            painter_.StrokeRoundedRect(pv_rc, 8.0f * s, all_picked ? cc.ok_ink : theme.stroke_card, line_w);
        }
        text_at(GitStr(plain_terms_, Sid::GitConflictPreviewPlain, Sid::GitConflictPreviewGit),
                D2D1::RectF(pv_rc.left + 12.0f * s, pv_rc.top + 8.0f * s, pv_rc.right - 12.0f * s, pv_rc.top + 28.0f * s),
                small_bold_.get(), all_picked ? cc.ok_ink : theme.text_secondary);
        text_at(pv_text, D2D1::RectF(pv_rc.left + 12.0f * s, pv_rc.top + 30.0f * s, pv_rc.right - 12.0f * s, pv_rc.bottom - 8.0f * s),
                mono_.get(), all_picked ? theme.text : theme.text_secondary);

        button(cf_apply_file_rc_, Str(Sid::GitConflictApplyFile), L"\xE73E",
               fluent::ButtonKind::Primary, !op_busy_ && all_picked, HitKind::CfApplyFile);
    }

    if (painter_ok && cf_body_content_h_ > cf_body_.bottom - cf_body_.top) {
        fluent::ScrollbarSpec bar{cf_body_, cf_body_scroll_, cf_body_.bottom - cf_body_.top, cf_body_content_h_};
        bar.expand_progress = (hover_.kind == HitKind::CfBodyBar || drag_bar_ == 5) ? 1.0f : 0.0f;
        painter_.DrawScrollbar(bar);
    }
    dc->PopAxisAlignedClip();
}

GitWindow::Hit GitWindow::HitTestConflict(float x, float y) const {
    if (Inside(cf_abort_rc_, x, y)) return {HitKind::CfAbort, -1};
    if (Inside(cf_complete_rc_, x, y)) return {HitKind::CfComplete, -1};

    for (size_t i = 0; i < cf_file_rects_.size(); ++i) {
        if (Inside(cf_file_rects_[i], x, y)) return {HitKind::CfFileRow, static_cast<int>(i)};
    }

    if (Inside(cf_body_, x, y)) {
        fluent::ScrollbarSpec bar{cf_body_, cf_body_scroll_, cf_body_.bottom - cf_body_.top, cf_body_content_h_};
        if (OnScrollbar(bar, x, y) && cf_body_content_h_ > cf_body_.bottom - cf_body_.top)
            return {HitKind::CfBodyBar, -1};
        if (Inside(cf_bin_ours_rc_, x, y)) return {HitKind::CfBinPickOurs, -1};
        if (Inside(cf_bin_theirs_rc_, x, y)) return {HitKind::CfBinPickTheirs, -1};
        if (Inside(cf_bin_copy_rc_, x, y)) return {HitKind::CfBinSaveCopyToggle, -1};
        if (Inside(cf_custom_apply_, x, y)) return {HitKind::CfCustomApply, -1};
        if (Inside(cf_apply_file_rc_, x, y)) return {HitKind::CfApplyFile, -1};

        for (size_t i = 0; i < cf_hunk_rects_.size(); ++i) {
            const auto& hr = cf_hunk_rects_[i];
            const int idx = static_cast<int>(i);
            if (Inside(hr.ours_btn, x, y)) return {HitKind::CfPickOurs, idx};
            if (Inside(hr.theirs_btn, x, y)) return {HitKind::CfPickTheirs, idx};
            if (Inside(hr.both_btn, x, y)) return {HitKind::CfPickBoth, idx};
            if (Inside(hr.custom_btn, x, y)) return {HitKind::CfToggleCustom, idx};
            if (Inside(hr.base_btn, x, y)) return {HitKind::CfToggleBase, idx};
        }
    }
    return {};
}

bool GitWindow::OnClickConflict(const Hit& hit, float, float y) {
    switch (hit.kind) {
    case HitKind::CfBodyBar: {
        fluent::ScrollbarSpec bar{cf_body_, cf_body_scroll_, cf_body_.bottom - cf_body_.top, cf_body_content_h_};
        const D2D1_RECT_F thumb = fluent::ScrollbarThumbRect(bar, scale_, HighContrastOn());
        drag_bar_ = 5;
        drag_grab_ = (y >= thumb.top && y < thumb.bottom) ? (y - thumb.top) : (thumb.bottom - thumb.top) * 0.5f;
        SetCapture(hwnd_);
        return true;
    }
    case HitKind::CfAbort:
        PromptAbortMerge();
        return true;
    case HitKind::CfComplete: {
        const int total = ConflictTotalCount();
        const int done = ConflictResolvedCount();
        if (!op_busy_ && (total > 0 ? done == total : merging_)) {
            OpRequest req;
            req.kind = OpKind::CompleteMerge;
            StartOp(std::move(req));
        }
        return true;
    }
    case HitKind::CfFileRow:
        if (hit.row >= 0 && hit.row < static_cast<int>(cf_files_.size())) {
            const std::wstring& next_path = cf_files_[static_cast<size_t>(hit.row)].path;
            if (next_path != cf_sel_path_) {
                cf_sel_path_ = next_path;
                StartConflictFileLoad();
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return true;
    case HitKind::CfPickOurs:
    case HitKind::CfPickTheirs:
    case HitKind::CfPickBoth:
        if (hit.row >= 0 && hit.row < static_cast<int>(cf_hunks_.size())) {
            if (cf_editing_hunk_ == hit.row) {
                cf_editing_hunk_ = -1;
                SyncChildEditsVisibility();
            }
            auto& hk = cf_hunks_[static_cast<size_t>(hit.row)];
            hk.choice = hit.kind == HitKind::CfPickOurs ? app::GitConflictChoice::Ours
                      : hit.kind == HitKind::CfPickTheirs ? app::GitConflictChoice::Theirs
                      : app::GitConflictChoice::Both;
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return true;
    case HitKind::CfToggleCustom:
        if (hit.row >= 0 && hit.row < static_cast<int>(cf_hunks_.size())) {
            cf_editing_hunk_ = (cf_editing_hunk_ == hit.row) ? -1 : hit.row;
            if (cf_editing_hunk_ >= 0) {
                SyncCfCustomEditBox();
            } else {
                Layout();
                LayoutEdits();
                SyncChildEditsVisibility();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return true;
    case HitKind::CfToggleBase:
        if (hit.row >= 0 && hit.row < static_cast<int>(cf_show_base_.size())) {
            cf_show_base_[static_cast<size_t>(hit.row)] = !cf_show_base_[static_cast<size_t>(hit.row)];
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return true;
    case HitKind::CfCustomApply:
        ApplyCustomEditBuffer();
        return true;
    case HitKind::CfApplyFile:
        if (!op_busy_ && !cf_sel_path_.empty() && !cf_hunks_.empty()) {
            bool all_picked = true;
            for (const auto& hk : cf_hunks_) {
                if (hk.choice == app::GitConflictChoice::None) {
                    all_picked = false;
                    break;
                }
            }
            if (all_picked) {
                OpRequest req;
                req.kind = OpKind::ResolveTextConflict;
                req.arg1 = cf_sel_path_;
                req.patch_bytes = app::ComposeResolvedText(cf_segments_, cf_hunks_);
                StartOp(std::move(req));
            }
        }
        return true;
    case HitKind::CfBinSaveCopyToggle:
        cf_save_other_copy_ = !cf_save_other_copy_;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    case HitKind::CfBinPickOurs:
    case HitKind::CfBinPickTheirs:
        if (!op_busy_ && !cf_sel_path_.empty()) {
            OpRequest req;
            req.kind = OpKind::ResolveBinaryConflict;
            req.arg1 = cf_sel_path_;
            req.flag1 = hit.kind == HitKind::CfBinPickOurs;
            req.flag2 = cf_save_other_copy_;
            req.arg2 = Str(Sid::GitConflictOtherSuffix);
            StartOp(std::move(req));
        }
        return true;
    default:
        break;
    }
    return false;
}

}  // namespace pulse::ui
