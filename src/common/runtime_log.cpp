#include "runtime_log.h"
#include "pulse_version.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace pulse::diagnostics::runtime {
namespace {
constexpr size_t kQueueLimit = 1024, kFieldLimit = 12;
constexpr size_t kCriticalLimit = 32;
struct Value { char name[48]{}; uint64_t value = 0; };
struct Record {
    char event[80]{};
    std::array<Value, kFieldLimit> fields{};
    size_t count = 0;
    uint64_t utc = 0, tick = 0, sequence = 0;
    DWORD thread = 0;
    Level level = Level::Info;
};
struct CriticalRecord { Record first, last; uint64_t occurrences = 1; };
bool SameFailure(const Record& a, const Record& b) {
    if (strcmp(a.event, b.event) != 0) return false;
    // Correlation IDs and timing vary on each retry. Only failure category
    // fields split a pattern; first/last records retain all original fields.
    constexpr std::string_view keys[] = {"error", "code", "hr", "hresult", "win32_error", "reason"};
    for (const auto key : keys) {
        const Value* av = nullptr; const Value* bv = nullptr;
        for (size_t i = 0; i < a.count; ++i) if (key == a.fields[i].name) av = &a.fields[i];
        for (size_t i = 0; i < b.count; ++i) if (key == b.fields[i].name) bv = &b.fields[i];
        if ((av == nullptr) != (bv == nullptr) || (av && av->value != bv->value)) return false;
    }
    return true;
}
bool Identifier(const char* text, size_t limit) {
    if (!text || !*text) return false;
    size_t n = 0;
    for (; n < limit && text[n]; ++n) {
        const char c = text[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return n < limit;
}
uint64_t Ft(FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
uint64_t Utc() { FILETIME t{}; GetSystemTimeAsFileTime(&t); return Ft(t); }
std::string Escape(std::wstring_view value) {
    std::string out;
    for (wchar_t c : value) {
        if (c == L'"' || c == L'\\') out += '\\';
        out += c >= 32 && c < 127 ? static_cast<char>(c) : '_';
    }
    return out;
}
bool Directory(const std::wstring& path) {
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetLastError(ERROR_ACCESS_DENIED); return false;
    }
    return true;
}
class Logger {
public:
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> ids{0}, dropped{0};
    std::atomic<uint64_t> write_errors{0};
    std::atomic<DWORD> initialization_error{0}, last_write_error{0};
    std::mutex mutex;
    std::condition_variable wake, drained;
    std::deque<Record> queue;
    std::vector<CriticalRecord> critical;
    std::thread worker;
    bool stopping = false, worker_failed = false, critical_dirty = false;
    uint64_t sequence = 0, session = 0, accepted = 0, completed = 0;
    std::wstring directory, path, critical_path;
    std::string component;
    Options options;
    ~Logger() { Stop(); }
    void Stop() noexcept {
        enabled = false;
        try {
            { std::lock_guard lock(mutex); stopping = true; }
            wake.notify_one();
            if (worker.joinable()) worker.join();
            drained.notify_all();
        } catch (...) {}
    }
    void WriteError(DWORD error) noexcept {
        last_write_error = error ? error : ERROR_WRITE_FAULT;
        ++write_errors;
    }
    std::string Line(const Record& record, uint64_t occurrences = 0, const char* evidence = nullptr) const {
        FILETIME ft{static_cast<DWORD>(record.utc), static_cast<DWORD>(record.utc >> 32)};
        SYSTEMTIME t{}; FileTimeToSystemTime(&ft, &t);
        char utc[40]{};
        sprintf_s(utc, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay,
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        std::string line = "{\"schema\":1,\"utc\":\"" + std::string(utc) + "\",\"tick_ms\":" + std::to_string(record.tick) +
            ",\"session\":" + std::to_string(session) + ",\"pid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"tid\":" + std::to_string(record.thread) + ",\"seq\":" + std::to_string(record.sequence) +
            ",\"component\":\"" + component + "\",\"version\":\"" PULSE_VERSION_STRING_A "\",\"build\":\"" +
            Escape(PULSE_BUILD_ID) + "\",\"event\":\"" + record.event + "\",\"severity\":" +
            std::to_string(static_cast<unsigned>(record.level));
        if (evidence) line += ",\"evidence\":\"" + std::string(evidence) + "\",\"occurrences\":" + std::to_string(occurrences);
        line += ",\"data\":{";
        for (size_t i = 0; i < record.count; ++i) {
            if (i) line += ',';
            line += "\"" + std::string(record.fields[i].name) + "\":" + std::to_string(record.fields[i].value);
        }
        return line + "}}\n";
    }
    void Write(Record record) {
        record.sequence = ++sequence;
        const auto line = Line(record);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) { WriteError(GetLastError()); return; }
        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(file, &info) || !GetFileSizeEx(file, &size)) {
            const auto error = GetLastError(); CloseHandle(file); WriteError(error); return;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            CloseHandle(file); WriteError(ERROR_ACCESS_DENIED); return;
        }
        if (static_cast<uint64_t>(size.QuadPart) + line.size() > options.rotate_bytes) {
            CloseHandle(file);
            if (!MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING)) { WriteError(GetLastError()); return; }
            file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) { WriteError(GetLastError()); return; }
        }
        LARGE_INTEGER end{}; DWORD written = 0;
        if (!SetFilePointerEx(file, end, nullptr, FILE_END) ||
            !WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) WriteError(GetLastError());
        else if (written != line.size()) WriteError(ERROR_WRITE_FAULT);
        CloseHandle(file);
    }
    void WriteCritical(const std::vector<CriticalRecord>& records) {
        std::string contents;
        for (auto entry : records) {
            entry.first.sequence = ++sequence;
            contents += Line(entry.first, entry.occurrences, "first");
            if (entry.occurrences > 1) {
                entry.last.sequence = ++sequence;
                contents += Line(entry.last, entry.occurrences, "last");
            }
        }
        const auto temporary = critical_path + L".tmp";
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) { WriteError(GetLastError()); return; }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info)) {
            const auto error = GetLastError(); CloseHandle(file); WriteError(error); return;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            CloseHandle(file); WriteError(ERROR_ACCESS_DENIED); return;
        }
        if (!SetEndOfFile(file)) {
            const auto error = GetLastError(); CloseHandle(file); WriteError(error); return;
        }
        DWORD written = 0;
        const bool success = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) != FALSE;
        const auto error = success ? ERROR_WRITE_FAULT : GetLastError();
        CloseHandle(file);
        if (!success || written != contents.size()) { WriteError(error); DeleteFileW(temporary.c_str()); return; }
        if (!MoveFileExW(temporary.c_str(), critical_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            WriteError(GetLastError()); DeleteFileW(temporary.c_str());
        }
    }
    Record Make(const char* name, std::initializer_list<Field> fields, Level level = Level::Info) {
        Record record;
        record.level = level;
        strcpy_s(record.event, name);
        record.utc = Utc(); record.tick = GetTickCount64(); record.thread = GetCurrentThreadId();
        for (const auto& field : fields) {
            auto& value = record.fields[record.count++];
            strcpy_s(value.name, field.name); value.value = field.value;
        }
        return record;
    }
    // Keep the newest 24 files (~48 MiB maximum at default size). Active files
    // deny write sharing, so a different process cannot prune an open writer.
    void Prune() {
        struct Entry { std::wstring path; uint64_t time; };
        std::vector<Entry> files, critical_files;
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW((directory + L"\\*.jsonl*").c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) return;
        do {
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            std::wstring_view name(data.cFileName);
            if (!(name.ends_with(L".jsonl") || name.ends_with(L".jsonl.1"))) continue;
            auto& entries = name.ends_with(L".critical.jsonl") ? critical_files : files;
            entries.push_back({directory + L"\\" + data.cFileName, Ft(data.ftLastWriteTime)});
        } while (FindNextFileW(search, &data));
        FindClose(search);
        auto prune = [&](std::vector<Entry>& entries, size_t keep) {
            std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time > b.time; });
            for (size_t i = keep; i < entries.size(); ++i) {
                if (entries[i].path == path || entries[i].path == path + L".1" || entries[i].path == critical_path) continue;
                HANDLE stale = CreateFileW(entries[i].path.c_str(), DELETE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (stale != INVALID_HANDLE_VALUE) CloseHandle(stale);
            }
        };
        prune(files, 24);
        prune(critical_files, 12);
    }
    void Run() noexcept {
        try {
            Prune();
            uint64_t last_tick = GetTickCount64(), last_cpu = 0;
            bool have_cpu = false;
            auto heartbeat = [&] {
                FILETIME created{}, exited{}, kernel{}, user{};
                const bool cpu_ok = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE;
                const auto cpu = (Ft(kernel) + Ft(user)) / 10;
                const auto tick = GetTickCount64();
                PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
                const bool memory_ok = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE;
                DWORD handles = 0;
                const bool handles_ok = GetProcessHandleCount(GetCurrentProcess(), &handles) != FALSE;
                SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
                auto record = Make("process_health", {{"interval_ms", tick - last_tick}, {"cpu_us", have_cpu && cpu_ok && cpu >= last_cpu ? cpu - last_cpu : 0},
                    {"cpu_total_us", cpu}, {"logical_processors", system.dwNumberOfProcessors},
                    {"working_set_bytes", memory.WorkingSetSize}, {"private_bytes", memory.PrivateUsage},
                    {"handles", handles}, {"dropped_events", dropped.load()}, {"write_errors", write_errors.load()},
                    {"cpu_sample_valid", have_cpu && cpu_ok}, {"memory_sample_valid", memory_ok}, {"handles_sample_valid", handles_ok}});
                Write(record); last_cpu = cpu; last_tick = tick; have_cpu = cpu_ok;
            };
            heartbeat();
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.heartbeat_ms);
            for (;;) {
                std::deque<Record> batch;
                std::vector<CriticalRecord> critical_batch;
                uint64_t target;
                bool stop;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_until(lock, deadline, [&] { return stopping || !queue.empty() || critical_dirty; });
                    batch.swap(queue); stop = stopping; target = accepted;
                    if (critical_dirty) { critical_batch = critical; critical_dirty = false; }
                }
                for (const auto& record : batch) Write(record);
                if (!critical_batch.empty()) WriteCritical(critical_batch);
                {
                    std::lock_guard lock(mutex);
                    completed = target;
                }
                drained.notify_all();
                if (stop) {
                    auto record = Make("process_stop", {{"dropped_events", dropped.load()}, {"write_errors", write_errors.load()}});
                    Write(record); break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    heartbeat(); Prune();
                    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.heartbeat_ms);
                }
            }
        } catch (...) {
            WriteError(ERROR_NOT_ENOUGH_MEMORY); enabled = false;
            { std::lock_guard lock(mutex); worker_failed = true; }
            drained.notify_all();
        }
    }
};
Logger& State() { static Logger logger; return logger; }
}
bool Initialize(const std::wstring& root, const char* component, Options options) noexcept {
    try {
        auto& state = State();
        if (state.enabled) return true;
        if (state.worker.joinable()) { state.initialization_error = ERROR_BUSY; return false; }
        if (!Identifier(component, 48) || root.empty() || options.rotate_bytes < 4096 || options.heartbeat_ms < 10) {
            state.initialization_error = ERROR_INVALID_PARAMETER; return false;
        }
        if (!Directory(root) || !Directory(root + L"\\Diagnostics") || !Directory(root + L"\\Diagnostics\\Runtime")) {
            state.initialization_error = GetLastError(); return false;
        }
        state.directory = root + L"\\Diagnostics\\Runtime";
        state.component = component;
        state.path = state.directory + L"\\" + std::wstring(state.component.begin(), state.component.end()) +
            L"-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
        state.critical_path = state.path.substr(0, state.path.size() - 6) + L".critical.jsonl";
        state.options = options; state.session = Utc(); state.stopping = false;
        state.sequence = 0; state.dropped = 0; state.write_errors = 0;
        state.initialization_error = 0; state.last_write_error = 0;
        {
            std::lock_guard lock(state.mutex);
            state.queue.clear(); state.critical.clear(); state.critical.reserve(kCriticalLimit);
            state.accepted = 0; state.completed = 0; state.worker_failed = false; state.critical_dirty = false;
        }
        state.enabled = true;
        OSVERSIONINFOW os{}; os.dwOSVersionInfoSize = sizeof(os);
        using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto version = reinterpret_cast<GetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        const bool have_os = version && version(&os) == 0;
        Event("process_start", {{"pointer_bits", sizeof(void*) * 8}, {"os_known", have_os},
            {"os_major", os.dwMajorVersion}, {"os_minor", os.dwMinorVersion}, {"os_build", os.dwBuildNumber}});
        state.worker = std::thread([&state] { state.Run(); });
        return true;
    } catch (...) {
        State().enabled = false; State().initialization_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
}
bool Enabled() noexcept { return State().enabled.load(std::memory_order_relaxed); }
Health GetHealth() noexcept {
    auto& state = State();
    Health health;
    health.enabled = state.enabled.load(); health.initialization_error = state.initialization_error.load();
    health.last_write_error = state.last_write_error.load(); health.write_errors = state.write_errors.load();
    health.dropped_events = state.dropped.load();
    try {
        std::lock_guard lock(state.mutex);
        health.pending_events = state.accepted - state.completed;
    } catch (...) {}
    return health;
}
bool Flush(uint32_t timeout_ms) noexcept {
    try {
        auto& state = State();
        std::unique_lock lock(state.mutex);
        if (!state.enabled) return false;
        const auto target = state.accepted;
        state.wake.notify_one();
        const bool ready = state.drained.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return state.completed >= target || state.worker_failed;
        });
        return ready && !state.worker_failed && state.completed >= target && state.write_errors.load() == 0;
    } catch (...) { return false; }
}
uint64_t NextId() noexcept { return ++State().ids; }
void Event(const char* name, std::initializer_list<Field> fields, Level level) noexcept {
    const DWORD caller_error = GetLastError();
    struct RestoreError { DWORD value; ~RestoreError() { SetLastError(value); } } restore{caller_error};
    try {
        auto& state = State();
        if (!state.enabled) return;
        if (!Identifier(name, 80) || fields.size() > kFieldLimit ||
            (level != Level::Info && level != Level::Warning && level != Level::Error)) { ++state.dropped; return; }
        for (const auto& field : fields) if (!Identifier(field.name, 48)) { ++state.dropped; return; }
        auto record = state.Make(name, fields, level);
        {
            std::lock_guard lock(state.mutex);
            if (!state.enabled || state.stopping) return;
            if (level == Level::Error) {
                auto found = std::find_if(state.critical.begin(), state.critical.end(), [&](const CriticalRecord& entry) {
                    return SameFailure(entry.first, record);
                });
                if (found != state.critical.end()) {
                    found->last = record; ++found->occurrences;
                } else {
                    if (state.critical.size() == kCriticalLimit) {
                        // Keep the earliest failure, then the most recent patterns.
                        state.dropped += state.critical[1].occurrences;
                        state.critical.erase(state.critical.begin() + 1);
                    }
                    state.critical.push_back({record, record, 1});
                    if (state.queue.size() < kQueueLimit) state.queue.push_back(record);
                }
                state.critical_dirty = true;
            } else {
                if (state.queue.size() >= kQueueLimit) { ++state.dropped; return; }
                state.queue.push_back(record);
            }
            ++state.accepted;
        }
        state.wake.notify_one();
    } catch (...) { ++State().dropped; }
}
void Shutdown() noexcept { State().Stop(); }
}
