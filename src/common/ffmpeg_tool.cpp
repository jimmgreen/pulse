#include "ffmpeg_tool.h"
#include "path_utils.h"
#include "preview_packs.h"
#include <algorithm>
#include <cmath>
#include <charconv>
#include <iterator>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

namespace pulse::ffmpeg {

bool IsVideoExtension(std::wstring_view e) {
    return std::find(std::begin(kVideoExtensions), std::end(kVideoExtensions), e) != std::end(kVideoExtensions);
}

bool IsAudioExtension(std::wstring_view e) {
    return std::find(std::begin(kAudioExtensions), std::end(kAudioExtensions), e) != std::end(kAudioExtensions);
}

std::wstring QuoteArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < argument.size() && argument[i] == L'\\') { ++i; ++slashes; }
        if (i == argument.size()) { out.append(slashes * 2, L'\\'); break; }
        if (argument[i] == L'"') { out.append(slashes * 2 + 1, L'\\'); out += L'"'; }
        else { out.append(slashes, L'\\'); out += argument[i]; }
    }
    out += L'"';
    return out;
}

std::wstring InputArgument(const std::wstring& path) {
    std::wstring plain = path.size() < MAX_PATH ? path::StripExtendedPathPrefix(path) : path;
    return L"file:" + plain;
}

Process::~Process() {
    Terminate();
    if (out_) CloseHandle(out_);
    if (err_) CloseHandle(err_);
    if (process_) CloseHandle(process_);
    if (job_) CloseHandle(job_);
}

bool Process::Start(const std::wstring& exe, const std::wstring& arguments, const LaunchOptions& options) {
    if (process_) return false;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out_write = nullptr, err_write = nullptr;
    if (!CreatePipe(&out_, &out_write, &inherit, options.stdout_buffer)) return false;
    if (!CreatePipe(&err_, &err_write, &inherit, 64 * 1024)) {
        CloseHandle(out_write);
        return false;
    }
    SetHandleInformation(out_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &inherit, OPEN_EXISTING, 0, nullptr);
    if (null_in == INVALID_HANDLE_VALUE) {
        CloseHandle(out_write);
        CloseHandle(err_write);
        return false;
    }
    if (options.discard_stderr) {
        CloseHandle(err_write);
        CloseHandle(err_);
        err_ = nullptr;
        err_write = null_in;
    }

    // Only the three standard handles are inherited, never a caller's pipe.
    HANDLE inherited[3] = {null_in, out_write, err_write};
    const DWORD inherited_count = err_write == null_in ? 2 : 3;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_storage(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
    const bool attrs_initialized = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) != FALSE;
    const bool attrs_ok = attrs_initialized &&
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                  inherited_count * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = null_in;
    si.StartupInfo.hStdOutput = out_write;
    si.StartupInfo.hStdError = err_write;
    si.lpAttributeList = attrs_ok ? attrs : nullptr;

    const auto cleanup = [&] {
        if (attrs_initialized) DeleteProcThreadAttributeList(attrs);
        CloseHandle(out_write);
        if (err_write != null_in) CloseHandle(err_write);
        CloseHandle(null_in);
    };
    const auto failed = [&](DWORD error) {
        if (out_) { CloseHandle(out_); out_ = nullptr; }
        if (err_) { CloseHandle(err_); err_ = nullptr; }
        if (job_) { CloseHandle(job_); job_ = nullptr; }
        SetLastError(error);
        return false;
    };
    if (!attrs_ok) {
        const DWORD error = GetLastError();
        cleanup();
        return failed(error);
    }
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_) {
        const DWORD error = GetLastError();
        cleanup();
        return failed(error);
    }
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
            (options.memory_limit ? JOB_OBJECT_LIMIT_PROCESS_MEMORY : 0);
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        limits.ProcessMemoryLimit = options.memory_limit;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            const DWORD error = GetLastError();
            cleanup();
            return failed(error);
        }
    }

    // Children inherit the error mode. Without SEM_FAILCRITICALERRORS a tool
    // that cannot start (DLL init failed under memory pressure, 0xC0000142)
    // puts a system "application error" box in front of the user instead of
    // just failing; the caller already handles the exit code.
    if (const UINT mode = GetErrorMode(); (mode & SEM_FAILCRITICALERRORS) == 0)
        SetErrorMode(mode | SEM_FAILCRITICALERRORS);
    std::wstring command = QuoteArgument(exe) + L" " + arguments;
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | options.priority |
        CREATE_UNICODE_ENVIRONMENT | (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const std::wstring directory = packs::DirectoryOf(exe);
    const bool started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                        directory.empty() ? nullptr : directory.c_str(),
                                        &si.StartupInfo, &pi) != FALSE;
    const DWORD error = GetLastError();
    cleanup();
    if (!started) return failed(error);
    if (!AssignProcessToJobObject(job_, pi.hProcess) || ResumeThread(pi.hThread) == DWORD(-1)) {
        const DWORD start_error = GetLastError();
        TerminateProcess(pi.hProcess, start_error);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return failed(start_error);
    }
    process_ = pi.hProcess;
    CloseHandle(pi.hThread);
    return true;
}

