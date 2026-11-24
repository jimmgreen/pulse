#include "../index/content_scope.h"
#include "../index/index_config.h"
#include "../index/index_volume_walk.h"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace pulse::index;

namespace {

int g_failures = 0;

void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!ok) ++g_failures;
}

bool WriteEmptyFile(const std::wstring& path) {
    const std::wstring long_path = ToWin32LongPath(path);
    HANDLE h = CreateFileW(long_path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool MakeDirectory(const std::wstring& path) {
    const std::wstring long_path = ToWin32LongPath(path);
    return CreateDirectoryW(long_path.c_str(), nullptr) != FALSE;
}

bool MakeJunction(const std::wstring& junction_path, const std::wstring& target_path) {
    const std::wstring long_junction = ToWin32LongPath(junction_path);
    if (!CreateDirectoryW(long_junction.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(long_junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    struct MountPointBuffer {
        DWORD ReparseTag;
        WORD ReparseDataLength;
        WORD Reserved;
        WORD SubstituteNameOffset;
        WORD SubstituteNameLength;
        WORD PrintNameOffset;
        WORD PrintNameLength;
        wchar_t PathBuffer[2048];
    } data{};

    std::wstring abs_target = std::filesystem::absolute(target_path).wstring();
    while (abs_target.size() > 3 && abs_target.back() == L'\\') abs_target.pop_back();
    const std::wstring nt_target = L"\\??\\" + abs_target;
    if (nt_target.size() + 2 > std::size(data.PathBuffer)) {
        CloseHandle(h);
        return false;
    }

    data.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    data.SubstituteNameOffset = 0;
    data.SubstituteNameLength = static_cast<WORD>(nt_target.size() * sizeof(wchar_t));
    data.PrintNameOffset = static_cast<WORD>(data.SubstituteNameLength + sizeof(wchar_t));
    data.PrintNameLength = 0;
    data.ReparseDataLength = static_cast<WORD>(8 + data.PrintNameOffset + sizeof(wchar_t));
    std::memcpy(data.PathBuffer, nt_target.c_str(), data.SubstituteNameLength + sizeof(wchar_t));

    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(
        h, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(data.ReparseDataLength) + 8, nullptr, 0, &bytes, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

void RemoveReparseAndDir(const std::wstring& path) {
    const std::wstring long_path = ToWin32LongPath(path);
    RemoveDirectoryW(long_path.c_str());
}

} // namespace

int main() {
    // 1. SelectVolumeBackend
    Check(SelectVolumeBackend(L"NTFS") == VolumeBackend::NtfsMftUsn,
          "SelectVolumeBackend NTFS -> NtfsMftUsn");
    Check(SelectVolumeBackend(L"ntfs") == VolumeBackend::NtfsMftUsn,
          "SelectVolumeBackend ntfs -> NtfsMftUsn");
    Check(SelectVolumeBackend(L"FAT32") == VolumeBackend::DirectoryWalk,
          "SelectVolumeBackend FAT32 -> DirectoryWalk");
    Check(SelectVolumeBackend(L"fat32") == VolumeBackend::DirectoryWalk,
          "SelectVolumeBackend fat32 -> DirectoryWalk");
    Check(SelectVolumeBackend(L"exFAT") == VolumeBackend::DirectoryWalk,
          "SelectVolumeBackend exFAT -> DirectoryWalk");
    Check(SelectVolumeBackend(L"EXFAT") == VolumeBackend::DirectoryWalk,
          "SelectVolumeBackend EXFAT -> DirectoryWalk");
    Check(SelectVolumeBackend(L"FAT") == VolumeBackend::DirectoryWalk,
          "SelectVolumeBackend FAT -> DirectoryWalk");
    Check(SelectVolumeBackend(L"FAT16") == VolumeBackend::DirectoryWalk,
          "SelectVolumeBackend FAT16 -> DirectoryWalk");
    Check(SelectVolumeBackend(L"FAT12") == VolumeBackend::DirectoryWalk,
          "SelectVolumeBackend FAT12 -> DirectoryWalk");
    Check(SelectVolumeBackend(L"ReFS") == VolumeBackend::Unsupported,
          "SelectVolumeBackend ReFS -> Unsupported");
    Check(SelectVolumeBackend(L"UDF") == VolumeBackend::Unsupported,
          "SelectVolumeBackend UDF -> Unsupported");
    Check(SelectVolumeBackend(L"CDFS") == VolumeBackend::Unsupported,
          "SelectVolumeBackend CDFS -> Unsupported");
    Check(SelectVolumeBackend(L"") == VolumeBackend::Unsupported,
          "SelectVolumeBackend empty -> Unsupported");

    // 2. FsTagFromName and FsNameFromTag round-trips
    Check(FsTagFromName(L"NTFS") == VolumeFsTag::UnknownOrNtfs, "FsTagFromName NTFS -> UnknownOrNtfs");
    Check(FsTagFromName(L"FAT32") == VolumeFsTag::Fat32, "FsTagFromName FAT32 -> Fat32");
    Check(FsTagFromName(L"exFAT") == VolumeFsTag::ExFat, "FsTagFromName exFAT -> ExFat");
    Check(FsTagFromName(L"FAT") == VolumeFsTag::Fat, "FsTagFromName FAT -> Fat");
    Check(FsTagFromName(L"FAT16") == VolumeFsTag::Fat, "FsTagFromName FAT16 -> Fat");
    Check(FsTagFromName(L"FAT12") == VolumeFsTag::Fat, "FsTagFromName FAT12 -> Fat");
    Check(FsTagFromName(L"ReFS") == VolumeFsTag::UnknownOrNtfs, "FsTagFromName ReFS -> UnknownOrNtfs");

    Check(FsNameFromTag(VolumeFsTag::Fat32, VolumeBackend::DirectoryWalk) == L"FAT32",
          "FsNameFromTag Fat32 -> FAT32");
    Check(FsNameFromTag(VolumeFsTag::ExFat, VolumeBackend::DirectoryWalk) == L"exFAT",
          "FsNameFromTag ExFat -> exFAT");
    Check(FsNameFromTag(VolumeFsTag::Fat, VolumeBackend::DirectoryWalk) == L"FAT",
          "FsNameFromTag Fat -> FAT");
    Check(FsNameFromTag(VolumeFsTag::UnknownOrNtfs, VolumeBackend::NtfsMftUsn) == L"NTFS",
          "FsNameFromTag UnknownOrNtfs + NtfsMftUsn -> NTFS");
    Check(FsNameFromTag(VolumeFsTag::UnknownOrNtfs, VolumeBackend::DirectoryWalk) == L"FAT/exFAT",
          "FsNameFromTag UnknownOrNtfs + DirectoryWalk -> FAT/exFAT");

    // 3. PackDiskVolKind and UnpackDiskVolMeta round-trips + legacy fallback
    bool meta_round_trip_ok = true;
    for (VolumeKind k : {VolumeKind::Fixed, VolumeKind::Removable, VolumeKind::Other}) {
        VolumeKind out_k = VolumeKind::Other;
        VolumeBackend out_b = VolumeBackend::Unsupported;
        VolumeFsTag out_t = VolumeFsTag::UnknownOrNtfs;
        uint32_t out_s = 123u;

        UnpackDiskVolMeta(
            PackDiskVolKind(k, VolumeBackend::Unsupported, VolumeFsTag::UnknownOrNtfs),
            0, 0, out_k, out_b, out_t, out_s);
        meta_round_trip_ok &=
            (out_k == k && out_b == VolumeBackend::Unsupported &&
             out_t == VolumeFsTag::UnknownOrNtfs && out_s == 0u);

        UnpackDiskVolMeta(
            PackDiskVolKind(k, VolumeBackend::NtfsMftUsn, VolumeFsTag::UnknownOrNtfs),
            99, 500, out_k, out_b, out_t, out_s);
        meta_round_trip_ok &=
            (out_k == k && out_b == VolumeBackend::NtfsMftUsn &&
             out_t == VolumeFsTag::UnknownOrNtfs && out_s == 0u);

        for (VolumeFsTag t : {VolumeFsTag::Fat32, VolumeFsTag::ExFat, VolumeFsTag::Fat}) {
            UnpackDiskVolMeta(
                PackDiskVolKind(k, VolumeBackend::DirectoryWalk, t),
                0, static_cast<int64_t>(0xDEADBEEFull), out_k, out_b, out_t, out_s);
            meta_round_trip_ok &=
                (out_k == k && out_b == VolumeBackend::DirectoryWalk &&
                 out_t == t && out_s == 0xDEADBEEFu);
        }
    }
    Check(meta_round_trip_ok, "PackDiskVolKind/UnpackDiskVolMeta round-trip all kind x backend x tag");

    {
        const uint16_t legacy_raw = static_cast<uint16_t>(VolumeKind::Fixed);
        VolumeKind out_k = VolumeKind::Other;
        VolumeBackend out_b = VolumeBackend::Unsupported;
        VolumeFsTag out_t = VolumeFsTag::UnknownOrNtfs;
        uint32_t out_s = 0;

        UnpackDiskVolMeta(legacy_raw, 42, 0, out_k, out_b, out_t, out_s);
        Check(out_k == VolumeKind::Fixed && out_b == VolumeBackend::NtfsMftUsn && out_s == 0,
              "legacy raw_backend==0 with journal_id!=0 -> NtfsMftUsn");

        UnpackDiskVolMeta(legacy_raw, 0, 100, out_k, out_b, out_t, out_s);
        Check(out_k == VolumeKind::Fixed && out_b == VolumeBackend::NtfsMftUsn && out_s == 0,
              "legacy raw_backend==0 with next_usn!=0 -> NtfsMftUsn");

        UnpackDiskVolMeta(legacy_raw, 0, 0, out_k, out_b, out_t, out_s);
        Check(out_k == VolumeKind::Fixed && out_b == VolumeBackend::Unsupported && out_s == 0,
              "legacy raw_backend==0 with journal_id==0 && next_usn==0 -> Unsupported");
    }

    // 4. VolumeIdentityMatches
    const wchar_t* const kVolGuid = L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\";
    const wchar_t* const kVolGuidNoSlash = L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}";
    Check(VolumeIdentityMatches(kVolGuid, L"FAT32", 0x11112222u,
                                kVolGuid, VolumeBackend::DirectoryWalk, VolumeFsTag::Fat32, 0x11112222u),
          "VolumeIdentityMatches exact match");
    Check(VolumeIdentityMatches(kVolGuid, L"FAT32", 0x11112222u,
                                kVolGuidNoSlash, VolumeBackend::DirectoryWalk, VolumeFsTag::Fat32, 0x11112222u),
          "VolumeIdentityMatches normalizes trailing backslash");
    Check(!VolumeIdentityMatches(kVolGuid, L"exFAT", 0x11112222u,
                                 kVolGuid, VolumeBackend::DirectoryWalk, VolumeFsTag::Fat32, 0x11112222u),
          "VolumeIdentityMatches rejects FAT32 -> exFAT reformat");
    Check(!VolumeIdentityMatches(kVolGuid, L"NTFS", 0x11112222u,
                                 kVolGuid, VolumeBackend::DirectoryWalk, VolumeFsTag::Fat32, 0x11112222u),
          "VolumeIdentityMatches rejects FAT32 -> NTFS reformat");
    Check(!VolumeIdentityMatches(kVolGuid, L"FAT32", 0x33334444u,
                                 kVolGuid, VolumeBackend::DirectoryWalk, VolumeFsTag::Fat32, 0x11112222u),
          "VolumeIdentityMatches rejects serial change");
    Check(VolumeIdentityMatches(kVolGuid, L"FAT32", 0u,
                                kVolGuid, VolumeBackend::DirectoryWalk, VolumeFsTag::Fat32, 0x11112222u),
          "VolumeIdentityMatches treats drive_serial == 0 as wildcard");
    Check(VolumeIdentityMatches(kVolGuid, L"FAT32", 0x33334444u,
                                kVolGuid, VolumeBackend::DirectoryWalk, VolumeFsTag::Fat32, 0u),
          "VolumeIdentityMatches treats state_serial == 0 as wildcard");

    // 5. WalkDirectoryTree isolated fixture tests
    std::error_code ec;
    const auto base = std::filesystem::absolute(
        std::filesystem::path(L"bench_data") /
        (L"fat_unit_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(base, ec);

    // 5a. Empty directory
    {
        const auto empty_dir = base / L"empty";
        std::filesystem::create_directories(empty_dir, ec);
        int32_t emitted = 0;
        WalkCallbacks cb;
        cb.emit_node = [&](int32_t, std::wstring_view, bool, bool, uint64_t, uint64_t) -> int32_t {
            ++emitted;
            return emitted;
        };
        WalkStats stats;
        const bool ok = WalkDirectoryTree(empty_dir.wstring(), 1, false, 1000, cb, &stats);
        Check(ok && stats.added == 0 && emitted == 0 && stats.root_error == ERROR_SUCCESS,
              "WalkDirectoryTree empty directory succeeds with 0 nodes");
    }

    // 5b. Deep path (> 360 chars > MAX_PATH)
    {
        const auto deep_root = base / L"deep_root";
        std::filesystem::create_directories(deep_root, ec);
        std::vector<std::wstring> deep_dirs;
        std::wstring cur = deep_root.wstring();
        bool created = true;
        for (int i = 0; i < 45; ++i) {
            cur += L"\\d12345678";
            if (!MakeDirectory(cur)) {
                created = false;
                break;
            }
            deep_dirs.push_back(cur);
        }
        const std::wstring deep_file = cur + L"\\leaf_deep.txt";
        if (created) created = WriteEmptyFile(deep_file);
        Check(created && deep_file.size() > 360,
              "created 45-level deep fixture path exceeding MAX_PATH");

        struct NodeRec {
            int32_t parent;
            std::wstring name;
            bool is_dir;
        };
        std::vector<NodeRec> nodes(1); // index 0 unused; root_id == 1
        nodes.push_back({-1, L"deep_root", true});
        WalkCallbacks cb;
        cb.emit_node = [&](int32_t parent_id, std::wstring_view name, bool is_dir,
                           bool, uint64_t, uint64_t) -> int32_t {
            const int32_t id = static_cast<int32_t>(nodes.size());
            nodes.push_back({parent_id, std::wstring(name), is_dir});
            return id;
        };
        WalkStats stats;
        const bool ok = WalkDirectoryTree(deep_root.wstring(), 1, false, 1000, cb, &stats);
        const bool found_leaf = !nodes.empty() && nodes.back().name == L"leaf_deep.txt" && !nodes.back().is_dir;
        Check(ok && stats.added == 46 && found_leaf,
              "WalkDirectoryTree emits all 45 nested directories and leaf file beyond MAX_PATH");

        DeleteFileW(ToWin32LongPath(deep_file).c_str());
        for (auto it = deep_dirs.rbegin(); it != deep_dirs.rend(); ++it) {
            RemoveDirectoryW(ToWin32LongPath(*it).c_str());
        }
    }

    // 5c. Reparse-point loop protection
    {
        const auto loop_root = base / L"loop_root";
        const auto sub_dir = loop_root / L"normal_sub";
        std::filesystem::create_directories(sub_dir, ec);
        WriteEmptyFile((sub_dir / L"real.txt").wstring());
        const std::wstring junction_dir = (sub_dir / L"self_junction").wstring();
        const bool junc_ok = MakeJunction(junction_dir, loop_root.wstring());
        Check(junc_ok, "created self-pointing NTFS directory junction");

        std::vector<std::wstring> names;
        int32_t next_id = 1;
        WalkCallbacks cb;
        cb.emit_node = [&](int32_t, std::wstring_view name, bool, bool, uint64_t, uint64_t) -> int32_t {
            names.emplace_back(name);
            return ++next_id;
        };
        WalkStats stats;
        const bool ok = WalkDirectoryTree(loop_root.wstring(), 1, false, 1000, cb, &stats);
        const bool has_junction = std::find(names.begin(), names.end(), L"self_junction") != names.end();
        const bool has_real = std::find(names.begin(), names.end(), L"real.txt") != names.end();
        Check(ok && stats.added == 3 && has_junction && has_real,
              "WalkDirectoryTree emits junction once and does not recurse infinitely");

        RemoveReparseAndDir(junction_dir);
    }

    // 5d. should_skip_name (parent_is_root), is_excluded_path, may_contain_exclusion hidden propagation
    {
        const auto vis_root = base / L"vis_root";
        std::filesystem::create_directories(vis_root / L"System Volume Information", ec);
        std::filesystem::create_directories(vis_root / L"nested" / L"System Volume Information", ec);
        std::filesystem::create_directories(vis_root / L"folder" / L"excluded_sub", ec);
        WriteEmptyFile((vis_root / L"System Volume Information" / L"root_secret.dat").wstring());
        WriteEmptyFile((vis_root / L"nested" / L"System Volume Information" / L"kept_sub.dat").wstring());
        WriteEmptyFile((vis_root / L"folder" / L"excluded_sub" / L"child.txt").wstring());
        WriteEmptyFile((vis_root / L"folder" / L"visible.txt").wstring());

        const std::wstring excluded_dir = (vis_root / L"folder" / L"excluded_sub").wstring();
        const std::wstring parent_folder = (vis_root / L"folder").wstring();
        auto eq_path = [](std::wstring_view a, std::wstring_view b) {
            return a.size() == b.size() &&
                CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                     b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        };

        struct Emitted {
            int32_t id;
            int32_t parent;
            std::wstring name;
            bool hidden;
        };
        std::vector<Emitted> seen;
        int32_t next_id = 1;
        WalkCallbacks cb;
        cb.should_skip_name = [](bool parent_is_root, std::wstring_view name) {
            return parent_is_root &&
                CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                                     L"System Volume Information", 25, TRUE) == CSTR_EQUAL;
        };
        cb.is_excluded_path = [&](std::wstring_view p) { return eq_path(p, excluded_dir); };
        cb.may_contain_exclusion = [&](std::wstring_view dir) {
            return eq_path(dir, vis_root.wstring()) || eq_path(dir, parent_folder) || eq_path(dir, excluded_dir);
        };
        cb.emit_node = [&](int32_t parent_id, std::wstring_view name, bool, bool hidden, uint64_t, uint64_t) -> int32_t {
            const int32_t id = ++next_id;
            seen.push_back({id, parent_id, std::wstring(name), hidden});
            return id;
        };
        WalkStats stats;
        const bool ok = WalkDirectoryTree(vis_root.wstring(), 1, true, 1000, cb, &stats);
        auto find_name = [&](const wchar_t* n) -> const Emitted* {
            for (const auto& item : seen) if (item.name == n) return &item;
            return nullptr;
        };
        const bool root_svi_skipped = find_name(L"root_secret.dat") == nullptr;
        const bool nested_svi_kept = find_name(L"kept_sub.dat") != nullptr;
        const auto* excl = find_name(L"excluded_sub");
        const auto* excl_child = find_name(L"child.txt");
        const auto* vis_file = find_name(L"visible.txt");
        Check(ok && root_svi_skipped && nested_svi_kept &&
              excl && excl->hidden && excl_child == nullptr &&
              vis_file && !vis_file->hidden,
              "WalkDirectoryTree parent_is_root skip and excluded subtree pruning");
    }

    // 5e. Mid-walk cancellation and max_items cap
    {
        const auto cancel_root = base / L"cancel_root";
        std::filesystem::create_directories(cancel_root / L"dir_a", ec);
        std::filesystem::create_directories(cancel_root / L"dir_b", ec);
        WriteEmptyFile((cancel_root / L"dir_a" / L"f1.txt").wstring());
        WriteEmptyFile((cancel_root / L"dir_a" / L"f2.txt").wstring());
        WriteEmptyFile((cancel_root / L"dir_b" / L"f3.txt").wstring());
        WriteEmptyFile((cancel_root / L"dir_b" / L"f4.txt").wstring());

        int32_t count = 0;
        WalkCallbacks cb_cancel;
        cb_cancel.should_cancel = [&]() { return count >= 2; };
        cb_cancel.emit_node = [&](int32_t, std::wstring_view, bool, bool, uint64_t, uint64_t) -> int32_t {
            ++count;
            return count + 1;
        };
        WalkStats cancel_stats;
        const bool cancel_ok = WalkDirectoryTree(cancel_root.wstring(), 1, false, 1000, cb_cancel, &cancel_stats);
        Check(!cancel_ok && cancel_stats.cancelled && cancel_stats.added == 2,
              "WalkDirectoryTree mid-walk cancellation stops and sets stats.cancelled");

        int32_t cap_id = 1;
        WalkCallbacks cb_cap;
        cb_cap.emit_node = [&](int32_t, std::wstring_view, bool, bool, uint64_t, uint64_t) -> int32_t {
            return ++cap_id;
        };
        WalkStats cap_stats;
        const bool cap_ok = WalkDirectoryTree(cancel_root.wstring(), 1, false, 2, cb_cap, &cap_stats);
        Check(!cap_ok && cap_stats.hit_cap && cap_stats.added == 2,
              "WalkDirectoryTree max_items cap stops and sets stats.hit_cap");
    }

    std::filesystem::remove_all(base, ec);

    // 6. SharedContentScope mount-point collision dedup & online preference
    {
        ContentIndexConfig prev;
        VolumeInfo v_e_offline;
        v_e_offline.mount_point = L"E:\\";
        v_e_offline.file_system = L"FAT32";
        v_e_offline.supported = true;
        v_e_offline.online = false;
        v_e_offline.enabled = true;

        VolumeInfo v_e_online;
        v_e_online.mount_point = L"e:\\";
        v_e_online.file_system = L"exFAT";
        v_e_online.supported = true;
        v_e_online.online = true;
        v_e_online.enabled = true;

        VolumeInfo v_c_online;
        v_c_online.mount_point = L"C:\\";
        v_c_online.file_system = L"NTFS";
        v_c_online.supported = true;
        v_c_online.online = true;
        v_c_online.enabled = true;

        VolumeInfo v_f_disabled;
        v_f_disabled.mount_point = L"F:\\";
        v_f_disabled.file_system = L"FAT32";
        v_f_disabled.supported = true;
        v_f_disabled.online = true;
        v_f_disabled.enabled = false;

        prev.roots.push_back({L"E:\\", pulse::text::Encoding::Utf8});
        const ContentIndexConfig scope = SharedContentScope(
            prev, {v_e_offline, v_e_online, v_c_online, v_f_disabled}, {});
        const bool has_two = scope.roots.size() == 2 &&
            scope.roots[0].path == L"c:\\" &&
            scope.roots[1].path == L"e:\\" && scope.roots[1].encoding == pulse::text::Encoding::Utf8;
        Check(has_two, "SharedContentScope deduplicates E:\\ preferring online volume and excludes disabled F:\\");
    }

    return g_failures ? 1 : 0;
}
