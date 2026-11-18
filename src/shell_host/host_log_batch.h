#pragma once
#include <windows.h>
#include <algorithm>
#include <mutex>
#include <cwchar>
#include <new>
#include <string>
#include <vector>

namespace pulse::shell {
struct HostLogStats {
    unsigned opens = 0, writes = 0, rotations = 0, failures = 0;
    DWORD error = ERROR_SUCCESS;
    size_t peak_rows = 0, peak_bytes = 0;
};

// Synchronous and scoped: no worker, unbounded queue or cross-call file handle.
class HostLogBatch {
public:
#ifdef PULSE_HOST_LOG_BATCH_TEST
    inline static bool fail_after_open_for_test = false;
    inline static bool fail_after_committed_block_for_test = false;
#endif
    HostLogBatch(std::wstring path, HostLogStats* stats = nullptr, size_t rows = 64,
                 LONGLONG rotate_bytes = 1024 * 1024, std::mutex* mutex = nullptr)
        : path_(std::move(path)), stats_(stats), rows_((std::max)(size_t(1), (std::min)(rows, size_t(64)))),
          rotate_bytes_(rotate_bytes), mutex_(mutex) {}
    ~HostLogBatch() {
        try { Flush(); } catch (...) { Fail(ERROR_NOT_ENOUGH_MEMORY); }
        if (!ok_) {
            wchar_t summary[128]{};
            swprintf_s(summary, L"pulse_shell_host: log flush failed, error=%lu\n", error_);
            OutputDebugStringW(summary);
        }
    }
    HostLogBatch(const HostLogBatch&) = delete;
    HostLogBatch& operator=(const HostLogBatch&) = delete;
    static std::string Encode(const wchar_t* message) {
        size_t length = 0;
        while (message && message[length] && length < 2048) ++length;
        if (length && message[length - 1] >= 0xD800 && message[length - 1] <= 0xDBFF) --length;
        std::wstring line = L"[" + std::to_wstring(GetCurrentProcessId()) + L"] ";
        if (length) line.append(message, length);
        line += L'\n';
        char buffer[8192];
        const int count = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
            buffer, static_cast<int>(sizeof(buffer)), nullptr, nullptr);
        return count > 0 ? std::string(buffer, count) : std::string();
    }
    bool Add(const wchar_t* message) {
        if (path_.empty()) return true;
        auto line = Encode(message);
        if (line.empty()) { Fail(GetLastError()); return false; }
        if (!lines_.empty() && (lines_.size() >= rows_ || bytes_ + line.size() > 32768)) Flush();
        bytes_ += line.size(); lines_.push_back(std::move(line));
        if (stats_) {
            stats_->peak_rows = (std::max)(stats_->peak_rows, lines_.size());
            stats_->peak_bytes = (std::max)(stats_->peak_bytes, bytes_);
        }
        return ok_;
    }
    bool Flush() {
        if (lines_.empty()) return ok_;
        std::unique_lock<std::mutex> lock;
        if (mutex_) lock = std::unique_lock<std::mutex>(*mutex_);
        HANDLE file = Open();
        struct FileGuard {
            HANDLE& file;
            ~FileGuard() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
        } guard{file};
#ifdef PULSE_HOST_LOG_BATCH_TEST
        if (fail_after_open_for_test) {
            fail_after_open_for_test = false;
            throw std::bad_alloc();
        }
#endif
        LARGE_INTEGER size{};
        if (file != INVALID_HANDLE_VALUE && !GetFileSizeEx(file, &size)) { Fail(GetLastError()); }
        size_t next = 0;
        while (file != INVALID_HANDLE_VALUE && next < lines_.size()) {
            if (size.QuadPart > rotate_bytes_) {
                const HANDLE previous = file;
                file = INVALID_HANDLE_VALUE;
                if (!CloseHandle(previous)) Fail(GetLastError());
                if (stats_) ++stats_->rotations;
                const bool moved = MoveFileExW(path_.c_str(), (path_ + L".1").c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
                // Preserve legacy behavior: failed rotation still appends, and the next line retries.
                if (!moved) Fail(GetLastError());
                file = Open();
                if (file == INVALID_HANDLE_VALUE) break;
                if (!GetFileSizeEx(file, &size)) { Fail(GetLastError()); break; }
            }
            std::string block;
            do {
                block += lines_[next++];
                size.QuadPart += static_cast<LONGLONG>(lines_[next - 1].size());
            } while (next < lines_.size() && size.QuadPart <= rotate_bytes_);
            size_t offset = 0;
            while (offset < block.size()) {
                DWORD written = 0;
                if (stats_) ++stats_->writes;
                if (!WriteFile(file, block.data() + offset, static_cast<DWORD>(block.size() - offset), &written, nullptr) || !written) {
                    Fail(GetLastError() ? GetLastError() : ERROR_WRITE_FAULT); break;
                }
                offset += written;
            }
            if (offset != block.size()) break;
            // std::string moves during vector erase do not allocate. Commit
            // progress before a later rotation/allocation can throw.
            bytes_ -= block.size();
            lines_.erase(lines_.begin(), lines_.begin() + next);
            next = 0;
#ifdef PULSE_HOST_LOG_BATCH_TEST
            if (fail_after_committed_block_for_test) {
                fail_after_committed_block_for_test = false;
                throw std::bad_alloc();
            }
#endif
        }
        if (file != INVALID_HANDLE_VALUE) {
            const HANDLE previous = file;
            file = INVALID_HANDLE_VALUE;
            if (!CloseHandle(previous)) Fail(GetLastError());
        }
        lines_.clear(); bytes_ = 0;
        return ok_;
    }
private:
    HANDLE Open() {
        if (stats_) ++stats_->opens;
        HANDLE file = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) Fail(GetLastError());
        return file;
    }
    void Fail(DWORD error) {
        ok_ = false;
        error_ = error;
        if (stats_) { ++stats_->failures; stats_->error = error; }
    }
    std::wstring path_;
    HostLogStats* stats_;
    size_t rows_, bytes_ = 0;
    LONGLONG rotate_bytes_;
    bool ok_ = true;
    DWORD error_ = ERROR_SUCCESS;
    std::mutex* mutex_;
    std::vector<std::string> lines_;
};
}
