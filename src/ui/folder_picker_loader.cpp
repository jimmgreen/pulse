#include "../common/windows_compat.h"
#include "folder_picker_loader.h"
#include "../common/picker_navigation.h"
#include "../common/picker_open_filter.h"
#include "../common/picker_user_storage.h"
#include "../common/picker_shell_namespace.h"

#include "../common/display_path.h"
#include "../common/localization.h"
#include "../fs/fs_enum.h"

#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <cwctype>
#include <cwchar>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <array>

namespace pulse::ui {
namespace {

bool IsShortcut(std::wstring_view name) {
    return name.size() >= 4 && CompareStringOrdinal(name.data() + name.size() - 4, 4, L".lnk", 4, TRUE) == CSTR_EQUAL;
}

std::wstring NativePath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') return L"\\\\?\\" + path;
    return path;
}

// Why a folder could not be listed. An empty drive root reports "no files"
// rather than an error.
DWORD ListingError(const std::wstring& path, DWORD enumeration_error) {
    const DWORD attributes = GetFileAttributesW(NativePath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error ? error : ERROR_PATH_NOT_FOUND;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
    if (enumeration_error == ERROR_FILE_NOT_FOUND || enumeration_error == ERROR_NO_MORE_FILES)
        return ERROR_SUCCESS;
    return enumeration_error ? enumeration_error : ERROR_READ_FAULT;
}

// Probe only directory admission, on the existing worker. Enumeration and
// access validation retain their original paths and cancellation generation.
DWORD ProbeFolder(const std::wstring& path, const std::function<bool()>& cancelled, uint32_t native_options) {
    if (path.empty() || picker_navigation::IsComputerPath(path)) return ERROR_SUCCESS;
    if (picker_shell::IsParsingName(path) || picker_shell::Enabled(native_options)) {
        if (!picker_shell::Enabled(native_options) || FAILED(picker_shell::ValidateOptions(native_options))) return ERROR_NOT_SUPPORTED;
        picker_shell::Apartment apartment;
        if (FAILED(apartment.Result())) return picker_shell::Error(apartment.Result());
        Microsoft::WRL::ComPtr<IShellItem> item;
        HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
        SFGAOF attributes = 0;
        if (SUCCEEDED(hr)) hr = item->GetAttributes(SFGAO_FOLDER, &attributes);
        if (cancelled()) return ERROR_CANCELLED;
        return FAILED(hr) ? picker_shell::Error(hr) : (attributes & SFGAO_FOLDER) ? ERROR_SUCCESS : ERROR_DIRECTORY;
    }
    picker_user_storage::Path storage;
    if (!picker_user_storage::Parse(path, storage)) return ERROR_NOT_SUPPORTED;
    const DWORD admission = picker_user_storage::CheckAncestors(storage, false,
        [](const std::wstring& root) { return GetDriveTypeW(root.c_str()); },
        [](const std::wstring& part, DWORD& attributes) {
            attributes = GetFileAttributesW(NativePath(part).c_str());
            return attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : DWORD{ERROR_SUCCESS};
        }, cancelled);
    if (admission) return admission;
    const HANDLE directory = CreateFileW(NativePath(path).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) { const DWORD error = GetLastError(); return error ? error : ERROR_PATH_NOT_FOUND; }
    BY_HANDLE_FILE_INFORMATION information{};
    const bool queried = GetFileInformationByHandle(directory, &information) != FALSE;
    DWORD error = queried ? ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? ERROR_SUCCESS : ERROR_DIRECTORY) : GetLastError();
    if (queried && (information.dwFileAttributes & picker_user_storage::kUnsupportedAttributes)) error = ERROR_NOT_SUPPORTED;
    if (!error) {
        std::array<wchar_t, 32768> final_path{};
        const DWORD length = GetFinalPathNameByHandleW(directory, final_path.data(), static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length) error = GetLastError();
        else if (length >= final_path.size()) error = ERROR_FILENAME_EXCED_RANGE;
        else if (!picker_user_storage::SamePath(storage.value, final_path.data())) error = ERROR_NOT_SUPPORTED;
    }
    CloseHandle(directory);
    if (cancelled()) return ERROR_CANCELLED;
    return !queried && !error ? ERROR_READ_FAULT : error;
}

// The main window's enumeration (fs::EnumerateDirectory) feeds both the
// picker's own rows (filtering, validation, type-ahead) and the directory
// snapshot the shared pane renderer draws, in one order.
PickerListing ReadShellListing(const std::wstring& path, PickerMode mode, const PickerOptions& options,
                               const std::function<bool()>& cancelled) {
    PickerListing listing; listing.path = path;
    if (!picker_shell::Enabled(options.native_options) || FAILED(picker_shell::ValidateOptions(options.native_options))) {
        listing.error = ERROR_NOT_SUPPORTED; return listing;
    }
    picker_shell::Apartment apartment;
    if (FAILED(apartment.Result())) { listing.error = picker_shell::Error(apartment.Result()); return listing; }
    Microsoft::WRL::ComPtr<IShellItem> folder;
    Microsoft::WRL::ComPtr<IShellFolder> shell_folder;
    Microsoft::WRL::ComPtr<IEnumIDList> enumerator;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (SUCCEEDED(hr)) hr = folder->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&shell_folder));
    SHCONTF flags = static_cast<SHCONTF>(SHCONTF_FOLDERS | SHCONTF_NONFOLDERS);
    if (options.show_hidden) flags = static_cast<SHCONTF>(flags | SHCONTF_INCLUDEHIDDEN | SHCONTF_INCLUDESUPERHIDDEN);
    if (SUCCEEDED(hr)) hr = shell_folder->EnumObjects(nullptr, flags, &enumerator);
    if (FAILED(hr)) { listing.error = picker_shell::Error(hr); return listing; }
    auto rows = std::make_shared<std::vector<fs::DirEntry>>();
    while (enumerator && !cancelled()) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        PITEMID_CHILD child = nullptr;
        hr = enumerator->Next(1, &child, nullptr);
        if (hr == S_FALSE) break;
        if (FAILED(hr)) { listing.error = picker_shell::Error(hr); break; }
        hr = SHCreateItemWithParent(nullptr, shell_folder.Get(), child, IID_PPV_ARGS(&item));
        CoTaskMemFree(child);
        if (FAILED(hr)) continue;
        PickerEntry entry;
        SFGAOF attributes = 0;
        hr = item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM | SFGAO_STREAM | SFGAO_HIDDEN | SFGAO_SYSTEM, &attributes);
        if (FAILED(hr)) continue;
        const bool folder_item = (attributes & SFGAO_FOLDER) != 0;
        if (!options.show_hidden && (attributes & (SFGAO_HIDDEN | SFGAO_SYSTEM))) continue;
        if (FAILED(picker_shell::Name(item.Get(), SIGDN_NORMALDISPLAY, entry.name))) continue;
        if (FAILED(picker_shell::Name(item.Get(), SIGDN_DESKTOPABSOLUTEPARSING, entry.path))) continue;
        entry.shell_attributes = attributes;
        entry.kind = folder_item && (mode == PickerMode::Folder || !picker_shell::IsStream(attributes, options.native_options)) ? PickerEntryKind::Folder : PickerEntryKind::File;
        if (attributes & SFGAO_FILESYSTEM) {
            std::wstring filesystem_path;
            if (SUCCEEDED(picker_shell::Name(item.Get(), SIGDN_FILESYSPATH, filesystem_path))) {
                const auto slash = filesystem_path.find_last_of(L"\\/");
                entry.name = filesystem_path.substr(slash == filesystem_path.npos ? 0 : slash + 1);
            }
        }
        const DWORD display_attributes = entry.kind == PickerEntryKind::Folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (!PickerShowsEntry(display_attributes, entry.name, mode, options)) continue;
        if (!folder_item && !picker_shell::IsSelectable(attributes, false, options.native_options)) continue;
        entry.source = rows->size();
        fs::DirEntry row; row.name = entry.name; row.full_path = entry.path;
        row.shell_id = entry.path; row.is_dir = entry.kind == PickerEntryKind::Folder;
        row.attrs = row.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        rows->push_back(std::move(row)); listing.entries.push_back(std::move(entry));
    }
    if (cancelled()) { listing.error = ERROR_CANCELLED; listing.entries.clear(); return listing; }
    SortPickerEntries(listing.entries, options);
    auto sorted = std::make_shared<std::vector<fs::DirEntry>>();
    sorted->reserve(listing.entries.size());
    for (const auto& entry : listing.entries) sorted->push_back(std::move((*rows)[entry.source]));
    listing.snapshot = std::move(sorted);
    return listing;
}

