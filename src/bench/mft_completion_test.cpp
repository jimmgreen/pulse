#include "../index/index_mft.h"
#include <vector>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>

int main() {
    using namespace pulse::index;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; };
    NTFS_VOLUME_DATA_BUFFER geometry{};
    geometry.BytesPerFileRecordSegment = 512; geometry.BytesPerCluster = 512; geometry.BytesPerSector = 512;
    geometry.MftValidDataLength.QuadPart = 8 * 1024 * 1024 + 512;
    auto record = [] (BYTE* data) {
        auto u16 = [&](size_t at, uint16_t value) { memcpy(data + at, &value, sizeof(value)); };
        auto u32 = [&](size_t at, uint32_t value) { memcpy(data + at, &value, sizeof(value)); };
        u32(0, 0x454c4946); u16(4, 42); u16(6, 1); u16(16, 1);
        u16(20, 48); u16(22, 1); u32(24, 160); u32(28, 512);
        u32(48, 0x30); u32(52, 96); u32(64, 68); u16(68, 24);
        data[72 + 64] = 1; data[72 + 65] = 1; u16(72 + 66, L'x'); u32(144, 0xffffffff);
    };
    for (int mode = 0; mode < 4; ++mode) {
        unsigned reads = 0, emitted = 0;
        std::atomic<bool> running{true};
        const bool complete = EnumerateMftRecords(geometry, [&](uint64_t, void* bytes, DWORD size) {
            ++reads;
            if (mode == 1 && reads == 3) return false;
            memset(bytes, 0, size);
            if (reads == 2) record(static_cast<BYTE*>(bytes));
            return true;
        }, &running, {}, [&](MftFile&& file) {
            ++emitted;
            if (file.name != L"x") return false;
            if (mode == 2) running = false;
            return mode != 3;
        });
        check(emitted == 1 && complete == (mode == 0), mode == 0 ? "complete source succeeds after all chunks" :
            mode == 1 ? "read failure after emitted records stays incomplete" :
            mode == 2 ? "cancellation after first record stays incomplete" : "consumer stop never reports complete volume");
    }
    // Fragmented large files keep $DATA in an extension record; FILE_NAME only
    // holds a creation-time size. Either record order must yield the real size.
    struct Writer {
        BYTE* d; size_t at = 48;
        void u16(size_t o, uint16_t v) { memcpy(d + o, &v, 2); }
        void u32(size_t o, uint32_t v) { memcpy(d + o, &v, 4); }
        void u64(size_t o, uint64_t v) { memcpy(d + o, &v, 8); }
        void header(uint64_t base) {
            u32(0, 0x454c4946); u16(4, 42); u16(6, 1); u16(16, 1); u16(20, 48); u16(22, 1);
            u32(28, 512); u64(32, base);
        }
        void resident(uint32_t type, const std::vector<BYTE>& value) {
            const uint32_t length = static_cast<uint32_t>((24 + value.size() + 7) & ~size_t(7));
            u32(at, type); u32(at + 4, length); u32(at + 16, static_cast<uint32_t>(value.size())); u16(at + 20, 24);
            memcpy(d + at + 24, value.data(), value.size()); at += length;
        }
        void name(const wchar_t* text, uint64_t stale_size, uint64_t parent = 5, BYTE type = 1) {
            const size_t n = wcslen(text);
            std::vector<BYTE> v(66 + n * 2);
            memcpy(v.data(), &parent, 8); memcpy(v.data() + 48, &stale_size, 8);
            v[64] = static_cast<BYTE>(n); v[65] = type; memcpy(v.data() + 66, text, n * 2);
            resident(0x30, v);
        }
        void data(uint64_t size) {
            u32(at, 0x80); u32(at + 4, 72); d[at + 8] = 1; u64(at + 24, 1000); u16(at + 32, 64);
            u64(at + 40, size + 4096); u64(at + 48, size); u64(at + 56, size); at += 72;
        }
        void finish() { u32(at, 0xffffffff); u32(24, static_cast<uint32_t>(at + 8)); }
    };
    constexpr uint64_t kReal = 287641720;
    for (int order = 0; order < 2; ++order) {
        geometry.MftValidDataLength.QuadPart = 4 * 512;
        std::map<std::wstring, uint64_t> sizes;
        unsigned emitted = 0, reads = 0;
        std::atomic<bool> running{true};
        const bool complete = EnumerateMftRecords(geometry, [&](uint64_t, void* bytes, DWORD size) {
            memset(bytes, 0, size);
            if (++reads != 2 || size < 4 * 512) return true;
            BYTE* base = static_cast<BYTE*>(bytes);
            const int big = order == 0 ? 0 : 1, extension = order == 0 ? 1 : 0;
            Writer b{base + big * 512}; b.header(0); b.resident(0x20, std::vector<BYTE>(8, 0)); b.name(L"big", 0); b.finish();
            Writer e{base + extension * 512}; e.header((1ull << 48) | static_cast<uint64_t>(big)); e.data(kReal); e.finish();
            Writer s{base + 2 * 512}; s.header(0); s.name(L"small", 0); s.resident(0x80, std::vector<BYTE>(7, 1)); s.finish();
            Writer o{base + 3 * 512}; o.header(0); o.resident(0x20, std::vector<BYTE>(8, 0)); o.name(L"orphan", 11); o.finish();
            return true;
        }, &running, {}, [&](MftFile&& file) { ++emitted; sizes[file.name] = file.size; return true; });
        check(complete && emitted == 3 && sizes[L"big"] == kReal,
              order == 0 ? "extension $DATA after its base record supplies the real file size" :
                           "extension $DATA before its base record supplies the real file size");
        check(sizes[L"small"] == 7 && sizes.count(L"orphan") && sizes[L"orphan"] == 11,
              "base $DATA and unmatched attribute lists keep their existing sizes");
    }
    // Hard links: every distinct Win32/POSIX FILE_NAME is a searchable name,
    // including names kept in extension records; DOS aliases are not.
    {
        geometry.MftValidDataLength.QuadPart = 4 * 512;
        std::map<std::wstring, MftFile> files;
        unsigned reads = 0;
        std::atomic<bool> running{true};
        const bool complete = EnumerateMftRecords(geometry, [&](uint64_t, void* bytes, DWORD size) {
            memset(bytes, 0, size);
            if (++reads != 2 || size < 4 * 512) return true;
            BYTE* base = static_cast<BYTE*>(bytes);
            Writer f{base}; f.header(0);
            f.name(L"a.txt", 0, 5, 1); f.name(L"b.txt", 0, 7, 0); f.name(L"A~1.TXT", 0, 5, 2); f.name(L"A.TXT", 0, 5, 0);
            f.resident(0x80, std::vector<BYTE>(3, 1)); f.finish();
            Writer d{base + 512}; d.header(0); d.u16(22, 3); d.name(L"dir", 0, 5, 1); d.name(L"dir2", 0, 9, 1); d.finish();
            Writer m{base + 2 * 512}; m.header(0); m.resident(0x20, std::vector<BYTE>(8, 0)); m.name(L"m1", 0, 5, 1);
            m.resident(0x80, std::vector<BYTE>(5, 1)); m.finish();
            Writer x{base + 3 * 512}; x.header((1ull << 48) | 2); x.name(L"m2", 0, 9, 1); x.name(L"M2~1", 0, 9, 2); x.finish();
            return true;
        }, &running, {}, [&](MftFile&& file) { std::wstring key = file.name; files[key] = std::move(file); return true; });
        const auto& a = files[L"a.txt"];
        check(complete && files.size() == 3 && a.links.size() == 1 && a.links[0].parent == 7 &&
              a.links[0].name == L"b.txt" && a.size == 3,
              "file record yields one primary name plus each distinct hard link");
        check(files[L"dir"].links.empty(), "directories never report hard links");
        const auto& m = files[L"m1"];
        check(m.links.size() == 1 && m.links[0].name == L"m2" && m.links[0].parent == 9 && m.size == 5,
              "hard link names in extension records are attached to their base file");
    }
    return failures ? 1 : 0;
}
