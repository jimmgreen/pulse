#include "../ui/markdown_view.h"
#include "../ui/table_view.h"
#include "../preview_host/markdown_document.h"
#include <d3d11.h>
#include <wincodec.h>
#include <objbase.h>
#include <string_view>
#include <filesystem>
#include <cstdio>
#include <algorithm>
#include <cmath>
using Microsoft::WRL::ComPtr;
namespace pulse::ui {
struct MarkdownScrollbarTest {
    static auto Bar(const MarkdownView& view, bool horizontal) { return view.Scrollbar(horizontal); }
    static float Position(const MarkdownView& view, bool horizontal) { return horizontal ? view.scroll_x_ : view.scroll_; }
};
struct TableViewTestAccess {
    static const auto& Tabs(const TableView& view) { return view.tab_rects_; }
    static auto Previous(const TableView& view) { return view.previous_sheet_; }
    static size_t First(const TableView& view) { return view.first_visible_sheet_; }
};
}
namespace {
int failures = 0;
void Check(bool ok, const char* name) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; }
bool SavePng(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, const std::filesystem::path& path) {
    ComPtr<ID2D1Bitmap1> readable;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
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
struct Canvas {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> bitmap;
    ComPtr<IDWriteFactory2> factory;
    bool Init(float scale) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) ||
            FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) || FAILED(d3d.As(&dxgi)) ||
            FAILED(D2D1CreateDevice(dxgi.Get(), nullptr, &device)) ||
            FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(600 * scale), static_cast<UINT32>(300 * scale)),
            nullptr, 0, props, &bitmap))) return false;
        dc->SetTarget(bitmap.Get());
        return true;
    }
    void Begin(bool dark) { dc->BeginDraw(); dc->Clear(D2D1::ColorF(dark ? 0x17191c : 0xffffff)); }
    void End() { Check(SUCCEEDED(dc->EndDraw()), "D2D drawing completes"); }
    std::vector<unsigned char> Pixels() {
        ComPtr<ID2D1Bitmap1> copy;
        const auto size = bitmap->GetPixelSize();
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, bitmap->GetPixelFormat());
        if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &copy)) ||
            FAILED(copy->CopyFromBitmap(nullptr, bitmap.Get(), nullptr))) return {};
        D2D1_MAPPED_RECT map{};
        if (FAILED(copy->Map(D2D1_MAP_OPTIONS_READ, &map))) return {};
        std::vector<unsigned char> bytes(size.width * size.height * 4);
        for (UINT32 y = 0; y < size.height; ++y)
            std::copy_n(map.bits + y * map.pitch, size.width * 4, bytes.data() + y * size.width * 4);
        copy->Unmap();
        return bytes;
    }
};
void MarkdownScrollbars(Canvas& c, float scale, bool dark) {
    using pulse::ui::MarkdownScrollbarTest;
    const auto rect = D2D1::RectF(0, 0, 600 * scale, 300 * scale);
    const auto theme = pulse::ui::MakeTheme(dark, pulse::ui::HexColor(0x0078d4));
    std::wstring source = L"# Scrollbar regression\n\n";
    for (int row = 0; row < 40; ++row) {
        for (int col = 0; col < 16; ++col) source += row == 1 ? L"| --- " : L"| Long column value ";
        source += L"|\n";
    }
    std::wstring payload;
    Check(pulse::preview::MakeMarkdownDocument(source, payload), "parse wide and tall Markdown fixture");
    pulse::ui::MarkdownView view;
    Check(view.SetPayload(payload, L""), "load scrollbar fixture");
    auto draw = [&](const D2D1_RECT_F& bounds) {
        c.Begin(dark);
        view.Draw(c.dc.Get(), c.factory.Get(), bounds, theme, dark, scale, nullptr, 1, 0, 0, {});
        c.End();
    };
    draw(rect);
    Check(!view.ScrollbarMouseDown(50 * scale, 40 * scale), "content clicks do not start scrollbar gestures");
    for (bool horizontal : {false, true}) {
        auto bar = MarkdownScrollbarTest::Bar(view, horizontal);
        Check(bar.maximum > 0 && bar.travel > 0, "overflow produces draggable scrollbar");
        const float x = (bar.thumb.left + bar.thumb.right) * 0.5f;
        const float y = (bar.thumb.top + bar.thumb.bottom) * 0.5f;
        Check(view.OverScrollbar(x, y), "painted thumb belongs to scrollbar hit lane");
        Check(view.ScrollbarMouseDown(x, y) && view.DraggingScrollbar(), "thumb press starts drag before text selection");
        Check(view.ScrollbarMouseMove(x + (horizontal ? bar.travel * 0.5f : 0),
            y + (horizontal ? 0 : bar.travel * 0.5f)), "drag moves document on intended axis");
        Check(std::abs(MarkdownScrollbarTest::Position(view, horizontal) - bar.maximum * 0.5f) < 0.1f,
            "thumb displacement maps to document scroll range");
        view.ScrollbarMouseMove(x + (horizontal ? 100000.0f : 0), y + (horizontal ? 0 : 100000.0f));
        Check(std::abs(MarkdownScrollbarTest::Position(view, horizontal) - bar.maximum) < 0.1f,
            "drag outside viewport clamps at document end");
        view.EndScrollbarDrag();
        Check(!view.DraggingScrollbar() && !view.ScrollbarMouseMove(x, y), "release or cancellation ends drag");
        bar = MarkdownScrollbarTest::Bar(view, horizontal);
        const float track_x = horizontal ? bar.hit.left + 1 : x;
        const float track_y = horizontal ? y : bar.hit.top + 1;
        Check(view.ScrollbarMouseDown(track_x, track_y) && !view.DraggingScrollbar() &&
            MarkdownScrollbarTest::Position(view, horizontal) < bar.maximum, "track click pages toward pointer");
        bar = MarkdownScrollbarTest::Bar(view, horizontal);
        Check(view.ScrollbarMouseDown((bar.thumb.left + bar.thumb.right) * 0.5f,
            (bar.thumb.top + bar.thumb.bottom) * 0.5f) && view.DraggingScrollbar(), "drag can restart after track click");
        draw(D2D1::RectF(0, 0, 590 * scale, 290 * scale));
        Check(!view.DraggingScrollbar(), "resize cancels stale drag geometry");
        draw(rect);
    }
    auto bar = MarkdownScrollbarTest::Bar(view, false);
    view.ScrollbarMouseDown((bar.thumb.left + bar.thumb.right) * 0.5f, (bar.thumb.top + bar.thumb.bottom) * 0.5f);
    view.Clear();
    Check(!view.DraggingScrollbar() && !view.ScrollbarMouseDown(598 * scale, 20 * scale),
        "clearing selection cancels drag and removes stale hit lanes");
    pulse::preview::MakeMarkdownDocument(L"short", payload);
    view.SetPayload(payload, L"");
    draw(rect);
    Check(!view.OverScrollbar(598 * scale, 20 * scale) && !view.OverScrollbar(20 * scale, 298 * scale),
        "short documents do not steal edge clicks");
}