PickerListing ReadListing(const std::wstring& path, PickerMode mode, const PickerOptions& options,
                          const std::function<bool()>& cancelled) {
    if (picker_shell::IsParsingName(path) || (!path.empty() && !picker_navigation::IsComputerPath(path) && picker_shell::Enabled(options.native_options)))
        return ReadShellListing(path, mode, options, cancelled);
    if (picker_navigation::IsComputerPath(path)) {
        auto listing = ReadListing(L"", mode, options, cancelled);
        listing.path = path;
        return listing;
    }
    PickerListing listing;
    listing.path = path;
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    std::vector<fs::DirEntry> raw;
    try {
        fs::EnumerateDirectory(path, raw);
    } catch (...) {
        const DWORD error = GetLastError();
        raw.clear();
        listing.error = ListingError(path, error);
    }
    SetThreadErrorMode(old_mode, nullptr);
    if (cancelled()) return listing;

    const std::wstring prefix = path.empty() || path.back() == L'\\' ? path : path + L'\\';
    listing.entries.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const fs::DirEntry& source = raw[i];
        PickerEntry entry;
        entry.source = i;
        entry.name = source.name;
        entry.path = !source.full_path.empty() ? pulse::path::FriendlyPathText(source.full_path)
                                               : prefix + source.name;
        entry.modified = source.mtime;
        if (path.empty()) {
            // This PC: drives come from the same rows as the main window's view.
            if (source.drive_type == DRIVE_NO_ROOT_DIR || source.drive_type == DRIVE_UNKNOWN) continue;
            entry.kind = PickerEntryKind::Drive;
            entry.size = source.drive_total;
            entry.free = source.drive_free;
        } else {
            if (source.full_path.empty() &&
                !PickerShowsEntry(source.attrs, source.name, mode, options)) continue;
            entry.kind = source.is_dir ? PickerEntryKind::Folder
                : mode == PickerMode::Image && !IsShortcut(source.name) ? PickerEntryKind::Image
                : PickerEntryKind::File;
            entry.size = source.size;
        }
        listing.entries.push_back(std::move(entry));
    }
    // Drives keep the drive-letter order the enumeration produced.
    if (!path.empty()) SortPickerEntries(listing.entries, options);
    auto rows = std::make_shared<std::vector<fs::DirEntry>>();
    rows->reserve(listing.entries.size());
    for (const PickerEntry& entry : listing.entries) rows->push_back(std::move(raw[entry.source]));
    listing.snapshot = std::move(rows);
    return listing;
}

