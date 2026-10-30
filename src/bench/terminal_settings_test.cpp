#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_renderer_internal.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {
bool Contains(D2D1_RECT_F outer, D2D1_RECT_F inner) {
    constexpr float epsilon = 0.1f;
    return inner.right > inner.left && inner.bottom > inner.top &&
        inner.left >= outer.left - epsilon && inner.top >= outer.top - epsilon &&
        inner.right <= outer.right + epsilon && inner.bottom <= outer.bottom + epsilon;
}

bool Overlaps(D2D1_RECT_F a, D2D1_RECT_F b) {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}
}

int main() {
    using namespace pulse;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        std::cout << "[FAIL] COM initialization\n";
        return 2;
    }
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const HWND window = CreateWindowExW(0, L"STATIC", L"Pulse terminal settings fixture",
        WS_OVERLAPPEDWINDOW, 0, 0, 1280, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int failures = 0;
    int configurations = 0;
    int snapshots = 0;
    {
        ui::Compositor compositor;
        if (!window || !compositor.Init(window)) {
            std::cout << "[FAIL] isolated renderer initialization\n";
            ++failures;
        } else {
            ui::MainRenderer renderer;
            renderer.SetCompositor(&compositor);
            ui::fluent::Painter painter;
            painter.SetCompositor(&compositor);
            const auto output = std::filesystem::absolute(std::filesystem::path(__FILE__)
                .parent_path().parent_path().parent_path() / L"bench_data/terminal-review");
            std::error_code directory_error;
            std::filesystem::create_directories(output, directory_error);
            if (directory_error) {
                std::cout << "[FAIL] snapshot directory creation\n";
                ++failures;
            }
            for (const auto language : {L"zh-CN", L"en-US"}) {
                l10n::SetLanguage(language);
                for (const float scale : {1.0f, 1.25f, 2.0f}) {
                    compositor.RecreateTextFormats(scale);
                    renderer.SetScale(scale);
                    painter.SetScale(scale);
                    for (const int width : {480, 760, 1280}) {
                        const auto rect = D2D1::RectF(0, 0, width * scale, 900 * scale);
                        compositor.Resize(static_cast<int>(rect.right), static_cast<int>(rect.bottom));
                        for (const bool custom : {false, true}) for (const bool dark : {false, true}) {
                            ++configurations;
                            const auto check = [&](bool condition, const char* label) {
                                if (condition) return;
                                ++failures;
                                std::cout << "[FAIL] " << label << " ("
                                    << (std::wstring_view(language) == L"zh-CN" ? "zh" : "en")
                                    << ", width=" << width << ", scale=" << scale
                                    << ", custom=" << custom << ", dark=" << dark << ")\n";
                            };
                            ui::WindowViewModel vm;
                            vm.settings_open = true;
                            vm.settings_page = 12;
                            vm.dark = dark;
                            if (custom) vm.settings_terminal_program =
                                L"C:\\Terminal Fixture\\A very long program directory with spaces\\"
                                L"Another nested directory for path ellipsis verification\\"
                                L"终端程序\\Terminal.exe";
                            const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
                            painter.BeginFrame(theme, false);
                            auto layout = ui::MakeSettingsLayout(vm, rect, scale, renderer.TitleBarHeight(), 28 * scale, &painter);
                            vm.settings_scroll = (std::max)(0.0f,
                                layout.terminal_program_card.top - layout.content.top - 72 * scale);
                            layout = ui::MakeSettingsLayout(vm, rect, scale, renderer.TitleBarHeight(), 28 * scale, &painter);
                            const auto card = layout.terminal_program_card;
                            const auto choose = layout.terminal_program_choose;
                            const auto reset = layout.terminal_program_reset;
                            const auto path = layout.terminal_program_path;
                            check(Contains(layout.content, card), "terminal card visible after scrolling");
                            check(Contains(card, choose) && Contains(card, reset) && Contains(card, path),
                                "terminal controls stay inside card");
                            check(!Overlaps(choose, reset) && !Overlaps(path, choose) && !Overlaps(path, reset),
                                "terminal path and buttons do not overlap");
                            check(choose.top >= path.bottom + 11.9f * scale,
                                "terminal buttons sit below path");
                            check(choose.right - choose.left + 0.1f >= painter.MeasureButtonWidth(l10n::Get(l10n::StringId::SettingsTerminalChoose)) &&
                                reset.right - reset.left + 0.1f >= painter.MeasureButtonWidth(l10n::Get(l10n::StringId::SettingsTerminalReset)),
                                "localized button labels fit");
                            const auto hit = [&](D2D1_RECT_F bounds) {
                                return renderer.HitTest(vm, rect, (bounds.left + bounds.right) / 2,
                                    (bounds.top + bounds.bottom) / 2);
                            };
                            const auto choose_hit = hit(choose);
                            const auto reset_hit = hit(reset);
                            const auto path_hit = hit(path);
                            check(choose_hit.region == ui::HitTestResult::SettingsTerminalProgram && choose_hit.index == 0,
                                "choose executable hit target");
                            check(custom ? reset_hit.region == ui::HitTestResult::SettingsTerminalProgram && reset_hit.index == 1
                                : !(reset_hit.region == ui::HitTestResult::SettingsTerminalProgram && reset_hit.index == 1),
                                "restore default is actionable only for custom program");
                            check(path_hit.region == ui::HitTestResult::SettingsTerminalProgram && path_hit.index == 2,
                                "displayed path has tooltip hit target");
                            const bool snapshot =
                                (scale == 1.25f && width == 480 && std::wstring_view(language) == L"en-US" && dark && custom) ||
                                (scale == 2.0f && width == 760 && std::wstring_view(language) == L"zh-CN" && !dark && !custom) ||
                                (scale == 1.0f && width == 1280 && std::wstring_view(language) == L"en-US" && !dark && custom) ||
                                (scale == 1.0f && width == 1280 && std::wstring_view(language) == L"zh-CN" && dark && !custom);
                            if (snapshot && !directory_error) {
                                compositor.Dc()->BeginDraw();
                                renderer.Render(vm, rect, theme);
                                check(SUCCEEDED(compositor.Dc()->EndDraw()), "settings renderer completes");
                                const auto file = output / (std::wstring(language) + L"-" + std::to_wstring(width) +
                                    (dark ? L"-dark" : L"-light") + (custom ? L"-custom.png" : L"-default.png"));
                                const bool saved = compositor.SaveSnapshot(file.c_str());
                                check(saved, "terminal settings snapshot saved");
                                if (saved) ++snapshots;
                            }
                        }
                    }
                }
            }
            painter.SetCompositor(nullptr);
            renderer.SetCompositor(nullptr);
        }
    }
    if (window) DestroyWindow(window);
    CoUninitialize();
    std::cout << (failures ? "[FAIL] " : "[PASS] ") << configurations
        << " terminal settings configurations, " << snapshots << " snapshots, " << failures << " failures\n";
    return failures ? 1 : 0;
}
