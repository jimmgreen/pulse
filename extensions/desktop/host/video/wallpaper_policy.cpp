// wallpaper_policy.cpp - Policy decision engine, pause/release state machine, and LRU cache.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include "host/video/wallpaper_policy.h"
#include "ipc/desktop_ipc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pulse::desktop::video {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kMb = 1024ull * 1024ull;

bool FileExistsNonEmpty(const std::wstring& path, uint64_t* out_size = nullptr) noexcept {
    if (path.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    const uint64_t sz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (out_size) *out_size = sz;
    return sz > 0;
}

bool AtomicWriteUtf8(const std::wstring& path, std::string_view utf8) {
    if (path.empty()) return false;
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = utf8.empty() || WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != utf8.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::string ReadUtf8File(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    std::string out;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 4 * 1024 * 1024) {
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr)) {
            out.clear();
        } else {
            out.resize(got);
        }
    }
    CloseHandle(h);
    return out;
}

TranscodeCodecChoice PickEncoder(const VideoProbeInfo& probe, const GpuCapabilities& gpu) noexcept {
    // §4: Only encode to HEVC if this machine can ALSO decode HEVC!
    if (gpu.has_hevc_hw_encoder && gpu.has_hevc_decoder) {
        return probe.bit_depth >= 10 ? TranscodeCodecChoice::HevcMain10Hw : TranscodeCodecChoice::HevcMainHw;
    }
    if (gpu.has_h264_hw_encoder) {
        return TranscodeCodecChoice::H264HighHw;
    }
    if (gpu.has_h264_sw_encoder) {
        return TranscodeCodecChoice::H264HighSw;
    }
    return TranscodeCodecChoice::None;
}

} // namespace

const wchar_t* CodecChoiceArgName(TranscodeCodecChoice choice) noexcept {
    switch (choice) {
    case TranscodeCodecChoice::HevcMain10Hw: return L"hevc_main10";
    case TranscodeCodecChoice::HevcMainHw:   return L"hevc_main";
    case TranscodeCodecChoice::H264HighHw:   return L"h264_hw";
    case TranscodeCodecChoice::H264HighSw:   return L"h264_sw";
    case TranscodeCodecChoice::None:
    default:                                 return L"none";
    }
}

uint64_t ComputeTranscodeJobMemoryLimit(uint64_t available_ram_bytes) noexcept {
    if (available_ram_bytes == 0) return kMaxTranscodeJobMemoryBytes;
    const uint64_t half_avail = available_ram_bytes / 2ull;
    return std::min<uint64_t>(kMaxTranscodeJobMemoryBytes, half_avail);
}

uint64_t ComputePlayOriginalVramBudget(const GpuCapabilities& gpu) noexcept {
    // §11 Q3: 25% of dedicated VRAM (e.g. 768 MB on 3 GB GPU), or 10% of system RAM for iGPU.
    if (gpu.is_integrated_gpu || gpu.dedicated_vram_bytes < 512ull * kMb) {
        return gpu.system_ram_bytes / 10ull;
    }
    return gpu.dedicated_vram_bytes / 4ull;
}

uint64_t EstimatePlaybackVramBytes(const VideoProbeInfo& probe, uint32_t surface_w, uint32_t surface_h) noexcept {
    const uint64_t cw = probe.coded_width ? probe.coded_width : probe.display_width;
    const uint64_t ch = probe.coded_height ? probe.coded_height : probe.display_height;
    const double bytes_per_px = probe.bit_depth >= 10 ? 3.0 : 1.5; // P010 = 3 B/px, NV12 = 1.5 B/px
    const uint64_t pool_frames = (probe.codec == VideoCodecKind::H264) ? 12ull : 8ull;
    const uint64_t decode_pool = static_cast<uint64_t>(static_cast<double>(cw) * static_cast<double>(ch) *
                                                       bytes_per_px * static_cast<double>(pool_frames));
    const uint64_t driver_fixed = 86ull * kMb;
    const uint64_t swap_chain = static_cast<uint64_t>(surface_w) * static_cast<uint64_t>(surface_h) * 4ull * 2ull;
    return decode_pool + driver_fixed + swap_chain;
}

