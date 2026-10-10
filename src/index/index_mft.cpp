// index_mft.cpp — Parse NTFS file records from $MFT data runs.
#include "index_mft.h"
#include <winioctl.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::index {
namespace {

constexpr uint32_t kAttrStdInfo = 0x10;
constexpr uint32_t kAttrList = 0x20;
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

// Large or heavily fragmented files keep their unnamed $DATA in an extension
// record listed by $ATTRIBUTE_LIST. Their FILE_NAME size is only a creation-
// time copy (often 0), so the base record is held until the extension's size
// is known. Files with many hard links also keep FILE_NAMEs in extension
// records, so every base record with an attribute list is held until the end.
struct OwnedName {
    uint64_t parent = 0;
    std::wstring name;
    uint64_t size = 0;
    uint8_t type = 0;
};
struct HeldFile {
    MftFile file;
    std::vector<OwnedName> names;
    bool have_data = false;
};
struct ExtensionSizes {
    std::unordered_map<uint64_t, uint64_t> data; // base FRN -> logical size
    std::unordered_map<uint64_t, std::vector<OwnedName>> names; // base FRN -> FILE_NAMEs
    std::vector<HeldFile> held;
};

struct NameView {
    uint64_t parent = 0;
    std::wstring_view name;
    uint64_t size = 0;
    uint8_t type = 0;
};

int NameRank(uint8_t type) { return (type == 1 || type == 3) ? 0 : (type == 0 ? 1 : 2); }

bool SameLink(uint64_t parent_a, std::wstring_view a, uint64_t parent_b, std::wstring_view b) {
    return parent_a == parent_b && a.size() == b.size() &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                             static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// The preferred Win32 name becomes the primary entry. Every other Win32 or
// POSIX FILE_NAME of a file is a separate hard link; DOS (type 2) names are
// 8.3 aliases of a Win32 name in the same directory.
void SelectNames(MftFile& file, const NameView* names, size_t count, bool have_data) {
    size_t best = count;
    for (size_t i = 0; i < count; ++i)
        if (best == count || NameRank(names[i].type) < NameRank(names[best].type)) best = i;
    if (best == count) return;
    file.parent = names[best].parent;
    file.name.assign(names[best].name);
    file.name_type = names[best].type;
    if (!have_data) file.size = names[best].size;
    file.links.clear();
    if (file.is_dir) return;
    for (size_t i = 0; i < count; ++i) {
        if (i == best || names[i].type == 2) continue;
        if (SameLink(names[i].parent, names[i].name, file.parent, file.name)) continue;
        const bool duplicate = std::any_of(file.links.begin(), file.links.end(), [&](const MftLink& link) {
            return SameLink(names[i].parent, names[i].name, link.parent, link.name);
        });
        if (!duplicate) file.links.push_back(MftLink{names[i].parent, std::wstring(names[i].name)});
    }
}

// Reads one resident FILE_NAME value. `name` points into the record buffer.
bool ReadFileName(const BYTE* attr, NameView& out) {
    uint32_t vlen = 0;
    const BYTE* v = AttrValue(attr, vlen);
    if (!v || vlen < 66) return false;
    std::memcpy(&out.parent, v, 8);
    std::memcpy(&out.size, v + 48, 8);
    const BYTE nlen = v[64];
    out.type = v[65];
    const uint32_t nbytes = static_cast<uint32_t>(nlen) * 2;
    if (nlen == 0 || nbytes > vlen - 66) return false;
    out.name = std::wstring_view(reinterpret_cast<const wchar_t*>(v + 66), nlen);
    return true;
}

void CollectExtensionNames(const BYTE* rec, const FileRecord* hdr, ExtensionSizes& extensions) {
    const BYTE* p = rec + hdr->attr_off;
    const BYTE* end = rec + hdr->bytes_used;
    while (static_cast<size_t>(end - p) >= sizeof(AttrHeader)) {
        auto* a = reinterpret_cast<const AttrHeader*>(p);
        if (a->type == kAttrEnd || a->length < sizeof(AttrHeader) || a->length > static_cast<size_t>(end - p)) break;
        NameView view;
        if (a->type == kAttrFileName && !a->non_resident && ReadFileName(p, view))
            extensions.names[hdr->base].push_back(OwnedName{view.parent, std::wstring(view.name), view.size, view.type});
        p += a->length;
    }
}

bool UnnamedDataSize(const BYTE* rec, const FileRecord* hdr, uint64_t& size) {
    const BYTE* p = rec + hdr->attr_off;
    const BYTE* end = rec + hdr->bytes_used;
    while (static_cast<size_t>(end - p) >= sizeof(AttrHeader)) {
        auto* a = reinterpret_cast<const AttrHeader*>(p);
        if (a->type == kAttrEnd || a->length < sizeof(AttrHeader) || a->length > static_cast<size_t>(end - p)) break;
        if (a->type == kAttrData && a->name_len == 0) {
            if (a->non_resident) {
                if (a->length < sizeof(AttrHeader) + sizeof(AttrNonResident)) return false;
                auto* nr = reinterpret_cast<const AttrNonResident*>(p + sizeof(AttrHeader));
                // Sizes are only valid in the segment that starts at VCN 0.
                if (nr->start_vcn != 0) return false;
                size = nr->real_size;
                return true;
            }
            uint32_t vlen = 0;
            if (!AttrValue(p, vlen)) return false;
            size = vlen;
            return true;
        }
        p += a->length;
    }
    return false;
}

bool ParseRecord(BYTE* rec, uint32_t rec_size, uint32_t sector, uint64_t index,
                 const std::function<bool(MftFile&&)>& emit, ExtensionSizes* extensions = nullptr) {
    if (rec_size < sizeof(FileRecord)) return true;
    auto* hdr = reinterpret_cast<FileRecord*>(rec);
    if (hdr->magic != 0x454C4946) return true; // 'FILE'
    if (!ApplyUsa(rec, rec_size, sector)) return true;
    if ((hdr->flags & 1) == 0) return true; // not in use
    if (hdr->bytes_used > rec_size || hdr->attr_off < sizeof(FileRecord) ||
        hdr->attr_off > hdr->bytes_used)
        return true;
    if (hdr->base != 0) {                   // extension record of a held base record
        uint64_t size = 0;
        if (extensions && UnnamedDataSize(rec, hdr, size)) extensions->data[hdr->base] = size;
        if (extensions) CollectExtensionNames(rec, hdr, *extensions);
        return true;
    }

    MftFile best;
    best.frn = (static_cast<uint64_t>(hdr->seq) << 48) | (index & 0xFFFFFFFFFFFFULL);
    best.is_dir = (hdr->flags & 2) != 0;
    uint64_t data_size = 0;
    bool have_data = false, have_list = false;
    thread_local std::vector<NameView> names;
    names.clear();

    const BYTE* p = rec + hdr->attr_off;
    const BYTE* end = rec + hdr->bytes_used;
    while (static_cast<size_t>(end - p) >= sizeof(AttrHeader)) {
        auto* a = reinterpret_cast<const AttrHeader*>(p);
        if (a->type == kAttrEnd || a->length < sizeof(AttrHeader)) break;
        if (a->length > static_cast<size_t>(end - p)) break;
        const bool unnamed = a->name_len == 0;
        if (a->type == kAttrList) have_list = true;

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
            NameView view;
            if (ReadFileName(p, view)) names.push_back(view);
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
    if (extensions && have_list && !best.is_dir) {
        HeldFile held;
        held.have_data = have_data;
        held.names.reserve(names.size());
        for (const NameView& name : names)
            held.names.push_back(OwnedName{name.parent, std::wstring(name.name), name.size, name.type});
        held.file = std::move(best);
        extensions->held.push_back(std::move(held));
        return true;
    }
    SelectNames(best, names.data(), names.size(), have_data);
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

MftReadResult EnumerateMft(HANDLE volume,
                  std::atomic<bool>* running,
                  const std::function<void(size_t)>& progress,
                  const std::function<bool(MftFile&&)>& emit) {
    NTFS_VOLUME_DATA_BUFFER vd{};
    DWORD br = 0;
    if (!DeviceIoControl(volume, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                         &vd, sizeof(vd), &br, nullptr))
        return MftReadResult::Failed;
    return EnumerateMftRecordsResult(vd, [volume](uint64_t offset, void* data, DWORD bytes) {
        return ReadAt(volume, offset, data, bytes);
    }, running, progress, emit);
}

MftReadResult EnumerateMftRecordsResult(const NTFS_VOLUME_DATA_BUFFER& vd,
    const std::function<bool(uint64_t, void*, DWORD)>& read,
    std::atomic<bool>* running, const std::function<void(size_t)>& progress,
    const std::function<bool(MftFile&&)>& emit) {
    const uint32_t rec_size = vd.BytesPerFileRecordSegment;
    const uint32_t cluster = vd.BytesPerCluster;
    const uint32_t sector = vd.BytesPerSector;
    if (rec_size < 512 || rec_size > 4096 || cluster == 0 || sector == 0) return MftReadResult::Failed;
    if (vd.MftStartLcn.QuadPart < 0 || vd.MftValidDataLength.QuadPart < 0) return MftReadResult::Failed;

    std::vector<BYTE> rec0(rec_size);
    uint64_t mft_off = 0;
    if (!CheckedMultiply(static_cast<uint64_t>(vd.MftStartLcn.QuadPart), cluster, mft_off))
        return MftReadResult::Failed;
    if (!read(mft_off, rec0.data(), rec_size)) return MftReadResult::Failed;

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
    ExtensionSizes extensions;
    const std::function<bool(MftFile&&)> deliver = [&](MftFile&& f) {
        ++count;
        if (progress && (count % 50000) == 0) progress(count);
        return emit(std::move(f));
    };
    for (const Run& run : runs) {
        if (running && !running->load()) return MftReadResult::Stopped;
        if (run.sparse) {
            uint64_t run_bytes = 0;
            if (!CheckedMultiply(run.clusters, cluster, run_bytes) ||
                !CheckedAdd(file_off, run_bytes, file_off))
                return MftReadResult::Failed;
            continue;
        }
        uint64_t disk = 0;
        uint64_t left = 0;
        if (!CheckedMultiply(run.lcn, cluster, disk) ||
            !CheckedMultiply(run.clusters, cluster, left))
            return MftReadResult::Failed;
        while (left >= rec_size) {
            if (running && !running->load()) return MftReadResult::Stopped;
            const uint64_t wanted = (std::min)(left, static_cast<uint64_t>(chunk_bytes));
            const DWORD bytes = static_cast<DWORD>(wanted - (wanted % rec_size));
            if (bytes < rec_size || !read(disk, chunk.data(), bytes)) return MftReadResult::Failed;
            for (DWORD offset = 0; offset < bytes; offset += rec_size) {
                if (running && !running->load()) return MftReadResult::Stopped;
                uint64_t record_off = 0;
                if (!CheckedAdd(file_off, offset, record_off)) return MftReadResult::Failed;
                const uint64_t index = record_off / rec_size;
                if (!ParseRecord(chunk.data() + offset, rec_size, sector, index, deliver, &extensions))
                    return MftReadResult::Stopped;
            }
            if (!CheckedAdd(disk, bytes, disk) ||
                !CheckedAdd(file_off, bytes, file_off))
                return MftReadResult::Failed;
            left -= bytes;
        }
        if (left && !CheckedAdd(file_off, left, file_off)) return MftReadResult::Failed;
    }
    // Extension records may precede or follow their base record anywhere in $MFT.
    std::vector<NameView> views;
    for (auto& held : extensions.held) {
        if (running && !running->load()) return MftReadResult::Stopped;
        MftFile& f = held.file;
        if (!held.have_data) {
            if (const auto found = extensions.data.find(f.frn); found != extensions.data.end()) {
                f.size = found->second;
                held.have_data = true;
            }
        }
        if (const auto found = extensions.names.find(f.frn); found != extensions.names.end())
            for (auto& name : found->second) held.names.push_back(std::move(name));
        views.clear();
        for (const OwnedName& name : held.names)
            views.push_back(NameView{name.parent, name.name, name.size, name.type});
        SelectNames(f, views.data(), views.size(), held.have_data);
        if (f.name.empty()) continue;
        if (!deliver(std::move(f))) return MftReadResult::Stopped;
    }
    if (running && !running->load()) return MftReadResult::Stopped;
    return count > 0 ? MftReadResult::Complete : MftReadResult::Failed;
}

} // namespace pulse::index

namespace pulse::index {
bool EnumerateMftRecords(const NTFS_VOLUME_DATA_BUFFER& vd,
    const std::function<bool(uint64_t, void*, DWORD)>& read, std::atomic<bool>* running,
    const std::function<void(size_t)>& progress, const std::function<bool(MftFile&&)>& emit) {
    return EnumerateMftRecordsResult(vd, read, running, progress, emit) == MftReadResult::Complete;
}
}
