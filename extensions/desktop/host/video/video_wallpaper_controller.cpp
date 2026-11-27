// video_wallpaper_controller.cpp - End-to-end host controller for video live wallpaper.
#include "host/video/video_wallpaper_controller.h"
#include "host/video/video_visibility.h"
#include "ipc/desktop_ipc.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <shlobj.h>

namespace pulse::desktop::video {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

void VideoLifecycleLog(const char* fmt, ...) noexcept {
    char line[512]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    wchar_t local_app[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_app))) return;
    const fs::path path = fs::path(local_app) / L"Pulse" / L"pulse_desktop_host.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") == 0 && f) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u video %s\r\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
        std::fclose(f);
    }
}

uint64_t NowUnixMs() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::wstring FormatDims(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return {};
    return std::to_wstring(w) + L"\x00D7" + std::to_wstring(h);
}

std::wstring PauseReasonBanner(VideoPauseReason r) {
    switch (r) {
    case VideoPauseReason::Manual:          return L"已暂停";
    case VideoPauseReason::Fullscreen:      return L"全屏程序运行中，已自动暂停";
    case VideoPauseReason::BatterySaver:    return L"节能模式下已自动暂停";
    case VideoPauseReason::SessionLocked:   return L"锁屏期间已自动暂停";
    case VideoPauseReason::DisplayOff:      return L"息屏期间已自动暂停";
    case VideoPauseReason::DesktopOccluded: return L"桌面被完全遮挡，已自动暂停";
    case VideoPauseReason::None:
    default:                                return {};
    }
}

bool FileExistsRegular(const std::wstring& path) noexcept {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

} // namespace

VideoWallpaperController::~VideoWallpaperController() {
    Shutdown();
}

bool VideoWallpaperController::Initialize(HWND notify_hwnd,
                                          UINT notify_msg,
                                          render::DesktopRenderer* renderer,
                                          VideoWallpaperControllerCallbacks callbacks) {
    notify_hwnd_ = notify_hwnd;
    notify_msg_ = notify_msg;
    renderer_ = renderer;
    callbacks_ = std::move(callbacks);
    glass_retry_.Initialize(notify_hwnd_, [this](const PendingGlassFrame& frame) {
        return !glass_frames_suspended_ && frame.generation == active_player_gen_ && renderer_ && persisted_.active &&
               renderer_->OnInternalVideoGlassFrame(frame.shared_handle, frame.width, frame.height,
                                                   overlay_glass_active_);
    }, [this] {
        if (callbacks_.on_live_glass_updated) callbacks_.on_live_glass_updated(false);
    });
    {
        std::lock_guard<std::mutex> lk(ev_mtx_);
        ev_mailbox_.Open();
    }
    cache_.EnsureDirectory();
    cache_.CleanupStaleTempFiles();
    cache_.LoadIndex();
    return true;
}

void VideoWallpaperController::Shutdown() noexcept {
    glass_frames_suspended_ = true;
    glass_retry_.Close();
    {
        std::lock_guard<std::mutex> lk(ev_mtx_);
        ev_mailbox_.Close();
    }
    generation_.fetch_add(1, std::memory_order_acq_rel);
    JoinProbeThread();
    transcoder_.Cancel();
    player_.Shutdown();
    player_started_ = false;
    if (renderer_ && renderer_->IsVideoWallpaperActive()) {
        renderer_->ClearVideoWallpaper();
    }
}

void VideoWallpaperController::JoinProbeThread() noexcept {
    if (probe_thread_.joinable()) {
        probe_thread_.join();
    }
}

void VideoWallpaperController::EnsurePlayerStarted() {
    if (player_started_) return;
    VideoPlayerCallbacks pcb{};
    pcb.on_swap_chain_ready = [this](uint64_t pgen, ComPtr<IDXGISwapChain1> sc,
                                     uint32_t surf_w, uint32_t surf_h, uint32_t /*vw*/, uint32_t /*vh*/) {
        UiEvent ev{};
        ev.kind = UiEvent::Kind::PlayerSwapChainReady;
        ev.generation = pgen;
        ev.swap_chain = std::move(sc);
        ev.u32_a = surf_w;
        ev.u32_b = surf_h;
        PostUiEvent(std::move(ev));
    };
    pcb.on_frozen_frame_ready = [this](uint64_t pgen, FrozenFrameSnapshot snap) {
        UiEvent ev{};
        ev.kind = UiEvent::Kind::PlayerFrozenReady;
        ev.generation = pgen;
        ev.frozen = std::move(snap);
        PostUiEvent(std::move(ev));
    };
    pcb.on_glass_frame_ready = [this](uint64_t pgen, uint64_t seq, HANDLE h, uint32_t gw, uint32_t gh) {
        UiEvent ev{};
        ev.kind = UiEvent::Kind::PlayerGlassFrame;
        ev.generation = pgen;
        ev.sequence = seq;
        ev.shared_handle = h;
        ev.u32_a = gw;
        ev.u32_b = gh;
        PostUiEvent(std::move(ev));
    };
    pcb.on_playback_error = [this](uint64_t pgen, std::wstring err) {
        UiEvent ev{};
        ev.kind = UiEvent::Kind::PlayerError;
        ev.generation = pgen;
        ev.text = std::move(err);
        PostUiEvent(std::move(ev));
    };

    const LUID luid = renderer_ ? renderer_->AdapterLuid() : LUID{};
    ID3D11Device* fallback = renderer_ ? renderer_->D3DDevice() : nullptr;
    player_started_ = player_.Start(luid, std::move(pcb), fallback, false);
    if (player_started_) {
        player_.SetGlassLiveRate(glass_live_rate_);
        player_.SetGlassConsumerActive(true);
    }
}

