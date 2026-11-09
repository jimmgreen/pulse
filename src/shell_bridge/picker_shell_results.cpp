#include "picker_shell_results.h"

#include "../common/path_utils.h"
#include "../common/picker_shell_namespace.h"
#include "../ui/file_save_model.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <filesystem>
#include <memory>
#include <new>

namespace pulse::shell_bridge {
namespace {
using Microsoft::WRL::ComPtr;
using ipc::picker::Operation;
struct TaskFree {
    using pointer = PIDLIST_ABSOLUTE; // PIDLs are explicitly __unaligned in the Windows SDK.
    void operator()(pointer value) const noexcept { ILFree(value); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, TaskFree>;

class FileBindData final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileSystemBindData> {
public:
    explicit FileBindData(const WIN32_FIND_DATAW& data) : data_(data) {}
    HRESULT STDMETHODCALLTYPE SetFindData(const WIN32_FIND_DATAW* data) override {
        if (!data) return E_POINTER;
        data_ = *data;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFindData(WIN32_FIND_DATAW* data) override {
        if (!data) return E_POINTER;
        *data = data_;
        return S_OK;
    }
private:
    WIN32_FIND_DATAW data_{};
};

std::wstring Native(const std::wstring& path) {
    return path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}

bool AbsolutePath(const std::wstring& path) {
    if (path.empty() || path.size() >= ipc::picker::kMaxStringUnits || path.find(L'\0') != path.npos ||
        path.starts_with(L"\\\\.\\") || path.find_first_of(L"<>\"|?*") != path.npos) return false;
    const bool drive = path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\';
    const auto slash = path.find(L'\\', 2);
    const bool unc = path.starts_with(L"\\\\") && slash != path.npos && slash > 2 &&
        slash + 1 < path.size() && path[slash + 1] != L'\\';
    return (drive || unc) && path.find(L':', drive ? 2 : 0) == path.npos;
}

HRESULT NewSaveItem(const std::wstring& path, IShellItem** output) {
    const auto parent_path = std::filesystem::path(path).parent_path().wstring();
    auto leaf = std::filesystem::path(path).filename().wstring();
    if (leaf.empty() || leaf.size() >= MAX_PATH) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    ComPtr<IShellItem> parent;
    HRESULT hr = SHCreateItemFromParsingName(parent_path.c_str(), nullptr, IID_PPV_ARGS(&parent));
    if (FAILED(hr)) return hr;
    ComPtr<IShellFolder> folder;
    hr = parent->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder));
    if (FAILED(hr)) return hr;
    WIN32_FIND_DATAW data{};
    data.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    wcscpy_s(data.cFileName, leaf.c_str());
    auto bind_data = Microsoft::WRL::Make<FileBindData>(data);
    if (!bind_data) return E_OUTOFMEMORY;
    ComPtr<IBindCtx> context;
    hr = CreateBindCtx(0, &context);
    if (FAILED(hr)) return hr;
    // Official filesystem parser parameter supplies metadata instead of probing
    // the nonexistent leaf. It creates a PIDL, not a file; no STGM_CREATE is used.
    // https://learn.microsoft.com/en-us/windows/win32/shell/str-constants#str_file_sys_bind_data
    hr = context->RegisterObjectParam(const_cast<wchar_t*>(STR_FILE_SYS_BIND_DATA), bind_data.Get());
    if (FAILED(hr)) return hr;
    PIDLIST_RELATIVE child_raw = nullptr;
    ULONG eaten = 0;
    SFGAOF attributes = 0;
    hr = folder->ParseDisplayName(nullptr, context.Get(), leaf.data(), &eaten, &child_raw, &attributes);
    Pidl child(child_raw);
    if (FAILED(hr)) return hr;
    if (!child) return E_UNEXPECTED;
    PIDLIST_ABSOLUTE parent_raw = nullptr;
    hr = SHGetIDListFromObject(parent.Get(), &parent_raw);
    Pidl parent_id(parent_raw);
    if (FAILED(hr)) return hr;
    return SHCreateItemWithParent(parent_id.get(), folder.Get(), child.get(), IID_PPV_ARGS(output));
}

HRESULT MissingFilesystemItem(const std::wstring& path, IShellItem** output) {
    ui::SaveTargetRequest request;
    request.directory = std::filesystem::path(path).parent_path().wstring();
    request.filename = std::filesystem::path(path).filename().wstring();
    const auto resolved = ui::ResolveSaveTarget(request);
    if (resolved.state != ui::SaveTargetState::Resolved || resolved.path != path || path.starts_with(L"\\\\")) return E_INVALIDARG;
    WIN32_FIND_DATAW data{}; data.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (request.filename.size() >= MAX_PATH) return E_INVALIDARG;
    wcscpy_s(data.cFileName, request.filename.c_str());
    auto bind_data = Microsoft::WRL::Make<FileBindData>(data);
    if (!bind_data) return E_OUTOFMEMORY;
    ComPtr<IBindCtx> context;
    HRESULT hr = CreateBindCtx(0, &context);
    if (FAILED(hr)) return hr;
    hr = context->RegisterObjectParam(const_cast<wchar_t*>(STR_FILE_SYS_BIND_DATA), bind_data.Get());
    if (FAILED(hr)) return hr;
    return SHCreateItemFromParsingName(path.c_str(), context.Get(), IID_PPV_ARGS(output));
}

HRESULT Item(Operation operation, const std::wstring& input, IShellItem** output, bool allow_missing, uint32_t native_options) {
    if (operation != Operation::OpenFiles && operation != Operation::PickFolder && operation != Operation::SaveFile)
        return E_INVALIDARG;
    if (FAILED(picker_shell::ValidateOptions(native_options))) return E_INVALIDARG;
    if (input.empty() || input.size() >= ipc::picker::kMaxStringUnits || input.find(L'\0') != input.npos) return E_INVALIDARG;
    if (picker_shell::IsParsingName(input) || (operation != Operation::SaveFile && picker_shell::Enabled(native_options))) {
        if (!picker_shell::Enabled(native_options) || operation == Operation::SaveFile) return E_INVALIDARG;
        ComPtr<IShellItem> item;
        HRESULT hr = SHCreateItemFromParsingName(input.c_str(), nullptr, IID_PPV_ARGS(&item));
        SFGAOF attributes = 0;
        if (SUCCEEDED(hr)) hr = item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_STREAM, &attributes);
        if (SUCCEEDED(hr)) {
            if (!picker_shell::IsSelectable(attributes, operation == Operation::PickFolder, native_options))
                return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            return item.CopyTo(output);
        }
        if (picker_shell::IsParsingName(input)) return hr;
    }
    auto path = pulse::path::StripExtendedPathPrefix(input);
    if (!AbsolutePath(path)) return E_INVALIDARG;
    if (operation == Operation::SaveFile) {
        if (allow_missing) {
            const DWORD attributes = GetFileAttributesW(Native(path).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                    return MissingFilesystemItem(path, output);
                return HRESULT_FROM_WIN32(error);
            }
        }
        ui::SaveTargetRequest request;
        request.directory = std::filesystem::path(path).parent_path().wstring();
        request.filename = std::filesystem::path(path).filename().wstring();
        const auto validated = ui::ValidateSaveTarget(request);
        if (validated.state == ui::SaveTargetState::Ready) return NewSaveItem(validated.path, output);
        if (validated.state != ui::SaveTargetState::ConfirmOverwrite)
            return validated.error ? HRESULT_FROM_WIN32(validated.error) :
                HRESULT_FROM_WIN32(validated.state == ui::SaveTargetState::Unsupported ? ERROR_NOT_SUPPORTED : ERROR_INVALID_NAME);
    }
    const DWORD attributes = GetFileAttributesW(Native(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (allow_missing && operation == Operation::OpenFiles && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
            return MissingFilesystemItem(path, output);
        return HRESULT_FROM_WIN32(error);
    }
    const bool folder = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((operation == Operation::PickFolder) != folder) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    return SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(output));
}
} // namespace

HRESULT CreatePickerShellItem(Operation operation, const std::wstring& path, IShellItem** item, bool allow_missing, uint32_t native_options) noexcept {
    if (!item) return E_POINTER;
    *item = nullptr;
    try {
        ComPtr<IShellItem> result;
        const HRESULT hr = Item(operation, path, &result, allow_missing, native_options);
        if (FAILED(hr)) return hr;
        if (!result) return E_UNEXPECTED;
        return result.CopyTo(item);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_INVALIDARG; }
}

HRESULT CreatePickerShellItemArray(Operation operation, const std::vector<std::wstring>& paths,
                                  IShellItemArray** items, bool allow_missing, uint32_t native_options) noexcept {
    if (!items) return E_POINTER;
    *items = nullptr;
    if (paths.empty() || paths.size() > ipc::picker::kMaxPaths ||
        (operation == Operation::SaveFile && paths.size() != 1)) return E_INVALIDARG;
    try {
        std::vector<Pidl> owned;
        std::vector<PCIDLIST_ABSOLUTE> ids;
        owned.reserve(paths.size());
        ids.reserve(paths.size());
        for (const auto& path : paths) {
            ComPtr<IShellItem> item;
            HRESULT hr = CreatePickerShellItem(operation, path, &item, allow_missing, native_options);
            if (FAILED(hr)) return hr;
            PIDLIST_ABSOLUTE raw = nullptr;
            hr = SHGetIDListFromObject(item.Get(), &raw);
            Pidl id(raw);
            if (FAILED(hr)) return hr;
            if (!id) return E_UNEXPECTED;
            ids.push_back(id.get());
            owned.push_back(std::move(id));
        }
        ComPtr<IShellItemArray> result;
        const HRESULT hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), ids.data(), &result);
        return FAILED(hr) ? hr : result.CopyTo(items);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_INVALIDARG; }
}

} // namespace pulse::shell_bridge
