#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <propsys.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
constexpr wchar_t kClass[] = L"PulseManualDialogAcceptance20261110";
constexpr char kVisible[] = "Pulse manual visible fixture v1\r\n";
constexpr char kBlocked[] = "Pulse manual blocked typed-path fixture v1\r\n";
constexpr char kSeed[] = "Pulse manual SaveAsItem source seed v1\r\n";
constexpr char kWritten[] = "Pulse manual SaveAsItem host write and reopen v1\r\n";
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) return "<invalid-utf16>";
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::string Json(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string output = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { output += '\\'; output += static_cast<char>(c); }
        else if (c < 32) { output += "\\u00"; output += hex[c >> 4]; output += hex[c & 15]; }
        else output += static_cast<char>(c);
    }
    return output + '"';
}
std::string Json(std::wstring_view value) { return Json(Utf8(value)); }
std::string Number(uint64_t value) { return std::to_string(value); }
std::string Boolean(bool value) { return value ? "true" : "false"; }
std::string Hr(HRESULT value) { char text[16]{}; std::snprintf(text, sizeof(text), "%08lX", static_cast<unsigned long>(value)); return Json(text); }
std::string Utc() {
    SYSTEMTIME t{}; GetSystemTime(&t); char value[40]{};
    std::snprintf(value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return value;
}
bool SamePath(const fs::path& first, const fs::path& second) {
    const auto a = first.lexically_normal().make_preferred().wstring(), b = second.lexically_normal().make_preferred().wstring();
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool PhysicalDirectory(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool PhysicalAncestors(fs::path path) {
    while (!path.empty()) { if (!PhysicalDirectory(path)) return false; const auto next = path.parent_path(); if (next == path) break; path = next; }
    return true;
}
std::optional<fs::path> AbsoluteLocal(const wchar_t* value) {
    if (!value || !*value) return {};
    const std::wstring_view text(value);
    if (text.size() < 4 || text[1] != L':' || (text[2] != L'\\' && text[2] != L'/') || text.find(L':', 2) != text.npos) return {};
    std::array<wchar_t, 32768> full{}; const DWORD length = GetFullPathNameW(value, static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!length || length >= full.size()) return {};
    auto path = fs::path(full.data()).lexically_normal().make_preferred();
    if (path == path.root_path() || path.filename().empty()) return {};
    return path;
}
bool CreatePayload(const fs::path& path, std::string_view bytes) {
    if (!PhysicalAncestors(path.parent_path())) return false;
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file) return false;
    BY_HANDLE_FILE_INFORMATION info{}; DWORD written = 0;
    return GetFileInformationByHandle(file.value, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file.value);
}
struct Identity {
    DWORD volume = 0, high = 0, low = 0;
    bool valid = false;
    bool operator==(const Identity&) const = default;
};
Identity FileIdentity(const fs::path& path, bool directory = false) {
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0u), nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file || !GetFileInformationByHandle(file.value, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory) return {};
    return {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow, true};
}
bool ReadEquals(const fs::path& path, std::string_view expected) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{}; std::array<char, 256> bytes{}; DWORD read = 0;
    return file && GetFileInformationByHandle(file.value, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        !info.nFileSizeHigh && info.nFileSizeLow == expected.size() && expected.size() < bytes.size() &&
        ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == expected.size() && std::string_view(bytes.data(), read) == expected;
}
std::string Sha256(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; DWORD size = 0, returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::string result;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &returned, 0) >= 0) {
        std::vector<UCHAR> object(size); std::array<UCHAR, 32> digest{}; std::array<UCHAR, 65536> buffer{};
        if (BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) >= 0) {
            DWORD read = 0; bool okay = true;
            while (okay) { if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { okay = false; break; }
                if (!read) break; okay = BCryptHashData(hash, buffer.data(), read, 0) >= 0; }
            if (okay && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0) {
                constexpr char hex[] = "0123456789abcdef"; for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
            }
            BCryptDestroyHash(hash);
        }
    }
    BCryptCloseAlgorithmProvider(algorithm, 0); return result;
}
struct Field { std::string name, value; };
class Logger {
    Handle file_;
    uint64_t sequence_ = 0;
public:
    bool failed = false;
    explicit Logger(const fs::path& path) : file_(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)) { failed = !file_; }
    void Record(std::string_view type, std::initializer_list<Field> fields = {}) {
        if (failed) return;
        std::string line = "{\"schema\":1,\"sequence\":" + Number(++sequence_) + ",\"utc\":" + Json(Utc()) + ",\"pid\":" + Number(GetCurrentProcessId()) + ",\"type\":" + Json(type);
        for (const auto& field : fields) line += ',' + Json(field.name) + ':' + field.value;
        line += "}\r\n"; DWORD written = 0;
        failed = !WriteFile(file_.value, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size() || !FlushFileBuffers(file_.value);
    }
};
fs::path ImagePath(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)); std::array<wchar_t, 32768> path{}; DWORD size = static_cast<DWORD>(path.size());
    return process && QueryFullProcessImageNameW(process.value, 0, path.data(), &size) ? fs::path(std::wstring(path.data(), size)) : fs::path{};
}
fs::path AddressModule(const void* address) {
    HMODULE module = nullptr; std::array<wchar_t, 32768> path{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module)) return {};
    const DWORD count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return count && count < path.size() ? fs::path(std::wstring(path.data(), count)) : fs::path{};
}
bool ModuleLoaded(DWORD pid, const fs::path& expected, DWORD& error) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) { error = GetLastError(); return false; }
    MODULEENTRY32W entry{sizeof(entry)}; error = ERROR_SUCCESS;
    if (!Module32FirstW(snapshot.value, &entry)) { error = GetLastError(); return false; }
    do { if (SamePath(entry.szExePath, expected)) return true; } while (Module32NextW(snapshot.value, &entry));
    return false;
}
struct VisibilityProcess {
    HANDLE handle = nullptr;
    ~VisibilityProcess() { if (handle) CloseHandle(handle); }
};
struct VisibilityBinding {
    HWND window = nullptr;
    DWORD pid = 0, thread = 0;
    uint64_t process_created = 0;
    fs::path image, luma;
    std::wstring title;
    std::shared_ptr<VisibilityProcess> process;
};
struct VisibilitySample {
    HWND window = nullptr;
    DWORD pid = 0, thread = 0, module_error = 0;
    uint64_t tick = 0;
    bool process_same = false, image_same = false, title_same = false, luma_same = false, visible = false;
};
bool SameBinding(const VisibilityBinding& first, const VisibilityBinding& second) {
    return first.window == second.window && first.pid == second.pid && first.thread == second.thread && first.process_created == second.process_created &&
        SamePath(first.image, second.image) && SamePath(first.luma, second.luma) && first.title == second.title;
}
bool ExactVisibleSample(const VisibilityBinding& bound, const VisibilitySample& sample) {
    return bound.window && bound.pid && bound.thread && sample.window == bound.window && sample.pid == bound.pid && sample.thread == bound.thread &&
        sample.process_same && sample.image_same && sample.title_same && sample.luma_same && sample.visible;
}
uint64_t FileTimeNumber(FILETIME time) { return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; }
std::optional<VisibilityBinding> BindVisibility(HWND window, DWORD pid, const fs::path& image, const fs::path& luma, const std::wstring& title, DWORD& error) {
    VisibilityBinding binding; binding.window = window; binding.pid = pid; binding.image = image; binding.luma = luma; binding.title = title;
    DWORD actual_pid = 0; binding.thread = GetWindowThreadProcessId(window, &actual_pid);
    if (!binding.thread || actual_pid != pid) { error = ERROR_INVALID_WINDOW_HANDLE; return {}; }
    auto process = std::make_shared<VisibilityProcess>();
    process->handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process->handle) { error = GetLastError(); return {}; }
    std::array<wchar_t, 32768> path{}; DWORD length = static_cast<DWORD>(path.size()); FILETIME created{}, exited{}, kernel{}, user{};
    if (WaitForSingleObject(process->handle, 0) != WAIT_TIMEOUT || !QueryFullProcessImageNameW(process->handle, 0, path.data(), &length) ||
        !SamePath(fs::path(std::wstring(path.data(), length)), image) || !GetProcessTimes(process->handle, &created, &exited, &kernel, &user)) {
        error = ERROR_INVALID_DATA; return {};
    }
    binding.process_created = FileTimeNumber(created); binding.process = std::move(process); error = ERROR_SUCCESS; return binding;
}
VisibilitySample ReadVisibility(const VisibilityBinding& bound) {
    VisibilitySample result; result.window = bound.window; result.tick = GetTickCount64();
    result.thread = GetWindowThreadProcessId(bound.window, &result.pid);
    if (!bound.process || !bound.process->handle || !IsWindow(bound.window) || result.pid != bound.pid || result.thread != bound.thread ||
        WaitForSingleObject(bound.process->handle, 0) != WAIT_TIMEOUT) return result;
    FILETIME created{}, exited{}, kernel{}, user{};
    result.process_same = GetProcessTimes(bound.process->handle, &created, &exited, &kernel, &user) && FileTimeNumber(created) == bound.process_created;
    std::array<wchar_t, 32768> image{}; DWORD length = static_cast<DWORD>(image.size());
    result.image_same = QueryFullProcessImageNameW(bound.process->handle, 0, image.data(), &length) && SamePath(fs::path(std::wstring(image.data(), length)), bound.image);
    result.visible = IsWindowVisible(bound.window) != FALSE;
    if (!result.visible || !result.process_same || !result.image_same) return result;
    std::array<wchar_t, 512> title{}; const int count = GetWindowTextW(bound.window, title.data(), static_cast<int>(title.size()));
    result.title_same = count > 0 && std::wstring_view(title.data(), static_cast<size_t>(count)) == bound.title;
    result.luma_same = ModuleLoaded(bound.pid, bound.luma, result.module_error);
    // Recheck HWND ownership after querying modules; a recycled handle must not
    // turn observations of two different windows into one successful sample.
    DWORD after_pid = 0; const DWORD after_thread = GetWindowThreadProcessId(bound.window, &after_pid);
    result.visible = result.visible && IsWindowVisible(bound.window) && after_pid == bound.pid && after_thread == bound.thread;
    return result;
}
struct VisibilityResult {
    bool started = false, bound = false, visible = false;
    DWORD observer_thread = 0, pid = 0, window_thread = 0, module_error = 0, worker_error = 0;
    uint64_t samples = 0, visible_samples = 0, rejected_samples = 0, first_visible = 0;
};
// This thread never calls COM, writes UI, sends input, or writes the logger.
// Only a window authenticated by the caller-STA callback can be published.
class VisibilityProbe {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<VisibilityBinding> binding_;
    uint64_t generation_ = 0;
    bool stop_ = false;
    VisibilityResult result_;
    std::function<VisibilitySample(const VisibilityBinding&)> read_;
    std::chrono::milliseconds interval_;
    std::thread worker_;
public:
    explicit VisibilityProbe(std::function<VisibilitySample(const VisibilityBinding&)> read = ReadVisibility,
        std::chrono::milliseconds interval = std::chrono::milliseconds(40)) : read_(std::move(read)), interval_(interval) {}
    ~VisibilityProbe() { Stop(); }
    bool Start() noexcept {
        try {
            worker_ = std::thread([this] {
              try {
                std::unique_lock lock(mutex_); result_.observer_thread = GetCurrentThreadId();
                for (;;) {
                    changed_.wait(lock, [&] { return stop_ || binding_.has_value(); });
                    if (stop_) return;
                    const auto binding = *binding_; const auto generation = generation_;
                    lock.unlock(); VisibilitySample sample;
                    try { sample = read_(binding); } catch (...) {}
                    lock.lock();
                    if (generation == generation_) {
                        ++result_.samples;
                        if (sample.module_error) result_.module_error = sample.module_error;
                        if (ExactVisibleSample(binding, sample)) {
                            if (!result_.visible) result_.first_visible = sample.tick;
                            result_.visible = true; ++result_.visible_samples;
                        } else ++result_.rejected_samples;
                    }
                    if (stop_) return;
                    changed_.wait_for(lock, interval_, [&] { return stop_ || generation_ != generation; });
                }
              } catch (...) { std::lock_guard lock(mutex_); result_.worker_error = ERROR_NOT_ENOUGH_MEMORY; }
            });
            std::lock_guard lock(mutex_); result_.started = true; return true;
        } catch (...) { return false; }
    }
    void Publish(VisibilityBinding binding) {
        std::lock_guard lock(mutex_);
        if (stop_ || (binding_ && SameBinding(*binding_, binding))) return;
        const bool started = result_.started; const DWORD thread = result_.observer_thread;
        binding_ = std::move(binding); ++generation_; result_ = {};
        result_.started = started; result_.observer_thread = thread; result_.bound = true;
        result_.pid = binding_->pid; result_.window_thread = binding_->thread; changed_.notify_all();
    }
    VisibilityResult Snapshot() { std::lock_guard lock(mutex_); return result_; }
    void Stop() noexcept {
        { std::lock_guard lock(mutex_); stop_ = true; changed_.notify_all(); }
        if (worker_.joinable()) worker_.join();
    }
};
int VisibilitySelfTest() {
    unsigned checks = 0, failures = 0;
    const auto line = [](const std::string& value) { DWORD written = 0; const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE); if (output && output != INVALID_HANDLE_VALUE) WriteFile(output, value.data(), static_cast<DWORD>(value.size()), &written, nullptr); };
    const auto check = [&](bool value, const char* name) { ++checks; if (!value) ++failures; line(std::string(value ? "[PASS] " : "[FAIL] ") + name + "\r\n"); };
    VisibilityBinding binding; binding.window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x1234)); binding.pid = 101; binding.thread = 202;
    binding.image = L"C:\\owned-bundle\\pulse.exe"; binding.luma = L"C:\\owned-bundle\\lumatext.dll"; binding.title = L"synthetic-owned-title";
    const auto good = [](const VisibilityBinding& value) {
        VisibilitySample sample; sample.window = value.window; sample.pid = value.pid; sample.thread = value.thread; sample.tick = GetTickCount64();
        sample.visible = sample.process_same = sample.image_same = sample.title_same = sample.luma_same = true; return sample;
    };
    check(ExactVisibleSample(binding, good(binding)), "exact bound visible sample qualifies");
    auto sample = good(binding); sample.visible = false; check(!ExactVisibleSample(binding, sample), "early hidden callback is not visible evidence");
    sample = good(binding); sample.window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x5678)); check(!ExactVisibleSample(binding, sample), "different HWND does not qualify");
    sample = good(binding); ++sample.pid; check(!ExactVisibleSample(binding, sample), "different PID does not qualify");
    sample = good(binding); ++sample.thread; check(!ExactVisibleSample(binding, sample), "different window thread does not qualify");
    sample = good(binding); sample.process_same = false; check(!ExactVisibleSample(binding, sample), "exited or recycled process does not qualify");
    sample = good(binding); sample.image_same = false; check(!ExactVisibleSample(binding, sample), "allowed peer image is not the activation bundle");
    sample = good(binding); sample.title_same = false; check(!ExactVisibleSample(binding, sample), "another window title does not qualify");
    sample = good(binding); sample.luma_same = false; check(!ExactVisibleSample(binding, sample), "missing or wrong luma module does not qualify");
    auto empty = binding; empty.window = nullptr; check(!ExactVisibleSample(empty, good(empty)), "unbound HWND does not qualify");
    {
        std::atomic<unsigned> reads{0}; VisibilityProbe unbound([&](const VisibilityBinding& value) { ++reads; return good(value); }, std::chrono::milliseconds(1));
        const bool started = unbound.Start(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); unbound.Stop();
        check(started && reads.load() == 0 && !unbound.Snapshot().bound && !unbound.Snapshot().visible, "unbound observer does not inspect other windows and stops cleanly");
    }
    const auto wait_samples = [](VisibilityProbe& probe, uint64_t count) {
        const auto end = GetTickCount64() + 2000;
        while (probe.Snapshot().samples < count && GetTickCount64() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return probe.Snapshot().samples >= count;
    };
    {
        std::atomic<unsigned> reads{0}; VisibilityProbe probe([&](const VisibilityBinding& value) { auto observed = good(value); observed.visible = ++reads == 2; return observed; }, std::chrono::milliseconds(1));
        const bool started = probe.Start(); probe.Publish(binding); const bool sampled = wait_samples(probe, 3); probe.Stop(); const auto result = probe.Snapshot();
        check(started && sampled && result.visible && result.visible_samples == 1 && result.first_visible != 0, "hidden callback then visible then hidden latches actual synthetic visible sample");
        check(result.observer_thread != 0 && result.observer_thread != GetCurrentThreadId(), "sampling runs independently of caller modal message dispatch");
        const auto stopped_reads = reads.load(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); check(reads.load() == stopped_reads, "stop joins sampler before result/lifetime teardown");
    }
    {
        VisibilityProbe wrong([&](const VisibilityBinding& value) { auto observed = good(value); observed.image_same = false; return observed; }, std::chrono::milliseconds(1));
        const bool started = wrong.Start(); wrong.Publish(binding); const bool sampled = wait_samples(wrong, 2); wrong.Stop();
        check(started && sampled && !wrong.Snapshot().visible && wrong.Snapshot().rejected_samples >= 2, "repeated visible peer-bundle samples never latch");
    }
    line("VISIBILITY_SELFTEST checks=" + Number(checks) + " failures=" + Number(failures) + " ui_windows=0 com_activations=0 synthetic_samples=1\r\n");
    return failures ? 1 : 0;
}
struct Metrics {
    std::atomic<ULONG> references{1};
    std::atomic<uint64_t> add_refs{0}, releases{0};
    std::atomic<bool> destroyed{false}, caller_sta{true};
    std::atomic<unsigned> enum_calls{0};
    unsigned includes = 0, blocked = 0, visible = 0;
    uint64_t first = 0, last = 0, sum_us = 0;
    std::bitset<1000> bulk;
};
struct App;
struct Attempt {
    App* app = nullptr;
    unsigned scenario = 0, ordinal = 0, folder_events = 0, file_ok_events = 0;
    DWORD thread = GetCurrentThreadId(), ui_pid = 0, module_error = 0;
    IFileDialog* dialog = nullptr;
    fs::path root, initial, target, blocked, visible, source, source_parent, folder_b;
    Identity source_before, parent_before;
    std::shared_ptr<Metrics> metrics;
    std::optional<size_t> activation_bundle;
    std::unique_ptr<VisibilityProbe> visibility_probe;
    VisibilityResult visibility;
    unsigned owner_timer_ticks = 0, getwindow_failures = 0;
    HRESULT last_getwindow_hr = E_PENDING;
    DWORD visibility_bind_error = 0;
    bool api_close_mode = false, api_close_called = false;
    HRESULT api_close_hr = E_PENDING;
    DWORD api_close_thread = 0;
    unsigned api_close_at_item = 0;
    bool probe_enabled = false;
    HRESULT probe_hr = E_PENDING, probe_query_hr = E_PENDING;
    std::atomic<bool> callback_error{false};
    bool busy_observing = false, remote_event_bound = false, remote_visible = false, luma = false, native_seen = false;
    bool same_activation_bundle = false, bundle_mismatch_seen = false;
    uint64_t show_begin = 0;
    std::wstring title;
};
struct App {
    std::vector<fs::path> bundles;
    fs::path out;
    std::unique_ptr<Logger> log;
    HWND window = nullptr, header = nullptr, instructions = nullptr, results = nullptr;
    std::array<HWND, 3> buttons{};
    HFONT font = nullptr;
    std::array<unsigned, 3> attempts{};
    Attempt* active = nullptr;
    bool running = false;
    bool api_close_mode = false, probe_invalid_root = false, last_pass = false;
    std::wstring result_text = L"尚未开始。请只在您方便时点击一个用例；每次只显示一个选择器。\r\n";
    void Status(const std::wstring& text) { result_text += text + L"\r\n"; if (results) { SetWindowTextW(results, result_text.c_str()); SendMessageW(results, EM_SETSEL, result_text.size(), result_text.size()); SendMessageW(results, EM_SCROLLCARET, 0, 0); } }
    std::optional<size_t> BundleFor(const fs::path& image, const wchar_t* leaf) const {
        for (size_t i = 0; i < bundles.size(); ++i) if (SamePath(image, bundles[i] / leaf)) return i;
        return {};
    }
    void Observe(Attempt& attempt, const char* source);
    void Run(unsigned scenario);
};
std::optional<fs::path> ItemPath(IShellItem* item) {
    PWSTR path = nullptr; const HRESULT result = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_POINTER;
    std::optional<fs::path> value; if (result == S_OK && path) value = fs::path(path); CoTaskMemFree(path); return value;
}
bool ExactItem(IShellItem* item, const fs::path& expected, bool exists) {
    const auto path = ItemPath(item); if (!path || !SamePath(*path, expected)) return false;
    if (!exists) return true;
    const auto actual = FileIdentity(*path), wanted = FileIdentity(expected); return actual.valid && wanted.valid && actual == wanted;
}
bool CallerSta(DWORD expected) {
    APTTYPE type{}; APTTYPEQUALIFIER qualifier{};
    return GetCurrentThreadId() == expected && CoGetApartmentType(&type, &qualifier) == S_OK && (type == APTTYPE_STA || type == APTTYPE_MAINSTA);
}
bool SameIdentity(IFileDialog* first, IFileDialog* second) {
    ComPtr<IUnknown> a, b;
    return first && second && first->QueryInterface(IID_PPV_ARGS(&a)) == S_OK && second->QueryInterface(IID_PPV_ARGS(&b)) == S_OK && a.Get() == b.Get();
}
void App::Observe(Attempt& attempt, const char* source) {
    if (attempt.busy_observing) return;
    attempt.busy_observing = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{attempt.busy_observing};
    ComPtr<IOleWindow> ole; HWND window_handle = nullptr;
    const HRESULT queried = attempt.dialog->QueryInterface(IID_PPV_ARGS(&ole));
    const HRESULT got = queried == S_OK ? ole->GetWindow(&window_handle) : queried;
    attempt.last_getwindow_hr = got;
    if (got != S_OK) ++attempt.getwindow_failures;
    if (got != S_OK || !window_handle || !IsWindow(window_handle)) return;
    DWORD pid = 0; GetWindowThreadProcessId(window_handle, &pid);
    if (pid == GetCurrentProcessId()) {
        if (!attempt.native_seen) log->Record("native_window_observed", {{"case", Number(attempt.scenario)}, {"attempt", Number(attempt.ordinal)}, {"observer", Json(source)}});
        attempt.native_seen = true; return;
    }
    const auto image = ImagePath(pid); const auto root = BundleFor(image, L"pulse.exe");
    if (!root) return;
    if (!attempt.activation_bundle || *root != *attempt.activation_bundle) {
        if (!attempt.bundle_mismatch_seen) log->Record("remote_bundle_mismatch", {{"case", Number(attempt.scenario)}, {"attempt", Number(attempt.ordinal)},
            {"ui_pid", Number(pid)}, {"image", Json(image.wstring())}, {"remote_bundle_index", Number(*root)}, {"same_activation_bundle", "false"}});
        attempt.bundle_mismatch_seen = true; return;
    }
    DWORD error = 0; const bool luma = attempt.ui_pid == pid && attempt.luma ? true : ModuleLoaded(pid, bundles[*root] / L"lumatext.dll", error);
    const bool changed = attempt.ui_pid != pid || (!attempt.luma && luma);
    attempt.ui_pid = pid; attempt.luma = luma; attempt.module_error = error;
    attempt.same_activation_bundle = true;
    attempt.remote_visible |= IsWindowVisible(window_handle) != FALSE;
    if (std::string_view(source) == "folder_changed" && luma) attempt.remote_event_bound = true;
    if (luma && attempt.visibility_probe && (std::string_view(source) == "folder_changed" || std::string_view(source) == "file_ok")) {
        auto binding = BindVisibility(window_handle, pid, bundles[*root] / L"pulse.exe", bundles[*root] / L"lumatext.dll", attempt.title, attempt.visibility_bind_error);
        if (binding) attempt.visibility_probe->Publish(std::move(*binding));
    }
    if (changed) log->Record("remote_window_observed", {{"case", Number(attempt.scenario)}, {"attempt", Number(attempt.ordinal)}, {"observer", Json(source)},
        {"ui_pid", Number(pid)}, {"image", Json(image.wstring())}, {"image_sha256", Json(Sha256(image))}, {"luma_exact_bundle", Boolean(luma)},
        {"activation_bundle_index", Number(*attempt.activation_bundle)}, {"same_activation_bundle", "true"},
        {"luma_sha256", Json(luma ? Sha256(bundles[*root] / L"lumatext.dll") : std::string{})}, {"module_snapshot_error", Number(error)}, {"visible", Boolean(IsWindowVisible(window_handle) != FALSE)}});
}
class Events final : public IFileDialogEvents {
    std::atomic<ULONG> references_{1}; Attempt& attempt_;
public:
    explicit Events(Attempt& value) : attempt_(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override { if (!output) return E_POINTER; *output = nullptr; if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE; *output = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        if (!CallerSta(attempt_.thread)) { attempt_.callback_error = true; return S_OK; }
        ++attempt_.file_ok_events;
        if (!SameIdentity(dialog, attempt_.dialog)) attempt_.callback_error = true;
        try { attempt_.app->Observe(attempt_, "file_ok"); } catch (...) { attempt_.callback_error = true; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        if (!CallerSta(attempt_.thread)) { attempt_.callback_error = true; return S_OK; }
        ++attempt_.folder_events;
        if (!SameIdentity(dialog, attempt_.dialog)) attempt_.callback_error = true;
        try { attempt_.app->Observe(attempt_, "folder_changed"); } catch (...) { attempt_.callback_error = true; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override { if (!response) return E_POINTER; *response = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override { if (!response) return E_POINTER; *response = FDEOR_DEFAULT; return S_OK; }
};
class Filter final : public IShellItemFilter {
    std::shared_ptr<Metrics> metrics_; Attempt& attempt_;
public:
    explicit Filter(Attempt& value) : metrics_(value.metrics), attempt_(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override { if (!output) return E_POINTER; *output = nullptr; if (iid != IID_IUnknown && iid != IID_IShellItemFilter) return E_NOINTERFACE; *output = static_cast<IShellItemFilter*>(this); AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { ++metrics_->add_refs; return ++metrics_->references; }
    ULONG STDMETHODCALLTYPE Release() override { ++metrics_->releases; const auto count = --metrics_->references; if (!count) { metrics_->destroyed = true; delete this; } return count; }
    HRESULT STDMETHODCALLTYPE IncludeItem(IShellItem* item) override {
        const auto begin = GetTickCount64(); LARGE_INTEGER started{}, ended{}, frequency{}; QueryPerformanceCounter(&started); QueryPerformanceFrequency(&frequency);
        auto& m = *metrics_;
        if (!CallerSta(attempt_.thread)) { m.caller_sta = false; return RPC_E_WRONG_THREAD; }
        ++m.includes;
        if (!m.first) m.first = begin;
        HRESULT answer = S_OK;
        try {
            const auto path = ItemPath(item);
            if (path && !attempt_.blocked.empty() && SamePath(*path, attempt_.blocked)) { ++m.blocked; answer = S_FALSE; }
            if (path && !attempt_.visible.empty() && SamePath(*path, attempt_.visible)) ++m.visible;
            if (path && attempt_.scenario == 3 && SamePath(path->parent_path(), attempt_.initial)) {
                const auto leaf = path->filename().wstring();
                if (leaf.size() == 13 && leaf.starts_with(L"bulk-") && leaf.ends_with(L".txt")) {
                    unsigned index = 0; bool digits = true;
                    for (size_t i = 5; i < 9; ++i) { digits &= leaf[i] >= L'0' && leaf[i] <= L'9'; index = index * 10 + static_cast<unsigned>(leaf[i] >= L'0' && leaf[i] <= L'9' ? leaf[i] - L'0' : 0); }
                    if (digits && index < m.bulk.size()) m.bulk.set(index);
                }
            }
            if (attempt_.api_close_mode && attempt_.dialog && attempt_.show_begin && m.bulk.count() >= 40 && !attempt_.api_close_called) {
                // This is a caller-STA COM API call inside the genuine filter
                // callback, not a click, key, window command, or user cancel.
                attempt_.api_close_called = true; attempt_.api_close_thread = GetCurrentThreadId();
                attempt_.api_close_at_item = static_cast<unsigned>(m.bulk.count());
                attempt_.api_close_hr = attempt_.dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            }
        } catch (...) { attempt_.callback_error = true; answer = E_OUTOFMEMORY; }
        QueryPerformanceCounter(&ended); if (frequency.QuadPart > 0) m.sum_us += static_cast<uint64_t>((ended.QuadPart - started.QuadPart) * 1000000 / frequency.QuadPart);
        m.last = GetTickCount64(); return answer;
    }
    HRESULT STDMETHODCALLTYPE GetEnumFlagsForItem(IShellItem*, SHCONTF*) override { ++metrics_->enum_calls; if (!CallerSta(attempt_.thread)) metrics_->caller_sta = false; return E_NOTIMPL; }
};
bool MakeDirectory(const fs::path& path) { return PhysicalAncestors(path.parent_path()) && CreateDirectoryW(path.c_str(), nullptr) != FALSE; }
bool Prepare(Attempt& attempt) {
    if (!MakeDirectory(attempt.root)) return false;
    if (attempt.scenario == 1) {
        attempt.initial = attempt.root; attempt.blocked = attempt.root / L"blocked.txt"; attempt.visible = attempt.root / L"visible.txt"; attempt.target = attempt.blocked;
        return CreatePayload(attempt.blocked, kBlocked) && CreatePayload(attempt.visible, kVisible);
    }
    if (attempt.scenario == 2) {
        attempt.source_parent = attempt.root / L"sourceA"; attempt.folder_b = attempt.root / L"folderB";
        if (!MakeDirectory(attempt.source_parent) || !MakeDirectory(attempt.folder_b)) return false;
        attempt.source = attempt.source_parent / L"seed.txt"; attempt.initial = attempt.source_parent; attempt.target = attempt.initial / L"saved-result.txt";
        if (!CreatePayload(attempt.source, kSeed)) return false;
        attempt.source_before = FileIdentity(attempt.source); attempt.parent_before = FileIdentity(attempt.initial, true);
        return attempt.source_before.valid && attempt.parent_before.valid;
    }
    attempt.initial = attempt.root;
    for (unsigned i = 0; i < 1000; ++i) { wchar_t leaf[32]{}; swprintf_s(leaf, L"bulk-%04u.txt", i); if (!CreatePayload(attempt.root / leaf, "owned bulk fixture\r\n")) return false; }
    return true;
}
bool NoProperties(IFileSaveDialog* dialog, HRESULT& result) { ComPtr<IPropertyStore> store; result = dialog->GetProperties(&store); return result == E_UNEXPECTED && !store; }
void App::Run(unsigned scenario) {
    last_pass = false;
    Attempt attempt; attempt.app = this; attempt.scenario = scenario; attempt.ordinal = ++attempts[scenario - 1]; attempt.metrics = std::make_shared<Metrics>();
    attempt.api_close_mode = api_close_mode;
    attempt.probe_enabled = probe_invalid_root;
    struct Receipt {
        Logger& log; Attempt& attempt;
        const char* pending = "setup_failed_no_show";
        bool finalized = false;
        ~Receipt() { if (!finalized) try { log.Record("attempt_terminal", {{"case", Number(attempt.scenario)}, {"attempt", Number(attempt.ordinal)}, {"outcome", Json(pending)}, {"probe_enabled", Boolean(attempt.probe_enabled)}, {"probe_hr", Hr(attempt.probe_hr)}, {"host_consumption_verified", "false"}, {"automatic_picker_input", "false"}}); } catch (...) {} }
    } receipt{*log, attempt};
    attempt.root = out / (L"case-" + std::to_wstring(scenario) + L"-attempt-" + std::to_wstring(attempt.ordinal));
    attempt.title = L"Pulse 手动验收 " + std::to_wstring(GetCurrentProcessId()) + L" 用例" + std::to_wstring(scenario) + L" 第" + std::to_wstring(attempt.ordinal) + L"次";
    log->Record("attempt_plan", {{"case", Number(scenario)}, {"attempt", Number(attempt.ordinal)}, {"entry_family", Json("COM")}, {"deployment", Json("ordinary_desktop_context_not_assumed")},
        {"expected_terminal", Json(scenario == 3 ? "cancelled" : "accepted")}, {"fixture_relative", Json(attempt.root.filename().wstring())}, {"automatic_picker_input", "false"},
        {"mode", Json(api_close_mode ? "api_close_test" : "manual")}, {"api_close_at_unique_item", Number(api_close_mode ? 40 : 0)}, {"ui_visibility_acceptance", Boolean(!api_close_mode)}});
    if (log->failed || !Prepare(attempt)) { Status(L"用例" + std::to_wstring(scenario) + L"：fixture/日志准备失败，没有打开选择器。" ); log->Record("attempt_setup_failed", {{"case", Number(scenario)}, {"attempt", Number(attempt.ordinal)}, {"win32_error", Number(GetLastError())}}); return; }
    receipt.pending = "activation_not_verified_no_show";
    ComPtr<IFileDialog> dialog; const bool save = scenario == 2;
    HRESULT configured = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    fs::path actual_auto; bool exact_auto = false; std::string auto_hash;
    if (configured == S_OK && dialog) {
        const auto table = *reinterpret_cast<void***>(dialog.Get()); actual_auto = AddressModule(table[3]);
        attempt.activation_bundle = BundleFor(actual_auto, sizeof(void*) == 8 ? L"PulseDialogAuto64.dll" : L"PulseDialogAuto32.dll");
        exact_auto = attempt.activation_bundle.has_value(); auto_hash = Sha256(actual_auto);
    }
    if (attempt.probe_enabled && configured == S_OK && dialog && exact_auto && !auto_hash.empty()) {
        ComPtr<IFileDialog2> dialog2; attempt.probe_query_hr = dialog.As(&dialog2);
        attempt.probe_hr = attempt.probe_query_hr == S_OK ? dialog2->SetNavigationRoot(nullptr) : attempt.probe_query_hr;
    }
    log->Record("activation", {{"case", Number(scenario)}, {"attempt", Number(attempt.ordinal)}, {"hr", Hr(configured)}, {"method", Json("ordinary_CoCreateInstance_CLSCTX_INPROC_SERVER")},
        {"show_vtable_module", Json(actual_auto.wstring())}, {"show_vtable_module_sha256", Json(auto_hash)}, {"exact_allowed_auto", Boolean(exact_auto)},
        {"activation_bundle_index", attempt.activation_bundle ? Number(*attempt.activation_bundle) : "null"}, {"caller_bits", Number(sizeof(void*) * 8)},
        {"probe_enabled", Boolean(attempt.probe_enabled)}, {"probe_hr", Hr(attempt.probe_hr)}, {"probe_query_hr", Hr(attempt.probe_query_hr)}});
    if (configured != S_OK || !dialog || !exact_auto || auto_hash.empty()) { Status(L"用例" + std::to_wstring(scenario) + L"：未验证到指定包的 Auto 对象；没有打开选择器。请交回 activation 日志。" ); return; }
    receipt.pending = "configuration_failed_no_show";
    if (attempt.probe_enabled && attempt.probe_hr != E_INVALIDARG) {
        log->Record("attempt_setup_failed", {{"case", Number(scenario)}, {"attempt", Number(attempt.ordinal)}, {"reason", Json("INVALID_ROOT_PROBE_EXPECTED_E_INVALIDARG")}, {"probe_enabled", "true"}, {"probe_hr", Hr(attempt.probe_hr)}});
        Status(L"无效 root 探测未返回 E_INVALIDARG；本次不显示选择器，也不算通过。"); return;
    }
    ComPtr<IShellItem> folder; configured = SHCreateItemFromParsingName((save ? attempt.folder_b : attempt.initial).c_str(), nullptr, IID_PPV_ARGS(&folder));
    const DWORD flags = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | (save ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN : FOS_FILEMUSTEXIST);
    if (configured == S_OK) configured = dialog->SetOptions(flags);
    if (configured == S_OK) configured = dialog->SetFolder(folder.Get());
    bool initial_folder_b = false, initial_folder_a = false, initial_name_seed = false, metadata_before = false;
    HRESULT metadata_before_hr = E_PENDING;
    ComPtr<IFileSaveDialog> save_dialog; ComPtr<IShellItem> source;
    if (save && configured == S_OK) {
        ComPtr<IShellItem> observed; dialog->GetFolder(&observed); const auto before = ItemPath(observed.Get()); initial_folder_b = before && SamePath(*before, attempt.folder_b);
        configured = dialog.As(&save_dialog);
        if (configured == S_OK) configured = SHCreateItemFromParsingName(attempt.source.c_str(), nullptr, IID_PPV_ARGS(&source));
        if (configured == S_OK) configured = save_dialog->SetSaveAsItem(source.Get());
        observed.Reset(); dialog->GetFolder(&observed); const auto after = ItemPath(observed.Get()); initial_folder_a = after && SamePath(*after, attempt.source_parent);
        PWSTR name = nullptr; const HRESULT named = dialog->GetFileName(&name); initial_name_seed = named == S_OK && name && std::wstring_view(name) == L"seed.txt"; CoTaskMemFree(name);
        metadata_before = save_dialog && NoProperties(save_dialog.Get(), metadata_before_hr);
        log->Record("save_initial_item", {{"case", Number(scenario)}, {"attempt", Number(attempt.ordinal)}, {"set_folder_b_observed", Boolean(initial_folder_b)}, {"source_parent_a_observed", Boolean(initial_folder_a)},
            {"filename_seed_observed", Boolean(initial_name_seed)}, {"source_file_id_volume", Number(attempt.source_before.volume)}, {"source_file_id_high", Number(attempt.source_before.high)}, {"source_file_id_low", Number(attempt.source_before.low)},
            {"metadata_hr", Hr(metadata_before_hr)}, {"explicit_metadata_setters", "false"}});
    }
    ComPtr<IShellItemFilter> filter;
    bool retained = true;
    if (!save && configured == S_OK) {
        filter.Attach(new Filter(attempt)); configured = dialog->SetFilter(filter.Get());
        const ULONG after_set = attempt.metrics->references.load(); filter.Reset();
        retained = after_set >= 2 && !attempt.metrics->destroyed && attempt.metrics->references.load() >= 1;
        log->Record("filter_ownership", {{"case", Number(scenario)}, {"attempt", Number(attempt.ordinal)}, {"set_hr", Hr(configured)}, {"refs_after_set", Number(after_set)}, {"refs_after_caller_release", Number(attempt.metrics->references.load())}, {"retained", Boolean(retained)}});
    }
    if (configured == S_OK) configured = dialog->SetTitle(attempt.title.c_str());
    ComPtr<IFileDialogEvents> events; events.Attach(new Events(attempt)); DWORD cookie = 0;
    if (configured == S_OK) configured = dialog->Advise(events.Get(), &cookie);
    attempt.dialog = dialog.Get();
    if (configured != S_OK || log->failed) {
        if (cookie) dialog->Unadvise(cookie);
        log->Record("attempt_setup_failed", {{"case", Number(scenario)}, {"attempt", Number(attempt.ordinal)}, {"hr", Hr(configured)}});
        Status(L"用例" + std::to_wstring(scenario) + L"：配置失败，没有打开选择器。"); return;
    }
    Status(L"用例" + std::to_wstring(scenario) + L" 第" + std::to_wstring(attempt.ordinal) + L"次：等待您手动操作；不会自动输入、点击或继续下一项。");
    receipt.pending = "show_interrupted_without_terminal_receipt";
    attempt.visibility_probe = std::make_unique<VisibilityProbe>();
    attempt.visibility_probe->Start();
    active = &attempt; attempt.show_begin = GetTickCount64(); const auto timer = api_close_mode ? 0 : SetTimer(window, 1, 200, nullptr);
    HRESULT shown = E_PENDING; uint64_t show_finished = 0;
    { struct ActiveShow { App& app; Attempt& attempt; UINT_PTR timer; ~ActiveShow() { attempt.visibility_probe->Stop(); if (timer) KillTimer(app.window, timer); app.active = nullptr; } } hold{*this, attempt, timer};
        shown = dialog->Show(window); show_finished = GetTickCount64(); }
    const auto finished = show_finished; const auto probe_join_ms = GetTickCount64() - finished; receipt.pending = "post_show_processing_incomplete";
    attempt.visibility = attempt.visibility_probe->Snapshot();
    const bool probe_visible = attempt.visibility.visible && attempt.visibility.pid == attempt.ui_pid && attempt.same_activation_bundle && !attempt.bundle_mismatch_seen;
    attempt.remote_visible |= probe_visible;
    if (probe_visible) log->Record("remote_window_observed", {{"case", Number(scenario)}, {"attempt", Number(attempt.ordinal)}, {"observer", Json("independent_read_only_probe")},
        {"ui_pid", Number(attempt.visibility.pid)}, {"window_thread", Number(attempt.visibility.window_thread)}, {"same_activation_bundle", "true"}, {"visible", "true"},
        {"first_visible_after_show_ms", Number(attempt.visibility.first_visible >= attempt.show_begin ? attempt.visibility.first_visible - attempt.show_begin : 0)},
        {"visibility_samples", Number(attempt.visibility.visible_samples)}, {"observer_thread", Number(attempt.visibility.observer_thread)}, {"com_calls_on_observer_thread", "0"}});
    if (cookie) dialog->Unadvise(cookie); events.Reset();
    ComPtr<IShellItem> result; const HRESULT result_hr = dialog->GetResult(&result);
    const bool cancelled = shown == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const bool result_present = result != nullptr;
    const bool exact_result = result && ExactItem(result.Get(), attempt.target, !save);
    const bool remote_identity = attempt.remote_event_bound && attempt.luma && attempt.same_activation_bundle && !attempt.bundle_mismatch_seen &&
        attempt.ui_pid != GetCurrentProcessId() && !attempt.native_seen;
    const bool remote = remote_identity && attempt.remote_visible;
    bool consumed = false, original_unchanged = true, metadata_after = true, apply_okay = true;
    HRESULT metadata_after_hr = E_PENDING, applied = E_PENDING;
    if (save) {
        metadata_after = NoProperties(save_dialog.Get(), metadata_after_hr);
        original_unchanged = FileIdentity(attempt.source) == attempt.source_before && ReadEquals(attempt.source, kSeed);
        if (shown == S_OK && exact_result && remote && metadata_after && metadata_before && original_unchanged &&
            FileIdentity(attempt.initial, true) == attempt.parent_before && PhysicalAncestors(attempt.initial)) {
            consumed = CreatePayload(attempt.target, kWritten) && ReadEquals(attempt.target, kWritten);
            if (consumed) { ComPtr<IPropertyStore> empty; const HRESULT created = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&empty));
                applied = created == S_OK ? save_dialog->ApplyProperties(result.Get(), empty.Get(), window, nullptr) : created;
                apply_okay = applied == S_OK && ReadEquals(attempt.target, kWritten); }
            else apply_okay = false;
        } else apply_okay = false;
        original_unchanged = original_unchanged && FileIdentity(attempt.source) == attempt.source_before && ReadEquals(attempt.source, kSeed);
    } else if (scenario == 1) consumed = shown == S_OK && exact_result && ReadEquals(attempt.target, kBlocked);
    const HRESULT filter_clear = !save ? dialog->SetFilter(nullptr) : S_OK;
    source.Reset(); save_dialog.Reset(); dialog.Reset(); result.Reset(); folder.Reset();
    const auto& m = *attempt.metrics;
    const bool references_released = save || (m.destroyed && m.references.load() == 0);
    const bool filter_okay = save || (retained && references_released && m.caller_sta && m.enum_calls == 0 && m.includes > 0 && filter_clear == S_OK);
    const bool cancelled_empty = cancelled && FAILED(result_hr) && !result_present;
    const bool accepted = shown == S_OK && result_hr == S_OK && exact_result;
    const bool api_close_valid = attempt.api_close_called && attempt.api_close_hr == S_OK && attempt.api_close_thread == attempt.thread &&
        attempt.api_close_at_item == 40 && attempt.file_ok_events == 0;
    const bool pass = (api_close_mode ? remote_identity : remote) && (!attempt.probe_enabled || attempt.probe_hr == E_INVALIDARG) && !attempt.callback_error && filter_okay && !log->failed &&
        (scenario == 1 ? accepted && consumed && m.blocked > 0 && m.visible > 0 : scenario == 2 ? accepted && consumed && apply_okay && original_unchanged &&
            initial_folder_b && initial_folder_a && initial_name_seed && metadata_before && metadata_after : cancelled_empty && m.bulk.count() > 0 && (!api_close_mode || api_close_valid));
    const std::string outcome = api_close_mode ? (pass ? "passed_api_close_contract" : "api_close_not_passed") :
        pass ? "passed_observed_contract" : cancelled && scenario != 3 ? "not_passed_user_cancelled" : "not_passed_or_inconclusive";
    receipt.finalized = true;
    log->Record("attempt_terminal", {{"case", Number(scenario)}, {"attempt", Number(attempt.ordinal)}, {"outcome", Json(outcome)}, {"show_hr", Hr(shown)}, {"result_hr", Hr(result_hr)}, {"result_present", Boolean(result_present)}, {"exact_owned_result", Boolean(exact_result)},
        {"remote_event_bound", Boolean(attempt.remote_event_bound)}, {"remote_visible", Boolean(attempt.remote_visible)}, {"ui_pid", Number(attempt.ui_pid)}, {"luma_exact_bundle", Boolean(attempt.luma)}, {"native_seen", Boolean(attempt.native_seen)},
        {"same_activation_bundle", Boolean(attempt.same_activation_bundle)}, {"bundle_mismatch_seen", Boolean(attempt.bundle_mismatch_seen)},
        {"visibility_probe_started", Boolean(attempt.visibility.started)}, {"visibility_probe_bound", Boolean(attempt.visibility.bound)}, {"visibility_probe_visible", Boolean(probe_visible)},
        {"visibility_probe_samples", Number(attempt.visibility.samples)}, {"visibility_probe_visible_samples", Number(attempt.visibility.visible_samples)}, {"visibility_probe_rejected_samples", Number(attempt.visibility.rejected_samples)},
        {"visibility_probe_thread", Number(attempt.visibility.observer_thread)}, {"visibility_probe_bind_error", Number(attempt.visibility_bind_error)}, {"visibility_probe_module_error", Number(attempt.visibility.module_error)},
        {"visibility_probe_worker_error", Number(attempt.visibility.worker_error)}, {"visibility_probe_join_ms", Number(probe_join_ms)},
        {"owner_timer_ticks", Number(attempt.owner_timer_ticks)}, {"getwindow_failures", Number(attempt.getwindow_failures)}, {"last_getwindow_hr", Hr(attempt.last_getwindow_hr)},
        {"termination_source", Json(api_close_mode ? "caller_sta_IFileDialog_Close" : "user_picker_action")}, {"api_close_called", Boolean(attempt.api_close_called)},
        {"api_close_hr", Hr(attempt.api_close_hr)}, {"api_close_thread", Number(attempt.api_close_thread)}, {"api_close_at_unique_item", Number(attempt.api_close_at_item)},
        {"api_close_in_filter_callback", Boolean(api_close_mode && attempt.api_close_called)}, {"api_contract_pass", Boolean(api_close_mode && pass)}, {"visible_ui_pass", Boolean(!api_close_mode && pass)},
        {"probe_enabled", Boolean(attempt.probe_enabled)}, {"probe_hr", Hr(attempt.probe_hr)}, {"probe_query_hr", Hr(attempt.probe_query_hr)},
        {"module_snapshot_error", Number(attempt.module_error)}, {"folder_events", Number(attempt.folder_events)}, {"file_ok_events", Number(attempt.file_ok_events)}, {"caller_sta", Boolean(m.caller_sta && !attempt.callback_error)},
        {"filter_applicable", Boolean(!save)}, {"include_calls", Number(m.includes)}, {"get_enum_calls", Number(m.enum_calls.load())}, {"blocked_s_false", Number(m.blocked)}, {"visible_s_ok", Number(m.visible)}, {"filter_clear_hr", Hr(filter_clear)}, {"filter_refs_retained", Boolean(retained)}, {"filter_refs_released", Boolean(references_released)},
        {"filter_add_refs", Number(m.add_refs.load())}, {"filter_releases", Number(m.releases.load())}, {"bulk_unique_observed", Number(m.bulk.count())}, {"bulk_expected", Number(scenario == 3 ? 1000 : 0)},
        {"show_ms", Number(finished - attempt.show_begin)}, {"first_to_last_filter_ms", Number(m.last >= m.first ? m.last - m.first : 0)}, {"filter_callback_us_sum", Number(m.sum_us)},
        {"first_filter_after_show_ms", Number(m.first >= attempt.show_begin ? m.first - attempt.show_begin : 0)}, {"return_after_last_filter_ms", Number(m.last && finished >= m.last ? finished - m.last : 0)},
        {"cancel_phase", Json(api_close_mode ? "api_close_issued_from_filter_callback" : scenario != 3 ? "not_stress_case" : m.bulk.count() == 1000 ? "after_all_1000_filter_items_observed" : m.bulk.any() ? "before_all_1000_filter_items_observed" : "no_fixture_filter_observed")},
        {"during_filter_verified", "false"}, {"metadata_before_hr", Hr(metadata_before_hr)}, {"metadata_after_hr", Hr(metadata_after_hr)}, {"apply_empty_hr", Hr(applied)}, {"source_unchanged", Boolean(original_unchanged)},
        {"host_consumption_verified", Boolean(pass && consumed)}, {"consumption_operation", Json(save ? "write_reopen" : scenario == 1 ? "read" : "none")}, {"visual_listing_verified_by_host", "false"}, {"automatic_picker_input", "false"}});
    std::wstring text = L"用例" + std::to_wstring(scenario) + L" 第" + std::to_wstring(attempt.ordinal) + L"次：";
    text += pass && !log->failed ? L"已通过已观测契约（列表视觉仍请您确认）。" : cancelled && scenario != 3 ? L"本次已取消，未通过；不会弹出第二个窗口。" : L"未通过/证据不足；请保留JSONL日志。";
    if (scenario == 3) text += L" 已观察 " + std::to_wstring(m.bulk.count()) + L"/1000 项；" + (m.bulk.count() == 1000 ? std::wstring(L"这是完整枚举后取消，不是枚举中取消证据。") : std::wstring(L"未测得用户点击的精确时刻，不声称枚举中取消。"));
    Status(text);
    last_pass = pass && !log->failed;
}
void Layout(App& app) {
    RECT client{}; GetClientRect(app.window, &client); using Dpi = UINT(WINAPI*)(HWND);
    const auto dpi_fn = reinterpret_cast<Dpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT dpi = dpi_fn ? dpi_fn(app.window) : 96; const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int margin = scale(16), width = std::max(scale(400), static_cast<int>(client.right) - margin * 2), gap = scale(10), button_width = (width - gap * 2) / 3;
    MoveWindow(app.header, margin, margin, width, scale(48), TRUE);
    for (size_t i = 0; i < app.buttons.size(); ++i) MoveWindow(app.buttons[i], margin + static_cast<int>(i) * (button_width + gap), scale(75), button_width, scale(46), TRUE);
    const int result_height = scale(148), bottom = client.bottom - margin - result_height;
    MoveWindow(app.instructions, margin, scale(135), width, std::max(scale(80), bottom - scale(145)), TRUE);
    MoveWindow(app.results, margin, std::max(scale(225), bottom), width, result_height, TRUE);
}
UINT WindowDpi(HWND window) {
    using Dpi = UINT(WINAPI*)(HWND);
    const auto dpi = reinterpret_cast<Dpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return dpi ? dpi(window) : 96;
}
void UpdateFont(App& app) {
    LOGFONTW font{}; font.lfHeight = -MulDiv(17, static_cast<int>(WindowDpi(app.window)), 96);
    wcscpy_s(font.lfFaceName, L"Microsoft YaHei UI");
    const HFONT replacement = CreateFontIndirectW(&font); if (!replacement) return;
    for (const auto child : {app.header, app.instructions, app.results, app.buttons[0], app.buttons[1], app.buttons[2]}) if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
    if (app.font) DeleteObject(app.font); app.font = replacement;
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); app->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app)); }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    try {
        if (message == WM_CREATE) {
            app->header = CreateWindowExW(0, L"STATIC", L"Pulse 对话框手动验收\r\n仅由您操作；不会自动输入、点击、取消或进入下一项。", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, nullptr, nullptr);
            constexpr std::array<const wchar_t*, 3> labels{L"1  过滤列表：手输隐藏文件打开", L"2  SaveAsItem：另存并写后重开", L"3  1000 项过滤：手动取消"};
            for (size_t i = 0; i < labels.size(); ++i) app->buttons[i] = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_MULTILINE, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(101 + i)), nullptr, nullptr);
            const wchar_t* steps = L"每次只点一个按钮；选择器关闭后看下方结果，可手动重试。所有尝试都会保留，不会把取消/失败改成成功。\r\n\r\n"
                L"1  列表应显示 visible.txt，不应显示 blocked.txt。请在名称栏手动输入 blocked.txt 并点“打开”。主机只读取精确 owned 文件；也会核对过滤回调、STA、引用释放和真实远程 Pulse。列表视觉请您自行确认。\r\n\r\n"
                L"2  预先 SetFolder(B)，再 SetSaveAsItem(sourceA/seed.txt)。应进入 sourceA 并初填 seed.txt。请手动改成 saved-result.txt 后点“保存”。主机仅在结果精确匹配该新文件时 CREATE_NEW 写入并重新打开；不会写其他文件或改动原 seed。\r\n\r\n"
                L"3  目录含 1000 个 owned 文本文件。您可先观察列表，再自己点取消。主机记录实际回调数量/耗时/取消结果；若1000项已回调完毕，结果明确标记为枚举后取消，不冒称枚举中取消。\r\n\r\n"
                L"如果出现 Windows 原生窗口或结果不符，请自己取消，并把日志交回。关闭控制窗只结束本宿主，不更改对话框登记。";
            app->instructions = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", steps, WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 0, 0, window, nullptr, nullptr, nullptr);
            app->results = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", app->result_text.c_str(), WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 0, 0, window, nullptr, nullptr, nullptr);
            UpdateFont(*app); Layout(*app); return 0;
        }
        if (message == WM_SIZE) { Layout(*app); return 0; }
        if (message == WM_GETMINMAXINFO) { auto* info = reinterpret_cast<MINMAXINFO*>(lparam); const int dpi = static_cast<int>(WindowDpi(window)); info->ptMinTrackSize = {MulDiv(760, dpi, 96), MulDiv(590, dpi, 96)}; return 0; }
        if (message == WM_DPICHANGED) { const auto* bounds = reinterpret_cast<RECT*>(lparam); SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE); UpdateFont(*app); Layout(*app); return 0; }
        if (message == WM_TIMER && app->active) { ++app->active->owner_timer_ticks; app->Observe(*app->active, "passive_timer"); return 0; }
        if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) >= 101 && LOWORD(wparam) <= 103) {
            if (app->running) return 0;
            app->running = true;
            struct Running { App& app; ~Running() { app.running = false; for (const auto button : app.buttons) EnableWindow(button, TRUE); } } hold{*app};
            for (const auto button : app->buttons) EnableWindow(button, FALSE);
            app->Run(LOWORD(wparam) - 100);
            for (const auto button : app->buttons) EnableWindow(button, TRUE);
            return 0;
        }
        if (message == WM_CLOSE && app->running) { app->Status(L"请先由您手动关闭当前选择器，然后再关闭控制窗。"); return 0; }
        if (message == WM_DESTROY) { app->log->Record("session_end", {{"registration_mutations", "0"}, {"automatic_picker_input", "false"}}); if (app->font) DeleteObject(app->font); PostQuitMessage(0); return 0; }
    } catch (...) { app->active = nullptr; app->running = false; for (const auto button : app->buttons) if (button) EnableWindow(button, TRUE); if (app->results) SetWindowTextW(app->results, L"宿主内部异常；本次不算通过。请保留已有JSONL，不会自动重开选择器。"); }
    return DefWindowProcW(window, message, wparam, lparam);
}
void Context(App& app, bool dpi) {
    std::wstring sid; DWORD elevated = 0, container = 0, virtualized = 0, size = 0;
    HANDLE raw = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        Handle owned(raw); GetTokenInformation(raw, TokenUser, nullptr, 0, &size); std::vector<BYTE> bytes(size);
        if (size && GetTokenInformation(raw, TokenUser, bytes.data(), size, &size)) { PWSTR value = nullptr; if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &value)) { sid = value; LocalFree(value); } }
        TOKEN_ELEVATION elevation{}; if (GetTokenInformation(raw, TokenElevation, &elevation, sizeof(elevation), &size)) elevated = elevation.TokenIsElevated;
        GetTokenInformation(raw, TokenIsAppContainer, &container, sizeof(container), &size); GetTokenInformation(raw, TokenVirtualizationEnabled, &virtualized, sizeof(virtualized), &size);
    }
    using PackageName = LONG(WINAPI*)(UINT32*, PWSTR);
    const auto package_fn = reinterpret_cast<PackageName>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentPackageFullName"));
    UINT32 length = 0; LONG package_result = package_fn ? package_fn(&length, nullptr) : ERROR_PROC_NOT_FOUND; std::wstring package;
    if (package_result == ERROR_INSUFFICIENT_BUFFER && length < 32768) { package.resize(length); package_result = package_fn(&length, package.data()); if (package_result == ERROR_SUCCESS && length) package.resize(length - 1); }
    app.log->Record("context", {{"sid", Json(sid)}, {"elevated", Boolean(elevated != 0)}, {"appcontainer", Boolean(container != 0)}, {"token_virtualization", Boolean(virtualized != 0)}, {"caller_bits", Number(sizeof(void*) * 8)},
        {"image", Json(ImagePath(GetCurrentProcessId()).wstring())}, {"package_query_result", Number(static_cast<DWORD>(package_result))}, {"package_full_name", Json(package)}, {"pmv2_enabled", Boolean(dpi)},
        {"launch_surface", Json("not_inferred_verify_recorded_context")}, {"registration_mutations", "0"}, {"automatic_picker_input", "false"}});
    for (size_t i = 0; i < app.bundles.size(); ++i) app.log->Record("allowed_bundle", {{"index", Number(i)}, {"root", Json(app.bundles[i].wstring())},
        {"auto_sha256", Json(Sha256(app.bundles[i] / (sizeof(void*) == 8 ? L"PulseDialogAuto64.dll" : L"PulseDialogAuto32.dll")))}, {"pulse_sha256", Json(Sha256(app.bundles[i] / L"pulse.exe"))}});
}
void Help() {
    constexpr char help[] = "Pulse manual dialog acceptance host (no automated picker input)\r\n"
        "Usage: dialog_manual_acceptance_test.exe --bundle ABS_ROOT [--bundle PEER_ROOT] --out NEW_OWNED_ROOT\r\n"
        "API-only: add --api-close-test (no control window; one ordinary COM Show; caller-STA Close at filter item 40)\r\n"
        "Optional API probe: --probe-invalid-root (SetNavigationRoot(NULL) must return E_INVALIDARG before Show)\r\n"
        "       dialog_manual_acceptance_test.exe --help\r\n"
        "       dialog_manual_acceptance_test.exe --visibility-selftest (headless synthetic sampling only)\r\n"
        "Launch the configured shortcut yourself from the real Windows desktop.\r\n"
        "Three independent buttons: filtered typed-path Open; filesystem SaveAsItem; 1000-item manual cancel.\r\n"
        "No picker opens until you click a button. No keys, clicks, picker WM_COMMAND or automatic next case.\r\n"
        "The output parent must already exist. The output directory must NOT exist; fixtures/logs are retained.\r\n"
        "events.jsonl records all attempts and actual SID/package/Auto vtable/remote Pulse/lumatext evidence.\r\n"
        "--help is headless: no window, COM activation, fixture, registry change or picker.\r\n";
    DWORD written = 0; const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE); if (output && output != INVALID_HANDLE_VALUE) WriteFile(output, help, static_cast<DWORD>(sizeof(help) - 1), &written, nullptr);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    int count = 0; PWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count); if (!args) return 2;
    if (count == 2 && std::wstring_view(args[1]) == L"--help") { Help(); LocalFree(args); return 0; }
    if (count == 2 && std::wstring_view(args[1]) == L"--visibility-selftest") { LocalFree(args); return VisibilitySelfTest(); }
    App app; bool valid = count >= 5;
    for (int i = 1; valid && i < count;) {
        const std::wstring_view argument(args[i++]);
        if (argument == L"--api-close-test" && !app.api_close_mode) { app.api_close_mode = true; continue; }
        if (argument == L"--probe-invalid-root" && !app.probe_invalid_root) { app.probe_invalid_root = true; continue; }
        if (i >= count) { valid = false; break; }
        const auto path = AbsoluteLocal(args[i++]); if (!path) { valid = false; break; }
        if (argument == L"--bundle" && app.bundles.size() < 2 && PhysicalAncestors(*path)) app.bundles.push_back(*path);
        else if (argument == L"--out" && app.out.empty()) app.out = *path;
        else valid = false;
    }
    LocalFree(args);
    if (!valid || (app.probe_invalid_root && !app.api_close_mode) || app.bundles.empty() || app.out.empty() || !PhysicalAncestors(app.out.parent_path()) ||
        (app.bundles.size() == 2 && SamePath(app.bundles[0], app.bundles[1])) || GetFileAttributesW(app.out.c_str()) != INVALID_FILE_ATTRIBUTES || !CreateDirectoryW(app.out.c_str(), nullptr)) { Help(); return 2; }
    app.log = std::make_unique<Logger>(app.out / L"events.jsonl"); if (app.log->failed) return 2;
    using SetDpi = BOOL(WINAPI*)(HANDLE); const auto set_dpi = reinterpret_cast<SetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    const bool dpi = set_dpi && set_dpi(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))) != FALSE;
    Context(app, dpi); if (!dpi) { app.log->Record("startup_failed", {{"reason", Json("PMV2_NOT_ENABLED")}}); return 2; }
    const auto app_id = L"Pulse.ManualDialogAcceptance." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    const HRESULT app_id_result = SetCurrentProcessExplicitAppUserModelID(app_id.c_str());
    app.log->Record("test_app_identity", {{"app_user_model_id", Json(app_id)}, {"hr", Hr(app_id_result)}, {"client_guid_set", "false"}});
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); if (FAILED(initialized)) { app.log->Record("startup_failed", {{"com_hr", Hr(initialized)}}); return 2; }
    if (app.api_close_mode) {
        Handle completed(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!completed) { CoUninitialize(); return 2; }
        std::thread watchdog;
        try { watchdog = std::thread([event = completed.value] { if (WaitForSingleObject(event, 45000) == WAIT_TIMEOUT) ExitProcess(124); }); }
        catch (...) { CoUninitialize(); return 2; }
        try { app.Run(3); } catch (...) { app.last_pass = false; }
        SetEvent(completed.value); watchdog.join();
        app.log->Record("session_end", {{"mode", Json("api_close_test")}, {"api_contract_pass", Boolean(app.last_pass)}, {"visible_ui_pass", "false"}, {"registration_mutations", "0"}, {"automatic_picker_input", "false"}});
        CoUninitialize(); return app.last_pass && !app.log->failed ? 0 : 1;
    }
    WNDCLASSW type{}; type.hInstance = instance; type.lpfnWndProc = WindowProc; type.lpszClassName = kClass; type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    const ATOM registered = RegisterClassW(&type);
    const HWND window = registered ? CreateWindowExW(0, kClass, L"Pulse 手动对话框验收（由用户决定何时开始）", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 760, nullptr, nullptr, instance, &app) : nullptr;
    int exit_code = 2;
    if (window) { ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window); MSG message{}; BOOL received = 0;
        while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) { if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
        exit_code = received < 0 || app.log->failed ? 2 : 0;
    }
    CoUninitialize(); return exit_code;
}
