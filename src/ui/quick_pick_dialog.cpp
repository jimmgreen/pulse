#include "quick_pick_dialog.h"
#include "dialog_lifecycle.h"
#include "edit_host.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "store_icons.h"
#include "typography.h"
#include "ui_compositor.h"
#include "window_helpers.h"
#include "../common/localization.h"
#include "../common/picker_owner_guard.h"
#include "../common/windows_compat.h"
#include "../index/filename_pinyin.h"

#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>

namespace pulse::ui {
namespace {

constexpr wchar_t kClass[] = L"PulseQuickPickDialogWindow";
constexpr float kDlgW = 540.0f;
constexpr float kTitleH = 36.0f;
constexpr float kPad = 16.0f;
constexpr float kSearchTop = 48.0f;
constexpr float kSearchH = 36.0f;
constexpr float kListTop = 94.0f;
constexpr float kBottomPad = 14.0f;
constexpr int kMaxVisibleRows = 8;

constexpr int kHitClose = 1;
constexpr int kHitField = 2;
constexpr int kHitRowBase = 100;

std::wstring FoldCase(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

std::wstring_view TrimWs(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
    return text;
}

std::wstring AsciiWide(std::string_view ascii) {
    std::wstring out;
    out.reserve(ascii.size());
    for (const char c : ascii) {
        out.push_back((c == '.' || c == '_' || c == ':' || c == '/' || c == '-') ? L' ' : static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

struct RankedRow {
    size_t item_index = 0;
    int tier = 0;
    uint32_t match_start = 0;
    uint32_t match_length = 0;
};

RankedRow ScoreItem(const QuickPickDialogItem& item, size_t index, const std::wstring& needle) {
    if (needle.empty()) {
        return {index, 1, 0, 0};
    }
    const std::wstring title = FoldCase(item.title);
    if (title == needle) {
        return {index, 6, 0, static_cast<uint32_t>((std::min)(title.size(), size_t{0xFFFFFFFFu}))};
    }
    if (title.starts_with(needle)) {
        return {index, 5, 0, static_cast<uint32_t>((std::min)(needle.size(), size_t{0xFFFFFFFFu}))};
    }
    RankedRow pinyin{index, 0, 0, 0};
    if (index::PinyinEligible(needle)) {
        const index::PinyinMatch m = index::FindPinyinMatch(item.title, needle);
        if (m.kind == index::PinyinMatchKind::Full && m.start == 0) {
            return {index, 4, m.start, m.length};
        }
        if (m.kind == index::PinyinMatchKind::Initials && m.start == 0) {
            return {index, 3, m.start, m.length};
        }
        if (m) {
            pinyin = {index, 2, m.start, m.length};
        }
    }
    if (const size_t at = title.find(needle); at != std::wstring::npos) {
        return {index, 2, static_cast<uint32_t>((std::min)(at, size_t{0xFFFFFFFFu})),
                static_cast<uint32_t>((std::min)(needle.size(), size_t{0xFFFFFFFFu}))};
    }
    if (pinyin.tier > 0) return pinyin;
    if (!item.detail.empty() && FoldCase(item.detail).find(needle) != std::wstring::npos) {
        return {index, 1, 0, 0};
    }
    if (FoldCase(AsciiWide(item.id)).find(needle) != std::wstring::npos) {
        return {index, 1, 0, 0};
    }
    return {index, 0, 0, 0};
}

class QuickPickWindow {
public:
    QuickPickDialogOutcome Show(HWND owner, const QuickPickDialogSpec& spec, bool dark, D2D1_COLOR_F accent) {
        QuickPickDialogOutcome outcome{};
        if (owner && !IsWindow(owner)) {
            outcome.status = QuickPickDialogStatus::Failed;
            outcome.error = ERROR_INVALID_WINDOW_HANDLE;
            return outcome;
        }
        if (spec.items.empty()) {
            outcome.status = QuickPickDialogStatus::Failed;
            outcome.error = ERROR_INVALID_PARAMETER;
            return outcome;
        }
        if (spec.cancel_event && WaitForSingleObject(spec.cancel_event, 0) == WAIT_OBJECT_0) {
            outcome.status = QuickPickDialogStatus::Cancelled;
            return outcome;
        }

        std::unique_ptr<pulse::PickerOwnerGuard> owner_guard;
        if (owner) {
            owner_guard = std::make_unique<pulse::PickerOwnerGuard>(owner);
            if (owner_guard->Error() != ERROR_SUCCESS) {
                outcome.status = QuickPickDialogStatus::Failed;
                outcome.error = owner_guard->Error();
                return outcome;
            }
            if (!owner_guard->Alive()) {
                outcome.status = QuickPickDialogStatus::Cancelled;
                return outcome;
            }
        }

        owner_ = owner;
        spec_ = &spec;
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow())) / 96.0f;

        has_details_ = false;
        for (const auto& item : spec.items) {
            if (!item.detail.empty()) {
                has_details_ = true;
                break;
            }
        }
        row_h_ = has_details_ ? 46.0f : 34.0f;
        const int visible_slots = std::clamp(static_cast<int>(spec.items.size()), 3, kMaxVisibleRows);
        list_h_ = static_cast<float>(visible_slots) * row_h_;
        dlg_h_ = kListTop + list_h_ + kBottomPad;

        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);

        const std::wstring window_title = spec.title.empty()
            ? std::wstring(l10n::Get(l10n::StringId::GlobalSearchSelect))
            : spec.title;
        const int width_px = static_cast<int>(std::lround(kDlgW * scale_));
        const int height_px = static_cast<int>(std::lround(dlg_h_ * scale_));
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass, window_title.c_str(),
                                WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, width_px, height_px,
                                owner, nullptr, wc.hInstance, this);
        if (!hwnd_) {
            outcome.status = QuickPickDialogStatus::Failed;
            outcome.error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
            return outcome;
        }

        RebuildFilter(spec.initial_query, true);
        CenterOwnedWindow(hwnd_, owner_, width_px, height_px);
        ShowWindow(hwnd_, SW_SHOW);
        LayoutEdit();
        SetForegroundWindow(hwnd_);
        if (edit_) {
            SetFocus(edit_);
            const int len = GetWindowTextLengthW(edit_);
            SendMessageW(edit_, EM_SETSEL, len, len);
        }

        MSG msg{};
        while (!done_) {
            if ((owner_guard && !owner_guard->Alive()) ||
                (spec.cancel_event && WaitForSingleObject(spec.cancel_event, 0) == WAIT_OBJECT_0)) {
                accepted_ = false;
                done_ = true;
                break;
            }
            if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(msg.wParam));
                    accepted_ = false;
                    done_ = true;
                    break;
                }
                RedirectStrayModalKey(msg, hwnd_);
                const bool edit_msg = edit_ && msg.hwnd == edit_;
                if (!edit_msg && msg.message != WM_KEYDOWN && IsDialogMessageW(hwnd_, &msg)) continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                continue;
            }
            const DWORD handle_count = spec.cancel_event ? 1u : 0u;
            const HANDLE* handles = spec.cancel_event ? &spec.cancel_event : nullptr;
            MsgWaitForMultipleObjectsEx(handle_count, handles, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }

