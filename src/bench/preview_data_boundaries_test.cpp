#include "../preview_host/icc_profile.h"
#include "../preview_host/psd_raster.h"
#include "../preview_host/shell_bitmap.h"
#include "../preview_host/table_document.h"
#include "../preview_host/office_model.h"
#include "../preview_host/xlsx_parts.h"
#include "preview_fixture_zip.h"
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <array>
#include <algorithm>
#include <cstdlib>

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
void Be16(Bytes& out, uint16_t v) { out.push_back(static_cast<unsigned char>(v >> 8)); out.push_back(static_cast<unsigned char>(v)); }
void Be32(Bytes& out, uint32_t v) { Be16(out, static_cast<uint16_t>(v >> 16)); Be16(out, static_cast<uint16_t>(v)); }
bool Write(const fs::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}
Bytes Psd(bool psb, bool rle, bool permute, bool solid = false, uint32_t count_override = 0) {
    Bytes out{'8','B','P','S'};
    Be16(out, psb ? 2 : 1); out.resize(out.size() + 6);
    Be16(out, 1); Be32(out, 1); Be32(out, 32); Be16(out, 8); Be16(out, 2);
    Be32(out, 768);
    std::array<unsigned char, 768> palette{};
    palette[permute ? 2 : 0] = 255; palette[256 + 1] = 255; palette[512 + (permute ? 0 : 2)] = 255;
    out.insert(out.end(), palette.begin(), palette.end());
    Be32(out, 0); Be32(out, 0); if (psb) Be32(out, 0);
    Be16(out, rle ? 1 : 0);
    if (rle) {
        const uint32_t count = count_override ? count_override : 33;
        if (psb) Be32(out, count); else Be16(out, static_cast<uint16_t>(count));
        out.push_back(31);
    }
    for (int i = 0; i < 32; ++i) {
        const bool red = solid || i % 2 == 0;
        out.push_back(static_cast<unsigned char>(red ? (permute ? 2 : 0) : (permute ? 0 : 2)));
    }
    return out;
}
// 2x1 RGB document with merged transparency, stored the way Photoshop does:
// colour composited over white. Pixel 0 is red at alpha 128 (stored
// 255,127,127), pixel 1 is clear (stored white). lr16: the transparency is
// announced only by an "Lr16" tagged block, as in 16-bit documents whose
// 8-bit layer info is empty.
Bytes TransparentPsd(uint16_t depth, bool lr16) {
    Bytes out{'8','B','P','S'};
    Be16(out, 1); out.resize(out.size() + 6);
    Be16(out, 4); Be32(out, 1); Be32(out, 2); Be16(out, depth); Be16(out, 3);
    Be32(out, 0);   // colour mode data
    Be32(out, 0);   // image resources
    Bytes layers;
    if (lr16) {
        Be32(layers, 0);   // empty layer info
        Be32(layers, 0);   // global layer mask info
        for (char c : std::string("8BIMLr16")) layers.push_back(static_cast<unsigned char>(c));
        Be32(layers, 2); Be16(layers, 0xFFFF);
    } else {
        Be32(layers, 2); Be16(layers, 0xFFFF);
    }
    Be32(out, static_cast<uint32_t>(layers.size()));
    out.insert(out.end(), layers.begin(), layers.end());
    Be16(out, 0);   // raw composite: planes R, G, B, A
    const uint16_t planes[4][2] = {{255, 255}, {127, 255}, {127, 255}, {128, 0}};
    for (const auto& plane : planes)
        for (const uint16_t v : plane) {
            if (depth == 16) Be16(out, static_cast<uint16_t>(v * 257));
            else out.push_back(static_cast<unsigned char>(v));
        }
    return out;
}
// Solid-colour JPEG, for the embedded thumbnail of a PSD fixture.
Bytes JpegSolid(UINT w, UINT h, unsigned char r, unsigned char g, unsigned char b) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(w, h)))
        return {};
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR) return {};
    Bytes pixels(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < pixels.size(); i += 3) { pixels[i] = b; pixels[i + 1] = g; pixels[i + 2] = r; }
    if (FAILED(frame->WritePixels(h, w * 3, static_cast<UINT>(pixels.size()), pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return {};
    HGLOBAL memory = nullptr;
    STATSTG stat{};
    if (FAILED(GetHGlobalFromStream(stream.Get(), &memory)) || FAILED(stream->Stat(&stat, STATFLAG_NONAME)))
        return {};
    const auto* data = static_cast<const unsigned char*>(GlobalLock(memory));
    if (!data) return {};
    Bytes out(data, data + static_cast<size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(memory);
    return out;
}
// Opaque solid red RGB document (PackBits rows) whose embedded thumbnail is
// `jpeg`, so the pixels tell which source a grid thumbnail came from.
Bytes OpaquePsd(uint32_t width, uint32_t height, const Bytes& jpeg, uint32_t thumb_w, uint32_t thumb_h, bool psb = false) {
    Bytes out{'8','B','P','S'};
    Be16(out, psb ? 2 : 1); out.resize(out.size() + 6);
    Be16(out, 3); Be32(out, height); Be32(out, width); Be16(out, 8); Be16(out, 3);
    Be32(out, 0);   // colour mode data
    Bytes resource{'8','B','I','M'};
    Be16(resource, 1036); Be16(resource, 0);   // id, empty Pascal name
    const uint32_t row_bytes = (thumb_w * 24 + 31) / 32 * 4;
    Be32(resource, static_cast<uint32_t>(28 + jpeg.size()));
    Be32(resource, 1); Be32(resource, thumb_w); Be32(resource, thumb_h);
    Be32(resource, row_bytes); Be32(resource, row_bytes * thumb_h);
    Be32(resource, static_cast<uint32_t>(jpeg.size()));
    Be16(resource, 24); Be16(resource, 1);
    resource.insert(resource.end(), jpeg.begin(), jpeg.end());
    if (resource.size() & 1) resource.push_back(0);
    Be32(out, static_cast<uint32_t>(resource.size()));
    out.insert(out.end(), resource.begin(), resource.end());
    Be32(out, 0); if (psb) Be32(out, 0);   // no layers
    Be16(out, 1);   // PackBits composite
    const auto packed = [width](unsigned char value) {
        Bytes row;
        for (uint32_t left = width; left;) {
            const uint32_t run = (std::min)(left, 128u);
            row.push_back(run == 1 ? 0 : static_cast<unsigned char>(257 - run));
            row.push_back(value);
            left -= run;
        }
        return row;
    };
    const Bytes rows[3] = {packed(255), packed(0), packed(0)};
    for (const Bytes& row : rows)
        for (uint32_t y = 0; y < height; ++y) {
            if (psb) Be32(out, static_cast<uint32_t>(row.size()));
            else Be16(out, static_cast<uint16_t>(row.size()));
        }
    for (const Bytes& row : rows)
        for (uint32_t y = 0; y < height; ++y) out.insert(out.end(), row.begin(), row.end());
    return out;
}
// A minimal ICC v2 gray display profile whose tone curve is a plain gamma
// (u8Fixed8): with gamma 1.0, device 128 is half linear light, sRGB 188.
Bytes GrayIcc(uint16_t gamma) {
    const auto pad = [](Bytes& b) { while (b.size() % 4) b.push_back(0); };
    const char text[] = "Pulse test gray";
    Bytes desc{'d','e','s','c',0,0,0,0};
    Be32(desc, sizeof(text));
    desc.insert(desc.end(), text, text + sizeof(text));
    Be32(desc, 0); Be32(desc, 0);          // no Unicode description
    desc.resize(desc.size() + 70, 0);       // no ScriptCode description
    pad(desc);
    Bytes white{'X','Y','Z',' ',0,0,0,0};
    Be32(white, 0xF6D6); Be32(white, 0x10000); Be32(white, 0xD32D);   // D50
    Bytes curve{'c','u','r','v',0,0,0,0};
    Be32(curve, 1); Be16(curve, gamma); Be16(curve, 0);
    Bytes copyright{'t','e','x','t',0,0,0,0,'P','D',0,0};
    const std::pair<const char*, const Bytes*> tags[] = {{"desc", &desc}, {"wtpt", &white}, {"kTRC", &curve},
                                                         {"cprt", &copyright}};
    Bytes table, body;
    Be32(table, 4);
    const uint32_t first = 128 + 4 + 12 * 4;
    for (const auto& [sig, data] : tags) {
        table.insert(table.end(), sig, sig + 4);
        Be32(table, first + static_cast<uint32_t>(body.size()));
        Be32(table, static_cast<uint32_t>(data->size()));
        body.insert(body.end(), data->begin(), data->end());
    }
    Bytes out;
    Be32(out, first + static_cast<uint32_t>(body.size()));
    Be32(out, 0); Be32(out, 0x02100000);
    for (const char* s : {"mntr", "GRAY", "XYZ "}) out.insert(out.end(), s, s + 4);
    out.resize(36, 0);
    for (char c : {'a', 'c', 's', 'p'}) out.push_back(static_cast<unsigned char>(c));
    out.resize(68, 0);
    Be32(out, 0xF6D6); Be32(out, 0x10000); Be32(out, 0xD32D);
    out.resize(128, 0);
    out.insert(out.end(), table.begin(), table.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
// A minimal ICC v2 RGB display profile: sRGB primaries (D50) and one plain
// gamma for all three channels, under the given name. With gamma 1.0,
// device 128 is half linear light, sRGB 188. lut adds an (empty) A2B0 tag.
Bytes RgbIcc(const char* name, uint16_t gamma, bool lut = false) {
    const auto pad = [](Bytes& b) { while (b.size() % 4) b.push_back(0); };
    const size_t name_size = std::strlen(name) + 1;
    Bytes desc{'d','e','s','c',0,0,0,0};
    Be32(desc, static_cast<uint32_t>(name_size));
    desc.insert(desc.end(), name, name + name_size);
    Be32(desc, 0); Be32(desc, 0);
    desc.resize(desc.size() + 70, 0);
    pad(desc);
    const auto xyz = [](uint32_t x, uint32_t y, uint32_t z) {
        Bytes b{'X','Y','Z',' ',0,0,0,0};
        Be32(b, x); Be32(b, y); Be32(b, z);
        return b;
    };
    const Bytes white = xyz(0xF6D6, 0x10000, 0xD32D);
    const Bytes red = xyz(0x6FA2, 0x38F5, 0x0390), green = xyz(0x6299, 0xB785, 0x18DA),
                blue = xyz(0x24A0, 0x0F84, 0xB6CF);
    Bytes curve{'c','u','r','v',0,0,0,0};
    Be32(curve, 1); Be16(curve, gamma); Be16(curve, 0);
    Bytes copyright{'t','e','x','t',0,0,0,0,'P','D',0,0};
    const Bytes table_tag{'m','f','t','2',0,0,0,0};
    const std::pair<const char*, const Bytes*> tags[] = {
        {"desc", &desc}, {"wtpt", &white}, {"rXYZ", &red}, {"gXYZ", &green}, {"bXYZ", &blue},
        {"rTRC", &curve}, {"gTRC", &curve}, {"bTRC", &curve}, {"cprt", &copyright}, {"A2B0", &table_tag}};
    const uint32_t count = static_cast<uint32_t>(std::size(tags)) - (lut ? 0u : 1u);
    Bytes table, body;
    Be32(table, count);
    const uint32_t first = 128 + 4 + 12 * count;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& [sig, data] = tags[i];
        table.insert(table.end(), sig, sig + 4);
        Be32(table, first + static_cast<uint32_t>(body.size()));
        Be32(table, static_cast<uint32_t>(data->size()));
        body.insert(body.end(), data->begin(), data->end());
    }
    Bytes out;
    Be32(out, first + static_cast<uint32_t>(body.size()));
    Be32(out, 0); Be32(out, 0x02100000);
    for (const char* s : {"mntr", "RGB ", "XYZ "}) out.insert(out.end(), s, s + 4);
    out.resize(36, 0);
    for (char c : {'a', 'c', 's', 'p'}) out.push_back(static_cast<unsigned char>(c));
    out.resize(68, 0);
    Be32(out, 0xF6D6); Be32(out, 0x10000); Be32(out, 0xD32D);
    out.resize(128, 0);
    out.insert(out.end(), table.begin(), table.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
// An uncompressed one-row 8-bit document; planar holds the channel planes.
Bytes ModePsd(uint16_t mode, uint16_t channels, uint32_t width, const Bytes& planar, const Bytes& icc) {
    Bytes out{'8','B','P','S'};
    Be16(out, 1); out.resize(out.size() + 6);
    Be16(out, channels); Be32(out, 1); Be32(out, width); Be16(out, 8); Be16(out, mode);
    Be32(out, 0);   // colour mode data
    Bytes resources;
    if (!icc.empty()) {
        resources = {'8','B','I','M'};
        Be16(resources, 1039); Be16(resources, 0);
        Be32(resources, static_cast<uint32_t>(icc.size()));
        resources.insert(resources.end(), icc.begin(), icc.end());
        if (resources.size() & 1) resources.push_back(0);
    }
    Be32(out, static_cast<uint32_t>(resources.size()));
    out.insert(out.end(), resources.begin(), resources.end());
    Be32(out, 0);   // no layer and mask information
    Be16(out, 0);   // raw image data
    out.insert(out.end(), planar.begin(), planar.end());
    return out;
}
bool ValidUtf16(const std::wstring& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] >= 0xD800 && text[i] <= 0xDBFF) {
            if (++i == text.size() || text[i] < 0xDC00 || text[i] > 0xDFFF) return false;
        } else if (text[i] >= 0xDC00 && text[i] <= 0xDFFF) return false;
    }
    return true;
}
std::vector<std::wstring> Fields(const std::wstring& payload, wchar_t marker) {
    const auto start = payload.find(std::wstring(1, marker) + L"\t");
    if (start == std::wstring::npos) return {};
    const auto end = payload.find(L'\n', start);
    const auto line = payload.substr(start, end - start);
    std::vector<std::wstring> fields;
    for (size_t from = 0;;) {
        const auto next = line.find(L'\t', from);
        fields.push_back(line.substr(from, next == std::wstring::npos ? next : next - from));
        if (next == std::wstring::npos) break;
        from = next + 1;
    }
    return fields;
}
bool Workbook(const fs::path& path, const std::string& date, const std::string& cells,
              const std::string& shared = {}) {
    const std::string workbook = "<workbook xmlns:r=\"r\"><workbookPr" + date +
        "/><sheets><sheet name=\"One\" r:id=\"r1\"/><sheet name=\"Two\" r:id=\"r2\"/></sheets></workbook>";
    const std::string sheet = "<worksheet><sheetData><row r=\"1\">" + cells + "</row></sheetData></worksheet>";
    return preview_fixture::Zip(path, {
        {"xl/workbook.xml", workbook},
        {"xl/_rels/workbook.xml.rels", "<Relationships><Relationship Id=\"r1\" Target=\"worksheets/s1.xml\"/><Relationship Id=\"r2\" Target=\"worksheets/s2.xml\"/></Relationships>"},
        {"xl/styles.xml", "<styleSheet><cellXfs count=\"3\"><xf numFmtId=\"0\"/><xf numFmtId=\"14\"/><xf numFmtId=\"22\"/></cellXfs></styleSheet>"},
        {"xl/worksheets/s1.xml", sheet}, {"xl/worksheets/s2.xml", sheet},
        {"xl/sharedStrings.xml", "<sst><si><t>" + shared + "</t></si></sst>"}
    });
}
}
int wmain(int argc, wchar_t** argv) {
    using namespace pulse::preview;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int failures = 0;
    auto check = [&](bool ok, const char* text) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; return ok; };
    const auto parent = fs::absolute(L"bench_data"); fs::create_directories(parent);
    const auto root = parent / (L"preview-data-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) return 1;
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--psd-progressive") {
        const auto jpeg = JpegSolid(160, 120, 0, 255, 0);
        auto data = OpaquePsd(11648, 8736, jpeg, 160, 120);
        const auto file = root / L"large.psd";
        check(!jpeg.empty() && Write(file, data), "create 11648x8736 PSD with 160x120 JPEG");
        Bytes pixels; UINT w = 0, h = 0, stride = 0, sw = 0, sh = 0;
        bool embedded = false; std::wstring error;
        const auto decode = [&](bool fast, UINT edge) {
            return RasterizePsdFile(file.wstring(), edge, fast, pixels, w, h, stride, sw, sh, &error, &embedded);
        };
        auto started = GetTickCount64();
        check(decode(true, 512) && embedded && w == 160 && h == 120 && pixels[1] > 240 &&
            sw == 11648 && sh == 8736, "large PSD first pass exposes embedded JPEG origin and source dimensions");
        printf("first_pass_ms=%llu\n", GetTickCount64() - started);
        started = GetTickCount64();
        check(decode(false, 512) && !embedded && w == 512 && h == 384 && pixels[2] == 255 &&
            pixels[1] == 0, "large PSD refinement returns full grid resolution composite");
        printf("refinement_ms=%llu\n", GetTickCount64() - started);
        check(decode(true, 128) && embedded && w == 128, "embedded origin survives size-covered first pass");
        if (argc == 3 && std::wstring_view(argv[2]) == L"--keep-fixture") {
            const auto retained = root / L"progressive.psd";
            check(Write(retained, data), "retain fixture for cache integration test");
            wprintf(L"fixture=%ls\n", retained.c_str());
        }
        data.resize(data.size() - 1024);
        check(Write(file, data) && decode(false, 512) && embedded && w == 160 && pixels[1] > 240,
            "broken composite fallback remains explicitly embedded and cannot be cached as refined");
        check(Write(file, OpaquePsd(11648, 8736, jpeg, 160, 120, true)) &&
            decode(true, 512) && embedded && w == 160 && decode(false, 512) && !embedded && w == 512,
            "large PSB also transitions from embedded preview to composite");
        std::error_code cleanup_error;
        if (argc < 3) {
            if (root.parent_path() == parent && root.filename().wstring().starts_with(L"preview-data-"))
                fs::remove_all(root, cleanup_error);
            check(!cleanup_error && !fs::exists(root), "private progressive fixtures removed");
        }
        CoUninitialize();
        return failures ? 1 : 0;
    }
    auto decode = [&](const Bytes& data, UINT edge, Bytes& pixels, std::wstring& error) {
        const auto file = root / L"indexed.psd";
        if (!Write(file, data)) return false;
        UINT w = 0, h = 0, stride = 0, sw = 0, sh = 0;
        return RasterizePsdFile(file.wstring(), edge, false, pixels, w, h, stride, sw, sh, &error);
    };
    for (bool rle : {false, true}) {
        Bytes first, permuted, full, solid; std::wstring error;
        check(decode(Psd(false, rle, false), 16, first, error) && first.size() == 64 &&
            first[0] == 128 && first[1] == 0 && first[2] == 128 && first[3] == 255,
            "indexed raw/RLE reduction averages palette colors instead of indices");
        check(decode(Psd(false, rle, true), 16, permuted, error) && first == permuted,
            "palette permutation preserves equivalent reduced image");
        check(decode(Psd(false, rle, false), 32, full, error) && full.size() == 128 &&
            full[2] == 255 && full[4] == 255, "unscaled indexed pixels preserve red and blue");
        check(decode(Psd(false, rle, false, true), 16, solid, error) && solid.size() == 64 &&
            solid[2] == 255 && solid[0] == 0, "same-color indexed area stays unchanged");
    }
    {
        Bytes pixels; std::wstring error;
        check(!decode(Psd(true, true, false, false, 0x7fffffffu), 16, pixels, error) &&
            error == L"psd-truncated" && pixels.empty(), "PSB advertised row larger than remaining file is rejected before allocation");
        auto over = Psd(true, true, false, false, 66000);
        over.resize(over.size() - 33 + 66000, 0x80);
        error.clear();
        check(!decode(over, 16, pixels, error) && error == L"psd-packed-row-limit",
            "available but excessive packed row is rejected by explicit row budget");
        auto short_row = Psd(true, true, false, false, 1); short_row.resize(short_row.size() - 32);
        error.clear();
        check(!decode(short_row, 16, pixels, error) && error == L"psd-bad-packbits",
            "incomplete PackBits literal cannot be zero-filled into a successful image");
        check(decode(Psd(true, true, false), 16, pixels, error), "normal PSB PackBits still decodes");
    }
    for (const bool lr16 : {false, true}) {
        Bytes pixels; std::wstring error;
        const Bytes expected = {0, 0, 128, 128, 0, 0, 0, 0};
        check(decode(TransparentPsd(lr16 ? 16 : 8, lr16), 16, pixels, error) && pixels == expected,
            lr16 ? "16-bit PSD transparency announced in Lr16 is unmatted from white"
                 : "8-bit PSD merged transparency is unmatted from white, clear stays clear");
    }
    {
        // Grid thumbnails: the embedded JPEG (green; the composite is red) only
        // for large opaque documents whose JPEG covers the requested edge.
        const auto grid = [&](const Bytes& data, UINT edge, UINT& w, UINT& h, Bytes& pixels) {
            const auto file = root / L"opaque.psd";
            UINT stride = 0, sw = 0, sh = 0;
            std::wstring error;
            return Write(file, data) &&
                RasterizePsdFile(file.wstring(), edge, true, pixels, w, h, stride, sw, sh, &error) &&
                pixels.size() >= 4;
        };
        const auto red = [](const Bytes& p) { return p[2] > 200 && p[1] < 60 && p[0] < 60; };
        const auto green = [](const Bytes& p) { return p[1] > 160 && p[2] < 90 && p[0] < 90; };
        Bytes pixels; UINT w = 0, h = 0;
        const Bytes small_jpeg = JpegSolid(64, 32, 0, 255, 0);
        check(!small_jpeg.empty() && grid(OpaquePsd(64, 32, small_jpeg, 64, 32), 256, w, h, pixels) &&
            red(pixels) && w == 64 && h == 32,
            "small opaque PSD grid thumbnail is the exact composite, not the lossy JPEG");
        const Bytes jpeg = JpegSolid(160, 133, 0, 255, 0);
        const Bytes large = OpaquePsd(1200, 1000, jpeg, 160, 133);
        check(!jpeg.empty() && grid(large, 128, w, h, pixels) && green(pixels) && w == 128,
            "large opaque PSD keeps the embedded JPEG when it covers the grid edge");
        check(grid(large, 256, w, h, pixels) && red(pixels) && w == 256 && h == 213,
            "large opaque PSD decodes the composite when its JPEG is smaller than the grid edge");
    }
    {
        // Shell thumbnails (BGRA): the alpha plane is read by content.
        using Px = std::vector<uint8_t>;
        Px cwebp{0, 0, 0, 0, 200, 100, 50, 255, 221, 60, 29, 128};
        NormalizeShellAlpha(cwebp);
        check(cwebp == Px{0, 0, 0, 0, 200, 100, 50, 255, 111, 30, 15, 128},
            "straight Shell alpha with black invisible pixels is premultiplied, clear stays clear");
        Px photoshop{255, 255, 255, 0, 221, 60, 29, 128};
        NormalizeShellAlpha(photoshop);
        check(photoshop == Px{0, 0, 0, 0, 111, 30, 15, 128},
            "straight Shell alpha with white invisible pixels is premultiplied, not shown white");
        Px plain{10, 20, 30, 0, 40, 50, 60, 0};
        NormalizeShellAlpha(plain);
        check(plain == Px{10, 20, 30, 255, 40, 50, 60, 255}, "Shell bitmap without alpha is opaque");
        Px premultiplied{100, 50, 25, 128, 0, 0, 0, 0, 1, 2, 3, 255};
        const Px kept = premultiplied;
        NormalizeShellAlpha(premultiplied);
        check(premultiplied == kept, "premultiplied Shell bitmap is kept");
    }
    {
        // Gray and CMYK composites go through their ICC profile (resource 1039).
        const auto rasterize = [&](const Bytes& data, Bytes& pixels) {
            const auto file = root / L"colour.psd";
            UINT w = 0, h = 0, stride = 0, sw = 0, sh = 0;
            std::wstring error;
            return Write(file, data) &&
                RasterizePsdFile(file.wstring(), 256, false, pixels, w, h, stride, sw, sh, &error) &&
                pixels.size() >= 4;
        };
        Bytes pixels;
        const Bytes gray{0, 128};
        check(rasterize(ModePsd(1, 1, 2, gray, GrayIcc(0x0100)), pixels) && pixels.size() == 8 && pixels[0] == 0 &&
            pixels[4] >= 185 && pixels[4] <= 191 && pixels[5] == pixels[4] && pixels[7] == 255,
            "gray PSD is converted through its embedded ICC profile");
        check(rasterize(ModePsd(1, 1, 2, gray, {}), pixels) && pixels.size() == 8 && pixels[4] == 128,
            "gray PSD without a profile keeps its values");
        Bytes broken = GrayIcc(0x0100);
        broken.resize(140);   // header promises tags beyond the end
        check(rasterize(ModePsd(1, 1, 2, gray, broken), pixels) && pixels.size() == 8 && pixels[4] == 128,
            "gray PSD with a broken profile falls back to its values");
        // RGB documents: a profile other than sRGB converts, an sRGB one is
        // left alone (by name, so the same curve under an sRGB name stays).
        const Bytes rgb{128, 128, 128};
        const auto mid = [&](const Bytes& icc) {
            return rasterize(ModePsd(3, 3, 1, rgb, icc), pixels) && pixels.size() == 4 && pixels[3] == 255 &&
                pixels[0] == pixels[1] && pixels[1] == pixels[2] ? int{pixels[0]} : -1;
        };
        const int wide = mid(RgbIcc("Pulse test linear RGB", 0x0100));
        check(wide >= 185 && wide <= 191, "RGB PSD with a non-sRGB profile is converted to sRGB");
        check(mid(RgbIcc("Pulse sRGB test", 0x0100)) == 128, "RGB PSD tagged sRGB keeps its values");
        const int linear = mid(RgbIcc("sRGB IEC61966-2.1 (Linear RGB Profile)", 0x0100));
        check(linear >= 185 && linear <= 191, "linear sRGB profile is not taken for sRGB");
        check(mid({}) == 128, "RGB PSD without a profile keeps its values");
        // The matrix/TRC path on its own, and against the system transform.
        {
            namespace icc = pulse::preview::icc;
            const Bytes linear_icc = RgbIcc("Pulse test linear RGB", 0x0100);
            icc::MatrixToSrgb matrix;
            unsigned char two[8] = {128, 128, 128, 77, 0, 64, 255, 0};
            const bool ready = matrix.Init(linear_icc.data(), linear_icc.size());
            if (ready) matrix.Apply(two, 2);
            check(ready && two[0] >= 187 && two[0] <= 189 && two[1] == two[0] && two[2] == two[0] && two[3] == 77 &&
                two[7] == 0, "matrix/TRC profile converts without the system transform, alpha untouched");
            const Bytes lut = RgbIcc("Pulse test LUT RGB", 0x0100, true);
            Bytes cut = linear_icc;
            cut.resize(200);
            icc::MatrixToSrgb other;
            check(!other.Init(lut.data(), lut.size()) && !other.Init(cut.data(), cut.size()),
                "profiles with lookup tables or cut short are left to the system transform");
            const Bytes gamma22 = RgbIcc("Pulse test gamma 2.2", 0x0233);
            std::vector<unsigned char> ramp(256 * 4), fast;
            for (int v = 0; v < 256; ++v) {
                ramp[4 * v] = static_cast<unsigned char>(v * 7);
                ramp[4 * v + 1] = static_cast<unsigned char>(255 - v);
                ramp[4 * v + 2] = static_cast<unsigned char>(v);
                ramp[4 * v + 3] = 255;
            }
            fast = ramp;
            int worst = -1;
            using Microsoft::WRL::ComPtr;
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICColorContext> source, target;
            ComPtr<IWICBitmap> bitmap;
            ComPtr<IWICColorTransform> transform;
            std::vector<unsigned char> wcs(ramp.size());
            if (matrix.Init(gamma22.data(), gamma22.size()) &&
                SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->CreateColorContext(&source)) && SUCCEEDED(factory->CreateColorContext(&target)) &&
                SUCCEEDED(source->InitializeFromMemory(gamma22.data(), static_cast<UINT>(gamma22.size()))) &&
                SUCCEEDED(target->InitializeFromExifColorSpace(1)) &&
                SUCCEEDED(factory->CreateBitmapFromMemory(256, 1, GUID_WICPixelFormat32bppBGRA, 1024,
                                                          static_cast<UINT>(ramp.size()), ramp.data(), &bitmap)) &&
                SUCCEEDED(factory->CreateColorTransformer(&transform)) &&
                SUCCEEDED(transform->Initialize(bitmap.Get(), source.Get(), target.Get(),
                                                GUID_WICPixelFormat32bppBGRA)) &&
                SUCCEEDED(transform->CopyPixels(nullptr, 1024, static_cast<UINT>(wcs.size()), wcs.data()))) {
                matrix.Apply(fast.data(), 256);
                worst = 0;
                for (size_t i = 0; i < fast.size(); ++i)
                    worst = (std::max)(worst, std::abs(int{fast[i]} - int{wcs[i]}));
            }
            printf("matrix vs system transform, gamma 2.2 ramp: largest difference %d\n", worst);
            check(worst >= 0 && worst <= 3, "matrix/TRC path matches the system transform");
        }
        // Full cyan ink, stored inverted: subtracting ink showed (0,255,255).
        wchar_t sys[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        const bool swop = n && n < MAX_PATH &&
            GetFileAttributesW((std::wstring(sys, n) + L"\\spool\\drivers\\color\\RSWOP.icm").c_str()) !=
                INVALID_FILE_ATTRIBUTES;
        check(rasterize(ModePsd(4, 4, 1, Bytes{0, 255, 255, 255}, {}), pixels) &&
            (swop ? pixels[0] > 150 && pixels[1] < 220 && pixels[2] < 80 : pixels[1] == 255),
            "CMYK PSD without a profile is converted as SWOP, not by subtracting ink");
    }
    uint32_t bytes = 0;
    const std::string cells =
        "<c r=\"A1\" s=\"1\"><v>0</v></c><c r=\"B1\" s=\"1\"><v>1</v></c>"
        "<c r=\"C1\" s=\"1\"><v>59</v></c><c r=\"D1\" s=\"1\"><v>60</v></c>"
        "<c r=\"E1\" s=\"1\"><v>61</v></c><c r=\"F1\" s=\"2\"><f>ignored</f><v>45000.5</v></c>"
        "<c r=\"G1\"><v>45000</v></c>";
    for (const std::string flag : {"", " date1904=\"0\"", " date1904=\"false\"", " date1904=\"1\"", " date1904=\"true\""}) {
        const bool modern = flag.find("\"1\"") != std::string::npos || flag.find("\"true\"") != std::string::npos;
        const auto file = root / L"dates.xlsx";
        check(Workbook(file, flag, cells), "write private date workbook");
        std::wstring payload;
        const bool made = MakeXlsxTable(file.wstring(), payload, bytes);
        const auto row = Fields(payload, L'R');
        check(made && row.size() == 8 &&
            row[1] == (modern ? L"1904-01-01" : L"1899-12-31") &&
            row[2] == (modern ? L"1904-01-02" : L"1900-01-01") &&
            row[3] == (modern ? L"1904-02-29" : L"1900-02-28") &&
            row[4] == (modern ? L"1904-03-01" : L"1900-02-29") &&
            row[5] == (modern ? L"1904-03-02" : L"1900-03-01"),
            "workbook date system and early serial boundaries are honored");
        check(row.size() == 8 && row[6] == (modern ? L"2027-03-16 12:00" : L"2023-03-15 12:00") &&
            row[7] == L"45000", "cached numeric date/time uses workbook epoch while plain number stays numeric");
        const auto first_row = row;
        check(MakeXlsxTable(file.wstring(), payload, bytes, 1) && Fields(payload, L'R') == first_row,
            "lazy sheet selection retains workbook date system");
    }
    for (size_t count : {size_t{1999}, size_t{2000}, size_t{2001}}) {
        for (bool surrogate : {false, true}) {
            std::wstring text(count - (surrogate ? 2 : 0), L'x');
            std::string xml(count - (surrogate ? 2 : 0), 'x');
            if (surrogate) { text += L"\U0001F600"; xml += "\xF0\x9F\x98\x80"; }
            Bytes utf16{0xff, 0xfe};
            for (wchar_t c : text) { utf16.push_back(static_cast<unsigned char>(c)); utf16.push_back(static_cast<unsigned char>(c >> 8)); }
            const auto csv = root / L"clip.csv";
            check(Write(csv, utf16), "write private UTF16 CSV boundary");
            bool truncated = false; pulse::ipc::PreviewTextEncoding encoding{};
            std::wstring payload;
            const bool csv_ok = MakeCsvTable(csv.wstring(), L".csv", payload, bytes, truncated, encoding);
            auto row = Fields(payload, L'R');
            check(csv_ok && truncated == (count > 2000) && ValidUtf16(payload) &&
                row.size() == 2 && (count > 2000 ? row[1].ends_with(L"\x2026") : row[1] == text),
                "CSV exact cell boundary marks loss and never splits a surrogate pair");
            for (bool shared : {false, true}) {
                const auto xlsx = root / L"clip.xlsx";
                const auto cell = shared ? "<c r=\"A1\" t=\"s\"><v>0</v></c>" :
                    "<c r=\"A1\" t=\"inlineStr\"><is><t>" + xml + "</t></is></c>";
                check(Workbook(xlsx, "", cell, xml) && MakeXlsxTable(xlsx.wstring(), payload, bytes),
                    "read private inline/shared-string workbook");
                const auto sheet = Fields(payload, L'S'); row = Fields(payload, L'R');
                check(sheet.size() > 5 && sheet[5] == (count > 2000 ? L"1" : L"0") &&
                    ValidUtf16(payload) && row.size() == 2 &&
                    (count > 2000 ? row[1].ends_with(L"\x2026") : row[1] == text),
                    "XLSX inline/shared cell clipping propagates completeness and preserves UTF16");
            }
        }
    }
    {
        // Parts named by relationships rather than by convention, cells
        // without r, and formulas without a cached value (shared members too).
        const auto file = root / L"renamed.xlsx";
        const std::string kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
        const std::string sheet =
            "<worksheet><sheetData>"
            "<row r=\"1\"><c t=\"s\"><v>0</v></c><c t=\"s\"><v>1</v></c><c><v>2</v></c></row>"
            "<row r=\"2\"><c r=\"A2\"><v>3</v></c><c r=\"B2\"><f t=\"shared\" ref=\"B2:B3\" si=\"0\">A2*2</f></c>"
            "<c r=\"C2\"><f>SUM($A$2:A2)</f></c></row>"
            "<row r=\"3\"><c r=\"A3\"><v>4</v></c><c r=\"B3\"><f t=\"shared\" si=\"0\"/></c></row>"
            "</sheetData></worksheet>";
        check(preview_fixture::Zip(file, {
            {"_rels/.rels", "<Relationships><Relationship Id=\"rId1\" Type=\"" + kRel +
                "officeDocument\" Target=\"book/main.xml\"/></Relationships>"},
            {"book/main.xml", "<workbook xmlns:r=\"" + kRel.substr(0, kRel.size() - 1) + "\"><sheets><sheet name=\"Data\" r:id=\"rId1\"/></sheets></workbook>"},
            {"book/_rels/main.xml.rels", "<Relationships><Relationship Id=\"rId1\" Type=\"" + kRel +
                "worksheet\" Target=\"sheets/data.xml\"/><Relationship Id=\"rId2\" Type=\"" + kRel +
                "sharedStrings\" Target=\"/book/strings.xml\"/></Relationships>"},
            {"book/sheets/data.xml", sheet},
            {"book/strings.xml", "<sst><si><t>Name</t></si><si><t>Total</t></si></sst>"}}),
            "write private workbook with renamed parts");
        std::wstring payload;
        const bool made = MakeXlsxTable(file.wstring(), payload, bytes);
        std::vector<std::vector<std::wstring>> rows;
        for (size_t at = 0; (at = payload.find(L"\nR\t", at)) != std::wstring::npos; ++at)
            rows.push_back(Fields(payload.substr(at + 1), L'R'));
        check(made && rows.size() == 3 && rows[0].size() == 4 && rows[0][1] == L"Name" &&
            rows[0][2] == L"Total" && rows[0][3] == L"2",
            "XLSX parts follow relationships; cells without r take the next column");
        check(rows.size() == 3 && rows[1].size() == 4 && rows[2].size() == 4 &&
            rows[1][2] == L"=A2*2" && rows[1][3] == L"=SUM($A$2:A2)" && rows[2][2] == L"=A3*2",
            "XLSX formulas without a cached value show their text, shared members shifted");
        SheetModel model; std::wstring model_error;
        check(ReadXlsxModel(file.wstring(), 4, 4, model, &model_error) && model.grid.size() == 4 &&
            model.grid[0][0] == L"Name" && model.grid[0][1] == L"Total" &&
            model.grid[1][1] == L"=A2*2" && model.grid[1][2] == L"=SUM($A$2:A2)" &&
            model.grid[2][1] == L"=A3*2",
            "sheet sketch follows renamed parts, cells without r and uncalculated formulas");
        check(ShiftFormula(L"SUM($A$1:B2)*Sheet2!C3+\"A1\"&'Q1 data'!D4+LOG10(A1)+T[Col1]", 2, 1) ==
                L"SUM($A$1:C4)*Sheet2!D5+\"A1\"&'Q1 data'!E6+LOG10(B3)+T[Col1]" &&
            ShiftFormula(L"A1+$B$2", -1, 0) == L"#REF!+$B$2" &&
            ResolvePartTarget("xl/workbook.xml", "../custom/./s.xml") == "custom/s.xml",
            "shared formula references shift like Excel; part targets resolve");
    }
    const auto csv = root / L"source.csv";
    { std::ofstream file(csv); for (int i = 0; i < 700; ++i) file << std::string(100, 'a') << '\n'; }
    std::wstring payload; bool truncated = false; pulse::ipc::PreviewTextEncoding encoding{};
    check(MakeCsvTable(csv.wstring(), L".csv", payload, bytes, truncated, encoding) && truncated,
        "CSV source-view character cap also reports incomplete content");
    std::error_code error;
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"preview-data-"))
        fs::remove_all(root, error);
    check(!error && !fs::exists(root), "private preview fixtures removed");
    CoUninitialize();
    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
