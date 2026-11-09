#include "ffmpeg_playback.h"
#include "ui_motion.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <sstream>

#pragma comment(lib, "winmm.lib")

namespace pulse::ui {
namespace {

constexpr int64_t kSecond = 10'000'000;          // 100 ns units
constexpr DWORD kSampleRate = 48000;
constexpr DWORD kBytesPerSecond = kSampleRate * 2 * 2;
constexpr DWORD kWaveBufferBytes = kBytesPerSecond / 25;   // 40 ms
constexpr size_t kMaxQueuedFrames = 4;

std::wstring Seconds(int64_t t) {
    wchar_t text[48];
    const int64_t ms = (std::max)(int64_t{0}, t) / 10'000;
    swprintf_s(text, L"%lld.%03lld", ms / 1000, ms % 1000);
    return text;
}

} // namespace

bool ParseFfmpegMediaInfo(std::string_view log, FfmpegMediaInfo& info) {
    info = {};
    info.duration = static_cast<int64_t>(ffmpeg::ParseDurationMs(log)) * 10'000;
    info.video = ffmpeg::HasStream(log, "Video");
    info.audio = ffmpeg::HasStream(log, "Audio");
    std::istringstream lines{std::string(log)};
    for (std::string line; std::getline(lines, line);) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line.compare(first, 10, "Stream #0:") != 0) continue;
        const auto a = line.find("Audio: "), sub = line.find("Subtitle: ");
        if (a == std::string::npos && sub == std::string::npos) continue;
        const bool audio = a != std::string::npos;
        auto& tracks = audio ? info.audio_tracks : info.subtitle_tracks;
        if (tracks.size() >= 128) continue;
        const auto begin = audio ? a + 7 : sub + 10;
        const auto end = line.find_first_of(" ,\r\n", begin);
        const std::string codec = line.substr(begin, end - begin);
        const bool supported = audio || codec == "subrip" || codec == "ass" || codec == "ssa" ||
            codec == "mov_text" || codec == "webvtt" || codec == "text";
        std::wstring label = std::to_wstring(tracks.size() + 1) + L" · " + std::wstring(codec.begin(), codec.end());
        const auto lang = line.find('(', line.find("Stream #0:"));
        const auto close = lang == std::string::npos ? lang : line.find(')', lang);
        if (close != std::string::npos && close < begin && close - lang < 32)
            label += L" · " + std::wstring(line.begin() + lang + 1, line.begin() + close);
        tracks.push_back({static_cast<int>(tracks.size()), std::move(label), codec, supported});
    }
    if (info.video) {
        UINT w = 0, h = 0, num = 1, den = 1;
        if (!ffmpeg::ParseVideoSize(log, w, h)) {
            info.video = false;
        } else {
            ffmpeg::ParseSampleAspect(log, num, den);
            const double display = static_cast<double>(w) * num / den;
            info.width = static_cast<uint32_t>(std::clamp(std::lround(display), 1L, 65535L));
            info.height = h;
            info.fps = ffmpeg::ParseFrameRate(log);
        }
    }
    return info.video || info.audio;
}

bool ParseFfmpegSubtitleFilters(std::string_view output) {
    bool subtitles = false, setpts = false;
    std::istringstream lines{std::string(output)};
    for (std::string line; std::getline(lines, line);) {
        std::istringstream fields(line);
        std::string flags, name, types;
        if (!(fields >> flags >> name >> types) || (flags.size() != 2 && flags.size() != 3) ||
            flags.find_first_not_of(".TSC") != std::string::npos || types != "V->V") continue;
        if (name == "subtitles") subtitles = true;
        if (name == "setpts") setpts = true;
    }
    return subtitles && setpts;
}

