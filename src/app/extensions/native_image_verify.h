#pragma once
#include <windows.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include "manifest.h"

namespace pulse::extensions {
inline constexpr size_t kMaxNativeImageBytes = 64 * 1024 * 1024;

// Never executes image code. Success transfers a read-only handle opened with
// FILE_SHARE_READ only; keep it open throughout module loading and execution.
// CloseHandle is the caller's responsibility. Failure leaves INVALID_HANDLE_VALUE.
// Module callers also supply an empty ancestor_locks vector, retain those handles
// alongside the file lock, and CloseHandle each after unloading the module.
bool OpenVerifiedNativeImage(const std::wstring& path, std::string_view expected_sha256,
    HANDLE& verified_file, std::string& error, std::vector<HANDLE>* ancestor_locks = nullptr);
// Locks every declared file and ancestor until the caller releases the handles.
bool OpenVerifiedNativePackage(const std::wstring& entry_path, const NativeRuntime& native,
    std::vector<HANDLE>& locks, std::string& error);
}
