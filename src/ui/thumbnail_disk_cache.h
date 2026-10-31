// thumbnail_disk_cache.h - Expensive grid thumbnails kept across sessions.
//
// %LOCALAPPDATA%\Pulse\thumbs\<xx>\<hash>.pt, one small file per thumbnail:
// JPEG (quality 90) for opaque pictures, PNG when there is transparency. Only
// results that were slow to make are written (video posters, HEIC, RAW, PDF,
// large photos, files on network shares); cheap ones are faster to decode
// again than to read back. The key holds the path, file size, modification
// time and pixel size, so an edited file simply misses. Nothing is written for
// EFS-encrypted files or removable media. Least recently used entries go once
// the folder passes 512 MB (thumbnail performance plan in plans/).
// PULSE_TEST_THUMB_CACHE=off disables it; any other value is the folder.
#pragma once
#include "thumbnail_cache.h"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <string>

namespace pulse::ui {

class ThumbnailDiskCache final : public ThumbnailStore {
public:
    static constexpr uint64_t kBudgetBytes = 512ull << 20;
    static constexpr uint32_t kMinDecodeMs = 30;
    static ThumbnailDiskCache& Shared();

    bool Load(const ThumbnailStoreKey& key, StoredThumbnail& out) override;
    void Save(const ThumbnailStoreKey& key, uint32_t decode_ms, const StoredThumbnail& thumb) override;
    // Deletes every entry (Settings). Entries being written meanwhile survive.
    void Clear();
    bool FlushForTesting(uint32_t timeout_ms);
    friend struct ThumbnailWriterTest;
    uint64_t BytesOnDisk();
    // Empty when the cache is turned off or has no folder.
    const std::wstring& Root();

private:
    ThumbnailDiskCache() = default;
    ~ThumbnailDiskCache();
    void WriterLoop();
    void SaveNow(const ThumbnailStoreKey& key, uint32_t decode_ms, const StoredThumbnail& thumb);
    struct WriteRequest { std::wstring path; DWORD attrs = 0; uint32_t pixels = 0; uint64_t modified = 0, size = 0; bool keep_always = false; uint32_t decode_ms = 0; StoredThumbnail thumb; };
    std::mutex writer_mutex_;
    std::condition_variable writer_cv_;
    std::deque<WriteRequest> writes_;
    std::thread writer_;
    size_t writer_bytes_ = 0;
    bool stopping_ = false;
    std::wstring EntryPath(const ThumbnailStoreKey& key) const;
    void ScanLocked();
    void TrimLocked();

    std::once_flag root_once_;
    std::wstring root_;
    std::mutex mutex_;
    bool scanned_ = false;
    uint64_t bytes_ = 0;
};

} // namespace pulse::ui
