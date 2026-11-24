#include "../../app/extensions/native/ui_adapter.h"
#include "../../common/json_value.h"
#include "../../common/windows_compat.h"
#include "ui_model_fixture.h"
#include "../../../sdk/native/include/pulse/model_builder.hpp"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <objbase.h>
#include <string_view>

namespace {
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
ULONGLONG menu_deadline = 0;
bool menu_found = false;
void CALLBACK MenuKeyboard(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND menu = nullptr;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND window, LPARAM value) -> BOOL {
            wchar_t name[64]{};
            GetClassNameW(window, name, 64);
            if (IsWindowVisible(window) && std::wstring_view(name) == L"PulseFluentMenu") {
                *reinterpret_cast<HWND *>(value) = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&menu));
    if (menu) {
        menu_found = true;
        KillTimer(nullptr, timer);
        // Post through the real modal menu queue; no adapter callback is invoked directly.
        PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
    } else if (GetTickCount64() >= menu_deadline) {
        KillTimer(nullptr, timer);
        PostThreadMessageW(GetCurrentThreadId(), WM_KEYDOWN, VK_ESCAPE, 0);
    }
}
void PULSE_CALL Command(void *user, uint64_t, PulseStringView id) {
    auto &events = *static_cast<std::map<std::string, int> *>(user);
    ++events[std::string(id.data, id.length)];
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    using namespace pulse::extensions;
    if ((argc != 2 && argc != 4) || !std::filesystem::path(argv[1]).is_absolute()) {
        std::fprintf(stderr, "Usage: pulse_native_ui_model_catalog_test ABSOLUTE-NEW-OUTPUT-DIRECTORY [--type TYPE]\n");
        return 2;
    }
    uint32_t selected_type = PULSE_UI_MODEL_COUNT;
    bool presentation_only = false;
    if (argc == 4) {
        if (std::wstring_view(argv[2]) != L"--type")
            return 2;
        presentation_only = std::wstring_view(argv[3]) == L"presentation";
        for (const uint32_t type : native_ui_fixture::kSupportedTypes) {
            if (pulse::json::Wide(native_ui_fixture::kExpectedTypeNames[type]) == argv[3])
                selected_type = type;
        }
        if (selected_type == PULSE_UI_MODEL_COUNT && !presentation_only)
            return 2;
    }
    const std::filesystem::path output(argv[1]);
    if (std::filesystem::exists(output)) {
        std::fprintf(stderr, "Output directory must be new\n");
        return 2;
    }
    std::filesystem::create_directories(output);
    pulse::compat::EnableDpiAwareness();
    Com com;
    if (FAILED(com.result))
        return 2;
    HWND owner = CreateWindowExW(0, L"STATIC", L"Native model catalog owner", WS_OVERLAPPEDWINDOW, 0, 0, 800, 680,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner)
        return 2;
    auto report = pulse::json::Value::Object();
    auto checks = pulse::json::Value::Array();
    int passed = 0, failed = 0, images = 0;
    auto check = [&](bool ok, const std::string &name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
        auto entry = pulse::json::Value::Object();
        entry.set("name", name);
        entry.set("pass", ok);
        checks.push(std::move(entry));
        ok ? ++passed : ++failed;
    };
    {
        native::UiAdapter adapter(owner, false, D2D1::ColorF(0x0078d4), 1, [] { return true; });
        PulseUiModelV1 ui{};
        PulseUiModelV1 model{};
        const bool queried = adapter.QueryModel(PULSE_UI_MODEL_VERSION, &ui, sizeof(ui)) == PULSE_OK &&
                             adapter.QueryModel(PULSE_UI_MODEL_VERSION, &model, sizeof(model)) == PULSE_OK;
        check(queried, "query actual typed model and shared state interface");
        if (!queried) {
            DestroyWindow(owner);
            return 1;
        }
        for (const uint32_t type : native_ui_fixture::kSupportedTypes) {
            if (presentation_only || (selected_type != PULSE_UI_MODEL_COUNT && type != selected_type))
                continue;
            native_ui_fixture::Model fixture(type);
            const std::string name(native_ui_fixture::kExpectedTypeNames[type]);
            const auto title = "Native " + name;
            uint64_t panel = 0;
            const auto status =
                model.create(model.context, {title.data(), static_cast<uint32_t>(title.size())}, &fixture.tree, &panel);
            check(status == PULSE_OK && panel, name + ": create real host panel");
            if (status != PULSE_OK || !panel)
                continue;
            Pump();
            const auto light = output / (std::to_string(type) + "-" + name + "-light.png");
            const bool captured = adapter.Snapshot(light.wstring(), false, 1);
            check(captured, name + ": native light capture");
            if (captured)
                ++images;
            std::map<std::string, int> events;
            bool subscribed = true;
            for (const auto command : fixture.commands) {
                uint64_t token = 0;
                subscribed &=
                    model.subscribe_command(model.context, panel, command, Command, &events, &token) == PULSE_OK;
            }
            check(subscribed, name + ": typed command subscriptions");
            const HWND control = adapter.NodeWindow("sample");
            if (type == PULSE_UI_MODEL_BUTTON) {
                SendMessageW(control, BM_CLICK, 0, 0);
                check(events["run"] == 1, name + ": actual click reaches registered command callback");
            }
            if (type == PULSE_UI_MODEL_MENU_BUTTON) {
                menu_found = false;
                menu_deadline = GetTickCount64() + 2000;
                const UINT_PTR timer = SetTimer(nullptr, 0, 20, MenuKeyboard);
                if (timer)
                    SendMessageW(control, WM_KEYDOWN, VK_RETURN, 0);
                if (timer)
                    KillTimer(nullptr, timer);
                check(timer && menu_found && events["menu"] == 1, "real FluentMenu selection reaches command callback");
            }
            if (type == PULSE_UI_MODEL_INFO_BAR) {
                SendMessageW(control, WM_KEYDOWN, VK_RIGHT, 0);
                SendMessageW(control, WM_KEYDOWN, VK_RIGHT, 0);
                SendMessageW(control, WM_KEYDOWN, VK_RETURN, 0);
                check(events["close"] == 1, "InfoBar close part reaches command callback");
            }
    
            if (type == PULSE_UI_MODEL_VIRTUAL_LIST || type == PULSE_UI_MODEL_TREE || type == PULSE_UI_MODEL_DATA_TABLE) {
                SendMessageW(control, WM_KEYDOWN, VK_RETURN, 0);
                check(events["row"] == 1, name + ": selected static data row reaches callback");
            }
            const uint32_t next = selected_type == PULSE_UI_MODEL_COUNT ? native_ui_fixture::NextType(type) : type;
            native_ui_fixture::Model replacement(next);
            if (selected_type != PULSE_UI_MODEL_COUNT && replacement.root.text.present)
                replacement.root.text = native_ui_fixture::T("Updated native model sample");
            const auto updated = model.update(model.context, panel, &replacement.tree);
            check(updated == PULSE_OK, name + ": update live typed model");
            Pump();
            if (updated == PULSE_OK) {
                const std::string next_name(native_ui_fixture::kExpectedTypeNames[next]);
                const auto dark = output / (std::to_string(next) + "-" + next_name + "-updated-dark.png");
                const bool dark_captured = adapter.Snapshot(dark.wstring(), true, 1);
                check(dark_captured, next_name + ": updated native dark capture");
                if (dark_captured) ++images;
            }
            check(ui.close(ui.context, panel) == PULSE_OK, name + ": model interface closes panel");
        }
        if (presentation_only || selected_type == PULSE_UI_MODEL_COUNT) {
            // Presentation extras on the real renderer: type ramp, tones, padding, tooltip, canvas pointer.
            using pulse::sdk::CanvasBuilder;
            using pulse::sdk::ModelNode;
            using pulse::sdk::ModelTree;
            auto tree = ModelTree(ModelNode::VStack("root", {
                ModelNode::HStack("crumbs", {ModelNode::Crumb("c1", "Home", "go"), ModelNode::Crumb("c2", "Archives", "go"),
                                             ModelNode::Crumb("c3", "2026", "go", true)}, 4),
                ModelNode::Card("card", {}, {
                    ModelNode::Label("title", "Archive health", "title"),
                    ModelNode::Label("subtitle", "Last verified 2 minutes ago", "subtitle", "secondary"),
                    ModelNode::Label("ok", "All 14 volumes passed", "bodyStrong", "success"),
                    ModelNode::Label("warn", "1 volume is close to its size limit", "body", "warning"),
                    ModelNode::Label("hash", "sha256  3f1a9c0b7e44d2a1", "code", "secondary"),
                    ModelNode::HStack("actions", {ModelNode::Button("verify", "Verify again", "go", "primary")
                                                      .Tooltip("Re-hash every volume").MaxWidth(160)}),
                }, 6).Padding(24),
                CanvasBuilder("chart").ViewBox(100, 40).Rect(2, 2, 96, 36, 6, "card", "border", 1)
                    .Rect(10, 10, 30, 20, 4, "accent").Text(50, 14, "click me", "secondary", "caption")
                    .PointerState("pointer").Command("go").MinHeight(96).Bordered().BuildNode(),
                ModelNode::StatusBar("status", "14 volumes", "1 warning", "Ready"),
            })).State("pointer", "").Command("go");
            uint64_t panel = 0;
            const auto created = model.create(model.context, {"Presentation", 12}, &tree.Build(), &panel);
            check(created == PULSE_OK && panel, "presentation: create panel with extras");
            if (created == PULSE_OK && panel) {
                Pump();
                std::map<std::string, int> events;
                uint64_t token = 0;
                check(model.subscribe_command(model.context, panel, {"go", 2}, Command, &events, &token) == PULSE_OK,
                      "presentation: subscribe canvas command");
                for (const bool dark : {false, true}) {
                    const auto path = output / (std::string("presentation-") + (dark ? "dark" : "light") + ".png");
                    const bool ok = adapter.Snapshot(path.wstring(), dark, 1);
                    check(ok, std::string("presentation: ") + (dark ? "dark" : "light") + " capture");
                    if (ok) ++images;
                }
                const auto large = output / "presentation-light-150.png";
                if (adapter.Snapshot(large.wstring(), false, 1.5f)) ++images;
                const auto read_state = [&](const char *key) {
                    char buffer[128]{};
                    uint32_t needed = 0;
                    const PulseStringView view{key, static_cast<uint32_t>(std::strlen(key))};
                    if (model.get_state(model.context, panel, view, buffer, sizeof(buffer), &needed) != PULSE_OK)
                        return std::string("<error>");
                    return std::string(buffer, needed);
                };
                const HWND canvas = adapter.NodeWindow("chart");
                RECT rc{};
                if (canvas) GetClientRect(canvas, &rc);
                const LPARAM centre = MAKELPARAM((rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2);
                SendMessageW(canvas, WM_MOUSEMOVE, 0, centre);
                const auto moved = read_state("pointer");
                float mx = -1, my = -1;
                const bool parsed_move = sscanf_s(moved.c_str(), "move %f %f", &mx, &my) == 2;
                check(canvas && parsed_move && std::fabs(mx - 50) <= 1.5f && std::fabs(my - 20) <= 1.5f,
                      "presentation: hover reports viewBox centre (" + moved + ")");
                SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
                SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(1, 1));
                const auto clicked = read_state("pointer");
                float cx = -1, cy = -1;
                const bool parsed_click = sscanf_s(clicked.c_str(), "click %f %f", &cx, &cy) == 2;
                check(parsed_click && cx >= 0 && cx < 50 && cy >= 0 && cy < 20 && events["go"] == 1,
                      "presentation: click writes press point then fires command (" + clicked + ")");
                SendMessageW(canvas, WM_MOUSELEAVE, 0, 0);
                check(read_state("pointer") == "leave", "presentation: leave reported");
                // Tooltip: hover the button for longer than the 500 ms delay, then capture the live frame.
                const HWND button = adapter.NodeWindow("verify");
                // The real cursor is elsewhere, so TME_LEAVE posts an immediate WM_MOUSELEAVE for this
                // synthetic hover; drop only that message instead of moving the user's cursor.
                SendMessageW(button, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
                const auto until = GetTickCount64() + 900;
                while (GetTickCount64() < until) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        if (message.message == WM_MOUSELEAVE && message.hwnd == button)
                            continue;
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    Sleep(15);
                }
                // The owner is never shown, so invalidation produces no WM_PAINT; paint the tree host once.
                if (button)
                    SendMessageW(GetParent(button), WM_PAINT, 0, 0);
                const auto tip = output / "presentation-tooltip.png";
                const bool tip_ok = adapter.SnapshotCurrent(tip.wstring());
                check(button && tip_ok, "presentation: live frame with tooltip captured");
                if (tip_ok) ++images;
                check(ui.close(ui.context, panel) == PULSE_OK, "presentation: close");
            }
        }
        adapter.Stop();
    }
    DestroyWindow(owner);
    report.set("checks", std::move(checks));
    report.set("passed", passed);
    report.set("failed", failed);
    report.set("images", images);
    report.set("modelTypes", presentation_only ? 0 : selected_type == PULSE_UI_MODEL_COUNT ? 39 : 1);
    report.set("visualReviewRequired", true);
    std::ofstream file(output / "checks.json", std::ios::binary);
    file << pulse::json::Serialize(report) << '\n';
    std::printf("%d PASS, %d FAIL, %d PNG\n", passed, failed, images);
    return failed || !file ? 1 : 0;
}
