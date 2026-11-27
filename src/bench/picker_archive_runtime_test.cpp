#include "../app/picker_archive_runtime.h"
#include <iostream>

namespace {
struct Package { pulse::extensions::Manifest manifest; bool enabled = true; std::wstring native; };
std::vector<Package> packages;
constexpr wchar_t kWorker[] = L"C:\\插件\\pulse.7zip\\pulse_7zip_worker.exe";
}
namespace pulse::extensions {
size_t PackageCount() { return packages.size(); }
const Manifest* PackageManifest(size_t index) { return index < packages.size() ? &packages[index].manifest : nullptr; }
bool PackageEnabled(size_t index) { return index < packages.size() && packages[index].enabled; }
std::wstring PackageNativePath(size_t index) { return index < packages.size() ? packages[index].native : std::wstring{}; }
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::picker_places;
    if (argc == 5 && std::wstring_view(argv[1]) == L"--child") {
        Snapshot loaded;
        ReadSharedVersion(loaded, true, std::wstring(argv[2]) + L".places");
        if (std::wstring_view(argv[4]) == L"no-endpoint") loaded.endpoint = nullptr;
        loaded.archive_worker = L"stale";
        archive_runtime_detail::Read(loaded, argv[2]);
        return loaded.archive_worker == (std::wstring_view(argv[3]) == L"worker" ? kWorker : L"") ? 0 : 1;
    }
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failed;
    };
    packages.emplace_back();
    packages.back().manifest.id = "unrelated";
    packages.back().native = L"C:\\unrelated\\plugin.dll";
    packages.emplace_back();
    packages.back().manifest.id = "pulse.7zip";
    packages.back().native = L"C:\\插件\\pulse.7zip\\pulse_7zip_native.dll";
    check(pulse::app::PickerArchiveWorker() == kWorker, "enabled startup package path");
    packages.back().enabled = false;
    check(pulse::app::PickerArchiveWorker().empty(), "disabled package omitted");
    packages.back().enabled = true;
    packages.back().native = L"relative\\plugin.dll";
    check(pulse::app::PickerArchiveWorker().empty(), "relative startup path omitted");
    packages.back().native = L"C:\\插件\\pulse.7zip\\pulse_7zip_native.dll";

    const auto name = L"Local\\Pulse.PickerArchive.Test." + std::to_wstring(GetCurrentProcessId());
    archive_runtime_detail::Writer writer(name);
    detail::SharedWriter shared(true, name + L".places");
    HWND endpoint = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!endpoint || !writer.blob || !shared.blob) { std::cout << "[FAIL] fixture creation\n"; return 1; }
    Snapshot snapshot;
    snapshot.endpoint = endpoint;
    snapshot.archive_worker = pulse::app::PickerArchiveWorker();
    PublishSharedVersion(snapshot, shared, true);
    archive_runtime_detail::Publish(snapshot, writer);
    const auto valid = writer.blob->payload;
    auto child = [&](bool expected, const wchar_t* mode = L"normal", const std::wstring& mapping = L"") {
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --child \"" + (mapping.empty() ? name : mapping) +
            L"\" " + (expected ? L"worker" : L"empty") + L" " + mode;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
        const auto wait = WaitForSingleObject(process.hProcess, 15000);
        DWORD code = 999;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
        else { TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, 5000); }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        return code == 0;
    };
    check(child(true), "cross-process enabled Unicode worker");
    auto reject = [&](const char* label, auto mutate) {
        writer.blob->payload = valid;
        mutate(writer.blob->payload);
        check(child(false), label);
    };
    reject("cross-process expired timestamp", [](auto& p) { p.timestamp = GetTickCount64() - 10001; });
    reject("cross-process future timestamp", [](auto& p) { p.timestamp = GetTickCount64() + 10000; });
    reject("cross-process mismatched PID", [](auto& p) { ++p.pid; });
    reject("cross-process missing endpoint", [](auto& p) { p.endpoint = 0; });
    HWND other_endpoint = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    reject("cross-process different live endpoint", [&](auto& p) { p.endpoint = reinterpret_cast<ULONG_PTR>(other_endpoint); });
    DestroyWindow(other_endpoint);
    reject("cross-process unknown version", [](auto& p) { ++p.version; });
    reject("cross-process oversized path", [](auto& p) { p.path_length = 1024; });
    reject("cross-process embedded NUL", [](auto& p) { p.worker[3] = 0; });
    writer.blob->payload = valid;
    check(child(false, L"no-endpoint"), "cross-process absent places endpoint");
    detail::SharedWriter old_publisher(true, name + L".missing.places");
    PublishSharedVersion(snapshot, old_publisher, true);
    check(child(false, L"normal", name + L".missing"), "cross-process old publisher without runtime mapping");
    packages.back().enabled = false;
    snapshot.archive_worker = pulse::app::PickerArchiveWorker();
    archive_runtime_detail::Publish(snapshot, writer);
    check(child(false), "cross-process disabling clears previous runtime");
    snapshot.archive_worker = L"relative\\pulse_7zip_worker.exe";
    archive_runtime_detail::Publish(snapshot, writer);
    check(child(false), "cross-process invalid path publishes empty");
    writer.blob->payload = valid;
    DestroyWindow(endpoint);
    check(child(false), "cross-process destroyed endpoint rejected");
    std::cout << "failed=" << failed << '\n';
    return failed ? 1 : 0;
}
