#include "../../app/extensions/native/settings_pages_adapter.h"
#include "../../../sdk/native/include/pulse/ui_data.h"
#include <cstdio>
#include <cstring>
#include <thread>
using namespace pulse::extensions::native;
namespace {
int passes = 0, failures = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); ok ? ++passes : ++failures; }
PulseStringView Text(const char* text) { return {text, static_cast<uint32_t>(std::strlen(text))}; }
}
int main() {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND owner = CreateWindowExW(0, L"STATIC", L"Settings fixture", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        bool live = true;
        SettingsPagesAdapter adapter(owner, false, D2D1_COLOR_F{0, 0.4f, 0.8f, 1}, 1, [&] { return live; });
        PulseSettingsPagesV1 api{};
        Check(adapter.Query(1, &api, sizeof(api)) == PULSE_OK, "query settings pages");
        Check(adapter.Query(9, &api, sizeof(api)) == PULSE_ERROR_ABI, "unknown ABI rejected");
        Check(!adapter.Page(), "no page before create");
        PulseUiModelNode root{}; root.size = sizeof(root); root.type = PULSE_UI_MODEL_TEXT; root.id = Text("label"); root.text = {1, Text("Settings content")};
        PulseUiState initial{Text("value"), Text("initial")};
        PulseUiModelTree tree{}; tree.size = sizeof(tree); tree.root = &root; tree.states = &initial; tree.state_count = 1;
        uint64_t id = 0;
        Check(api.create(api.context, Text(""), &tree, &id) == PULSE_ERROR_ARGUMENT && !id, "empty title rejected");
        Check(api.create(api.context, Text("Archive"), &tree, &id) == PULSE_OK && id, "create model settings page");
        Check(adapter.Page() && adapter.Page()->id == id && adapter.Page()->title == L"Archive", "host descriptor matches copied title");
        const auto window = adapter.Ui().Window();
        Check(window && GetParent(window) == owner && (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) &&
            !(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE), "page starts hidden as child without popup");
        uint64_t duplicate = 99;
        Check(api.create(api.context, Text("Second"), &tree, &duplicate) == PULSE_ERROR_STATE && !duplicate, "one page per session");
        PulseUiModelV1 model{}; PulseUiEventsV1 events{};
        Check(api.query(api.context, id, Text("pulse.ui.model"), 1, &model, sizeof(model)) == PULSE_OK &&
            api.query(api.context, id, Text("pulse.ui.events"), 1, &events, sizeof(events)) == PULSE_OK, "page delegates real UI interfaces");
        PulseUiDataV1 data{};
        Check(api.query(api.context, id, Text(PULSE_UI_DATA_INTERFACE), PULSE_UI_DATA_VERSION,
            &data, sizeof(data)) == PULSE_OK && data.version == PULSE_UI_DATA_VERSION && data.context &&
            data.register_provider && data.bind && data.request && data.publish, "page exposes the real UI data interface");
        Check(api.query(api.context, id, Text(PULSE_UI_DATA_INTERFACE), PULSE_UI_DATA_VERSION + 1,
            &data, sizeof(data)) == PULSE_ERROR_ABI, "page rejects an unsupported UI data ABI");
        Check(api.query(api.context, id, Text("pulse.ui.mount"), 1, &model, sizeof(model)) == PULSE_ERROR_UNAVAILABLE, "public page query excludes mounting");
        PulseUiMountV1 mount{}; adapter.Ui().QueryMount(1, &mount, sizeof(mount));
        Check(mount.set(mount.context, id, PULSE_UI_MOUNT_WINDOW) == PULSE_ERROR_UNAVAILABLE, "embedded adapter cannot escape via mount");
        Check(adapter.Show(id, RECT{10, 20, 400, 300}, true, D2D1_COLOR_F{0, 0.4f, 0.8f, 1}, 1.25f) &&
            (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE), "host shows embedded page");
        adapter.Hide(); Check(!(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE), "host hides page");
        PulseStatus worker_status = PULSE_OK;
        std::thread worker([&] { worker_status = api.remove(api.context, id); }); worker.join();
        Check(worker_status == PULSE_ERROR_STATE && adapter.Page(), "worker calls rejected");
        Check(api.remove(api.context, id) == PULSE_OK && !adapter.Page() && !IsWindow(window), "remove closes page and descriptor");
        Check(api.remove(api.context, id) == PULSE_ERROR_STATE && api.query(api.context, id, Text("pulse.ui.model"), 1, &model, sizeof(model)) == PULSE_ERROR_STATE, "stale IDs rejected");
        Check(api.query(api.context, id, Text(PULSE_UI_DATA_INTERFACE), PULSE_UI_DATA_VERSION,
            &data, sizeof(data)) == PULSE_ERROR_STATE, "closed page no longer exposes UI data");
        Check(model.create(model.context, Text("Bypass"), &tree, &duplicate) == PULSE_ERROR_STATE, "delegated create cannot bypass page ownership");
        uint64_t second = 0; Check(api.create(api.context, Text("Again"), &tree, &second) == PULSE_OK && second != id, "remove permits fresh nonreused ID");
        live = false; Check(!adapter.Page() && api.remove(api.context, second) == PULSE_ERROR_STATE, "dead binding rejects access");
        live = true;
        api.query(api.context, second, Text("pulse.ui.model"), 1, &model, sizeof(model));
        api.query(api.context, second, Text("pulse.ui.events"), 1, &events, sizeof(events));
        struct Callback { SettingsPagesAdapter* adapter; bool called = false, held = false, held_after = false; } callback{&adapter};
        uint64_t token = 0;
        Check(events.subscribe_state(events.context, second, {}, PULSE_UI_STATE_ALL,
            [](void* user, uint64_t, const PulseUiStateBatch*) {
                auto& value = *static_cast<Callback*>(user); value.called = true; value.held = value.adapter->InCallback();
                value.adapter->Close(); value.held_after = value.adapter->InCallback();
            }, &callback, &token) == PULSE_OK, "subscribe real model state callback");
        const PulseUiState change{Text("value"), Text("changed")};
        model.set_states(model.context, second, &change, 1);
        const auto deadline = GetTickCount64() + 2000;
        while (!callback.called && GetTickCount64() < deadline) {
            MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            Sleep(1);
        }
        Check(callback.called && callback.held && callback.held_after && !adapter.InCallback() && !adapter.Page(), "reentrant stop holds callback gate until return");
        adapter.Close(); Check(!adapter.InCallback() && !adapter.Ui().Window(), "stop closes without pending callback");
        live = true; Check(api.create(api.context, Text("Stopped"), &tree, &id) == PULSE_ERROR_STATE, "stop cannot be revived");
    }
    {
        bool live = true;
        SettingsPagesAdapter adapter(owner, false, D2D1_COLOR_F{0, 0.4f, 0.8f, 1}, 1, [&] { return live; });
        PulseSettingsPagesV2 v2{};
        Check(adapter.Query(PULSE_SETTINGS_PAGES_VERSION_2, &v2, sizeof(v2)) == PULSE_OK &&
            v2.version == PULSE_SETTINGS_PAGES_VERSION_2 && v2.create_tab && v2.set_status && v2.set_side_preview,
            "query settings pages v2");
        PulseUiModelNode root{}; root.size = sizeof(root); root.type = PULSE_UI_MODEL_TEXT; root.id = Text("label"); root.text = {1, Text("Tab content")};
        PulseUiModelTree tree{}; tree.size = sizeof(tree); tree.root = &root;
        auto MakeTab = [](const char* id, const char* title, const char* glyph, int32_t order) {
            PulseSettingsTabDesc desc{};
            desc.size = sizeof(desc);
            desc.id = Text(id);
            desc.title = Text(title);
            desc.glyph = Text(glyph);
            desc.order = order;
            return desc;
        };
        uint64_t tab_b = 0, tab_a = 0, dup = 99;
        const auto desc_b = MakeTab("formats", "Formats", "icon:folder", 20);
        const auto desc_a = MakeTab("general", "General", "icon:settings", 10);
        Check(v2.create_tab(v2.context, &desc_b, &tree, &tab_b) == PULSE_OK && tab_b, "create second ordered v2 tab");
        Check(v2.create_tab(v2.context, &desc_a, &tree, &tab_a) == PULSE_OK && tab_a && tab_a != tab_b, "create first ordered v2 tab");
        const auto tabs = adapter.Tabs();
        Check(tabs.size() == 2 && tabs[0].id == tab_a && tabs[0].tab_id == "general" && tabs[0].title == L"General" &&
            tabs[0].glyph == "icon:settings" && tabs[0].order == 10 &&
            tabs[1].id == tab_b && tabs[1].tab_id == "formats" && tabs[1].order == 20, "v2 tabs sorted by order");
        Check(v2.create_tab(v2.context, &desc_a, &tree, &dup) == PULSE_ERROR_STATE && !dup, "duplicate tab id rejected");
        PulseUiDataV1 data{};
        Check(v2.query(v2.context, tab_a, Text(PULSE_UI_DATA_INTERFACE), PULSE_UI_DATA_VERSION, &data, sizeof(data)) == PULSE_OK &&
            data.version == PULSE_UI_DATA_VERSION && data.context != nullptr, "v2 tab queries pulse.ui.data");
        Check(v2.set_status(v2.context, tab_a, PULSE_SETTINGS_STATUS_SAVED, Text("Saved cleanly")) == PULSE_OK &&
            adapter.PageStatus(tab_a) == SettingsPageStatus{PULSE_SETTINGS_STATUS_SAVED, L"Saved cleanly"}, "v2 set_status updates tab header pill");
        Check(v2.set_status(v2.context, tab_a, PULSE_SETTINGS_STATUS_ERROR + 1, Text("bad")) == PULSE_ERROR_ARGUMENT &&
            adapter.PageStatus(tab_a) == SettingsPageStatus{PULSE_SETTINGS_STATUS_SAVED, L"Saved cleanly"}, "v2 invalid status kind rejected");
        PulseUiModelNode preview_root{}; preview_root.size = sizeof(preview_root); preview_root.type = PULSE_UI_MODEL_TEXT;
        preview_root.id = Text("preview_label"); preview_root.text = {1, Text("Side preview")};
        PulseUiModelTree preview_tree{}; preview_tree.size = sizeof(preview_tree); preview_tree.root = &preview_root;
        Check(v2.set_side_preview(v2.context, tab_a, &preview_tree, 320) == PULSE_OK && adapter.HasSidePreview(tab_a) &&
            !adapter.HasSidePreview(tab_b), "v2 set_side_preview attaches preview to target tab only");
        PulseUiModelNode bad_node{}; bad_node.size = sizeof(bad_node); bad_node.type = 999999u; bad_node.id = Text("bad");
        PulseUiModelTree bad_tree{}; bad_tree.size = sizeof(bad_tree); bad_tree.root = &bad_node;
        Check(v2.set_side_preview(v2.context, tab_a, &bad_tree, 320) != PULSE_OK && adapter.HasSidePreview(tab_a),
            "invalid side preview update preserves existing preview");
        Check(v2.set_side_preview(v2.context, tab_a, nullptr, 0) == PULSE_OK && !adapter.HasSidePreview(tab_a),
            "v2 nullptr side preview clears preview");
        Check(v2.set_side_preview(v2.context, tab_a, &preview_tree, 320) == PULSE_OK && adapter.HasSidePreview(tab_a),
            "re-attach side preview before max tab & remove checks");
        uint64_t tab_c = 0, tab_d = 0, tab_e = 0, tab_f = 0, tab_g = 0;
        const auto dc = MakeTab("menu", "Menu", "", 30), dd = MakeTab("icons", "Icons", "", 40),
            de = MakeTab("lang", "Language", "", 50), df = MakeTab("about", "About", "", 60),
            dg = MakeTab("extra", "Extra", "", 70);
        Check(v2.create_tab(v2.context, &dc, &tree, &tab_c) == PULSE_OK &&
            v2.create_tab(v2.context, &dd, &tree, &tab_d) == PULSE_OK &&
            v2.create_tab(v2.context, &de, &tree, &tab_e) == PULSE_OK &&
            v2.create_tab(v2.context, &df, &tree, &tab_f) == PULSE_OK &&
            adapter.Tabs().size() == 6, "up to 6 v2 tabs allowed");
        Check(v2.create_tab(v2.context, &dg, &tree, &tab_g) == PULSE_ERROR_STATE && !tab_g, "7th v2 tab rejected");
        Check(v2.remove(v2.context, tab_a) == PULSE_OK && !adapter.HasSidePreview(tab_a) &&
            adapter.Tabs().size() == 5 && adapter.Tabs().front().id == tab_b, "removing tab clears its preview and preserves siblings");
        adapter.Close();
        Check(adapter.Tabs().empty(), "close removes remaining v2 tabs");
    }
    if (owner) DestroyWindow(owner);
    if (SUCCEEDED(initialized)) CoUninitialize();
    std::printf("%d passed, %d failed\n", passes, failures); return failures ? 1 : 0;
}