std::wstring ResolvePath(const std::wstring& current, const std::wstring& input) {
    auto path = NormalizePickerInput(input);
    if (path.empty()) return {};
    if (picker_shell::IsParsingName(path)) return path;
    if (picker_shell::IsParsingName(current)) return current + L"\\" + path;
    if (path.rfind(L"\\", 0) != 0 && !(path.size() > 1 && path[1] == L':')) {
        if (current.empty()) return {};
        path = current + L"\\" + path;
    } else if (path[0] == L'\\' && path.rfind(L"\\\\", 0) != 0 && current.size() > 1 && current[1] == L':') {
        path = current.substr(0, 2) + path;
    }
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!needed) return {};
    std::wstring absolute(needed, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), needed, absolute.data(), nullptr);
    if (!written || written >= needed) return {};
    absolute.resize(written);
    return NormalizePickerInput(absolute);
}

DWORD ResolveShortcuts(std::wstring& path, DWORD& attributes, const std::function<bool()>& cancelled) {
    return FollowPickerShortcutChain(path, [&](std::wstring& current, bool& followed) -> DWORD {
        if (!IsShortcut(current) || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_SUCCESS;
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(initialized)) return ERROR_NOT_SUPPORTED;
        DWORD error = ERROR_SUCCESS;
        {
            Microsoft::WRL::ComPtr<IShellLinkW> link;
            Microsoft::WRL::ComPtr<IPersistFile> file;
            std::array<wchar_t, 32768> raw{};
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
                FAILED(link.As(&file)) || FAILED(file->Load(NativePath(current).c_str(), STGM_READ)) ||
                FAILED(link->GetPath(raw.data(), static_cast<int>(raw.size()), nullptr, SLGP_RAWPATH)) || !raw[0]) {
                error = ERROR_INVALID_DATA;
            } else {
                // Parsing only: never invoke Resolve, target tracking, UI or Shell execution.
                current = ResolvePath(PickerParent(current), raw.data());
                if (current.empty()) error = ERROR_INVALID_NAME;
            }
        }
        CoUninitialize();
        if (error != ERROR_SUCCESS) return error;
        if (cancelled()) return ERROR_CANCELLED;
        attributes = GetFileAttributesW(NativePath(current).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
        followed = true;
        return ERROR_SUCCESS;
    }, cancelled);
}

// Worker-only policy. No content read, creation, truncation or write access.
// Checks a point-in-time ordinary filesystem object, not Windows dialog equivalence.
// Retain the existing V1 GENERIC_READ obligation; storage admission is separate.
DWORD CheckLocalOpenAccessV1(const std::wstring& input, const std::function<bool()>& cancelled, bool probe = true) {
    picker_user_storage::Path storage;
    if (!picker_user_storage::Parse(input, storage)) return ERROR_NOT_SUPPORTED;
    const std::wstring& path = storage.value;
    constexpr DWORD unsupported = picker_user_storage::kUnsupportedAttributes;
    const DWORD admission = picker_user_storage::CheckAncestors(storage, probe,
        [](const std::wstring& root) { return GetDriveTypeW(root.c_str()); },
        [](const std::wstring& part, DWORD& attributes) {
            attributes = GetFileAttributesW(NativePath(part).c_str());
            return attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : DWORD{ERROR_SUCCESS};
        }, cancelled);
    if (admission) return admission;
    if (!probe) return ERROR_SUCCESS;
    HANDLE handle = CreateFileW(NativePath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD error = ERROR_SUCCESS;
    FILE_BASIC_INFO info{};
    if (GetFileType(handle) != FILE_TYPE_DISK) error = ERROR_NOT_SUPPORTED;
    else if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info))) error = GetLastError();
    else if (info.FileAttributes & unsupported) error = ERROR_NOT_SUPPORTED;
    else if (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) error = ERROR_DIRECTORY;
    std::array<wchar_t, 32768> final_path{};
    if (!error) {
        const DWORD length = GetFinalPathNameByHandleW(handle, final_path.data(), static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length) error = GetLastError();
        else if (length >= final_path.size()) error = ERROR_FILENAME_EXCED_RANGE;
        else {
            if (!picker_user_storage::SamePath(path, final_path.data()))
                error = ERROR_NOT_SUPPORTED;
        }
    }
    CloseHandle(handle);
    return cancelled() ? ERROR_CANCELLED : error;
}