bool FfmpegSubtitleFilters(const std::wstring& executable, const std::atomic<bool>* stop) {
    auto cancelled = [&] { return stop && stop->load(); };
    if (cancelled()) return false;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(executable.c_str(), GetFileExInfoStandard, &attributes)) return false;
    struct Entry { std::wstring executable; FILETIME modified; DWORD size_high, size_low; bool supported; };
    static std::mutex mutex;
    static std::vector<Entry> cache;
    {
        std::lock_guard lock(mutex);
        for (const auto& entry : cache)
            if (entry.executable == executable && CompareFileTime(&entry.modified, &attributes.ftLastWriteTime) == 0 &&
                entry.size_high == attributes.nFileSizeHigh && entry.size_low == attributes.nFileSizeLow)
                return entry.supported;
    }
    ffmpeg::Process process;
    ffmpeg::LaunchOptions options;
    options.discard_stderr = true;
    options.memory_limit = static_cast<SIZE_T>(256) * 1024 * 1024;
    if (!process.Start(executable, L"-hide_banner -nostdin -filters", options)) return false;
    std::string output;
    std::thread reader([&] {
        char buffer[4096];
        for (DWORD got; (got = process.ReadOut(buffer, sizeof(buffer))) > 0;)
            if (output.size() < 256 * 1024) output.append(buffer, got);
    });
    const ULONGLONG deadline = GetTickCount64() + 5000;
    bool finished = false;
    while (!cancelled() && GetTickCount64() < deadline)
        if (process.Wait(25)) { finished = true; break; }
    if (!finished) {
        process.Terminate(cancelled() ? ERROR_CANCELLED : ERROR_TIMEOUT);
        process.Wait(INFINITE);
    }
    reader.join();
    if (!finished || cancelled() || process.ExitCode() != 0) return false;
    const bool supported = ParseFfmpegSubtitleFilters(output);
    {
        std::lock_guard lock(mutex);
        if (cache.size() >= 8) cache.erase(cache.begin());
        cache.push_back({executable, attributes.ftLastWriteTime, attributes.nFileSizeHigh, attributes.nFileSizeLow, supported});
    }
    return supported;
}

bool ProbeFfmpegMedia(const std::wstring& ffmpeg_exe, const std::wstring& path, FfmpegMediaInfo& info,
                      const std::atomic<bool>* stop) {
    info = {};
    static std::timed_mutex probe_slot;
    auto cancelled = [&] { return stop && stop->load(); };
    std::unique_lock<std::timed_mutex> slot(probe_slot, std::defer_lock);
    while (!slot.try_lock_for(std::chrono::milliseconds(25)))
        if (cancelled()) return false;
    if (cancelled()) return false;
    ffmpeg::Process process;
    ffmpeg::LaunchOptions options;
    options.priority = NORMAL_PRIORITY_CLASS;
    options.memory_limit = static_cast<SIZE_T>(512) * 1024 * 1024;
    // No output file: ffmpeg describes the input and exits.
    const std::wstring args = L"-hide_banner -nostdin -i " + ffmpeg::QuoteArgument(ffmpeg::InputArgument(path));
    if (!process.Start(ffmpeg_exe, args, options)) return false;
    std::string log;
    std::thread reader([&] {
        char buffer[8192];
        for (DWORD got; (got = process.ReadErr(buffer, sizeof(buffer))) > 0;)
            if (log.size() < 256 * 1024) log.append(buffer, got);
    });
    const ULONGLONG deadline = GetTickCount64() + 8000;
    bool finished = false;
    while (!cancelled() && GetTickCount64() < deadline)
        if (process.Wait(25)) { finished = true; break; }
    if (!finished) {
        process.Terminate(cancelled() ? ERROR_CANCELLED : ERROR_TIMEOUT);
        process.Wait(INFINITE); // Backend thread: the volume-release barrier needs real process exit.
    }
    reader.join();
    if (!finished || cancelled() || !ParseFfmpegMediaInfo(log, info)) return false;
    info.subtitle_filters = FfmpegSubtitleFilters(ffmpeg_exe, stop);
    if (cancelled()) return false;
    if (!info.subtitle_filters)
        for (auto& track : info.subtitle_tracks) track.supported = false;
    return true;
}

SIZE FfmpegFrameSize(uint32_t width, uint32_t height, SIZE box) {
    if (!width || !height) return {16, 16};
    const double bw = (std::max)(16L, box.cx), bh = (std::max)(16L, box.cy);
    const double scale = (std::min)({1.0, bw / width, bh / height});
    auto even = [](double v) { return (std::max)(16L, static_cast<LONG>(std::lround(v / 2.0)) * 2); };
    return {even(width * scale), even(height * scale)};
}

double FfmpegOutputRate(double source_fps) {
    if (!(source_fps > 0.5)) return 25.0;
    return (std::min)(source_fps, 30.0);
}

