#include "../common/volume_path.h"
// Keep MF header feature gates consistent with the Windows 8.1 app target.
// The project NTDDI value is newer than WINVER in current Windows SDK headers.
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x06030000
#include "video_preview.h"
#include "../common/runtime_log.h"
#include "../common/path_utils.h"
#include "../common/preview_packs.h"
#include "ffmpeg_playback.h"
#include "../common/preview_extensions.h"
#include <mfplay.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#pragma comment(lib, "mfuuid.lib")

namespace pulse::ui {
using Microsoft::WRL::ComPtr;
namespace {
constexpr wchar_t kVideoClass[] = L"Pulse.VideoPreview";
constexpr UINT kRetireVideo = WM_APP + 87;

struct Event {
    MFP_EVENT_TYPE type{};
    HRESULT error = S_OK;
    ComPtr<IMFPMediaItem> item;
};

// Callback never touches the player, a window, or QuickPreviewWindow.
class Events final : public IMFPMediaPlayerCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMFPMediaPlayerCallback))
            return E_NOINTERFACE;
        *out = static_cast<IMFPMediaPlayerCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* header) override {
        if (!header) return;
        Event event{header->eEventType, header->hrEvent, {}};
        if (header->eEventType == MFP_EVENT_TYPE_MEDIAITEM_CREATED && SUCCEEDED(header->hrEvent))
            event.item = reinterpret_cast<MFP_MEDIAITEM_CREATED_EVENT*>(header)->pMediaItem;
        std::lock_guard lock(mutex_);
        if (events_.size() < 64) events_.push_back(std::move(event));
    }
    std::deque<Event> Take() {
        std::lock_guard lock(mutex_);
        return std::exchange(events_, {});
    }
private:
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    std::deque<Event> events_;
};

int64_t Position(IMFPMediaPlayer* player, bool duration) {
    PROPVARIANT value{};
    const HRESULT hr = duration ? player->GetDuration(MFP_POSITIONTYPE_100NS, &value)
                                : player->GetPosition(MFP_POSITIONTYPE_100NS, &value);
    int64_t result = 0;
    if (SUCCEEDED(hr)) {
        if (value.vt == VT_I8) result = value.hVal.QuadPart;
        else if (value.vt == VT_UI8) result = static_cast<int64_t>(value.uhVal.QuadPart);
    }
    PropVariantClear(&value);
    return (std::max)(int64_t{0}, result);
}
struct CodecInfo {
    std::wstring short_name, name;
    const wchar_t* store = nullptr;
};

CodecInfo DescribeCodec(const GUID& subtype) {
    // FourCC subtypes share the {XXXXXXXX-0000-0010-8000-00AA00389B71} base.
    static constexpr BYTE kBase4[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    static constexpr GUID kMpeg2 = {0xe06d8026, 0xdb46, 0x11cf, {0xb4, 0xd1, 0x00, 0x80, 0x5f, 0x6c, 0xbb, 0xea}};
    if (subtype == kMpeg2) return {L"MPEG-2", L"MPEG-2", nullptr};
    if (subtype.Data2 != 0x0000 || subtype.Data3 != 0x0010 ||
        memcmp(subtype.Data4, kBase4, sizeof(kBase4)) != 0)
        return {L"?", L"?", nullptr};
    std::wstring fourcc;
    for (int i = 0; i < 4; ++i) {
        const wchar_t c = static_cast<wchar_t>((subtype.Data1 >> (8 * i)) & 0xFF);
        if (c > L' ' && c < 0x7F) fourcc += static_cast<wchar_t>(std::towupper(c));
    }
    if (fourcc == L"HEVC" || fourcc == L"HEVS" || fourcc == L"H265" || fourcc == L"HVC1" ||
        fourcc == L"HEV1")
        return {L"HEVC", L"HEVC (H.265)", L"9NMZLZ57R3T7"};
    if (fourcc == L"AV01") return {L"AV1", L"AV1", L"9MVZQVXJBQ9V"};
    if (fourcc == L"VP90" || fourcc == L"VP09") return {L"VP9", L"VP9", L"9N4D0MSMP0PT"};
    if (fourcc == L"VP80") return {L"VP8", L"VP8", nullptr};
    if (fourcc.empty()) fourcc = L"?";
    return {fourcc, fourcc, nullptr};
}

// True when the file has a video track that no installed decoder (software,
// hardware or Store extension) accepts. Everything is loaded on demand.
struct NativeProbe {
    HRESULT result = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    GUID subtype{};
    bool subtype_known = false;
    UINT32 decoders = 0;
};
bool MissingVideoDecoder(const std::wstring& path, CodecInfo& info, NativeProbe& probe) {
    HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE readwrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    bool missing = false;
    if (plat && readwrite) {
        using Startup = HRESULT (WINAPI*)(ULONG, DWORD);
        using Shutdown = HRESULT (WINAPI*)();
        using EnumEx = HRESULT (WINAPI*)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
                                         const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*);
        using CreateReader = HRESULT (WINAPI*)(LPCWSTR, IMFAttributes*, IMFSourceReader**);
        const auto startup = reinterpret_cast<Startup>(GetProcAddress(plat, "MFStartup"));
        const auto shutdown = reinterpret_cast<Shutdown>(GetProcAddress(plat, "MFShutdown"));
        const auto enumerate = reinterpret_cast<EnumEx>(GetProcAddress(plat, "MFTEnumEx"));
        const auto create = reinterpret_cast<CreateReader>(
            GetProcAddress(readwrite, "MFCreateSourceReaderFromURL"));
        if (startup && shutdown && enumerate && create &&
            SUCCEEDED(probe.result = startup(MF_VERSION, MFSTARTUP_LITE))) {
            {
                ComPtr<IMFSourceReader> reader;
                ComPtr<IMFMediaType> type;
                GUID subtype{};
                if (SUCCEEDED(probe.result = create(path.c_str(), nullptr, &reader)) &&
                    SUCCEEDED(probe.result = reader->GetNativeMediaType(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &type)) &&
                    SUCCEEDED(probe.result = type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
                    probe.subtype = subtype;
                    probe.subtype_known = true;
                    const MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, subtype};
                    IMFActivate** found = nullptr;
                    UINT32 count = 0;
                    if (SUCCEEDED(probe.result = enumerate(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_ALL, &input,
                                            nullptr, &found, &count))) {
                        probe.decoders = count;
                        missing = count == 0;
                        for (UINT32 i = 0; i < count; ++i)
                            if (found[i]) found[i]->Release();
                        CoTaskMemFree(found);
                    }
                    if (missing) info = DescribeCodec(subtype);
                }
            }
            shutdown();
        }
    }
    if (readwrite) FreeLibrary(readwrite);
    if (plat) FreeLibrary(plat);
    return missing;
}
} // namespace