void VideoWallpaperController::PostUiEvent(UiEvent ev) {
    std::lock_guard<std::mutex> lk(ev_mtx_);
    if (ev.kind == UiEvent::Kind::PlayerGlassFrame) {
        const uint64_t generation = ev.generation;
        const uint64_t sequence = ev.sequence;
        ev_mailbox_.PushFrame(std::move(ev), generation, sequence);
    } else {
        ev_mailbox_.PushControl(std::move(ev));
    }
    RetryUiWakeLocked();
}

void VideoWallpaperController::RetryUiWakeLocked() {
    ev_mailbox_.TryWake([this] {
        return notify_hwnd_ && notify_msg_ != 0 &&
               PostMessageW(notify_hwnd_, notify_msg_, 0, 0) != FALSE;
    });
}

void VideoWallpaperController::RestorePersisted(const VideoWallpaperPersistedState& persisted,
                                                bool icon_takeover_active,
                                                uint32_t target_w,
                                                uint32_t target_h,
                                                uint8_t glass_live_rate) {
    persisted_ = persisted;
    icon_takeover_active_ = icon_takeover_active;
    target_w_ = std::max(320u, target_w);
    target_h_ = std::max(240u, target_h);
    glass_live_rate_ = std::min<uint8_t>(glass_live_rate, 2u);

    if (!persisted_.active || persisted_.source_path.empty()) {
        phase_ = VideoWallpaperPhase::Inactive;
        NotifyUiChanged();
        return;
    }

    if (!icon_takeover_active_) {
        phase_ = VideoWallpaperPhase::RequiresIconTakeover;
        NotifyUiChanged();
        return;
    }

    const bool has_cached = !persisted_.cached_video_path.empty() && FileExistsRegular(persisted_.cached_video_path);
    const bool has_source = FileExistsRegular(persisted_.source_path);

    if (!has_source && !has_cached) {
        phase_ = VideoWallpaperPhase::FileMissing;
        status_text_ = L"找不到这个视频，可能已移动或删除";
        sub_text_.clear();
        store_uri_.clear();
        NotifyUiChanged();
        return;
    }

    if (has_cached && persisted_.optimized) {
        active_source_w_ = persisted_.source_width;
        active_source_h_ = persisted_.source_height;
        active_play_w_ = persisted_.target_width ? persisted_.target_width : target_w_;
        active_play_h_ = persisted_.target_height ? persisted_.target_height : target_h_;
        if (renderer_ && FileExistsRegular(persisted_.poster_path)) {
            uint32_t acc = 0;
            renderer_->SetVideoWallpaperPosterFile(persisted_.poster_path, persisted_.fill_mode, &acc);
            if (acc != 0 && callbacks_.on_accent_extracted) callbacks_.on_accent_extracted(acc);
        }
        StartPlayerOnPath(persisted_.cached_video_path, active_play_w_, active_play_h_, true);
        if (callbacks_.on_video_active_changed) callbacks_.on_video_active_changed();
        NotifyUiChanged();
        return;
    }

    SetVideoWallpaper(persisted_.source_path, icon_takeover_active_, target_w_, target_h_, glass_live_rate_);
}

void VideoWallpaperController::SetVideoWallpaper(const std::wstring& source_path,
                                                 bool icon_takeover_active,
                                                 uint32_t target_w,
                                                 uint32_t target_h,
                                                 uint8_t glass_live_rate) {
    icon_takeover_active_ = icon_takeover_active;
    target_w_ = std::max(320u, target_w);
    target_h_ = std::max(240u, target_h);
    glass_live_rate_ = std::min<uint8_t>(glass_live_rate, 2u);

    if (source_path.empty()) {
        StopVideoWallpaper(true);
        return;
    }

    if (!icon_takeover_active_) {
        phase_ = VideoWallpaperPhase::RequiresIconTakeover;
        NotifyUiChanged();
        return;
    }

    const uint64_t gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    JoinProbeThread();
    has_pending_heavy_ = false;
    tried_source_path_.clear();
    tried_display_name_.clear();
    tried_duration_ms_ = 0;
    store_uri_.clear();

    const std::wstring cache_dir = cache_.CacheDir();
    const uint32_t tw = target_w_;
    const uint32_t th = target_h_;
    const uint8_t gr = glass_live_rate_;

    probe_thread_ = std::thread([this, gen, source_path, tw, th, gr, cache_dir]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        PendingProbeOutcome out{};
        out.generation = gen;
        out.source_path = source_path;
        out.target_w = tw;
        out.target_h = th;
        out.glass_rate = gr;
        VideoProbeJob probe_job([this, gen] {
            return generation_.load(std::memory_order_acquire) != gen;
        });
        out.gpu = probe_job.ProbeCapabilities();
        out.probe = probe_job.ProbeFile(source_path);
        out.policy = EvaluateWallpaperPolicy(out.probe, tw, th, out.gpu);

        if (out.probe.open_ok) {
            CacheKeyInput cki{};
            cki.source_path = source_path;
            cki.file_size_bytes = out.probe.file_size_bytes;
            cki.file_mtime_100ns = out.probe.file_mtime_100ns;
            cki.target_width = out.policy.target_width ? out.policy.target_width : tw;
            cki.target_height = out.policy.target_height ? out.policy.target_height : th;
            cki.codec_pref = CodecChoiceArgName(out.policy.encoder);
            out.cache_key = ComputeWallpaperCacheKey(cki);

            WallpaperCacheManager local_cache(cache_dir);
            local_cache.EnsureDirectory();
            local_cache.LoadIndex();
            if (out.policy.kind == PolicyDecisionKind::TranscodeThenSwitch) {
                if (const auto hit = local_cache.LookupValidVideo(out.cache_key)) {
                    out.cached_hit_path = hit->video_path;
                    if (FileExistsRegular(hit->poster_path)) {
                        out.poster_path = hit->poster_path;
                        out.poster_ok = true;
                    }
                }
            }

            const bool should_extract_poster =
                (out.policy.kind == PolicyDecisionKind::PlayOriginal ||
                 out.policy.kind == PolicyDecisionKind::TranscodeThenSwitch);
            if (should_extract_poster && !out.poster_ok && generation_.load(std::memory_order_acquire) == gen) {
                const std::wstring poster_dst = local_cache.PosterFilePathForKey(out.cache_key);
                uint32_t accent = 0;
                if (probe_job.ExtractPoster(source_path, poster_dst, 1920u, 1080u, &accent)) {
                    out.poster_path = poster_dst;
                    out.accent_argb = accent;
                    out.poster_ok = true;
                }
            }
        }
        probe_job.Reset();
        CoUninitialize();
        if (generation_.load(std::memory_order_acquire) != gen) return;

        UiEvent ev{};
        ev.kind = UiEvent::Kind::ProbeDone;
        ev.generation = gen;
        ev.probe = std::move(out);
        PostUiEvent(std::move(ev));
    });
}