DWORD Process::ReadOut(void* buffer, DWORD size) {
    DWORD got = 0;
    if (!out_ || !ReadFile(out_, buffer, size, &got, nullptr)) return 0;
    return got;
}

DWORD Process::ReadErr(void* buffer, DWORD size) {
    DWORD got = 0;
    if (!err_ || !ReadFile(err_, buffer, size, &got, nullptr)) return 0;
    return got;
}

bool Process::ReadOutExact(void* buffer, DWORD size) {
    auto* p = static_cast<uint8_t*>(buffer);
    while (size) {
        const DWORD got = ReadOut(p, size);
        if (!got) return false;
        p += got;
        size -= got;
    }
    return true;
}

bool Process::Wait(DWORD timeout_ms) {
    return process_ && WaitForSingleObject(process_, timeout_ms) == WAIT_OBJECT_0;
}

void Process::Terminate(DWORD code) {
    if (!process_) return;
    if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) return;
    if (job_) TerminateJobObject(job_, code);
    else TerminateProcess(process_, code);
    WaitForSingleObject(process_, 2000);
}

DWORD Process::ExitCode() const {
    DWORD code = STILL_ACTIVE;
    if (process_) GetExitCodeProcess(process_, &code);
    return code;
}

int64_t ParseDuration100ns(std::string_view log) {
    size_t at = 0;
    while (at < log.size() && !log.substr(at).starts_with("  Duration: ")) {
        at = log.find('\n', at);
        if (at == std::string_view::npos) return 0;
        ++at;
    }
    if (at == log.size()) return 0;
    at += 2;
    const char* p = log.data() + at + 10;
    const char* end = log.data() + log.size();
    uint64_t hours = 0;
    unsigned minutes = 0, seconds = 0;
    auto number = [&](auto& value) {
        const auto result = std::from_chars(p, end, value);
        p = result.ptr;
        return result.ec == std::errc{};
    };
    if (!number(hours) || p == end || *p++ != ':' || !number(minutes) ||
        p == end || *p++ != ':' || !number(seconds) || minutes >= 60 || seconds >= 60) return 0;
    int64_t fraction = 0;
    if (p != end && *p == '.') {
        ++p;
        for (int scale = 1000000; p != end && *p >= '0' && *p <= '9'; ++p, scale /= 10)
            fraction += (*p - '0') * scale;
    }
    constexpr int64_t hour_ticks = 3600ll * 10000000;
    const int64_t tail = (minutes * 60ll + seconds) * 10000000 + fraction;
    if (hours > static_cast<uint64_t>((INT64_MAX - tail) / hour_ticks)) return 0;
    return static_cast<int64_t>(hours) * hour_ticks + tail;
}

uint32_t ParseDurationMs(std::string_view log) {
    return static_cast<uint32_t>((std::min<int64_t>)(ParseDuration100ns(log) / 10000, UINT32_MAX));
}

