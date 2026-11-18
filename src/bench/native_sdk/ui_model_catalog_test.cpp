#include "../../app/extensions/native/ui_adapter.h"
#include "../../common/json_value.h"
#include "../../common/windows_compat.h"
#include "ui_model_fixture.h"
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
    if (argc == 4) {
        if (std::wstring_view(argv[2]) != L"--type")
            return 2;
        for (const uint32_t type : native_ui_fixture::kSupportedTypes) {
            if (pulse::json::Wide(native_ui_fixture::kExpectedTypeNames[type]) == argv[3])
                selected_type = type;
        }
        if (selected_type == PULSE_UI_MODEL_COUNT)
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
            if (selected_type != PULSE_UI_MODEL_COUNT && type != selected_type)
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
        adapter.Stop();
    }
    DestroyWindow(owner);
    report.set("checks", std::move(checks));
    report.set("passed", passed);
    report.set("failed", failed);
    report.set("images", images);
    report.set("modelTypes", selected_type == PULSE_UI_MODEL_COUNT ? 27 : 1);
    report.set("visualReviewRequired", true);
    std::ofstream file(output / "checks.json", std::ios::binary);
    file << pulse::json::Serialize(report) << '\n';
    std::printf("%d PASS, %d FAIL, %d PNG\n", passed, failed, images);
    return failed || !file ? 1 : 0;
}
