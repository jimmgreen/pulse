#include "../app/duplicate_scan.h"

#include <cstdio>

using namespace pulse;

namespace {

int passed = 0;
int failed = 0;

void Check(bool condition, const wchar_t* name) {
    ++(condition ? passed : failed);
    wprintf(L"[%s] %s\n", condition ? L"PASS" : L"FAIL", name);
}

} // namespace

int wmain() {
    app::DuplicateScanSession session;
    Check(app::DuplicateScanSession::DefaultMinimumBytes(app::DuplicateScanScope::Folder) == 1024,
          L"folder default minimum is 1 KB");
    Check(app::DuplicateScanSession::DefaultMinimumBytes(app::DuplicateScanScope::Drive) ==
              1024ull * 1024ull,
          L"drive default minimum is 1 MB");
    Check(app::DuplicateScanSession::NormalizeDriveRoot(L"c:") == L"C:\\" &&
              app::DuplicateScanSession::NormalizeDriveRoot(L"D:\\") == L"D:\\",
          L"drive roots normalize to X:\\");

    index::VolumeInfo fixed;
    fixed.mount_point = L"C:\\";
    fixed.kind = index::VolumeKind::Fixed;
    index::VolumeInfo usb;
    usb.mount_point = L"E:\\";
    usb.kind = index::VolumeKind::Removable;
    auto all = app::DuplicateScanSession::ResolveRoots(
        app::DuplicateScanScope::AllFixed, L"", L"", {fixed, usb});
    Check(all.size() == 1 && all[0] == L"C:\\", L"all-local-disks uses fixed volumes only");
    auto drive = app::DuplicateScanSession::ResolveRoots(
        app::DuplicateScanScope::Drive, L"", L"E:\\", {fixed, usb});
    Check(drive.size() == 1 && drive[0] == L"E:\\", L"drive scope uses the selected root");
    auto folder = app::DuplicateScanSession::ResolveRoots(
        app::DuplicateScanScope::Folder, L"C:\\Windows", L"", {fixed, usb});
    Check(folder.size() == 1 && folder[0] == L"C:\\Windows", L"folder scope uses the folder path");

    session.generation = 1;
    session.scanning = true;
    index::ContentHit older;
    older.path = L"C:\\old.bin";
    older.name = L"old.bin";
    older.size = 100;
    older.modified = 10;
    older.group = 3;
    index::ContentHit newer;
    newer.path = L"D:\\new.bin";
    newer.name = L"new.bin";
    newer.size = 100;
    newer.modified = 20;
    newer.group = 3;
    index::ContentSearchProgress progress;
    progress.generation = 1;
    progress.done = true;
    session.ApplyUpdate(progress, {older, newer});
    Check(session.groups.size() == 1 && session.groups[0].files.size() == 2,
          L"hits with the same group become one card");
    Check(session.groups[0].keep_index == 0 &&
              session.groups[0].files[0].path == L"D:\\new.bin",
          L"default keep is the newest modified file");
    auto extra = session.FilesToDelete(0);
    Check(extra.size() == 1 && extra[0] == L"C:\\old.bin", L"FilesToDelete omits the kept file");
    session.SetKeep(0, 1);
    extra = session.FilesToDelete(0);
    Check(extra.size() == 1 && extra[0] == L"D:\\new.bin", L"SetKeep changes the file to delete");
    Check(session.AllFilesToDelete().size() == 1 && session.ExtraCount() == 1,
          L"all-extras matches the remaining copy");
    session.RemoveDeleted({L"D:\\new.bin"});
    Check(session.groups.empty(), L"groups drop when fewer than two files remain");

    app::DuplicateScanSession epoch;
    epoch.generation = 1;
    epoch.ApplyUpdate(progress, {older, newer});
    Check(epoch.result_epoch > 0, L"results bump the view epoch");
    const uint64_t after_hits = epoch.result_epoch;
    epoch.SetKeep(0, 1);
    Check(epoch.result_epoch > after_hits, L"keep changes bump the view epoch");
    const uint64_t after_keep = epoch.result_epoch;
    epoch.SetKeep(0, 1);
    Check(epoch.result_epoch == after_keep, L"unchanged keep leaves the view epoch");
    epoch.RemoveDeleted({L"D:\\new.bin"});
    Check(epoch.result_epoch > after_keep, L"deletes bump the view epoch");
    const uint64_t after_delete = epoch.result_epoch;
    epoch.ResetResults();
    Check(epoch.result_epoch > after_delete && epoch.groups.empty(),
          L"reset bumps the view epoch");

    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
