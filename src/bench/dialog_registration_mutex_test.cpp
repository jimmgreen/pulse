#include "../common/current_user_security.h"
#ifndef PULSE_MUTEX_BASELINE
#include "../common/dialog_registration_mutex.h"
#endif
#include <cstdio>
#include <thread>
#include <atomic>

int wmain() {
    int failures = 0;
    const auto check = [&](bool okay, const char* label) {
        std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", label);
        failures += !okay;
    };
    const auto sid = pulse::CurrentUserSidString();
    const auto name = L"Local\\Pulse.RegistrationMutex.Test." + std::to_wstring(GetCurrentProcessId());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto sddl = L"D:P(A;;0x00100000;;;" + sid + L")";
    check(!sid.empty() && ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
        SDDL_REVISION_1, &descriptor, nullptr), "create current-user SYNCHRONIZE-only descriptor");
    if (!descriptor) return 2;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    HANDLE keeper = CreateMutexExW(&security, name.c_str(), 0, SYNCHRONIZE);
    LocalFree(descriptor);
    check(keeper != nullptr, "create isolated restricted mutex");
    if (!keeper) return 2;
    SetLastError(ERROR_SUCCESS);
    HANDLE legacy = CreateMutexW(nullptr, FALSE, name.c_str());
    const DWORD legacy_error = GetLastError();
    check(!legacy && legacy_error == ERROR_ACCESS_DENIED, "legacy ALL_ACCESS open fails with error 5");
    if (legacy) CloseHandle(legacy);
    const auto open = [&] {
#ifdef PULSE_MUTEX_BASELINE
        return CreateMutexW(nullptr, FALSE, name.c_str());
#else
        return pulse::dialog_auto::CreateRegistrationMutex(name.c_str());
#endif
    };
    HANDLE mutex = open();
    check(mutex != nullptr, "registration mutex opens with granted synchronization rights");
    if (mutex) {
        const DWORD first = WaitForSingleObject(mutex, 0);
        const DWORD recursive = first == WAIT_OBJECT_0 ? WaitForSingleObject(mutex, 0) : WAIT_FAILED;
        check(first == WAIT_OBJECT_0 && recursive == WAIT_OBJECT_0, "same thread acquires recursively");
        if (first == WAIT_OBJECT_0 && recursive == WAIT_OBJECT_0) {
            HANDLE proceed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            std::atomic<int> observed{0};
            std::thread contender([&] {
                HANDLE other = open();
                if (other && WaitForSingleObject(other, 0) == WAIT_TIMEOUT) observed.fetch_add(1);
                SetEvent(done);
                WaitForSingleObject(proceed, 5000);
                if (other && WaitForSingleObject(other, 0) == WAIT_TIMEOUT) observed.fetch_add(1);
                SetEvent(done);
                WaitForSingleObject(proceed, 5000);
                if (other && WaitForSingleObject(other, 1000) == WAIT_OBJECT_0) {
                    observed.fetch_add(1);
                    if (ReleaseMutex(other)) observed.fetch_add(1);
                }
                if (other) CloseHandle(other);
            });
            check(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 && observed == 1,
                "other thread stays excluded while recursively owned");
            check(ReleaseMutex(mutex) != FALSE, "first recursive release succeeds with synchronization-only handle");
            SetEvent(proceed);
            check(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 && observed == 2,
                "one release does not expose a recursively owned mutex");
            check(ReleaseMutex(mutex) != FALSE, "final recursive release succeeds");
            SetEvent(proceed);
            contender.join();
            check(observed == 4, "other thread acquires after final release and releases successfully");
            CloseHandle(done); CloseHandle(proceed);
        } else if (first == WAIT_OBJECT_0) ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    CloseHandle(keeper);
    std::printf("FAILURES=%d\n", failures);
    return failures ? 1 : 0;
}
