// fs_net_cache.h — Persistent UNC directory snapshots + one-shot connectivity probe.
#pragma once
#include "fs_enum.h"
#include "fs_snapshot.h"
#include <cstdint>
#include <string>
#include <windows.h>

namespace pulse::fs {

struct UncProbeResult {
    std::wstring unc;
    NetStatus status = NetStatus::Unknown;
    DWORD rtt_ms = 0;
};

bool SaveNetSnapshot(const std::wstring& path, const SnapshotPtr& snapshot);
SnapshotPtr LoadNetSnapshot(const std::wstring& path, uint64_t* unix_sec = nullptr);
std::wstring FormatCacheAge(uint64_t unix_sec);

// Background probe: posts `msg` to `hwnd` with lParam = heap UncProbeResult*.
void StartUncProbe(HWND hwnd, UINT msg, std::wstring unc);

} // namespace pulse::fs
