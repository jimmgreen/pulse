#include "shortcut_help.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "command_icons.h"
#include "typography.h"
#include "../app/keymap.h"
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace pulse::ui {
namespace {
using Id = l10n::StringId;
// `actions`: the keymap actions the row shows; once the user changes one of
// them the row lists the user's keys instead of `keys`.
struct HelpRow { Id label; std::wstring keys; std::vector<app::KeyAction> actions = {}; };
struct HelpGroup { Id title; std::vector<HelpRow> rows; };

// What the key below Esc types on this keyboard layout; the same label the
// window switcher hint shows (app/window_switcher_ui.cpp SwitcherKeyLabel).
std::wstring SwitcherKey() {
    const UINT vk = MapVirtualKeyW(0x29, MAPVK_VSC_TO_VK);
    const UINT ch = vk ? (MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu) : 0;  // high bit: dead key
    if (ch >= 0x20) return std::wstring(1, static_cast<wchar_t>(ch));
    wchar_t name[32]{};
    if (GetKeyNameTextW(0x29 << 16, name, 32) > 0) return name;
    return L"`";
}

// Every shortcut HandleKeyDown (app_input.cpp) and WM_SYSKEYDOWN (app_main.cpp)
// handle, grouped. In the two-column layout the first kLeftGroups groups fill
// the left column; tabs and windows stay side by side on the right.
constexpr size_t kLeftGroups = 3;
std::vector<HelpGroup> HelpGroups() {
    using K = app::KeyAction;
    std::vector<HelpGroup> groups = {
        {Id::HelpGroupNav, {
            {Id::HelpOpen, l10n::Get(l10n::StringId::EnterDoubleClick).c_str()},
            {Id::HelpBackForward, L"Backspace / Alt ← / →", {K::Back, K::Forward}},
            {Id::Up, L"Alt ↑", {K::Up}},
            {Id::HelpGoToPath, L"Ctrl L / Alt D / F4", {K::GoToPath}},
            {Id::Refresh, L"F5", {K::Refresh}},
            {Id::HelpMoveFocus, L"↑ ↓ PgUp PgDn Home End"},
        }},
        {Id::HelpGroupFiles, {
            {Id::QuickPreview, L"Space", {K::QuickPreview}},
            {Id::HelpClipboard, L"Ctrl C / X / V", {K::Copy, K::Cut, K::Paste}},
            {Id::CopyPath, L"Ctrl Shift C", {K::CopyPath}},
            {Id::Undo, L"Ctrl Z", {K::Undo}},
            {Id::Rename, L"F2", {K::Rename}},
            {Id::BatchRename, L"Ctrl Shift R", {K::BatchRename}},
            {Id::NewFolder, L"Ctrl Shift N / F7", {K::NewFolder}},
            {Id::Delete, L"Delete / Ctrl D", {K::Delete}},
            {Id::PermanentDelete, L"Shift Delete", {K::PermanentDelete}},
            {Id::Properties, L"Alt Enter", {K::Properties}},
            {Id::HelpContextMenu, L"Shift F10", {K::ContextMenu}},
            {Id::HelpToggleTag, L"Alt 1–9", {K::ToggleTag}},
            {Id::HelpClearTags, L"Alt 0", {K::ClearTags}},
            {Id::HelpTagPanel, L"Alt T", {K::TagPanel}},
        }},
        {Id::HelpGroupSelect, {
            {Id::SelectAll, L"Ctrl A", {K::SelectAll}},
            {Id::InvertSelection, L"Ctrl I", {K::InvertSelection}},
            {Id::HelpSelectPattern, L"Ctrl Shift A", {K::SelectPattern}},
            {Id::SelectionHint, l10n::Get(Id::SelectionKeys)},
        }},
        {Id::HelpGroupSearch, {
            {Id::Search, L"Ctrl K", {K::Search}},
            {Id::HelpCommandBar, L"Ctrl Shift K", {K::CommandBar}},
            {Id::HelpProject, L"Ctrl P", {K::Project}},
            {Id::HelpFilter, L"Ctrl F / Ctrl E / F3", {K::Filter}},
            {Id::AdvancedSearch, L"Ctrl Shift F", {K::AdvancedSearch}},
        }},
        {Id::HelpGroupTabs, {
            {Id::TooltipNewTab, L"Ctrl T", {K::NewTab}},
            {Id::TabClose, L"Ctrl W", {K::CloseTab}},
            {Id::HelpSwitchTab, L"Ctrl Tab / Ctrl 1–9", {K::NextTab, K::TabNumber}},
            {Id::SplitLayout, L"Ctrl Alt 1 / 2 / 3 / 4", {K::Layout}},
            {Id::HelpViewMode, L"Ctrl Shift 1–8", {K::ViewMode}},
            {Id::HelpNextPane, L"F6", {K::NextPane}},
            {Id::HelpMarkTarget, L"Ctrl Alt D", {K::MarkTarget}},
            {Id::HelpTransferTarget, L"Ctrl Alt C / X", {K::CopyToTarget, K::MoveToTarget}},
            {Id::HelpSidebar, L"Ctrl B", {K::Sidebar}},
            {Id::Maximize, L"F11", {K::Maximize}},
        }},
        {Id::HelpGroupWindows, {
            {Id::HelpNewWindow, L"Win E", {K::NewWindow}},
            {Id::HelpCloseWindow, L"Ctrl Shift W / Alt F4", {K::CloseWindow}},
            {Id::HelpTabToWindow, L"Ctrl Shift M", {K::TabToWindow}},
            {Id::HelpReopenClosed, L"Ctrl Shift T", {K::ReopenClosed}},
            {Id::HelpSwitchWindow, L"Ctrl " + SwitcherKey(), {K::SwitchWindow}},
            {Id::HelpWindowOverview, L"Ctrl Shift O", {K::Overview}},
            {Id::HelpMergeWindows, L"Ctrl Shift J", {K::MergeWindows}},
        }},
    };
    for (auto& group : groups) {
        for (auto& row : group.rows) {
            std::wstring keys = app::CustomizedKeysText(row.actions);
            if (keys.empty()) continue;
            if (row.label == Id::HelpCloseWindow) keys += L" / Alt F4";  // fixed
            row.keys = std::move(keys);
        }
    }
    return groups;
}

// Compact metrics in DIPs, shared by layout and drawing so the scroll extent
// always matches what is painted. 26 DIP rows (22 DIP key chips) keep all
// groups unscrolled in a 1600x900 window now that tags have keys.
constexpr float kPad = 24, kHeader = 86, kFooter = 40, kRowH = 26, kGroupH = 32,
                kColumnGap = 32;
float GroupHeightDip(const HelpGroup& g) {
    return kGroupH + kRowH * static_cast<float>(g.rows.size());
}
float ColumnHeightDip(const std::vector<HelpGroup>& groups, size_t first, size_t last) {
    float h = 0;
    for (size_t i = first; i < last && i < groups.size(); ++i) h += GroupHeightDip(groups[i]);
    return h;
}
}

ShortcutHelpLayout LayoutShortcutHelp(float width, float height, float scale) {
    ShortcutHelpLayout l;
    const float w = std::max(0.0f, std::min(820.0f * scale, width - 32 * scale));
    l.narrow = w < 640 * scale;
    const auto groups = HelpGroups();
    const float content = l.narrow
        ? ColumnHeightDip(groups, 0, groups.size())
        : std::max(ColumnHeightDip(groups, 0, kLeftGroups),
                   ColumnHeightDip(groups, kLeftGroups, groups.size()));
    l.content_height = content * scale;
    const float header = kHeader + (l.narrow ? 20.0f : 0.0f);
    // The card hugs its content and only scrolls when the window is short.
    const float wanted = (header + content + kFooter) * scale;
    const float h = std::max(0.0f, std::min(wanted, height - 32 * scale));
    l.card = D2D1::RectF((width-w)/2, (height-h)/2, (width+w)/2, (height+h)/2);
    l.close = D2D1::RectF(l.card.right-46*scale, l.card.top,
                         l.card.right, l.card.top+36*scale);
    l.body = D2D1::RectF(l.card.left+kPad*scale, l.card.top+header*scale,
                        l.card.right-kPad*scale, l.card.bottom-kFooter*scale);
    l.max_scroll = std::max(0.0f, l.content_height-(l.body.bottom-l.body.top));
    return l;
}

void DrawShortcutHelp(Compositor& compositor, bool dark, D2D1_COLOR_F accent,
                      float scale, float scroll, bool close_hover) {
    auto* dc = compositor.Dc();
    if (!dc) return;
    const auto l = LayoutShortcutHelp(static_cast<float>(compositor.Width()),
                                      static_cast<float>(compositor.Height()), scale);
    const auto surface = dark ? HexColor(0x20242E) : HexColor(0xFFFFFF);
    const auto ink = dark ? HexColor(0xE7EAF5) : HexColor(0x303C54);
    const auto muted = dark ? HexColor(0xA7B3CA) : HexColor(0x71819D);
    const auto line = dark ? HexColor(0x363E50) : HexColor(0xE3E8F3);
    const auto theme = MakeTheme(dark, accent);
    const auto heading = theme.accent;
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(ink, &brush);
    if (!brush.get()) return;
    auto fill = [&](D2D1_RECT_F r, D2D1_COLOR_F color, float radius) {
        brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius*scale, radius*scale), brush.get());
    };
    auto make_format = [&](float size, bool bold) {
        ComPtr<IDWriteTextFormat> format;
        const wchar_t* family = l10n::effective_language() == l10n::Language::ZhTW
            ? L"Microsoft JhengHei" : L"Microsoft YaHei";
        compositor.DwriteFactory()->CreateTextFormat(family, nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size*scale*typography::UiFontScale(), L"", &format);
        return format;
    };
    auto text = [&](std::wstring_view value, D2D1_RECT_F r, float size,
                    D2D1_COLOR_F color, bool bold = false) {
        const auto format = make_format(size, bold);
        if (!format.get()) return;
        brush->SetColor(color);
        dc->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format.get(), r,
                      brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    // Key captions mix Latin, arrows and CJK, so measure them instead of guessing.
    const auto key_format = make_format(11.5f, false);
    auto text_width = [&](std::wstring_view value) {
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (key_format.get() && SUCCEEDED(compositor.DwriteFactory()->CreateTextLayout(
                value.data(), static_cast<UINT32>(value.size()), key_format.get(),
                10000.0f, 100.0f, &layout)) && layout.get() &&
            SUCCEEDED(layout->GetMetrics(&metrics))) {
            return metrics.widthIncludingTrailingWhitespace;
        }
        return static_cast<float>(value.size()) * 7.0f * scale;
    };
    // Soft layered shadow keeps the card distinct from the dimmed owner.
    for (int i=10; i>0; --i) {
        auto r=l.card;
        r.left-=i*scale; r.right+=i*scale; r.top-=(i-4)*scale; r.bottom+=(i+4)*scale;
        fill(r, D2D1::ColorF(0,0,0,0.012f), 18);
    }
    fill(l.card, surface, 14);
    const float left = l.card.left + kPad*scale;
    text(l10n::Get(Id::HintActShortcuts),
         D2D1::RectF(left,l.card.top+18*scale,l.close.left-8*scale,l.card.top+48*scale),
         20,ink,true);
    text(l10n::Get(Id::HelpDescription),
         D2D1::RectF(left,l.card.top+52*scale,l.card.right-kPad*scale,
                     l.body.top-6*scale),12.5f,muted);
    if (close_hover) {
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1RoundedRectangleGeometry> card_clip;
        if (factory.get() && SUCCEEDED(factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(l.card, 14.0f * scale, 14.0f * scale), &card_clip)) && card_clip.get()) {
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), card_clip.get()), nullptr);
            brush->SetColor(theme.danger);
            dc->FillRectangle(l.close, brush.get());
            dc->PopLayer();
        } else {
            brush->SetColor(theme.danger);
            dc->FillRectangle(l.close, brush.get());
        }
    }
    brush->SetColor(close_hover ? HexColor(0xFFFFFF) : ink);
    ComPtr<ID2D1StrokeStyle> close_stroke;
    command_icons::CreateStrokeStyle(dc, &close_stroke);
    const auto close_icon = command_icons::CenteredBounds(l.close, 16.0f * 0.66f * scale);
    if (!command_icons::Draw(dc, brush.get(), close_stroke.get(), command_icons::Icon::Close, close_icon)) {
        const float cx=(l.close.left+l.close.right)/2, cy=(l.close.top+l.close.bottom)/2;
        dc->DrawLine({cx-5*scale,cy-5*scale},{cx+5*scale,cy+5*scale},brush.get(),1.5f*scale);
        dc->DrawLine({cx+5*scale,cy-5*scale},{cx-5*scale,cy+5*scale},brush.get(),1.5f*scale);
    }

    dc->PushAxisAlignedClip(l.body,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float top = l.body.top - std::clamp(scroll,0.0f,l.max_scroll);
    const float gap = kColumnGap*scale;
    const float column_w = l.narrow ? l.body.right-l.body.left
                                    : (l.body.right-l.body.left-gap)/2;
    const auto groups = HelpGroups();
    float y = top;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const bool right_column = !l.narrow && gi >= kLeftGroups;
        if (!l.narrow && gi == kLeftGroups) y = top;
        const float x0 = right_column ? l.body.left+column_w+gap : l.body.left;
        const float x1 = x0 + column_w;
        const auto& group = groups[gi];
        text(l10n::Get(group.title), D2D1::RectF(x0,y+8*scale,x1,y+kGroupH*scale),
             12, heading, true);
        y += kGroupH*scale;
        for (const auto& row : group.rows) {
            const float key_w = std::min(column_w*0.62f, text_width(row.keys)+14*scale);
            const auto key = D2D1::RectF(x1-key_w, y+(kRowH-22)/2*scale,
                                         x1, y+(kRowH+22)/2*scale);
            text(l10n::Get(row.label), D2D1::RectF(x0,y+(kRowH-20)/2*scale,
                 key.left-10*scale,y+(kRowH+20)/2*scale),13,ink);
            fill(key,dark?HexColor(0x282E3B):HexColor(0xF7F8FC),4);
            brush->SetColor(line);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(key,4*scale,4*scale),brush.get(),scale);
            text(row.keys,D2D1::RectF(key.left+7*scale,key.top+3*scale,key.right-2*scale,key.bottom),
                 11.5f,muted);
            y += kRowH*scale;
            brush->SetColor(line);
            dc->DrawLine({x0,y},{x1,y},brush.get(),scale*0.75f);
        }
    }
    dc->PopAxisAlignedClip();
    text(l10n::Get(Id::HelpTip),D2D1::RectF(left,l.body.bottom+10*scale,
         l.card.right-kPad*scale,l.card.bottom-4*scale),11.5f,muted);
    if (l.max_scroll>0) {
        const float track=l.body.bottom-l.body.top;
        const float thumb=std::max(24*scale,track*track/l.content_height);
        const float bar=l.body.top+(track-thumb)*std::clamp(scroll/l.max_scroll,0.0f,1.0f);
        fill(D2D1::RectF(l.card.right-9*scale,bar,l.card.right-6*scale,bar+thumb),line,2);
    }
}

