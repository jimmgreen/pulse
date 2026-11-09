// selection_caps_test.cpp - what a selection can do by what it is: drives get
// a rename and their own verbs but no cut / copy / delete, a server's shares and
// the Network view get no file commands, Del in the Network view only unpins.
#include "../app/selection_caps.h"
#include "../fs/drive_kind.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

// Nothing here is saved (PlacesCatalog::persist is off).
namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }

using namespace pulse;
using namespace pulse::app;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

fs::DirEntry Entry(const wchar_t* name, const wchar_t* full, uint8_t drive_type = 0, bool locked = false) {
    fs::DirEntry e;
    e.name = name;
    e.full_path = full;
    e.is_dir = true;
    e.attrs = FILE_ATTRIBUTE_DIRECTORY;
    e.drive_type = drive_type;
    e.drive_locked = locked;
    return e;
}

void Fill(Tab& tab, const wchar_t* path, std::vector<fs::DirEntry> entries) {
    tab.current_path = path;
    tab.SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>(std::move(entries)));
    tab.ClearSelection();
}

} // namespace

namespace {
int RunTrayRefreshTest() {
    namespace disk = std::filesystem;
    const auto root = disk::absolute(disk::path(L"bench_data") /
        (L"tray_refresh_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    std::error_code error;
    if (!disk::create_directories(root, error) || error) {
        Check(false, "create isolated tray refresh fixture");
        return 1;
    }
    const auto file = root / L"item.txt";
    { std::ofstream output(file); output << "fixture"; }
    StagingTray tray;
    tray.Collect({file.wstring()}, false);
    Check(tray.batches()[0].items[0].exists, "local tray fixture initially exists");
    Check(disk::remove(file, error) && !error, "delete isolated tray fixture");

    struct Environment {
        std::wstring name, value;
        bool present = false;
        explicit Environment(const wchar_t* key) : name(key) {
            const DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
            present = size != 0;
            if (size) {
                value.resize(size);
                value.resize(GetEnvironmentVariableW(key, value.data(), size));
            }
        }
        ~Environment() { SetEnvironmentVariableW(name.c_str(), present ? value.c_str() : nullptr); }
    } data_dir(L"PULSE_TEST_DATA_DIR"), remote_drives(L"PULSE_TEST_REMOTE_LIKE_DRIVES");
    const wchar_t marked[] = {fs::DriveLetterOf(file.wstring()), 0};
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", root.c_str());
    SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", marked);
    Check(fs::IsRemoteLikeDrive(file.wstring()), "fixture drive marked remote-like");
    Check(tray.batches()[0].items[0].path.starts_with(L"\\\\?\\"), "tray stores extended drive paths");
    Check(!tray.RefreshExists() && tray.batches()[0].items[0].exists,
          "remote-like refresh preserves known existence despite missing backing file");
    const std::wstring unc = L"\\\\pulse-tray-fixture.invalid\\share\\item.txt";
    tray.ReplacePath(file.wstring(), unc);
    Check(!tray.RefreshExists() && tray.batches()[0].items[0].exists,
          "UNC refresh preserves existence without accessing the share");
    tray.ReplacePath(unc, file.wstring());
    SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", nullptr);
    Check(!fs::IsRemoteLikeDrive(file.wstring()), "fixture returns to ordinary local drive");
    Check(tray.RefreshExists() && !tray.batches()[0].items[0].exists,
          "local deletion still updates tray existence");
    SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", marked);
    Check(!tray.RefreshExists() && !tray.batches()[0].items[0].exists,
          "remote-like refresh preserves previously missing state");
    Check(disk::remove(root, error) && !error, "remove isolated empty fixture directory");
    return g_failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--tray-refresh-only") == 0) return RunTrayRefreshTest();
    Check(IsUncServerRoot(L"\\\\nas") && IsUncServerRoot(L"\\\\nas\\") && IsUncServerRoot(L"\\\\?\\UNC\\nas"),
          "\\\\server is a server root");
    Check(!IsUncServerRoot(L"\\\\nas\\share") && !IsUncServerRoot(L"C:\\") && !IsUncServerRoot(L"\\\\?\\C:\\x"),
          "a share, a drive or a long local path is not a server root");
    Check(IsUncShareRoot(L"\\\\nas\\share") && IsUncShareRoot(L"\\\\nas\\share\\"),
          "\\\\server\\share is a share root");
    Check(!IsUncShareRoot(L"\\\\nas") && !IsUncShareRoot(L"\\\\nas\\share\\dir") && !IsUncShareRoot(L"D:\\share"),
          "a server, a folder in a share or a local path is not a share root");

    PlacesCatalog places;
    places.persist = false;
    Tab tab;

    // Ordinary folder: everything, rename for one item only.
    Fill(tab, L"C:\\work", {Entry(L"a", L"C:\\work\\a"), Entry(L"b", L"C:\\work\\b")});
    Check(ComputeSelectionCaps(tab, &places).kind == SelectionKind::None, "nothing selected: no commands");
    tab.ToggleSelect(0);
    SelectionCaps caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::Files && caps.cut && caps.copy && caps.del && caps.rename && caps.tags,
          "one file: cut, copy, delete, rename, tags");
    tab.ToggleSelect(1);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.cut && caps.copy && caps.del && !caps.rename, "two files: no single rename");

    // Search may contain an original-path replacement alongside a recycled item.
    auto recycled = Entry(L"a", L"C:\\work\\a");
    recycled.recycle_path = L"C:\\$Recycle.Bin\\S-1-5-21-test\\$Rtest";
    Fill(tab, L"pulse:search", {recycled, Entry(L"a", L"C:\\work\\a")});
    tab.ToggleSelect(0);
    caps = ComputeSelectionCaps(tab, &places);
    Check(RecycleSelectionOf(tab) == RecycleSelection::All && caps.del &&
          !caps.copy && !caps.cut && !caps.rename && !caps.tags,
          "recycled search result: only permanent deletion allowed");
    tab.ToggleSelect(1);
    caps = ComputeSelectionCaps(tab, &places);
    Check(RecycleSelectionOf(tab) == RecycleSelection::Mixed && !caps.del &&
          !caps.copy && !caps.cut && !caps.rename && !caps.tags,
          "mixed original-path replacement: no ordinary mutation allowed");

    // This PC: drives.
    Fill(tab, L"", {Entry(L"Win11 (C:)", L"C:\\", DRIVE_FIXED), Entry(L"USB (H:)", L"H:\\", DRIVE_REMOVABLE),
                    Entry(L"share (Y:)", L"Y:\\", DRIVE_REMOTE), Entry(L"Locked (L:)", L"L:\\", DRIVE_FIXED, true),
                    Entry(L"DVD (E:)", L"E:\\", DRIVE_CDROM)});
    tab.ToggleSelect(0);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::Drives && !caps.cut && !caps.copy && !caps.del && caps.rename && !caps.tags,
          "local drive: label rename only, no tags");
    Check(!caps.any_removable && !caps.any_remote && !caps.any_locked, "local drive: no drive verbs");
    tab.ClearSelection();
    tab.ToggleSelect(1);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.any_removable && caps.rename && !caps.del, "USB stick: eject and rename, no delete");
    tab.ClearSelection();
    tab.ToggleSelect(2);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.any_remote && caps.rename && !caps.copy, "mapped drive: disconnect and rename, no copy");
    tab.ClearSelection();
    tab.ToggleSelect(3);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.any_locked && caps.all_locked && !caps.rename, "locked drive: unlock, no rename");
    tab.ClearSelection();
    tab.ToggleSelect(4);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.any_removable && !caps.rename, "optical drive: eject, no label rename");
    tab.ToggleSelect(1);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::Drives && !caps.rename && !caps.del, "two drives: no rename, no delete");

    // A server's share list.
    Fill(tab, L"\\\\nas", {Entry(L"share", L"\\\\nas\\share")});
    tab.ToggleSelect(0);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::NetworkShares && !caps.cut && !caps.copy && !caps.del && !caps.rename && !caps.tags,
          "share in \\\\server: no file commands or tags");

    // Inside a share: ordinary files again.
    Fill(tab, L"\\\\nas\\share", {Entry(L"dir", L"\\\\nas\\share\\dir")});
    tab.ToggleSelect(0);
    Check(ComputeSelectionCaps(tab, &places).kind == SelectionKind::Files, "folder inside a share: files");

    // Network view: Del unpins Pulse's own pins only.
    places.PinNetwork(L"\\\\nas\\pinned", L"");
    Fill(tab, L"pulse:networks", {Entry(L"pinned", L"\\\\nas\\pinned"), Entry(L"system", L"\\\\nas\\system")});
    tab.ToggleSelect(1);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::NetworkPlaces && !caps.del && !caps.any_network_pin && !caps.rename && !caps.tags,
          "system network location: nothing to delete or tag");
    tab.ToggleSelect(0);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.any_network_pin && caps.del && !caps.cut && !caps.copy, "pinned location: Del unpins, no copy");

    // Recycle Bin: permanent delete only.
    Fill(tab, L"pulse:recycle", {Entry(L"old", L"C:\\old")});
    tab.ToggleSelect(0);
    caps = ComputeSelectionCaps(tab, &places);
    Check(caps.kind == SelectionKind::Recycle && caps.del && !caps.cut && !caps.copy && !caps.rename && !caps.tags,
          "recycle item: delete only");

    // Cloud mounts (C5): a WinFsp / Dokan drive is listed as a network drive but
    // has no WNet connection to disconnect; renaming its label stays possible.
    {
        fs::DirEntry cloud = Entry(L"BaiduNetdisk (B:)", L"B:\\", DRIVE_FIXED);
        cloud.drive_remote_like = true;
        Fill(tab, L"", {cloud});
        tab.ToggleSelect(0);
        caps = ComputeSelectionCaps(tab, &places);
        Check(caps.kind == SelectionKind::Drives && !caps.any_remote && !caps.any_removable && caps.rename && !caps.copy,
              "cloud mount: drive verbs only, no disconnect");
    }
    Check(fs::IsUserModeVolumeDevice(L"\\Device\\Volume{d6cc17c5-1722-4085-bce7-964f1e9f5de9}") &&
          fs::IsUserModeVolumeDevice(L"\\device\\volume{x}"),
          "WinFsp / Dokan device names are user-mode volumes");
    Check(!fs::IsUserModeVolumeDevice(L"\\Device\\HarddiskVolume3") && !fs::IsUserModeVolumeDevice(L"\\?" L"?\\C:\\sub") &&
          !fs::IsUserModeVolumeDevice(L"\\Device\\ImDisk0") && !fs::IsUserModeVolumeDevice(L"\\Device\\Volume{") &&
          !fs::IsUserModeVolumeDevice(L""),
          "partitions, subst, RAM disks are not");
    Check(fs::IsRemoteLikeFileSystemName(L"FUSE-rclone") && fs::IsRemoteLikeFileSystemName(L"WinFsp-FUSE") &&
          fs::IsRemoteLikeFileSystemName(L"Dokan") && fs::IsRemoteLikeFileSystemName(L"CloudDrive") &&
          fs::IsRemoteLikeFileSystemName(L"BaiduNetdiskFS") && fs::IsRemoteLikeFileSystemName(L"cbfs"),
          "cloud file system names");
    Check(!fs::IsRemoteLikeFileSystemName(L"NTFS") && !fs::IsRemoteLikeFileSystemName(L"exFAT") &&
          !fs::IsRemoteLikeFileSystemName(L"FAT32") && !fs::IsRemoteLikeFileSystemName(L"ReFS") &&
          !fs::IsRemoteLikeFileSystemName(L""),
          "disk file systems are not");
    Check(fs::DriveLetterOf(L"c:\\x") == L'C' && fs::DriveLetterOf(L"\\\\?\\D:\\") == L'D' &&
          fs::DriveLetterOf(L"\\\\.\\E:") == L'E' && fs::DriveLetterOf(L"\\\\?\\UNC\\srv\\s") == 0 &&
          fs::DriveLetterOf(L"\\\\srv\\s") == 0 && fs::DriveLetterOf(L"pulse:recycle") == 0,
          "drive letter of plain, extended and UNC paths");
    {
        wchar_t windir[MAX_PATH]{};
        GetWindowsDirectoryW(windir, MAX_PATH);
        Check(!fs::IsRemoteLikeDrive(windir) && !fs::IsRemoteLikeDrive(L"\\\\srv\\share") && !fs::IsRemoteLikeDrive(L""),
              "system drive and UNC paths are not cloud mounts");
        const std::wstring sys(1, windir[0]);
        SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", sys.c_str());
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", L"x");
        const bool marked = fs::IsRemoteLikeDrive(windir);
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr);
        const bool outside_tests = fs::IsRemoteLikeDrive(windir);
        SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", nullptr);
        Check(marked && !outside_tests, "test marker only applies to test instances");
    }

    {
        // This PC program icons (网盘程序入口): open only, even next to a drive.
        fs::DirEntry launcher;
        launcher.name = L"Baidu";
        launcher.full_path = L"shell:::{679F137C-3162-45da-BE3C-2F9C3D093F64}";
        launcher.pc_launcher = 2;
        Fill(tab, L"", {Entry(L"Win11 (C:)", L"C:\\", DRIVE_FIXED), launcher});
        tab.ToggleSelect(1);
        caps = ComputeSelectionCaps(tab, &places);
        Check(caps.kind == SelectionKind::Launchers && !caps.cut && !caps.copy && !caps.del && !caps.rename &&
              !caps.tags, "launcher: no file commands");
        tab.ToggleSelect(0);
        caps = ComputeSelectionCaps(tab, &places);
        Check(caps.kind == SelectionKind::Launchers && !caps.rename && !caps.del, "drive + launcher: still none");
        tab.view_mode = ui::ViewMode::LargeIcons;
        const int pc_icons = tab.EffectiveGroup();
        tab.view_mode = ui::ViewMode::List;
        const int pc_list = tab.EffectiveGroup();
        tab.view_mode = ui::ViewMode::Tiles;
        tab.group_by = 0;
        Check(pc_icons == 7 && pc_list == 0 && tab.EffectiveGroup() == 7,
              "This PC groups by kind in every view but List");
        Fill(tab, L"C:\\work", {Entry(L"a", L"C:\\work\\a")});
        Check(tab.EffectiveGroup() == 0, "real folders keep their own grouping");
    }

    {
        // This PC phones (D2): open only, never a copy / delete / rename source.
        fs::DirEntry phone;
        phone.name = L"Xiaomi 14";
        phone.full_path = L"pulse:device:0123456789ab";
        phone.pc_device = 1;
        Fill(tab, L"", {Entry(L"Win11 (C:)", L"C:\\", DRIVE_FIXED), phone});
        tab.ToggleSelect(1);
        caps = ComputeSelectionCaps(tab, &places);
        Check(!caps.cut && !caps.copy && !caps.del && !caps.rename && !caps.tags, "phone: no file commands");
        tab.ToggleSelect(0);
        caps = ComputeSelectionCaps(tab, &places);
        Check(!caps.rename && !caps.del && !caps.cut, "drive + phone: still none");
    }

    {
        // Inside a phone: Explorer's file commands, run on the shell ids.
        fs::DirEntry photo = Entry(L"IMG_1.jpg", L"pulse:device:0123456789ab\\Internal\\DCIM\\IMG_1.jpg");
        photo.is_dir = false;
        photo.attrs = FILE_ATTRIBUTE_NORMAL;
        fs::DirEntry video = photo;
        video.name = L"VID_2.mp4";
        video.full_path = L"pulse:device:0123456789ab\\Internal\\DCIM\\VID_2.mp4";
        Fill(tab, L"pulse:device:0123456789ab\\Internal\\DCIM", {photo, video});
        tab.ToggleSelect(0);
        caps = ComputeSelectionCaps(tab, &places);
        Check(caps.kind == SelectionKind::Device && caps.cut && caps.copy && caps.del && caps.rename && !caps.tags,
              "phone file: cut, copy, delete, rename; no tags");
        tab.ToggleSelect(1);
        caps = ComputeSelectionCaps(tab, &places);
        Check(caps.cut && caps.copy && caps.del && !caps.rename, "two phone files: no single rename");

        // An iPhone (or a file the shell will not delete): copy only.
        fs::DirEntry locked_photo = photo;
        locked_photo.attrs |= FILE_ATTRIBUTE_READONLY;
        Fill(tab, L"pulse:device:0123456789ab\\Internal\\DCIM", {locked_photo});
        tab.ToggleSelect(0);
        caps = ComputeSelectionCaps(tab, &places);
        Check(caps.kind == SelectionKind::Device && caps.copy && !caps.cut && !caps.del && !caps.rename,
              "read-only phone file: copy only");

        // A device root lists storages: copy only, never delete / rename.
        Fill(tab, L"pulse:device:0123456789ab", {Entry(L"Internal", L"pulse:device:0123456789ab\\Internal")});
        tab.ToggleSelect(0);
        caps = ComputeSelectionCaps(tab, &places);
        Check(caps.kind == SelectionKind::Device && caps.copy && !caps.cut && !caps.del && !caps.rename,
              "phone storage: copy only");

        // Mixed selection: one read-only item keeps delete off for all.
        Fill(tab, L"pulse:device:0123456789ab\\Internal\\DCIM", {locked_photo, video});
        tab.ToggleSelect(0);
        tab.ToggleSelect(1);
        caps = ComputeSelectionCaps(tab, &places);
        Check(caps.copy && !caps.del && !caps.cut, "read-only + writable phone files: copy only");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
