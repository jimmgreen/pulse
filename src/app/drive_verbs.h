// drive_verbs.h — File Explorer's drive verbs that Pulse runs itself:
// 弹出 (eject), 断开连接 (disconnect a mapped drive), 映射网络驱动器.
// They come from the This PC folder's own menu, not the registry, so the
// merged Explorer verbs never carry them.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

// Invalidate immediately on device change; queries in flight cannot repopulate old data.
void InvalidateDriveEjectableCache();
// Worker-only: all mounted paths on the same disk, including other partitions.
struct DriveEjectPlan {
    std::vector<std::wstring> roots;
    std::wstring volume_name;
};
DWORD PrepareDriveEject(const std::wstring& root, DriveEjectPlan& plan);
DWORD GetDriveEjectRoots(const std::wstring& root, std::vector<std::wstring>& roots);
// Call from the drive enumeration worker before publishing its snapshot.
void RefreshDriveEjectable(const std::wstring& root, uint8_t drive_type);
// Removable/optical media, hot-plug fixed disks and VHD/VHDX; cache-only on UI thread.
bool DriveEjectable(const std::wstring& root, uint8_t drive_type);

// The eject glyph: Segoe Fluent Icons has one; Segoe MDL2 (Windows 10) does not.
const wchar_t* EjectGlyph();

enum class DriveVerb : uint8_t { Eject, Disconnect };

struct DriveVerbResult {
    DriveVerb verb = DriveVerb::Eject;
    std::wstring root;        // "H:\"
    DWORD error = NO_ERROR;   // Win32 error, or ERROR_DEVICE_IN_USE for a veto
    std::wstring veto_name;   // eject: the device or program that refused
    bool open_files = false;  // disconnect: files are open (retry with force)
};

// Worker thread; posts `msg` with LPARAM = DriveVerbResult* (receiver deletes).
bool StartDriveEject(HWND notify, UINT msg, const std::wstring& root, uint8_t drive_type,
                     const std::wstring& expected_volume = {});
bool StartDriveDisconnect(HWND notify, UINT msg, const std::wstring& root, bool force);

// The system 映射网络驱动器 dialog, `share` prefilled (empty: blank).
// Modal on `owner`. NO_ERROR when a drive was mapped.
DWORD MapNetworkDrive(HWND owner, const std::wstring& share);

} // namespace pulse::app
