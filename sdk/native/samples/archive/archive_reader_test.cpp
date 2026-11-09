#include "archive_reader.h"
#include <cstdio>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: archive_reader_test fixture.7z fixture.rar [large.7z]\n"); return 2; }
    int failures = 0;
    const auto check = [&](bool ok, const char* text) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); if (!ok) ++failures; };
    for (int i = 1; i <= 2; ++i) {
        auto result = archive_reader::Read(argv[i]);
        std::printf("format=%s status=%d entries=%zu error=%s\n", result.format.c_str(), static_cast<int>(result.status), result.entries.size(), result.error.c_str());
        for (const auto& entry : result.entries) std::printf("  %s size=%llu packed=%llu dir=%d\n", entry.path.c_str(), static_cast<unsigned long long>(entry.size), static_cast<unsigned long long>(entry.packed), entry.dir);
        check(result.status == archive_reader::Status::Ok && !result.incomplete && !result.entries.empty(), "real archive lists metadata");
        if (i == 1) check(result.format == "7z" && result.entries[0].size == 13, "7z entry has expected uncompressed size");
        else check(result.format == "Rar" || result.format == "Rar5", "real RAR handler selected");
        const auto stage = std::filesystem::temp_directory_path() / (L"PulseExtract-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i));
        std::filesystem::create_directory(stage);
        uint64_t progress_calls = 0;
        auto extracted = archive_reader::Extract(argv[i], stage.wstring(), {}, [&](const auto&) { ++progress_calls; });
        check(extracted.status == archive_reader::Status::Ok && progress_calls > 0, "real archive extracts with progress");
        for (const auto& entry : result.entries) if (!entry.dir) {
            const auto output = stage / std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(entry.path.data()), entry.path.size()));
            check(std::filesystem::exists(output) && std::filesystem::file_size(output) == entry.size, "extracted size matches archive metadata");
            if (i == 2) { std::ifstream f(output, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(f)), {}); check(bytes == "test text document\r\n", "RAR extracted bytes match source"); }
            if (i == 1) { std::ifstream f(output, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(f)), {}); check(bytes == "Hello archive", "7z extracted bytes match source"); }
        }
        std::filesystem::remove_all(stage);
        std::filesystem::create_directory(stage);
        bool stop = false;
        extracted = archive_reader::Extract(argv[i], stage.wstring(), [&] { return stop; }, [&](const auto& p) { if (!p.path.empty()) stop = true; });
        check(extracted.status == archive_reader::Status::Cancelled, "extraction callback cancellation propagates");
        check(std::filesystem::remove_all(stage) > 0, "cancelled output handles closed and staging removable");
        std::filesystem::create_directory(stage);
        extracted = archive_reader::Extract(argv[i], stage.wstring(), {}, {}, 0);
        check(extracted.status == archive_reader::Status::LimitReached && std::filesystem::is_empty(stage), "output budget rejects before writes");
        std::filesystem::remove_all(stage);
        auto cancelled = archive_reader::Read(argv[i], [] { return true; });
        check(cancelled.status == archive_reader::Status::Cancelled && cancelled.entries.empty(), "pre-cancel has no partial success");
        int polls = 0;
        cancelled = archive_reader::Read(argv[i], [&] { return ++polls > 6; });
        check(cancelled.status == archive_reader::Status::Cancelled, "cancellation during archive stream work propagates");
    }
    for (const auto* path : {"../escape", "/root", "C:/drive", "\\\\server/share", "a/../b", "a:stream", "a/CON.txt", "a/b.", "a//b"})
        check(!archive_reader::ValidExtractionPath(path), "unsafe stored path rejected");
    check(archive_reader::ValidExtractionPath("folder/file.txt"), "ordinary relative path accepted");
    const auto missing = archive_reader::Read(std::wstring(argv[1]) + L".missing");
    check(missing.status == archive_reader::Status::IoError, "missing archive reports IO error");
    const auto bad = std::filesystem::path(argv[1]).parent_path() / L"invalid.fixture";
    { std::ofstream file(bad, std::ios::binary); file << "not an archive"; }
    const auto invalid = archive_reader::Read(bad.wstring());
    check(invalid.status != archive_reader::Status::Ok && invalid.entries.empty(), "invalid input never reports success");
    std::filesystem::remove(bad);
    if (argc > 3) {
        const auto large = archive_reader::Read(argv[3]);
        check(large.status == archive_reader::Status::LimitReached && large.incomplete && large.entries.size() == 4096, "listing budget returns explicit incomplete status");
    }
    for (const auto* name : {L"encrypted.7z", L"truncated.7z"}) {
        const auto fixture = std::filesystem::path(argv[1]).parent_path() / name;
        if (!std::filesystem::exists(fixture)) continue;
        const auto rejected = archive_reader::Read(fixture.wstring());
        check(rejected.status != archive_reader::Status::Ok && rejected.entries.empty(), "encrypted or truncated headers do not report success");
    }
    const auto staging = std::filesystem::temp_directory_path() / (L"PulseExtractReject-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(staging);
    for (const auto* name : {L"encrypted.7z", L"truncated.7z", L"sample.rar"}) {
        const auto input = std::filesystem::path(argv[1]).parent_path() / name;
        if (!std::filesystem::exists(input)) continue;
        const auto rejected = archive_reader::Extract(input.wstring(), staging.wstring());
        check(rejected.status != archive_reader::Status::Ok && std::filesystem::is_empty(staging),
              "encrypted damaged or symlink archive rejected before output");
    }
    { std::ofstream f(staging / L"existing"); f << "preserved"; }
    check(archive_reader::Extract(argv[1], staging.wstring()).status == archive_reader::Status::IoError,
          "nonempty staging rejected without overwrite");
    std::filesystem::remove_all(staging);
    if (argc > 4) {
        std::filesystem::create_directory(staging);
        bool cancelled = false;
        uint64_t last_bytes = 0, total_bytes = 0;
        auto extracted = archive_reader::Extract(argv[4], staging.wstring(), [&] { return cancelled; },
            [&](const auto& p) {
                last_bytes = p.completed_bytes; total_bytes = p.total_bytes;
                if (p.completed_bytes >= 1024 * 1024) cancelled = true;
            });
        check(extracted.status == archive_reader::Status::Cancelled && cancelled, "large stream cancels after observed byte progress");
        check(last_bytes > 0 && last_bytes < total_bytes &&
              std::filesystem::file_size(staging / L"payload.bin") == last_bytes,
              "cancelled byte progress equals actual partial output and is below total");
        check(std::filesystem::remove_all(staging) > 0, "partial large extraction handles close before cleanup");
        std::filesystem::create_directory(staging);
        extracted = archive_reader::Extract(argv[4], staging.wstring(), {}, [&](const auto& p) {
            last_bytes = p.completed_bytes; total_bytes = p.total_bytes;
        });
        check(extracted.status == archive_reader::Status::Ok && std::filesystem::file_size(staging / L"payload.bin") == 256ull * 1024 * 1024,
              "256 MiB actual archive completes within default output budget");
        check(last_bytes == total_bytes && total_bytes == 256ull * 1024 * 1024,
              "completed byte progress equals total bytes actually written");
        std::filesystem::remove_all(staging);
    }
    return failures ? 1 : 0;
}
