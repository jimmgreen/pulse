#pragma once
#include <windows.h>
#include <atomic>

// Only force-included into pulse_dialog_controller_dual_test. The process-wide
// HKCU override remains the outer isolation boundary for linked dependencies.
namespace pulse::bench::registry_views {
inline std::atomic_bool enabled{false};
LSTATUS WINAPI Create(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
LSTATUS WINAPI Open(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
LSTATUS WINAPI DeleteKey(HKEY, LPCWSTR, REGSAM, DWORD);
}
#define RegCreateKeyExW pulse::bench::registry_views::Create
#define RegOpenKeyExW pulse::bench::registry_views::Open
#define RegDeleteKeyExW pulse::bench::registry_views::DeleteKey
