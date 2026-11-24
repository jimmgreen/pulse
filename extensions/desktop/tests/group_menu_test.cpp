#include "host/overlays/desktop_overlays.h"
#include <cstdio>
#include <set>
using namespace pulse::desktop;
namespace pulse::desktop::overlays {
struct DesktopGroupMenuTestPeer {
    static int Run() {
        int count = 0, failed = 0;
        auto check = [&](bool ok, const char* label) {
            ++count; failed += !ok;
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        };
        DesktopOverlayController overlay;
        overlay.menu_spec_.target_kind = ContextMenuTargetKind::GroupHeader;
        for (bool tabbed : {false,true}) for (int mode = 0; mode < 4; ++mode) {
            overlay.menu_spec_.group_tabbed = tabbed;
            overlay.menu_spec_.group_view_mode = static_cast<model::GroupViewMode>(mode);
            overlay.BuildMenuRows({});
            int detaches = 0, selected = 0;
            std::set<std::string> commands;
            bool labels = true, checked_mode = false;
            for (const auto& row : overlay.menu_rows_) {
                if (row.command_id == "group-detach-tab") ++detaches;
                if (row.command_id != "sub:group-view") continue;
                for (const auto& option : row.submenu_rows) {
                    commands.insert(option.command_id);
                    labels &= !option.label.empty();
                    selected += option.checked;
                    if (option.checked) checked_mode = option.command_id == "group-view:" + std::to_string(mode);
                }
            }
            check(commands.size() == 4 && commands.contains("group-view:0") && commands.contains("group-view:3") && labels,
                  "view submenu exposes four distinct localized commands");
            check(selected == 1 && checked_mode, "only current group view mode is checked");
            check(detaches == (tabbed ? 1 : 0), "detach command appears only for a tabbed group");
        }
        overlay.menu_spec_.group_material = model::CardMaterial::LiquidGlass;
        overlay.BuildMenuRows({});
        bool found_liquid = false;
        int mat_checked = 0;
        for (const auto& row : overlay.menu_rows_) {
            if (row.command_id != "sub:group-mat") continue;
            for (const auto& opt : row.submenu_rows) {
                mat_checked += opt.checked;
                if (opt.command_id == "mat-liquid" && opt.checked) found_liquid = true;
            }
        }
        check(found_liquid && mat_checked == 1, "group material submenu exposes and checks mat-liquid");
        std::printf("Group menu: %d cases, %d failures\n",count,failed);
        return failed ? 1 : 0;
    }
};
}
int main() { return overlays::DesktopGroupMenuTestPeer::Run(); }