uint64_t EstimateSoftwareDecodePeakRamBytes(const VideoProbeInfo& probe) noexcept {
    const uint64_t cw = probe.coded_width ? probe.coded_width : probe.display_width;
    const uint64_t ch = probe.coded_height ? probe.coded_height : probe.display_height;
    const double bytes_per_px = probe.bit_depth >= 10 ? 3.0 : 1.5;
    // Calibrated against RESULTS.md: 8K H.264 SW decode peaks ~2.4-2.6 GB; 4K SW ~600-800 MB.
    const double frame_bytes = static_cast<double>(cw) * static_cast<double>(ch) * bytes_per_px;
    const double frames_est = (probe.codec == VideoCodecKind::H264) ? 45.0 : 28.0;
    return static_cast<uint64_t>(frame_bytes * frames_est) + 256ull * kMb;
}

PolicyDecision EvaluateWallpaperPolicy(const VideoProbeInfo& probe,
                                       uint32_t screen_target_w,
                                       uint32_t screen_target_h,
                                       const GpuCapabilities& gpu) {
    PolicyDecision d{};
    d.vram_budget_bytes = ComputePlayOriginalVramBudget(gpu);
    d.job_memory_limit_bytes = ComputeTranscodeJobMemoryLimit(gpu.available_ram_bytes);

    if (!probe.file_exists) {
        d.kind = PolicyDecisionKind::RejectMissingFile;
        d.user_message = L"找不到这个视频，可能已移动或删除";
        return d;
    }

    if (probe.color.is_hdr) {
        d.kind = PolicyDecisionKind::RejectHdr;
        d.user_message = L"暂不支持 HDR 视频";
        return d;
    }

    if (!probe.open_ok || !probe.has_mf_decoder) {
        if (probe.codec == VideoCodecKind::HEVC) {
            d.kind = PolicyDecisionKind::RejectMissingHevc;
            d.store_uri = !probe.store_uri.empty()
                              ? probe.store_uri
                              : L"ms-windows-store://pdp/?ProductId=9NMZLZ57R3T7";
            d.user_message = L"需要 HEVC 视频扩展才能播放这个视频";
            return d;
        }
        if (probe.codec == VideoCodecKind::AV1) {
            d.kind = PolicyDecisionKind::RejectMissingAv1;
            d.store_uri = !probe.store_uri.empty()
                              ? probe.store_uri
                              : L"ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V";
            d.user_message = L"需要 AV1 视频扩展才能播放这个视频";
            return d;
        }
        d.kind = PolicyDecisionKind::RejectUnsupportedCodec;
        d.user_message = L"这台电脑缺少该视频格式的解码器，暂时无法播放";
        return d;
    }

    const uint32_t vw = probe.display_width ? probe.display_width : probe.coded_width;
    const uint32_t vh = probe.display_height ? probe.display_height : probe.coded_height;
    const uint32_t sw = std::max<uint32_t>(2u, screen_target_w);
    const uint32_t sh = std::max<uint32_t>(2u, screen_target_h);
    ComputeAspectFitEvenSize(vw, vh, sw, sh, d.target_width, d.target_height);

    // §4: dimension <= target * 1.1 -> fits screen
    const bool fits_1_1x = (static_cast<double>(vw) <= static_cast<double>(sw) * 1.1 + 0.5) &&
                           (static_cast<double>(vh) <= static_cast<double>(sh) * 1.1 + 0.5);

    d.estimated_play_vram_bytes = EstimatePlaybackVramBytes(probe, d.target_width, d.target_height);

    if (probe.hw_decode_supported && fits_1_1x) {
        d.kind = PolicyDecisionKind::PlayOriginal;
        d.target_width = vw;
        d.target_height = vh;
        return d;
    }

    d.encoder = PickEncoder(probe, gpu);
    if (d.encoder == TranscodeCodecChoice::None) {
        d.kind = PolicyDecisionKind::RejectUnsupportedCodec;
        d.user_message = L"这台电脑缺少可用的视频编码器，无法优化此视频";
        return d;
    }

    if (probe.hw_decode_supported) {
        // Larger than target * 1.1, hardware decodeable: transcode in background;
        // play original meanwhile if within VRAM budget (§11 Q3).
        d.kind = PolicyDecisionKind::TranscodeThenSwitch;
        d.requires_software_decode = false;
        d.play_original_while_transcoding = (d.vram_budget_bytes > 0 &&
                                             d.estimated_play_vram_bytes <= d.vram_budget_bytes);
        d.require_heavy_confirm = false;
        return d;
    }

    // Cannot hardware-decode: must transcode via software decode in isolated job-limited process.
    d.requires_software_decode = true;
    d.estimated_transcode_ram_bytes = EstimateSoftwareDecodePeakRamBytes(probe);
    if (d.job_memory_limit_bytes > 0 && d.estimated_transcode_ram_bytes > d.job_memory_limit_bytes) {
        d.kind = PolicyDecisionKind::RejectTooLarge;
        d.user_message = L"这个视频太大，这台电脑转换不了";
        return d;
    }

    d.kind = PolicyDecisionKind::TranscodeThenSwitch;
    d.play_original_while_transcoding = false; // §4: show poster only while SW-decoding transcode runs
    d.require_heavy_confirm = (d.estimated_transcode_ram_bytes > kHeavyTranscodeConfirmThresholdBytes);
    return d;
}

