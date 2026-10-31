// drive_verbs.cpp — see drive_verbs.h.
#include "drive_verbs.h"
#include "../common/virtual_disk_eject.h"
#include "../ops/elevated_transfer_client.h"
#include <winioctl.h>
#include <mutex>
#include <map>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <virtdisk.h>
#include <devguid.h>
#include <ntddstor.h>
#include <winnetwk.h>
#include <thread>
#include <vector>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "virtdisk.lib")

namespace pulse::app {
namespace {

std::wstring DriveCacheKey(const std::wstring& root) {
    std::wstring key = root;
    if (key.starts_with(L"\\\\?\\") && (key.size() == 6 || key.size() == 7) && key[5] == L':')
        key.erase(0, 4);
    if ((key.size() == 2 || (key.size() == 3 && (key[2] == L'\\' || key[2] == L'/'))) && key[1] == L':') {
        if (key[0] >= L'a' && key[0] <= L'z') key[0] -= L'a' - L'A';
        if (key[0] >= L'A' && key[0] <= L'Z') return key.substr(0, 2) + L"\\";
    }
    return root;
}

std::wstring VolumeDevice(const std::wstring& root) {
    if (root.starts_with(L"\\\\?\\GLOBALROOT\\Device\\CdRom")) return root;
    if (root.starts_with(L"\\\\?\\Volume{")) {
        auto device = root;
        if (device.back() == L'\\') device.pop_back();
        return device;
    }
    if (root.size() < 2 || root[1] != L':') return {};
    return std::wstring(L"\\\\.\\") + root[0] + L":";
}

bool PhysicalCdDevice(const std::wstring& root, std::wstring& device) {
    if (root.size() < 2 || root[1] != L':' || GetDriveTypeW(root.c_str()) != DRIVE_CDROM) return false;
    HANDLE handle = CreateFileW(VolumeDevice(root).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE buffer[1024]{};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                    buffer, sizeof(buffer), &bytes, nullptr);
    CloseHandle(handle);
    if (!ok || bytes < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return false;
    const auto bus = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer)->BusType;
    if (bus != BusTypeAta && bus != BusTypeAtapi && bus != BusTypeSata && bus != BusTypeUsb) return false;
    wchar_t native[MAX_PATH]{};
    if (!QueryDosDeviceW(root.substr(0, 2).c_str(), native, MAX_PATH) ||
        std::wstring_view(native).find(L"\\Device\\CdRom") != 0) return false;
    device = L"\\\\?\\GLOBALROOT";
    device += native;
    return true;
}

DWORD CheckDriveEjectIdentity(const std::wstring& root, const std::wstring& expected_volume) {
    if (expected_volume.empty()) return ERROR_SUCCESS;
    if (expected_volume.starts_with(L"\\\\?\\GLOBALROOT\\Device\\CdRom")) {
        std::wstring current_device;
        return PhysicalCdDevice(root, current_device) && current_device == expected_volume
                   ? ERROR_SUCCESS : ERROR_MEDIA_CHANGED;
    }
    wchar_t current[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), current, MAX_PATH) ||
        _wcsicmp(current, expected_volume.c_str()) != 0) return ERROR_MEDIA_CHANGED;
    return ERROR_SUCCESS;
}

std::mutex eject_cache_mutex;
std::map<std::wstring, bool> eject_cache;
uint64_t eject_cache_generation = 0;

using storage::VirtualDiskImage;
using storage::DeviceNumber;
using storage::DiskVolumes;
using storage::DetachImage;

