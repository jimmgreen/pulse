#include "../../app/extensions/native/settings_pages_adapter.h"
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
    if (owner) DestroyWindow(owner);
    if (SUCCEEDED(initialized)) CoUninitialize();
    std::printf("%d passed, %d failed\n", passes, failures); return failures ? 1 : 0;
}