void VideoWallpaperController::StopVideoWallpaper(bool persist_inactive) {
    glass_frames_suspended_ = true;
    glass_retry_.Clear();
    generation_.fetch_add(1, std::memory_order_acq_rel);
    JoinProbeThread();
    transcoder_.Cancel();
    active_player_gen_ = 0;
    if (player_started_) {
        player_.CloseVideo();
    }
    if (renderer_) {
        renderer_->ClearVideoWallpaper();
    }
    const bool was_active = persisted_.active;
    if (persist_inactive) {
        persisted_.active = false;
    }
    phase_ = VideoWallpaperPhase::Inactive;
    active_pause_reason_ = VideoPauseReason::None;
    user_paused_ = false;
    playing_original_while_transcoding_ = false;
    optimize_progress_ = 0.0f;
    has_pending_heavy_ = false;
    status_text_.clear();
    sub_text_.clear();
    store_uri_.clear();
    tried_source_path_.clear();
    tried_display_name_.clear();
    tried_duration_ms_ = 0;
    pause_sm_.Reset();

    if (was_active && callbacks_.on_video_active_changed) {
        callbacks_.on_video_active_changed();
    }
    if (persist_inactive) {
        NotifyPersistedChanged();
    }
    NotifyUiChanged();
}

void VideoWallpaperController::HandleProbeOutcome(const PendingProbeOutcome& out) {
    if (out.generation != generation_.load(std::memory_order_acquire)) return;

    const std::wstring disp_name = VideoDisplayName(out.source_path);
    const uint32_t dur_ms = out.probe.DurationMs();

    auto keep_previous_with_error = [&](VideoWallpaperPhase err_phase,
                                        std::wstring msg,
                                        std::wstring uri = {}) {
        tried_source_path_ = out.source_path;
        tried_display_name_ = disp_name;
        tried_duration_ms_ = dur_ms;
        phase_ = err_phase;
        status_text_ = std::move(msg);
        store_uri_ = std::move(uri);
        if (persisted_.active && !persisted_.source_path.empty()) {
            sub_text_ = L"壁纸仍是 " + VideoStemName(persisted_.source_path);
        } else {
            sub_text_.clear();
        }
        NotifyUiChanged();
    };

    switch (out.policy.kind) {
    case PolicyDecisionKind::RejectMissingFile:
        if (persisted_.active && NormalizePathForCompare(persisted_.source_path) == NormalizePathForCompare(out.source_path)) {
            phase_ = VideoWallpaperPhase::FileMissing;
            status_text_ = L"找不到这个视频，可能已移动或删除";
            sub_text_.clear();
            NotifyUiChanged();
        } else {
            keep_previous_with_error(VideoWallpaperPhase::FileMissing, L"找不到这个视频，可能已移动或删除");
        }
        return;
    case PolicyDecisionKind::RejectHdr:
        keep_previous_with_error(VideoWallpaperPhase::UnsupportedHdr, L"暂不支持 HDR 视频");
        return;
    case PolicyDecisionKind::RejectMissingHevc:
        keep_previous_with_error(VideoWallpaperPhase::MissingCodecHevc,
                                 L"需要 HEVC 视频扩展才能播放这个视频",
                                 out.policy.store_uri);
        return;
    case PolicyDecisionKind::RejectMissingAv1:
        keep_previous_with_error(VideoWallpaperPhase::MissingCodecAv1,
                                 L"需要 AV1 视频扩展才能播放这个视频",
                                 out.policy.store_uri);
        return;
    case PolicyDecisionKind::RejectTooLarge:
        keep_previous_with_error(VideoWallpaperPhase::TooLargeToTranscode, L"这个视频太大，这台电脑转换不了");
        return;
    case PolicyDecisionKind::RejectUnsupportedCodec:
        keep_previous_with_error(VideoWallpaperPhase::UnsupportedCodec,
                                 out.policy.user_message.empty() ? L"无法播放这个视频格式" : out.policy.user_message);
        return;
    case PolicyDecisionKind::PlayOriginal:
    case PolicyDecisionKind::TranscodeThenSwitch:
        break;
    }

    // Valid video — cancel any prior transcode and update active state.
    transcoder_.Cancel();
    const bool was_active = persisted_.active;
    const uint32_t src_w = out.probe.display_width ? out.probe.display_width : out.probe.coded_width;
    const uint32_t src_h = out.probe.display_height ? out.probe.display_height : out.probe.coded_height;

    persisted_.active = true;
    persisted_.source_path = out.source_path;
    persisted_.cache_key = out.cache_key;
    persisted_.source_width = src_w;
    persisted_.source_height = src_h;
    persisted_.duration_ms = dur_ms;
    if (out.poster_ok) {
        persisted_.poster_path = out.poster_path;
    }

    VideoWallpaperRecentItem rec{};
    rec.path = out.source_path;
    rec.poster_path = out.poster_path;
    rec.width = src_w;
    rec.height = src_h;
    rec.duration_ms = dur_ms;
    PushRecentVideo(persisted_.recent, rec, 3u);

    active_source_w_ = src_w;
    active_source_h_ = src_h;
    user_paused_ = false;
    pause_sm_.Reset();

    // Immediately show poster frame so transition has zero black flash.
    if (renderer_ && out.poster_ok && FileExistsRegular(out.poster_path)) {
        uint32_t acc = out.accent_argb;
        renderer_->SetVideoWallpaperPosterFile(out.poster_path, persisted_.fill_mode, acc == 0 ? &acc : nullptr);
        if (callbacks_.on_live_glass_updated) {
            callbacks_.on_live_glass_updated(true);
        }
        if (acc != 0 && callbacks_.on_accent_extracted) {
            callbacks_.on_accent_extracted(acc);
        }
    } else if (out.accent_argb != 0 && callbacks_.on_accent_extracted) {
        callbacks_.on_accent_extracted(out.accent_argb);
    }

    if (out.policy.kind == PolicyDecisionKind::PlayOriginal) {
        persisted_.optimized = false;
        persisted_.cached_video_path.clear();
        persisted_.target_width = src_w;
        persisted_.target_height = src_h;
        active_play_w_ = src_w;
        active_play_h_ = src_h;
        playing_original_while_transcoding_ = false;
        StartPlayerOnPath(out.source_path, target_w_, target_h_, false);
    } else if (!out.cached_hit_path.empty() && FileExistsRegular(out.cached_hit_path)) {
        persisted_.optimized = true;
        persisted_.cached_video_path = out.cached_hit_path;
        persisted_.target_width = out.policy.target_width;
        persisted_.target_height = out.policy.target_height;
        active_play_w_ = out.policy.target_width;
        active_play_h_ = out.policy.target_height;
        playing_original_while_transcoding_ = false;
        cache_.Touch(out.cache_key, NowUnixMs());
        cache_.SaveIndex();
        StartPlayerOnPath(out.cached_hit_path, active_play_w_, active_play_h_, true);
    } else {
        PendingTranscodeJob job{};
        job.source_path = out.source_path;
        job.cache_key = out.cache_key;
        job.temp_output_path = cache_.TempVideoFilePathForKey(out.cache_key);
        job.final_output_path = cache_.VideoFilePathForKey(out.cache_key);
        job.target_w = out.policy.target_width;
        job.target_h = out.policy.target_height;
        job.source_w = src_w;
        job.source_h = src_h;
        job.duration_ms = dur_ms;
        job.policy = out.policy;

        persisted_.optimized = false;
        persisted_.cached_video_path.clear();
        persisted_.target_width = job.target_w;
        persisted_.target_height = job.target_h;

        if (out.policy.require_heavy_confirm) {
            if (player_started_) player_.CloseVideo();
            playing_original_while_transcoding_ = false;
            has_pending_heavy_ = true;
            pending_heavy_job_ = job;
            phase_ = VideoWallpaperPhase::ConfirmHeavyTranscode;
            wchar_t msg[256]{};
            const double gb = static_cast<double>(out.policy.estimated_transcode_ram_bytes) / (1024.0 * 1024.0 * 1024.0);
            std::swprintf(msg, ARRAYSIZE(msg),
                          L"这个视频需要软件优化，预计临时占用约 %.1f GB 内存", std::max(1.0, gb));
            status_text_ = msg;
            sub_text_ = L"原片 " + FormatDims(src_w, src_h) + L"，优化好之前先显示第一帧";
        } else {
            playing_original_while_transcoding_ = out.policy.play_original_while_transcoding;
            if (playing_original_while_transcoding_) {
                StartPlayerOnPath(out.source_path, target_w_, target_h_, false);
            } else if (player_started_) {
                player_.CloseVideo();
            }
            StartTranscodeJob(job);
        }
    }

    if (!was_active && callbacks_.on_video_active_changed) {
        callbacks_.on_video_active_changed();
    }
    NotifyPersistedChanged();
    NotifyUiChanged();
}

