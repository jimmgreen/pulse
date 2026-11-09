#include "../../app/extensions/native/commands_adapter.h"
#include <pulse/sdk.hpp>
#include <cstdio>
using namespace pulse::extensions::native;
using pulse::sdk::View;
struct Fixture {
    bool live = true, invalidate_capture = false;
    int called = 0, captures = 0;
    uint64_t revision = 1;
    static PulseStatus PULSE_CALL Summary(void* user, PulseContextChangeFn callback, void* context) {
        PulseContextSnapshot snapshot{}; snapshot.selection_revision = static_cast<Fixture*>(user)->revision;
        callback(context, &snapshot); return PULSE_OK;
    }
    std::string name = "sample.7z";
    static PulseStatus PULSE_CALL Capture(void* user, PulseContextSelectionFn callback, void* context) {
        auto& self = *static_cast<Fixture*>(user);
        ++self.captures;
        const PulseContextSelectionEntry row{sizeof(row), 0, View("C:\\data\\sample.7z"), View(self.name)};
        const PulseContextSelectionSnapshot selection{sizeof(selection), 5, View("C:\\data"), &row, 1};
        if (self.invalidate_capture) self.live = false;
        callback(context, &selection); return PULSE_OK;
    }
    static void PULSE_CALL Invoke(void* user, PulseCommandId, const PulseContextSelectionSnapshot* selection) {
        if (selection->entry_count == 1 && selection->selection_revision == 5) ++static_cast<Fixture*>(user)->called;
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
    return failures ? 1 : 0;
}
