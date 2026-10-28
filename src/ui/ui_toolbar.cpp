#include "ui_renderer_internal.h"
#include "toolbar_layout.h"
#include "pane_header_icons.h"
#include "toolbar_pin_drag.h"
#include "toolbar_picker.h"
#include "command_icons.h"
#include <algorithm>
#include <unordered_map>

namespace pulse::ui {
namespace {
// Toolbar motion (pulse_motion_v2, the sidebar's and menus' plate): one hover
// plate per row glides between the buttons under the pointer - the navigation
// buttons, the address bar's breadcrumb segments, and the command row - and
// fades in and out at the ends. The state lives here, per renderer; frames are
// asked for from the renderer's frame pump, so nothing ticks once it settles.
constexpr float kPlateFadeInMs = 90.0f, kPlateFadeOutMs = 120.0f;

struct GlidePlate {
    motion::SpringRectMotion spring;
    uint64_t context = 0;
    uint64_t last = 0;
    int64_t key = -1;
    float alpha = 0.0f;
    float radius = 0.0f;
    D2D1_RECT_F rect{}, target{};

    // hot: the hovered button's rect (nullptr: nothing hovered). Returns true
    // while the plate still moves or fades.
    bool Step(int64_t hot_key, const D2D1_RECT_F* hot, float hot_radius, uint64_t frame, uint64_t now,
              bool animations) {
        const float dt = static_cast<float>((std::min<uint64_t>)(now - last, 64));
        last = now;
        spring.SetAnimationsEnabled(animations);
        if (hot) {
            // Coming from nothing, the plate appears at the button (no glide
            // from wherever it was last).
            if (alpha <= 0.0f) ++context;
            key = hot_key;
            target = *hot;
            radius = hot_radius;
            rect = spring.Update(context, key, target, frame, now, motion::spring::Plate);
            alpha = animations ? (std::min)(1.0f, alpha + dt / kPlateFadeInMs) : 1.0f;
        } else {
            // Fades out where it was; a pointer coming back mid-fade glides on.
            alpha = animations ? (std::max)(0.0f, alpha - dt / kPlateFadeOutMs) : 0.0f;
            if (alpha > 0.0f) rect = spring.Update(context, key, target, frame, now, motion::spring::Plate);
            else spring.Hide();
        }
        return spring.Active(now) || (alpha > 0.0f && alpha < 1.0f);
    }
    void Reset() noexcept {
        alpha = 0.0f;
        spring.Hide();
    }
};

// Pinned tools and apps (toolbar picker T2): a pin that shows up pops in
// from 0.55 on the Pop spring; a toggle that flips turns its glyph in from
// -50 degrees and half size (300 ms, slight overshoot). Keyed by command or
// app name, so a pin keeps its state when it moves along the row. Nothing
// pops on the first paint.
constexpr float kPinPopFrom = 0.55f;
// Toolbar icons (统一绘制): command icons in an 18 DIP box, icons inside a
// field or chip a little smaller, chevrons and x smaller still; every one with
// this line weight.
constexpr float kIconBoxDip = 18.0f;
constexpr float kIconStrokeDip = 1.4f;
constexpr float kIconField = 0.85f;
constexpr float kIconSmall = 0.65f;
constexpr float kToggleTurnMs = 300.0f;
constexpr float kToggleTurnDeg = -50.0f;

struct PinMotion {
    uint64_t born = 0, flipped = 0, seen = 0;
    bool checked = false;
    // Picker T3c: a pin whose place in the row changed (reordered, a neighbour
    // added or removed, or let go after a drag) slides there from where it was
    // drawn (240 ms, ease out). A resize or the selection chip moves pins
    // without changing their index, so they don't slide then.
    bool placed = false, dragged = false;
    int index = -1;
    float visual = 0.0f, slide_from = 0.0f;
    uint64_t slide_at = 0;
};
constexpr float kPinSlideMs = 240.0f;

struct PinMotions {
    std::unordered_map<std::wstring, PinMotion> map;
    bool primed = false;
    bool moving = false;   // set while any pin pops or turns this frame

    PinMotion& Touch(const std::wstring& key, bool checked, uint64_t frame, uint64_t now, bool animate) {
        auto [it, added] = map.try_emplace(key);
        PinMotion& m = it->second;
        if (added) {
            m.checked = checked;
            m.born = primed && animate ? (now ? now : 1) : 0;
        } else if (m.checked != checked) {
            m.checked = checked;
            m.flipped = animate ? (now ? now : 1) : 0;
        }
        m.seen = frame;
        return m;
    }
    // Pins not drawn this frame are forgotten: they pop again when they return.
    void Sweep(uint64_t frame) {
        std::erase_if(map, [frame](const auto& entry) { return entry.second.seen != frame; });
        primed = true;
    }
    // Scale of the whole button while it pops in (1 when settled).
    float Pop(PinMotion& m, uint64_t now) {
        if (!m.born) return 1.0f;
        const float t = static_cast<float>(now - m.born);
        if (t >= static_cast<float>(motion::SpringSettleMs(motion::spring::Pop))) { m.born = 0; return 1.0f; }
        moving = true;
        return kPinPopFrom + (1.0f - kPinPopFrom) * motion::SpringProgress(motion::spring::Pop, t);
    }
    // Where the pin is drawn this frame: its slot, sliding, or under the pointer.
    D2D1_RECT_F Place(PinMotion& m, const D2D1_RECT_F& b, int index, const ToolbarPinDragState& drag, uint64_t now,
                      bool animate) {
        const float w = b.right - b.left;
        if (drag.index == index && drag.index >= 0) {
            m.placed = m.dragged = true;
            m.index = index;
            m.visual = drag.left;
            m.slide_at = 0;
            return D2D1::RectF(drag.left, b.top + drag.dy, drag.left + w, b.bottom + drag.dy);
        }
        if (!m.placed) {
            m.placed = true;
            m.visual = b.left;
        } else if ((index != m.index || m.dragged) && animate && primed && std::fabs(m.visual - b.left) > 0.5f) {
            m.slide_from = m.visual - b.left;
            m.slide_at = now ? now : 1;
        }
        m.index = index;
        m.dragged = false;
        float off = 0.0f;
        if (m.slide_at) {
            const float t = static_cast<float>(now - m.slide_at) / kPinSlideMs;
            if (t >= 1.0f) m.slide_at = 0;
            else { off = m.slide_from * (1.0f - motion::EaseOutCubic(t)); moving = true; }
        }
        m.visual = b.left + off;
        return D2D1::RectF(b.left + off, b.top, b.right + off, b.bottom);
    }
    // Glyph turn and scale while a toggle flips.
    void Turn(PinMotion& m, uint64_t now, float& degrees, float& scale) {
        degrees = 0.0f;
        scale = 1.0f;
        if (!m.flipped) return;
        const float t = static_cast<float>(now - m.flipped) / kToggleTurnMs;
        if (t >= 1.0f) { m.flipped = 0; return; }
        moving = true;
        const float e = motion::EaseOutBack(t);
        degrees = kToggleTurnDeg * (1.0f - e);
        scale = 0.5f + 0.5f * e;
    }
};

// Scales (and turns) what is drawn inside around a centre; no-op at rest.
struct AroundCentre {
    ID2D1DeviceContext* dc;
    D2D1_MATRIX_3X2_F saved{};
    bool on = false;
    AroundCentre(ID2D1DeviceContext* context, D2D1_POINT_2F centre, float scale, float degrees = 0.0f) : dc(context) {
        if (!dc || (scale == 1.0f && degrees == 0.0f)) return;
        dc->GetTransform(&saved);
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(degrees, centre) *
                         D2D1::Matrix3x2F::Scale(scale, scale, centre) * saved);
        on = true;
    }
    ~AroundCentre() { if (on) dc->SetTransform(&saved); }
    AroundCentre(const AroundCentre&) = delete;
    AroundCentre& operator=(const AroundCentre&) = delete;
};

struct ToolbarMotion {
    GlidePlate nav, crumbs, commands;
    PinMotions pins;
    PinMotions favorites;   // 收藏栏 items: the same slide / pop as the pins
    // The More button's count of pins folded into it; pops when it changes.
    int folded = 0;
    uint64_t folded_born = 0;
    bool animations = true;
    uint64_t animations_checked = 0;
};

ToolbarMotion& ToolbarMotionFor(const void* renderer) {
    static std::unordered_map<const void*, ToolbarMotion> motions;
    return motions[renderer];
}

// GulpToolbarApp: the app pin that took a drop, per renderer.
struct ToolbarGulp { int app = -1; uint64_t born = 0; };
std::unordered_map<const void*, ToolbarGulp>& Gulps() {
    static std::unordered_map<const void*, ToolbarGulp> gulps;
    return gulps;
}
std::unordered_map<const void*, ToolbarPinDragState>& PinDrags() {
    static std::unordered_map<const void*, ToolbarPinDragState> drags;
    return drags;
}
const ToolbarPinDragState& PinDragFor(const void* renderer) {
    static const ToolbarPinDragState none;
    const auto it = PinDrags().find(renderer);
    return it == PinDrags().end() ? none : it->second;
}

// Dashed outline of the picker's preview pin; one per D2D factory.
ID2D1StrokeStyle* PreviewDashes(ID2D1DeviceContext* dc) {
    static ComPtr<ID2D1StrokeStyle> style;
    static ID2D1Factory* owner = nullptr;
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory.get()) return nullptr;
    if (owner != factory.get()) {
        style.reset();
        const float dashes[] = { 3.0f, 2.5f };
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                   D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f),
                                   dashes, 2, &style);
        owner = factory.get();
    }
    return style.get();
}

