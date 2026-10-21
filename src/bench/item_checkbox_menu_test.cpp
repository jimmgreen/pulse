// Focused, windowless regression for the two View-menu entry points.
#include "../app/view_menu.h"
#include "../app/context_menu.h"
#include "../common/localization.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace pulse;

constexpr int kCheckboxCommands[] = {
    app::CmdItemCheckboxAuto, app::CmdItemCheckboxAlways, app::CmdItemCheckboxNever,
};
static_assert(app::CmdItemCheckboxAlways == app::CmdItemCheckboxAuto + 1 &&
              app::CmdItemCheckboxNever == app::CmdItemCheckboxAuto + 2);

int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

bool SameRows(const std::vector<ui::FluentMenuItem>& a,
              const std::vector<ui::FluentMenuItem>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.command != y.command || x.text != y.text || x.glyph != y.glyph ||
            x.enabled != y.enabled || x.checked != y.checked || x.radio != y.radio ||
            x.radio_group != y.radio_group || x.separator_after != y.separator_after ||
            !x.children.empty() || !y.children.empty()) return false;
    }
    return true;
}

bool CheckPolicy(const std::wstring& policy, int selected) {
    for (int mode_index = 0; mode_index < 8; ++mode_index) {
        for (const bool filesystem : { false, true }) {
            for (const bool details : { false, true }) {
                const auto mode = ui::ViewModeFromIndex(mode_index);
                const auto menu = app::BuildViewMenu(mode, details, filesystem, policy);
                if (menu.size() != (filesystem ? 13u : 12u)) return false;
                for (int i = 0; i < 8; ++i) {
                    const auto& row = menu[static_cast<size_t>(i)];
                    if (row.command != app::CmdViewBase + i || row.radio != (i == mode_index))
                        return false;
                }
                if (!menu[7].separator_after || menu[8].command != app::CmdDetailsPanel ||
                    menu[8].checked != details || !menu[8].separator_after) return false;
                for (int i = 0; i < 3; ++i) {
                    const auto& row = menu[static_cast<size_t>(9 + i)];
                    if (row.command != kCheckboxCommands[i] || !row.enabled || !row.radio_group ||
                        row.radio != (i == selected) || row.checked || !row.children.empty() ||
                        row.text.empty()) return false;
                }
                if (menu[11].separator_after != filesystem ||
                    (filesystem && menu.back().command != app::CmdApplyViewToAllFolders)) return false;

                app::BackgroundViewOptions options;
                options.view_mode = mode;
                options.details_panel = details;
                options.filesystem = filesystem;
                options.item_checkboxes = policy;
                const auto background_view = app::BuildViewSubmenuItem(options);
                if (background_view.command != app::CmdNone ||
                    background_view.text != l10n::Get(l10n::StringId::View) ||
                    !SameRows(menu, background_view.children)) return false;
            }
        }
    }
    return true;
}

} // namespace

int wmain() {
    for (const auto language : { L"zh-CN", L"en-US" }) {
        pulse::l10n::Initialize(GetModuleHandleW(nullptr), language);
        Check(CheckPolicy(L"auto", 0), "auto: one selected policy; both View entry points agree in all modes");
        Check(CheckPolicy(L"always", 1), "always: one selected policy; both View entry points agree in all modes");
        Check(CheckPolicy(L"never", 2), "never: one selected policy; both View entry points agree in all modes");
        Check(CheckPolicy(L"invalid", 0) && CheckPolicy(L"", 0),
              "unknown and empty values fall back to auto, matching the renderer resolver");
        const auto defaults = pulse::app::BuildViewMenu(pulse::ui::ViewMode::Details);
        const bool chinese = std::wstring(language) == L"zh-CN";
        Check(defaults.size() == 12 && defaults[9].radio &&
              defaults[9].text == (chinese ? L"项目复选框：自动显示" : L"Item checkboxes: Show automatically") &&
              defaults[10].text == (chinese ? L"项目复选框：始终显示" : L"Item checkboxes: Always show") &&
              defaults[11].text == (chinese ? L"项目复选框：隐藏" : L"Item checkboxes: Hide"),
              "default and localized policy labels remain explicit in both languages");
    }
    std::printf("item_checkbox_menu_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
