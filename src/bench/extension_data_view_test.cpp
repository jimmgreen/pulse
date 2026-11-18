#include "../ui/extension_data_view.h"
#include "../common/localization.h"
#include <cstdio>
#include <filesystem>
#include <wrl/client.h>
using namespace pulse;
using Microsoft::WRL::ComPtr;
namespace {
int failures = 0;
void Check(bool value, const char* message) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", message);
    if (!value) ++failures;
}
bool Capture(ui::Compositor& compositor, const std::filesystem::path& path) {
    compositor.CaptureNextFrame();
    if (FAILED(compositor.Present()) || !compositor.HeldFrame()) return false;
    auto* frame = compositor.HeldFrame();
    const auto size = frame->GetPixelSize();
    ComPtr<ID2D1Bitmap1> readable;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        frame->GetPixelFormat(), 96, 96);
    if (FAILED(compositor.Dc()->CreateBitmap(size, nullptr, 0, &props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, frame, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> output;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&output, nullptr)) && SUCCEEDED(output->Initialize(nullptr)) &&
        SUCCEEDED(output->SetSize(size.width, size.height));
    auto format = GUID_WICPixelFormat32bppPBGRA;
    ok = ok && SUCCEEDED(output->SetPixelFormat(&format)) &&
        SUCCEEDED(output->WritePixels(size.height, mapped.pitch, mapped.pitch * size.height, mapped.bits)) &&
        SUCCEEDED(output->Commit()) && SUCCEEDED(encoder->Commit());
    readable->Unmap();
    return ok;
}
extensions::UiNode Rows(extensions::UiNodeType type) {
    extensions::UiNode node;
    node.type = type;
    node.id = "data";
    for (int i = 0; i < 4096; ++i) {
        extensions::UiDataRow row;
        row.id = "row-" + std::to_string(i);
        row.text = "Item " + std::to_string(i);
        row.cells = {row.text, i % 2 ? "Ready" : "Review", std::to_string(i)};
        row.command = "inspect";
        node.rows.push_back(std::move(row));
    }
    if (type == extensions::UiNodeType::DataTable)
        node.data_columns = {{"name", "Name", 240}, {"status", "Status", 160}, {"size", "Count", 100}};
    return node;
}
void Interactions() {
    ui::ExtensionDataView view;
    auto node = Rows(extensions::UiNodeType::VirtualList);
    node.rows[1].enabled = false;
    view.Configure(node);
    view.SetViewport(D2D1::RectF(0, 0, 320, 240), 1);
    Check(view.VisibleRowCount() == 4096, "large dataset contains 4096 model rows");
    Check(view.SetSelection("row-0"), "select first row by stable identity");
    Check(view.KeyDown(VK_DOWN).selection_changed && view.Selection() == "row-2", "keyboard skips disabled row");
    Check(!view.SetSelection("row-1") && view.Selection() == "row-2", "disabled row rejects programmatic selection");
    Check(!view.SetSelection("missing") && view.Selection() == "row-2", "unknown row leaves selection intact");
    Check(view.KeyDown(VK_END).selection_changed && view.Selection() == "row-4095", "End selects final row");
    Check(view.ScrollOffset() > 130000, "virtual scrolling reveals final row without per-row windows");
    Check(view.KeyDown(VK_RETURN).command == "inspect", "activation returns declared command for host authorization");
    view.Wheel(120);
    const auto scrolled = view.ScrollOffset();
    view.SetSelection("row-4095");
    Check(view.ScrollOffset() == scrolled, "unchanged state refresh does not snap independently scrolled viewport");
    const auto before = view.ScrollOffset();
    view.SetViewport(D2D1::RectF(0, 0, 480, 360), 1.5f);
    Check(view.ScrollOffset() > before && view.Selection() == "row-4095", "DPI change keeps stable selection and proportional scroll");
    view.KeyDown(VK_HOME);
    view.PointerDown(470, 80);
    Check(view.Dragging(), "shared scrollbar track starts drag");
    view.PointerMove(470, 350);
    Check(view.ScrollOffset() > 100000, "shared scrollbar drag reaches lower dataset");
    view.PointerUp();
    Check(!view.Dragging(), "pointer release ends scrollbar drag");
    view.Configure(Rows(extensions::UiNodeType::DataTable));
    view.SetViewport(D2D1::RectF(0, 0, 320, 240), 1);
    view.PointerDown(20, 16);
    view.KeyDown(VK_HOME);
    Check(view.Selection() == "row-0", "table header sorts ascending text cells");
    view.PointerDown(20, 16);
    view.KeyDown(VK_HOME);
    Check(view.Selection() == "row-999", "second header click sorts descending with stable row identity");
    Check(view.Wheel(-120, true), "wide table supports horizontal scrolling");
    auto textless = Rows(extensions::UiNodeType::DataTable);
    textless.rows.front().text.clear();
    view.Configure(textless);
    view.SetSelection("row-0");
    Check(view.SelectedText() == L"Item 0", "table selected accessible label falls back to first cell");
    extensions::UiNode tree;
    tree.type = extensions::UiNodeType::Tree;
    extensions::UiDataRow parent;
    parent.id = "parent"; parent.text = "Folder"; parent.expanded = false;
    extensions::UiDataRow child;
    child.id = "child"; child.text = "Document"; child.command = "open";
    parent.children.push_back(child);
    tree.rows.push_back(parent);
    view.Configure(tree);
    Check(view.VisibleRowCount() == 1, "collapsed tree initially omits descendants");
    Check(view.SetSelection("child") && view.VisibleRowCount() == 2, "select hidden descendant expands ancestors");
    view.KeyDown(VK_LEFT);
    Check(view.Selection() == "parent", "tree left arrow selects parent");
    view.KeyDown(VK_LEFT);
    Check(view.VisibleRowCount() == 1, "tree left arrow collapses branch");
    view.KeyDown(VK_RIGHT);
    view.KeyDown(VK_RIGHT);
    Check(view.Selection() == "child" && view.KeyDown(VK_RETURN).command == "open", "tree right arrow opens branch and activates child");
    tree.rows.front().children.front().enabled = false;
    auto enabled_child = child;
    enabled_child.id = "enabled-child";
    tree.rows.front().children.push_back(enabled_child);
    view.Configure(tree);
    view.SetSelection("parent");
    view.KeyDown(VK_RIGHT);
    view.KeyDown(VK_RIGHT);
    Check(view.Selection() == "enabled-child", "tree right arrow skips disabled first descendant");
    view.Configure(extensions::UiNode{});
    Check(view.VisibleRowCount() == 0 && view.Selection().empty() && view.KeyDown(VK_END).handled,
        "replacement with empty dataset clears stale identities safely");
}
void Render(const std::filesystem::path& output, bool dark, float scale) {
    const int width = static_cast<int>(640 * scale), height = static_cast<int>(360 * scale);
    HWND window = CreateWindowExW(0, L"STATIC", L"Virtual data view", WS_POPUP, 0, 0, width, height,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        ui::Compositor compositor;
        const bool ready = window && compositor.Init(window);
        Check(ready, "data view uses actual Pulse compositor");
        if (ready) {
            compositor.Resize(width, height);
            compositor.RecreateTextFormats(scale);
            ui::fluent::Painter painter(&compositor);
            painter.SetScale(scale);
            const auto theme = ui::MakeTheme(dark, D2D1::ColorF(0.1f, 0.4f, 0.8f));
            ui::ExtensionDataView view;
            view.Configure(Rows(extensions::UiNodeType::DataTable));
            view.SetViewport(D2D1::RectF(16 * scale, 16 * scale, width - 16 * scale, height - 16 * scale), scale);
            view.SetSelection("row-2000");
            auto* dc = compositor.Dc();
            dc->BeginDraw();
            dc->Clear(theme.bg);
            painter.BeginFrame(theme);
            ui::fluent::ControlState state;
            state.focused = state.keyboard_focus = true;
            view.Draw(painter, compositor, theme, state);
            Check(SUCCEEDED(dc->EndDraw()), "virtual rows render with existing ListRow and ColumnHeader specs");
            Check(view.LastDrawnRowCount() > 0 && view.LastDrawnRowCount() <= 11,
                "4096-row model paints only visible viewport rows");
            const auto name = std::string(dark ? "data-dark-" : "data-light-") + std::to_string(static_cast<int>(scale * 100)) + ".png";
            Check(Capture(compositor, output / name), "capture actual application-rendered data viewport");
            if (dark && scale == 1.0f) {
                compositor.NotifyDeviceLost(DXGI_ERROR_DEVICE_RESET);
                painter.SetCompositor(nullptr);
                Check(compositor.NeedsRecovery() && compositor.Recover(), "recover actual compositor after injected device reset");
                painter.SetCompositor(&compositor);
                compositor.RecreateTextFormats(scale);
                painter.InvalidateTypography();
                const auto high_contrast = ui::MakeHighContrastTheme();
                auto* recovered = compositor.Dc();
                if (recovered) {
                    recovered->BeginDraw();
                    recovered->Clear(high_contrast.bg);
                    painter.BeginFrame(high_contrast, true);
                    view.Draw(painter, compositor, high_contrast, state);
                    const auto draw_result = recovered->EndDraw();
                    if (FAILED(draw_result)) std::printf("recovered draw HRESULT=0x%08lX\n", static_cast<unsigned long>(draw_result));
                    Check(SUCCEEDED(draw_result), "shared high-contrast rendering survives compositor recovery");
                    Check(view.Selection() == "row-2000", "graphics recovery preserves local selection model");
                    Check(Capture(compositor, output / "data-high-contrast-recovered.png"), "capture recovered high-contrast data view");
                } else Check(false, "recovered compositor exposes drawing context");
            }
        }
    }
    if (window) DestroyWindow(window);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path output(argv[1]);
    std::error_code error;
    if (!output.is_absolute() || !std::filesystem::create_directory(output, error)) return 2;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    Interactions();
    for (bool dark : {false, true}) for (float scale : {1.0f, 1.5f, 2.0f}) Render(output, dark, scale);
    CoUninitialize();
    return failures ? 1 : 0;
}
