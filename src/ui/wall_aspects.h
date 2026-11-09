#pragma once
// Photo wall aspect ratios (P2): width /
// height of pictures and videos. Decoded thumbnails report theirs while they
// are drawn; a background probe reads the rest from the file header (JPEG with
// its EXIF orientation, PNG, GIF, BMP, WebP) or, for other formats, the
// shell's System.Image.Dimensions / System.Video.FrameWidth+Height.
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <map>
#include <deque>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse::ui {

// Files the photo wall shows as pictures (images and videos), by extension.
bool IsWallPicture(const std::wstring& name, bool is_dir);

// Header parse over a whole buffer (tests); 0 when the format is unknown or
// the buffer ends first. EXIF orientations 5..8 swap width and height.
float HeaderAspect(const uint8_t* data, size_t size) noexcept;
// Reads only the bytes it needs; falls back to the shell's properties for
// formats the header parser does not know. 0 when nothing tells.
float ProbeFileAspect(const std::wstring& path);

class WallAspects {
public:
    static WallAspects& Instance();
    // Key of a path (never 0).
    static uint64_t Hash(const std::wstring& path) noexcept;
    static uint64_t ContentHash(const std::wstring& path, uint64_t modified, uint64_t size) noexcept;
    struct AspectChange { uint64_t hash; float aspect; };
    bool ChangesSince(uint64_t& version, std::vector<AspectChange>& changes) const;
    void ReplaceDemand(uint64_t owner, HWND notify, std::vector<std::pair<uint64_t, std::wstring>> files);
    void RemoveDemand(uint64_t owner);
    // out[i] = known aspect of hashes[i], left alone when not known (0 hashes skipped).
    bool Lookup(const uint64_t* hashes, float* out, size_t count) const;
    // A decoded thumbnail's aspect; true when it differs from what was known
    // by more than rounding (a 256 px decode of a 1500x1000 photo is 1.506).
    bool Note(uint64_t hash, float aspect);
    // Changes with every new or changed aspect.
    uint64_t Version() const noexcept { return version_.load(std::memory_order_acquire); }
    // Probe these files in the background, replacing the previous request
    // (only the folder on screen matters). notify is invalidated as results
    // come in, at most every 150 ms.
    void Probe(std::vector<std::pair<uint64_t, std::wstring>> files, HWND notify);
    size_t SizeForTest() const;

private:
    WallAspects() = default;
    static void CALLBACK Work(PTP_CALLBACK_INSTANCE, void* context);
    void Run();
    bool Store(uint64_t hash, float aspect, float tolerance);
    bool StoreLocked(uint64_t hash, float aspect, float tolerance);
    bool TakeLocked(std::pair<uint64_t, std::wstring>& file);
    bool WantedLocked(uint64_t hash) const;
    void NotifyLocked();
    friend struct WallAspectDemandTest;

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, float> known_;
    std::atomic<uint64_t> version_{1};
    struct Demand {
        HWND notify = nullptr;
        std::vector<std::pair<uint64_t, std::wstring>> files;
        std::unordered_set<uint64_t> attempted;
    };
    std::map<uint64_t, Demand> demands_;
    uint64_t last_owner_ = 0;
    std::unordered_set<HWND> dirty_windows_;
    std::deque<std::pair<uint64_t, AspectChange>> changes_;
    bool running_ = false;
};

}  // namespace pulse::ui