std::wstring FfmpegTempoFilter(float rate) {
    rate = std::clamp(rate, 0.25f, 4.0f);
    if (std::fabs(rate - 1.0f) < 0.001f) return {};
    // atempo takes 0.5..2 per stage.
    std::wstring chain;
    auto add = [&](float stage) {
        wchar_t text[32];
        swprintf_s(text, L"atempo=%.4g", stage);
        if (!chain.empty()) chain += L',';
        chain += text;
    };
    while (rate > 2.0f) { add(2.0f); rate /= 2.0f; }
    while (rate < 0.5f) { add(0.5f); rate /= 0.5f; }
    add(rate);
    return chain;
}

// Frame buffers are recycled: a 1080p frame is 8 MB, 30 times a second.
struct FfmpegPlayback::Pool {
    std::mutex mutex;
    std::vector<std::vector<uint8_t>> spare;
    std::shared_ptr<FfmpegFrame> Make(std::shared_ptr<Pool> self, size_t bytes) {
        std::vector<uint8_t> pixels;
        {
            std::lock_guard lock(mutex);
            while (!spare.empty()) {
                pixels = std::move(spare.back());
                spare.pop_back();
                if (pixels.size() == bytes) break;
                pixels.clear();
            }
        }
        if (pixels.size() != bytes) pixels.resize(bytes);
        auto* frame = new FfmpegFrame();
        frame->pixels = std::move(pixels);
        return std::shared_ptr<FfmpegFrame>(frame, [self](FfmpegFrame* f) {
            {
                std::lock_guard lock(self->mutex);
                if (self->spare.size() < kMaxQueuedFrames + 2) self->spare.push_back(std::move(f->pixels));
            }
            delete f;
        });
    }
};

FfmpegPlayback::FfmpegPlayback(std::wstring ffmpeg, std::wstring path, const FfmpegMediaInfo& info,
                               std::function<void()> wake)
    : ffmpeg_(std::move(ffmpeg)), path_(std::move(path)), info_(info), pool_(std::make_shared<Pool>()), wake_(std::move(wake)) {}

FfmpegPlayback::~FfmpegPlayback() { Stop(); }

void FfmpegPlayback::Stop() {
    stop_.store(true);
    // Killing the decoders unblocks the pipe reads of both threads.
    if (video_) video_->Terminate();
    if (audio_) audio_->Terminate();
    // Stop runs on the preview backend. Do not report release while a decoder
    // still owns a file; the eject caller has its own bounded wait.
    if (video_) video_->Wait(INFINITE);
    if (audio_) audio_->Wait(INFINITE);
    queue_cv_.notify_all();
    if (wave_event_) SetEvent(wave_event_);
    if (video_reader_.joinable()) video_reader_.join();
    if (audio_writer_.joinable()) audio_writer_.join();
    if (video_errors_.joinable()) video_errors_.join();
    if (audio_errors_.joinable()) audio_errors_.join();
    video_.reset();
    audio_.reset();
    if (wave_) {
        waveOutReset(wave_);
        for (auto& header : headers_)
            if (header.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wave_, &header, sizeof(header));
        waveOutClose(wave_);
        wave_ = nullptr;
    }
    if (wave_event_) { CloseHandle(wave_event_); wave_event_ = nullptr; }
    for (auto& header : headers_) header = {};
    std::lock_guard lock(queue_mutex_);
    queue_.clear();
    video_eof_ = false;
    audio_eof_.store(false);
    stepped_frame_.reset();
}

bool FfmpegPlayback::SelectTracks(int audio, int subtitle) {
    if (audio < 0 || (info_.audio && static_cast<size_t>(audio) >= info_.audio_tracks.size())) return false;
    if (subtitle < -1 || (subtitle >= 0 && (!info_.subtitle_filters || static_cast<size_t>(subtitle) >= info_.subtitle_tracks.size() ||
        !info_.subtitle_tracks[subtitle].supported))) return false;
    audio_track_ = audio;
    subtitle_track_ = subtitle;
    return true;
}

