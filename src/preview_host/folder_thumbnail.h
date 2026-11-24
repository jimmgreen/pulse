#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {
// Background preview-host only. COM must be initialized by the caller.
// False retains the ordinary icon; ERROR_TIMEOUT means retryable budget exhaustion.
bool DecodeFolderThumbnail(const std::wstring& path, UINT pixels,
    std::vector<uint8_t>& bgra, UINT& width, UINT& height, UINT& stride,
    bool single = false, bool force_refresh = false);
void ShutdownFolderThumbnailResources() noexcept;
#ifdef PULSE_FOLDER_THUMBNAIL_TESTING
void SetFolderThumbnailBudgetForTest(DWORD budget_ms, unsigned completed_pictures = ~0u);
#endif
}

