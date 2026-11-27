#include "file_save_model.h"
#include "../common/picker_save_filter.h"
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

bool UsesSaveNameMask(const std::vector<PickerFilter>& filters) {
    return std::any_of(filters.begin(), filters.end(), [](const PickerFilter& filter) {
        const auto mask = picker_filter::ParseSaveMask(std::wstring_view(filter.pattern));
        return mask.valid && mask.name_mask;
    });
}

std::wstring NativeSaveMaskFilename(std::wstring_view filename, const std::vector<PickerFilter>& filters,
    size_t index, std::wstring_view default_extension, bool type_change) {
    std::wstring result(filename);
    if (index >= filters.size() || filename.empty() || default_extension.empty()) return result;
    const std::wstring_view pattern(filters[index].pattern);
    const auto mask = picker_filter::ParseSaveMask(pattern);
    if (!mask.valid || (type_change && (pattern == L"*" || pattern == L"*.*"))) return result;
    const auto extension = mask.extension.empty() ? default_extension : mask.extension;
    const auto slash = filename.find_last_of(L"\\/");
    const auto leaf = filename.substr(slash == filename.npos ? 0 : slash + 1);
    if (leaf.empty() || leaf.back() == L'.' || leaf.back() == L' ' || leaf.find_first_of(L"<>:\"|?*") != leaf.npos)
        return result;
    const auto suffix_matches = [&](std::wstring_view ext) {
        return !ext.empty() && leaf.size() > ext.size() && leaf[leaf.size() - ext.size() - 1] == L'.' &&
            CompareStringOrdinal(leaf.data() + leaf.size() - ext.size(), static_cast<int>(ext.size()),
                ext.data(), static_cast<int>(ext.size()), TRUE) == CSTR_EQUAL;
    };
    if (suffix_matches(extension)) return result;
    const auto dot = leaf.rfind(L'.');
    if (dot != leaf.npos) {
        if (type_change) result.resize(filename.size() - (leaf.size() - dot));
        else {
            // A registered extension is an explicit file type, even if this
            // filter does not list it. Never instantiate the associated handler.
            const std::wstring suffix(leaf.substr(dot));
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, suffix.c_str(), 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
                RegCloseKey(key); return result;
            }
            for (const auto& filter : filters) {
                std::wstring_view rest(filter.pattern);
                while (!rest.empty()) {
                    const auto end = rest.find(L';');
                    if (suffix_matches(picker_filter::ParseSaveMask(rest.substr(0, end)).extension)) return result;
                    if (end == rest.npos) break;
                    rest.remove_prefix(end + 1);
                }
            }
        }
    }
    return result.append(L".").append(extension);
}

std::optional<size_t> InferSaveFilterFromFilename(std::wstring_view filename,
    const std::vector<PickerFilter>& filters, size_t current_filter) {
    if (current_filter >= filters.size() || !ValidText(filename)) return std::nullopt;
    // An explicit all-files selection accepts the typed suffix without changing the caller's format.
    const auto& current_pattern = filters[current_filter].pattern;
    if (current_pattern == L"*.*" || current_pattern == L"*") return std::nullopt;
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
    bool ambiguous = false;
    size_t best_count = static_cast<size_t>(-1);
    for (size_t i = 0; i < filters.size(); ++i) {
        if (!matches(filters[i].pattern)) continue;
        const size_t count = filters[i].pattern.find(L';') == std::wstring::npos ? 1 : 2;
        if (count < best_count) { inferred = i; best_count = count; ambiguous = false; }
        else if (count == best_count) ambiguous = true;
    }
    if (ambiguous) return std::nullopt;
    return inferred;
}

