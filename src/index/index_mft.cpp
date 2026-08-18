// index_mft.cpp — Parse NTFS file records from $MFT data runs.
#include "index_mft.h"
#include <winioctl.h>
#include <algorithm>
#include <cstring>
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
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(off);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(h, buf, len, &got, nullptr) && got == len;
}

int64_t ReadLe(const BYTE* p, int n, bool sign) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    if (sign && n > 0 && (p[n - 1] & 0x80)) v |= ~0ull << (8 * n);
    return static_cast<int64_t>(v);
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
        if (p + len_n + off_n > end) return false;
        const uint64_t clusters = static_cast<uint64_t>(ReadLe(p, len_n, false));
        p += len_n;
        Run r;
        r.clusters = clusters;
        if (off_n == 0) {
            r.sparse = true;
        } else {
            lcn += ReadLe(p, off_n, true);
            r.lcn = static_cast<uint64_t>(lcn);
            p += off_n;
        }
        if (r.clusters) out.push_back(r);
    }
    return true;
}

bool ApplyUsa(BYTE* rec, uint32_t rec_size, uint32_t sector) {
    auto* h = reinterpret_cast<FileRecord*>(rec);
    if (h->usa_off < sizeof(FileRecord) || h->usa_count < 1) return false;
    const uint32_t usa_bytes = static_cast<uint32_t>(h->usa_count) * 2;
    if (static_cast<uint32_t>(h->usa_off) + usa_bytes > rec_size) return false;
    auto* usa = reinterpret_cast<uint16_t*>(rec + h->usa_off);
    for (uint16_t i = 1; i < h->usa_count; ++i) {
        const uint32_t off = static_cast<uint32_t>(i) * sector - 2;
        if (off + 2 > rec_size) return false;
        *reinterpret_cast<uint16_t*>(rec + off) = usa[i];
    }
    return true;
}

const BYTE* AttrValue(const BYTE* attr, uint32_t& len) {
    auto* h = reinterpret_cast<const AttrHeader*>(attr);
    auto* r = reinterpret_cast<const AttrResident*>(attr + sizeof(AttrHeader));
    len = r->value_len;
    if (r->value_off + len > h->length) { len = 0; return nullptr; }
    return attr + r->value_off;
}

bool ParseRecord(BYTE* rec, uint32_t rec_size, uint32_t sector, uint64_t index,
                 const std::function<bool(MftFile&&)>& emit) {
    auto* hdr = reinterpret_cast<FileRecord*>(rec);
    if (hdr->magic != 0x454C4946) return true; // 'FILE'
    if (!ApplyUsa(rec, rec_size, sector)) return true;
    if ((hdr->flags & 1) == 0) return true; // not in use
    if (hdr->base != 0) return true;        // extension record; base already holds names
    if (hdr->attr_off >= rec_size) return true;

    MftFile best;
    best.frn = (static_cast<uint64_t>(hdr->seq) << 48) | (index & 0xFFFFFFFFFFFFULL);
    best.is_dir = (hdr->flags & 2) != 0;
    uint8_t best_name_type = 0xFF;
    uint64_t data_size = 0;
    bool have_data = false;

    const BYTE* p = rec + hdr->attr_off;
    const BYTE* end = rec + (std::min)(hdr->bytes_used, rec_size);
    while (p + sizeof(AttrHeader) <= end) {
        auto* a = reinterpret_cast<const AttrHeader*>(p);
        if (a->type == kAttrEnd || a->length < sizeof(AttrHeader)) break;
        if (p + a->length > end) break;
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
                    if (66 + nbytes <= vlen && nlen > 0 &&
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
    auto* hdr = reinterpret_cast<FileRecord*>(rec);
    if (hdr->magic != 0x454C4946) return false;
    if (!ApplyUsa(rec, rec_size, sector)) return false;
    const BYTE* p = rec + hdr->attr_off;
    const BYTE* end = rec + (std::min)(hdr->bytes_used, rec_size);
    while (p + sizeof(AttrHeader) <= end) {
        auto* a = reinterpret_cast<const AttrHeader*>(p);
        if (a->type == kAttrEnd || a->length < sizeof(AttrHeader)) break;
        if (p + a->length > end) break;
        if (a->type == kAttrData && a->name_len == 0 && a->non_resident) {
            auto* nr = reinterpret_cast<const AttrNonResident*>(p + sizeof(AttrHeader));
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

    std::vector<BYTE> rec0(rec_size);
    const uint64_t mft_off = static_cast<uint64_t>(vd.MftStartLcn.QuadPart) * cluster;
    if (!ReadAt(volume, mft_off, rec0.data(), rec_size)) return false;

    std::vector<Run> runs;
    if (!ParseMftRecord0Runs(rec0.data(), rec_size, sector, runs) || runs.empty()) {
        // Contiguous fallback: treat the start LCN as a long run covering ValidDataLength.
        Run r;
        r.lcn = static_cast<uint64_t>(vd.MftStartLcn.QuadPart);
        r.clusters = static_cast<uint64_t>((vd.MftValidDataLength.QuadPart + cluster - 1) / cluster);
        runs.push_back(r);
    }

    std::vector<BYTE> rec(rec_size);
    size_t count = 0;
    uint64_t file_off = 0;
    for (const Run& run : runs) {
        if (running && !running->load()) return count > 0;
        if (run.sparse) {
            file_off += run.clusters * cluster;
            continue;
        }
        uint64_t disk = run.lcn * cluster;
        uint64_t left = run.clusters * cluster;
        while (left >= rec_size) {
            if (running && !running->load()) return count > 0;
            if (!ReadAt(volume, disk, rec.data(), rec_size)) break;
            const uint64_t index = file_off / rec_size;
            if (!ParseRecord(rec.data(), rec_size, sector, index, [&](MftFile&& f) {
                ++count;
                if (progress && (count % 50000) == 0) progress(count);
                return emit(std::move(f));
            })) return count > 0;
            disk += rec_size;
            file_off += rec_size;
            left -= rec_size;
        }
        if (left) file_off += left; // partial cluster padding
    }
    return count > 0;
}

} // namespace pulse::index