// The disk device node that holds the volume.
DEVINST DiskDevInst(const std::wstring& root) {
    const std::wstring device = VolumeDevice(root);
    if (device.empty()) return 0;
    HANDLE volume = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return 0;
    STORAGE_DEVICE_NUMBER want{};
    const bool got = DeviceNumber(volume, want);
    CloseHandle(volume);
    if (!got) return 0;
    // Never send virtual storage through PnP parent ejection when image lookup fails.
    HANDLE probe = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return 0;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE properties[1024]{};
    DWORD bytes = 0;
    const BOOL queried = DeviceIoControl(probe, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                         properties, sizeof(properties), &bytes, nullptr);
    CloseHandle(probe);
    if (!queried || bytes < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return 0;
    const auto bus = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(properties)->BusType;
    if (bus == BusTypeVirtual || bus == BusTypeFileBackedVirtual) return 0;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    DEVINST disk = 0;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    for (DWORD i = 0; !disk && SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_DISK, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buffer(need + sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA info{sizeof(info)};
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, &info)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER n{};
        if (DeviceNumber(h, n) && n.DeviceNumber == want.DeviceNumber && n.DeviceType == want.DeviceType)
            disk = info.DevInst;
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return disk;
}

bool Removable(DEVINST node) {
    DWORD caps = 0, size = sizeof(caps), type = 0;
    return CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_CAPABILITIES, &type, &caps, &size, 0) ==
               CR_SUCCESS && (caps & CM_DEVCAP_REMOVABLE);
}

// Safely Remove Hardware: the nearest removable node from the disk up (the
// USB device for a USB stick), else the disk's parent, as Explorer does.
DWORD EjectDevice(const std::wstring& root, std::wstring& veto_name) {
    const DEVINST disk = DiskDevInst(root);
    if (!disk) return ERROR_FILE_NOT_FOUND;
    DEVINST target = 0;
    for (DEVINST node = disk; node && !target;) {
        if (Removable(node)) target = node;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    if (!target && CM_Get_Parent(&target, disk, 0) != CR_SUCCESS) return ERROR_NOT_SUPPORTED;
    PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
    wchar_t name[MAX_PATH]{};
    const CONFIGRET cr = CM_Request_Device_EjectW(target, &veto, name, MAX_PATH, 0);
    if (cr == CR_SUCCESS && veto == PNP_VetoTypeUnknown && !name[0]) return NO_ERROR;
    // A program or service is worth naming; for an open handle the name is
    // only the volume's device instance path (STORAGE\Volume\{...}).
    if (veto == PNP_VetoWindowsApp || veto == PNP_VetoWindowsService) veto_name = name;
    return cr == CR_SUCCESS || cr == CR_REMOVE_VETOED ? ERROR_DEVICE_IN_USE
                                                     : CM_MapCrToWin32Err(cr, ERROR_NOT_READY);
}

// Optical drives: open the tray (Explorer's 弹出).
DWORD EjectMedia(const std::wstring& root) {
    std::wstring image;
    if (VirtualDiskImage(root, image) == ERROR_SUCCESS) return DetachImage(root, image);
    const std::wstring device = VolumeDevice(root);
    HANDLE h = CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD bytes = 0;
    if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr)) {
        const DWORD lock_error = GetLastError();
        // A verified physical empty tray has no filesystem to lock.
        if (!root.starts_with(L"\\\\?\\GLOBALROOT\\Device\\CdRom") ||
            (lock_error != ERROR_NOT_READY && lock_error != ERROR_NO_MEDIA_IN_DRIVE)) {
            CloseHandle(h);
            return lock_error == ERROR_ACCESS_DENIED || lock_error == ERROR_SHARING_VIOLATION
                       ? ERROR_DEVICE_IN_USE : lock_error;
        }
    }
    const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &bytes, nullptr);
    const DWORD error = ok ? NO_ERROR : GetLastError();
    CloseHandle(h);
    return error;
}

void Post(HWND notify, UINT msg, DriveVerbResult* result) {
    if (!notify || !PostMessageW(notify, msg, 0, reinterpret_cast<LPARAM>(result))) delete result;
}

int CALLBACK FontFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

} // namespace

