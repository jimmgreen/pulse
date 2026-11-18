#define PULSE_NATIVE_DELETE_TEST
#include <windows.h>
namespace {
unsigned work_attempts = 0, work_created = 0, work_closed = 0;
bool fail_work_creation = false;
PTP_WORK WINAPI TestCreateWork(PTP_WORK_CALLBACK callback, PVOID context, PTP_CALLBACK_ENVIRON environment) {
    ++work_attempts;
    if (fail_work_creation) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    auto work = CreateThreadpoolWork(callback, context, environment);
    work_created += work != nullptr;
    return work;
}
void WINAPI TestCloseWork(PTP_WORK work) { ++work_closed; CloseThreadpoolWork(work); }
void ResetWorkCounts() { work_attempts = work_created = work_closed = 0; }
}
#define CreateThreadpoolWork TestCreateWork
#define CloseThreadpoolWork TestCloseWork
#include "../ops/native_delete_stream.h"
#undef CreateThreadpoolWork
#undef CloseThreadpoolWork
#include <filesystem>
#include <winioctl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs = std::filesystem;
int wmain() {
    const fs::path root = fs::current_path() / L"bench_data" /
        (L"delete_stream_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    if (fs::exists(root) || !fs::create_directories(root)) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; failures += !ok; };
    std::atomic<bool> cancel{false};
    auto run = [&](const fs::path& path) { return pulse::ops::native_delete::DeleteNative(path.wstring(), cancel, [](const auto&, float) {}); };
    auto tree = root / L"success";
    fs::create_directories(tree / L"nested");
    std::ofstream(tree / L"nested" / L"file") << "fixture";
    check(run(tree).done && !fs::exists(tree), "nested postorder success");
    fs::create_directory(tree);
    check(run(tree).done && !fs::exists(tree), "empty tree");
    fs::create_directory(tree);
    for (int i = 0; i < 1024; ++i) std::ofstream(tree / std::to_wstring(i)) << "fixture";
    auto stopped = pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [&](const auto&, float) { cancel.store(true); });
    check(stopped.cancelled && stopped.mutated && !stopped.done && fs::exists(tree), "wide directory cancellation after first mutation");
    cancel.store(false);
    auto remaining = run(tree);
    check(remaining.done, "explicit new request cleans remaining fixture");
    fs::create_directory(tree);
    std::ofstream(tree / L"first") << "fixture";
    std::ofstream(tree / L"second") << "fixture";
    auto late_error = pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [&](const auto&, float) {
        pulse::ops::native_delete::fail_next_enumeration = true;
    });
    check(!late_error.done && late_error.mutated && late_error.enumeration_failed &&
          late_error.error == ERROR_READ_FAULT && fs::exists(tree), "late enumeration failure preserves partial outcome");
    check(fs::directory_iterator(tree) != fs::directory_iterator{}, "late enumeration failure does not replay remaining root");
    pulse::ops::native_delete::fail_next_enumeration = false;
    check(run(tree).done, "explicit cleanup after enumeration failure");
    fs::create_directories(tree / L"child");
    std::ofstream(tree / L"child" / L"file") << "fixture";
    check(run(tree / L"child").done && run(tree).done, "serial descendant then ancestor roots");
    fs::create_directories(tree / L"child");
    std::ofstream(tree / L"child" / L"file") << "fixture";
    const auto ancestor = run(tree);
    const auto absent_child = run(tree / L"child");
    check(ancestor.done && !absent_child.mutated, "serial ancestor then descendant has no second mutation");
    const auto duplicate = run(tree);
    check(!duplicate.mutated, "duplicate deleted root has no mutation");
    fs::create_directory(tree);
    pulse::ops::native_delete::deny_first_enumeration = true;
    const auto denied = run(tree);
    check(!denied.done && !denied.mutated && !denied.enumeration_failed &&
          denied.error == ERROR_ACCESS_DENIED, "initial enumeration denial permits elevation fallback");
    check(run(tree).done, "cleanup initially denied fixture");
    fs::create_directory(tree);
    std::ofstream(tree / L"guarded") << "fixture";
    bool rename_attempted = false;
    bool rename_blocked = false;
    const auto guarded = pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [&](const auto&, float) {
        if (!rename_attempted) {
            rename_attempted = true;
            rename_blocked = !MoveFileExW(tree.c_str(), (root / L"renamed").c_str(), 0);
        }
    });
    printf("guard diagnostic done=%d error=%lu attempted=%d blocked=%d\n", guarded.done, guarded.error, rename_attempted, rename_blocked);
    check(guarded.done && rename_attempted && rename_blocked, "active directory guard prevents replacement");
    fs::create_directory(tree);
    for (int i = 0; i < 8; ++i) std::ofstream(tree / std::to_wstring(i));
    pulse::ops::native_delete::force_disposition_unsupported = true;
    pulse::ops::native_delete::disposition_probes = 0;
    check(run(tree).done && pulse::ops::native_delete::disposition_probes == 1,
          "unsupported disposition probes once per root");
    fs::create_directory(tree);
    check(run(tree).done && pulse::ops::native_delete::disposition_probes == 2,
          "next root probes disposition capability again");
    pulse::ops::native_delete::force_disposition_unsupported = false;
    ResetWorkCounts();
    fs::create_directory(tree);
    for (int i = 0; i < 96; ++i) std::ofstream(tree / std::to_wstring(i));
    check(pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [](const auto&, float) {}, 4).done,
          "four worker bounded batches succeed");
    check(work_created == 1 && work_closed == 1, "one work object spans all drains and closes at traversal end");
    fs::create_directory(tree);
    std::ofstream(tree / L"single");
    ResetWorkCounts();
    check(pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [](const auto&, float) {}, 4).done && work_attempts == 0,
          "singleton batch does not create threadpool work");
    fs::create_directory(tree);
    for (int i = 0; i < 96; ++i) std::ofstream(tree / std::to_wstring(i));
    ResetWorkCounts();
    fail_work_creation = true;
    const auto creation_failure = pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel, [](const auto&, float) {}, 4);
    fail_work_creation = false;
    check(creation_failure.done && work_attempts == 1 && work_created == 0,
          "work creation failure uses synchronous fallback for the remaining traversal");
    fs::create_directory(tree);
    for (int i = 0; i < 96; ++i) std::ofstream(tree / std::to_wstring(i));
    ResetWorkCounts();
    bool progress_threw = false;
    try {
        pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel,
            [](const auto&, float) { throw std::runtime_error("fixture progress exception"); }, 4);
    } catch (const std::runtime_error&) { progress_threw = true; }
    check(progress_threw && work_created == 1 && work_closed == 1,
          "progress exception joins callbacks and releases persistent work");
    check(run(tree).done, "exception fixture cleanup");
    fs::create_directory(tree);
    for (int i = 0; i < 96; ++i) std::ofstream(tree / std::to_wstring(i));
    auto batch_cancel = pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel,
        [&](const auto&, float) { cancel.store(true); }, 4);
    check(batch_cancel.cancelled && batch_cancel.mutated && !batch_cancel.done && fs::exists(tree),
          "four worker cancellation retains partial outcome");
    cancel.store(false);
    check(run(tree).done, "batch cancellation fixture cleanup");
    fs::create_directory(tree);
    for (int i = 0; i < 96; ++i) std::ofstream(tree / std::to_wstring(i));
    HANDLE batch_lock = INVALID_HANDLE_VALUE;
    const auto locked_path = tree / L"locked";
    const auto batch_failure = pulse::ops::native_delete::DeleteNative(tree.wstring(), cancel,
        [&](const auto&, float) {
            if (batch_lock == INVALID_HANDLE_VALUE) {
                std::ofstream(locked_path) << "fixture";
                batch_lock = CreateFileW(locked_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, 0, nullptr);
            }
        }, 4);
    check(batch_failure.mutated && !batch_failure.done && batch_failure.error != ERROR_SUCCESS,
          "four worker locked remainder preserves partial failure");
    if (batch_lock != INVALID_HANDLE_VALUE) CloseHandle(batch_lock);
    check(run(tree).done, "batch failure fixture cleanup");
    auto outside = root / L"target";
    fs::create_directory(outside);
    std::ofstream(outside / L"keep") << "fixture";
    auto link = root / L"link";
    if (CreateSymbolicLinkW(link.c_str(), outside.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 2)) {
        check(run(link).done && fs::exists(outside / L"keep"), "directory link does not follow target");
    } else { std::cout << "[SKIP] symlink creation unavailable: " << GetLastError() << '\n'; }
    const auto junction = root / L"junction";
    fs::create_directory(junction);
    struct JunctionData {
        DWORD tag; WORD length; WORD reserved;
        WORD substitute_offset; WORD substitute_length; WORD print_offset; WORD print_length;
        wchar_t names[4096];
    } data{};
    const std::wstring target = L"\\??\\" + outside.wstring();
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.print_offset + sizeof(wchar_t));
    memcpy(data.names, target.c_str(), data.substitute_length);
    HANDLE junction_handle = CreateFileW(junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    DWORD returned = 0;
    const BOOL junction_ok = DeviceIoControl(junction_handle, FSCTL_SET_REPARSE_POINT, &data,
        data.length + 8, nullptr, 0, &returned, nullptr);
    if (junction_handle != INVALID_HANDLE_VALUE) CloseHandle(junction_handle);
    check(junction_ok && run(junction).done && fs::exists(outside / L"keep"), "junction does not follow target");
    cancel.store(true);
    check(run(outside).cancelled && fs::exists(outside / L"keep"), "pre-cancel preserves tree");
    cancel.store(false);
    // Only this process-created root is removed; no external fixtures are touched.
    check(run(root).done && !fs::exists(root), "isolated fixture cleanup");
    return failures ? 1 : 0;
}
