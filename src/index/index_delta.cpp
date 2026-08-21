// index_delta.cpp — See index_delta.h.
#include "index_delta.h"
#include "index_config.h"
#include "index_paths.h"
#include "index_shard.h"
#include <algorithm>
#include <cwctype>

namespace pulse::index {
namespace {

#pragma pack(push, 1)
struct DiskDeltaHeader {
    char magic[4];
    uint32_t ver;
    uint64_t base_built;
};
#pragma pack(pop)

bool WriteAll(HANDLE h, const void* p, size_t n) {
    const BYTE* b = static_cast<const BYTE*>(p);
    while (n) {
        DWORD w = 0;
        DWORD chunk = n > (1u << 20) ? (1u << 20) : static_cast<DWORD>(n);
        if (!WriteFile(h, b, chunk, &w, nullptr) || w == 0) return false;
        b += w;
        n -= w;
    }
    return true;
}

bool ReadAll(HANDLE h, void* p, size_t n) {
    BYTE* b = static_cast<BYTE*>(p);
    while (n) {
        DWORD r = 0;
        if (!ReadFile(h, b, static_cast<DWORD>(n), &r, nullptr) || r == 0) return false;
        b += r;
        n -= r;
    }
    return true;
}

} // namespace

std::wstring DeltaFilePath(wchar_t letter) {
    std::wstring dir = DataDir();
    if (dir.empty()) return {};
    wchar_t name[40];
    if (letter)
        swprintf_s(name, L"pulse-index-%c.dlt", static_cast<wchar_t>(towupper(letter)));
    else
        swprintf_s(name, L"pulse-index.dlt");
    return dir + L"\\" + name;
}

std::wstring DeltaFilePathForVolume(const std::wstring& volume_id) {
    std::wstring dir = DataDir();
    if (dir.empty()) return {};
    const std::wstring id = NormalizeVolumeId(volume_id);
    if (id.empty()) return {};
    // New logs live beside the volume shard. The old root-level file is still
    // accepted by the caller as a migration fallback, but new writes never
    // mix two volumes under a drive-letter filename.
    return MakeShardPaths(dir + L"\\Volumes", id).wal_a;
}

void DeltaLog::PutU8(uint8_t v) { pending_.push_back(v); }
void DeltaLog::PutU16(uint16_t v) { PutBytes(&v, sizeof(v)); }
void DeltaLog::PutI32(int32_t v) { PutBytes(&v, sizeof(v)); }
void DeltaLog::PutU64(uint64_t v) { PutBytes(&v, sizeof(v)); }
void DeltaLog::PutBytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    pending_.insert(pending_.end(), b, b + n);
}

bool DeltaLog::Open(const std::wstring& path, uint64_t base_built) {
    Close();
    path_ = path;
    base_built_ = base_built;
    if (path_.empty()) return false;
    file_ = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(file_, &sz)) {
        Close();
        return false;
    }
    bytes_on_disk_ = static_cast<uint64_t>(sz.QuadPart);
    if (bytes_on_disk_ == 0) {
        DiskDeltaHeader hdr{};
        memcpy(hdr.magic, "PDLT", 4);
        hdr.ver = kDeltaVer;
        hdr.base_built = base_built;
        if (!WriteAll(file_, &hdr, sizeof(hdr)) || !FlushFileBuffers(file_)) {
            Close();
            return false;
        }
        bytes_on_disk_ = sizeof(hdr);
        return true;
    }
    DiskDeltaHeader hdr{};
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file_, zero, nullptr, FILE_BEGIN) || !ReadAll(file_, &hdr, sizeof(hdr))) {
        Close();
        return false;
    }
    if (memcmp(hdr.magic, "PDLT", 4) != 0 || hdr.ver != kDeltaVer ||
        hdr.base_built != base_built) {
        Close();
        return false;
    }
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(bytes_on_disk_);
    SetFilePointerEx(file_, end, nullptr, FILE_BEGIN);
    return true;
}