bool ValidOpenExtension(std::wstring_view extension) {
    if (extension.empty()) return true; // Explicit empty setter is distinct from absent.
    if (extension.size() > 255 || extension.back() == L'.' || extension.back() == L' ') return false;
    if (extension.find_first_of(L"<>:\"/\\") != extension.npos) return false;
    for (size_t i = 0; i < extension.size(); ++i) {
        const auto c = static_cast<unsigned>(extension[i]);
        if (c < 32) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == extension.size() || extension[i] < 0xdc00 || extension[i] > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}
DWORD OpenExtension(const PickerOptions& options, std::wstring& extension) {
    if (!options.use_default_extension) return ERROR_SUCCESS;
    if (!ValidOpenExtension(options.default_extension)) return ERROR_INVALID_NAME;
    extension = options.default_extension;
    if (options.filters.empty()) return ERROR_SUCCESS;
    if (options.filter_index >= options.filters.size()) return ERROR_INVALID_PARAMETER;
    const auto& pattern = options.filters[options.filter_index].pattern;
    if (!picker_filter::OpenPattern(std::wstring_view(pattern))) return ERROR_NOT_SUPPORTED;
    const auto selected = picker_filter::FirstLiteralExtension(std::wstring_view(pattern));
    // Native Open uses the selected first literal mask from initial display,
    // not just after a type change. A wildcard first mask restores the caller's
    // original default; later alternatives never supply a guessed extension.
    if (!selected.empty()) extension.assign(selected);
    return ERROR_SUCCESS;
}

PickerValidation ValidatePaths(const std::wstring& current, const std::vector<std::wstring>& names,
                               PickerMode mode, const PickerOptions& options, const std::function<bool()>& cancelled) {
    PickerValidation result;
    if (FAILED(picker_shell::ValidateOptions(options.native_options))) { result.error = ERROR_INVALID_PARAMETER; return result; }
    if (names.empty()) { result.error = ERROR_INVALID_NAME; return result; }
    const bool create_prompt = (options.native_options & FOS_CREATEPROMPT) != 0;
    if ((options.allow_missing_open && (mode != PickerMode::File ||
            (!create_prompt && (!options.skip_access_validation || options.validate_local_open_access_v1)))) ||
        (options.require_existing_open_parent && !options.allow_missing_open)) {
        result.error = ERROR_NOT_SUPPORTED; return result;
    }
    std::wstring extension;
    if (mode == PickerMode::File) {
        result.error = OpenExtension(options, extension);
        if (result.error) return result;
    }
    // Same bounded selection limit as picker protocol kMaxPaths and the v2 array ABI.
    constexpr size_t kMaxLocalOpenSelections = 1024;
    if (options.validate_local_open_access_v1 &&
        (mode != PickerMode::File || names.size() > kMaxLocalOpenSelections || options.skip_access_validation)) {
        result.error = ERROR_NOT_SUPPORTED; return result;
    }
    // LocalOpen applies the same access/shortcut checks to every selected file.
    // No prefix escapes on an error: the batch is cleared below.
    std::wstring sharing_path;
    for (const auto& name : names) {
        if (cancelled()) { result.error = ERROR_CANCELLED; break; }
        auto path = ResolvePath(current, name);
        result.failed_path = path.empty() ? name : path;
        if (path.empty()) { result.error = ERROR_INVALID_NAME; break; }
        if (picker_shell::IsParsingName(path) || picker_shell::Enabled(options.native_options)) {
            if (!picker_shell::Enabled(options.native_options)) { result.error = ERROR_NOT_SUPPORTED; break; }
            picker_shell::Apartment apartment;
            Microsoft::WRL::ComPtr<IShellItem> item;
            HRESULT hr = apartment.Result();
            if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
            SFGAOF attributes = 0;
            if (SUCCEEDED(hr)) hr = item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM | SFGAO_STREAM, &attributes);
            DWORD filesystem_attributes = INVALID_FILE_ATTRIBUTES;
            if (SUCCEEDED(hr) && (attributes & SFGAO_FILESYSTEM)) {
                std::wstring filesystem_path;
                if (SUCCEEDED(picker_shell::Name(item.Get(), SIGDN_FILESYSPATH, filesystem_path)))
                    filesystem_attributes = GetFileAttributesW(NativePath(filesystem_path).c_str());
            }
            const bool provider_stream = SUCCEEDED(hr) && picker_shell::IsStream(attributes, options.native_options) &&
                (!(attributes & SFGAO_FILESYSTEM) || (filesystem_attributes != INVALID_FILE_ATTRIBUTES &&
                    (filesystem_attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))));
            if (SUCCEEDED(hr) && (attributes & SFGAO_FILESYSTEM) && !provider_stream) {
                hr = picker_shell::Name(item.Get(), SIGDN_FILESYSPATH, path);
                if (FAILED(hr)) { result.error = picker_shell::Error(hr); break; }
                // Real disk items retain the existing shortcut, extension,
                // access and sharing checks even in a Shell namespace listing.
            } else if (SUCCEEDED(hr)) {
                const bool folder_item = (attributes & SFGAO_FOLDER) != 0;
                if (folder_item && mode != PickerMode::Folder && !picker_shell::IsStream(attributes, options.native_options)) {
                    if (names.size() != 1) { result.error = ERROR_DIRECTORY; break; }
                    result.navigate = path;
                } else if (!picker_shell::IsSelectable(attributes, mode == PickerMode::Folder, options.native_options)) {
                    result.error = ERROR_NOT_SUPPORTED; break;
                } else {
                    std::wstring display;
                    hr = picker_shell::Name(item.Get(), SIGDN_NORMALDISPLAY, display);
                    if (FAILED(hr)) { result.error = picker_shell::Error(hr); break; }
                    if (mode != PickerMode::Folder && !(mode == PickerMode::File && options.advisory_file_filters) &&
                        !PickerMatchesFilter(display, mode, options)) { result.error = ERROR_UNSUPPORTED_TYPE; break; }
                    std::wstring canonical;
                    hr = picker_shell::Name(item.Get(), SIGDN_DESKTOPABSOLUTEPARSING, canonical);
                    if (FAILED(hr)) { result.error = picker_shell::Error(hr); break; }
                    if (provider_stream && mode != PickerMode::Folder && !options.skip_access_validation) {
                        hr = picker_shell::ValidateStream(item.Get());
                        if (cancelled()) { result.error = ERROR_CANCELLED; break; }
                        if (FAILED(hr)) {
                            result.error = picker_shell::Error(hr);
                            if (result.error == ERROR_SHARING_VIOLATION && (options.native_options & FOS_SHAREAWARE)) {
                                if (sharing_path.empty()) sharing_path = canonical;
                                result.error = ERROR_SUCCESS;
                            } else break;
                        }
                    }
                    result.paths.push_back(std::move(canonical));
                }
                continue;
            }
            if (picker_shell::IsParsingName(path)) { result.error = picker_shell::Error(hr); break; }
            // Existing filesystem rules retain missing-name and default-extension handling.
        }
        if (options.allow_missing_open) {
            picker_user_storage::Path storage;
            if (!picker_user_storage::Parse(path, storage)) { result.error = ERROR_INVALID_NAME; break; }
            path = storage.value;
        }
        if (options.validate_local_open_access_v1) result.error = CheckLocalOpenAccessV1(path, cancelled, false);
        DWORD attributes = INVALID_FILE_ATTRIBUTES;
        if (!result.error) {
            attributes = GetFileAttributesW(NativePath(path).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) result.error = GetLastError();
        }
        // Native Open resolves an existing exact typed name first. Only absence
        // permits trying a suffix; access/sharing/policy errors never choose a
        // different file. Selected rows carry typed_names=false and stay exact.
        const auto leaf = path.substr(path.find_last_of(L"\\/") + 1);
        if (result.error == ERROR_FILE_NOT_FOUND && mode == PickerMode::File &&
            options.use_default_extension && options.typed_names && !extension.empty() &&
            leaf.find(L'.') == leaf.npos) {
            // Wildcard/pipe defaults may accompany exact selections, but are
            // never expanded or truncated during missing-name lookup.
            if (extension.find_first_of(L"*?|") != extension.npos) {
                result.error = ERROR_INVALID_NAME;
                break;
            }
            path += L'.' + extension;
            result.failed_path = path;
            result.error = options.validate_local_open_access_v1 ? CheckLocalOpenAccessV1(path, cancelled, false) : ERROR_SUCCESS;
            if (!result.error) {
                attributes = GetFileAttributesW(NativePath(path).c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) result.error = GetLastError();
            }
        }
        bool missing = false;
        if (options.allow_missing_open && (options.skip_access_validation || create_prompt) &&
            (result.error == ERROR_FILE_NOT_FOUND || result.error == ERROR_PATH_NOT_FOUND)) {
            if (cancelled()) { result.error = ERROR_CANCELLED; break; }
            picker_user_storage::Path storage;
            if (!picker_user_storage::Parse(path, storage)) { result.error = ERROR_INVALID_NAME; break; }
            path = storage.value;
            if (options.require_existing_open_parent) {
                const auto slash = path.find_last_of(L'\\');
                auto parent = path.substr(0, slash);
                if (parent.size() < storage.root_length) parent = storage.root;
                const DWORD parent_attributes = GetFileAttributesW(NativePath(parent).c_str());
                if (parent_attributes == INVALID_FILE_ATTRIBUTES) { result.error = GetLastError(); break; }
                if (!(parent_attributes & FILE_ATTRIBUTE_DIRECTORY)) { result.error = ERROR_DIRECTORY; break; }
            }
            missing = true;
            result.missing_paths.push_back(path);
            result.error = ERROR_SUCCESS;
            attributes = FILE_ATTRIBUTE_NORMAL;
        }
        if (result.error) break;
        if (attributes == INVALID_FILE_ATTRIBUTES) { result.error = GetLastError(); break; }
        result.error = (missing || options.no_dereference_links) ? ERROR_SUCCESS : ResolveShortcuts(path, attributes, cancelled);
        if (result.error != ERROR_SUCCESS) break;
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (mode != PickerMode::Folder && names.size() != 1) { result.error = ERROR_DIRECTORY; break; }
            if (mode == PickerMode::Folder) result.paths.push_back(path);
            else result.navigate = path;
        } else if (!(mode == PickerMode::File && options.advisory_file_filters) && !PickerMatchesFilter(path.substr(path.find_last_of(L'\\') + 1), mode, options)) {
            result.error = ERROR_UNSUPPORTED_TYPE;
            break;
        } else {
            if (options.validate_local_open_access_v1 && !missing) {
                result.error = CheckLocalOpenAccessV1(path, cancelled);
                if (result.error == ERROR_SHARING_VIOLATION && (options.native_options & FOS_SHAREAWARE)) {
                    if (sharing_path.empty()) sharing_path = path;
                    result.error = ERROR_SUCCESS;
                } else if (result.error) break;
            }
            result.paths.push_back(path);
        }
    }
    if (result.error != ERROR_SUCCESS) { result.paths.clear(); result.missing_paths.clear(); result.navigate.clear(); }
    else if (!sharing_path.empty()) { result.error = ERROR_SHARING_VIOLATION; result.failed_path = std::move(sharing_path); }
    else result.failed_path.clear();
    return result;
}

