// index_mft.cpp — Parse NTFS file records from $MFT data runs.
#include "index_mft.h"
#include <winioctl.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace pulse::index {
namespace {

constexpr uint32_t kAttrStdInfo = 0x10;
constexpr uint32_t kAttrFileName = 0x30;
constexpr uint32_t kAttrData = 0x80;
constexpr uint32_t kAttrEnd = 0xFFFFFFFF;

#pragma pack(push, 1)
struct FileRecord {
    uint32_t magic;
    uint16_t usa_off;
    uint16_t usa_count;
    uint64_t lsn;
    uint16_t seq;
    uint16_t links;
    uint16_t attr_off;
    uint16_t flags;
    uint32_t bytes_used;
    uint32_t bytes_alloc;
    uint64_t base;
    uint16_t next_attr;
};
struct AttrHeader {
    uint32_t type;
    uint32_t length;
    uint8_t non_resident;
    uint8_t name_len;
    uint16_t name_off;
    uint16_t flags;
    uint16_t id;
};
struct AttrResident {
    uint32_t value_len;
    uint16_t value_off;
    uint8_t indexed;
    uint8_t pad;
};
struct AttrNonResident {
    uint64_t start_vcn;
    uint64_t last_vcn;
    uint16_t pairs_off;
    uint8_t compression;
    uint8_t pad[5];
    uint64_t allocated;
    uint64_t real_size;
    uint64_t initialized;
};
#pragma pack(pop)

bool ReadAt(HANDLE h, uint64_t off, void* buf, DWORD len) {
    if (off > static_cast<uint64_t>((std::numeric_limits<LONGLONG>::max)())) return false;
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(off);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(h, buf, len, &got, nullptr) && got == len;
}

int64_t ReadLe(const BYTE* p, int n, bool sign) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    if (sign && n > 0 && n < 8 && (p[n - 1] & 0x80)) v |= ~0ull << (8 * n);
    return static_cast<int64_t>(v);
}

bool CheckedAdd(uint64_t a, uint64_t b, uint64_t& result) {
    if (b > (std::numeric_limits<uint64_t>::max)() - a) return false;
    result = a + b;
    return true;
}

bool CheckedMultiply(uint64_t a, uint64_t b, uint64_t& result) {
    if (a != 0 && b > (std::numeric_limits<uint64_t>::max)() / a) return false;
    result = a * b;
    return true;
}

struct Run {
    uint64_t lcn = 0;
    uint64_t clusters = 0;
    bool sparse = false;
};

bool DecodeRuns(const BYTE* p, const BYTE* end, std::vector<Run>& out) {
    int64_t lcn = 0;
    while (p < end && *p != 0) {
        const int len_n = *p & 0x0F;
        const int off_n = (*p >> 4) & 0x0F;
        ++p;
        if (len_n == 0 || len_n > 8 || off_n > 8 ||
            static_cast<size_t>(end - p) < static_cast<size_t>(len_n + off_n))
            return false;
        const uint64_t clusters = static_cast<uint64_t>(ReadLe(p, len_n, false));
        p += len_n;
        Run r;
        r.clusters = clusters;
        if (off_n == 0) {
            r.sparse = true;
        } else {
            const int64_t delta = ReadLe(p, off_n, true);
            if ((delta > 0 && lcn > (std::numeric_limits<int64_t>::max)() - delta) ||
                (delta < 0 && lcn < (std::numeric_limits<int64_t>::min)() - delta))
                return false;
            lcn += delta;
            if (lcn < 0) return false;
            r.lcn = static_cast<uint64_t>(lcn);
            p += off_n;
        }
        if (r.clusters) out.push_back(r);
    }
    return p < end && *p == 0;
}

bool ApplyUsa(BYTE* rec, uint32_t rec_size, uint32_t sector) {
    if (rec_size < sizeof(FileRecord) || sector < 2) return false;
    auto* h = reinterpret_cast<FileRecord*>(rec);
    if (h->usa_off < sizeof(FileRecord) || h->usa_count < 1) return false;
    const uint32_t usa_bytes = static_cast<uint32_t>(h->usa_count) * 2;
    if (static_cast<uint32_t>(h->usa_off) + usa_bytes > rec_size) return false;
    auto* usa = reinterpret_cast<uint16_t*>(rec + h->usa_off);
    for (uint16_t i = 1; i < h->usa_count; ++i) {
        const uint64_t off = static_cast<uint64_t>(i) * sector - 2;
        if (off > rec_size || rec_size - static_cast<uint32_t>(off) < 2) return false;
        *reinterpret_cast<uint16_t*>(rec + static_cast<uint32_t>(off)) = usa[i];
    }
    return true;
}