bool QueryDriveEjectable(const std::wstring& root, uint8_t drive_type) {
    if (drive_type == DRIVE_REMOVABLE || drive_type == DRIVE_CDROM) return true;
    if (drive_type != DRIVE_FIXED) return false;
    const auto query_root = DriveCacheKey(root);
    std::wstring image;
    if (VirtualDiskImage(query_root, image) == ERROR_SUCCESS) return true;
    const std::wstring device = VolumeDevice(query_root);
    if (device.empty()) return false;
    HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE buffer[1024]{};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer,
                                    sizeof(buffer), &bytes, nullptr);
    CloseHandle(h);
    if (!ok || bytes < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return false;
    const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
    switch (descriptor->BusType) {
    case BusTypeUsb:
    case BusType1394:
    case BusTypeSd:
    case BusTypeMmc:
        return true;
    default:
        return descriptor->RemovableMedia != FALSE;
    }
}

void InvalidateDriveEjectableCache() {
    std::lock_guard lock(eject_cache_mutex);
    eject_cache.clear();
    ++eject_cache_generation;
}

DWORD PrepareDriveEject(const std::wstring& root, DriveEjectPlan& plan) {
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volume, MAX_PATH)) {
        const DWORD error = GetLastError();
        if ((error == ERROR_NOT_READY || error == ERROR_NO_MEDIA_IN_DRIVE) && PhysicalCdDevice(root, plan.volume_name)) {
            plan.roots = {root};
            return ERROR_SUCCESS;
        }
        return error;
    }
    plan.volume_name = volume;
    return GetDriveEjectRoots(plan.volume_name, plan.roots);
}

DWORD GetDriveEjectRoots(const std::wstring& root, std::vector<std::wstring>& roots) {
    std::vector<std::wstring> volumes;
    const DWORD error = DiskVolumes(root, volumes);
    if (error != ERROR_SUCCESS) return error;
    for (auto volume : volumes) {
        volume += L"\\";
        DWORD needed = 0;
        GetVolumePathNamesForVolumeNameW(volume.c_str(), nullptr, 0, &needed);
        if (!needed) return GetLastError();
        std::vector<wchar_t> paths(needed);
        if (!GetVolumePathNamesForVolumeNameW(volume.c_str(), paths.data(), needed, &needed)) return GetLastError();
        for (const wchar_t* path = paths.data(); *path; path += wcslen(path) + 1) roots.emplace_back(path);
    }
    return ERROR_SUCCESS;
}

void RefreshDriveEjectable(const std::wstring& root, uint8_t drive_type) {
    uint64_t generation = 0;
    {
        std::lock_guard lock(eject_cache_mutex);
        generation = eject_cache_generation;
    }
    const bool ejectable = QueryDriveEjectable(root, drive_type);
    std::lock_guard lock(eject_cache_mutex);
    if (generation == eject_cache_generation) eject_cache[DriveCacheKey(root)] = ejectable;
}

bool DriveEjectable(const std::wstring& root, uint8_t drive_type) {
    if (drive_type == DRIVE_REMOVABLE || drive_type == DRIVE_CDROM) return true;
    if (drive_type != DRIVE_FIXED) return false;
    std::lock_guard lock(eject_cache_mutex);
    const auto it = eject_cache.find(DriveCacheKey(root));
    return it != eject_cache.end() && it->second;
}

const wchar_t* EjectGlyph() {
    static const bool fluent = [] {
        bool found = false;
        LOGFONTW font{};
        font.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(font.lfFaceName, L"Segoe Fluent Icons");
        HDC dc = GetDC(nullptr);
        if (dc) {
            EnumFontFamiliesExW(dc, &font, FontFound, reinterpret_cast<LPARAM>(&found), 0);
            ReleaseDC(nullptr, dc);
        }
        return found;
    }();
    return fluent ? L"\xF847" : L"\xE88E";  // Eject : USB
}

