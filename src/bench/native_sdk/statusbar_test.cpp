#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../app/extensions/statusbar_registry.h"
#include "../../app/extensions/native/statusbar_adapter.h"
#include "../../app/extensions/native/diagnostics_adapter.h"

#include <pulse/client.hpp>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace pulse::extensions::native;
using pulse::extensions::statusbar::Registry;
using pulse::sdk::StatusBarClient;
using pulse::sdk::StatusItem;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (ok) ++g_passed;
    else ++g_failed;
}

struct MockHost {
    StatusBarAdapter* adapter = nullptr;

    static PulseStatus PULSE_CALL QueryInterface(void* context, PulseStringView name, uint32_t version,
                                                 void* out_table, uint32_t out_size) noexcept {
        auto* self = static_cast<MockHost*>(context);
        if (!self || !out_table) return PULSE_ERROR_ARGUMENT;
        if (std::string_view(name.data ? name.data : "", name.length) == PULSE_STATUSBAR_INTERFACE_NAME)
            return self->adapter->Query(version, out_table, out_size);
        return PULSE_ERROR_UNAVAILABLE;
    }
    PulseHostV1 Host() noexcept { return PulseHostV1{sizeof(PulseHostV1), PULSE_ABI_VERSION_1, this, &QueryInterface}; }
};

PulseStringView V(std::string_view text) { return PulseStringView{text.data(), static_cast<uint32_t>(text.size())}; }

PulseStatusItemDesc Desc(std::string_view id, std::string_view text, std::string_view icon = {}) {
    PulseStatusItemDesc desc{};
    desc.size = sizeof(desc);
    desc.id = V(id);
    desc.text = V(text);
    desc.icon = V(icon);
    return desc;
}

} // namespace

