#include "file_save_access_policy.h"
#include <algorithm>
#include <array>
#include <cwchar>
#include <new>
#include <vector>

namespace pulse::ui {
namespace {
constexpr DWORD kUnsupportedAttributes = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
    0x00040000u /* RECALL_ON_OPEN */ | 0x00400000u /* RECALL_ON_DATA_ACCESS */;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& h) noexcept : value(h.value) { h.value = INVALID_HANDLE_VALUE; }
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    DWORD Close() noexcept {
        if (value == INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
        const HANDLE closing = value; value = INVALID_HANDLE_VALUE;
        return CloseHandle(closing) ? ERROR_SUCCESS : GetLastError();
    }
};
std::wstring Native(const std::wstring& path) { return L"\\\\?\\" + path; }
DWORD Cancelled(const std::function<bool()>& fn) noexcept {
    try { return fn && fn() ? ERROR_CANCELLED : ERROR_SUCCESS; }
    catch (...) { return ERROR_UNHANDLED_EXCEPTION; }
}
DWORD Notify(const SaveAccessObserver& fn, SaveAccessStage stage) noexcept {
    try { if (fn) fn(stage); return ERROR_SUCCESS; }
    catch (...) { return ERROR_UNHANDLED_EXCEPTION; }
}
DWORD Metadata(HANDLE handle, SaveObjectIdentity& id, DWORD& attributes) {
    FILE_BASIC_INFO basic{}; FILE_ID_INFO file{};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) return GetLastError();
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &file, sizeof(file))) return GetLastError();
    attributes = basic.FileAttributes;
    id.volume = file.VolumeSerialNumber; id.creation_time = basic.CreationTime.QuadPart;
    std::copy_n(file.FileId.Identifier, id.file.size(), id.file.begin());
    return ERROR_SUCCESS;
}
DWORD SamePath(HANDLE handle, const std::wstring& path) {
    std::array<wchar_t, 32768> name{};
    const DWORD n = GetFinalPathNameByHandleW(handle, name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n) return GetLastError();
    if (n >= name.size()) return ERROR_FILENAME_EXCED_RANGE;
    const wchar_t* actual = name.data();
    if (wcsncmp(actual, L"\\\\?\\", 4) == 0) actual += 4;
    return CompareStringOrdinal(actual, -1, path.c_str(), -1, TRUE) == CSTR_EQUAL ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
}
DWORD Scope(const std::wstring& path) {
    if (path.size() < 3 || !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || path[2] != L'\\' || path.find(L':', 2) != path.npos) return ERROR_NOT_SUPPORTED;
    return GetDriveTypeW(path.substr(0, 3).c_str()) == DRIVE_FIXED ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
}
// Attribute-only opens do not participate in normal sharing checks. Request
// actual directory read access while withholding write/delete sharing, retaining
// each ordinary ancestor until the probe and cleanup have finished.
DWORD GuardParents(const SaveConfirmationSnapshot& snapshot, std::vector<Handle>& guards,
                   const std::function<bool()>& cancelled, bool require_unencrypted) {
    const auto& parent = snapshot.Parent();
    DWORD error = Scope(parent); if (error) return error;
    for (size_t end = 3;;) {
        if ((error = Cancelled(cancelled))) return error;
        const auto path = parent.substr(0, end);
        Handle handle(CreateFileW(Native(path).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (handle.value == INVALID_HANDLE_VALUE) return GetLastError();
        SaveObjectIdentity identity; DWORD attributes = 0;
        if ((error = Metadata(handle.value, identity, attributes))) return error;
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
        if (const DWORD rejected = ValidateSaveAccessAttributes(attributes, require_unencrypted)) return rejected;
        if ((error = SamePath(handle.value, path))) return error;
        if (end == parent.size() && identity != snapshot.ParentIdentity()) return ERROR_FILE_INVALID;
        guards.push_back(std::move(handle));
        if (end == parent.size()) break;
        const auto next = parent.find(L'\\', end + (end == 3 ? 0 : 1));
        end = next == parent.npos ? parent.size() : next;
    }
    return ERROR_SUCCESS;
}
DWORD RecheckParents(const SaveConfirmationSnapshot& snapshot, const std::vector<Handle>& guards, bool require_unencrypted) {
    const auto& parent = snapshot.Parent();
    size_t end = 3;
    for (const auto& guard : guards) {
        const DWORD path_error = SamePath(guard.value, parent.substr(0, end));
        if (path_error) return path_error == ERROR_NOT_SUPPORTED ? ERROR_FILE_INVALID : path_error;
        if (require_unencrypted && end != parent.size()) {
            SaveObjectIdentity identity; DWORD attributes = 0;
            const DWORD error = Metadata(guard.value, identity, attributes);
            if (error) return error;
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
            if (const DWORD rejected = ValidateSaveAccessAttributes(attributes, true)) return rejected;
        }
        if (end == parent.size()) {
            SaveObjectIdentity identity; DWORD attributes = 0;
            const DWORD error = Metadata(guard.value, identity, attributes);
            if (error) return error;
            if (identity != snapshot.ParentIdentity()) return ERROR_FILE_INVALID;
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
            if (const DWORD rejected = ValidateSaveAccessAttributes(attributes, require_unencrypted)) return rejected;
            return ERROR_SUCCESS;
        }
        const auto next = parent.find(L'\\', end + (end == 3 ? 0 : 1));
        end = next == parent.npos ? parent.size() : next;
    }
    return ERROR_INVALID_HANDLE;
}
void Refresh(SaveAccessResult& result, const SaveConfirmationSnapshot& snapshot, const std::function<bool()>& cancelled) {
    const auto fresh = RevalidateSaveConfirmation(snapshot, cancelled);
    if (fresh.state == SaveRevalidationState::ConfirmOverwrite && fresh.snapshot) {
        result.state = SaveAccessState::NeedsConfirmation; result.error = ERROR_SUCCESS;
        result.confirmation = fresh.snapshot;
    } else {
        result.state = SaveAccessState::Failed;
        result.error = fresh.error ? fresh.error : ERROR_RETRY;
    }
}
}

DWORD ValidateSaveAccessAttributes(DWORD attributes, bool require_unencrypted) noexcept {
    return (attributes & (kUnsupportedAttributes | (require_unencrypted ? FILE_ATTRIBUTE_ENCRYPTED : 0u))) ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS;
}

SaveAccessResult ValidateLocalSaveAccessV1(const SaveConfirmationSnapshot& snapshot,
    const std::function<bool()>& cancelled, const SaveAccessObserver& observer, bool require_unencrypted) noexcept {
    SaveAccessResult result;
    try {
        result.path = snapshot.Path(); result.parent_identity = snapshot.ParentIdentity();
        result.error = Scope(result.path); if (result.error) return result;
        std::vector<Handle> guards;
        result.error = GuardParents(snapshot, guards, cancelled, require_unencrypted); if (result.error) return result;
        result.stage = SaveAccessStage::Revalidation;
        auto revalidated = RevalidateSaveConfirmation(snapshot, cancelled);
        if (revalidated.state != SaveRevalidationState::Ready || !revalidated.snapshot) {
            if (revalidated.state == SaveRevalidationState::ConfirmOverwrite && revalidated.snapshot) {
                result.state = SaveAccessState::NeedsConfirmation; result.error = ERROR_SUCCESS;
                result.confirmation = std::move(revalidated.snapshot);
            } else result.error = revalidated.error;
            return result;
        }
        const auto& current = *revalidated.snapshot;
        // Allocate native path before creating the temporary object.
        const auto native = Native(result.path);
        result.stage = SaveAccessStage::BeforeOpen;
        result.error = Notify(observer, result.stage); if (result.error) return result;
        result.error = Cancelled(cancelled); if (result.error) return result;
        // Also reject a namespace change observed after callbacks and before any
        // destructive-capable open. This is additional evidence, not a substitute
        // for the retained read/share guard across the pathname-based CreateFile.
        result.error = RecheckParents(current, guards, require_unencrypted); if (result.error) return result;
        const bool existing = current.TargetExists();
        result.stage = existing ? SaveAccessStage::OpenExisting : SaveAccessStage::CreateNew;
        Handle handle(CreateFileW(native.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | (existing ? 0 : DELETE),
            existing ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : 0, nullptr,
            existing ? OPEN_EXISTING : CREATE_NEW,
            FILE_FLAG_OPEN_REPARSE_POINT | (existing ? 0 : FILE_FLAG_DELETE_ON_CLOSE), nullptr));
        if (handle.value == INVALID_HANDLE_VALUE) {
            result.error = GetLastError();
            if ((!existing && result.error == ERROR_FILE_EXISTS) || (existing && result.error == ERROR_FILE_NOT_FOUND)) {
                const DWORD cancellation = Cancelled(cancelled);
                if (cancellation) result.error = cancellation;
                else Refresh(result, current, cancelled);
            }
            return result;
        }
        if (!existing) result.cleanup = SaveAccessCleanup::Unconfirmed;
        // From this point all work is nonthrowing until Close and cleanup query.
        DWORD attributes = 0;
        SaveObjectIdentity identity;
        DWORD primary = Metadata(handle.value, identity, attributes);
        if (!primary) result.probe_identity = identity;
        if (!primary) primary = ValidateSaveAccessAttributes(attributes, require_unencrypted);
        if (!primary && (attributes & FILE_ATTRIBUTE_DIRECTORY)) primary = ERROR_DIRECTORY;
        if (!primary && (attributes & FILE_ATTRIBUTE_READONLY)) primary = ERROR_ACCESS_DENIED;
        if (!primary) primary = SamePath(handle.value, result.path);
        const bool replaced = existing && !primary && identity != *current.TargetIdentity();
        if (primary) result.primary_stage = SaveAccessStage::Identity;
        result.stage = existing ? SaveAccessStage::Identity : SaveAccessStage::Created;
        if (!primary) {
            result.primary_stage = result.stage;
            primary = Notify(observer, result.stage);
        }
        if (!primary) primary = Cancelled(cancelled);
        result.primary_error = primary;
        result.stage = SaveAccessStage::Close;
        const DWORD close_error = handle.Close();
        if (close_error) {
            result.error = close_error; result.cleanup_error = close_error;
            return result;
        }
        if (!primary) {
            result.primary_stage = SaveAccessStage::AfterClose;
            primary = Cancelled(cancelled);
        }
        result.primary_error = primary;
        if (!existing) {
            result.stage = SaveAccessStage::AfterClose;
            const DWORD observer_error = Notify(observer, result.stage);
            if (!primary) {
                result.primary_stage = SaveAccessStage::AfterClose;
                primary = observer_error;
            }
            if (!primary) primary = Cancelled(cancelled);
            result.primary_error = primary;
            result.stage = SaveAccessStage::CleanupQuery;
            Handle check(CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            if (check.value == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND) {
                    result.error = error; result.cleanup_error = error; return result;
                }
                result.cleanup = SaveAccessCleanup::Confirmed;
            } else {
                SaveObjectIdentity found; DWORD found_attributes = 0;
                const DWORD error = Metadata(check.value, found, found_attributes);
                if (error || !result.probe_identity || found == *result.probe_identity) {
                    result.error = error ? error : ERROR_BUSY; result.cleanup_error = result.error; return result;
                }
                result.cleanup = SaveAccessCleanup::Confirmed;
                const DWORD check_close = check.Close();
                if (check_close) { result.error = check_close; result.cleanup_error = check_close; return result; }
                if (primary) { result.error = primary; return result; }
                Refresh(result, current, cancelled); return result;
            }
        }
        if (primary) { result.error = primary; return result; }
        if (replaced) { Refresh(result, current, cancelled); return result; }
        result.stage = SaveAccessStage::ParentCheck;
        auto final = RevalidateSaveConfirmation(current, cancelled);
        if (final.state == SaveRevalidationState::ConfirmOverwrite && final.snapshot) {
            result.state = SaveAccessState::NeedsConfirmation; result.error = ERROR_SUCCESS;
            result.confirmation = std::move(final.snapshot); return result;
        }
        if (final.state != SaveRevalidationState::Ready || !final.snapshot) {
            result.error = final.error ? final.error : ERROR_INVALID_DATA; return result;
        }
        // A write probe of a now-disappeared existing object does not validate a
        // fresh create. A later explicit attempt may take the creation branch.
        if (existing && !final.snapshot->TargetExists()) { result.error = ERROR_RETRY; return result; }
        result.stage = SaveAccessStage::Complete; result.state = SaveAccessState::Ready; result.error = ERROR_SUCCESS;
        return result;
    } catch (const std::bad_alloc&) { result.state = SaveAccessState::Failed; result.error = ERROR_NOT_ENOUGH_MEMORY; }
    catch (...) { result.state = SaveAccessState::Failed; result.error = ERROR_UNHANDLED_EXCEPTION; }
    return result;
}
}
