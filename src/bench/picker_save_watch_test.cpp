#include "../common/picker_save_watch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <winioctl.h>

namespace {
using namespace pulse::picker_save;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
int failures = 0;
uint64_t next_id = 1;
fs::path root;

void Check(bool passed, const char* name) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++failures;
}

Observer::Config Fast(std::chrono::milliseconds deadline = 2200ms) {
    Observer::Config config;
    config.quiet = 180ms;
    config.deadline = deadline;
    config.poll = 10ms;
    return config;
}

Request Make(const wchar_t* name) {
    Request request;
    request.path = (root / name).wstring();
    request.app_name = L"save observer test";
    request.source_pid = GetCurrentProcessId();
    request.request_id = next_id++;
    if (Capture(request.path, request.before) != ERROR_SUCCESS) ++failures;
    return request;
}

bool Write(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size();
    CloseHandle(file);
    return ok;
}

std::vector<Completion> Await(Observer& observer, std::chrono::milliseconds timeout = 1500ms) {
    const auto limit = std::chrono::steady_clock::now() + timeout;
    do {
        auto ready = observer.Drain();
        if (!ready.empty()) return ready;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < limit);
    return {};
}

void CreatedAndEmpty() {
    Observer observer(Fast());
    auto fresh = Make(L"created.txt");
    Check(!fresh.before.exists && observer.Submit(fresh), "new file accepted with absent baseline");
    Check(Write(fresh.path, "created bytes"), "new file writer succeeds");
    auto done = Await(observer);
    Check(done.size() == 1 && done[0].request.request_id == fresh.request_id &&
        done[0].after.exists && done[0].after.size == 13, "new file completes with real metadata");
    auto empty = Make(L"empty.txt");
    Check(observer.Submit(empty) && Write(empty.path, ""), "empty file created");
    done = Await(observer);
    Check(done.size() == 1 && done[0].after.exists && done[0].after.size == 0,
        "new empty file completes");
}

void HeldWriter() {
    Observer observer(Fast());
    auto request = Make(L"held.txt");
    Check(observer.Submit(request), "held writer request accepted");
    HANDLE writer = CreateFileW(request.path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, 0, nullptr);
    Check(writer != INVALID_HANDLE_VALUE, "held writer opens with permissive sharing");
    if (writer == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    Check(WriteFile(writer, "part1", 5, &written, nullptr) && written == 5, "held writer first chunk");
    Check(Await(observer, 430ms).empty(), "held writer never completes after quiet interval");
    Check(WriteFile(writer, "part2", 5, &written, nullptr) && written == 5, "held writer second chunk");
    Check(Await(observer, 430ms).empty(), "second chunk stays pending while writer remains open");
    CloseHandle(writer);
    const auto done = Await(observer);
    Check(done.size() == 1 && done[0].after.size == 10, "held writer completes only after close");
}

void ReopenAndUnchanged() {
    Observer observer(Fast());
    auto request = Make(L"reopen.txt");
    Check(observer.Submit(request) && Write(request.path, "first"), "reopen first write");
    std::this_thread::sleep_for(100ms);
    Check(observer.Drain().empty(), "no completion inside reopen gap");
    Check(Write(request.path, "second longer"), "reopen second write");
    Check(Await(observer, 110ms).empty(), "reopen restarts quiet interval");
    auto done = Await(observer);
    Check(done.size() == 1 && done[0].after.size == 13, "reopen completes with second write");
    auto unchanged = Make(L"reopen.txt");
    Check(unchanged.before.exists && observer.Submit(unchanged), "unchanged existing file accepted");
    Check(Await(observer, 420ms).empty(), "unchanged returned path is not a save");
    observer.Cancel(unchanged.request_id);
}

void AtomicReplaceAndOverwrite() {
    const auto path = (root / L"replace.txt").wstring();
    Check(Write(path, "old"), "replacement baseline written");
    auto request = Make(L"replace.txt");
    Observer observer(Fast());
    Check(observer.Submit(request), "replacement request accepted");
    const auto temp = (root / L"replace.tmp").wstring();
    Check(Write(temp, "new") && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING),
        "atomic same-size replacement succeeds");
    auto done = Await(observer);
    Check(done.size() == 1 && done[0].after.file_id != request.before.file_id,
        "atomic replacement detected by file identity");
    request = Make(L"replace.txt");
    Check(observer.Submit(request) && Write(path, "overwrite longer"), "overwrite writes existing target");
    done = Await(observer);
    Check(done.size() == 1 && done[0].after.size == 16, "overwrite completes");
}