void VideoWallpaperController::StartPlayerOnPath(const std::wstring& play_path,
                                                 uint32_t cap_w,
                                                 uint32_t cap_h,
                                                 bool is_optimized) {
    glass_frames_suspended_ = true;
    glass_retry_.Clear();
    EnsurePlayerStarted();
    if (!player_started_) {
        phase_ = VideoWallpaperPhase::Error;
        status_text_ = L"无法启动视频播放";
        return;
    }
    const bool start_paused = ShouldStartVideoPaused(user_paused_, pause_sm_.State());
    active_player_gen_ = player_.OpenVideo(play_path, std::max(320u, cap_w), std::max(240u, cap_h), start_paused, 0);
    if (pause_sm_.State() == PauseLifecycleState::FrozenReleased) player_.FreezeAndRelease();
    if (!transcoder_.IsActive() && !start_paused) {
        phase_ = is_optimized ? VideoWallpaperPhase::PlayingOptimized : VideoWallpaperPhase::PlayingOriginal;
        if (is_optimized && active_source_w_ > 0 && active_play_w_ > 0) {
            status_text_ = L"已为这块屏幕优化成 " + FormatDims(active_play_w_, active_play_h_) +
                           L"（原片 " + FormatDims(active_source_w_, active_source_h_) + L"）";
        } else if (active_source_w_ > 0) {
            status_text_ = FormatDims(active_source_w_, active_source_h_) + L" · 正在播放";
        } else {
            status_text_ = L"正在播放";
        }
        sub_text_.clear();
    }
}

