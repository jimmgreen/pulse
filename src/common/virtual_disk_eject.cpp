#include "virtual_disk_eject.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <ntddstor.h>
#include <virtdisk.h>
#pragma comment(lib, "virtdisk.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
namespace pulse::storage {
#ifdef PULSE_VIRTUAL_DISK_FAULT_TEST
thread_local DWORD test_detach_error = ERROR_SUCCESS;
#endif
namespace {
std::wstring VolumeDevice(const std::wstring& root) {
    if (root.starts_with(L"\\\\?\\Volume{")) {
        auto device = root;
        if (device.back() == L'\\') device.pop_back();
        return device;
    }
    if (root.size() < 2 || root[1] != L':') return {};
    return std::wstring(L"\\\\.\\") + root[0] + L":";
}
}
DWORD VirtualDiskImage(const std::wstring& root, std::wstring& image) {
    const auto device = VolumeDevice(root);
    HANDLE volume = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return GetLastError();
    std::vector<BYTE> buffer(sizeof(STORAGE_DEPENDENCY_INFO));
    ULONG needed = 0;
    DWORD error = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto* info = reinterpret_cast<STORAGE_DEPENDENCY_INFO*>(buffer.data());
        info->Version = STORAGE_DEPENDENCY_INFO_VERSION_2;
        error = GetStorageDependencyInformation(volume, GET_STORAGE_DEPENDENCY_FLAG_HOST_VOLUMES,
                                                static_cast<ULONG>(buffer.size()), info, &needed);
        if (error != ERROR_INSUFFICIENT_BUFFER || needed <= buffer.size()) break;
        buffer.resize(needed);
    }
    CloseHandle(volume);
    if (error != ERROR_SUCCESS) return error;
    const auto* info = reinterpret_cast<const STORAGE_DEPENDENCY_INFO*>(buffer.data());
    for (ULONG i = 0; i < info->NumberEntries; ++i) {
        const auto& entry = info->Version2Entries[i];
        if (entry.AncestorLevel != 0 ||
            !IsEqualGUID(entry.VirtualStorageType.VendorId, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT) ||
            (entry.VirtualStorageType.DeviceId != VIRTUAL_STORAGE_TYPE_DEVICE_VHD &&
             entry.VirtualStorageType.DeviceId != VIRTUAL_STORAGE_TYPE_DEVICE_VHDX &&
             entry.VirtualStorageType.DeviceId != VIRTUAL_STORAGE_TYPE_DEVICE_ISO) ||
            !entry.HostVolumeName || !entry.DependentVolumeRelativePath) continue;
        image = entry.HostVolumeName;
        const wchar_t* relative = entry.DependentVolumeRelativePath;
        if (!image.empty() && image.back() == L'\\' && *relative == L'\\') ++relative;
        image += relative;
        return ERROR_SUCCESS;
    }
    return ERROR_NOT_FOUND;
}

bool DeviceNumber(HANDLE h, STORAGE_DEVICE_NUMBER& n) {
    DWORD bytes = 0;
    return DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &n, sizeof(n), &bytes,
                           nullptr) != FALSE;
}

DWORD DiskVolumes(const std::wstring& root, std::vector<std::wstring>& volumes) {
    const auto device = VolumeDevice(root);
    HANDLE source = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (source == INVALID_HANDLE_VALUE) return GetLastError();
    STORAGE_DEVICE_NUMBER wanted{};
    const bool got = DeviceNumber(source, wanted);
    const DWORD source_error = got ? ERROR_SUCCESS : GetLastError();
    CloseHandle(source);
    if (!got) return source_error;
    wchar_t name[MAX_PATH]{};
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD error = ERROR_SUCCESS;
    do {
        std::wstring path = name;
        path.pop_back();
        HANDLE volume = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (volume == INVALID_HANDLE_VALUE) { error = GetLastError(); break; }
        STORAGE_DEVICE_NUMBER number{};
        const bool identified = DeviceNumber(volume, number);
        const DWORD query_error = identified ? ERROR_SUCCESS : GetLastError();
        CloseHandle(volume);
        // Unsupported devices (e.g. network redirectors) have no local disk number.
        if (!identified && query_error != ERROR_INVALID_FUNCTION && query_error != ERROR_NOT_SUPPORTED) {
            error = query_error;
            break;
        }
        if (identified && number.DeviceType == wanted.DeviceType && number.DeviceNumber == wanted.DeviceNumber)
            volumes.push_back(path);
    } while (FindNextVolumeW(find, name, MAX_PATH));
    if (error == ERROR_SUCCESS && GetLastError() != ERROR_NO_MORE_FILES) error = GetLastError();
    FindVolumeClose(find);
    if (error == ERROR_SUCCESS && volumes.empty()) error = ERROR_NOT_FOUND;
    return error;
}