const BYTE* AttrValue(const BYTE* attr, uint32_t& len) {
    auto* h = reinterpret_cast<const AttrHeader*>(attr);
    if (h->non_resident ||
        h->length < sizeof(AttrHeader) + sizeof(AttrResident)) {
        len = 0;
        return nullptr;
    }
    auto* r = reinterpret_cast<const AttrResident*>(attr + sizeof(AttrHeader));
    len = r->value_len;
    const uint32_t min_off = sizeof(AttrHeader) + sizeof(AttrResident);
    if (r->value_off < min_off || r->value_off > h->length ||
        len > h->length - r->value_off) {
        len = 0;
        return nullptr;
    }
    return attr + r->value_off;
}

bool ParseRecord(BYTE* rec, uint32_t rec_size, uint32_t sector, uint64_t index,
                 const std::function<bool(MftFile&&)>& emit) {
    if (rec_size < sizeof(FileRecord)) return true;
    auto* hdr = reinterpret_cast<FileRecord*>(rec);
    if (hdr->magic != 0x454C4946) return true; // 'FILE'
    if (!ApplyUsa(rec, rec_size, sector)) return true;
    if ((hdr->flags & 1) == 0) return true; // not in use
    if (hdr->base != 0) return true;        // extension record; base already holds names
    if (hdr->bytes_used > rec_size || hdr->attr_off < sizeof(FileRecord) ||
        hdr->attr_off > hdr->bytes_used)
        return true;

    MftFile best;
    best.frn = (static_cast<uint64_t>(hdr->seq) << 48) | (index & 0xFFFFFFFFFFFFULL);
    best.is_dir = (hdr->flags & 2) != 0;
    uint8_t best_name_type = 0xFF;
    uint64_t data_size = 0;
    bool have_data = false;

    const BYTE* p = rec + hdr->attr_off;
    const BYTE* end = rec + hdr->bytes_used;
    while (static_cast<size_t>(end - p) >= sizeof(AttrHeader)) {
        auto* a = reinterpret_cast<const AttrHeader*>(p);
        if (a->type == kAttrEnd || a->length < sizeof(AttrHeader)) break;
        if (a->length > static_cast<size_t>(end - p)) break;
        const bool unnamed = a->name_len == 0;

        if (a->type == kAttrStdInfo && !a->non_resident) {
            uint32_t vlen = 0;
            const BYTE* v = AttrValue(p, vlen);
            if (v && vlen >= 24)
                std::memcpy(&best.mtime, v + 8, 8); // last modification
            if (v && vlen >= 36) {
                uint32_t attrs = 0;
                std::memcpy(&attrs, v + 32, 4);
                if (attrs & FILE_ATTRIBUTE_DIRECTORY) best.is_dir = true;
            }
        } else if (a->type == kAttrFileName && !a->non_resident) {
            uint32_t vlen = 0;
            const BYTE* v = AttrValue(p, vlen);
            if (v && vlen >= 66) {
                uint64_t parent = 0, fsize = 0;
                std::memcpy(&parent, v, 8);
                std::memcpy(&fsize, v + 48, 8);
                const BYTE nlen = v[64];
                const BYTE ntype = v[65];
                const uint32_t nbytes = static_cast<uint32_t>(nlen) * 2;
                auto rank = [](uint8_t t) { return (t == 1 || t == 3) ? 0 : (t == 0 ? 1 : 2); };
                if (nbytes <= vlen - 66 && nlen > 0 &&
                    (best.name.empty() || rank(ntype) < rank(best_name_type))) {
                    best.parent = parent;
                    best.name.assign(reinterpret_cast<const wchar_t*>(v + 66), nlen);
                    best.name_type = ntype;
                    best_name_type = ntype;
                    if (!have_data) best.size = fsize;
                }
            }
        } else if (a->type == kAttrData && unnamed) {
            if (a->non_resident) {
                if (a->length < sizeof(AttrHeader) + sizeof(AttrNonResident)) break;
                auto* nr = reinterpret_cast<const AttrNonResident*>(p + sizeof(AttrHeader));
                data_size = nr->real_size;
                have_data = true;
            } else {
                uint32_t vlen = 0;
                AttrValue(p, vlen);
                data_size = vlen;
                have_data = true;
            }
        }
        p += a->length;
        if (a->length == 0) break;
    }
    if (have_data) best.size = data_size;
    if (best.name.empty()) return true;
    return emit(std::move(best));
}