PickerValidation MakeFolder(const std::wstring& current, const std::wstring& name) {
    PickerValidation result;
    result.failed_path = name;
    const bool invalid = current.empty() || !IsPickerFolderName(name);
    if (invalid) { result.error = ERROR_INVALID_NAME; return result; }
    const auto path = ResolvePath(current, name);
    if (path.empty()) { result.error = ERROR_INVALID_NAME; return result; }
    if (!CreateDirectoryW(NativePath(path).c_str(), nullptr)) result.error = GetLastError();
    else { result.navigate = path; result.failed_path.clear(); }
    return result;
}
} // namespace

struct PickerLoader::Target {
    std::mutex lock;
    std::condition_variable changed;
    HWND hwnd = nullptr;
    UINT message = 0;
    UINT validation_message = 0;
    UINT folder_commit_message = 0;
    std::atomic<uint64_t> serial{0};
    bool closed = false;
    std::function<void(uint64_t)> pending;
};

PickerListing ReadPickerListing(const std::wstring& path, PickerMode mode, const PickerOptions& options) {
    return ReadListing(path, mode, options, [] { return false; });
}

PickerLoader::PickerLoader(HWND hwnd, UINT message, UINT validation_message, UINT folder_commit_message) : target_(std::make_shared<Target>()) {
    target_->hwnd = hwnd;
    target_->message = message;
    target_->validation_message = validation_message;
    target_->folder_commit_message = folder_commit_message;
    for (int worker = 0; worker < 2; ++worker) std::thread([target = target_] {
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        std::unique_lock lock(target->lock);
        for (;;) {
            target->changed.wait(lock, [&] { return target->closed || !!target->pending; });
            if (target->closed) return;
            auto request = std::move(target->pending);
            target->pending = {};
            const uint64_t serial = target->serial.load();
            lock.unlock();
            request(serial);
            lock.lock();
        }
    }).detach();
}

