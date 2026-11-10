#pragma once
// 媒体文件夹专用列: the metadata behind them, read off the UI thread through
// the shell property system and kept in a cache keyed by path, modified time
// and size. Files that would have to be downloaded first (cloud placeholders,
// offline files) are never opened: they read as unknown.
#include "media_folder.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app::media {

struct Props {
    uint64_t taken = 0;       // 拍摄日期, UTC FILETIME ticks; 0 unknown
    uint32_t width = 0, height = 0;
    uint32_t track = 0;       // 0 unknown
    uint32_t fps_milli = 0;   // frames per 1000 s; 0 unknown
    uint64_t duration = 0;    // 100 ns; 0 unknown
    std::wstring camera, title, artist, album;
};

struct Job {
    std::wstring path;        // as the tab lists it (\\?\ prefix allowed)
    uint64_t mtime = 0;       // FILETIME ticks
    uint64_t size = 0;
    DWORD attrs = 0;
    Kind kind = Kind::None;
    const void* owner = nullptr;  // who asked (a tab); see Request
    std::shared_ptr<std::atomic<bool>> cancelled; // owner request generation
};

// "SONY" + "ILCE-7M3" → "SONY ILCE-7M3"; "Canon" + "Canon EOS R5" → "Canon EOS R5".
std::wstring CameraLabel(const std::wstring& make, const std::wstring& model);

// Worker side: reads one file (COM initialised by the caller).
Props ReadProps(const Job& job);

// The UI thread's handle on the shared reader.
// Replaces whatever `owner` still has queued (its folder changed) and reads
// these first; paths whose cached values are current are skipped.
void Request(const void* owner, std::vector<Job> jobs);
// Stops queued work and rejects completion of this owner's in-flight read.
void Cancel(const void* owner);
// Only publications directly inside this folder invalidate its ordering.
uint64_t Revision(const std::wstring& folder);
// Bumped when a batch is stored (up to 32 files or 150 ms of reading); the window timer repaints and re-sorts
// media-ordered listings when it moves (like CloudSyncProbe::Revision).
uint64_t Revision();
// The cached value for this exact file version; nullptr while pending or
// never asked for.
std::shared_ptr<const Props> Find(const std::wstring& path, uint64_t mtime, uint64_t size);
// Requests still waiting or being read.
size_t Pending();

// Every cached file directly inside `prefix` (a folder path ending in a
// backslash, spelt as the requests spelt it), whatever version it was read
// from: callers compare mtime / size with their listing. `fn` runs under the
// cache lock, so it only copies.
void ForEachInFolder(const std::wstring& prefix,
                     const std::function<void(std::wstring_view name, uint64_t mtime, uint64_t size,
                                              const Props& props)>& fn);

}  // namespace pulse::app::media
