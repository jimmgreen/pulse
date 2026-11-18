#include "../../app/extensions/native/ui_adapter.h"
#include "../../common/windows_compat.h"
#include "../../ui/extension_tree.h"
#include "ui_model_fixture.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <commctrl.h>
#include <filesystem>
#include <objbase.h>
#include <string>

namespace {
PulseStringView S(const char *text) { return {text, static_cast<uint32_t>(std::strlen(text))}; }
struct Com {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Com() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT)
            break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
struct Model {
    std::array<PulseUiModelNode, 5> nodes{};
    std::array<PulseStringView, 2> options{S("First"), S("Second")};
    std::array<PulseUiState, 3> states{{{S("input"), S("initial")}, {S("output"), S("Waiting")}, {S("selected"), S("0")}}};
    PulseStringView command = S("apply");
    PulseUiModelTree tree{};
    Model() {
        using native_ui_fixture::Node;
        using native_ui_fixture::T;
        nodes = {Node(PULSE_UI_MODEL_STACK, "root"), Node(PULSE_UI_MODEL_TEXT_INPUT, "input-node"),
                 Node(PULSE_UI_MODEL_BUTTON, "apply"), Node(PULSE_UI_MODEL_TEXT, "output-node"), Node(PULSE_UI_MODEL_SELECT, "list-node")};
        nodes[0].children = {1, nodes.data() + 1, 4};
        nodes[0].gap = {1, 12};
        nodes[1].text = T("Name"); nodes[1].state = T("input");
        nodes[2].text = T("Apply"); nodes[2].command = T("apply");
        nodes[3].binding = T("state.output");
        nodes[4].state = T("selected"); nodes[4].options = {1, options.data(), 2};
        tree.size = sizeof(tree); tree.root = nodes.data(); tree.states = states.data(); tree.state_count = 3;
        tree.commands = &command; tree.command_count = 1;
    }
};
std::string Get(const PulseUiModelV1 &api, uint64_t panel, const char *key) {
    char bytes[1024]{};
    uint32_t needed = 0;
    if (api.get_state(api.context, panel, S(key), bytes, sizeof(bytes), &needed) != PULSE_OK)
        return "<failed>";
    return {bytes, needed};
}
struct Callback {
    PulseUiModelV1 *api;
    pulse::extensions::native::UiAdapter *adapter;
    Model *model;
    int calls = 0;
    bool inside = false;
    bool succeeded = false;
    bool close = false;
    static void PULSE_CALL Run(void *context, uint64_t panel, PulseStringView) {
        auto &self = *static_cast<Callback *>(context);
        ++self.calls;
        self.inside = self.adapter->InCallback();
        if (self.close) {
            self.succeeded = self.api->close(self.api->context, panel) == PULSE_OK;
            return;
        }
        const auto input = Get(*self.api, panel, "input");
        const auto result = "Hello " + input;
        PulseUiState value{S("output"), {result.data(), static_cast<uint32_t>(result.size())}};
        self.succeeded = self.api->set_states(self.api->context, panel, &value, 1) == PULSE_OK;
        // A callback changes real node structure, not only state or manifest commands.
        self.model->nodes[3].id = S("replacement-output");
        self.succeeded &= self.api->update(self.api->context, panel, &self.model->tree) == PULSE_OK;
    }
};
struct PositionProbe {
    unsigned changes = 0;
    unsigned enables = 0;
    static LRESULT CALLBACK Watch(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                  UINT_PTR, DWORD_PTR reference) {
        auto& self = *reinterpret_cast<PositionProbe*>(reference);
        if (message == WM_WINDOWPOSCHANGING)
            ++self.changes;
        if (message == WM_ENABLE)
            ++self.enables;
        return DefSubclassProc(window, message, wparam, lparam);
    }
};
RECT Bounds(HWND window) {
    RECT bounds{};
    GetWindowRect(window, &bounds);
    MapWindowPoints(nullptr, GetParent(window), reinterpret_cast<POINT*>(&bounds), 2);
    return bounds;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    pulse::compat::EnableDpiAwareness();
    Com com;
    if (FAILED(com.result))
        return 2;
    int failed = 0, passed = 0;
    auto check = [&](bool result, const char *name) {
        std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
        result ? ++passed : ++failed;
    };
    HWND owner = CreateWindowExW(0, L"STATIC", L"Native UI owner", WS_OVERLAPPEDWINDOW, 0, 0, 720, 600, nullptr,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner)
        return 2;
    bool alive = true;
    {
        pulse::extensions::native::UiAdapter adapter(owner, false, D2D1::ColorF(0x0078d4), 1.0f, [&] { return alive; });
        PulseUiModelV1 api{};
        const bool queried = adapter.QueryModel(PULSE_UI_MODEL_VERSION, &api, sizeof(api)) == PULSE_OK;
        check(queried, "query typed UI v1");
        if (!queried) {
            DestroyWindow(owner);
            return 1;
        }
        Model model;
        pulse::extensions::UiTree converted;
        std::set<std::string> commands;
        std::string error;
        const auto converted_status = pulse::extensions::native::ConvertUiModel(&model.tree, converted, commands, error);
        if (converted_status != PULSE_OK) std::printf("Model conversion: %s\n", error.c_str());
        check(converted_status == PULSE_OK, "model fixture passes public conversion");
        uint64_t panel = 0;
        check(api.create(api.context, S("Native callback test"), &model.tree, &panel) == PULSE_OK && panel,
              "create actual host panel and typed controls");
        Pump();
        HWND input = adapter.NodeWindow("input-node");
        check(input && adapter.NodeWindow("apply") && adapter.NodeWindow("list-node"),
              "actual input button select HWNDs exist");
        SetWindowTextW(input, L"Developer");
        Pump();
        check(Get(api, panel, "input") == "Developer", "native edit notification commits input state");
        const auto stable_bounds = Bounds(input);
        PositionProbe position;
        check(SetWindowSubclass(input, PositionProbe::Watch, 0x4c41594f, reinterpret_cast<DWORD_PTR>(&position)) != FALSE,
              "observe actual edit positioning messages");
        bool updates_ok = true;
        for (unsigned i = 0; i < 32; ++i) {
            const PulseUiState value{S("input"), S(i % 2 ? "Developer A" : "Developer B")};
            updates_ok &= api.set_states(api.context, panel, &value, 1) == PULSE_OK;
        }
        std::printf("[METRIC] stableEditPositionRequests=%u\n", position.changes);
        check(updates_ok && position.changes == 0, "state-only updates skip unchanged native window geometry");
        RemoveWindowSubclass(input, PositionProbe::Watch, 0x4c41594f);
        MoveWindow(input, stable_bounds.left + 7, stable_bounds.top + 3,
                   stable_bounds.right - stable_bounds.left, stable_bounds.bottom - stable_bounds.top, FALSE);
        const PulseUiState restored{S("input"), S("Developer")};
        check(api.set_states(api.context, panel, &restored, 1) == PULSE_OK, "restore input after geometry updates");
        const auto repaired = Bounds(input);
        check(EqualRect(&stable_bounds, &repaired) != FALSE, "layout repairs externally changed native bounds");
        RECT outer{};
        GetWindowRect(adapter.Window(), &outer);
        MoveWindow(adapter.Window(), outer.left, outer.top, outer.right - outer.left + 80,
                   outer.bottom - outer.top, TRUE);
        Pump();
        const auto wider = Bounds(input);
        check(wider.right - wider.left > stable_bounds.right - stable_bounds.left,
              "real panel resizing still updates edit geometry");
        MoveWindow(adapter.Window(), outer.left, outer.top, outer.right - outer.left, outer.bottom - outer.top, TRUE);
        Pump();
        uint32_t needed = 0;
        char tiny = '#';
        check(api.get_state(api.context, panel, S("input"), nullptr, 0, &needed) == PULSE_OK && needed == 9 &&
                  api.get_state(api.context, panel, S("input"), &tiny, 1, &needed) == PULSE_ERROR_ARGUMENT &&
                  tiny == '#',
              "get_state sizing and no partial buffer overwrite");
        HWND list = adapter.NodeWindow("list-node");
        SendMessageW(list, CB_SETCURSEL, 1, 0);
        SendMessageW(GetParent(list), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(list), CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(list));
        check(Get(api, panel, "selected") == "1", "real select selection updates declared state");
        PulseUiState invalid[] = {{S("output"), S("must not commit")}, {S("selected"), S("99")}};
        check(api.set_states(api.context, panel, invalid, 2) != PULSE_OK && Get(api, panel, "output") == "Waiting",
              "invalid state batch has no partial effects");
        Callback callback{&api, &adapter, &model};
        uint64_t subscription = 0;
        check(api.subscribe_command(api.context, panel, S("apply"), Callback::Run, &callback, &subscription) == PULSE_OK,
              "register native developer callback");
        SendMessageW(adapter.NodeWindow("apply"), BM_CLICK, 0, 0);
        Pump();
        check(callback.calls == 1 && callback.inside && callback.succeeded && !adapter.InCallback() &&
                  Get(api, panel, "output") == "Hello Developer",
              "real button callback reads input and updates output");
        check(!adapter.NodeWindow("output-node") && adapter.NodeWindow("replacement-output") &&
                  adapter.NodeWindow("input-node") == input && Get(api, panel, "input") == "Developer",
              "callback replaces structure while preserving retained edit identity and state");
        auto old_children = model.nodes[0].children;
        model.nodes[0].children = {1, model.nodes.data(), 1};
        check(api.update(api.context, panel, &model.tree) == PULSE_ERROR_ARGUMENT &&
                  adapter.NodeWindow("replacement-output"),
              "invalid topology rejected without replacing live tree");
        model.nodes[0].children = old_children;
        check(api.unsubscribe(api.context, subscription) == PULSE_OK, "unsubscribe succeeds");
        SendMessageW(adapter.NodeWindow("apply"), BM_CLICK, 0, 0);
        check(callback.calls == 1, "unsubscribed callback no longer runs");
        if (argc == 2) {
            const std::filesystem::path output(argv[1]);
            if (!output.is_absolute())
                return 2;
            std::filesystem::create_directories(output);
            check(adapter.Snapshot((output / "native-ui.png").wstring(), false, 1), "capture actual host UI");
        }
        callback.close = true;
        check(api.subscribe_command(api.context, panel, S("apply"), Callback::Run, &callback, &subscription) == PULSE_OK,
              "register reentrant close callback");
        Callback later{&api, &adapter, &model};
        uint64_t later_subscription = 0;
        check(api.subscribe_command(api.context, panel, S("apply"), Callback::Run, &later, &later_subscription) ==
                  PULSE_OK,
              "register callback after the closing callback");
        SendMessageW(adapter.NodeWindow("apply"), BM_CLICK, 0, 0);
        Pump();
        check(callback.calls == 2 && callback.succeeded && later.calls == 0 && !adapter.Window() &&
                  !adapter.InCallback(),
              "callback closes its panel safely before returning");
        check(api.close(api.context, panel) == PULSE_ERROR_STATE &&
                  api.unsubscribe(api.context, subscription) == PULSE_ERROR_STATE,
              "closed panel and subscriptions are stale");
        const auto stale = panel;
        check(api.create(api.context, S("Reopened"), &model.tree, &panel) == PULSE_OK && panel != stale,
              "reopening uses new panel generation");
        check(api.update(api.context, stale, &model.tree) == PULSE_ERROR_STATE,
              "old handle cannot alter reopened panel");
        alive = false;
        check(api.update(api.context, panel, &model.tree) == PULSE_ERROR_STATE,
              "owner revocation rejects retained API table");
        adapter.Stop();
        check(!adapter.Window(), "stop destroys panel after revocation");
    }
    {
        using namespace pulse::extensions;
        UiTree model;
        model.root.type = UiNodeType::Stack;
        model.root.id = "layout-root";
        UiNode input, button;
        input.type = UiNodeType::TextInput;
        input.id = "layout-input";
        input.state = "value";
        button.type = UiNodeType::Button;
        button.id = "layout-button";
        button.text = "Action";
        button.enabled_binding = "state.enabled";
        button.visible_binding = "state.shown";
        model.root.children = {input, button};
        model.state = {{"value", "first"}, {"enabled", "false"}, {"shown", "true"}};
        pulse::ui::ExtensionTreeView tree;
        const bool created = tree.Create(owner, model, {}, false, D2D1::ColorF(0x0078d4), 1);
        check(created, "create real layout and binding regression tree");
        if (created) {
            tree.SetBounds({0, 0, 400, 240});
            HWND edit = tree.NodeWindow("layout-input"), action = tree.NodeWindow("layout-button");
            PositionProbe disabled;
            SetWindowSubclass(action, PositionProbe::Watch, 0x4c41594f, reinterpret_cast<DWORD_PTR>(&disabled));
            check(!IsWindowEnabled(action) && tree.SetState("value", "second") &&
                      !IsWindowEnabled(action) && disabled.enables == 0,
                  "unrelated state update does not transiently enable a disabled control");
            check(tree.SetState("enabled", "true") && IsWindowEnabled(action), "enabled binding still enables control");
            check(tree.SetState("shown", "false") && !(GetWindowLongPtrW(action, GWL_STYLE) & WS_VISIBLE),
                  "visible binding still hides control");
            check(tree.SetState("shown", "true") && (GetWindowLongPtrW(action, GWL_STYLE) & WS_VISIBLE),
                  "visible binding still restores control");
            RemoveWindowSubclass(action, PositionProbe::Watch, 0x4c41594f);
            const auto original = Bounds(edit);
            tree.SetTheme(true, D2D1::ColorF(0x0078d4), 1.5f);
            const auto scaled = Bounds(edit);
            check(scaled.bottom - scaled.top > original.bottom - original.top,
                  "live DPI and theme change recomputes edit line height");
            tree.SetTheme(false, D2D1::ColorF(0x0078d4), 1);
            const auto reset = Bounds(edit);
            check(EqualRect(&original, &reset) != FALSE, "restoring live DPI restores original edit geometry");
            HRGN before = CreateRectRgn(0, 0, 0, 0), after = CreateRectRgn(0, 0, 0, 0);
            GetWindowRgn(edit, before);
            tree.SetBounds({0, 0, 400, 24});
            GetWindowRgn(edit, after);
            check(!EqualRgn(before, after), "narrow viewport changes the native clipping region");
            tree.SetBounds({0, 0, 400, 240});
            GetWindowRgn(edit, after);
            check(EqualRgn(before, after) != FALSE, "expanded viewport restores native clipping region");
            DeleteObject(before);
            DeleteObject(after);
            tree.Close();
        }
    }
    DestroyWindow(owner);
    std::printf("%d PASS, %d FAIL\n", passed, failed);
    return failed ? 1 : 0;
}
