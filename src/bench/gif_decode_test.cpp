#include "../preview_host/gif_decoder.h"
#include "gif_fixture.h"
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
#include <algorithm>
#include <thread>
#include <chrono>
using namespace pulse::preview;
using Bytes = std::vector<uint8_t>;
namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; failures += !ok; }
using pulse_test::gif_fixture::Word;
using pulse_test::gif_fixture::Frame;
Bytes Animation(uint32_t count = 4, uint8_t base = 0) {
    Bytes out{'G','I','F','8','9','a'}; Word(out, 4); Word(out, 2);
    out.insert(out.end(), {0x81,0,0, 255,0,0, 0,255,0, 0,0,255, 255,255,255});
    out.insert(out.end(), {0x21,0xff,11,'N','E','T','S','C','A','P','E','2','.','0',3,1,2,0,0});
    Frame(out, 0, 4, 2, base, 1);
    for (uint32_t i = 1; i < count; ++i) Frame(out, i % 4, 1, 1, static_cast<uint8_t>(i % 4), i % 4 == 1 ? 2 : i % 4 == 2 ? 3 : 1);
    out.push_back(0x3b); return out;
}
bool Write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); return out.good();
}
bool Pixel(const GifFrame& frame, UINT x, std::array<uint8_t, 4> value) {
    return frame.pixels.size() >= (size_t(x) + 1) * 4 && std::equal(value.begin(), value.end(), frame.pixels.begin() + x * 4);
}
}
int MemoryRegression() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = std::filesystem::absolute(L"bench_data");
    std::filesystem::create_directories(root);
    const auto folder = root / (L"gif-memory-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(folder.c_str(), nullptr)) return 2;
    {
        struct Patch { uint32_t x, y, w, h; uint8_t color, disposal; bool transparent; };
        const Patch patches[] = {
            {0,0,16,16,0,1,false}, {0,0,16,16,1,3,false},
            {3,5,4,6,2,3,false}, {14,13,2,3,3,3,true},
            {13,12,3,4,1,3,false}, {0,0,1,1,2,1,false}};
        Bytes bytes{'G','I','F','8','9','a'}; Word(bytes,16); Word(bytes,16);
        bytes.insert(bytes.end(), {0x81,0,0,255,0,0,0,255,0,0,0,255,255,255,255});
        std::vector<Bytes> expected;
        Bytes canvas(16*16*4), backup;
        const std::array<uint8_t,4> colors[] = {{0,0,255,255},{0,255,0,255},{255,0,0,255},{0,0,0,0}};
        for (size_t i=0;i<std::size(patches);++i) {
            const auto& p=patches[i];
            if (i && patches[i-1].disposal==3) canvas=backup;
            if (p.disposal==3) backup=canvas;
            for (uint32_t y=p.y;y<p.y+p.h;++y) for(uint32_t x=p.x;x<p.x+p.w;++x)
                if (!(p.transparent && p.color==3)) std::copy(colors[p.color].begin(),colors[p.color].end(),canvas.begin()+(y*16+x)*4);
            expected.push_back(canvas);
            Frame(bytes,p.x,p.w,p.h,p.color,p.disposal,p.transparent,p.y);
        }
        bytes.push_back(0x3b);
        const auto fixture=folder/L"regions.gif";
        Check(Write(fixture,bytes),"write full, partial, edge, transparent and consecutive disposal-3 fixture");
        GifDecoder decoder; GifFrame frame;
        bool pixels=true;
        for (uint32_t i : {0u,1u,2u,3u,4u,5u,2u,5u,0u,4u,3u,1u,5u})
            pixels &= decoder.Decode(fixture,16,i,frame) && frame.pixels==expected[i];
        Check(pixels,"all pixels match independent full-canvas reference after forward/reverse/repeated seeks");

        Bytes large{'G','I','F','8','9','a'}; Word(large,2048); Word(large,2048);
        large.insert(large.end(),{0x81,0,0,255,0,0,0,255,0,0,0,255,255,255,255});
        Frame(large,0,1,1,0,1);
        Frame(large,17,32,32,1,3,false,13);
        Frame(large,2040,8,7,2,3,false,2041);
        Frame(large,0,1,1,0,1);
        large.push_back(0x3b);
        const auto large_path=folder/L"large.gif";
        Check(Write(large_path,large),"write large canvas with small disposal patches");
        GifDecoder resident;
        uint64_t checksum=1469598103934665603ull;
        bool decoded=true;
        const auto begin=std::chrono::steady_clock::now();
        for(int repeat=0;repeat<30;++repeat) for(uint32_t index=0;index<4;++index) {
            decoded &= resident.Decode(large_path,256,index,frame);
            for(auto pixel:frame.pixels) { checksum^=pixel; checksum*=1099511628211ull; }
        }
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        const auto stats=resident.Stats();
        std::cout << "MEMORY backup_capacity_bytes=" << stats.backup_capacity_bytes
            << " loads=" << stats.cache_loads << " decoded=" << stats.decoded_frames
            << " repeated_ms=" << elapsed << " checksum=" << checksum << '\n';
        Check(decoded && stats.cache_loads==1 && stats.decoded_frames==120,"repeated playback keeps one decoder and linear frame decode count");
        Check(stats.backup_capacity_bytes<=32*32*4,"backup allocation follows largest affected rectangle instead of full canvas");
        resident.Reset();
        Check(resident.Stats().backup_capacity_bytes==0,"reset releases backup allocation");
    }
    std::filesystem::remove_all(folder);
    if(SUCCEEDED(com)) CoUninitialize();
    return failures?1:0;
}
int BlendPerf() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 2;
    const auto root = std::filesystem::absolute(L"bench_data");
    std::filesystem::create_directories(root);
    const auto folder = root / (L"gif-blend-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(folder.c_str(), nullptr)) { CoUninitialize(); return 2; }
    Bytes bytes{'G','I','F','8','9','a'}; Word(bytes, 1024); Word(bytes, 1024);
    bytes.insert(bytes.end(), {0x81,0,0,255,0,0,0,255,0,0,0,255,255,255,255});
    Frame(bytes, 0, 1024, 1024, 0, 1);
    Frame(bytes, 0, 1024, 1024, 1, 1);
    Frame(bytes, 0, 1024, 1024, 3, 1, true);
    Frame(bytes, 0, 1024, 1024, 2, 1);
    bytes.push_back(0x3b);
    const auto path = folder / L"full-frames.gif";
    Check(Write(path, bytes), "write full-frame opaque and transparent GIF fixture");
    GifDecoder decoder; GifFrame frame;
    uint64_t checksum = 1469598103934665603ull;
    bool valid = true;
    const auto begin = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < 10; ++repeat) for (uint32_t index = 0; index < 4; ++index) {
        valid &= decoder.Decode(path, 256, index, frame);
        const std::array<uint8_t,4> expected = index == 0 ? std::array<uint8_t,4>{0,0,255,255} :
            index == 3 ? std::array<uint8_t,4>{255,0,0,255} : std::array<uint8_t,4>{0,255,0,255};
        valid &= frame.width == 256 && frame.height == 256 && Pixel(frame, 0, expected);
        for (const auto byte : frame.pixels) { checksum ^= byte; checksum *= 1099511628211ull; }
    }
    const auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    const auto stats = decoder.Stats();
    std::cout << "BLEND ms=" << elapsed << " checksum=" << checksum << " loads=" << stats.cache_loads
              << " frames=" << stats.decoded_frames << '\n';
    Check(valid && stats.cache_loads == 1 && stats.decoded_frames == 40, "full-frame GIF playback preserves pixels and cache reuse");
    decoder.Reset();
    std::error_code error; std::filesystem::remove_all(folder, error);
    Check(!error, "clean owned full-frame GIF fixture");
    CoUninitialize();
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc==2 && std::wstring_view(argv[1])==L"--blend-perf") return BlendPerf();
    if (argc==2 && std::wstring_view(argv[1])==L"--memory") return MemoryRegression();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = std::filesystem::absolute(L"bench_data"); std::filesystem::create_directories(root);
    const auto folder = root / (L"gif-budget-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(folder.c_str(), nullptr)) return 2;
    {
        const auto path = folder / L"disposal.gif";
        Check(Write(path, Animation()), "write exclusive GIF disposal fixture");
        GifDecoder decoder; GifFrame frame;
        std::vector<Bytes> reference;
        for (uint32_t i = 0; i < 4; ++i) {
            Check(decoder.Decode(path, 256, i, frame), "sequential real WIC frame decodes");
            reference.push_back(frame.pixels);
            Check(frame.count == 4 && frame.delay == 100 && frame.loops == 2, "frame timing and finite loop metadata retained");
            if (i == 0) Check(Pixel(frame, 0, {0,0,255,255}), "initial red canvas");
            if (i == 1) Check(Pixel(frame, 1, {0,255,0,255}), "disposal 2 frame green patch");
            if (i == 2) Check(Pixel(frame, 1, {0,0,255,255}) && Pixel(frame, 2, {255,0,0,255}), "opaque disposal 2 restores red global background before blue patch");
            if (i == 3) Check(Pixel(frame, 2, {0,0,255,255}) && Pixel(frame, 3, {255,255,255,255}), "disposal 3 restores prior canvas before white patch");
        }
        Check(decoder.Stats().decoded_frames == 4 && decoder.Stats().cache_loads == 1, "four forward requests decode four frames with one identity load");
        Check(decoder.Decode(path, 256, 3, frame) && decoder.Stats().decoded_frames == 4, "same frame reuses canvas without decoding");
        for (uint32_t i : {2u,1u,0u,3u,0u,2u,1u,3u,0u})
            Check(decoder.Decode(path, 256, i, frame) && frame.pixels == reference[i], "reverse random seek and loop retain reference pixels");
        Check(decoder.Decode(path, 1, 1, frame) && frame.width == 1 && frame.height == 1 && decoder.Stats().reserved_bytes < 96ull * 1024 * 1024,
              "output scaling does not bypass source memory accounting");
        GifFrame wrong_thread;
        std::thread wrong([&] { Check(!decoder.Decode(path, 16, 0, wrong_thread) && wrong_thread.error == L"gif-wrong-thread", "cross-thread use rejected before touching WIC state"); }); wrong.join();
        HANDLE cancel = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        Check(cancel && !decoder.Decode(path, 256, 2, frame, cancel) && frame.error == L"gif-cancelled" && decoder.Stats().reserved_bytes == 0, "cancellation clears cached work and returns explicit failure");
        if (cancel) CloseHandle(cancel);
        Check(decoder.Decode(path, 256, 0, frame), "fresh request succeeds after cancellation");
        const auto replacement = folder / L"replacement.gif";
        Check(Write(replacement, Animation(4, 2)) && MoveFileExW(replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING), "replace same path with new file identity");
        Check(decoder.Decode(path, 256, 0, frame) && Pixel(frame, 0, {255,0,0,255}), "replacement invalidates old canvas");
        Check(Write(path, Animation(4, 1)), "rewrite same file identity");
        HANDLE changed = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        FILETIME modified{}; GetSystemTimeAsFileTime(&modified); modified.dwHighDateTime += 1;
        if (changed != INVALID_HANDLE_VALUE) { SetFileTime(changed, nullptr, nullptr, &modified); CloseHandle(changed); }
        Check(decoder.Decode(path, 256, 0, frame) && Pixel(frame, 0, {0,255,0,255}), "mtime change invalidates same-size same-ID data");
        const auto long_path = folder / L"long.gif"; Check(Write(long_path, Animation(160)), "write harmless 160-frame GIF");
        auto large_tail = Animation(160);
        large_tail.pop_back();
        for (unsigned i = 0; i < 70000; ++i) {
            large_tail.insert(large_tail.end(), {0x21, 0xfe, 255});
            large_tail.insert(large_tail.end(), 255, uint8_t('x'));
            large_tail.push_back(0);
        }
        large_tail.push_back(0x3b);
        const auto large_path = folder / L"large-tail.gif";
        Check(Write(large_path, large_tail), "write valid GIF over 16 MiB with small first frame");
        Check(DecodeGifFirstFrameThumbnail(large_path, 256, frame) && frame.count == 1,
            "large animation tail does not prevent bounded first frame decode");
        GifFrame small_first;
        Check(DecodeGifFirstFrameThumbnail(long_path, 256, small_first) && small_first.pixels == frame.pixels,
            "truncated WIC stream length preserves identical first frame for large tail");
        GifDecoder sequential;
        bool linear = true;
        for (uint32_t i = 0; i < 160; ++i) linear &= sequential.Decode(long_path, 256, i, frame);
        Check(linear && sequential.Stats().decoded_frames == 160, "first long playback decodes N frames instead of N squared prefix work");
        GifDecoder cold;
        Check(cold.Decode(long_path, 256, 159, frame) && cold.Stats().decoded_frames == 160,
              "cold distant seek decodes without a prefix-work cap");
        auto outside = Animation();
        const auto descriptor = std::find(outside.begin(), outside.end(), uint8_t(0x2c));
        if (descriptor != outside.end()) *(descriptor + 5) = 5;
        const auto bounds = folder / L"outside.gif"; Write(bounds, outside);
        Check(!decoder.Decode(bounds, 256, 0, frame) && frame.error == L"gif-frame-bounds" && frame.pixels.empty(),
              "out-of-canvas frame rejected before source allocation");
        auto malformed = Animation(); malformed.pop_back(); const auto bad = folder / L"truncated.gif"; Write(bad, malformed);
        Check(!decoder.Decode(bad, 256, 0, frame) && frame.error == L"gif-truncated", "truncated structure never publishes cached pixels");
        for (bool transparent : {false, true}) {
            Bytes bytes{'G','I','F','8','9','a'}; Word(bytes, 4); Word(bytes, 2);
            bytes.insert(bytes.end(), {0x81,3,0, 255,0,0, 0,255,0, 0,0,255, 255,255,255});
            Frame(bytes, 0, 1, 2, 0, 2, transparent);
            Frame(bytes, 1, 1, 2, 2, 3, transparent);
            Frame(bytes, 2, 1, 2, 1, 1, transparent);
            bytes.push_back(0x3b);
            const auto fixture = folder / (transparent ? L"transparent.gif" : L"white-background.gif");
            Check(Write(fixture, bytes), "write explicit background and transparency fixture");
            GifDecoder background_decoder;
            GifFrame thumbnail, expected;
            Check(background_decoder.Decode(fixture, 256, 0, expected) &&
                DecodeGifFirstFrameThumbnail(fixture, 256, thumbnail) && thumbnail.pixels == expected.pixels && thumbnail.count == 1,
                "static first frame matches animation background transparency and offsets");
            const std::array<uint8_t, 4> bg = transparent ? std::array<uint8_t, 4>{0,0,0,0} :
                std::array<uint8_t, 4>{255,255,255,255};
            for (uint32_t i : {0u,1u,2u,0u,2u,1u}) {
                Check(background_decoder.Decode(fixture, 256, i, frame), "background fixture sequential and backward WIC decode");
                Check(Pixel(frame, 3, bg), "initial and replay uncovered canvas uses correct background alpha");
                Check(Pixel(frame, 0, i == 0 ? std::array<uint8_t, 4>{0,0,255,255} : bg),
                    "disposal 2 restores opaque white or transparent background");
                Check(Pixel(frame, 1, i == 1 ? std::array<uint8_t, 4>{255,0,0,255} : bg),
                    "disposal 3 restores saved background without stale blue");
                Check(Pixel(frame, 2, i == 2 ? std::array<uint8_t, 4>{0,255,0,255} : bg),
                    "later patch and backward replay preserve full expected row");
            }
        }
    }
    // Only this process's successfully created exclusive fixture directory is removed.
    std::filesystem::remove_all(folder);
    if (SUCCEEDED(com)) CoUninitialize();
    std::cout << "Failures: " << failures << '\n'; return failures ? 1 : 0;
}
