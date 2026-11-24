// Includes the codec implementation to verify persisted version boundaries directly.
#include "../ui/thumbnail_disk_cache.cpp"
#include <cstdio>
#include <fstream>

void pulse::ui::ThumbnailCache::SetGridStore(pulse::ui::ThumbnailStore*) {}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    using namespace pulse::ui;
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
        failures += !ok;
    };
    const std::wstring path = L"fixture.png";
    ThumbnailStoreKey key{path, 0, 4, 123, 456};
    StoredThumbnail thumb{};
    thumb.width = thumb.height = 4;
    thumb.pixels.resize(64, 255);
    std::vector<uint8_t> payload;
    check(Encode(thumb, true, payload), "encode current thumbnail");
    EntryHeader header{};
    header.magic = kMagic;
    header.version = 1;
    header.codec = kCodecPng;
    header.width = header.height = 4;
    header.pixels = 4;
    header.modified = 123;
    header.size = 456;
    header.path_chars = static_cast<uint32_t>(key.path.size());
    header.payload_bytes = static_cast<uint32_t>(payload.size());
    const size_t path_bytes = key.path.size() * sizeof(wchar_t);
    std::vector<uint8_t> entry(sizeof(header) + path_bytes + payload.size());
    std::memcpy(entry.data(), &header, sizeof(header));
    std::memcpy(entry.data() + sizeof(header), key.path.data(), path_bytes);
    std::memcpy(entry.data() + sizeof(header) + path_bytes, payload.data(), payload.size());
    StoredThumbnail decoded;
    check(!Decode(entry, key, decoded), "reject version 1 pixel cache");
    header.version = 2;
    std::memcpy(entry.data(), &header, sizeof(header));
    check(Decode(entry, key, decoded) && decoded.pixels == thumb.pixels, "read version 2 pixel cache");

    const std::filesystem::path root = argv[1];
    std::filesystem::create_directories(root);
    MicroThumbPack pack;
    check(pack.Open(root, false), "open new micro pack");
    MicroThumbData micro;
    check(EncodeMicroThumb(thumb.pixels.data(), 4, 4, 16, 4, 4, 4, micro), "encode micro");
    MicroThumbPack::PendingRecord record{};
    record.identity_hash = 7;
    record.mtime = 123;
    record.file_size = 456;
    record.thumb = micro;
    check(pack.AppendBatch({record}) == 1, "write version 2 micro cache");
    pack.Close();
    check(pack.Open(root, true) && pack.Read(7, 123, 456, micro), "reopen version 2 micro cache");
    pack.Close();
    const auto current = root / L"micro_v2.pack";
    {
        std::fstream file(current, std::ios::binary | std::ios::in | std::ios::out);
        uint32_t old_version = 1;
        file.seekp(4);
        file.write(reinterpret_cast<char*>(&old_version), sizeof(old_version));
    }
    check(pack.Open(root, true) && !pack.Read(7, 123, 456, micro), "reject version 1 micro header");
    pack.Close();
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
