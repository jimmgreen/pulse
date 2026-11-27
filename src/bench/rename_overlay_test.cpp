#include "../ui/ui_renderer.h"
#include "../common/localization.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace pulse::ui {
struct RenameOverlayTest {
    static bool Init(Compositor& c) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &c.d3dDevice_, nullptr, nullptr))) return false;
        if (FAILED(c.d3dDevice_->QueryInterface(IID_PPV_ARGS(&c.dxgiDevice_)))) return false;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &c.d2dFactory_))) return false;
        if (FAILED(c.d2dFactory_->CreateDevice(c.dxgiDevice_.get(), &c.d2dDevice_))) return false;
        if (FAILED(c.d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &c.dc_))) return false;
        return SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
            reinterpret_cast<IUnknown**>(&c.dwriteFactory_)));
    }
    static std::vector<uint32_t> Read(ID2D1DeviceContext* dc, ID2D1Bitmap1* target) {
        const auto size = target->GetPixelSize();
        auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            target->GetPixelFormat());
        ComPtr<ID2D1Bitmap1> read;
        if (FAILED(dc->CreateBitmap(size, nullptr, 0, &props, &read)) ||
            FAILED(read->CopyFromBitmap(nullptr, target, nullptr))) return {};
        D2D1_MAPPED_RECT map{};
        if (FAILED(read->Map(D2D1_MAP_OPTIONS_READ, &map))) return {};
        std::vector<uint32_t> result(static_cast<size_t>(size.width) * size.height);
        for (UINT y = 0; y < size.height; ++y)
            std::copy_n(reinterpret_cast<const uint32_t*>(map.bits + y * map.pitch), size.width,
                result.data() + static_cast<size_t>(y) * size.width);
        read->Unmap();
        return result;
    }
    static bool Save(const std::filesystem::path& path, const std::vector<uint32_t>& pixels, UINT w, UINT h) {
        BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + w * h * 4;
        BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = static_cast<LONG>(w);
        info.biHeight = -static_cast<LONG>(h); info.biPlanes = 1; info.biBitCount = 32;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * 4));
        return out.good();
    }
    static int Run(const std::filesystem::path& output) {
        Compositor c;
        if (!Init(c)) { std::puts("[FAIL] offscreen WARP device"); return 2; }
        unsigned failures = 0, cases = 0;
        for (const bool dark : {false, true}) for (float scale : {1.0f, 1.5f, 2.0f})
        for (float width : {320.0f, 800.0f})
        for (auto mode : {ViewMode::Details, ViewMode::Content, ViewMode::MediumIcons, ViewMode::Tiles}) {
            c.RecreateTextFormats(scale);
            MainRenderer r;
            r.SetCompositor(&c); r.SetScale(scale);
            r.motion_now_ = 100000;
            r.rename_pop_[0].index = 0;
            const auto theme = MakeTheme(dark, D2D1::ColorF(0.0f, 0.45f, 0.85f));
            const float w = width * scale, h = 600.0f * scale;
            const auto size = D2D1::SizeU(static_cast<UINT>(w), static_cast<UINT>(h));
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ComPtr<ID2D1Bitmap1> bitmap;
            if (FAILED(c.Dc()->CreateBitmap(size, nullptr, 0, &props, &bitmap))) return 2;
            c.Dc()->SetTarget(bitmap.get());
            PaneViewModel vm;
            vm.view_mode = mode; vm.path = L"C:\\PulseRenameOverlayFixture";
            vm.entries.resize(4);
            for (auto& e : vm.entries) { e.name = L"Pulse-diagnostics-20261126-161132-26568-0.zip"; }
            vm.rename_index = 0; vm.rename_text = L"Pulse-diagnostics-20261126-161132-26568-0.doc";
            vm.rename_hint = l10n::Get(l10n::StringId::RenameExtensionConfirm); vm.rename_hint_ms = -1;
            if (vm.rename_hint.empty()) { std::puts("[FAIL] confirmation text resource is missing"); return 2; }
            vm.group_by = 5;
            auto groups = std::make_shared<ListGroups>(2);
            (*groups)[0].first = 0; (*groups)[0].count = 1; (*groups)[0].label = L"Earlier";
            (*groups)[1].first = 1; (*groups)[1].count = 3; (*groups)[1].label = L"Yesterday - group header";
            vm.groups = groups;
            const auto view = D2D1::RectF(0, 0, w, h);
            const auto field = r.RenameFieldRect(vm, view, 0);
            const float pad = 10.0f * scale;
            const float hint_w = std::min(std::max(field.right - field.left, 220.0f * scale), w - 8.0f * scale);
            ComPtr<IDWriteTextLayout> text;
            if (FAILED(c.DwriteFactory()->CreateTextLayout(vm.rename_hint.c_str(), static_cast<UINT32>(vm.rename_hint.size()),
                c.SmallFormat(), hint_w - 2 * pad, 1.0e6f, &text))) return 2;
            text->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            const DWRITE_TRIMMING untrimmed{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
            text->SetTrimming(&untrimmed, nullptr);
            DWRITE_TEXT_METRICS metrics{}; if (FAILED(text->GetMetrics(&metrics))) return 2;
            const float left = std::clamp((field.left + field.right - hint_w) * .5f, 4 * scale, std::max(4 * scale, w - 4 * scale - hint_w));
            const float top = field.bottom + 6 * scale, bottom = top + std::ceil(metrics.height) + 14 * scale;
            c.Dc()->BeginDraw(); c.Dc()->Clear(theme.bg); r.painter_.BeginFrame(theme);
            r.DrawList(vm, 0, 0, w, h, theme);
            if (FAILED(c.Dc()->EndDraw())) return 2;
            const auto actual = Read(c.Dc(), bitmap.get());
            // Oracle: a modal rename note repainted last must not change its opaque interior.
            c.Dc()->BeginDraw(); r.DrawRenameHint(vm, field, view, theme);
            if (FAILED(c.Dc()->EndDraw())) return 2;
            const auto expected = Read(c.Dc(), bitmap.get());
            if (actual.empty() || actual.size() != expected.size()) return 2;
            unsigned different = 0, compared = 0;
            const int inset = static_cast<int>(std::ceil(9 * scale));
            for (int y = static_cast<int>(std::ceil(top)) + inset; y < std::min(static_cast<int>(bottom) - inset, static_cast<int>(size.height)); ++y)
                for (int x = static_cast<int>(std::ceil(left)) + inset; x < static_cast<int>(left + hint_w) - inset; ++x) {
                    const auto at = static_cast<size_t>(y) * size.width + x;
                    ++compared; if (actual[at] != expected[at]) ++different;
                }
            ++cases;
            const bool ok = compared > 0 && different == 0;
            if (!ok) ++failures;
            std::printf("[%s] mode=%d dark=%d scale=%.1f width=%.0f compared=%u overwritten=%u\n",
                ok ? "PASS" : "FAIL", static_cast<int>(mode), dark, scale, width, compared, different);
            if (scale == 1.5f && width == 800 && mode == ViewMode::Details) {
                if (!Save(output / (dark ? "dark-actual.bmp" : "light-actual.bmp"), actual, size.width, size.height) ||
                    !Save(output / (dark ? "dark-oracle.bmp" : "light-oracle.bmp"), expected, size.width, size.height)) return 2;
            }
            c.Dc()->SetTarget(nullptr);
        }
        std::printf("RESULT cases=%u failures=%u\n", cases, failures);
        return failures ? 1 : 0;
    }
};
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path output(argv[1]); std::filesystem::create_directories(output);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const int result = pulse::ui::RenameOverlayTest::Run(output);
    CoUninitialize(); return result;
}