namespace {
struct HelpWindow {
    HWND hwnd{};
    Compositor compositor;
    bool dark=false, done=false, hover=false;
    // Set by a button press inside this window. The release of the click that
    // opened the help (pressed in the owner) must not dismiss it.
    bool pressed=false;
    D2D1_COLOR_F accent{};
    float scale=1, scroll=0;
    void Render() {
        auto* dc=compositor.Dc();
        if (!dc) return;
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.12f,0.16f,0.23f,0.40f));
        DrawShortcutHelp(compositor,dark,accent,scale,scroll,hover);
        if (SUCCEEDED(dc->EndDraw())) compositor.Present();
    }
    static LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<HelpWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (msg==WM_NCCREATE) {
            self=static_cast<HelpWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd=hwnd;
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd,msg,wp,lp);
        const auto l=LayoutShortcutHelp(static_cast<float>(self->compositor.Width()),
            static_cast<float>(self->compositor.Height()),self->scale);
        auto inside=[&](D2D1_RECT_F r) {
            return GET_X_LPARAM(lp)>=r.left && GET_X_LPARAM(lp)<=r.right &&
                   GET_Y_LPARAM(lp)>=r.top && GET_Y_LPARAM(lp)<=r.bottom;
        };
        switch(msg) {
        case WM_CREATE: return self->compositor.Init(hwnd)?0:-1;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps{}; BeginPaint(hwnd,&ps); self->Render(); EndPaint(hwnd,&ps); return 0; }
        case WM_MOUSEMOVE: self->hover=inside(l.close); InvalidateRect(hwnd,nullptr,FALSE); return 0;
        case WM_LBUTTONDOWN: self->pressed=true; return 0;
        case WM_LBUTTONUP:
            if (self->pressed && (!inside(l.card)||inside(l.close))) self->done=true;
            self->pressed=false;
            return 0;
        case WM_MOUSEWHEEL:
            self->scroll=std::clamp(self->scroll-GET_WHEEL_DELTA_WPARAM(wp)*self->scale*0.5f,0.0f,l.max_scroll);
            InvalidateRect(hwnd,nullptr,FALSE); return 0;
        case WM_KEYDOWN:
            if (wp==VK_ESCAPE || wp==VK_RETURN) self->done=true;
            if (wp==VK_DOWN || wp==VK_NEXT) self->scroll=std::min(l.max_scroll,self->scroll+80*self->scale);
            if (wp==VK_UP || wp==VK_PRIOR) self->scroll=std::max(0.0f,self->scroll-80*self->scale);
            if (wp==VK_HOME) self->scroll=0;
            if (wp==VK_END) self->scroll=l.max_scroll;
            InvalidateRect(hwnd,nullptr,FALSE); return 0;
        case WM_CLOSE: self->done=true; return 0;
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
};
}
void ShowShortcutHelp(HWND owner,bool dark,D2D1_COLOR_F accent) {
    HelpWindow window;
    window.dark=dark; window.accent=accent;
    window.scale=static_cast<float>(compat::WindowDpi(owner))/96.0f;
    WNDCLASSW wc{};
    wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=HelpWindow::Proc;
    wc.lpszClassName=L"PulseShortcutHelp"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{}; GetClientRect(owner,&r);
    POINT p{}; ClientToScreen(owner,&p);
    HWND previous=GetFocus();
    HWND hwnd=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,
        l10n::Get(l10n::StringId::ShortcutHints).c_str(),WS_POPUP,
        p.x,p.y,r.right,r.bottom,owner,nullptr,wc.hInstance,&window);
    if (!hwnd) return;
    EnableWindow(owner,FALSE);
    ShowWindow(hwnd,SW_SHOW); SetFocus(hwnd);
    MSG msg{};
    int status=1;
    while (!window.done && (status=GetMessageW(&msg,nullptr,0,0))>0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    EnableWindow(owner,TRUE);
    DestroyWindow(hwnd);
    SetForegroundWindow(owner);
    if (IsWindow(previous)) SetFocus(previous);
    if (status==0) PostQuitMessage(static_cast<int>(msg.wParam));
}
}
