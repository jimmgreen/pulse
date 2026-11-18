#include "legacy_ansi_adapter.h"
#include "legacy_dialog_error.h"
#include "../ipc/picker_client.h"
#include <algorithm>
#include <cstring>
#include <objbase.h>
#include <utility>

namespace pulse::shell_bridge {
namespace {
namespace picker = ipc::picker;
namespace legacy = picker::legacy;
constexpr size_t kMaxBytes = picker::kMaxMessageBytes;

// Capture the real ACP once per invocation. UTF-8 is used only when it IS ACP.
bool Encode(UINT cp, std::wstring_view text, std::string& bytes) {
    bytes.clear();
    if (text.empty()) return true;
    BOOL replaced = FALSE;
    const DWORD flags = cp == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL* used = cp == CP_UTF8 ? nullptr : &replaced;
    const int count = WideCharToMultiByte(cp, flags, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, used);
    if (!count || replaced) return false;
    bytes.resize(static_cast<size_t>(count));
    if (!WideCharToMultiByte(cp, flags, text.data(), static_cast<int>(text.size()), bytes.data(), count, nullptr, used) || replaced) return false;
    const int units = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes.data(), count, nullptr, 0);
    if (units != static_cast<int>(text.size())) return false;
    std::wstring check(static_cast<size_t>(units), 0);
    return MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes.data(), count, check.data(), units) == units && check == text;
}
bool Decode(UINT cp, std::string_view bytes, std::u16string& text) {
    text.clear();
    if (bytes.empty()) return true;
    const int units = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (!units) return false;
    std::wstring wide(static_cast<size_t>(units), 0);
    if (!MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), wide.data(), units)) return false;
    std::string check;
    if (!Encode(cp, wide, check) || check != bytes) return false;
    text.assign(wide.begin(), wide.end());
    return true;
}
bool String(UINT cp, const char* value, std::u16string& result) {
    if (!value) return true;
    size_t size = 0;
    while (size < kMaxBytes && value[size]) ++size;
    return size < kMaxBytes && Decode(cp, {value, size}, result);
}
bool Prepare(const OPENFILENAMEA& ofn, bool save, UINT cp, legacy::Prepared& prepared) {
    if (!ofn.lpstrFile || ofn.nMaxFile < sizeof(WORD) || ofn.nMaxFile > kMaxBytes ||
        ofn.nMaxFileTitle > kMaxBytes || (ofn.lpstrFileTitle && !ofn.nMaxFileTitle) ||
        ofn.lpfnHook || ofn.lpTemplateName || ofn.lpstrCustomFilter || ofn.nMaxCustFilter ||
        ofn.pvReserved || ofn.dwReserved || ofn.FlagsEx || ofn.lCustData) return false;
    // The native ANSI multi-select contract has a 32 KiB ceiling. Larger
    // caller contracts stay native rather than advertising Unicode capacity.
    if ((ofn.Flags & OFN_ALLOWMULTISELECT) && ofn.nMaxFile > 32768) return false;
    size_t size = 0;
    while (size < ofn.nMaxFile && ofn.lpstrFile[size]) ++size;
    std::u16string initial, filters;
    if (size == ofn.nMaxFile || !Decode(cp, {ofn.lpstrFile, size + 1}, initial)) return false;
    if (ofn.lpstrFilter) {
        size = 0;
        for (; size < kMaxBytes; ++size) {
            if (!ofn.lpstrFilter[size] && (!size || !ofn.lpstrFilter[size - 1])) break;
        }
        if (size == kMaxBytes || !Decode(cp, {ofn.lpstrFilter, size + 1}, filters)) return false;
    }
    legacy::Input input;
    input.save = save;
    input.flags = ofn.Flags;
    input.filter_index = ofn.nFilterIndex;
    input.file_title_present = ofn.lpstrFileTitle != nullptr;
    input.initial_filename = initial;
    input.filter_multisz = filters;
    if (!String(cp, ofn.lpstrInitialDir, input.initial_directory) ||
        !String(cp, ofn.lpstrDefExt, input.default_extension) || !String(cp, ofn.lpstrTitle, input.title)) return false;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return false;
    picker::RequestId id{};
    static_assert(sizeof(guid) == sizeof(id));
    std::memcpy(id.data(), &guid, sizeof(guid));
    return legacy::Prepare(input, id, prepared) == legacy::Result::Ready;
}
bool EncodeUnits(UINT cp, std::span<const char16_t> text, std::string& bytes) {
    const std::wstring wide(text.begin(), text.end());
    return Encode(cp, wide, bytes);
}
}

