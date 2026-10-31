#pragma once
#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
namespace pulse::storage {
struct EjectDiagnostics { DWORD config_result = 0, veto_type = 0, capabilities = 0; std::wstring veto_name; };
struct VirtualDiskIdentity {
    std::wstring volume_name;
    std::wstring image_path;
    DWORD serial = 0, index_high = 0, index_low = 0;
};
#ifdef PULSE_VIRTUAL_DISK_FAULT_TEST
DWORD TestDetachFailureRecovery(const VirtualDiskIdentity& identity, DWORD error);
#endif
bool ValidVolumeName(const std::wstring& name);
DWORD CaptureVirtualDiskIdentity(const std::wstring& volume, VirtualDiskIdentity& identity);
DWORD DetachVirtualDiskSafely(const VirtualDiskIdentity& identity, std::wstring* veto_name = nullptr);
DWORD VirtualDiskImage(const std::wstring& root, std::wstring& image);
bool DeviceNumber(HANDLE handle, STORAGE_DEVICE_NUMBER& number);
DWORD DiskVolumes(const std::wstring& root, std::vector<std::wstring>& volumes);
DWORD DetachImage(const std::wstring& root, const std::wstring& image, std::wstring* veto_name = nullptr, EjectDiagnostics* diagnostics = nullptr);
}
