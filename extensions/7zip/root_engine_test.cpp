#include "archive_paths.h"
#include "engine.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
namespace {
int failures = 0;
void Check(bool ok, const char* text, const std::string& error = {}) {
    std::printf("[%s] %s %s\n", ok ? "PASS" : "FAIL", text, error.c_str()); failures += !ok;
}
struct Drive {
    std::wstring name, target;
    bool Map(const fs::path& folder) {
        target = LR"(\??\)" + folder.wstring();
        wchar_t existing[32768]{};
        for (wchar_t letter = L'Z'; letter >= L'R'; --letter) {
            name = std::wstring(1, letter) + L":";
            if (QueryDosDeviceW(name.c_str(), existing, 32768)) continue;
            const auto query_error = GetLastError();
            if (query_error != ERROR_FILE_NOT_FOUND) { std::printf("drive query error=%lu\n", query_error); continue; }
            if (DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, name.c_str(), target.c_str())) return true;
            std::printf("drive map error=%lu\n", GetLastError());
        }
        name.clear(); return false;
    }
    bool Remove() {
        if (name.empty()) return true;
        const bool ok = DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE |
            DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, name.c_str(), target.c_str()) != FALSE;
        if (ok) name.clear();
        return ok;
    }
    ~Drive() { Remove(); }
};
std::string Read(const fs::path& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
}
int main(int argc, char** argv) {
    const bool workspace_only = argc == 2 && std::string(argv[1]) == "--workspace-only";
    if (argc > 1 && !workspace_only) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto fixture = fs::absolute(fs::path("bench_data/sevenzip-root-path-20261119") /
        ("engine-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    fs::create_directories(fixture / "source");
    std::string data(262144, '\0');
    unsigned seed = 42;
    for (auto& ch : data) { seed = seed * 1664525u + 1013904223u; ch = static_cast<char>(seed >> 24); }
    std::ofstream(fixture / "source" / "payload.bin", std::ios::binary) << data;
    Drive mapping;
    if (!workspace_only) {
        Check(mapping.Map(fixture), "isolated fixture DOS mapping created");
        if (mapping.name.empty()) return 1;
    }
    const auto root = workspace_only ? fs::path(LR"(\\?\)" + fixture.wstring()) :
        fs::path(LR"(\\?\)" + mapping.name + L"\\");
    std::printf("MODE=%s\n", workspace_only ? "EXTENDED_WORKSPACE_NOT_DRIVE_ROOT" : "EXTENDED_DRIVE_ROOT");
    const auto source = root / "source" / "payload.bin";
    const engine::Cancelled never = [] { return false; };
    for (const auto format : {engine::Format::Zip, engine::Format::SevenZip, engine::Format::TarGz}) {
        engine::CreateOptions options;
        options.format = format;
        options.level = 0;
        if (format == engine::Format::SevenZip) options.volume_size = 32768;
        const auto name = L"root" + engine::FormatExtension(format);
        const auto created = engine::Create({source.wstring()}, root.wstring(), name, options, never, {});
        Check(created.status == engine::Status::Ok && !created.outputs.empty(), "create archive at selected extended fixture", created.error);
        if (created.outputs.empty()) continue;
        const auto archive = created.outputs.front();
        Check(archive_paths::Parent(archive) == root, "production archive parent preserves selected absolute destination");
        const auto listed = engine::List(archive, never, {});
        Check(listed.status == engine::Status::Ok && !listed.entries.empty(), "root archive list including split or wrapped archive", listed.error);
        const auto tested = engine::Extract(archive, root.wstring(), true, never, {}, {});
        Check(tested.status == engine::Status::Ok, "test root archive", tested.error);
        const auto output = root / "extracted";
        fs::create_directory(output);
        const auto extracted = engine::Extract(archive, output.wstring(), false, never, {}, {});
        Check(extracted.status == engine::Status::Ok && Read(output / "payload.bin") == data, "extract archive-root file with full payload verification", extracted.error);
        fs::remove_all(output);
    }
    const auto long_dir = root / std::wstring(140, L'a') / std::wstring(140, L'b');
    fs::create_directories(long_dir);
    engine::CreateOptions zip; zip.format = engine::Format::Zip;
    const auto created = engine::Create({source.wstring()}, long_dir.wstring(), L"long.zip", zip, never, {});
    Check(created.status == engine::Status::Ok, "extended path over MAX_PATH create", created.error);
    const auto long_archive = long_dir / "long.zip";
    const auto long_list = engine::List(long_archive.wstring(), never, {});
    Check(long_list.status == engine::Status::Ok && archive_paths::Parent(long_archive) == long_dir, "extended path over MAX_PATH list and parent", long_list.error);
#ifdef ROOT_WORKER_TEST
    engine::ShutdownWorkers();
#endif
    if (!workspace_only) Check(mapping.Remove(), "exact isolated DOS mapping removed");
    fs::remove_all(fs::path(LR"(\\?\)" + fixture.wstring()));
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