struct VideoPreview::Shared {
    const uint64_t session = diagnostics::runtime::NextId();
    bool tracks_dirty = false;
    bool enable_track_playback = false;
    int64_t ffmpeg_resume_position = 0;
    int ffmpeg_resume_audio = 0;
    int requested_audio = -1, requested_subtitle = -1;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Shared() { if (wake) CloseHandle(wake); }
    void Wake() { if (wake) SetEvent(wake); }
    std::mutex mutex;
    State snapshot;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    std::wstring path;
    std::promise<void> released;
    std::shared_future<void> release_done = released.get_future().share();
    std::atomic<bool> repaint{true};
    HWND child = nullptr;
    bool card = false;        // set before the player thread starts (Open)
    bool playing = true;
    bool seek_pending = false;
    double seek_fraction = 0;
    unsigned steps = 0;
    bool audio_dirty = true;  // volume / mute / rate to apply on the player thread
    // FFmpeg playback: the latest picture. The owner composes it (a GDI child
    // would not show in a WS_EX_NOREDIRECTIONBITMAP window).
    std::shared_ptr<const FfmpegFrame> frame;
    uint64_t frame_serial = 0;
};

VideoPreview::~VideoPreview() { Reset(); }

namespace {
// Pack resolution is cached (one tick-count compare), so this is cheap on paint.
bool MediaPackPlayable() {
    return packs::ResolvePack(packs::PackId::Media).source != packs::ToolSource::None;
}

// Formats Media Foundation is asked to open at all.
bool NativeExtension(const std::wstring& ext) {
    for (const auto* known : {L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".mkv", L".webm", L".mpg",
                              L".mpeg", L".m2ts", L".mts", L".3gp", L".mp3", L".wav", L".flac", L".m4a",
                              L".aac", L".wma", L".ogg", L".oga", L".opus", L".aif", L".aiff"})
        if (ext == known) return true;
    return false;
}

std::wstring LowerExtension(const std::wstring& path) {
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

// ".ts" is both MPEG transport stream and TypeScript. A transport stream
// repeats the 0x47 sync byte every 188 bytes (192 with a BDAV timecode);
// anything else is source text. The last answer is cached because playback
// and paint code ask about the same item repeatedly.
bool TransportStream(const std::wstring& path) {
    static thread_local std::wstring cached_path;
    static thread_local bool cached = false;
    if (path == cached_path) return cached;
    unsigned char head[4 + 192 + 1]{};
    DWORD got = 0;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        if (!ReadFile(file, head, sizeof(head), &got, nullptr)) got = 0;
        CloseHandle(file);
    }
    const bool plain = got > 188 && head[0] == 0x47 && head[188] == 0x47;
    const bool bdav = got > 196 && head[4] == 0x47 && head[196] == 0x47;
    // An unreadable file keeps the media route (offline placeholders, locks).
    cached = file == INVALID_HANDLE_VALUE || plain || bdav;
    cached_path = path;
    return cached;
}
} // namespace

bool VideoPreview::IsAudio(const std::wstring& path) {
    const std::wstring ext = LowerExtension(path);
    return preview::IsAudioExtension(ext) ||
        (ffmpeg::IsAudioExtension(ext) && MediaPackPlayable());
}

bool VideoPreview::Supports(const std::wstring& path) {
    const std::wstring ext = LowerExtension(path);
    if (ext == L".ts" && !TransportStream(path)) return false;
    return preview::IsVideoExtension(ext) || preview::IsAudioExtension(ext) ||
        ((ffmpeg::IsVideoExtension(ext) || ffmpeg::IsAudioExtension(ext)) && MediaPackPlayable());
}

