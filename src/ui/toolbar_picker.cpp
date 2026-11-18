#include "../common/windows_compat.h"
#include "toolbar_picker.h"

#include "fluent_components.h"
#include "typography.h"
#include "ui_compositor.h"
#include "ui_motion.h"
#include "window_helpers.h"

#include <d2d1effects.h>
#pragma comment(lib, "dxguid.lib")
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <utility>
#if defined(PULSE_WITH_SELFTEST) || defined(PULSE_WITH_TEST_HOOKS)
#include <filesystem>
#include <fstream>
#endif

namespace pulse::ui {
namespace {

constexpr wchar_t kPickerClass[] = L"PulseToolbarPicker";
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT_PTR kPrefetchTimer = 2;
constexpr UINT_PTR kCaretTimer = 3;
constexpr UINT kDestroyMessage = WM_APP + 41;

// Geometry in DIPs, as in the prototype (#pop, .tile, .ptabs).
constexpr float kMargin = 28.0f;     // around the card: shadow, notch, pop overshoot
constexpr float kCardW = 476.0f;
constexpr float kPadX = 14.0f;
constexpr float kTitleY = 14.0f, kTitleH = 24.0f;
constexpr float kTabsY = 48.0f, kTabsH = 30.0f;
constexpr float kDescY = 86.0f;
constexpr float kPadBottom = 12.0f;
constexpr float kFootH = 36.0f;      // under the grid: a divider, then the 快捷键 row
constexpr float kGridMax = 280.0f;   // remaining rows scroll
constexpr int kMaxCols = 3;
constexpr float kTileMinW = 136.0f, kTileGap = 8.0f;
constexpr float kLabelY = 40.0f;     // below the icon, switch and separator control
constexpr float kSecH = 24.0f, kSecGap = 12.0f;
constexpr float kSearchW = 150.0f;
constexpr float kRadius = 12.0f;
// Pages, in tab order.
constexpr int kTools = 0, kSel = 1, kApps = 2, kPages = 3;

bool g_open = false;
class PickerWindow;
PickerWindow* g_window = nullptr;


float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
D2D1::Matrix3x2F M(const D2D1_MATRIX_3X2_F& m) { return *D2D1::Matrix3x2F::ReinterpretBaseType(&m); }

// A value that springs towards its target (integrated, so it can be
// retargeted mid-flight and keeps its velocity).
struct Spring1 {
    float v = 0.0f, vel = 0.0f, to = 0.0f;
    bool live = false;
    void Snap(float x) { v = to = x; vel = 0.0f; live = false; }
    void To(float x) { if (x != to) { to = x; live = true; } }
    bool Step(float dt, motion::SpringSpec s) {
        if (!live) return false;
        while (dt > 0.0f) {
            const float h = std::min(dt, 0.004f);
            vel += (s.k * (to - v) - s.c * vel) * h;
            v += vel * h;
            dt -= h;
        }
        if (std::fabs(to - v) < 0.2f && std::fabs(vel) < 6.0f) Snap(to);
        return live;
    }
};

enum class Part { None, Outside, Card, Done, Tab, Search, Tile, Switch, Add, Keys, Break };
struct Hit {
    Part part = Part::None;
    int index = -1;
    bool operator==(const Hit& o) const { return part == o.part && index == o.index; }
    bool operator!=(const Hit& o) const { return !(*this == o); }
};

struct TileAnim {
    Spring1 x, y;
    Spring1 gone;                // 0 shown, 1 filtered out
    bool placed = false;
    uint64_t check_at = 0;       // checked state changed
    uint64_t switch_at = 0;
    uint64_t shake_at = 0;
};

struct PageState {
    std::vector<TileAnim> tiles;   // tools: one per pin; apps: one per app + the add tile
    std::vector<Spring1> heads;    // section headings (tools)
    std::vector<bool> head_shown;
    float content_h = 0.0f;
    size_t visible = 0;
    // Per section (one on the selection and apps pages): its top and the
    // shown tiles in display order (the apps page's add tile not included).
    std::vector<float> sec_top;
    std::vector<std::vector<size_t>> sec_list;
};

// A tile being dragged to a new place (positions in DIPs, grid space).
struct TileDrag {
    int page = -1;
    size_t tile = 0;
    POINT start{};
    float ox = 0.0f, oy = 0.0f, scroll0 = 0.0f;
    int last_x = 0;
    uint64_t moved_at = 0;
    bool active = false;
    std::vector<size_t> before;   // the page order when the drag started
};

// Device bitmaps of the app icons; dropped with the device.
struct IconBitmaps {
    ID2D1DeviceContext* dc = nullptr;
    std::vector<std::pair<common::MenuIconRef, ComPtr<ID2D1Bitmap>>> list;
    ID2D1Bitmap* For(ID2D1DeviceContext* target, const common::MenuIconRef& pixels) {
        if (!target || !pixels || !pixels->Valid()) return nullptr;
        if (target != dc) { list.clear(); dc = target; }
        for (const auto& [key, bitmap] : list)
            if (key == pixels) return bitmap.get();
        ComPtr<ID2D1Bitmap> bitmap;
        const auto props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(target->CreateBitmap(D2D1::SizeU(pixels->width, pixels->height), pixels->bgra.data(),
                                        pixels->width * 4u, props, &bitmap)))
            bitmap.reset();
        list.emplace_back(pixels, std::move(bitmap));
        return list.back().second.get();
    }
};

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

class PickerWindow {
public:
    void Run(HWND owner, ToolbarCustomizeSpec& spec, bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        spec_ = &spec;
        dark_ = dark;
        accent_ = accent;
        page_ = std::clamp(spec.page, 0, kPages - 1);
        InitOrder(kTools, spec.pins_order, spec.pins.size());
        InitOrder(kSel, spec.selection_order, spec.selection.size());
        InitOrder(kApps, {}, spec.apps.size());
        animations_ = motion::SystemAnimationsEnabled() && !IsHighContrast();
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow())) / 96.0f;
        Place();

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kPickerClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kPickerClass, &wc)) RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, kPickerClass, spec.title.c_str(),
                                WS_POPUP, win_.left, win_.top, win_.right - win_.left, win_.bottom - win_.top,
                                owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return;
        g_window = this;
        // Text metrics need the compositor's factory; size the hidden window
        // again before showing it so wrapping never changes under the pointer.
        EnsureFormats();
        Place();
        SetWindowPos(hwnd_, nullptr, win_.left, win_.top, win_.right - win_.left, win_.bottom - win_.top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        compositor_.Resize(win_.right - win_.left, win_.bottom - win_.top);
        apps_loaded_ = !spec.load_apps;
        if (page_ == kApps) EnsureApps();
        for (int p = 0; p < kPages; ++p) Layout(p, false);
        grid_h_.Snap(TargetGridH());
        tab_x_.Snap(TabRect(page_).left);
        tab_w_.Snap(TabRect(page_).right - TabRect(page_).left);
        opened_at_ = motion::NowMs();
        enter_at_ = opened_at_;
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        if (!apps_loaded_) SetTimer(hwnd_, kPrefetchTimer, 650, nullptr);
        live_ = true;

        MSG message{};
        bool quit = false;
        WPARAM quit_code = 0;
        while (hwnd_) {
            // WM_TIMER and WM_PAINT come after every posted message. While the
            // owner animates on its frame pump (the hovered tile's pin breathes
            // a ring), they starved and the picker froze mid-animation: a check
            // badge stuck half-popped, or never appeared after the first one.
            // An overdue frame is drawn here, between messages.
            if (frame_due_ && motion::NowMs() >= frame_due_ + 8) {
                frame_due_ = 0;
                KillTimer(hwnd_, kFrameTimer);
                Render();
                ValidateRect(hwnd_, nullptr);
                continue;
            }
            const BOOL got = GetMessageW(&message, nullptr, 0, 0);
            if (got <= 0) { quit = got == 0; quit_code = message.wParam; if (hwnd_) DestroyWindow(hwnd_); break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (quit) PostQuitMessage(static_cast<int>(quit_code));
    }

#if defined(PULSE_WITH_SELFTEST) || defined(PULSE_WITH_TEST_HOOKS)
    HWND Probe(const wchar_t* keyword, POINT* center, const wchar_t* snapshot) {
        if (snapshot) { Render(); if (!compositor_.SaveSnapshot(snapshot)) return nullptr; }
        if (keyword && center) {
            for (size_t i = 0; i < spec_->pins.size(); ++i) {
                if (spec_->pins[i].keywords != keyword) continue;
                const auto r = TileRect(kTools, i);
                *center = POINT{static_cast<LONG>((r.left+r.right)/2), static_cast<LONG>((r.top+r.bottom)/2)};
                return page_ == kTools && Inside(GridRect(), static_cast<float>(center->x), static_cast<float>(center->y)) ? hwnd_ : nullptr;
            }
            return nullptr;
        }
        return hwnd_;
    }

    bool ProbeLayout(const wchar_t* report, const wchar_t* snapshot_prefix) {
        std::ofstream log{std::filesystem::path(report)};
        if (!log) return false;
        int failures = 0;
        const auto check = [&](bool ok, const char* what) {
            log << (ok ? "[PASS] " : "[FAIL] ") << what << '\n';
            if (!ok) ++failures;
        };
        const float saved_scale = scale_, saved_scroll = scroll_;
        const int saved_page = page_;
        const bool saved_dark = dark_, saved_animations = animations_;
        const auto saved_query = query_;
        const Hit saved_hover = hover_;
        animations_ = false;
        hover_ = {};
        query_.clear();
        page_ = kTools;
        for (int dpi : {96, 144, 192}) for (bool narrow : {false, true}) {
            scale_ = static_cast<float>(dpi) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            EnsureFormats();
            // Explicit physical-pixel work areas exercise the production sizing
            // path without changing desktop settings or relying on its monitor.
            const RECT work{0, 0, narrow ? 640 : 1920, 1080};
            PlaceInWorkArea(work);
            for (int p = 0; p < kPages; ++p) Layout(p, false);
            grid_h_.Snap(TargetGridH());
            tab_x_.Snap(TabRect(page_).left);
            tab_w_.Snap(TabRect(page_).right - TabRect(page_).left);
            painter_.SetScale(scale_);
            compositor_.Resize(win_.right - win_.left, win_.bottom - win_.top);
            log << "dpi=" << dpi << " narrow=" << narrow << " columns=" << cols_ << " tile_width=" << TileW() << '\n';
            const D2D1_RECT_F card = Card();
            check(card.left + static_cast<float>(win_.left) >= work.left &&
                  card.right + static_cast<float>(win_.left) <= work.right + 1.0f &&
                  card.top + static_cast<float>(win_.top) >= work.top &&
                  card.bottom + static_cast<float>(win_.top) <= work.bottom + 1.0f, "card fits work area");
            check(cols_ >= 1 && cols_ <= kMaxCols && TileW() >= kTileMinW, "adaptive columns retain readable tile width");
            const auto fits = [&](const std::wstring& text, IDWriteTextFormat* format, float width, float height) {
                if (text.empty()) return true;
                auto* layout = WrappedLayoutFor(text, format, S(width), S(height),
                    format == desc_fmt_.get() ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
                if (!layout) return false;
                DWRITE_TEXT_METRICS metrics{};
                if (FAILED(layout->GetMetrics(&metrics)) || metrics.height > S(height) || metrics.width > S(width) + 0.5f) return false;
                UINT32 count = 0;
                layout->GetLineMetrics(nullptr, 0, &count);
                std::vector<DWRITE_LINE_METRICS> lines(count);
                if (FAILED(layout->GetLineMetrics(lines.data(), count, &count))) return false;
                return std::none_of(lines.begin(), lines.end(), [](const auto& line) { return line.isTrimmed != FALSE; });
            };
            for (int p = kTools; p <= kSel; ++p) {
                for (size_t i = 0; i < Tiles(p).size(); ++i) {
                    log << "page=" << p << " tile=" << i << ' ';
                    check(fits(Tiles(p)[i].label, tile_fmt_.get(), TileW() - 10.0f, label_h_[p]), "full built-in label without trimming");
                    check(fits(Tiles(p)[i].badge, note_fmt_.get(), TileW() - 8.0f, note_h_[p]), "conditional note fits below label");
                }
            }
            check(fits(spec_->on_toolbar_text, note_fmt_.get(), TileW() - 8.0f, note_h_[kSel]), "on-toolbar note fits");
            const float content_w = card_w_ - 2 * kPadX;
            const float desc_w = search_stacked_ ? content_w : content_w - kSearchW - 8.0f;
            for (const auto* text : {&spec_->tools_desc, &spec_->tools_full_text, &spec_->add_break_text, &spec_->remove_break_text})
                check(fits(*text, desc_fmt_.get(), desc_w, desc_h_), "full tools description/hint fits");
            for (const auto* text : {&spec_->sel_desc, &spec_->apps_desc, &spec_->apps_full_text})
                check(fits(*text, desc_fmt_.get(), content_w, desc_h_), "full selection/apps description fits");
            check(SearchRect().bottom < GridRect().top, "search stays above grid");
            // Show the appearance controls that originally lost their English
            // labels. All other built-ins are checked above, even off screen.
            scroll_ = 0.0f;
            for (size_t i = 0; i < spec_->pins.size(); ++i)
                if (spec_->pins[i].keywords.starts_with(L"hidden")) {
                    scroll_ = std::clamp(S(pages_[kTools].tiles[i].y.to), 0.0f, MaxScroll());
                    break;
                }
            for (bool dark : {false, true}) {
                dark_ = dark;
                Render();
                if (snapshot_prefix && *snapshot_prefix) {
                    const std::wstring file = std::wstring(snapshot_prefix) + L"-" + std::to_wstring(dpi) +
                        (narrow ? L"-narrow" : L"-normal") + (dark ? L"-dark.png" : L"-light.png");
                    check(compositor_.SaveSnapshot(file.c_str()), "picker snapshot saved");
                }
            }
        }
        scale_ = saved_scale;
        page_ = saved_page;
        dark_ = saved_dark;
        animations_ = saved_animations;
        query_ = saved_query;
        hover_ = saved_hover;
        compositor_.RecreateTextFormats(scale_);
        EnsureFormats();
        Place();
        for (int p = 0; p < kPages; ++p) Layout(p, false);
        grid_h_.Snap(TargetGridH());
        tab_x_.Snap(TabRect(page_).left);
        tab_w_.Snap(TabRect(page_).right - TabRect(page_).left);
        scroll_ = std::clamp(saved_scroll, 0.0f, MaxScroll());
        painter_.SetScale(scale_);
        compositor_.Resize(win_.right - win_.left, win_.bottom - win_.top);
        Frame();
        log << "failures=" << failures << '\n';
        return failures == 0;
    }
#endif
private:
    // ---- geometry -------------------------------------------------------
    float S(float dip) const { return dip * scale_; }
    D2D1_RECT_F Card() const {
        const float h = S(grid_y_ + kFootH + kPadBottom) + grid_h_.v;
        return D2D1::RectF(S(kMargin), S(kMargin), S(kMargin + card_w_), S(kMargin) + h);
    }
    float TileW() const { return (card_w_ - 2 * kPadX - kTileGap * (cols_ - 1)) / cols_; }
    float TileH(int page) const { return kLabelY + label_h_[page] + note_h_[page] + 8.0f; }
    D2D1_RECT_F DoneRect() const {
        const float w = done_w_ > 0 ? done_w_ : S(48.0f);
        const D2D1_RECT_F c = Card();
        return D2D1::RectF(c.right - S(kPadX) - w, c.top + S(kTitleY), c.right - S(kPadX), c.top + S(kTitleY + kTitleH));
    }
    D2D1_RECT_F TabsRect() const {
        const D2D1_RECT_F c = Card();
        return D2D1::RectF(c.left + S(kPadX), c.top + S(kTabsY), c.right - S(kPadX), c.top + S(kTabsY + kTabsH));
    }
    D2D1_RECT_F TabRect(int i) const {
        const D2D1_RECT_F t = TabsRect();
        const float w = (t.right - t.left - S(4.0f)) / static_cast<float>(kPages);
        const float x = t.left + S(2.0f) + w * static_cast<float>(i);
        return D2D1::RectF(x, t.top + S(2.0f), x + w, t.bottom - S(2.0f));
    }
    D2D1_RECT_F SearchRect() const {
        const D2D1_RECT_F c = Card();
        const float top = c.top + S(search_stacked_ ? kDescY + desc_h_ + 6.0f : kDescY + (desc_h_ - 26.0f) * 0.5f);
        const float width = std::min(kSearchW, card_w_ - 2 * kPadX);
        return D2D1::RectF(c.right - S(kPadX + width), top, c.right - S(kPadX), top + S(26.0f));
    }
    D2D1_RECT_F GridRect() const {
        const D2D1_RECT_F c = Card();
        return D2D1::RectF(c.left + S(kPadX - 6.0f), c.top + S(grid_y_ - 6.0f), c.right - S(kPadX - 6.0f),
                           c.top + S(grid_y_) + grid_h_.v);
    }
    // The 快捷键 row under the grid (spec.keys_text).
    D2D1_RECT_F KeysRect() const {
        const D2D1_RECT_F c = Card();
        const float top = c.top + S(grid_y_) + grid_h_.v + S(kFootH - 26.0f);
        return D2D1::RectF(c.left + S(kPadX - 6.0f), top, c.right - S(kPadX - 6.0f), top + S(26.0f));
    }
    D2D1_POINT_2F GridOrigin() const {
        const D2D1_RECT_F c = Card();
        return D2D1::Point2F(c.left + S(kPadX), c.top + S(grid_y_) - scroll_);
    }
    std::vector<ToolbarPickerTile>& Tiles(int page) {
        return page == kTools ? spec_->pins : page == kSel ? spec_->selection : spec_->apps;
    }
    const std::vector<ToolbarPickerTile>& Tiles(int page) const {
        return page == kTools ? spec_->pins : page == kSel ? spec_->selection : spec_->apps;
    }
    size_t TileCount(int page) const {
        return Tiles(page).size() + (page == kApps && spec_->add_app ? 1u : 0u);
    }
    bool IsAdd(int page, size_t i) const { return page == kApps && i >= spec_->apps.size(); }
    // A selection action whose command is pinned as a tool: the chip skips it.
    bool OnToolbar(int page, size_t i) const {
        if (page != kSel || i >= spec_->selection.size()) return false;
        const int tool = spec_->selection[i].tool;
        return tool >= 0 && static_cast<size_t>(tool) < spec_->pins.size() && spec_->pins[static_cast<size_t>(tool)].checked;
    }
    D2D1_RECT_F TileRect(int page, size_t i, float dx = 0.0f) const {
        const D2D1_POINT_2F o = GridOrigin();
        const TileAnim& t = pages_[page].tiles[i];
        const float x = o.x + S(t.x.v) + dx, y = o.y + S(t.y.v);
        return D2D1::RectF(x, y, x + S(TileW()), y + S(TileH(page)));
    }
    // The separator button, right of the icon: a short bar, accent when set.
    D2D1_RECT_F BreakRect(const D2D1_RECT_F& tile) const {
        return D2D1::RectF(tile.right - S(22.0f), tile.top + S(23.0f), tile.right - S(6.0f), tile.top + S(39.0f));
    }
    int BreakState(int page, size_t i) const {
        if (page == kSel || !spec_->break_state || IsAdd(page, i) || i >= Tiles(page).size() ||
            !Tiles(page)[i].checked)
            return -1;
        return spec_->break_state(page, i, Tiles(page)[i].value);
    }
    void ToggleBreak(size_t i) {
        if (BreakState(page_, i) < 0 || !spec_->toggle_break) return;
        spec_->toggle_break(page_, i, Tiles(page_)[i].value);
        Frame();
    }

    D2D1_RECT_F SwitchRect(const D2D1_RECT_F& tile) const {
        return D2D1::RectF(tile.left + S(7.0f), tile.top + S(7.0f), tile.left + S(29.0f), tile.top + S(20.0f));
    }

    void MeasureContent() {
        const float content_w = std::max(1.0f, card_w_ - 2 * kPadX);
        search_stacked_ = content_w - kSearchW - 8.0f < 200.0f;
        const float tools_desc_w = search_stacked_ ? content_w : content_w - kSearchW - 8.0f;
        const auto height = [&](IDWriteTextFormat* format, const std::wstring& text, float width) {
            return text.empty() ? 0.0f :
                std::ceil(typography::MeasureWrapped(compositor_.DwriteFactory(), format, text, S(width)) / scale_) + 2.0f;
        };
        desc_h_ = 26.0f;
        for (const auto* text : {&spec_->tools_desc, &spec_->tools_full_text, &spec_->add_break_text, &spec_->remove_break_text})
            desc_h_ = std::max(desc_h_, height(desc_fmt_.get(), *text, tools_desc_w));
        for (const auto* text : {&spec_->sel_desc, &spec_->apps_desc, &spec_->apps_full_text})
            desc_h_ = std::max(desc_h_, height(desc_fmt_.get(), *text, content_w));
        grid_y_ = kDescY + desc_h_ + (search_stacked_ ? 32.0f : 0.0f) + 10.0f;

        // Measure every built-in label, not just visible/checked tiles. Filtering
        // and toggling must not resize rows or invalidate drag targets.
        for (int page = kTools; page <= kSel; ++page) {
            label_h_[page] = 34.0f;
            note_h_[page] = page == kSel ? height(note_fmt_.get(), spec_->on_toolbar_text, TileW() - 8.0f) : 0.0f;
            for (const auto& tile : Tiles(page)) {
                label_h_[page] = std::max(label_h_[page], height(tile_fmt_.get(), tile.label, TileW() - 10.0f));
                note_h_[page] = std::max(note_h_[page], height(note_fmt_.get(), tile.badge, TileW() - 8.0f));
            }
        }
        // Arbitrary application names remain bounded, unlike built-in commands.
        label_h_[kApps] = std::max(34.0f, height(tile_fmt_.get(), spec_->add_app_text, TileW() - 10.0f));
        note_h_[kApps] = 15.0f;
    }

    // Screen placement: prefer the anchor, but keep the card inside the work area.
    void Place() {
        const RECT a = spec_->anchor;
        MONITORINFO mi{ sizeof(mi) };
        RECT work{ a.left - 800, a.top, a.right + 800, a.bottom + 1200 };
        if (GetMonitorInfoW(MonitorFromRect(&a, MONITOR_DEFAULTTONEAREST), &mi)) work = mi.rcWork;
        PlaceInWorkArea(work);
    }
    void PlaceInWorkArea(const RECT& work) {
        const RECT a = spec_->anchor;
        const int pad = static_cast<int>(S(8.0f));
        card_w_ = std::min(kCardW, std::max(1.0f, static_cast<float>(work.right - work.left - 2 * pad) / scale_));
        cols_ = std::clamp(static_cast<int>((card_w_ - 2 * kPadX + kTileGap) / (kTileMinW + kTileGap)), 1, kMaxCols);
        MeasureContent();
        const int card_w = static_cast<int>(std::lround(S(card_w_)));
        int card_right = a.right + static_cast<int>(S(8.0f));
        int card_left = card_right - card_w;
        if (card_right > work.right - pad) { card_right = work.right - pad; card_left = card_right - card_w; }
        if (card_left < work.left + pad) { card_left = work.left + pad; card_right = card_left + card_w; }
        const float fixed_h = S(grid_y_ + kFootH + kPadBottom);
        const LONG latest_top = work.bottom - pad - static_cast<LONG>(std::ceil(fixed_h + S(90.0f)));
        const LONG card_top = std::clamp(a.bottom + static_cast<LONG>(S(10.0f)), work.top + pad, std::max(work.top + pad, latest_top));
        const float room = static_cast<float>(work.bottom - pad - card_top) - fixed_h;
        grid_max_ = std::clamp(room, 1.0f, S(kGridMax));
        const int m = static_cast<int>(std::lround(S(kMargin)));
        win_.left = card_left - m;
        win_.top = card_top - m;
        win_.right = card_right + m;
        win_.bottom = card_top + static_cast<int>(std::ceil(S(grid_y_ + kFootH + kPadBottom) + grid_max_)) + m;
        notch_x_ = static_cast<float>((a.left + a.right) / 2 - win_.left);
    }

    // ---- tiles ----------------------------------------------------------
    bool Matches(int page, size_t i) const {
        if (i < Tiles(page).size() && Tiles(page)[i].hidden) return false;
        if (page != 0 || query_.empty()) return true;
        const ToolbarPickerTile& t = spec_->pins[i];
        const std::wstring q = Lower(query_);
        return Lower(t.label + L" " + t.keywords).find(q) != std::wstring::npos;
    }

    void InitOrder(int page, const std::vector<size_t>& wanted, size_t n) {
        std::vector<size_t>& order = order_[page];
        order.clear();
        std::vector<bool> seen(n, false);
        for (size_t i : wanted)
            if (i < n && !seen[i]) { seen[i] = true; order.push_back(i); }
        for (size_t i = 0; i < n; ++i)
            if (!seen[i]) order.push_back(i);
    }

    int SectionOf(int page, size_t i) const {
        return page == kTools ? std::max(0, spec_->pins[i].section) : 0;
    }

    void Layout(int page, bool animate) {
        PageState& ps = pages_[page];
        const size_t n = TileCount(page);
        ps.tiles.resize(n);
        const float tw = TileW(), th = TileH(page);
        float y = 0.0f;
        ps.visible = 0;
        auto place = [&](size_t i, size_t k, float top) {
            TileAnim& t = ps.tiles[i];
            if (drag_.active && drag_.page == page && drag_.tile == i) return;   // follows the pointer
            const float px = static_cast<float>(k % cols_) * (tw + kTileGap);
            const float py = top + static_cast<float>(k / cols_) * (th + kTileGap);
            if (!animate || !t.placed || !animations_) { t.x.Snap(px); t.y.Snap(py); }
            else { t.x.To(px); t.y.To(py); }
            t.placed = true;
        };
        auto set_gone = [&](size_t i, bool gone) {
            TileAnim& t = ps.tiles[i];
            if (!animate || !animations_) t.gone.Snap(gone ? 1.0f : 0.0f);
            else t.gone.To(gone ? 1.0f : 0.0f);
        };
        const bool titled = page == kTools && !spec_->sections.empty();
        const size_t sections = page == kTools ? std::max<size_t>(1, spec_->sections.size()) : 1;
        ps.heads.resize(titled ? sections : 0);
        ps.head_shown.assign(ps.heads.size(), false);
        ps.sec_top.assign(sections, 0.0f);
        ps.sec_list.assign(sections, {});
        for (size_t sec = 0; sec < sections; ++sec) {
            std::vector<size_t>& shown = ps.sec_list[sec];
            for (size_t i : order_[page]) {
                if (i >= Tiles(page).size() || static_cast<size_t>(SectionOf(page, i)) != sec) continue;
                const bool match = Matches(page, i);
                set_gone(i, !match);
                if (match) shown.push_back(i);
            }
            const bool add = page == kApps && spec_->add_app;
            const size_t count = shown.size() + (add ? 1u : 0u);
            if (count == 0) { ps.sec_top[sec] = y; continue; }
            if (titled) {
                ps.head_shown[sec] = true;
                if (!animate || !animations_) ps.heads[sec].Snap(y); else ps.heads[sec].To(y);
                y += kSecH;
            }
            ps.sec_top[sec] = y;
            for (size_t k = 0; k < shown.size(); ++k) place(shown[k], k, y);
            if (add) { place(spec_->apps.size(), shown.size(), y); set_gone(spec_->apps.size(), false); }
            ps.visible += count;
            y += std::ceil(static_cast<float>(count) / cols_) * (th + kTileGap) - kTileGap + kSecGap;
        }
        ps.content_h = std::max(0.0f, y - kSecGap);
    }

    float TargetGridH() const {
        return std::min(grid_max_, S(std::max(pages_[page_].content_h, 60.0f) + 10.0f));
    }
    float MaxScroll() const {
        return std::max(0.0f, S(pages_[page_].content_h + 10.0f) - grid_h_.to);
    }

    void EnsureApps() {
        if (apps_loaded_) return;
        apps_loaded_ = true;
        if (spec_->load_apps) spec_->load_apps(*spec_);
        InitOrder(kApps, {}, spec_->apps.size());
        pages_[kApps].tiles.clear();
        Layout(kApps, false);
    }

    size_t CheckedCount(int page) const {
        if (page == kApps && !apps_loaded_) return spec_->apps_pinned;
        size_t n = 0;
        for (size_t i = 0; i < Tiles(page).size(); ++i)
            n += Tiles(page)[i].checked && !OnToolbar(page, i) ? 1u : 0u;
        return n;
    }

    // Hands the page's checked tiles, in display order, to the app.
    void Emit(int page) {
        if (page == kApps) {
            if (!spec_->on_apps) return;
            std::vector<ToolbarPickerTile> rows;
            for (size_t i : order_[kApps]) if (i < spec_->apps.size()) rows.push_back(spec_->apps[i]);
            spec_->on_apps(rows);
            return;
        }
        std::vector<size_t> checked;
        for (size_t i : order_[page]) if (i < Tiles(page).size() && Tiles(page)[i].checked) checked.push_back(i);
        if (page == kTools && spec_->on_tools) spec_->on_tools(checked);
        if (page == kSel && spec_->on_selection) spec_->on_selection(checked);
    }

    // Moves tile i to slot `to` of its section; the other sections keep their
    // places in the page order. True when the order changed.
    bool MoveInSection(int page, size_t i, int to) {
        const PageState& ps = pages_[page];
        const size_t sec = static_cast<size_t>(SectionOf(page, i));
        if (sec >= ps.sec_list.size()) return false;
        const std::vector<size_t> list = ps.sec_list[sec];
        const auto at = std::find(list.begin(), list.end(), i);
        if (at == list.end() || list.empty()) return false;
        const int cur = static_cast<int>(at - list.begin());
        to = std::clamp(to, 0, static_cast<int>(list.size()) - 1);
        if (to == cur) return false;
        std::vector<size_t> moved = list;
        moved.erase(moved.begin() + cur);
        moved.insert(moved.begin() + to, i);
        size_t k = 0;
        for (size_t& slot : order_[page])
            if (std::find(list.begin(), list.end(), slot) != list.end()) slot = moved[k++];
        Layout(page, true);
        return true;
    }

    // ---- actions --------------------------------------------------------
    void SetPage(int page) {
        page = std::clamp(page, 0, kPages - 1);
        if (page == page_) return;
        from_page_ = page_;
        page_ = page;
        if (page_ == kApps) EnsureApps();
        page_at_ = motion::NowMs();
        enter_at_ = page_at_;
        scroll_ = 0.0f;
        Layout(page_, false);
        const D2D1_RECT_F tab = TabRect(page_);
        if (animations_) { tab_x_.To(tab.left); tab_w_.To(tab.right - tab.left); grid_h_.To(TargetGridH()); }
        else { tab_x_.Snap(tab.left); tab_w_.Snap(tab.right - tab.left); grid_h_.Snap(TargetGridH()); }
        if (focus_.part == Part::Tile || focus_.part == Part::Switch || focus_.part == Part::Add)
            focus_ = FirstTile();
        Frame();
    }

    void ToggleTile(int page, size_t i) {
        const uint64_t now = motion::NowMs();
        if (page == kApps && i >= spec_->apps.size()) { AddApp(); return; }
        if (i >= Tiles(page).size()) return;
        ToolbarPickerTile& t = Tiles(page)[i];
        const size_t limit = page == kApps ? spec_->max_apps : page == kTools ? spec_->max_tools : 0;
        if (!t.checked && limit && CheckedCount(page) >= limit) {
            pages_[page].tiles[i].shake_at = now;
            note_until_ = now + 2200;
            note_page_ = page;
            MessageBeep(MB_OK);
            Frame();
            return;
        }
        t.checked = !t.checked;
        pages_[page].tiles[i].check_at = now;
        Emit(page);
        Frame();
    }

    void FlipSwitch(size_t i) {
        if (i >= spec_->pins.size() || !spec_->pins[i].toggle) return;
        ToolbarPickerTile& t = spec_->pins[i];
        const bool was = t.toggle_on;
        t.toggle_on = spec_->on_switch ? spec_->on_switch(i) : !t.toggle_on;
        if (t.toggle_on != was) pages_[0].tiles[i].switch_at = motion::NowMs();
        if (spec_->dark_now) dark_ = spec_->dark_now();   // 深色主题 restyles the popover too
        Frame();
    }

public:
    // The toolbar changed beside the popover (a pin dragged, removed or
    // clicked): reread it; changed tiles pop their check, moved ones slide.
    void Resync() {
        if (!spec_->resync || closing_at_ || !hwnd_) return;
        std::vector<char> pin_was, on_was, app_was;
        for (const auto& t : spec_->pins) { pin_was.push_back(t.checked); on_was.push_back(t.toggle_on); }
        for (const auto& t : spec_->apps) app_was.push_back(t.checked);
        spec_->apps_order.clear();
        spec_->resync(*spec_);
        const uint64_t now = motion::NowMs();
        PageState& tools = pages_[kTools];
        for (size_t i = 0; i < spec_->pins.size() && i < pin_was.size() && i < tools.tiles.size(); ++i) {
            if (spec_->pins[i].checked != static_cast<bool>(pin_was[i])) tools.tiles[i].check_at = now;
            if (spec_->pins[i].toggle_on != static_cast<bool>(on_was[i])) tools.tiles[i].switch_at = now;
        }
        PageState& apps = pages_[kApps];
        for (size_t i = 0; i < spec_->apps.size() && i < app_was.size() && i < apps.tiles.size(); ++i)
            if (spec_->apps[i].checked != static_cast<bool>(app_was[i])) apps.tiles[i].check_at = now;
        InitOrder(kTools, spec_->pins_order, spec_->pins.size());
        if (apps_loaded_ && !spec_->apps_order.empty()) InitOrder(kApps, spec_->apps_order, spec_->apps.size());
        for (int p = 0; p < kPages; ++p) Layout(p, animations_);
        if (animations_) grid_h_.To(TargetGridH()); else grid_h_.Snap(TargetGridH());
        Frame();
    }
    void Activate() {
        parked_ = false;
        Resync();
        if (hwnd_ && !closing_at_ && GetForegroundWindow() != hwnd_) SetForegroundWindow(hwnd_);
    }
    void CloseNow() { Close(); }
private:
    // The tools page's hovered tile that is not on the toolbar (and would
    // fit there) shows as a dashed pin on the toolbar.
    void UpdatePreview() {
        int want = -1;
        if (page_ == kTools && !closing_at_ && !drag_.active && hover_.part == Part::Tile && hover_.index >= 0 &&
            static_cast<size_t>(hover_.index) < spec_->pins.size() && !spec_->pins[hover_.index].checked &&
            (!spec_->max_tools || CheckedCount(kTools) < spec_->max_tools))
            want = hover_.index;
        if (want != preview_) {
            preview_ = want;
            if (spec_->on_preview) spec_->on_preview(want);
        }
        UpdateLink();
    }
    // The hovered (or dragged) tile that is checked: its toolbar pin rings.
    void UpdateLink() {
        int page = -1, tile = -1;
        if (!closing_at_) {
            if (drag_.active) { if (drag_.page == page_) tile = static_cast<int>(drag_.tile); }
            else if (hover_.part == Part::Tile || hover_.part == Part::Break) tile = hover_.index;
            if (tile >= 0 && (static_cast<size_t>(tile) >= Tiles(page_).size() || !Tiles(page_)[tile].checked)) tile = -1;
            if (tile >= 0) page = page_;
        }
        if (page == link_page_ && tile == link_tile_) return;
        link_page_ = page;
        link_tile_ = tile;
        if (spec_->on_link) spec_->on_link(page, tile, tile >= 0 ? Tiles(page)[tile].value : std::wstring{});
    }

    void AddApp() {
        if (!spec_->add_app) return;
        ToolbarPickerTile row;
        busy_ = true;   // the file dialog takes the activation; stay open
        const bool picked = spec_->add_app(hwnd_, row);
        busy_ = false;
        if (!hwnd_) return;
        if (GetForegroundWindow() != hwnd_) SetForegroundWindow(hwnd_);
        if (!picked || row.value.empty()) return;
        const uint64_t now = motion::NowMs();
        for (size_t i = 0; i < spec_->apps.size(); ++i) {
            ToolbarPickerTile& existing = spec_->apps[i];
            if (_wcsicmp(existing.value.c_str(), row.value.c_str()) != 0) continue;
            if (!existing.checked && CheckedCount(kApps) < spec_->max_apps) {
                existing.checked = true;
                pages_[kApps].tiles[i].check_at = now;
                Emit(kApps);
            }
            Frame();
            return;
        }
        row.checked = CheckedCount(kApps) < spec_->max_apps;
        spec_->apps.push_back(std::move(row));
        order_[kApps].push_back(spec_->apps.size() - 1);
        // The new tile takes the add tile's place and pops in; the add tile moves on.
        PageState& ps = pages_[kApps];
        TileAnim fresh;
        if (!ps.tiles.empty() && spec_->add_app) fresh = ps.tiles.back();
        fresh.check_at = now;
        ps.tiles.insert(ps.tiles.begin() + static_cast<std::ptrdiff_t>(spec_->apps.size() - 1), fresh);
        Layout(kApps, true);
        grid_h_.To(TargetGridH());
        Emit(kApps);
        Frame();
    }

    // Alt+←/→: the focused tile one slot along its section.
    void MoveFocused(int step) {
        const size_t i = static_cast<size_t>(focus_.index);
        if (focus_.index < 0 || IsAdd(page_, i) || i >= Tiles(page_).size()) return;
        const PageState& ps = pages_[page_];
        const size_t sec = static_cast<size_t>(SectionOf(page_, i));
        if (sec >= ps.sec_list.size()) return;
        const auto& list = ps.sec_list[sec];
        const auto at = std::find(list.begin(), list.end(), i);
        if (at == list.end()) return;
        if (MoveInSection(page_, i, static_cast<int>(at - list.begin()) + step)) {
            Emit(page_);
            ScrollIntoView(i);
        }
        show_focus_ = true;
        Frame();
    }

    // ---- dragging a tile -------------------------------------------------
    void DragTo(int x, int y) {
        if (drag_.page != page_ || drag_.tile >= pages_[page_].tiles.size()) { drag_ = TileDrag{}; return; }
        const float dx = static_cast<float>(x - drag_.start.x), dy = static_cast<float>(y - drag_.start.y);
        if (!drag_.active) {
            if (std::hypot(dx, dy) < S(4.0f)) return;
            drag_.active = true;
            pressed_ = Hit{};
            hover_ = Hit{};
            show_focus_ = false;
        }
        // Near the grid's edges the page scrolls along.
        const D2D1_RECT_F grid = GridRect();
        if (static_cast<float>(y) < grid.top + S(16.0f)) scroll_ = std::clamp(scroll_ - S(8.0f), 0.0f, MaxScroll());
        else if (static_cast<float>(y) > grid.bottom - S(16.0f)) scroll_ = std::clamp(scroll_ + S(8.0f), 0.0f, MaxScroll());
        TileAnim& t = pages_[page_].tiles[drag_.tile];
        const float gx = drag_.ox + dx / scale_;
        const float gy = drag_.oy + (dy + scroll_ - drag_.scroll0) / scale_;
        t.x.Snap(gx);
        t.y.Snap(gy);
        const float vx = static_cast<float>(x - drag_.last_x) / scale_;
        drag_.last_x = x;
        drag_.moved_at = motion::NowMs();
        if (animations_) tilt_.To(std::clamp(vx * 0.6f, -6.0f, 6.0f));
        // The slot under the tile's centre, inside its own section.
        const PageState& ps = pages_[page_];
        const size_t sec = static_cast<size_t>(SectionOf(page_, drag_.tile));
        if (sec < ps.sec_top.size()) {
            const float tw = TileW(), th = TileH(page_);
            const int col = std::clamp(static_cast<int>(std::floor((gx + tw * 0.5f) / (tw + kTileGap))), 0, cols_ - 1);
            const int row = std::max(0, static_cast<int>(std::floor((gy + th * 0.5f - ps.sec_top[sec]) / (th + kTileGap))));
            MoveInSection(page_, drag_.tile, row * cols_ + col);
        }
        Frame();
    }

    void EndDrag() {
        if (!drag_.active) { drag_ = TileDrag{}; return; }
        const int page = drag_.page;
        const bool changed = order_[page] != drag_.before;
        drag_ = TileDrag{};
        tilt_.Snap(0.0f);
        Layout(page, true);   // the tile springs into its slot from where it was let go
        if (changed) Emit(page);
        Frame();
    }

    void Close() {
        if (closing_at_ || !hwnd_) return;
        closing_at_ = motion::NowMs();
        if (!animations_) { destroy_posted_ = true; PostMessageW(hwnd_, kDestroyMessage, 0, 0); return; }
        Frame();
    }

    // ---- focus ----------------------------------------------------------
    Hit FirstTile() const {
        const PageState& ps = pages_[page_];
        for (const auto& list : ps.sec_list)
            for (size_t i : list)
                if (i < ps.tiles.size() && ps.tiles[i].gone.to < 0.5f) return Hit{ Part::Tile, static_cast<int>(i) };
        if (page_ == kApps && spec_->add_app && ps.tiles.size() > spec_->apps.size())
            return Hit{ Part::Add, static_cast<int>(spec_->apps.size()) };
        return Hit{ Part::Search, -1 };
    }

    void MoveFocus(int dx, int dy) {
        if (focus_.part != Part::Tile && focus_.part != Part::Add) {
            focus_ = FirstTile();
            show_focus_ = true;
            Frame();
            return;
        }
        const PageState& ps = pages_[page_];
        const size_t cur = static_cast<size_t>(focus_.index);
        if (cur >= ps.tiles.size()) return;
        const float cx = ps.tiles[cur].x.to, cy = ps.tiles[cur].y.to;
        float best = 1e9f;
        int pick = -1;
        for (size_t i = 0; i < ps.tiles.size(); ++i) {
            if (i == cur || ps.tiles[i].gone.to > 0.5f) continue;
            const float ddx = ps.tiles[i].x.to - cx, ddy = ps.tiles[i].y.to - cy;
            float score;
            if (dx != 0) {
                if (std::fabs(ddy) > 1.0f) {   // wrap to the neighbouring row, reading order
                    const bool after = ddy > 0.0f;
                    if ((dx > 0) != after) continue;
                    score = 10000.0f + std::fabs(ddy) * 10.0f + (dx > 0 ? ps.tiles[i].x.to : -ps.tiles[i].x.to);
                } else {
                    if (ddx * static_cast<float>(dx) <= 0.0f) continue;
                    score = std::fabs(ddx);
                }
            } else {
                if (ddy * static_cast<float>(dy) <= 1.0f) continue;
                score = std::fabs(ddy) * 2.0f + std::fabs(ddx);
            }
            if (score < best) { best = score; pick = static_cast<int>(i); }
        }
        if (pick < 0) {
            if (dy < 0 && page_ == 0) { focus_ = Hit{ Part::Search, -1 }; show_focus_ = true; Frame(); }
            return;
        }
        focus_ = Hit{ IsAdd(page_, static_cast<size_t>(pick)) ? Part::Add : Part::Tile, pick };
        show_focus_ = true;
        ScrollIntoView(static_cast<size_t>(pick));
        Frame();
    }

    void ScrollIntoView(size_t i) {
        const TileAnim& t = pages_[page_].tiles[i];
        const float top = S(t.y.to), bottom = S(t.y.to + TileH(page_));
        const float view = grid_h_.to - S(10.0f);
        if (top < scroll_) scroll_ = top;
        else if (bottom > scroll_ + view) scroll_ = bottom - view;
        scroll_ = std::clamp(scroll_, 0.0f, MaxScroll());
    }

    // ---- hit testing ----------------------------------------------------
    Hit HitAt(float x, float y) const {
        const D2D1_RECT_F card = Card();
        if (!Inside(card, x, y)) return Hit{ Part::Outside, -1 };
        if (Inside(DoneRect(), x, y)) return Hit{ Part::Done, -1 };
        if (!spec_->keys_text.empty() && Inside(KeysRect(), x, y)) return Hit{ Part::Keys, -1 };
        for (int i = 0; i < kPages; ++i)
            if (Inside(TabRect(i), x, y)) return Hit{ Part::Tab, i };
        if (page_ == kTools && Inside(SearchRect(), x, y)) return Hit{ Part::Search, -1 };
        const D2D1_RECT_F grid = GridRect();
        if (Inside(grid, x, y)) {
            const PageState& ps = pages_[page_];
            for (size_t i = 0; i < ps.tiles.size(); ++i) {
                if (ps.tiles[i].gone.to > 0.5f) continue;
                const D2D1_RECT_F r = TileRect(page_, i);
                if (!Inside(r, x, y)) continue;
                if (IsAdd(page_, i)) return Hit{ Part::Add, static_cast<int>(i) };
                if (page_ == kTools && spec_->pins[i].toggle) {
                    D2D1_RECT_F sw = SwitchRect(r);
                    sw.left -= S(5.0f); sw.top -= S(5.0f); sw.right += S(5.0f); sw.bottom += S(5.0f);
                    if (Inside(sw, x, y)) return Hit{ Part::Switch, static_cast<int>(i) };
                }
                if (BreakState(page_, i) >= 0) {
                    D2D1_RECT_F br = BreakRect(r);
                    br.left -= S(3.0f); br.top -= S(3.0f); br.right += S(3.0f); br.bottom += S(3.0f);
                    if (Inside(br, x, y)) return Hit{ Part::Break, static_cast<int>(i) };
                }
                return Hit{ Part::Tile, static_cast<int>(i) };
            }
        }
        return Hit{ Part::Card, -1 };
    }

    void Activate(const Hit& hit) {
        switch (hit.part) {
        case Part::Done: Close(); break;
        case Part::Keys: {
            const auto open = spec_->on_shortcuts;
            Close();
            if (open) open();
            break;
        }
        case Part::Tab: SetPage(hit.index); break;
        case Part::Search: search_focus_ = true; focus_ = hit; Frame(); break;
        case Part::Tile: ToggleTile(page_, static_cast<size_t>(hit.index)); break;
        case Part::Switch: FlipSwitch(static_cast<size_t>(hit.index)); break;
        case Part::Break: ToggleBreak(static_cast<size_t>(hit.index)); break;
        case Part::Add: AddApp(); break;
        default: break;
        }
    }

    // ---- frames ---------------------------------------------------------
    void Frame() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

    // Advances the springs; true while anything still moves.
    bool Step() {
        const uint64_t now = motion::NowMs();
        const float dt = last_step_ ? static_cast<float>(std::min<uint64_t>(now - last_step_, 64)) * 0.001f : 0.0f;
        last_step_ = now;
        bool moving = false;
        moving |= grid_h_.Step(dt, motion::spring::Snappy);
        moving |= tab_x_.Step(dt, motion::spring::Snappy);
        moving |= tab_w_.Step(dt, motion::spring::Snappy);
        if (drag_.active && now - drag_.moved_at > 70) tilt_.To(0.0f);
        moving |= tilt_.Step(dt, motion::spring::Snappy);
        for (auto& ps : pages_) {
            for (auto& t : ps.tiles) {
                moving |= t.x.Step(dt, motion::spring::Snappy);
                moving |= t.y.Step(dt, motion::spring::Snappy);
                moving |= t.gone.Step(dt, motion::spring::Snappy);
                if (now - t.check_at < 600 || now - t.switch_at < 400 || now - t.shake_at < 340) moving = true;
            }
            for (auto& h : ps.heads) moving |= h.Step(dt, motion::spring::Snappy);
        }
        if (now - opened_at_ < 700 || now - page_at_ < 400 || now - enter_at_ < 500) moving = true;
        if (note_until_ && now < note_until_ + 50) moving = true;
        if (closing_at_) moving = true;
        scroll_ = std::clamp(scroll_, 0.0f, MaxScroll());
        return moving && animations_;
    }

    // ---- painting -------------------------------------------------------
    struct Palette {
        D2D1_COLOR_F card, text, sub, faint, line2, hover, tile, tile_off, dash, accent, accent_text, acc_line, plate;
    };
    Palette Colors(const Theme& theme) const {
        Palette p;
        p.card = theme.surface_flyout;
        p.card.a = 1.0f;
        p.text = theme.text;
        p.sub = theme.text_secondary;
        p.faint = Alpha(theme.text_secondary, 0.7f);
        p.line2 = dark_ ? HexColor(0xFFFFFF, 0.12f) : HexColor(0x111827, 0.13f);
        p.hover = dark_ ? HexColor(0xFFFFFF, 0.07f) : HexColor(0x111827, 0.06f);
        p.tile = dark_ ? HexColor(0xFFFFFF, 0.075f) : HexColor(0xFFFFFF, 0.74f);
        p.tile_off = dark_ ? HexColor(0xFFFFFF, 0.012f) : HexColor(0xFFFFFF, 0.18f);
        p.dash = dark_ ? HexColor(0xFFFFFF, 0.22f) : HexColor(0x111827, 0.24f);
        p.accent = theme.accent;
        p.accent_text = theme.accent_text;
        p.acc_line = Alpha(theme.accent, dark_ ? 0.6f : 0.45f);
        p.plate = dark_ ? HexColor(0x3A3E46) : HexColor(0xFFFFFF);
        if (IsHighContrast()) {
            p.tile = p.tile_off = Alpha(theme.bg, 1.0f);
            p.dash = p.line2 = theme.text;
            p.plate = theme.fill_selected;
        }
        return p;
    }

    void EnsureFormats() {
        if (formats_scale_ == scale_ && title_fmt_.get()) return;
        IDWriteFactory2* f = compositor_.DwriteFactory();
        if (!f) return;
        wrapped_layouts_.clear();
        title_fmt_.reset(); label_fmt_.reset(); small_fmt_.reset(); tiny_fmt_.reset(); head_fmt_.reset();
        tile_fmt_.reset(); desc_fmt_.reset(); note_fmt_.reset();
        typography::CreateTextFormat(f, {typography::FontRole::Text, 13.0f * scale_, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &title_fmt_);
        typography::CreateTextFormat(f, {typography::FontRole::Text, 12.0f * scale_, DWRITE_FONT_WEIGHT_MEDIUM}, &label_fmt_);
        typography::CreateTextFormat(f, {typography::FontRole::Text, 11.5f * scale_}, &small_fmt_);
        typography::CreateTextFormat(f, {typography::FontRole::Text, 10.0f * scale_, DWRITE_FONT_WEIGHT_MEDIUM}, &tiny_fmt_);
        typography::CreateTextFormat(f, {typography::FontRole::Text, 11.0f * scale_, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &head_fmt_);
        for (IDWriteTextFormat* fmt : {title_fmt_.get(), label_fmt_.get(), small_fmt_.get(), tiny_fmt_.get(), head_fmt_.get()}) {
            if (!fmt) continue;
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ComPtr<IDWriteInlineObject> sign;
            if (SUCCEEDED(f->CreateEllipsisTrimmingSign(fmt, &sign))) {
                DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                fmt->SetTrimming(&trim, sign.get());
            }
        }
        typography::CreateTextFormat(f, {typography::FontRole::Text, 12.0f * scale_, DWRITE_FONT_WEIGHT_MEDIUM}, &tile_fmt_);
        typography::CreateTextFormat(f, {typography::FontRole::Text, 11.5f * scale_}, &desc_fmt_);
        typography::CreateTextFormat(f, {typography::FontRole::Text, 10.0f * scale_, DWRITE_FONT_WEIGHT_MEDIUM}, &note_fmt_);
        for (IDWriteTextFormat* fmt : {tile_fmt_.get(), desc_fmt_.get(), note_fmt_.get()}) {
            if (!fmt) continue;
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // Only unbounded application names need a last-line ellipsis; built-in
        // labels have enough measured height to render every wrapped line.
        if (tile_fmt_.get()) {
            ComPtr<IDWriteInlineObject> sign;
            if (SUCCEEDED(f->CreateEllipsisTrimmingSign(tile_fmt_.get(), &sign))) {
                DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                tile_fmt_->SetTrimming(&trim, sign.get());
            }
        }
        formats_scale_ = scale_;
        done_w_ = std::ceil(typography::MeasureLine(&compositor_, label_fmt_.get(), spec_->done_text)) + S(18.0f);
    }

    ID2D1SolidColorBrush* BrushOf(const D2D1_COLOR_F& c) {
        if (!brush_.get() && compositor_.Dc()) compositor_.Dc()->CreateSolidColorBrush(c, &brush_);
        if (brush_.get()) brush_->SetColor(c);
        return brush_.get();
    }
    struct WrappedLayout {
        std::wstring text;
        IDWriteTextFormat* format = nullptr;
        float width = 0.0f, height = 0.0f;
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        ComPtr<IDWriteTextLayout> layout;
    };
    IDWriteTextLayout* WrappedLayoutFor(std::wstring_view text, IDWriteTextFormat* format,
                                       float width, float height, DWRITE_TEXT_ALIGNMENT alignment) {
        if (text.empty() || !format || width <= 0.0f || height <= 0.0f) return nullptr;
        for (const auto& cached : wrapped_layouts_)
            if (cached.format == format && cached.alignment == alignment && cached.text == text &&
                std::fabs(cached.width - width) < 0.01f && std::fabs(cached.height - height) < 0.01f)
                return cached.layout.get();
        WrappedLayout entry{std::wstring(text), format, width, height, alignment, {}};
        if (FAILED(compositor_.DwriteFactory()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
            format, width, height, &entry.layout))) return nullptr;
        entry.layout->SetTextAlignment(alignment);
        if (wrapped_layouts_.size() >= 256) wrapped_layouts_.clear();
        wrapped_layouts_.push_back(std::move(entry));
        return wrapped_layouts_.back().layout.get();
    }
    void DrawWrapped(std::wstring_view text, const D2D1_RECT_F& bounds, IDWriteTextFormat* format,
                     const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        const auto snapped = typography::SnapVerticalBounds(bounds);
        auto* layout = WrappedLayoutFor(text, format, snapped.right - snapped.left, snapped.bottom - snapped.top, alignment);
        auto* brush = BrushOf(color);
        if (!layout || !brush) return;
        const auto origin = D2D1::Point2F(snapped.left, snapped.top);
        // Painter::DrawText's fast Luma path is single-line. Submit the shaped
        // layout instead so both Luma and DirectWrite honor wrapping/alignment.
        if (IsHighContrast())
            compositor_.Dc()->DrawTextLayout(origin, layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        else
            compositor_.DrawTextLayout(origin, layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    ID2D1StrokeStyle* Dashes() {
        if (dash_.get()) return dash_.get();
        ComPtr<ID2D1Factory> factory;
        compositor_.Dc()->GetFactory(&factory);
        if (!factory.get()) return nullptr;
        const float dashes[] = { 3.0f, 2.5f };
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                   D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f),
                                   dashes, 2, &dash_);
        return dash_.get();
    }

    // Draws `glyph` at `size` px, centred on `c`.
    void Glyph(std::wstring_view glyph, D2D1_POINT_2F c, float size, const D2D1_COLOR_F& color) {
        ID2D1DeviceContext* dc = compositor_.Dc();
        D2D1_MATRIX_3X2_F old;
        dc->GetTransform(&old);
        const float k = size / S(20.0f);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, c) * M(old));
        const float h = S(14.0f);
        painter_.DrawGlyph(glyph, D2D1::RectF(c.x - h, c.y - h, c.x + h, c.y + h), color);
        dc->SetTransform(old);
    }

    // Built before any layer is pushed (SetTarget inside a layer is not allowed).
    ComPtr<ID2D1Effect> MakeShadow(const D2D1_RECT_F& card, float radius) {
        ComPtr<ID2D1Effect> shadow;
        ID2D1DeviceContext* dc = compositor_.Dc();
        ComPtr<ID2D1CommandList> list;
        if (FAILED(dc->CreateCommandList(&list))) return shadow;
        ComPtr<ID2D1Image> target;
        dc->GetTarget(&target);
        dc->SetTarget(list.get());
        dc->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), BrushOf(D2D1::ColorF(0, 0, 0, 1)));
        list->Close();
        dc->SetTarget(target.get());
        if (FAILED(dc->CreateEffect(CLSID_D2D1Shadow, &shadow))) { shadow.reset(); return shadow; }
        shadow->SetInput(0, list.get());
        shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 9.0f * scale_);
        shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, dark_ ? 0.55f : 0.26f));
        return shadow;
    }

    void DrawTile(const Palette& pal, int page, size_t i, const D2D1_RECT_F& base, float alpha, uint64_t now,
                  float enter) {
        if (alpha <= 0.01f) return;
        ID2D1DeviceContext* dc = compositor_.Dc();
        const TileAnim& anim = pages_[page].tiles[i];
        const bool add = IsAdd(page, i);
        const ToolbarPickerTile* t = add ? nullptr : &Tiles(page)[i];
        const bool on = t && t->checked;
        const bool dragged = drag_.active && drag_.page == page && drag_.tile == i;
        // A selection action pinned as a tool: the chip leaves it out, the tile says so.
        const bool on_toolbar = OnToolbar(page, i);
        const Hit me{ add ? Part::Add : Part::Tile, static_cast<int>(i) };
        const bool hot = page == page_ && (hover_ == me || ((hover_.part == Part::Switch || hover_.part == Part::Break) &&
                                                            hover_.index == static_cast<int>(i)));
        const bool pressed = page == page_ && pressed_ == me && hover_ == me;

        D2D1_RECT_F r = base;
        // Shake (the apps page is full): 20% -4, 45% +4, 70% -2.
        if (anim.shake_at && now - anim.shake_at < 320) {
            const float k = static_cast<float>(now - anim.shake_at) / 320.0f;
            const float dx = k < 0.2f ? -4.0f * k / 0.2f : k < 0.45f ? -4.0f + 8.0f * (k - 0.2f) / 0.25f
                           : k < 0.7f ? 4.0f - 6.0f * (k - 0.45f) / 0.25f : -2.0f + 2.0f * (k - 0.7f) / 0.3f;
            r.left += S(dx); r.right += S(dx);
        }
        // Entrance (open / page switch): rise 6 px, 0.94 -> 1.
        float sc = 0.94f + 0.06f * motion::EaseOutBack(enter);
        if (pressed) sc *= 0.95f;
        const float lift = S(6.0f) * (1.0f - motion::EaseOutCubic(enter));
        sc *= 1.0f - 0.12f * (1.0f - alpha);   // filtered out: shrink to 0.88 while fading
        if (dragged) sc *= 1.05f;              // lifted
        D2D1_MATRIX_3X2_F old;
        dc->GetTransform(&old);
        const D2D1_POINT_2F centre = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f + lift);
        D2D1::Matrix3x2F local = D2D1::Matrix3x2F::Translation(0.0f, lift) * D2D1::Matrix3x2F::Scale(sc, sc, centre);
        if (dragged) local = local * D2D1::Matrix3x2F::Rotation(tilt_.v, centre);
        dc->SetTransform(local * M(old));

        const float radius = S(11.0f);
        const D2D1_RECT_F inner = D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f);
        if (dragged) {
            // Lifted: a soft shadow under an opaque card, so the tiles below don't show through.
            for (int k = 3; k >= 1; --k) {
                const float g = S(2.0f * static_cast<float>(k));
                painter_.FillRoundedRect(D2D1::RectF(r.left - g, r.top - g + S(5.0f), r.right + g, r.bottom + g + S(5.0f)),
                                         radius + g, HexColor(0x000000, (dark_ ? 0.10f : 0.05f) * alpha));
            }
            painter_.FillRoundedRect(r, radius, Alpha(card_color_, alpha));
        }
        if (add) {
            if (hot) painter_.FillRoundedRect(r, radius, Alpha(pal.hover, alpha));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius, radius), BrushOf(Alpha(pal.acc_line, alpha)),
                                     1.0f, Dashes());
        } else if (on) {
            D2D1_COLOR_F fill = pal.tile;
            if (hot) fill = Mix(fill, Alpha(pal.text, 0.16f), 0.3f);
            painter_.FillRoundedRect(r, radius, Alpha(fill, alpha));
            painter_.StrokeRoundedRect(inner, radius, Alpha(pal.line2, alpha), 1.0f);
        } else {
            painter_.FillRoundedRect(r, radius, Alpha(hot ? pal.hover : pal.tile_off, alpha));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius, radius),
                                     BrushOf(Alpha(hot ? pal.acc_line : pal.dash, alpha)), 1.0f, Dashes());
        }
        if (page == page_ && show_focus_ && (focus_ == me || (focus_.part == Part::Switch && focus_.index == static_cast<int>(i))))
            painter_.StrokeRoundedRect(D2D1::RectF(r.left - S(1.5f), r.top - S(1.5f), r.right + S(1.5f), r.bottom + S(1.5f)),
                                       radius + S(1.5f), Alpha(pal.accent, alpha), S(2.0f));

        // Keep art/controls above the wrapped label and its optional note.
        const std::wstring& note = on_toolbar ? spec_->on_toolbar_text : (t ? t->badge : spec_->add_app_text);
        const bool has_note = t && !note.empty();
        const float art_cy = r.top + S(22.0f);
        const float label_top = r.top + S(kLabelY);
        const float label_bottom = label_top + S(label_h_[page]);
        const float dim = (on && !on_toolbar) || add || hot ? 1.0f : 0.6f;
        if (add) {
            Glyph(L"\xE710", D2D1::Point2F((r.left + r.right) * 0.5f, art_cy), S(20.0f), Alpha(pal.accent, alpha));
        } else if (page == kApps) {
            const float half = S(13.0f);
            const float cx = std::round((r.left + r.right) * 0.5f), cy = std::round(art_cy);
            if (ID2D1Bitmap* bitmap = icons_.For(dc, t->icon))
                dc->DrawBitmap(bitmap, D2D1::RectF(cx - half, cy - half, cx + half, cy + half), alpha * dim,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            else
                Glyph(L"\xECAA", D2D1::Point2F(cx, cy), S(22.0f), Alpha(pal.sub, alpha * dim));
        } else {
            Glyph(t->glyph, D2D1::Point2F((r.left + r.right) * 0.5f, art_cy), S(21.0f),
                  Alpha(on ? pal.accent : pal.text, alpha * dim));
        }
        const std::wstring& label = add ? spec_->add_app_text : t->label;
        const D2D1_COLOR_F label_color = on || hot ? pal.text : pal.sub;
        DrawWrapped(label, D2D1::RectF(r.left + S(5.0f), label_top, r.right - S(5.0f), label_bottom),
                    tile_fmt_.get(), Alpha(label_color, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (has_note)
            DrawWrapped(note, D2D1::RectF(r.left + S(4.0f), label_bottom, r.right - S(4.0f), label_bottom + S(note_h_[page])),
                        note_fmt_.get(), Alpha(on_toolbar ? pal.faint : pal.accent, alpha), DWRITE_TEXT_ALIGNMENT_CENTER);

        // Check badge, top right: pops in (Pop spring), drops out quickly.
        if (t) {
            float k = on ? 1.0f : 0.0f;
            if (anim.check_at && animations_) {
                const float ms = static_cast<float>(now - anim.check_at);
                if (on) k = motion::SpringProgress(motion::spring::Pop, ms - 40.0f);
                else { const float q = Clamp01(ms / 200.0f); k = 1.0f - q * q * q; }
            }
            if (k > 0.01f) {
                const D2D1_POINT_2F c = D2D1::Point2F(r.right - S(14.0f), r.top + S(14.0f));
                const float rad = S(8.0f) * k;
                painter_.FillRoundedRect(D2D1::RectF(c.x - rad, c.y - rad, c.x + rad, c.y + rad), rad, Alpha(pal.accent, alpha));
                Glyph(L"\xE73E", c, S(12.0f) * k, Alpha(pal.accent_text, alpha));
            }
        }
        // Separator after it on the toolbar: a short accent bar when set; a faint
        // one on the hovered tile offers it.
        if (t && page == page_) {
            const int brk = BreakState(page, i);
            const bool brk_hot = hover_.part == Part::Break && hover_.index == static_cast<int>(i);
            if (brk == 1 || (brk == 0 && hot)) {
                const D2D1_RECT_F br = BreakRect(r);
                if (brk_hot) painter_.FillRoundedRect(br, S(4.0f), Alpha(pal.hover, alpha * 1.6f));
                else if (brk == 1) painter_.FillRoundedRect(br, S(4.0f), Alpha(pal.accent, alpha * 0.14f));
                const float cx = std::round((br.left + br.right) * 0.5f);
                const D2D1_COLOR_F bar = brk == 1 ? pal.accent : brk_hot ? pal.text : pal.faint;
                painter_.FillRoundedRect(D2D1::RectF(cx - S(1.0f), br.top + S(3.0f), cx + S(1.0f), br.bottom - S(3.0f)),
                                         S(1.0f), Alpha(bar, alpha));
            }
        }
        // Switch, top left: flips the setting without pinning the tool.
        if (t && page == kTools && t->toggle) {
            const D2D1_RECT_F sw = SwitchRect(r);
            float k = t->toggle_on ? 1.0f : 0.0f;
            if (anim.switch_at && animations_) {
                const float q = motion::EaseOutBack(Clamp01(static_cast<float>(now - anim.switch_at) / 260.0f));
                k = t->toggle_on ? q : 1.0f - q;
            }
            const float hgt = sw.bottom - sw.top;
            const D2D1_COLOR_F track = Mix(pal.dash, pal.accent, Clamp01(k));
            painter_.FillRoundedRect(sw, hgt * 0.5f, Alpha(track, alpha));
            const float knob = S(9.0f);
            const float kx = sw.left + S(2.0f) + S(9.0f) * k;
            const float ky = sw.top + S(2.0f);
            painter_.FillRoundedRect(D2D1::RectF(kx, ky, kx + knob, ky + knob), knob * 0.5f, Alpha(HexColor(0xFFFFFF), alpha));
        }
        dc->SetTransform(old);
    }

    void DrawPage(const Palette& pal, int page, float dx, float alpha, uint64_t now) {
        if (alpha <= 0.01f) return;
        const PageState& ps = pages_[page];
        const D2D1_POINT_2F o = GridOrigin();
        if (page == kTools) {
            for (size_t sec = 0; sec < ps.heads.size() && sec < spec_->sections.size(); ++sec) {
                if (!ps.head_shown[sec]) continue;
                const float y = o.y + S(ps.heads[sec].v);
                painter_.DrawText(spec_->sections[sec], D2D1::RectF(o.x + S(2.0f) + dx, y, o.x + S(400.0f) + dx, y + S(20.0f)),
                                  head_fmt_.get(), Alpha(pal.sub, alpha));
            }
        }
        // Reading order (sections, then the page order) staggers the entrance.
        std::vector<size_t> seq;
        for (const auto& list : ps.sec_list) seq.insert(seq.end(), list.begin(), list.end());
        if (page == kApps && spec_->add_app && ps.tiles.size() > spec_->apps.size()) seq.push_back(spec_->apps.size());
        for (size_t i = 0; i < ps.tiles.size(); ++i)
            if (std::find(seq.begin(), seq.end(), i) == seq.end()) seq.push_back(i);   // fading out
        const bool lifting = drag_.active && drag_.page == page;
        size_t order = 0;
        for (size_t i : seq) {
            if (i >= ps.tiles.size()) continue;
            const float shown = 1.0f - Clamp01(ps.tiles[i].gone.v);
            if (shown <= 0.01f) continue;
            float enter = 1.0f;
            if (animations_ && page == page_) {
                const float delay = 20.0f + 9.0f * static_cast<float>(std::min<size_t>(order, 8));
                enter = Clamp01((static_cast<float>(now - enter_at_) - delay) / 260.0f);
            }
            ++order;
            if (lifting && i == drag_.tile) continue;   // drawn last, above the rest
            DrawTile(pal, page, i, TileRect(page, i, dx), alpha * shown * motion::EaseOutCubic(enter), now, enter);
        }
        if (lifting && drag_.tile < ps.tiles.size())
            DrawTile(pal, page, drag_.tile, TileRect(page, drag_.tile, dx), alpha, now, 1.0f);
    }

    void Render() {
        frame_due_ = 0;   // re-armed at the end while something still moves
        UpdatePreview();
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            icons_ = IconBitmaps{};
            brush_.reset();
            dash_.reset();
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        ID2D1DeviceContext* dc = compositor_.Dc();
        if (!dc) return;
        EnsureFormats();
        const bool moving = Step();
        const uint64_t now = motion::NowMs();
        const bool hc = IsHighContrast();
        const Theme theme = hc ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        const Palette pal = Colors(theme);
        card_color_ = pal.card;

        // Open: Pop 0.94 -> 1 from the notch, fading in fast. Close: 110 ms to 0.97.
        float alpha = 1.0f, sc = 1.0f, ty = 0.0f;
        if (closing_at_) {
            const float k = animations_ ? Clamp01(static_cast<float>(now - closing_at_) / 110.0f) : 1.0f;
            alpha = 1.0f - k;
            sc = 1.0f - 0.03f * k;
            if (k >= 1.0f) {
                if (!destroy_posted_) { destroy_posted_ = true; PostMessageW(hwnd_, kDestroyMessage, 0, 0); }
                return;
            }
        } else if (animations_) {
            const float p = motion::SpringProgress(motion::spring::Pop, static_cast<float>(now - opened_at_));
            alpha = Clamp01(p * 1.7f);
            sc = 0.94f + 0.06f * p;
            ty = -S(6.0f) * (1.0f - p);
        }

        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0.0f));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        painter_.BeginFrame(theme, hc);
        const D2D1_RECT_F card = Card();
        const float radius = S(14.0f);
        ComPtr<ID2D1Effect> shadow;
        if (!hc) shadow = MakeShadow(card, radius);
        const D2D1_POINT_2F origin = D2D1::Point2F(std::clamp(notch_x_, card.left + S(20.0f), card.right - S(20.0f)), card.top - S(6.0f));
        dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, origin) * D2D1::Matrix3x2F::Translation(0.0f, ty));
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), alpha), nullptr);
        if (shadow.get()) dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, S(8.0f)));
        painter_.FillRoundedRect(card, radius, pal.card);
        painter_.StrokeRoundedRect(D2D1::RectF(card.left + 0.5f, card.top + 0.5f, card.right - 0.5f, card.bottom - 0.5f),
                                   radius, dark_ ? HexColor(0xFFFFFF, 0.09f) : HexColor(0x000000, 0.10f));
        // Notch towards the button.
        {
            D2D1_MATRIX_3X2_F old;
            dc->GetTransform(&old);
            const D2D1_POINT_2F c = D2D1::Point2F(origin.x, card.top + 0.5f);
            dc->SetTransform(D2D1::Matrix3x2F::Rotation(45.0f, c) * M(old));
            const float h = S(6.0f);
            const D2D1_RECT_F sq = D2D1::RectF(c.x - h, c.y - h, c.x + h, c.y + h);
            painter_.FillRoundedRect(sq, S(2.0f), dark_ ? HexColor(0xFFFFFF, 0.09f) : HexColor(0x000000, 0.10f));
            painter_.FillRoundedRect(D2D1::RectF(sq.left + 0.75f, sq.top + 0.75f, sq.right, sq.bottom), S(2.0f), pal.card);
            dc->SetTransform(old);
            painter_.FillRoundedRect(D2D1::RectF(origin.x - S(10.0f), card.top + 1.0f, origin.x + S(10.0f), card.top + S(10.0f)), 0.0f, pal.card);
        }

        // Title row.
        painter_.DrawText(spec_->title, D2D1::RectF(card.left + S(kPadX), card.top + S(kTitleY), card.right - S(80.0f),
                          card.top + S(kTitleY + kTitleH)), title_fmt_.get(), pal.text);
        const D2D1_RECT_F done = DoneRect();
        if (hover_.part == Part::Done) painter_.FillRoundedRect(done, S(6.0f), pressed_.part == Part::Done ? Alpha(pal.hover, 1.6f) : pal.hover);
        if (show_focus_ && focus_.part == Part::Done) painter_.StrokeRoundedRect(done, S(6.0f), pal.accent, S(1.5f));
        painter_.DrawText(spec_->done_text, done, label_fmt_.get(), pal.accent, fluent::HorizontalAlignment::Center);

        // Tabs: a sliding plate behind the active one, counts in pills.
        const D2D1_RECT_F tabs = TabsRect();
        painter_.FillRoundedRect(tabs, S(9.0f), pal.hover);
        const D2D1_RECT_F plate = D2D1::RectF(tab_x_.v, tabs.top + S(2.0f), tab_x_.v + tab_w_.v, tabs.bottom - S(2.0f));
        painter_.FillRoundedRect(D2D1::RectF(plate.left, plate.top + S(1.0f), plate.right, plate.bottom + S(1.0f)), S(7.0f),
                                 HexColor(0x000000, dark_ ? 0.25f : 0.08f));
        painter_.FillRoundedRect(plate, S(7.0f), pal.plate);
        painter_.StrokeRoundedRect(plate, S(7.0f), pal.line2, 0.75f);
        for (int i = 0; i < kPages; ++i) {
            const D2D1_RECT_F tr = TabRect(i);
            const std::wstring& name = i == kTools ? spec_->tools_tab : i == kSel ? spec_->sel_tab : spec_->apps_tab;
            const std::wstring count = std::to_wstring(CheckedCount(i));
            const float pill_w = std::max(S(16.0f), std::ceil(typography::MeasureLine(&compositor_, tiny_fmt_.get(), count)) + S(8.0f));
            const float name_w = std::min(std::ceil(typography::MeasureLine(&compositor_, label_fmt_.get(), name)),
                                          std::max(1.0f, tr.right - tr.left - pill_w - S(14.0f)));
            const float total = name_w + S(6.0f) + pill_w;
            const float x = (tr.left + tr.right - total) * 0.5f;
            const bool active = i == page_;
            const D2D1_COLOR_F c = active || (hover_.part == Part::Tab && hover_.index == i) ? pal.text : pal.sub;
            painter_.DrawText(name, D2D1::RectF(x, tr.top, x + name_w + S(2.0f), tr.bottom), label_fmt_.get(), c);
            const float cy = (tr.top + tr.bottom) * 0.5f;
            const D2D1_RECT_F pill = D2D1::RectF(x + name_w + S(6.0f), cy - S(8.0f), x + name_w + S(6.0f) + pill_w, cy + S(8.0f));
            painter_.FillRoundedRect(pill, S(8.0f), active ? Alpha(pal.accent, 0.16f) : pal.hover);
            painter_.DrawText(count, D2D1::RectF(pill.left, pill.top - S(2.0f), pill.right, pill.bottom + S(2.0f)), tiny_fmt_.get(),
                              active ? pal.accent : pal.sub, fluent::HorizontalAlignment::Center);
            if (show_focus_ && focus_.part == Part::Tab && focus_.index == i) painter_.StrokeRoundedRect(tr, S(7.0f), pal.accent, S(1.5f));
        }

        // Description (or the apps-full note), and the search field on the tools page.
        const float desc_right = page_ == kTools && !search_stacked_ ? SearchRect().left - S(8.0f) : card.right - S(kPadX);
        const bool note = note_until_ && now < note_until_ && page_ == note_page_;
        const bool break_hint = hover_.part == Part::Break && BreakState(page_, static_cast<size_t>(hover_.index)) >= 0;
        const std::wstring& desc = break_hint
            ? (BreakState(page_, static_cast<size_t>(hover_.index)) == 1 ? spec_->remove_break_text : spec_->add_break_text)
            : page_ == kTools ? spec_->tools_desc : page_ == kSel ? spec_->sel_desc : spec_->apps_desc;
        const std::wstring& full = page_ == kTools ? spec_->tools_full_text : spec_->apps_full_text;
        DrawWrapped(note ? full : desc,
                    D2D1::RectF(card.left + S(kPadX), card.top + S(kDescY), desc_right, card.top + S(kDescY + desc_h_)),
                    desc_fmt_.get(), note ? pal.accent : pal.sub);
        if (page_ == kTools) {
            const D2D1_RECT_F sr = SearchRect();
            painter_.FillRoundedRect(sr, S(7.0f), pal.hover);
            if (search_focus_) painter_.StrokeRoundedRect(sr, S(7.0f), pal.accent, 1.0f);
            Glyph(L"\xE721", D2D1::Point2F(sr.left + S(15.0f), (sr.top + sr.bottom) * 0.5f), S(13.0f), pal.faint);
            const D2D1_RECT_F text = D2D1::RectF(sr.left + S(28.0f), sr.top, sr.right - S(8.0f), sr.bottom);
            if (query_.empty()) {
                painter_.DrawText(spec_->search_hint, text, small_fmt_.get(), pal.faint);
            } else {
                painter_.DrawText(query_, text, small_fmt_.get(), pal.text);
            }
            // Blink like a native caret: solid again after each focus change or edit.
            if (search_focus_ != caret_focus_ || query_ != caret_query_) {
                caret_focus_ = search_focus_; caret_query_ = query_; caret_epoch_ = GetTickCount();
            }
            const UINT blink = GetCaretBlinkTime();
            const DWORD since = GetTickCount() - caret_epoch_;
            const bool caret_on = blink == INFINITE || blink == 0 || (since / blink) % 2u == 0u;
            if (search_focus_ && blink != INFINITE && blink != 0 && hwnd_)
                SetTimer(hwnd_, kCaretTimer, blink - since % blink, nullptr);
            else if (hwnd_)
                KillTimer(hwnd_, kCaretTimer);
            if (search_focus_ && caret_on) {
                const float w = query_.empty() ? 0.0f : typography::MeasureLine(&compositor_, small_fmt_.get(), query_) - S(1.0f);
                const float cx = std::min(text.left + w + S(1.0f), text.right);
                painter_.FillRoundedRect(D2D1::RectF(cx, sr.top + S(6.0f), cx + S(1.0f), sr.bottom - S(6.0f)), 0.0f, pal.text);
            }
        }

        // Footer: 快捷键, a row that opens the shortcut list (replaces More's entry).
        if (!spec_->keys_text.empty()) {
            const D2D1_RECT_F kr = KeysRect();
            painter_.FillRoundedRect(D2D1::RectF(card.left + S(kPadX), kr.top - S(5.0f), card.right - S(kPadX),
                                                 kr.top - S(5.0f) + 0.75f), 0.0f, pal.line2);
            const bool hot = hover_.part == Part::Keys;
            if (hot) painter_.FillRoundedRect(kr, S(7.0f), pressed_.part == Part::Keys ? Alpha(pal.hover, 1.6f) : pal.hover);
            if (show_focus_ && focus_.part == Part::Keys) painter_.StrokeRoundedRect(kr, S(7.0f), pal.accent, S(1.5f));
            const float cy = (kr.top + kr.bottom) * 0.5f;
            Glyph(L"\xE765", D2D1::Point2F(kr.left + S(16.0f), cy), S(14.0f), hot ? pal.text : pal.sub);
            painter_.DrawText(spec_->keys_text, D2D1::RectF(kr.left + S(32.0f), kr.top, kr.right - S(96.0f), kr.bottom),
                              small_fmt_.get(), hot ? pal.text : pal.sub);
            if (!spec_->keys_hint.empty()) {
                const float kw = std::ceil(typography::MeasureLine(&compositor_, tiny_fmt_.get(), spec_->keys_hint)) + S(12.0f);
                const D2D1_RECT_F kb = D2D1::RectF(kr.right - S(8.0f) - kw, cy - S(9.0f), kr.right - S(8.0f), cy + S(9.0f));
                painter_.StrokeRoundedRect(kb, S(5.0f), pal.line2, 1.0f);
                painter_.DrawText(spec_->keys_hint, D2D1::RectF(kb.left, kb.top - S(2.0f), kb.right, kb.bottom + S(2.0f)),
                                  tiny_fmt_.get(), pal.sub, fluent::HorizontalAlignment::Center);
            }
        }

        // Grids: the incoming page slides in from 16 px, the outgoing one leaves.
        const D2D1_RECT_F grid = GridRect();
        dc->PushAxisAlignedClip(grid, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float since = static_cast<float>(now - page_at_);
        if (animations_ && page_at_ && since < 220.0f && from_page_ != page_) {
            const float dir = page_ > from_page_ ? 1.0f : -1.0f;
            const float e = motion::EaseOutCubic(Clamp01(since / 220.0f));
            DrawPage(pal, from_page_, -S(16.0f) * dir * e, 1.0f - Clamp01(since / 130.0f), now);
            DrawPage(pal, page_, S(16.0f) * dir * (1.0f - e), Clamp01(since / 130.0f), now);
        } else {
            DrawPage(pal, page_, 0.0f, 1.0f, now);
        }
        if (page_ == kTools && pages_[kTools].visible == 0)
            painter_.DrawText(spec_->empty_text, D2D1::RectF(grid.left, grid.top + S(30.0f), grid.right, grid.top + S(50.0f)),
                              small_fmt_.get(), pal.faint, fluent::HorizontalAlignment::Center);
        // Scroll hint: a thin thumb when the page is taller than the popover.
        const float content = S(pages_[page_].content_h + 10.0f);
        if (content > grid_h_.v + 1.0f) {
            const float track = grid.bottom - grid.top - S(8.0f);
            const float thumb = std::max(S(24.0f), track * grid_h_.v / content);
            const float y = grid.top + S(4.0f) + (track - thumb) * (scroll_ / std::max(1.0f, content - grid_h_.v));
            painter_.FillRoundedRect(D2D1::RectF(grid.right - S(4.0f), y, grid.right - S(1.5f), y + thumb), S(1.5f), pal.dash);
        }
        dc->PopAxisAlignedClip();

        dc->PopLayer();
        dc->SetTransform(D2D1::IdentityMatrix());
        EndSurface(compositor_);
        if (moving && hwnd_) SetTimer(hwnd_, kFrameTimer, 8, nullptr);
        frame_due_ = moving ? motion::NowMs() + 8 : 0;
    }

    // ---- window ---------------------------------------------------------
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<PickerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<PickerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void OnChar(wchar_t ch) {
        if (page_ != kTools) return;
        if (ch == 8) {   // Backspace
            if (query_.empty()) return;
            query_.pop_back();
            if (!query_.empty() && IS_HIGH_SURROGATE(query_.back())) query_.pop_back();
        } else if (ch == 127) {   // Ctrl+Backspace
            query_.clear();
        } else if (ch >= 32) {
            query_.push_back(ch);
        } else {
            return;
        }
        search_focus_ = true;
        focus_ = Hit{ Part::Search, -1 };
        scroll_ = 0.0f;
        Layout(0, true);
        if (animations_) grid_h_.To(TargetGridH()); else grid_h_.Snap(TargetGridH());
        Frame();
    }

    bool OnKey(WPARAM key) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        switch (key) {
        case VK_ESCAPE:
            if (!query_.empty()) { query_.clear(); Layout(0, true); grid_h_.To(TargetGridH()); Frame(); }
            else Close();
            return true;
        case VK_TAB:
            if (ctrl) { SetPage((page_ + (shift ? kPages - 1 : 1)) % kPages); return true; }
            // Search -> tiles -> tabs -> Done.
            if (search_focus_ || focus_.part == Part::Search) { search_focus_ = false; focus_ = FirstTile(); }
            else if (focus_.part == Part::Tile || focus_.part == Part::Add || focus_.part == Part::Switch) focus_ = Hit{ Part::Tab, page_ };
            else if (focus_.part == Part::Tab) focus_ = Hit{ Part::Done, -1 };
            else if (page_ == kTools) { focus_ = Hit{ Part::Search, -1 }; search_focus_ = true; }
            else focus_ = FirstTile();
            show_focus_ = true;
            Frame();
            return true;
        case VK_LEFT:
        case VK_RIGHT:
            if (focus_.part == Part::Tab) { SetPage(page_ + (key == VK_LEFT ? -1 : 1)); focus_ = Hit{ Part::Tab, page_ }; show_focus_ = true; return true; }
            if (search_focus_) return true;
            MoveFocus(key == VK_LEFT ? -1 : 1, 0);
            return true;
        case VK_UP:
        case VK_DOWN:
            if (search_focus_ || focus_.part == Part::Search) {
                if (key == VK_DOWN) { search_focus_ = false; focus_ = FirstTile(); show_focus_ = true; Frame(); }
                return true;
            }
            MoveFocus(0, key == VK_UP ? -1 : 1);
            return true;
        case VK_SPACE:
        case VK_RETURN:
            if (search_focus_) {
                if (key == VK_RETURN) { search_focus_ = false; focus_ = FirstTile(); show_focus_ = true; Frame(); }
                return key == VK_RETURN;
            }
            if (focus_.part == Part::Tile && shift && page_ == kTools) FlipSwitch(static_cast<size_t>(focus_.index));
            else Activate(focus_);
            show_focus_ = true;
            return true;
        case 'F':
            if (ctrl && page_ == kTools) { search_focus_ = true; focus_ = Hit{ Part::Search, -1 }; Frame(); return true; }
            return false;
        }
        return false;
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            compositor_.Resize(win_.right - win_.left, win_.bottom - win_.top);
            g_open = true;
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST: {
            if (closing_at_) return HTTRANSPARENT;
            POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ScreenToClient(hwnd_, &pt);
            return Inside(Card(), static_cast<float>(pt.x), static_cast<float>(pt.y)) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE && live_ && !busy_) {
                // A window of our own (the file dialog) does not count, nor
                // a press on the toolbar's pins (they can be dragged while the
                // popover is open; the owner gives the activation back).
                const HWND next = reinterpret_cast<HWND>(lparam);
                POINT pt{};
                GetCursorPos(&pt);
                if (next && GetWindow(next, GW_OWNER) == hwnd_) return 0;
                if (next == owner_ && spec_->keep_open && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) && spec_->keep_open(pt))
                    parked_ = true;
                else
                    Close();
            } else if (LOWORD(wparam) != WA_INACTIVE) {
                parked_ = false;
            }
            return 0;
        case WM_TIMER:
            if (wparam == kFrameTimer) {
                KillTimer(hwnd_, kFrameTimer);
                Frame();
                return 0;
            }
            if (wparam == kCaretTimer) {
                KillTimer(hwnd_, kCaretTimer);
                Frame();
                return 0;
            }
            if (wparam == kPrefetchTimer) {
                KillTimer(hwnd_, kPrefetchTimer);
                if (!closing_at_ && !apps_loaded_) { EnsureApps(); Frame(); }
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            if (drag_.page >= 0 && (wparam & MK_LBUTTON)) {
                DragTo(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                return 0;
            }
            const Hit hit = HitAt(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (hit != hover_) { hover_ = hit; Frame(); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hover_.part != Part::None) { hover_ = Hit{}; Frame(); }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            pressed_ = HitAt(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            drag_ = TileDrag{};
            if (pressed_.part == Part::Tile && message == WM_LBUTTONDOWN) {   // may become a drag
                const size_t i = static_cast<size_t>(pressed_.index);
                drag_.page = page_;
                drag_.tile = i;
                drag_.start = POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                drag_.ox = pages_[page_].tiles[i].x.v;
                drag_.oy = pages_[page_].tiles[i].y.v;
                drag_.scroll0 = scroll_;
                drag_.last_x = drag_.start.x;
                drag_.before = order_[page_];
            }
            if (pressed_.part != Part::Search) search_focus_ = false;
            show_focus_ = false;
            SetCapture(hwnd_);
            Frame();
            return 0;
        case WM_LBUTTONUP: {
            if (drag_.active) {
                pressed_ = Hit{};
                EndDrag();
                if (GetCapture() == hwnd_) ReleaseCapture();
                return 0;
            }
            drag_ = TileDrag{};
            const Hit hit = HitAt(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            const Hit pressed = pressed_;
            pressed_ = Hit{};
            if (GetCapture() == hwnd_) ReleaseCapture();
            if (hit == pressed && hit.part != Part::None) {
                if (hit.part == Part::Tile || hit.part == Part::Add || hit.part == Part::Switch) focus_ = hit;
                Activate(hit);
            }
            Frame();
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (drag_.active && reinterpret_cast<HWND>(lparam) != hwnd_) EndDrag();
            return 0;
        case WM_MOUSEWHEEL: {
            const float step = S(48.0f) * static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            const float next = std::clamp(scroll_ - step, 0.0f, MaxScroll());
            if (next != scroll_) { scroll_ = next; Frame(); }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (message == WM_SYSKEYDOWN && (wparam == VK_LEFT || wparam == VK_RIGHT)) {
                const int step = wparam == VK_LEFT ? -1 : 1;
                if (focus_.part == Part::Tile && !search_focus_) {   // Alt+←/→ on a tile: move it
                    MoveFocused(step);
                    return 0;
                }
                SetPage(page_ + step);   // elsewhere: pages
                return 0;
            }
            if (OnKey(wparam)) return 0;
            break;
        case WM_CHAR:
            if (wparam == L' ' && !search_focus_) return 0;   // Space toggles (WM_KEYDOWN)
            if (wparam == L'|' && !search_focus_ && focus_.part == Part::Tile) {
                ToggleBreak(static_cast<size_t>(focus_.index));   // |: separator after the tile
                return 0;
            }
            if ((wparam == L't' || wparam == L'T') && !search_focus_ && focus_.part == Part::Tile && page_ == kTools) {
                FlipSwitch(static_cast<size_t>(focus_.index));   // T: the tile's switch
                return 0;
            }
            if (wparam == 9 || wparam == 13 || wparam == 27) return 0;
            OnChar(static_cast<wchar_t>(wparam));
            return 0;
        case WM_CLOSE:
            Close();
            return 0;
        case kDestroyMessage:
            DestroyWindow(hwnd_);
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
            KillTimer(hwnd_, kFrameTimer);
            KillTimer(hwnd_, kPrefetchTimer);
            KillTimer(hwnd_, kCaretTimer);
            compositor_.Shutdown();
            hwnd_ = nullptr;
            g_open = false;
            if (g_window == this) g_window = nullptr;
            if (owner_) InvalidateRect(owner_, nullptr, FALSE);   // its customize button drops the accent
            if (preview_ >= 0 && spec_->on_preview) spec_->on_preview(-1);
            preview_ = -1;
            if (link_page_ >= 0 && spec_->on_link) spec_->on_link(-1, -1, std::wstring{});
            link_page_ = link_tile_ = -1;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    ToolbarCustomizeSpec* spec_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{&compositor_};
    IconBitmaps icons_;
    ComPtr<IDWriteTextFormat> title_fmt_, label_fmt_, small_fmt_, tiny_fmt_, head_fmt_;
    ComPtr<IDWriteTextFormat> tile_fmt_, desc_fmt_, note_fmt_;
    std::vector<WrappedLayout> wrapped_layouts_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> dash_;
    float formats_scale_ = 0.0f;
    float done_w_ = 0.0f;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    RECT win_{};
    float scale_ = 1.0f;
    float notch_x_ = 0.0f;
    float card_w_ = kCardW;
    int cols_ = kMaxCols;
    float desc_h_ = 26.0f;
    float grid_y_ = 122.0f;
    float label_h_[kPages] = {34.0f, 34.0f, 34.0f};
    float note_h_[kPages] = {};
    bool search_stacked_ = false;
    float grid_max_ = 370.0f;
    float scroll_ = 0.0f;
    PageState pages_[kPages];
    std::vector<size_t> order_[kPages];   // display order of each page's tiles
    TileDrag drag_;
    Spring1 grid_h_, tab_x_, tab_w_, tilt_;
    D2D1_COLOR_F card_color_{};
    int note_page_ = kApps;
    int page_ = 0;
    int from_page_ = 0;
    Hit hover_, pressed_, focus_;
    std::wstring query_;
    std::wstring caret_query_;
    DWORD caret_epoch_ = 0;
    uint64_t frame_due_ = 0;   // a frame is wanted from this time (see Run)
    bool caret_focus_ = false;
    uint64_t opened_at_ = 0, page_at_ = 0, enter_at_ = 0, closing_at_ = 0, note_until_ = 0, last_step_ = 0;
    bool dark_ = false;
    bool animations_ = true;
    bool show_focus_ = false;
    bool search_focus_ = false;
    bool busy_ = false;
    bool live_ = false;
    bool destroy_posted_ = false;
    bool apps_loaded_ = true;
    bool parked_ = false;   // inactive while the owner drags a toolbar pin
    int preview_ = -1;
    int link_page_ = -1, link_tile_ = -1;
};

} // namespace

bool ToolbarPickerOpen() noexcept { return g_open; }

void ToolbarPickerActivate() {
    if (g_window) g_window->Activate();
}

void CloseToolbarPicker() {
    if (g_window) g_window->CloseNow();
}

void ShowToolbarPicker(HWND owner, ToolbarCustomizeSpec& spec, bool dark, D2D1_COLOR_F accent) {
    if (g_open) return;
    PickerWindow window;
    window.Run(owner, spec, dark, accent);
}

#if defined(PULSE_WITH_SELFTEST) || defined(PULSE_WITH_TEST_HOOKS)
HWND ProbeToolbarPicker(const wchar_t* keyword, POINT* center, const wchar_t* snapshot) {
    return g_window ? g_window->Probe(keyword, center, snapshot) : nullptr;
}
bool ProbeToolbarPickerLayout(const wchar_t* report, const wchar_t* snapshot_prefix) {
    return g_window && report && g_window->ProbeLayout(report, snapshot_prefix);
}
#endif
} // namespace pulse::ui
