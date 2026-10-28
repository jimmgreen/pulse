#include "../ui/typography.h"
#include "../ui/fluent_components.h"
#include "../ui/fluent_menu.h"
#include "../ui/edit_host.h"
#include "../common/localization.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace pulse::ui::fluent {
struct PainterTestPeer {
    static IDWriteTextFormat* Body(const Painter& painter) { return painter.BodyFormat(); }
    static IDWriteTextFormat* Nav(const Painter& painter) { return painter.NavFormat(); }
    static IDWriteTextFormat* MenuChevron(const Painter& painter) { return painter.MenuChevronFormat(); }
};
}

namespace pulse::ui {
struct FluentMenuTestPeer {
    static bool CheckSnapshotReplacement() {
        Compositor compositor;
        FluentMenu menu;
        menu.compositor_ = &compositor;
        menu.open_ = true;
        FluentMenuItem parent{1, L"Parent"};
        parent.children = {{10, L"A"}, {11, L"B"}};
        menu.model_.SetItems({parent});
        menu.sub_model_.SetItems(parent.children);
        menu.sub_parent_row_ = 0;
        parent.children[0].command = 11;
        parent.children[1].command = 10;
        bool ok = !menu.ReplaceItems({parent});
        ok &= menu.model_.At(0)->children[0].command == 10;
        ok &= menu.sub_model_.At(0)->command == 10;
        parent.children[0].command = 20;
        parent.children[1].command = 21;
        ok &= !menu.ReplaceItems({parent});
        std::swap(parent.children[0], parent.children[1]);
        ok &= !menu.ReplaceItems({parent});
        std::swap(parent.children[0], parent.children[1]);
        menu.sub_parent_row_ = -1;
        ok &= menu.ReplaceItems({parent});
        menu.sub_model_.SetItems(menu.model_.At(0)->children);
        ok &= menu.sub_model_.At(0)->command == 20;
        menu.open_ = false;
        return ok;
    }
    static HWND Filter(FluentMenu& menu) {
        menu.filter_fn_ = [](const std::wstring&) {
            return std::vector<FluentMenuItem>{{1, L"A long reusable menu command label"}};
        };
        menu.model_.SetItems(menu.filter_fn_(L""));
        menu.model_.Layout(menu.compositor_->DwriteFactory(), menu.scale_);
        return menu.EnsureFilterEdit() ? menu.edit_ : nullptr;
    }
    static void Show(FluentMenu& menu) {
        SetWindowPos(menu.hwnd_, nullptr, -30000, -30000, 500, 160,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(menu.edit_, nullptr, 10, 10, 400, 32,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    static void Reopen(FluentMenu& menu) {
        ShowWindow(menu.hwnd_, SW_HIDE);
        menu.EnsureFilterEdit();
        Show(menu);
    }
    // A pixel of the card's top padding (no row content) as {R, G, B}; the DIB
    // is premultiplied BGRA and the card is opaque.
    static void CardCenter(const FluentMenu& menu, int rgb[3]) {
        CardPixel(menu, menu.surf_.w / 2, FluentMenu::kShadowMargin + 3, rgb);
    }
    static void CardPixel(const FluentMenu& menu, int x, int y, int rgb[3]) {
        const auto* px = static_cast<const unsigned char*>(menu.surf_.bits) +
                         (static_cast<size_t>(y) * menu.surf_.w + x) * 4;
        rgb[0] = px[2]; rgb[1] = px[1]; rgb[2] = px[0];
    }
    // Acrylic menus blur the owner's last presented frame under the card and
    // fall back to the solid plate where there is nothing to sample.
    static int CheckAcrylicPlate(int (*check)(bool, const char*)) {
        int fails = 0;
        HWND host = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"",
            WS_POPUP | WS_VISIBLE, -30000, -30000, 600, 400, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        {
            FluentMenu::SetAcrylicEnabled(true);
            Compositor compositor;
            if (!host || !compositor.Init(host)) {
                fails += check(false, "create acrylic test window");
                FluentMenu::SetAcrylicEnabled(false);
                if (host) DestroyWindow(host);
                return fails;
            }
            ID2D1DeviceContext* dc = compositor.Dc();
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0.85f, 0.10f, 0.10f, 1.0f));
            dc->EndDraw();
            compositor.Present();
            fails += check(compositor.FrameCopy() != nullptr, "present keeps a frame copy while menu acrylic is on");

            FluentMenu menu;
            fails += check(menu.Create(host, &compositor, 1.0f), "create acrylic test menu");
            menu.SetTheme(false, D2D1::ColorF(0.2f, 0.4f, 0.8f));
            menu.model_.SetItems({{1, L"Open"}, {2, L"Copy path"}, {3, L"Properties"}});
            menu.model_.Layout(compositor.DwriteFactory(), 1.0f);
            POINT client{0, 0};
            ClientToScreen(host, &client);
            menu.base_x_ = client.x + 40;
            menu.base_y_ = client.y + 30;

            menu.acrylic_active_ = false;
            int solid[3]{};
            const bool solid_ok = menu.Render();
            if (solid_ok) CardCenter(menu, solid);
            menu.acrylic_active_ = true;
            int glass[3]{};
            const bool glass_ok = menu.Render();
            if (glass_ok) CardCenter(menu, glass);
            std::printf("acrylic values: solid %d,%d,%d glass %d,%d,%d\n",
                solid[0], solid[1], solid[2], glass[0], glass[1], glass[2]);
            fails += check(solid_ok && std::abs(solid[0] - solid[2]) <= 2,
                "solid menu plate stays neutral");
            fails += check(glass_ok && menu.surf_.plate.get() != nullptr && glass[0] - glass[2] >= 12,
                "acrylic menu plate carries the colour of the window under it");
            ID2D1Bitmap1* baked = menu.surf_.plate.get();
            menu.hover_row_ = 1;
            fails += check(menu.Render() && menu.surf_.plate.get() == baked,
                "hover redraws reuse the baked acrylic plate");

            // Nowhere near the window: nothing to sample, so the solid plate.
            menu.base_x_ = client.x + 5000;
            menu.base_y_ = client.y + 5000;
            int away[3]{};
            const bool away_ok = menu.Render();
            if (away_ok) CardCenter(menu, away);
            fails += check(away_ok && menu.surf_.plate.get() == nullptr &&
                std::abs(away[0] - solid[0]) <= 2 && std::abs(away[2] - solid[2]) <= 2,
                "a menu outside the window keeps the solid plate");

            // Hanging past the bottom-right corner: the part outside the
            // window continues the same material instead of a flat plate.
            menu.base_x_ = client.x + 560 - FluentMenu::kShadowMargin;
            menu.base_y_ = client.y + 370 - FluentMenu::kShadowMargin;
            int inside[3]{}, outside[3]{};
            const bool edge_ok = menu.Render() && menu.surf_.plate.get() != nullptr;
            if (edge_ok) {
                const int m = FluentMenu::kShadowMargin;
                CardPixel(menu, m + 10, m + 3, inside);
                CardPixel(menu, menu.surf_.w - m - 12, menu.surf_.h - m - 4, outside);
            }
            std::printf("acrylic edge: inside %d,%d,%d outside %d,%d,%d\n", inside[0], inside[1],
                inside[2], outside[0], outside[1], outside[2]);
            fails += check(edge_ok && outside[0] - outside[2] >= 12 &&
                std::abs(outside[0] - inside[0]) <= 4 && std::abs(outside[1] - inside[1]) <= 4 &&
                std::abs(outside[2] - inside[2]) <= 4,
                "a menu hanging past the window edge stays one acrylic material");

            // Worst cases for body text: a dark menu over white, a light menu
            // over black. Text is #FFFFFF (dark) and #303B50 (light).
            const auto luminance = [](const int rgb[3]) {
                double l[3]{};
                for (int i = 0; i < 3; ++i) {
                    const double c = rgb[i] / 255.0;
                    l[i] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
                }
                return 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2];
            };
            const auto contrast = [&](const int a[3], const int b[3]) {
                const double la = luminance(a), lb = luminance(b);
                return ((std::max)(la, lb) + 0.05) / ((std::min)(la, lb) + 0.05);
            };
            const struct { bool dark; float back; int text[3]; const char* label; } worst[] = {
                {true, 1.0f, {255, 255, 255}, "dark acrylic menu over white keeps body text at 4.5:1"},
                {false, 0.0f, {0x30, 0x3B, 0x50}, "light acrylic menu over black keeps body text at 4.5:1"},
            };
            for (const auto& w : worst) {
                FluentMenu::SetAcrylicEnabled(true);
                dc->BeginDraw();
                dc->Clear(D2D1::ColorF(w.back, w.back, w.back, 1.0f));
                dc->EndDraw();
                compositor.Present();
                menu.SetTheme(w.dark, D2D1::ColorF(0.2f, 0.4f, 0.8f));
                menu.base_x_ = client.x + 40;
                menu.base_y_ = client.y + 30;
                menu.surf_.plate.reset();
                menu.acrylic_active_ = true;
                int plate_rgb[3]{};
                const bool ok = menu.Render() && menu.surf_.plate.get() != nullptr;
                if (ok) CardCenter(menu, plate_rgb);
                const double ratio = ok ? contrast(plate_rgb, w.text) : 0.0;
                std::printf("acrylic contrast: plate %d,%d,%d ratio %.2f\n",
                    plate_rgb[0], plate_rgb[1], plate_rgb[2], ratio);
                fails += check(ok && ratio >= 4.5, w.label);
            }

            FluentMenu::SetAcrylicEnabled(false);
            compositor.Present();
            fails += check(compositor.FrameCopy() == nullptr, "turning menu acrylic off drops the frame copy");
        }
        DestroyWindow(host);
        return fails;
    }
};
}

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ui;
    if (argc == 2 && std::wstring(argv[1]) == L"--menu-identity") {
        Check(FluentMenuTestPeer::CheckSnapshotReplacement(),
            "open flyout rejects swapped/shifted/reordered snapshots; reopen uses accepted IDs");
        return failures ? 1 : 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HWND foreground = GetForegroundWindow();
    HWND owner = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, -30000, -30000, 700, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        Check(owner && compositor.Init(owner), "create typography test compositor");
        if (!compositor.DwriteFactory()) return 1;
        fluent::Painter painter(&compositor);
        const auto theme = MakeTheme(true, D2D1::ColorF(0.2f, 0.4f, 0.8f));
        FluentMenuModel model;
        model.SetItems({{1, L"Reusable menu command"}});
        typography::SetUiFontScale(100);
        pulse::l10n::SetLanguage(L"zh-CN");
        typography::InvalidateCaches();
        Check(painter.BeginFrame(theme), "warm painter frame initializes its own font cache");
        compositor.RecreateTextFormats(1.0f);
        Check(compositor.FileNameFormat() && fluent::PainterTestPeer::Nav(painter) &&
            std::abs(fluent::PainterTestPeer::Nav(painter)->GetFontSize() -
                     compositor.FileNameFormat()->GetFontSize()) < 0.01f &&
            std::abs(compositor.FileNameFormat()->GetFontSize() - FluentMenuModel::kLabelDip) < 0.01f,
            "menu labels use the file list name size");
        IDWriteTextFormat* chevron = fluent::PainterTestPeer::MenuChevron(painter);
        Check(chevron && compositor.IconFormat() &&
            chevron->GetFontSize() <= FluentMenuModel::kLabelDip &&
            chevron->GetFontSize() < compositor.IconFormat()->GetFontSize(),
            "submenu chevron is no larger than the menu labels");
        {
            // An open context menu patches in place or grows below; no shown
            // row moves (右键菜单优化方案 §4.1).
            FluentMenuModel menu;
            std::vector<FluentMenuItem> shown = {{1, L"打开"}, {2, L"复制"},
                {3, L"Scan with Defender"}, {4, L"管理右键项…"}};
            shown[1].separator_after = true;
            shown[2].separator_after = true;
            menu.SetItems(shown);
            menu.Layout(compositor.DwriteFactory(), 1.0f);
            const int height = menu.HeightPx();
            const int width = menu.WidthPx();
            auto late = shown;
            late[2].enabled = false;
            late[2].text = L"Scan with Defender, with a label much longer than the menu is wide";
            Check(menu.PatchInPlace(late, compositor.DwriteFactory()) && menu.HeightPx() == height &&
                menu.WidthPx() == width && menu.Truncated(2),
                "a patched open menu keeps its height and width; a longer label ellipsizes");
            Check(menu.HitTestRow(menu.RowTopPx(2) + menu.RowHeightPx() / 2) == 2 &&
                !menu.At(2)->enabled, "a greyed row stays in its place and is still hit");
            auto moved = late;
            moved[0].separator_after = true;
            Check(!menu.PatchInPlace(moved, compositor.DwriteFactory()) && menu.HeightPx() == height &&
                menu.Count() == 4, "another shape is refused and changes nothing");

            std::vector<float> tops;
            for (int i = 0; i < menu.Count(); ++i) tops.push_back(menu.RowTopPx(i));
            auto grown = late;
            grown[3].separator_after = true;
            grown.push_back({9, L"Git GUI"});
            FluentMenuItem flyout{0, L"Git"};
            flyout.children = {{8100, L"Git Bash"}, {8101, L"Open with Code"}};
            grown.push_back(flyout);
            bool still = true;
            Check(menu.PatchGrow(grown, compositor.DwriteFactory()) && menu.Count() == 6 &&
                menu.HeightPx() > height && menu.WidthPx() >= width && menu.At(5)->children.size() == 2,
                "rows appended below grow the open menu downwards");
            for (int i = 0; i < 4; ++i) still &= std::abs(menu.RowTopPx(i) - tops[static_cast<size_t>(i)]) < 0.01f;
            Check(still, "growing moves no shown row");
            auto reordered = grown;
            reordered.push_back({10, L"More"});
            reordered[1].separator_after = false;
            auto shorter = grown;
            shorter.pop_back();
            shorter.pop_back();
            shorter.pop_back();
            Check(!menu.PatchGrow(reordered, compositor.DwriteFactory()) &&
                !menu.PatchGrow(shorter, compositor.DwriteFactory()) && menu.Count() == 6,
                "growing refuses moved separators and never shrinks the menu");
        }
        const float original_size = fluent::PainterTestPeer::Body(painter)->GetFontSize();
        model.Layout(compositor.DwriteFactory(), 1.0f);
        const int original_width = model.WidthPx();
        typography::SetUiFontScale(125);
        Check(painter.BeginFrame(theme), "scaled painter frame refreshes its own font cache");
        model.Layout(compositor.DwriteFactory(), 1.0f);
        std::printf("scale values: body %.2f -> %.2f, menu %d -> %d, truncated=%d\n", original_size,
            fluent::PainterTestPeer::Body(painter)->GetFontSize(), original_width, model.WidthPx(), model.Truncated(0));
        Check(std::abs(fluent::PainterTestPeer::Body(painter)->GetFontSize() - original_size * 1.25f) < 0.01f &&
            model.WidthPx() > original_width, "warm painter and menu layout follow font scale without manual painter invalidation");
        pulse::l10n::SetLanguage(L"zh-TW");
        typography::InvalidateCaches(); // same language-change entry point as the application.
        Check(painter.BeginFrame(theme), "language painter frame refreshes its own font cache");
        wchar_t locale[32]{};
        fluent::PainterTestPeer::Body(painter)->GetLocaleName(locale, 32);
        wchar_t family[128]{};
        fluent::PainterTestPeer::Body(painter)->GetFontFamilyName(family, 128);
        IDWriteTextFormat* expected = nullptr;
        typography::CreateTextFormat(compositor.DwriteFactory(), {}, &expected);
        wchar_t expected_family[128]{};
        if (expected) expected->GetFontFamilyName(expected_family, 128);
        std::wprintf(L"language values: locale=%s family=%s expected=%s generation=%llu\n", locale, family,
            expected_family, static_cast<unsigned long long>(typography::Generation()));
        Check(CompareStringOrdinal(locale, -1, L"zh-TW", -1, TRUE) == CSTR_EQUAL &&
            std::wstring(family) == expected_family,
            "warm painter follows traditional Chinese locale and family");
        if (expected) expected->Release();
        {
            // Settings > General > Interface font: a chosen family replaces the
            // language cascade for text and display only, a name that is not
            // installed keeps the cascade, and the list offers real families.
            const std::wstring original_family = typography::UiFontFamily();
            Check(original_family.empty(), "no interface font family is chosen by default");
            const auto families = typography::UiFontFamilies(compositor.DwriteFactory());
            bool listed = families.size() > 1 && families[0].value.empty();
            for (const auto& choice : families)
                listed &= choice.value.empty() ? choice.label.empty() : !choice.label.empty();
            Check(listed, "the font list leads with the built-in cascade and names the families it offers");
            const auto family_of = [&](typography::FontRole role) {
                IDWriteTextFormat* format = nullptr;
                typography::CreateTextFormat(compositor.DwriteFactory(), {role, 14.0f}, &format);
                std::wstring name;
                if (format) {
                    wchar_t buffer[256]{};
                    format->GetFontFamilyName(buffer, ARRAYSIZE(buffer));
                    name = buffer;
                    format->Release();
                }
                return name;
            };
            const std::wstring builtin_text = family_of(typography::FontRole::Text);
            const std::wstring builtin_mono = family_of(typography::FontRole::Monospace);
            const std::wstring builtin_icon = family_of(typography::FontRole::Icon);
            std::wstring wanted;
            for (size_t i = 1; i < families.size(); ++i) {
                if (families[i].value != builtin_text) {
                    wanted = families[i].value;
                    break;
                }
            }
            Check(!wanted.empty(), "the font list offers a family other than the built-in one");
            if (!wanted.empty()) {
                const unsigned long long generation = typography::Generation();
                typography::SetUiFontFamily(wanted);
                Check(typography::UiFontFamily() == wanted && typography::Generation() > generation,
                    "choosing an interface font bumps the typography generation");
                Check(family_of(typography::FontRole::Text) == wanted,
                    "interface text uses the chosen family");
                Check(family_of(typography::FontRole::Monospace) == builtin_mono &&
                    family_of(typography::FontRole::Icon) == builtin_icon,
                    "monospace and icon faces keep their own families");
                Check(painter.BeginFrame(theme),
                    "font family painter frame refreshes its own font cache");
                wchar_t body_family[256]{};
                fluent::PainterTestPeer::Body(painter)->GetFontFamilyName(body_family,
                    ARRAYSIZE(body_family));
                Check(std::wstring(body_family) == wanted,
                    "the warm painter draws interface text in the chosen family");
                Check(std::wstring(typography::PreferredTextFamily()) == wanted,
                    "native edit controls follow the chosen interface family");
                Check(!typography::UiFontFamilyLabel(compositor.DwriteFactory(), wanted).empty(),
                    "the chosen family keeps a name for the settings row");
                typography::SetUiFontFamily(L"Pulse Missing Interface Font");
                Check(family_of(typography::FontRole::Text) == builtin_text,
                    "an uninstalled interface family falls back to the language cascade");
                Check(typography::UiFontFamilyLabel(compositor.DwriteFactory(),
                        L"Pulse Missing Interface Font").empty(),
                    "a family that is not installed has no name to show");
                typography::SetUiFontFamily(original_family);
                typography::InvalidateCaches();
                Check(painter.BeginFrame(theme),
                    "painter frame refreshes after restoring the built-in family");
                fluent::PainterTestPeer::Body(painter)->GetFontFamilyName(body_family,
                    ARRAYSIZE(body_family));
                Check(std::wstring(body_family) == builtin_text,
                    "restoring the built-in family returns the language family");
            }
        }
        for (int percent : {90, 100, 112, 125}) {
            typography::SetUiFontScale(percent);
            const float interface_scale = static_cast<float>(percent) / 100.0f;
            for (float dpi : {1.0f, 1.5f}) {
                HFONT font = typography::CreateEditFont(dpi);
                LOGFONTW details{};
                const int height = static_cast<int>(std::lround(14.0f * dpi * interface_scale));
                Check(font && GetObjectW(font, sizeof(details), &details) && details.lfHeight == -height,
                    "native edit font follows interface scale and DPI matrix");
                IDWriteTextFormat* format = nullptr;
                typography::CreateTextFormat(compositor.DwriteFactory(), {typography::FontRole::Text, 14.0f * dpi}, &format);
                Check(format && std::abs(format->GetFontSize() - 14.0f * dpi * interface_scale) < 0.01f,
                    "DirectWrite font follows the same interface scale and DPI matrix");
                if (font) DeleteObject(font);
                if (format) format->Release();
            }
        }
        typography::SetUiFontScale(100);
        FluentMenu menu;
        Check(menu.Create(owner, &compositor, 1.0f), "create real reusable filter menu");
        HWND edit = FluentMenuTestPeer::Filter(menu);
        Check(edit != nullptr, "create real filter editor through menu lifecycle");
        if (edit) {
            typography::SetUiFontScale(125);
            Check(FluentMenuTestPeer::Filter(menu) == edit, "filter editor is reused after generation change");
            LOGFONTW font{};
            HFONT handle = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
            Check(GetObjectW(handle, sizeof(font), &font) && font.lfHeight == -18,
                "reused filter native font refreshes to 125 percent");
            FluentMenuTestPeer::Show(menu);
            SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_PRESENT_FAILURE", L"1");
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"fallback 中文"));
            SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_PRESENT_FAILURE", nullptr);
            Check(GetPropW(edit, L"Pulse.NativeEditFallback") != nullptr,
                "real menu presentation failure selects persistent native fallback");
            SendMessageW(edit, EM_SETSEL, 0, 8);
            SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"native"));
            wchar_t text[64]{};
            GetWindowTextW(edit, text, 64);
            Check(std::wstring(text) == L"native 中文", "real menu native fallback preserves Unicode selection and input");
            SendMessageW(edit, WM_UNDO, 0, 0);
            GetWindowTextW(edit, text, 64);
            Check(std::wstring(text) == L"fallback 中文", "real menu native fallback preserves undo");
            FluentMenuTestPeer::Reopen(menu);
            Check(GetPropW(edit, L"Pulse.NativeEditFallback") != nullptr,
                "reopening the menu preserves the working native fallback");
        }
        typography::SetUiFontScale(100);
    }
    DestroyWindow(owner);
    FluentMenuTestPeer::CheckAcrylicPlate([](bool ok, const char* label) {
        Check(ok, label);
        return ok ? 0 : 1;
    });
    Check(GetForegroundWindow() == foreground, "typography tests preserve foreground window");
    CoUninitialize();
    return failures ? 1 : 0;
}