int main() {
    // 1. Query and raw descriptor validation.
    {
        Registry registry;
        auto sink = std::make_shared<DiagnosticsSink>();
        StatusBarAdapter adapter("pulse.demo", [] { return true; }, sink, &registry);
        PulseStatusBarV1 table{};
        Check(adapter.Query(2, &table, sizeof(table)) == PULSE_ERROR_ABI &&
                  adapter.Query(PULSE_STATUSBAR_VERSION_1, &table, sizeof(table) - 1) == PULSE_ERROR_ARGUMENT,
              "Query rejects unknown versions and short tables");
        Check(adapter.Query(PULSE_STATUSBAR_VERSION_1, &table, sizeof(table)) == PULSE_OK && table.add_item &&
                  table.update_item && table.set_visible && table.remove_item,
              "Query fills every function");

        PulseStatusItemHandle h = 0;
        PulseStatusItemDesc bad = Desc("bad id", "x");
        Check(table.add_item(table.context, &bad, &h) == PULSE_ERROR_ARGUMENT && h == 0 && sink->Read().field == "id",
              "add_item rejects an invalid id with a diagnostic");
        bad = Desc("empty", "");
        Check(table.add_item(table.context, &bad, &h) == PULSE_ERROR_ARGUMENT && sink->Read().field == "text",
              "add_item needs text or an icon");
        const std::string long_text(65, 'a');
        bad = Desc("long", long_text);
        Check(table.add_item(table.context, &bad, &h) == PULSE_ERROR_ARGUMENT && sink->Read().field == "text",
              "add_item rejects text over 64 bytes");
        bad = Desc("ctl", "a\nb");
        Check(table.add_item(table.context, &bad, &h) == PULSE_ERROR_ARGUMENT, "add_item rejects control characters");
        bad = Desc("icon", "", "no-such-icon");
        Check(table.add_item(table.context, &bad, &h) == PULSE_ERROR_ARGUMENT && sink->Read().field == "icon",
              "add_item rejects unknown icon names");
        bad = Desc("tone", "x");
        bad.tone = 6;
        Check(table.add_item(table.context, &bad, &h) == PULSE_ERROR_ARGUMENT && sink->Read().field == "tone",
              "add_item rejects unknown tones");
        bad = Desc("small", "x");
        bad.size = sizeof(bad) - 8;
        Check(table.add_item(table.context, &bad, &h) == PULSE_ERROR_ABI, "add_item rejects a short desc");
        Check(registry.Visible().empty(), "Rejected items never reach the registry");

        PulseStatusItemDesc ok = Desc("jobs", "3 jobs", "icon:star");
        ok.tone = PULSE_STATUS_TONE_ACCENT;
        Check(table.add_item(table.context, &ok, &h) == PULSE_OK && h != 0, "add_item accepts a valid item");
        auto views = registry.Visible();
        Check(views.size() == 1 && views[0].text == L"3 jobs" && views[0].icon != 0 &&
                  views[0].tone == PULSE_STATUS_TONE_ACCENT && !views[0].clickable,
              "The registry shows text, the resolved icon and the tone; no click means a plain label");
        Check(!registry.Click(h), "Clicking a plain label does nothing");
        PulseStatusItemHandle dup = 0;
        Check(table.add_item(table.context, &ok, &dup) == PULSE_ERROR_STATE && dup == 0, "Duplicate ids are rejected");
        Check(table.update_item(table.context, h + 100, &ok) == PULSE_ERROR_ARGUMENT, "update_item rejects unknown handles");
        PulseStatusItemDesc renamed = Desc("other", "x");
        Check(table.update_item(table.context, h, &renamed) == PULSE_ERROR_ARGUMENT, "update_item cannot change the id");
        Check(table.set_visible(table.context, h + 100, 0) == PULSE_ERROR_ARGUMENT &&
                  table.remove_item(table.context, h + 100) == PULSE_ERROR_ARGUMENT,
              "set_visible and remove_item reject unknown handles");
    }

    // 2. Client round trip: click, update, visibility, limit, RAII, Close.
    {
        Registry registry;
        auto sink = std::make_shared<DiagnosticsSink>();
        StatusBarAdapter adapter("pulse.demo", [] { return true; }, sink, &registry);
        MockHost mock{&adapter};
        const PulseHostV1 raw_host = mock.Host();
        const pulse::sdk::Host host(raw_host);
        auto client = StatusBarClient::TryQuery(host);
        Check(client.has_value() && client->Available(), "StatusBarClient attaches through the adapter");

        int clicks = 0;
        StatusItem spec;
        spec.id = "sync";
        spec.text = "同步中";
        spec.tooltip = "Click to open the sync page";
        spec.on_click = [&] { ++clicks; };
        auto item = client->Add(spec);
        Check(item.has_value() && item->Active(), "Add returns an active item");
        const uint64_t revision = registry.Revision();
        auto views = registry.Visible();
        Check(views.size() == 1 && views[0].text == L"同步中" && views[0].clickable &&
                  views[0].tooltip == L"Click to open the sync page",
              "UTF-8 text and tooltip arrive as wide strings; on_click makes it clickable");
        Check(registry.VisibleClickable(0) && !registry.VisibleClickable(1) && !registry.VisibleClickable(-1),
              "VisibleClickable checks the index");
        Check(registry.Click(views[0].handle) && clicks == 1, "Click runs the plugin callback");

        spec.text = "Done";
        spec.tone = PULSE_STATUS_TONE_SUCCESS;
        int new_clicks = 0;
        spec.on_click = [&] { ++new_clicks; };
        Check(item->Update(spec) == PULSE_OK && registry.Revision() > revision, "Update succeeds and bumps the revision");
        views = registry.Visible();
        Check(views.size() == 1 && views[0].text == L"Done" && views[0].tone == PULSE_STATUS_TONE_SUCCESS,
              "Update replaces text and tone");
        Check(registry.Click(views[0].handle) && clicks == 1 && new_clicks == 1, "Update replaces the click callback");
        spec.on_click = nullptr;
        Check(item->Update(spec) == PULSE_OK && !registry.Visible()[0].clickable, "Update can drop the click callback");

        Check(item->SetVisible(false) == PULSE_OK && registry.Visible().empty(), "SetVisible(false) hides the item");
        Check(!registry.Click(item->Raw()), "Hidden items cannot be clicked");
        Check(item->SetVisible(true) == PULSE_OK && registry.Visible().size() == 1, "SetVisible(true) shows it again");

        StatusItem second;
        second.id = "second";
        second.icon = "folder";
        auto item2 = client->Add(second);
        StatusItem third;
        third.id = "third";
        third.text = "3";
        PulseStatus st = PULSE_OK;
        auto item3 = client->Add(third, &st);
        Check(item2.has_value() && !item3.has_value() && st == PULSE_ERROR_STATE && registry.Visible().size() == 2,
              "At most 2 items per package");
        views = registry.Visible();
        Check(views[0].text == L"Done" && views[1].icon != 0 && views[1].text.empty(),
              "Items keep registration order; icon-only items are allowed");

        // A click that removes its own item.
        std::optional<StatusBarClient::Item> self_removing;
        StatusItem remover;
        remover.id = "remover";
        remover.text = "x";
        remover.on_click = [&] { self_removing.reset(); };
        item2.reset();
        self_removing = client->Add(remover);
        const auto remover_handle = self_removing ? self_removing->Raw() : 0;
        Check(remover_handle != 0 && registry.Click(remover_handle) && !self_removing.has_value() &&
                  registry.Find(remover_handle) == nullptr,
              "A click callback may remove its own item");

        item.reset();
        Check(registry.Visible().empty(), "Destroying an Item removes it");

        // Close removes everything and turns the table off.
        auto keep = client->Add(spec);
        Check(keep.has_value() && registry.Visible().size() == 1, "Re-adding after removal works");
        adapter.Close();
        Check(registry.Visible().empty(), "Close removes every item of the session");
        StatusItem late;
        late.id = "late";
        late.text = "late";
        Check(!client->Add(late).has_value(), "Calls after Close fail");
        keep.reset();   // remove_item on a closed session must be harmless
    }

    // 3. Thread affinity and sessions are independent.
    {
        Registry registry;
        auto sink = std::make_shared<DiagnosticsSink>();
        StatusBarAdapter a("pulse.a", [] { return true; }, sink, &registry);
        StatusBarAdapter b("pulse.b", [] { return true; }, sink, &registry);
        PulseStatusBarV1 ta{}, tb{};
        (void)a.Query(PULSE_STATUSBAR_VERSION_1, &ta, sizeof(ta));
        (void)b.Query(PULSE_STATUSBAR_VERSION_1, &tb, sizeof(tb));
        PulseStatusItemDesc da = Desc("one", "A");
        PulseStatusItemDesc db = Desc("one", "B");
        PulseStatusItemHandle ha = 0, hb = 0;
        Check(ta.add_item(ta.context, &da, &ha) == PULSE_OK && tb.add_item(tb.context, &db, &hb) == PULSE_OK,
              "Two packages may use the same id");
        Check(tb.remove_item(tb.context, ha) == PULSE_ERROR_ARGUMENT, "A session cannot remove another session's item");
        PulseStatus from_thread = PULSE_OK;
        std::thread([&] {
            PulseStatusItemDesc dt = Desc("t", "T");
            PulseStatusItemHandle ht = 0;
            from_thread = ta.add_item(ta.context, &dt, &ht);
        }).join();
        Check(from_thread == PULSE_ERROR_STATE && sink->Read().field == "thread", "Calls from another thread fail");
        b.Close();
        auto views = registry.Visible();
        Check(views.size() == 1 && views[0].text == L"A", "Closing one session keeps the other's items");
    }

    std::printf("pulse_native_statusbar_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