bool FfmpegPlayback::Start(int64_t position, float rate, SIZE box, bool playing) {
    Stop();
    stop_.store(false);
    failed_ = false;
    decoder_exit_ = 0;
    audio_device_error_ = MMSYSERR_NOERROR;
    { std::lock_guard lock(error_mutex_); diagnostic_.clear(); }
    first_shown_ = false;
    stepped_ = false;
    rate_ = std::clamp(rate, 0.25f, 4.0f);
    playing_ = playing;
    start_ = std::clamp<int64_t>(position, 0, info_.duration > 0 ? info_.duration : INT64_MAX);
    shown_time_ = paused_at_ = wall_base_ = start_;
    wall_tick_ = motion::NowMs();
    const std::wstring input = ffmpeg::QuoteArgument(ffmpeg::InputArgument(path_));
    const std::wstring seek = start_ > 0 ? L" -ss " + Seconds(start_) : std::wstring();
    bool any = false;

    if (info_.video) {
        frame_size_ = FfmpegFrameSize(info_.width, info_.height,
                                      {(std::min)(box.cx, 1920L), (std::min)(box.cy, 1080L)});
        out_fps_ = FfmpegOutputRate(info_.fps);
        wchar_t filter[160];
        swprintf_s(filter, L"fps=%.4f,scale=%ld:%ld:flags=bilinear,format=bgra", out_fps_,
                   frame_size_.cx, frame_size_.cy);
        std::wstring video_filter = filter;
        if (subtitle_track_ >= 0) {
            // Escape the option value and then the containing filter graph.
            auto escape = [](std::wstring value, const wchar_t* special) {
                std::wstring result;
                for (wchar_t c : value) {
                    if (wcschr(special, c)) result += L'\\';
                    result += c;
                }
                return result;
            };
            std::wstring filename = ffmpeg::InputArgument(path_);
            std::replace(filename.begin(), filename.end(), L'\\', L'/');
            filename = escape(escape(filename, L"\\':"), L"\\'[],;");
            video_filter = L"setpts=PTS+" + Seconds(start_) + L"/TB,subtitles=filename=" + filename +
                L":si=" + std::to_wstring(subtitle_track_) + L",setpts=PTS-STARTPTS," + video_filter;
        }
        const std::wstring args = L"-hide_banner -nostdin -nostats -loglevel error" + seek + L" -i " + input +
            L" -map 0:v:0 -an -sn -dn -vf " + ffmpeg::QuoteArgument(video_filter) + L" -f rawvideo -pix_fmt bgra pipe:1";
        ffmpeg::LaunchOptions options;
        options.priority = NORMAL_PRIORITY_CLASS;
        options.memory_limit = static_cast<SIZE_T>(2048) * 1024 * 1024;
        options.stdout_buffer = 4u << 20;
        options.discard_stderr = false;
        video_ = std::make_unique<ffmpeg::Process>();
        if (video_->Start(ffmpeg_, args, options)) {
            any = true;
            video_errors_ = std::thread([this] { DrainError(*video_); });
            video_reader_ = std::thread(&FfmpegPlayback::ReadVideo, this);
        } else {
            video_.reset();
            failed_ = true;
            std::lock_guard lock(queue_mutex_);
            video_eof_ = true;
        }
    } else {
        std::lock_guard lock(queue_mutex_);
        video_eof_ = true;
    }

    if (info_.audio) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = kBytesPerSecond;
        wave_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        audio_device_error_ = wave_event_ ? waveOutOpen(&wave_, WAVE_MAPPER, &format,
            reinterpret_cast<DWORD_PTR>(wave_event_), 0, CALLBACK_EVENT) : MMSYSERR_NOMEM;
        if (audio_device_error_ == MMSYSERR_NOERROR) {
            if (!playing_) waveOutPause(wave_);
            std::wstring args = L"-hide_banner -nostdin -nostats -loglevel error" + seek + L" -i " + input +
                L" -map 0:a:" + std::to_wstring(audio_track_) + L" -vn -sn -dn";
            const std::wstring tempo = FfmpegTempoFilter(rate_);
            if (!tempo.empty()) args += L" -af " + tempo;
            args += L" -ac 2 -ar 48000 -f s16le pipe:1";
            ffmpeg::LaunchOptions options;
            options.priority = NORMAL_PRIORITY_CLASS;
            options.memory_limit = static_cast<SIZE_T>(512) * 1024 * 1024;
            options.stdout_buffer = 256u * 1024;
            options.discard_stderr = false;
            audio_ = std::make_unique<ffmpeg::Process>();
            if (audio_->Start(ffmpeg_, args, options)) {
                any = true;
                for (int i = 0; i < kWaveBuffers; ++i) wave_data_[i].resize(kWaveBufferBytes);
                audio_errors_ = std::thread([this] { DrainError(*audio_); });
                audio_writer_ = std::thread(&FfmpegPlayback::WriteAudio, this);
            } else {
                audio_.reset();
                failed_ = true;
            }
        }
        if (!audio_ && wave_) {
            waveOutClose(wave_);
            wave_ = nullptr;
        }
    }
    // Without an audio device the frames follow the wall clock.
    wall_clock_ = wave_ == nullptr;
    if (!any) failed_ = true;
    return any;
}