// ---------------------------------------------------------------------------
// VideoPauseStateMachine
// ---------------------------------------------------------------------------
PauseStepResult VideoPauseStateMachine::Step(const PauseSignals& signals, uint64_t now_ms) noexcept {
    PauseStepResult out{};
    const bool want_pause = signals.AnyPaused();
    const VideoPauseReason reason = signals.PrimaryReason();

    if (want_pause) {
        reason_ = reason;
        if (state_ == PauseLifecycleState::Playing) {
            state_ = PauseLifecycleState::PausedHot;
            paused_since_ms_ = now_ms;
            out.entered_pause = true;
        } else if (state_ == PauseLifecycleState::PausedHot) {
            if (now_ms >= paused_since_ms_ && (now_ms - paused_since_ms_) >= freeze_delay_ms_) {
                state_ = PauseLifecycleState::FrozenReleased;
                out.should_freeze_release = true;
            }
        }
    } else {
        if (state_ == PauseLifecycleState::PausedHot) {
            out.resumed_from_hot = true;
        } else if (state_ == PauseLifecycleState::FrozenReleased) {
            out.resumed_from_frozen = true;
        }
        state_ = PauseLifecycleState::Playing;
        reason_ = VideoPauseReason::None;
        paused_since_ms_ = 0;
    }

    out.state = state_;
    out.reason = reason_;
    return out;
}

// ---------------------------------------------------------------------------
// SHA-256 & WallpaperCacheManager
// ---------------------------------------------------------------------------
std::wstring ComputeSha256HexWide(std::string_view data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    DWORD hash_len = 0, cb_res = 0;
    uint8_t digest[32]{};
    bool ok = false;
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &cb_res, 0) == 0 &&
        hash_len == sizeof(digest)) {
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hash, const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data.data())),
                               static_cast<ULONG>(data.size()), 0) == 0 &&
                BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                ok = true;
            }
            BCryptDestroyHash(hash);
        }
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};

    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring hex;
    hex.reserve(64);
    for (uint8_t b : digest) {
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 0xFu]);
    }
    return hex;
}

std::wstring ComputeWallpaperCacheKey(const CacheKeyInput& input) {
    const std::wstring norm_path = NormalizePathForCompare(input.source_path);
    const std::string utf8_path = ipc::WideToUtf8(norm_path);
    const std::string utf8_codec = ipc::WideToUtf8(input.codec_pref);
    char meta[160]{};
    std::snprintf(meta, sizeof(meta), "|%llu|%llu|%u|%u|%u|",
                  static_cast<unsigned long long>(input.file_size_bytes),
                  static_cast<unsigned long long>(input.file_mtime_100ns),
                  input.target_width,
                  input.target_height,
                  input.scheme_version);
    std::string material;
    material.reserve(utf8_path.size() + utf8_codec.size() + 160);
    material.append(utf8_path);
    material.append(meta);
    material.append(utf8_codec);
    return ComputeSha256HexWide(material);
}

std::wstring DefaultWallpaperCacheDir() {
    wchar_t override_dir[MAX_PATH * 2]{};
    const DWORD env_len = GetEnvironmentVariableW(L"PULSE_WALLPAPER_CACHE_DIR", override_dir, ARRAYSIZE(override_dir));
    if (env_len > 0 && env_len < ARRAYSIZE(override_dir)) {
        return std::wstring(override_dir, env_len);
    }
    wchar_t local_app[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_app))) {
        return (fs::path(local_app) / L"Pulse" / L"wallpaper-cache").wstring();
    }
    return L"wallpaper-cache";
}