LRESULT CALLBACK VideoPreview::VideoProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    const HWND parent = GetParent(hwnd);
    auto* holder = reinterpret_cast<std::shared_ptr<Shared>*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == kRetireVideo) {
        if (holder && holder->get() == reinterpret_cast<Shared*>(lparam) && (*holder)->stop.load())
            DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_NCDESTROY && holder) {
        (*holder)->stop.store(true);
        (*holder)->Wake();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete holder;
    }
    // The hover card's video never takes the pointer: the list under it
    // keeps tracking hover and clicks.
    if (message == WM_NCHITTEST && holder && (*holder)->card) return HTTRANSPARENT;
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        if (holder) (*holder)->repaint.store(true);
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN: SetFocus(parent); return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_CONTEXTMENU:
        PostMessageW(parent, message, wparam, lparam);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void VideoPreview::Open(HWND owner, const std::wstring& path, bool card) {
    Reset();
    static std::once_flag registered;
    std::call_once(registered, [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = VideoProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kVideoClass;
        RegisterClassW(&wc);
    });
    child_ = CreateWindowExW(0, kVideoClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    state_ = std::make_shared<Shared>();
    state_->snapshot.opening = true;
    state_->path = path;
    state_->child = child_;
    state_->card = card;
    if (card) state_->snapshot.muted = true;
    if (!child_) { state_->snapshot.error = HRESULT_FROM_WIN32(GetLastError()); state_->finished = true; state_->released.set_value(); return; }
    SetWindowLongPtrW(child_, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(new std::shared_ptr<Shared>(state_)));
    try {
        std::thread(Run, state_, path).detach();
    } catch (...) {
        state_->snapshot.error = E_OUTOFMEMORY;
        state_->finished.store(true);
        state_->released.set_value();
    }
}

void VideoPreview::Reset() {
    std::erase_if(retired_, [](const auto& weak) { return weak.expired(); });
    if (state_) { state_->stop.store(true); state_->Wake(); retired_.push_back(state_); }
    if (child_) {
        ShowWindow(child_, SW_HIDE);
        // Retire the render target only after MFPlay has released it. The child
        // holds the mailbox alive; late retirement messages verify its identity.
        if (!state_ || state_->finished.load()) DestroyWindow(child_);
        child_ = nullptr;
    }
    state_.reset();
    layout_valid_ = false;
    region_size_ = {};
    region_radius_ = 0;
}

std::vector<std::shared_future<void>> VideoPreview::SuspendVolume(const std::wstring& root) {
    if (state_ && path::OnVolume(state_->path, root)) Reset();
    std::vector<std::shared_future<void>> pending;
    std::erase_if(retired_, [&](const auto& weak) {
        const auto state = weak.lock();
        if (!state) return true;
        if (path::OnVolume(state->path, root)) {
            state->stop = true;
            state->Wake();
            pending.push_back(state->release_done);
        }
        return state->finished.load();
    });
    return pending;
}

void VideoPreview::Layout(const RECT& bounds, bool visible, int corner_radius) {
    if (!child_) return;
    const LONG width = (std::max)(1L, bounds.right - bounds.left);
    const LONG height = (std::max)(1L, bounds.bottom - bounds.top);
    const RECT normalized{bounds.left, bounds.top, bounds.left + width, bounds.top + height};
    if (!layout_valid_ || !EqualRect(&normalized, &layout_bounds_) || visible != layout_visible_) {
        if (SetWindowPos(child_, nullptr, bounds.left, bounds.top, width, height,
            SWP_NOACTIVATE | SWP_NOZORDER | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW))) {
            layout_bounds_ = normalized;
            layout_visible_ = visible;
            layout_valid_ = true;
        }
    }
    if (width != region_size_.cx || height != region_size_.cy || corner_radius != region_radius_) {
        if (state_) state_->Wake();
        region_size_ = SIZE{width, height};
        region_radius_ = corner_radius;
        // The window owns the region after SetWindowRgn.
        HRGN region = corner_radius > 0
            ? CreateRoundRectRgn(0, 0, width + 1, height + 1, corner_radius * 2, corner_radius * 2)
            : nullptr;
        if (!SetWindowRgn(child_, region, TRUE) && region) DeleteObject(region);
    }
}

std::shared_ptr<const FfmpegFrame> VideoPreview::Frame(uint64_t& serial) const {
    if (!state_) return nullptr;
    std::lock_guard lock(state_->mutex);
    serial = state_->frame_serial;
    return state_->frame;
}

VideoPreview::State VideoPreview::Snapshot() const {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    return state_->snapshot;
}
void VideoPreview::Play(bool playing) {
    if (!state_) return;
    struct Notify { Shared* state; ~Notify() { state->Wake(); } } notify{state_.get()};
    std::lock_guard lock(state_->mutex);
    state_->playing = playing;
    state_->snapshot.playing = playing;
    state_->steps = 0;
    state_->snapshot.control_error = S_OK;
    if (playing && state_->snapshot.ended) {
        state_->seek_fraction = 0;
        state_->seek_pending = true;
    }
}
void VideoPreview::Seek(double fraction) {
    if (!state_) return;
    struct Notify { Shared* state; ~Notify() { state->Wake(); } } notify{state_.get()};
    std::lock_guard lock(state_->mutex);
    if (!state_->snapshot.can_seek || state_->snapshot.duration <= 0) return;
    state_->seek_fraction = std::clamp(fraction, 0.0, 1.0);
    state_->seek_pending = true;
    state_->steps = 0;
    state_->snapshot.control_error = S_OK;
}
void VideoPreview::SetVolume(float volume) {
    if (!state_) return;
    struct Notify { Shared* state; ~Notify() { state->Wake(); } } notify{state_.get()};
    std::lock_guard lock(state_->mutex);
    state_->snapshot.volume = std::clamp(volume, 0.0f, 1.0f);
    state_->audio_dirty = true;
}
void VideoPreview::SetMuted(bool muted) {
    if (!state_) return;
    struct Notify { Shared* state; ~Notify() { state->Wake(); } } notify{state_.get()};
    std::lock_guard lock(state_->mutex);
    state_->snapshot.muted = muted;
    state_->audio_dirty = true;
}
void VideoPreview::SetRate(float rate) {
    if (!state_) return;
    struct Notify { Shared* state; ~Notify() { state->Wake(); } } notify{state_.get()};
    std::lock_guard lock(state_->mutex);
    state_->snapshot.rate = std::clamp(rate, 0.25f, 4.0f);
    state_->audio_dirty = true;
}
void VideoPreview::EnableTrackPlayback() {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    if (!state_->snapshot.ffmpeg_available || state_->snapshot.ffmpeg || !state_->snapshot.ready) return;
    state_->enable_track_playback = true;
    state_->snapshot.opening = true;
    state_->Wake();
}
void VideoPreview::SelectAudioTrack(int id) {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    const auto& s = state_->snapshot;
    if (!s.can_select_audio || !std::any_of(s.audio_tracks.begin(), s.audio_tracks.end(),
        [id](const Track& t) { return t.id == id && t.supported; })) return;
    state_->requested_audio = id;
    state_->tracks_dirty = true;
    state_->Wake();
}
void VideoPreview::SelectSubtitleTrack(int id) {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    const auto& s = state_->snapshot;
    if (!s.can_select_subtitle || (id != -1 && !std::any_of(s.subtitle_tracks.begin(), s.subtitle_tracks.end(),
        [id](const Track& t) { return t.id == id && t.supported; }))) return;
    state_->requested_subtitle = id;
    state_->tracks_dirty = true;
    state_->Wake();
}

void VideoPreview::Step() {
    if (!state_) return;
    struct Notify { Shared* state; ~Notify() { state->Wake(); } } notify{state_.get()};
    std::lock_guard lock(state_->mutex);
    if (!state_->snapshot.ready || state_->snapshot.ended) return;
    state_->playing = false;
    state_->snapshot.playing = false;
    state_->steps = (std::min)(state_->steps + 1, 16u);
    state_->snapshot.control_error = S_OK;
}

