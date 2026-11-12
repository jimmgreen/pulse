#include "../common/localization.h"
#include "preview_host_client.h"
#include "../common/office_preview_policy.h"
#include "../ui/markdown_view.h"
#include "../ui/ui_compositor.h"
#include <wincodec.h>
#include <filesystem>
#include <cstdio>
#include <string>
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


bool EmptyMessageHasInk(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap) {
    WrlPtr<ID2D1Bitmap1> readable;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        bitmap->GetPixelFormat());
    const auto size = bitmap->GetPixelSize();
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    size_t ink = 0;
    for (unsigned y = 250; y < 550; ++y)
        for (unsigned x = 250; x < 1000; ++x) {
            const auto pixel = mapped.bits + y * mapped.pitch + x * 4;
            if (pixel[0] < 180 && pixel[1] < 180 && pixel[2] < 180) ++ink;
        }
    readable->Unmap();
    return ink > 50;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    OleInitialize(nullptr);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const std::filesystem::path root(argv[1]);
    int failed = 0;
    const auto check = [&](bool ok, const char* name) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failed += !ok;
    };
    using namespace pulse;
    check(!l10n::Get(l10n::StringId::PreviewNativeDocumentEmpty).empty() &&
          !l10n::Get(l10n::StringId::PreviewNextSlide).empty(), "empty-state and slide navigation resources are loaded");
    for (const auto ext : {L".DOC", L".dot", L".wps", L".rtf", L".docx", L".docm", L".dotx", L".dotm",
                           L".PPT", L".pptx", L".pptm", L".pps", L".ppsx", L".ppsm", L".pot", L".potx", L".potm", L".dps"})
        check(preview::IsNativeOfficeDocumentExtension(ext), "Office format defaults to native document routing");
    check(!preview::IsNativeOfficeDocumentExtension(L".pdf"), "other native families unchanged");
    pulse_test::Host host;
    if (!host.Start()) return 2;
    HWND window = CreateWindowExW(0, L"STATIC", L"Office offscreen test", WS_POPUP,
        0, 0, 1100, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        ui::Compositor comp;
        if (!comp.Init(window)) return 2;
        auto dc = comp.Dc();
        WrlPtr<ID2D1Bitmap1> bitmap;
        auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(1100, 800), nullptr, 0, props, &bitmap))) return 2;
        dc->SetTarget(bitmap.Get()); dc->SetDpi(96, 96);
        ui::MarkdownView view;
        struct Case { const wchar_t* file; const wchar_t* tail; int sections; };
        const Case cases[] = {
            {L"fixture/sample.doc", L"After", 0},
            {L"fixture/sample.rtf", L"EndOfDocument", 0},
            {L"fixture/disguised.doc", L"Paragraph-99", 0},
            {L"fixture/zip-disguised.doc", L"EndOfDocx", 0},
            {L"fixture/sample.docx", L"EndOfDocx", 0},
            {L"fixtures/ordered.ppt", L"Last physical first", 3},
            {L"fixtures/ordered.pptx", L"Slide 3", 3},
            {L"fixtures/shared-text.ppt", L"Last physical first", 3},
            {L"fixture/sample.doc", L"After", 0}
        };
        int index = 0;
        for (const auto& test : cases) {
            const auto file = (root / test.file).wstring();
            pulse_test::Result result;
            const bool received = host.Request(file, result, MAXDWORD, 1024,
                ipc::PreviewRequestKind::Content, ipc::kPreviewRequestFlagRichText);
            check(received && result.response.kind == ipc::PreviewContentKind::Markdown && result.response.status == 0,
                  "actual preview worker returns native document");
            check(view.SetPayload(result.text, file) && !view.DocFormat().empty(), "shared document view accepts payload");
            check(view.PlainText().find(test.tail) != std::wstring::npos, "tail content present, not first-page thumbnail");
            check(view.SectionCount() == test.sections, "section count and presentation order preserved");
            if (std::wstring(test.file).find(L"shared-text") != std::wstring::npos) {
                const auto& text = view.PlainText();
                const auto first = text.find(L"Shared placeholder"), second = text.find(L"Independent body");
                const auto last = text.find(L"Last physical first");
                check(first < second && second < last && text.find(L"Last physical first", last + 1) == text.npos,
                      "PPT shared text is resolved in shape order without duplicate body text");
            }
            for (const bool dark : {false, true}) {
                const auto bg = ui::HexColor(dark ? 0x202020 : 0xffffff);
                const float scale = dark ? 1.5f : 1.0f;
                const auto rect = D2D1::RectF(0, 0, dark ? 650.0f : 1100.0f, 800.0f);
                bool drawn = true;
                for (int frame = 0; frame < 30; ++frame) {
                    dc->BeginDraw(); dc->Clear(bg);
                    view.Draw(dc, comp.DwriteFactory(), rect, ui::MakeTheme(dark, ui::HexColor(0x0078d4)),
                        dark, scale, nullptr, 1, 0, 0, {});
                    drawn = drawn && SUCCEEDED(dc->EndDraw());
                }
                check(drawn && view.PlainText().find(test.tail) != std::wstring::npos,
                      "30 redraws retain content in light/wide and dark/narrow high-scale layouts");
                check(SavePng(dc, bitmap.Get(), root / (L"view-" + std::to_wstring(index) + (dark ? L"-dark.png" : L"-light.png"))),
                      "save actual document renderer");
            }
            if (test.sections) {
                check(view.ShowSection(1) && !view.CurrentSectionHasContent(), "empty middle slide has explicit empty-content state");
                dc->BeginDraw(); dc->Clear(ui::HexColor(0xffffff));
                view.Draw(dc, comp.DwriteFactory(), D2D1::RectF(0,0,1100,800), ui::MakeTheme(false,ui::HexColor(0x0078d4)),false,1,nullptr,1,0,0,{});
                check(SUCCEEDED(dc->EndDraw()) && SavePng(dc,bitmap.Get(),root/(L"empty-"+std::to_wstring(index)+L".png")),
                      "empty slide renders explanatory message, not silent white");
                check(EmptyMessageHasInk(dc, bitmap.Get()), "empty-page explanatory message has visible pixels");
                check(view.ShowSection(2) && view.CurrentSectionHasContent(), "last slide reachable");
                check(view.ShowSection(0) && view.ShowSection(2) && view.ShowSection(0) && view.SectionIndex() == 0,
                      "rapid slide switches keep latest selection");
                check(!view.ShowSection(3), "out-of-range slide rejected");
            }
            ++index;
        }
        for (const auto name : {L"fixture/encrypted.doc", L"fixture/damaged.doc", L"fixture/damaged.ppt",
                               L"fixtures/malformed.pptx", L"fixtures/encrypted.ppt", L"fixtures/broken.ppt", L"fixtures/garbage.ppt"}) {
            pulse_test::Result result;
            check(host.Request((root/name).wstring(), result, MAXDWORD, 1024, ipc::PreviewRequestKind::Content,
                  ipc::kPreviewRequestFlagRichText) && result.response.status != 0 && result.text.empty(),
                  "damaged/encrypted file returns explicit failure without shell fallback or stale text");
        }
        pulse_test::Result artwork;
        const auto artwork_path = (root/L"fixture/artwork.pptx").wstring();
        check(host.Request(artwork_path, artwork, MAXDWORD, 1024, ipc::PreviewRequestKind::Content,
              ipc::kPreviewRequestFlagRichText) && (artwork.response.flags & ipc::kPreviewFlagTruncated) &&
              view.SetPayload(artwork.text, artwork_path) && !view.CurrentSectionHasContent(),
              "image-only slide explicitly reports partial content and empty-text state");
        check(view.ShowSection(1) && view.CurrentSectionHasContent() && view.PlainText().find(L"TableCell") != std::wstring::npos,
              "PPTX table-cell text survives native content conversion");
        pulse_test::Result offline;
        check(host.Request((root/L"fixture/sample.doc").wstring(), offline, FILE_ATTRIBUTE_OFFLINE, 1024,
              ipc::PreviewRequestKind::Content, ipc::kPreviewRequestFlagRichText) && offline.response.bytes_read == 0 &&
              offline.error == L"offline-placeholder", "native Office respects cloud placeholder no-read guard");
        pulse_test::Result limit;
        check(host.Request((root/L"fixtures/limit.ppsx").wstring(), limit, MAXDWORD, 1024,
              ipc::PreviewRequestKind::Content, ipc::kPreviewRequestFlagRichText) &&
              limit.text.find(L"V\tchapters\t512\t514") != std::wstring::npos,
              "presentation limit retains explicit loaded/total counts");
        view.Clear();
        check(!view.HasData() && view.PlainText().empty(), "close/cancel clears document view state");
    }
    DestroyWindow(window); OleUninitialize();
    printf("office-view failed=%d\n", failed);
    return failed ? 1 : 0;
}
