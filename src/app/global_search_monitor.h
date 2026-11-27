#pragma once
#include <algorithm>
#include <cmath>
#include <windows.h>

namespace pulse::app {
inline constexpr float kSearchMinWidth = 640, kSearchMinHeight = 420;
inline constexpr float kSearchDefaultWidth = 960, kSearchDefaultHeight = 620;
inline HMONITOR SearchSummonMonitor() {
    POINT cursor{};
    return GetCursorPos(&cursor) ? MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST)
        : MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
}
inline bool SearchRestorePosition(HMONITOR target, bool positioned, POINT saved) {
    return target && positioned && MonitorFromPoint(saved, MONITOR_DEFAULTTONULL) == target;
}
inline RECT SearchMonitorWorkArea(HMONITOR& monitor) {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info) && info.rcWork.right > info.rcWork.left && info.rcWork.bottom > info.rcWork.top)
        return info.rcWork;
    monitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoW(monitor, &info) && info.rcWork.right > info.rcWork.left && info.rcWork.bottom > info.rcWork.top)
        return info.rcWork;
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) && work.right > work.left && work.bottom > work.top) return work;
    const int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
    return {0, 0, width > 0 ? width : 1, height > 0 ? height : 1};
}
inline RECT SearchWindowPlacement(const RECT& work, float scale, bool positioned, POINT saved,
    SIZE size_dip, bool restore_position) {
    if (work.right <= work.left || work.bottom <= work.top) return {work.left, work.top, work.left + 1, work.top + 1};
    const auto px = [scale](float dip) { return static_cast<int>(std::lround(dip * scale)); };
    const float want_w = size_dip.cx > 0 ? static_cast<float>(size_dip.cx) : kSearchDefaultWidth;
    const float want_h = size_dip.cy > 0 ? static_cast<float>(size_dip.cy) : kSearchDefaultHeight;
    const int max_w = (std::max)(1, static_cast<int>(work.right - work.left) - px(32));
    const int max_h = (std::max)(1, static_cast<int>(work.bottom - work.top) - px(32));
    const int w = std::clamp(px(want_w), (std::min)(px(kSearchMinWidth), max_w), max_w);
    const int h = std::clamp(px(want_h), (std::min)(px(kSearchMinHeight), max_h), max_h);
    const bool restore = positioned && restore_position;
    const int x = work.left + (work.right - work.left - w) / 2;
    const int y = work.top + (work.bottom - work.top - h) / (positioned && !restore ? 2 : 4);
    const int left = restore ? std::clamp(static_cast<int>(saved.x), static_cast<int>(work.left), static_cast<int>(work.right) - w) : x;
    const int top = restore ? std::clamp(static_cast<int>(saved.y), static_cast<int>(work.top), static_cast<int>(work.bottom) - h) : y;
    return {left, top, left + w, top + h};
}
}