void VideoWallpaperController::StartTranscodeJob(const PendingTranscodeJob& job) {
    active_transcode_job_ = job;
    has_pending_heavy_ = false;
    optimize_progress_ = 0.0f;
    phase_ = VideoWallpaperPhase::Optimizing;
    status_text_ = L"正在为这块屏幕优化… 0%";
    sub_text_ = L"原片 " + FormatDims(job.source_w, job.source_h) +
                (playing_original_while_transcoding_
                     ? L"，优化完成后将自动切换"
                     : L"，优化好之前先显示第一帧");

    const uint64_t gen = generation_.load(std::memory_order_acquire);

    TranscodeJobSpec spec{};
    spec.job_id = next_job_id_++;
    spec.source_path = job.source_path;
    spec.cache_key = job.cache_key;
    spec.temp_output_path = job.temp_output_path;
    spec.final_output_path = job.final_output_path;
    spec.target_width = job.target_w;
    spec.target_height = job.target_h;
    spec.codec = job.policy.encoder;
    spec.memory_limit_bytes = job.policy.job_memory_limit_bytes;

    TranscodeCallbacks tcb{};
    tcb.on_progress = [this, gen](uint64_t /*job_id*/, float prog) {
        UiEvent ev{};
        ev.kind = UiEvent::Kind::TranscodeProgress;
        ev.generation = gen;
        ev.progress = prog;
        PostUiEvent(std::move(ev));
    };
    tcb.on_finished = [this, gen](uint64_t /*job_id*/, TranscodeOutcome outcome, const std::wstring& final_path) {
        UiEvent ev{};
        ev.kind = UiEvent::Kind::TranscodeFinished;
        ev.generation = gen;
        ev.transcode_outcome = outcome;
        ev.text = final_path;
        PostUiEvent(std::move(ev));
    };

    if (!transcoder_.StartJob(spec, std::move(tcb))) {
        phase_ = VideoWallpaperPhase::Error;
        status_text_ = L"无法启动后台视频优化";
        sub_text_.clear();
    }
    NotifyUiChanged();
}

void VideoWallpaperController::ConfirmHeavyOptimize() {
    if (!has_pending_heavy_) return;
    const PendingTranscodeJob job = pending_heavy_job_;
    StartTranscodeJob(job);
}

void VideoWallpaperController::CancelOptimize() {
    if (has_pending_heavy_) {
        has_pending_heavy_ = false;
        StopVideoWallpaper(true);
        return;
    }
    if (transcoder_.IsActive()) {
        transcoder_.Cancel();
        if (playing_original_while_transcoding_ && player_started_ && player_.Stats().open) {
            playing_original_while_transcoding_ = false;
            phase_ = VideoWallpaperPhase::PlayingOriginal;
            status_text_ = FormatDims(active_source_w_, active_source_h_) + L" · 正在播放";
            sub_text_.clear();
            NotifyUiChanged();
        } else {
            StopVideoWallpaper(true);
        }
    }
}

void VideoWallpaperController::ToggleManualPause() {
    if (!persisted_.active) return;
    user_paused_ = !user_paused_;
    // Removing the manual reason must not override a still-active automatic
    // reason or reset the continuous 30-second freeze deadline.
    TickSecond(last_auto_sleep_enabled_, last_desktop_hwnd_, last_overlay_hwnd_, false);
}

void VideoWallpaperController::SetFillMode(VideoFillMode fill) {
    if (persisted_.fill_mode == fill) return;
    persisted_.fill_mode = fill;
    if (renderer_ && renderer_->IsVideoWallpaperActive()) {
        renderer_->UpdateVideoWallpaperFillMode(fill);
    }
    NotifyPersistedChanged();
    NotifyUiChanged();
}

void VideoWallpaperController::SetGlassLiveRate(uint8_t rate_mode) {
    glass_live_rate_ = std::min<uint8_t>(rate_mode, 2u);
    if (player_started_) {
        player_.SetGlassLiveRate(glass_live_rate_);
    }
}

void VideoWallpaperController::SetGlassConsumers(bool desktop_glass_active, bool overlay_active) noexcept {
    const bool prev_ov = overlay_glass_active_;
    overlay_glass_active_ = overlay_active;
    if (player_started_) {
        player_.SetGlassConsumerActive(desktop_glass_active || overlay_active);
        player_.SetPreviewConsumerActive(overlay_active);
        if (overlay_active && !prev_ov && persisted_.active) {
            player_.RequestGlassRefresh();
        }
    }
}

void VideoWallpaperController::SetIconTakeoverActive(bool active) {
    const bool prev = icon_takeover_active_;
    icon_takeover_active_ = active;
    if (active && !prev) {
        if (persisted_.active && !persisted_.source_path.empty()) {
            RestorePersisted(persisted_, true, target_w_, target_h_, glass_live_rate_);
            return;
        }
        if (phase_ == VideoWallpaperPhase::RequiresIconTakeover) {
            phase_ = VideoWallpaperPhase::Inactive;
        }
        NotifyUiChanged();
    } else if (!active && prev) {
        if (player_started_) player_.CloseVideo();
        if (renderer_) renderer_->ClearVideoWallpaper();
        phase_ = VideoWallpaperPhase::RequiresIconTakeover;
        NotifyUiChanged();
    }
}

void VideoWallpaperController::OnDisplayOffChanged(bool display_off) {
    display_off_ = display_off;
}

void VideoWallpaperController::OnSessionLockChanged(bool locked) {
    session_locked_ = locked;
}

void VideoWallpaperController::OnMonitorGeometryChanged(uint32_t target_w, uint32_t target_h) {
    target_w_ = std::max(320u, target_w);
    target_h_ = std::max(240u, target_h);
}

