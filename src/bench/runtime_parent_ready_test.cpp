#include "../shell_bridge/runtime_parent_ready.h"
#include <filesystem>
#include <memory>
#include <cstdio>
namespace config = pulse::shell_bridge::runtime;
int main() {
    unsigned checks = 0, failures = 0;
    auto check = [&](bool ok, const char* text) {
        ++checks; if (!ok) ++failures;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
    };
    const auto directory = std::filesystem::current_path() / L"bench_data" /
        (L"parent-ready-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    check(CreateDirectoryW(directory.c_str(), nullptr) != FALSE, "exclusive owned receipt fixture");
    if (failures) return 1;
    auto session = std::make_unique<config::Session>();
    session->target_pid = GetCurrentProcessId(); session->machine = IMAGE_FILE_MACHINE_AMD64;
    FILETIME exited{}, kernel{}, user{};
    check(GetProcessTimes(GetCurrentProcess(), &session->target_created, &exited, &kernel, &user) != FALSE, "current process identity");
    wcscpy_s(session->nonce, L"{11111111-1111-1111-1111-111111111111}");
    const auto receipt = directory / config::kReadyFile;
    check(config::ValidateRuntimeReadyReceipt(directory.wstring(), *session) == ERROR_FILE_NOT_FOUND,
        "parent still alive without success receipt denies descendant UI even without disable marker");
    config::Ready ready; ready.target_pid = session->target_pid; ready.machine = session->machine;
    ready.target_created = session->target_created; wcscpy_s(ready.nonce, session->nonce);
    ready.installed_entries = config::kCoCreateInstance; ready.status = S_OK;
    auto write = [&](const config::Ready& value, DWORD bytes = sizeof(config::Ready)) {
        HANDLE file = CreateFileW(receipt.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, &value, bytes, &written, nullptr) && written == bytes;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        check(ok, "write owned exact receipt");
        return ok;
    };
    auto reject = [&](config::Ready value, const char* label, DWORD bytes = sizeof(config::Ready)) {
        if (write(value, bytes)) {
            check(config::ValidateRuntimeReadyReceipt(directory.wstring(), *session) != ERROR_SUCCESS, label);
            check(DeleteFileW(receipt.c_str()) != FALSE, "remove owned negative receipt");
        }
    };
    auto bad = ready; bad.status = E_FAIL; reject(bad, "failed initialization receipt cannot authorize");
    bad = ready; ++bad.target_pid; reject(bad, "other process receipt rejected");
    bad = ready; ++bad.target_created.dwLowDateTime; reject(bad, "reused PID creation mismatch rejected");
    bad = ready; bad.nonce[1] = L'2'; reject(bad, "other session receipt rejected");
    bad = ready; bad.installed_entries = 0; reject(bad, "missing installed entry rejected");
    reject(ready, "partial publication rejected", sizeof(config::Ready) - 1);
    if (write(ready)) {
        check(config::ValidateRuntimeReadyReceipt(directory.wstring(), *session) == ERROR_SUCCESS,
            "exact successful published receipt supplies positive authority");
        check(DeleteFileW(receipt.c_str()) != FALSE, "remove owned successful receipt");
    }
    if (!failures) check(RemoveDirectoryW(directory.c_str()) != FALSE, "remove empty owned fixture");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
