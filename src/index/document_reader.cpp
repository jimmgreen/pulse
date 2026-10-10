#include "document_reader.h"
#include "document_protocol.h"
#include "document_admission.h"
#include "../common/runtime_log.h"
#include <shlobj.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

namespace pulse::index {
namespace {
static_assert(document::kProcessSlots * document::kMemoryBytes <= 1024ull * 1024 * 1024);
class Handle {
public:
    HANDLE value = nullptr;
    ~Handle() { Reset(); }
    void Reset(HANDLE next = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
};
std::wstring Extension(const std::wstring& path) {
    const auto dot = path.find_last_of(L'.');
    const auto slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    auto ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return ext;
}
bool Equal(std::wstring_view a, std::wstring_view b) noexcept {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool AdmissionDirectory(std::wstring& directory, DWORD& error) {
    wchar_t isolated[32768]{};
    const DWORD isolated_length = GetEnvironmentVariableW(L"PULSE_DOCUMENT_ADMISSION_DIR", isolated, ARRAYSIZE(isolated));
    if (isolated_length >= ARRAYSIZE(isolated)) { error = ERROR_BAD_PATHNAME; return false; }
    std::wstring resolved;
    if (isolated_length) {
        // Explicit test runs use an existing isolated directory. Production
        // instances use the shared per-user directory below.
        const auto attributes = GetFileAttributesW(isolated);
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = ERROR_PATH_NOT_FOUND; return false;
        }
        directory = isolated;
        return true;
    }
    PWSTR local = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local);
    if (FAILED(result)) { error = ERROR_PATH_NOT_FOUND; return false; }
    resolved = local;
    CoTaskMemFree(local);
    for (const auto* part : {L"\\Pulse", L"\\document-slots-v1"}) {
        resolved += part;
        if (!CreateDirectoryW(resolved.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            error = GetLastError(); return false;
        }
    }
    directory = std::move(resolved);
    return true;
}
class DocumentWorker {
public:
    ~DocumentWorker() { Stop(); }
    void ReclaimIdle() {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (lock.owns_lock() && process_.value && !admission_directory_.empty() &&
            document::AdmissionContended(admission_directory_)) Stop();
    }
    bool Read(const std::wstring& path, uint64_t maximum_bytes, std::wstring& output,
              uint64_t& bytes_read, DWORD& error,
              const std::function<bool()>& cancelled, std::wstring_view stop_needle, bool case_sensitive,
              DocumentReadMetrics* metrics) {
        DocumentReadMetrics own_metrics;
        struct SaveMetrics {
            DocumentReadMetrics* output;
            DocumentReadMetrics& value;
            ~SaveMetrics() { if (output) AddReaderMetrics(*output, value); }
        } save_metrics{metrics, own_metrics};
        DocumentMetricScope queue_time(own_metrics.reader_queue_us);
        std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(25))) {
            if (cancelled && cancelled()) { error = ERROR_CANCELLED; return false; }
        }
        queue_time.Finish();
        if (cancelled && cancelled()) { error = ERROR_CANCELLED; return false; }
        const ULONGLONG deadline = GetTickCount64() + document::kTimeoutMs;
        auto bounded_cancelled = [&] { return (cancelled && cancelled()) || GetTickCount64() >= deadline; };
        document::Begin initial;
        DWORD initial_error = ERROR_SUCCESS;
        for (unsigned attempt = 0; attempt != 2; ++attempt) {
            if (bounded_cancelled()) {
                error = cancelled && cancelled() ? ERROR_CANCELLED : ERROR_TIMEOUT;
                Stop(&own_metrics); return false;
            }
            if (!Start(error, bounded_cancelled, own_metrics)) {
                if (cancelled && cancelled()) error = ERROR_CANCELLED;
                else if (GetTickCount64() >= deadline) error = ERROR_TIMEOUT;
                return false;
            }
            if (bounded_cancelled()) {
                error = cancelled && cancelled() ? ERROR_CANCELLED : ERROR_TIMEOUT;
                Stop(&own_metrics); return false;
            }
            if (attempt && document::PdfEngineMode() != initial.config_mode) {
                ++own_metrics.fresh_filter_skips; error = initial_error; Stop(&own_metrics); return false;
            }
            document::Begin begun;
            bool source_rejected = false;
            const bool ok = ReadAttempt(path, maximum_bytes, output, bytes_read, error, cancelled,
                stop_needle, case_sensitive, deadline, attempt ? &initial : nullptr,
                begun, source_rejected, own_metrics, metrics);
            if (cancelled && cancelled()) {
                output.clear(); bytes_read = 0; error = ERROR_CANCELLED; Stop(&own_metrics); return false;
            }
            if (ok) {
                if (attempt) ++own_metrics.fresh_filter_recoveries;
                return true;
            }
            if (attempt) {
                if (source_rejected && error != ERROR_TIMEOUT) { ++own_metrics.fresh_filter_skips; error = initial_error; }
                if (IsOom(error, own_metrics)) Stop(&own_metrics);
                return false;
            }
            if (!IsOom(error, own_metrics) || !Equal(Extension(path), L".pdf") ||
                begun.config_mode != 0 || begun.engine != 1) return false;
            initial_error = error; initial = begun;
            own_metrics.fresh_filter_first_error = error;
            own_metrics.fresh_filter_first_exit = own_metrics.child_exit_known ? own_metrics.child_exit_code : 0;
            Stop(&own_metrics);
            if (cancelled && cancelled()) { error = ERROR_CANCELLED; return false; }
            if (GetTickCount64() >= deadline) { error = ERROR_TIMEOUT; return false; }
            if (!(initial.flags & 1) || document::PdfEngineMode() != initial.config_mode) {
                ++own_metrics.fresh_filter_skips; return false;
            }
        }
        return false;
    }
private:
    static bool IsOom(DWORD error, const DocumentReadMetrics& metrics) noexcept {
        return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ||
            ((error == ERROR_BROKEN_PIPE || error == ERROR_PROCESS_ABORTED) && metrics.child_exit_known &&
             metrics.child_exit_code == document::kPdfiumOomExitCode);
    }
    bool ReadAttempt(const std::wstring& path, uint64_t maximum_bytes, std::wstring& output,
        uint64_t& bytes_read, DWORD& error, const std::function<bool()>& cancelled,
        std::wstring_view stop_needle, bool case_sensitive, ULONGLONG deadline,
        const document::Begin* expected, document::Begin& begun, bool& source_rejected,
        DocumentReadMetrics& own_metrics, DocumentReadMetrics* metrics) {
        document::Request request;
        request.path_chars = static_cast<uint32_t>(path.size());
        request.maximum_bytes = (std::min)(maximum_bytes, document::kMaximumFileBytes);
        if (stop_needle.size() > 4096) stop_needle = {};
        request.stop_needle_chars = static_cast<uint32_t>(stop_needle.size());
        request.flags = case_sensitive ? 1u : 0u;
        if (expected) {
            request.flags |= 2;
            request.expected_version = expected->version; request.expected_config = expected->config_mode;
            request.override_engine = 2;
        }
        std::vector<uint8_t> payload(sizeof(request) + (path.size() + stop_needle.size()) * sizeof(wchar_t));
        memcpy(payload.data(), &request, sizeof(request));
        memcpy(payload.data() + sizeof(request), path.data(), path.size() * sizeof(wchar_t));
        if (!stop_needle.empty()) memcpy(payload.data() + sizeof(request) + path.size() * sizeof(wchar_t),
            stop_needle.data(), stop_needle.size() * sizeof(wchar_t));
        ++own_metrics.read_attempts;
        attempt_cpu_running_ = ProcessCpu(attempt_cpu_start_);
        DWORD written = 0;
        // The request remains below the 128 KiB pipe buffer.
        if (!WriteFile(input_.value, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) || written != payload.size()) {
            error = ERROR_BROKEN_PIPE; CaptureFailure(own_metrics, 1, error); Stop(&own_metrics); return false;
        }
        SetEvent(request_ready_.value);
        document::Begin begin;
        if (!Receive(&begin, sizeof(begin), deadline, cancelled, error, own_metrics)) {
            CaptureFailure(own_metrics, 6, error); Stop(&own_metrics); return false;
        }
        if (begin.magic != document::kMagic || begin.size != sizeof(begin) || begin.flags > 1 ||
            begin.engine > 2 || begin.config_mode > 3 || ((begin.flags & 1) && (!begin.version.id || !begin.version.changed)) ||
            (!begin.error && begin.version.bytes > request.maximum_bytes)) {
            error = ERROR_INVALID_DATA; CaptureFailure(own_metrics, 6, error); Stop(&own_metrics); return false;
        }
        begun = begin;
        if (expected) {
            source_rejected = begin.error != ERROR_SUCCESS;
            if (!begin.error && (!(begin.flags & 1) || begin.version != expected->version || begin.config_mode != expected->config_mode || begin.engine != 2)) {
                error = ERROR_INVALID_DATA; CaptureFailure(own_metrics, 6, error); Stop(&own_metrics); return false;
            }
            if (!source_rejected) ++own_metrics.fresh_filter_fallbacks;
        }
        document::Response response;
        if (!Receive(&response, sizeof(response), deadline, cancelled, error, own_metrics)) {
            CaptureFailure(own_metrics, 2, error); Stop(&own_metrics); return false;
        }
        if (response.magic != document::kMagic || response.text_chars > document::kMaximumTextChars || response.reserved ||
            response.file_bytes > request.maximum_bytes || (response.error && response.text_chars) ||
            (begin.error && response.error != begin.error)) {
            error = ERROR_INVALID_DATA; CaptureFailure(own_metrics, 2, error); Stop(&own_metrics); return false;
        }
        if (metrics) AddExtractionMetrics(*metrics, response.metrics);
        std::wstring body(response.text_chars, L'\0');
        if (!Receive(body.data(), response.text_chars * sizeof(wchar_t), deadline, cancelled, error, own_metrics)) {
            CaptureFailure(own_metrics, 3, error); Stop(&own_metrics); return false;
        }
        document::Completion completion;
        if (!Receive(&completion, sizeof(completion), deadline, cancelled, error, own_metrics)) {
            CaptureFailure(own_metrics, 5, error); Stop(&own_metrics); return false;
        }
        const bool known = (completion.flags & 1) != 0;
        const bool retire = (completion.flags & 2) != 0;
        if (completion.magic != document::kMagic || completion.size != sizeof(completion) || completion.flags > 3 ||
            completion.reserved || completion.private_bytes > document::kMemoryBytes || (!known && completion.private_bytes) ||
            retire != (known && completion.private_bytes >= document::kRetirePrivateBytes)) {
            error = ERROR_INVALID_DATA; CaptureFailure(own_metrics, 5, error); Stop(&own_metrics); return false;
        }
        if (known) { own_metrics.released_private_bytes = completion.private_bytes; own_metrics.released_memory_known = 1; }
        if (response.error == ERROR_NOT_ENOUGH_MEMORY || response.error == ERROR_OUTOFMEMORY)
            CaptureFailure(own_metrics, 4, response.error);
        FinishAttemptCpu(own_metrics);
        if (retire) { ++own_metrics.memory_recycles; Stop(&own_metrics); }
        else CompleteRead(own_metrics);
        error = response.error;
        if (error) return false;
        output = std::move(body); bytes_read = response.file_bytes;
        return true;
    }
    bool ProcessCpu(uint64_t& value) const noexcept {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!process_.value || !GetProcessTimes(process_.value, &created, &exited, &kernel, &user)) return false;
        value = (((uint64_t{kernel.dwHighDateTime} << 32) | kernel.dwLowDateTime) +
            ((uint64_t{user.dwHighDateTime} << 32) | user.dwLowDateTime)) / 10;
        return true;
    }
    void FinishAttemptCpu(DocumentReadMetrics& metrics) noexcept {
        uint64_t end = 0;
        if (attempt_cpu_running_ && ProcessCpu(end) && end >= attempt_cpu_start_)
            metrics.attempt_cpu_us += end - attempt_cpu_start_;
        attempt_cpu_running_ = false;
    }
    void CaptureFailureState(DocumentReadMetrics& metrics, uint64_t stage, DWORD error) noexcept {
        metrics.child_failure_stage = stage; // 1=request, 2=header, 3=body, 4=allocation, 5=completion, 6=begin.
        if (!process_.value) return;
        // A broken pipe may race the process exit. Wait only briefly for that
        // case; cancellation/timeout must preserve the pre-termination state.
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PROCESS_ABORTED)
            WaitForSingleObject(process_.value, 25);
        DWORD exit_code = 0;
        if (GetExitCodeProcess(process_.value, &exit_code)) {
            metrics.child_exit_known = 1;
            metrics.child_exit_code = exit_code;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if (job_.value && QueryInformationJobObject(job_.value, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits), nullptr)) {
            metrics.child_peak_private_bytes = (std::max)(metrics.child_peak_private_bytes, uint64_t{limits.PeakProcessMemoryUsed});
            metrics.child_memory_known = 1;
        }
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (K32GetProcessMemoryInfo(process_.value, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
            metrics.child_private_bytes = memory.PrivateUsage;
            metrics.child_peak_private_bytes = (std::max)(metrics.child_peak_private_bytes, uint64_t{memory.PeakPagefileUsage});
            metrics.child_memory_known |= 2;
        }
    }
    void CaptureFailure(DocumentReadMetrics& metrics, uint64_t stage, DWORD error) noexcept {
        CaptureFailureState(metrics, stage, error);
        // Parent-side, numeric-only evidence. Pulse.Document.exe is short-lived
        // and intentionally has no logger of its own: one runtime file per child
        // would evict long-lived app/service logs from the shared retention.
        const bool cancelled = error == ERROR_CANCELLED;
        diagnostics::runtime::Event(cancelled ? "document_child_cancelled" : "document_child_failure", {
            {"stage", stage}, {"error", error}, {"exit_known", metrics.child_exit_known},
            {"exit_code", metrics.child_exit_code}, {"memory_known", metrics.child_memory_known},
            {"peak_private_bytes", metrics.child_peak_private_bytes}},
            cancelled ? diagnostics::runtime::Level::Info :
            stage == 4 ? diagnostics::runtime::Level::Warning : diagnostics::runtime::Level::Error);
    }
    void CompleteRead(DocumentReadMetrics& metrics) {
        last_use_ = GetTickCount64();
        // Release at a document boundary when another reader needs capacity;
        // uncontended tasks keep their warm parser for the next document.
        if (document::AdmissionContended(admission_directory_)) Stop(&metrics);
    }
    bool Start(DWORD& error, const std::function<bool()>& cancelled, DocumentReadMetrics& metrics) {
        if (process_.value && GetTickCount64() - last_use_ < document::kIdleMs - 1000 &&
            WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT) return true;
        Stop(&metrics);
        if (admission_directory_.empty() && !AdmissionDirectory(admission_directory_, error)) return false;
        document::AdmissionHandle admission;
        DocumentMetricScope admission_time(metrics.admission_wait_us);
        if (!document::AcquireAdmission(admission_directory_, admission, cancelled, error)) return false;
        admission_time.Finish();
        DocumentMetricScope startup_time(metrics.process_start_us);
        wchar_t module[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
        if (!length || length >= ARRAYSIZE(module)) { error = ERROR_BAD_PATHNAME; return false; }
        std::wstring exe(module, length);
        const auto slash = exe.find_last_of(L"\\/");
        if (slash == std::wstring::npos) { error = ERROR_BAD_PATHNAME; return false; }
        exe.resize(slash + 1); exe += L"Pulse.Document.exe";
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Handle child_input, child_output;
        request_ready_.Reset(CreateEventW(&security, FALSE, FALSE, nullptr));
        response_ready_.Reset(CreateEventW(&security, FALSE, FALSE, nullptr));
        if (!request_ready_.value || !response_ready_.value ||
            !CreatePipe(&child_input.value, &input_.value, &security, 128 * 1024) ||
            !CreatePipe(&output_.value, &child_output.value, &security, 128 * 1024) ||
            !SetHandleInformation(input_.value, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(output_.value, HANDLE_FLAG_INHERIT, 0)) {
            error = GetLastError(); Stop(); return false;
        }
        job_.Reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        limits.ProcessMemoryLimit = static_cast<SIZE_T>(document::kMemoryBytes);
        if (!job_.value || !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            error = GetLastError(); Stop(); return false;
        }
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
        std::vector<uint8_t> attributes(bytes);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(list, 2, 0, &bytes)) { error = GetLastError(); Stop(); return false; }
        HANDLE inherited[]{child_input.value, child_output.value, request_ready_.value, response_ready_.value,
            admission.value};
        bool ok = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
        // Atomic association closes the parent-crash window between creating a
        // suspended child and assigning it to the kill-on-close job.
        if (ok) ok = UpdateProcThreadAttribute(list, 0, document::kJobListAttribute,
            &job_.value, sizeof(job_.value), nullptr, nullptr) != FALSE;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.lpAttributeList = list;
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + exe + L"\" " +
            std::to_wstring(reinterpret_cast<uintptr_t>(child_input.value)) + L" " +
            std::to_wstring(reinterpret_cast<uintptr_t>(child_output.value)) + L" " +
            std::to_wstring(reinterpret_cast<uintptr_t>(request_ready_.value)) + L" " +
            std::to_wstring(reinterpret_cast<uintptr_t>(response_ready_.value));
        if (ok) ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | BELOW_NORMAL_PRIORITY_CLASS,
            nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
        error = ok ? ERROR_SUCCESS : GetLastError();
        DeleteProcThreadAttributeList(list);
        if (!ok) { Stop(); return false; }
        ++metrics.process_starts;
        process_.Reset(process.hProcess);
        Handle thread; thread.value = process.hThread;
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
            error = GetLastError(); TerminateProcess(process_.value, error); Stop(); return false;
        }
        return true;
    }
    bool Receive(void* destination, size_t length, ULONGLONG deadline,
                 const std::function<bool()>& cancelled, DWORD& error, DocumentReadMetrics& metrics) {
        DocumentMetricScope response_time(metrics.response_us);
        auto* bytes = static_cast<uint8_t*>(destination);
        while (length) {
            if (cancelled && cancelled()) { error = ERROR_CANCELLED; return false; }
            if (GetTickCount64() >= deadline) { error = ERROR_TIMEOUT; return false; }
            DWORD available = 0;
            if (!PeekNamedPipe(output_.value, nullptr, 0, nullptr, &available, nullptr)) {
                error = ERROR_BROKEN_PIPE; return false;
            }
            if (!available) {
                HANDLE events[]{response_ready_.value, process_.value};
                const auto ready = WaitForMultipleObjects(2, events, FALSE, 10);
                if (ready == WAIT_OBJECT_0 + 1 || ready == WAIT_FAILED) {
                    error = ERROR_PROCESS_ABORTED; return false;
                }
                continue;
            }
            DWORD read = 0;
            const auto count = static_cast<DWORD>((std::min)(length, static_cast<size_t>(available)));
            if (!ReadFile(output_.value, bytes, count, &read, nullptr) || !read) {
                error = ERROR_BROKEN_PIPE; return false;
            }
            bytes += read; length -= read;
        }
        return true;
    }
    void Stop(DocumentReadMetrics* metrics = nullptr) {
        uint64_t unused = 0;
        DocumentMetricScope stop_time(metrics ? metrics->process_stop_us : unused);
        if (metrics) FinishAttemptCpu(*metrics);
        else attempt_cpu_running_ = false;
        job_.Reset();
        if (process_.value) WaitForSingleObject(process_.value, 1000);
        process_.Reset(); input_.Reset(); output_.Reset(); request_ready_.Reset(); response_ready_.Reset();
    }
    std::timed_mutex mutex_;
    ULONGLONG last_use_ = 0;
    uint64_t attempt_cpu_start_ = 0;
    bool attempt_cpu_running_ = false;
    std::wstring admission_directory_;
    Handle input_, output_, process_, job_, request_ready_, response_ready_;
};
struct TaskReader {
    unsigned depth = 0;
    std::unique_ptr<DocumentWorker> worker;
    std::shared_ptr<DocumentReadPool> pool;
};
TaskReader& CurrentTaskReader() {
    thread_local TaskReader task;
    return task;
}
}
class DocumentReadPool {
public:
    ~DocumentReadPool() {
        if (reaper_.joinable()) {
            reaper_.request_stop();
            wake_.notify_all();
            reaper_.join();
        }
    }
    bool Read(const std::wstring& path, uint64_t maximum_bytes, std::wstring& output,
              uint64_t& bytes_read, DWORD& error, const std::function<bool()>& cancelled,
              std::wstring_view stop_needle, bool case_sensitive, DocumentReadMetrics* metrics) {
        uint64_t unused = 0;
        DocumentMetricScope queue_time(metrics ? metrics->reader_queue_us : unused);
        StartReaper();
        std::unique_lock lock(mutex_);
        int waiter = 0;
        waiters_.push_back(&waiter);
        size_t selected = workers_.size();
        try { for (;;) {
            if (cancelled && cancelled()) {
                const auto found = std::find(waiters_.begin(), waiters_.end(), &waiter);
                waiters_.erase(found);
                lock.unlock();
                wake_.notify_all();
                error = ERROR_CANCELLED;
                return false;
            }
            if (waiters_.front() == &waiter) {
                for (size_t i = 0; i < workers_.size(); ++i) {
                    if (!busy_[i]) { selected = i; break; }
                }
                if (selected != workers_.size()) break;
            }
            wake_.wait_for(lock, std::chrono::milliseconds(25));
        } } catch (...) {
            waiters_.erase(std::find(waiters_.begin(), waiters_.end(), &waiter));
            lock.unlock();
            wake_.notify_all();
            throw;
        }
        waiters_.pop_front();
        busy_[selected] = true;
        lock.unlock();
        wake_.notify_all();
        queue_time.Finish();
        struct Lease {
            DocumentReadPool& pool;
            size_t index;
            ~Lease() {
                { std::lock_guard guard(pool.mutex_); pool.busy_[index] = false; }
                pool.wake_.notify_all();
            }
        } lease{*this, selected};
        return workers_[selected].Read(path, maximum_bytes, output, bytes_read, error,
            cancelled, stop_needle, case_sensitive, metrics);
    }
private:
    void StartReaper() {
        std::call_once(reaper_once_, [this] {
            reaper_ = std::jthread([this](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    std::unique_lock lock(mutex_);
                    wake_.wait_for(lock, std::chrono::milliseconds(100), [&] { return stop.stop_requested(); });
                    lock.unlock();
                    if (stop.stop_requested()) break;
                    // An idle pool has no next document boundary at which to
                    // yield. Only unlocked workers can be reclaimed here.
                    for (auto& worker : workers_) {
                        try { worker.ReclaimIdle(); }
                        catch (...) { /* A failed idle probe must not abort active readers. */ }
                    }
                }
            });
        });
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<const int*> waiters_;
    std::array<bool, document::kProcessSlots> busy_{};
    std::array<DocumentWorker, document::kProcessSlots> workers_;
    std::once_flag reaper_once_;
    std::jthread reaper_;
};
std::shared_ptr<DocumentReadPool> CreateDocumentReadPool() { return std::make_shared<DocumentReadPool>(); }
DocumentReadSession::DocumentReadSession() { ++CurrentTaskReader().depth; }
DocumentReadSession::DocumentReadSession(const std::shared_ptr<DocumentReadPool>& pool) {
    auto& task = CurrentTaskReader();
    previous_pool_ = std::move(task.pool);
    task.pool = pool;
    replaced_pool_ = true;
    ++task.depth;
}
DocumentReadSession::~DocumentReadSession() {
    auto& task = CurrentTaskReader();
    if (replaced_pool_) task.pool = std::move(previous_pool_);
    if (!--task.depth) task.worker.reset();
}
bool IsOfficeDocumentExtension(std::wstring_view extension) noexcept {
    for (const auto* ext : {L".docx", L".xlsx", L".pptx", L".doc", L".xls", L".ppt", L".rtf"}) {
        if (Equal(extension, ext)) return true;
    }
    return false;
}
bool IsIndexedContentExtension(std::wstring_view extension) noexcept {
    return text::IsKnownTextExtension(extension) || IsExtractedDocumentExtension(extension);
}
bool IsExtractedDocumentExtension(std::wstring_view extension) noexcept {
    return IsOfficeDocumentExtension(extension) || Equal(extension, L".pdf");
}
bool ReadSearchableDocument(const std::wstring& path, uint64_t maximum_bytes,
                            std::wstring& output, uint64_t& bytes_read, DWORD* error,
                            text::Encoding encoding, const std::function<bool()>& cancelled,
                            std::wstring_view stop_needle, bool case_sensitive, DocumentReadMetrics* metrics) {
    if (metrics) *metrics = {};
    output.clear(); bytes_read = 0;
    auto fail = [&](DWORD code) { if (error) *error = code; return false; };
    if (cancelled && cancelled()) return fail(ERROR_CANCELLED);
    const auto ext = Extension(path);
    if (!IsExtractedDocumentExtension(ext)) return text::ReadFile(path, maximum_bytes, output, bytes_read, error, encoding);
    if (path.empty() || path.size() > 32767 || path.find(L'\0') != std::wstring::npos) return fail(ERROR_BAD_PATHNAME);
    static DocumentWorker background_worker;
    auto& task = CurrentTaskReader();
    DWORD code = ERROR_SUCCESS;
    if (task.pool) {
        if (!task.pool->Read(path, maximum_bytes, output, bytes_read, code, cancelled, stop_needle, case_sensitive, metrics)) return fail(code);
        if (error) *error = ERROR_SUCCESS;
        return true;
    }
    if (task.depth && !task.worker) task.worker = std::make_unique<DocumentWorker>();
    auto& worker = task.depth ? *task.worker : background_worker;
    if (!worker.Read(path, maximum_bytes, output, bytes_read, code, cancelled, stop_needle, case_sensitive, metrics)) return fail(code);
    if (error) *error = ERROR_SUCCESS;
    return true;
}
}
