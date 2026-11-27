// settings_render_test.cpp - Paints the 桌面整理 settings page the way Pulse's settings host does: the page builds
// its tree against the settings_page_test fake host, Pulse's ConvertUiModel validates it, and Pulse's own
// ExtensionTreeView draws it. Writes one PNG per 小组件 tab and theme into the given (absolute, new) folder.
#define main settings_page_test_main
#include "settings_page_test.cpp"
#undef main
#include "../../src/ui/extension_tree.h"
#include "ipc/desktop_ipc.h"
#include <filesystem>
#include <wincodec.h>
#include <wrl/client.h>

namespace {
bool SavePng(const std::filesystem::path& path, UINT width, UINT height, std::vector<unsigned char> pixels) {
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height)))
        return false;
    auto format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), pixels.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// The page's content stack without the scroll view, so the whole page fits one picture.
bool Paint(const pulse::extensions::UiTree& page, const std::filesystem::path& out, bool dark, float scale, int width_dip) {
    pulse::extensions::UiTree tree = page;
    if (tree.root.type == pulse::extensions::UiNodeType::ScrollView && tree.root.children.size() == 1) {
        auto content = tree.root.children[0];
        tree.root = std::move(content);
    }
    const int width = static_cast<int>(width_dip * scale);
    int height = static_cast<int>(3200 * scale);
    HWND parent = CreateWindowExW(0, L"STATIC", L"settings render", WS_POPUP, 0, 0, width, height, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (!parent) return false;
    pulse::ui::ExtensionTreeView view;
    pulse::ui::ExtensionTreeCallbacks callbacks;  // as the settings host: every declared command is live
    callbacks.on_command = [](const std::string&) {};
    callbacks.command_enabled = [](const std::string&) { return true; };
    bool ok = view.Create(parent, tree, callbacks, dark, D2D1::ColorF(0x0A84FF), scale);
    if (ok) {
        view.SetBounds({0, 0, width, height});
        height = std::min(height, view.ContentHeight() + 2 * static_cast<int>(pulse::ui::kExtensionTreeContentInset * scale));
        SetWindowPos(parent, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        view.SetBounds({0, 0, width, height});
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ prev = bmp ? SelectObject(dc, bmp) : nullptr;
        ok = bmp && view.PaintTo(dc, 0, 0);
        if (ok) {
            const auto* begin = static_cast<unsigned char*>(bits);
            ok = SavePng(out, width, height, std::vector<unsigned char>(begin, begin + static_cast<size_t>(width) * height * 4));
        }
        if (prev) SelectObject(dc, prev);
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc);
    }
    view.Close();
    DestroyWindow(parent);
    return ok;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || !std::filesystem::path(argv[1]).is_absolute()) return 2;
    const std::filesystem::path dir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    // argv[2] == "tint": 着色 with a picked swatch and three cities (shows the swatches and 添加城市).
    const bool tint = argc >= 3 && std::wstring(argv[2]) == L"tint";
    const std::string prefs = "{}";
    int failed = 0;
    for (bool dark : {false, true}) {
        for (int tab : {0, 1, 3, 6}) {
            Host h;
            PulseHostV1 host{sizeof(PulseHostV1), 1, &h, Host::Query};
            pulse::desktop::plugin::DesktopSettingsPage page;
            if (page.Open(&host, {}, {}) != PULSE_OK) { ++failed; continue; }
            h.Load(prefs.c_str(), 1);
            const auto t = std::to_string(tab);
            if (tab) h.SelectKey("w_tab", t.c_str());
            if (tint) { h.SelectKey("w_scheme_3", "true"); h.Command("w-tint-3"); h.Command("w-city-del-3"); }
            if (!g_validator_errors.empty()) {
                std::printf("[FAIL] validator: %s\n", g_validator_errors.back().c_str());
                ++failed;
                continue;
            }
            auto tree = g_last_ui_tree;
            for (auto& e : tree.state) {
                const auto at = h.all_states.find(e.key);
                if (at != h.all_states.end()) e.value = at->second;
            }
            const auto name = std::string(dark ? "dark" : "light") + "-tab" + t + ".png";
            const bool ok = Paint(tree, dir / name, dark, 1.0f, 760);
            std::printf("[%s] render %s\n", ok ? "PASS" : "FAIL", name.c_str());
            failed += !ok;
            page.Close();
        }
    }
    CoUninitialize();
    return failed ? 1 : 0;
}
