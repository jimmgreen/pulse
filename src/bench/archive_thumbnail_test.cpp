// archive_thumbnail_test - grid covers read from inside ZIP/7z archives.
//
// Each case starts Pulse.Preview.exe through preview_host_client.h and asks it
// for a Content preview of an isolated fixture under bench_data/. The archive
// fixtures are built here: ZIP by hand (the same minimal structure
// archive_plugin_required_test.cpp uses) and 7z through the system
// libarchive's write side (archiveint.dll), so no third-party library is
// introduced.
#include "preview_host_client.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

bool Write(const std::filesystem::path& path, const std::string& data) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.close();
    return !stream.fail();
}

// A small legal PNG: 2x2, 8-bit RGB, one IDAT. Kept above 1x1 because WIC's
// PNG decoder in the preview host rejects the minimal 1x1 image. The literal
// carries NUL bytes, so its length must be taken explicitly.
const char kPngBytes[] =
    "\x89PNG\r\n\x1a\n"
    "\x00\x00\x00\x0dIHDR\x00\x00\x00\x02\x00\x00\x00\x02\x08\x02\x00\x00\x00"
    "\xfd\xd4\x9as"
    "\x00\x00\x00\x12IDAT\x78\x9c\x63\xf8\xcf\xc0\xc0\x00\xc2\x0c\xff\x81\x00"
    "\x00\x1f\xee\x05\xfb\x0b\xd9\x68\x8b"
    "\x00\x00\x00\x00IEND\xae\x42\x60\x82";
const std::string kPng(kPngBytes, sizeof(kPngBytes) - 1);

void Put(std::string& data, size_t offset, uint32_t value, size_t count) {
    for (size_t i = 0; i < count; ++i) data[offset + i] = static_cast<char>(value >> (8 * i));
}