PickerLoader::~PickerLoader() {
    std::lock_guard guard(target_->lock);
    target_->closed = true;
    ++target_->serial;
    target_->pending = {};
    // Remove already-posted owned payloads before the window can be destroyed.
    MSG message{};
    if (target_->hwnd) {
        while (PeekMessageW(&message, target_->hwnd, target_->message, target_->message, PM_REMOVE)) Take(message.lParam);
        if (target_->folder_commit_message)
            while (PeekMessageW(&message, target_->hwnd, target_->folder_commit_message, target_->folder_commit_message, PM_REMOVE)) TakeFolderCommit(message.lParam);
        if (target_->validation_message)
            while (PeekMessageW(&message, target_->hwnd, target_->validation_message, target_->validation_message, PM_REMOVE)) TakeValidation(message.lParam);
    }
    target_->hwnd = nullptr;
    target_->changed.notify_all();
}

void PickerLoader::Load(uint64_t generation, std::wstring path, PickerMode mode, PickerOptions options) {
    std::lock_guard guard(target_->lock);
    ++target_->serial;
    target_->pending = [target = target_.get(), generation, path = std::move(path), mode, options = std::move(options)](uint64_t serial) {
        auto cancelled = [&] { return target->serial.load() != serial; };
        DWORD admission_error = ERROR_SUCCESS;
        if (target->folder_commit_message) {
            admission_error = ProbeFolder(path, cancelled, options.native_options);
            if (cancelled()) return;
            if (admission_error == ERROR_SUCCESS) {
                auto commit = std::make_unique<PickerFolderCommit>();
                commit->generation = generation;
                commit->path = path;
                std::lock_guard guard(target->lock);
                if (target->closed || cancelled() || !PostMessageW(target->hwnd, target->folder_commit_message, 0,
                    reinterpret_cast<LPARAM>(commit.get()))) return;
                commit.release();
            }
        }
        auto result = std::make_unique<PickerListing>();
        if (admission_error == ERROR_SUCCESS) *result = ReadListing(path, mode, options, cancelled);
        else { result->path = path; result->error = admission_error; }
        result->generation = generation;
        std::lock_guard guard(target->lock);
        if (!target->closed && !cancelled() && PostMessageW(target->hwnd, target->message, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    };
    target_->changed.notify_one();
}

void PickerLoader::Validate(uint64_t generation, std::wstring current, std::vector<std::wstring> names,
                            PickerMode mode, PickerOptions options) {
    std::lock_guard guard(target_->lock);
    ++target_->serial;
    target_->pending = [target = target_.get(), generation, current = std::move(current), names = std::move(names), mode, options = std::move(options)](uint64_t serial) {
        auto cancelled = [&] { return target->serial.load() != serial; };
        auto result = std::make_unique<PickerValidation>(ValidatePaths(current, names, mode, options, cancelled));
        result->generation = generation;
        std::lock_guard guard(target->lock);
        if (!target->closed && !cancelled() && target->validation_message && PostMessageW(target->hwnd, target->validation_message, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    };
    target_->changed.notify_one();
}

void PickerLoader::CreateFolder(uint64_t generation, std::wstring current, std::wstring name) {
    std::lock_guard guard(target_->lock);
    ++target_->serial;
    target_->pending = [target = target_.get(), generation, current = std::move(current), name = std::move(name)](uint64_t serial) {
        if (target->serial.load() != serial) return;
        auto result = std::make_unique<PickerValidation>(MakeFolder(current, name));
        result->generation = generation;
        std::lock_guard guard(target->lock);
        if (!target->closed && target->serial.load() == serial && target->validation_message && PostMessageW(target->hwnd, target->validation_message, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    };
    target_->changed.notify_one();
}

namespace {
void ApplySaveSnapshot(PickerValidation& result, SaveRevalidationResult save) {
    result.error = save.error;
    if (save.state != SaveRevalidationState::Failed && save.snapshot && !save.error) {
        result.paths.push_back(save.snapshot->Path());
        result.overwrite_required = save.state == SaveRevalidationState::ConfirmOverwrite;
        result.save_snapshot = std::move(save.snapshot);
    } else if (!result.error) result.error = ERROR_INVALID_DATA;
}
}

void PickerLoader::ValidateSave(uint64_t generation, SaveTargetRequest request) {
    std::lock_guard guard(target_->lock);
    ++target_->serial;
    target_->pending = [target = target_.get(), generation, request = std::move(request)](uint64_t serial) {
        auto cancelled = [&] { return target->serial.load() != serial; };
        if (cancelled()) return;
        auto result = std::make_unique<PickerValidation>();
        result->generation = generation;
        const auto resolved = ResolveSaveTarget(request);
        result->failed_path = resolved.path.empty() ? request.filename : resolved.path;
        auto save = CaptureSaveConfirmation(request, cancelled);
        // Initially typing a folder retains the existing navigation behavior.
        // A folder replacing a confirmed file is never treated as navigation.
        if (save.error == ERROR_DIRECTORY && !cancelled()) {
            const auto initial = ValidateSaveTarget(request);
            if (initial.state == SaveTargetState::TargetDirectory) result->navigate = initial.path;
            else ApplySaveSnapshot(*result, std::move(save));
        } else ApplySaveSnapshot(*result, std::move(save));
        std::lock_guard guard(target->lock);
        if (!target->closed && !cancelled() && target->validation_message &&
            PostMessageW(target->hwnd, target->validation_message, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    };
    target_->changed.notify_one();
}

void PickerLoader::RevalidateSave(uint64_t generation, SaveConfirmationSnapshot snapshot) {
    std::lock_guard guard(target_->lock);
    ++target_->serial;
    target_->pending = [target = target_.get(), generation, snapshot = std::move(snapshot)](uint64_t serial) {
        auto cancelled = [&] { return target->serial.load() != serial; };
        if (cancelled()) return;
        auto result = std::make_unique<PickerValidation>();
        result->generation = generation;
        result->failed_path = snapshot.Path();
        ApplySaveSnapshot(*result, RevalidateSaveConfirmation(snapshot, cancelled));
        std::lock_guard guard(target->lock);
        if (!target->closed && !cancelled() && target->validation_message &&
            PostMessageW(target->hwnd, target->validation_message, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    };
    target_->changed.notify_one();
}

void PickerLoader::CheckSaveAccess(uint64_t generation, SaveConfirmationSnapshot snapshot,
                                   bool after_file_ok, uint64_t ui_generation, bool require_unencrypted, bool skip_access_validation, uint32_t native_options) {
    std::lock_guard guard(target_->lock);
    ++target_->serial;
    target_->pending = [target = target_.get(), generation, snapshot = std::move(snapshot), after_file_ok, ui_generation, require_unencrypted, skip_access_validation, native_options](uint64_t serial) {
        auto cancelled = [&] { return target->serial.load() != serial; };
        if (cancelled()) return;
        auto result = std::make_unique<PickerValidation>();
        result->generation = generation; result->failed_path = snapshot.Path();
        result->save_ui_generation = ui_generation;
        auto fresh = RevalidateSaveConfirmation(snapshot, cancelled);
        if (fresh.state != SaveRevalidationState::Ready || !fresh.snapshot) {
            ApplySaveSnapshot(*result, std::move(fresh));
        } else {
            if (skip_access_validation || fresh.snapshot->NoTestFileCreate()) {
                // Explicit caller-validation or no-create policy; retain its snapshot metadata/overwrite checks.
                result->paths.push_back(fresh.snapshot->Path());
                result->save_snapshot = std::move(fresh.snapshot);
                result->save_access_checked = true;
                result->save_file_ok_checked = after_file_ok;
            } else {
            auto access = ValidateLocalSaveAccessV1(*fresh.snapshot, cancelled, {}, require_unencrypted);
            result->error = access.error;
            if (!access.error && access.state == SaveAccessState::Ready) {
                result->paths.push_back(fresh.snapshot->Path());
                result->save_snapshot = std::move(fresh.snapshot);
                result->save_access_checked = true;
                result->save_file_ok_checked = after_file_ok;
            } else if (access.error == ERROR_SHARING_VIOLATION && (native_options & FOS_SHAREAWARE)) {
                result->paths.push_back(fresh.snapshot->Path());
                result->save_snapshot = std::move(fresh.snapshot);
                result->save_access_checked = true;
                result->save_file_ok_checked = after_file_ok;
            } else if (!access.error && access.state == SaveAccessState::NeedsConfirmation && access.confirmation) {
                result->paths.push_back(access.confirmation->Path());
                result->save_snapshot = access.confirmation;
                result->overwrite_required = true;
            } else if (!result->error) result->error = ERROR_INVALID_DATA;
            // Keep cleanup evidence even when cancellation makes the reply stale.
            // Do not retry or remove a pathname after an uncertain close/delete.
            if (access.cleanup == SaveAccessCleanup::Unconfirmed || access.cleanup_error) {
                wchar_t diagnostic[192]{};
                swprintf_s(diagnostic, L"Pulse LocalSave cleanup failure stage=%u error=%lu cleanup=%lu primary=%lu\n",
                    static_cast<unsigned>(access.stage), access.error, access.cleanup_error, access.primary_error);
                OutputDebugStringW(diagnostic);
            }
            result->save_access_diagnostic = std::move(access);
            }
        }
        std::lock_guard guard(target->lock);
        if (!target->closed && !cancelled() && target->validation_message &&
            PostMessageW(target->hwnd, target->validation_message, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    };
    target_->changed.notify_one();
}

std::unique_ptr<PickerListing> PickerLoader::Take(LPARAM lparam) {
    return std::unique_ptr<PickerListing>(reinterpret_cast<PickerListing*>(lparam));
}
std::unique_ptr<PickerFolderCommit> PickerLoader::TakeFolderCommit(LPARAM lparam) {
    return std::unique_ptr<PickerFolderCommit>(reinterpret_cast<PickerFolderCommit*>(lparam));
}
std::unique_ptr<PickerValidation> PickerLoader::TakeValidation(LPARAM lparam) {
    return std::unique_ptr<PickerValidation>(reinterpret_cast<PickerValidation*>(lparam));
}

} // namespace pulse::ui
