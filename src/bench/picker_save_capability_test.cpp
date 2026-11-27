#include "../common/picker_places_snapshot.h"
#include <cstdio>
#include <string_view>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::picker_places;
    if (argc == 4 && std::wstring_view(argv[1]) == L"--child") {
        Snapshot snapshot;
        ReadSharedVersion(snapshot, true, std::wstring(argv[2]) + L".places");
        snapshot.save_followup_available = true;
        save_capability_detail::Read(snapshot, argv[2]);
        return snapshot.save_followup_available == (std::wstring_view(argv[3]) == L"enabled") ? 0 : 1;
    }
    int failures = 0;
    auto check = [&](bool condition, const char* label) {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) ++failures;
    };
    const std::wstring name = L"Local\\Pulse.PickerSaveCapability.Test." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HWND endpoint = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    detail::SharedWriter shared(true, name + L".places");
    if (!endpoint || !shared.blob) { std::puts("[FAIL] fixture creation"); return 2; }
    Snapshot snapshot;
    snapshot.endpoint = endpoint;
    snapshot.save_followup_available = true;
    PublishSharedVersion(snapshot, shared, true);
    auto child = [&](bool enabled, const std::wstring& mapping = L"") {
        wchar_t exe[32768]{};
        if (!GetModuleFileNameW(nullptr, exe, 32768)) return false;
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --child \"" +
            (mapping.empty() ? name : mapping) + L"\" " + (enabled ? L"enabled" : L"disabled");
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process)) return false;
        DWORD code = 99;
        if (WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
        else { TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, 5000); }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return code == 0;
    };
    check(!Snapshot{}.save_followup_available, "new snapshot defaults to unavailable");
    check(child(false), "old publisher without capability mapping remains unavailable");
    {
        save_capability_detail::Writer writer(name);
        if (!writer.blob || !writer.mutex) { std::puts("[FAIL] capability mapping creation"); return 2; }
        save_capability_detail::Publish(snapshot, writer);
        const auto valid = writer.blob->payload;
        check(child(true), "cross-process enabled capability for live endpoint");
        auto reject = [&](const char* label, auto mutate) {
            writer.blob->payload = valid;
            mutate(writer.blob->payload);
            check(child(false), label);
        };
        reject("cross-process bad magic rejected", [](auto& value) { ++value.magic; });
        reject("cross-process unknown version rejected", [](auto& value) { ++value.version; });
        reject("cross-process mismatched PID rejected", [](auto& value) { ++value.pid; });
        reject("cross-process absent PID rejected", [](auto& value) { value.pid = 0; });
        reject("cross-process missing HWND rejected", [](auto& value) { value.endpoint = 0; });
        HWND other = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        reject("cross-process different live HWND rejected", [&](auto& value) { value.endpoint = reinterpret_cast<ULONG_PTR>(other); });
        DestroyWindow(other);
        reject("cross-process stale timestamp rejected", [](auto& value) { value.timestamp = GetTickCount64() - 10001; });
        reject("cross-process future timestamp rejected", [](auto& value) { value.timestamp = GetTickCount64() + 10000; });
        reject("cross-process malformed boolean rejected", [](auto& value) { value.available = 2; });
        writer.blob->payload = valid;
        InterlockedIncrement(&writer.blob->sequence);
        check(child(false), "bounded reader rejects in-progress writer");
        InterlockedIncrement(&writer.blob->sequence);
        snapshot.endpoint_pid = GetCurrentProcessId();
        snapshot.shared_available = true;
        check(save_capability_detail::Valid(valid, snapshot, valid.timestamp + 10000) &&
            !save_capability_detail::Valid(valid, snapshot, valid.timestamp + 10001), "freshness boundary is inclusive 10 seconds");
        snapshot.save_followup_available = false;
        save_capability_detail::Publish(snapshot, writer);
        check(child(false), "cross-process disabling clears previous capability");
        snapshot.save_followup_available = true;
        save_capability_detail::Publish(snapshot, writer);
        check(child(true), "cross-process reenable publishes latest capability");

        HANDLE locked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!locked || !release) return 2;
        std::thread holder([&] {
            WaitForSingleObject(writer.mutex, INFINITE);
            SetEvent(locked);
            WaitForSingleObject(release, INFINITE);
            ReleaseMutex(writer.mutex);
        });
        WaitForSingleObject(locked, INFINITE);
        snapshot.save_followup_available = false;
        const auto before = GetTickCount64();
        save_capability_detail::Publish(snapshot, writer);
        const auto elapsed = GetTickCount64() - before;
        SetEvent(release);
        holder.join();
        CloseHandle(release);
        CloseHandle(locked);
        check(elapsed < 100 && child(true), "contended publisher returns without blocking or corrupting latest snapshot");
    }
    check(child(false), "closed capability mapping clears stale reader state");
    {
        save_capability_detail::Writer writer(name);
        snapshot.save_followup_available = true;
        save_capability_detail::Publish(snapshot, writer);
        DestroyWindow(endpoint);
        check(child(false), "closed endpoint makes capability unavailable");
    }
    std::printf("RESULT: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
