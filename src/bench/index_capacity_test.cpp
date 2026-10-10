#include "../index/index_engine.h"
#include "../index/index_paths.h"
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <limits>

namespace pulse::index {
struct EngineTestAccess {
    static bool RoundTrip(const std::wstring& path, size_t count) {
        Engine engine;
        Engine::Store store;
        store.pool = {L'Q', L':', L'\0', L'x', L'\0'};
        store.nodes.resize(count);
        store.attrs.resize(count);
        for (size_t i = 0; i < count; ++i) {
            store.nodes[i].parent = i ? 0 : -1;
            store.nodes[i].off = i ? 3 : 0;
            store.nodes[i].len = i ? 1 : 2;
            store.nodes[i].flags = i ? 0 : Engine::kFlagDir;
        }
        if (!engine.WriteIndexFile(path, store, {}, 123456)) return false;
        store.Clear(); store.Shrink();
        if (!MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!engine.MapIndexFile(path, mapped) || mapped->hdr->node_count != count) return false;
        engine.AdoptMappedLocked(std::move(mapped));
        engine.ready_ = true;
        Query query; query.needle = L"\"x\""; query.rank = false; query.limit = 1;
        const auto found = engine.Search(query);
        return engine.Count() == count && found.total == count - 1 && found.hits.size() == 1;
    }
    static bool RejectOversizedCount(const std::wstring& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DiskHeader header{}; DWORD read = 0;
        bool ok = ReadFile(file, &header, sizeof(header), &read, nullptr) && read == sizeof(header);
        header.node_count = (std::numeric_limits<uint32_t>::max)();
        LARGE_INTEGER start{}; SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
        DWORD written = 0;
        ok = ok && WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header);
        CloseHandle(file);
        Engine engine; std::unique_ptr<Engine::MappedFile> mapped;
        return ok && !engine.MapIndexFile(path, mapped);
    }
};
}
int main() {
    namespace fs = std::filesystem;
    using namespace pulse::index;
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto root = parent / (L"index-capacity-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(parent);
    if (!fs::create_directory(root)) return 2;
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    SetMachineIndexScope(false); SetActiveIndexDirectory(root.wstring());
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(stdout); failures += !ok; };
    const auto file = root / L"large.bin";
    // Cross the old aggregate cap + 64; no real drive, service or preferences.
    const bool saved = EngineTestAccess::RoundTrip(file.wstring(), 5000065);
    check(saved, "production writer, mapper and search accept more than five million entries");
    if (saved) check(EngineTestAccess::RejectOversizedCount(file.wstring()), "corrupt count beyond signed node IDs remains rejected");
    SetActiveIndexDirectory({});
    if (root.parent_path() != parent || !root.filename().wstring().starts_with(L"index-capacity-")) return 2;
    std::error_code error; fs::remove_all(root, error);
    check(!error && !fs::exists(root), "exclusive fixture is released and cleaned");
    return failures ? 1 : 0;
}
