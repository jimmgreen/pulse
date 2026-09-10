#include "../ui/edit_host.h"
#include "../ui/ui_compositor.h"
#include <cstdio>
#include <string>

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
}
int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HWND foreground = GetForegroundWindow();
    HWND parent = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, -30000, -30000, 700, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        pulse::ui::Compositor compositor;
        Check(parent && compositor.Init(parent), "create composition host for native child editors");
        Check(compositor.LumaTextEnabled(), "LumaText remains enabled");
        for (float scale : {1.0f, 1.5f}) {
            compositor.RecreateTextFormats(scale);
            HWND edit = pulse::ui::CreateChildEdit(parent, L"show 中文");
            Check(edit && IsChild(parent, edit) && GetAncestor(edit, GA_ROOT) == parent &&
                !(GetWindowLongPtrW(edit, GWL_STYLE) & WS_POPUP), "editor is a real child, not an owned top-level popup");
            if (!edit) continue;
            SetWindowPos(edit, nullptr, 20, 30, static_cast<int>(320 * scale), static_cast<int>(30 * scale),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(parent, SW_SHOWNOACTIVATE);
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
            RECT owner{};
            GetWindowRect(parent, &owner);
            SetWindowPos(parent, nullptr, owner.left + 70, owner.top + 40, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetWindowRect(edit, &after);
            Check(after.left == before.left + 70 && after.top == before.top + 40,
                "moving parent moves child without manual repositioning");
            ShowWindow(parent, SW_HIDE);
            Check(!IsWindowVisible(edit), "parent hide automatically hides the editor");
            DestroyWindow(edit);
        }
        HWND edit = pulse::ui::CreateChildEdit(parent);
        DestroyWindow(parent);
        Check(!IsWindow(edit), "destroying parent automatically destroys its editor");
    }
    Check(GetForegroundWindow() == foreground, "tests preserve the user's foreground window");
    CoUninitialize();
    return failures ? 1 : 0;
}