void VideoWallpaperController::DrainUiEvents() {
    std::deque<UiEvent> batch;
    {
        std::lock_guard<std::mutex> lk(ev_mtx_);
        batch = ev_mailbox_.TakeBatch();
    }
    const uint64_t cur_gen = generation_.load(std::memory_order_acquire);

    for (auto& ev : batch) {
        switch (ev.kind) {
        case UiEvent::Kind::ProbeDone:
            if (ev.generation == cur_gen) HandleProbeOutcome(ev.probe);
            break;
        case UiEvent::Kind::PlayerSwapChainReady:
            if (ev.generation == active_player_gen_ && ev.swap_chain && renderer_ && persisted_.active) {
                glass_frames_suspended_ = false;
                error_recovery_attempts_ = 0;
                next_error_recovery_ms_ = 0;
                renderer_->SetVideoWallpaperSwapChain(ev.swap_chain.Get(), ev.u32_a, ev.u32_b, persisted_.fill_mode);
                active_play_w_ = ev.u32_a;
                active_play_h_ = ev.u32_b;
                glass_retry_.RetryNow();
                VideoLifecycleLog("swap_chain_ready gen=%llu size=%ux%u",
                                  static_cast<unsigned long long>(ev.generation), ev.u32_a, ev.u32_b);
            }
            break;
        case UiEvent::Kind::PlayerFrozenReady:
            if (ev.generation == active_player_gen_) {
                glass_frames_suspended_ = true;
                glass_retry_.Clear();
            }
            if (ev.generation == active_player_gen_ && renderer_ && persisted_.active && !ev.frozen.bgra.empty()) {
                renderer_->SetVideoWallpaperFrozenBgra(ev.frozen.bgra.data(), ev.frozen.width, ev.frozen.height,
                                                       ev.frozen.stride, persisted_.fill_mode);
                renderer_->TrimDxgiMemory();
                if (player_started_) player_.TrimDeviceMemory();
                if (callbacks_.on_live_glass_updated) callbacks_.on_live_glass_updated(true);
            }
            break;
        case UiEvent::Kind::PlayerGlassFrame:
            if (ev.generation == active_player_gen_ && renderer_ && persisted_.active && ev.shared_handle) {
                // The mailbox supplies at most one frame, in notification order
                // relative to freeze/swap/error controls, without holding ev_mtx_.
                const PendingGlassFrame frame{ev.generation, ev.shared_handle, ev.u32_a, ev.u32_b};
                // OpenPipeline publishes its first glass frame before the new
                // swap chain callback, including Static mode after frozen resume.
                if (glass_frames_suspended_) glass_retry_.Stage(frame);
                else glass_retry_.Submit(frame);
            }
            break;
        case UiEvent::Kind::PlayerError:
            if (ev.generation == active_player_gen_ && persisted_.active) {
                phase_ = VideoWallpaperPhase::Error;
                status_text_ = ev.text.empty() ? L"视频播放出现错误" : ev.text;
                const uint64_t backoff_ms = 2000ull << std::min<uint32_t>(error_recovery_attempts_, 2u);
                next_error_recovery_ms_ = GetTickCount64() + backoff_ms;
                VideoLifecycleLog("player_error gen=%llu attempt=%u err=%s",
                                  static_cast<unsigned long long>(ev.generation),
                                  error_recovery_attempts_,
                                  ipc::WideToUtf8(status_text_).c_str());
                NotifyUiChanged();
            }
            break;
        case UiEvent::Kind::TranscodeProgress:
            if (ev.generation == cur_gen && persisted_.active) {
                optimize_progress_ = std::clamp(ev.progress, 0.0f, 1.0f);
                const int pct = std::clamp(static_cast<int>(std::lround(optimize_progress_ * 100.0f)), 0, 100);
                status_text_ = L"正在为这块屏幕优化… " + std::to_wstring(pct) + L"%";
                NotifyUiChanged();
            }
            break;
        case UiEvent::Kind::TranscodeFinished:
            if (ev.generation == cur_gen && persisted_.active) {
                if (ev.transcode_outcome == TranscodeOutcome::Succeeded && !ev.text.empty()) {
                    playing_original_while_transcoding_ = false;
                    optimize_progress_ = 1.0f;
                    persisted_.optimized = true;
                    persisted_.cached_video_path = ev.text;
                    persisted_.target_width = active_transcode_job_.target_w;
                    persisted_.target_height = active_transcode_job_.target_h;
                    active_play_w_ = active_transcode_job_.target_w;
                    active_play_h_ = active_transcode_job_.target_h;

                    CacheEntry ce{};
                    ce.key = active_transcode_job_.cache_key;
                    ce.source_path = active_transcode_job_.source_path;
                    ce.video_path = ev.text;
                    ce.poster_path = persisted_.poster_path;
                    ce.last_used_ms = NowUnixMs();
                    std::error_code ec;
                    ce.size_bytes = fs::file_size(ev.text, ec);
                    cache_.UpsertEntry(ce, ce.key);
                    cache_.SaveIndex();

                    StartPlayerOnPath(ev.text, active_play_w_, active_play_h_, true);
                    NotifyPersistedChanged();
                    NotifyUiChanged();
                } else if (ev.transcode_outcome != TranscodeOutcome::Cancelled) {
                    if (playing_original_while_transcoding_ && player_started_ && player_.Stats().open) {
                        playing_original_while_transcoding_ = false;
                        phase_ = VideoWallpaperPhase::PlayingOriginal;
                        status_text_ = FormatDims(active_source_w_, active_source_h_) + L" · 正在播放";
                        sub_text_.clear();
                    } else if (ev.transcode_outcome == TranscodeOutcome::OutOfMemory) {
                        phase_ = VideoWallpaperPhase::TooLargeToTranscode;
                        status_text_ = L"这个视频太大，这台电脑转换不了";
                        sub_text_.clear();
                    } else {
                        phase_ = VideoWallpaperPhase::Error;
                        status_text_ = L"后台优化失败";
                        sub_text_.clear();
                    }
                    NotifyUiChanged();
                }
            }
            break;
        }
    }
}

