// Focused offscreen regression for the production icon-name layout.
#include "../ui/icon_name_layout.h"
#include "../ui/view_layout.h"
#include "../ui/lumatext_renderer.h"
#include <filesystem>
#include <cstdio>

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", message);
    failures += !value;
}

bool Capture(pulse::ui::Compositor& compositor, ID2D1Bitmap1* bitmap, const wchar_t* file) {
    using namespace pulse::ui;
    auto* dc = compositor.Dc();
    ComPtr<ID2D1Bitmap1> readable;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        bitmap->GetPixelFormat());
    const auto size = bitmap->GetPixelSize();
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    auto pixel_format = GUID_WICPixelFormat32bppBGRA;
    const bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file, GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(size.width, size.height)) && SUCCEEDED(frame->SetPixelFormat(&pixel_format)) &&
        SUCCEEDED(frame->WritePixels(size.height, mapped.pitch, mapped.pitch * size.height, mapped.bits)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    readable->Unmap();
    return ok;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ui;
    using namespace pulse::ui::icon_name;
    if (argc != 2) return 2;
    const std::filesystem::path output = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(output);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Folder grid probe",
        WS_POPUP, -30000, -30000, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        if (!window || !compositor.Init(window)) {
            Check(false, "initialize isolated compositor");
            return 2;
        }
        std::vector<int> one(8, 1), two(8, 2), three(8, 3), many(8, 8), files(8, 1);
        Check(AutoIconNameLines(one, true) == 3, "short folders keep two name slots plus their size slot");
        const int total = AutoIconNameLines(two, true);
        std::printf("REGRESSION name_needed=2 folder_size_slots=1 block_lines=%d available_name_lines=%d\n", total, total - 1);
        Check(total == 3, "two-line folder names do not donate their second line to size");
        Check(AutoIconNameLines(three, true) == 4, "three-line folder names plus size fit the existing maximum");
        Check(AutoIconNameLines(many, true) == 4, "very long names keep the four-slot cap and selected expansion");
        Check(AutoIconNameLines(files, false) == 1, "file-only short-name density stays unchanged");

        auto* dc = compositor.Dc();
        ComPtr<ID2D1Image> original;
        dc->GetTarget(&original);
        for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
            compositor.RecreateTextFormats(scale);
            auto* format = compositor.FileNameFormat();
            for (bool luma : {false, true}) {
                typography::SetTextRenderMode(luma ? typography::TextRenderMode::Auto : typography::TextRenderMode::Sharp);
                Check(compositor.LumaTextEnabled() == luma, "exercise the requested real text backend");
                for (bool dark : {false, true}) {
                    ComPtr<ID2D1Bitmap1> target;
                    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT>(650 * scale),
                        static_cast<UINT>(370 * scale)), nullptr, 0, props, &target))) return 2;
                    dc->SetTarget(target.get());
                    const auto luma_draws_before = compositor.GetLumaTextStats()->draw_calls;
                    dc->BeginDraw();
                    const auto background = D2D1::ColorF(dark ? 0x20252E : 0xFAFAFA);
                    const auto foreground = D2D1::ColorF(dark ? 0xF4F4F4 : 0x202020);
                    dc->Clear(background);
                    ComPtr<ID2D1SolidColorBrush> ink, fill;
                    dc->CreateSolidColorBrush(foreground, &ink);
                    dc->CreateSolidColorBrush(D2D1::ColorF(dark ? 0x384B65 : 0xDAE9FC), &fill);
                    const std::wstring names[] = {L"不锈钢真空双层保温壶", L"带茶漏便携商务保温杯", L"儿童水杯(可爱小兔子)",
                        L"产品说明_保温杯_2026.pdf", L"这是一个非常非常长的文件夹名称用于检验选中后的完整名称展开"};
                    for (int i = 0; i < 5; ++i) {
                        const bool folder = i != 3;
                        const bool expanded = i == 4;
                        const float left = (12.0f + i * 126.0f) * scale;
                        const float top = 20 * scale;
                        const float width = (expanded ? 116.0f : 104.0f) * scale;
                        std::vector<int> needed(8, 2);
                        const int slots = AutoIconNameLines(needed, true);
                        const int allowed = expanded ? 6 : slots - (folder ? 1 : 0);
                        const auto fit = LayoutIconName(&compositor, compositor.DwriteFactory(), format, names[i], width, allowed);
                        Check(!fit.truncated, "fitting CJK, punctuation, file suffix and selected names remain complete");
                        const auto name_rect = D2D1::RectF(left, top + 92 * scale, left + width,
                            top + (92 + allowed * 18 + 2) * scale);
                        if (i == 1 || expanded) dc->FillRectangle(name_rect, fill.get());
                        // A simple folder silhouette marks this owned fixture; text below
                        // follows the production DirectWrite layout + compositor draw path.
                        dc->FillRectangle(D2D1::RectF(left + 8 * scale, top + 12 * scale,
                            left + 88 * scale, top + 80 * scale), fill.get());
                        ComPtr<IDWriteTextLayout> layout;
                        compositor.DwriteFactory()->CreateTextLayout(fit.shown.c_str(), static_cast<UINT32>(fit.shown.size()),
                            format, width, name_rect.bottom - name_rect.top, &layout);
                        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                        const DWRITE_TRIMMING untrimmed{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
                        layout->SetTrimming(&untrimmed, nullptr);
                        DWRITE_TEXT_METRICS metrics{};
                        layout->GetMetrics(&metrics);
                        Check(metrics.lineCount <= static_cast<UINT>(allowed) && metrics.width <= width + 0.1f,
                            "real DirectWrite line count and horizontal bounds fit the allotted name slots");
                        compositor.DrawTextLayout({name_rect.left, name_rect.top}, layout.get(), ink.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                        if (folder) {
                            const float size_y = name_rect.top + allowed * kIconGridNameLineDip * scale;
                            DWRITE_OVERHANG_METRICS overhang{};
                            layout->GetOverhangMetrics(&overhang);
                            const float ink_bottom = name_rect.bottom + overhang.bottom;
                            if (i == 0) std::printf("INK scale=%.2f metric_height=%.3f ink_bottom=%.3f size_top=%.3f\n", scale, metrics.height, ink_bottom - name_rect.top, size_y - name_rect.top);
                            Check(ink_bottom <= size_y + 0.1f, "folder size begins below the actual filename ink");
                            const auto size_rect = D2D1::RectF(left, size_y, left + width, size_y + 18 * scale);
                            auto* size_format = compositor.SmallFormat();
                            size_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                            compositor.DrawLumaText(L"73.4 MB", size_format, size_rect, foreground, background, DWRITE_TEXT_ALIGNMENT_CENTER);
                            if (!luma) dc->DrawText(L"73.4 MB", 7, size_format, size_rect, ink.get());
                        }
                    }
                    // Narrow panes retain the cap; the selected overlay still shows all
                    // content. Verify natural wrapping without a fake character-width model.
                    const auto narrow = LayoutIconName(&compositor, compositor.DwriteFactory(), format,
                        L"儿童保温杯(小兔子)", 72 * scale, 4);
                    Check(!narrow.truncated && std::count(narrow.shown.begin(), narrow.shown.end(), L'\u2028') >= 1,
                        "narrow CJK names wrap rather than becoming one-line middle ellipses");
                    const auto overflow = LayoutIconName(&compositor, compositor.DwriteFactory(), format,
                        L"非常长的产品文件名_版本号_20261115_未压缩的完整文档.pdf", 104 * scale, 2);
                    Check(overflow.truncated && overflow.shown.ends_with(L".pdf") &&
                        std::count(overflow.shown.begin(), overflow.shown.end(), L'\u2028') == 1,
                        "names above the cap preserve their extension on the last allowed line");
                    const auto result = dc->EndDraw();
                    const auto* stats = compositor.GetLumaTextStats();
                    Check(!luma || stats->draw_calls > luma_draws_before,
                        "Auto actually draws through LumaText rather than passing by initialization alone");
                    std::printf("TEXT_BACKEND luma=%d draw_calls=%llu fallback_layout=%llu\n", luma,
                        static_cast<unsigned long long>(stats->draw_calls),
                        static_cast<unsigned long long>(stats->fallback_layout));
                    const auto file = output / ((luma ? L"luma-" : L"dwrite-") +
                        std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png"));
                    Check(SUCCEEDED(result) && Capture(compositor, target.get(), file.c_str()), "capture actual offscreen name rendering");
                    std::printf("capture=%ls\n", file.c_str());
                }
            }
            // Shared geometry is used by paint, hit-testing, scrolling and rename.
            ViewLayout grid(ViewMode::LargeIcons, D2D1::RectF(0, 0, 570 * scale, 500 * scale),
                10, 0, 0, scale, IconGridNameBlockDip(3));
            const auto name = grid.NameRect(0), size = grid.FolderSizeRect(0), cell = grid.ItemRect(0);
            Check(size.top == name.top + 36 * scale && size.bottom <= cell.bottom &&
                grid.ItemRect(grid.Metrics().columns).top >= cell.bottom,
                "two-line name, separate size and following row never overlap at each DPI");
        }
        dc->SetTarget(original.get());
        compositor.Shutdown();
    }
    DestroyWindow(window);
    CoUninitialize();
    std::printf("SUMMARY failures=%d\n", failures);
    return failures ? 1 : 0;
}