void CancelTimeoutAndLatest() {
    Observer observer(Fast(700ms));
    auto canceled = Make(L"canceled.txt");
    Check(observer.Submit(canceled), "cancel request accepted");
    observer.Cancel(canceled.request_id);
    Check(Write(canceled.path, "canceled") && Await(observer, 300ms).empty(), "cancel suppresses late write");
    auto timeout = Make(L"timeout.txt");
    Check(observer.Submit(timeout) && Await(observer, 820ms).empty(), "missing file expires without completion");
    Check(Write(timeout.path, "too late") && Await(observer, 300ms).empty(), "expired request ignores later creation");
    auto old = Make(L"latest.txt");
    auto latest = old;
    latest.request_id = next_id++;
    Check(observer.Submit(old) && !observer.Submit(old), "duplicate active request ID rejected");
    Check(observer.Submit(latest) && Write(latest.path, "latest"), "same-path latest request replaces old");
    const auto done = Await(observer);
    Check(done.size() == 1 && done[0].request.request_id == latest.request_id,
        "only latest same-path request completes");
}

void CaptureErrorsAndCapacity() {
    Stamp stamp;
    Check(Capture(L"", stamp) == ERROR_INVALID_NAME && !stamp.exists, "invalid path is not missing-file success");
    Check(Capture((root / L"invalid|name").wstring(), stamp) == ERROR_INVALID_NAME && !stamp.exists,
        "filesystem invalid-name error stays distinct from missing target");
    Check(Capture(root.wstring(), stamp) == ERROR_DIRECTORY && !stamp.exists, "directory is excluded");
    auto offline = Make(L"offline.txt");
    Check(Write(offline.path, "offline") && SetFileAttributesW(offline.path.c_str(), FILE_ATTRIBUTE_OFFLINE),
        "offline fixture prepared");
    Check(Capture(offline.path, stamp) == ERROR_FILE_OFFLINE && !stamp.exists, "offline target excluded without content read");
    Check(SetFileAttributesW(offline.path.c_str(), FILE_ATTRIBUTE_NORMAL) != 0, "offline fixture attribute restored");
    auto locked = Make(L"locked.txt");
    HANDLE file = CreateFileW(locked.path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "exclusive fixture opens");
    if (file != INVALID_HANDLE_VALUE) {
        Check(Capture(locked.path, stamp) == ERROR_SUCCESS && stamp.exists,
            "metadata-only baseline can inspect an exclusive writer without reading data");
        CloseHandle(file);
    }
    auto config = Fast();
    config.capacity = 1;
    Observer bounded(config);
    auto first = Make(L"bounded1.txt");
    auto second = Make(L"bounded2.txt");
    Check(bounded.Submit(first) && !bounded.Submit(second), "pending queue capacity enforced");
    Check(Write(first.path, "one"), "bounded target writes");
    std::this_thread::sleep_for(350ms);
    Check(!bounded.Submit(second), "ready queue also consumes bounded capacity");
    bounded.Cancel(first.request_id);
    Check(bounded.Drain().empty() && bounded.Submit(second), "cancel removes queued completion and frees capacity");
    bounded.Cancel(second.request_id);
}

