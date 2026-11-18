#include "../ui/thumbnail_disk_cache.h"
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <cwctype>
#include <string_view>
namespace pulse::ui {
void ThumbnailCache::SetGridStore(ThumbnailStore*) {}
struct ThumbnailWriterTest {
    static std::wstring LegacyEntryPath(ThumbnailDiskCache& store, const ThumbnailStoreKey& key) {
        // Frozen pre-icon-policy cache format, used to place an actual old entry.
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&](const void* bytes, size_t count) {
            const auto* data = static_cast<const unsigned char*>(bytes);
            for (size_t i = 0; i < count; ++i) { hash ^= data[i]; hash *= 1099511628211ull; }
        };
        for (const wchar_t c : key.path) {
            const wchar_t lower = static_cast<wchar_t>(std::towlower(c));
            mix(&lower, sizeof(lower));
        }
        mix(&key.size, sizeof(key.size)); mix(&key.modified, sizeof(key.modified));
        mix(&key.pixels, sizeof(key.pixels));
        wchar_t name[40]{};
        swprintf_s(name, L"\\%02x\\%016llx.pt", static_cast<unsigned>(hash >> 56),
            static_cast<unsigned long long>(hash));
        return store.Root() + name;
    }
    static bool IconPolicy(ThumbnailDiskCache& store, const std::filesystem::path& root) {
        bool ok = true;
        const auto check = [&](bool value, const char* label) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); ok &= value;
        };
        unsigned fixture_index = 0;
        for (const wchar_t* extension : {L".ico", L".ICO", L".cur", L".CUR", L".png", L".jpg"}) {
            const std::wstring path = (root / (std::wstring(L"fixture") + std::to_wstring(fixture_index++) + extension)).wstring();
            const ThumbnailStoreKey key{path, 0, 256, 123, 456, true};
            const bool icon = std::towlower(extension[1]) == L'i' || std::towlower(extension[1]) == L'c';
            const auto legacy = LegacyEntryPath(store, key);
            const auto current = store.EntryPath(key);
            StoredThumbnail legacy_image;
            legacy_image.width = legacy_image.height = 16; legacy_image.source_width = legacy_image.source_height = 256;
            legacy_image.pixels.assign(16 * 16 * 4, 255);
            store.Save(key, 100, legacy_image);
            const bool written = store.FlushForTesting(10000);
            check(written, "cache policy fixture write completes");
            if (!written) continue;
            if (icon) {
                check(current != legacy, "ICO/CUR receives a format-specific cache version");
                std::filesystem::create_directories(std::filesystem::path(legacy).parent_path());
                std::filesystem::rename(current, legacy);
                StoredThumbnail read;
                check(!store.Load(key, read) && std::filesystem::exists(legacy),
                    "old small ICO/CUR cache misses without deleting the legacy file");
                StoredThumbnail sharp;
                sharp.width = sharp.height = 256; sharp.source_width = sharp.source_height = 256;
                sharp.pixels.assign(256 * 256 * 4, 255);
                store.Save(key, 100, sharp);
                check(store.FlushForTesting(10000) && store.Load(key, read) && read.width == 256 &&
                    store.Load(key, read) && read.width == 256,
                    "new ICO/CUR cache remains a warm hit at the requested resolution");
            } else {
                StoredThumbnail read;
                check(current == legacy && store.Load(key, read) && read.width == 16,
                    "PNG/JPEG retains its existing cache key and warm entry");
            }
        }
        return ok;
    }

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
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const auto root = std::filesystem::path(temp) / (L"pulse-writer-test-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"PULSE_TEST_THUMB_CACHE", root.c_str());
    auto& store = pulse::ui::ThumbnailDiskCache::Shared();
    if (argc == 2 && std::wstring_view(argv[1]) == L"--icon-cache-version") {
        const bool ok = pulse::ui::ThumbnailWriterTest::IconPolicy(store, root);
        store.Clear(); std::filesystem::remove_all(root);
        CoUninitialize();
        return ok ? 0 : 1;
    }
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
