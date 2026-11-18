#include "../ui/edit_host.h"
#include "../ui/ui_compositor.h"
#include "../ui/typography.h"
#include "../ui/lumatext_renderer.h"
#include <cstdio>
#include <string>
#include <commctrl.h>
#include <cstdint>
#include <utility>

namespace {
int failures = 0;
bool force_present_failure = false;
void Check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& compositor = *reinterpret_cast<pulse::ui::Compositor*>(data);
    auto* format = force_present_failure ? nullptr : compositor.TextFormat();
    const auto foreground = D2D1::ColorF(1, 1, 1);
    DWORD flags = 0;
    const bool redirected = GetLayeredWindowAttributes(hwnd, nullptr, nullptr, &flags) && (flags & LWA_ALPHA);
    const auto background = D2D1::ColorF(0, redirected ? 1.0f : 0.0f);
    LRESULT result = 0;
    if (pulse::ui::HandleChildEditMessage(compositor, format, foreground,
        background, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)), hwnd, msg, wp, lp, result)) return result;
    return pulse::ui::DefPresentedChildEditProc(compositor, format, foreground,
        background, hwnd, msg, wp, lp);
}
bool HasRenderedText(pulse::ui::Compositor& compositor, HWND edit) {
    RECT rect{};
    GetClientRect(edit, &rect);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return false;
    }
    const auto old = SelectObject(dc, bitmap);
    const bool painted = compositor.PaintLumaEdit(edit, dc, compositor.TextFormat(),
        D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0));
    GdiFlush();
    int ink = 0;
    const auto* data = static_cast<const std::uint32_t*>(pixels);
    if (painted) {
        for (int i = 0; i < rect.right * rect.bottom; ++i)
            if ((data[i] & 0xff) > 64) ++ink;
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    if (!painted || ink <= 20) {
        const auto* stats = compositor.GetLumaTextStats();
        std::printf("[DIAG] edit painted=%d ink=%d size=%ldx%ld format=%d builds=%llu hits=%llu\n",
            painted ? 1 : 0, ink, rect.right, rect.bottom, compositor.TextFormat() ? 1 : 0,
            static_cast<unsigned long long>(stats ? stats->edit_layout_builds : 0),
            static_cast<unsigned long long>(stats ? stats->edit_layout_cache_hits : 0));
    }
    return painted && ink > 20;
}