class LoadDriverPrivilege {
public:
    ~LoadDriverPrivilege() {
        if (active_) {
            if (previous_) SetThreadToken(nullptr, previous_);
            else RevertToSelf();
        }
        if (token_) CloseHandle(token_);
        if (previous_) CloseHandle(previous_);
    }
    DWORD Enable() {
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE | TOKEN_QUERY, TRUE, &previous_) && GetLastError() != ERROR_NO_TOKEN)
            return GetLastError();
        if (!ImpersonateSelf(SecurityImpersonation)) return GetLastError();
        active_ = true;
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &token_)) return GetLastError();
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, SE_LOAD_DRIVER_NAME, &privileges.Privileges[0].Luid)) return GetLastError();
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!AdjustTokenPrivileges(token_, FALSE, &privileges, 0, nullptr, nullptr)) return GetLastError();
        return GetLastError() == ERROR_NOT_ALL_ASSIGNED ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
    }
private:
    HANDLE previous_ = nullptr, token_ = nullptr;
    bool active_ = false;
};

DWORD WaitForDetached(HANDLE disk) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        GET_VIRTUAL_DISK_INFO info{}; info.Version = GET_VIRTUAL_DISK_INFO_IS_LOADED;
        ULONG info_size = sizeof(info);
        const DWORD state_error = GetVirtualDiskInformation(disk, &info_size, &info, nullptr);
        wchar_t path[MAX_PATH]{}; ULONG path_size = sizeof(path);
        const DWORD path_error = GetVirtualDiskPhysicalPath(disk, &path_size, path);
        if (state_error == ERROR_SUCCESS && !info.IsLoaded && path_error == ERROR_DEV_NOT_EXIST) return ERROR_SUCCESS;
        if ((state_error == ERROR_NOT_SUPPORTED || state_error == ERROR_INVALID_PARAMETER) && path_error == ERROR_DEV_NOT_EXIST)
            return ERROR_SUCCESS;
        if (state_error != ERROR_SUCCESS && state_error != ERROR_NOT_SUPPORTED && state_error != ERROR_INVALID_PARAMETER) return state_error;
        if (path_error != ERROR_SUCCESS && path_error != ERROR_DEV_NOT_EXIST && path_error != ERROR_NOT_READY) return path_error;
        Sleep(100);
    }
    return ERROR_TIMEOUT;
}

