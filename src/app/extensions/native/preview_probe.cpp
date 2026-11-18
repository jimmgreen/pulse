#include "preview_probe.h"
#include "previews_adapter.h"
#include "../../app_runtime.h"
#include "../../../ui/quick_preview_window.h"
#include "../../../ui/extension_panel.h"
#include <filesystem>
#include <functional>
#include <cstring>
#include <ostream>

namespace pulse::ui {
struct NativePreviewProbe {
    static bool Archive(QuickPreviewWindow& view, const wchar_t* entry) {
        view.Render();
        if (!view.native_preview_.HasResult() || view.native_kind_ != QuickPreviewWindow::NativeKind::Archive) return false;
        view.archive_.SetFilter(entry);
        const bool found = view.archive_.FilterHits() != 0;
        view.archive_.SetFilter(L"");
        return found;
    }
    static bool Capture(QuickPreviewWindow& view, const std::filesystem::path& path) {
        view.Render(); return view.compositor_.SaveSnapshot(path.c_str());
    }
    static bool Model(QuickPreviewWindow& view, bool click = false) {
        view.Render();
        auto* model = view.native_preview_.model_.get();
        if (!model || !model->IsOpen() || view.native_kind_ != QuickPreviewWindow::NativeKind::Model) return false;
        if (click) {
            const auto button = model->TreeNodeWindow("change");
            if (!button) return false;
            SendMessageW(button, BM_CLICK, 0, 0);
            if (model->TreeStateValue("message") != "Changed locally") return false;
        }
        RECT bounds{}, client{};
        GetWindowRect(model->Hwnd(), &bounds); MapWindowPoints(nullptr, view.hwnd(), reinterpret_cast<POINT*>(&bounds), 2);
        GetClientRect(view.hwnd(), &client);
        return bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom;
    }
    static bool Bitmap(QuickPreviewWindow& view) {
        view.Render();
        return view.native_preview_.HasResult() && view.native_kind_ == QuickPreviewWindow::NativeKind::Bitmap && view.decoded_w_ == 64;
    }
    static bool CaptureModel(QuickPreviewWindow& view, const std::filesystem::path& path) {
        return view.native_preview_.CaptureModel(path.c_str());
    }
};
}
namespace pulse::extensions::native {
namespace {
bool Wait(const std::function<bool()>& check) {
    const auto until = GetTickCount64() + 10000;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { PostQuitMessage(static_cast<int>(msg.wParam)); return false; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (check()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < until);
    return false;
}
ui::QuickPreviewItem Item(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data);
    ui::QuickPreviewItem item;
    item.path = path.wstring(); item.name = path.filename().wstring(); item.attrs = data.dwFileAttributes;
    item.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    item.modified = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return item;
}
PulseStringView View(const char* text) { return {text, static_cast<uint32_t>(strlen(text))}; }
PulseStatus PULSE_CALL Model(void*, const PulsePreviewRequest* request) {
    PulseUiModelNode children[3]{};
    for (auto& child : children) child.size = sizeof(child);
    children[0].type = PULSE_UI_MODEL_TEXT; children[0].id = View("heading"); children[0].text = {1, View("Native preview controls")};
    children[1].type = PULSE_UI_MODEL_TEXT; children[1].id = View("message"); children[1].binding = {1, View("state.message")};
    children[2].type = PULSE_UI_MODEL_BUTTON; children[2].id = View("change"); children[2].text = {1, View("Update preview")}; children[2].command = {1, View("change")};
    PulseUiModelNode root{}; root.size = sizeof(root); root.type = PULSE_UI_MODEL_STACK; root.id = View("root"); root.children = {1, children, 3};
    PulseUiState initial{View("message"), View("Plugin model with local state")};
    PulseUiState updated{View("message"), View("Changed locally")};
    PulseUiModelAction action{}; action.size = sizeof(action); action.id = View("change"); action.updates = &updated; action.update_count = 1;
    PulseUiModelTree tree{}; tree.size = sizeof(tree); tree.root = &root; tree.states = &initial; tree.state_count = 1; tree.actions = &action; tree.action_count = 1;
    PulsePreviewResult result{}; result.size = sizeof(result); result.kind = PULSE_PREVIEW_UI_MODEL; result.model = &tree;
    return request->publish(request->context, &result);
}
PulseStatus PULSE_CALL Bitmap(void*, const PulsePreviewRequest* request) {
    std::vector<uint8_t> pixels(64 * 64 * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) { pixels[i] = 210; pixels[i + 1] = 110; pixels[i + 2] = 25; pixels[i + 3] = 255; }
    PulsePreviewResult result{}; result.size = sizeof(result); result.kind = PULSE_PREVIEW_BITMAP;
    result.width = result.height = 64; result.stride = 256; result.pixels = pixels.data(); result.byte_count = static_cast<uint32_t>(pixels.size());
    return request->publish(request->context, &result);
}
}
bool RunArchivePreviewProbe(AppState& owner, const std::wstring& directory, std::ostream& report) {
    const std::filesystem::path output(directory), fixture = output.parent_path() / "fixture";
    if (!std::filesystem::exists(fixture / "sample.rar")) return true; // P1.2 command-only fixture.
    bool passed = true;
    const auto check = [&](bool okay, const char* label) {
        report << (okay ? "[PASS] " : "[FAIL] ") << label << '\n'; report.flush(); passed &= okay;
    };
    ui::QuickPreviewWindow details, quick;
    check(details.InitializeEmbedded(owner.hwnd, 0, 0, 0) && quick.Initialize(owner.hwnd, 0, 0), "preview consumers initialize");
    const auto seven = Item(fixture / "sample.7z"), rar = Item(fixture / "sample.rar");
    for (const bool dark : {false, true}) {
        details.ShowEmbedded(seven, RECT{30, 100, 390, 620}, dark, false);
        check(Wait([&] { return ui::NativePreviewProbe::Archive(details, L"hello.txt"); }), "details uses plugin 7z tree");
        check(ui::NativePreviewProbe::Capture(details, output / (dark ? "preview-details-dark.png" : "preview-details-light.png")), "details app screenshot");
        details.Update(rar);
        check(Wait([&] { return ui::NativePreviewProbe::Archive(details, L"test.txt"); }), "details uses plugin rar tree");
        details.Close();
        quick.Show(rar, dark, ui::WindowEffect::None, false);
        check(Wait([&] { return ui::NativePreviewProbe::Archive(quick, L"test.txt"); }), "quick look uses plugin rar tree");
        check(ui::NativePreviewProbe::Capture(quick, output / (dark ? "preview-quick-dark.png" : "preview-quick-light.png")), "quick look app screenshot");
        for (int i = 0; i < 20; ++i) quick.Update(i % 2 ? rar : seven);
        quick.Update(seven);
        check(Wait([&] { return ui::NativePreviewProbe::Archive(quick, L"hello.txt"); }) &&
            !ui::NativePreviewProbe::Archive(quick, L"test.txt"), "rapid switching discards old provider results");
        quick.Close();
    }
    PreviewsAdapter contributed(owner.hwnd, [] { return true; });
    PulsePreviewV1 api{}; contributed.Query(1, &api, sizeof(api));
    PulsePreviewProvider model{sizeof(model), View(".pulsemodel"), Model, nullptr};
    PulsePreviewProvider bitmap{sizeof(bitmap), View(".pulsebitmap"), Bitmap, nullptr};
    PulsePreviewProviderId model_id = 0, bitmap_id = 0;
    check(api.register_provider(api.context, &model, &model_id) == PULSE_OK &&
        api.register_provider(api.context, &bitmap, &bitmap_id) == PULSE_OK, "model and bitmap providers register");
    auto model_item = seven; model_item.path = (fixture / "sample.pulsemodel").wstring(); model_item.name = L"sample.pulsemodel";
    auto bitmap_item = seven; bitmap_item.path = (fixture / "sample.pulsebitmap").wstring(); bitmap_item.name = L"sample.pulsebitmap";
    for (const bool dark : {false, true}) {
        details.ShowEmbedded(model_item, RECT{30, 100, 350, 620}, dark, false);
        check(Wait([&] { return ui::NativePreviewProbe::Model(details); }) && ui::NativePreviewProbe::Model(details, true), "details model uses native controls and local actions within bounds");
        check(ui::NativePreviewProbe::CaptureModel(details, output / (dark ? "preview-model-dark.png" : "preview-model-light.png")), "model app screenshot");
        details.Update(bitmap_item);
        check(Wait([&] { return ui::NativePreviewProbe::Bitmap(details); }), "details bitmap replaces model");
        details.Close();
        quick.Show(model_item, dark, ui::WindowEffect::None, false);
        check(Wait([&] { return ui::NativePreviewProbe::Model(quick); }) && ui::NativePreviewProbe::Model(quick, true), "quick look model local actions");
        quick.Update(bitmap_item);
        check(Wait([&] { return ui::NativePreviewProbe::Bitmap(quick); }), "bitmap replaces model without stale controls");
        check(ui::NativePreviewProbe::Capture(quick, output / (dark ? "preview-bitmap-dark.png" : "preview-bitmap-light.png")), "bitmap app screenshot");
        quick.Close();
    }
    details.Close(); quick.Close(); contributed.Close();
    check(Wait([&] { return !contributed.CallbackActive(); }), "preview fixture callbacks drain before destruction");
    return passed;
}
}