WallpaperCacheManager::WallpaperCacheManager(std::wstring cache_dir, uint64_t budget_bytes)
    : cache_dir_(cache_dir.empty() ? DefaultWallpaperCacheDir() : std::move(cache_dir)),
      budget_bytes_(budget_bytes) {}

std::wstring WallpaperCacheManager::IndexPath() const {
    return (fs::path(cache_dir_) / L"index.json").wstring();
}

bool WallpaperCacheManager::EnsureDirectory() {
    std::error_code ec;
    fs::create_directories(fs::path(cache_dir_), ec);
    return !ec;
}

std::wstring WallpaperCacheManager::VideoFilePathForKey(std::wstring_view key) const {
    return (fs::path(cache_dir_) / (std::wstring(key) + L".mp4")).wstring();
}

std::wstring WallpaperCacheManager::TempVideoFilePathForKey(std::wstring_view key) const {
    return (fs::path(cache_dir_) / (std::wstring(key) + L".tmp.mp4")).wstring();
}

std::wstring WallpaperCacheManager::PosterFilePathForKey(std::wstring_view key) const {
    return (fs::path(cache_dir_) / (std::wstring(key) + L".poster.jpg")).wstring();
}

bool WallpaperCacheManager::LoadIndex() {
    entries_.clear();
    const std::string raw = ReadUtf8File(IndexPath());
    if (raw.empty()) return false;

    const size_t arr = raw.find("\"entries\"");
    if (arr == std::string::npos) return false;
    size_t pos = raw.find('[', arr);
    if (pos == std::string::npos) return false;
    while (pos < raw.size()) {
        const size_t ob = raw.find('{', pos);
        if (ob == std::string::npos) break;
        const size_t cb = raw.find('}', ob);
        if (cb == std::string::npos) break;
        const std::string_view obj(raw.data() + ob, cb - ob + 1);
        CacheEntry e{};
        ipc::JsonFindWide(obj, "key", e.key);
        ipc::JsonFindWide(obj, "video_path", e.video_path);
        ipc::JsonFindWide(obj, "poster_path", e.poster_path);
        ipc::JsonFindWide(obj, "source_path", e.source_path);
        ipc::JsonFindUint64(obj, "size_bytes", e.size_bytes);
        ipc::JsonFindUint64(obj, "last_used_ms", e.last_used_ms);
        if (!e.key.empty()) {
            entries_.push_back(std::move(e));
        }
        pos = cb + 1;
    }
    RefreshEntrySizes();
    return true;
}

bool WallpaperCacheManager::SaveIndex() const {
    std::string out = "{\"version\":1,\"entries\":[";
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (i > 0) out.push_back(',');
        const auto& e = entries_[i];
        out += "{\"key\":" + ipc::JsonQuoteWide(e.key);
        out += ",\"video_path\":" + ipc::JsonQuoteWide(e.video_path);
        out += ",\"poster_path\":" + ipc::JsonQuoteWide(e.poster_path);
        out += ",\"source_path\":" + ipc::JsonQuoteWide(e.source_path);
        out += ",\"size_bytes\":" + std::to_string(e.size_bytes);
        out += ",\"last_used_ms\":" + std::to_string(e.last_used_ms) + "}";
    }
    out += "]}\n";
    return AtomicWriteUtf8(IndexPath(), out);
}

void WallpaperCacheManager::RefreshEntrySizes() {
    std::vector<CacheEntry> kept;
    kept.reserve(entries_.size());
    for (auto& e : entries_) {
        uint64_t vsz = 0, psz = 0;
        const bool has_v = FileExistsNonEmpty(e.video_path, &vsz);
        const bool has_p = FileExistsNonEmpty(e.poster_path, &psz);
        if (!has_v && !has_p) continue;
        if (!has_v) e.video_path.clear();
        if (!has_p) e.poster_path.clear();
        e.size_bytes = vsz + psz;
        kept.push_back(std::move(e));
    }
    entries_ = std::move(kept);
}

