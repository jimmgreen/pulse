#pragma once

#include "thumbnail_bc1.h"
#include "thumbnail_decode_policy.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse::ui {

class MicroThumbPack {
public:
    struct PendingRecord {
        uint64_t identity_hash = 0;
        int64_t mtime = 0;
        uint64_t file_size = 0;
        MicroThumbData thumb;
    };

    MicroThumbPack() {
        InitializeSRWLock(&io_lock_);
        InitializeSRWLock(&index_lock_);
    }

    ~MicroThumbPack() { Close(); }

    MicroThumbPack(const MicroThumbPack&) = delete;
    MicroThumbPack& operator=(const MicroThumbPack&) = delete;

    static uint64_t IdentityHash(const std::wstring& path) noexcept {
        uint64_t hash = 14695981039346656037ull;
        for (wchar_t ch : path) {
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
            const auto v = static_cast<uint16_t>(ch);
            hash ^= v & 0xFFu;
            hash *= 1099511628211ull;
            hash ^= (v >> 8) & 0xFFu;
            hash *= 1099511628211ull;
        }
        if (const uint32_t policy = ThumbnailDecodePolicy(path)) {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                hash ^= (policy >> shift) & 0xFFu;
                hash *= 1099511628211ull;
            }
        }
        return hash;
    }

    bool Open(const std::filesystem::path& root_dir, bool read_only) {
        std::lock_guard wlock(write_mutex_);
        CloseLocked();
        if (root_dir.empty()) return false;

        std::error_code ec;
        if (!read_only) std::filesystem::create_directories(root_dir, ec);
        pack_path_ = root_dir / L"micro_v2.pack";
        read_only_ = read_only;

        if (!OpenHandlesLocked()) return false;
        ScanExistingLocked();
        if (!read_only_ && write_offset_ > kSoftMaxPackBytes) {
            CompactLocked();
        }
        return read_handle_ != INVALID_HANDLE_VALUE;
    }

    void Close() noexcept {
        std::lock_guard wlock(write_mutex_);
        CloseLocked();
    }

    bool Clear() noexcept {
        std::lock_guard wlock(write_mutex_);
        const auto path = pack_path_;
        CloseLocked();
        if (path.empty()) return true;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return !ec || !std::filesystem::exists(path, ec);
    }

    bool Contains(uint64_t identity_hash, int64_t mtime, uint64_t file_size) const noexcept {
        if (!identity_hash) return false;
        AcquireSRWLockShared(&index_lock_);
        const auto it = index_.find(identity_hash);
        const bool match = it != index_.end() && it->second.mtime == mtime && it->second.file_size == file_size;
        ReleaseSRWLockShared(&index_lock_);
        return match;
    }

    bool Read(uint64_t identity_hash, int64_t mtime, uint64_t file_size, MicroThumbData& out) const {
        if (!identity_hash) return false;
        Entry entry{};
        AcquireSRWLockShared(&index_lock_);
        const auto it = index_.find(identity_hash);
        const bool found = it != index_.end() && it->second.mtime == mtime && it->second.file_size == file_size;
        if (found) entry = it->second;
        ReleaseSRWLockShared(&index_lock_);
        if (!found || entry.block_bytes == 0) return false;

        std::vector<uint8_t> blocks(entry.block_bytes);
        AcquireSRWLockShared(&io_lock_);
        const bool ok = read_handle_ != INVALID_HANDLE_VALUE &&
            ReadPositional(read_handle_, entry.offset, blocks.data(), entry.block_bytes);
        ReleaseSRWLockShared(&io_lock_);
        if (!ok) return false;

        out.width = entry.width;
        out.height = entry.height;
        out.content_width = entry.content_width;
        out.content_height = entry.content_height;
        out.source_width = entry.source_width;
        out.source_height = entry.source_height;
        out.format = entry.format;
        out.blocks = std::move(blocks);
        return true;
    }

    size_t AppendBatch(const std::vector<PendingRecord>& records) {
        if (records.empty() || read_only_) return 0;
        std::lock_guard wlock(write_mutex_);
        if (write_handle_ == INVALID_HANDLE_VALUE || write_offset_ >= kHardMaxPackBytes) return 0;

        uint64_t cursor = write_offset_;
        if (cursor < sizeof(PackFileHeader)) {
            PackFileHeader fh{};
            if (!WritePositionalLocked(0, &fh, sizeof(fh))) return 0;
            cursor = sizeof(PackFileHeader);
            write_offset_ = cursor;
        }

        std::vector<uint8_t> buffer;
        size_t reserve_bytes = 0;
        for (const auto& rec : records) {
            if (!IsValidRecord(rec)) continue;
            reserve_bytes += sizeof(PackRecordHeader) + rec.thumb.blocks.size();
        }
        if (reserve_bytes == 0) return 0;
        buffer.reserve(reserve_bytes);

        struct StagedEntry {
            uint64_t identity_hash = 0;
            Entry entry{};
        };
        std::vector<StagedEntry> staged;
        staged.reserve(records.size());

        for (const auto& rec : records) {
            if (!IsValidRecord(rec)) continue;
            if (cursor + buffer.size() + sizeof(PackRecordHeader) + rec.thumb.blocks.size() > kHardMaxPackBytes) {
                break;
            }
            PackRecordHeader hdr{};
            hdr.magic = kRecordMagic;
            hdr.block_bytes = static_cast<uint32_t>(rec.thumb.blocks.size());
            hdr.identity_hash = rec.identity_hash;
            hdr.mtime = rec.mtime;
            hdr.file_size = rec.file_size;
            hdr.source_width = rec.thumb.source_width;
            hdr.source_height = rec.thumb.source_height;
            hdr.width = rec.thumb.width;
            hdr.height = rec.thumb.height;
            hdr.content_width = rec.thumb.content_width;
            hdr.content_height = rec.thumb.content_height;
            hdr.format = rec.thumb.format;

            const size_t hdr_pos = buffer.size();
            buffer.resize(hdr_pos + sizeof(PackRecordHeader) + hdr.block_bytes);
            std::memcpy(buffer.data() + hdr_pos, &hdr, sizeof(PackRecordHeader));
            std::memcpy(buffer.data() + hdr_pos + sizeof(PackRecordHeader),
                        rec.thumb.blocks.data(), hdr.block_bytes);

            Entry e{};
            e.offset = cursor + hdr_pos + sizeof(PackRecordHeader);
            e.mtime = rec.mtime;
            e.file_size = rec.file_size;
            e.source_width = rec.thumb.source_width;
            e.source_height = rec.thumb.source_height;
            e.width = rec.thumb.width;
            e.height = rec.thumb.height;
            e.content_width = rec.thumb.content_width;
            e.content_height = rec.thumb.content_height;
            e.block_bytes = hdr.block_bytes;
            e.format = rec.thumb.format;
            staged.push_back({rec.identity_hash, e});
        }

        if (buffer.empty()) return 0;
        if (!WritePositionalLocked(cursor, buffer.data(), buffer.size())) return 0;
        write_offset_ = cursor + buffer.size();

        AcquireSRWLockExclusive(&index_lock_);
        for (const auto& s : staged) {
            index_[s.identity_hash] = s.entry;
        }
        ReleaseSRWLockExclusive(&index_lock_);
        return staged.size();
    }

    size_t EntryCount() const noexcept {
        AcquireSRWLockShared(&index_lock_);
        const size_t n = index_.size();
        ReleaseSRWLockShared(&index_lock_);
        return n;
    }

    uint64_t FileBytes() const noexcept { return write_offset_; }