void VideoWallpaperController::ApplyPauseStep(const PauseStepResult& tr) {
    const bool reason_changed = active_pause_reason_ != tr.reason;
    active_pause_reason_ = tr.reason;

    if (tr.entered_pause || (tr.state != PauseLifecycleState::Playing && reason_changed)) {
        if (tr.entered_pause && player_started_) player_.SetPaused(true);
        const HWND fg = GetForegroundWindow();
        wchar_t fg_cls[64]{};
        if (fg) GetClassNameW(fg, fg_cls, ARRAYSIZE(fg_cls));
        const LONG_PTR fg_ex = fg ? GetWindowLongPtrW(fg, GWL_EXSTYLE) : 0;
        QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
        (void)SHQueryUserNotificationState(&quns);
        VideoLifecycleLog("pause state=%u reason=%u quns=%d fg_cls=%s fg_ex=0x%llX",
                          static_cast<unsigned>(tr.state), static_cast<unsigned>(tr.reason),
                          static_cast<int>(quns), ipc::WideToUtf8(fg_cls).c_str(),
                          static_cast<unsigned long long>(fg_ex));
        if (tr.reason == VideoPauseReason::Manual) {
            phase_ = VideoWallpaperPhase::PausedManual;
            status_text_ = L"已暂停";
            sub_text_.clear();
        } else {
            phase_ = VideoWallpaperPhase::PausedAuto;
            status_text_ = PauseReasonBanner(tr.reason);
            sub_text_ = (tr.reason == VideoPauseReason::Fullscreen || tr.reason == VideoPauseReason::BatterySaver)
                            ? L"“全屏或节能时暂停”在插件设置里"
                            : L"";
        }
        NotifyUiChanged();
    }
    if (tr.should_freeze_release) {
        VideoLifecycleLog("freeze_release reason=%u", static_cast<unsigned>(tr.reason));
        glass_frames_suspended_ = true;
        glass_retry_.Clear();
        if (player_started_) player_.FreezeAndRelease();
    } else if (tr.resumed_from_hot) {
        VideoLifecycleLog("resume_from_hot");
        if (player_started_) player_.SetPaused(false);
        if (transcoder_.IsActive()) {
            phase_ = VideoWallpaperPhase::Optimizing;
            const int pct = std::clamp(static_cast<int>(std::lround(optimize_progress_ * 100.0f)), 0, 100);
            status_text_ = L"正在为这块屏幕优化… " + std::to_wstring(pct) + L"%";
        } else {
            phase_ = persisted_.optimized ? VideoWallpaperPhase::PlayingOptimized : VideoWallpaperPhase::PlayingOriginal;
            if (persisted_.optimized && active_source_w_ > 0 && active_play_w_ > 0) {
                status_text_ = L"已为这块屏幕优化成 " + FormatDims(active_play_w_, active_play_h_) +
                               L"（原片 " + FormatDims(active_source_w_, active_source_h_) + L"）";
            } else {
                status_text_ = FormatDims(active_source_w_, active_source_h_) + L" · 正在播放";
            }
            sub_text_.clear();
        }
        NotifyUiChanged();
    } else if (tr.resumed_from_frozen) {
        VideoLifecycleLog("resume_from_frozen");
        if (player_started_) player_.ResumeFromFrozen();
        if (transcoder_.IsActive()) {
            phase_ = VideoWallpaperPhase::Optimizing;
        } else {
            phase_ = persisted_.optimized ? VideoWallpaperPhase::PlayingOptimized : VideoWallpaperPhase::PlayingOriginal;
            if (persisted_.optimized && active_source_w_ > 0 && active_play_w_ > 0) {
                status_text_ = L"已为这块屏幕优化成 " + FormatDims(active_play_w_, active_play_h_) +
                               L"（原片 " + FormatDims(active_source_w_, active_source_h_) + L"）";
            } else {
                status_text_ = FormatDims(active_source_w_, active_source_h_) + L" · 正在播放";
            }
            sub_text_.clear();
        }
        NotifyUiChanged();
    }
}

