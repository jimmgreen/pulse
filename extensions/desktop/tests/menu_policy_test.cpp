#include "host/overlays/desktop_overlays.h"
#include "ipc/assistant_availability.h"
#include "../../../src/common/utf8_file.h"
#include <filesystem>
#include <cstdio>

namespace pulse::desktop::overlays {
struct DesktopMenuPolicyTestPeer {
    static int Run(const std::filesystem::path& dir) {
        int failures = 0;
        auto check = [&](bool ok, const char* name) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
            failures += !ok;
        };
        std::filesystem::create_directories(dir);
        const auto file = (dir / L"context_menu.json").wstring();
        const auto json_for = [](std::wstring_view mode, bool health) {
            return L"{\"extensions\":{\"placement\":{\"pulse.desktop\":{\"mode\":\"" +
                std::wstring(mode) + L"\"}},\"commands\":{\"native:pulse.desktop:desktop-health\":{\"visible\":" +
                std::wstring(health ? L"true" : L"false") + L"}}}}";
        };
        DesktopOverlayController overlay;
        overlay.menu_spec_.target_kind = ContextMenuTargetKind::BlankDesktop;
        check(WriteUtf8FileAtomic(file, json_for(L"submenu", false)), "write isolated settings fixture");
        check(WriteUtf8FileAtomic((dir / L"app.json").wstring(), L"{\"language\":\"zh-CN\"}"), "isolate initial product language");
        overlay.menu_policy_.Start(file);
        auto wait_for = [&](DesktopMenuPolicy::Placement placement, bool health) {
            for (int i = 0; i < 50; ++i) {
                const auto policy = overlay.menu_policy_.Snapshot();
                if (policy.placement == placement && policy.Visible("health-check") == health) return true;
                Sleep(20);
            }
            return false;
        };
        auto contains = [](auto&& self, const auto& rows, std::string_view id) -> bool {
            for (const auto& row : rows)
                if (row.command_id == id || self(self, row.submenu_rows, id)) return true;
            return false;
        };
        auto root = [&](std::string_view id) {
            return std::find_if(overlay.menu_rows_.begin(), overlay.menu_rows_.end(),
                [&](const auto& row) { return row.command_id == id; }) != overlay.menu_rows_.end();
        };
        check(wait_for(DesktopMenuPolicy::Placement::Submenu, false), "background reader consumes persisted settings");
        model::BeautifySettings settings;
        overlay.BuildMenuRows(settings);
        check(!contains(contains, overlay.menu_rows_, "health-check"), "disabled health command absent from actual menu");
        check(root("sub:desktop-organize") && !root("ai-organize"), "actual menu groups desktop commands");
        check(std::all_of(overlay.menu_rows_.begin(), overlay.menu_rows_.end(),
            [](const auto& row) { return !row.label.empty(); }), "all menu rows have localized labels");
        check(contains(contains, overlay.menu_rows_, "toggle-icons"), "grouping preserves toggle icons command");
        auto& shell = overlay.shell_verbs_.emplace_back();
        shell.label = L"Fixture Shell action"; shell.item_id = 91; shell.enabled = true;
        overlay.BuildMenuRows(settings);
        check(root("sub:system-extra") && !root("shell:Fixture Shell action:0:91"), "Shell extras use separate submenu");
        check(overlay.SaveContextMenuSnapshot((dir / L"submenu.png").c_str(), overlay.menu_spec_, settings, false),
            "render grouped menu through actual Fluent renderer");
        check(WriteUtf8FileAtomic(file, json_for(L"flat", true)) &&
            wait_for(DesktopMenuPolicy::Placement::Flat, true), "settings change hot reloads without desktop restart");
        overlay.BuildMenuRows(settings);
        check(root("health-check") && root("ai-organize") && root("toggle-icons") && !root("sub:desktop-organize"),
            "flat mode restores all registered rows including toggle icons");
        check(WriteUtf8FileAtomic(file, json_for(L"hidden", false)) &&
            wait_for(DesktopMenuPolicy::Placement::Hidden, false), "hidden settings hot reload");
        overlay.BuildMenuRows(settings);
        check(!contains(contains, overlay.menu_rows_, "ai-organize") &&
            !contains(contains, overlay.menu_rows_, "toggle-icons") && root("open-pack-settings"),
            "hidden mode removes extension commands and retains settings recovery");
        check(WriteUtf8FileAtomic(file, L"{incomplete"), "write malformed isolated fixture");
        Sleep(250);
        check(overlay.menu_policy_.Snapshot().placement == DesktopMenuPolicy::Placement::Hidden,
            "malformed update retains last valid policy");
        check(WriteUtf8FileAtomic(file, L"{}") && wait_for(DesktopMenuPolicy::Placement::Default, true),
            "reset restores defaults and visibility");
        overlay.BuildMenuRows(settings);
        check(root("sub:desktop-organize") && contains(contains, overlay.menu_rows_, "health-check"),
            "default desktop menu stays compact");
        const auto ai_dir = dir / L"ai";
        std::filesystem::create_directories(ai_dir);
        const auto ai_file = (ai_dir / L"ai.json").wstring();
        ai::AssistantAvailability assistant(nullptr, ai_dir.wstring());
        check(WriteUtf8FileAtomic(ai_file, L"{\"mcp_enabled\":true,\"enabled\":false}"),
            "write master switch off with separate MCP switch on");
        overlay.menu_spec_.ai_organize_enabled = assistant.Enabled();
        overlay.BuildMenuRows(settings);
        check(!contains(contains, overlay.menu_rows_, "ai-organize") &&
              contains(contains, overlay.menu_rows_, "auto-organize") &&
              contains(contains, overlay.menu_rows_, "health-check"),
            "master off omits AI from desktop popup and preserves rule organization and health");
        overlay.menu_spec_.target_kind = ContextMenuTargetKind::GroupHeader;
        overlay.BuildMenuRows(settings);
        check(!contains(contains, overlay.menu_rows_, "ai-organize-group") &&
              contains(contains, overlay.menu_rows_, "edit-rule"),
            "master off omits group AI archive and preserves group rules");
        check(WriteUtf8FileAtomic(ai_file, L"{\"assistant\":{\"enabled\":false}}"),
            "replace master setting with enabled default and no credentials");
        overlay.menu_spec_.ai_organize_enabled = assistant.Enabled();
        overlay.BuildMenuRows(settings);
        check(contains(contains, overlay.menu_rows_, "ai-organize-group"),
            "master on restores group AI archive without a model key");
        overlay.menu_spec_.target_kind = ContextMenuTargetKind::BlankDesktop;
        overlay.BuildMenuRows(settings);
        check(contains(contains, overlay.menu_rows_, "ai-organize"),
            "master on restores desktop AI popup entry on the same controller");
        check(WriteUtf8FileAtomic((dir / L"app.json").wstring(), L"{\"language\":\"en-US\"}"), "write product language fixture");
        for (int i = 0; i < 50 && overlay.menu_policy_.Snapshot().language != L"en-US"; ++i) Sleep(20);
        overlay.BuildMenuRows(settings);
        check(std::any_of(overlay.menu_rows_.begin(), overlay.menu_rows_.end(), [](const auto& row) {
            return row.command_id == "sub:desktop-organize" && row.label == L"Desktop organization";
        }), "menu labels follow explicit product language rather than OS language");
        DesktopMenuPolicy parsed;
        check(DesktopMenuPolicy::Parse(L"{\"extensions\":{\"placement\":{\"another.package\":{\"mode\":\"hidden\"}},"
            L"\"commands\":{\"native:another.package:desktop-health\":{\"visible\":false}}}}", parsed) &&
            parsed.placement == DesktopMenuPolicy::Placement::Default && parsed.Visible("health-check"),
            "other package preferences cannot affect desktop");
        overlay.menu_policy_.Stop();
        std::printf("RESULT: failures=%d\n", failures);
        return failures ? 1 : 0;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int result = pulse::desktop::overlays::DesktopMenuPolicyTestPeer::Run(argv[1]);
    CoUninitialize();
    return result;
}
