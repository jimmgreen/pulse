#include "file_save_model.h"
#include "../common/path_utils.h"
#include "../common/picker_user_storage.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <shobjidl.h>

namespace pulse::ui {
namespace {

bool ValidText(std::wstring_view text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned>(text[i]);
        if (c < 32) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}

bool Extension(std::wstring_view text) {
    return !text.empty() && ValidText(text) && text.front() != L'.' && text.back() != L'.' &&
        text.find_first_of(L"<>:\"/\\|?*; ") == std::wstring_view::npos;
}

bool Reserved(std::wstring_view name) {
    std::wstring stem(name.substr(0, name.find(L'.')));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        stem == L"CONIN$" || stem == L"CONOUT$") return true;
    if (stem.size() != 4 || (stem.substr(0, 3) != L"COM" && stem.substr(0, 3) != L"LPT")) return false;
    return (stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' ||
        stem[3] == L'\u00b2' || stem[3] == L'\u00b3';
}

bool FilterExtension(std::wstring_view pattern, std::wstring& extension) {
    if (pattern == L"*.*") return true;
    size_t start = 0;
    while (start < pattern.size()) {
        const auto end = pattern.find(L';', start);
        const auto part = pattern.substr(start, end == pattern.npos ? pattern.size() - start : end - start);
        if (!part.starts_with(L"*.") || !Extension(part.substr(2))) return false;
        if (start == 0) extension = part.substr(2);
        if (end == pattern.npos) return true;
        start = end + 1;
    }
    return false;
}

std::wstring Native(const std::wstring& path) {
    return path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}

bool Missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }

} // namespace

