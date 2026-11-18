#include "preview_host_client.h"
#include "../preview_host/image_frames.h"
#include "../preview_host/svg_raster.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <chrono>
#include <psapi.h>

using Microsoft::WRL::ComPtr;
using Bytes = std::vector<unsigned char>;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!ok) ++failures;
}
void U32(Bytes& bytes, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<unsigned char>(value >> shift));
}
void Chunk(Bytes& file, const char* type, const Bytes& data) {
    U32(file, static_cast<uint32_t>(data.size()));
    const size_t start = file.size();
    file.insert(file.end(), type, type + 4);
    file.insert(file.end(), data.begin(), data.end());
    uint32_t crc = 0xffffffffu;
    for (size_t i = start; i < file.size(); ++i) {
        crc ^= file[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    U32(file, ~crc);
}
Bytes Animation(uint32_t actual, uint32_t declared) {
    Bytes file{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes ihdr;
    U32(ihdr, 1); U32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    Chunk(file, "IHDR", ihdr);
    Bytes control; U32(control, declared); U32(control, 0);
    Chunk(file, "acTL", control);
    // A zlib stream with one uncompressed DEFLATE block: filter byte + red RGB pixel.
    const Bytes raw{0, 255, 0, 0};
    Bytes zlib{0x78, 0x01, 0x01, 4, 0, 0xfb, 0xff};
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    uint32_t a = 1, b = 0;
    for (const auto value : raw) { a = (a + value) % 65521; b = (b + a) % 65521; }
    U32(zlib, (b << 16) | a);
    uint32_t sequence = 0;
    for (uint32_t i = 0; i < actual; ++i) {
        Bytes frame; U32(frame, sequence++); U32(frame, 1); U32(frame, 1);
        U32(frame, 0); U32(frame, 0);
        frame.insert(frame.end(), {0, 1, 0, 10, 0, 0});
        Chunk(file, "fcTL", frame);
        if (i == 0) Chunk(file, "IDAT", zlib);
        else {
            Bytes data; U32(data, sequence++); data.insert(data.end(), zlib.begin(), zlib.end());
            Chunk(file, "fdAT", data);
        }
    }
    Chunk(file, "IEND", {});
    return file;
}
Bytes BlendedAnimation() {
    Bytes file{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes ihdr; U32(ihdr, 17); U32(ihdr, 2);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // RGBA, no filtering/interlace.
    Chunk(file, "IHDR", ihdr);
    Bytes control; U32(control, 2); U32(control, 0);
    Chunk(file, "acTL", control);
    uint32_t sequence = 0;
    for (unsigned frame_index = 0; frame_index < 2; ++frame_index) {
        Bytes raw;
        for (unsigned y = 0; y < 2; ++y) {
            raw.push_back(0);
            for (unsigned x = 0; x < 17; ++x) {
                if (!frame_index) {
                    raw.insert(raw.end(), {static_cast<unsigned char>(40 + x),
                        static_cast<unsigned char>(80 + y), static_cast<unsigned char>(120 + x + y), 255});
                } else {
                    const unsigned char alpha[] = {0, 128, 255};
                    // RGB 0/255 makes WIC premultiplication exact for every alpha.
                    raw.insert(raw.end(), {255, 0, 255, alpha[(x + y) % 3]});
                }
            }
        }
        const auto length = static_cast<uint16_t>(raw.size());
        const auto inverse = static_cast<uint16_t>(~length);
        Bytes zlib{0x78, 0x01, 0x01, static_cast<unsigned char>(length),
            static_cast<unsigned char>(length >> 8), static_cast<unsigned char>(inverse),
            static_cast<unsigned char>(inverse >> 8)};
        zlib.insert(zlib.end(), raw.begin(), raw.end());
        uint32_t a = 1, b = 0;
        for (const auto value : raw) { a = (a + value) % 65521; b = (b + a) % 65521; }
        U32(zlib, (b << 16) | a);
        Bytes frame; U32(frame, sequence++); U32(frame, 17); U32(frame, 2);
        U32(frame, 0); U32(frame, 0);
        frame.insert(frame.end(), {0, 1, 0, 10, 0, static_cast<unsigned char>(frame_index)});
        Chunk(file, "fcTL", frame);
        if (!frame_index) Chunk(file, "IDAT", zlib);
        else {
            Bytes data; U32(data, sequence++); data.insert(data.end(), zlib.begin(), zlib.end());
            Chunk(file, "fdAT", data);
        }
    }
    Chunk(file, "IEND", {});
    return file;
}

bool BlendedPixels(const pulse_test::Result& result, bool overlay) {
    if (result.response.kind != pulse::ipc::PreviewContentKind::Bitmap ||
        result.response.width != 17 || result.response.height != 2 || result.response.stride < 17 * 4 ||
        result.pixels.size() < static_cast<size_t>(result.response.stride) * 2) {
        std::cout << "  blend geometry/payload mismatch; expected Bitmap 17x2 stride>=68 bytes>=stride*2\n";
        return false;
    }
    for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 17; ++x) {
        const unsigned base[] = {120 + x + y, 80 + y, 40 + x, 255};
        const unsigned alpha[] = {0, 128, 255};
        const unsigned a = alpha[(x + y) % 3];
        const unsigned source[] = {a, 0, a, a};
        for (unsigned channel = 0; channel < 4; ++channel) {
            const unsigned expected = overlay ? source[channel] + (base[channel] * (255 - a) + 127) / 255 : base[channel];
            const unsigned actual = result.pixels[static_cast<size_t>(y) * result.response.stride + x * 4 + channel];
            if (actual != expected) {
                std::cout << "  blend first mismatch x=" << x << " y=" << y << " channel(BGRA)=" << channel
                    << " actual=" << actual << " expected=" << expected << " source_alpha=" << a << '\n';
                return false;
            }
        }
    }
    return true;
}

bool Write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}
Bytes StoredZlib(const Bytes& raw) {
    Bytes zlib{0x78, 0x01};
    for (size_t at = 0; at < raw.size();) {
        const auto length = static_cast<uint16_t>((std::min)(size_t{65535}, raw.size() - at));
        const auto inverse = static_cast<uint16_t>(~length);
        zlib.insert(zlib.end(), {static_cast<unsigned char>(at + length == raw.size()),
            static_cast<unsigned char>(length), static_cast<unsigned char>(length >> 8),
            static_cast<unsigned char>(inverse), static_cast<unsigned char>(inverse >> 8)});
        zlib.insert(zlib.end(), raw.begin() + at, raw.begin() + at + length);
        at += length;
    }
    uint32_t a = 1, b = 0;
    for (const auto value : raw) { a = (a + value) % 65521; b = (b + a) % 65521; }
    U32(zlib, (b << 16) | a);
    return zlib;
}
Bytes SolidPngStream(unsigned width, unsigned height, unsigned char red, unsigned char green, unsigned char blue) {
    Bytes raw;
    raw.reserve(static_cast<size_t>(height) * (width * 4 + 1));
    for (unsigned y = 0; y < height; ++y) {
        raw.push_back(0);
        for (unsigned x = 0; x < width; ++x) raw.insert(raw.end(), {red, green, blue, 255});
    }
    return StoredZlib(raw);
}
Bytes PngHeader(unsigned width, unsigned height) {
    Bytes file{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes ihdr; U32(ihdr, width); U32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    Chunk(file, "IHDR", ihdr);
    return file;
}
Bytes MemoryAnimation() {
    Bytes file = PngHeader(2048, 2048);
    Bytes control; U32(control, 3); U32(control, 0); Chunk(file, "acTL", control);
    uint32_t sequence = 0;
    for (unsigned index = 0; index < 3; ++index) {
        const unsigned extent = index == 0 ? 2048 : index == 1 ? 32 : 16;
        Bytes frame; U32(frame, sequence++); U32(frame, extent); U32(frame, extent);
        U32(frame, index == 0 ? 0 : index == 1 ? 128 : 2032);
        U32(frame, index == 0 ? 0 : index == 1 ? 320 : 2032);
        frame.insert(frame.end(), {0, 1, 0, 10, static_cast<unsigned char>(index == 1 ? 2 : 0), 0});
        Chunk(file, "fcTL", frame);
        const Bytes stream = index == 0 ? SolidPngStream(extent, extent, 40, 80, 120) :
            index == 1 ? SolidPngStream(extent, extent, 0, 255, 0) : SolidPngStream(extent, extent, 255, 0, 0);
        if (!index) Chunk(file, "IDAT", stream);
        else { Bytes data; U32(data, sequence++); data.insert(data.end(), stream.begin(), stream.end()); Chunk(file, "fdAT", data); }
    }
    Chunk(file, "IEND", {});
    return file;
}
bool MemoryPixels(const pulse_test::Result& result, unsigned frame, bool is_small) {
    const unsigned width = is_small ? 8 : 512;
    if (result.response.status != 0 || result.response.kind != pulse::ipc::PreviewContentKind::Bitmap ||
        result.response.width != width || result.response.height != width || result.response.stride < width * 4 ||
        result.pixels.size() < static_cast<size_t>(result.response.stride) * width ||
        result.response.frame_count != (is_small ? 1u : 3u)) return false;
    for (unsigned y = 0; y < width; ++y) for (unsigned x = 0; x < width; ++x) {
        unsigned expected[] = {120, 80, 40, 255};
        if (is_small) { expected[0] = 30; expected[1] = 20; expected[2] = 10; }
        else if (frame == 1 && x >= 32 && x < 40 && y >= 80 && y < 88) {
            expected[0] = 0; expected[1] = 255; expected[2] = 0;
        } else if (frame == 2 && x >= 508 && y >= 508) {
            expected[0] = 0; expected[1] = 0; expected[2] = 255;
        }
        for (unsigned c = 0; c < 4; ++c) {
            const unsigned actual = result.pixels[static_cast<size_t>(y) * result.response.stride + x * 4 + c];
            if (actual != expected[c]) {
                std::cout << "  memory pixel mismatch x=" << x << " y=" << y << " channel=" << c
                    << " actual=" << actual << " expected=" << expected[c] << '\n';
                return false;
            }
        }
    }
    return true;
}
int ApngMemoryOnly() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { Check(false, "memory COM initialization"); return 1; }
    const auto parent = std::filesystem::absolute("bench_data"); std::filesystem::create_directories(parent);
    const auto directory = parent / ("apng-memory-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    if (!CreateDirectoryW(directory.c_str(), nullptr)) { CoUninitialize(); return 2; }
    Check(Write(directory / "large.png", MemoryAnimation()), "write 2048-square APNG with multi-block DEFLATE and local dispose-previous patch");
    Bytes thumbnail_file = PngHeader(8, 8); Chunk(thumbnail_file, "IDAT", SolidPngStream(8, 8, 10, 20, 30)); Chunk(thumbnail_file, "IEND", {});
    Check(Write(directory / "small.png", thumbnail_file), "write small static PNG switch target");
    pulse_test::Host host;
    const bool started = host.Start(); Check(started, "start isolated APNG memory host");
    if (started) {
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, host.ProcessId());
        const auto sample = [&](const char* stage) {
            Sleep(200); // The IPC ack has been consumed and per-request locals can retire.
            PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
            bool valid = process && GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
            std::vector<unsigned char> storage(4 * 1024 * 1024);
            valid = valid && QueryWorkingSet(process, storage.data(), static_cast<DWORD>(storage.size()));
            size_t private_ws = 0;
            if (valid) {
                SYSTEM_INFO system{}; GetSystemInfo(&system);
                const auto* ws = reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(storage.data());
                for (ULONG_PTR i = 0; i < ws->NumberOfEntries; ++i)
                    if (!ws->WorkingSetInfo[i].Shared) private_ws += system.dwPageSize;
            }
            std::cout << "MEMORY stage=" << stage << " pid=" << host.ProcessId() << " private_commit=" << counters.PrivateUsage
                << " private_ws=" << private_ws << " valid=" << valid << '\n';
            Check(valid, "sample child private commit and resident pages");
        };
        const auto request = [&](unsigned frame, bool is_small, const char* label) {
            pulse_test::Result result;
            const auto begin = std::chrono::steady_clock::now();
            const bool received = host.Request((directory / (is_small ? "small.png" : "large.png")).wstring(), result,
                MAXDWORD, 1024, pulse::ipc::PreviewRequestKind::Content, 0, frame);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            uint64_t checksum = 1469598103934665603ull;
            for (const auto byte : result.pixels) { checksum ^= byte; checksum *= 1099511628211ull; }
            std::cout << "REQUEST " << label << " frame=" << frame << " received=" << received << " ms=" << ms
                << " status=" << result.response.status << " kind=" << static_cast<unsigned>(result.response.kind)
                << " width=" << result.response.width << " height=" << result.response.height
                << " count=" << result.response.frame_count << " checksum=" << checksum << '\n';
            if (!result.error.empty()) std::wcout << L"  error=" << result.error << L'\n';
            Check(received && MemoryPixels(result, frame, is_small), label);
        };
        sample("startup-idle");
        request(0, false, "initial full canvas pixels");
        request(1, false, "local dispose-previous patch pixels");
        sample("large-patch-idle");
        request(2, false, "third frame restores prior patch before drawing edge patch");
        request(0, false, "backward seek restores full background");
        request(2, false, "jump from first to third applies disposal correctly");
        request(1, false, "reverse seek reconstructs local patch");
        request(2, false, "repeated forward seek restores local patch");
        sample("large-reused-idle");
        request(0, true, "switch to small static PNG has correct pixels");
        sample("small-idle");
        request(0, true, "repeat small static PNG remains correct");
        sample("small-reused-idle");
        request(2, false, "return to animation with distant seek preserves pixels");
        sample("large-returned-idle");
        if (process) CloseHandle(process);
        host.Stop();
    }
    std::error_code cleanup_error; std::filesystem::remove_all(directory, cleanup_error);
    Check(!cleanup_error, "clean isolated APNG memory fixtures");
    CoUninitialize();
    return failures ? 1 : 0;
}

bool Tiff(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatTiff, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    for (unsigned page = 0; page < 2; ++page) {
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        Bytes pixel = page == 0 ? Bytes{0, 0, 255} : Bytes{255, 0, 0};
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(1, 1)) || FAILED(frame->SetPixelFormat(&format)) ||
            !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) ||
            FAILED(frame->WritePixels(1, 3, 3, pixel.data())) || FAILED(frame->Commit())) return false;
    }
    return SUCCEEDED(encoder->Commit());
}
struct SvgImage {
    Bytes pixels;
    UINT width = 0, height = 0, stride = 0, source_width = 0, source_height = 0;
    std::wstring error;
    unsigned Channel(UINT x, UINT y, UINT channel) const {
        return x < width && y < height ? pixels[static_cast<size_t>(y) * stride + x * 4 + channel] : 0;
    }
};
bool Svg(const std::filesystem::path& directory, std::string_view source, SvgImage& image) {
    const auto path = directory / "case.svg";
    if (!Write(path, Bytes(source.begin(), source.end()))) return false;
    image = {};
    return pulse::preview::RasterizeSvgFile(path.wstring(), 512, image.pixels, image.width, image.height,
        image.stride, image.source_width, image.source_height, &image.error);
}
void SvgCases(const std::filesystem::path& directory) {
    SvgImage image;
    const std::string simple = "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='80'><rect x='20' y='20' width='40' height='20' fill='red'/><!-- <text> --><title>中文标题</title></svg>";
    Check(Svg(directory, simple, image) && image.Channel(25, 25, 2) > 240 &&
        image.Channel(25, 25, 0) < 10 && image.Channel(5, 5, 3) == 0,
        "system SVG paints actual red shape and ignores only harmless title/comment");
    Check(Write(directory / "simple.svg", Bytes(simple.begin(), simple.end())), "write system SVG fixture");
    Check(Svg(directory, "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='80'><defs><clipPath id='c'><rect x='20' y='20' width='20' height='20'/></clipPath></defs><rect x='20' y='20' width='60' height='20' fill='red' clip-path='url(#c)'/></svg>", image) &&
        image.Channel(25, 25, 3) > 240 && image.Channel(45, 25, 3) == 0, "system SVG clipPath removes outside pixels");
    Check(Svg(directory, "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='30'><defs><linearGradient id='g'><stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs><rect width='100' height='30' fill='url(#g)'/></svg>", image) &&
        image.Channel(10, 15, 2) > image.Channel(10, 15, 0) && image.Channel(90, 15, 0) > image.Channel(90, 15, 2), "system SVG gradient preserves colors");
    const std::string chinese = "<svg xmlns='http://www.w3.org/2000/svg'><text>中文预览 VISIBLE_LABEL</text></svg>";
    Check(!Svg(directory, chinese, image) && image.error == L"svg-unsupported:element:text" && image.pixels.empty(),
        "SVG text is rejected before Direct2D can omit it");
    Check(Write(directory / "text.svg", Bytes(chinese.begin(), chinese.end())), "write multilingual source fallback fixture");
    const std::wstring utf16 = L"<svg xmlns='http://www.w3.org/2000/svg'><text>中文预览</text></svg>";
    Bytes utf16_bytes{0xff, 0xfe};
    for (wchar_t value : utf16) { utf16_bytes.push_back(static_cast<unsigned char>(value)); utf16_bytes.push_back(static_cast<unsigned char>(value >> 8)); }
    const auto utf16_path = directory / "utf16.svg";
    Check(Write(utf16_path, utf16_bytes) && !pulse::preview::RasterizeSvgFile(utf16_path.wstring(), 512,
        image.pixels, image.width, image.height, image.stride, image.source_width, image.source_height, &image.error) &&
        image.error == L"svg-unsupported:element:text" && image.pixels.empty(), "UTF-16 SVG cannot bypass unsupported-text detection");
    for (const auto& item : std::vector<std::pair<std::string, std::wstring>>{
        {"<svg><style>rect{fill:red}</style><rect/></svg>", L"svg-unsupported:element:style"},
        {"<svg><rect style='filter:blur(3px)'/></svg>", L"svg-unsupported:attribute:style"},
        {"<svg><filter id='f'/></svg>", L"svg-unsupported:element:filter"},
        {"<svg><script>VISIBLE_LABEL</script></svg>", L"svg-unsupported:element:script"},
        {"<svg><animate attributeName='x'/></svg>", L"svg-unsupported:element:animate"},
        {"<svg><foreignObject/></svg>", L"svg-unsupported:element:foreignObject"},
        {"<svg xmlns:xlink='http://www.w3.org/1999/xlink'><image xlink:href='file:///C:/private.png'/></svg>", L"svg-unsupported:attribute:href"},
        {"<svg><rect width='2em'/></svg>", L"svg-unsupported:attribute:width"},
        {"<svg xmlns:x='urn:test'><rect x:fill='red'/></svg>", L"svg-unsupported:attribute-namespace"},
        {"<svg><rect></svg>", L"svg-parse-failed"}})
        Check(!Svg(directory, item.first, image) && image.pixels.empty() && image.error == item.second,
            "system SVG rejects unsupported content with explicit reason and no partial pixels");
    std::string nested = "<svg>";
    for (int i = 0; i < 129; ++i) nested += "<g>";
    for (int i = 0; i < 129; ++i) nested += "</g>";
    nested += "</svg>";
    Check(!Svg(directory, nested, image) && image.error == L"svg-budget-exceeded", "SVG nesting budget reports explicit rejection");
    Check(!Svg(directory, std::string(8u * 1024 * 1024 + 1, ' '), image) && image.error == L"svg-budget-exceeded",
        "SVG source byte budget rejects before renderer allocation");
}
void SvgIpcCases(pulse_test::Host& host, const std::filesystem::path& directory) {
    pulse_test::Result result;
    Check(host.Request((directory / "simple.svg").wstring(), result) &&
        result.response.kind == pulse::ipc::PreviewContentKind::Bitmap && !result.pixels.empty() && result.error.empty(),
        "system SVG shapes render through host IPC");
    Check(host.Request((directory / "text.svg").wstring(), result) &&
        result.response.kind == pulse::ipc::PreviewContentKind::Text && result.text.find(L"中文预览 VISIBLE_LABEL") != std::wstring::npos &&
        result.error == L"svg-unsupported:element:text", "SVG source fallback preserves text and warning through IPC");
    Check(host.Request((directory / "utf16.svg").wstring(), result) &&
        result.response.kind == pulse::ipc::PreviewContentKind::Text && result.text.find(L"中文预览") != std::wstring::npos &&
        result.error == L"svg-unsupported:element:text", "UTF-16 SVG source and warning survive host IPC");
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--apng-memory-only") return ApngMemoryOnly();
    const bool apng_only = argc > 1 && std::string_view(argv[1]) == "--apng-only";
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { Check(false, "COM initialization"); return 1; }
    const auto directory = std::filesystem::absolute(std::filesystem::path("bench_data") /
        ("image-integrity-" + std::to_string(GetCurrentProcessId())));
    std::filesystem::create_directories(directory);
    if (!apng_only) SvgCases(directory);
    if (argc > 1 && std::string_view(argv[1]) == "--svg-only") {
        pulse_test::Host svg_host;
        const bool svg_started = svg_host.Start();
        Check(svg_started, "start SVG preview host");
        if (svg_started) SvgIpcCases(svg_host, directory);
        svg_host.Stop();
        std::error_code cleanup_error;
        std::filesystem::remove_all(directory, cleanup_error);
        Check(!cleanup_error, "clean SVG fixtures");
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (!apng_only) Check(Tiff(directory / "pages.tiff"), "encode two distinct TIFF pages");
    const auto capped = Animation(4097, 4097);
    pulse::preview::ApngInfo info;
    Check(pulse::preview::ParseApng(capped, info) && info.declared_frames == 4097 && info.observed_frames == 4097 &&
        info.frames.size() == 4096 && info.incomplete_reason == L"frame-limit", "APNG retains declared and bounded playable frame counts");
    Check(Write(directory / "capped.png", capped), "write bounded APNG fixture");
    auto truncated = Animation(2, 3);
    Check(pulse::preview::ParseApng(truncated, info) && info.frames.size() == 2 &&
        info.incomplete_reason == L"frame-count-mismatch", "APNG declared count mismatch is explicit");
    Check(Write(directory / "mismatch.png", truncated), "write mismatched APNG fixture");
    truncated.resize(truncated.size() - 12);
    Check(pulse::preview::ParseApng(truncated, info) && info.incomplete_reason == L"truncated-data", "APNG missing IEND reports truncation");
    Check(Write(directory / "limited.png", Animation(2, 2)), "write APNG limit fallback fixture");
    std::filesystem::resize_file(directory / "limited.png", 97ull * 1024 * 1024);
    Check(Write(directory / "blend.png", BlendedAnimation()), "write 17x2 APNG over-blend fixture");
    pulse_test::Host host;
    const bool started = host.Start();
    Check(started, "start isolated preview host");
    if (started) {
        pulse_test::Result result;
        for (uint32_t frame : {0u, 1u}) {
            result = {};
            // Small request sizes select the grid/static path even without the grid flag.
            const bool received = host.Request((directory / "blend.png").wstring(), result, MAXDWORD, 1024,
                pulse::ipc::PreviewRequestKind::Content, 0, frame);
            std::cout << "  blend request frame=" << frame << " flags=0 pixels=1024 received=" << received
                << " status=" << result.response.status << " kind=" << static_cast<unsigned>(result.response.kind)
                << " width=" << result.response.width << " height=" << result.response.height
                << " stride=" << result.response.stride << " count=" << result.response.frame_count
                << " response_flags=" << result.response.flags << " payload_bytes=" << result.pixels.size() << '\n';
            if (!result.error.empty()) std::wcout << L"  blend error=" << result.error << L'\n';
            const bool pixels_ok = received && BlendedPixels(result, frame == 1);
            Check(received && result.response.frame_count == 2 && pixels_ok,
                frame == 0 ? "APNG IPC frame 0 retains nonzero opaque background" :
                             "APNG IPC frame 1 over-blends alpha 0/128/255 across vector blocks and row tail");
        }
        if (!apng_only) SvgIpcCases(host, directory);
        if (!apng_only) for (uint32_t page : {0u, 1u, 99u}) {
            result = {};
            const bool received = host.Request((directory / "pages.tiff").wstring(), result, MAXDWORD, 128,
                pulse::ipc::PreviewRequestKind::Content, 0, page);
            const bool blue = page != 0;
            Check(received && result.response.kind == pulse::ipc::PreviewContentKind::Bitmap &&
                result.response.frame_count == 2 && result.response.frame_delay_ms == 0 && result.pixels.size() >= 4 &&
                result.pixels[blue ? 0 : 2] > 240 && result.pixels[blue ? 2 : 0] < 10 &&
                result.text.find(blue ? L"F\t2\t2\t1" : L"F\t2\t2\t0") != std::wstring::npos,
                "TIFF frame request selects actual page, clamps and never animates");
        }
        for (const auto& item : std::vector<std::pair<std::wstring, std::wstring>>{
            {L"capped.png", L"frame-limit"}, {L"mismatch.png", L"frame-count-mismatch"}, {L"limited.png", L"file-limit"}}) {
            result = {};
            const bool received = host.Request((directory / item.first).wstring(), result);
            const uint32_t count = item.first == L"capped.png" ? 4096u : item.first == L"limited.png" ? 1u : 2u;
            Check(received && result.response.kind == pulse::ipc::PreviewContentKind::Bitmap &&
                result.response.frame_count == count && (result.response.flags & pulse::ipc::kPreviewFlagTruncated) &&
                result.error == L"image-preview-incomplete:" + item.second &&
                result.text.find(L"W\t" + item.second) != std::wstring::npos,
                "APNG IPC preserves warning and honest playable count");
            if (!result.error.empty()) std::wcout << L"  reason: " << result.error << L'\n';
            if (count == 1) Check(result.response.frame_delay_ms == 0 &&
                result.text.find(L"T\tstatic-fallback") != std::wstring::npos &&
                result.text.find(L"F\t1\t2\t0") != std::wstring::npos, "static APNG fallback preserves declared count as metadata only");
        }
        host.Stop();
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    Check(!cleanup_error, "clean isolated fixtures");
    CoUninitialize();
    return failures ? 1 : 0;
}