void DefaultQuietAndDestruction() {
    auto request = Make(L"default-quiet.txt");
    Observer observer;
    Check(observer.Submit(request) && Write(request.path, "default"), "production-default observer accepted write");
    Check(Await(observer, 1100ms).empty(), "production default does not complete before 1500ms quiet");
    Check(Await(observer, 1400ms).size() == 1, "production default completes after stable quiet interval");
    const auto begin = std::chrono::steady_clock::now();
    {
        Observer pending;
        Check(pending.Submit(Make(L"destroy-pending.txt")), "destruction fixture has pending request");
    }
    Check(std::chrono::steady_clock::now() - begin < 500ms, "destruction does not wait for request deadline");
}

void LongPathAndReparse() {
    const auto long_directory = L"\\\\?\\" + root.wstring() + L"\\" + std::wstring(90, L'a') +
        L"\\" + std::wstring(90, L'b');
    std::error_code error;
    Check(fs::create_directories(long_directory, error) && !error, "long-path directories prepared");
    Request request;
    request.path = long_directory + L"\\long.txt";
    request.request_id = next_id++;
    Check(request.path.size() > MAX_PATH && Capture(request.path, request.before) == ERROR_SUCCESS,
        "long-path absent baseline captured");
    Observer observer(Fast());
    Check(observer.Submit(request) && Write(request.path, "long path"), "long-path save writes");
    Check(Await(observer).size() == 1, "extended long-path save completes");

    const auto target = (root / L"junction-target").wstring();
    const auto link = (root / L"junction-link").wstring();
    Check(CreateDirectoryW(target.c_str(), nullptr) && CreateDirectoryW(link.c_str(), nullptr),
        "reparse fixture directories prepared");
    HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Check(directory != INVALID_HANDLE_VALUE, "reparse fixture handle opened");
    if (directory == INVALID_HANDLE_VALUE) return;
    struct JunctionData {
        DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        WORD data_length = 0;
        WORD reserved = 0;
        WORD substitute_offset = 0;
        WORD substitute_length = 0;
        WORD print_offset = 0;
        WORD print_length = 0;
        wchar_t paths[1024]{};
    } data;
    const auto substitute = L"\\??\\" + target;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.print_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data.data_length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data.paths);
    std::copy(target.begin(), target.end(), data.paths + data.print_offset / sizeof(wchar_t));
    DWORD bytes = 0;
    const bool created = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(8 + data.data_length), nullptr, 0, &bytes, nullptr) != 0;
    CloseHandle(directory);
    Check(created, "real junction created without symbolic-link privilege");
    Stamp stamp;
    Check(Capture(link, stamp) == ERROR_REPARSE_TAG_INVALID && !stamp.exists,
        "reparse target rejected without following junction");
}

void ReplaceReadyCompletion() {
    Observer observer(Fast());
    auto old = Make(L"ready-replace.txt");
    Check(observer.Submit(old) && Write(old.path, "old ready"), "ready replacement first save writes");
    std::this_thread::sleep_for(400ms);
    auto latest = Make(L"ready-replace.txt");
    Check(observer.Submit(latest) && observer.Drain().empty(), "latest request removes old undrained completion");
    Check(Await(observer, 250ms).empty(), "latest request waits for change against its own baseline");
    Check(Write(latest.path, "newer ready"), "ready replacement latest write succeeds");
    const auto done = Await(observer);
    Check(done.size() == 1 && done[0].request.request_id == latest.request_id,
        "ready replacement returns only latest completion");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const fs::path base = argc > 1 ? fs::path(argv[1]) : fs::path(L"bench_data/picker-save-followup-20261126");
    root = fs::absolute(base / (L"observer-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64())));
    std::error_code error;
    if (!fs::create_directories(root, error) || error) {
        std::printf("[FAIL] isolated fixture directory creation\n");
        return 2;
    }
    CreatedAndEmpty();
    HeldWriter();
    ReopenAndUnchanged();
    AtomicReplaceAndOverwrite();
    CancelTimeoutAndLatest();
    CaptureErrorsAndCapacity();
    DefaultQuietAndDestruction();
    LongPathAndReparse();
    ReplaceReadyCompletion();
    std::printf("picker_save_watch_test: failures=%d\n", failures);
    return failures ? 1 : 0;
}
