#include "index_engine.h"
#include "filename_pinyin.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <malloc.h>

namespace pulse::index {
namespace {

using SidecarIdentity = std::array<uint64_t, 10>;
constexpr uint64_t kPinyinSidecarVersion = 3;
constexpr uint64_t kHashBasis = 1469598103934665603ull;
constexpr uint64_t kHashPrime = 1099511628211ull;

#pragma pack(push, 1)
struct SidecarHeaderV3 {
    SidecarIdentity identity{};
    uint64_t hash = 0;
    uint32_t chinese_count = 0;
    uint32_t pair_offsets[677]{};
};
#pragma pack(pop)
static_assert(sizeof(SidecarHeaderV3) == 2800);
static_assert(sizeof(SidecarHeaderV3) % alignof(int32_t) == 0);

void HashSpan(uint64_t& hash, std::span<const int32_t> ids) noexcept {
    hash = (hash ^ ids.size()) * kHashPrime;
    for (int32_t id : ids) {
        hash = (hash ^ static_cast<uint32_t>(id)) * kHashPrime;
    }
}

SidecarIdentity Identity(HANDLE file, const DiskHeader& header) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return {};
    return {0x31595045534c5550ull, (kPinyinSidecarVersion << 32) | kPinyinDataVersion, header.built_unix,
        header.node_count, header.pool_chars, info.dwVolumeSerialNumber,
        (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
        sizeof(wchar_t)};
}

std::wstring FinalSnapshotPath(HANDLE file) {
    std::wstring path(32768, L'\0');
    const DWORD count = GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= path.size()) return {};
    path.resize(count);
    return path;
}

bool ValidateAndHashSpan(std::span<const int32_t> ids, uint64_t max_id,
                         uint64_t& hash, const std::atomic<bool>& stopping) noexcept {
    HashSpan(hash, ids);
    int32_t previous = -1;
    size_t checked = 0;
    for (int32_t id : ids) {
        if ((++checked & 0xfff) == 0 && stopping.load(std::memory_order_relaxed)) return false;
        if (id <= previous || static_cast<uint64_t>(id) >= max_id) return false;
        previous = id;
    }
    return true;
}

bool OpenMappedSidecar(const std::wstring& path, const SidecarIdentity& identity,
                       PinyinPostingStore& out, const std::atomic<bool>& stopping) {
    out.Reset();
    if (path.empty() || !identity[0]) return false;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file, &file_size) ||
        file_size.QuadPart < static_cast<LONGLONG>(sizeof(SidecarHeaderV3)) ||
        static_cast<uint64_t>(file_size.QuadPart) > (std::numeric_limits<size_t>::max)()) {
        CloseHandle(file);
        return false;
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        return false;
    }

    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }

    auto cleanup_fail = [&]() {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    };

    const auto* header = reinterpret_cast<const SidecarHeaderV3*>(view);
    if (header->identity != identity || header->chinese_count > identity[3] || header->pair_offsets[0] != 0) {
        return cleanup_fail();
    }

    for (size_t b = 0; b < 676; ++b) {
        if (header->pair_offsets[b + 1] < header->pair_offsets[b] ||
            (header->pair_offsets[b + 1] - header->pair_offsets[b]) > header->chinese_count) {
            return cleanup_fail();
        }
    }

    const uint64_t total_ids = static_cast<uint64_t>(header->chinese_count) + header->pair_offsets[676];
    if (total_ids > (UINT64_MAX - sizeof(SidecarHeaderV3)) / sizeof(int32_t)) {
        return cleanup_fail();
    }
    const uint64_t expected_bytes = sizeof(SidecarHeaderV3) + total_ids * sizeof(int32_t);
    if (static_cast<uint64_t>(file_size.QuadPart) != expected_bytes) {
        return cleanup_fail();
    }

    const auto* chinese_ptr = reinterpret_cast<const int32_t*>(
        static_cast<const uint8_t*>(view) + sizeof(SidecarHeaderV3));
    const auto* pair_ids_ptr = chinese_ptr + header->chinese_count;

    uint64_t hash = kHashBasis;
    if (!ValidateAndHashSpan(
            header->chinese_count ? std::span<const int32_t>(chinese_ptr, header->chinese_count)
                                  : std::span<const int32_t>{},
            identity[3], hash, stopping)) {
        return cleanup_fail();
    }
    for (size_t b = 0; b < 676; ++b) {
        const uint32_t begin = header->pair_offsets[b];
        const uint32_t end = header->pair_offsets[b + 1];
        const std::span<const int32_t> bucket =
            end > begin ? std::span<const int32_t>(pair_ids_ptr + begin, end - begin) : std::span<const int32_t>{};
        if (!ValidateAndHashSpan(bucket, identity[3], hash, stopping)) {
            return cleanup_fail();
        }
    }
    if (hash != header->hash) {
        return cleanup_fail();
    }

    out.file = file;
    out.mapping = mapping;
    out.view = view;
    out.view_bytes = static_cast<size_t>(file_size.QuadPart);
    out.chinese_ptr = chinese_ptr;
    out.chinese_count = header->chinese_count;
    out.pair_offsets_ptr = header->pair_offsets;
    out.pair_ids_ptr = pair_ids_ptr;
    return true;
}

