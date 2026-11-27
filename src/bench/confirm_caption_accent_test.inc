void CaptionChecks(const std::filesystem::path& out) {
    const COLORREF colors[] = {RGB(78, 91, 96), RGB(200, 30, 40), RGB(20, 130, 210)};
    for (const bool dark : {false, true}) {
        test::ClearCaptionWrites();
        auto spec = Spec();
        spec.title = L"清空回收站";
        spec.message = L"清空回收站后无法还原这些项目。确定继续？";
        spec.confirm_text = L"清空回收站";
        spec.danger = true;
        spec.on_open = [&](HWND hwnd) {
            if (!hwnd) return;
            const auto initial = test::LastCaptionWrite(hwnd);
            std::printf("[DWM] dark=%d initial_caption_write=0x%08lx hr=0x%08lx calls=%u\n", dark, initial.value, initial.result, initial.calls);
            Check(initial.calls > 0 && SUCCEEDED(initial.result) && initial.value == DWMWA_COLOR_NONE,
                  "actual confirmation HWND suppresses native caption tint at first show");
            RECT client{}, window{};
            GetClientRect(hwnd, &client); GetWindowRect(hwnd, &window);
            Check(client.right == window.right - window.left && client.bottom == window.bottom - window.top,
                  "custom title retains the complete client area without a native title strip");
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            for (const auto effect : {WindowEffect::None, WindowEffect::Mica, WindowEffect::MicaAlt, WindowEffect::Acrylic}) {
                for (const auto color : colors) {
                    // Force the offending native caption layer on this HWND only.
                    // Do not change the user's global ColorPrevalence setting.
                    Check(SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &color, sizeof(color))),
                          "inject accent-colored native caption on the isolated HWND");
                    test::ClearCaptionWrites();
                    ApplyBackdrop(hwnd, dark, effect);
                    SendMessageW(hwnd, WM_DWMCOLORIZATIONCOLORCHANGED, color, FALSE);
                    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
                    SendMessageW(hwnd, WM_NCACTIVATE, FALSE, 0);
                    SendMessageW(hwnd, WM_NCACTIVATE, TRUE, 0);
                    const auto applied = test::LastCaptionWrite(hwnd);
                    Check(applied.calls > 0 && SUCCEEDED(applied.result) && applied.value == DWMWA_COLOR_NONE,
                          "production backdrop policy removes caption tint across material/theme/activation changes");
                    DWORD corner = 0;
                    const bool round = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner))) && corner == DWMWCP_ROUND;
                    Check(round && GetWindowLongPtrW(hwnd, GWL_STYLE) == style,
                          "rounded DWM frame and window style remain intact");
                }
            }
            // App-surface capture validates readable controls, not the DWM layer.
            Check(SaveConfirmDialogFrame(hwnd, (out / (dark ? L"dark-content.png" : L"light-content.png")).wstring()),
                  "capture live confirmation controls without claiming DWM-composited screenshot coverage");
            PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        };
        Check(ShowConfirmDialogEx(nullptr, spec, dark, D2D1::ColorF(0x0078D4)) == ConfirmChoice::Cancel,
              "real confirmation closes through Escape without touching recycle-bin contents");
    }
}