        if (IsWindow(hwnd_)) {
            if (owner_guard) owner_guard->RestoreBeforeDialogHide(hwnd_);
            HideComposedDialog(hwnd_);
            DestroyEdit();
            DestroyWindow(hwnd_);
        } else {
            DestroyEdit();
        }
        hwnd_ = nullptr;
        owner_guard.reset();

        if (accepted_ && !chosen_id_.empty()) {
            outcome.status = QuickPickDialogStatus::Accepted;
            outcome.selected_id = std::move(chosen_id_);
        } else {
            outcome.status = QuickPickDialogStatus::Cancelled;
        }
        return outcome;
    }

private:
    D2D1_RECT_F CloseRect() const { return DipRect(scale_, kDlgW - 46.0f, 0.0f, 46.0f, kTitleH); }
    D2D1_RECT_F FieldRect() const { return DipRect(scale_, kPad, kSearchTop, kDlgW - 2.0f * kPad, kSearchH); }
    D2D1_RECT_F ListViewport() const { return DipRect(scale_, kPad, kListTop, kDlgW - 2.0f * kPad, list_h_); }

    D2D1_COLOR_F EditForeground() const { return ColorFromRef(EditTextColor(dark_)); }
    D2D1_COLOR_F EditBackground() const { return ColorFromRef(EditBackColor(dark_)); }

    float MaxScrollPx() const {
        const float content_px = static_cast<float>(filtered_.size()) * row_h_ * scale_;
        const float view_px = list_h_ * scale_;
        return (std::max)(0.0f, content_px - view_px);
    }

    void ClampScroll() {
        scroll_px_ = std::clamp(scroll_px_, 0.0f, MaxScrollPx());
    }

