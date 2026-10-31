#include "../ui/thumbnail_disk_cache.h"
#include <filesystem>
#include <cstdio>
#include <fstream>
namespace pulse::ui {
void ThumbnailCache::SetGridStore(ThumbnailStore*) {}
struct ThumbnailWriterTest {
    static bool Bounds(ThumbnailDiskCache& store, const ThumbnailStoreKey& key, const StoredThumbnail& image) {
        std::unique_lock maintenance(store.mutex_);
        const auto started = GetTickCount64();
        for (int i = 0; i < 200; ++i) store.Save(key, 100, image);
        bool ok;
        { std::lock_guard queue(store.writer_mutex_);
          ok = store.writes_.size() <= 64 && store.writer_bytes_ <= (16u << 20); }
        ok = ok && GetTickCount64() - started < 1000;
        maintenance.unlock();
        return ok && store.FlushForTesting(10000);
    }
};
}
int wmain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const auto root = std::filesystem::path(temp) / (L"pulse-writer-test-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"PULSE_TEST_THUMB_CACHE", root.c_str());
    auto& store = pulse::ui::ThumbnailDiskCache::Shared();
    pulse::ui::StoredThumbnail image; image.width = image.height = 32; image.pixels.assign(4096, 255);
    const std::wstring path = root.wstring() + L"\\fixture.png";
    pulse::ui::ThumbnailStoreKey key{path, 0, 32, 123, 456, true};
    { std::wstring short_lived = path;
      store.Save({short_lived, 0, 32, 123, 456, true}, 100, image); }
    bool ok = store.FlushForTesting(10000);
    pulse::ui::StoredThumbnail read;
    ok = ok && store.Load(key, read) && read.width == 32 && read.pixels.size() == 4096;
    std::printf("[%s] asynchronous writer round trip\n", ok ? "PASS" : "FAIL");
    const bool bounded = pulse::ui::ThumbnailWriterTest::Bounds(store, key, image);
    std::printf("[%s] writer queue remains bounded without waiting for blocked maintenance\n", bounded ? "PASS" : "FAIL");
    ok = ok && bounded;
    pulse::ui::StoredThumbnail oversized; oversized.width = oversized.height = 4096;
    oversized.pixels.resize((16u << 20) + 1);
    store.Save(key, 100, oversized);
    ok = store.FlushForTesting(1000) && ok;
    store.Clear(); std::filesystem::remove_all(root);
    { std::ofstream blocker(root); blocker << "blocked cache directory"; }
    store.Save(key, 100, image);
    const bool failed_write_finished = store.FlushForTesting(10000);
    std::filesystem::remove(root);
    store.Save(key, 100, image);
    const bool recovered = failed_write_finished && store.FlushForTesting(10000) && store.Load(key, read);
    std::printf("[%s] writer recovers after cache directory write failure\n", recovered ? "PASS" : "FAIL");
    ok = ok && recovered;
    store.Clear(); std::filesystem::remove_all(root);
    CoUninitialize();
    return ok ? 0 : 1;
}