void VideoPreview::Run(std::shared_ptr<Shared> state, std::wstring path) {
    struct Finish {
        std::shared_ptr<Shared> state;
        ~Finish() {
            diagnostics::runtime::Event("playback_close", {{"session", state->session}, {"cancelled", state->stop.load()}});
            {
                std::lock_guard lock(state->mutex);
                state->snapshot.opening = false;
            }
            state->finished.store(true);
            state->released.set_value();
            if (state->stop.load())
                PostMessageW(state->child, kRetireVideo, 0, reinterpret_cast<LPARAM>(state.get()));
        }
    } finish{state};
    const ULONGLONG opened_at = GetTickCount64();
    diagnostics::runtime::Event("playback_open", {{"session", state->session},
        {"remote", path.rfind(L"\\\\", 0) == 0}, {"card", state->card}});
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE library = nullptr;
    ComPtr<IMFPMediaPlayer> player;
    ComPtr<Events> events;
    auto fail = [&](HRESULT hr) {
        diagnostics::runtime::Event("playback_error", {{"session", state->session}, {"hr", static_cast<uint32_t>(hr)}}, diagnostics::runtime::Level::Warning);
        std::lock_guard lock(state->mutex);
        state->snapshot.error = hr;
        state->snapshot.ready = false;
        state->snapshot.playing = false;
        state->snapshot.unsupported_audio = IsAudio(path) &&
            (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_INVALIDMEDIATYPE ||
             hr == MF_E_TOPO_CODEC_NOT_FOUND);
    };
    if (FAILED(com)) { fail(com); return; }
    if (!IsAudio(path) && !state->stop.load()) {
        const ULONGLONG probe_started = GetTickCount64();
        CodecInfo codec;
        NativeProbe probe;
        const bool missing = MissingVideoDecoder(pulse::path::StripExtendedPathPrefix(path), codec, probe);
        if (missing) {
            std::lock_guard lock(state->mutex);
            state->snapshot.missing_decoder = true;
            state->snapshot.codec = codec.short_name;
            state->snapshot.codec_name = codec.name;
            state->snapshot.store_id = codec.store;
        }
        uint64_t subtype_low = 0, subtype_high = 0;
        memcpy(&subtype_low, &probe.subtype, sizeof(subtype_low));
        memcpy(&subtype_high, reinterpret_cast<const BYTE*>(&probe.subtype) + sizeof(subtype_low), sizeof(subtype_high));
        diagnostics::runtime::Event("playback_native_probe", {{"session", state->session},
            {"elapsed_ms", GetTickCount64() - probe_started}, {"probe_hr", static_cast<uint32_t>(probe.result)},
            {"subtype_known", probe.subtype_known}, {"subtype_low", subtype_low}, {"subtype_high", subtype_high},
            {"decoders", probe.decoders}, {"missing_decoder", missing}},
            FAILED(probe.result) ? diagnostics::runtime::Level::Warning : diagnostics::runtime::Level::Info);
    }
    // The FFmpeg preview pack takes over what Media Foundation cannot play:
    // containers it does not know, and videos without an installed decoder.
    const std::wstring pack_ffmpeg = packs::PackToolPath(packs::PackId::Media, L"ffmpeg.exe");
    const ULONGLONG capability_start = GetTickCount64();
    const bool subtitles_available = !pack_ffmpeg.empty() && FfmpegSubtitleFilters(pack_ffmpeg, &state->stop);
    if (!pack_ffmpeg.empty()) diagnostics::runtime::Event("playback_pack_capability", {{"session", state->session},
        {"subtitle_filters", subtitles_available}, {"elapsed_ms", GetTickCount64() - capability_start},
        {"cancelled", state->stop.load()}});
    {
        std::lock_guard lock(state->mutex);
        state->snapshot.ffmpeg_subtitles_available = subtitles_available;
        state->snapshot.ffmpeg_available = !pack_ffmpeg.empty();
    }
    const std::wstring ext = LowerExtension(path);
    const bool mf_format = NativeExtension(ext);
    // MPEG program streams: Media Foundation runs the clock and the audio but
    // presents only a grey picture (MPEG-1 and MPEG-2 alike), so the pack goes
    // first when it is installed.
    const bool pack_first = ext == L".mpg" || ext == L".mpeg";
    if (!pack_ffmpeg.empty() && !state->stop.load() &&
        (!mf_format || pack_first || state->snapshot.missing_decoder)) {
        if (RunFfmpeg(state, path, pack_ffmpeg)) { CoUninitialize(); return; }
    }
    if (!mf_format) {
        fail(MF_E_UNSUPPORTED_BYTESTREAM_TYPE);
        CoUninitialize();
        return;
    }
    library = LoadLibraryExW(L"mfplay.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using CreatePlayer = HRESULT (WINAPI*)(LPCWSTR, BOOL, MFP_CREATION_OPTIONS,
        IMFPMediaPlayerCallback*, HWND, IMFPMediaPlayer**);
    const auto create = library ? reinterpret_cast<CreatePlayer>(
        GetProcAddress(library, "MFPCreateMediaPlayer")) : nullptr;
    HRESULT hr = create ? S_OK : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    if (SUCCEEDED(hr) && !state->stop.load()) {
        events.Attach(new Events());
        hr = create(nullptr, FALSE, MFP_OPTION_FREE_THREADED_CALLBACK,
            events.Get(), state->child, &player);
        // MFPlay resolves a URL and rejects filesystem extended-length prefixes.
        const std::wstring media_path = pulse::path::StripExtendedPathPrefix(path);
        if (SUCCEEDED(hr)) hr = player->CreateMediaItemFromURL(media_path.c_str(), FALSE, 0, nullptr);
    }
    if (FAILED(hr)) fail(hr);
    bool ready = false;
    bool busy = false;
    bool positioning = false;
    bool ended = false;
    bool operation_error = false;
    uint32_t stepped = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool switch_backend = false;
    bool logged_ready = false;
    int64_t last_position = -1;
    ULONGLONG last_progress = GetTickCount64(), last_stall = last_progress;
    while (SUCCEEDED(hr) && player && !state->stop.load()) {
        for (const auto& event : events->Take()) {
            if (FAILED(event.error)) {
                if (!ready || event.type == MFP_EVENT_TYPE_ERROR) { hr = event.error; fail(hr); break; }
                busy = false;
                positioning = false;
                operation_error = true;
                std::lock_guard lock(state->mutex);
                diagnostics::runtime::Event("playback_control_error", {{"session", state->session}, {"hr", static_cast<uint32_t>(event.error)}}, diagnostics::runtime::Level::Warning);
                state->snapshot.control_error = event.error;
                state->playing = false;
                state->steps = 0;
                state->seek_pending = false;
                continue;
            }
            switch (event.type) {
            case MFP_EVENT_TYPE_MEDIAITEM_CREATED:
                hr = event.item ? player->SetMediaItem(event.item.Get()) : E_FAIL;
                break;
            case MFP_EVENT_TYPE_MEDIAITEM_SET: {
                ready = true;
                MFP_MEDIAITEM_CHARACTERISTICS flags{};
                ComPtr<IMFPMediaItem> item;
                if (SUCCEEDED(player->GetMediaItem(&item))) item->GetCharacteristics(&flags);
                SIZE native{}, aspect{};
                player->GetNativeVideoSize(&native, &aspect);
                std::vector<Track> audio_tracks, subtitle_tracks;
                int selected = -1;
                DWORD count = 0;
                if (item && SUCCEEDED(item->GetNumberOfStreams(&count))) {
                    for (DWORD index = 0; index < (std::min)(count, 128UL); ++index) {
                        PROPVARIANT type{};
                        if (SUCCEEDED(item->GetStreamAttribute(index, MF_MT_MAJOR_TYPE, &type)) &&
                            type.vt == VT_CLSID && type.puuid) {
                            if (*type.puuid == MFMediaType_Audio) {
                                PROPVARIANT subtype{};
                                const HRESULT subtype_hr = item->GetStreamAttribute(index, MF_MT_SUBTYPE, &subtype);
                                const bool known = SUCCEEDED(subtype_hr) && subtype.vt == VT_CLSID && subtype.puuid;
                                diagnostics::runtime::Event("playback_native_audio_stream", {{"session", state->session},
                                    {"index", index}, {"subtype_known", known}, {"hr", static_cast<uint32_t>(subtype_hr)},
                                    {"subtype_data1", known ? subtype.puuid->Data1 : 0}});
                                PropVariantClear(&subtype);
                                BOOL enabled = FALSE;
                                item->GetStreamSelection(index, &enabled);
                                if (enabled) selected = static_cast<int>(index);
                                std::wstring label = std::to_wstring(audio_tracks.size() + 1);
                                PROPVARIANT language{};
                                if (SUCCEEDED(item->GetStreamAttribute(index, MF_SD_LANGUAGE, &language)) &&
                                    language.vt == VT_LPWSTR && language.pwszVal) label += L" · " + std::wstring(language.pwszVal);
                                PropVariantClear(&language);
                                audio_tracks.push_back({static_cast<int>(index), std::move(label), true});
                            } else if (*type.puuid != MFMediaType_Video) {
                                subtitle_tracks.push_back({static_cast<int>(index), std::to_wstring(index + 1), false});
                            }
                        }
                        PropVariantClear(&type);
                    }
                }
                if (!logged_ready) {
                    diagnostics::runtime::Event("playback_ready", {{"session", state->session}, {"backend", 1},
                        {"elapsed_ms", GetTickCount64() - opened_at}, {"audio_tracks", audio_tracks.size()},
                        {"audio_selected", selected >= 0}});
                    logged_ready = true;
                }
                std::lock_guard lock(state->mutex);
                state->snapshot.audio_tracks = std::move(audio_tracks);
                state->snapshot.subtitle_tracks = std::move(subtitle_tracks);
                state->snapshot.audio_track = selected;
                state->snapshot.can_select_audio = state->snapshot.audio_tracks.size() > 1;
                state->snapshot.can_select_subtitle = false;
                state->snapshot.can_seek = (flags & MFP_MEDIAITEM_CAN_SEEK) != 0;
                state->snapshot.width = static_cast<uint32_t>((std::max)(0L, native.cx));
                state->snapshot.height = static_cast<uint32_t>((std::max)(0L, native.cy));
                break;
            }
            case MFP_EVENT_TYPE_PLAYBACK_ENDED: {
                ended = true;
                busy = false;
                positioning = false;
                std::lock_guard lock(state->mutex);
                state->playing = false;
                state->steps = 0;
                break;
            }
            case MFP_EVENT_TYPE_FRAME_STEP: ++stepped; busy = false; break;
            case MFP_EVENT_TYPE_POSITION_SET:
                ended = false; stepped = 0; busy = false; positioning = false; break;
            case MFP_EVENT_TYPE_PLAY: ended = false; stepped = 0; busy = false; break;
            case MFP_EVENT_TYPE_PAUSE: busy = false; break;
            default: break;
            }
        }
        if (FAILED(hr)) { fail(hr); break; }
        MFP_MEDIAPLAYER_STATE actual = MFP_MEDIAPLAYER_STATE_EMPTY;
        player->GetState(&actual);
        const int64_t duration = ready ? Position(player.Get(), true) : 0;
        const int64_t position = ready ? Position(player.Get(), false) : 0;
        {
            std::lock_guard lock(state->mutex);
            if (state->enable_track_playback && ready) {
                state->enable_track_playback = false;
                state->ffmpeg_resume_position = position;
                const auto& tracks = state->snapshot.audio_tracks;
                const auto selected = std::find_if(tracks.begin(), tracks.end(), [&](const Track& track) {
                    return track.id == state->snapshot.audio_track;
                });
                state->ffmpeg_resume_audio = selected == tracks.end() ? 0 : static_cast<int>(selected - tracks.begin());
                switch_backend = true;
                break;
            }
        }
        const ULONGLONG observed_at = GetTickCount64();
        if (position != last_position || actual != MFP_MEDIAPLAYER_STATE_PLAYING) last_progress = observed_at;
        last_position = position;
        if (ready && actual == MFP_MEDIAPLAYER_STATE_PLAYING && observed_at - last_progress >= 2000 && observed_at - last_stall >= 5000) {
            diagnostics::runtime::Event("playback_clock_stall", {{"session", state->session}, {"backend", 1},
                {"elapsed_ms", observed_at - last_progress}});
            last_stall = observed_at;
        }
        int select_audio = -1;
        std::vector<Track> audio_tracks;
        {
            std::lock_guard lock(state->mutex);
            if (ready && !busy && state->tracks_dirty) {
                state->tracks_dirty = false;
                select_audio = state->requested_audio;
                audio_tracks = state->snapshot.audio_tracks;
            }
        }
        if (select_audio >= 0) {
            ComPtr<IMFPMediaItem> item;
            HRESULT changed = player->GetMediaItem(&item);
            for (const auto& track : audio_tracks) {
                if (FAILED(changed)) break;
                changed = item->SetStreamSelection(static_cast<DWORD>(track.id), track.id == select_audio);
            }
            // MFPlay applies selection only when the item is set again.
            if (SUCCEEDED(changed)) changed = player->SetMediaItem(item.Get());
            diagnostics::runtime::Event("playback_track", {{"session", state->session}, {"backend", 1},
                {"audio", static_cast<uint32_t>(select_audio)}, {"hr", static_cast<uint32_t>(changed)}},
                FAILED(changed) ? diagnostics::runtime::Level::Warning : diagnostics::runtime::Level::Info);
            std::lock_guard lock(state->mutex);
            state->snapshot.control_error = changed;
            if (SUCCEEDED(changed)) {
                ready = false;
                ended = false;
                state->seek_pending = duration > 0;
                state->seek_fraction = duration > 0 ? static_cast<double>(position) / duration : 0;
                state->audio_dirty = true;
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            }
        }
        bool want_play = false, seek = false, step = false;
        double fraction = 0;
        {
            std::lock_guard lock(state->mutex);
            state->snapshot.ready = ready;
            if (ready) state->snapshot.opening = false;
            state->snapshot.playing = state->playing && !ended;
            state->snapshot.ended = ended;
            state->snapshot.busy = busy || state->seek_pending || state->steps != 0;
            state->snapshot.duration = duration;
            state->snapshot.position = ended ? duration : position;
            state->snapshot.stepped_frames = stepped;
            want_play = state->playing;
            // Explicit user input clears a recoverable control error.
            if (SUCCEEDED(state->snapshot.control_error)) operation_error = false;
            if (ready && (!busy || positioning) && !operation_error &&
                (state->playing || actual != MFP_MEDIAPLAYER_STATE_PLAYING)) {
                // Finish the pause before consuming a scrub request. Otherwise
                // continuous mouse seeks can starve Pause and leave audio playing.
                seek = state->seek_pending;
                fraction = state->seek_fraction;
                if (seek) state->seek_pending = false;
                else if (!busy && state->steps && actual != MFP_MEDIAPLAYER_STATE_PLAYING) {
                    step = true;
                    --state->steps;
                }
            }
        }
        HRESULT control = S_OK;
        bool issued = false;
        if (ready && (!busy || (positioning && seek)) && !operation_error) {
            if (seek && duration > 0) {
                PROPVARIANT target{};
                target.vt = VT_I8;
                // Leave a small decodable tail: seeking past the final video sample
                // can stall MFPlay permanently, including subsequent seeks.
                const int64_t last_seek = duration - (std::min)(duration / 2, int64_t{1000000});
                target.hVal.QuadPart = (std::min)(last_seek,
                    static_cast<int64_t>(fraction * static_cast<double>(duration)));
                control = player->SetPosition(MFP_POSITIONTYPE_100NS, &target);
                issued = true;
            } else if (!want_play && actual == MFP_MEDIAPLAYER_STATE_PLAYING) {
                control = player->Pause(); issued = true;
            } else if (step) {
                control = player->FrameStep(); issued = true;
            } else if (want_play && actual != MFP_MEDIAPLAYER_STATE_PLAYING) {
                control = player->Play(); issued = true;
            }
            if (issued && SUCCEEDED(control)) {
                busy = true;
                positioning = seek;
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            }
            if (FAILED(control)) {
                diagnostics::runtime::Event("playback_control_error", {{"session", state->session}, {"hr", static_cast<uint32_t>(control)}}, diagnostics::runtime::Level::Warning);
                operation_error = true;
                std::lock_guard lock(state->mutex);
                state->snapshot.control_error = control;
                state->playing = false;
                state->steps = 0;
            }
        }
        if ((!ready || busy) && std::chrono::steady_clock::now() > deadline) {
            if (!ready) {
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); fail(hr); break;
            }
            // A control request timing out must not tear down an already open
            // media item. A later seek/play request can recover the player.
            busy = false;
            positioning = false;
            operation_error = true;
            std::lock_guard lock(state->mutex);
            diagnostics::runtime::Event("playback_control_error", {{"session", state->session},
                {"hr", static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_TIMEOUT))}}, diagnostics::runtime::Level::Warning);
            state->snapshot.control_error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            state->snapshot.busy = false;
            state->playing = false;
            state->steps = 0;
        }
        if (ready) {
            bool apply = false;
            float volume = 1.0f, rate = 1.0f;
            bool muted = false;
            {
                std::lock_guard lock(state->mutex);
                apply = std::exchange(state->audio_dirty, false);
                volume = state->snapshot.volume;
                muted = state->snapshot.muted;
                rate = state->snapshot.rate;
            }
            if (apply) {
                const HRESULT volume_hr = player->SetVolume(volume);
                const HRESULT mute_hr = player->SetMute(muted ? TRUE : FALSE);
                if (FAILED(volume_hr) || FAILED(mute_hr)) {
                    diagnostics::runtime::Event("playback_audio_control_error", {{"session", state->session},
                        {"volume_hr", static_cast<uint32_t>(volume_hr)}, {"mute_hr", static_cast<uint32_t>(mute_hr)}}, diagnostics::runtime::Level::Warning);
                    std::lock_guard lock(state->mutex);
                    state->snapshot.control_error = FAILED(volume_hr) ? volume_hr : mute_hr;
                }
                float applied_rate = 1.0f;
                HRESULT rate_hr = player->SetRate(rate);
                if (SUCCEEDED(rate_hr)) rate_hr = player->GetRate(&applied_rate);
                if (FAILED(rate_hr)) {
                    applied_rate = 1.0f;
                    diagnostics::runtime::Event("playback_control_error", {{"session", state->session},
                        {"hr", static_cast<uint32_t>(rate_hr)}, {"control", 4}}, diagnostics::runtime::Level::Warning);
                    std::lock_guard lock(state->mutex);
                    state->snapshot.control_error = rate_hr;
                }
                std::lock_guard lock(state->mutex);
                if (!state->audio_dirty) state->snapshot.rate = applied_rate;
            }
        }
        // Repaint paused frames as well after exposure/resize; no provider calls on UI.
        if (ready && state->repaint.exchange(false)) player->UpdateVideo();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (player) player->Shutdown();
    player.Reset();
    events.Reset();
    if (library) FreeLibrary(library);
    // Media Foundation could not open it at all (e.g. MKV with an unknown
    // audio codec, broken indexes): let the pack try before giving up.
    if ((switch_backend || (!ready && FAILED(hr))) && !pack_ffmpeg.empty() && !state->stop.load()) {
        if (!RunFfmpeg(state, path, pack_ffmpeg) && !state->stop.load()) fail(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    }
    CoUninitialize();
}

