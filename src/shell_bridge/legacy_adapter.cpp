#include "legacy_adapter.h"
#include "legacy_dialog_error.h"
#include "../ipc/picker_client.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <objbase.h>

namespace pulse::shell_bridge {
namespace {
namespace picker = ipc::picker;
namespace legacy = picker::legacy;
static_assert(sizeof(wchar_t) == 2 && sizeof(char16_t) == 2);
constexpr size_t kMaxBufferUnits = picker::kMaxMessageBytes / sizeof(char16_t);

bool String(const wchar_t* value, std::u16string& output) {
    if (!value) return true;
    for (size_t i = 0; i <= picker::kMaxStringUnits; ++i) {
        if (!value[i]) return true;
        if (i == picker::kMaxStringUnits) return false;
        output.push_back(static_cast<char16_t>(value[i]));
    }
    return false;
}
bool Filter(const wchar_t* value, std::vector<char16_t>& output) {
    if (!value) return true;
    for (size_t i = 0; i < kMaxBufferUnits; ++i) {
        output.push_back(static_cast<char16_t>(value[i]));
        if (!value[i] && (i == 0 || !value[i - 1])) return true;
    }
    return false;
}
bool Prepare(const OPENFILENAMEW& ofn, bool save, legacy::Prepared& prepared) {
    if (ofn.lStructSize != sizeof(OPENFILENAMEW) || !ofn.lpstrFile || !ofn.nMaxFile ||
        ofn.nMaxFile > kMaxBufferUnits || ofn.nMaxFileTitle > kMaxBufferUnits ||
        (ofn.lpstrFileTitle && !ofn.nMaxFileTitle) || ofn.lpfnHook || ofn.lpTemplateName ||
        ofn.lpstrCustomFilter || ofn.nMaxCustFilter || ofn.pvReserved || ofn.dwReserved || ofn.FlagsEx ||
        ofn.lCustData) return false;
    std::vector<char16_t> initial;
    for (size_t i = 0; i < ofn.nMaxFile; ++i) {
        initial.push_back(static_cast<char16_t>(ofn.lpstrFile[i]));
        if (!ofn.lpstrFile[i]) break;
    }
    if (initial.empty() || initial.back()) return false;
    std::vector<char16_t> filters;
    legacy::Input input;
    input.save = save;
    input.flags = ofn.Flags;
    input.flags_ex = ofn.FlagsEx;
    input.file_title_present = ofn.lpstrFileTitle != nullptr;
    input.filter_index = ofn.nFilterIndex;
    if (!Filter(ofn.lpstrFilter, filters) || !String(ofn.lpstrInitialDir, input.initial_directory) ||
        !String(ofn.lpstrDefExt, input.default_extension) || !String(ofn.lpstrTitle, input.title)) return false;
    input.initial_filename = initial;
    input.filter_multisz = filters;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return false;
    picker::RequestId id{};
    static_assert(sizeof(guid) == sizeof(id));
    std::memcpy(id.data(), &guid, sizeof(guid));
    return legacy::Prepare(input, id, prepared) == legacy::Result::Ready;
}
}

LegacyFileDialogAdapter::LegacyFileDialogAdapter(LegacyDialogServices services) : services_(std::move(services)) {}
void LegacyFileDialogAdapter::UseNativeError() noexcept { legacy_dialog_error::UseNative(); }
DWORD LegacyFileDialogAdapter::ExtendedError() const {
    return legacy_dialog_error::Query(services_.native_error);
}
BOOL LegacyFileDialogAdapter::Open(OPENFILENAMEW* input) const { return Invoke(input, false); }
BOOL LegacyFileDialogAdapter::Save(OPENFILENAMEW* input) const { return Invoke(input, true); }
BOOL LegacyFileDialogAdapter::Invoke(OPENFILENAMEW* input, bool save) const {
    const auto native = [&]() -> BOOL {
        UseNativeError();
        const BOOL result = save ? services_.save_native(input) : services_.open_native(input);
        // A nested adapter call in a native callback must not replace the outer
        // native dialog's error source. Never eagerly call CommDlgExtendedError.
        UseNativeError();
        return result;
    };
    if (!services_.verified_abi_checks || !input || input->lStructSize != sizeof(OPENFILENAMEW)) return native();
    legacy::Prepared prepared;
    try {
        if (!Prepare(*input, save, prepared)) return native();
        const uint32_t verified = services_.verified_abi_checks(prepared);
        if ((verified & prepared.required_abi_checks) != prepared.required_abi_checks) return native();
    } catch (...) { return native(); }

    // From dispatch onward there is never a native replay, even for Unsupported.
    DWORD error = CDERR_DIALOGFAILURE;
    BOOL success = FALSE;
    try {
        const auto response = services_.run_bridge ? services_.run_bridge(prepared.request, input->hwndOwner) :
            picker::RunPickerClient(services_.executable, prepared.request, input->hwndOwner);
        std::vector<char16_t> file(input->nMaxFile), title(input->lpstrFileTitle ? input->nMaxFileTitle : 0);
        legacy::Output output;
        output.file = file;
        output.file_title = title;
        const auto completion = legacy::Complete(prepared, response, output);
        error = completion.extended_error;
        if (completion.result == legacy::Result::Accepted) {
            // Only actual result units are committed; trailing caller bytes stay unchanged.
            for (size_t i = 0; i < completion.required_file_units; ++i) input->lpstrFile[i] = static_cast<wchar_t>(file[i]);
            if (input->lpstrFileTitle) {
                size_t i = 0;
                do { input->lpstrFileTitle[i] = static_cast<wchar_t>(title[i]); } while (title[i++]);
            }
            input->nFileOffset = output.file_offset;
            input->nFileExtension = output.extension_offset;
            input->nFilterIndex = output.filter_index;
            input->Flags = output.flags;
            success = TRUE;
        } else if (completion.result == legacy::Result::BufferTooSmall) {
            input->lpstrFile[0] = static_cast<wchar_t>(file[0]);
        } else if (completion.result != legacy::Result::Cancelled && !error) error = CDERR_DIALOGFAILURE;
    } catch (...) { error = CDERR_DIALOGFAILURE; }
    legacy_dialog_error::SetBridge(error);
    return success;
}
}