int64_t PlateKey(HitTestResult::Region region, int index) {
    return static_cast<int64_t>(region) * 4096 + index;
}

std::wstring SelectionChipLabel(int count) {
    wchar_t buf[64]{};
    swprintf_s(buf,l10n::Get(l10n::StringId::SelectedCountFormat).c_str(),count);
    return buf;
}
}
float MainRenderer::SelectionChipLabelWidth(float width, int selected) const {
    // Narrow windows show the buttons only, as in the prototype.
    if (selected <= 0 || width-EffectiveSidebarWidth(width) < 700*scale_ || !compositor_ || !compositor_->SmallFormat())
        return 0.0f;
    return std::ceil(typography::MeasureLine(compositor_,compositor_->SmallFormat(),SelectionChipLabel(selected)));
}
// Pinned apps (prototype app_pins.html): program icons after the pinned
// commands. The dot under an icon says it opens every selected (or dragged)
// item, a ring some of them; an app that opens none is dimmed. In drop mode
// they sit on a faint accent band behind a label, and the app under the
// drag gets an accent outline.
void MainRenderer::DrawToolbarApps(const WindowViewModel& vm, const ToolbarLayout& layout, const Theme& theme) {
    const bool plate_hover=!IsHighContrast();  // DrawToolbar's gliding plate shows the hover
    ID2D1DeviceContext* dc=compositor_->Dc();
    ToolbarMotion& tm=ToolbarMotionFor(this);
    const size_t from=vm.toolbar_pins.size();
    D2D1_RECT_F band=layout.pin_label;
    bool any=band.right > band.left;
    for (size_t i=0;i<vm.toolbar_apps.size() && from+i<layout.pins.size();++i) {
        const D2D1_RECT_F b=layout.pins[from+i];
        if (b.right <= b.left) continue;
        if (!any) band=b;
        band.left=std::min(band.left,b.left);
        band.right=std::max(band.right,b.right);
        any=true;
    }
    if (!any) return;
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    if (vm.toolbar_drop_mode) {
        D2D1_COLOR_F fill=theme.accent;
        fill.a*=0.07f;
        painter_.FillRoundedRect(D2D1::RectF(band.left-3*scale_,band.top-2*scale_,band.right+3*scale_,band.bottom+2*scale_),
            8*scale_,fill);
    }
    FillRect(dc,brStrokeDivider_.get(),band.left-7*scale_,band.top+7*scale_,scale_,18*scale_);
    if (layout.pin_label.right > layout.pin_label.left && !vm.toolbar_drop_label.empty()) {
        const D2D1_RECT_F l=layout.pin_label;
        if (vm.toolbar_drop_pin) {
            D2D1_COLOR_F hot=theme.accent;
            hot.a*=0.18f;
            painter_.FillRoundedRect(l,7*scale_,hot);
        }
        DrawIconText(l.left+8*scale_,l.top,20*scale_,l.bottom-l.top,vm.toolbar_drop_pin ? L"\xE718" : L"\xE8A7",
            L"",theme.accent);
        painter_.DrawText(vm.toolbar_drop_label,D2D1::RectF(l.left+32*scale_,l.top,l.right-6*scale_,l.bottom),
            compositor_->SmallFormat(),theme.accent);
    }
    ComPtr<ID2D1SolidColorBrush> accent;
    dc->CreateSolidColorBrush(theme.accent,&accent);
    // A drop's gulp (prototype .tb.gulp): 0.82 at 35 %, then an overshooting
    // spring back to 1, 340 ms.
    int gulp_app=-1;
    float gulp_scale=1.0f;
    if (auto it=Gulps().find(this); it!=Gulps().end()) {
        ToolbarGulp& g=it->second;
        if (!g.born) g.born=motion_now_ ? motion_now_ : 1;
        const float t=static_cast<float>(motion_now_-g.born)/340.0f;
        if (t>=1.0f || !tm.animations) {
            Gulps().erase(it);
        } else {
            gulp_app=g.app;
            if (t<0.35f) {
                const float v=1.0f-t/0.35f;
                gulp_scale=1.0f-0.18f*(1.0f-v*v*v);
            } else {
                const float u=(t-0.35f)/0.65f, c1=1.70158f*1.4f, c3=c1+1.0f;
                gulp_scale=0.82f+0.18f*(1.0f+c3*(u-1.0f)*(u-1.0f)*(u-1.0f)+c1*(u-1.0f)*(u-1.0f));
            }
            tm.pins.moving=true;
        }
    }
    for (size_t i=0;i<vm.toolbar_apps.size() && from+i<layout.pins.size();++i) {
        const D2D1_RECT_F slot=layout.pins[from+i];
        if (slot.right <= slot.left) continue;
        const WindowViewModel::ToolbarAppPin& app=vm.toolbar_apps[i];
        if ((i==0 && from > 0) || (i > 0 && from+i < 64 && (layout.pin_dividers >> (from+i) & 1))) {
            // Short divider between the commands and the apps, or the user's separator.
            const float x=slot.left-6.5f*scale_;
            FillRect(dc,brStrokeDivider_.get(),x,slot.top+9*scale_,scale_,14*scale_);
        }
        PinMotion& pm=tm.pins.Touch(L"a"+app.name,false,motion_frame_,motion_now_,tm.animations);
        const ToolbarPinDragState& drag=PinDragFor(this);
        const bool dragged=drag.index==static_cast<int>(from+i);
        const D2D1_RECT_F b=tm.pins.Place(pm,slot,static_cast<int>(from+i),drag,motion_now_,tm.animations);
        const AroundCentre pop(dc,D2D1::Point2F((b.left+b.right)*0.5f,(b.top+b.bottom)*0.5f),
                               tm.pins.Pop(pm,motion_now_)*(dragged ? 1.08f : 1.0f));
        const bool target=vm.toolbar_drop_app==static_cast<int>(i);
        if (target) {
            D2D1_COLOR_F hot=theme.accent;
            hot.a*=0.16f;
            painter_.FillRoundedRect(b,6*scale_,hot);
            if (accent.get()) {
                const float h=scale_;
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left+h,b.top+h,b.right-h,b.bottom-h),
                    6*scale_,6*scale_),accent.get(),2*scale_);
            }
        } else if (!plate_hover && IsHovered(vm,HitTestResult::ToolbarPin,static_cast<int>(from+i))) {
            painter_.FillRoundedRect(b,6*scale_,theme.fill_hover);
        }
        const float cx=std::round((b.left+b.right)*0.5f);
        const float cy=std::round((b.top+b.bottom)*0.5f-(app.fit!=0 ? 1.5f*scale_ : 0.0f));
        const float half=std::round(8*scale_);
        float opacity=app.fit==3 && !(target && vm.toolbar_drop_force) ? 0.35f : 1.0f;
        if (dragged && drag.out) opacity*=0.35f;
        const AroundCentre gulp(dc,D2D1::Point2F(cx,cy),gulp_app==static_cast<int>(i) ? gulp_scale : 1.0f);
        if (ID2D1Bitmap* bitmap=AppIconBitmap(app.icon)) {
            dc->DrawBitmap(bitmap,D2D1::RectF(cx-half,cy-half,cx+half,cy+half),opacity,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            D2D1_COLOR_F c=theme.text_secondary;
            c.a*=opacity;
            DrawIconText(b.left,cy-15*scale_,b.right-b.left,30*scale_,L"\xECAA",L"",c);
        }
        if (accent.get() && (app.fit==1 || app.fit==2)) {
            const D2D1_POINT_2F dot=D2D1::Point2F(cx,b.bottom-4.5f*scale_);
            if (app.fit==1) dc->FillEllipse(D2D1::Ellipse(dot,2*scale_,2*scale_),accent.get());
            else dc->DrawEllipse(D2D1::Ellipse(dot,1.75f*scale_,1.75f*scale_),accent.get(),scale_);
        }
    }
}