void FfmpegPlayback::ReadVideo() {
    const size_t bytes = static_cast<size_t>(frame_size_.cx) * frame_size_.cy * 4;
    const double interval = kSecond / out_fps_;
    for (uint64_t index = 0; !stop_.load(); ++index) {
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [&] { return stop_.load() || queue_.size() < kMaxQueuedFrames; });
            if (stop_.load()) break;
        }
        auto frame = pool_->Make(pool_, bytes);
        if (!video_->ReadOutExact(frame->pixels.data(), static_cast<DWORD>(bytes))) break;
        frame->width = static_cast<uint32_t>(frame_size_.cx);
        frame->height = static_cast<uint32_t>(frame_size_.cy);
        frame->time = start_ + static_cast<int64_t>(std::llround(index * interval));
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(std::move(frame));
        if (wake_) wake_();
    }
    CheckDecoderExit(*video_);
    std::lock_guard lock(queue_mutex_);
    video_eof_ = true;
    if (wake_) wake_();
}

void FfmpegPlayback::WriteAudio() {
    for (int next = 0; !stop_.load(); next = (next + 1) % kWaveBuffers) {
        WAVEHDR& header = headers_[next];
        // Wait for this buffer to come back from the device.
        while ((header.dwFlags & WHDR_PREPARED) && !(header.dwFlags & WHDR_DONE) && !stop_.load())
            WaitForSingleObject(wave_event_, INFINITE);
        if (stop_.load()) break;
        if (header.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wave_, &header, sizeof(header));
        auto& data = wave_data_[next];
        DWORD filled = 0;
        while (filled < data.size()) {
            const DWORD got = audio_->ReadOut(data.data() + filled, static_cast<DWORD>(data.size()) - filled);
            if (!got) break;
            filled += got;
        }
        filled &= ~3u;   // whole stereo frames
        if (filled) {
            const float gain = gain_.load();
            if (gain < 0.999f) {
                auto* samples = reinterpret_cast<int16_t*>(data.data());
                for (DWORD i = 0; i < filled / 2; ++i)
                    samples[i] = static_cast<int16_t>(std::lround(samples[i] * gain));
            }
            header = {};
            header.lpData = reinterpret_cast<LPSTR>(data.data());
            header.dwBufferLength = filled;
            if (waveOutPrepareHeader(wave_, &header, sizeof(header)) != MMSYSERR_NOERROR ||
                waveOutWrite(wave_, &header, sizeof(header)) != MMSYSERR_NOERROR) {
                failed_ = true;
                audio_->Terminate();
                break;
            }
        }
        if (filled < data.size()) break;   // end of stream
    }
    CheckDecoderExit(*audio_);
    audio_eof_.store(true);
    if (wake_) wake_();
}

void FfmpegPlayback::CheckDecoderExit(ffmpeg::Process& process) {
    while (!stop_.load() && !process.Wait(25)) {}
    if (!stop_.load() && process.ExitCode() != 0) {
        decoder_exit_ = process.ExitCode();
        failed_ = true;
    }
}

void FfmpegPlayback::DrainError(ffmpeg::Process& process) {
    char buffer[4096];
    for (DWORD got; (got = process.ReadErr(buffer, sizeof(buffer))) > 0;) {
        std::lock_guard lock(error_mutex_);
        diagnostic_.append(buffer, got);
        if (diagnostic_.size() > 16384) diagnostic_.erase(0, diagnostic_.size() - 16384);
    }
}

