#include "../index/index_shard.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
using namespace pulse::index;
int main() {
    int failures = 0;
    auto check = [&](bool value, const char* name) { std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; failures += !value; };
    const auto root = std::filesystem::absolute("bench_data/index-publish-fix/run-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto paths = MakeShardPaths(root.wstring(), L"mapped-reader");
    const auto temp = paths.directory + L"\\incoming.bin";
    auto write = [&](uint64_t built) {
        char bytes[128]{}; memcpy(bytes, "PIDX", 4); uint32_t version = kIndexSnapshotVersion;
        memcpy(bytes + 4, &version, 4); memcpy(bytes + 16, &built, 8);
        std::ofstream file(std::filesystem::path(temp), std::ios::binary); file.write(bytes, sizeof(bytes));
    };
    ShardManifest manifest;
    write(100); check(PublishShardBase(paths, temp, 100, 0, manifest), "publish first generation");
    HANDLE file = CreateFileW(paths.base_a.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    auto* view = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    check(view != nullptr, "hold first generation mapped across slot rotations");
    for (uint64_t built : {200ull, 300ull, 400ull, 500ull}) {
        write(built); check(PublishShardBase(paths, temp, built, 0, manifest), "publish with old reader alive");
        ShardManifest loaded; std::wstring active;
        check(ResolveActiveShard(paths, loaded, active) && loaded.active_built == built, "restart resolves latest committed generation");
        uint64_t old = 0; if (view) memcpy(&old, view + 16, 8);
        check(old == 100, "old reader retains original bytes");
    }
    // Deny manifest replacement after the new base has been staged.
    HANDLE manifest_lock = CreateFileW(paths.manifest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    write(600);
    HANDLE incoming = CreateFileW(temp.c_str(), GENERIC_READ, 7, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE incoming_mapping = CreateFileMappingW(incoming, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* incoming_view = MapViewOfFile(incoming_mapping, FILE_MAP_READ, 0, 0, 0);
    uint32_t stage = 0;
    check(incoming_view && !PublishShardBase(paths, temp, 600, 0, manifest, nullptr, {}, &stage) && stage == 4,
          "mapped incoming base rolls back and reports manifest failure stage");
    if (incoming_view) UnmapViewOfFile(incoming_view);
    CloseHandle(incoming_mapping); CloseHandle(incoming);
    CloseHandle(manifest_lock);
    ShardManifest loaded; std::wstring active;
    check(ResolveActiveShard(paths, loaded, active) && loaded.active_built == 500, "failed publication preserves committed manifest");
    check(ValidateShardBase(paths.base_b), "manifest rollback restores previous inactive slot");
    write(650);
    HANDLE blocked_source = CreateFileW(temp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    stage = 0;
    check(!PublishShardBase(paths, temp, 650, 0, manifest, nullptr, {}, &stage) && stage == 3 &&
              ValidateShardBase(paths.base_b), "base move failure restores retired slot and reports stage");
    CloseHandle(blocked_source);
    write(700); check(PublishShardBase(paths, temp, 700, 0, manifest), "publication recovers after failure");
    if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping); if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    write(800); check(PublishShardBase(paths, temp, 800, 0, manifest), "publication cleans retired generations after readers close");
    size_t retired = 0;
    for (const auto& entry : std::filesystem::directory_iterator(paths.directory)) if (entry.path().filename().wstring().find(L".retired.") != std::wstring::npos) ++retired;
    check(retired == 0, "retired files do not accumulate after release");
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
