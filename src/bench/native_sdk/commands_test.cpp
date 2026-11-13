#include "../../app/extensions/icon_registry.h"
#include "../../app/extensions/native/commands_adapter.h"
#include "../../app/extensions/native/icons_adapter.h"
#include <pulse/client.hpp>
#include <cstdio>
#include <cstring>
using namespace pulse::extensions::native;
using pulse::sdk::View;
struct Fixture {
    bool live = true, invalidate_capture = false, empty_selection = false, multiple_selection = false;
    std::string invoked_path;
    int called = 0, bg_called = 0, captures = 0;
    uint64_t revision = 1;
    static PulseStatus PULSE_CALL Summary(void* user, PulseContextChangeFn callback, void* context) {
        PulseContextSnapshot snapshot{}; snapshot.selection_revision = static_cast<Fixture*>(user)->revision;
        callback(context, &snapshot); return PULSE_OK;
    }
    std::string name = "sample.7z";
    static PulseStatus PULSE_CALL Capture(void* user, PulseContextSelectionFn callback, void* context) {
        auto& self = *static_cast<Fixture*>(user);
        ++self.captures;
        const PulseContextSelectionEntry rows[] = {
            {sizeof(PulseContextSelectionEntry), 0, View("C:\\data\\sample.7z"), View(self.name)},
            {sizeof(PulseContextSelectionEntry), 0, View("C:\\data\\other.7z"), View("other.7z")}
        };
        const PulseContextSelectionSnapshot selection{
            sizeof(selection),
            self.revision,
            View("C:\\data"),
            self.empty_selection ? nullptr : rows,
            self.empty_selection ? 0u : self.multiple_selection ? 2u : 1u,
        };
        if (self.invalidate_capture) self.live = false;
        callback(context, &selection); return PULSE_OK;
    }
    static void PULSE_CALL Invoke(void* user, PulseCommandId, const PulseContextSelectionSnapshot* selection) {
        if (selection->entry_count == 1) {
            auto& self = *static_cast<Fixture*>(user);
            ++self.called;
            self.invoked_path.assign(selection->entries[0].path.data, selection->entries[0].path.length);
        }
    }
    static void PULSE_CALL InvokeBg(void* user, PulseCommandId, const PulseContextSelectionSnapshot* selection) {
        if (selection->entry_count == 0 && selection->directory.length > 0) ++static_cast<Fixture*>(user)->bg_called;
    }
};
int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; };
    Fixture fixture;
    PulseContextV1 context{}; context.context = &fixture; context.get_selection = Fixture::Capture; context.get = Fixture::Summary;
    const auto owner = reinterpret_cast<HWND>(1);
    CommandsAdapter adapter(owner, "archive", context, [&] { return fixture.live; });
    PulseCommandsV1 table{};
    check(adapter.Query(1, &table, sizeof(table)) == PULSE_OK, "commands table available");
    const PulseStringView extension = View(".7z");
    PulseCommandDescriptor command{}; command.size = sizeof(command); command.id = View("extract"); command.title = View("Extract");
    command.kinds = PULSE_COMMAND_FILES; command.min_selection = 1; command.max_selection = 1;
    command.extensions = &extension; command.extension_count = 1; command.invoke = Fixture::Invoke; command.user = &fixture;
    PulseCommandId id = 0, duplicate = 0;
    check(table.register_command(table.context, &command, &id) == PULSE_OK, "register native callback and copied filter");
    check(table.register_command(table.context, &command, &duplicate) == PULSE_ERROR_STATE, "duplicate identity rejected");
    auto rows = CommandsAdapter::Entries(owner);
    check(rows.size() == 1 && rows[0].enabled, "archive selection matches");
    const auto captures = fixture.captures;
    CommandsAdapter::Entries(owner); CommandsAdapter::Entries(owner);
    check(fixture.captures == captures, "unchanged toolbar state reuses bounded selection snapshot");
    CommandsAdapter::Invoke(owner, static_cast<int>(id)); check(fixture.called == 1, "callback receives current selection");
    fixture.name = "ordinary.txt";
    CommandsAdapter::Invoke(owner, static_cast<int>(id)); check(fixture.called == 1, "stale menu rechecks changed extension");
    fixture.name = "sample.7z";
    CommandsAdapter::Invoke(reinterpret_cast<HWND>(2), static_cast<int>(id)); check(fixture.called == 1, "foreign owner cannot dispatch");
    fixture.invalidate_capture = true;
    CommandsAdapter::Invoke(owner, static_cast<int>(id)); check(fixture.called == 1, "capture revocation prevents callback");
    fixture.live = true; fixture.invalidate_capture = false;
    check(table.unregister_command(table.context, id) == PULSE_OK, "unregister succeeds");
    check(CommandsAdapter::Invoke(owner, static_cast<int>(id)) && fixture.called == 1, "retired menu token swallowed");
    command.virtual_key = 'X'; command.modifiers = PULSE_COMMAND_CTRL | PULSE_COMMAND_SHIFT;
    check(table.register_command(table.context, &command, &id) == PULSE_OK, "optional shortcut registration");
    check(CommandsAdapter::Shortcut(owner, 'X', command.modifiers) && fixture.called == 2, "shortcut dispatch shares filter and callback");
    adapter.Close();
    check(CommandsAdapter::Entries(owner).empty() && CommandsAdapter::Invoke(owner, static_cast<int>(id)) && fixture.called == 2,
        "stop invalidates contributions and pending tokens");

    {
        Fixture v2_fix;
        v2_fix.revision = 10;
        PulseContextV1 v2_ctx{}; v2_ctx.context = &v2_fix; v2_ctx.get_selection = Fixture::Capture; v2_ctx.get = Fixture::Summary;
        const auto owner2 = reinterpret_cast<HWND>(11);
        IconsAdapter icons_adapter("demo.tools", [&] { return v2_fix.live; });
        CommandsAdapter v2_adapter(owner2, "demo.tools", v2_ctx, [&] { return v2_fix.live; });

        struct GuardedIcons {
            PulseIconsV1 table{};
            uint64_t canary = 0xCAFEBABECAFEBABEu;
        } guarded_icons;
        check(icons_adapter.Query(PULSE_ICONS_VERSION_1, &guarded_icons.table, sizeof(guarded_icons)) == PULSE_OK &&
            guarded_icons.canary == 0xCAFEBABECAFEBABEu,
            "pulse.icons V1 query preserves trailing caller memory");
        PulseIconId builtin_sparkle = 0;
        check(guarded_icons.table.resolve_builtin(guarded_icons.table.context, View("sparkle"), &builtin_sparkle) == PULSE_OK &&
            builtin_sparkle == PULSE_ICON_SPARKLE,
            "pulse.icons resolve_builtin resolves built-in StoreIcon IDs");

        auto icon_builder = pulse::sdk::IconBuilder("plus-accent")
            .StrokePath("M 3 12 L 21 12")
            .StrokePath("M 12 3 L 12 21", 1.8, false, PULSE_ICON_COLOR_ACCENT);
        pulse::sdk::IconRegistration icon_reg;
        check(icon_reg.Register(guarded_icons.table, icon_builder.Build()) == PULSE_OK &&
            pulse::extensions::icons::IsCustomIconId(icon_reg.Id()),
            "IconRegistration registers custom multi-layer vector icon");

        PulseCommandsV2 v2{};
        check(v2_adapter.Query(PULSE_COMMANDS_VERSION_2, &v2, sizeof(v2)) == PULSE_OK, "PulseCommandsV2 table available");

        // Legacy V2-sized descriptor (72 bytes) remains binary-compatible
        PulseCommandDescriptor legacy_desc{};
        legacy_desc.size = PULSE_COMMAND_DESCRIPTOR_SIZE_V2;
        legacy_desc.id = View("legacy-v2");
        legacy_desc.title = View("Legacy V2");
        legacy_desc.icon = PULSE_ICON_SPARKLE;
        legacy_desc.kinds = PULSE_COMMAND_FILES;
        legacy_desc.min_selection = 1;
        legacy_desc.max_selection = 4;
        legacy_desc.invoke = Fixture::Invoke;
        legacy_desc.user = &v2_fix;
        PulseCommandId legacy_id = 0;
        check(v2.register_command(v2.context, &legacy_desc, &legacy_id) == PULSE_OK,
            "72-byte V2 command descriptor registers cleanly with extended built-in icon");

        // V3 descriptor with custom icon, group_icon, presentation flags & dynamic update/show/hide
        PulseCommandDescriptor dyn_desc{};
        dyn_desc.size = sizeof(dyn_desc);
        dyn_desc.id = View("toggle-tool");
        dyn_desc.title = View("Toggle Tool");
        dyn_desc.group = View("Tools");
        dyn_desc.flags = PULSE_COMMAND_GROUP_SUBMENU | PULSE_COMMAND_PRIMARY | PULSE_COMMAND_SEPARATOR_BEFORE;
        dyn_desc.icon = icon_reg.Id();
        dyn_desc.group_icon = PULSE_ICON_GEAR;
        dyn_desc.kinds = PULSE_COMMAND_FILES;
        dyn_desc.min_selection = 1;
        dyn_desc.max_selection = 8;
        pulse::sdk::CommandRegistration dyn_reg;
        check(dyn_reg.Register(v2, dyn_desc, [&](PulseCommandId, const PulseContextSelectionSnapshot& sel) {
            if (sel.entry_count == 1) ++v2_fix.called;
        }) == PULSE_OK && dyn_reg.Id() != 0, "CommandRegistration V2 registers command");
        const PulseCommandId original_id = dyn_reg.Id();

        dyn_desc.title = View("Toggle Tool (Checked)");
        dyn_desc.flags |= PULSE_COMMAND_CHECKED;
        dyn_desc.icon = 0;
        dyn_desc.icon_name = View("puzzle");
        check(dyn_reg.Update(dyn_desc) == PULSE_OK && dyn_reg.Id() == original_id,
            "update_command mutates metadata in place while preserving PulseCommandId");
        auto v2_rows = CommandsAdapter::Entries(owner2);
        check(v2_rows.size() == 2 &&
            v2_rows[1].title == "Toggle Tool (Checked)" &&
            v2_rows[1].icon == PULSE_ICON_PUZZLE &&
            v2_rows[1].group_icon == PULSE_ICON_GEAR &&
            (v2_rows[1].flags & PULSE_COMMAND_CHECKED) != 0,
            "updated command reflects new title, resolved icon_name, group_icon, and checked flag");

        // Dynamic hide via set_command_flags
        check(dyn_reg.SetFlags(dyn_desc.flags | PULSE_COMMAND_HIDDEN) == PULSE_OK, "set_command_flags sets PULSE_COMMAND_HIDDEN");
        const auto summaries = CommandsAdapter::LivePackageCommands(owner2, "demo.tools");
        v2_rows = CommandsAdapter::Entries(owner2);
        check(summaries.size() == 2 && summaries[1].hidden_by_plugin && !v2_rows[1].enabled,
            "PULSE_COMMAND_HIDDEN disables command in menu while reporting hidden_by_plugin in settings summary");
        const int before_hidden_invoke = v2_fix.called;
        CommandsAdapter::Invoke(owner2, static_cast<int>(original_id));
        check(v2_fix.called == before_hidden_invoke, "hidden command cannot be invoked");

        // Re-show dynamically
        check(dyn_reg.SetFlags(dyn_desc.flags & ~static_cast<uint32_t>(PULSE_COMMAND_HIDDEN)) == PULSE_OK,
            "set_command_flags clears PULSE_COMMAND_HIDDEN");
        CommandsAdapter::Invoke(owner2, static_cast<int>(original_id));
        check(v2_fix.called == before_hidden_invoke + 1, "re-shown command dispatches again");

        // Background folder context menu command
        PulseCommandDescriptor bg_desc{};
        bg_desc.size = sizeof(bg_desc);
        bg_desc.id = View("open-here");
        bg_desc.title = View("Open Here");
        bg_desc.detail = View("Folder: {base}");
        bg_desc.icon = PULSE_ICON_FOLDER;
        bg_desc.kinds = PULSE_COMMAND_BACKGROUND;
        bg_desc.min_selection = 0;
        bg_desc.max_selection = 0;
        bg_desc.invoke = Fixture::InvokeBg;
        bg_desc.user = &v2_fix;
        PulseCommandId bg_id = 0;
        check(v2.register_command(v2.context, &bg_desc, &bg_id) == PULSE_OK, "register PULSE_COMMAND_BACKGROUND command");
        v2_rows = CommandsAdapter::Entries(owner2);
        check(!v2_rows[2].enabled, "background-only command does not match file selection");

        v2_fix.empty_selection = true;
        ++v2_fix.revision;
        v2_rows = CommandsAdapter::Entries(owner2);
        check(v2_rows[2].enabled && v2_rows[2].detail == "Folder: data",
            "background command matches empty folder selection and formats {base} folder name");
        CommandsAdapter::Invoke(owner2, static_cast<int>(bg_id));
        check(v2_fix.bg_called == 1, "background command invokes with empty selection and folder path");

        const PulseIconId custom_id = icon_reg.Id();
        v2_adapter.Close();
        icons_adapter.Close();
        check(!pulse::extensions::icons::GlobalRegistry().Snapshot(custom_id).has_value(),
            "closing IconsAdapter unregisters session dynamic icons");
    }
    {
        Fixture hidden;
        hidden.multiple_selection = true;
        PulseContextV1 ctx{}; ctx.context = &hidden; ctx.get_selection = Fixture::Capture; ctx.get = Fixture::Summary;
        const auto window = reinterpret_cast<HWND>(3);
        CommandsAdapter hidden_adapter(window, "archive", ctx, [&] { return hidden.live; });
        PulseCommandsV1 api{};
        check(hidden_adapter.Query(1, &api, sizeof(api)) == PULSE_OK, "hidden dispatch commands table available");
        auto descriptor = command;
        descriptor.virtual_key = 0; descriptor.modifiers = 0;
        descriptor.id = View("extract-auto"); descriptor.flags = PULSE_COMMAND_HIDDEN; descriptor.user = &hidden;
        PulseCommandId hidden_id = 0;
        check(api.register_command(api.context, &descriptor, &hidden_id) == PULSE_OK, "hidden auto command registered");
        const auto identity = CommandsAdapter::Identity(window, static_cast<int>(hidden_id));
        CommandSelection selected{"C:\\clicked", {{"C:\\clicked\\actual.7Z", "actual.7Z", false}}, true};
        const auto before = hidden.captures;
        check(CommandsAdapter::InvokeHiddenIdentity(window, identity, selected) && hidden.called == 1 &&
              hidden.invoked_path == "C:\\clicked\\actual.7Z" && hidden.captures == before,
              "hidden dispatch uses explicit clicked path without capturing current multiselection");
        CommandsAdapter::Invoke(window, static_cast<int>(hidden_id));
        const auto entries = CommandsAdapter::Entries(window);
        check(hidden.called == 1 && entries.size() == 1 && !entries[0].enabled,
              "hidden command remains unavailable through ordinary menu invocation");
        auto bad = selected; bad.entries[0].name = "file.txt";
        check(!CommandsAdapter::InvokeHiddenIdentity(window, identity, bad), "hidden dispatch rejects wrong extension");
        bad = selected; bad.entries[0].directory = true;
        check(!CommandsAdapter::InvokeHiddenIdentity(window, identity, bad), "hidden dispatch rejects directory");
        bad = selected; bad.entries.push_back(selected.entries[0]);
        check(!CommandsAdapter::InvokeHiddenIdentity(window, identity, bad), "hidden dispatch enforces maximum selection");
        bad = selected; bad.valid = false;
        check(!CommandsAdapter::InvokeHiddenIdentity(window, identity, bad), "hidden dispatch rejects invalid snapshot");
        hidden.live = false;
        check(!CommandsAdapter::InvokeHiddenIdentity(window, identity, selected) && hidden.called == 1,
              "hidden dispatch rejects disabled live package");
        hidden.live = true;
        descriptor.id = View("ordinary"); descriptor.flags = 0;
        PulseCommandId ordinary_id = 0;
        check(api.register_command(api.context, &descriptor, &ordinary_id) == PULSE_OK &&
              !CommandsAdapter::InvokeHiddenIdentity(window, CommandsAdapter::Identity(window, static_cast<int>(ordinary_id)), selected),
              "hidden dispatch rejects ordinary command");
        check(!CommandsAdapter::InvokeHiddenIdentity(window, "missing", selected) &&
              !CommandsAdapter::InvokeHiddenIdentity(reinterpret_cast<HWND>(4), identity, selected),
              "hidden dispatch rejects missing identity and foreign owner");
    }
    return failures ? 1 : 0;
}
