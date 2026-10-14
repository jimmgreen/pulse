#include "../ui/fluent_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<BYTE> bgra;

    const BYTE* Pixel(int x, int y) const {
        return bgra.data() + (static_cast<size_t>(y) * width + x) * 4;
    }
};

int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

pulse::MenuIcon CircleIcon(bool amber = false) {
    pulse::MenuIcon icon;
    icon.width = icon.height = 32;
    icon.bgra.resize(32 * 32 * 4);
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - 16.0f;
            const float dy = static_cast<float>(y) + 0.5f - 16.0f;
            const float coverage = std::clamp(14.0f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            BYTE* pixel = icon.bgra.data() + (static_cast<size_t>(y) * 32 + x) * 4;
            const int alpha = static_cast<int>(std::lround(coverage * 255.0f));
            pixel[0] = static_cast<BYTE>((amber ? 30 : 220) * alpha / 255);
            pixel[1] = static_cast<BYTE>((amber ? 150 : 110) * alpha / 255);
            pixel[2] = static_cast<BYTE>((amber ? 240 : 30) * alpha / 255);
            pixel[3] = static_cast<BYTE>(alpha);
        }
    }
    return icon;
}

std::vector<pulse::ui::FluentMenuItem> MenuItems() {
    using pulse::ui::FluentMenuItem;
    FluentMenuItem parent{ 1, L"7-Zip archive tools" };
    parent.icon = CircleIcon(true);
    FluentMenuItem child{ 11, L"Extract here / 解压到此处" };
    child.icon = CircleIcon(true);
    FluentMenuItem disabled_child{ 12, L"Test archive (unavailable)" };
    disabled_child.icon = child.icon;
    disabled_child.enabled = false;
    parent.children = { child, disabled_child, {13, L"Command without an icon"} };
    FluentMenuItem enabled{ 2, L"Open with editor / 编辑" };
    enabled.icon = CircleIcon();
    FluentMenuItem disabled{ 3, L"Open with editor (disabled)" };
    disabled.icon = enabled.icon;
    disabled.enabled = false;
    FluentMenuItem shortcut{ 4, L"Editor with shortcut" };
    shortcut.icon = CircleIcon(true);
    shortcut.shortcut = L"Ctrl+E";
    return { parent, enabled, disabled, shortcut, {5, L"Command without an icon"} };
}

bool SavePng(const std::wstring& path, const Image& image) {
    using pulse::ui::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height))))
        return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(image.width),
        static_cast<UINT>(image.height), GUID_WICPixelFormat32bppPBGRA,
        static_cast<UINT>(image.width * 4), static_cast<UINT>(image.bgra.size()),
        const_cast<BYTE*>(image.bgra.data()), &bitmap))) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(bitmap.get(), format, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->WriteSource(converter.get(), nullptr)) || FAILED(frame->Commit()))
        return false;
    return SUCCEEDED(encoder->Commit());
}

void TestModelIdentity() {
    using namespace pulse::ui;
    FluentMenuItem item{ 1, L"Editor" };
    FluentMenuModel model;
    model.SetItems({item});
    item.icon = CircleIcon();
    Check(!model.PatchCommands({item}) && !model.At(0)->icon.Valid(),
          "adding an icon rejects the command-only patch and preserves the visible snapshot");
    model.SetItems({item});
    item.command = 2;
    Check(model.PatchCommands({item}) && model.At(0)->command == 2,
          "unchanged icon pixels allow a command-only patch");
    item.icon = CircleIcon(true);
    Check(!model.PatchCommands({item}) && model.At(0)->icon != item.icon,
          "changed icon pixels reject the command-only patch");
    item.children = {{3, L"Child"}};
    model.SetItems({item});
    item.children[0].icon = CircleIcon();
    Check(!model.PatchCommands({item}), "changed child icons reject the command-only patch");
    item.children.clear();
    model.SetItems({item});
    item.icon = {};
    Check(!model.PatchCommands({item}), "removing an icon rejects the command-only patch");
}

} // namespace

namespace pulse::ui {

struct FluentMenuTestPeer {
    static Image Copy(const FluentMenu::Surface& surface) {
        Image image;
        image.width = surface.w;
        image.height = surface.h;
        image.bgra.resize(static_cast<size_t>(surface.w) * surface.h * 4);
        std::memcpy(image.bgra.data(), surface.bits, image.bgra.size());
        return image;
    }

    static bool RenderMain(FluentMenu& menu, const std::vector<FluentMenuItem>& items,
                           Image& image) {
        menu.model_.SetItems(items);
        menu.model_.Layout(menu.compositor_->DwriteFactory(), menu.scale_);
        if (!menu.RenderSurface(menu.model_, 0, -1, 0.0f, false, menu.surf_)) return false;
        image = Copy(menu.surf_);
        return true;
    }

    static bool CacheChanges(FluentMenu& menu) {
        FluentMenuItem item{ 1, L"Editor cache" };
        Image image;
        if (!RenderMain(menu, {item}, image) || menu.surf_.icons.size() != 1 ||
            menu.surf_.icons[0].bitmap.get()) return false;
        item.icon = CircleIcon();
        if (!RenderMain(menu, {item}, image) || !menu.surf_.icons[0].bitmap.get() ||
            menu.surf_.icons[0].pixels != item.icon) return false;
        ComPtr<ID2D1Bitmap1> original;
        original.p = menu.surf_.icons[0].bitmap.get();
        original->AddRef();
        if (!RenderMain(menu, {item}, image) || menu.surf_.icons[0].bitmap.get() != original.get())
            return false;
        item.icon = CircleIcon(true);
        if (!RenderMain(menu, {item}, image) || !menu.surf_.icons[0].bitmap.get() ||
            menu.surf_.icons[0].bitmap.get() == original.get() ||
            menu.surf_.icons[0].pixels != item.icon) return false;
        item.icon = {};
        return RenderMain(menu, {item}, image) && !menu.surf_.icons[0].bitmap.get() &&
            !menu.surf_.icons[0].pixels.Valid();
    }