std::optional<CacheEntry> WallpaperCacheManager::LookupValidVideo(std::wstring_view key) const {
    for (const auto& e : entries_) {
        if (e.key == key && FileExistsNonEmpty(e.video_path)) {
            return e;
        }
    }
    // Also check filesystem directly in case index wasn't updated yet.
    const std::wstring vpath = VideoFilePathForKey(key);
    uint64_t sz = 0;
    if (FileExistsNonEmpty(vpath, &sz)) {
        CacheEntry e{};
        e.key = std::wstring(key);
        e.video_path = vpath;
        const std::wstring ppath = PosterFilePathForKey(key);
        if (FileExistsNonEmpty(ppath)) e.poster_path = ppath;
        e.size_bytes = sz;
        return e;
    }
    return std::nullopt;
}

std::optional<CacheEntry> WallpaperCacheManager::FindEntry(std::wstring_view key) const {
    for (const auto& e : entries_) {
        if (e.key == key) return e;
    }
    return std::nullopt;
}

bool WallpaperCacheManager::Touch(std::wstring_view key, uint64_t now_ms) {
    for (auto& e : entries_) {
        if (e.key == key) {
            e.last_used_ms = now_ms;
            return SaveIndex();
        }
    }
    return false;
}

bool WallpaperCacheManager::UpsertEntry(const CacheEntry& entry, std::wstring_view protected_active_key) {
    if (entry.key.empty()) return false;
    EnsureDirectory();
    CacheEntry updated = entry;
    uint64_t vsz = 0, psz = 0;
    FileExistsNonEmpty(updated.video_path, &vsz);
    FileExistsNonEmpty(updated.poster_path, &psz);
    if (vsz + psz > 0) updated.size_bytes = vsz + psz;

    bool found = false;
    for (auto& e : entries_) {
        if (e.key == updated.key) {
            e = updated;
            found = true;
            break;
        }
    }
    if (!found) {
        entries_.push_back(std::move(updated));
    }
    EvictToBudget(protected_active_key);
    return SaveIndex();
}

uint64_t WallpaperCacheManager::TotalTrackedBytes() const noexcept {
    uint64_t sum = 0;
    for (const auto& e : entries_) sum += e.size_bytes;
    return sum;
}

size_t WallpaperCacheManager::EvictToBudget(std::wstring_view protected_active_key) {
    if (budget_bytes_ == 0) return 0;
    uint64_t total = TotalTrackedBytes();
    if (total <= budget_bytes_) return 0;

    // Sort indices by oldest last_used_ms first (LRU eviction).
    std::vector<size_t> order(entries_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return entries_[a].last_used_ms < entries_[b].last_used_ms;
    });

    std::vector<bool> remove_flag(entries_.size(), false);
    size_t evicted = 0;
    for (size_t idx : order) {
        if (total <= budget_bytes_) break;
        const auto& e = entries_[idx];
        if (!protected_active_key.empty() && e.key == protected_active_key) continue;
        if (!e.video_path.empty()) DeleteFileW(e.video_path.c_str());
        if (!e.poster_path.empty()) DeleteFileW(e.poster_path.c_str());
        total = (total > e.size_bytes) ? (total - e.size_bytes) : 0;
        remove_flag[idx] = true;
        ++evicted;
    }

    if (evicted > 0) {
        std::vector<CacheEntry> kept;
        kept.reserve(entries_.size() - evicted);
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (!remove_flag[i]) kept.push_back(std::move(entries_[i]));
        }
        entries_ = std::move(kept);
    }
    return evicted;
}

size_t WallpaperCacheManager::CleanupStaleTempFiles() {
    std::error_code ec;
    const fs::path dir(cache_dir_);
    if (!fs::exists(dir, ec)) return 0;
    size_t removed = 0;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file(ec)) continue;
        const std::wstring name = de.path().filename().wstring();
        if (name.ends_with(L".tmp.mp4") || name.ends_with(L".tmp.jpg") || name.ends_with(L".tmp")) {
            if (DeleteFileW(de.path().c_str())) ++removed;
        }
    }
    return removed;
}

void PushRecentVideo(std::vector<VideoWallpaperRecentItem>& recent,
                     const VideoWallpaperRecentItem& item,
                     size_t max_items) {
    if (item.path.empty() || max_items == 0) return;
    const std::wstring key = NormalizePathForCompare(item.path);
    std::erase_if(recent, [&](const VideoWallpaperRecentItem& r) {
        return NormalizePathForCompare(r.path) == key;
    });
    recent.insert(recent.begin(), item);
    if (recent.size() > max_items) {
        recent.resize(max_items);
    }
}

} // namespace pulse::desktop::video
