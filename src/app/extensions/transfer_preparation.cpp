#include "transfer_preparation.h"
#include <algorithm>
#include <filesystem>
#include <memory>

namespace pulse::extensions::transfer_preparation {
namespace {
transfer_plan::Error Check(const authorization::PrincipalBinding& principal,
    authorization::Registry& registry, const std::atomic<bool>& cancelled) {
    if (cancelled.load()) return transfer_plan::Error::Cancelled;
    return registry.ValidateBinding(principal) ? transfer_plan::Error::None : transfer_plan::Error::Revoked;
}
std::wstring Extended(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}
std::wstring Join(const std::wstring& parent, std::wstring_view leaf) {
    return parent + (parent.ends_with(L"\\") ? L"" : L"\\") + std::wstring(leaf);
}
uint64_t FileTime(const FILETIME& value) {
    return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
struct FindHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
};
struct TreeCapture {
    transfer_plan::Draft draft;
    std::vector<transfer_snapshot::Target> directories;
    std::vector<transfer_snapshot::Source> files;
    size_t plan_bytes = 0;
    size_t directory_count = 0;
    size_t file_count = 0;
};
bool AddBytes(TreeCapture& tree, size_t chars) {
    if (chars > (1024 * 1024 - tree.plan_bytes) / sizeof(wchar_t)) return false;
    tree.plan_bytes += chars * sizeof(wchar_t);
    return true;
}
DWORD WalkTree(const std::wstring& path, const std::wstring& relative, TreeCapture& tree,
    const authorization::PrincipalBinding& principal, authorization::Registry& registry,
    const std::atomic<bool>& cancelled) {
    const auto state = Check(principal, registry, cancelled);
    if (state != transfer_plan::Error::None) return state == transfer_plan::Error::Cancelled ? ERROR_CANCELLED : ERROR_ACCESS_DENIED;
    FindHandle find;
    WIN32_FIND_DATAW row{};
    find.value = FindFirstFileW(Join(Extended(path), L"*").c_str(), &row);
    if (find.value == INVALID_HANDLE_VALUE) return GetLastError();
    std::vector<WIN32_FIND_DATAW> entries;
    do {
        if (wcscmp(row.cFileName, L".") != 0 && wcscmp(row.cFileName, L"..") != 0)
            entries.push_back(row);
    } while (FindNextFileW(find.value, &row));
    const DWORD enumeration_error = GetLastError();
    if (enumeration_error != ERROR_NO_MORE_FILES) return enumeration_error;
    std::sort(entries.begin(), entries.end(), [](const WIN32_FIND_DATAW& left, const WIN32_FIND_DATAW& right) {
        const int insensitive = CompareStringOrdinal(left.cFileName, -1, right.cFileName, -1, TRUE);
        if (insensitive != CSTR_EQUAL) return insensitive == CSTR_LESS_THAN;
        return CompareStringOrdinal(left.cFileName, -1, right.cFileName, -1, FALSE) == CSTR_LESS_THAN;
    });
    for (const auto& entry : entries) {
        if (cancelled.load()) return ERROR_CANCELLED;
        const DWORD attrs = entry.dwFileAttributes;
        if ((attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SPARSE_FILE |
                FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_COMPRESSED)) != 0) return ERROR_NOT_SUPPORTED;
        const std::wstring child = Join(path, entry.cFileName);
        const std::wstring child_relative = relative.empty() ? entry.cFileName : Join(relative, entry.cFileName);
        if (!AddBytes(tree, child.size() + child_relative.size())) return ERROR_BUFFER_OVERFLOW;
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            if (++tree.directory_count > 128) return ERROR_BUFFER_OVERFLOW;
            auto opened = transfer_snapshot::CaptureTarget(child, cancelled);
            if (!opened) return opened.error;
            // Directory enumeration timestamps can lag the handle metadata on NTFS.
            // Freeze the opened directory identity and validate it again before execution.
            tree.draft.tree_directories.push_back({child, child_relative, opened.value->snapshot.identity});
            tree.directories.push_back(std::move(*opened.value));
            if (const auto error = WalkTree(child, child_relative, tree, principal, registry, cancelled)) return error;
        } else {
            if (++tree.file_count > 512) return ERROR_BUFFER_OVERFLOW;
            auto opened = transfer_snapshot::CaptureSource(child, cancelled);
            if (!opened) return opened.error;
            const uint64_t enumerated_size = (static_cast<uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
            if (opened.value->snapshot.identity.size != enumerated_size ||
                opened.value->snapshot.identity.last_write != FileTime(entry.ftLastWriteTime)) return ERROR_FILE_INVALID;
            auto snapshot = opened.value->snapshot;
            snapshot.relative_destination = child_relative;
            tree.draft.sources.push_back(std::move(snapshot));
            tree.files.push_back(std::move(*opened.value));
        }
    }
    return ERROR_SUCCESS;
}
}

