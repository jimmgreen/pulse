#include "../common/dialog_auto_registration.h"
#include "../common/dialog_bundle.h"
#include "../common/runtime_log.h"
#include "../common/json_value.h"
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
    const std::wstring_view command = argc > 1 ? argv[1] : L"";
    const bool json_output = command == L"--status-json" || command == L"--enable-json" || command == L"--disable-for-json" || command == L"--enable-from-json";
    if (argc == 2 && std::wstring_view(argv[1]) == L"--disable") {
        error = automatic::Disable();
    } else if (argc == 2 && (command == L"--status" || command == L"--status-json")) {
        error = automatic::Inspect().error;
    } else if (argc == 4 && command == L"--enable-from-json") {
        pulse::dialog_bundle::BundleLease old_bundle, new_bundle;
        error = pulse::dialog_bundle::VerifyBundle(argv[2], {}, old_bundle);
        if (!error) error = pulse::dialog_bundle::VerifyBundle(argv[3], {}, new_bundle);
        const WORD machine = automatic::CurrentArchitecture == automatic::Architecture::X64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
        if (!error && (old_bundle.Machine() != machine || new_bundle.Machine() != machine)) error = ERROR_BAD_EXE_FORMAT;
        if (!error) error = pulse::dialog_bundle::RecheckDirectory(old_bundle);
        if (!error) error = pulse::dialog_bundle::RecheckDirectory(new_bundle);
        if (!error) {
            const auto old_dll = (std::filesystem::path(old_bundle.Directory()) / automatic::DllName).wstring();
            const auto new_dll = (std::filesystem::path(new_bundle.Directory()) / automatic::DllName).wstring();
            error = automatic::EnableIfDisabledFor(old_dll, new_dll);
        }
    } else if (argc == 3 && (command == L"--enable" || command == L"--enable-json" || command == L"--disable-for" || command == L"--disable-for-json")) {
        pulse::dialog_bundle::BundleLease bundle;
        error = pulse::dialog_bundle::VerifyBundle(argv[2], {}, bundle);
        if (!error) error = pulse::dialog_bundle::RecheckDirectory(bundle);
        if (!error) {
            const auto dll = (std::filesystem::path(bundle.Directory()) / automatic::DllName).wstring();
            error = command == L"--disable-for" || command == L"--disable-for-json" ? automatic::DisableFor(dll) :
                command == L"--enable-json" ? automatic::EnableIfUnclaimed(dll) : automatic::Enable(dll);
        }
    } else {
        std::wcerr << L"Usage: PulseDialogAutoControl --status[-json] | --disable | --enable[-json] ABSOLUTE_BUNDLE | --disable-for[-json] ABSOLUTE_BUNDLE | --enable-from-json ABS_OLD_BUNDLE ABS_NEW_BUNDLE\n";
    }
    const auto status = automatic::Inspect();
    if (json_output) {
        auto value = pulse::json::Value::Object();
        value.set("error", static_cast<int64_t>(error));
        value.set("enabled", status.enabled);
        value.set("conflict", status.conflict);
        value.set("status_error", static_cast<int64_t>(status.error));
        value.set("dll_path", pulse::json::Utf8(status.dll_path));
        std::cout << pulse::json::Serialize(value) << '\n';
    } else std::wcout << L"error=" << error << L" enabled=" << status.enabled
        << L" conflict=" << status.conflict << L" status_error=" << status.error << L"\n";
    log::Event("auto_control_result", {{"error", error}, {"enabled", status.enabled}, {"conflict", status.conflict}},
        error ? log::Level::Error : log::Level::Info);
    log::Flush();
    log::Shutdown();
    return error ? 1 : 0;
}