bool VideoPreview::RunFfmpeg(const std::shared_ptr<Shared>& state, const std::wstring& path,
                             const std::wstring& ffmpeg_exe) {
    {
        std::lock_guard lock(state->mutex);
        state->snapshot.opening = true;
        state->snapshot.ready = false;
        state->snapshot.playing = false;
    }
    FfmpegMediaInfo info;
    const std::wstring media_path = pulse::path::StripExtendedPathPrefix(path);
    const ULONGLONG probe_start = GetTickCount64();
    const bool probed = !state->stop.load() && ProbeFfmpegMedia(ffmpeg_exe, media_path, info, &state->stop);
    diagnostics::runtime::Event("playback_probe", {{"session", state->session}, {"backend", 2},
        {"elapsed_ms", GetTickCount64() - probe_start}, {"ok", probed}, {"cancelled", state->stop.load()},
        {"width", info.width}, {"height", info.height}, {"audio_tracks", info.audio_tracks.size()},
        {"subtitle_tracks", info.subtitle_tracks.size()}});
    if (!probed || state->stop.load()) return false;
    for (const auto& track : info.audio_tracks) {
        // Only codec identifiers, never FFmpeg stderr (which includes paths/metadata).
        if (track.codec.size() <= 40 && track.codec.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos)
            diagnostics::runtime::TextEvent("playback_audio_stream", {{"codec", std::wstring(track.codec.begin(), track.codec.end())}},
                {{"session", state->session}, {"index", static_cast<uint64_t>(track.id)}});
    }
    const bool audio_file = IsAudio(path);
    if (audio_file ? !info.audio : !info.video) return false;
    if (audio_file) info.video = false;   // cover art is drawn by the Quick Look itself

    float rate = 1.0f, volume = 1.0f;
    bool muted = false;
    {
        std::lock_guard lock(state->mutex);
        auto& s = state->snapshot;
        s.ready = false;
        s.error = S_OK;
        s.control_error = S_OK;
        s.missing_decoder = false;
        s.unsupported_audio = false;
        s.codec.clear();
        s.codec_name.clear();
        s.store_id = nullptr;
        s.ffmpeg = true;
        s.ffmpeg_subtitles_available = info.subtitle_filters;
        s.audio_tracks.clear();
        s.subtitle_tracks.clear();
        for (const auto& track : info.audio_tracks) s.audio_tracks.push_back({track.id, track.label, track.supported});
        for (const auto& track : info.subtitle_tracks) s.subtitle_tracks.push_back({track.id, track.label, track.supported});
        const int resume_audio = state->ffmpeg_resume_audio >= 0 &&
            static_cast<size_t>(state->ffmpeg_resume_audio) < info.audio_tracks.size() ? state->ffmpeg_resume_audio : 0;
        s.audio_track = info.audio ? resume_audio : -1;
        s.subtitle_track = -1;
        s.can_select_audio = s.audio_tracks.size() > 1;
        s.can_select_subtitle = info.video && std::any_of(info.subtitle_tracks.begin(), info.subtitle_tracks.end(),
            [](const FfmpegTrack& track) { return track.supported; });
        state->requested_audio = s.audio_track;
        state->requested_subtitle = -1;
        state->tracks_dirty = false;
        s.width = info.video ? info.width : 0;
        s.height = info.video ? info.height : 0;
        s.duration = info.duration;
        s.can_seek = info.duration > 0;
        rate = s.rate;
        volume = s.volume;
        muted = s.muted;
        state->audio_dirty = false;
        state->seek_pending = false;
        state->steps = 0;
    }
    // Frames are scaled for the Quick Look's client area (the video child may
    // not be laid out yet); a much larger window later restarts the decoder.
    auto box = [&] {
        if (state->card) return SIZE{640, 360};  // the card's well, 2x for high DPI
        RECT client{};
        GetClientRect(GetParent(state->child), &client);
        return SIZE{(std::max)(client.right - client.left, 320L), (std::max)(client.bottom - client.top, 180L)};
    };
    FfmpegPlayback player(ffmpeg_exe, media_path, info, [state] { state->Wake(); });
    player.SetVolume(volume, muted);
    bool playing = false;
    {
        std::lock_guard lock(state->mutex);
        playing = state->playing;
    }
    int initial_audio = 0;
    {
        std::lock_guard lock(state->mutex);
        initial_audio = (std::max)(0, state->requested_audio);
    }
    player.SelectTracks(initial_audio, -1);
    player.Start(state->ffmpeg_resume_position, rate, box(), playing);
    diagnostics::runtime::Event("playback_decoder_start", {{"session", state->session}, {"audio_open", player.audio_open()},
        {"audio_device_error", player.AudioDeviceError()}, {"output_fps_milli", static_cast<uint64_t>(FfmpegOutputRate(info.fps) * 1000)}},
        info.audio && !player.audio_open() ? diagnostics::runtime::Level::Warning : diagnostics::runtime::Level::Info);
    // 1 ms timer resolution only while frames are being paced.
    struct TimerResolution {
        bool on = false;
        void Set(bool want) {
            if (want == on) return;
            on = want;
            if (want) timeBeginPeriod(1); else timeEndPeriod(1);
        }
        ~TimerResolution() { Set(false); }
    } resolution;
    bool ended = false;
    uint32_t stepped = 0;
    SIZE grown_box{};
    ULONGLONG grown_since = 0;
    ULONGLONG last_frame_at = GetTickCount64(), last_stall_at = last_frame_at;
    bool logged_frame = false;
    while (!state->stop.load()) {
        bool want_play = false, seek = false, step = false, dirty = false;
        double fraction = 0;
        float wanted_rate = rate;
        bool tracks_dirty = false;
        int wanted_audio = 0, wanted_subtitle = -1;
        {
            std::lock_guard lock(state->mutex);
            want_play = state->playing;
            tracks_dirty = state->tracks_dirty;
            state->tracks_dirty = false;
            wanted_audio = state->requested_audio;
            wanted_subtitle = state->requested_subtitle;
            if (state->seek_pending) {
                seek = true;
                fraction = state->seek_fraction;
                state->seek_pending = false;
            }
            if (state->steps && !want_play) {
                step = true;
                --state->steps;
            }
            if (state->audio_dirty) {
                dirty = true;
                state->audio_dirty = false;
                volume = state->snapshot.volume;
                muted = state->snapshot.muted;
                wanted_rate = state->snapshot.rate;
            }
        }
        if (tracks_dirty && player.SelectTracks((std::max)(0, wanted_audio), wanted_subtitle)) {
            const int64_t position = player.Position();
            const bool started = player.Start(position, rate, box(), want_play);
            diagnostics::runtime::Event("playback_track", {{"session", state->session}, {"backend", 2},
                {"audio", static_cast<uint32_t>(wanted_audio)}, {"subtitle", static_cast<uint32_t>(wanted_subtitle)}, {"ok", started}},
                started ? diagnostics::runtime::Level::Info : diagnostics::runtime::Level::Warning);
            std::lock_guard lock(state->mutex);
            if (started) {
                state->snapshot.audio_track = wanted_audio;
                state->snapshot.subtitle_track = wanted_subtitle;
            }
            state->snapshot.control_error = started ? S_OK : E_FAIL;
            ended = false;
        }
        if (dirty) {
            player.SetVolume(volume, muted);
            if (std::fabs(wanted_rate - rate) > 0.001f) {
                rate = wanted_rate;
                if (!ended) player.Start(player.Position(), rate, player.frame_size(), want_play);
            }
        }
        if (seek && info.duration > 0) {
            // Stay clear of the very end, where some files decode no frame.
            const int64_t last = info.duration - (std::min)(info.duration / 2, int64_t{5'000'000});
            player.Start((std::min)(last, static_cast<int64_t>(fraction * static_cast<double>(info.duration))),
                         rate, box(), want_play);
            ended = false;
            stepped = 0;
        } else if (want_play != player.playing() && !ended) {
            player.SetPlaying(want_play);
        }
        if (step) {
            if (player.Step()) ++stepped;
            else {
                std::lock_guard lock(state->mutex);
                ++state->steps;   // the next frame is still decoding
            }
        }
        // Grow the frames once the window has been much larger for a moment.
        if (info.video && !ended) {
            const SIZE now_box = box();
            const SIZE fit = FfmpegFrameSize(info.width, info.height,
                                             {(std::min)(now_box.cx, 1920L), (std::min)(now_box.cy, 1080L)});
            const SIZE have = player.frame_size();
            if (static_cast<double>(fit.cx) * fit.cy > 1.4 * have.cx * have.cy) {
                if (now_box.cx != grown_box.cx || now_box.cy != grown_box.cy) {
                    grown_box = now_box;
                    grown_since = GetTickCount64();
                } else if (GetTickCount64() - grown_since > 400) {
                    player.Start(player.Position(), rate, now_box, want_play);
                    grown_box = {};
                }
            } else {
                grown_box = {};
            }
        }
        if (auto frame = player.Pump()) {
            last_frame_at = GetTickCount64();
            if (!logged_frame) {
                diagnostics::runtime::Event("playback_ready", {{"session", state->session}, {"backend", 2},
                    {"elapsed_ms", last_frame_at - probe_start}, {"audio_open", player.audio_open()}});
                logged_frame = true;
            }
            {
                std::lock_guard lock(state->mutex);
                state->frame = std::move(frame);
                ++state->frame_serial;
            }
            InvalidateRect(GetParent(state->child), nullptr, FALSE);
        }
        if (!ended && player.Ended()) {
            ended = true;
            std::lock_guard lock(state->mutex);
            state->playing = false;
        }
        const ULONGLONG now = GetTickCount64();
        if (!want_play || ended) last_frame_at = now;
        if (info.video && want_play && !ended && now - last_frame_at >= 2000 && now - last_stall_at >= 5000) {
            diagnostics::runtime::Event("playback_frame_stall", {{"session", state->session},
                {"elapsed_ms", now - last_frame_at}, {"audio_open", player.audio_open()}});
            last_stall_at = now;
        }
        const bool failed = player.Failed();
        {
            std::lock_guard lock(state->mutex);
            auto& s = state->snapshot;
            s.ready = !failed;
            s.opening = false;
            s.playing = state->playing && !ended;
            s.ended = ended;
            s.busy = false;
            s.position = ended ? info.duration : player.Position();
            s.stepped_frames = stepped;
            s.rate = rate;
            if (failed) s.error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        if (failed) {
            diagnostics::runtime::Event("playback_error", {{"session", state->session}, {"backend", 2}, {"hr", static_cast<uint32_t>(E_FAIL)}, {"decoder_exit", player.DecoderExitCode()}}, diagnostics::runtime::Level::Warning);
            break;
        }
        const bool pacing = want_play && !ended;
        resolution.Set(pacing && info.video);
        // Paused/ended playback sleeps until a command, resize or decoded frame.
        // A pending resize gets one delayed wake to finish its debounce.
        const DWORD delay = pacing ? 4 : grown_box.cx ? 400 : INFINITE;
        if (state->wake) WaitForSingleObject(state->wake, delay);
        else Sleep(100);
    }
    return true;
}
} // namespace pulse::ui