void DeltaLog::Close() {
    if (file_ != INVALID_HANDLE_VALUE) {
        Flush();
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    pending_.clear();
    bytes_on_disk_ = 0;
    path_.clear();
}

bool DeltaLog::Reset(uint64_t base_built) {
    const std::wstring path = path_;
    if (path.empty()) return false;
    Close();
    DeleteFileW(path.c_str());
    return Open(path, base_built);
}

void DeltaLog::QueueAdd(int32_t parent, uint8_t flags, uint32_t mtime, uint64_t size,
                        std::wstring_view name, uint64_t frn) {
    const uint16_t nlen = static_cast<uint16_t>((std::min)(name.size(), static_cast<size_t>(65535)));
    PutU8(static_cast<uint8_t>(DeltaOp::Add));
    PutI32(parent);
    PutU8(flags);
    PutBytes(&mtime, 4);
    PutU64(size);
    PutU16(nlen);
    if (nlen) PutBytes(name.data(), nlen * sizeof(wchar_t));
    PutU64(frn);
}

void DeltaLog::QueuePatch(int32_t idx, uint8_t which, int32_t parent, uint8_t flags,
                          uint32_t mtime, uint64_t size, std::wstring_view name) {
    PutU8(static_cast<uint8_t>(DeltaOp::Patch));
    PutI32(idx);
    PutU8(which);
    if (which & static_cast<uint8_t>(PatchBits::Meta)) {
        PutI32(parent);
        PutU8(flags);
    }
    if (which & static_cast<uint8_t>(PatchBits::Attr)) {
        PutBytes(&mtime, 4);
        PutU64(size);
    }
    if (which & static_cast<uint8_t>(PatchBits::Name)) {
        const uint16_t nlen = static_cast<uint16_t>((std::min)(name.size(), static_cast<size_t>(65535)));
        PutU16(nlen);
        if (nlen) PutBytes(name.data(), nlen * sizeof(wchar_t));
    }
}

void DeltaLog::QueueTomb(int32_t idx) {
    PutU8(static_cast<uint8_t>(DeltaOp::Tomb));
    PutI32(idx);
}

void DeltaLog::QueueUsn(uint64_t journal_id, int64_t next_usn) {
    PutU8(static_cast<uint8_t>(DeltaOp::UsnCkpt));
    PutU64(journal_id);
    PutU64(static_cast<uint64_t>(next_usn));
}

bool DeltaLog::Flush() {
    if (pending_.empty() || file_ == INVALID_HANDLE_VALUE) return true;
    if (!WriteAll(file_, pending_.data(), pending_.size())) return false;
    bytes_on_disk_ += pending_.size();
    pending_.clear();
    return FlushFileBuffers(file_) != FALSE;
}

bool DeltaLog::Replay(const std::wstring& path, uint64_t expected_built, ReplayFn fn) {
    if (path.empty() || !fn) return true;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return true;
    DiskDeltaHeader hdr{};
    if (!ReadAll(h, &hdr, sizeof(hdr)) || memcmp(hdr.magic, "PDLT", 4) != 0 ||
        hdr.ver != kDeltaVer || hdr.base_built != expected_built) {
        CloseHandle(h);
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    const uint64_t total = static_cast<uint64_t>(sz.QuadPart);
    uint64_t pos = sizeof(hdr);
    std::vector<wchar_t> name;
    auto read_u8 = [&](uint8_t& v) {
        if (pos + 1 > total) return false;
        if (!ReadAll(h, &v, 1)) return false;
        ++pos;
        return true;
    };
    auto read_u16 = [&](uint16_t& v) {
        if (pos + 2 > total) return false;
        if (!ReadAll(h, &v, 2)) return false;
        pos += 2;
        return true;
    };
    auto read_i32 = [&](int32_t& v) {
        if (pos + 4 > total) return false;
        if (!ReadAll(h, &v, 4)) return false;
        pos += 4;
        return true;
    };
    auto read_u32 = [&](uint32_t& v) {
        if (pos + 4 > total) return false;
        if (!ReadAll(h, &v, 4)) return false;
        pos += 4;
        return true;
    };
    auto read_u64 = [&](uint64_t& v) {
        if (pos + 8 > total) return false;
        if (!ReadAll(h, &v, 8)) return false;
        pos += 8;
        return true;
    };
    bool ok = true;
    while (ok && pos < total) {
        uint8_t type = 0;
        if (!read_u8(type)) { ok = false; break; }
        const auto op = static_cast<DeltaOp>(type);
        int32_t idx = -1, parent = -1;
        uint8_t flags = 0, which = 0;
        uint32_t mtime = 0;
        uint64_t size = 0, frn = 0, journal_id = 0, next_usn = 0;
        std::wstring_view nm;
        if (op == DeltaOp::Add) {
            uint16_t nlen = 0;
            if (!read_i32(parent) || !read_u8(flags) || !read_u32(mtime) || !read_u64(size) ||
                !read_u16(nlen)) { ok = false; break; }
            name.resize(nlen);
            if (nlen) {
                if (pos + nlen * 2 > total || !ReadAll(h, name.data(), nlen * 2)) { ok = false; break; }
                pos += nlen * 2ull;
            }
            if (!read_u64(frn)) { ok = false; break; }
            nm = { name.data(), nlen };
            fn(op, -1, parent, flags, 0, mtime, size, nm, frn, 0, 0);
        } else if (op == DeltaOp::Patch) {
            if (!read_i32(idx) || !read_u8(which)) { ok = false; break; }
            if (which & static_cast<uint8_t>(PatchBits::Meta)) {
                if (!read_i32(parent) || !read_u8(flags)) { ok = false; break; }
            }
            if (which & static_cast<uint8_t>(PatchBits::Attr)) {
                if (!read_u32(mtime) || !read_u64(size)) { ok = false; break; }
            }
            if (which & static_cast<uint8_t>(PatchBits::Name)) {
                uint16_t nlen = 0;
                if (!read_u16(nlen)) { ok = false; break; }
                name.resize(nlen);
                if (nlen) {
                    if (pos + nlen * 2 > total || !ReadAll(h, name.data(), nlen * 2)) {
                        ok = false; break;
                    }
                    pos += nlen * 2ull;
                }
                nm = { name.data(), nlen };
            }
            fn(op, idx, parent, flags, which, mtime, size, nm, 0, 0, 0);
        } else if (op == DeltaOp::Tomb) {
            if (!read_i32(idx)) { ok = false; break; }
            fn(op, idx, -1, 0, 0, 0, 0, {}, 0, 0, 0);
        } else if (op == DeltaOp::UsnCkpt) {
            if (!read_u64(journal_id) || !read_u64(next_usn)) { ok = false; break; }
            fn(op, -1, -1, 0, 0, 0, 0, {}, 0, journal_id, static_cast<int64_t>(next_usn));
        } else {
            ok = false;
            break;
        }
    }
    CloseHandle(h);
    return ok;
}

} // namespace pulse::index
