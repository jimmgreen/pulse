#if defined(PULSE_WITH_TEST_HOOKS)
#include "app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include "settings_context_tree.h"
#include "settings_context_extension.h"
#include "system_ctx_model.h"
#include "context_menu_controller.h"
#include "context_menu.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
namespace pulse::ui {
bool SystemCtxReadOnlyStatusGeometry(float text_left, float note_right, float note_width,
                                    float scale, float& badge_right);
}
#include "system_ctx_tree_ui_test.inc"
#include "native_explorer_menu_ui_test.inc"
#include "native_explorer_live_test.inc"
#include "open_with_settings_ui_test.inc"

DWORD PrepareSystemCtxUiDesktop(int argc, wchar_t** argv) {
    if (!GetEnvironmentVariableW(L"PULSE_TEST_SYSTEM_CTX_UI", nullptr, 0)) return ERROR_SUCCESS;
    if (argc != 2 || wcscmp(argv[1], L"--test-instance") != 0 ||
        !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) return ERROR_INVALID_PARAMETER;
    if (GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_EXPLORER_LIVE", nullptr, 0)) return ERROR_SUCCESS;
    const auto name = L"PulseContextReview-" + std::to_wstring(GetCurrentProcessId());
    static HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return GetLastError();
    return ERROR_SUCCESS;
}

int RunSystemCtxUiTest(pulse::AppState& s, const wchar_t* output) {
    using namespace pulse;
    const auto base = std::filesystem::path(output).parent_path();
    std::filesystem::create_directories(base);
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; failures += !ok;
        log.flush();
    };
    if (GetEnvironmentVariableW(L"PULSE_TEST_OPEN_WITH_SETTINGS_UI", nullptr, 0)) {
        RunOpenWithSettingsUiChecks(s, base, check);
        log << "failures=" << failures << '\n';
        return failures ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_EXPLORER_LIVE", nullptr, 0)) {
        RunNativeExplorerLiveCheck(s, base, log, check);
        log << "failures=" << failures << '\n';
        return failures ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_NATIVE_EXPLORER_MENU_UI", nullptr, 0)) {
        RunNativeExplorerMenuUiChecks(s, base, check);
        log << "failures=" << failures << '\n';
        return failures ? 1 : 0;
    }
    RunSystemCtxTreeUiChecks(s, base, check);
    const auto language = l10n::preference();
    const auto original_width = s.compositor.Width(), original_height = s.compositor.Height();
    const float original_scale = s.scale;
    ui::fluent::Painter painter(&s.compositor);
    for (const auto* locale : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(locale);
        for (float scale : {1.0f, 1.5f, 2.0f}) {
            s.compositor.RecreateTextFormats(scale);
            s.renderer.SetScale(scale);
            painter.SetScale(scale);
            painter.InvalidateTypography();
            for (bool narrow : {false, true}) for (bool dark : {false, true}) {
                const int width = static_cast<int>((narrow ? 760 : 1500) * scale);
                const int height = static_cast<int>(1080 * scale);
                s.compositor.Resize(width, height);
                ui::WindowViewModel vm;
                vm.settings_open = true; vm.settings_page = 6;
                vm.dark = dark; vm.window_effect = ui::WindowEffect::None;
                auto view = std::make_shared<ui::SysCtxView>();
                view->state = ui::SysCtxView::State::Ready;
                view->scene_title = locale[0] == L'z' ? L"文件夹" : L"Folders";
                view->scenes.push_back({view->scene_title});
                for (const auto* name : {L"NanaZip", L"百度网盘", L"Bandizip", L"Notepad", L"QQ"}) {
                    ui::SysCtxRowView row;
                    row.title = name;
                    row.subtitle = l10n::Get(l10n::StringId::PackagedMenuSource);
                    row.read_only = true;
                    row.pulse = view->rows.size() == 1 ? 2 : 1;
                    row.method = l10n::Get(l10n::StringId::PackagedMenuReadOnly);
                    view->rows.push_back(std::move(row));
                }
                ui::SysCtxRowView registry_child;
                registry_child.title = L"Nested application command with a long descriptive title";
                registry_child.depth = 3;
                registry_child.read_only = true;
                registry_child.method = l10n::Get(l10n::StringId::ContextTreeRegistryChild);
                view->rows.push_back(std::move(registry_child));
                view->filter_counts[0] = 6;
                vm.settings_sysctx = view;
                const auto bounds = D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));
                const auto layout = ui::MakeSettingsLayout(vm, bounds, scale,
                    s.renderer.TitleBarHeight(), 28 * scale, &painter);
                const auto& o = layout.sysctx;
                const auto& tree = o.tree_button;
                check(tree.right > tree.left && tree.bottom > tree.top && tree.left >= 0 &&
                    tree.right <= width && ui::HitTestSettingsSysCtx(layout, vm,
                        (tree.left + tree.right) / 2, (tree.top + tree.bottom) / 2) == ui::kSysCtxTree,
                    "submenu organizer entry fits and production hit testing reaches it");
                const auto& extension = o.extension_button;
                check(extension.right > extension.left && (extension.left >= tree.right || extension.top >= tree.bottom) &&
                    extension.left >= 0 && extension.right <= width &&
                    ui::HitTestSettingsSysCtx(layout, vm, (extension.left + extension.right) / 2,
                        (extension.top + extension.bottom) / 2) == ui::kSysCtxOtherType,
                    "other file type entry fits beside organizer and production hit testing reaches it");
                bool fit = o.pulse_buttons.size() == view->rows.size();
                bool hits = fit;
                for (size_t i = 0; i < o.pulse_buttons.size(); ++i) {
                    const auto& b = o.pulse_buttons[i];
                    const auto& row = o.rows[i];
                    if (view->rows[i].pulse == 0) {
                        fit &= b.left == b.right && o.more[i].left >= row.left + 90 * scale;
                        const float text_left = row.left + 86 * scale;
                        float badge_right = 0;
                        const bool second_line = ui::SystemCtxReadOnlyStatusGeometry(text_left,
                            o.switches[i].right, 160 * scale, scale, badge_right);
                        fit &= badge_right - 6 * scale >= text_left + 120 * scale;
                        if (narrow) fit &= second_line;
                        continue;
                    }
                    const auto text = l10n::Get(view->rows[i].pulse == 2 ?
                        l10n::StringId::SysCtxRestoreInPulse : l10n::StringId::SysCtxHideInPulse);
                    fit &= b.left >= row.left + 90 * scale && b.right <= o.more[i].left &&
                        b.top >= row.top && b.bottom <= row.bottom &&
                        b.right - b.left >= painter.MeasureButtonWidth(text, L"") - 0.5f;
                    hits &= ui::HitTestSettingsSysCtx(layout, vm, (b.left + b.right) / 2,
                        (b.top + b.bottom) / 2) == ui::kSysCtxPulseBase + static_cast<int>(i);
                }
                check(fit, "Pulse hide/restore labels fit real measured buttons without overlapping more or title");
                check(hits, "production hit testing routes all readonly rows to Pulse actions");
                view->state = ui::SysCtxView::State::Loading;
                const auto& button = o.pulse_buttons[0];
                check(ui::HitTestSettingsSysCtx(layout, vm, (button.left + button.right) / 2,
                    (button.top + button.bottom) / 2) == -1, "loading blocks Pulse action hit target");
                view->state = ui::SysCtxView::State::Ready;
                auto* dc = s.compositor.Dc();
                dc->BeginDraw();
                s.renderer.Render(vm, bounds, ui::MakeTheme(dark, s.accentColor));
                check(SUCCEEDED(dc->EndDraw()), "production system settings renderer completes");
                const auto name = L"sysctx-" + std::wstring(locale) + L"-" +
                    std::to_wstring(static_cast<int>(scale * 100)) + (narrow ? L"-narrow" : L"-wide") +
                    (dark ? L"-dark.png" : L"-light.png");
                check(s.compositor.SaveSnapshot((base / name).c_str()), "production renderer snapshot saved");
            }
        }
    }
    l10n::SetLanguage(l10n::LanguageId(language));
    s.compositor.Resize(original_width, original_height);
    s.compositor.RecreateTextFormats(original_scale); s.renderer.SetScale(original_scale);
    log << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}

#endif
