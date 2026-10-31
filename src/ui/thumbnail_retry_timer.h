#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
namespace pulse::ui {
inline constexpr UINT_PTR kThumbnailRetryTimer = 0x5452;
inline constexpr wchar_t kThumbnailRetryArmed[] = L"Pulse.ThumbnailRetryArmed";
inline void ArmThumbnailRetryTimer(HWND window, uint64_t deadline) {
    if (!deadline) { KillTimer(window, kThumbnailRetryTimer); RemovePropW(window, kThumbnailRetryArmed); return; }
    const auto now = GetTickCount64();
    const auto delay = deadline > now ? (std::min)(deadline - now, uint64_t(60000)) : 1;
    if (SetTimer(window, kThumbnailRetryTimer, static_cast<UINT>(delay), nullptr))
        SetPropW(window, kThumbnailRetryArmed, reinterpret_cast<HANDLE>(1));
}
inline bool HandleThumbnailRetryTimer(HWND window, UINT_PTR timer) {
    if (timer != kThumbnailRetryTimer) return false;
    KillTimer(window, kThumbnailRetryTimer);
    if (!RemovePropW(window, kThumbnailRetryArmed)) return false;
    InvalidateRect(window, nullptr, FALSE);
    return true;
}
}