Result Prepare(const Request& request, const authorization::PrincipalBinding& principal,
    authorization::Registry& registry, transfer_plan::Store& store,
    const std::atomic<bool>& cancelled) {
    using Error = transfer_plan::Error;
    auto state = Check(principal, registry, cancelled);
    if (state != Error::None) return {state, ERROR_SUCCESS, std::nullopt};
    const transfer_plan::Limits bounds;
    if ((request.operation != transfer_plan::Operation::Copy && request.operation != transfer_plan::Operation::Move) ||
        (request.directory_tree ? (request.operation != transfer_plan::Operation::Copy || request.tree_source.empty())
                                : (request.sources.empty() || request.sources.size() > bounds.sources)) || request.target.empty() ||
        request.target.size() > bounds.path_chars) return {Error::InvalidDraft, ERROR_SUCCESS, std::nullopt};
    if (request.directory_tree && (request.tree_source.size() > bounds.path_chars ||
        !request.sources.empty() || !request.source_sizes.empty() || !request.source_modified.empty()))
        return {Error::InvalidDraft, ERROR_SUCCESS, std::nullopt};
    if ((!request.source_sizes.empty() && request.source_sizes.size() != request.sources.size()) ||
        (!request.source_modified.empty() && request.source_modified.size() != request.sources.size()))
        return {Error::InvalidDraft, ERROR_SUCCESS, std::nullopt};
    // Bound work before any I/O, not merely when serializing the final plan.
    size_t chars = request.target.size();
    for (size_t index = 0; index < request.sources.size(); ++index) {
        const auto& path = request.sources[index];
        if (path.empty() || path.size() > bounds.path_chars ||
            chars > bounds.plan_bytes / sizeof(wchar_t) - path.size())
            return {Error::InvalidDraft, ERROR_SUCCESS, std::nullopt};
        chars += path.size();
    }
    auto target = transfer_snapshot::CaptureTarget(request.target, cancelled);
    if (!target) return {cancelled.load() ? Error::Cancelled : Error::InvalidDraft, target.error, std::nullopt};
    transfer_plan::Draft draft;
    draft.operation = request.operation;
    draft.target = target.value->snapshot;
    if (request.directory_tree) {
        auto root = transfer_snapshot::CaptureTarget(request.tree_source, cancelled);
        if (!root) return {cancelled.load() ? Error::Cancelled : Error::InvalidDraft, root.error, std::nullopt};
        if (request.tree_source_modified && root.value->snapshot.identity.last_write != request.tree_source_modified)
            return {Error::InvalidDraft, ERROR_FILE_INVALID, std::nullopt};
        TreeCapture tree;
        tree.draft.operation = transfer_plan::Operation::Copy;
        tree.draft.target = draft.target;
        tree.draft.directory_tree = true;
        tree.draft.tree_root_name = std::filesystem::path(request.tree_source).filename().wstring();
        tree.draft.tree_directories.push_back({root.value->snapshot.path, L"", root.value->snapshot.identity});
        tree.directories.push_back(std::move(*root.value));
        tree.directory_count = 1;
        if (!AddBytes(tree, tree.draft.tree_root_name.size())) return {Error::InvalidDraft, ERROR_BUFFER_OVERFLOW, std::nullopt};
        const DWORD scan_error = WalkTree(request.tree_source, L"", tree, principal, registry, cancelled);
        if (scan_error) return {cancelled.load() ? Error::Cancelled : Error::InvalidDraft, scan_error, std::nullopt};
        state = Check(principal, registry, cancelled);
        if (state != Error::None) return {state, ERROR_SUCCESS, std::nullopt};
        auto created = store.Create(principal, tree.draft);
        if (!created) return {created.error, ERROR_SUCCESS, std::nullopt};
        state = Check(principal, registry, cancelled);
        if (state != Error::None) {
            store.Cancel(*created.value, principal);
            return {state, ERROR_SUCCESS, std::nullopt};
        }
        return {Error::None, ERROR_SUCCESS, std::move(created.value)};
    }
    draft.sources.reserve(request.sources.size());
    for (size_t index = 0; index < request.sources.size(); ++index) {
        const auto& path = request.sources[index];
        state = Check(principal, registry, cancelled);
        if (state != Error::None) return {state, ERROR_SUCCESS, std::nullopt};
        auto source = transfer_snapshot::CaptureSource(path, cancelled);
        if (!source) return {cancelled.load() ? Error::Cancelled : Error::InvalidDraft, source.error, std::nullopt};
        const auto& identity = source.value->snapshot.identity;
        if ((!request.source_sizes.empty() && identity.size != request.source_sizes[index]) ||
            (!request.source_modified.empty() && identity.last_write != request.source_modified[index]))
            return {Error::InvalidDraft, ERROR_FILE_INVALID, std::nullopt};
        draft.sources.push_back(std::move(source.value->snapshot));
    }
    state = Check(principal, registry, cancelled);
    if (state != Error::None) return {state, ERROR_SUCCESS, std::nullopt};
    auto created = store.Create(principal, draft);
    if (!created) return {created.error, ERROR_SUCCESS, std::nullopt};
    // Cancellation racing publication must not leave a usable pending plan.
    state = Check(principal, registry, cancelled);
    if (state != Error::None) {
        store.Cancel(*created.value, principal);
        return {state, ERROR_SUCCESS, std::nullopt};
    }
    return {Error::None, ERROR_SUCCESS, std::move(created.value)};
}
}