uint32_t Crc32(const std::string& bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// One stored (method 0) member. libarchive's seekable ZIP reader verifies the
// CRC while reading the member, so it is the real checksum; the DOS timestamp
// must be a valid date (1980-01-01) or the member is skipped.
std::string ZipWithOneMember(const std::string& name, const std::string& payload) {
    const uint32_t crc = Crc32(payload);
    const uint32_t size = static_cast<uint32_t>(payload.size());
    constexpr uint16_t kDosTime = 0;      // 00:00:00
    constexpr uint16_t kDosDate = 0x0021; // 1980-01-01
    std::string local(30, '\0'), central(46, '\0'), end(22, '\0');
    Put(local, 0, 0x04034b50, 4);
    Put(local, 4, 20, 2);
    Put(local, 10, kDosTime, 2);
    Put(local, 12, kDosDate, 2);
    Put(local, 14, crc, 4);
    Put(local, 18, size, 4);  // compressed size (stored)
    Put(local, 22, size, 4);  // uncompressed size
    Put(local, 26, static_cast<uint32_t>(name.size()), 2);
    Put(central, 0, 0x02014b50, 4);
    Put(central, 4, 0x0314, 2);  // version made by: Unix, 2.0
    Put(central, 6, 20, 2);      // version needed
    Put(central, 12, kDosTime, 2);
    Put(central, 14, kDosDate, 2);
    Put(central, 16, crc, 4);
    Put(central, 20, size, 4);  // compressed size
    Put(central, 24, size, 4);  // uncompressed size
    Put(central, 28, static_cast<uint32_t>(name.size()), 2);
    Put(central, 38, 0x81A40000, 4);  // external attributes: regular file 0644
    // The single local header starts at file offset 0, so its recorded offset
    // is zero, not the local-header length.
    Put(central, 42, 0, 4);
    Put(end, 0, 0x06054b50, 4);
    Put(end, 8, 1, 2);
    Put(end, 10, 1, 2);
    Put(end, 12, static_cast<uint32_t>(central.size() + name.size()), 4);
    Put(end, 16, static_cast<uint32_t>(local.size() + name.size() + payload.size()), 4);
    return local + name + payload + central + name + end;
}

// 7z through the system libarchive's writer. Returns false when the DLL or one
// of its write entry points is missing, so the caller can skip the case
// instead of failing a machine that simply lacks it.
struct Archive;
struct ArchiveEntry;
bool Write7z(const std::filesystem::path& path, const std::string& name,
             const std::string& payload) {
    HMODULE module = LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    struct Unload { HMODULE module; ~Unload() { FreeLibrary(module); } } unload{module};
    auto load = [&](const char* symbol) {
        return reinterpret_cast<void*>(GetProcAddress(module, symbol));
    };
    using New = Archive* (*)();
    using Set7z = int (*)(Archive*);
    using Open = int (*)(Archive*, const wchar_t*, size_t);
    using Header = int (*)(Archive*, ArchiveEntry*);
    using Data = int64_t (*)(Archive*, const void*, size_t);
    using Close = int (*)(Archive*);
    using Free = int (*)(Archive*);
    using EntryNew = ArchiveEntry* (*)();
    using SetPath = void (*)(ArchiveEntry*, const char*);
    using SetSize = void (*)(ArchiveEntry*, int64_t);
    using SetType = void (*)(ArchiveEntry*, unsigned int);
    using SetPerm = void (*)(ArchiveEntry*, unsigned int);
    using EntryFree = void (*)(ArchiveEntry*);    const auto archive_new = reinterpret_cast<New>(load("archive_write_new"));
    const auto set_7z = reinterpret_cast<Set7z>(load("archive_write_set_format_7zip"));
    const auto open = reinterpret_cast<Open>(load("archive_write_open_filename_w"));
    const auto header = reinterpret_cast<Header>(load("archive_write_header"));
    const auto data = reinterpret_cast<Data>(load("archive_write_data"));
    const auto close = reinterpret_cast<Close>(load("archive_write_close"));
    const auto free_ = reinterpret_cast<Free>(load("archive_write_free"));
    const auto entry_new = reinterpret_cast<EntryNew>(load("archive_entry_new"));
    const auto set_path = reinterpret_cast<SetPath>(load("archive_entry_set_pathname_utf8"));
    const auto set_size = reinterpret_cast<SetSize>(load("archive_entry_set_size"));
    const auto set_type = reinterpret_cast<SetType>(load("archive_entry_set_filetype"));
    const auto set_perm = reinterpret_cast<SetPerm>(load("archive_entry_set_perm"));
    const auto entry_free = reinterpret_cast<EntryFree>(load("archive_entry_free"));
    if (!archive_new || !set_7z || !open || !header || !data || !close || !free_ ||
        !entry_new || !set_path || !set_size || !set_type || !set_perm || !entry_free)
        return false;
    Archive* archive = archive_new();
    if (!archive) return false;
    struct Owner { Free free_; Archive* archive; ~Owner() { free_(archive); } } owner{free_, archive};
    if (set_7z(archive) != 0 || open(archive, path.c_str(), 64 * 1024) != 0) return false;
    ArchiveEntry* entry = entry_new();
    if (!entry) return false;
    struct EntryOwner { EntryFree free_; ArchiveEntry* entry; ~EntryOwner() { free_(entry); } }
        entry_owner{entry_free, entry};
    set_path(entry, name.c_str());
    set_size(entry, static_cast<int64_t>(payload.size()));
    set_type(entry, 0100000);  // regular file
    set_perm(entry, 0644);
    if (header(archive, entry) != 0) return false;
    if (!payload.empty() &&
        data(archive, payload.data(), payload.size()) != static_cast<int64_t>(payload.size()))
        return false;
    return close(archive) == 0;
}

bool RequestGrid(pulse_test::Host& host, const std::filesystem::path& path,
                 pulse_test::Result& result) {
    return host.Request(path.wstring(), result, MAXDWORD, 256,
                        pulse::ipc::PreviewRequestKind::Content,
                        pulse::ipc::kPreviewRequestFlagGrid);
}

} // namespace