namespace {
std::string_view StreamLine(std::string_view log, std::string_view kind) {
    for (size_t start = 0; start < log.size();) {
        const size_t end = log.find('\n', start);
        const auto line = log.substr(start, end == std::string_view::npos ? log.size() - start : end - start);
        if (line.starts_with("  Stream #")) {
            // Metadata and its continuation lines are indented further. Only
            // accept the type immediately following the stream's identifier.
            const size_t type = line.find(": ", 10);
            if (type != std::string_view::npos && line.substr(type + 2).starts_with(std::string(kind) + ": ") &&
                line.find("(attached pic)") == std::string_view::npos)
                return line.substr(type + 2);
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}
// Lines of real video streams (cover art excluded).
template <typename F>
bool ForEachVideoLine(std::string_view log, F&& visit) {
    const auto line = StreamLine(log, "Video");
    return !line.empty() && visit(line);
}
} // namespace

bool ParseVideoSize(std::string_view log, UINT& width, UINT& height) {
    return ForEachVideoLine(log, [&](std::string_view line) {
        // The size token is digits 'x' digits, delimited by space/comma.
        for (size_t i = 1; i + 2 < line.size(); ++i) {
            if (line[i] != 'x' || !isdigit(static_cast<unsigned char>(line[i - 1])) ||
                !isdigit(static_cast<unsigned char>(line[i + 1]))) continue;
            size_t a = i; while (a > 0 && isdigit(static_cast<unsigned char>(line[a - 1]))) --a;
            size_t b = i + 1; while (b < line.size() && isdigit(static_cast<unsigned char>(line[b]))) ++b;
            if (a == 0 || line[a - 1] != ' ') continue;
            if (b < line.size() && line[b] != ' ' && line[b] != ',') continue;
            const UINT w = static_cast<UINT>(strtoul(std::string(line.substr(a, i - a)).c_str(), nullptr, 10));
            const UINT h = static_cast<UINT>(strtoul(std::string(line.substr(i + 1, b - i - 1)).c_str(), nullptr, 10));
            if (w > 0 && h > 0 && w <= 65535 && h <= 65535) { width = w; height = h; return true; }
        }
        return false;
    });
}

bool ParseVideoQuarterTurn(std::string_view log) {
    const std::string_view video_line = StreamLine(log, "Video");
    if (video_line.empty()) return false;
    const size_t stream_pos = static_cast<size_t>(video_line.data() - log.data());
    size_t pos = log.find('\n', stream_pos);
    if (pos == std::string_view::npos) return false;
    ++pos;

    auto is_quarter_turn = [](std::string_view num_text) -> bool {
        while (!num_text.empty() && (num_text.front() == ' ' || num_text.front() == '\t'))
            num_text.remove_prefix(1);
        if (num_text.empty()) return false;
        char buf[32]{};
        const size_t n = (std::min)(num_text.size(), sizeof(buf) - 1);
        std::copy_n(num_text.data(), n, buf);
        char* end = nullptr;
        const double val = strtod(buf, &end);
        if (end == buf) return false;
        const long deg = std::lround(std::fabs(val)) % 360;
        return deg == 90 || deg == 270;
    };

    while (pos < log.size()) {
        const size_t end = log.find('\n', pos);
        std::string_view line = log.substr(pos, end == std::string_view::npos ? log.size() - pos : end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = end == std::string_view::npos ? log.size() : end + 1;
        if (line.empty()) continue;
        // Stream sub-blocks (Metadata / Side data) are indented by at least 4 spaces.
        size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) ++indent;
        if (indent < 4) break;
        const std::string_view trimmed = line.substr(indent);
        constexpr std::string_view kDisplayMatrix = "displaymatrix: rotation of ";
        if (const size_t dm = trimmed.find(kDisplayMatrix); dm != std::string_view::npos) {
            if (is_quarter_turn(trimmed.substr(dm + kDisplayMatrix.size()))) return true;
        }
        if (trimmed.starts_with("rotate")) {
            size_t rest = 6;
            while (rest < trimmed.size() && (trimmed[rest] == ' ' || trimmed[rest] == '\t')) ++rest;
            if (rest < trimmed.size() && trimmed[rest] == ':' && is_quarter_turn(trimmed.substr(rest + 1)))
                return true;
        }
    }
    return false;
}

double ParseFrameRate(std::string_view log) {
    double rate = 0;
    ForEachVideoLine(log, [&](std::string_view line) {
        for (const char* unit : {" fps", " tbr"}) {
            const size_t at = line.find(unit);
            if (at == std::string_view::npos) continue;
            size_t a = at;
            while (a > 0 && (isdigit(static_cast<unsigned char>(line[a - 1])) || line[a - 1] == '.' ||
                             line[a - 1] == 'k')) --a;
            std::string number(line.substr(a, at - a));
            double value = strtod(number.c_str(), nullptr);
            if (!number.empty() && number.back() == 'k') value *= 1000.0;
            if (value >= 0.0001 && value < 1000.0) { rate = value; return true; }
        }
        return false;
    });
    return rate;
}

void ParseSampleAspect(std::string_view log, UINT& num, UINT& den) {
    num = den = 1;
    ForEachVideoLine(log, [&](std::string_view line) {
        const size_t at = line.find("SAR ");
        if (at == std::string_view::npos) return true;   // first video stream only
        unsigned a = 0, b = 0;
        const std::string token(line.substr(at + 4, 16));
        if (sscanf_s(token.c_str(), "%u:%u", &a, &b) == 2 && a && b && a < 100000 && b < 100000) {
            num = a;
            den = b;
        }
        return true;
    });
}

bool HasStream(std::string_view log, std::string_view kind) {
    return !StreamLine(log, kind).empty();
}

} // namespace pulse::ffmpeg
