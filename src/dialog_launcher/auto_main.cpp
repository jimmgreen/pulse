#include "../common/dialog_auto_registration.h"
#include "../common/dialog_bundle.h"
#include "../common/runtime_log.h"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    namespace automatic = pulse::dialog_auto;
    namespace log = pulse::diagnostics::runtime;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) {
        log::Initialize(std::wstring(local) + L"\\Pulse", "dialog_auto_control");
        CoTaskMemFree(local);
    }
    DWORD error = ERROR_INVALID_PARAMETER;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--disable") {
        error = automatic::Disable();
    } else if (argc == 2 && std::wstring_view(argv[1]) == L"--status") {
        error = automatic::Inspect().error;
    } else if (argc == 3 && std::wstring_view(argv[1]) == L"--enable") {
        pulse::dialog_bundle::BundleLease bundle;
        error = pulse::dialog_bundle::VerifyBundle(argv[2], {}, bundle);
        if (!error) error = pulse::dialog_bundle::RecheckDirectory(bundle);
        if (!error) error = automatic::Enable((std::filesystem::path(bundle.Directory()) / automatic::DllName).wstring());
    } else {
        std::wcerr << L"Usage: PulseDialogAutoControl --status | --disable | --enable ABSOLUTE_BUNDLE\n";
    }
    const auto status = automatic::Inspect();
    std::wcout << L"error=" << error << L" enabled=" << status.enabled
        << L" conflict=" << status.conflict << L" status_error=" << status.error << L"\n";
    log::Event("auto_control_result", {{"error", error}, {"enabled", status.enabled}, {"conflict", status.conflict}},
        error ? log::Level::Error : log::Level::Info);
    log::Flush();
    log::Shutdown();
    return error ? 1 : 0;
}