bool ParseMftRecord0Runs(BYTE* rec, uint32_t rec_size, uint32_t sector,
                         std::vector<Run>& runs) {
    if (rec_size < sizeof(FileRecord)) return false;
    auto* hdr = reinterpret_cast<FileRecord*>(rec);
    if (hdr->magic != 0x454C4946) return false;
    if (!ApplyUsa(rec, rec_size, sector)) return false;
    if (hdr->bytes_used > rec_size || hdr->attr_off < sizeof(FileRecord) ||
        hdr->attr_off > hdr->bytes_used)
        return false;
    const BYTE* p = rec + hdr->attr_off;
    const BYTE* end = rec + hdr->bytes_used;
    while (static_cast<size_t>(end - p) >= sizeof(AttrHeader)) {
        auto* a = reinterpret_cast<const AttrHeader*>(p);
        if (a->type == kAttrEnd || a->length < sizeof(AttrHeader)) break;
        if (a->length > static_cast<size_t>(end - p)) break;
        if (a->type == kAttrData && a->name_len == 0 && a->non_resident) {
            if (a->length < sizeof(AttrHeader) + sizeof(AttrNonResident)) return false;
            auto* nr = reinterpret_cast<const AttrNonResident*>(p + sizeof(AttrHeader));
            const uint32_t min_pairs_off = sizeof(AttrHeader) + sizeof(AttrNonResident);
            if (nr->pairs_off < min_pairs_off || nr->pairs_off >= a->length) return false;
            const BYTE* pairs = p + nr->pairs_off;
            return DecodeRuns(pairs, p + a->length, runs);
        }
        p += a->length;
    }
    return false;
}

} // namespace

bool EnumerateMft(HANDLE volume,
                  std::atomic<bool>* running,
                  const std::function<void(size_t)>& progress,
                  const std::function<bool(MftFile&&)>& emit) {
    NTFS_VOLUME_DATA_BUFFER vd{};
    DWORD br = 0;
    if (!DeviceIoControl(volume, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                         &vd, sizeof(vd), &br, nullptr))
        return false;
    const uint32_t rec_size = vd.BytesPerFileRecordSegment;
    const uint32_t cluster = vd.BytesPerCluster;
    const uint32_t sector = vd.BytesPerSector;
    if (rec_size < 512 || rec_size > 4096 || cluster == 0 || sector == 0) return false;
    if (vd.MftStartLcn.QuadPart < 0 || vd.MftValidDataLength.QuadPart < 0) return false;

    std::vector<BYTE> rec0(rec_size);
    uint64_t mft_off = 0;
    if (!CheckedMultiply(static_cast<uint64_t>(vd.MftStartLcn.QuadPart), cluster, mft_off))
        return false;
    if (!ReadAt(volume, mft_off, rec0.data(), rec_size)) return false;

    std::vector<Run> runs;
    if (!ParseMftRecord0Runs(rec0.data(), rec_size, sector, runs) || runs.empty()) {
        // Contiguous fallback: treat the start LCN as a long run covering ValidDataLength.
        Run r;
        r.lcn = static_cast<uint64_t>(vd.MftStartLcn.QuadPart);
        const uint64_t valid_bytes = static_cast<uint64_t>(vd.MftValidDataLength.QuadPart);
        r.clusters = valid_bytes / cluster + (valid_bytes % cluster != 0 ? 1 : 0);
        runs.push_back(r);
    }

    // Read the MFT in large sequential blocks. The previous implementation
    // moved the volume file pointer and issued one ReadFile per record,
    // turning a multi-million-record scan into millions of kernel calls.
    constexpr DWORD kReadChunkBytes = 8u * 1024u * 1024u;
    const DWORD chunk_bytes = kReadChunkBytes - (kReadChunkBytes % rec_size);
    std::vector<BYTE> chunk(chunk_bytes);
    size_t count = 0;
    uint64_t file_off = 0;
    for (const Run& run : runs) {
        if (running && !running->load()) return count > 0;
        if (run.sparse) {
            uint64_t run_bytes = 0;
            if (!CheckedMultiply(run.clusters, cluster, run_bytes) ||
                !CheckedAdd(file_off, run_bytes, file_off))
                return false;
            continue;
        }
        uint64_t disk = 0;
        uint64_t left = 0;
        if (!CheckedMultiply(run.lcn, cluster, disk) ||
            !CheckedMultiply(run.clusters, cluster, left))
            return false;
        while (left >= rec_size) {
            if (running && !running->load()) return count > 0;
            const uint64_t wanted = (std::min)(left, static_cast<uint64_t>(chunk_bytes));
            const DWORD bytes = static_cast<DWORD>(wanted - (wanted % rec_size));
            if (bytes < rec_size || !ReadAt(volume, disk, chunk.data(), bytes)) break;
            for (DWORD offset = 0; offset < bytes; offset += rec_size) {
                uint64_t record_off = 0;
                if (!CheckedAdd(file_off, offset, record_off)) return false;
                const uint64_t index = record_off / rec_size;
                if (!ParseRecord(chunk.data() + offset, rec_size, sector, index,
                                 [&](MftFile&& f) {
                    ++count;
                    if (progress && (count % 50000) == 0) progress(count);
                    return emit(std::move(f));
                })) return count > 0;
            }
            if (!CheckedAdd(disk, bytes, disk) ||
                !CheckedAdd(file_off, bytes, file_off))
                return false;
            left -= bytes;
        }
        if (left && !CheckedAdd(file_off, left, file_off)) return false;
    }
    return count > 0;
}

} // namespace pulse::index