std::optional<size_t> InferSaveFilterFromFilename(std::wstring_view filename,
    const std::vector<PickerFilter>& filters, size_t current_filter) {
    if (current_filter >= filters.size() || !ValidText(filename)) return std::nullopt;
    const auto slash = filename.find_last_of(L"\\/");
    const auto leaf = filename.substr(slash == filename.npos ? 0 : slash + 1);
    if (leaf.empty() || leaf.front() == L'.' || leaf.back() == L'.' || std::iswspace(leaf.back()) ||
        leaf.find_first_of(L"<>:\"|?*") != leaf.npos || leaf.find(L'.') == leaf.npos) return std::nullopt;
    const auto matches = [&](std::wstring_view pattern) {
        bool matched = false;
        for (size_t start = 0; start < pattern.size();) {
            const auto end = pattern.find(L';', start);
            const auto part = pattern.substr(start, end == pattern.npos ? pattern.size() - start : end - start);
            if (!part.starts_with(L"*.") || !Extension(part.substr(2))) return false;
            const auto extension = part.substr(2);
            if (leaf.size() > extension.size() + 1 && leaf[leaf.size() - extension.size() - 1] == L'.') {
                const auto suffix = leaf.substr(leaf.size() - extension.size());
                matched |= std::equal(suffix.begin(), suffix.end(), extension.begin(),
                    [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
            }
            if (end == pattern.npos) return matched;
            start = end + 1;
            if (start == pattern.size()) return false;
        }
        return false;
    };
    if (matches(filters[current_filter].pattern)) return std::nullopt;
    std::optional<size_t> inferred;
    for (size_t i = 0; i < filters.size(); ++i) {
        if (!matches(filters[i].pattern)) continue;
        if (inferred) return std::nullopt;
        inferred = i;
    }
    return inferred;
}

SaveTargetResult ResolveSaveTarget(const SaveTargetRequest& request) {
    if (request.filename.find_first_of(L"\\/") != std::wstring::npos) {
        auto input = std::filesystem::path(request.filename);
        if (!input.is_absolute()) {
            if (input.has_root_name() || input.has_root_directory()) return {};
            input = std::filesystem::path(request.directory) / input;
        }
        input.make_preferred();
        pulse::picker_user_storage::Path parsed;
        if (!pulse::picker_user_storage::Parse(input.wstring(), parsed)) return {};
        SaveTargetRequest leaf = request;
        leaf.directory = std::filesystem::path(parsed.value).parent_path().wstring();
        leaf.filename = std::filesystem::path(parsed.value).filename().wstring();
        if (leaf.filename.empty()) return {};
        return ResolveSaveTarget(leaf);
    }
    SaveTargetResult result;
    result.filter_index = request.filter_index;
    if ((!request.filters.empty() && request.filter_index >= request.filters.size()) ||
        (request.filters.empty() && request.filter_index != 0)) {
        result.state = SaveTargetState::InvalidFilter;
        return result;
    }
    const auto& name = request.filename;
    if (name.empty() || name.size() > 255 || !ValidText(name) || Reserved(name) ||
        name.find_first_of(L"<>:\"/\\|?*") != name.npos) return result;
    if (name.back() == L'.' || name.back() == L' ') {
        result.state = SaveTargetState::Unsupported;
        return result;
    }
    std::wstring extension = request.default_extension;
    std::wstring selected_extension;
    const bool pattern_default = request.native_save_defaults && extension.starts_with(L"*.") && Extension(std::wstring_view(extension).substr(2));
    if ((!extension.empty() && !Extension(extension) && !pattern_default) || (!request.filters.empty() &&
        !FilterExtension(request.filters[request.filter_index].pattern, selected_extension))) {
        result.state = SaveTargetState::Unsupported;
        return result;
    }
    // Native-defaults callers use the current filter even before a type change.
    // The older wire policy retains its original initialization behavior.
    // All-files has no literal extension and restores the caller's default.
    if ((request.filter_changed || request.native_save_defaults) && !selected_extension.empty()) extension = selected_extension;
    if (request.native_save_defaults && name.find(L'.') == name.npos && !extension.empty() && !Extension(extension)) {
        result.state = SaveTargetState::Unsupported;
        return result;
    }
    std::wstring filename = name;
    if (filename.find(L'.') == filename.npos && !extension.empty()) filename += L'.' + extension;
    if (filename.size() > 255) return result;
    auto directory = pulse::path::StripExtendedPathPrefix(request.directory);
    std::replace(directory.begin(), directory.end(), L'/', L'\\');
    const bool drive = directory.size() >= 3 && std::iswalpha(directory[0]) &&
        directory[1] == L':' && directory[2] == L'\\';
    const auto server_end = directory.find(L'\\', 2);
    const bool unc = directory.starts_with(L"\\\\") && server_end != directory.npos && server_end > 2 &&
        server_end + 1 < directory.size() && directory[2] != L'.' && directory[2] != L'?';
    if ((!drive && !unc) || !ValidText(directory) || directory.find_first_of(L"<>\"|?*") != directory.npos ||
        directory.find(L':', drive ? 2 : 0) != directory.npos) {
        result.state = SaveTargetState::InvalidDirectory;
        return result;
    }
    const auto parent = std::filesystem::path(directory).lexically_normal();
    result.path = (parent / filename).wstring();
    if (result.path.size() >= 32760) {
        result.path.clear();
        result.state = SaveTargetState::Unsupported;
        return result;
    }
    result.state = SaveTargetState::Resolved;
    return result;
}

SaveTargetResult ValidateSaveTarget(const SaveTargetRequest& request) {
    auto result = ResolveSaveTarget(request);
    if (result.state != SaveTargetState::Resolved) return result;
    const auto parent = std::filesystem::path(result.path).parent_path().wstring();
    const DWORD parent_attributes = GetFileAttributesW(Native(parent).c_str());
    if (parent_attributes == INVALID_FILE_ATTRIBUTES) {
        result.error = GetLastError();
        result.state = Missing(result.error) ? SaveTargetState::ParentMissing : SaveTargetState::IoError;
        return result;
    }
    if (!(parent_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result.state = SaveTargetState::ParentNotDirectory;
        return result;
    }
    const DWORD attributes = GetFileAttributesW(Native(result.path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        result.error = GetLastError();
        result.state = result.error == ERROR_FILE_NOT_FOUND ? SaveTargetState::Ready : SaveTargetState::IoError;
        if (result.state == SaveTargetState::Ready) result.error = ERROR_SUCCESS;
        return result;
    }
    result.target_exists = true;
    result.read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    result.state = attributes & FILE_ATTRIBUTE_DIRECTORY ? SaveTargetState::TargetDirectory : SaveTargetState::ConfirmOverwrite;
    return result;
}


namespace {
DWORD DirectoryCreationPolicy(const SaveTargetRequest& request, uint32_t native_options) {
    if (request.require_existing_save_parent || request.no_test_file_create ||
        (native_options & (FOS_PATHMUSTEXIST | FOS_NOTESTFILECREATE))) return ERROR_PATH_NOT_FOUND;
    const std::filesystem::path input(request.filename);
    if (input.is_absolute() || input.has_root_name() || input.has_root_directory() || !input.has_parent_path())
        return ERROR_NOT_SUPPORTED;
    for (const auto& part : input) if (part == L".." || part == L".") return ERROR_INVALID_NAME;
    return ERROR_SUCCESS;
}
std::vector<std::wstring> DirectoryAncestors(const std::wstring& target) {
    const auto parent = std::filesystem::path(target).parent_path();
    std::vector<std::wstring> result;
    auto path = parent.root_path();
    result.push_back(path.wstring());
    for (const auto& part : parent.relative_path()) {
        path /= part;
        result.push_back(path.wstring());
    }
    return result;
}
}

SaveDirectoryPlan PlanSaveDirectories(const SaveTargetRequest& request, uint32_t native_options,
                                     const std::function<bool()>& cancelled) {
    SaveDirectoryPlan plan;
    plan.error = DirectoryCreationPolicy(request, native_options);
    if (plan.error) return plan;
    const auto resolved = ResolveSaveTarget(request);
    if (resolved.state != SaveTargetState::Resolved) { plan.error = ERROR_INVALID_NAME; return plan; }
    plan.target = resolved.path;
    // The picker base must already exist. Never reconstruct a removed base folder.
    const DWORD base = GetFileAttributesW(Native(pulse::path::StripExtendedPathPrefix(request.directory)).c_str());
    if (base == INVALID_FILE_ATTRIBUTES) { plan.error = GetLastError(); return plan; }
    if (!(base & FILE_ATTRIBUTE_DIRECTORY)) { plan.error = ERROR_DIRECTORY; return plan; }
    for (const auto& path : DirectoryAncestors(plan.target)) {
        if (cancelled && cancelled()) { plan.error = ERROR_CANCELLED; return plan; }
        const DWORD attr = GetFileAttributesW(Native(path).c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (!Missing(error)) { plan.error = error; return plan; }
            plan.missing.push_back(path);
        } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) { plan.error = ERROR_DIRECTORY; return plan; }
        else if (attr & FILE_ATTRIBUTE_REPARSE_POINT) { plan.error = ERROR_NOT_SUPPORTED; return plan; }
    }
    return plan;
}

SaveDirectoryCreation CreateSaveDirectories(const SaveTargetRequest& request, const SaveDirectoryPlan& approved,
                                            uint32_t native_options, const std::function<bool()>& cancelled) {
    SaveDirectoryCreation result;
    if (approved.error || approved.missing.empty()) { result.error = ERROR_INVALID_PARAMETER; return result; }
    const auto fresh = PlanSaveDirectories(request, native_options, cancelled);
    if (fresh.error) { result.error = fresh.error; result.failed_path = fresh.target; return result; }
    if (fresh.target != approved.target || fresh.missing != approved.missing) {
        result.error = ERROR_RETRY; result.failed_path = fresh.target; return result;
    }
    struct HeldDirectories {
        std::vector<HANDLE> values;
        ~HeldDirectories() { for (HANDLE h : values) CloseHandle(h); }
    } held;
    for (const auto& path : DirectoryAncestors(fresh.target)) {
        result.failed_path = path;
        if (cancelled && cancelled()) { result.error = ERROR_CANCELLED; return result; }
        if (std::find(approved.missing.begin(), approved.missing.end(), path) != approved.missing.end()) {
            if (!CreateDirectoryW(Native(path).c_str(), nullptr)) { result.error = GetLastError(); return result; }
            result.created.push_back(path);
        }
        // Retain every ancestor without DELETE sharing while descending. Reparse
        // replacements are rejected; renamed ancestors cannot redirect later creates.
        HANDLE handle = CreateFileW(Native(path).c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
        held.values.push_back(handle);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info)) { result.error = GetLastError(); return result; }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { result.error = ERROR_DIRECTORY; return result; }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { result.error = ERROR_NOT_SUPPORTED; return result; }
    }
    result.failed_path.clear();
    return result;
}

} // namespace pulse::ui
