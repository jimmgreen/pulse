#include "../shell_bridge/file_dialog_wrapper.h"
#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <filesystem>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <io.h>

namespace {
using Microsoft::WRL::ComPtr;
constexpr DWORD kEncoding = 100, kOption = 101;
int failures = 0, cleanup_failures = 0;
void Check(bool ok, const char* message, bool cleanup = false) {
    std::printf("[%s] %s%s\n", ok ? "PASS" : "FAIL", cleanup ? "cleanup: " : "", message);
    std::fflush(stdout);
    if (!ok) { if (cleanup) ++cleanup_failures; else ++failures; }
}
std::wstring Path(IFileDialog* dialog) {
    ComPtr<IShellItem> item;
    PWSTR value = nullptr;
    std::wstring result;
    if (SUCCEEDED(dialog->GetResult(&item)) && item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)) && value)
        result = value;
    CoTaskMemFree(value);
    return result;
}
bool SamePath(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
class Events final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileDialogEvents, IFileDialogControlEvents> {
public:
    Events(IUnknown* expected, std::wstring target) : expected_(expected), target_(std::move(target)) {}
    bool identity = true, encoding_selected = false, option_checked = false;
    unsigned file_ok = 0;
    std::wstring accepted_path;
    template<class T> void Verify(T* sender, const char* callback) {
        ComPtr<IUnknown> identity_value;
        const bool matches = sender && SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(&identity_value))) &&
            identity_value.Get() == expected_.Get();
        identity = identity && matches;
        std::printf("[CALLBACK] %s wrapper_identity=%d\n", callback, matches);
        std::fflush(stdout);
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        Verify(dialog, "OnFileOk");
        ++file_ok;
        const auto selected = Path(dialog);
        if (!SamePath(selected, target_)) {
            std::puts("[REJECT] Only this test's source.txt fixture may be accepted.");
            std::fflush(stdout);
            return S_FALSE;
        }
        accepted_path = selected;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize* sender, DWORD control, DWORD item) override {
        Verify(sender, "OnItemSelected");
        std::printf("[CALLBACK] control=%lu item=%lu\n", control, item);
        if (control == kEncoding && item == 20) encoding_selected = true;
        std::fflush(stdout);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize* sender, DWORD control, BOOL checked) override {
        Verify(sender, "OnCheckButtonToggled");
        std::printf("[CALLBACK] control=%lu checked=%d\n", control, checked);
        if (control == kOption && checked) option_checked = true;
        std::fflush(stdout);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize* sender, DWORD) override { Verify(sender, "OnButtonClicked"); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize* sender, DWORD) override { Verify(sender, "OnControlActivating"); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (!response) return E_POINTER; *response = FDESVR_DEFAULT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override {
        if (!response) return E_POINTER; *response = FDEOR_DEFAULT; return S_OK;
    }
private:
    ComPtr<IUnknown> expected_;
    std::wstring target_;
};