bool WriteSidecarV3(const std::wstring& path, const SidecarHeaderV3& header,
                    const std::vector<int32_t>& chinese, const std::vector<int32_t>& flat_pairs) {
    if (path.empty() || !header.identity[0]) return false;
    const std::wstring temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream file(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        if (!chinese.empty()) {
            file.write(reinterpret_cast<const char*>(chinese.data()),
                       static_cast<std::streamsize>(chinese.size() * sizeof(int32_t)));
        }
        if (!flat_pairs.empty()) {
            file.write(reinterpret_cast<const char*>(flat_pairs.data()),
                       static_cast<std::streamsize>(flat_pairs.size() * sizeof(int32_t)));
        }
        file.flush();
        if (!file) {
            file.close();
            DeleteFileW(temporary.c_str());
            return false;
        }
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(temporary.c_str());
    return false;
}

} // namespace

void Engine::RequestPinyinBuildLocked() {
    pinyin_ready_ = false;
    {
        std::lock_guard<std::mutex> guard(pinyin_build_mutex_);
        ++pinyin_request_;
        if (!pinyin_thread_.joinable()) {
            pinyin_stopping_ = false;
            pinyin_thread_ = std::thread(&Engine::PinyinWorker, this);
        }
    }
    pinyin_build_cv_.notify_one();
}

void Engine::StopPinyinWorker() {
    {
        std::lock_guard<std::mutex> guard(pinyin_build_mutex_);
        pinyin_stopping_ = true;
    }
    pinyin_build_cv_.notify_one();
    if (pinyin_thread_.joinable()) pinyin_thread_.join();
    pinyin_ready_ = false;
}

