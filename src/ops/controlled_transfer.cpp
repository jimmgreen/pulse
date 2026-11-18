#include "controlled_transfer.h"
#include "../app/extensions/transfer_snapshot.h"
#include <winternl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <new>
#include <unordered_map>
#include <utility>
namespace pulse::ops {
namespace {
using namespace extensions;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
DWORD CreateRelativeDirectory(HANDLE parent, const std::wstring& name, Handle& output) {
    using Create = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using Convert = ULONG (WINAPI*)(NTSTATUS);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const auto create = reinterpret_cast<Create>(GetProcAddress(module, "NtCreateFile"));
    const auto convert = reinterpret_cast<Convert>(GetProcAddress(module, "RtlNtStatusToDosError"));
    if (!create || !convert || name.empty() || name.size() > 255) return ERROR_NOT_SUPPORTED;
    UNICODE_STRING text{};
    text.Buffer = const_cast<PWSTR>(name.data());
    text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    text.MaximumLength = text.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes); attributes.RootDirectory = parent;
    attributes.ObjectName = &text; attributes.Attributes = 0x40;
    IO_STATUS_BLOCK io{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto status = create(&handle, FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
        FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &io, nullptr, FILE_ATTRIBUTE_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 2 /* FILE_CREATE */, 0x1 /* FILE_DIRECTORY_FILE */ | 0x20 /* synchronous */ |
        0x00200000 /* FILE_OPEN_REPARSE_POINT */, nullptr, 0);
    if (status < 0) return convert(status);
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
        !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        const auto error = GetLastError(); CloseHandle(handle); return error ? error : ERROR_REPARSE_TAG_INVALID;
    }
    output.value = handle;
    return ERROR_SUCCESS;
}
std::wstring ParentRelative(std::wstring_view value) {
    const auto split = value.find_last_of(L'\\');
    return split == std::wstring_view::npos ? std::wstring{} : std::wstring(value.substr(0, split));
}
std::wstring LeafRelative(std::wstring_view value) {
    const auto split = value.find_last_of(L'\\');
    return std::wstring(value.substr(split == std::wstring_view::npos ? 0 : split + 1));
}
std::wstring DestinationPath(const extensions::transfer_plan::Draft& plan, std::wstring_view relative) {
    auto path = (std::filesystem::path(plan.target.path) / plan.tree_root_name / std::wstring(relative)).wstring();
    return path;
}
DWORD Check(const authorization::PrincipalBinding& binding, authorization::Registry& registry, const std::atomic<bool>& cancelled) {
    if (cancelled.load()) return ERROR_CANCELLED;
    return registry.ValidateBinding(binding) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}
DWORD Streams(HANDLE file) {
    // Bounded, aligned stream enumeration; no named stream is silently discarded.
    std::vector<uint64_t> storage(8192);
    const DWORD bytes = static_cast<DWORD>(storage.size() * sizeof(uint64_t));
    if (!GetFileInformationByHandleEx(file, FileStreamInfo, storage.data(), bytes)) {
        const auto error = GetLastError();
        return error == ERROR_HANDLE_EOF ? ERROR_SUCCESS : error;
    }
    size_t offset = 0;
    for (;;) {
        if (offset + offsetof(FILE_STREAM_INFO, StreamName) > bytes) return ERROR_INVALID_DATA;
        const auto* stream = reinterpret_cast<const FILE_STREAM_INFO*>(reinterpret_cast<const char*>(storage.data()) + offset);
        if (stream->StreamNameLength % sizeof(wchar_t) || offset + offsetof(FILE_STREAM_INFO, StreamName) + stream->StreamNameLength > bytes)
            return ERROR_INVALID_DATA;
        if (std::wstring_view(stream->StreamName, stream->StreamNameLength / sizeof(wchar_t)) != L"::$DATA") return ERROR_NOT_SUPPORTED;
        if (!stream->NextEntryOffset) break;
        if (stream->NextEntryOffset < offsetof(FILE_STREAM_INFO, StreamName) || stream->NextEntryOffset % 8 ||
            stream->NextEntryOffset > bytes - offset) return ERROR_INVALID_DATA;
        offset += stream->NextEntryOffset;
    }
    return ERROR_SUCCESS;
}
DWORD Metadata(HANDLE file, FILE_BASIC_INFO& basic) {
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) return GetLastError();
    constexpr DWORD supported = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN |
        FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if (basic.FileAttributes & ~supported) return ERROR_NOT_SUPPORTED;
    return Streams(file);
}
DWORD OpenOutput(HANDLE directory, std::wstring& name, Handle& file, bool create) {
    if (create) {
    std::array<uint8_t, 16> random{};
    if (!authorization::SecureRandomBytes(random)) return ERROR_GEN_FAILURE;
    name = L".pulse-stage-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (auto byte : random) { name.push_back(hex[byte >> 4]); name.push_back(hex[byte & 15]); }
    }
    using Create = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using Convert = ULONG (WINAPI*)(NTSTATUS);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const auto open = reinterpret_cast<Create>(GetProcAddress(module, "NtCreateFile"));
    const auto convert = reinterpret_cast<Convert>(GetProcAddress(module, "RtlNtStatusToDosError"));
    if (!open || !convert) return ERROR_NOT_SUPPORTED;
    UNICODE_STRING text{};
    text.Buffer = name.data(); text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t)); text.MaximumLength = text.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes); attributes.RootDirectory = directory; attributes.ObjectName = &text; attributes.Attributes = 0x40;
    IO_STATUS_BLOCK io{};
    const NTSTATUS result = open(&file.value, create ? (FILE_READ_DATA | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES |
        FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE) : (FILE_READ_ATTRIBUTES | SYNCHRONIZE), &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
        create ? 0 : (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), create ? 2 : 1,
        (create ? 0x40 : 0) /* FILE_NON_DIRECTORY_FILE */ | 0x20 /* synchronous */ |
        0x00200000 /* open reparse point */, nullptr, 0);
    if (result < 0) { file.value = INVALID_HANDLE_VALUE; return convert(result); }
    return ERROR_SUCCESS;
}
DWORD Commit(HANDLE file, HANDLE directory, const std::wstring& name) {
    const DWORD name_bytes = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::vector<uint64_t> storage((sizeof(FILE_RENAME_INFO) + name_bytes + 7) / 8);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists = FALSE; info->RootDirectory = directory; info->FileNameLength = name_bytes;
    std::memcpy(info->FileName, name.data(), name_bytes);
    using Set = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    using Convert = ULONG (WINAPI*)(NTSTATUS);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const auto set = reinterpret_cast<Set>(GetProcAddress(module, "NtSetInformationFile"));
    const auto convert = reinterpret_cast<Convert>(GetProcAddress(module, "RtlNtStatusToDosError"));
    if (!set || !convert) return ERROR_NOT_SUPPORTED;
    IO_STATUS_BLOCK io{};
    const auto result = set(file, &io, info, static_cast<ULONG>(sizeof(FILE_RENAME_INFO) + name_bytes),
        static_cast<FILE_INFORMATION_CLASS>(10));
    return result >= 0 ? ERROR_SUCCESS : convert(result);
}
DWORD DeleteOwned(HANDLE file, bool clear_readonly) {
    if (clear_readonly) {
        FILE_BASIC_INFO attributes{};
        attributes.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(file, FileBasicInfo, &attributes, sizeof(attributes))) return GetLastError();
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) ? ERROR_SUCCESS : GetLastError();
}
}
ControlledTransferResult ExecuteControlledTransfer(const extensions::transfer_plan::ExecutionPermit& permit,
    extensions::authorization::Registry& registry, const extensions::authorization::PrincipalBinding& expected,
    const std::atomic<bool>& cancelled, const std::function<void(const ControlledTransferProgress&)>& progress) {
    ControlledTransferResult result;
    size_t active_item = 0;
    try {
    result.permit_error = permit.TryStart(registry, expected);
    if (result.permit_error != extensions::transfer_plan::Error::None) return result;
    const auto& plan = permit.Plan().transfer;
    result.directory_tree = plan.directory_tree;
    if (plan.directory_tree && (plan.operation != extensions::transfer_plan::Operation::Copy ||
        plan.tree_root_name.empty() || plan.tree_directories.empty() || plan.tree_directories.size() > 128 ||
        plan.sources.size() > 512)) {
        result.permit_error = extensions::transfer_plan::Error::InvalidDraft;
        return result;
    }
    const bool move = plan.operation == extensions::transfer_plan::Operation::Move;
    for (const auto& source : plan.sources) {
        ControlledTransferItem item;
        item.source = source.path;
        item.destination = plan.directory_tree ? DestinationPath(plan, source.relative_destination) :
            (std::filesystem::path(plan.target.path) / std::filesystem::path(source.path).filename()).wstring();
        item.relative_destination = source.relative_destination;
        result.items.push_back(std::move(item));
    }
    auto target = extensions::transfer_snapshot::AcquireTransferTarget(plan.target, cancelled);
    if (!target) { if (!result.items.empty()) { result.items[0].state = target.error == ERROR_CANCELLED ? ControlledTransferState::Cancelled : ControlledTransferState::Failed; result.items[0].error = target.error; } return result; }
    std::vector<Handle> tree_directory_handles;
    std::unordered_map<std::wstring, HANDLE> tree_directory_by_relative;
    if (plan.directory_tree) {
        const auto root = std::find_if(plan.tree_directories.begin(), plan.tree_directories.end(),
            [](const auto& directory) { return directory.relative_destination.empty(); });
        if (root == plan.tree_directories.end()) {
            result.permit_error = extensions::transfer_plan::Error::InvalidDraft;
            return result;
        }
        for (const auto& directory : plan.tree_directories) {
            auto current = extensions::transfer_snapshot::CaptureTarget(directory.path, cancelled);
            if (!current || current.value->snapshot.identity != directory.identity) {
                const DWORD error = current ? ERROR_FILE_INVALID : current.error;
                if (!result.items.empty()) { result.items[0].error = error; result.items[0].state = ControlledTransferState::Failed; }
                else result.error = error;
                return result;
            }
        }
        for (size_t index = 0; index < plan.sources.size(); ++index) {
            auto source = extensions::transfer_snapshot::AcquireTransferSource(plan.sources[index], false, cancelled);
            if (!source) {
                result.items[index].error = source.error;
                result.items[index].state = source.error == ERROR_CANCELLED ? ControlledTransferState::Cancelled : ControlledTransferState::Failed;
                return result;
            }
            FILE_BASIC_INFO basic{};
            result.items[index].error = Metadata(source.value->handles.Leaf(), basic);
            if (result.items[index].error) return result;
        }
        Handle existing_root;
        auto root_name = plan.tree_root_name;
        const DWORD root_status = OpenOutput(target.value->handles.Leaf(), root_name, existing_root, false);
        if (!root_status || (root_status != ERROR_FILE_NOT_FOUND && root_status != ERROR_PATH_NOT_FOUND)) {
            const DWORD error = root_status ? root_status : ERROR_FILE_EXISTS;
            if (!result.items.empty()) { result.items[0].error = error; result.items[0].state = ControlledTransferState::Failed; }
            else result.error = error;
            return result;
        }
        Handle root_handle;
        DWORD tree_error = CreateRelativeDirectory(target.value->handles.Leaf(), plan.tree_root_name, root_handle);
        if (tree_error) {
            if (!result.items.empty()) { result.items[0].error = tree_error; result.items[0].state = ControlledTransferState::Failed; }
            else result.error = tree_error;
            return result;
        }
        tree_directory_handles.push_back(std::move(root_handle));
        tree_directory_by_relative.emplace(L"", tree_directory_handles.back().value);
        std::vector<const extensions::transfer_plan::TreeDirectory*> ordered;
        for (const auto& directory : plan.tree_directories)
            if (!directory.relative_destination.empty()) ordered.push_back(&directory);
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
            return std::count(left->relative_destination.begin(), left->relative_destination.end(), L'\\') <
                std::count(right->relative_destination.begin(), right->relative_destination.end(), L'\\');
        });
        for (const auto* directory : ordered) {
            if ((tree_error = Check(permit.Principal(), registry, cancelled))) break;
            const auto parent = tree_directory_by_relative.find(ParentRelative(directory->relative_destination));
            if (parent == tree_directory_by_relative.end()) { tree_error = ERROR_INVALID_NAME; break; }
            Handle child;
            tree_error = CreateRelativeDirectory(parent->second, LeafRelative(directory->relative_destination), child);
            if (tree_error) break;
            tree_directory_handles.push_back(std::move(child));
            tree_directory_by_relative.emplace(directory->relative_destination, tree_directory_handles.back().value);
        }
        if (tree_error) {
            if (!result.items.empty()) { result.items[0].error = tree_error; result.items[0].state = tree_error == ERROR_CANCELLED ? ControlledTransferState::Cancelled : ControlledTransferState::Failed; }
            else result.error = tree_error;
            return result;
        }
    }
    for (size_t i = 0; i < result.items.size(); ++i) {
        if (plan.directory_tree) break; // The new root is empty, so no descendant can conflict yet.
        auto name = std::filesystem::path(result.items[i].source).filename().wstring();
        for (size_t j = 0; j < i; ++j) {
            const auto prior = std::filesystem::path(result.items[j].source).filename().wstring();
            if (CompareStringOrdinal(name.c_str(), -1, prior.c_str(), -1, TRUE) == CSTR_EQUAL) {
                result.items[i].state = ControlledTransferState::Failed;
                result.items[i].error = ERROR_FILE_EXISTS;
                return result;
            }
        }
        Handle existing;
        const auto error = OpenOutput(target.value->handles.Leaf(), name, existing, false);
        if (!error || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) {
            result.items[i].state = ControlledTransferState::Failed;
            result.items[i].error = error ? error : ERROR_FILE_EXISTS;
            return result;
        }
    }
    for (size_t index = 0; index < plan.sources.size(); ++index) {
        active_item = index;
        auto& item = result.items[index];
        item.state = ControlledTransferState::Failed;
        struct CancelState {
            ControlledTransferItem& item;
            ~CancelState() { if (item.error == ERROR_CANCELLED && item.state == ControlledTransferState::Failed) item.state = ControlledTransferState::Cancelled; }
        } cancel_state{item};
        item.error = Check(permit.Principal(), registry, cancelled);
        if (item.error) return result;
        auto source = extensions::transfer_snapshot::AcquireTransferSource(plan.sources[index], move, cancelled);
        if (!source) { item.error = source.error; return result; }
        const HANDLE input = source.value->handles.Leaf();
        FILE_BASIC_INFO basic{};
        item.error = Metadata(input, basic);
        if (item.error) return result;
        std::vector<unsigned char> buffer(256 * 1024);
        Handle staging;
        std::wstring staging_name;
        const auto output_parent = plan.directory_tree
            ? tree_directory_by_relative.find(ParentRelative(plan.sources[index].relative_destination))
            : tree_directory_by_relative.end();
        if (plan.directory_tree && output_parent == tree_directory_by_relative.end()) { item.error = ERROR_INVALID_NAME; return result; }
        const HANDLE output_directory = plan.directory_tree ? output_parent->second : target.value->handles.Leaf();
        item.error = Check(permit.Principal(), registry, cancelled);
        if (!item.error) item.error = extensions::transfer_snapshot::VerifyTransferTarget(target.value->handles.Leaf(), plan.target);
        if (!item.error) item.error = OpenOutput(output_directory, staging_name, staging, true);
        if (item.error) return result;
        bool committed = false;
        struct Cleanup {
            Handle& handle; ControlledTransferItem& item; bool& committed; std::wstring& name;
            ~Cleanup() {
                if (!committed) {
                    item.cleanup_error = DeleteOwned(handle.value, true);
                    if (item.cleanup_error) item.remaining_staging_name = name;
                    if (item.error == ERROR_CANCELLED) item.state = ControlledTransferState::Cancelled;
                }
            }
        } owned_cleanup{staging, item, committed, staging_name};
        // Cleanup is always by the still-owned staging handle, never by its path.
        while (!item.error && item.bytes < plan.sources[index].identity.size) {
            item.error = Check(permit.Principal(), registry, cancelled);
            if (item.error) break;
            DWORD count = 0;
            const DWORD requested = static_cast<DWORD>((std::min)(static_cast<uint64_t>(buffer.size()), plan.sources[index].identity.size - item.bytes));
            if (!ReadFile(input, buffer.data(), requested, &count, nullptr)) { item.error = GetLastError(); break; }
            if (!count) break;
            if (count > plan.sources[index].identity.size - item.bytes) { item.error = ERROR_FILE_INVALID; break; }
            DWORD written = 0;
            if (!WriteFile(staging.value, buffer.data(), count, &written, nullptr)) { item.error = GetLastError(); break; }
            if (written != count) { item.error = ERROR_WRITE_FAULT; break; }
            item.bytes += count;
            if (progress) { try { progress({index, item.bytes, plan.sources[index].identity.size, false}); } catch (...) { item.error = ERROR_UNHANDLED_EXCEPTION; } }
        }
        if (!item.error && item.bytes != plan.sources[index].identity.size) item.error = ERROR_FILE_INVALID;
        FILE_BASIC_INFO after{};
        FILE_STANDARD_INFO size{};
        if (!item.error) item.error = Metadata(input, after);
        if (!item.error && !GetFileInformationByHandleEx(input, FileStandardInfo, &size, sizeof(size))) item.error = GetLastError();
        if (!item.error && (after.LastWriteTime.QuadPart != basic.LastWriteTime.QuadPart ||
            static_cast<uint64_t>(size.EndOfFile.QuadPart) != plan.sources[index].identity.size)) item.error = ERROR_FILE_INVALID;
        if (!item.error && !FlushFileBuffers(staging.value)) item.error = GetLastError();
        basic.ChangeTime.QuadPart = 0; // destination change time belongs to this operation.
        if (!item.error && !SetFileInformationByHandle(staging.value, FileBasicInfo, &basic, sizeof(basic))) item.error = GetLastError();
        if (!item.error) item.error = Check(permit.Principal(), registry, cancelled);
        if (!item.error) item.error = extensions::transfer_snapshot::VerifyTransferTarget(target.value->handles.Leaf(), plan.target);
        if (!item.error) item.error = Commit(staging.value, output_directory,
            plan.directory_tree ? LeafRelative(plan.sources[index].relative_destination) : std::filesystem::path(item.source).filename().wstring());
        if (item.error) return result;
        committed = true;
        item.state = move ? ControlledTransferState::DestinationCommittedSourceRetained : ControlledTransferState::Copied;
        if (progress) { try { progress({index, item.bytes, plan.sources[index].identity.size, true}); } catch (...) { item.error = ERROR_UNHANDLED_EXCEPTION; return result; } }
        if (move) {
            item.error = Check(permit.Principal(), registry, cancelled);
            FILE_BASIC_INFO final_basic{};
            if (!item.error) item.error = Metadata(input, final_basic);
            if (!item.error && final_basic.LastWriteTime.QuadPart != basic.LastWriteTime.QuadPart) item.error = ERROR_FILE_INVALID;
            if (!item.error) item.error = DeleteOwned(input, false);
            if (item.error) return result;
            item.state = ControlledTransferState::Moved;
        }
    }
    if (plan.directory_tree) {
        const DWORD final_check = Check(permit.Principal(), registry, cancelled);
        if (final_check) {
            if (!result.items.empty()) {
                auto& item = result.items.back(); item.error = final_check;
                item.state = final_check == ERROR_CANCELLED ? ControlledTransferState::Cancelled : ControlledTransferState::Failed;
            } else result.error = final_check;
            return result;
        }
    }
    result.complete = true;
    return result;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        result.error = ERROR_UNHANDLED_EXCEPTION;
    }
    if (active_item < result.items.size()) {
        auto& item = result.items[active_item];
        item.error = result.error;
        if (item.state == ControlledTransferState::NotStarted) item.state = ControlledTransferState::Failed;
    }
    return result;
}
}
