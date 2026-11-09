#include "../common/localization.h"
#include "../ui/FluentTokens.h"
#include "../ui/extension_tree.h"
#include "../ui/fluent_components.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using namespace pulse;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}
bool SavePng(const std::filesystem::path& path, UINT width, UINT height, std::vector<unsigned char> pixels) {
    for (size_t i = 3; i < pixels.size(); i += 4) {
        pixels[i] = 255;
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height))) {
        return false;
    }
    auto format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(
               frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), pixels.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}
bool Reference(HWND window, int width, int height, float scale, bool dark, D2D1_COLOR_F accent,
               std::vector<unsigned char>& pixels) {
    ui::Compositor compositor;
    if (!compositor.Init(window)) {
        return false;
    }
    compositor.Resize(width, height);
    compositor.RecreateTextFormats(scale);
    ui::fluent::Painter painter(&compositor);
    painter.SetScale(scale);
    auto* dc = compositor.Dc();
    const auto theme = ui::MakeTheme(dark, accent);
    dc->BeginDraw();
    dc->Clear(theme.bg);
    if (!painter.BeginFrame(theme)) {
        return false;
    }
    const auto bounds = [&](int row) {
        const float top = 8.0f + static_cast<float>(row) * 44.0f * scale;
        return D2D1::RectF(8, top, static_cast<float>(width - 8), top + 32 * scale);
    };
    ui::fluent::ButtonSpec button;
    button.bounds = bounds(0);
    button.text = L"Primary action";
    button.kind = ui::fluent::ButtonKind::Primary;
    painter.DrawButton(button);
    ui::fluent::ControlState checked;
    checked.checked = true;
    painter.DrawCheckBox(bounds(1), L"Check option", checked);
    painter.DrawSwitch(bounds(2), L"Switch option", checked);
    ui::fluent::ButtonSpec select;
    select.bounds = bounds(3);
    select.text = L"First choice";
    select.drop_down = true;
    painter.DrawButton(select);
    ui::fluent::ProgressSpec progress;
    progress.bounds = bounds(4);
    progress.value = 0.75f;
    painter.DrawProgressBar(progress);
    if (FAILED(dc->EndDraw())) {
        return false;
    }
    compositor.CaptureNextFrame();
    if (FAILED(compositor.Present()) || !compositor.HeldFrame()) {
        return false;
    }
    auto* captured = compositor.HeldFrame();
    ComPtr<ID2D1Bitmap1> readable;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                         captured->GetPixelFormat(), 96, 96);
    if (FAILED(dc->CreateBitmap(captured->GetPixelSize(), nullptr, 0, &props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, captured, nullptr))) {
        return false;
    }
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
        return false;
    }
    pixels.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                    mapped.bits + static_cast<size_t>(y) * mapped.pitch, width * 4);
    }
    readable->Unmap();
    return true;
}
void Run(const std::filesystem::path& output, bool dark, float scale) {
    const int width = static_cast<int>(480 * scale), height = static_cast<int>(320 * scale);
    HWND parent = CreateWindowExW(0, L"STATIC", L"Identity actual", WS_POPUP, 0, 0, width, height, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND reference_window = CreateWindowExW(0, L"STATIC", L"Identity reference", WS_POPUP, 0, 0, width,
                                            height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent || !reference_window) {
        Check(false, "hidden windows created");
        return;
    }
    extensions::UiTree model;
    model.root.id = "root";
    auto add = [&](extensions::UiNodeType type, const char* id, const char* text) -> extensions::UiNode& {
        extensions::UiNode node;
        node.type = type;
        node.id = id;
        node.text = text;
        model.root.children.push_back(std::move(node));
        return model.root.children.back();
    };
    add(extensions::UiNodeType::Button, "button", "Primary action").button_kind = "primary";
    add(extensions::UiNodeType::Checkbox, "check", "Check option").checked = true;
    add(extensions::UiNodeType::Switch, "switch", "Switch option").checked = true;
    add(extensions::UiNodeType::Select, "select", "Choice").options = {"First choice", "Second choice"};
    add(extensions::UiNodeType::Progress, "progress", "").value = 75;
    const auto accent = D2D1::ColorF(0.1f, 0.4f, 0.8f);
    ui::ExtensionTreeView tree;
    bool created = tree.Create(parent, model, {}, dark, accent, scale);
    Check(created, "tree compositor created");
    if (created) {
        tree.SetBounds({0, 0, width, height});
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC target = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(target, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ previous = bitmap ? SelectObject(target, bitmap) : nullptr;
        const bool painted = bitmap && tree.PaintTo(target, 0, 0);
        Check(painted, "actual tree PaintTo succeeds");
        if (painted) {
            const auto* begin = static_cast<unsigned char*>(bits);
            std::vector<unsigned char> actual(begin, begin + static_cast<size_t>(width) * height * 4),
                expected;
            const bool rendered = Reference(reference_window, width, height, scale, dark, accent, expected);
            Check(rendered, "independent Fluent Painter reference succeeds");
            if (rendered) {
                size_t different = 0;
                for (size_t i = 0; i < actual.size(); ++i) {
                    if (i % 4 != 3 && actual[i] != expected[i]) {
                        ++different;
                    }
                }
                std::printf("theme=%s scale=%.1f different RGB channels=%zu\n", dark ? "dark" : "light",
                            scale, different);
                Check(different == 0, "exact RGB identity (alpha ignored for GDI DIB transfer)");
                const auto stem =
                    std::string(dark ? "dark-" : "light-") + std::to_string(static_cast<int>(scale * 100));
                Check(SavePng(output / (stem + "-actual.png"), width, height, actual), "save actual PNG");
                Check(SavePng(output / (stem + "-reference.png"), width, height, expected),
                      "save reference PNG");
            }
        }
        if (previous) {
            SelectObject(target, previous);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        if (target) {
            DeleteDC(target);
        }
    }
    tree.Close();
    DestroyWindow(reference_window);
    DestroyWindow(parent);
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || !std::filesystem::path(argv[1]).is_absolute()) {
        return 2;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        return 2;
    }
    l10n::Initialize(GetModuleHandleW(nullptr), L"en");
    std::error_code error;
    const bool created = std::filesystem::create_directory(argv[1], error);
    if (!created || error) {
        CoUninitialize();
        return 2;
    }
    for (bool dark : {false, true}) {
        for (float scale : {1.0f, 1.5f, 2.0f}) {
            Run(argv[1], dark, scale);
        }
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