void CheckEditLayoutCache(HWND parent, pulse::ui::Compositor& compositor) {
    // Init creates graphics resources; production callers initialize formats
    // separately before presenting editors, as the other fixtures do below.
    compositor.RecreateTextFormats(1.0f);
    Check(compositor.TextFormat() != nullptr, "initialize cache fixture text format");
    HWND edit = pulse::ui::CreateChildEdit(parent, L"cache 中文");
    Check(edit != nullptr, "create isolated edit layout cache fixture");
    if (!edit) return;
    SetWindowPos(edit, nullptr, 20, 30, 320, 30, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    Check(HasRenderedText(compositor, edit), "warm actual DirectWrite editor layout");
    auto builds = compositor.GetLumaTextStats()->edit_layout_builds;
    const auto hits = compositor.GetLumaTextStats()->edit_layout_cache_hits;
    Check(HasRenderedText(compositor, edit) && HasRenderedText(compositor, edit) &&
        compositor.GetLumaTextStats()->edit_layout_builds == builds &&
        compositor.GetLumaTextStats()->edit_layout_cache_hits >= hits + 2,
        "unchanged editor repaints reuse layout without reshaping");
    SetWindowTextW(edit, L"changed 中文");
    Check(HasRenderedText(compositor, edit) && compositor.GetLumaTextStats()->edit_layout_builds > builds,
        "text change rebuilds editor layout");
    builds = compositor.GetLumaTextStats()->edit_layout_builds;
    SetWindowPos(edit, nullptr, 20, 30, 320, 45, SWP_NOZORDER | SWP_NOACTIVATE);
    Check(HasRenderedText(compositor, edit) && compositor.GetLumaTextStats()->edit_layout_builds > builds,
        "height change rebuilds editor layout");
    builds = compositor.GetLumaTextStats()->edit_layout_builds;
    pulse::ui::typography::InvalidateCaches();
    Check(HasRenderedText(compositor, edit) && compositor.GetLumaTextStats()->edit_layout_builds > builds,
        "font generation change rebuilds editor layout with the same format pointer");
    builds = compositor.GetLumaTextStats()->edit_layout_builds;
    compositor.RecreateTextFormats(1.5f);
    Check(HasRenderedText(compositor, edit) && compositor.GetLumaTextStats()->edit_layout_builds > builds,
        "new format and font size rebuild editor layout");
    builds = compositor.GetLumaTextStats()->edit_layout_builds;
    compositor.TextFormat()->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
    Check(HasRenderedText(compositor, edit) && compositor.GetLumaTextStats()->edit_layout_builds > builds,
        "in-place reading direction change rebuilds editor layout");
    builds = compositor.GetLumaTextStats()->edit_layout_builds;
    compositor.TextFormat()->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 28.0f, 22.0f);
    Check(HasRenderedText(compositor, edit) && compositor.GetLumaTextStats()->edit_layout_builds > builds,
        "in-place line spacing change rebuilds editor layout");
    compositor.RecreateTextFormats(1.0f);
    for (int i = 0; i < 5; ++i) {
        const std::wstring text = L"bounded layout " + std::to_wstring(i);
        SetWindowTextW(edit, text.c_str());
        Check(HasRenderedText(compositor, edit), "populate bounded editor layout slots");
    }
    builds = compositor.GetLumaTextStats()->edit_layout_builds;
    SetWindowTextW(edit, L"bounded layout 0");
    Check(HasRenderedText(compositor, edit) && compositor.GetLumaTextStats()->edit_layout_builds > builds,
        "fifth distinct layout evicts the oldest of four retained slots");
    const std::wstring long_text(4097, L'a');
    SetWindowTextW(edit, long_text.c_str());
    builds = compositor.GetLumaTextStats()->edit_layout_builds;
    Check(HasRenderedText(compositor, edit) && HasRenderedText(compositor, edit) &&
        compositor.GetLumaTextStats()->edit_layout_builds == builds + 2,
        "oversized editor text is rendered without cache retention");
    DestroyWindow(edit);
    ShowWindow(parent, SW_HIDE);
}
void CheckSelectionSync(pulse::ui::Compositor& compositor, HWND edit) {
    const std::wstring text = L"0123456789abcdef";
    auto set_text = [&](const std::wstring& value) {
        SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value.c_str()));
    };
    auto point = [&](int index) {
        float width = 0;
        compositor.MeasureLumaText(text.substr(0, static_cast<size_t>(index)), compositor.TextFormat(), width);
        return MAKELPARAM(static_cast<int>(width + 2.5f), 10);
    };
    auto click = [&](int index, bool shift = false) {
        SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON | (shift ? MK_SHIFT : 0), point(index));
        SendMessageW(edit, WM_LBUTTONUP, 0, point(index));
    };
    auto selection = [&] {
        DWORD first = 0, last = 0;
        SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
        return std::pair{first, last};
    };
    set_text(text); click(5);
    SendMessageW(edit, WM_KEYDOWN, VK_HOME, 0);
    click(10, true);
    auto range = selection();
    Check(range.first == 0 && range.second == 10, "mouse then native Home then Shift-click uses current caret anchor");
    SendMessageW(edit, WM_CHAR, L'X', 0);
    wchar_t content[128]{};
    GetWindowTextW(edit, content, ARRAYSIZE(content));
    Check(std::wstring(content) == L"Xabcdef", "typing replaces the visibly selected current range");
    // #116: an IME composition starts from a caret, never from a selection.
    set_text(L"a.txt");
    SendMessageW(edit, EM_SETSEL, 0, 1);
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    GetWindowTextW(edit, content, ARRAYSIZE(content));
    Check(std::wstring(content) == L".txt" && selection() == std::pair<DWORD, DWORD>{0, 0},
        "IME composition deletes the selected stem before it starts");
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    set_text(L"a.txt");
    SendMessageW(edit, EM_SETSEL, 1, 1);
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    GetWindowTextW(edit, content, ARRAYSIZE(content));
    Check(std::wstring(content) == L"a.txt" && selection() == std::pair<DWORD, DWORD>{1, 1},
        "IME composition at a caret leaves the text alone");
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    set_text(text);
    SendMessageW(edit, EM_SETSEL, 8, 3);
    click(5, true);
    Check(selection() == std::pair<DWORD, DWORD>{5, 8}, "programmatic reverse selection keeps its original active end and anchor");
    SendMessageW(edit, EM_SETSEL, 0, -1);
    click(5, true);
    Check(selection() == std::pair<DWORD, DWORD>{0, 5}, "select-all followed by Shift-click starts at the new native anchor");
    set_text(text); click(2);
    SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, point(2));
    SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, point(7));
    SendMessageW(edit, WM_LBUTTONUP, 0, point(7));
    Check(selection() == std::pair<DWORD, DWORD>{2, 7}, "internal mouse EM_SETSEL preserves the drag anchor");
    SendMessageW(edit, EM_SETSEL, 4, 4);
    SendMessageW(edit, WM_KEYDOWN, VK_RIGHT, 0);
    click(9, true);
    Check(selection() == std::pair<DWORD, DWORD>{5, 9}, "native arrow updates the subsequent mouse anchor");
    HWND other = pulse::ui::CreateChildEdit(GetParent(edit), L"other field");
    SetWindowSubclass(other, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
    SetWindowPos(other, nullptr, 20, 90, 180, 30, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(),
        D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), other);
    SendMessageW(edit, EM_SETSEL, 8, 3);
    SendMessageW(other, EM_SETSEL, 7, 7);
    DestroyWindow(other);
    click(5, true);
    Check(selection() == std::pair<DWORD, DWORD>{5, 8},
        "another editor and its destruction cannot leak a stale anchor to this field");
    RECT rect{}; GetClientRect(edit, &rect);
    SetWindowPos(edit, nullptr, 0, 0, 90, rect.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    set_text(L"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    click(2); SendMessageW(edit, WM_KEYDOWN, VK_END, 0);
    SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(45, 10));
    SendMessageW(edit, WM_LBUTTONUP, 0, MAKELPARAM(45, 10));
    range = selection();
    Check(range.first == range.second && range.first > 40,
        "click after native End uses the same scrolled text origin as painting");
    SetWindowPos(edit, nullptr, 0, 0, rect.right, rect.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    set_text(L"show 中文");
}
}
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc == 2 && std::wstring(argv[1]) == L"--first-frame-alpha") {
        HWND host = CreateWindowW(L"STATIC", L"", WS_POPUP, -30000, -30000,
            400, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        for (const bool init_failure : {false, true}) {
            SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", init_failure ? L"1" : nullptr);
            pulse::ui::Compositor compositor;
            Check(host && compositor.Init(host), "first-frame host initializes");
            Check(compositor.CustomEditEnabled() == !init_failure,
                "fault injection selects the intended editor backend");
            HWND edit = pulse::ui::CreateRedirectedChildEdit(host);
            Check(edit != nullptr, "first-frame editor initializes");
            SetWindowPos(edit, nullptr, 0, 0, 300, 30, SWP_NOZORDER | SWP_NOACTIVATE);
            SetLayeredWindowAttributes(edit, 0, 0, LWA_ALPHA);
            if (init_failure) pulse::ui::SynchronizeChildEditBackend(compositor, edit);
            else Check(!pulse::ui::PresentChildEdit(compositor, nullptr,
                D2D1::ColorF(0, 0, 0), D2D1::ColorF(1, 1, 1), edit), "null format forces presentation fallback");
            BYTE alpha = 255; DWORD flags = 0;
            Check(GetLayeredWindowAttributes(edit, nullptr, &alpha, &flags) && alpha == 0,
                init_failure ? "initialization fallback preserves hidden first frame" : "presentation fallback preserves hidden first frame");
            SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA);
            Check(GetLayeredWindowAttributes(edit, nullptr, &alpha, &flags) && alpha == 255,
                "host can reveal the prepared native editor");
            DestroyWindow(edit);
        }
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", nullptr);
        DestroyWindow(host); CoUninitialize(); return failures ? 1 : 0;
    }
    const HWND foreground = GetForegroundWindow();
    HWND parent = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, -30000, -30000, 700, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", L"1");
        pulse::ui::Compositor compositor;
        Check(parent && compositor.Init(parent), "composition survives a late text engine initialization failure");
        Check(!compositor.LumaTextEnabled(), "partially initialized text engine does not suppress native editing");
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", nullptr);
    }
    {
        pulse::ui::Compositor compositor;
        Check(parent && compositor.Init(parent), "create composition host for native child editors");
        Check(compositor.LumaTextEnabled(), "LumaText remains enabled");
        CheckEditLayoutCache(parent, compositor);
        for (const auto [redirected, scale] : {std::pair{false, 1.0f}, std::pair{false, 1.5f},
                std::pair{true, 1.0f}, std::pair{true, 1.5f}}) {
            compositor.RecreateTextFormats(scale);
            HWND edit = pulse::ui::CreateChildEdit(parent, L"show 中文");
            Check(edit && IsChild(parent, edit) && GetAncestor(edit, GA_ROOT) == parent &&
                !(GetWindowLongPtrW(edit, GWL_STYLE) & WS_POPUP), "editor is a real child, not an owned top-level popup");
            if (!edit) continue;
            DWORD initial_flags = 0;
            BYTE initial_alpha = 0;
            Check(GetLayeredWindowAttributes(edit, nullptr, &initial_alpha, &initial_flags) &&
                (initial_flags & LWA_ALPHA) && initial_alpha == 255,
                "shared factory initializes an opaque redirected surface without host setup");
            if (!redirected) {
                // Retain low-level uploaded-surface coverage explicitly; production
                // hosts must inherit the factory's redirected default.
                const LONG_PTR style = GetWindowLongPtrW(edit, GWL_EXSTYLE);
                SetWindowLongPtrW(edit, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
                SetWindowLongPtrW(edit, GWL_EXSTYLE, style | WS_EX_LAYERED);
            }
            SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
            SetWindowPos(edit, nullptr, 20, 30, static_cast<int>(320 * scale), static_cast<int>(30 * scale),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(parent, SW_SHOWNOACTIVATE);
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"show 中文"));
            Check(compositor.PresentLumaEdit(edit, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                D2D1::ColorF(0, redirected ? 1.0f : 0.0f)), "editor presents after native redraw suppression");
            Check(HasRenderedText(compositor, edit), "rendered editor contains visible Unicode glyph pixels");
            RECT before{}, after{};
            GetWindowRect(edit, &before);
            Check(compositor.PresentLumaEdit(edit, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                D2D1::ColorF(0.1f, 0.1f, 0.1f)), "LumaText presents a child bitmap at 100 and 150 percent scale");
            GetWindowRect(edit, &after);
            Check(EqualRect(&before, &after), "text repaint does not move the child into screen coordinates");
            SendMessageW(edit, EM_SETSEL, 0, 4);
            SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"debug"));
            wchar_t text[64]{};
            GetWindowTextW(edit, text, ARRAYSIZE(text));
            Check(std::wstring(text) == L"debug 中文", "native Unicode editing and selection remain functional");
            SendMessageW(edit, WM_UNDO, 0, 0);
            GetWindowTextW(edit, text, ARRAYSIZE(text));
            Check(std::wstring(text) == L"show 中文", "native undo remains functional");
            for (const auto mode : {pulse::ui::typography::TextRenderMode::Auto,
                    pulse::ui::typography::TextRenderMode::Sharp,
                    pulse::ui::typography::TextRenderMode::Smooth,
                    pulse::ui::typography::TextRenderMode::Auto}) {
                pulse::ui::typography::SetTextRenderMode(mode);
                const bool luma_ui = mode == pulse::ui::typography::TextRenderMode::Auto;
                Check(compositor.LumaTextAvailable() && compositor.LumaTextEnabled() == luma_ui &&
                    compositor.CustomEditEnabled(), "inputs retain DirectWrite presentation across all UI modes");
                DWORD flags = 0;
                const bool opaque = GetLayeredWindowAttributes(edit, nullptr, nullptr, &flags) && (flags & LWA_ALPHA);
                Check(opaque == redirected, "existing editor preserves its original presentation surface");
                const auto before_draws = compositor.GetLumaTextStats()->edit_directwrite_draws;
                Check(HasRenderedText(compositor, edit) &&
                    compositor.GetLumaTextStats()->edit_directwrite_draws > before_draws,
                    "every text mode draws actual DirectWrite input glyph pixels");
                Check(compositor.GetLumaTextStats()->edit_rendering_mode ==
                    (mode == pulse::ui::typography::TextRenderMode::Sharp ? DWRITE_RENDERING_MODE_GDI_CLASSIC
                                                                       : DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC),
                    "input uses the list's rendering parameters for the selected mode");
                IDWriteTextLayout* layout = nullptr;
                compositor.DwriteFactory()->CreateTextLayout(L"show 中文", 7, compositor.TextFormat(),
                    1.0e6f, 30 * scale, &layout);
                Check(layout != nullptr, "create independent DirectWrite selection reference");
                if (layout) {
                    float x = 0, y = 0;
                    DWRITE_HIT_TEST_METRICS hit{};
                    layout->HitTestTextPosition(3, FALSE, &x, &y, &hit);
                    const int hit_x = static_cast<int>(x + hit.width * 0.25f + 2.0f);
                    SendMessageW(edit, EM_SETSEL, 0, 0);
                    const auto before_hits = compositor.GetLumaTextStats()->edit_directwrite_hits;
                    SetCapture(edit);
                    SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(hit_x, 4));
                    SendMessageW(edit, WM_LBUTTONUP, 0, MAKELPARAM(hit_x, 4));
                    DWORD lo = 0, hi = 0;
                    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&lo), reinterpret_cast<LPARAM>(&hi));
                    Check(lo == 0 && hi == 3 && compositor.GetLumaTextStats()->edit_directwrite_hits > before_hits,
                        "real editor drag selection matches DirectWrite cluster hit positions");
                    layout->Release();
                }
                SendMessageW(edit, EM_SETSEL, 0, 4);
                SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"mode"));
                SendMessageW(edit, WM_UNDO, 0, 0);
                GetWindowTextW(edit, text, ARRAYSIZE(text));
                Check(std::wstring(text) == L"show 中文", "selection and undo survive backend migration");
                HWND fresh = pulse::ui::CreateChildEdit(parent, L"new 中文");
                SetWindowSubclass(fresh, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
                SetWindowPos(fresh, nullptr, 20, 80, 320, 30, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                    D2D1::ColorF(0, 0, 0), fresh);
                flags = 0;
                const bool fresh_native = GetLayeredWindowAttributes(fresh, nullptr, nullptr, &flags) && (flags & LWA_ALPHA);
                Check(fresh_native && !GetPropW(fresh, L"Pulse.NativeEditFallback"),
                    "new editor keeps redirected DirectWrite presentation in every mode");
                force_present_failure = true;
                SendMessageW(fresh, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"fail 中文"));
                force_present_failure = false;
                Check(GetPropW(fresh, L"Pulse.NativeEditFallback") &&
                    !pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                        D2D1::ColorF(0, 0, 0), fresh), "each text mode retains sticky native fallback after presentation failure");
                SendMessageW(fresh, EM_SETSEL, 0, 4);
                SendMessageW(fresh, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"safe"));
                SendMessageW(fresh, WM_UNDO, 0, 0);
                GetWindowTextW(fresh, text, ARRAYSIZE(text));
                Check(std::wstring(text) == L"fail 中文", "each mode preserves native input and undo after presentation failure");
                DestroyWindow(fresh);
            }
            CheckSelectionSync(compositor, edit);
            RECT owner{};
            GetWindowRect(parent, &owner);
            SetWindowPos(parent, nullptr, owner.left + 70, owner.top + 40, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetWindowRect(edit, &after);
            Check(after.left == before.left + 70 && after.top == before.top + 40,
                "moving parent moves child without manual repositioning");
            force_present_failure = true;
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"fallback"));
            force_present_failure = false;
            LRESULT handled = 0;
            Check(!pulse::ui::HandleChildEditMessage(compositor, compositor.TextFormat(),
                D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), nullptr,
                edit, WM_PAINT, 0, 0, handled), "presentation failure restores native paint handling");
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"native 中文"));
            GetWindowTextW(edit, text, ARRAYSIZE(text));
            Check(std::wstring(text) == L"native 中文", "editing remains functional after presentation failure");
            pulse::ui::typography::SetTextRenderMode(pulse::ui::typography::TextRenderMode::Sharp);
            pulse::ui::typography::SetTextRenderMode(pulse::ui::typography::TextRenderMode::Auto);
            Check(!pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
                D2D1::ColorF(0, 0, 0), edit), "backend changes preserve a presentation-failure fallback");
            ShowWindow(parent, SW_HIDE);
            Check(!IsWindowVisible(edit), "parent hide automatically hides the editor");
            DestroyWindow(edit);
        }
        {
            HWND plain = pulse::ui::CreatePulsePaintedChildEdit(parent, L"filter 中文", 0, true);
            Check(plain && !(GetWindowLongPtrW(plain, GWL_EXSTYLE) & WS_EX_LAYERED),
                "filter input has no independent layered surface");
            if (plain) {
                SetWindowSubclass(plain, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
                SetWindowPos(plain, nullptr, 20, 30, 320, 30, SWP_NOZORDER | SWP_NOACTIVATE);
                SendMessageW(plain, EM_SETSEL, 2, 5);
                compositor.Dc()->BeginDraw();
                const bool drawn = pulse::ui::DrawPulsePaintedChildEdit(compositor, nullptr,
                    D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), plain, D2D1::RectF(20, 30, 340, 60));
                const HRESULT hr = compositor.Dc()->EndDraw();
                Check(drawn && SUCCEEDED(hr), "filter raster fallback draws into parent frame");
                DWORD from = 0, to = 0;
                SendMessageW(plain, EM_GETSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
                Check(from == 2 && to == 5 && GetWindowTextLengthW(plain) == 9,
                    "parent raster fallback preserves native text and selection");
                DestroyWindow(plain);
            }
        }
        for (const bool fail : {false, true}) {
            // A dialog field's first bitmap right after it is shown (#41): a
            // failed present must leave a visible, clickable native editor.
            HWND fresh = pulse::ui::CreateChildEdit(parent, L"typed 中文");
            if (!fresh) continue;
            SetWindowSubclass(fresh, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
            SetWindowPos(fresh, nullptr, 20, 30, 320, 30, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(parent, SW_SHOWNOACTIVATE);
            const bool shown = pulse::ui::PresentChildEdit(compositor, fail ? nullptr : compositor.TextFormat(),
                D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), fresh);
            BYTE alpha = 0;
            DWORD flags = 0;
            const bool opaque_native = GetLayeredWindowAttributes(fresh, nullptr, &alpha, &flags) &&
                (flags & LWA_ALPHA) && alpha == 255;
            LRESULT handled = 0;
            const bool native_paint = !pulse::ui::HandleChildEditMessage(compositor, compositor.TextFormat(),
                D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), nullptr, fresh, WM_PAINT, 0, 0, handled);
            if (fail) {
                Check(!shown && opaque_native, "failed initial present leaves an opaque native editor");
                Check(native_paint, "failed initial present hands painting to the native editor");
                Check(!pulse::ui::PresentChildEdit(compositor, compositor.TextFormat(),
                    D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), fresh),
                    "later presents keep the native fallback instead of painting over it");
            } else {
                Check(shown && opaque_native && !native_paint, "initial present draws custom text on the redirected surface");
            }
            ShowWindow(parent, SW_HIDE);
            DestroyWindow(fresh);
        }
        HWND edit = pulse::ui::CreateChildEdit(parent);
        DestroyWindow(parent);
        Check(!IsWindow(edit), "destroying parent automatically destroys its editor");
    }
    Check(GetForegroundWindow() == foreground, "tests preserve the user's foreground window");
    CoUninitialize();
    return failures ? 1 : 0;
}