    void ScrollSelectedIntoView() {
        if (selected_ < 0 || selected_ >= static_cast<int>(filtered_.size())) return;
        const float row_px = row_h_ * scale_;
        const float view_px = list_h_ * scale_;
        const float top = static_cast<float>(selected_) * row_px;
        const float bottom = top + row_px;
        if (top < scroll_px_) scroll_px_ = top;
        else if (bottom > scroll_px_ + view_px) scroll_px_ = bottom - view_px;
        ClampScroll();
    }

    void RebuildFilter(std::wstring_view raw_query, bool initial) {
        query_ = std::wstring(raw_query);
        const std::wstring needle = FoldCase(TrimWs(raw_query));
        filtered_.clear();
        filtered_.reserve(spec_->items.size());
        for (size_t i = 0; i < spec_->items.size(); ++i) {
            const RankedRow r = ScoreItem(spec_->items[i], i, needle);
            if (r.tier > 0) filtered_.push_back(r);
        }
        if (!needle.empty()) {
            std::stable_sort(filtered_.begin(), filtered_.end(), [&](const RankedRow& a, const RankedRow& b) {
                const bool da = spec_->items[a.item_index].disabled;
                const bool db = spec_->items[b.item_index].disabled;
                if (da != db) return !da;
                return a.tier > b.tier;
            });
        }
        selected_ = -1;
        if (initial) {
            for (size_t i = 0; i < filtered_.size(); ++i) {
                const auto& item = spec_->items[filtered_[i].item_index];
                if (item.selected && !item.disabled) {
                    selected_ = static_cast<int>(i);
                    break;
                }
            }
        }
        if (selected_ < 0) {
            for (size_t i = 0; i < filtered_.size(); ++i) {
                if (!spec_->items[filtered_[i].item_index].disabled) {
                    selected_ = static_cast<int>(i);
                    break;
                }
            }
        }
        scroll_px_ = 0.0f;
        ScrollSelectedIntoView();
    }