int Run(const std::filesystem::path& root) {
    const auto target = root / L"source.txt";
    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 2;
    DWORD written = 0;
    const bool made = WriteFile(file, "control-fixture", 15, &written, nullptr) && written == 15;
    CloseHandle(file);
    if (!made) return 2;
    unsigned bridge_calls = 0;
    pulse::shell_bridge::FileDialogBridgeServices services;
    services.run_bridge = [&](const auto&, HWND, HANDLE) { ++bridge_calls; return pulse::ipc::picker::Message{}; };
    ComPtr<IFileDialog> dialog;
    HRESULT hr = pulse::shell_bridge::CreatePulseFileDialog(pulse::shell_bridge::DialogFactory::Open,
        services, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return 2;
    GUID guid{}; wchar_t guid_text[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, guid_text, 40)) return 2;
    // Persist before SetClientGuid so even watchdog termination leaves its exact identity.
    FILE* journal = nullptr;
    if (_wfopen_s(&journal, (root / L"client-guid.tsv").c_str(), L"wb") || !journal) return 2;
    std::fprintf(journal, "Open\t%ls\tallocated-before-SetClientGuid\n", guid_text);
    const bool persisted = std::fflush(journal) == 0 && _commit(_fileno(journal)) == 0;
    if (!persisted) { std::fclose(journal); return 2; }
    hr = dialog->SetClientGuid(guid);
    const bool guid_installed = SUCCEEDED(hr);
    std::printf("[FACT] ClientGuid=%ls SetClientGuid=0x%08lX\n", guid_text, static_cast<unsigned long>(hr));
    ComPtr<IFileDialogCustomize> controls;
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(hr)) hr = dialog.As(&controls);
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
    if (SUCCEEDED(hr)) hr = dialog->SetFileName(L"source.txt");
    if (SUCCEEDED(hr)) hr = dialog->SetTitle(L"Pulse control verification");
    if (SUCCEEDED(hr)) hr = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
        FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | FOS_NOVALIDATE | FOS_NODEREFERENCELINKS);
    if (SUCCEEDED(hr)) hr = controls->AddComboBox(kEncoding);
    if (SUCCEEDED(hr)) hr = controls->SetControlLabel(kEncoding, L"Test encoding");
    if (SUCCEEDED(hr)) hr = controls->AddControlItem(kEncoding, 10, L"UTF-8");
    if (SUCCEEDED(hr)) hr = controls->AddControlItem(kEncoding, 20, L"UTF-16");
    if (SUCCEEDED(hr)) hr = controls->SetSelectedControlItem(kEncoding, 10);
    if (SUCCEEDED(hr)) hr = controls->AddCheckButton(kOption, L"Test option", FALSE);
    ComPtr<IUnknown> identity;
    if (SUCCEEDED(hr)) hr = dialog.As(&identity);
    auto events = Microsoft::WRL::Make<Events>(identity.Get(), target.wstring());
    DWORD cookie = 0;
    if (SUCCEEDED(hr)) hr = events ? dialog->Advise(events.Get(), &cookie) : E_OUTOFMEMORY;
    Check(SUCCEEDED(hr), "visible native controls and event sink configured");
    HRESULT shown = hr;
    if (SUCCEEDED(hr)) {
        DWORD initial_item = 0; BOOL initial_checked = TRUE;
        Check(SUCCEEDED(controls->GetSelectedControlItem(kEncoding, &initial_item)) && initial_item == 10 &&
            SUCCEEDED(controls->GetCheckButtonState(kOption, &initial_checked)) && !initial_checked, "initial UTF-8 and unchecked state");
        HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!finished) { shown = E_FAIL; } else {
            std::thread watchdog([finished] {
                if (WaitForSingleObject(finished, 90000) != WAIT_OBJECT_0) {
                    std::puts("[FAIL] 90-second user interaction deadline expired; ending only this test process.");
                    std::fflush(stdout);
                    TerminateProcess(GetCurrentProcess(), 99);
                }
            });
            std::puts("[READY] Select UTF-16, check Test option, then Open source.txt; Cancel is reported separately.");
            std::fflush(stdout);
            shown = dialog->Show(nullptr);
            SetEvent(finished); watchdog.join(); CloseHandle(finished);
        }
    }
    const auto selected = Path(dialog.Get());
    const bool cancelled = shown == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    std::printf("[FACT] Show=0x%08lX outcome=%s result=%ls\n", static_cast<unsigned long>(shown),
        cancelled ? "cancelled-not-verified" : SUCCEEDED(shown) ? "accepted" : "failed", selected.c_str());
    if (cancelled) Check(selected.empty(), "cancel returns no selected path");
    else {
        DWORD item = 0; BOOL checked = FALSE;
        Check(SUCCEEDED(shown) && SamePath(selected, target.wstring()) && events &&
            events->file_ok > 0 && SamePath(events->accepted_path, selected), "OnFileOk and final result agree on exact fixture");
        Check(events && events->identity && events->encoding_selected && events->option_checked,
            "real control callbacks carry requested values and wrapper identity");
        Check(controls && SUCCEEDED(controls->GetSelectedControlItem(kEncoding, &item)) && item == 20 &&
            SUCCEEDED(controls->GetCheckButtonState(kOption, &checked)) && checked, "final UTF-16 and checked state");
    }
    Check(bridge_calls == 0, "default closed validation gate uses native Show only");
    if (cookie) Check(SUCCEEDED(dialog->Unadvise(cookie)), "event subscription removed");
    const HRESULT cleared = guid_installed ? dialog->ClearClientData() : E_PENDING;
    std::fprintf(journal, "Open\t%ls\tClearClientData\t0x%08lX\n", guid_text, static_cast<unsigned long>(cleared));
    const bool cleanup_recorded = std::fflush(journal) == 0 && _commit(_fileno(journal)) == 0;
    std::fclose(journal);
    Check(cleanup_recorded, "cleanup HRESULT persisted to journal", true);
    std::printf("[FACT] ClearClientData=0x%08lX\n", static_cast<unsigned long>(cleared));
    Check(SUCCEEDED(cleared), "test GUID history cleared", true);
    Check(DeleteFileW(target.c_str()) != FALSE, "exact source fixture removed", true);
    std::printf("[SUMMARY] functional_failures=%d cleanup_failures=%d cancelled=%d\n", failures, cleanup_failures, cancelled);
    return cancelled ? 3 : failures || cleanup_failures ? 1 : 0;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto root = std::filesystem::absolute(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        L"bench_data" / (L"PulseControlVerification-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 2;
    FILE* output = nullptr;
    if (_wfreopen_s(&output, (root / L"verification.log").c_str(), L"wb", stdout)) return 2;
    std::printf("[FIXTURE] %ls\n", root.c_str()); std::fflush(stdout);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    const int result = Run(root);
    CoUninitialize();
    std::fflush(stdout);
    return result;
}