void Markdown(Canvas& c, float scale, bool dark) {
    const auto rect = D2D1::RectF(0, 0, 600 * scale, 300 * scale);
    const auto theme = pulse::ui::MakeTheme(dark, pulse::ui::HexColor(0x0078d4));
    for (int columns : {2, 8, 16, 64}) {
        std::wstring source, separator, body;
        for (int i = 0; i < columns; ++i) {
            source += i + 1 == columns ? L"| [TARGET](https://example.invalid/target) " : L"| a ";
            separator += L"| --- ";
            body += L"| b ";
        }
        source += L"|\n" + separator + L"|\n" + body + L"|\n";
        std::wstring payload;
        Check(pulse::preview::MakeMarkdownDocument(source, payload), "parse real Markdown table");
        pulse::ui::MarkdownView view;
        Check(view.SetPayload(payload, L""), "load Markdown payload");
        auto draw = [&] { c.Begin(dark); view.Draw(c.dc.Get(), c.factory.Get(), rect, theme, dark, scale, nullptr, 1, 0, 0, {}); c.End(); };
        draw();
        const auto target = static_cast<uint32_t>(view.PlainText().find(L"TARGET"));
        view.Reveal(target);
        draw();
        bool reachable = false, selectable = false, end_visible = false;
        for (int y = 0; y < static_cast<int>(140 * scale); y += 2)
            for (int x = 0; x < static_cast<int>(600 * scale); x += 2) {
                if (view.LinkAt(static_cast<float>(x), static_cast<float>(y)).empty()) continue;
                reachable = true;
                uint32_t offset = 0;
                selectable |= view.HitTest(static_cast<float>(x), static_cast<float>(y), offset) && offset >= target && offset <= target + 6;
                end_visible |= offset == target + 6;
            }
        Check(reachable && selectable, "rightmost link and selection reachable after Reveal");
        Check(end_visible, "Reveal exposes final glyph trailing edge of the rightmost link");
        if (columns >= 16) {
            Check(view.ScrollHorizontal(10000), "wide table can return horizontally");
            Check(view.Key(VK_RIGHT), "keyboard exposes horizontal scroll");
        }
        if (columns == 16) Check(SavePng(c.dc.Get(), c.bitmap.Get(), std::filesystem::path(L"bench_data") /
            (L"m16-table-right-" + std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png"))), "save rightmost table screenshot");
        if (columns == 16) {
            payload.insert(payload.find(L'\n') + 1, L"P\tFirst\n");
            payload += L"P\tSecond\nB\tp\t\t0\t0\t\tSECOND\t\n";
            Check(view.SetPayload(payload, L""), "load chaptered wide table");
            draw();
            view.Reveal(static_cast<uint32_t>(view.PlainText().find(L"SECOND")));
            draw();
            Check(view.SectionIndex() == 1 && !view.ScrollHorizontal(-1), "new narrow chapter clears horizontal extent");
            view.Reveal(target);
            draw();
            Check(view.SectionIndex() == 0 && view.ScrollHorizontal(10000), "cross-chapter Reveal restores right column");
        }
    }
    const std::wstring source = L"emoji \U0001F600 \U00020000 e\u0301 العربية $x^2$ end";
    std::wstring payload;
    pulse::preview::MakeMarkdownDocument(source, payload);
    pulse::ui::MarkdownView view;
    view.SetPayload(payload, L"");
    c.Begin(dark); view.Draw(c.dc.Get(), c.factory.Get(), rect, theme, dark, scale, nullptr, 1, 0, 0, {}); c.End();
    bool valid = true, after_emoji = false;
    const auto& text = view.PlainText();
    const auto emoji = text.find(L"\U0001F600");
    for (int y = 0; y < static_cast<int>(100 * scale); ++y)
        for (int x = 0; x < static_cast<int>(600 * scale); ++x) {
            uint32_t offset = 0;
            if (!view.HitTest(static_cast<float>(x), static_cast<float>(y), offset)) continue;
            valid &= offset <= text.size();
            if (offset > 0 && offset < text.size())
                valid &= !(text[offset - 1] >= 0xd800 && text[offset - 1] <= 0xdbff &&
                           text[offset] >= 0xdc00 && text[offset] <= 0xdfff);
            after_emoji |= offset == emoji + 2;
        }
    Check(valid && after_emoji, "real prose trailing hits preserve supplementary characters");
}
void SheetTabs(Canvas& c, float scale, bool dark) {
    using pulse::ui::TableViewTestAccess;
    pulse::ui::TableView view;
    const auto theme = pulse::ui::MakeTheme(dark, pulse::ui::HexColor(0x0078d4));
    const auto payload = [](unsigned selected) {
        return std::wstring(L"PULSETBL\t1\n") +
            (selected == 0 ? L"S\txlsx\tDates\t1\t1\t0\t\t0\nR\tDATE-CONTENT\n" :
                             L"S\txlsx\tDates\t0\t0\t0\tnot-loaded\t0\n") +
            (selected == 1 ? L"S\txlsx\tLongFields\t1\t1\t0\t\t0\nR\tLONG-CONTENT\n" :
                             L"S\txlsx\tLongFields\t0\t0\t0\tnot-loaded\t0\n") +
            L"W\t2\t1\t" + std::to_wstring(selected) + L"\n";
    };
    auto draw = [&](float width) {
        c.Begin(dark);
        view.Draw(c.dc.Get(), c.factory.Get(), D2D1::RectF(0, 0, width * scale, 300 * scale),
            theme, dark, scale, D2D1::ColorF(0xffffff), {});
        c.End();
    };
    const auto click = [&](D2D1_RECT_F r) {
        return view.MouseDown((r.left + r.right) / 2, (r.top + r.bottom) / 2, false);
    };
    Check(view.SetPayload(payload(0)), "tabs load initial real lazy payload");
    draw(600);
    Check(TableViewTestAccess::Tabs(view).size() == 2, "wide strip shows both sheets initially");
    if (TableViewTestAccess::Tabs(view).size() != 2) return;
    click(TableViewTestAccess::Tabs(view)[1]);
    uint32_t requested = UINT32_MAX;
    Check(view.TakePendingSheetRequest(requested) && requested == 1, "drawn second tab center requests second sheet");
    Check(view.SetPayload(payload(1)) && view.PlainText().find(L"LONG-CONTENT") != std::wstring::npos,
        "successful second response shows its body");
    draw(600);
    Check(TableViewTestAccess::First(view) == 0 && TableViewTestAccess::Tabs(view).size() == 2,
        "selecting second sheet preserves earlier visible tab");
    click(TableViewTestAccess::Previous(view));
    Check(view.TakePendingSheetRequest(requested) && requested == 0, "drawn previous arrow center requests first sheet");
    Check(view.SetPayload(payload(0)) && view.PlainText().find(L"DATE-CONTENT") != std::wstring::npos,
        "successful previous response restores first body");
    draw(600);
    click(TableViewTestAccess::Tabs(view)[1]);
    Check(view.TakePendingSheetRequest(requested) && requested == 1, "second sheet remains selectable after round trip");
    Check(view.SetPayload(payload(1)), "second response can be reused from cache");
    draw(260);
    Check(TableViewTestAccess::First(view) == 1 && !TableViewTestAccess::Tabs(view).empty(),
        "narrow strip scroll origin keeps selected sheet visible");
    if (!TableViewTestAccess::Tabs(view).empty()) click(TableViewTestAccess::Tabs(view)[0]);
    Check(view.SelectedSheetIndex() == 1 && !view.TakePendingSheetRequest(requested),
        "narrow visible tab hit maps to actual second sheet");
    draw(600);
    Check(TableViewTestAccess::First(view) == 0 && TableViewTestAccess::Tabs(view).size() == 2,
        "widening restores both tabs without changing selection");
    click(TableViewTestAccess::Tabs(view)[0]);
    Check(view.TakePendingSheetRequest(requested) && requested == 0, "visible earlier tab center maps to first sheet");
}
void Headers(Canvas& c, float scale, bool dark) {
    const auto rect = D2D1::RectF(0, 0, 600 * scale, 300 * scale);
    const auto theme = pulse::ui::MakeTheme(dark, pulse::ui::HexColor(0x0078d4));
    for (bool body : {false, true}) {
        pulse::ui::TableView view;
        Check(view.SetPayload(L"PULSETBL\t1\nS\tcsv\tCSV\t2\t" + std::wstring(body ? L"2" : L"1") +
            L"\t0\t\t1\nR\tNeedle\tOther\n" + (body ? L"R\tNeedle\t2\n" : L"")), "load CSV header fixture");
        auto draw = [&](const std::vector<pulse::ui::TableView::Highlight>& matches) {
            c.Begin(dark);
            view.Draw(c.dc.Get(), c.factory.Get(), rect, theme, dark, scale, D2D1::ColorF(dark ? 0x17191c : 0xffffff), matches);
            c.End(); return c.Pixels();
        };
        const auto plain = draw({});
        const auto marked = draw({{0, 6, true}});
        Check(!plain.empty() && plain != marked, "header-only search changes rendered pixels");
        const auto secondary = draw({{0, 6, false}});
        Check(!secondary.empty() && secondary != marked, "current header highlight has distinct feedback");
        Check(SavePng(c.dc.Get(), c.bitmap.Get(), std::filesystem::path(L"bench_data") /
            (L"m16-header-" + std::to_wstring(static_cast<int>(scale * 100)) + (body ? L"-body" : L"-empty") +
             (dark ? L"-dark.png" : L"-light.png"))), "save fixed-header screenshot");
    }
    pulse::ui::TableView wide;
    std::wstring payload = L"PULSETBL\t1\nS\tcsv\tCSV\t16\t1\t0\t\t1\nR";
    for (int i = 0; i < 16; ++i) payload += i == 15 ? L"\tNeedle" : L"\tOther";
    payload += L"\n";
    Check(wide.SetPayload(payload), "load horizontally clipped header");
    auto draw = [&](bool mark) {
        c.Begin(dark);
        const auto offset = static_cast<uint32_t>(wide.PlainText().find(L"Needle"));
        wide.Draw(c.dc.Get(), c.factory.Get(), rect, theme, dark, scale, D2D1::ColorF(0xffffff),
            mark ? std::vector<pulse::ui::TableView::Highlight>{{offset, 6, true}} : std::vector<pulse::ui::TableView::Highlight>{});
        c.End(); return c.Pixels();
    };
    draw(false);
    wide.Reveal(static_cast<uint32_t>(wide.PlainText().find(L"Needle")));
    const auto before = draw(false);
    const auto after = draw(true);
    Check(!before.empty() && before != after, "far header search is highlighted after horizontal Reveal");
}
}
int wmain(int argc, wchar_t** argv) {
    const bool markdown = argc < 2 || std::wstring_view(argv[1]) == L"--markdown";
    const bool scrollbars = argc < 2 || std::wstring_view(argv[1]) == L"--markdown-scrollbars";
    const bool headers = argc < 2 || std::wstring_view(argv[1]) == L"--headers";
    const bool tabs = argc < 2 || std::wstring_view(argv[1]) == L"--sheet-tabs";
    if (!markdown && !scrollbars && !headers && !tabs) return 2;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::filesystem::create_directories(L"bench_data");
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        Canvas canvas;
        const bool ready = canvas.Init(scale);
        Check(ready, "create WARP D2D and DirectWrite");
        if (!ready) continue;
        for (bool dark : {false, true}) {
            if (markdown) Markdown(canvas, scale, dark);
            if (scrollbars) MarkdownScrollbars(canvas, scale, dark);
            if (headers) Headers(canvas, scale, dark);
            if (tabs) SheetTabs(canvas, scale, dark);
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return failures ? 1 : 0;
}
