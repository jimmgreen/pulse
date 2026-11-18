int RunSharedSearchHeaderTests(const std::filesystem::path& output) {
    using State = pulse::GlobalSearchWindowTestPeer::State;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    std::filesystem::create_directories(output);
    const auto profile = std::filesystem::absolute(output / L"profile");
    std::filesystem::create_directories(profile);
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; if (!ok) ++failures;
    };
    {
        State state; state.dark = false; state.animations = false;
        check(state.Create(), "shared search header creates only a hidden owner");
        if (!state.hwnd || !state.edit) return 2;
        state.BuildScopes(); state.ApplyAppearance();
        const auto edit_matches = [&] {
            RECT rect{}; GetWindowRect(state.edit, &rect);
            MapWindowPoints(HWND_DESKTOP, state.hwnd, reinterpret_cast<POINT*>(&rect), 2);
            const auto layout = state.SearchFieldLayout();
            return std::abs(rect.left - state.Px(layout.input.left)) <= 1 &&
                std::abs(rect.right - state.Px(layout.input.right)) <= 1 &&
                rect.left >= state.Px(layout.scope.right) && rect.right <= state.Px(layout.mode.left);
        };
        for (const float scale : {1.0f, 1.5f, 2.0f}) {
            for (const int width : {960, 640}) {
                state.scale = scale;
                SetWindowPos(state.hwnd, nullptr, -30000, -30000, state.Px(static_cast<float>(width)), state.Px(420), SWP_NOACTIVATE | SWP_NOZORDER);
                state.Layout();
                SetWindowTextW(state.edit, L"上周的图片");
                check(!state.parsed_query.tokens.empty() && edit_matches(), "shared field keeps native edit between chips at each DPI/width");
                state.SetMode(true); check(state.content_mode && edit_matches(), "mode toggle preserves native edit bounds");
                state.SetMode(false);
                if (state.scopes.size() > 1) { state.SetScope(1); check(edit_matches(), "scope change preserves native edit bounds"); state.SetScope(0); }
                state.Render();
                const auto close = state.SearchFieldLayout().close;
                state.Click((close.left + close.right) / 2, (close.top + close.bottom) / 2);
                check(GetWindowTextLengthW(state.edit) == 0 && state.raw_query.empty() && state.parsed_query.tokens.empty() && state.showing_recent,
                    "shared trailing clear refreshes query and parsed filters");
                SetWindowTextW(state.edit, L"Pulse 搜索文件");
                state.compositor.CaptureNextFrame(); state.Render();
                const auto file = output / (L"header-" + std::to_wstring(static_cast<int>(scale * 100)) + L"-" + std::to_wstring(width) + L".png");
                check(CapturePresentedFrame(state, file.wstring()), "capture hidden shared search header");
                state.compositor.ReleaseHeldFrame();
                check(!IsWindowVisible(state.hwnd), "header verification never shows the owner");
            }
        }
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