ToolbarSelectionChip MainRenderer::SelectionChipAt(const ToolbarLayout& layout, float width, int selected,
                                                   int action_count) const {
    if (selected <= 0 || embedded_) return {};
    return MakeToolbarSelectionChip(layout,scale_,SelectionChipLabelWidth(width,selected),action_count);
}
void MainRenderer::DrawToolbar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    auto* dc = compositor_->Dc();
    struct IconScope {
        float* stroke; float* box;
        ~IconScope() { *stroke=0.0f; *box=20.0f; }
    } icon_scope{&icon_stroke_px_,&icon_box_dip_};
    icon_stroke_px_=kIconStrokeDip*scale_;
    icon_box_dip_=kIconBoxDip;
    const float left = EffectiveSidebarWidth(rect.right);
    const bool compact = rect.right-left < 600*scale_;
    const auto layout = ToolbarLayoutAt(rect.right,NewButtonWidthPx(compact),vm.pane.filter_expand);
    const bool searchOverlay=vm.address_searching && rect.right-left<480*scale_;
    ToolbarMotion& tm=ToolbarMotionFor(this);
    if (motion_now_-tm.animations_checked > 2000 || !tm.animations_checked) {
        // The Windows animation switch, re-read every couple of seconds.
        tm.animations_checked=motion_now_ ? motion_now_ : 1;
        tm.animations=motion::SystemAnimationsEnabled();
    }
    // High contrast keeps each button's own static hover.
    const bool plates=!IsHighContrast();
    bool toolbar_moving=false;
    // Every return below asks for the next frame while a plate moves.
    struct FrameRequest {
        const MainRenderer* self;
        const bool& moving;
        ~FrameRequest() {
            if (moving) self->RequestMotionFrame();
        }
    } frame_request{this,toolbar_moving};
    const auto draw_plate=[&](const GlidePlate& plate) {
        if (plate.alpha <= 0.0f) return;
        D2D1_COLOR_F fill=theme.fill_hover;
        fill.a*=plate.alpha;
        painter_.FillRoundedRect(plate.rect,plate.radius,fill);
    };
    if (searchOverlay || !plates) tm.nav.Reset();
    if (searchOverlay || !plates || (vm.address_editing && !vm.address_searching)) tm.crumbs.Reset();
    if (!searchOverlay) {
        const wchar_t* nav_glyphs[] = {kIconBack,kIconForward,kIconUp,kIconRefresh};
        const HitTestResult::Region nav_hits[] = {HitTestResult::NavBack,HitTestResult::NavForward,HitTestResult::NavUp,HitTestResult::NavRefresh};
        if (plates) {
            const D2D1_RECT_F* hot=nullptr;
            int hot_i=-1;
            for (int i=0;i<4;++i) {
                const auto& r=layout.navigation[i];
                const bool enabled=i==0 ? vm.can_go_back : i==1 ? vm.can_go_forward : true;
                if (r.right>r.left && enabled && IsHovered(vm,nav_hits[i])) { hot=&r; hot_i=i; }
            }
            if (tm.nav.Step(hot_i,hot,theme.radius_control*scale_,motion_frame_,motion_now_,tm.animations))
                toolbar_moving=true;
            draw_plate(tm.nav);
        }
        for (int i=0;i<4;++i) {
            const auto r=layout.navigation[i];
            if (r.right<=r.left) continue;
            const bool enabled=i==0 ? vm.can_go_back : i==1 ? vm.can_go_forward : true;
            fluent::ButtonSpec button;
            button.bounds = r; button.glyph = nav_glyphs[i];
            button.state.enabled = enabled;
            button.state.hovered = !plates && enabled && IsHovered(vm, nav_hits[i]);
            // DrawCommandButton's look, with the toolbar's icon size.
            if (button.state.hovered)
                painter_.FillRoundedRect(r, theme.radius_control * scale_, theme.fill_hover);
            DrawIconText(r.left, r.top, r.right - r.left, r.bottom - r.top, nav_glyphs[i], L"",
                         enabled ? theme.text_secondary : theme.text_disabled);
        }
        // Breadcrumb address bar: segments clickable, empty area -> edit mode.
        D2D1_RECT_F addrRc = AddressBarRect(rect.right);
        fluent::ControlState addrState{};
        addrState.focused = vm.address_editing && !vm.address_searching;
        addrState.hovered = !vm.address_editing && IsHovered(vm, HitTestResult::AddressBar);
        painter_.DrawTextFieldFrame(addrRc, addrState);
        // A long path scrolled toward its root eases there, and back to its
        // end 1.5 s after the pointer leaves (SettleBreadcrumbScroll).
        const bool addr_hot = IsHovered(vm, HitTestResult::AddressBar) ||
                              IsHovered(vm, HitTestResult::BreadcrumbSegment) ||
                              IsHovered(vm, HitTestResult::BreadcrumbArrow);
        if (addr_hot) crumb_hover_left_ = 0;
        else if (crumb_scroll_ > 0.0f && crumb_hover_left_ == 0) crumb_hover_left_ = GetTickCount64();
        if (std::fabs(crumb_scroll_ - crumb_scroll_shown_) > 0.25f) {
            const float dt = crumb_scroll_tick_ && motion_now_ > crumb_scroll_tick_
                ? (std::min)(50.0f, static_cast<float>(motion_now_ - crumb_scroll_tick_))
                : (crumb_scroll_tick_ ? 0.0f : 16.0f);
            crumb_scroll_shown_ += (crumb_scroll_ - crumb_scroll_shown_) * (1.0f - std::exp(-dt / 70.0f));
            if (std::fabs(crumb_scroll_ - crumb_scroll_shown_) <= 0.25f) crumb_scroll_shown_ = crumb_scroll_;
            crumb_scroll_tick_ = motion_now_;
            toolbar_moving = true;
        } else {
            crumb_scroll_shown_ = crumb_scroll_;
            crumb_scroll_tick_ = 0;
        }
        dc->PushAxisAlignedClip(addrRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (!vm.address_editing || vm.address_searching) {
            std::vector<BreadcrumbPlaced> placed;
            BreadcrumbStrip strip;
            BreadcrumbLayout(vm.pane, rect.right, placed, &strip);
            // The strip shows through the bar; an end with more path past it fades.
            struct StripScope {
                ID2D1DeviceContext* dc;
                bool clip = false, layer = false;
                ~StripScope() {
                    if (layer) dc->PopLayer();
                    if (clip) dc->PopAxisAlignedClip();
                }
            } strip_scope{dc};
            if (strip.overflow > 0.0f) {
                const D2D1_RECT_F view = D2D1::RectF(strip.view.left - 2.0f * scale_, addrRc.top,
                                                     strip.view.right + 2.0f * scale_, addrRc.bottom);
                dc->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                strip_scope.clip = true;
                const bool more_left = strip.offset < strip.overflow - 0.5f;
                const bool more_right = strip.offset > 0.5f;
                const float view_w = view.right - view.left;
                const float f = view_w > 0.0f ? (std::min)(0.3f, 26.0f * scale_ / view_w) : 0.0f;
                if ((more_left || more_right) && f > 0.0f) {
                    const D2D1_GRADIENT_STOP stops[] = {
                        {0.0f, D2D1::ColorF(0, 0, 0, more_left ? 0.0f : 1.0f)},
                        {f, D2D1::ColorF(0, 0, 0, 1.0f)},
                        {1.0f - f, D2D1::ColorF(0, 0, 0, 1.0f)},
                        {1.0f, D2D1::ColorF(0, 0, 0, more_right ? 0.0f : 1.0f)}};
                    ComPtr<ID2D1GradientStopCollection> collection;
                    ComPtr<ID2D1LinearGradientBrush> mask;
                    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 4, &collection)) &&
                        SUCCEEDED(dc->CreateLinearGradientBrush(
                            D2D1::LinearGradientBrushProperties(D2D1::Point2F(view.left, 0.0f),
                                                                D2D1::Point2F(view.right, 0.0f)),
                            collection.get(), &mask))) {
                        dc->PushLayer(D2D1::LayerParameters1(view, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                             D2D1::IdentityMatrix(), 1.0f, mask.get()),
                                      nullptr);
                        strip_scope.layer = true;
                    }
                }
            }
            if (plates) {
                const int hot_i = vm.breadcrumb_hover >= 0 && vm.breadcrumb_hover < static_cast<int>(placed.size()) &&
                                  vm.breadcrumb_hover != vm.breadcrumb_drop ? vm.breadcrumb_hover : -1;
                if (tm.crumbs.Step(hot_i, hot_i >= 0 ? &placed[static_cast<size_t>(hot_i)].rc : nullptr,
                                   theme.radius_control * scale_, motion_frame_, motion_now_, tm.animations))
                    toolbar_moving = true;
                draw_plate(tm.crumbs);
            }
            {
                std::vector<std::wstring> keys;
                keys.reserve(placed.size());
                for (const auto& seg : placed) keys.push_back(seg.path + L"|" + seg.text);
                if (keys != crumb_keys_) {
                    const bool animate = !crumb_keys_.empty() && motion::SystemAnimationsEnabled() && !IsHighContrast();
                    int k = 0;
                    for (const auto& key : keys) {
                        if (std::find(crumb_keys_.begin(), crumb_keys_.end(), key) != crumb_keys_.end()) continue;
                        if (animate) crumb_born_[key] = motion_now_ + 30u * static_cast<uint64_t>(k++);
                    }
                    crumb_keys_ = std::move(keys);
                }
                const uint64_t settle = motion::SpringSettleMs(motion::spring::Smooth);
                for (auto it = crumb_born_.begin(); it != crumb_born_.end();) {
                    if (motion_now_ > it->second && motion_now_ - it->second > settle) it = crumb_born_.erase(it);
                    else ++it;
                }
                if (!crumb_born_.empty()) RequestMotionFrame();
            }
            for (size_t i = 0; i < placed.size(); ++i) {
                const auto& seg = placed[i];
                struct CrumbEnter {
                    ID2D1DeviceContext* dc;
                    D2D1_MATRIX_3X2_F saved{};
                    bool moved = false, faded = false;
                    ~CrumbEnter() {
                        if (faded) dc->PopLayer();
                        if (moved) dc->SetTransform(saved);
                    }
                } enter{dc};
                if (!crumb_born_.empty() && i < crumb_keys_.size()) {
                    const auto born = crumb_born_.find(crumb_keys_[i]);
                    if (born != crumb_born_.end()) {
                        const float age = static_cast<float>(static_cast<int64_t>(motion_now_ - born->second));
                        const float p = motion::SpringProgress(motion::spring::Smooth, age);
                        if (p <= 0.0f) continue;
                        dc->GetTransform(&enter.saved);
                        dc->SetTransform(D2D1::Matrix3x2F::Translation(std::round(10.0f * scale_ * (1.0f - p)), 0.0f) *
                                         enter.saved);
                        enter.moved = true;
                        if (p < 0.999f) {
                            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                                                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                                 D2D1::IdentityMatrix(), (std::min)(1.0f, p)),
                                          nullptr);
                            enter.faded = true;
                        }
                    }
                }
                if ((int)i == vm.breadcrumb_drop) {
                    // Drop target: accent 2px stroke (ui.md §5.2 rule 7).
                    dc->DrawRoundedRectangle(
                        D2D1::RoundedRect(seg.rc, theme.radius_control * scale_, theme.radius_control * scale_),
                        brAccent_.get(), 2.0f * scale_);
                } else if (!plates && (int)i == vm.breadcrumb_hover) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), seg.rc.left, seg.rc.top,
                        seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top,
                        theme.radius_control * scale_);
                }
                if (i + 1 < placed.size() || (!vm.pane.is_query_search &&
                    (vm.pane.path.empty() || !fs::IsVirtualPath(vm.pane.path)))) {
                    const float chX = seg.rc.right;
                    if (IsHovered(vm, HitTestResult::BreadcrumbArrow, static_cast<int>(i))) {
                        MakeBrush(dc, theme.fill_hover, brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), chX, seg.rc.top,
                            14.0f * scale_, seg.rc.bottom - seg.rc.top, theme.radius_control * scale_);
                    }
                    DrawIconText(chX, addrRc.top, 14.0f * scale_, addrRc.bottom - addrRc.top,
                        kIconChevronRight, L">", seg.ghost ? WithAlpha(theme.text_secondary, 0.55f)
                                                           : theme.text_secondary, kIconSmall);
                }
                const bool search_crumb = vm.pane.is_query_search && vm.pane.has_search_origin &&
                    i + 1 == placed.size();
                if (search_crumb && (int)i != vm.breadcrumb_hover && (int)i != vm.breadcrumb_drop) {
                    auto tint = theme.accent;
                    tint.a *= 0.16f;
                    ComPtr<ID2D1SolidColorBrush> tint_brush;
                    dc->CreateSolidColorBrush(tint, &tint_brush);
                    if (tint_brush.get()) FillRoundedRect(dc, tint_brush.get(), seg.rc.left, seg.rc.top,
                        seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top, theme.radius_control * scale_);
                }
                float text_left = seg.rc.left + 8 * scale_;
                if (seg.alias) {
                    // 别名 crumb: the mark, then the alias (pulse_alias.html).
                    const float m = kAliasMarkDip * scale_;
                    const float my = std::round((seg.rc.top + seg.rc.bottom - m) * 0.5f);
                    const float mx = std::round(text_left);
                    painter_.DrawAliasMark(D2D1::RectF(mx, my, mx + m, my + m));
                    text_left += kCrumbAliasMarkDip * scale_;
                }
                // A faint tail segment comes forward under the pointer.
                MakeBrush(dc, !seg.ghost || (int)i == vm.breadcrumb_hover ? theme.text
                              : WithAlpha(theme.text_secondary, 0.85f), brText_);
                // Width measured exactly; let the ink use the right padding as slack
                // so the trailing glyph is not shaved by the clip rect.
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), seg.text,
                    text_left, seg.rc.top, seg.rc.right - text_left,
                    seg.rc.bottom - seg.rc.top);
            }
            if (strip_scope.layer) { dc->PopLayer(); strip_scope.layer = false; }
            if (strip_scope.clip) { dc->PopAxisAlignedClip(); strip_scope.clip = false; }
            if (strip.overflow > 0.0f && addr_hot && strip.width > 0.0f) {
                // Where in the path the bar is: a short thumb along its foot.
                const float track = 120.0f * scale_;
                const float view_w = strip.view.right - strip.view.left;
                const float thumb = (std::max)(18.0f * scale_, track * (std::min)(1.0f, view_w / strip.width));
                const float track_left = std::round((strip.view.left + strip.view.right - track) * 0.5f);
                const float tx = std::round(track_left + (track - thumb) * (1.0f - strip.offset / strip.overflow));
                painter_.FillRoundedRect(D2D1::RectF(tx, addrRc.bottom - 4.0f * scale_, tx + thumb,
                                                     addrRc.bottom - 2.0f * scale_),
                                         scale_, WithAlpha(theme.text_secondary, 0.45f));
            }
            if (placed.empty() && !vm.pane.path.empty()) {
                MakeBrush(dc, theme.text, brText_);
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), vm.pane.path,
                    addrRc.left + 12.0f * scale_, addrRc.top,
                    addrRc.right - addrRc.left - 18.0f * scale_,
                    addrRc.bottom - addrRc.top);
            }
        }
        if (FavoritesButtonShown() && !vm.address_editing && !vm.address_searching) {
            // ★⌄: the favorites dropdown while the 收藏栏 is hidden; filled
            // when the current folder is starred.
            const D2D1_RECT_F fb = AddressFavoritesRect(rect.right);
            if (IsHovered(vm, HitTestResult::AddressFavorites))
                painter_.FillRoundedRect(fb, theme.radius_control * scale_, theme.fill_hover);
            DrawIconText(fb.left + 2 * scale_, fb.top, 16 * scale_, fb.bottom - fb.top,
                         vm.favorites_here ? L"\xE735" : L"\xE734", L"",
                         vm.favorites_here ? theme.accent : theme.text_secondary, kIconField);
            DrawIconText(fb.left + 15 * scale_, fb.top, 12 * scale_, fb.bottom - fb.top, L"\xE70D", L"",
                         theme.text_secondary, kIconSmall);
        }
        dc->PopAxisAlignedClip();
    }
    DrawAddressSearchChrome(vm, rect.right, theme);
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    FillRect(dc,brStrokeDivider_.get(),left+margin_,title_bar_height_+44*scale_,rect.right-left-2*margin_,scale_);
    // Embedded hosts (the file picker) keep the address row only.
    if (embedded_) return;
    // The command row's plate: the file and pane commands, overflow, pinned
    // commands and apps, Sort / Filter / Group.
    const bool filter_field=!(vm.pane.filter_expand <= 0.015f && !vm.filter_editing);
    const bool group_chip=toolbar_group_ > 0;
    if (plates) {
        const D2D1_RECT_F* hot=nullptr;
        int64_t hot_key=-1;
        float hot_radius=theme.radius_control*scale_;
        const auto consider=[&](const D2D1_RECT_F& r, bool hovered, int64_t key, float radius) {
            if (!hot && hovered && r.right > r.left) { hot=&r; hot_key=key; hot_radius=radius; }
        };
        const float rc=theme.radius_control*scale_;
        consider(layout.overflow,IsHovered(vm,HitTestResult::ToolbarMore),PlateKey(HitTestResult::ToolbarMore,0),rc);
        const size_t from=vm.toolbar_pins.size();
        for (size_t i=0;i<layout.pins.size();++i) {
            const int idx=static_cast<int>(i);
            if (!IsHovered(vm,HitTestResult::ToolbarPin,idx)) continue;
            if (i<from) consider(layout.pins[i],vm.toolbar_pins[i].enabled,PlateKey(HitTestResult::ToolbarPin,idx),rc);
            else if (i-from<vm.toolbar_apps.size() && vm.toolbar_drop_app!=static_cast<int>(i-from))
                consider(layout.pins[i],true,PlateKey(HitTestResult::ToolbarPin,idx),6*scale_);
        }
        consider(layout.sort,IsHovered(vm,HitTestResult::ToolbarSort),PlateKey(HitTestResult::ToolbarSort,0),rc);
        if (!filter_field)
            consider(layout.filter,IsHovered(vm,HitTestResult::FilterBox),PlateKey(HitTestResult::FilterBox,0),rc);
        if (!group_chip)
            consider(layout.group,IsHovered(vm,HitTestResult::ToolbarGroup),PlateKey(HitTestResult::ToolbarGroup,0),rc);
        if (tm.commands.Step(hot_key,hot,hot_radius,motion_frame_,motion_now_,tm.animations)) toolbar_moving=true;
        draw_plate(tm.commands);
    } else {
        tm.commands.Reset();
    }
    // Split New button: the main part creates a folder (Ctrl+Shift+N), the
    // chevron segment opens the New menu. Each segment gets its own hover.
    // The network page and a server's share list put 连接服务器 in this slot
    // (same width: the label is no wider than 新建文件夹 in any language).
    const bool connect=vm.pane.connect_server;
    // A phone folder takes 新建文件夹 but no text document (CreateDeviceFolder).
    const bool text_primary=new_primary_==1 && new_last_text_;
    const bool primary_enabled=connect || (text_primary ? vm.pane.can_create : vm.pane.can_create_folder);
    const std::wstring label=connect ? pulse::l10n::Get(pulse::l10n::StringId::ConnectServer) : NewButtonLabel();
    const std::wstring primary_glyph=connect ? std::wstring(L"\xE968") : std::wstring(NewButtonGlyph());
    const D2D1_RECT_F newArrow=NewArrowSegment(layout.create,compact);
    fluent::ButtonSpec create;
    create.bounds=layout.create; create.text=compact ? std::wstring_view{} : std::wstring_view{label};
    create.glyph=primary_glyph.c_str(); create.kind=fluent::ButtonKind::Primary;
    create.drop_down=true;
    // This PC, Pulse views: nothing can be created there.
    create.state.enabled=primary_enabled;
    create.skip_glyph=true;   // drawn below at the toolbar's icon size
    painter_.DrawButton(create);
    const bool newMainHot=primary_enabled && IsHovered(vm,HitTestResult::NewButton);
    const bool newArrowHot=IsHovered(vm,HitTestResult::NewButtonArrow);
    if (newMainHot || newArrowHot) {
        D2D1_RECT_F hot=layout.create;
        if (newArrowHot) hot.left=newArrow.left; else hot.right=newArrow.left;
        dc->PushAxisAlignedClip(hot,D2D1_ANTIALIAS_MODE_ALIASED);
        create.state.hovered=true;
        painter_.DrawButton(create);
        dc->PopAxisAlignedClip();
    }
    {
        // The painter's primary-button glyph colour, in its glyph slot.
        const D2D1_COLOR_F ink=IsHighContrast() ? (primary_enabled ? theme.accent_text : theme.text_disabled)
            : !primary_enabled ? D2D1::ColorF(1.0f,1.0f,1.0f,vm.dark ? 0.43f : 0.90f)
            : vm.dark ? D2D1::ColorF(0.0f,0.0f,0.0f,1.0f) : D2D1::ColorF(1.0f,1.0f,1.0f,1.0f);
        DrawIconText(layout.create.left+8*scale_,layout.create.top,20*scale_,layout.create.bottom-layout.create.top,
                     primary_glyph.c_str(),L"",ink);
    }
    if (newArrow.right > newArrow.left && newArrow.left > layout.create.left) {
        auto divider=theme.accent_text;
        divider.a*=0.35f;
        ComPtr<ID2D1SolidColorBrush> divider_brush;
        dc->CreateSolidColorBrush(divider,&divider_brush);
        if (divider_brush.get()) FillRect(dc,divider_brush.get(),newArrow.left-0.5f*scale_,newArrow.top+7*scale_,
            scale_,newArrow.bottom-newArrow.top-14*scale_);
    }
    // The split tool's pin shows the current layout (1 / 2 side by side /
    // 2 stacked / 3 / 4). A pane count that does not match the preset (a
    // custom split) is described from the pane rectangles instead.
    const auto split_icon=[&vm]() {
        constexpr int kPaneCount[]={1,2,2,3,4};
        int preset=std::clamp(vm.layout_preset,0,4);
        const int panes=static_cast<int>(vm.pane_slots.size());
        if (panes>0 && kPaneCount[preset]!=panes) {
            if (panes==1) preset=0;
            else if (panes==2) preset=vm.pane_slots[1].rect.top>=vm.pane_slots[0].rect.bottom-1.0f ? 2 : 1;
            else preset=panes==3 ? 3 : 4;
        }
        constexpr PaneHeaderIcon kIcons[]={PaneHeaderIcon::SplitSingle,PaneHeaderIcon::Split,
            PaneHeaderIcon::SplitStacked,PaneHeaderIcon::SplitThree,PaneHeaderIcon::SplitFour};
        return kIcons[preset];
    };
    if (layout.overflow.right > layout.overflow.left) {
        // The customize button (prototype #custBtn, T5): sliders, accent
        // while the popover is open.
        const bool open=ToolbarPickerOpen();
        DrawButton(layout.overflow,theme,!plates && IsHovered(vm,HitTestResult::ToolbarMore) ? theme.fill_hover
                   : open ? theme.fill_selected : kTransparent,
            L"\xE9E9",L"",open ? theme.accent : theme.text_secondary,true,true);
    }
    // Pinned commands (自定义工具栏), after a divider.
    const ToolbarPinDragState& pin_drag=PinDragFor(this);
    for (size_t i=0;i<layout.pins.size() && i<vm.toolbar_pins.size();++i) {
        const D2D1_RECT_F slot=layout.pins[i];
        if (slot.right <= slot.left) continue;
        const SelectionChipAction& pin=vm.toolbar_pins[i];
        const bool split_pin=pin.glyph==kToolbarSplitGlyph;
        if (i==0) {
            MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
            FillRect(dc,brStrokeDivider_.get(),slot.left-7*scale_,slot.top+7*scale_,scale_,18*scale_);
        }
        const bool dragged=pin_drag.index==static_cast<int>(i);
        const bool preview=pin_drag.preview_command!=0 && pin.command==pin_drag.preview_command;
        // Toggle tools that are on look like the selected pane buttons.
        const bool hovered=pin.enabled && IsHovered(vm,HitTestResult::ToolbarPin,static_cast<int>(i));
        D2D1_COLOR_F bg=hovered && !plates ? theme.fill_hover : pin.checked ? theme.fill_selected : kTransparent;
        D2D1_COLOR_F fg=!pin.enabled ? theme.text_disabled : pin.checked ? theme.accent : theme.text_secondary;
        if (dragged && pin_drag.out) { fg.a*=0.35f; bg.a*=0.35f; }
        PinMotion& pm=tm.pins.Touch(L"c"+std::to_wstring(pin.command),pin.checked,motion_frame_,motion_now_,tm.animations);
        float turn=0.0f, glyph_scale=1.0f;
        tm.pins.Turn(pm,motion_now_,turn,glyph_scale);
        const D2D1_RECT_F b=tm.pins.Place(pm,slot,static_cast<int>(i),pin_drag,motion_now_,tm.animations);
        if (i < 64 && (layout.pin_dividers >> i & 1)) {
            // The user's separator (自定义工具栏): a short line like the one before the apps.
            const float x=(dragged ? slot.left : b.left)-6.5f*scale_;
            MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
            FillRect(dc,brStrokeDivider_.get(),x,slot.top+9*scale_,scale_,14*scale_);
        }
        const D2D1_POINT_2F centre=D2D1::Point2F((b.left+b.right)*0.5f,(b.top+b.bottom)*0.5f);
        const AroundCentre pop(dc,centre,tm.pins.Pop(pm,motion_now_)*(dragged ? 1.08f : 1.0f));
        if (preview) {
            // The picker's hovered tool: where it would go, dashed and faint.
            D2D1_COLOR_F line=theme.accent;
            line.a*=0.7f;
            ComPtr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(line,&brush);
            const float h=0.5f*scale_;
            if (brush.get())
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left+h,b.top+h,b.right-h,b.bottom-h),
                    theme.radius_control*scale_,theme.radius_control*scale_),brush.get(),scale_,PreviewDashes(dc));
            D2D1_COLOR_F faint=theme.text_secondary;
            faint.a*=0.5f;
            DrawIconText(b.left,b.top,b.right-b.left,b.bottom-b.top,pin.glyph,L"",faint);
        } else if (turn==0.0f && glyph_scale==1.0f) {
            DrawButton(b,theme,bg,split_pin ? L"" : pin.glyph.c_str(),L"",fg,true,true);
            if (split_pin) DrawPaneHeaderIcon(b,split_icon(),fg);
        } else {
            if (bg.a > 0.0f) painter_.FillRoundedRect(b,theme.radius_control*scale_,bg);
            const AroundCentre spin(dc,centre,glyph_scale,turn);
            if (split_pin) DrawPaneHeaderIcon(b,split_icon(),fg);
            else DrawIconText(b.left,b.top,b.right-b.left,b.bottom-b.top,pin.glyph,L"",fg);
        }
    }
    DrawToolbarApps(vm,layout,theme);
    // The picker's hovered pinned tile: its pin breathes an accent ring
    // (prototype .tb.linked: 1.5 px strong to 3 px soft, 0.9 s each way).
    if (pin_drag.linked>=0 && static_cast<size_t>(pin_drag.linked)<layout.pins.size() &&
        layout.pins[pin_drag.linked].right>layout.pins[pin_drag.linked].left) {
        float k=0.0f;
        if (tm.animations) {
            const float t=static_cast<float>(motion_now_%1800)/900.0f;
            const float tri=t<=1.0f ? t : 2.0f-t;
            k=tri*tri*(3.0f-2.0f*tri);
            toolbar_moving=true;
        }
        const float width=(1.5f+1.5f*k)*scale_;
        D2D1_COLOR_F ring=theme.accent;
        ring.a*=0.7f+(0.22f-0.7f)*k;
        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(ring,&brush);
        if (brush.get()) {
            const D2D1_RECT_F r=layout.pins[pin_drag.linked];
            const float o=width*0.5f, radius=theme.radius_control*scale_+o;
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left-o,r.top-o,r.right+o,r.bottom+o),radius,radius),
                                     brush.get(),width);
        }
    }
    // Pins with no room (narrow window, open filter) are in More: a small
    // count on it says how many.
    if (layout.overflow.right > layout.overflow.left) {
        const size_t total=vm.toolbar_pins.size()+vm.toolbar_apps.size();
        int folded=0;
        for (size_t i=0;i<total;++i)
            if (i>=layout.pins.size() || layout.pins[i].right <= layout.pins[i].left) ++folded;
        if (folded!=tm.folded) {
            if (folded>tm.folded && tm.pins.primed && tm.animations) tm.folded_born=motion_now_ ? motion_now_ : 1;
            tm.folded=folded;
        }
        if (folded>0) {
            float s=1.0f;
            if (tm.folded_born) {
                const float t=static_cast<float>(motion_now_-tm.folded_born);
                if (t < static_cast<float>(motion::SpringSettleMs(motion::spring::Pop))) {
                    s=kPinPopFrom+(1.0f-kPinPopFrom)*motion::SpringProgress(motion::spring::Pop,t);
                    toolbar_moving=true;
                } else {
                    tm.folded_born=0;
                }
            }
            const std::wstring count=folded>9 ? L"9+" : std::to_wstring(folded);
            const float h=16*scale_;
            const float w=(count.size()>1 ? 22.0f : 16.0f)*scale_;
            const D2D1_RECT_F badge=D2D1::RectF(layout.overflow.right-w+3*scale_,layout.overflow.top-2*scale_,
                                                layout.overflow.right+3*scale_,layout.overflow.top-2*scale_+h);
            const AroundCentre pop(dc,D2D1::Point2F((badge.left+badge.right)*0.5f,(badge.top+badge.bottom)*0.5f),s);
            painter_.FillRoundedRect(badge,h*0.5f,theme.accent);
            // The caption box is taller than the pill; let it overhang so the
            // digits are not clipped.
            painter_.DrawText(count,D2D1::RectF(badge.left,badge.top-3*scale_,badge.right,badge.bottom+3*scale_),
                              compositor_->SmallFormat(),theme.accent_text,fluent::HorizontalAlignment::Center,theme.accent);
        }
    } else {
        tm.folded=0;
    }
    tm.pins.Sweep(motion_frame_);
    if (tm.pins.moving) toolbar_moving=true;
    tm.pins.moving=false;
    const auto command = [&](D2D1_RECT_F bounds, const wchar_t* glyph, l10n::StringId label,
                             HitTestResult::Region region, bool dropdown, float turn = 0.0f) {
        fluent::ButtonSpec button;
        button.bounds=bounds;
        button.glyph=glyph;
        button.icon_only=bounds.right-bounds.left < 60*scale_;
        button.text=button.icon_only ? std::wstring_view{} : std::wstring_view{l10n::Get(label)};
        button.kind=fluent::ButtonKind::Transparent;
        button.bordered=false;
        button.drop_down=dropdown && !button.icon_only;
        button.chevron_turn=turn;
        button.state.hovered=!plates && IsHovered(vm,region);
        // The icon is drawn here like every other toolbar icon (20 DIP, the
        // pins' colour): icon only, the painter's padding would shrink it.
        button.skip_glyph=true;
        painter_.DrawButton(button);
        const float icon_left=button.icon_only ? bounds.left : bounds.left+8*scale_;
        const float icon_w=button.icon_only ? bounds.right-bounds.left : 20*scale_;
        DrawIconText(icon_left,bounds.top,icon_w,bounds.bottom-bounds.top,glyph,L"",theme.text_secondary);
    };
    // The divider after 新建 comes with the first pin (above); the pane
    // buttons are pins now (T3d), so More needs none.
    // The shared drum picker turns the chevron of the button that opened it,
    // in step with its open/close fade.
    const float picker_open=group_wheel_.OpenAmount();
    const bool sort_picker=group_wheel_.IsSortPicker();
    if (layout.sort.right > layout.sort.left)
        command(layout.sort,L"\xE8CB",l10n::StringId::ToolbarSort,HitTestResult::ToolbarSort,true,
            sort_picker ? picker_open : 0.0f);
    if (layout.filter.right <= layout.filter.left) {
        // hidden (自定义工具栏 → 排序与筛选)
    } else if (!filter_field) {
        command(layout.filter,kIconFilter,l10n::StringId::ToolbarFilter,HitTestResult::FilterBox,false);
    } else {
        fluent::TextFieldSpec field;
        field.bounds=layout.filter;
        field.placeholder=l10n::Get(l10n::StringId::FilterPlaceholder);
        field.text=vm.pane.filter_text;
        field.leading_glyph=kIconFilter;
        field.compact_leading_glyph=true;
        field.suppress_text=vm.filter_editing;
        field.state.focused=vm.filter_editing;
        field.state.hovered=IsHovered(vm,HitTestResult::FilterBox);
        if (!vm.pane.filter_text.empty()) field.trailing_width=30;
        painter_.DrawTextField(field);
        if (!vm.pane.filter_text.empty() && vm.pane.filter_expand >= 0.985f) {
            DrawButton(FilterClearRect(rect,vm.pane.filter_expand),theme,
                IsHovered(vm,HitTestResult::FilterClear) ? theme.fill_hover : kTransparent,
                L"\xE711",L"",theme.text_secondary,true,true,kIconSmall);
        }
    }
    // "Group" sits with Sort and Filter so grouping is findable and, once on,
    // always visible: the chip names the grouping and its x switches it off.
    if (layout.group.right > layout.group.left) {
        const D2D1_RECT_F g=layout.group;
        const bool icon_only=g.right-g.left < 60*scale_;
        if (!group_chip) {
            command(g,L"\xF168",l10n::StringId::ToolbarGroup,HitTestResult::ToolbarGroup,true,
                sort_picker ? 0.0f : picker_open);
        } else if (icon_only) {
            DrawButton(g,theme,IsHovered(vm,HitTestResult::ToolbarGroup) ? theme.fill_hover : theme.fill_selected,
                L"\xF168",L"",theme.accent,true,true);
        } else {
            const float radius=(g.bottom-g.top)*0.5f;
            D2D1_COLOR_F fill=theme.accent;
            fill.a*=IsHovered(vm,HitTestResult::ToolbarGroup) ? 0.26f : 0.16f;
            painter_.FillRoundedRect(g,radius,fill);
            DrawIconText(g.left+6*scale_,g.top,24*scale_,g.bottom-g.top,L"\xF168",L"",theme.accent,kIconField);
            const D2D1_RECT_F x=ToolbarGroupClearRect(g,scale_);
            if (IsHovered(vm,HitTestResult::ToolbarGroupClear)) {
                D2D1_COLOR_F xf=theme.accent;
                xf.a*=0.28f;
                painter_.FillRoundedRect(x,(x.bottom-x.top)*0.5f,xf);
            }
            DrawIconText(x.left,x.top,x.right-x.left,x.bottom-x.top,kIconCloseSmall,L"x",theme.accent,kIconSmall);
            std::wstring group_label=ToolbarGroupLabel();
            const D2D1_RECT_F text_rc=D2D1::RectF(g.left+30*scale_,g.top,x.left-2*scale_,g.bottom);
            if (IDWriteFactory2* f=compositor_->DwriteFactory()) {
                group_label=FitEndEllipsis(group_label,std::max(0.0f,text_rc.right-text_rc.left),
                    [&](const std::wstring& t){ return MeasureTextWidth(f,compositor_->TextFormat(),t); });
            }
            painter_.DrawText(group_label,text_rc,compositor_->TextFormat(),theme.accent);
        }
    }
    // Selection context (prototype note 4): shows up after the file commands
    // while something is selected and folds away with the selection.
    const ToolbarSelectionChip sel=SelectionChipAt(layout,rect.right,vm.toolbar_drop_mode ? 0 : vm.pane.selected_count,
        static_cast<int>(vm.selection_actions.size()));
    if (sel.chip.right > sel.chip.left) {
        FillRect(dc,brStrokeDivider_.get(),sel.chip.left-7*scale_,sel.chip.top+7*scale_,scale_,18*scale_);
        D2D1_COLOR_F fill=theme.accent;
        fill.a*=0.10f;
        painter_.FillRoundedRect(sel.chip,7*scale_,fill);
        D2D1_COLOR_F edge=theme.accent;
        edge.a*=0.28f;
        ComPtr<ID2D1SolidColorBrush> edge_brush;
        dc->CreateSolidColorBrush(edge,&edge_brush);
        if (edge_brush.get()) {
            const float h=0.5f*scale_;
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sel.chip.left+h,sel.chip.top+h,
                sel.chip.right-h,sel.chip.bottom-h),7*scale_,7*scale_),edge_brush.get(),scale_);
        }
        D2D1_COLOR_F hot=theme.accent;
        hot.a*=0.22f;
        if (sel.label.right > sel.label.left) {
            // The count opens the selection menu: it lights up like a button.
            if (IsHovered(vm,HitTestResult::ToolbarSelAction,kSelChipLabel))
                painter_.FillRoundedRect(D2D1::RectF(sel.label.left-6*scale_,sel.label.top+2*scale_,
                    sel.label.right+4*scale_,sel.label.bottom-2*scale_),5*scale_,hot);
            painter_.DrawText(SelectionChipLabel(vm.pane.selected_count),sel.label,compositor_->SmallFormat(),theme.accent);
        }
        int last_action=-1;
        for (int i=0;i<kSelChipSlots;++i) {
            const D2D1_RECT_F b=sel.buttons[static_cast<size_t>(i)];
            if (b.right <= b.left) continue;
            if (i < kSelChipPin) last_action=i;
            const bool action=i < kSelChipPin && static_cast<size_t>(i) < vm.selection_actions.size();
            const std::wstring glyph=i==kSelChipMore ? std::wstring(L"\xE712")
                : i==kSelChipPin ? std::wstring(L"\xE718")
                : i==kSelChipClear ? std::wstring(kIconCloseSmall)
                : action ? vm.selection_actions[static_cast<size_t>(i)].glyph : std::wstring();
            DrawButton(b,theme,IsHovered(vm,HitTestResult::ToolbarSelAction,i) ? hot : kTransparent,
                glyph,L"",theme.accent,true,true,i==kSelChipClear ? kIconSmall : 1.0f);
            if (action && vm.selection_actions[static_cast<size_t>(i)].situational) {
                // Offered because of the moment: an accent underline.
                const float cx=(b.left+b.right)*0.5f;
                painter_.FillRoundedRect(D2D1::RectF(cx-6*scale_,b.bottom-2.5f*scale_,cx+6*scale_,b.bottom-0.5f*scale_),
                    scale_,theme.accent);
                const bool next_common=static_cast<size_t>(i+1) < vm.selection_actions.size() && i+1 < kSelChipPin &&
                    sel.buttons[static_cast<size_t>(i+1)].right > sel.buttons[static_cast<size_t>(i+1)].left &&
                    !vm.selection_actions[static_cast<size_t>(i+1)].situational;
                if (next_common)   // situational | the user's own
                    FillRect(dc,brStrokeDivider_.get(),b.right+0.5f*scale_,b.top+8*scale_,scale_,b.bottom-b.top-16*scale_);
            }
        }
        if (last_action >= 0) {
            // Quick actions | more and clear.
            const D2D1_RECT_F b=sel.buttons[static_cast<size_t>(last_action)];
            FillRect(dc,brStrokeDivider_.get(),b.right+4.5f*scale_,b.top+7*scale_,scale_,b.bottom-b.top-14*scale_);
        }
    }
}
std::wstring MainRenderer::ToolbarGroupLabel() const {
    constexpr l10n::StringId names[]={l10n::StringId::GroupByName,l10n::StringId::GroupByDate,
        l10n::StringId::GroupByType,l10n::StringId::GroupBySize,l10n::StringId::GroupByTag,
        l10n::StringId::Location};
    std::wstring label=l10n::Get(l10n::StringId::ToolbarGroupActive);
    const size_t at=label.find(L"{g}");
    if (at!=std::wstring::npos) label.replace(at,3,l10n::Get(names[std::clamp(toolbar_group_,1,6)-1]));
    return label;
}