    static bool Snapshot(FluentMenu& menu, const std::wstring& path) {
        auto items = MenuItems();
        Image main;
        if (!RenderMain(menu, items, main)) return false;
        menu.sub_model_.SetItems(items[0].children);
        menu.sub_model_.Layout(menu.compositor_->DwriteFactory(), menu.scale_);
        if (!menu.RenderSub()) return false;
        const Image child = Copy(menu.sub_surf_);
        const int gap = static_cast<int>(std::lround(8.0f * menu.scale_));
        Image combined;
        combined.width = main.width + child.width + gap;
        combined.height = std::max(main.height, child.height);
        combined.bgra.resize(static_cast<size_t>(combined.width) * combined.height * 4);
        for (const auto& part : {std::pair<const Image*, int>{ &main, 0 },
                                 { &child, main.width + gap }}) {
            for (int y = 0; y < part.first->height; ++y)
                std::memcpy(combined.bgra.data() +
                    (static_cast<size_t>(y) * combined.width + part.second) * 4,
                    part.first->Pixel(0, y), static_cast<size_t>(part.first->width) * 4);
        }
        if (!SavePng(path, combined)) return false;

        for (auto& item : items) item.icon = {};
        Image baseline;
        if (!RenderMain(menu, items, baseline)) return false;
        const float scale = menu.scale_;
        const int left = static_cast<int>(std::floor(FluentMenu::kShadowMargin + 16.0f * scale));
        const int right = static_cast<int>(std::ceil(FluentMenu::kShadowMargin + 36.0f * scale));
        bool within_slot = true;
        int changed_pixels = 0;
        for (int row : {1, 2}) {
            const int top = FluentMenu::kShadowMargin +
                static_cast<int>(std::lround(menu.model_.RowTopPx(row)));
            const int bottom = top + static_cast<int>(std::lround(menu.model_.RowHeightPx()));
            for (int y = top; y < bottom; ++y) {
                for (int x = 0; x < main.width; ++x) {
                    if (std::memcmp(main.Pixel(x, y), baseline.Pixel(x, y), 4) == 0) continue;
                    ++changed_pixels;
                    within_slot &= x >= left - 1 && x <= right + 1;
                }
            }
        }
        Check(within_slot && changed_pixels > 20,
              "actual menu icon pixels stay inside the existing 20-DIP column without touching text");
        const int x = static_cast<int>(std::lround(FluentMenu::kShadowMargin + 26.0f * scale));
        const auto contrast = [&](int row) {
            const int y = FluentMenu::kShadowMargin + static_cast<int>(std::lround(
                menu.model_.RowTopPx(row) + menu.model_.RowHeightPx() * 0.5f));
            const BYTE* actual = main.Pixel(x, y);
            const BYTE* blank = baseline.Pixel(x, y);
            int sum = 0;
            for (int channel = 0; channel < 3; ++channel)
                sum += std::abs(static_cast<int>(actual[channel]) - blank[channel]);
            return sum;
        };
        const int enabled = contrast(1), disabled = contrast(2);
        Check(enabled > 0 && disabled > enabled * 0.30f && disabled < enabled * 0.50f,
              "disabled bitmap contrast follows the renderer's 40-percent opacity");
        return true;
    }
};

} // namespace pulse::ui

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ui;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    const HWND foreground = GetForegroundWindow();
    const std::wstring directory = argc == 2 ? argv[1] : L"bench_data";
    CreateDirectoryW(directory.c_str(), nullptr);
    TestModelIdentity();
    HWND owner = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, -30000, -30000, 700, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        const bool initialized = owner && compositor.Init(owner);
        Check(initialized, "initialize hidden menu rendering compositor");
        if (initialized) {
            compositor.RecreateTextFormats(1.0f);
            FluentMenu cache_menu;
            Check(cache_menu.Create(owner, &compositor, 1.0f) &&
                  FluentMenuTestPeer::CacheChanges(cache_menu),
                  "render cache creates, reuses, refreshes and clears icon bitmaps");
            struct Case { bool dark; float scale; const wchar_t* filename; };
            for (const auto& test : {Case{false, 1.0f, L"menu_icons_light.png"},
                                    Case{true, 1.0f, L"menu_icons_dark.png"},
                                    Case{false, 1.5f, L"menu_icons_DPI150.png"}}) {
                compositor.RecreateTextFormats(test.scale);
                FluentMenu menu;
                menu.SetTheme(test.dark, D2D1::ColorF(0.2f, 0.4f, 0.8f));
                const std::wstring path = directory + L"\\" + test.filename;
                Check(menu.Create(owner, &compositor, test.scale) &&
                      FluentMenuTestPeer::Snapshot(menu, path),
                      "save actual main menu and child menu render snapshot");
                std::wprintf(L"  %s\n", path.c_str());
            }
        }
    }
    if (owner) DestroyWindow(owner);
    Check(GetForegroundWindow() == foreground, "hidden rendering tests preserve the foreground window");
    CoUninitialize();
    return failures ? 1 : 0;
}