std::string FfmpegPlayback::Diagnostic() {
    std::lock_guard lock(error_mutex_);
    return diagnostic_;
}

bool FfmpegPlayback::AudioDrained() {
    if (!wave_) return true;
    // audio_eof_ is stored after the writer's last header update, so once it
    // reads true only the device still touches the headers (WHDR_DONE).
    if (!audio_eof_.load()) return false;
    for (const auto& header : headers_)
        if ((header.dwFlags & WHDR_PREPARED) && !(header.dwFlags & WHDR_DONE)) return false;
    return true;
}

int64_t FfmpegPlayback::Clock() {
    if (!playing_) return paused_at_;
    if (!wall_clock_ && wave_) {
        if (AudioDrained()) {
            // Audio shorter than the video: continue on the wall clock.
            MMTIME time{TIME_BYTES};
            waveOutGetPosition(wave_, &time, sizeof(time));
            wall_base_ = start_ + static_cast<int64_t>(static_cast<double>(time.u.cb) * kSecond / kBytesPerSecond * rate_);
            wall_tick_ = motion::NowMs();
            wall_clock_ = true;
        } else {
            MMTIME time{TIME_BYTES};
            if (waveOutGetPosition(wave_, &time, sizeof(time)) == MMSYSERR_NOERROR && time.wType == TIME_BYTES)
                return start_ + static_cast<int64_t>(static_cast<double>(time.u.cb) * kSecond / kBytesPerSecond * rate_);
        }
    }
    return wall_base_ + static_cast<int64_t>((motion::NowMs() - wall_tick_) * 10'000.0 * rate_);
}

void FfmpegPlayback::SetPlaying(bool playing) {
    if (playing == playing_) return;
    if (playing && stepped_) {
        // Frame steps moved the picture away from the queued audio.
        Start(shown_time_, rate_, frame_size_, true);
        return;
    }
    if (!playing) paused_at_ = Clock();
    playing_ = playing;
    if (wave_) {
        if (playing) waveOutRestart(wave_);
        else waveOutPause(wave_);
    }
    if (playing) {
        wall_base_ = paused_at_;
        wall_tick_ = motion::NowMs();
    }
}

void FfmpegPlayback::SetVolume(float volume, bool muted) {
    gain_.store(muted ? 0.0f : std::clamp(volume, 0.0f, 1.0f));
}

std::shared_ptr<const FfmpegFrame> FfmpegPlayback::Pump() {
    std::shared_ptr<const FfmpegFrame> show;
    {
        std::lock_guard lock(queue_mutex_);
        if (stepped_frame_) {
            show = std::move(stepped_frame_);
        } else if (!first_shown_) {
            if (!queue_.empty()) {
                show = std::move(queue_.front());
                queue_.pop_front();
                first_shown_ = true;
            }
        } else if (playing_) {
            const int64_t now = Clock();
            // Late frames are dropped; the decoder catches up through back-pressure.
            while (!queue_.empty() && queue_.front()->time <= now) {
                show = std::move(queue_.front());
                queue_.pop_front();
            }
        }
    }
    if (show) {
        shown_time_ = show->time;
        queue_cv_.notify_one();
    }
    return show;
}

bool FfmpegPlayback::Step() {
    if (playing_) return false;
    std::lock_guard lock(queue_mutex_);
    if (queue_.empty()) return false;
    stepped_frame_ = std::move(queue_.front());
    queue_.pop_front();
    paused_at_ = stepped_frame_->time;
    stepped_ = true;
    queue_cv_.notify_one();
    return true;
}

int64_t FfmpegPlayback::Position() {
    int64_t now = playing_ ? Clock() : paused_at_;
    if (info_.video && !playing_ && first_shown_) now = (std::max)(now, shown_time_);
    if (info_.duration > 0) now = (std::min)(now, info_.duration);
    return (std::max)(int64_t{0}, now);
}

bool FfmpegPlayback::Ended() {
    if (failed_) return false;
    bool video_done = true;
    {
        std::lock_guard lock(queue_mutex_);
        if (info_.video) video_done = video_eof_ && queue_.empty() && !stepped_frame_;
    }
    return video_done && AudioDrained();
}

} // namespace pulse::ui
