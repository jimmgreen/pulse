#include "preview_host_client.h"
#include "../preview_host/image_frames.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
void Put16(std::vector<uint8_t>& b, size_t at, uint16_t value) {
    b[at] = static_cast<uint8_t>(value); b[at + 1] = static_cast<uint8_t>(value >> 8);
}
void Put32(std::vector<uint8_t>& b, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(value >> (8 * i));
}
std::vector<uint8_t> Icon(const std::vector<unsigned>& sizes, bool cursor) {
    std::vector<uint8_t> file(6 + 16 * sizes.size());
    Put16(file, 2, cursor ? 2 : 1); Put16(file, 4, static_cast<uint16_t>(sizes.size()));
    for (size_t i = 0; i < sizes.size(); ++i) {
        const auto edge = sizes[i];
        const uint32_t mask = ((edge + 31) / 32) * 4 * edge;
        std::vector<uint8_t> dib(40 + edge * edge * 4 + mask);
        Put32(dib, 0, 40); Put32(dib, 4, edge); Put32(dib, 8, edge * 2);
        Put16(dib, 12, 1); Put16(dib, 14, 32);
        for (size_t p = 40; p < 40 + edge * edge * 4; p += 4) {
            dib[p] = static_cast<uint8_t>(20 + i * 20); dib[p + 1] = 100;
            dib[p + 2] = 210; dib[p + 3] = 255;
        }
        const auto at = 6 + 16 * i;
        file[at] = file[at + 1] = static_cast<uint8_t>(edge == 256 ? 0 : edge);
        Put16(file, at + 4, cursor ? 7 : 1); Put16(file, at + 6, cursor ? 9 : 32);
        Put32(file, at + 8, static_cast<uint32_t>(dib.size()));
        Put32(file, at + 12, static_cast<uint32_t>(file.size()));
        file.insert(file.end(), dib.begin(), dib.end());
    }
    return file;
}
void Save(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
}

int main() {
    using namespace pulse;
    int passed = 0, failed = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        ok ? ++passed : ++failed;
    };
    const auto root = std::filesystem::absolute("bench_data") /
        (L"icon-decode-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{root};
    pulse_test::Host host;
    if (!host.Start()) { std::puts("[FAIL] preview host startup"); return 1; }
    for (const bool cursor : {false, true}) for (const bool reversed : {false, true}) {
        std::vector<unsigned> sizes{16, 24, 32, 48, 64, 96, 128, 256};
        if (reversed) std::reverse(sizes.begin(), sizes.end());
        const auto bytes = Icon(sizes, cursor);
        const auto path = root / (cursor ? L"frames.cur" : L"frames.ico");
        Save(path, bytes);
        std::vector<preview::IconEntry> entries;
        bool parsed_cursor = false;
        check(preview::ParseIconDirectory(bytes, entries, parsed_cursor) && entries.size() == sizes.size() && parsed_cursor == cursor,
              "fixture directory preserves source order");
        for (size_t i = 0; i < sizes.size(); ++i) {
            std::vector<uint8_t> single;
            check(preview::MakeIconEntryFile(bytes, entries[i], cursor, single) &&
                  single.size() == entries[i].size + 22 && single[2] == 1 && single[4] == 1 && single[18] == 22,
                  "selected icon/cursor payload is isolated into one icon entry");
            pulse_test::Result result;
            const bool received = host.Request(path.wstring(), result, MAXDWORD, 256,
                ipc::PreviewRequestKind::Content, 0, static_cast<uint32_t>(i + 1));
            check(received && !result.response.status && result.response.width == sizes[i] &&
                  result.response.height == sizes[i] && result.response.source_width == sizes[i] &&
                  !result.pixels.empty() && result.pixels[0] == 20 + i * 20,
                  "explicit size preview decodes the requested file entry, not the WIC frame index");
        }
        const auto best = preview::DefaultIconEntry(entries);
        host.SetBudgeted(true);
        for (const uint32_t request : {48u, 96u, 144u, 256u, 512u}) {
            pulse_test::Result result;
            const auto begin = GetTickCount64();
            const bool received = host.Request(path.wstring(), result, MAXDWORD, request);
            check(received && !result.response.status && result.response.source_width == 256 &&
                  result.response.width == (std::min)(request, 256u) &&
                  !result.pixels.empty() && result.pixels[0] == 20 + best * 20,
                  "budgeted grid uses the high-resolution source without inventing larger pixels");
            std::printf("[TIME] requested=%u returned=%u source=%u elapsed_ms=%llu\n", request,
                result.response.width, result.response.source_width, GetTickCount64() - begin);
        }
        host.SetBudgeted(false);
        std::vector<uint8_t> invalid;
        auto bad = entries.front(); bad.offset = static_cast<uint32_t>(bytes.size());
        check(!preview::MakeIconEntryFile(bytes, bad, cursor, invalid) && invalid.empty(),
              "out-of-bounds icon payload is rejected");
    }
    const auto small_path = root / L"small.ico";
    Save(small_path, Icon({16}, false));
    pulse_test::Result result;
    check(host.Request(small_path.wstring(), result, MAXDWORD, 256) && !result.response.status && result.response.width == 16,
          "legitimate small-only icon remains supported");
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
