#include "../index/index_engine.h"
#include "../index/filename_pinyin.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
namespace pulse::index {
struct EngineTestAccess {
    static bool Fixture(Engine& e, const std::wstring& path, int count) {
        Engine::Store store;
        e.AddNodeLocked(store, -1, L"C:", Engine::kFlagDir);
        for (int i = 0; i < count; ++i) {
            const std::wstring prefix = i % 20 == 0 ? L"中国报告" : L"ordinary";
            e.AddNodeLocked(store, 0, prefix + std::to_wstring(i) + L".txt", 0);
        }
        e.AddNodeLocked(store, 0, L"重庆音乐.txt", 0);
        std::vector<Engine::VolState> volumes(1);
        volumes[0].letter = L'C';
        volumes[0].volume_id = L"pinyin-test-volume";
        volumes[0].root_idx = 0;
        volumes[0].first_idx = 0;
        volumes[0].item_count = store.nodes.size();
        if (!e.WriteIndexFile(path, store, volumes, 123456) ||
            !MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!e.MapIndexFile(path, mapped)) return false;
        e.AdoptMappedLocked(std::move(mapped));
        for (int i = 0; i < 1000 && !e.PinyinReady(); ++i) Sleep(10);
        return e.PinyinReady();
    }
    static bool Reload(Engine& e, const std::wstring& path) {
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!e.MapIndexFile(path, mapped)) return false;
        e.AdoptMappedLocked(std::move(mapped));
        for (int i = 0; i < 1000 && !e.PinyinReady(); ++i) Sleep(10);
        return e.PinyinReady();
    }
    static void Rename(Engine& e, int32_t id, std::wstring_view name) {
        auto& patch = e.patches_[id];
        patch.off = static_cast<uint32_t>(e.live_.pool.size());
        patch.len = static_cast<uint16_t>(name.size());
        patch.has_name = true;
        e.live_.pool.insert(e.live_.pool.end(), name.begin(), name.end());
        e.InvalidateFilterLocked();
    }
    static void Delete(Engine& e, int32_t id) {
        e.tombstones_.insert(id);
        e.InvalidateFilterLocked();
    }
    static void ClearQueryCache(Engine& e) { e.InvalidateFilterLocked(); }
    static size_t ChineseCandidates(const Engine& e) { return e.PinyinChineseIdsLocked().size(); }
    static bool IsMapped(const Engine& e) { return e.PinyinIsMappedLocked(); }
    static size_t PrivateHeapBytes(const Engine& e) { return e.PinyinPrivateHeapBytesLocked(); }
};
}
int wmain(int argc, wchar_t** argv) {
    using namespace pulse::index;
    const int count = argc > 1 && std::wstring_view(argv[1]) == L"--million" ? 1000000 : 10000;
    const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"pinyin-test-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(dir);
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    {
        Engine engine;
        check(EngineTestAccess::Fixture(engine, (dir / L"snapshot.bin").wstring(), count), "real snapshot fixture");
        auto search = [&](const wchar_t* text) { Query q; q.needle = text; return engine.Search(q); };
        check(search(L"chongqing").total == 1, "pinyin survives literal posting optimization");
        check(search(L"cqyy").total == 1, "polyphonic initials in snapshot");
        check(search(L"chongqing nopinyin:").total == 0, "disabled pinyin snapshot");
        check(search(L"zgbg").total == static_cast<size_t>(count / 20), "all Chinese-only candidates found");
        check(EngineTestAccess::ChineseCandidates(engine) == static_cast<size_t>(count / 20 + 1), "auxiliary IDs contain only Chinese names");
        check(EngineTestAccess::IsMapped(engine) && EngineTestAccess::PrivateHeapBytes(engine) == 0, "pinyin sidecar mapped zero-copy with zero private heap");
        check(search(L"z").total == 0 && search(L"zh").total != 0, "typing transition invalidates narrow reuse");
        EngineTestAccess::Rename(engine, count + 1, L"银行.txt");
        check(search(L"chongqing").total == 0 && search(L"yinhang").total == 1, "rename replaces cached pronunciation");
        EngineTestAccess::Rename(engine, 2, L"音乐.txt");
        check(search(L"yinyue").total == 1, "ASCII to Chinese rename enters overlay");
        EngineTestAccess::Delete(engine, 2);
        check(search(L"yinyue").total == 0, "delete removes pronunciation match");
        engine.AddForTest(L"C:\\新增文件.txt", L"新增文件.txt", false);
        check(search(L"xin增wenjian").total == 1, "new mutable name supports mixed script");
        std::vector<double> latencies;
        for (int i = 0; i < 25; ++i) {
            EngineTestAccess::ClearQueryCache(engine); // measure hot auxiliary index, not same-query cache
            const auto start = std::chrono::steady_clock::now();
            const auto result = search(i % 2 ? L"zhongguobaogao999" : L"zgbg999");
            (void)result;
            latencies.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(latencies.begin(), latencies.end());
        std::cout << "[INFO] " << count << " filenames, 5% Chinese, hot auxiliary P95=" << latencies[23] << " ms\n";
        check(latencies[23] <= 100.0, "hot pinyin P95 <= 100ms");
        engine.Stop();
        check(EngineTestAccess::Reload(engine, (dir / L"snapshot.bin").wstring()) && search(L"chongqing").total == 1,
              "restart loads snapshot-bound sidecar and drops old overlay");
        engine.Stop();
        {
            std::ofstream corrupt(dir / L"snapshot.bin.pinyin-v2", std::ios::binary | std::ios::trunc);
            corrupt << "invalid auxiliary data";
        }
        check(EngineTestAccess::Reload(engine, (dir / L"snapshot.bin").wstring()) && search(L"chongqing").total == 1,
              "corrupt sidecar rebuilds without changing filename snapshot");
    }
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