private:
    static constexpr uint64_t kFileMagic = 0x314B504F5243494Dull;  // "MICROPK1"
    // Version 1 may contain vertically inverted top-down Shell DIBs.
    static constexpr uint32_t kFileVersion = 2u;
    static constexpr uint32_t kRecordMagic = 0x4345524Du;          // "MREC"
    static constexpr uint64_t kSoftMaxPackBytes = 64ull << 20;
    static constexpr uint64_t kHardMaxPackBytes = 96ull << 20;

    struct PackFileHeader {
        uint64_t magic = kFileMagic;
        uint32_t version = kFileVersion;
        uint32_t reserved = 0;
    };
    static_assert(sizeof(PackFileHeader) == 16);

    struct PackRecordHeader {
        uint32_t magic = kRecordMagic;
        uint32_t block_bytes = 0;
        uint64_t identity_hash = 0;
        int64_t mtime = 0;
        uint64_t file_size = 0;
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t content_width = 0;
        uint16_t content_height = 0;
        uint8_t format = 0;
        uint8_t reserved[3]{};
        uint32_t reserved2 = 0;
    };
    static_assert(sizeof(PackRecordHeader) == 56);

    struct Entry {
        uint64_t offset = 0;
        int64_t mtime = 0;
        uint64_t file_size = 0;
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t content_width = 0;
        uint16_t content_height = 0;
        uint32_t block_bytes = 0;
        uint8_t format = 0;
    };

    struct ThreadIoEvent {
        HANDLE handle = nullptr;
        ThreadIoEvent() noexcept : handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
        ~ThreadIoEvent() noexcept {
            if (handle) CloseHandle(handle);
        }
    };

    static HANDLE ThreadEventHandle() noexcept {
        thread_local ThreadIoEvent ev;
        return ev.handle;
    }

    static bool IsValidHeader(const PackRecordHeader& hdr) noexcept {
        if (hdr.magic != kRecordMagic || hdr.identity_hash == 0) return false;
        if (hdr.width < 4u || hdr.width > kMicroThumbMaxEdge || (hdr.width & 3u) != 0) return false;
        if (hdr.height < 4u || hdr.height > kMicroThumbMaxEdge || (hdr.height & 3u) != 0) return false;
        if (hdr.content_width == 0 || hdr.content_width > hdr.width) return false;
        if (hdr.content_height == 0 || hdr.content_height > hdr.height) return false;
        if (hdr.format != kMicroThumbFormatBc1 && hdr.format != kMicroThumbFormatBc3) return false;
        const uint32_t block_size = hdr.format == kMicroThumbFormatBc3 ? 16u : 8u;
        const uint32_t expected = (uint32_t(hdr.width) / 4u) * (uint32_t(hdr.height) / 4u) * block_size;
        return hdr.block_bytes == expected;
    }

    static bool IsValidRecord(const PendingRecord& rec) noexcept {
        if (rec.identity_hash == 0) return false;
        if (rec.thumb.width < 4u || rec.thumb.width > kMicroThumbMaxEdge || (rec.thumb.width & 3u) != 0) return false;
        if (rec.thumb.height < 4u || rec.thumb.height > kMicroThumbMaxEdge || (rec.thumb.height & 3u) != 0) return false;
        if (rec.thumb.content_width == 0 || rec.thumb.content_width > rec.thumb.width) return false;
        if (rec.thumb.content_height == 0 || rec.thumb.content_height > rec.thumb.height) return false;
        if (rec.thumb.format != kMicroThumbFormatBc1 && rec.thumb.format != kMicroThumbFormatBc3) return false;
        const uint32_t block_size = rec.thumb.format == kMicroThumbFormatBc3 ? 16u : 8u;
        const size_t expected = size_t(rec.thumb.width / 4u) * (rec.thumb.height / 4u) * block_size;
        return rec.thumb.blocks.size() == expected;
    }

    static bool ReadPositional(HANDLE handle, uint64_t offset, void* dst, size_t bytes) noexcept {
        HANDLE ev = ThreadEventHandle();
        if (!ev || handle == INVALID_HANDLE_VALUE) return false;
        size_t done = 0;
        while (done < bytes) {
            const uint64_t pos = offset + done;
            const DWORD want = static_cast<DWORD>((std::min<size_t>)(bytes - done, 1u << 20));
            ResetEvent(ev);
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFull);
            ov.OffsetHigh = static_cast<DWORD>((pos >> 32) & 0xFFFFFFFFull);
            ov.hEvent = ev;
            DWORD got = 0;
            BOOL ok = ReadFile(handle, static_cast<uint8_t*>(dst) + done, want, &got, &ov);
            if (!ok) {
                const DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    ok = GetOverlappedResult(handle, &ov, &got, TRUE);
                }
                if (!ok) return false;
            }
            if (got == 0) return false;
            done += got;
        }
        return true;
    }

    bool WritePositionalLocked(uint64_t offset, const void* src, size_t bytes) noexcept {
        HANDLE ev = ThreadEventHandle();
        if (!ev) return false;
        AcquireSRWLockShared(&io_lock_);
        const HANDLE wh = write_handle_;
        bool ok_all = wh != INVALID_HANDLE_VALUE;
        size_t done = 0;
        while (ok_all && done < bytes) {
            const uint64_t pos = offset + done;
            const DWORD want = static_cast<DWORD>((std::min<size_t>)(bytes - done, 1u << 20));
            ResetEvent(ev);
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFull);
            ov.OffsetHigh = static_cast<DWORD>((pos >> 32) & 0xFFFFFFFFull);
            ov.hEvent = ev;
            DWORD wrote = 0;
            BOOL ok = WriteFile(wh, static_cast<const uint8_t*>(src) + done, want, &wrote, &ov);
            if (!ok) {
                const DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    ok = GetOverlappedResult(wh, &ov, &wrote, TRUE);
                }
                if (!ok) {
                    ok_all = false;
                    break;
                }
            }
            if (wrote == 0) {
                ok_all = false;
                break;
            }
            done += wrote;
        }
        ReleaseSRWLockShared(&io_lock_);
        return ok_all;
    }

    bool OpenHandlesLocked() noexcept {
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED;
        HANDLE rh = CreateFileW(pack_path_.c_str(), GENERIC_READ, share, nullptr,
                                read_only_ ? OPEN_EXISTING : OPEN_ALWAYS, flags, nullptr);
        if (rh == INVALID_HANDLE_VALUE) return false;
        HANDLE wh = INVALID_HANDLE_VALUE;
        if (!read_only_) {
            wh = CreateFileW(pack_path_.c_str(), GENERIC_WRITE, share, nullptr,
                             OPEN_ALWAYS, flags, nullptr);
            if (wh == INVALID_HANDLE_VALUE) {
                CloseHandle(rh);
                return false;
            }
        }
        AcquireSRWLockExclusive(&io_lock_);
        read_handle_ = rh;
        write_handle_ = wh;
        ReleaseSRWLockExclusive(&io_lock_);
        return true;
    }

    void CloseLocked() noexcept {
        CloseHandlesOnlyLocked();
        AcquireSRWLockExclusive(&index_lock_);
        index_.clear();
        ReleaseSRWLockExclusive(&index_lock_);
        write_offset_ = 0;
    }

    void CloseHandlesOnlyLocked() noexcept {
        AcquireSRWLockExclusive(&io_lock_);
        const HANDLE rh = read_handle_;
        const HANDLE wh = write_handle_;
        read_handle_ = INVALID_HANDLE_VALUE;
        write_handle_ = INVALID_HANDLE_VALUE;
        ReleaseSRWLockExclusive(&io_lock_);
        if (wh != INVALID_HANDLE_VALUE) CloseHandle(wh);
        if (rh != INVALID_HANDLE_VALUE) CloseHandle(rh);
    }

    void ScanExistingLocked() {
        LARGE_INTEGER size_li{};
        if (read_handle_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(read_handle_, &size_li) ||
            size_li.QuadPart < static_cast<LONGLONG>(sizeof(PackFileHeader))) {
            write_offset_ = 0;
            return;
        }
        const uint64_t total_size = static_cast<uint64_t>(size_li.QuadPart);
        PackFileHeader fh{};
        if (!ReadPositional(read_handle_, 0, &fh, sizeof(fh)) ||
            fh.magic != kFileMagic || fh.version != kFileVersion) {
            write_offset_ = 0;
            return;
        }

        // Stream linearly in 256 KiB chunks so opening 10k+ records finishes in milliseconds.
        constexpr size_t kChunkBytes = 256u << 10;
        std::vector<uint8_t> chunk;
        uint64_t pos = sizeof(PackFileHeader);
        uint64_t last_good = sizeof(PackFileHeader);
        std::unordered_map<uint64_t, Entry> parsed;
        parsed.reserve(4096);

        while (pos + sizeof(PackRecordHeader) <= total_size) {
            const size_t to_read = static_cast<size_t>((std::min<uint64_t>)(total_size - pos, kChunkBytes));
            chunk.resize(to_read);
            if (!ReadPositional(read_handle_, pos, chunk.data(), to_read)) break;

            size_t cursor = 0;
            bool stopped = false;
            bool need_next_chunk = false;
            while (cursor + sizeof(PackRecordHeader) <= chunk.size()) {
                PackRecordHeader hdr{};
                std::memcpy(&hdr, chunk.data() + cursor, sizeof(hdr));
                if (!IsValidHeader(hdr)) {
                    stopped = true;
                    break;
                }
                const uint64_t rec_total = sizeof(PackRecordHeader) + uint64_t(hdr.block_bytes);
                if (pos + cursor + rec_total > total_size) {
                    stopped = true;
                    break;
                }
                Entry e{};
                e.offset = pos + cursor + sizeof(PackRecordHeader);
                e.mtime = hdr.mtime;
                e.file_size = hdr.file_size;
                e.source_width = hdr.source_width;
                e.source_height = hdr.source_height;
                e.width = hdr.width;
                e.height = hdr.height;
                e.content_width = hdr.content_width;
                e.content_height = hdr.content_height;
                e.block_bytes = hdr.block_bytes;
                e.format = hdr.format;
                parsed[hdr.identity_hash] = e;

                last_good = pos + cursor + rec_total;
                if (cursor + rec_total <= chunk.size()) {
                    cursor += static_cast<size_t>(rec_total);
                } else {
                    need_next_chunk = true;
                    break;
                }
            }
            if (stopped) break;
            if (!need_next_chunk && pos == last_good) break;
            pos = last_good;
        }

        AcquireSRWLockExclusive(&index_lock_);
        index_ = std::move(parsed);
        ReleaseSRWLockExclusive(&index_lock_);
        write_offset_ = last_good;
    }

    void CompactLocked() {
        if (read_handle_ == INVALID_HANDLE_VALUE || write_handle_ == INVALID_HANDLE_VALUE) return;
        std::vector<std::pair<uint64_t, Entry>> items;
        AcquireSRWLockShared(&index_lock_);
        items.reserve(index_.size());
        for (const auto& kv : index_) items.push_back(kv);
        ReleaseSRWLockShared(&index_lock_);
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            return a.second.offset > b.second.offset;
        });

        const auto tmp_path = pack_path_.wstring() + L".compact.tmp";
        HANDLE th = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (th == INVALID_HANDLE_VALUE) return;

        HANDLE ev = ThreadEventHandle();
        auto write_tmp = [&](uint64_t off, const void* ptr, size_t n) -> bool {
            if (!ev) return false;
            size_t d = 0;
            while (d < n) {
                const uint64_t p = off + d;
                const DWORD w = static_cast<DWORD>((std::min<size_t>)(n - d, 1u << 20));
                OVERLAPPED ov{};
                ov.Offset = static_cast<DWORD>(p & 0xFFFFFFFFull);
                ov.OffsetHigh = static_cast<DWORD>((p >> 32) & 0xFFFFFFFFull);
                ov.hEvent = ev;
                DWORD wrote = 0;
                BOOL ok = WriteFile(th, static_cast<const uint8_t*>(ptr) + d, w, &wrote, &ov);
                if (!ok) {
                    if (GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(th, &ov, &wrote, TRUE);
                    if (!ok || !wrote) return false;
                }
                d += wrote;
            }
            return true;
        };

        PackFileHeader fh{};
        std::vector<uint8_t> out_buf;
        out_buf.reserve(512u << 10);
        out_buf.resize(sizeof(fh));
        std::memcpy(out_buf.data(), &fh, sizeof(fh));

        uint64_t file_offset = 0;
        uint64_t kept_bytes = sizeof(fh);
        constexpr uint64_t kKeepBudget = 40ull << 20;
        std::unordered_map<uint64_t, Entry> new_index;
        new_index.reserve(items.size());
        std::vector<uint8_t> read_buf;
        bool write_ok = true;

        for (const auto& [hash, old_entry] : items) {
            if (kept_bytes + sizeof(PackRecordHeader) + old_entry.block_bytes > kKeepBudget) break;
            read_buf.resize(old_entry.block_bytes);
            if (!ReadPositional(read_handle_, old_entry.offset, read_buf.data(), old_entry.block_bytes)) {
                continue;
            }
            PackRecordHeader hdr{};
            hdr.magic = kRecordMagic;
            hdr.block_bytes = old_entry.block_bytes;
            hdr.identity_hash = hash;
            hdr.mtime = old_entry.mtime;
            hdr.file_size = old_entry.file_size;
            hdr.source_width = old_entry.source_width;
            hdr.source_height = old_entry.source_height;
            hdr.width = old_entry.width;
            hdr.height = old_entry.height;
            hdr.content_width = old_entry.content_width;
            hdr.content_height = old_entry.content_height;
            hdr.format = old_entry.format;

            const uint64_t rec_start = file_offset + out_buf.size();
            const size_t pos = out_buf.size();
            out_buf.resize(pos + sizeof(hdr) + read_buf.size());
            std::memcpy(out_buf.data() + pos, &hdr, sizeof(hdr));
            std::memcpy(out_buf.data() + pos + sizeof(hdr), read_buf.data(), read_buf.size());

            Entry ne = old_entry;
            ne.offset = rec_start + sizeof(hdr);
            new_index[hash] = ne;
            kept_bytes += sizeof(hdr) + read_buf.size();

            if (out_buf.size() >= (256u << 10)) {
                if (!write_tmp(file_offset, out_buf.data(), out_buf.size())) {
                    write_ok = false;
                    break;
                }
                file_offset += out_buf.size();
                out_buf.clear();
            }
        }
        if (write_ok && !out_buf.empty()) {
            if (!write_tmp(file_offset, out_buf.data(), out_buf.size())) write_ok = false;
            else file_offset += out_buf.size();
        }
        CloseHandle(th);
        std::error_code ec;
        if (write_ok) {
            CloseHandlesOnlyLocked();
            if (MoveFileExW(tmp_path.c_str(), pack_path_.c_str(), MOVEFILE_REPLACE_EXISTING) &&
                OpenHandlesLocked()) {
                AcquireSRWLockExclusive(&index_lock_);
                index_ = std::move(new_index);
                ReleaseSRWLockExclusive(&index_lock_);
                write_offset_ = file_offset;
                return;
            }
            OpenHandlesLocked();
        }
        std::filesystem::remove(tmp_path, ec);
    }

    std::filesystem::path pack_path_;
    bool read_only_ = false;
    mutable SRWLOCK io_lock_{};
    mutable SRWLOCK index_lock_{};
    mutable std::mutex write_mutex_;
    HANDLE read_handle_ = INVALID_HANDLE_VALUE;
    HANDLE write_handle_ = INVALID_HANDLE_VALUE;
    uint64_t write_offset_ = 0;
    std::unordered_map<uint64_t, Entry> index_;
};

}  // namespace pulse::ui