std::wstring RewriteSaveFilenameForFilter(std::wstring_view filename,
    const std::vector<PickerFilter>& filters, size_t selected_filter) {
    const std::wstring unchanged(filename);
    if (selected_filter >= filters.size() || !ValidText(filename)) return unchanged;
    std::wstring extension;
    if (!FilterExtension(filters[selected_filter].pattern, extension) || extension.empty()) return unchanged;
    const auto slash = filename.find_last_of(L"\\/");
    const size_t begin = slash == filename.npos ? 0 : slash + 1;
    const auto leaf = filename.substr(begin);
    if (leaf.empty() || leaf.front() == L'.' || leaf.back() == L'.' || std::iswspace(leaf.back()) ||
        leaf.find_first_of(L"<>:\"|?*") != leaf.npos) return unchanged;
    const auto suffix_length = [&](std::wstring_view pattern) {
        size_t longest = 0;
        std::wstring ignored;
        if (!FilterExtension(pattern, ignored)) return longest;
        for (size_t start = 0; start < pattern.size();) {
            const auto end = pattern.find(L';', start);
            const auto part = pattern.substr(start, end == pattern.npos ? pattern.size() - start : end - start);
            if (part.starts_with(L"*.") && Extension(part.substr(2))) {
                const auto suffix = part.substr(1);
                if (leaf.size() > suffix.size() && std::equal(suffix.begin(), suffix.end(), leaf.end() - suffix.size(),
                    [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }))
                    longest = (std::max)(longest, suffix.size());
            }
            if (end == pattern.npos) break;
            start = end + 1;
        }
        return longest;
    };
    if (suffix_length(filters[selected_filter].pattern)) return unchanged;
    size_t remove = 0;
    for (const auto& filter : filters) remove = (std::max)(remove, suffix_length(filter.pattern));
    if (!remove) {
        const auto dot = leaf.find_last_of(L'.');
        if (dot != leaf.npos && dot != 0) remove = leaf.size() - dot;
    }
    return std::wstring(filename.substr(0, filename.size() - remove)) + L"." + extension;
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
    const bool named = request.native_save_defaults && UsesSaveNameMask(request.filters);
    // Native SetDefaultExtension(".ext") appends another dot when no concrete
    // filter overrides it. Preserve the literal value rather than stripping it.
    const auto literal_default = [&](std::wstring_view value) {
        return Extension(value) || (request.native_save_defaults && value.starts_with(L".") && Extension(value.substr(1)));
    };
    const bool pattern_default = request.native_save_defaults && extension.starts_with(L"*.") && Extension(std::wstring_view(extension).substr(2));
    if ((named && pattern_default) || (!extension.empty() && !literal_default(extension) && !pattern_default) || (!request.filters.empty() &&
        (named ? !picker_filter::ParseSaveMask(std::wstring_view(request.filters[request.filter_index].pattern)).valid :
            !FilterExtension(request.filters[request.filter_index].pattern, selected_extension)))) {
        result.state = SaveTargetState::Unsupported;
        return result;
    }
    // Native-defaults callers use the current filter even before a type change.
    // The older wire policy retains its original initialization behavior.
    // All-files has no literal extension and restores the caller's default.
    if ((request.filter_changed || request.native_save_defaults) && !selected_extension.empty()) extension = selected_extension;
    if (request.native_save_defaults && name.find(L'.') == name.npos && !extension.empty() && !literal_default(extension)) {
        result.state = SaveTargetState::Unsupported;
        return result;
    }
    std::wstring filename = name;
    if (named) filename = NativeSaveMaskFilename(name, request.filters, request.filter_index, extension, false);
    else if (filename.find(L'.') == filename.npos && !extension.empty()) filename += L'.' + extension;
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

bool NormalizeLocalSaveDirectory(std::wstring_view path, std::wstring& normalized) {
    picker_user_storage::Path parsed;
    if (path.size() >= 32760 || !ValidText(path) || !picker_user_storage::Parse(path, parsed) || parsed.unc)
        return false;
    // Do not change extended-path semantics when the picker removes the prefix.
    const std::wstring_view value(parsed.value);
    for (size_t start = parsed.root_length; start < value.size();) {
        const auto end = value.find(L'\\', start);
        const auto part = value.substr(start, end - start);
        if (part.empty() || part.back() == L'.' || part.back() == L' ' || Reserved(part))
            return false;
        if (end == value.npos) break;
        start = end + 1;
    }
    normalized = std::move(parsed.value);
    return true;
}

LocalSaveDraft PrepareLocalSaveDraft(const SaveTargetRequest& request) {
    LocalSaveDraft plan;
    if (!NormalizeLocalSaveDirectory(request.directory, plan.directory)) return plan;
    plan.state = LocalSaveDraftState::UnsupportedTarget;
    SaveTargetRequest draft = request;
    draft.directory = plan.directory;
    if (draft.filename.empty()) draft.filename = L"pulse-validation";
    const auto target = ResolveSaveTarget(draft);
    if (target.state == SaveTargetState::Resolved) {
        // An absolute filename can redirect the target away from the initial
        // folder. Its parent still has to be handled by LocalSaveV1.
        const auto target_parent = std::filesystem::path(target.path).parent_path().wstring();
        std::wstring parent;
        picker_user_storage::Path parsed;
        // The existing no-test route does metadata/identity checks only and
        // already supports UNC targets supplied from a local starting folder.
        if (NormalizeLocalSaveDirectory(target_parent, parent) ||
            (request.no_test_file_create && picker_user_storage::Parse(target_parent, parsed) && parsed.unc))
            plan.state = LocalSaveDraftState::Ready;
        return plan;
    }

    // A bad draft leaf can be corrected in the existing filename edit. Keep
    // it verbatim and never give path-like drafts or unsupported filters a pass.
    if ((target.state == SaveTargetState::InvalidName || target.state == SaveTargetState::Unsupported) &&
        !request.filename.empty() && request.filename.find_first_of(L"\\/:") == request.filename.npos) {
        draft.filename = L"pulse-validation";
        if (ResolveSaveTarget(draft).state == SaveTargetState::Resolved)
            plan.state = LocalSaveDraftState::EditableName;
    }
    return plan;
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