    void MoveSelection(int step) {
        if (filtered_.empty() || step == 0) return;
        const int n = static_cast<int>(filtered_.size());
        int cur = selected_ < 0 ? (step > 0 ? -1 : n) : selected_;
        for (int tries = 0; tries < n; ++tries) {
            cur = (cur + (step > 0 ? 1 : -1) + n) % n;
            if (!spec_->items[filtered_[static_cast<size_t>(cur)].item_index].disabled) {
                selected_ = cur;
                ScrollSelectedIntoView();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
    }

    void JumpSelection(bool to_end) {
        if (filtered_.empty()) return;
        const int n = static_cast<int>(filtered_.size());
        if (!to_end) {
            for (int i = 0; i < n; ++i) {
                if (!spec_->items[filtered_[static_cast<size_t>(i)].item_index].disabled) {
                    selected_ = i;
                    ScrollSelectedIntoView();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
        } else {
            for (int i = n - 1; i >= 0; --i) {
                if (!spec_->items[filtered_[static_cast<size_t>(i)].item_index].disabled) {
                    selected_ = i;
                    ScrollSelectedIntoView();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
        }
    }

    void PageSelection(int dir) {
        const int page = (std::max)(1, static_cast<int>(std::floor(list_h_ / row_h_)) - 1);
        for (int i = 0; i < page; ++i) MoveSelection(dir);
    }

    void AcceptCurrent() {
        if (selected_ < 0 || selected_ >= static_cast<int>(filtered_.size())) return;
        const auto& item = spec_->items[filtered_[static_cast<size_t>(selected_)].item_index];
        if (item.disabled) return;
        chosen_id_ = item.id;
        accepted_ = true;
        done_ = true;
    }

    void Cancel() {
        accepted_ = false;
        done_ = true;
    }

    void CreateEdit() {
        font_ = typography::CreateEditFont(scale_, 14.0f);
        edit_ = CreateChildEdit(hwnd_, spec_->initial_query.c_str(), 0);
        if (!edit_) return;
        SetWindowTheme(edit_, L"", L"");
        if (font_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowSubclass(edit_, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    void LayoutEdit() {
        if (done_ || !edit_ || !hwnd_) return;
        const D2D1_RECT_F cell = FieldRect();
        const int left_px = static_cast<int>(std::lround(cell.left + 34.0f * scale_));
        const int right_px = static_cast<int>(std::lround(cell.right - 46.0f * scale_));
        const int w = (std::max)(40, right_px - left_px);
        const int cell_h = (std::max)(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
        const int line_h = EditLineHeight(edit_, font_, cell_h);
        const int top_px = static_cast<int>(std::lround(cell.top)) + (std::max)(0, (cell_h - line_h) / 2);
        RECT prev{};
        GetWindowRect(edit_, &prev);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&prev), 2);
        const bool changed = !IsWindowVisible(edit_) || prev.left != left_px || prev.top != top_px ||
                             (prev.right - prev.left) != w || (prev.bottom - prev.top) != line_h;
        SetWindowPos(edit_, HWND_TOP, left_px, top_px, w, line_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (changed) {
            PresentChildEdit(compositor_, compositor_.TextFormat(), EditForeground(), EditBackground(), edit_);
        }
    }

    void DestroyEdit() {
        if (edit_) {
            DestroyWindow(edit_);
            edit_ = nullptr;
        }
    }

    std::wstring ReadEditText() const {
        if (!edit_) return {};
        const int n = GetWindowTextLengthW(edit_);
        std::wstring text(static_cast<size_t>(n), L'\0');
        if (n > 0) GetWindowTextW(edit_, text.data(), n + 1);
        return text;
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<QuickPickWindow*>(ref);
        if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { self->Cancel(); return 0; }
            if (wparam == VK_RETURN) { self->AcceptCurrent(); return 0; }
            if (wparam == VK_DOWN) { self->MoveSelection(1); return 0; }
            if (wparam == VK_UP) { self->MoveSelection(-1); return 0; }
            if (wparam == VK_NEXT) { self->PageSelection(1); return 0; }
            if (wparam == VK_PRIOR) { self->PageSelection(-1); return 0; }
            if (wparam == VK_TAB) return 0;
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) return 0;
            break;
        }
        const bool focus_ring = msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK;
        LRESULT result = 0;
        if (!HandleChildEditMessage(self->compositor_, self->compositor_.TextFormat(), self->EditForeground(),
                                    self->EditBackground(), EditBackBrush(self->edit_brush_), hwnd, msg, wparam,
                                    lparam, result)) {
            result = DefPresentedChildEditProc(self->compositor_, self->compositor_.TextFormat(), self->EditForeground(),
                                               self->EditBackground(), hwnd, msg, wparam, lparam);
        }
        if (focus_ring && self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }

    int HitTest(float x, float y) const {
        if (ContainsRect(CloseRect(), x, y)) return kHitClose;
        if (ContainsRect(FieldRect(), x, y)) return kHitField;
        const D2D1_RECT_F view = ListViewport();
        if (ContainsRect(view, x, y) && !filtered_.empty()) {
            const float rel_y = (y - view.top) + scroll_px_;
            const int idx = static_cast<int>(std::floor(rel_y / (row_h_ * scale_)));
            if (idx >= 0 && idx < static_cast<int>(filtered_.size())) {
                return kHitRowBase + idx;
            }
        }
        return 0;
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        auto* dc = compositor_.Dc();
        if (!dc) return;

        const bool hc = IsHighContrast();
        const Theme theme = hc ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        BeginSurface(compositor_, painter_, theme, dark_, hc, backdrop_, scale_);

        const std::wstring title = spec_->title.empty()
            ? std::wstring(l10n::Get(l10n::StringId::GlobalSearchSelect))
            : spec_->title;
        painter_.DrawText(title, DipRect(scale_, kPad, 0.0f, kDlgW - 64.0f, kTitleH),
                          compositor_.SmallFormat(), theme.text);

        fluent::ControlState close_state{};
        close_state.hovered = hover_ == kHitClose;
        close_state.pressed = pressed_ == kHitClose;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);
        painter_.FillRoundedRect(DipRect(scale_, 0.0f, kTitleH, kDlgW, 1.0f), 0.0f, theme.stroke_divider);

        fluent::CommandSearchBoxSpec search_spec{};
        search_spec.bounds = FieldRect();
        search_spec.placeholder = spec_->placeholder.empty()
            ? l10n::Get(l10n::StringId::StoreCatSearch)
            : std::wstring_view(spec_->placeholder);
        search_spec.keycap = L"Esc";
        search_spec.state.focused = edit_ && GetFocus() == edit_;
        search_spec.state.hovered = hover_ == kHitField;
        search_spec.text = query_;
        search_spec.suppress_text = true;
        search_spec.hosted_edit = true;
        painter_.DrawCommandSearchBox(search_spec);

        const D2D1_RECT_F viewport = ListViewport();
        dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);

        if (filtered_.empty()) {
            painter_.DrawText(l10n::Get(l10n::StringId::GlobalSearchNoResults), viewport,
                              compositor_.TextFormat(), theme.text_secondary,
                              fluent::HorizontalAlignment::Center);
        } else {
            const float row_px = row_h_ * scale_;
            const float scroll_pad = MaxScrollPx() > 0.0f ? 10.0f * scale_ : 0.0f;
            for (size_t i = 0; i < filtered_.size(); ++i) {
                const float top = viewport.top + static_cast<float>(i) * row_px - scroll_px_;
                const float bottom = top + row_px;
                if (bottom <= viewport.top || top >= viewport.bottom) continue;

                const auto& entry = filtered_[i];
                const auto& item = spec_->items[entry.item_index];
                const D2D1_RECT_F row_bounds{viewport.left, top, viewport.right - scroll_pad, bottom};
                const bool is_sel = static_cast<int>(i) == selected_ && !item.disabled;
                const bool is_hov = hover_ == (kHitRowBase + static_cast<int>(i)) && !item.disabled;

                if (is_sel || is_hov) {
                    const D2D1_RECT_F plate{row_bounds.left, row_bounds.top + 1.0f * scale_,
                                            row_bounds.right, row_bounds.bottom - 1.0f * scale_};
                    painter_.FillRoundedRect(plate, 6.0f * scale_,
                                             is_sel ? theme.fill_input_hover : theme.fill_hover);
                    if (is_sel) {
                        const float pill_h = 16.0f * scale_;
                        const float cy = (plate.top + plate.bottom) * 0.5f;
                        painter_.FillRoundedRect(
                            D2D1::RectF(plate.left + 2.0f * scale_, cy - pill_h * 0.5f,
                                        plate.left + 5.0f * scale_, cy + pill_h * 0.5f),
                            1.5f * scale_, theme.accent);
                    }
                }

                float left_x = row_bounds.left + 12.0f * scale_;
                if (!item.glyph.empty()) {
                    const float icon_sz = 16.0f * scale_;
                    const float cy = (row_bounds.top + row_bounds.bottom) * 0.5f;
                    const D2D1_RECT_F icon_box{left_x, cy - icon_sz * 0.5f, left_x + icon_sz, cy + icon_sz * 0.5f};
                    const D2D1_COLOR_F icon_color = item.disabled ? theme.text_disabled : theme.text_secondary;
                    if (!DrawExtensionIconGlyph(dc, item.glyph, icon_box, icon_color, &theme)) {
                        painter_.DrawGlyph(item.glyph, icon_box, icon_color);
                    }
                    left_x += 24.0f * scale_;
                }

                float right_x = row_bounds.right - 10.0f * scale_;
                if (!item.shortcut.empty()) {
                    const float badge_w = painter_.MeasureBadgeWidth(item.shortcut);
                    const float badge_h = 20.0f * scale_;
                    const float cy = (row_bounds.top + row_bounds.bottom) * 0.5f;
                    fluent::BadgeSpec badge{};
                    badge.bounds = D2D1::RectF(right_x - badge_w, cy - badge_h * 0.5f, right_x, cy + badge_h * 0.5f);
                    badge.text = item.shortcut;
                    badge.kind = fluent::BadgeKind::Keycap;
                    painter_.DrawBadge(badge);
                    right_x = badge.bounds.left - 8.0f * scale_;
                }

                const D2D1_COLOR_F title_color = item.disabled ? theme.text_disabled : theme.text;
                const D2D1_COLOR_F detail_color = item.disabled ? theme.text_disabled : theme.text_secondary;
                if (has_details_ && !item.detail.empty()) {
                    const D2D1_RECT_F title_rc{left_x, row_bounds.top + 5.0f * scale_,
                                               (std::max)(left_x, right_x), row_bounds.top + 24.0f * scale_};
                    const D2D1_RECT_F detail_rc{left_x, row_bounds.top + 24.0f * scale_,
                                                (std::max)(left_x, right_x), row_bounds.bottom - 4.0f * scale_};
                    painter_.DrawText(item.title, title_rc, compositor_.TextFormat(), title_color);
                    painter_.DrawText(item.detail, detail_rc, compositor_.SmallFormat(), detail_color);
                } else {
                    const D2D1_RECT_F title_rc{left_x, row_bounds.top, (std::max)(left_x, right_x), row_bounds.bottom};
                    painter_.DrawText(item.title, title_rc, compositor_.TextFormat(), title_color);
                }
            }
        }

        dc->PopAxisAlignedClip();

        if (MaxScrollPx() > 0.0f) {
            fluent::ScrollbarSpec sb{};
            sb.viewport = viewport;
            sb.offset = scroll_px_;
            sb.viewport_extent = viewport.bottom - viewport.top;
            sb.content_extent = static_cast<float>(filtered_.size()) * row_h_ * scale_;
            sb.opacity = 0.85f;
            painter_.DrawScrollbar(sb);
        }

        EndSurface(compositor_);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<QuickPickWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<QuickPickWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(msg, wparam, lparam) : DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    LRESULT Handle(UINT msg, WPARAM wparam, LPARAM lparam) {
        switch (msg) {
        case WM_CREATE:
            backdrop_ = ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateEdit();
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST: {
            const LRESULT hit = BorderlessHitTest(hwnd_, lparam, kTitleH * scale_, CloseRect());
            return hit == HTCAPTION || hit == HTCLIENT ? hit : HTCLIENT;
        }
        case WM_DPICHANGED: {
            const float prev_scale = scale_ > 0.0f ? scale_ : 1.0f;
            scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
            if (scale_ <= 0.0f) scale_ = 1.0f;
            scroll_px_ = scroll_px_ * (scale_ / prev_scale);
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            if (font_) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            font_ = typography::CreateEditFont(scale_, 14.0f);
            if (edit_ && font_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            if (const auto* suggested = reinterpret_cast<const RECT*>(lparam)) {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ClampScroll();
            LayoutEdit();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            LayoutEdit();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            LayoutEdit();
            return 0;
        case WM_CTLCOLOREDIT: {
            const HDC hdc = reinterpret_cast<HDC>(wparam);
            SetTextColor(hdc, EditTextColor(dark_));
            SetBkColor(hdc, EditBackColor(dark_));
            return reinterpret_cast<LRESULT>(EditBackBrush(edit_brush_));
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE) {
                RebuildFilter(ReadEditText(), false);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            scroll_px_ -= (static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA)) * (row_h_ * scale_ * 2.5f);
            ClampScroll();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int next = HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) {
                hover_ = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const int hit = HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (hit == kHitField) {
                if (edit_) SetFocus(edit_);
                pressed_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (hit >= kHitRowBase) {
                const int idx = hit - kHitRowBase;
                if (idx >= 0 && idx < static_cast<int>(filtered_.size()) &&
                    !spec_->items[filtered_[static_cast<size_t>(idx)].item_index].disabled) {
                    selected_ = idx;
                }
            }
            pressed_ = hit;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const int hit = HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            const int was = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit != 0 && hit == was) {
                if (hit == kHitClose) {
                    Cancel();
                    return 0;
                }
                if (hit >= kHitRowBase) {
                    const int idx = hit - kHitRowBase;
                    if (idx >= 0 && idx < static_cast<int>(filtered_.size()) &&
                        !spec_->items[filtered_[static_cast<size_t>(idx)].item_index].disabled) {
                        selected_ = idx;
                        AcceptCurrent();
                        return 0;
                    }
                }
            }
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { Cancel(); return 0; }
            if (wparam == VK_RETURN) { AcceptCurrent(); return 0; }
            if (wparam == VK_DOWN) { MoveSelection(1); return 0; }
            if (wparam == VK_UP) { MoveSelection(-1); return 0; }
            if (wparam == VK_NEXT) { PageSelection(1); return 0; }
            if (wparam == VK_PRIOR) { PageSelection(-1); return 0; }
            if (wparam == VK_HOME) { JumpSelection(false); return 0; }
            if (wparam == VK_END) { JumpSelection(true); return 0; }
            break;
        case WM_CLOSE:
            Cancel();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            DestroyEdit();
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            compositor_.Shutdown();
            hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    const QuickPickDialogSpec* spec_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    std::wstring query_;
    std::vector<RankedRow> filtered_;
    std::string chosen_id_;
    bool dark_ = false;
    bool backdrop_ = false;
    bool has_details_ = false;
    bool done_ = false;
    bool accepted_ = false;
    float scale_ = 1.0f;
    float row_h_ = 34.0f;
    float list_h_ = 200.0f;
    float dlg_h_ = 308.0f;
    float scroll_px_ = 0.0f;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    int hover_ = 0;
    int pressed_ = 0;
    int selected_ = -1;
};

} // namespace

QuickPickDialogOutcome ShowQuickPickDialog(HWND owner, const QuickPickDialogSpec& spec,
                                           bool dark, D2D1_COLOR_F accent) {
    QuickPickWindow window;
    return window.Show(owner, spec, dark, accent);
}

} // namespace pulse::ui