int wmain() {
    namespace fs = std::filesystem;
    namespace ipc = pulse::ipc;
    const fs::path parent = fs::current_path() / L"bench_data";
    fs::create_directories(parent);
    const fs::path directory = parent / (L"archive-thumbnail-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(directory)) return 1;

    const fs::path png_zip = directory / L"cover.zip";
    const fs::path text_zip = directory / L"text.zip";
    const fs::path png_7z = directory / L"cover.7z";
    Check(Write(png_zip, ZipWithOneMember("cover.png", kPng)), "write ZIP fixture with cover.png");
    Check(Write(text_zip, ZipWithOneMember("notes.txt", "plain text member\n")),
          "write ZIP fixture with text only");
    const bool have_7z = Write7z(png_7z, "cover.png", kPng);
    Check(have_7z || !fs::exists(png_7z), "write 7z fixture with cover.png (skipped if no writer)");

    pulse_test::Host host;
    const bool started = host.Start();
    Check(started, "start real preview host");
    if (started) {
        // 1. Grid request on a ZIP with a picture: a bitmap cover.
        pulse_test::Result grid;
        const bool grid_ok = RequestGrid(host, png_zip, grid) &&
            grid.response.kind == ipc::PreviewContentKind::Bitmap &&
            grid.response.width > 0 && grid.response.height > 0 && !grid.pixels.empty();
        Check(grid_ok, "ZIP cover.png grid request returns a bitmap");
        if (!grid_ok) std::wprintf(L"  kind=%u width=%u height=%u pixels=%zu error=%ls\n",
            static_cast<unsigned>(grid.response.kind), grid.response.width, grid.response.height,
            grid.pixels.size(), grid.error.c_str());

        // 2. The same ZIP outside the grid keeps the built-in archive tree.
        pulse_test::Result tree;
        const bool tree_ok = host.Request(png_zip.wstring(), tree) &&
            tree.response.kind == ipc::PreviewContentKind::Archive &&
            tree.text.rfind(L"PULSEARC\t", 0) == 0;
        Check(tree_ok, "ZIP cover.png details request keeps the archive tree");
        if (!tree_ok) std::wprintf(L"  kind=%u text=%.40ls\n",
            static_cast<unsigned>(tree.response.kind), tree.text.c_str());

        // 3. A ZIP without a picture declines the tile, so the grid keeps the
        //    type icon rather than a fabricated thumbnail.
        pulse_test::Result text;
        const bool text_ok = RequestGrid(host, text_zip, text) &&
            text.response.kind != ipc::PreviewContentKind::Bitmap && text.pixels.empty();
        Check(text_ok, "ZIP without a picture does not fabricate a grid bitmap");
        if (!text_ok) std::wprintf(L"  kind=%u pixels=%zu\n",
            static_cast<unsigned>(text.response.kind), text.pixels.size());

        // 4. 7z cover, when the system writer is present.
        if (have_7z) {
            pulse_test::Result seven;
            const bool seven_ok = RequestGrid(host, png_7z, seven) &&
                seven.response.kind == ipc::PreviewContentKind::Bitmap &&
                seven.response.width > 0 && seven.response.height > 0 && !seven.pixels.empty();
            Check(seven_ok, "7z cover.png grid request returns a bitmap");
            if (!seven_ok) std::wprintf(L"  kind=%u width=%u height=%u error=%ls\n",
                static_cast<unsigned>(seven.response.kind), seven.response.width,
                seven.response.height, seven.error.c_str());
        } else {
            std::printf("[SKIP] 7z cover.png grid request (no archiveint.dll writer)\n");
        }
    }
    host.Stop();
    std::error_code error;
    for (const auto& name : {L"cover.zip", L"text.zip", L"cover.7z"})
        fs::remove(directory / name, error);
    Check(fs::remove(directory, error) && !error, "clean isolated fixtures");
    return failures == 0 ? 0 : 1;
}
