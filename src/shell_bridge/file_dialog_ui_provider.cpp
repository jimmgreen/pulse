#include "file_dialog_ui_provider.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace pulse::shell_bridge {
namespace {
constexpr wchar_t ui_name[] = L"PulsePickerUiProof.dll";
HRESULT LastFailure() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    FILE_ID_INFO identity{};
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : handle(other.handle), identity(other.identity) { other.handle = INVALID_HANDLE_VALUE; }
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            handle = other.handle; identity = other.identity; other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
struct Lifetime {
    std::vector<File> directories;
    File executable, ui_file;
    HMODULE module = nullptr;
    ~Lifetime() {
        // Release module first, while all verified files and directory guards live.
        if (module) FreeLibrary(module);
    }
};
bool Equal(const std::wstring& left, const std::wstring& right) noexcept {
    return !left.empty() && !right.empty() && CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::wstring Parent(const std::wstring& value) {
    const auto end = value.find_last_of(L'\\');
    return end == value.npos ? std::wstring() : value.substr(0, end);
}
HRESULT Normalize(std::wstring_view input, std::wstring& result) {
    if (input.empty() || input.size() > 32767 || input.find(L'\0') != input.npos) return E_INVALIDARG;
    std::wstring path(input);
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (path.find_first_of(L"/:*?\"<>|", 2) != path.npos) return E_INVALIDARG;
    // Reject Win32 trailing-dot/space aliases rather than silently normalize them.
    for (size_t pos = 3; pos < path.size();) {
        size_t end = path.find(L'\\', pos); if (end == path.npos) end = path.size();
        if (end == pos || path[end - 1] == L'.' || path[end - 1] == L' ') return E_INVALIDARG;
        pos = end + 1;
    }
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!count) return LastFailure();
    if (count >= buffer.size()) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    result.assign(buffer.data(), count);
    const auto leaf = result.substr(result.find_last_of(L'\\') + 1);
    if (!Equal(leaf, L"pulse.exe")) return E_INVALIDARG;
    return S_OK;
}
HRESULT Open(const std::wstring& path, bool directory, File& output) {
    // Metadata-only access does not enforce the required Windows sharing guard.
    // Actual read/list access is requested, without reading file contents.
    // Directory writes remain shared: only ancestor rename/delete is guarded,
    // not unrelated file creation or editing in the user's directory tree.
    output.handle = CreateFileW(path.c_str(), directory ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES : GENERIC_READ,
        directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
    if (output.handle == INVALID_HANDLE_VALUE) return LastFailure();
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(output.handle, FileAttributeTagInfo, &attributes, sizeof(attributes))) return LastFailure();
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (!GetFileInformationByHandleEx(output.handle, FileIdInfo, &output.identity, sizeof(output.identity))) return LastFailure();
    return S_OK;
}
HRESULT Canonical(const File& file, std::wstring& path) {
    const DWORD required = GetFinalPathNameByHandleW(file.handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!required) return LastFailure();
    if (required > 32768) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
    const DWORD copied = GetFinalPathNameByHandleW(file.handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!copied) return LastFailure();
    if (copied >= buffer.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    path.assign(buffer.data(), copied); return S_OK;
}
bool Identity(const File& left, const File& right) noexcept {
    return left.identity.VolumeSerialNumber == right.identity.VolumeSerialNumber &&
        std::memcmp(left.identity.FileId.Identifier, right.identity.FileId.Identifier, sizeof(left.identity.FileId.Identifier)) == 0;
}
bool InModule(HMODULE module, const void* address) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    return address && VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
        info.Type == MEM_IMAGE && info.State == MEM_COMMIT && info.AllocationBase == module;
}
HRESULT Load(std::wstring_view input, std::shared_ptr<const FileDialogInprocessProvider>& output) {
    std::wstring executable;
    HRESULT hr = Normalize(input, executable); if (FAILED(hr)) return hr;
    auto lifetime = std::make_shared<Lifetime>();
    const auto directory = Parent(executable);
    // Acquire every ancestor from root downward, rejecting reparses before any
    // DLL code runs. All directory/file identities remain held for provider life.
    for (size_t end = 2;;) {
        File guard;
        const std::wstring current = end == 2 ? executable.substr(0, 3) : directory.substr(0, end);
        hr = Open(L"\\\\?\\" + current, true, guard); if (FAILED(hr)) return hr;
        lifetime->directories.push_back(std::move(guard));
        if (end >= directory.size()) break;
        const size_t next = directory.find(L'\\', end + 1);
        end = next == directory.npos ? directory.size() : next;
    }
    hr = Open(L"\\\\?\\" + executable, false, lifetime->executable); if (FAILED(hr)) return hr;
    std::wstring canonical_exe;
    hr = Canonical(lifetime->executable, canonical_exe); if (FAILED(hr)) return hr;
    const std::wstring expected_ui = Parent(canonical_exe) + L"\\" + ui_name;
    hr = Open(expected_ui, false, lifetime->ui_file); if (FAILED(hr)) return hr;
    std::wstring canonical_ui;
    hr = Canonical(lifetime->ui_file, canonical_ui); if (FAILED(hr)) return hr;
    if (!Equal(Parent(canonical_ui), Parent(canonical_exe)) || !Equal(canonical_ui, expected_ui))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    // Existing mappings may refer to an old, renamed file. A path lookup cannot
    // establish their actual file ID. Refuse rather than certify a false identity.
    if (GetModuleHandleW(ui_name) || GetModuleHandleW(canonical_ui.c_str())) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    lifetime->module = LoadLibraryExW(canonical_ui.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lifetime->module) return LastFailure();
    std::vector<wchar_t> module_path(32768);
    SetLastError(ERROR_SUCCESS);
    const DWORD count = GetModuleFileNameW(lifetime->module, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (!count) return LastFailure();
    if (count >= module_path.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    File actual;
    hr = Open(std::wstring(module_path.data(), count), false, actual); if (FAILED(hr)) return hr;
    std::wstring canonical_actual;
    hr = Canonical(actual, canonical_actual); if (FAILED(hr)) return hr;
    if (!Identity(actual, lifetime->ui_file) || !Equal(canonical_actual, canonical_ui)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    using GetApi = HRESULT (WINAPI*)(uint32_t, uint32_t, PulsePickerUiV2Api*);
    SetLastError(ERROR_SUCCESS);
    const auto get_api = reinterpret_cast<GetApi>(GetProcAddress(lifetime->module, "PulsePickerUiGetApiV2"));
    if (!get_api) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (!InModule(lifetime->module, reinterpret_cast<const void*>(get_api))) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    auto provider = std::make_shared<FileDialogInprocessProvider>();
    hr = get_api(PULSE_PICKER_UI_V2_VERSION, sizeof(provider->api), &provider->api);
    if (hr != S_OK) return FAILED(hr) ? hr : E_UNEXPECTED;
    const auto& api = provider->api;
    if (api.size != sizeof(api) || api.version != PULSE_PICKER_UI_V2_VERSION || api.reserved ||
        !InModule(lifetime->module, reinterpret_cast<const void*>(api.Initialize)) ||
        !InModule(lifetime->module, reinterpret_cast<const void*>(api.ValidateConfig)) ||
        !InModule(lifetime->module, reinterpret_cast<const void*>(api.Show)) ||
        !InModule(lifetime->module, reinterpret_cast<const void*>(api.GetWindow)) ||
        !InModule(lifetime->module, reinterpret_cast<const void*>(api.Query)) ||
        !InModule(lifetime->module, reinterpret_cast<const void*>(api.Close)) ||
        !InModule(lifetime->module, reinterpret_cast<const void*>(api.Shutdown)) ||
        !InModule(lifetime->module, reinterpret_cast<const void*>(api.UpdateControl)) ||
        ((api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_NAVIGATE) &&
         !InModule(lifetime->module, reinterpret_cast<const void*>(api.Navigate)))) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    provider->module_lifetime = lifetime;
    provider->allow_unverified_proof = true;
    output = std::move(provider); return S_OK;
}
}
HRESULT LoadFileDialogUiProvider(std::wstring_view absolute_pulse_executable,
    std::shared_ptr<const FileDialogInprocessProvider>& output, bool allow_unverified_proof) noexcept {
    output.reset();
    if (!allow_unverified_proof) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    try { return Load(absolute_pulse_executable, output); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
}