void GulpToolbarApp(const void* renderer, int app) {
    Gulps()[renderer] = ToolbarGulp{app, 0};   // born on the next frame
}
void SetToolbarPinDrag(const void* renderer, const ToolbarPinDragState& state) {
    if (state.index < 0 && state.preview_command == 0 && state.linked < 0) PinDrags().erase(renderer);
    else PinDrags()[renderer] = state;
}
// 收藏栏 (toolbar redesign §8): the row under the command row, as wide as the
// toolbar; icons only when the toolbar is narrower than 820 DIP.
FavoritesBarLayout MainRenderer::FavoritesBarLayoutAt(const WindowViewModel& vm, float w) const {
    const float left=EffectiveSidebarWidth(w);
    const float bottom=title_bar_height_+toolbar_height_;
    const D2D1_RECT_F band=D2D1::RectF(left+margin_-10*scale_,bottom-kFavoritesBarHeightDip*scale_,
                                       w-margin_+8*scale_,bottom);
    const bool icon_only=vm.favorites_bar_icon_only || w-left < kFavoritesBarIconOnlyBelowDip*scale_;
    std::vector<float> widths;
    std::vector<bool> separators;
    widths.reserve(vm.favorites_bar.size());
    separators.reserve(vm.favorites_bar.size());
    // A folder keeps 12 DIP more for the ⌄ it shows on hover, so the name
    // does not shorten under the pointer; a group always shows its count and ⌄.
    for (const auto& f : vm.favorites_bar) {
        separators.push_back(f.separator);
        float tw=0.0f;
        if (!icon_only && f.group) tw=CellTextWidth(f.label,true)+CellTextWidth(std::to_wstring(f.count),true)+24*scale_;
        else if (!icon_only && !f.separator) tw=CellTextWidth(f.label,true)+(f.folder && !f.offline ? 12*scale_ : 0.0f);
        widths.push_back(tw);
    }
    return MakeFavoritesBarLayout(band,scale_,widths,icon_only,separators,true);
}