DWORD DetachImage(const std::wstring& root, const std::wstring& image, std::wstring* veto_detail, EjectDiagnostics* diagnostics) {
    wchar_t volume_name[MAX_PATH]{};
    if (root.starts_with(L"\\\\?\\Volume{")) wcscpy_s(volume_name, root.c_str());
    else if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volume_name, MAX_PATH)) return GetLastError();
    VirtualDiskIdentity identity;
    DWORD error = CaptureVirtualDiskIdentity(volume_name, identity);
    if (error != ERROR_SUCCESS) return error;
    HANDLE file = CreateFileW(image.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    error = GetFileInformationByHandle(file, &info) ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (error != ERROR_SUCCESS) return error;
    if (info.dwVolumeSerialNumber != identity.serial || info.nFileIndexHigh != identity.index_high ||
        info.nFileIndexLow != identity.index_low) return ERROR_MEDIA_CHANGED;
    HANDLE volume = CreateFileW(VolumeDevice(volume_name).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return GetLastError();
    STORAGE_DEVICE_NUMBER wanted{};
    const bool got = DeviceNumber(volume, wanted);
    error = got ? ERROR_SUCCESS : GetLastError();
    CloseHandle(volume);
    if (!got) return error;
    const GUID& interface_class = wanted.DeviceType == FILE_DEVICE_CD_ROM ? GUID_DEVINTERFACE_CDROM : GUID_DEVINTERFACE_DISK;
    HDEVINFO devices = SetupDiGetClassDevsW(&interface_class, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return GetLastError();
    DEVINST target = 0;
    std::wstring target_path;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    for (DWORD i = 0; !target && SetupDiEnumDeviceInterfaces(devices, nullptr, &interface_class, i, &iface); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &needed, nullptr);
        if (!needed) continue;
        std::vector<BYTE> buffer(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA data{sizeof(data)};
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, needed, nullptr, &data)) continue;
        HANDLE device = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER actual{};
        if (DeviceNumber(device, actual) && actual.DeviceType == wanted.DeviceType && actual.DeviceNumber == wanted.DeviceNumber) {
            target = data.DevInst;
            target_path = detail->DevicePath;
        }
        CloseHandle(device);
    }
    SetupDiDestroyDeviceInfoList(devices);
    if (!target) return ERROR_NOT_FOUND;
    // Release inspection handles before PnP negotiates removal with volume owners.
    // The exact virtual disk/CD-ROM node is used; never climb to a physical parent.
    VirtualDiskIdentity rechecked;
    error = CaptureVirtualDiskIdentity(volume_name, rechecked);
    if (error != ERROR_SUCCESS) return error;
    if (rechecked.serial != identity.serial || rechecked.index_high != identity.index_high ||
        rechecked.index_low != identity.index_low) return ERROR_MEDIA_CHANGED;
    volume = CreateFileW(VolumeDevice(volume_name).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return GetLastError();
    STORAGE_DEVICE_NUMBER current{};
    const bool current_got = DeviceNumber(volume, current);
    error = current_got ? ERROR_SUCCESS : GetLastError();
    CloseHandle(volume);
    if (!current_got) return error;
    if (current.DeviceType != wanted.DeviceType || current.DeviceNumber != wanted.DeviceNumber) return ERROR_MEDIA_CHANGED;
    HANDLE exact_device = CreateFileW(target_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
    if (exact_device == INVALID_HANDLE_VALUE) return GetLastError();
    STORAGE_DEVICE_NUMBER exact{};
    const bool exact_got = DeviceNumber(exact_device, exact);
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty; query.QueryType = PropertyStandardQuery;
    BYTE descriptor_bytes[1024]{}; DWORD count = 0;
    const BOOL described = DeviceIoControl(exact_device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                           descriptor_bytes, sizeof(descriptor_bytes), &count, nullptr);
    CloseHandle(exact_device);
    if (!exact_got || exact.DeviceType != current.DeviceType || exact.DeviceNumber != current.DeviceNumber ||
        !described || count < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return ERROR_MEDIA_CHANGED;
    const auto bus = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(descriptor_bytes)->BusType;
    if (bus != BusTypeVirtual && bus != BusTypeFileBackedVirtual) return ERROR_NOT_SUPPORTED;
    DWORD capabilities = 0, capability_size = sizeof(capabilities), property_type = 0;
    const CONFIGRET capability_result = CM_Get_DevNode_Registry_PropertyW(target, CM_DRP_CAPABILITIES, &property_type, &capabilities, &capability_size, 0);
    if (capability_result != CR_SUCCESS) return CM_MapCrToWin32Err(capability_result, ERROR_NOT_SUPPORTED);
    PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
    wchar_t veto_name[MAX_PATH]{};
    CONFIGRET result = CR_SUCCESS;
    if (capabilities & CM_DEVCAP_SURPRISEREMOVALOK) {
        VIRTUAL_STORAGE_TYPE storage{}; storage.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
        const bool iso = current.DeviceType == FILE_DEVICE_CD_ROM;
        if (iso) storage.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
        OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
        parameters.Version = iso ? OPEN_VIRTUAL_DISK_VERSION_1 : OPEN_VIRTUAL_DISK_VERSION_2;
        if (!iso) parameters.Version2.GetInfoOnly = TRUE;
        const auto access = iso ? static_cast<VIRTUAL_DISK_ACCESS_MASK>(VIRTUAL_DISK_ACCESS_DETACH | VIRTUAL_DISK_ACCESS_GET_INFO) : VIRTUAL_DISK_ACCESS_NONE;
        HANDLE disk = INVALID_HANDLE_VALUE;
        error = OpenVirtualDisk(&storage, image.c_str(), access, OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, &disk);
        if (error != ERROR_SUCCESS) return error;
        wchar_t physical[MAX_PATH]{}; ULONG physical_size = sizeof(physical);
        error = GetVirtualDiskPhysicalPath(disk, &physical_size, physical);
        if (error == ERROR_SUCCESS) {
            HANDLE mapped = CreateFileW(physical, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (mapped == INVALID_HANDLE_VALUE) error = GetLastError();
            else {
                STORAGE_DEVICE_NUMBER bound{};
                if (!DeviceNumber(mapped, bound)) error = GetLastError();
                else if (bound.DeviceType != current.DeviceType || bound.DeviceNumber != current.DeviceNumber) error = ERROR_MEDIA_CHANGED;
                CloseHandle(mapped);
            }
        }
        if (error != ERROR_SUCCESS) { CloseHandle(disk); return error; }
        result = CM_Request_Device_EjectW(target, &veto, veto_name, MAX_PATH, 0);
        if (result == CR_SUCCESS) error = WaitForDetached(disk);
        CloseHandle(disk);
        if (result == CR_SUCCESS) return error;
    } else {
        LoadDriverPrivilege privilege;
        error = privilege.Enable();
        if (error != ERROR_SUCCESS) return error;
        VIRTUAL_STORAGE_TYPE storage{}; storage.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
        const bool iso = current.DeviceType == FILE_DEVICE_CD_ROM;
        if (iso) storage.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
        OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
        parameters.Version = iso ? OPEN_VIRTUAL_DISK_VERSION_1 : OPEN_VIRTUAL_DISK_VERSION_2;
        const auto access = iso ? static_cast<VIRTUAL_DISK_ACCESS_MASK>(VIRTUAL_DISK_ACCESS_DETACH | VIRTUAL_DISK_ACCESS_GET_INFO) : VIRTUAL_DISK_ACCESS_NONE;
        HANDLE disk = INVALID_HANDLE_VALUE;
        error = OpenVirtualDisk(&storage, image.c_str(), access, OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, &disk);
        if (error != ERROR_SUCCESS) return error;
        wchar_t physical[MAX_PATH]{}; ULONG physical_size = sizeof(physical);
        error = GetVirtualDiskPhysicalPath(disk, &physical_size, physical);
        if (error == ERROR_SUCCESS) {
            HANDLE mapped = CreateFileW(physical, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (mapped == INVALID_HANDLE_VALUE) error = GetLastError();
            else {
                STORAGE_DEVICE_NUMBER bound{};
                if (!DeviceNumber(mapped, bound)) error = GetLastError();
                else if (bound.DeviceType != current.DeviceType || bound.DeviceNumber != current.DeviceNumber) error = ERROR_MEDIA_CHANGED;
                CloseHandle(mapped);
            }
        }
        if (error != ERROR_SUCCESS) { CloseHandle(disk); return error; }
        // Query/remove negotiates with every child volume. NO_RESTART closes the
        // gap before final detach; a failed detach restores this exact devnode.
        result = CM_Query_And_Remove_SubTreeW(target, &veto, veto_name, MAX_PATH,
                                             CM_REMOVE_UI_NOT_OK | CM_REMOVE_NO_RESTART);
        if (result == CR_SUCCESS) {
#ifdef PULSE_VIRTUAL_DISK_FAULT_TEST
            error = test_detach_error ? test_detach_error : DetachVirtualDisk(disk, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
#else
            error = DetachVirtualDisk(disk, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
#endif
            if (error != ERROR_SUCCESS) {
                const CONFIGRET reset = CM_Setup_DevNode(target, CM_SETUP_DEVNODE_RESET);
                const CONFIGRET restart = reset == CR_SUCCESS ? CM_Reenumerate_DevNode(target, CM_REENUMERATE_SYNCHRONOUS) : reset;
                if (restart != CR_SUCCESS) error = CM_MapCrToWin32Err(restart, ERROR_NOT_READY);
            } else error = WaitForDetached(disk);
            CloseHandle(disk);
            return error;
        }
        CloseHandle(disk);
    }
    if (diagnostics) { diagnostics->config_result = result; diagnostics->veto_type = veto; diagnostics->veto_name = veto_name; diagnostics->capabilities = capabilities; }
    if (result == CR_SUCCESS) return ERROR_SUCCESS;
    if (result == CR_REMOVE_VETOED) {
        if (veto == PNP_VetoInsufficientRights) return ERROR_ACCESS_DENIED;
        if (veto_detail && (veto == PNP_VetoWindowsApp || veto == PNP_VetoWindowsService)) *veto_detail = veto_name;
        if (veto == PNP_VetoIllegalDeviceRequest || veto == PNP_VetoNonDisableable) return ERROR_NOT_SUPPORTED;
        return ERROR_DEVICE_IN_USE;
    }
    return CM_MapCrToWin32Err(result, ERROR_NOT_READY);
}

bool ValidVolumeName(const std::wstring& name) {
    if (name.size() != 49 || !name.starts_with(L"\\\\?\\Volume{") || name[47] != L'}' || name[48] != L'\\') return false;
    for (size_t i = 0; i < 36; ++i) {
        const wchar_t c = name[11 + i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != L'-') return false; }
        else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) return false;
    }
    return true;
}
DWORD CaptureVirtualDiskIdentity(const std::wstring& volume, VirtualDiskIdentity& identity) {
    if (!ValidVolumeName(volume)) return ERROR_INVALID_PARAMETER;
    identity.volume_name = volume;
    DWORD error = VirtualDiskImage(volume, identity.image_path);
    if (error != ERROR_SUCCESS) return error;
    HANDLE file = CreateFileW(identity.image_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    error = GetFileInformationByHandle(file, &info) ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    identity.serial = info.dwVolumeSerialNumber;
    identity.index_high = info.nFileIndexHigh;
    identity.index_low = info.nFileIndexLow;
    return error;
}
DWORD DetachVirtualDiskSafely(const VirtualDiskIdentity& identity, std::wstring* veto_name) {
    VirtualDiskIdentity actual;
    const DWORD error = CaptureVirtualDiskIdentity(identity.volume_name, actual);
    if (error != ERROR_SUCCESS) return error;
    if (_wcsicmp(actual.image_path.c_str(), identity.image_path.c_str()) != 0 || actual.serial != identity.serial ||
        actual.index_high != identity.index_high || actual.index_low != identity.index_low) return ERROR_MEDIA_CHANGED;
    return DetachImage(actual.volume_name, actual.image_path, veto_name);
}
#ifdef PULSE_VIRTUAL_DISK_FAULT_TEST
DWORD TestDetachFailureRecovery(const VirtualDiskIdentity& identity, DWORD error) {
    test_detach_error = error;
    const DWORD result = DetachVirtualDiskSafely(identity);
    test_detach_error = ERROR_SUCCESS;
    return result;
}
#endif

}
