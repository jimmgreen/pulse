#include "../shell_bridge/winrt_picker_adapter.h"
#include <winrt/Windows.Storage.Streams.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>
#include <memory>

namespace api = pulse::shell_bridge::winrt_picker;
namespace wire = pulse::ipc::picker;
using namespace winrt::Windows::Storage;
int Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    int failures = 0;
    auto check = [&](bool ok, const char* name) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; };
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Owned WinRT adapter test", WS_OVERLAPPED, 0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto folder = std::filesystem::absolute(std::filesystem::path(L"bench_data/winrt_adapter") / (L"fixture-" + std::to_wstring(GetCurrentProcessId()))).make_preferred();
    std::filesystem::create_directory(folder);
    const auto first = folder / L"one.txt", second = folder / L"two.txt", saved = folder / L"saved.txt";
    std::ofstream(first) << "one"; std::ofstream(second) << "two";
    auto service = [&](std::vector<std::filesystem::path> paths, wire::Status status = wire::Status::Accepted) {
        return api::Services{[=](const wire::Message& input, HWND hwnd, HANDLE cancel) {
            if (hwnd != owner || !cancel) winrt::throw_hresult(E_FAIL);
            wire::Response response;
            response.operation = std::get<wire::Request>(input.body).operation;
            response.status = status;
            for (const auto& path : paths) response.paths.push_back(path.u16string());
            return wire::Message{input.request_id, response};
        }};
    };
    try {
        printf("INFO eligibility=0x%08X owner=%p\n", static_cast<unsigned>(api::DesktopEligibility(owner)), owner);
        check(std::filesystem::file_size(first) == 3, "native fixture is readable");
        printf("INFO stage=baseline-storage\n");
        auto baseline_file = StorageFile::GetFileFromPathAsync(first.wstring()).get();
        check(FileIO::ReadTextAsync(baseline_file).get() == L"one", "baseline desktop StorageFile is consumable");
        printf("INFO stage=adapter-open\n");
        wire::Request open;
        auto file = api::PickFileAsync(open, owner, service({first})).get();
        check(FileIO::ReadTextAsync(file).get() == L"one", "desktop async open returns consumable StorageFile (not brokered grant)");
        open.required_capabilities |= wire::MultiSelect; open.options |= wire::AllowMultiSelect;
        auto files = api::PickFilesAsync(open, owner, service({first, second})).get();
        check(files.Size() == 2 && FileIO::ReadTextAsync(files.GetAt(1)).get() == L"two", "multi selection preserves both consumable objects");
        wire::Request pick_folder; pick_folder.operation = wire::Operation::PickFolder; pick_folder.required_capabilities = wire::Folder;
        auto storage_folder = api::PickFolderAsync(pick_folder, owner, service({folder})).get();
        check(storage_folder.Path() == folder.wstring(), "folder async returns real StorageFolder");
        wire::Request save; save.operation = wire::Operation::SaveFile; save.required_capabilities = wire::Save; save.options = wire::OverwritePrompt;
        auto target = api::PickSaveAsync(save, owner, service({saved})).get();
        FileIO::WriteTextAsync(target, L"saved-content").get();
        auto existing = api::PickSaveAsync(save, owner, service({saved})).get();
        check(FileIO::ReadTextAsync(existing).get() == L"saved-content", "save creates missing file and preserves existing contents until host writes");
        check(!api::PickFileAsync(wire::Request{}, owner, service({}, wire::Status::Cancelled)).get(), "user cancellation completes with null StorageFile");
        bool missing = false;
        try { (void)api::PickFileAsync(wire::Request{}, owner, service({folder / L"missing.txt"})).get(); }
        catch (const winrt::hresult_error&) { missing = true; }
        check(missing, "missing result never becomes stale success");
        bool malformed = false;
        auto bad = api::Services{[&](const wire::Message& input, HWND hwnd, HANDLE cancel) { auto result = service({first}).run(input, hwnd, cancel); result.request_id[0] ^= 1; return result; }};
        try { (void)api::PickFileAsync(wire::Request{}, owner, bad).get(); } catch (const winrt::hresult_error&) { malformed = true; }
        check(malformed, "mismatched response identity rejected");
        struct CancelProbe {
            HANDLE started=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            HANDLE finished=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            std::atomic<bool> signaled{false};
            ~CancelProbe(){if(started)CloseHandle(started);if(finished)CloseHandle(finished);}
        };
        const auto probe=std::make_shared<CancelProbe>();
        if(!probe->started || !probe->finished)winrt::throw_last_error();
        auto slow = api::Services{[probe,owner](const wire::Message& input, HWND hwnd, HANDLE cancel) {
            if(hwnd!=owner || !cancel)winrt::throw_hresult(E_FAIL);
            SetEvent(probe->started);
            probe->signaled = WaitForSingleObject(cancel, 5000) == WAIT_OBJECT_0;
            wire::Response response;
            response.operation=std::get<wire::Request>(input.body).operation;
            response.status=wire::Status::Cancelled;
            wire::Message reply{input.request_id,response};
            SetEvent(probe->finished);
            return reply;
        }};
        auto pending = api::PickFileAsync(wire::Request{}, owner, slow);
        const DWORD began=WaitForSingleObject(probe->started,5000);
        pending.Cancel();
        bool canceled = false;
        try { (void)pending.get(); } catch (const winrt::hresult_error&) { canceled = true; }
        // Async cancellation can complete before the transport observes its
        // signal. Keep the worker-owned state alive and wait for its explicit
        // acknowledgement rather than racing cleanup against the callback.
        const DWORD finished=WaitForSingleObject(probe->finished,5000);
        printf("INFO cancel_started=%lu cancel_finished=%lu canceled=%d signaled=%d\n",began,finished,canceled,probe->signaled.load());
        check(began==WAIT_OBJECT_0 && finished==WAIT_OBJECT_0 && canceled && probe->signaled,
              "IAsyncInfo Cancel propagates to transport and rejects results");
        DestroyWindow(owner);
        bool invalid = false;
        try { (void)api::PickFileAsync(wire::Request{}, owner, service({first})).get(); } catch (const winrt::hresult_error&) { invalid = true; }
        check(invalid, "destroyed owner rejected before transport");
    } catch (const winrt::hresult_error& e) { printf("[FAIL] unexpected hr=0x%08X\n", static_cast<unsigned>(e.code().value)); ++failures; }
    if (IsWindow(owner)) DestroyWindow(owner);
    std::filesystem::remove(first); std::filesystem::remove(second); std::filesystem::remove(saved); std::filesystem::remove(folder);
    printf("failures=%d appcontainer_grant_verified=0\n", failures);
    return failures ? 1 : 0;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    try { return Run(); }
    catch (const winrt::hresult_error& e) { printf("[FAIL] setup hr=0x%08X\n", static_cast<unsigned>(e.code().value)); }
    catch (const std::exception& e) { printf("[FAIL] setup %s\n", e.what()); }
    return 2;
}
