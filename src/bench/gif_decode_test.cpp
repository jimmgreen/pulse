#include "../preview_host/gif_decoder.h"
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
#include <algorithm>
#include <thread>
using namespace pulse::preview;
using Bytes = std::vector<uint8_t>;
namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; failures += !ok; }
void Word(Bytes& out, uint32_t n) { out.push_back(static_cast<uint8_t>(n)); out.push_back(static_cast<uint8_t>(n >> 8)); }
void Frame(Bytes& out, uint32_t x, uint32_t width, uint32_t height, uint8_t color, uint8_t disposal, bool transparent = false) {
    out.insert(out.end(), {0x21, 0xf9, 4, static_cast<uint8_t>((disposal << 2) | (transparent ? 1 : 0)), 10, 0, 3, 0, 0x2c});
    Word(out, x); Word(out, 0); Word(out, width); Word(out, height); out.push_back(0);
    Bytes data; uint32_t bits = 0; unsigned used = 0;
    auto code = [&](uint32_t value) {
        bits |= value << used; used += 3;
        while (used >= 8) { data.push_back(static_cast<uint8_t>(bits)); bits >>= 8; used -= 8; }
    };
    for (uint32_t i = 0; i < width * height; ++i) { code(4); code(color); }
    code(5); if (used) data.push_back(static_cast<uint8_t>(bits));
    out.push_back(2);
    for (size_t at = 0; at < data.size();) {
        const auto n = (std::min)(size_t(255), data.size() - at);
        out.push_back(static_cast<uint8_t>(n)); out.insert(out.end(), data.begin() + at, data.begin() + at + n); at += n;
    }
    out.push_back(0);
}
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
int wmain() {
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