void VideoWallpaperController::TickSecond(bool auto_sleep_enabled,
                                          HWND desktop_hwnd,
                                          HWND overlay_hwnd,
                                          bool drawer_open_on_wall_tab) {
    last_auto_sleep_enabled_ = auto_sleep_enabled;
    last_desktop_hwnd_ = desktop_hwnd;
    last_overlay_hwnd_ = overlay_hwnd;
    glass_retry_.EnsureTimer();
    {
        // Recover a failed PostMessage even if no producer sends another event.
        std::lock_guard<std::mutex> lk(ev_mtx_);
        RetryUiWakeLocked();
    }
    if (!persisted_.active || !icon_takeover_active_) return;
    if (phase_ == VideoWallpaperPhase::FileMissing ||
        phase_ == VideoWallpaperPhase::ConfirmHeavyTranscode) {
        return;
    }

    const HWND fg = GetForegroundWindow();
    const bool fg_is_desktop = IsForegroundDesktopShell(fg, desktop_hwnd, overlay_hwnd, GetCurrentProcessId());
    const VideoPauseReason sys_r = render::DesktopRenderer::QuerySystemPauseReason(auto_sleep_enabled, fg_is_desktop);
    const PauseSignals sig = MakeVisibilityPauseSignals(user_paused_, session_locked_, display_off_,
        auto_sleep_enabled, sys_r,
        auto_sleep_enabled && IsWindowCoveringDesktop(fg, desktop_hwnd, overlay_hwnd, GetCurrentProcessId()));

    // Bounded automatic self-recovery if a transient D3D11/DXGI/VRAM error stopped playback
    // (e.g. reopening under 3 GB VRAM pressure after returning from maximized AutoCAD).
    if (phase_ == VideoWallpaperPhase::Error) {
        const uint64_t now_tick = GetTickCount64();
        if (!sig.AnyPaused() && error_recovery_attempts_ < 3u && now_tick >= next_error_recovery_ms_) {
            const std::wstring play_path = (persisted_.optimized && FileExistsRegular(persisted_.cached_video_path))
                ? persisted_.cached_video_path
                : persisted_.source_path;
            if (FileExistsRegular(play_path)) {
                ++error_recovery_attempts_;
                next_error_recovery_ms_ = now_tick + (2000ull << std::min<uint32_t>(error_recovery_attempts_, 2u));
                VideoLifecycleLog("auto_recover attempt=%u", error_recovery_attempts_);
                const uint32_t cw = active_play_w_ ? active_play_w_ : target_w_;
                const uint32_t ch = active_play_h_ ? active_play_h_ : target_h_;
                StartPlayerOnPath(play_path, cw, ch, persisted_.optimized && play_path == persisted_.cached_video_path);
                NotifyUiChanged();
            }
        }
        return;
    }

    // Use all system signals, not just the highest-priority banner reason:
    // manual pause may coexist with fullscreen, occlusion or display-off.
    transcoder_.SetPaused(ShouldPauseVideoTranscoder(sig));

    const PauseStepResult tr = pause_sm_.Step(sig, GetTickCount64());
    if (tr.entered_pause || tr.should_freeze_release || tr.resumed_from_hot || tr.resumed_from_frozen ||
        tr.reason != active_pause_reason_) {
        ApplyPauseStep(tr);
    } else if (!sig.AnyPaused() && pause_sm_.State() == PauseLifecycleState::Playing &&
               player_started_ && !transcoder_.IsActive() && renderer_ && !renderer_->IsVideoWallpaperActive()) {
        // Recover if the renderer recreated its DComp target after a device reset.
        const std::wstring play_path = (persisted_.optimized && FileExistsRegular(persisted_.cached_video_path))
            ? persisted_.cached_video_path
            : persisted_.source_path;
        if (FileExistsRegular(play_path)) {
            VideoLifecycleLog("rebind_after_renderer_reset");
            StartPlayerOnPath(play_path, active_play_w_ ? active_play_w_ : target_w_,
                              active_play_h_ ? active_play_h_ : target_h_,
                              persisted_.optimized && play_path == persisted_.cached_video_path);
        }
    }

    // Note: BeautifyDrawer preview & Liquid Glass backdrop now consume the zero-copy shared
    // GPU texture directly via on_live_glass_updated(), avoiding 1Hz CPU staging readbacks.
    (void)drawer_open_on_wall_tab;
}

VideoWallpaperUiState VideoWallpaperController::BuildUiState() const {
    VideoWallpaperUiState ui{};
    ui.icon_takeover_active = icon_takeover_active_;
    ui.phase = icon_takeover_active_
                   ? phase_
                   : (persisted_.active ? VideoWallpaperPhase::RequiresIconTakeover : VideoWallpaperPhase::Inactive);
    ui.pause_reason = active_pause_reason_;
    ui.fill_mode = persisted_.fill_mode;
    ui.user_paused = user_paused_;
    ui.playing_original_while_transcoding = playing_original_while_transcoding_;
    ui.optimize_progress = optimize_progress_;
    ui.active_source_path = persisted_.active ? persisted_.source_path : L"";
    ui.active_display_name = persisted_.active ? VideoDisplayName(persisted_.source_path) : L"";
    ui.active_poster_path = persisted_.active ? persisted_.poster_path : L"";
    ui.source_w = persisted_.source_width;
    ui.source_h = persisted_.source_height;
    ui.target_w = persisted_.target_width;
    ui.target_h = persisted_.target_height;
    ui.duration_ms = persisted_.duration_ms;
    ui.tried_source_path = tried_source_path_;
    ui.tried_display_name = tried_display_name_;
    ui.tried_duration_ms = tried_duration_ms_;
    ui.status_text = status_text_;
    ui.sub_text = sub_text_;
    ui.store_uri = store_uri_;
    ui.recent = persisted_.recent;
    return ui;
}

std::string VideoWallpaperController::StatusSummaryUtf8() const {
    if (!persisted_.active) return "未选择视频 · 支持 MP4、MOV、MKV";
    if (!status_text_.empty()) return ipc::WideToUtf8(status_text_);
    return "正在播放";
}

std::string VideoWallpaperController::DisplayNameUtf8() const {
    if (!persisted_.active || persisted_.source_path.empty()) return {};
    return ipc::WideToUtf8(VideoDisplayName(persisted_.source_path));
}

void VideoWallpaperController::NotifyUiChanged() {
    if (callbacks_.on_ui_state_changed) {
        callbacks_.on_ui_state_changed(BuildUiState());
    }
}

void VideoWallpaperController::NotifyPersistedChanged() {
    if (callbacks_.on_persisted_changed) {
        callbacks_.on_persisted_changed(persisted_);
    }
}

std::wstring VideoWallpaperController::PickVideoFileDialog(HWND owner) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        return {};
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"选择视频壁纸");
    static constexpr COMDLG_FILTERSPEC kSpecs[] = {
        {L"视频文件 (*.mp4;*.mov;*.mkv;*.m4v;*.webm;*.wmv)", L"*.mp4;*.mov;*.mkv;*.m4v;*.webm;*.wmv"},
        {L"所有文件 (*.*)", L"*.*"},
    };
    dlg->SetFileTypes(ARRAYSIZE(kSpecs), kSpecs);
    dlg->SetFileTypeIndex(1);

    if (FAILED(dlg->Show(owner))) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item)) || !item) return {};
    PWSTR raw_path = nullptr;
    std::wstring result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) && raw_path) {
        result = raw_path;
        CoTaskMemFree(raw_path);
    }
    return result;
}

bool VideoWallpaperController::IsDesktopOccludedByForeground(HWND desktop_hwnd, HWND overlay_hwnd) noexcept {
    return IsWindowCoveringDesktop(GetForegroundWindow(), desktop_hwnd, overlay_hwnd, GetCurrentProcessId());
}

} // namespace pulse::desktop::video
