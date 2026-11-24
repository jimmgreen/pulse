#pragma once

#include "host/overlays/desktop_overlays.h"
#include "../../../src/ui/edit_host.h"

#include <windows.h>
#include <commctrl.h>
#include <imm.h>
#include <objbase.h>

#include <cstdio>
#include <string>

namespace pulse::desktop::tests {

inline void PumpHostedEditMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

inline bool CheckHostedEdit(bool cond, const char* label) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", label);
    return cond;
}

inline int RunHostedEditVerification() {
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool all_ok = true;
    std::string last_note_id;
    std::wstring last_note_body;
    std::wstring last_search_query;

    overlays::DesktopOverlayController ov;
    ov.SetDipScale(1.0f);
    ov.SetAppearance(true, 0xFF4CC2FFu);

    overlays::OverlayCallbacks cb{};
    cb.on_sticky_note_changed = [&](const std::string& id, const std::wstring& body) {
        last_note_id = id;
        last_note_body = body;
    };
    cb.on_search_query_changed = [&](const std::wstring& q) {
        last_search_query = q;
    };

    all_ok &= CheckHostedEdit(ov.Initialize(cb), "Initialize DesktopOverlayController");

    // 1. StickyNotePopover: real Pulse child EDIT, focus, cue, caret paint, IME position, live auto-save
    {
        const RECT anchor{960, 260, 1280, 500};
        all_ok &= CheckHostedEdit(ov.OpenStickyNotePopover(anchor, "tile_note", L""),
                                  "OpenStickyNotePopover empty");
        PumpHostedEditMessages();

        HWND edit = FindWindowExW(ov.OverlayHwnd(), nullptr, L"EDIT", nullptr);
        all_ok &= CheckHostedEdit(edit != nullptr && IsWindowVisible(edit),
                                  "StickyNotePopover creates visible child EDIT");
        all_ok &= CheckHostedEdit(GetFocus() == edit,
                                  "StickyNotePopover focuses child EDIT for caret blink");
        all_ok &= CheckHostedEdit(edit && pulse::ui::PulsePaintedChildEditError(edit) == 0,
                                  "DrawPulsePaintedChildEdit succeeded on StickyNotePopover");

        const auto* cue_prop = static_cast<const wchar_t*>(edit ? GetPropW(edit, L"Pulse.EditCue") : nullptr);
        all_ok &= CheckHostedEdit(cue_prop != nullptr && std::wstring(cue_prop) == L"写下待办与灵感…",
                                  "StickyNotePopover attaches multiline placeholder cue");
        all_ok &= CheckHostedEdit(edit && GetPropW(edit, L"Pulse.EditMultiline") != nullptr,
                                  "StickyNotePopover sets Pulse.EditMultiline metric");

        if (edit) {
            RECT ov_rc{}, ed_rc{};
            GetWindowRect(ov.OverlayHwnd(), &ov_rc);
            GetWindowRect(edit, &ed_rc);
            const bool inside_card = ed_rc.left > ov_rc.left && ed_rc.top > ov_rc.top &&
                                     ed_rc.right < ov_rc.right && ed_rc.bottom < ov_rc.bottom;
            all_ok &= CheckHostedEdit(inside_card,
                                      "StickyNotePopover EDIT screen rect sits inside overlay card");

            if (HIMC imc = ImmGetContext(edit)) {
                COMPOSITIONFORM cf{};
                const BOOL got = ImmGetCompositionWindow(imc, &cf);
                ImmReleaseContext(edit, imc);
                const LONG scr_x = ed_rc.left + cf.ptCurrentPos.x;
                const LONG scr_y = ed_rc.top + cf.ptCurrentPos.y;
                const bool ime_ok = got && (cf.dwStyle & CFS_POINT) != 0 &&
                                    scr_x >= ed_rc.left && scr_y >= ed_rc.top &&
                                    scr_x < ed_rc.right && scr_y < ed_rc.bottom;
                if (!ime_ok) {
                    std::printf("  [diag] got=%d style=0x%lX pt=(%ld,%ld) ed=(%ld,%ld..%ld,%ld)\n",
                                got, static_cast<unsigned long>(cf.dwStyle),
                                cf.ptCurrentPos.x, cf.ptCurrentPos.y,
                                ed_rc.left, ed_rc.top, ed_rc.right, ed_rc.bottom);
                }
                all_ok &= CheckHostedEdit(
                    ime_ok,
                    "StickyNotePopover positions IMM32 composition window at editor caret");
            }

            // Exercise caret blink timer tick (kEditCaretTimer == 71 in edit_host.cpp)
            SendMessageW(edit, WM_TIMER, 71, 0);
            PumpHostedEditMessages();
            all_ok &= CheckHostedEdit(pulse::ui::PulsePaintedChildEditError(edit) == 0,
                                      "Caret blink timer tick repaints cleanly");

            const wchar_t* sample = L"周五交汇报\r\n整理安装包";
            SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(sample));
            PumpHostedEditMessages();
            all_ok &= CheckHostedEdit(
                last_note_id == "tile_note" && last_note_body == L"周五交汇报\n整理安装包",
                "StickyNotePopover live-syncs multiline edits with normalized LF");

            // Simulate starting Chinese IME composition on line 2 and verify Win32 EDIT
            // does not reset IMM32 composition window back to CFS_RECT (4,0).
            SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
            if (HIMC imc = ImmGetContext(edit)) {
                COMPOSITIONFORM cf2{};
                const BOOL got2 = ImmGetCompositionWindow(imc, &cf2);
                ImmReleaseContext(edit, imc);
                all_ok &= CheckHostedEdit(
                    got2 && (cf2.dwStyle & CFS_POINT) != 0 &&
                        cf2.ptCurrentPos.x > 30 && cf2.ptCurrentPos.y >= 18,
                    "StickyNotePopover keeps IMM32 caret at line-2 end on WM_IME_STARTCOMPOSITION");
            }
            SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
        }
        ov.Dismiss();
    }

    // 2. Single-line StickyNotePopover (@weather_city): single-line cue banner via ComCtl32 v6
    {
        const RECT anchor{960, 260, 1280, 500};
        all_ok &= CheckHostedEdit(ov.OpenStickyNotePopover(anchor, "@weather_city", L""),
                                  "OpenStickyNotePopover @weather_city");
        PumpHostedEditMessages();

        HWND edit = FindWindowExW(ov.OverlayHwnd(), nullptr, L"EDIT", nullptr);
        all_ok &= CheckHostedEdit(edit != nullptr && GetPropW(edit, L"Pulse.EditMultiline") == nullptr,
                                  "@weather_city uses single-line hosted EDIT");
        if (edit) {
            wchar_t cue_buf[128]{};
            SendMessageW(edit, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(cue_buf), ARRAYSIZE(cue_buf));
            all_ok &= CheckHostedEdit(std::wstring(cue_buf) == L"输入城市名称，如 北京、上海…",
                                      "@weather_city sets ComCtl32 v6 EM_SETCUEBANNER placeholder");
        }
        ov.Dismiss();
    }

    // 3. SearchPopup: single-line cue banner, multi-char Chinese AppendSearchChar, IME caret advance
    {
        const RECT wa{0, 0, 1920, 1040};
        all_ok &= CheckHostedEdit(ov.OpenSearchPopup(wa, L"", {}), "OpenSearchPopup empty");
        PumpHostedEditMessages();

        HWND edit = FindWindowExW(ov.OverlayHwnd(), nullptr, L"EDIT", nullptr);
        all_ok &= CheckHostedEdit(edit != nullptr && GetFocus() == edit,
                                  "SearchPopup focuses child EDIT");
        all_ok &= CheckHostedEdit(edit && pulse::ui::PulsePaintedChildEditError(edit) == 0,
                                  "DrawPulsePaintedChildEdit succeeded on SearchPopup");

        if (edit) {
            wchar_t cue_buf[128]{};
            SendMessageW(edit, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(cue_buf), ARRAYSIZE(cue_buf));
            all_ok &= CheckHostedEdit(
                std::wstring(cue_buf) == L"搜索桌面上的文件，顺便找找 Pulse 里的…",
                "SearchPopup sets EM_SETCUEBANNER placeholder");

            ov.AppendSearchChar(L'下');
            ov.AppendSearchChar(L'载');
            PumpHostedEditMessages();
            all_ok &= CheckHostedEdit(
                ov.CurrentSearchQuery() == L"下载" && last_search_query == L"下载",
                "AppendSearchChar preserves multi-character Chinese IME commit");
            all_ok &= CheckHostedEdit(GetFocus() == edit,
                                      "SearchPopup keeps focus on EDIT after height expansion");

            if (HIMC imc = ImmGetContext(edit)) {
                COMPOSITIONFORM cf{};
                const BOOL got = ImmGetCompositionWindow(imc, &cf);
                ImmReleaseContext(edit, imc);
                all_ok &= CheckHostedEdit(got && cf.ptCurrentPos.x > 16,
                                          "SearchPopup IMM32 caret X advances past typed CJK text");
            }
        }
        ov.Dismiss();
    }

    // 4. InlineRename: stem selection, F2 selection cycle, Enter commit
    {
        HWND dummy_parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 100, 100, 800, 600,
                                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        std::wstring committed_name;
        const bool started = ov.BeginInlineRename(
            dummy_parent, model::RectI{120, 160, 110, 28}, L"季度汇报.pptx", false,
            [&](const std::wstring& txt) { committed_name = txt; }, 4);
        PumpHostedEditMessages();

        HWND edit = FindWindowExW(ov.OverlayHwnd(), nullptr, L"EDIT", nullptr);
        all_ok &= CheckHostedEdit(started && ov.ActiveKind() == overlays::OverlayKind::InlineRename &&
                                      edit != nullptr && GetFocus() == edit,
                                  "BeginInlineRename opens hosted InlineRename overlay with focused EDIT");

        if (edit) {
            DWORD s0 = 0, s1 = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&s0), reinterpret_cast<LPARAM>(&s1));
            all_ok &= CheckHostedEdit(s0 == 0 && s1 == 4, "InlineRename initially selects filename stem");

            SendMessageW(edit, WM_KEYDOWN, VK_F2, 0);
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&s0), reinterpret_cast<LPARAM>(&s1));
            all_ok &= CheckHostedEdit(s0 == 0 && s1 == 9, "InlineRename F2 cycles to full filename");

            SendMessageW(edit, WM_KEYDOWN, VK_F2, 0);
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&s0), reinterpret_cast<LPARAM>(&s1));
            all_ok &= CheckHostedEdit(s0 == 5 && s1 == 9, "InlineRename F2 cycles to extension");

            SetWindowTextW(edit, L"年度汇报.pptx");
            SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
            PumpHostedEditMessages();
            all_ok &= CheckHostedEdit(committed_name == L"年度汇报.pptx" && !ov.IsOpen(),
                                      "InlineRename Enter commits edited filename and closes overlay");
        }
        if (dummy_parent) DestroyWindow(dummy_parent);
    }

    ov.Shutdown();
    if (SUCCEEDED(co_hr)) CoUninitialize();
    return all_ok ? 0 : 1;
}

} // namespace pulse::desktop::tests