void MainRenderer::DrawFavoritesBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    struct IconScope {
        float* stroke; float* box;
        ~IconScope() { *stroke=0.0f; *box=20.0f; }
    } icon_scope{&icon_stroke_px_,&icon_box_dip_};
    icon_stroke_px_=kIconStrokeDip*scale_;
    icon_box_dip_=kIconBoxDip;
    const float bottom=title_bar_height_+toolbar_height_;
    const float top=bottom-kFavoritesBarHeightDip*scale_;
    const FavoritesBarLayout lay=FavoritesBarLayoutAt(vm,rect.right);
    if (lay.dock.right>lay.dock.left) {
        // 放回侧边栏: the list goes back to the sidebar (Ctrl+Shift+B).
        const bool hot=IsHovered(vm,HitTestResult::FavoritesBarDock);
        if (hot) painter_.FillRoundedRect(lay.dock,6*scale_,theme.fill_hover);
        DrawIconText(lay.dock.left,lay.dock.top,lay.dock.right-lay.dock.left,lay.dock.bottom-lay.dock.top,
                     L"\xE90C",L"",hot ? theme.text : WithAlpha(theme.text_secondary,0.8f),kIconField);
    }
    if (vm.favorites_bar.empty()) {
        // Nothing starred yet: say how to add something.
        const float x=EffectiveSidebarWidth(rect.right)+margin_;
        const float h=30*scale_;
        const float text_right=lay.dock.right>lay.dock.left ? lay.dock.left-6*scale_ : rect.right-margin_;
        DrawIconText(x,top,20*scale_,h,L"\xE734",L"",theme.text_secondary,kIconField);
        painter_.DrawText(vm.favorites_bar_hint,D2D1::RectF(x+26*scale_,top,text_right,top+h),
                          compositor_->SmallFormat(),theme.text_secondary);
        return;
    }
    const float radius=6*scale_;
    const auto measure=[&](const std::wstring& text) { return CellTextWidth(text,true); };
    ID2D1DeviceContext* dc=compositor_->Dc();
    ToolbarMotion& tm=ToolbarMotionFor(this);
    // A dragged favorite rides the toolbar pins' motion (PinMotions): it
    // follows the pointer, the others slide to their new places.
    ToolbarPinDragState drag;
    drag.index=vm.favorites_bar_drag;
    drag.left=vm.favorites_bar_drag_left;
    drag.dy=vm.favorites_bar_drag_dy;
    drag.out=vm.favorites_bar_drag_out;
    // Keyboard focus (F6): the Fluent focus stroke, 2px in the text colour.
    const auto draw_focus=[&](const D2D1_RECT_F& b) {
        ComPtr<ID2D1SolidColorBrush> pen;
        dc->CreateSolidColorBrush(theme.text,&pen);
        const float o=1*scale_;
        if (pen.get())
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left-o,b.top-o,b.right+o,b.bottom+o),radius+o,radius+o),
                                     pen.get(),2*scale_);
    };
    const auto draw_item=[&](size_t i) {
        const D2D1_RECT_F slot=lay.items[i];
        const auto& f=vm.favorites_bar[i];
        const bool dragged=drag.index==static_cast<int>(i);
        PinMotion& pm=tm.favorites.Touch(L"f"+f.path,false,motion_frame_,motion_now_,tm.animations);
        const D2D1_RECT_F b=tm.favorites.Place(pm,slot,static_cast<int>(i),drag,motion_now_,tm.animations);
        if (f.separator) {
            // 收藏夹's separator: the toolbar's short divider line.
            MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
            FillRect(dc,brStrokeDivider_.get(),std::round((b.left+b.right)*0.5f),b.top+7*scale_,scale_,b.bottom-b.top-14*scale_);
            return;
        }
        const D2D1_POINT_2F centre=D2D1::Point2F((b.left+b.right)*0.5f,(b.top+b.bottom)*0.5f);
        const AroundCentre pop(dc,centre,tm.favorites.Pop(pm,motion_now_)*(dragged ? 1.08f : 1.0f));
        const float fade=dragged && drag.out ? 0.35f : f.offline ? 0.45f : 1.0f;
        const bool chevron_hot=IsHovered(vm,HitTestResult::FavoritesBarChevron,static_cast<int>(i));
        const bool hot=vm.favorites_bar_drag<0 &&
            (chevron_hot || IsHovered(vm,HitTestResult::FavoritesBarItem,static_cast<int>(i)));
        if (f.current || hot || dragged) {
            D2D1_COLOR_F bg=theme.fill_hover;
            bg.a*=fade;
            painter_.FillRoundedRect(b,radius,bg);
        }
        if (vm.favorites_bar_drop==static_cast<int>(i)) {
            // Files dragged over a favorite folder: the drop-target ring
            // (ui.md §5.2 rule 7), like the breadcrumb.
            ComPtr<ID2D1SolidColorBrush> ring;
            dc->CreateSolidColorBrush(theme.accent,&ring);
            if (ring.get()) dc->DrawRoundedRectangle(D2D1::RoundedRect(b,radius,radius),ring.get(),2*scale_);
        }
        if (vm.favorites_bar_focus==static_cast<int>(i)) draw_focus(b);
        // The sidebar's starred folder colour; files in the secondary text.
        D2D1_COLOR_F icon=f.folder ? HexColor(0xFBBF24) : theme.text_secondary;
        D2D1_COLOR_F ink=theme.text;
        icon.a*=fade;
        ink.a*=fade;
        const float icon_x=lay.icon_only ? std::round((b.left+b.right)*0.5f-10*scale_) : b.left+5*scale_;
        DrawIconText(icon_x,b.top,20*scale_,b.bottom-b.top,f.group ? command_icons::kGlyphFolderList : f.folder ? L"\xE8B7" : L"\xE8A5",L"",
                     icon,kIconField);
        if (f.group) {
            // A group: its name, how many, and ⌄ (a click lists them).
            if (!lay.icon_only) {
                const std::wstring count=std::to_wstring(f.count);
                const float count_w=CellTextWidth(count,true);
                const float chevron_left=b.right-20*scale_;
                const float count_left=chevron_left-count_w-2*scale_;
                const float text_left=b.left+29*scale_;
                painter_.DrawText(FitEndEllipsis(f.label,count_left-6*scale_-text_left,measure),
                                  D2D1::RectF(text_left,b.top,count_left-4*scale_,b.bottom),compositor_->SmallFormat(),ink);
                painter_.DrawText(count,D2D1::RectF(count_left,b.top,chevron_left+2*scale_,b.bottom),
                                  compositor_->SmallFormat(),theme.text_secondary);
                DrawIconText(chevron_left,b.top,17*scale_,b.bottom-b.top,L"\xE70D",L"",theme.text_secondary,kIconSmall);
            }
            if (f.current)
                painter_.FillRoundedRect(D2D1::RectF(b.left+8*scale_,b.bottom-3*scale_,b.right-8*scale_,b.bottom-1*scale_),
                                         scale_,theme.accent);
            return;
        }
        if (f.offline)   // §8 不可用: dimmed, with a small ⚠ on the icon
            DrawIconText(icon_x+9*scale_,b.top+8*scale_,12*scale_,b.bottom-b.top-8*scale_,L"\xE7BA",L"!",
                         HexColor(0xF59E0B),0.5f);
        if (!lay.icon_only) {
            // A hovered folder shows ⌄ in its last 20 DIP (the subfolders).
            const bool chevron=hot && f.folder && !f.offline;
            const float text_left=b.left+29*scale_;
            const float text_right=b.right-(chevron ? 20 : 8)*scale_;
            painter_.DrawText(FitEndEllipsis(f.label,text_right-text_left,measure),
                              D2D1::RectF(text_left,b.top,text_right+2*scale_,b.bottom),
                              compositor_->SmallFormat(),ink);
            if (chevron) {
                if (chevron_hot) {
                    D2D1_COLOR_F plate=theme.fill_hover;
                    painter_.FillRoundedRect(D2D1::RectF(b.right-20*scale_,b.top+4*scale_,b.right-3*scale_,b.bottom-4*scale_),
                                             4*scale_,plate);
                }
                DrawIconText(b.right-20*scale_,b.top,17*scale_,b.bottom-b.top,L"\xE70D",L"",theme.text_secondary,kIconSmall);
            }
        }
        if (f.current)   // prototype .fav.cur: a 2px accent underline
            painter_.FillRoundedRect(D2D1::RectF(b.left+8*scale_,b.bottom-3*scale_,b.right-8*scale_,b.bottom-1*scale_),
                                     scale_,theme.accent);
    };
    // The dragged one last, over its neighbours.
    for (size_t i=0;i<vm.favorites_bar.size() && i<lay.items.size();++i) {
        if (lay.items[i].right<=lay.items[i].left || drag.index==static_cast<int>(i)) continue;
        draw_item(i);
    }
    if (drag.index>=0 && static_cast<size_t>(drag.index)<vm.favorites_bar.size() &&
        static_cast<size_t>(drag.index)<lay.items.size() &&
        lay.items[static_cast<size_t>(drag.index)].right>lay.items[static_cast<size_t>(drag.index)].left)
        draw_item(static_cast<size_t>(drag.index));
    tm.favorites.Sweep(motion_frame_);
    if (tm.favorites.moving) RequestMotionFrame();
    tm.favorites.moving=false;
    if (vm.favorites_bar_drop_add) {
        // Items over the bar's empty part: where they would land, after the last.
        float x=EffectiveSidebarWidth(rect.right)+8*scale_;
        for (const auto& item : lay.items) if (item.right>item.left) x=std::max(x,item.right);
        painter_.FillRoundedRect(D2D1::RectF(x+3*scale_,top+7*scale_,x+5*scale_,top+27*scale_),scale_,theme.accent);
    }
    if (lay.first_hidden>=0) {
        const D2D1_RECT_F m=lay.more;
        if (IsHovered(vm,HitTestResult::FavoritesBarMore)) painter_.FillRoundedRect(m,radius,theme.fill_hover);
        painter_.DrawText(L"\u00BB",D2D1::RectF(m.left+10*scale_,m.top,m.right,m.bottom),
                          compositor_->SmallFormat(),theme.text_secondary);
        if (vm.favorites_bar_focus==static_cast<int>(vm.favorites_bar.size())) draw_focus(m);
    }
}

}