DWORD EjectVirtualDiskWithAuthorization(const storage::VirtualDiskIdentity& identity, HWND owner, std::wstring* veto_name = nullptr) {
    DWORD error = storage::DetachVirtualDiskSafely(identity, veto_name);
    if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD) {
        std::atomic<bool> cancel{false};
        const auto elevated = ops::EjectWithElevatedHelper(identity, owner, cancel);
        error = SUCCEEDED(elevated.hr) ? ERROR_SUCCESS :
            HRESULT_FACILITY(elevated.hr) == FACILITY_WIN32 ? HRESULT_CODE(elevated.hr) : ERROR_GEN_FAILURE;
        if (veto_name && error == ERROR_DEVICE_IN_USE) *veto_name = elevated.error;
    }
    return error;
}

bool StartDriveEject(HWND notify, UINT msg, const std::wstring& root, uint8_t drive_type,
                     const std::wstring& expected_volume) {
    try {
        std::thread([notify, msg, root, drive_type, expected_volume] {
            auto* result = new DriveVerbResult;
            result->verb = DriveVerb::Eject;
            result->root = root;
            result->error = CheckDriveEjectIdentity(root, expected_volume);
            if (result->error != ERROR_SUCCESS) {
                Post(notify, msg, result);
                return;
            }
            const auto& target = expected_volume.empty() ? root : expected_volume;
            const UINT actual_type = GetDriveTypeW(root.c_str());
            if (actual_type != drive_type) result->error = ERROR_MEDIA_CHANGED;
            else if (actual_type == DRIVE_FIXED || actual_type == DRIVE_REMOVABLE || actual_type == DRIVE_CDROM) {
                storage::VirtualDiskIdentity identity;
                const DWORD identity_error = storage::CaptureVirtualDiskIdentity(expected_volume, identity);
                if (identity_error == ERROR_SUCCESS) {
                    result->error = EjectVirtualDiskWithAuthorization(identity, notify, &result->veto_name);
                } else if (actual_type == DRIVE_CDROM) {
                    result->error = EjectMedia(target);
                } else if (QueryDriveEjectable(target, static_cast<uint8_t>(actual_type))) {
                    result->error = EjectDevice(target, result->veto_name);
                } else result->error = identity_error == ERROR_ACCESS_DENIED ? identity_error : ERROR_NOT_SUPPORTED;
            } else result->error = ERROR_MEDIA_CHANGED;
            Post(notify, msg, result);
        }).detach();
        return true;
    } catch (...) {
        return false;
    }
}

bool StartDriveDisconnect(HWND notify, UINT msg, const std::wstring& root, bool force) {
    if (root.size() < 2 || root[1] != L':') return false;
    try {
        std::thread([notify, msg, root, force] {
            auto* result = new DriveVerbResult;
            result->verb = DriveVerb::Disconnect;
            result->root = root;
            const std::wstring local = root.substr(0, 2);
            // Like Explorer: the mapping is gone for good, not only this session.
            result->error = WNetCancelConnection2W(local.c_str(), CONNECT_UPDATE_PROFILE, force);
            result->open_files = !force &&
                (result->error == ERROR_OPEN_FILES || result->error == ERROR_DEVICE_IN_USE);
            Post(notify, msg, result);
        }).detach();
        return true;
    } catch (...) {
        return false;
    }
}

DWORD MapNetworkDrive(HWND owner, const std::wstring& share) {
    if (share.empty()) {
        const DWORD error = WNetConnectionDialog(owner, RESOURCETYPE_DISK);
        return error == static_cast<DWORD>(-1) ? ERROR_CANCELLED : error;
    }
    std::wstring remote = share;
    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = remote.data();
    CONNECTDLGSTRUCTW dialog{};
    dialog.cbStructure = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpConnRes = &resource;
    dialog.dwFlags = CONNDLG_RO_PATH;  // prefill the selected share; this flag excludes USE_MRU
    const DWORD error = WNetConnectionDialog1W(&dialog);
    return error == static_cast<DWORD>(-1) ? ERROR_CANCELLED : error;
}

} // namespace pulse::app