LegacyAnsiFileDialogAdapter::LegacyAnsiFileDialogAdapter(LegacyAnsiDialogServices services) : services_(std::move(services)) {}
void LegacyAnsiFileDialogAdapter::UseNativeError() noexcept { legacy_dialog_error::UseNative(); }
DWORD LegacyAnsiFileDialogAdapter::ExtendedError() const { return legacy_dialog_error::Query(services_.native_error); }
BOOL LegacyAnsiFileDialogAdapter::Open(OPENFILENAMEA* input) const { return Invoke(input, false); }
BOOL LegacyAnsiFileDialogAdapter::Save(OPENFILENAMEA* input) const { return Invoke(input, true); }
BOOL LegacyAnsiFileDialogAdapter::Invoke(OPENFILENAMEA* input, bool save) const {
    const auto native = [&]() {
        UseNativeError();
        const BOOL result = save ? services_.save_native(input) : services_.open_native(input);
        UseNativeError();
        return result;
    };
    if (!services_.verified_abi_checks || !input || input->lStructSize != sizeof(OPENFILENAMEA)) return native();
    const UINT cp = GetACP();
    legacy::Prepared prepared;
    try {
        if (!Prepare(*input, save, cp, prepared)) return native();
        if ((services_.verified_abi_checks(prepared) & prepared.required_abi_checks) != prepared.required_abi_checks) return native();
    } catch (...) { return native(); }
    DWORD error = CDERR_DIALOGFAILURE;
    BOOL success = FALSE;
    try {
        const auto response = services_.run_bridge ? services_.run_bridge(prepared.request, input->hwndOwner) :
            picker::RunPickerClient(services_.executable, prepared.request, input->hwndOwner);
        // Model staging is independent of caller byte capacities. Only the fully
        // encoded byte result may determine shortage and native byte offsets.
        std::vector<char16_t> file(kMaxBytes), title(input->lpstrFileTitle ? kMaxBytes : 0);
        legacy::Output output;
        output.file = file;
        output.file_title = title;
        const auto completion = legacy::Complete(prepared, response, output);
        error = completion.extended_error;
        if (completion.result == legacy::Result::Accepted) {
            std::string bytes, title_bytes, prefix, extension_prefix;
            const auto units = std::span<const char16_t>(file.data(), completion.required_file_units);
            const size_t title_units = title.empty() ? 0 : static_cast<size_t>(std::find(title.begin(), title.end(), 0) - title.begin()) + 1;
            if (!EncodeUnits(cp, units, bytes) || !EncodeUnits(cp, {title.data(), title_units}, title_bytes) ||
                !EncodeUnits(cp, units.first(output.file_offset), prefix) ||
                !EncodeUnits(cp, units.first(output.extension_offset), extension_prefix) ||
                prefix.size() > MAXWORD || extension_prefix.size() > MAXWORD) {
                error = FNERR_INVALIDFILENAME;
            } else if ((prepared.flags & OFN_ALLOWMULTISELECT) && bytes.size() > 32768) {
                error = FNERR_INVALIDFILENAME;
            } else if (title_bytes.size() > input->nMaxFileTitle && input->lpstrFileTitle) {
                error = CDERR_INITIALIZATION;
            } else if (bytes.size() > input->nMaxFile) {
                error = FNERR_BUFFERTOOSMALL;
                if (bytes.size() <= MAXWORD) {
                    const WORD required = static_cast<WORD>(bytes.size());
                    std::memcpy(input->lpstrFile, &required, sizeof(required));
                }
            } else {
                std::memcpy(input->lpstrFile, bytes.data(), bytes.size());
                if (input->lpstrFileTitle) std::memcpy(input->lpstrFileTitle, title_bytes.data(), title_bytes.size());
                input->nFileOffset = static_cast<WORD>(prefix.size());
                input->nFileExtension = static_cast<WORD>(extension_prefix.size());
                input->nFilterIndex = output.filter_index;
                input->Flags = output.flags;
                success = TRUE;
            }
        } else if (completion.result != legacy::Result::Cancelled && !error) error = CDERR_DIALOGFAILURE;
    } catch (...) { error = CDERR_DIALOGFAILURE; }
    legacy_dialog_error::SetBridge(error);
    return success;
}
}
