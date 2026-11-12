#include "preview_host_client.h"
#include "../preview_host/table_document.h"
#include "../ui/table_view.h"
#include "../ui/ui_compositor.h"
#include <wincodec.h>
#include <filesystem>
#include <cstdio>
#include <sstream>
template<class T> using WrlPtr = Microsoft::WRL::ComPtr<T>;
bool SavePng(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, const std::filesystem::path& path) {
    WrlPtr<ID2D1Bitmap1> readable;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, bitmap->GetPixelFormat());
    const auto size = bitmap->GetPixelSize();
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) || FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    WrlPtr<IWICImagingFactory> wic;
    WrlPtr<IWICStream> stream;
    WrlPtr<IWICBitmapEncoder> encoder;
    WrlPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(size.width, size.height)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WritePixels(size.height, mapped.pitch, mapped.pitch * size.height, mapped.bits)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    readable->Unmap();
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    OleInitialize(nullptr);
    int failed = 0;
    const auto check = [&](bool ok, const char* name) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failed += !ok;
    };
    pulse_test::Host host;
    if (!host.Start()) return 2;
    pulse::ui::TableView table;
    std::wstring first;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        pulse_test::Result result;
        const bool received = host.Request(argv[1], result, MAXDWORD, 1024,
            pulse::ipc::PreviewRequestKind::Content, pulse::ipc::kPreviewRequestFlagRichText);
        check(received && result.response.kind == pulse::ipc::PreviewContentKind::Table,
              "host returns native Table, not Office handler");
        check(table.SetPayload(result.text) && table.IsSpreadsheet(), "shared table view accepts XLS payload");
        check(!table.PlainText().empty(),
              "real workbook cells survive repeated preview");
        first = result.text;
    }
    if (first.find(L"W\t2\t1\t0") != std::wstring::npos) {
        check(table.Key(VK_NEXT, false, true), "Ctrl+PageDown selects second worksheet");
        uint32_t index = 0;
        check(table.TakePendingSheetRequest(index) && index == 1, "sheet change queues native decoder request");
        check(table.SetPayload(first) && table.SelectedSheetIndex() == 1 && table.IsSheetRequestPending(),
              "stale first-sheet response does not undo pending switch");
        pulse_test::Result second;
        check(host.Request(argv[1], second, MAXDWORD, 1024, pulse::ipc::PreviewRequestKind::Content,
            pulse::ipc::kPreviewRequestFlagRichText, index), "request second sheet from actual host");
        check(table.SetPayload(second.text) && table.SelectedSheetIndex() == 1 &&
            !table.IsSheetRequestPending() && !table.PlainText().empty(), "second sheet displays native content");
        table.Key(VK_PRIOR, false, true);
        table.Key(VK_NEXT, false, true);
        check(table.SelectedSheetIndex() == 1, "rapid switching keeps latest selected sheet");
        table.Key(VK_PRIOR, false, true);
        check(table.TakePendingSheetRequest(index) && index == 0 && table.SetPayload(first) &&
            table.SelectedSheetIndex() == 0 && !table.PlainText().empty(), "switch back restores first worksheet");
    }
    HWND window = CreateWindowExW(0, L"STATIC", L"XLS offscreen test", WS_POPUP,
        0, 0, 1100, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        pulse::ui::Compositor comp;
        if (!comp.Init(window)) return 2;
        auto dc = comp.Dc();
        WrlPtr<ID2D1Bitmap1> bitmap;
        auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(1100, 800), nullptr, 0, props, &bitmap))) return 2;
        dc->SetTarget(bitmap.Get());
        for (const bool dark : {false, true}) {
            dc->SetDpi(96, 96);
            const auto bg = pulse::ui::HexColor(dark ? 0x202020 : 0xffffff);
            for (int frame = 0; frame < 30; ++frame) {
                dc->BeginDraw(); dc->Clear(bg);
                table.Draw(dc, comp.DwriteFactory(), D2D1::RectF(0, 0, 1100, 800),
                    pulse::ui::MakeTheme(dark, pulse::ui::HexColor(0x0078d4)), dark, 1, bg, {});
                if (FAILED(dc->EndDraw())) { ++failed; break; }
            }
            const std::wstring output = std::wstring(argv[2]) + (dark ? L"\\xls-dark.png" : L"\\xls-light.png");
            check(SavePng(dc, bitmap.Get(), output), "save shared table renderer after 30 frames");
            check(!table.PlainText().empty(), "content retained after redraws");
        }
    }
    DestroyWindow(window);
    OleUninitialize();
    printf("xls-view failed=%d\n", failed);
    return failed ? 1 : 0;
}