void Engine::PinyinWorker() {
    uint64_t processed = 0;
    for (;;) {
        uint64_t generation;
        {
            std::unique_lock<std::mutex> guard(pinyin_build_mutex_);
            pinyin_build_cv_.wait(guard, [&] { return pinyin_stopping_.load() || pinyin_request_.load() != processed; });
            if (pinyin_stopping_) return;
            generation = pinyin_request_.load();
        }
        processed = generation;
        try {
            PinyinPostingStore store;
            bool built_heap = false;
            const MappedFile* snapshot = nullptr;
            std::wstring v2_legacy_path;
            {
                // Queries may run concurrently. Snapshot replacement waits until
                // this reader finishes, so no IDs can outlive their mapped names.
                std::shared_lock<std::shared_mutex> lock(mutex_);
                if (!map_ || generation != pinyin_request_.load()) continue;
                snapshot = map_.get();
                const auto identity = Identity(snapshot->file, *snapshot->hdr);
                const std::wstring base_path = FinalSnapshotPath(snapshot->file);
                const std::wstring path = base_path.empty() ? std::wstring{} : base_path + L".pinyin-v3";
                if (!base_path.empty()) v2_legacy_path = base_path + L".pinyin-v2";

                if (!OpenMappedSidecar(path, identity, store, pinyin_stopping_)) {
                    if (pinyin_stopping_) return;
                    std::vector<int32_t> chinese;
                    std::vector<std::vector<int32_t>> pairs(676);
                    for (int32_t id = 0; id < BaseCount(); ++id) {
                        if ((id & 0x3ff) == 0 && pinyin_stopping_) return;
                        const auto& node = snapshot->nodes[id];
                        const std::wstring_view name(snapshot->pool + node.off, node.len);
                        if (!HasPinyinCharacters(name)) continue;
                        chinese.push_back(id);
                        const auto signature = PinyinCandidatePairs(name);
                        for (size_t word = 0; word < signature.size(); ++word) {
                            uint64_t mask = signature[word];
                            while (mask) {
                                const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
                                pairs[word * 64 + bit].push_back(id);
                                mask &= mask - 1;
                            }
                        }
                    }

                    uint64_t total_pairs = 0;
                    for (const auto& bucket : pairs) total_pairs += bucket.size();
                    if (chinese.size() > (std::numeric_limits<uint32_t>::max)() ||
                        total_pairs > (std::numeric_limits<uint32_t>::max)()) {
                        continue;
                    }

                    SidecarHeaderV3 header{};
                    header.identity = identity;
                    header.chinese_count = static_cast<uint32_t>(chinese.size());
                    std::vector<int32_t> flat_pairs;
                    flat_pairs.reserve(static_cast<size_t>(total_pairs));

                    uint64_t hash = kHashBasis;
                    HashSpan(hash, chinese);
                    uint32_t cursor = 0;
                    for (size_t b = 0; b < 676; ++b) {
                        header.pair_offsets[b] = cursor;
                        auto& bucket = pairs[b];
                        HashSpan(hash, bucket);
                        flat_pairs.insert(flat_pairs.end(), bucket.begin(), bucket.end());
                        cursor += static_cast<uint32_t>(bucket.size());
                        std::vector<int32_t>().swap(bucket);
                    }
                    header.pair_offsets[676] = cursor;
                    header.hash = hash;
                    std::vector<std::vector<int32_t>>().swap(pairs);

                    const bool wrote = WriteSidecarV3(path, header, chinese, flat_pairs);
                    if (wrote && OpenMappedSidecar(path, identity, store, pinyin_stopping_)) {
                        std::vector<int32_t>().swap(chinese);
                        std::vector<int32_t>().swap(flat_pairs);
                        built_heap = true;
                    } else {
                        store.mem_chinese = std::move(chinese);
                        std::copy(std::begin(header.pair_offsets), std::end(header.pair_offsets),
                                  store.mem_offsets.begin());
                        store.mem_pairs = std::move(flat_pairs);
                        built_heap = true;
                    }
                }
            }
            if (!v2_legacy_path.empty()) DeleteFileW(v2_legacy_path.c_str());
            if (built_heap) _heapmin();

            {
                std::unique_lock<std::shared_mutex> lock(mutex_);
                if (pinyin_stopping_ || generation != pinyin_request_.load() || map_.get() != snapshot) continue;
                pinyin_postings_ = std::move(store);
                pinyin_snapshot_ = snapshot;
                pinyin_version_ = kPinyinDataVersion;
                pinyin_ready_ = true;
                InvalidateFilterLocked();
            }
            PingNotify(true);
        } catch (...) {
            // Auxiliary failure must not stop filename indexing or literal search.
            pinyin_ready_ = false;
        }
    }
}

} // namespace pulse::index
