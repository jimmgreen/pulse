#pragma once
// Running the preview pack's ffmpeg / ffprobe.
//
// Shared by the preview host (thumbnails, details) and the Quick Look
// playback fallback in the UI process. A tool runs inside a kill-on-close Job
// Object with a memory cap and inherits only its three standard handles, so a
// crash, hang or exit of the caller never leaves an ffmpeg behind.
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::ffmpeg {

inline constexpr std::wstring_view kVideoExtensions[] = {
    L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi", L".wmv", L".flv",
    L".f4v", L".mpg", L".mpeg", L".m2v", L".ts", L".m2t", L".mts", L".m2ts", L".vob",
    L".3gp", L".3g2", L".asf", L".ogv", L".rm", L".rmvb", L".mxf", L".divx", L".dv",
    L".y4m", L".mjpeg", L".hevc", L".h264", L".264", L".265"};
inline constexpr std::wstring_view kAudioExtensions[] = {
    L".mp3", L".wav", L".flac", L".m4a", L".m4b", L".aac", L".wma", L".ogg", L".oga",
    L".opus", L".aif", L".aiff", L".aifc", L".ape", L".wv", L".tta", L".dsf", L".dff", L".mka",
    L".ac3", L".dts", L".amr", L".caf", L".mpc"};

// Containers and audio formats the media pack handles (lower case, with dot).
bool IsVideoExtension(std::wstring_view extension);
bool IsAudioExtension(std::wstring_view extension);

// Command-line quoting for CreateProcess (CommandLineToArgvW rules).
std::wstring QuoteArgument(const std::wstring& argument);

// The child died before its main(): DLL initialisation failed (0xC0000142)
// or the machine was out of commit (0xC0000017, 0xC000012D). Happens when
// many decoders start at once on a loaded machine; worth one more try.
inline bool StartupFailed(unsigned long exit_code) {
    return exit_code == 0xC0000142ul || exit_code == 0xC0000017ul || exit_code == 0xC000012Dul;
}
// ffmpeg's own path syntax: "file:" stops "C:..." or names containing ':'
// from being read as protocols; \\?\ prefixes are dropped when not needed.
std::wstring InputArgument(const std::wstring& path);

struct LaunchOptions {
    DWORD priority = BELOW_NORMAL_PRIORITY_CLASS;
    SIZE_T memory_limit = static_cast<SIZE_T>(1024) * 1024 * 1024;
    DWORD stdout_buffer = 1u << 20;
    bool discard_stderr = false;   // long-running tools: nobody drains stderr
};

// A started tool. Destroying it (or Terminate) kills the process.
class Process {
public:
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process();

    bool Start(const std::wstring& exe, const std::wstring& arguments, const LaunchOptions& options = {});
    // Blocking read from stdout / stderr; 0 at end of stream or after Terminate.
    DWORD ReadOut(void* buffer, DWORD size);
    DWORD ReadErr(void* buffer, DWORD size);
    // Reads exactly `size` bytes unless the stream ends first.
    bool ReadOutExact(void* buffer, DWORD size);
    bool Wait(DWORD timeout_ms);
    void Terminate(DWORD code = ERROR_CANCELLED);
    DWORD ExitCode() const;
    bool started() const noexcept { return process_ != nullptr; }

private:
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE out_ = nullptr;
    HANDLE err_ = nullptr;
};

// ---- ffmpeg's log ("ffmpeg -i file" prints the input description) ---------

// "Duration: 00:42:18.12," ; 0 when absent or N/A.
int64_t ParseDuration100ns(std::string_view log);
uint32_t ParseDurationMs(std::string_view log);
// First "Video: ... 3840x2160" stream size.
bool ParseVideoSize(std::string_view log, UINT& width, UINT& height);
// True when the first video stream carries a 90/270-degree display rotation.
bool ParseVideoQuarterTurn(std::string_view log);
// First video stream's "29.97 fps" (else "25 tbr"); 0 when absent.
double ParseFrameRate(std::string_view log);
// First video stream's sample aspect ratio ("SAR 64:45"); 1:1 when absent.
void ParseSampleAspect(std::string_view log, UINT& num, UINT& den);
// Whether a stream of the kind ("Video" / "Audio") is listed. Cover art
// ("(attached pic)") does not count as video.
bool HasStream(std::string_view log, std::string_view kind);

} // namespace pulse::ffmpeg
