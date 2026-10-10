#include "shortcut_help.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "typography.h"
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace pulse::ui {
namespace {
using Id = l10n::StringId;
struct HelpRow { Id label; std::wstring keys; };
struct HelpGroup { Id title; std::vector<HelpRow> rows; };

// Every shortcut HandleKeyDown (app_input.cpp) and WM_SYSKEYDOWN (app_main.cpp)
// handle, grouped. In the two-column layout the first kLeftGroups groups fill
// the left column.
constexpr size_t kLeftGroups = 2;
std::vector<HelpGroup> HelpGroups() {
    return {
        {Id::HelpGroupNav, {
            {Id::HelpOpen, l10n::Pick(L"Enter / 双击", L"Enter / Double-click")},
            {Id::HelpBackForward, L"Backspace / Alt ← / →"},
            {Id::Up, L"Alt ↑"},
            {Id::HelpGoToPath, L"Ctrl L / Alt D / F4"},
            {Id::Refresh, L"F5"},
            {Id::HelpMoveFocus, L"↑ ↓ PgUp PgDn Home End"},
        }},
        {Id::HelpGroupFiles, {
            {Id::QuickPreview, L"Space"},
            {Id::HelpClipboard, L"Ctrl C / X / V"},
            {Id::CopyPath, L"Ctrl Shift C"},
            {Id::Undo, L"Ctrl Z"},
            {Id::Rename, L"F2"},
            {Id::BatchRename, L"Ctrl Shift R"},
            {Id::NewFolder, L"Ctrl Shift N / F7"},
            {Id::Delete, L"Delete / Ctrl D"},
            {Id::PermanentDelete, L"Shift Delete"},
            {Id::Properties, L"Alt Enter"},
            {Id::HelpContextMenu, L"Shift F10"},
            {Id::HelpToggleTag, L"Ctrl Alt Shift 1–7"},
        }},
        {Id::HelpGroupSearch, {
            {Id::Search, L"Ctrl K"},
            {Id::HelpCommandBar, L"Ctrl Shift K"},
            {Id::HelpProject, L"Ctrl P"},
            {Id::HelpFilter, L"Ctrl F / Ctrl E / F3"},
            {Id::AdvancedSearch, L"Ctrl Shift F"},
        }},
        {Id::HelpGroupSelect, {
            {Id::SelectAll, L"Ctrl A"},
            {Id::InvertSelection, L"Ctrl I"},
            {Id::HelpSelectPattern, L"Ctrl Shift A"},
            {Id::SelectionHint, l10n::Get(Id::SelectionKeys)},
        }},
        {Id::HelpGroupTabs, {
            {Id::TooltipNewTab, L"Ctrl T"},
            {Id::TabClose, L"Ctrl W"},
            {Id::HelpSwitchTab, L"Ctrl Tab / Ctrl 1–9"},
            {Id::SplitLayout, L"Ctrl Alt 1 / 2 / 3 / 4"},
            {Id::HelpViewMode, L"Ctrl Shift 1–8"},
            {Id::HelpNextPane, L"F6"},
            {Id::HelpMarkTarget, L"Ctrl Alt D"},
            {Id::HelpTransferTarget, L"Ctrl Alt C / X"},
            {Id::HelpSidebar, L"Ctrl B"},
            {Id::Maximize, L"F11"},
        }},
    };
}

// Compact metrics in DIPs, shared by layout and drawing so the scroll extent
// always matches what is painted.
constexpr float kPad = 24, kHeader = 86, kFooter = 40, kRowH = 28, kGroupH = 32,
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
    l.close = D2D1::RectF(l.card.right-48*scale, l.card.top+16*scale,
                         l.card.right-16*scale, l.card.top+48*scale);
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
    const auto tint = dark ? HexColor(0x2B324B) : HexColor(0xEEF1FD);
    const auto heading = MakeTheme(dark, accent).accent;
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
    if (close_hover) fill(l.close,tint,6);
    brush->SetColor(muted);
    const float cx=(l.close.left+l.close.right)/2, cy=(l.close.top+l.close.bottom)/2;
    dc->DrawLine({cx-5*scale,cy-5*scale},{cx+5*scale,cy+5*scale},brush.get(),1.5f*scale);
    dc->DrawLine({cx+5*scale,cy-5*scale},{cx-5*scale,cy+5*scale},brush.get(),1.5f*scale);

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

