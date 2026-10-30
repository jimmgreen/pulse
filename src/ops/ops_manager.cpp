#include "shell_command_template.h"
// ops_manager.cpp — See ops_manager.h for the contract.
#include "ops_manager.h"
#include "../common/runtime_log.h"
#include "operation_presentation.h"
#include "transfer_rate_estimator.h"
#include "transfer_engine.h"
#include "shell_command.h"
#include "shortcut_launch.h"
#include "elevated_transfer.h"
#include "source_listing.h"
#include "../ipc/shell_client.h"
#include "../common/json_utils.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/shell_id.h"
#include "../common/rename_filename.h"
#include "../common/utf8_file.h"
#include "recycle_empty.h"
#include <objbase.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <system_error>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <winioctl.h>

namespace pulse::ops {

#ifdef PULSE_ELEVATED_TEST_CLIENT
void ObserveTransferScanForReview(OpsManager&, const std::wstring&);
void ObserveTransferStartForReview(OpsManager&, uint64_t);
void ObserveShellCallbacksForReview(const ipc::ShellClient::Callbacks&);
#endif

namespace {

std::wstring FileName(const std::wstring& path) {
    std::wstring_view v = path;
    if (v.size() > 1 && v.back() == L'\\') v.remove_suffix(1);
    auto pos = v.find_last_of(L"\\/");
    if (pos != std::wstring_view::npos) return std::wstring(v.substr(pos + 1));
    return std::wstring(v);
}

// Merged Explorer properties sheet for several items. Runs on its own STA
// thread: the sheet may call back into the data object through COM, so the
// owning apartment must keep pumping messages while the sheet holds it.
void ShowMultiFilePropertiesAsync(std::vector<std::wstring> paths) {
    std::thread([paths = std::move(paths)]() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return;
        std::vector<PIDLIST_ABSOLUTE> pidls;
        pidls.reserve(paths.size());
        for (const auto& path : paths) {
            const std::wstring shell_path = pulse::path::StripExtendedPathPrefix(path);
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(shell_id::ParseId(shell_path, &pidl)) && pidl)
                pidls.push_back(pidl);
        }
        IDataObject* data = nullptr;
        if (pidls.size() > 1) {
            IShellItemArray* array = nullptr;
            if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                    const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array)) && array) {
                array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
                array->Release();
            }
        }
        for (auto* pidl : pidls) CoTaskMemFree(pidl);
        bool shown = data && SUCCEEDED(SHMultiFileProperties(data, 0));
        if (!shown) {
            wchar_t message[160]{};
            swprintf_s(message, L"Pulse: SHMultiFileProperties failed for %zu items\n", paths.size());
            OutputDebugStringW(message);
            const std::wstring first = pulse::path::StripExtendedPathPrefix(paths.front());
            SHObjectProperties(nullptr, SHOP_FILEPATH, first.c_str(), nullptr);
        }
        // The sheet may live on this thread (a thread's windows die with it)
        // or on a shell thread that calls back into `data` through COM. Pump
        // while either holds on; a short grace covers asynchronous creation.
        const ULONGLONG started = GetTickCount64();
        while (shown) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 200, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (!IsDialogMessageW(GetActiveWindow(), &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            bool owns_window = false;
            EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM found) -> BOOL {
                if (!IsWindowVisible(hwnd)) return TRUE;
                *reinterpret_cast<bool*>(found) = true;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&owns_window));
            ULONG refs = 1;
            if (data) {
                data->AddRef();
                refs = data->Release();
            }
            if (!owns_window && refs <= 1 && GetTickCount64() - started > 3000) break;
        }
        if (data) data->Release();
        CoUninitialize();
    }).detach();
}

std::wstring ParentOf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();

    std::wstring prefix;
    std::wstring_view core = p;
    if (p.starts_with(L"\\\\?\\UNC\\")) {
        prefix = L"\\\\?\\UNC\\";
        core.remove_prefix(8);
    } else if (p.starts_with(L"\\\\?\\")) {
        prefix = L"\\\\?\\";
        core.remove_prefix(4);
    } else if (p.starts_with(L"\\\\")) {
        prefix = L"\\\\";
        core.remove_prefix(2);
    }

    std::wstring temp(core);
    const auto pos = temp.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos == 0) return path;
    temp.resize(pos);
    if (temp.size() == 2 && temp[1] == L':') temp += L'\\';
    return prefix + temp;
}


bool IsVolumeRoot(const std::wstring& path) {
    std::wstring_view view = path;
    while (view.size() > 1 && (view.back() == L'\\' || view.back() == L'/'))
        view.remove_suffix(1);
    if (view.starts_with(L"\\\\?\\UNC\\")) {
        view.remove_prefix(8);
        const auto slash = view.find(L'\\');
        if (slash == std::wstring_view::npos) return true;
        return view.find(L'\\', slash + 1) == std::wstring_view::npos;
    }
    if (view.starts_with(L"\\\\?\\")) view.remove_prefix(4);
    else if (view.starts_with(L"\\\\")) {
        view.remove_prefix(2);
        const auto slash = view.find(L'\\');
        if (slash == std::wstring_view::npos) return true;
        return view.find(L'\\', slash + 1) == std::wstring_view::npos;
    }
    return view.size() == 2 && view[1] == L':';
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

// Long paths get the \\?\ form so Win32 calls accept deep trees. Short
// paths and paths already in that form pass through unchanged.
std::wstring LongPathForApi(const std::wstring& path) {
    if (path.size() < 240 || path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring DisplayPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

const wchar_t* OpVerb(OpType t) {
    switch (t) {
    case OpType::Copy: return l10n::Get(l10n::StringId::TrayIntentCopy).c_str();
    case OpType::Move: return l10n::Get(l10n::StringId::TrayIntentMove).c_str();
    case OpType::RecycleDelete: return l10n::Get(l10n::StringId::Delete).c_str();
    case OpType::RealDelete: return l10n::Get(l10n::StringId::PermanentlyDelete).c_str();
    case OpType::Rename: return l10n::Get(l10n::StringId::Rename).c_str();
    case OpType::CreateFolder: return l10n::Get(l10n::StringId::NewFolder).c_str();
    case OpType::CreateTextFile: return l10n::Get(l10n::StringId::NewTextDocument).c_str();
    case OpType::RestoreRecycle: return l10n::Get(l10n::StringId::Restore).c_str();
    case OpType::EmptyRecycle: return l10n::Get(l10n::StringId::EmptyRecycleConfirmTitle).c_str();
    case OpType::BatchRename: return l10n::Get(l10n::StringId::BatchRename).c_str();
    }
    return l10n::Get(l10n::StringId::Operation).c_str();
}

// English progress wording ("Copying 3 items"); Chinese keeps 正在 + OpVerb.
const wchar_t* OpGerund(OpType t) {
    switch (t) {
    case OpType::Copy: return L"Copying";
    case OpType::Move: return L"Moving";
    case OpType::RecycleDelete: return L"Deleting";
    case OpType::RealDelete: return L"Permanently deleting";
    case OpType::Rename: return L"Renaming";
    case OpType::CreateFolder: return L"Creating folder";
    case OpType::CreateTextFile: return L"Creating text document";
    case OpType::RestoreRecycle: return L"Restoring";
    case OpType::EmptyRecycle: return L"Emptying Recycle Bin";
    case OpType::BatchRename: return L"Batch renaming";
    }
    return L"Working";
}

std::wstring ProgressSummary(OpType t, size_t count) {
    if (l10n::IsChinese())
        return std::wstring(l10n::Cn(L"正在")) + OpVerb(t) + L" " + std::to_wstring(count) +
               l10n::Cn(L" 个项目");
    return std::wstring(OpGerund(t)) + L" " + std::to_wstring(count) + (count == 1 ? L" item" : L" items");
}

std::wstring LowerFirst(std::wstring text) {
    if (!text.empty() && text[0] >= L'A' && text[0] <= L'Z') text[0] = static_cast<wchar_t>(text[0] - L'A' + L'a');
    return text;
}

std::wstring Describe(const OpRequest& r) {
    std::wstring s = OpVerb(r.type);
    // 新建 entries other than a text document: "新建 新建 Word 文档.docx" would
    // repeat itself, and "新建文本文档" would be wrong.
    if (r.type == OpType::CreateTextFile && !r.sources.empty()) {
        const std::wstring name = FileName(r.sources.front());
        const size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos || _wcsicmp(name.c_str() + dot, L".txt") != 0)
            s = l10n::Get(l10n::StringId::Create).c_str();
    }
    if (r.type == OpType::EmptyRecycle) return s;
    s += L" ";
    if (!r.sources.empty()) s += r.display_name.empty() ? FileName(r.sources.front()) : r.display_name;
    if (r.sources.size() > 1) {
        wchar_t buf[32];
        swprintf_s(buf, l10n::Get(l10n::StringId::ZuItems).c_str(), r.sources.size());
        s += buf;
    }
    if (r.type == OpType::Copy || r.type == OpType::Move) {
        s += L" → " + DisplayPath(r.dest_dir);
    } else if (r.type == OpType::Rename) {
        s += L" → " + r.new_name;
    } else if (r.type == OpType::BatchRename && r.sources.size() > 1) {
        wchar_t buf[32];
        swprintf_s(buf, l10n::Get(l10n::StringId::ZuItems_2).c_str(), r.sources.size());
        s += buf;
    }
    return s;
}

// In-place rename avoids the Shell IPC round trip. Failed renames report their
// error directly: the current Shell host suppresses confirmation and can
// overwrite an existing target, so it is not a safe rename fallback.
bool IsRenameComponent(const std::wstring& name) {
    return IsRenameFilename(name);
}

std::wstring Win32Message(DWORD code);

// Says why a name is refused instead of only "invalid".
std::wstring RenameNameText(const std::wstring& name) {
    switch (RenameFilenameProblem(name)) {
    case RenameFilenameIssue::Empty:
        return l10n::Get(l10n::StringId::TheNameCanTBeEmpty).c_str();
    case RenameFilenameIssue::InvalidCharacter:
        return l10n::Get(l10n::StringId::ANameCanTContainAnyOfTheseCharacters).c_str();
    case RenameFilenameIssue::TrailingDotOrSpace:
        return l10n::Get(l10n::StringId::ANameCanTEndWithASpaceOrAPeriod).c_str();
    case RenameFilenameIssue::Reserved:
        return l10n::Get(l10n::StringId::WindowsReservesThisNameLikeCONNULOrCOM1ChooseADifferentName).c_str();
    case RenameFilenameIssue::None:
        break;
    }
    return l10n::Get(l10n::StringId::InvalidName_2).c_str();
}

// The reason a rename failed, in the user's words; never a bare error number.
// "目标名称已存在" stays the key the UI maps to its own localized text.
std::wstring RenameFailureText(DWORD code) {
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_ELEVATION_REQUIRED:
        return l10n::Get(l10n::StringId::YouMayNeedAdministratorPermissionOrAnotherProgramMayBeUsingT).c_str();
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return l10n::Get(l10n::StringId::TheItemIsInUseByAnotherProgramCloseItAndTryAgain).c_str();
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return l10n::Get(l10n::StringId::TheItemCanTBeFoundItMayHaveBeenMovedRenamedOrDeletedRefreshA).c_str();
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return l10n::Get(l10n::StringId::TheTargetNameAlreadyExists).c_str();
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_DIRECTORY:
        return l10n::Get(l10n::StringId::ThisLocationDoesnTAcceptThatNameChooseADifferentName).c_str();
    case ERROR_FILENAME_EXCED_RANGE:
        return l10n::Get(l10n::StringId::TheNameIsTooLongOrTheFullPathExceedsTheSystemLimitUseAShorte).c_str();
    case ERROR_WRITE_PROTECT:
        return l10n::Get(l10n::StringId::TheDiskIsWriteProtected).c_str();
    case ERROR_NOT_READY:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_BAD_NETPATH:
    case ERROR_NETNAME_DELETED:
    case ERROR_UNEXP_NET_ERR:
    case ERROR_NETWORK_UNREACHABLE:
        return l10n::Get(l10n::StringId::ThisLocationIsnTAvailableTheDeviceMayBeDisconnectedOrTheNetw).c_str();
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return l10n::Get(l10n::StringId::ThereIsnTEnoughDiskSpace).c_str();
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        return l10n::Get(l10n::StringId::ThisLocationDoesnTSupportRenaming).c_str();
    case ERROR_CANT_ACCESS_FILE:
#ifdef ERROR_CLOUD_FILE_PROVIDER_NOT_RUNNING
    case ERROR_CLOUD_FILE_PROVIDER_NOT_RUNNING:
#endif
#ifdef ERROR_CLOUD_FILE_IN_USE
    case ERROR_CLOUD_FILE_IN_USE:
#endif
#ifdef ERROR_CLOUD_FILE_UNSUCCESSFUL
    case ERROR_CLOUD_FILE_UNSUCCESSFUL:
#endif
        return l10n::Get(l10n::StringId::ThisCloudFileCanTBeChangedRightNowMakeSureTheSyncAppIsRunnin).c_str();
    default:
        // The system's own sentence, in the system language, without the number.
        return Win32Message(code);
    }
}

// Why one item of a copy or move failed, in the user's words, without the
// path (the dialog shows the name beside it).
std::wstring TransferFailureText(HRESULT hr) {
    const DWORD code = HRESULT_CODE(hr);
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_ELEVATION_REQUIRED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_CANT_ACCESS_FILE:
        if (std::wstring text = RenameFailureText(code); !text.empty()) return text;
        break;
    default:
        break;
    }
    return Win32Message(code);
}

// A catalog prefix such as "无法创建目标目录：" as a reason on its own,
// optionally followed by the system's detail.
std::wstring FailureReason(const std::wstring& prefix, const std::wstring& detail = {}) {
    std::wstring reason = prefix;
    while (!reason.empty() && (reason.back() == L' ' || reason.back() == L'|' ||
                               reason.back() == L':' || reason.back() == L'\xFF1A'))
        reason.pop_back();
    if (reason.empty()) return detail;
    if (!detail.empty()) reason += (l10n::IsChinese() ? L"\xFF1A" : L": ") + detail;
    return reason;
}

// An ops error text ("message | path", or a catalog prefix with the path) as
// a reason without the path.
std::wstring ErrorWithoutPath(const std::wstring& error) {
    const size_t bar = error.rfind(L" | ");
    const std::wstring folder = l10n::Get(l10n::StringId::CouldNotCreateTheDestinationFolder).c_str();
    if (error.starts_with(folder)) {
        const std::wstring not_folder = l10n::Get(l10n::StringId::TheTargetExistsAndIsNotAFolder).c_str();
        std::wstring detail = bar != std::wstring::npos ? error.substr(bar + 3)
            : error.ends_with(not_folder) ? FailureReason(not_folder) : std::wstring{};
        while (!detail.empty() && (detail.front() == L' ' || detail.front() == L'(' ||
                                   detail.front() == L'\xFF08' || detail.front() == L'\xFF0C'))
            detail.erase(detail.begin());
        while (!detail.empty() && (detail.back() == L')' || detail.back() == L'\xFF09'))
            detail.pop_back();
        return FailureReason(folder, detail);
    }
    return bar == std::wstring::npos ? error : error.substr(0, bar);
}

// Why a new folder or text file could not be created. "没有权限" is the key
// the UI maps to its write-permission message.
std::wstring CreateFailureText(DWORD code) {
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_ELEVATION_REQUIRED:
        return l10n::Get(l10n::StringId::YouDonTHavePermissionToCreateItemsHere).c_str();
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return l10n::Get(l10n::StringId::ThisFolderNoLongerExistsItMayHaveBeenMovedOrDeletedRefreshAn).c_str();
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return l10n::Get(l10n::StringId::AnItemWithThisNameAlreadyExists).c_str();
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        return l10n::Get(l10n::StringId::ThisLocationDoesnTSupportCreatingItems).c_str();
    default:
        return RenameFailureText(code);
    }
}

// New folder / new text file: the same plain Win32 creation the Shell host
// ran, done here so a busy, restarting or unreachable host can never leave a
// new folder waiting with no progress. The UI already picked a unique name.
// 新建 entries (ShellNew) reuse the text-file path: a template file is copied
// (CopyFile fails rather than overwrite), Data bytes are written, else the
// file stays empty like NullFile.
bool CreateInProcess(const OpRequest& req, std::wstring* error) {
    const std::wstring path = req.sources.empty() ? std::wstring{} : req.sources.front();
    DWORD code = ERROR_SUCCESS;
    if (path.empty()) {
        code = ERROR_INVALID_NAME;
    } else if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        code = ERROR_FILE_EXISTS;
    } else if (req.type == OpType::CreateFolder) {
        if (!CreateDirectoryW(path.c_str(), nullptr)) code = GetLastError();
    } else if (!req.template_file.empty()) {
        if (!CopyFileW(req.template_file.c_str(), path.c_str(), TRUE)) {
            code = GetLastError();
        } else {
            // Templates under Program Files are often read-only, and CopyFile
            // keeps the template's date; a new file is read-write and new.
            const DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
                SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
            HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                FILETIME now{};
                GetSystemTimeAsFileTime(&now);
                SetFileTime(file, &now, &now, &now);
                CloseHandle(file);
            }
        }
    } else {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            code = GetLastError();
        } else {
            DWORD written = 0;
            const bool ok = req.template_data.empty() ||
                (WriteFile(file, req.template_data.data(), static_cast<DWORD>(req.template_data.size()),
                           &written, nullptr) && written == req.template_data.size());
            if (!ok) code = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            CloseHandle(file);
            if (!ok) DeleteFileW(path.c_str());
        }
    }
    if (code != ERROR_SUCCESS && error) *error = CreateFailureText(code);
    return code == ERROR_SUCCESS;
}

enum class RenameResult { Completed, Rejected };
RenameResult RenameInProcess(const std::wstring& source, const std::wstring& new_name,
                     std::wstring* error, DWORD* code = nullptr) {
    if (code) *code = ERROR_SUCCESS;
    if (source.empty() || !IsRenameComponent(new_name)) {
        if (error) *error = source.empty() ? l10n::Get(l10n::StringId::InvalidName_2).c_str() : RenameNameText(new_name);
        return RenameResult::Rejected;
    }
    const std::wstring target = JoinPath(ParentOf(source), new_name);
    if (target.empty()) {
        if (error) *error = l10n::Get(l10n::StringId::InvalidName_2).c_str();
        return RenameResult::Rejected;
    }
    // The current shell host suppresses confirmation UI, so sending an existing
    // target there could silently overwrite it. Reject that conflict here.
    if (CompareStringOrdinal(source.c_str(), -1, target.c_str(), -1, TRUE) != CSTR_EQUAL &&
        GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (error) *error = l10n::Get(l10n::StringId::TheTargetNameAlreadyExists).c_str();
        return RenameResult::Rejected;
    }
    if (!MoveFileW(source.c_str(), target.c_str())) {
        const DWORD failure = GetLastError();
        if (code) *code = failure;
        if (error) *error = RenameFailureText(failure);
        return RenameResult::Rejected;
    }
    return RenameResult::Completed;
}

// A drive root ("C:\", "\\?\C:\") as "C:\".
bool DriveRootOf(const std::wstring& path, std::wstring& root) {
    const std::wstring plain = path::StripExtendedPathPrefix(path);
    if (plain.size() < 2 || plain.size() > 3 || plain[1] != L':' || !std::iswalpha(plain[0])) return false;
    if (plain.size() == 3 && plain[2] != L'\\' && plain[2] != L'/') return false;
    root = { static_cast<wchar_t>(std::towupper(plain[0])), L':', L'\\' };
    return true;
}

// This PC rows read "Label (C:)", or "share (\\server) (Z:)" for a network
// drive without a label, and the rename field starts from that text.
std::wstring DriveLabelFromRename(std::wstring name, wchar_t letter) {
    const auto trim = [](std::wstring& text) {
        while (!text.empty() && std::iswspace(text.back())) text.pop_back();
        while (!text.empty() && std::iswspace(text.front())) text.erase(text.begin());
    };
    trim(name);
    size_t open = name.rfind(L" (");
    if (open != std::wstring::npos && name.size() == open + 5 && name[open + 3] == L':' && name.back() == L')' &&
        std::towupper(name[open + 2]) == std::towupper(letter)) {
        name.erase(open);
        trim(name);
    }
    open = name.rfind(L" (\\\\");
    if (open != std::wstring::npos && !name.empty() && name.back() == L')') {
        name.erase(open);
        trim(name);
    }
    return name;
}

// Drives are renamed by label, as in File Explorer (#208): This PC's own
// SetNameOf sets the volume label (asking for elevation the way Explorer
// does) or the Shell's label of a mapped network drive. A drive root cannot
// be moved, so the file rename path always failed for it.
bool RenameDriveLabel(const std::wstring& root, const std::wstring& typed, HWND owner, std::wstring* error) {
    const std::wstring label = DriveLabelFromRename(typed, root.front());
    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHParseDisplayName(root.c_str(), nullptr, &pidl, 0, nullptr);
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(hr)) hr = SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child);
    if (SUCCEEDED(hr)) {
        PITEMID_CHILD renamed = nullptr;
        hr = parent->SetNameOf(owner, child, label.c_str(), SHGDN_NORMAL, &renamed);
        if (renamed) CoTaskMemFree(renamed);
        parent->Release();
    }
    if (pidl) CoTaskMemFree(pidl);
    if (SUCCEEDED(hr)) return true;
    if (error) {
        *error = hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? l10n::Get(l10n::StringId::Canceled).c_str()
            : RenameFailureText(HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE);
    }
    return false;
}

struct TransferEntry {
    std::wstring source;
    std::wstring destination;
    bool directory = false;
    bool reparse = false;
    bool destination_preexisting = false;
    bool directory_prepared = false;
    bool renamed = false;  // moved by a same-volume rename while scanning
    // Filled in as items finish, possibly out of order on the copy pool; the
    // completed lists are built from these in entry order afterwards.
    bool completed = false;
    bool source_removed = false;
    bool deferred = false;  // a large file moved behind the small ones
    uint64_t bytes = 0;
    DWORD attributes = FILE_ATTRIBUTE_NORMAL;
    FILETIME created{};
    FILETIME accessed{};
    FILETIME modified{};
};

struct CopyProgressContext {
    std::atomic<bool>* cancel = nullptr;
    std::atomic<bool>* pause = nullptr;
    std::function<void(uint64_t, uint64_t)> report;
    bool pause_sent = false;
    ULONGLONG last_report = 0;
};

COPYFILE2_MESSAGE_ACTION CALLBACK CopyProgress(const COPYFILE2_MESSAGE* message,
                                               void* raw) {
    auto* ctx = static_cast<CopyProgressContext*>(raw);
    if (!ctx || !message) return COPYFILE2_PROGRESS_CANCEL;
    uint64_t transferred = 0;
    uint64_t total = 0;
    // Only these carry byte counts. The start messages report 0 of 0, which
    // must neither publish nor restart the throttle: a chunk-started before
    // every chunk-finished kept the bar at 0% for the whole file.
    bool counted = true;
    switch (message->Type) {
    case COPYFILE2_CALLBACK_CHUNK_FINISHED:
        transferred = message->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
        total = message->Info.ChunkFinished.uliTotalFileSize.QuadPart;
        break;
    case COPYFILE2_CALLBACK_STREAM_FINISHED:
        transferred = message->Info.StreamFinished.uliTotalBytesTransferred.QuadPart;
        total = message->Info.StreamFinished.uliTotalFileSize.QuadPart;
        break;
    case COPYFILE2_CALLBACK_ERROR:
        transferred = message->Info.Error.uliTotalBytesTransferred.QuadPart;
        total = message->Info.Error.uliTotalFileSize.QuadPart;
        break;
    default:
        counted = false;
        break;
    }
    const ULONGLONG now = GetTickCount64();
    if (ctx->report && counted &&
        ((total > 0 && transferred == total) || now - ctx->last_report >= 100)) {
        ctx->last_report = now;
        ctx->report(transferred, total);
    }
    if (ctx->cancel && ctx->cancel->load()) return COPYFILE2_PROGRESS_CANCEL;
    if (ctx->pause && ctx->pause->load() && !ctx->pause_sent) {
        ctx->pause_sent = true;
        return COPYFILE2_PROGRESS_PAUSE;
    }
    return COPYFILE2_PROGRESS_CONTINUE;
}

bool ReadEntryMetadata(const std::wstring& path, TransferEntry& entry) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    entry.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    entry.attributes = data.dwFileAttributes;
    entry.created = data.ftCreationTime;
    entry.accessed = data.ftLastAccessTime;
    entry.modified = data.ftLastWriteTime;
    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    entry.bytes = entry.directory ? 0 : size.QuadPart;
    return true;
}

bool PathExists(const std::wstring& path, bool* directory = nullptr) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (directory) *directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

std::wstring Win32Message(DWORD code) {
    wchar_t* raw = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring text = raw ? raw : l10n::Get(l10n::StringId::FileOperationFailed).c_str();
    if (raw) LocalFree(raw);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    return text;
}

bool EnsureDirectories(const std::wstring& dir, std::wstring& failure) {
    if (dir.empty() || IsVolumeRoot(dir)) return true;
    bool is_directory = false;
    if (PathExists(dir, &is_directory)) {
        if (is_directory) return true;
        failure = l10n::Get(l10n::StringId::CouldNotCreateTheDestinationFolder).c_str() + dir + l10n::Get(l10n::StringId::TheTargetExistsAndIsNotAFolder).c_str();
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(dir), error);
    if (error) {
        failure = l10n::Get(l10n::StringId::CouldNotCreateTheDestinationFolder).c_str() + dir + L" | " + Win32Message(error.value());
        return false;
    }
    return true;
}

std::wstring UniqueCopyPath(const std::wstring& destination, bool directory) {
    if (!PathExists(destination)) return destination;
    const std::wstring parent = ParentOf(destination);
    const std::wstring leaf = FileName(destination);
    std::wstring stem = leaf;
    std::wstring extension;
    if (!directory) {
        const size_t dot = leaf.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) {
            stem = leaf.substr(0, dot);
            extension = leaf.substr(dot);
        }
    }
    for (unsigned index = 1; index < 10000; ++index) {
        // Same suffix as File Explorer in the UI language.
        std::wstring name = stem + l10n::Get(l10n::StringId::Copy_2).c_str();
        if (index > 1) name += L" (" + std::to_wstring(index) + L")";
        std::wstring candidate = JoinPath(parent, name + extension);
        if (!PathExists(candidate)) return candidate;
    }
    return JoinPath(parent, stem + l10n::Get(l10n::StringId::Copy_3).c_str() + std::to_wstring(GetTickCount64()) + extension);
}

// Files below this size are copied straight to a free final name (#60). The
// temporary name costs one extra create and rename per file, which dominates
// batches of small files; large files keep it so a crash mid-copy cannot
// leave a long partial file under the real name.
constexpr uint64_t kDirectCopyMaxBytes = 64ull << 20;

std::wstring UniqueTemporaryPath(const std::wstring& destination,
                                 const wchar_t* marker,
                                 uint64_t task_id,
                                 size_t index) {
    const std::wstring base = destination + marker + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(task_id) + L"-" + std::to_wstring(index);
    if (!PathExists(base)) return base;
    for (unsigned suffix = 2; suffix < 10000; ++suffix) {
        const std::wstring candidate = base + L"-" + std::to_wstring(suffix);
        if (!PathExists(candidate)) return candidate;
    }
    return base + L"-" + std::to_wstring(GetTickCount64());
}

bool SetCopiedDirectoryMetadata(const TransferEntry& entry) {
    HANDLE handle = CreateFileW(entry.destination.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const BOOL times = SetFileTime(handle, &entry.created, &entry.accessed, &entry.modified);
    CloseHandle(handle);
    DWORD attrs = entry.attributes & ~(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY);
    if (attrs == 0) attrs = FILE_ATTRIBUTE_NORMAL;
    SetFileAttributesW(entry.destination.c_str(), attrs);
    return times != FALSE;
}

bool CopyReparsePoint(const TransferEntry& entry, std::wstring& error) {
    HANDLE source = CreateFileW(entry.source.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        error = Win32Message(GetLastError()) + L" | " + entry.source;
        return false;
    }
    std::vector<BYTE> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD bytes = 0;
    const BOOL read = DeviceIoControl(source, FSCTL_GET_REPARSE_POINT, nullptr, 0,
        buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr);
    CloseHandle(source);
    if (!read) {
        error = Win32Message(GetLastError()) + L" | " + entry.source;
        return false;
    }

    if (entry.directory) {
        if (!CreateDirectoryW(entry.destination.c_str(), nullptr)) {
            error = Win32Message(GetLastError()) + L" | " + entry.destination;
            return false;
        }
    } else {
        HANDLE placeholder = CreateFileW(entry.destination.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (placeholder == INVALID_HANDLE_VALUE) {
            error = Win32Message(GetLastError()) + L" | " + entry.destination;
            return false;
        }
        CloseHandle(placeholder);
    }

    HANDLE destination = CreateFileW(entry.destination.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (destination == INVALID_HANDLE_VALUE) {
        error = Win32Message(GetLastError()) + L" | " + entry.destination;
        if (entry.directory) RemoveDirectoryW(entry.destination.c_str());
        else DeleteFileW(entry.destination.c_str());
        return false;
    }
    DWORD written = 0;
    const BOOL set = DeviceIoControl(destination, FSCTL_SET_REPARSE_POINT,
        buffer.data(), bytes, nullptr, 0, &written, nullptr);
    if (set) SetFileTime(destination, &entry.created, &entry.accessed, &entry.modified);
    CloseHandle(destination);
    if (!set) {
        error = Win32Message(GetLastError()) + L" | " + entry.destination;
        if (entry.directory) RemoveDirectoryW(entry.destination.c_str());
        else DeleteFileW(entry.destination.c_str());
        return false;
    }
    DWORD attributes = entry.attributes & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
    if (attributes == 0) attributes = FILE_ATTRIBUTE_NORMAL;
    SetFileAttributesW(entry.destination.c_str(), attributes);
    return true;
}

bool StartsWithPath(const std::wstring& path, const std::wstring& prefix) {
    if (path.size() < prefix.size() || _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0)
        return false;
    return path.size() == prefix.size() || path[prefix.size()] == L'\\' || path[prefix.size()] == L'/';
}

bool Sha256File(const std::wstring& path, const std::atomic<bool>& cancel,
                const std::function<void()>& wait_if_paused, std::array<uint8_t, 32>& digest,
                std::wstring& error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_bytes = 0;
    DWORD hash_bytes = 0;
    DWORD returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
                          &returned, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hash_bytes), sizeof(hash_bytes),
                          &returned, 0) < 0 || hash_bytes != digest.size()) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        error = l10n::Get(l10n::StringId::CouldNotInitializeSHA256Verification).c_str();
        return false;
    }
    std::vector<uint8_t> object(object_bytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_bytes,
                         nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = l10n::Get(l10n::StringId::CouldNotInitializeSHA256Verification).c_str();
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    std::vector<uint8_t> buffer(1024 * 1024);
    while (ok && !cancel.load()) {
        wait_if_paused();
        if (cancel.load()) break;
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0) {
            ok = false;
            break;
        }
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (ok && !cancel.load() && BCryptFinishHash(hash, digest.data(),
                                                 static_cast<ULONG>(digest.size()), 0) < 0)
        ok = false;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok && error.empty()) error = Win32Message(GetLastError()) + L" | " + path;
    return ok && !cancel.load();
}

std::wstring JsonString(const std::wstring& value) {
    std::wstring escaped;
    json::Escape(value, escaped);
    return L"\"" + escaped + L"\"";
}

std::wstring StringArrayJson(const std::vector<std::wstring>& values) {
    std::wstring out = L"[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += L",";
        out += JsonString(values[i]);
    }
    out += L"]";
    return out;
}

std::vector<std::wstring> ExtractObjectArray(const std::wstring& input,
                                             const std::wstring& key) {
    std::vector<std::wstring> result;
    size_t pos = json::ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size() || input[pos] != L'[') return result;
    ++pos;
    while (pos < input.size()) {
        json::SkipWhitespace(input, pos);
        if (pos >= input.size() || input[pos] == L']') break;
        if (input[pos] == L',') { ++pos; continue; }
        if (input[pos] != L'{') return {};
        const size_t start = pos++;
        int depth = 1;
        bool quoted = false;
        bool escaped = false;
        while (pos < input.size() && depth > 0) {
            const wchar_t c = input[pos++];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == L'\\') escaped = true;
                else if (c == L'\"') quoted = false;
                continue;
            }
            if (c == L'\"') quoted = true;
            else if (c == L'{') ++depth;
            else if (c == L'}') --depth;
        }
        if (depth != 0) return {};
        result.push_back(input.substr(start, pos - start));
    }
    return result;
}

bool ParseRecoveryEntry(const std::wstring& object, RecoveryEntry& entry) {
    const int type = json::ExtractInt(object, L"type", -1);
    const int policy = json::ExtractInt(object, L"policy", -1);
    if (type < static_cast<int>(OpType::Copy) || type > static_cast<int>(OpType::BatchRename) ||
        policy < static_cast<int>(CollisionPolicy::System) ||
        policy > static_cast<int>(CollisionPolicy::KeepBoth)) return false;
    entry.sequence = _wcstoui64(json::ExtractString(object, L"seq", L"0").c_str(), nullptr, 10);
    entry.was_active = json::ExtractBool(object, L"active", false);
    entry.request.type = static_cast<OpType>(type);
    entry.request.collision_policy = static_cast<CollisionPolicy>(policy);
    entry.request.sources = json::ExtractStringArray(object, L"sources");
    entry.request.dest_dir = json::ExtractString(object, L"dest");
    entry.request.new_name = json::ExtractString(object, L"name");
    entry.request.new_names = json::ExtractStringArray(object, L"names");
    entry.request.is_undo = json::ExtractBool(object, L"undo", false);
    entry.request.duplicate_cleanup = json::ExtractBool(object, L"duplicate_cleanup", false);
    entry.request.only_if_empty = json::ExtractBool(object, L"only_if_empty", false);
    if (entry.request.type == OpType::EmptyRecycle) return entry.sequence != 0;
    return entry.sequence != 0 && !entry.request.sources.empty();
}

void ReconcileTemporaryFiles(const std::wstring& root) {
    // Schema 1 journals do not record artifact identities. A name alone cannot
    // establish ownership; preserve both user files and interrupted backups.
    (void)root;
}

bool SameFileObject(const std::wstring& left, const std::wstring& right) {
    std::error_code error;
    return std::filesystem::equivalent(left, right, error) && !error;
}

// Folder that contains `path`, for ShellExecuteEx's lpDirectory. Empty for
// shell namespace paths or when the parent is not an existing directory.
// Runs on the open thread (the parent may be on a slow network share).
std::wstring OpenItemWorkingDirectory(const std::wstring& path) {
    const std::wstring plain = pulse::path::StripExtendedPathPrefix(path);
    if (plain.size() < 3 || plain.rfind(L"::", 0) == 0) return {};
    const size_t slash = plain.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return {};
    std::wstring dir = plain.substr(0, slash);
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\'; // drive root: "C:" must become "C:\\"
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return {};
    return dir;
}

// Undo history keeps the newest kMaxUndoSteps steps, so session.json stops
// growing. A step is one entry, or a run of entries sharing a nonzero group
// (undone together); trimming drops whole steps from the oldest end.
constexpr size_t kMaxUndoSteps = 100;

void TrimUndoHistory(std::deque<UndoEntry>& undo) {
    size_t steps = 0;
    for (size_t i = 0; i < undo.size(); ++i)
        if (i == 0 || undo[i].group == 0 || undo[i].group != undo[i - 1].group) ++steps;
    while (steps > kMaxUndoSteps && !undo.empty()) {
        const uint64_t group = undo.front().group;
        do {
            undo.pop_front();
        } while (group != 0 && !undo.empty() && undo.front().group == group);
        --steps;
    }
}

} // namespace

// See ops_manager.h.
bool IsStopTransferError(HRESULT hr) {
    if (hr == E_OUTOFMEMORY) return true;
    // The Shell's copy engine codes share their low word with unrelated
    // Win32 codes: COPYENGINE_E_SHARING_VIOLATION_SRC (0x80270027) would read
    // as ERROR_HANDLE_DISK_FULL and stop a whole task over one file in use.
    if (HRESULT_FACILITY(hr) == FACILITY_SHELL) {
        switch (static_cast<uint32_t>(hr)) {
        case 0x8027000Eu:  // COPYENGINE_E_REMOVABLE_FULL
        case 0x80270025u:  // COPYENGINE_E_NET_DISCONNECT_SRC
        case 0x80270026u:  // COPYENGINE_E_NET_DISCONNECT_DEST
        case 0x80270032u:  // COPYENGINE_E_DISK_FULL
        case 0x80270033u:  // COPYENGINE_E_DISK_FULL_CLEAN
            return true;
        default:
            return false;
        }
    }
    // Win32 codes, as HRESULTs or bare; STG_E_* mirror them on purpose.
    if ((static_cast<uint32_t>(hr) & 0xFFFF0000u) != 0 && HRESULT_FACILITY(hr) != FACILITY_WIN32 &&
        HRESULT_FACILITY(hr) != FACILITY_STORAGE)
        return false;
    switch (HRESULT_CODE(hr)) {
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_QUOTA_EXCEEDED:
    case ERROR_NOT_ENOUGH_QUOTA:
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_NOT_READY:
    case ERROR_WRITE_PROTECT:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_DEVICE_REMOVED:
    case ERROR_NO_SUCH_DEVICE:
    case ERROR_NO_MEDIA_IN_DRIVE:
    case ERROR_MEDIA_CHANGED:
    case ERROR_IO_DEVICE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_NETNAME_DELETED:
    case ERROR_UNEXP_NET_ERR:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_SEM_TIMEOUT:
    case ERROR_OPERATION_ABORTED:
    case ERROR_REQUEST_ABORTED:
    case ERROR_CANCELLED:
        return true;
    default:
        return false;
    }
}

OpsManager::~OpsManager() {
    Stop();
}

void OpsManager::SetJournalPath(std::wstring path) {
    if (running_) return;
    journal_path_ = std::move(path);
    LoadRecoveryJournal();
}

void OpsManager::LoadRecoveryJournal() {
    pending_recovery_.clear();
    if (journal_path_.empty()) return;
    std::wstring text;
    if (!ReadUtf8File(journal_path_, text) || json::ExtractInt(text, L"schema", 0) != 1) return;
    for (const auto& object : ExtractObjectArray(text, L"entries")) {
        RecoveryEntry entry;
        if (ParseRecoveryEntry(object, entry)) pending_recovery_.push_back(std::move(entry));
    }
}

RecoverySnapshot OpsManager::PendingRecovery() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RecoverySnapshot result;
    result.entries = pending_recovery_;
    result.has_uncertain_destructive = std::any_of(
        result.entries.begin(), result.entries.end(), [](const RecoveryEntry& entry) {
            return entry.was_active && entry.request.type == OpType::RealDelete;
        });
    return result;
}

bool OpsManager::RetryRecovery() {
    bool all_retryable = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (update_preparing_) return false;
        for (auto& recovery : pending_recovery_) {
            if (!recovery.request.dest_dir.empty())
                recovery_cleanup_roots_.push_back(recovery.request.dest_dir);
            if (recovery.request.type == OpType::RealDelete || recovery.request.duplicate_cleanup) {
                all_retryable = false;
                continue;
            }
            QueueItem item;
            item.seq = next_seq_++;
            item.req = std::move(recovery.request);
            queue_.push_back(std::move(item));
        }
        pending_recovery_.clear();
    }
    cv_.notify_one();
    if (notify_) notify_();
    return all_retryable;
}

void OpsManager::DiscardRecovery() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (update_preparing_) return;
        for (const auto& recovery : pending_recovery_) {
            if (!recovery.request.dest_dir.empty())
                recovery_cleanup_roots_.push_back(recovery.request.dest_dir);
        }
        pending_recovery_.clear();
    }
    if (!journal_path_.empty()) DeleteFileW(journal_path_.c_str());
    cv_.notify_one();
}

std::wstring OpsManager::JournalJsonLocked() const {
    std::wstring out = L"{\n  \"schema\":1,\n  \"entries\":[";
    bool first = true;
    auto append = [&](const QueueItem& item, bool active) {
        if (item.req.sources.empty() && item.req.type != OpType::EmptyRecycle) return;
        // A phone operation is not resumed after a restart: the device may be
        // gone, and a half-done copy is left for the user to look at.
        if (IsShellTransferRequest(item.req)) return;
        if (!first) out += L",";
        first = false;
        out += L"\n    {\"seq\":" + JsonString(std::to_wstring(item.seq)) +
            L",\"active\":" + (active ? std::wstring(L"true") : std::wstring(L"false")) +
            L",\"type\":" + std::to_wstring(static_cast<int>(item.req.type)) +
            L",\"policy\":" + std::to_wstring(static_cast<int>(item.req.collision_policy)) +
            L",\"undo\":" + (item.req.is_undo ? std::wstring(L"true") : std::wstring(L"false")) +
            L",\"duplicate_cleanup\":" + (item.req.duplicate_cleanup ? std::wstring(L"true") : std::wstring(L"false")) +
            L",\"sources\":" + StringArrayJson(item.req.sources) +
            L",\"dest\":" + JsonString(item.req.dest_dir) +
            L",\"name\":" + JsonString(item.req.new_name) +
            L",\"names\":" + StringArrayJson(item.req.new_names) +
            (item.req.only_if_empty ? std::wstring(L",\"only_if_empty\":true") : std::wstring()) + L"}";
    };
    if (active_item_) append(*active_item_, true);
    for (const auto& item : queue_) append(item, false);
    out += first ? L"]\n}\n" : L"\n  ]\n}\n";
    return out;
}

void OpsManager::PersistJournal() {
    if (journal_path_.empty()) return;
    std::wstring text;
    bool empty = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        empty = !active_item_ && queue_.empty();
        if (!empty) text = JournalJsonLocked();
    }
    if (empty) DeleteFileW(journal_path_.c_str());
    else WriteUtf8FileAtomic(journal_path_, text);
}

void OpsManager::Start(std::function<void()> notify, bool own_shell_client) {
    if (running_) return;
    own_shell_client_ = own_shell_client;
    notify_ = std::move(notify);
    stopping_ = false;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        menu_running_ = own_shell_client_;
    }
    thread_ = std::thread([this] { WorkerThread(); });
#ifndef PULSE_ELEVATED_HOST
    if (own_shell_client_) menu_thread_ = std::thread([this] { MenuThread(); });
    {
        std::lock_guard<std::mutex> lock(open_mutex_);
        open_running_ = own_shell_client_;
    }
    if (own_shell_client_) open_thread_ = std::thread([this] { OpenThread(); });
#endif
}

void OpsManager::Stop() {
    if (!running_) return;
    stopping_ = true;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        menu_running_ = false;
    }
    menu_cv_.notify_all();
    if (menu_thread_.joinable()) menu_thread_.join();
    {
        std::lock_guard<std::mutex> lock(open_mutex_);
        open_running_ = false;
    }
    open_cv_.notify_all();
    if (open_thread_.joinable()) open_thread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    CancelCurrent(); // unblock an in-flight RunShellOp via DONE(cancelled)
    cv_.notify_all();
    done_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : queue_)
            if (item.open_path.empty()) finished_tasks_.push_back({item.seq, {}});
    }
    if (notify_) notify_();
    {
        std::scoped_lock lock(done_mutex_, menu_mutex_);
        shell_request_ids_.clear(); done_results_.clear();
        menu_queue_.clear(); menu_session_by_token_.clear(); menu_token_by_session_.clear();
        ctx_invoke_ids_.clear(); menu_dispatching_ = false;
    }
    PersistJournal();
    // Queued requests were cancelled above. Preserve their recovery journal,
    // but never execute an already-finished task if this manager is restarted.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
}

bool OpsManager::TryPrepareForUpdate() {
    std::scoped_lock lock(mutex_, menu_mutex_);
    if (update_preparing_ || active_item_ || !queue_.empty() || status_.active ||
        recovery_cleanup_active_ || !recovery_cleanup_roots_.empty() ||
        menu_dispatching_ || !menu_queue_.empty() || !ctx_invoke_ids_.empty()) return false;
    update_preparing_ = true;
    return true;
}

void OpsManager::CancelUpdatePreparation() {
    std::lock_guard<std::mutex> lock(mutex_);
    update_preparing_ = false;
}

uint64_t OpsManager::Submit(OpRequest req) {
    if (!own_shell_client_ && req.type != OpType::Copy && req.type != OpType::Move &&
        req.type != OpType::Rename) return 0;
    QueueItem item;
    item.req = std::move(req);
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (update_preparing_) return 0;
        seq = next_seq_++;
        item.seq = seq;
        queue_.push_back(std::move(item));
    }
    if (notify_) notify_();
    cv_.notify_one();
    return seq;
}

void OpsManager::OpenWith(const std::wstring& path) {
    QueueItem item;
    item.open_path = path;
    // Some registered defaults expose "edit" or a delegated default action.
    // Leave the verb empty so Windows selects the association's default.
    EnqueueOpen(std::move(item));
}

void OpsManager::ShowProperties(const std::wstring& path) {
    ExecuteVerb(path, L"properties");
}

void OpsManager::ShowProperties(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    if (paths.size() == 1) {
        ShowProperties(paths.front());
        return;
    }
    QueueItem item;
    item.open_path = paths.front();
    item.open_paths = paths;
    item.open_verb = L"properties";
    EnqueueOpen(std::move(item));
}

void OpsManager::ExecuteVerb(const std::wstring& path, const std::wstring& verb) {
    QueueItem item;
    item.open_path = path;
    item.open_verb = verb;
    // 打开方式… / 属性 are interactive dialogs: they must answer the click even
    // when a slow open is still in flight, so they go to the front of the queue.
    EnqueueOpen(std::move(item), true);
}

void OpsManager::OpenWithApp(const std::wstring& app_exe, const std::wstring& file) {
    QueueItem item;
    item.open_path = ParentOf(file);      // lpDirectory
    item.open_file = app_exe;
    item.open_verb = L"open";
    item.open_args = L"\"" + pulse::path::StripExtendedPathPrefix(file) + L"\"";
    EnqueueOpen(std::move(item));
}

void OpsManager::ExecuteCommand(const std::wstring& command, const std::wstring& path) {
    std::wstring expanded = ExpandShellCommand(command, path);
    if (expanded.empty()) return;
    QueueItem item;
    item.open_path = ParentOf(path);
    item.open_file = expanded;
    item.open_verb = L"__cmdline";
    EnqueueOpen(std::move(item));
}

void OpsManager::OpenTerminal(const std::wstring& dir, const std::wstring& program) {
    QueueItem item;
    item.open_path = dir;
    item.open_file = program;
    item.open_verb = L"__context_terminal";
    EnqueueOpen(std::move(item));
}

void OpsManager::OpenProgramIn(const std::wstring& exe, const std::wstring& dir,
                               const std::wstring& args) {
    if (exe.empty()) return;
    QueueItem item;
    item.open_path = dir;      // lpDirectory
    item.open_file = exe;
    item.open_verb = L"open";
    if (_wcsicmp(exe.c_str(), L"wt.exe") == 0) {
        item.open_verb = L"__terminal";
        item.open_args = TerminalCommandLine(dir);
        if (!args.empty()) item.open_args += L" " + args;
    } else {
        item.open_args = args;
    }
    EnqueueOpen(std::move(item));
}

void OpsManager::OpenShortcutProgram(const std::wstring& lnk, const std::wstring& program,
                                     const std::wstring& args) {
    if (lnk.empty() || program.empty()) return;
    QueueItem item;
    item.open_path = lnk;      // read on the open thread, then the start-in folder
    item.open_file = program;
    item.open_verb = L"__shortcut";
    item.open_args = args;
    EnqueueOpen(std::move(item));
}

OpsManager::QueueItem* OpsManager::CurrentTransferLocked() {
    QueueItem* item = active_item_ ? &*active_item_ : queue_.empty() ? nullptr : &queue_.front();
    return item && item->open_path.empty() &&
        (item->req.type == OpType::Copy || item->req.type == OpType::Move) ? item : nullptr;
}

void OpsManager::CancelCurrent() {
    bool transfer = false;
    uint64_t task = 0;
    {
        // The same locks protect request-owned state, engine initialization and
        // pause wait predicates. No startup store can erase an accepted command.
        std::scoped_lock lock(mutex_, transfer_control_mutex_);
        auto* current = active_item_ ? &*active_item_ : queue_.empty() ? nullptr : &queue_.front();
        if (current) {
            current->cancel_requested = true;
            shell_cancel_requested_ = true;
        }
        if (auto* item = CurrentTransferLocked()) {
            item->transfer_cancelled = true;
            item->transfer_paused = false;
            task = item->seq;
            transfer = true;
        }
        if (transfer_active_.load()) {
            transfer_cancel_.store(true);
            transfer_pause_.store(false);
            if (!task) task = status_.task_id;
            transfer = true;
        }
    }
    if (transfer) {
        transfer_control_cv_.notify_all();
        SetStatus([&](OpStatus& status) {
            status.pause_requested = false;
            if (status.active && status.task_id == task) status.phase = OpPhase::Cancelling;
        });
    }
    uint32_t id = current_req_id_.load();
    if (id != 0) ipc::ShellClient::Instance().Cancel(id);
}

void OpsManager::PauseCurrent() {
    bool requested = false;
    {
        std::scoped_lock lock(mutex_, transfer_control_mutex_);
        if (auto* item = CurrentTransferLocked(); item && !item->transfer_cancelled)
            item->transfer_paused = true;
        if (transfer_active_.load() && !transfer_cancel_.load()) {
            transfer_pause_.store(true);
            // Shown at once: a buffered write can sit in the cache manager's
            // dirty-page throttle for seconds before the next progress callback
            // reaches the pause check.
            requested = status_.active && !status_.pause_requested;
            if (requested) status_.pause_requested = true;
        }
    }
    if (requested) NotifyStatusChanged();
}

void OpsManager::ResumeCurrent() {
    bool changed = false;
    {
        std::scoped_lock lock(mutex_, transfer_control_mutex_);
        if (auto* item = CurrentTransferLocked()) item->transfer_paused = false;
        if (transfer_active_.load()) transfer_pause_.store(false);
        changed = status_.pause_requested;
        status_.pause_requested = false;
    }
    transfer_control_cv_.notify_all();
    if (changed) NotifyStatusChanged();
}

std::optional<ConflictItemInfo> OpsManager::PendingConflict() const {
    std::lock_guard<std::mutex> lock(transfer_control_mutex_);
    return pending_conflict_;
}

void OpsManager::ResolveConflict(uint64_t token, ConflictChoice choice, bool apply_to_all) {
    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        if (!pending_conflict_ || pending_conflict_->token != token) return;
        resolved_conflict_token_ = token;
        resolved_conflict_choice_ = choice;
        resolved_conflict_apply_all_ = apply_to_all;
    }
    transfer_control_cv_.notify_all();
}

// --- Locked items (B站 #12) ---------------------------------------------------
// Runs on the ops worker after a failed delete/move/copy. Restart Manager may
// take a moment, which is why this never happens on the UI thread.
LockReport OpsManager::ProbeLock(const OpRequest& req, uint64_t task_id, HRESULT hr,
                                 const std::wstring& error) {
    LockReport report;
    const bool lockable = req.type == OpType::Copy || req.type == OpType::Move
        || req.type == OpType::RecycleDelete || req.type == OpType::RealDelete
        || req.type == OpType::Rename;
    if (lockable) {
        report = ProbeLockFailure(hr, error, req.sources, CurrentModuleDirectory());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (report.Empty()) {
        lock_retry_.reset();
        report.path.clear();
        return report;
    }
    LockRetry retry;
    retry.task_id = task_id;
    retry.req = req;
    retry.req.close_first.clear();
    retry.owners = report.owners;
    retry.path = report.path;
    lock_retry_ = std::move(retry);
    return report;
}

namespace {
bool IsSkippableSource(const OpRequest& req, const std::wstring& path) {
    if (req.sources.size() < 2 || path.empty()) return false;
    if (req.type == OpType::Rename || req.type == OpType::BatchRename) return false;
    return std::any_of(req.sources.begin(), req.sources.end(), [&](const std::wstring& source) {
        return _wcsicmp(source.c_str(), path.c_str()) == 0;
    });
}
}

bool OpsManager::CanSkipLockedItem(uint64_t task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lock_retry_ && lock_retry_->task_id == task_id && IsSkippableSource(lock_retry_->req, lock_retry_->path);
}

bool OpsManager::RetryLockedOperation(uint64_t task_id, bool close_owners, bool skip_item) {
    OpRequest req;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !lock_retry_ || lock_retry_->task_id != task_id) return false;
        if (skip_item && !IsSkippableSource(lock_retry_->req, lock_retry_->path)) return false;
        req = std::move(lock_retry_->req);
        req.lock_retry = true;
        if (skip_item) {
            const std::wstring skipped = lock_retry_->path;
            std::erase_if(req.sources, [&](const std::wstring& source) {
                return _wcsicmp(source.c_str(), skipped.c_str()) == 0;
            });
            close_owners = false;
        }
        if (close_owners) {
            for (const auto& owner : lock_retry_->owners)
                if (owner.closable) req.close_first.push_back(owner);
        }
        lock_retry_.reset();
    }
    Submit(std::move(req));
    return true;
}

std::optional<OpStatus> OpsManager::FinalStatus(uint64_t task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = final_statuses_.rbegin(); it != final_statuses_.rend(); ++it)
        if (it->task_id == task_id) return *it;
    return std::nullopt;
}

std::vector<OpsManager::RetrySpawn> OpsManager::RetryFailedItems(uint64_t task_id,
                                                                const std::vector<size_t>& indices) {
    std::vector<OpRequest> requests;
    std::vector<RetrySpawn> spawns;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto retry = std::find_if(failed_retries_.begin(), failed_retries_.end(),
            [&](const FailedItemsRetry& known) { return known.task_id == task_id; });
        if (!running_ || retry == failed_retries_.end()) return {};
        const auto& failures = retry->failures;
        std::vector<size_t> chosen = indices;
        if (chosen.empty())
            for (size_t i = 0; i < failures.size(); ++i) chosen.push_back(i);
        // Each item goes back to the folder it was meant for, under the name
        // it was meant to have; items for one folder under their own names
        // share a request. A delete's items all share one.
        const bool deleting = retry->req.type == OpType::RecycleDelete || retry->req.type == OpType::RealDelete;
        for (size_t i : chosen) {
            if (i >= failures.size()) continue;
            const OpFailure& failed = failures[i];
            if (failed.source.empty()) continue;
            if (deleting) {
                if (requests.empty()) {
                    OpRequest request = retry->req;
                    request.sources.clear();
                    request.lock_retry = false;
                    request.close_first.clear();
                    request.is_undo = false;
                    request.undo_group = 0;
                    requests.push_back(std::move(request));
                    spawns.push_back({0, {}});
                }
                requests.front().sources.push_back(failed.source);
                spawns.front().items.push_back(i);
                continue;
            }
            if (failed.destination.empty()) continue;
            const std::wstring folder = ParentOf(failed.destination);
            const bool renamed = _wcsicmp(FileName(failed.source).c_str(),
                                          FileName(failed.destination).c_str()) != 0;
            size_t shared = requests.size();
            if (!renamed) {
                for (size_t r = 0; r < requests.size(); ++r)
                    if (requests[r].new_name.empty() && _wcsicmp(requests[r].dest_dir.c_str(), folder.c_str()) == 0)
                        shared = r;
            }
            if (shared < requests.size()) {
                auto& sources = requests[shared].sources;
                if (std::none_of(sources.begin(), sources.end(), [&](const std::wstring& known) {
                        return _wcsicmp(known.c_str(), failed.source.c_str()) == 0; }))
                    sources.push_back(failed.source);
                spawns[shared].items.push_back(i);
                continue;
            }
            OpRequest request = retry->req;
            request.sources = {failed.source};
            request.dest_dir = folder;
            request.new_name = renamed ? FileName(failed.destination) : std::wstring{};
            request.lock_retry = false;
            request.close_first.clear();
            request.is_undo = false;
            request.undo_group = 0;
            requests.push_back(std::move(request));
            spawns.push_back({0, {i}});
        }
        if (requests.empty()) return {};
        if (indices.empty()) failed_retries_.erase(retry);
    }
    for (size_t r = 0; r < requests.size(); ++r) spawns[r].task_id = Submit(std::move(requests[r]));
    std::erase_if(spawns, [](const RetrySpawn& spawn) { return spawn.task_id == 0; });
    return spawns;
}

// Ends the chosen lock owners (if any), then drops delete/move sources that are
// already gone (the failed attempt may have handled part of the selection).
// Both retry buttons go through here. Returns false when the operation must
// not run; the status is published here then.
bool OpsManager::PrepareLockRetry(OpRequest& req, uint64_t task_id) {
    const std::vector<LockOwner> owners = std::move(req.close_first);
    req.close_first.clear();
    const std::wstring summary = Describe(req);
    if (!owners.empty()) {
        SetStatus([&](OpStatus& st) {
            st.active = true;
            st.type = req.type;
            st.task_id = task_id;
            st.phase = OpPhase::Running;
            st.percent = -1.0f;
            st.summary = l10n::Get(l10n::StringId::EndingTheProcessesThatAreUsingTheFiles).c_str();
            st.last_error.clear();
            st.locked_path.clear();
            st.lock_owners.clear();
        });
    }
    std::wstring failure;
    for (const auto& owner : owners) {
        DWORD error = ERROR_SUCCESS;
        const CloseOwnerResult result = CloseLockOwner(owner, 5000, &error);
        if (result == CloseOwnerResult::Closed || result == CloseOwnerResult::AlreadyGone) continue;
        failure = l10n::Get(l10n::StringId::CouldNotEndProcess).c_str() + DescribeLockOwner(owner);
        if (result == CloseOwnerResult::TimedOut) failure += l10n::Get(l10n::StringId::TheProcessDidNotExitInTime).c_str();
        else if (error != ERROR_SUCCESS) failure += L" | " + Win32Message(error);
        break;
    }
    bool nothing_left = false;
    if (failure.empty() && (req.type == OpType::RecycleDelete || req.type == OpType::RealDelete
                            || req.type == OpType::Move)) {
        std::vector<std::wstring> remaining;
        for (auto& source : req.sources)
            if (PathExists(source)) remaining.push_back(std::move(source));
        req.sources = std::move(remaining);
        nothing_left = req.sources.empty();
    }
    if (failure.empty() && !nothing_left) return true;
    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.type = req.type;
        st.task_id = task_id;
        st.current_item.clear();
        st.locked_path.clear();
        st.lock_owners.clear();
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.items_per_second = st.peak_items_per_second = 0.0;
        st.speed_basis = SpeedBasisFor(req.type);
        st.eta_seconds = 0;
        st.completed_ops++;
        if (nothing_left) {
            st.phase = OpPhase::Completed;
            st.percent = 100.0f;
            st.last_error.clear();
            st.summary = summary + l10n::Get(l10n::StringId::Completed).c_str();
        } else {
            st.phase = OpPhase::Failed;
            st.percent = -1.0f;
            st.last_error = failure;
            st.summary = std::wstring(OpVerb(req.type)) + l10n::Get(l10n::StringId::Failed).c_str();
        }
    });
    return false;
}

OpStatus OpsManager::Status() const {
    // Cleared before the snapshot: a change made after it posts again.
    notify_pending_since_.store(0);
    std::lock_guard<std::mutex> lock(mutex_);
    return PresentOperationStatus(status_);
}

constexpr ULONGLONG kNotifyPendingMs = 250;

void OpsManager::NotifyStatusChanged() {
    if (!notify_) return;
    const ULONGLONG now = GetTickCount64();
    ULONGLONG pending = notify_pending_since_.load();
    do {
        if (pending != 0 && now - pending < kNotifyPendingMs) return;
    } while (!notify_pending_since_.compare_exchange_weak(pending, now));
    last_status_notify_.store(now);
    notify_();
}

void OpsManager::SetStatus(const std::function<void(OpStatus&)>& fn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn(status_);
        ApplyUndoSummaryLocked();
        SampleSpeedLocked();
    }
    NotifyStatusChanged();
}

// An undo that went through reads as what it reversed (已撤销复制), not as
// its inverse (删除完成): the pill and the card show it right away.
void OpsManager::ApplyUndoSummaryLocked() {
    if (!active_item_ || !active_item_->req.undo_of || active_item_->seq != status_.task_id) return;
    if (status_.active || status_.phase != OpPhase::Completed || !status_.last_error.empty()) return;
    l10n::StringId id = l10n::StringId::OpUndone;
    switch (*active_item_->req.undo_of) {
    case OpType::Copy: id = l10n::StringId::OpUndoneCopy; break;
    case OpType::Move: id = l10n::StringId::OpUndoneMove; break;
    case OpType::Rename:
    case OpType::BatchRename: id = l10n::StringId::OpUndoneRename; break;
    case OpType::RecycleDelete: id = l10n::StringId::OpUndoneDelete; break;
    case OpType::CreateFolder:
    case OpType::CreateTextFile: id = l10n::StringId::OpUndoneCreate; break;
    default: break;
    }
    status_.summary = l10n::Get(id).c_str();
}

void OpsManager::SampleSpeedLocked() {
    if (status_.task_id != speed_graph_task_) {
        speed_graph_.Reset();
        speed_graph_task_ = status_.task_id;
    }
    // Same gate as the card: only a running, unpaused transfer moves the line.
    if (status_.phase != OpPhase::Running || status_.pause_requested) return;
    if (status_.speed_basis == OpSpeedBasis::None) return;
    const uint64_t now = GetTickCount64();
    if (!speed_graph_.Due(now)) return;
    speed_graph_.Sample(now, status_.percent, status_.speed_basis == OpSpeedBasis::Items
        ? status_.items_per_second : status_.bytes_per_second);
}

OpSpeedGraph OpsManager::SpeedGraph(uint64_t task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_id == 0 || task_id != speed_graph_task_) return {};
    return speed_graph_;
}

constexpr ULONGLONG kProgressNotifyMs = 50;
// Tasks this small show every finished item; it costs nothing at that count.
constexpr uint64_t kEveryItemMaxTotal = 64;
// Below this size CopyFile2 runs without a progress callback, and on the
// copy pool when there are at least kPoolMinFiles such files.
constexpr uint64_t kProgressCallbackMinBytes = 1024 * 1024;
constexpr size_t kPoolMinFiles = 16;

void OpsManager::SetProgressStatus(const std::function<void(OpStatus&)>& fn) {
    bool item_finished = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t items_before = status_.completed_items;
        fn(status_);
        ApplyUndoSummaryLocked();
        SampleSpeedLocked();
        item_finished = status_.completed_items != items_before &&
            status_.total_items <= kEveryItemMaxTotal;
    }
    if (item_finished || GetTickCount64() - last_status_notify_.load() >= kProgressNotifyMs)
        NotifyStatusChanged();
}

bool OpsManager::CanUndo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !undo_.empty() && undo_.back().supported;
}

bool OpsManager::CanUndoTask(uint64_t task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return task_id != 0 && !undo_.empty() && undo_.back().supported && undo_.back().task_id == task_id;
}

std::wstring OpsManager::UndoLabel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (undo_.empty()) return L"";
    const UndoEntry& e = undo_.back();
    if (e.supported && !e.label.empty())
        return l10n::IsChinese() ? std::wstring(l10n::Cn(L"撤销")) + e.label : L"Undo " + LowerFirst(e.label);
    if (!e.supported) {
        if (l10n::IsChinese())
            return std::wstring(l10n::Cn(L"撤销")) + OpVerb(e.type) + l10n::Cn(L"（不支持）");
        return L"Undo " + LowerFirst(OpVerb(e.type)) + L" (not supported)";
    }
    OpRequest r;
    r.type = e.type;
    r.sources = e.sources;
    r.dest_dir = e.dest_dir;
    r.new_name = e.new_name;
    if (l10n::IsChinese()) return std::wstring(l10n::Cn(L"撤销")) + Describe(r);
    return L"Undo " + LowerFirst(Describe(r));
}

void OpsManager::PushUndo(const OpRequest& req, uint64_t task_id,
                          const std::vector<std::wstring>* actual_destinations) {
    CompletedOperation completed;
    completed.task_id = task_id;
    completed.type = req.type;
    completed.sources = req.sources;
    if (req.type == OpType::CreateFolder || req.type == OpType::CreateTextFile)
        completed.destinations = req.sources;
    if (actual_destinations) {
        completed.destinations = *actual_destinations;
    } else if (req.type == OpType::Rename && !req.sources.empty()) {
        completed.destinations.push_back(JoinPath(ParentOf(req.sources.front()), req.new_name));
    } else if (req.type == OpType::BatchRename) {
        for (size_t i = 0; i < req.sources.size(); ++i) {
            const std::wstring name = i < req.new_names.size() ? req.new_names[i] : req.new_name;
            completed.destinations.push_back(JoinPath(ParentOf(req.sources[i]), name));
        }
    } else if (req.type == OpType::Copy || req.type == OpType::Move) {
        for (const auto& source : req.sources)
            completed.destinations.push_back(JoinPath(req.dest_dir,
                req.type == OpType::Move && req.sources.size() == 1 && !req.new_name.empty()
                    ? req.new_name : FileName(source)));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completions_.push_back(std::move(completed));
    }
    if (req.is_undo) return;
    UndoEntry e;
    e.type = req.type;
    e.sources = req.sources;
    if (actual_destinations) e.destinations = *actual_destinations;
    e.dest_dir = req.dest_dir;
    e.new_name = req.new_name;
    e.group = req.undo_group;
    e.label = req.undo_label;
    e.task_id = task_id;
    if (req.type == OpType::RealDelete) return;              // never undoable, not recorded
    if (req.type == OpType::RestoreRecycle) return;
    if (req.type == OpType::EmptyRecycle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    undo_.push_back(std::move(e));
    TrimUndoHistory(undo_);
}

std::vector<CompletedOperation> OpsManager::DrainCompletions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CompletedOperation> out;
    out.reserve(completions_.size());
    while (!completions_.empty()) {
        out.push_back(std::move(completions_.front()));
        completions_.pop_front();
    }
    return out;
}

std::vector<FinishedTask> OpsManager::DrainFinishedTasks() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FinishedTask> out;
    out.reserve(finished_tasks_.size());
    while (!finished_tasks_.empty()) {
        out.push_back(std::move(finished_tasks_.front()));
        finished_tasks_.pop_front();
    }
    return out;
}

bool OpsManager::GroupPendingLocked(uint64_t group) const {
    if (!group) return false;
    if (active_item_ && active_item_->req.undo_group == group) return true;
    return std::any_of(queue_.begin(), queue_.end(), [group](const QueueItem& item) {
        return item.req.undo_group == group;
    });
}

void OpsManager::Undo() { UndoNewest(nullptr); }

uint64_t OpsManager::TopUndoGroup() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return undo_.empty() ? 0 : undo_.back().group;
}

OpsManager::GroupUndo OpsManager::UndoGroup(uint64_t group) {
    return group ? UndoNewest(&group) : GroupUndo::NotOnTop;
}

void OpsManager::RecordExternalCreate(const std::wstring& path, uint64_t group, std::wstring label) {
    // Refresh only: no task of ours made it, so no arrival selection or
    // rename intent applies.
    CompletedOperation completed;
    completed.type = OpType::CreateTextFile;
    completed.sources.push_back(path);
    completed.destinations.push_back(path);
    completed.refresh_only = true;
    UndoEntry e;
    e.type = OpType::CreateTextFile;
    e.sources.push_back(path);
    e.destinations.push_back(path);
    e.group = group;
    e.label = std::move(label);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (update_preparing_) return;
        completions_.push_back(std::move(completed));
        undo_.push_back(std::move(e));
        TrimUndoHistory(undo_);
    }
    NotifyStatusChanged();
}

OpsManager::GroupUndo OpsManager::UndoNewest(const uint64_t* required_group) {
    std::vector<UndoEntry> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (update_preparing_ || undo_.empty()) return GroupUndo::Unavailable;
        if (required_group) {
            if (undo_.back().group != *required_group) return GroupUndo::NotOnTop;
            if (!undo_.back().supported) return GroupUndo::Unavailable;
            if (GroupPendingLocked(*required_group)) return GroupUndo::Pending;
        }
        if (GroupPendingLocked(undo_.back().group)) {
            // Undoing half an action would strand the rest; wait for it.
            deferred_undo_group_ = undo_.back().group;
            return GroupUndo::Pending;
        }
        if (!undo_.back().supported) {
            // Leave the entry; report why it cannot be undone.
            status_.last_error = l10n::Get(l10n::StringId::UndoIsNotSupportedForItemsMovedToTheRecycleBin).c_str();
        } else {
            // One user action recorded as several operations undoes as one
            // step, newest first; the queue runs the inverses in that order.
            const uint64_t group = undo_.back().group;
            do {
                batch.push_back(std::move(undo_.back()));
                undo_.pop_back();
            } while (group && !undo_.empty() && undo_.back().group == group && undo_.back().supported);
        }
    }
    if (batch.empty()) {
        if (notify_) notify_();
        return GroupUndo::Unavailable;
    }

    for (const UndoEntry& e : batch) {
    switch (e.type) {
    case OpType::Move: {
        // Move each actual destination back to its exact original name and
        // parent, newest first: an item merged into a folder that an earlier
        // root of the same move created goes back before that folder does,
        // instead of travelling with it.
        for (size_t i = e.sources.size(); i-- > 0;) {
            const auto& src = e.sources[i];
            OpRequest inv;
            inv.type = OpType::Move;
            inv.is_undo = true;
            inv.undo_of = e.type;
            inv.sources.push_back(i < e.destinations.size()
                ? e.destinations[i] : JoinPath(e.dest_dir, FileName(src)));
            inv.dest_dir = ParentOf(src);
            inv.new_name = FileName(src);
            Submit(std::move(inv));
        }
        break;
    }
    case OpType::Rename: {
        OpRequest inv;
        inv.type = OpType::Rename;
        inv.is_undo = true;
        inv.undo_of = e.type;
        std::wstring new_path = JoinPath(ParentOf(e.sources.front()), e.new_name);
        inv.sources.push_back(new_path);
        inv.new_name = FileName(e.sources.front());
        Submit(std::move(inv));
        break;
    }
    case OpType::Copy: {
        // Explorer semantics: undo copy = delete the produced copies (to bin).
        OpRequest inv;
        inv.type = OpType::RecycleDelete;
        inv.is_undo = true;
        inv.undo_of = e.type;
        if (!e.destinations.empty()) inv.sources = e.destinations;
        else for (const auto& src : e.sources)
            inv.sources.push_back(JoinPath(e.dest_dir, FileName(src)));
        Submit(std::move(inv));
        break;
    }
    case OpType::CreateFolder:
    case OpType::CreateTextFile: {
        // Undo create = recycle-delete the created item.
        OpRequest inv;
        inv.type = OpType::RecycleDelete;
        inv.is_undo = true;
        inv.undo_of = e.type;
        inv.sources = e.sources;
        Submit(std::move(inv));
        break;
    }
    case OpType::RecycleDelete: {
        OpRequest inv;
        inv.type = OpType::RestoreRecycle;
        inv.is_undo = true;
        inv.undo_of = e.type;
        inv.sources = e.sources;
        Submit(std::move(inv));
        break;
    }
    case OpType::BatchRename: {
        for (int i = static_cast<int>(e.sources.size()) - 1; i >= 0; --i) {
            const size_t index = static_cast<size_t>(i);
            OpRequest inv;
            inv.type = OpType::Rename;
            inv.is_undo = true;
            inv.undo_of = e.type;
            inv.sources.push_back(index < e.destinations.size()
                ? e.destinations[index]
                : JoinPath(ParentOf(e.sources[index]), e.new_name));
            inv.new_name = FileName(e.sources[index]);
            Submit(std::move(inv));
        }
        break;
    }
    default:
        break;
    }
    }
    return GroupUndo::Started;
}

// ---------------------------------------------------------------------------
// Worker thread: serialized queue -> ShellClient -> event wait.
// ---------------------------------------------------------------------------

void OpsManager::EnqueueOpen(QueueItem item, bool front) {
    {
        std::lock_guard<std::mutex> lock(open_mutex_);
        if (!open_running_) return;
        item.seq = next_seq_++;
        item.enqueued_at = GetTickCount64();
        if (front) open_queue_.push_front(std::move(item));
        else open_queue_.push_back(std::move(item));
        const auto& queued = front ? open_queue_.front() : open_queue_.back();
        diagnostics::runtime::Event("shell_open_queued", {{"task", queued.seq},
            {"default_verb", queued.open_verb.empty()}, {"explicit_app", !queued.open_file.empty()},
            {"path_chars", static_cast<uint64_t>(queued.open_path.size())},
            {"queue_size", static_cast<uint64_t>(open_queue_.size())}});
    }
    open_cv_.notify_one();
}

void OpsManager::OpenThread() {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(open_mutex_);
            open_cv_.wait(lock, [this] { return !open_queue_.empty() || !open_running_; });
            if (!open_running_ && open_queue_.empty()) break;
            if (open_queue_.empty()) continue;
            item = std::move(open_queue_.front());
            open_queue_.pop_front();
        }

        // Dialogs and association errors belong to the Pulse window, the way
        // Explorer parents them to the folder window. The fallback covers a UI
        // thread that has not registered its HWND yet.
        HWND dialog_owner = ui_hwnd_.load();
        if (!dialog_owner || !IsWindow(dialog_owner)) dialog_owner = GetForegroundWindow();

        if (_wcsicmp(item.open_verb.c_str(), L"__terminal") == 0 || item.open_verb == L"__context_terminal") {
            const auto started = GetTickCount64();
            const auto result = item.open_verb == L"__context_terminal"
                ? LaunchContextTerminal(item.open_file, item.open_path, dialog_owner)
                : LaunchTerminal(item.open_file, item.open_args, item.open_path, dialog_owner);
            diagnostics::runtime::Event("terminal_launch_result", {{"task", item.seq},
                {"open_error", result.open_error}, {"error", result.error},
                {"cancelled", result.error == ERROR_CANCELLED},
                {"elevation_requested", result.elevation_requested}, {"elapsed_ms", GetTickCount64() - started}});
            if (result.error && result.error != ERROR_CANCELLED) {
                wchar_t detail[512]{};
                FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                    result.error, 0, detail, ARRAYSIZE(detail), nullptr);
                const auto message = std::wstring(l10n::Get(l10n::StringId::CouldNotOpenTheTerminal).c_str()) +
                    L"\n" + detail + L" (" + std::to_wstring(result.error) + L")";
                MessageBoxW(dialog_owner, message.c_str(), L"Pulse", MB_OK | MB_ICONERROR);
            }
            continue;
        }

        if (_wcsicmp(item.open_verb.c_str(), L"__cmdline") == 0) {
            const auto started = GetTickCount64();
            const auto result = LaunchShellCommand(item.open_file, item.open_path, dialog_owner);
            diagnostics::runtime::Event("shell_command_result", {{"task", item.seq},
                {"create_error", result.create_error}, {"error", result.error},
                {"elevation_requested", result.elevation_requested}, {"elapsed_ms", GetTickCount64() - started}});
            if (result.error && result.error != ERROR_CANCELLED) {
                wchar_t detail[512]{};
                FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                    result.error, 0, detail, ARRAYSIZE(detail), nullptr);
                const auto message = std::wstring(l10n::Get(l10n::StringId::CouldNotLaunchThisMenuCommand).c_str()) +
                    L"\n" + detail + L" (" + std::to_wstring(result.error) + L")";
                MessageBoxW(dialog_owner, message.c_str(), L"Pulse", MB_OK | MB_ICONERROR);
            }
            continue;
        }

        if (_wcsicmp(item.open_verb.c_str(), L"openas") == 0 ||
            _wcsicmp(item.open_verb.c_str(), L"openwith") == 0) {
            const std::wstring shell_path =
                pulse::path::StripExtendedPathPrefix(item.open_path);
            OPENASINFO info{};
            info.pcszFile = shell_path.c_str();
            info.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT | OAIF_EXEC;
            const ULONGLONG dialog_started = GetTickCount64();
            const HRESULT hr = SHOpenWithDialog(dialog_owner, &info);
            if (item.enqueued_at != 0) {
                // Queue wait and dialog cost need different fixes, so report both
                // (DebugView) instead of guessing which one the user feels.
                wchar_t timing[192]{};
                swprintf_s(timing, L"Pulse: open-with queued %llu ms, dialog %llu ms\n",
                           dialog_started - item.enqueued_at,
                           GetTickCount64() - dialog_started);
                OutputDebugStringW(timing);
            }
            if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                SHELLEXECUTEINFOW fallback{sizeof(fallback)};
                fallback.hwnd = dialog_owner;
                fallback.lpVerb = L"openas";
                fallback.lpFile = shell_path.c_str();
                fallback.nShow = SW_SHOWNORMAL;
                fallback.fMask = SEE_MASK_INVOKEIDLIST;
                const BOOL opened = ShellExecuteExW(&fallback);
                if (!opened) {
                    // Used to fail silently, which read as a dead menu row.
                    wchar_t message[320]{};
                    swprintf_s(message,
                        L"Pulse: open-with failed for %ls (SHOpenWithDialog 0x%08lX, "
                        L"shell error %lu)\n",
                        shell_path.c_str(), static_cast<unsigned long>(hr), GetLastError());
                    OutputDebugStringW(message);
                }
            }
            continue;
        }

        if (_wcsicmp(item.open_verb.c_str(), L"properties") == 0 && item.open_paths.size() > 1) {
            ShowMultiFilePropertiesAsync(std::move(item.open_paths));
            continue;
        }

        if (_wcsicmp(item.open_verb.c_str(), L"properties") == 0) {
            const std::wstring shell_path = pulse::path::StripExtendedPathPrefix(item.open_path);
            bool shown = SHObjectProperties(dialog_owner, SHOP_FILEPATH,
                                            shell_path.c_str(), nullptr) != FALSE;
            if (!shown) {
                SHELLEXECUTEINFOW fallback{sizeof(fallback)};
                fallback.lpVerb = L"properties";
                fallback.lpFile = shell_path.c_str();
                fallback.nShow = SW_SHOWNORMAL;
                fallback.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC;
                shown = ShellExecuteExW(&fallback) != FALSE;
            }
            if (!shown) {
                const DWORD error = GetLastError();
                wchar_t message[256]{};
                swprintf_s(message, L"Pulse: SHObjectProperties failed for %ls (error %lu)\n",
                           shell_path.c_str(), error);
                OutputDebugStringW(message);
            }
            continue;
        }

        if (_wcsicmp(item.open_verb.c_str(), L"__shortcut") == 0) {
            // Dropped on a program shortcut: what the .lnk adds to its program.
            ShortcutLaunch link;
            const bool read = ReadShortcutLaunch(item.open_path, link);
            item.open_args = CombineShortcutArguments(read ? link.arguments : std::wstring(), item.open_args);
            item.open_path = !link.directory.empty() ? link.directory : ParentOf(item.open_file);
            item.open_verb = link.run_as ? L"runas" : L"open";
        }

        // Association work stays on the dedicated STA, never the UI/transfer
        // thread. Complete handoff here because the idle worker does not pump.
        const ULONGLONG started = GetTickCount64();
        diagnostics::runtime::Event("shell_open_begin", {{"task", item.seq},
            {"default_verb", item.open_verb.empty()}, {"explicit_app", !item.open_file.empty()},
            {"queue_ms", item.enqueued_at ? started - item.enqueued_at : 0},
            {"com_hresult", static_cast<uint32_t>(com_hr)}});
        const std::wstring shell_path = pulse::path::StripExtendedPathPrefix(item.open_path);
        const std::wstring target = item.open_file.empty() ? shell_path : item.open_file;
        const std::wstring item_dir = item.open_file.empty()
            ? OpenItemWorkingDirectory(item.open_path) : shell_path;
        const auto result = LaunchShellItem(target, item.open_verb, item.open_args, item_dir, dialog_owner);
        diagnostics::runtime::Event("shell_open_result", {{"task", item.seq},
            {"default_verb", item.open_verb.empty()}, {"explicit_app", !item.open_file.empty()},
            {"error", result.error}, {"cancelled", result.error == ERROR_CANCELLED},
            {"process_id", result.process_id}, {"shell_mask", result.mask},
            {"elapsed_ms", GetTickCount64() - started}});
        if (result.error && result.error != ERROR_CANCELLED) {
            wchar_t detail[512]{};
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                result.error, 0, detail, ARRAYSIZE(detail), nullptr);
            const auto message = std::wstring(item.open_verb.empty()
                ? l10n::Get(l10n::StringId::CouldNotOpenThisFileWithItsDefaultApplication).c_str()
                : l10n::Get(l10n::StringId::CouldNotCompleteThisOpenOperation).c_str()) +
                L"\n" + target + L"\n" + detail + L" (" + std::to_wstring(result.error) + L")";
            MessageBoxW(dialog_owner, message.c_str(), L"Pulse", MB_OK | MB_ICONERROR);
        }
    }
    if (SUCCEEDED(com_hr)) CoUninitialize();
}

void OpsManager::RecordShellDone(uint32_t id, uint32_t hr, bool cancelled, std::wstring error) {
        {
            std::unique_lock<std::mutex> lock(menu_mutex_);
            if (const auto verb = shell_verb_ids_.find(id); verb != shell_verb_ids_.end()) {
                const bool refresh = verb->second;
                shell_verb_ids_.erase(verb);
                if (FAILED(static_cast<HRESULT>(hr)) && !cancelled)
                    shell_verb_error_ = error.empty() ? l10n::Get(l10n::StringId::OpFailed).c_str() : error;
                lock.unlock();
                // Copy / open / properties change nothing on the phone: no
                // re-listing of a big DCIM for them.
                if (refresh) ctx_invoke_done_.fetch_add(1, std::memory_order_acq_rel);
                if (notify_) notify_();
                return;
            }
        }
        if (ConsumeCtxInvokeDone(id)) {
            ctx_invoke_done_.fetch_add(1, std::memory_order_acq_rel);
            if (notify_) notify_();
            return;
        }
        if (ConsumeCtxQueryDone(id)) return;
        {
            std::lock_guard<std::mutex> lock(done_mutex_);
            if (!shell_request_ids_.contains(id) || done_results_.contains(id)) return;
            done_results_.emplace(id, ShellDoneResult{hr, cancelled, std::move(error)});
            shell_activity_tick_ = GetTickCount64();
        }
        done_cv_.notify_all();
    }

void OpsManager::ConfigureShellCallbacks() {
#ifndef PULSE_ELEVATED_HOST
    ipc::ShellClient::Callbacks cb;
    // Item operations (delete, restore) only ever report item counts — the shell has no
    // byte callback for them — so their speed comes from counting items per second.
    // The estimator lives with the callback because that outlives any single task, and
    // it resets whenever the task id changes.
    auto shell_item_rate = std::make_shared<TransferRateEstimator>();
    // Deleting a folder tree runs well under one item per second and still deserves a
    // countdown, so this estimator drops the byte-transfer floor.
    shell_item_rate->SetEtaFloor(0.0);
    auto shell_rate_task = std::make_shared<uint64_t>(0);
    // A phone transfer (RunShellTransfer) reports a percent of its bytes.
    auto shell_byte_rate = std::make_shared<TransferRateEstimator>();
    auto shell_byte_task = std::make_shared<uint64_t>(0);
    cb.progress = [this, shell_item_rate, shell_rate_task, shell_byte_rate, shell_byte_task](
                      uint32_t id, float pct, std::wstring item,
                      uint32_t items_done, uint32_t total_items) {
        if (id != current_req_id_.load()) return;
        shell_activity_tick_ = GetTickCount64();
        SetStatus([&](OpStatus& st) {
            st.percent = pct;
            st.current_item = item;
            if (total_items > 0) st.total_items = total_items;
            st.completed_items = (std::min)(static_cast<uint64_t>(items_done),
                                             st.total_items);
            if (st.speed_basis == OpSpeedBasis::Items) {
                const ULONGLONG now = GetTickCount64();
                if (*shell_rate_task != st.task_id) {
                    *shell_rate_task = st.task_id;
                    shell_item_rate->Reset(now, st.completed_items);
                }
                shell_item_rate->Observe(now, st.completed_items, st.total_items);
                st.items_per_second = shell_item_rate->speed();
                st.peak_items_per_second = (std::max)(st.peak_items_per_second,
                                                      st.items_per_second);
                st.eta_seconds = shell_item_rate->eta_seconds();
            } else if (st.speed_basis == OpSpeedBasis::Bytes && st.total_bytes > 0 && pct >= 0.0f) {
                const double share = (std::min)(100.0, static_cast<double>(pct)) / 100.0;
                st.transferred_bytes = static_cast<uint64_t>(static_cast<double>(st.total_bytes) * share);
                const ULONGLONG now = GetTickCount64();
                if (*shell_byte_task != st.task_id) {
                    *shell_byte_task = st.task_id;
                    shell_byte_rate->Reset(now, st.transferred_bytes);
                }
                shell_byte_rate->Observe(now, st.transferred_bytes, st.total_bytes);
                st.bytes_per_second = shell_byte_rate->speed();
                st.peak_bytes_per_second = (std::max)(st.peak_bytes_per_second, st.bytes_per_second);
                st.eta_seconds = shell_byte_rate->eta_seconds();
            }
            if (!item.empty()) {
                std::wstring base = st.summary;
                auto sep = base.find(L"  (");
                if (sep != std::wstring::npos) base.erase(sep);
                wchar_t buf[32];
                swprintf_s(buf, L"  (%.0f%%)", pct);
                st.summary = base + buf;
            }
        });
    };
    // Ownership is registered before Submit can emit any callback. Menu query
    // failures and invoke completions must never become file-op terminal data.
    cb.done = [this](uint32_t id, uint32_t hr, bool cancelled, std::wstring error) {
        RecordShellDone(id, hr, cancelled, std::move(error));
    };
    cb.ctx_items = [this](uint32_t id, std::vector<ipc::CtxMenuItem> items, bool partial,
                          ipc::CtxItemsExtra extra) {
        std::vector<ShellMenuItem> out;
        out.reserve(items.size());
        for (auto& it : items) {
            ShellMenuItem m;
            m.id = it.id;
            m.enabled = it.enabled;
            m.separator_after = it.separator_after;
            m.has_children = it.has_children;
            m.child = it.child;
            m.verb = std::move(it.verb);
            m.text = std::move(it.text);
            m.clsid = std::move(it.clsid);
            m.handler = std::move(it.handler);
            m.mnemonic = it.mnemonic;
            m.icon = std::move(it.icon);
            out.push_back(std::move(m));
        }
        ShellMenuAnswer answer;
        answer.slow_clsids = std::move(extra.slow_clsids);
        answer.handlers.reserve(extra.handlers.size());
        for (auto& h : extra.handlers) {
            ShellMenuHandlerStatus status;
            status.clsid = std::move(h.clsid);
            status.done = (h.flags & ipc::CTX_HANDLER_DONE) != 0;
            status.duplicate = (h.flags & ipc::CTX_HANDLER_DUPLICATE) != 0;
            status.elapsed_ms = h.elapsed_ms;
            status.rows = h.rows;
            answer.handlers.push_back(std::move(status));
        }
        OnCtxItems(id, std::move(out), partial, std::move(answer));
    };
#ifdef PULSE_ELEVATED_TEST_CLIENT
    ObserveShellCallbacksForReview(cb);
#endif
    if (own_shell_client_) ipc::ShellClient::Instance().Start(cb);
#endif
}

void OpsManager::WorkerThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    ConfigureShellCallbacks();

    for (;;) {
        QueueItem item;
        std::wstring cleanup_root;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !queue_.empty() || !recovery_cleanup_roots_.empty() || !running_;
            });
            if (!running_) break;
            if (!recovery_cleanup_roots_.empty()) {
                cleanup_root = std::move(recovery_cleanup_roots_.front());
                recovery_cleanup_roots_.pop_front();
                recovery_cleanup_active_ = true;
            } else {
                item = std::move(queue_.front());
                queue_.pop_front();
                if (item.open_path.empty()) active_item_ = item;
            }
        }

        if (!cleanup_root.empty()) {
            ReconcileTemporaryFiles(cleanup_root);
            std::lock_guard<std::mutex> lock(mutex_);
            recovery_cleanup_active_ = false;
            continue;
        }

        if (item.open_path.empty()) PersistJournal();

        // Opens/verbs run on OpenThread — never block transfers.
        if (!item.open_path.empty()) continue;

        if (item.req.type == OpType::RecycleDelete && item.req.only_if_empty) {
            auto& sources = item.req.sources;
            sources.erase(std::remove_if(sources.begin(), sources.end(), [](const std::wstring& path) {
                std::error_code error;
                return !std::filesystem::is_directory(path, error) || !std::filesystem::is_empty(path, error) || error;
            }), sources.end());
        }
        const bool skip = item.req.only_if_empty && item.req.sources.empty();
        const bool run = !skip && ((!item.req.lock_retry && item.req.close_first.empty()) ||
                         PrepareLockRetry(item.req, item.seq));
        moved_sources_.clear();
        recycle_refused_.clear();
        recycle_refusal_ = RecycleRefusal::None;
        const auto diagnostic_started = GetTickCount64();
        diagnostics::runtime::Event("file_operation_begin", {{"task", item.seq},
            {"type", static_cast<uint64_t>(item.req.type)}, {"sources", item.req.sources.size()},
            {"undo", item.req.is_undo}, {"run", run}});
        if (!run) {
            // PrepareLockRetry already published the outcome.
        } else if (item.req.type == OpType::Copy || item.req.type == OpType::Move) {
            RunTransfer(item.req, item.seq);
        } else {
            RunShellOp(item.req, item.seq);
        }
        const auto diagnostic_status = Status();
        bool run_deferred_undo = false;
        diagnostics::runtime::Event("file_operation_end", {{"task", item.seq},
            {"phase", static_cast<uint64_t>(diagnostic_status.phase)}, {"items", diagnostic_status.completed_items},
            {"bytes", diagnostic_status.transferred_bytes}, {"has_error", !diagnostic_status.last_error.empty()},
            {"elapsed_ms", GetTickCount64() - diagnostic_started}});
        {
            std::lock_guard<std::mutex> lock(mutex_);
            FinishedTask finished;
            finished.task_id = item.seq;
            finished.type = item.req.type;
            finished.is_undo = item.req.is_undo;
            finished.sources = item.req.sources;
            finished.success = diagnostic_status.phase == OpPhase::Completed;
            finished.recycle_refused = std::move(recycle_refused_);
            finished.recycle_refusal = recycle_refusal_;
            recycle_refused_.clear();
            if (!moved_sources_.empty()) {
                // One set instead of a search per source: a move of n items
                // compared n * n paths here.
                std::unordered_set<std::wstring> moved;
                moved.reserve(moved_sources_.size());
                for (const auto& path : moved_sources_) moved.insert(pulse::path::FoldedKey(path));
                for (const auto& source : item.req.sources)
                    if (moved.contains(pulse::path::FoldedKey(source))) finished.moved_sources.push_back(source);
            }
            finished_tasks_.push_back(std::move(finished));
            if (!diagnostic_status.active && diagnostic_status.task_id == item.seq) {
                final_statuses_.push_back(diagnostic_status);
                while (final_statuses_.size() > 16) final_statuses_.pop_front();
            }
            active_item_.reset();
            if (deferred_undo_group_ && !GroupPendingLocked(deferred_undo_group_)) {
                deferred_undo_group_ = 0;
                run_deferred_undo = true;
            }
        }
        if (run_deferred_undo) Undo();
        if (notify_) notify_();
        PersistJournal();
    }

#ifndef PULSE_ELEVATED_HOST
    if (own_shell_client_) ipc::ShellClient::Instance().Stop();
#endif
    CoUninitialize();
}

// ---------------------------------------------------------------------------
// Context-menu forwarding thread: keeps ShellClient::Submit (which may spawn
// or reconnect to pulse_shell) off the UI thread and out of the transfer queue.
// ---------------------------------------------------------------------------
void OpsManager::SetShellMenuCallback(ShellMenuCallback cb) {
    std::lock_guard<std::mutex> lock(menu_mutex_);
    menu_cb_ = std::move(cb);
}

uint32_t OpsManager::QueryShellMenu(std::vector<std::wstring> paths, void* owner_hwnd,
                                    bool background, bool extended,
                                    std::vector<std::wstring> disabled_clsids) {
    MenuJob job;
    job.kind = MenuJob::Kind::Query;
    job.owner_hwnd = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(owner_hwnd));
    job.background = background;
    job.extended = extended;
    job.paths = std::move(paths);
    job.disabled_clsids = std::move(disabled_clsids);
    uint32_t token = 0;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        if (!menu_running_) return 0;
        token = next_menu_token_++;
        job.token = token;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
    return token;
}

void OpsManager::InvokeShellMenu(uint32_t token, uint32_t item_id,
                                 std::wstring verb, std::wstring text) {
    MenuJob job;
    job.kind = MenuJob::Kind::Invoke;
    job.token = token;
    job.item_id = item_id;
    job.verb = std::move(verb);
    job.text = std::move(text);
    {
        std::scoped_lock lock(mutex_, menu_mutex_);
        if (update_preparing_ || !menu_running_) return;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
}

void OpsManager::CloseShellMenu(uint32_t token) {
    MenuJob job;
    job.kind = MenuJob::Kind::Close;
    job.token = token;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        if (!menu_running_) return;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
}

bool OpsManager::RunShellVerb(std::vector<std::wstring> shell_ids, std::wstring verb, bool background,
                              void* owner_hwnd) {
    if (shell_ids.empty() || std::any_of(shell_ids.begin(), shell_ids.end(),
                                         [](const std::wstring& id) { return id.empty(); }))
        return false;
    MenuJob job;
    job.kind = MenuJob::Kind::Verb;
    job.owner_hwnd = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(owner_hwnd));
    job.background = background;
    job.paths = std::move(shell_ids);
    job.verb = std::move(verb);
    {
        std::scoped_lock lock(mutex_, menu_mutex_);
        if (update_preparing_ || !menu_running_) return false;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
    return true;
}

bool OpsManager::RenameShellItem(std::wstring shell_id, std::wstring new_name) {
    if (shell_id.empty() || new_name.empty()) return false;
    MenuJob job;
    job.kind = MenuJob::Kind::Rename;
    job.paths.push_back(std::move(shell_id));
    job.text = std::move(new_name);
    {
        std::scoped_lock lock(mutex_, menu_mutex_);
        if (update_preparing_ || !menu_running_) return false;
        menu_queue_.push_back(std::move(job));
    }
    menu_cv_.notify_one();
    return true;
}

std::wstring OpsManager::TakeShellVerbError() {
    std::lock_guard<std::mutex> lock(menu_mutex_);
    return std::exchange(shell_verb_error_, std::wstring());
}

bool OpsManager::ConsumeCtxQueryDone(uint32_t id) {
    ShellMenuCallback cb;
    uint32_t token = 0;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        const auto it = menu_token_by_session_.find(id);
        if (it == menu_token_by_session_.end()) return false;
        token = it->second;
        menu_token_by_session_.erase(it);
        menu_session_by_token_.erase(token);
        cb = menu_cb_;
    }
    // An empty final response ends the UI token, including synchronous failures.
    if (cb) cb(token, {}, false, {});
    return true;
}

bool OpsManager::ConsumeCtxInvokeDone(uint32_t id) {
    std::lock_guard<std::mutex> lock(menu_mutex_);
    return ctx_invoke_ids_.erase(id) != 0;
}

bool OpsManager::TakeCtxInvokeDone() {
    return ctx_invoke_done_.exchange(0, std::memory_order_acq_rel) != 0;
}

void OpsManager::OnCtxItems(uint32_t client_id, std::vector<ShellMenuItem> items,
                            bool partial, ShellMenuAnswer answer) {
    ShellMenuCallback cb;
    uint32_t token = 0;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        auto it = menu_token_by_session_.find(client_id);
        if (it == menu_token_by_session_.end()) return; // session already closed
        token = it->second;
        cb = menu_cb_;
        if (!partial) {
            // Keep token mapping until close/invoke; partial must not drop it.
        }
    }
    if (cb) cb(token, std::move(items), partial, std::move(answer));
}

void OpsManager::DispatchMenuJob(const MenuJob& job) {
    auto& client = ipc::ShellClient::Instance();
    switch (job.kind) {
    case MenuJob::Kind::Query: {
        const uint32_t id = client.QueryContextMenu(
            job.paths, job.owner_hwnd, job.background, job.extended,
            job.disabled_clsids, [this, token = job.token](uint32_t request) {
                std::lock_guard<std::mutex> lock(menu_mutex_);
                menu_session_by_token_[token] = request;
                menu_token_by_session_[request] = token;
            });
        if (id == 0) {
            ShellMenuCallback cb;
            { std::lock_guard<std::mutex> lock(menu_mutex_); cb = menu_cb_; }
            if (cb) cb(job.token, {}, false, {});
        }
        break;
    }
    case MenuJob::Kind::Invoke: {
        uint32_t session = 0;
        {
            std::lock_guard<std::mutex> lock(menu_mutex_);
            auto it = menu_session_by_token_.find(job.token);
            if (it == menu_session_by_token_.end()) break;
            session = it->second;
            menu_session_by_token_.erase(it);
            menu_token_by_session_.erase(session);
        }
        const uint32_t id = client.InvokeContextMenu(session, job.item_id,
            job.verb, job.text, [this](uint32_t request) {
                std::lock_guard<std::mutex> lock(menu_mutex_);
                ctx_invoke_ids_.insert(request);
            });
        if (id == 0) {
            ctx_invoke_done_.fetch_add(1, std::memory_order_acq_rel);
            if (notify_) notify_();
        }
        break;
    }
    case MenuJob::Kind::Close: {
        uint32_t session = 0;
        {
            std::lock_guard<std::mutex> lock(menu_mutex_);
            auto it = menu_session_by_token_.find(job.token);
            if (it == menu_session_by_token_.end()) break;
            session = it->second;
            menu_session_by_token_.erase(it);
            menu_token_by_session_.erase(session);
        }
        client.CloseContextMenu(session);
        break;
    }
    case MenuJob::Kind::Verb:
    case MenuJob::Kind::Rename: {
        const bool refresh = job.kind == MenuJob::Kind::Rename || _wcsicmp(job.verb.c_str(), L"delete") == 0 ||
                             _wcsicmp(job.verb.c_str(), L"paste") == 0 ||
                             _wcsnicmp(job.verb.c_str(), L"drop:", 5) == 0;
        const auto accepted = [this, refresh](uint32_t request) {
            std::lock_guard<std::mutex> lock(menu_mutex_);
            shell_verb_ids_[request] = refresh;
        };
        const uint32_t id = job.kind == MenuJob::Kind::Verb
            ? client.ShellVerb(job.paths, job.owner_hwnd, job.background, job.verb, accepted)
            : client.Rename(job.paths.front(), job.text, accepted);
        if (id == 0) {
            {
                std::lock_guard<std::mutex> lock(menu_mutex_);
                shell_verb_error_ = l10n::Get(l10n::StringId::TheFileOperationServiceIsNotRunning).c_str();
            }
            if (notify_) notify_();
        }
        break;
    }
    }
}

void OpsManager::MenuThread() {
    for (;;) {
        MenuJob job;
        {
            std::unique_lock<std::mutex> lock(menu_mutex_);
            menu_cv_.wait(lock, [this] { return !menu_queue_.empty() || !menu_running_; });
            if (!menu_running_) break; // drop queued jobs; sessions die with the host
            job = std::move(menu_queue_.front());
            menu_queue_.pop_front();
            menu_dispatching_ = true;
        }
        DispatchMenuJob(job);
        {
            std::lock_guard<std::mutex> lock(menu_mutex_);
            menu_dispatching_ = false;
        }
    }
}

namespace {

// System.FileName / System.Size / System.DateModified (propkey.h), kept in
// this file: the many targets built from it need no propsys import.
constexpr PROPERTYKEY kKeyFileName{{0x41CF5AE0, 0xF75A, 0x4806, {0xBD, 0x87, 0x59, 0xC7, 0xD9, 0x24, 0x8E, 0xB9}}, 100};
constexpr PROPERTYKEY kKeySize{{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}}, 12};
constexpr PROPERTYKEY kKeyDateModified{{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}}, 14};

// ---------------------------------------------------------------------------
// Phone operations (IsShellTransferRequest): what Pulse reads itself before
// handing the transfer to the shell host. The worker thread is an STA, as
// the WPD shell folders want; every call here may wait on the device.
// ---------------------------------------------------------------------------
struct ShellEntry {
    std::wstring name;        // leaf name with its extension (System.FileName)
    bool directory = false;
    uint64_t bytes = 0;
    FILETIME modified{};
};

class ShellItemRef {
public:
    ShellItemRef() = default;
    ShellItemRef(const ShellItemRef&) = delete;
    ShellItemRef& operator=(const ShellItemRef&) = delete;
    ~ShellItemRef() { if (item_) item_->Release(); }
    IShellItem* get() const { return item_; }
    IShellItem** put() { if (item_) { item_->Release(); item_ = nullptr; } return &item_; }
private:
    IShellItem* item_ = nullptr;
};

std::wstring ShellParsingName(const std::wstring& path) {
    return IsShellItemId(path) ? path : pulse::path::StripExtendedPathPrefix(path);
}

HRESULT OpenShellItem(const std::wstring& path, ShellItemRef& out) {
    return shell_id::CreateItem(ShellParsingName(path), IID_PPV_ARGS(out.put()));
}

std::wstring ShellItemName(IShellItem* item, SIGDN form) {
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(form, &raw)) || !raw) return {};
    std::wstring text(raw);
    CoTaskMemFree(raw);
    return text;
}

ShellEntry ReadShellEntry(IShellItem* item) {
    ShellEntry entry;
    SFGAOF attrs = 0;
    item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attrs);
    entry.directory = (attrs & SFGAO_FOLDER) && !(attrs & SFGAO_STREAM);
    IShellItem2* item2 = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&item2))) && item2) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(item2->GetString(kKeyFileName, &raw)) && raw) {
            entry.name = raw;
            CoTaskMemFree(raw);
        }
        ULONGLONG size = 0;
        if (!entry.directory && SUCCEEDED(item2->GetUInt64(kKeySize, &size))) entry.bytes = size;
        item2->GetFileTime(kKeyDateModified, &entry.modified);
        item2->Release();
    }
    if (entry.name.empty()) entry.name = ShellItemName(item, SIGDN_PARENTRELATIVEEDITING);
    if (entry.name.empty()) entry.name = ShellItemName(item, SIGDN_NORMALDISPLAY);
    return entry;
}

std::wstring LowerName(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

// The children of a phone folder by lower-case name: what a copy into it
// would collide with.
std::unordered_map<std::wstring, ShellEntry> ListShellChildren(IShellItem* folder,
                                                               const std::atomic<bool>& cancel) {
    std::unordered_map<std::wstring, ShellEntry> out;
    IEnumShellItems* children = nullptr;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&children))) || !children)
        return out;
    for (;;) {
        if (cancel.load()) break;
        IShellItem* child = nullptr;
        if (children->Next(1, &child, nullptr) != S_OK || !child) break;
        ShellEntry entry = ReadShellEntry(child);
        child->Release();
        if (!entry.name.empty()) out.emplace(LowerName(entry.name), std::move(entry));
    }
    children->Release();
    return out;
}

// Whether `item` sits right in `folder`. Compared as shell items: a phone
// item's id is its ID list, with no path to cut a parent from.
bool ShellItemInFolder(IShellItem* item, IShellItem* folder) {
    if (!item || !folder) return false;
    IShellItem* parent = nullptr;
    if (FAILED(item->GetParent(&parent)) || !parent) return false;
    int order = 1;
    const bool same = SUCCEEDED(parent->Compare(folder, SICHINT_CANONICAL, &order)) && order == 0;
    parent->Release();
    return same;
}

// 保留两者 in a phone folder: the name File Explorer would give the copy
// (\"a - 副本.jpg\", \"a - 副本 (2).jpg\"), checked against the folder's names.
std::wstring KeepBothShellName(const std::wstring& name, bool directory,
                               const std::function<bool(const std::wstring&)>& taken) {
    std::wstring stem = name;
    std::wstring extension;
    if (!directory) {
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) {
            stem = name.substr(0, dot);
            extension = name.substr(dot);
        }
    }
    // The name the shell gives a kept copy (FOF_RENAMEONCOLLISION), as in
    // Explorer's 保留两个文件: "a (2).jpg", "a (3).jpg"...
    for (unsigned index = 2; index < 10000; ++index) {
        std::wstring candidate = stem + L" (" + std::to_wstring(index) + L")" + extension;
        if (!taken(candidate)) return candidate;
    }
    return stem + L" (" + std::to_wstring(GetTickCount64()) + L")" + extension;
}

} // namespace

void OpsManager::CompleteShellRequest(const OpRequest& req, uint64_t task_id,
                                      std::vector<std::wstring> destinations) {
    CompletedOperation completed;
    completed.task_id = task_id;
    completed.type = req.type;
    // Local targets of a copy off the phone are real paths: the arrivals are
    // selected like any paste. Shell ids are never remapped as paths.
    completed.refresh_only = destinations.empty();
    if (!completed.refresh_only) {
        for (const auto& source : req.sources) completed.sources.push_back(source);
        completed.destinations = std::move(destinations);
    }
    completed.refresh_directories = req.refresh_directories;
    if (!IsShellItemId(req.dest_dir) && !req.dest_dir.empty())
        completed.refresh_directories.push_back(req.dest_dir);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completions_.push_back(std::move(completed));
    }
    // The pane on the phone folder re-lists itself (like a phone verb).
    ctx_invoke_done_.fetch_add(1, std::memory_order_acq_rel);
}

void OpsManager::RunShellTransfer(const OpRequest& req, uint64_t task_id) {
    {
        std::scoped_lock lock(mutex_, transfer_control_mutex_);
        const auto* item = active_item_ && active_item_->seq == task_id ? &*active_item_ : nullptr;
        transfer_cancel_.store(item && item->transfer_cancelled);
        transfer_pause_.store(false);
        pending_conflict_.reset();
        resolved_conflict_token_ = 0;
        transfer_active_.store(true);
        shell_cancel_requested_ = item && item->cancel_requested;
    }
    shell_activity_tick_ = GetTickCount64();
    SetStatus([&](OpStatus& st) {
        st.active = true;
        st.can_pause = false;  // the shell's copy cannot pause
        st.pause_requested = false;
        st.authorization = AuthorizationState::None;
        st.can_skip_authorization = false;
        st.type = req.type;
        st.task_id = task_id;
        st.phase = OpPhase::Scanning;
        st.percent = -1.0f;
        st.failures.clear();
        st.failed_items = 0;
        st.summary = l10n::IsChinese()
            ? std::wstring(l10n::Cn(L"正在准备")) + OpVerb(req.type) + L"…"
            : std::wstring(OpGerund(req.type)) + L" \u2014 preparing\u2026";
        st.last_error.clear();
        st.locked_path.clear();
        st.lock_owners.clear();
        st.source_label.clear();
        st.destination_label.clear();
        st.destination_dir = IsShellItemId(req.dest_dir) ? std::wstring() : req.dest_dir;
        st.current_item.clear();
        st.total_bytes = st.transferred_bytes = 0;
        st.total_items = st.completed_items = 0;
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.items_per_second = st.peak_items_per_second = 0.0;
        st.speed_basis = OpSpeedBasis::Bytes;
        st.eta_seconds = 0;
    });
    const auto stop_requested = [&] {
        return transfer_cancel_.load() || shell_cancel_requested_.load() || stopping_.load();
    };
    OpRequest shown = req;  // names instead of shell ids, for the summaries
    const auto finish = [&](OpPhase phase, std::wstring error) {
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.pause_requested = false;
            st.completed_ops++;
            st.bytes_per_second = 0.0;
            st.eta_seconds = 0;
            st.phase = phase;
            if (phase == OpPhase::Completed) {
                st.percent = 100.0f;
                st.transferred_bytes = st.total_bytes;
                st.completed_items = (std::max)(st.completed_items, st.total_items);
                st.summary = Describe(shown) + l10n::Get(l10n::StringId::Completed).c_str();
                st.last_error.clear();
            } else if (error.empty()) {
                st.last_error = l10n::Get(l10n::StringId::Canceled).c_str();
                st.summary = std::wstring(OpVerb(req.type)) + l10n::Get(l10n::StringId::Canceled_2).c_str();
            } else {
                st.last_error = std::move(error);
                st.summary = std::wstring(OpVerb(req.type)) + l10n::Get(l10n::StringId::Failed).c_str();
            }
        });
        {
            std::lock_guard<std::mutex> lock(transfer_control_mutex_);
            pending_conflict_.reset();
        }
        transfer_active_.store(false);
        current_req_id_.store(0);
        shell_cancel_requested_ = false;
    };

    ShellItemRef dest;
    const bool dest_shell = IsShellItemId(req.dest_dir);
    HRESULT hr = req.dest_dir.empty() ? E_INVALIDARG : OpenShellItem(req.dest_dir, dest);
    if (FAILED(hr)) {
        diagnostics::runtime::Event("shell_transfer_failed", {{"stage", 1},
            {"hresult", static_cast<uint32_t>(hr)}, {"dest_shell", dest_shell}});
        finish(OpPhase::Failed, Win32Message(static_cast<DWORD>(HRESULT_CODE(hr))));
        return;
    }
    shown.dest_dir = dest_shell ? ShellItemName(dest.get(), SIGDN_DESKTOPABSOLUTEEDITING) : req.dest_dir;
    if (shown.dest_dir.empty()) shown.dest_dir = ShellItemName(dest.get(), SIGDN_NORMALDISPLAY);
    std::unordered_map<std::wstring, ShellEntry> dest_children;
    if (dest_shell) dest_children = ListShellChildren(dest.get(), transfer_cancel_);
    const auto dest_entry = [&](const std::wstring& name, ShellEntry* out) {
        if (dest_shell) {
            const auto found = dest_children.find(LowerName(name));
            if (found == dest_children.end()) return false;
            if (out) *out = found->second;
            return true;
        }
        const std::wstring path = JoinPath(req.dest_dir, name);
        bool directory = false;
        if (!PathExists(path, &directory)) return false;
        if (out) {
            TransferEntry metadata;
            out->name = name;
            out->directory = directory;
            if (ReadEntryMetadata(path, metadata)) {
                out->bytes = metadata.bytes;
                out->modified = metadata.modified;
            }
        }
        return true;
    };

    struct Planned {
        std::wstring source;
        ShellEntry entry;
        std::wstring display;      // where the dialog says it comes from
        bool conflict = false;
        bool self_copy = false;    // copied into its own folder: \"a - 副本\"
        bool keep_both = false;    // 保留两者: the shell names it \"a (2)\"
        std::wstring new_name;     // the name it is expected to get
    };
    std::vector<Planned> planned;
    planned.reserve(req.sources.size());
    for (const auto& source : req.sources) {
        if (stop_requested()) break;
        ShellItemRef item;
        hr = OpenShellItem(source, item);
        if (FAILED(hr)) {
            diagnostics::runtime::Event("shell_transfer_failed", {{"stage", 2},
                {"hresult", static_cast<uint32_t>(hr)}, {"dest_shell", dest_shell}});
            finish(OpPhase::Failed, Win32Message(static_cast<DWORD>(HRESULT_CODE(hr))) + L" | " +
                                        (IsShellItemId(source) ? std::wstring() : source));
            return;
        }
        Planned p;
        p.source = source;
        p.entry = ReadShellEntry(item.get());
        p.display = IsShellItemId(source) ? ShellItemName(item.get(), SIGDN_DESKTOPABSOLUTEEDITING) : source;
        if (p.display.empty()) p.display = p.entry.name;
        const bool same_folder = ShellItemInFolder(item.get(), dest.get());
        if (same_folder) {
            if (req.type == OpType::Move) continue;  // already there
            p.self_copy = true;
        } else {
            p.conflict = dest_entry(p.entry.name, nullptr);
        }
        planned.push_back(std::move(p));
    }
    diagnostics::runtime::Event("shell_transfer_plan", {{"items", planned.size()},
        {"self_copy", static_cast<uint64_t>(std::count_if(planned.begin(), planned.end(),
                                                         [](const Planned& p) { return p.self_copy; }))},
        {"conflicts", static_cast<uint64_t>(std::count_if(planned.begin(), planned.end(),
                                                         [](const Planned& p) { return p.conflict; }))},
        {"dest_shell", dest_shell}});
    if (stop_requested()) {
        finish(OpPhase::Failed, {});
        return;
    }
    shown.sources.clear();
    for (const auto& p : planned) shown.sources.push_back(p.entry.name);
    if (shown.sources.empty() && !req.sources.empty()) shown.sources.push_back(FileName(req.sources.front()));
    SetStatus([&](OpStatus& st) {
        st.source_label = shown.sources.empty() ? std::wstring() : shown.sources.front();
        st.destination_label = FileName(shown.dest_dir);
        if (st.destination_label.empty()) st.destination_label = shown.dest_dir;
    });

    // Pulse's own conflict dialog, one item at a time (全部应用 repeats it).
    size_t conflicts_left = static_cast<size_t>(std::count_if(planned.begin(), planned.end(),
        [](const Planned& p) { return p.conflict; }));
    size_t conflicts_asked = 0;
    bool apply_all = false;
    ConflictChoice repeated = ConflictChoice::Replace;
    bool cancelled = false;
    std::vector<Planned> run;
    run.reserve(planned.size());
    for (auto& p : planned) {
        if (!p.conflict) {
            run.push_back(std::move(p));
            continue;
        }
        ConflictChoice choice = repeated;
        if (!apply_all) {
            ConflictItemInfo info;
            info.task_id = task_id;
            info.source = p.display;
            info.destination = JoinPath(shown.dest_dir, p.entry.name);
            info.source_size = p.entry.bytes;
            info.source_modified = p.entry.modified;
            info.source_is_directory = p.entry.directory;
            ShellEntry existing;
            if (dest_entry(p.entry.name, &existing)) {
                info.destination_size = existing.bytes;
                info.destination_modified = existing.modified;
                info.destination_is_directory = existing.directory;
            }
            info.remaining = conflicts_left;
            info.ordinal = ++conflicts_asked;
            info.type = req.type;
            info.keep_both_name = KeepBothShellName(p.entry.name, p.entry.directory,
                [&](const std::wstring& name) { return dest_entry(name, nullptr); });
            {
                std::lock_guard<std::mutex> lock(transfer_control_mutex_);
                info.token = next_conflict_token_++;
                pending_conflict_ = info;
                resolved_conflict_token_ = 0;
            }
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::WaitingForConflict;
                st.current_item = p.entry.name;
                st.summary = l10n::Get(l10n::StringId::WaitingForFileConflictsToBeResolved).c_str();
            });
            bool selected_apply_all = false;
            {
                std::unique_lock<std::mutex> lock(transfer_control_mutex_);
                transfer_control_cv_.wait(lock, [&] {
                    return transfer_cancel_.load() || stopping_.load() || resolved_conflict_token_ == info.token;
                });
                if (transfer_cancel_.load() || stopping_.load()) {
                    pending_conflict_.reset();
                    cancelled = true;
                    break;
                }
                choice = resolved_conflict_choice_;
                selected_apply_all = resolved_conflict_apply_all_;
                pending_conflict_.reset();
            }
            if (choice == ConflictChoice::Cancel) {
                cancelled = true;
                transfer_cancel_.store(true);
                break;
            }
            if (selected_apply_all) {
                apply_all = true;
                repeated = choice;
            }
        }
        --conflicts_left;
        if (choice == ConflictChoice::Skip) continue;
        if (choice == ConflictChoice::KeepBoth) {
            p.keep_both = true;
            p.new_name = KeepBothShellName(p.entry.name, p.entry.directory,
                [&](const std::wstring& name) {
                    if (dest_entry(name, nullptr)) return true;
                    return std::any_of(run.begin(), run.end(), [&](const Planned& other) {
                        return _wcsicmp(other.new_name.c_str(), name.c_str()) == 0;
                    });
                });
        }
        run.push_back(std::move(p));
    }
    if (cancelled || stop_requested()) {
        finish(OpPhase::Failed, {});
        return;
    }
    if (run.empty()) {
        finish(OpPhase::Completed, {});
        return;
    }

    uint64_t total_bytes = 0;
    bool has_directory = false;
    for (const auto& p : run) {
        total_bytes += p.entry.bytes;
        has_directory = has_directory || p.entry.directory;
    }
    SetStatus([&](OpStatus& st) {
        st.phase = OpPhase::Running;
        st.percent = 0.0f;
        st.total_items = run.size();
        st.completed_items = 0;
        // A folder's size is not known without walking the phone: count
        // items then; plain files show bytes, speed and the time left.
        st.speed_basis = has_directory || total_bytes == 0 ? OpSpeedBasis::Items : OpSpeedBasis::Bytes;
        st.total_bytes = st.speed_basis == OpSpeedBasis::Bytes ? total_bytes : 0;
        st.summary = ProgressSummary(req.type, run.size());
        st.current_item = run.front().entry.name;
    });

    // Plain, replaced and kept (保留两者) items go in one operation: a kept
    // one is given its \"a (2)\" name, which the shell also picks itself where
    // the original name is taken (Explorer's 保留两个文件). Copies into their
    // own folder go in a second with FOF_RENAMEONCOLLISION: \"a - 副本\", as a
    // paste in the same folder in Explorer.
    auto& client = ipc::ShellClient::Instance();
    const uint32_t op = req.type == OpType::Move ? ipc::STO_MOVE : ipc::STO_COPY;
    std::wstring failure;
    bool any_done = false;
    for (int pass = 0; pass < 2 && failure.empty() && !cancelled; ++pass) {
        std::vector<std::wstring> sources;
        std::vector<std::wstring> names;
        for (const auto& p : run) {
            if (p.self_copy != (pass == 1)) continue;
            sources.push_back(ShellParsingName(p.source));
            names.push_back(p.keep_both ? p.new_name : std::wstring());
        }
        if (sources.empty()) continue;
        // A phone (WPD) refuses a copy into the item's own folder with
        // FOF_RENAMEONCOLLISION (ERROR_NOT_SUPPORTED); Explorer pastes there
        // through the folder's drop target, which names it "a - 副本".
        const bool shell_paste = pass == 1 && dest_shell;
        if (shell_paste) sources.insert(sources.begin(), ShellParsingName(req.dest_dir));
        const auto accepted = [this](uint32_t request) { RegisterShellRequest(request); };
        const uint32_t id = shell_paste
            ? client.ShellVerb(sources, 0, true, L"drop:copy", accepted)
            : client.ShellTransfer(op, pass == 1 ? ipc::STF_RENAME_ON_COLLISION : 0u,
                                   ShellParsingName(req.dest_dir), sources, names, accepted);
        if (id == 0) {
            failure = l10n::Get(l10n::StringId::TheFileOperationServiceIsNotRunning).c_str();
            break;
        }
        if (stop_requested()) client.Cancel(id);
        uint32_t result = 0;
        bool request_cancelled = false;
        std::wstring error;
        if (!WaitShellDone(id, result, request_cancelled, error)) {
            // WaitShellDone published the interruption; end the task with it.
            transfer_active_.store(false);
            return;
        }
        diagnostics::runtime::Event("file_operation_shell_transfer", {{"task", task_id},
            {"request", id}, {"hresult", result}, {"cancelled", request_cancelled},
            {"items", sources.size()}, {"pass", static_cast<uint64_t>(pass)}, {"drop", shell_paste}});
        if (request_cancelled) cancelled = true;
        else if (FAILED(static_cast<HRESULT>(result)))
            failure = error.empty() ? l10n::Get(l10n::StringId::OpFailed).c_str() : error;
        else any_done = true;
    }
    if (any_done || cancelled || !failure.empty()) {
        // Even a cancelled or failed copy may have written part of it.
        std::vector<std::wstring> destinations;
        if (any_done && !dest_shell && failure.empty() && !cancelled)
            for (const auto& p : run)
                if (!p.self_copy) destinations.push_back(JoinPath(req.dest_dir, p.new_name.empty() ? p.entry.name : p.new_name));
        CompleteShellRequest(req, task_id, std::move(destinations));
    }
    if (cancelled) finish(OpPhase::Failed, {});
    else if (!failure.empty()) finish(OpPhase::Failed, failure);
    else finish(OpPhase::Completed, {});
}

void OpsManager::RunShellItemOp(const OpRequest& req, uint64_t task_id) {
    const bool create = req.type == OpType::CreateFolder || req.type == OpType::CreateTextFile;
    {
        std::lock_guard lock(mutex_);
        shell_cancel_requested_ = active_item_ && active_item_->seq == task_id && active_item_->cancel_requested;
    }
    shell_activity_tick_ = GetTickCount64();
    OpRequest shown = req;
    if (create) {
        shown.sources = {req.new_name};
    } else {
        // The summary names the first item, not its shell id.
        shown.sources.clear();
        for (size_t i = 0; i < req.sources.size(); ++i) {
            std::wstring name;
            if (i == 0) {
                ShellItemRef item;
                if (SUCCEEDED(OpenShellItem(req.sources[i], item))) name = ReadShellEntry(item.get()).name;
            }
            shown.sources.push_back(name.empty() ? FileName(req.sources[i]) : name);
        }
    }
    SetStatus([&](OpStatus& st) {
        st.active = true;
        st.can_pause = false;
        st.authorization = AuthorizationState::None;
        st.can_skip_authorization = false;
        st.type = req.type;
        st.task_id = task_id;
        st.phase = OpPhase::Running;
        st.percent = 0.0f;
        st.summary = Describe(shown);
        st.last_error.clear();
        st.failures.clear();
        st.failed_items = 0;
        st.locked_path.clear();
        st.lock_owners.clear();
        st.source_label = shown.sources.empty() ? L"" : shown.sources.front();
        st.destination_label.clear();
        st.current_item = st.source_label;
        st.total_bytes = st.transferred_bytes = 0;
        st.total_items = create ? 1 : req.sources.size();
        st.completed_items = 0;
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.items_per_second = st.peak_items_per_second = 0.0;
        st.speed_basis = create ? OpSpeedBasis::None : OpSpeedBasis::Items;
        st.eta_seconds = 0;
    });
    const uint32_t op = req.type == OpType::CreateFolder ? ipc::STO_NEW_FOLDER
                      : req.type == OpType::CreateTextFile ? ipc::STO_NEW_FILE
                      : ipc::STO_DELETE;
    std::vector<std::wstring> sources;
    if (!create)
        for (const auto& source : req.sources) sources.push_back(ShellParsingName(source));
    const uint32_t id = shell_cancel_requested_.load() || stopping_.load() ? 0u
        : ipc::ShellClient::Instance().ShellTransfer(op, 0u, create ? ShellParsingName(req.dest_dir) : std::wstring(),
              sources, create ? std::vector<std::wstring>{req.new_name} : std::vector<std::wstring>{},
              [this](uint32_t request) { RegisterShellRequest(request); });
    uint32_t hr = 0;
    bool cancelled = shell_cancel_requested_.load() || stopping_.load();
    std::wstring error;
    if (id != 0) {
        if (shell_cancel_requested_.load()) ipc::ShellClient::Instance().Cancel(id);
        if (!WaitShellDone(id, hr, cancelled, error)) return;
    } else if (!cancelled) {
        hr = static_cast<uint32_t>(E_FAIL);
        error = l10n::Get(l10n::StringId::TheFileOperationServiceIsNotRunning).c_str();
    }
    const bool ok = id != 0 && SUCCEEDED(static_cast<HRESULT>(hr)) && !cancelled;
    // A new folder is named by its Pulse path (the phone folder's, plus the
    // leaf), so the rename-in-place intent finds the row once it is listed.
    std::vector<std::wstring> destinations;
    if (create && ok && !req.refresh_directories.empty())
        destinations.push_back(JoinPath(req.refresh_directories.front(), req.new_name));
    if (id != 0) CompleteShellRequest(req, task_id, std::move(destinations));
    current_req_id_.store(0);
    shell_cancel_requested_ = false;
    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.percent = -1.0f;
        st.completed_ops++;
        if (ok) {
            st.phase = OpPhase::Completed;
            st.completed_items = st.total_items;
            st.summary = Describe(shown) + l10n::Get(l10n::StringId::Completed).c_str();
        } else if (cancelled) {
            st.phase = OpPhase::Failed;
            st.last_error = l10n::Get(l10n::StringId::Canceled).c_str();
        } else {
            st.phase = OpPhase::Failed;
            st.last_error = error.empty() ? l10n::Get(l10n::StringId::OpFailed).c_str() : error;
        }
    });
}

void OpsManager::RunTransfer(const OpRequest& req, uint64_t task_id) {
    // A phone's items have no path for Pulse's engine: Explorer's does it.
    if (IsShellTransferRequest(req)) {
        RunShellTransfer(req, task_id);
        return;
    }
#ifdef PULSE_ELEVATED_TEST_CLIENT
    ObserveTransferStartForReview(*this, task_id);
#endif
    {
        std::scoped_lock lock(mutex_, transfer_control_mutex_);
        const auto* item = active_item_ && active_item_->seq == task_id ? &*active_item_ : nullptr;
        transfer_cancel_.store(item && item->transfer_cancelled);
        transfer_pause_.store(item && item->transfer_paused && !item->transfer_cancelled);
        pending_conflict_.reset();
        resolved_conflict_token_ = 0;
        transfer_active_.store(true);
    }

    SetStatus([&](OpStatus& st) {
        st.active = true;
        st.can_pause = true;
        st.pause_requested = transfer_pause_.load();
        st.authorization = AuthorizationState::None;
        st.can_skip_authorization = false;
        st.type = req.type;
        st.task_id = task_id;
        st.phase = OpPhase::Scanning;
        st.percent = -1.0f;
        st.failures.clear();
        st.failed_items = 0;
        st.summary = l10n::IsChinese()
            ? std::wstring(l10n::Cn(L"正在准备")) + OpVerb(req.type) + L"…"
            : std::wstring(OpGerund(req.type)) + L" \u2014 preparing\u2026";
        st.last_error.clear();
        st.locked_path.clear();
        st.lock_owners.clear();
        st.source_label = req.sources.empty() ? L"" : FileName(req.sources.front());
        st.destination_label = FileName(req.dest_dir);
        if (st.destination_label.empty()) st.destination_label = req.dest_dir;
        st.destination_dir = req.dest_dir;
        st.current_item.clear();
        st.total_bytes = st.transferred_bytes = 0;
        st.total_items = st.completed_items = 0;
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.items_per_second = st.peak_items_per_second = 0.0;
        st.speed_basis = OpSpeedBasis::Bytes;
        st.eta_seconds = 0;
    });

    // Time spent paused or waiting on a conflict. The rate clock skips it, so
    // after a resume the speed and estimate carry on where they were instead
    // of dropping to 0 and warming up again.
    std::atomic<ULONGLONG> held_ms{ 0 };
    const auto rate_now = [&] { return GetTickCount64() - held_ms.load(); };
    std::wstring failure;
    HRESULT failure_hr = S_OK;   // lets a lock-style failure be traced to its owner
    std::optional<LockReport> move_lock_report;
    bool cancelled = transfer_cancel_.load() || stopping_.load();
    const auto stop_requested = [&] {
        cancelled = cancelled || transfer_cancel_.load() || stopping_.load();
        if (!cancelled && transfer_pause_.load()) {
            const auto previous = Status();
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Paused;
                st.summary = std::wstring(OpVerb(req.type)) + l10n::Get(l10n::StringId::Paused).c_str();
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            const ULONGLONG held_from = GetTickCount64();
            std::unique_lock lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] {
                return transfer_cancel_.load() || stopping_.load() || !transfer_pause_.load();
            });
            cancelled = transfer_cancel_.load() || stopping_.load();
            lock.unlock();
            held_ms += GetTickCount64() - held_from;
            if (!cancelled) SetStatus([&](OpStatus& st) {
                st.phase = previous.phase;
                st.summary = previous.summary;
                st.bytes_per_second = previous.bytes_per_second;
                st.eta_seconds = previous.eta_seconds;
            });
        }
        return cancelled;
    };
    if (!req.new_name.empty() && (req.type != OpType::Move || req.sources.size() != 1 ||
        !pulse::IsRenameFilename(req.new_name))) {
        failure_hr = E_INVALIDARG;
        failure = l10n::Get(l10n::StringId::TheMoveDestinationNameIsInvalid).c_str();
    }
    const auto root_name = [&](const std::wstring& source) {
        return req.new_name.empty() ? FileName(source) : req.new_name;
    };
    // Reject a folder targeted at itself or its own subtree before any fast
    // rename or authorization probe: MoveFileEx reports that as access denied.
    if (failure.empty() && !req.dest_dir.empty()) {
        for (const auto& source : req.sources) {
            std::wstring folder = source;
            while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
            if (!folder.empty() && StartsWithPath(req.dest_dir, folder)) {
                failure_hr = HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
                failure = l10n::Get(l10n::StringId::CannotCopyOrMoveAFolderIntoItself).c_str() + source;
                break;
            }
        }
    }
    // Decide before the fast move mutates any source. A whole-request retry
    // after partial completion could duplicate copies or move items twice.
#ifndef PULSE_ELEVATED_HOST
    if (failure.empty() && !stop_requested() &&
        !req.sources.empty() && !req.dest_dir.empty() &&
        NeedsShellTransfer(req.sources, req.dest_dir, req.type == OpType::Move, req.new_name)) {
        RunAuthorizedTransfer(req, task_id);
        return;
    }
#endif


    std::vector<TransferEntry> entries;
    std::vector<std::wstring> completed_sources;
    std::vector<std::wstring> completed_destinations;
    struct ReplacementBackup {
        std::wstring original;
        std::wstring backup;
    };
    std::vector<ReplacementBackup> replacement_backups;

    if (req.sources.empty() || req.dest_dir.empty()) {
        failure = l10n::Get(l10n::StringId::TheCopyOrMoveRequestIsMissingASourceOrDestination).c_str();
    }
    if (failure.empty() && !stop_requested() && req.is_undo && req.type == OpType::Move) {
        // A merged move can remove the now-empty source directories. Its
        // per-file undo must recreate them before attempting the fast rename.
        EnsureDirectories(req.dest_dir, failure);
    }

    // Source folders a rename into dest_dir has found on another volume.
    // Whether a rename can work depends only on the two parent folders, so
    // the other items of such a folder go straight to the per-file path
    // instead of each paying for a rename that fails the same way.
    std::unordered_set<std::wstring> rename_other_volume;
    // A single unobstructed same-volume move is an atomic rename and should
    // not be expanded into per-file work.
    if (failure.empty() && !stop_requested() && req.type == OpType::Move && req.sources.size() == 1) {
        const std::wstring destination = JoinPath(req.dest_dir, root_name(req.sources.front()));
        if (!PathExists(destination) && !stop_requested()) {
            if (MoveFileExW(req.sources.front().c_str(), destination.c_str(), 0)) {
                completed_sources = req.sources;
                moved_sources_ = req.sources;
                completed_destinations.push_back(destination);
                PushUndo(req, task_id, &completed_destinations);
                SetStatus([&](OpStatus& st) {
                    st.active = false;
                    st.pause_requested = false;
                    st.phase = OpPhase::Completed;
                    st.percent = 100.0f;
                    st.total_items = st.completed_items = 1;
                    st.summary = Describe(req) + l10n::Get(l10n::StringId::Completed).c_str();
                    st.completed_ops++;
                });
                transfer_active_.store(false);
                return;
            }
            const DWORD move_error = GetLastError();
#ifndef PULSE_ELEVATED_HOST
            if (move_error == ERROR_ACCESS_DENIED || move_error == ERROR_PRIVILEGE_NOT_HELD ||
                move_error == ERROR_ELEVATION_REQUIRED) {
                // A locked descendant also makes a directory rename return access
                // denied. Elevation cannot release that handle; retain lock retry.
                move_lock_report = ProbeLock(req, task_id, HRESULT_FROM_WIN32(move_error),
                    Win32Message(move_error) + L" | " + req.sources.front());
                if (move_lock_report->Empty()) {
                    RunAuthorizedTransfer(req, task_id);
                    return;
                }
            }
#endif
            if (move_error != ERROR_NOT_SAME_DEVICE) {
                failure_hr = HRESULT_FROM_WIN32(move_error);
                failure = Win32Message(move_error) + L" | " + req.sources.front();
            } else {
                rename_other_volume.insert(pulse::path::FoldedKey(ParentOf(req.sources.front())));
            }
        }
    }

    namespace fsys = std::filesystem;
    // Where each source's own entry is (npos: nothing to do for it). The
    // commit below looks roots up here instead of searching every entry.
    std::vector<size_t> root_entries(req.sources.size(), std::wstring::npos);
    if (failure.empty() && !stop_requested()) {
        const SourceListing listing(req.sources);
        // Names a rename below brings in are added, so a later source with
        // the same name still finds its target taken.
        TargetListing target_listing(req.dest_dir, req.sources.size());
        for (size_t source_index = 0; source_index < req.sources.size(); ++source_index) {
            const std::wstring& source = req.sources[source_index];
#ifdef PULSE_ELEVATED_TEST_CLIENT
            ObserveTransferScanForReview(*this, source);
#endif
            if (stop_requested()) break;
            TransferEntry root;
            root.source = source;
            root.destination = JoinPath(req.dest_dir, root_name(source));
            if (const ListedSource* listed = listing.Find(source)) {
                root.directory = (listed->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                root.reparse = (listed->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                root.attributes = listed->attributes;
                root.created = listed->created;
                root.accessed = listed->accessed;
                root.modified = listed->modified;
                root.bytes = root.directory ? 0 : listed->bytes;
            } else if (!ReadEntryMetadata(source, root)) {
                {
                    const DWORD code = GetLastError();
                    failure_hr = HRESULT_FROM_WIN32(code);
                    failure = Win32Message(code) + L" | " + source;
                }
                break;
            }
            // Opening both ends to compare them is only needed when the target
            // name is taken; most targets are new.
            bool destination_exists = !target_listing.SurelyAbsent(root_name(source)) &&
                PathExists(root.destination);
            const bool same_location =
                _wcsicmp(root.source.c_str(), root.destination.c_str()) == 0 ||
                (destination_exists && SameFileObject(root.source, root.destination));
            if (same_location && req.type == OpType::Move) {
                // Dropping an item onto its current folder is a no-op, like Explorer.
                continue;
            }
            if (same_location && req.type == OpType::Copy) {
                root.destination = UniqueCopyPath(root.destination, root.directory);
                destination_exists = PathExists(root.destination);
            }
            if (root.directory && StartsWithPath(req.dest_dir, root.source)) {
                failure = l10n::Get(l10n::StringId::CannotCopyOrMoveAFolderIntoItself).c_str() + source;
                break;
            }
            root.destination_preexisting = destination_exists;
            // On one volume a move is a rename however many items are dropped
            // (#60). Recorded as completed at once so a later failure still
            // offers undo for it; anything a rename cannot do (another volume,
            // a lock) takes the per-file path below.
            if (stop_requested()) break;
            if (req.type == OpType::Move && !root.destination_preexisting) {
                std::wstring parent_key = pulse::path::FoldedKey(ParentOf(source));
                if (!rename_other_volume.contains(parent_key)) {
                    if (MoveFileExW(source.c_str(), root.destination.c_str(), 0)) {
                        target_listing.Added(root_name(source));
                        root.renamed = true;
                        root.completed = true;
                        root.source_removed = true;
                        root_entries[source_index] = entries.size();
                        entries.push_back(root);
                        continue;
                    }
                    if (GetLastError() == ERROR_NOT_SAME_DEVICE) rename_other_volume.insert(std::move(parent_key));
                }
            }
            root_entries[source_index] = entries.size();
            entries.push_back(root);
            if (!root.directory || root.reparse) continue;

            // One FindFirstFileEx pass per folder: attributes, size and times
            // arrive with the listing, so nothing is queried per entry. Depth
            // first and pre-order, a folder before what it contains, which the
            // copy loop and the keep-both rename below rely on. Links are
            // copied as links and never entered.
            struct OpenFolder {
                HANDLE find = INVALID_HANDLE_VALUE;
                std::wstring source;
                std::wstring destination;
            };
            std::vector<OpenFolder> folders;
            WIN32_FIND_DATAW pending_find{};
            bool has_pending_find = false;
            const auto open_folder = [&](const std::wstring& folder_source,
                                         const std::wstring& folder_destination, bool is_root) {
                WIN32_FIND_DATAW data{};
                const std::wstring pattern = JoinPath(LongPathForApi(folder_source), L"*");
                HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                               FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (find == INVALID_HANDLE_VALUE) {
                    const DWORD code = GetLastError();
                    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_NO_MORE_FILES) return true;  // empty
                    failure_hr = HRESULT_FROM_WIN32(code);
                    failure = (is_root ? l10n::Get(l10n::StringId::CouldNotReadTheSourceFolder).c_str()
                                       : l10n::Get(l10n::StringId::CouldNotScanTheSourceFolder).c_str()) +
                        folder_source + L" | " + Win32Message(code);
                    return false;
                }
                folders.push_back({find, folder_source, folder_destination});
                // The first result is consumed by the loop below.
                pending_find = data;
                has_pending_find = true;
                return true;
            };
            bool scan_ok = open_folder(source, root.destination, true);
            while (scan_ok && !folders.empty()) {
                if (stop_requested()) break;
                WIN32_FIND_DATAW data{};
                if (has_pending_find) {
                    data = pending_find;
                    has_pending_find = false;
                } else if (!FindNextFileW(folders.back().find, &data)) {
                    const DWORD code = GetLastError();
                    FindClose(folders.back().find);
                    if (code != ERROR_NO_MORE_FILES) {
                        failure_hr = HRESULT_FROM_WIN32(code);
                        failure = l10n::Get(l10n::StringId::CouldNotScanTheSourceFolder).c_str() +
                            folders.back().source + L" | " + Win32Message(code);
                        folders.pop_back();
                        scan_ok = false;
                        break;
                    }
                    folders.pop_back();
                    continue;
                }
                const wchar_t* name = data.cFileName;
                if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
                TransferEntry child;
                child.source = JoinPath(folders.back().source, name);
                child.destination = JoinPath(folders.back().destination, name);
#ifdef PULSE_ELEVATED_TEST_CLIENT
                ObserveTransferScanForReview(*this, child.source);
#endif
                child.attributes = data.dwFileAttributes;
                child.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                child.reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                child.created = data.ftCreationTime;
                child.accessed = data.ftLastAccessTime;
                child.modified = data.ftLastWriteTime;
                child.bytes = child.directory ? 0
                    : (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                const bool descend = child.directory && !child.reparse;
                entries.push_back(std::move(child));
                if (descend) {
                    const TransferEntry& folder = entries.back();
                    scan_ok = open_folder(folder.source, folder.destination, false);
                }
            }
            for (auto& folder : folders) FindClose(folder.find);
            folders.clear();
            if (!failure.empty() || cancelled) break;
        }
    }

    // Accidental same-folder drops leave zero work. Do not flash the status bar
    // with "移动 foo → dest 完成" for a no-op Explorer already ignores.
    if (failure.empty() && !stop_requested() && entries.empty()) {
        SetStatus([&](OpStatus& st) {
            st.completed_ops++;
            st.active = false;
            st.pause_requested = false;
            st.phase = OpPhase::Completed;
            st.percent = 100.0f;
            st.summary.clear();
            st.last_error.clear();
            st.current_item.clear();
            st.total_bytes = st.transferred_bytes = 0;
            st.total_items = st.completed_items = 0;
            st.bytes_per_second = st.peak_bytes_per_second = 0.0;
            st.eta_seconds = 0;
        });
        transfer_active_.store(false);
        return;
    }

    uint64_t total_bytes = 0;
    for (const auto& entry : entries) total_bytes += entry.bytes;
    SetStatus([&](OpStatus& st) {
        st.total_bytes = total_bytes;
        st.total_items = entries.size();
        if (failure.empty() && !cancelled) {
            st.phase = OpPhase::Running;
            st.percent = 0.0f;
            st.summary = ProgressSummary(req.type, entries.size());
        }
    });

    bool apply_all = req.collision_policy != CollisionPolicy::System;
    ConflictChoice repeated = req.collision_policy == CollisionPolicy::KeepBoth
        ? ConflictChoice::KeepBoth : ConflictChoice::Replace;
    std::vector<std::wstring> skipped_prefixes;
    std::vector<std::wstring> skipped_sources;
    uint64_t committed_bytes = 0;
    uint64_t published_bytes = 0;
    uint64_t processed_items = 0;
    TransferRateEstimator rate;
    rate.Reset(rate_now(), 0);

    auto publish_progress = [&](const TransferEntry& entry, uint64_t file_done,
                                uint64_t file_total, uint64_t completed_items) {
        const uint64_t bounded_file_done = file_total > 0
            ? (std::min)(file_done, file_total) : file_done;
        uint64_t overall = committed_bytes + bounded_file_done;
        if (overall < committed_bytes) overall = UINT64_MAX;
        overall = (std::max)(published_bytes, overall);
        if (total_bytes > 0) overall = (std::min)(overall, total_bytes);
        published_bytes = overall;
        rate.Observe(rate_now(), overall, total_bytes);
        SetProgressStatus([&](OpStatus& st) {
            st.current_item = FileName(entry.source);
            st.transferred_bytes = overall;
            st.completed_items = completed_items;
            st.bytes_per_second = rate.speed();
            st.peak_bytes_per_second = (std::max)(st.peak_bytes_per_second, rate.speed());
            if (st.total_bytes > 0) {
                st.percent = static_cast<float>((std::min)(100.0,
                    static_cast<double>(overall) * 100.0 / st.total_bytes));
                st.eta_seconds = rate.eta_seconds();
            } else if (st.total_items > 0) {
                st.percent = static_cast<float>(completed_items * 100.0 / st.total_items);
            }
        });
    };

    auto mark_skipped = [&](const TransferEntry& entry) {
        skipped_sources.push_back(entry.source);
        total_bytes -= (std::min)(total_bytes, entry.bytes);
        ++processed_items;
        rate.Observe(rate_now(), published_bytes, total_bytes);
        SetProgressStatus([&](OpStatus& st) {
            st.total_bytes = total_bytes;
            st.transferred_bytes = published_bytes;
            st.completed_items = processed_items;
            st.current_item = FileName(entry.source);
            st.eta_seconds = rate.eta_seconds();
            if (st.total_bytes > 0) {
                st.percent = static_cast<float>((std::min)(100.0,
                    static_cast<double>(published_bytes) * 100.0 / st.total_bytes));
            } else if (st.total_items > 0) {
                st.percent = static_cast<float>(processed_items * 100.0 / st.total_items);
            }
        });
    };

    size_t conflicts_asked = 0;
    auto remaining_conflicts = [&](size_t start) {
        size_t count = 0;
        for (size_t j = start; j < entries.size(); ++j) {
            if (entries[j].renamed) continue;
            bool dest_dir = false;
            if (PathExists(entries[j].destination, &dest_dir) &&
                !(entries[j].directory && !entries[j].reparse && dest_dir)) ++count;
        }
        return count;
    };

    // Folders this task created (nothing inside can pre-exist, so the
    // per-entry existence and same-object probes are skipped) and folders
    // known to exist (no EnsureDirectories walk up the tree per file).
    std::unordered_set<std::wstring> fresh_dirs;
    std::unordered_set<std::wstring> ready_dirs;
    const auto dir_key = [](const std::wstring& dir) {
        std::wstring key = dir;
        while (key.size() > 3 && key.back() == L'\\') key.pop_back();
        if (!key.empty()) CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
        return key;
    };
    // False: `error` says why (message | folder).
    const auto ensure_parent = [&](const std::wstring& dir, std::wstring& error) {
        std::wstring key = dir_key(dir);
        if (ready_dirs.contains(key)) return true;
        if (!EnsureDirectories(dir, error)) return false;
        ready_dirs.insert(std::move(key));
        return true;
    };

    // Small files without a conflict go to a copy pool sized for the storage;
    // everything else stays on this thread, in order.
    std::unique_ptr<SmallCopyPool> pool;
    std::unordered_set<std::wstring> pooled_destinations;  // outside fresh folders
    if (!verify_copies_.load()) {
        size_t small_files = 0;
        for (const auto& candidate : entries)
            if (!candidate.directory && !candidate.reparse && !candidate.renamed &&
                candidate.bytes < kProgressCallbackMinBytes) ++small_files;
        const unsigned threads = small_files >= kPoolMinFiles
            ? TransferThreadCount(entries.empty() ? std::wstring{} : entries.front().source, req.dest_dir) : 1;
        if (threads > 1)
            pool = std::make_unique<SmallCopyPool>(threads, transfer_cancel_, transfer_pause_,
                                                   transfer_control_mutex_, transfer_control_cv_);
    }
    // Items that failed while the task went on (partial failure). A failure
    // about the target rather than the item (IsStopTransferError), or a run
    // of failures with no success between, stops the task instead.
    std::vector<OpFailure> item_failures;
    uint64_t failed_items = 0;
    size_t failures_in_a_row = 0;
    constexpr size_t kStopAfterFailuresInARow = 64;
    std::wstring kept_backup_note;
    // A replaced item whose replacement failed gets its original back now; the
    // task's end would otherwise discard the backup as committed.
    const auto restore_backup = [&](const std::wstring& destination) {
        const auto found = std::find_if(replacement_backups.begin(), replacement_backups.end(),
            [&](const ReplacementBackup& backup) {
                return _wcsicmp(backup.original.c_str(), destination.c_str()) == 0;
            });
        if (found == replacement_backups.end()) return;
        DWORD error = ERROR_SUCCESS;
        if (PathExists(found->original)) error = ERROR_ALREADY_EXISTS;
        else if (!MoveFileExW(found->backup.c_str(), found->original.c_str(), MOVEFILE_WRITE_THROUGH))
            error = GetLastError();
        if (error != ERROR_SUCCESS) {
            kept_backup_note += l10n::Get(l10n::StringId::CouldNotRestore).c_str() + found->original +
                l10n::Get(l10n::StringId::OriginalKeptAt).c_str() + found->backup + L" | " + Win32Message(error);
        }
        replacement_backups.erase(found);  // restored, or kept where the note says
    };
    // Records the failed item and returns true to go on; false: stop (the
    // failure is the task's, as before).
    const auto item_failed = [&](TransferEntry& entry, HRESULT hr, std::wstring reason,
                                 const std::wstring& path) -> bool {
        if (cancelled || transfer_cancel_.load()) { cancelled = true; return false; }
        if (IsStopTransferError(hr) || ++failures_in_a_row >= kStopAfterFailuresInARow) {
            if (failure.empty()) {
                failure_hr = hr;
                failure = reason + L" | " + path;
            }
            return false;
        }
        restore_backup(entry.destination);
        if (item_failures.size() < kMaxReportedFailures)
            item_failures.push_back({entry.source, entry.destination, std::move(reason), hr});
        ++failed_items;
        if (entry.directory) skipped_prefixes.push_back(entry.source);
        mark_skipped(entry);
        return true;
    };

    // Moves whose copy is written out but whose source is still there. The
    // sources go in batches, after one flush per volume has made every copy
    // in the batch durable (FlushCopyToDisk): a power cut leaves a moved file
    // in one place or both, never in neither.
    std::vector<size_t> pending_moves;
    ULONGLONG pending_since = 0;
    std::wstring last_volume_parent, last_volume_root;
    const auto volume_of = [&](const std::wstring& file) {
        const std::wstring parent = ParentOf(file);
        if (_wcsicmp(parent.c_str(), last_volume_parent.c_str()) != 0) {
            std::vector<wchar_t> root(32768);
            last_volume_root = GetVolumePathNameW(LongPathForApi(file).c_str(), root.data(),
                                                  static_cast<DWORD>(root.size())) ? root.data() : parent;
            last_volume_parent = parent;
        }
        return last_volume_root;
    };
    const auto commit_moves = [&]() -> bool {
        if (pending_moves.empty()) return true;
        const std::vector<size_t> batch = std::move(pending_moves);
        pending_moves.clear();
        std::vector<std::pair<std::wstring, size_t>> volumes;  // volume -> a copy on it
        for (size_t i : batch) {
            const std::wstring volume = volume_of(entries[i].destination);
            if (std::none_of(volumes.begin(), volumes.end(), [&](const auto& known) {
                    return _wcsicmp(known.first.c_str(), volume.c_str()) == 0; }))
                volumes.emplace_back(volume, i);
        }
        for (const auto& [volume, i] : volumes) {
            DWORD flush_error = 0;
            if (!FlushCopyToDisk(entries[i].destination, entries[i].attributes, flush_error, true)) {
                if (!flush_error) flush_error = ERROR_WRITE_FAULT;
                if (failure.empty()) {
                    failure_hr = HRESULT_FROM_WIN32(flush_error);
                    failure = l10n::Get(l10n::StringId::MoveKeptSourceNotFlushed).c_str() + entries[i].destination +
                              L" | " + Win32Message(flush_error);
                }
                return false;  // every source of the batch stays
            }
        }
        bool ok = true;
        for (size_t i : batch) {
            auto& moved = entries[i];
            // The attributes were read while scanning.
            if (moved.attributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(moved.source.c_str(), moved.attributes & ~FILE_ATTRIBUTE_READONLY);
            if (!DeleteFileW(moved.source.c_str())) {
                const DWORD code = GetLastError();
                if (moved.attributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesW(moved.source.c_str(), moved.attributes);
                const HRESULT hr = HRESULT_FROM_WIN32(code);
                std::wstring reason = FailureReason(l10n::Get(l10n::StringId::MoveCopiedSourceKept).c_str(),
                                                    TransferFailureText(hr));
                if (IsStopTransferError(hr)) {
                    if (failure.empty()) {
                        failure_hr = hr;
                        failure = reason + L" | " + moved.source;
                    }
                    ok = false;
                    continue;
                }
                // Both copies stay; the item is listed, the task goes on.
                if (item_failures.size() < kMaxReportedFailures)
                    item_failures.push_back({moved.source, moved.destination, std::move(reason), hr});
                ++failed_items;
                skipped_sources.push_back(moved.source);
                continue;
            }
            moved.source_removed = true;
            moved.completed = true;
        }
        return ok;
    };
    const auto queue_move = [&](size_t index) -> bool {
        if (pending_moves.empty()) pending_since = GetTickCount64();
        pending_moves.push_back(index);
        if (pending_moves.size() >= 512 || GetTickCount64() - pending_since >= 2000) return commit_moves();
        return true;
    };
    const auto apply_pooled = [&](std::vector<SmallCopyResult> results) {
        for (const auto& result : results) {
            if (!result.ran) continue;
            auto& done = entries[result.index];
            const DWORD code = FAILED(result.hr) ? HRESULT_CODE(result.hr) : result.delete_error;
            if (code != ERROR_SUCCESS) {
                const HRESULT hr = FAILED(result.hr) ? result.hr : HRESULT_FROM_WIN32(code);
                if (result.not_flushed) {
                    if (failure.empty()) {
                        failure_hr = hr;
                        failure = l10n::Get(l10n::StringId::MoveKeptSourceNotFlushed).c_str() + done.destination +
                                  L" | " + Win32Message(code);
                    }
                    pool->Stop();
                } else if (!item_failed(done, hr, TransferFailureText(hr), done.source)) {
                    pool->Stop();
                }
                continue;
            }
            failures_in_a_row = 0;
            if (result.move_pending) {
                if (!queue_move(result.index)) pool->Stop();
            } else {
                done.completed = true;
                done.source_removed = result.source_removed;
            }
            ++processed_items;
            publish_progress(done, done.bytes, done.bytes, processed_items);
            committed_bytes += done.bytes;
        }
    };
    const auto drain_pool = [&] {
        while (pool && pool->Outstanding() > 0) apply_pooled(pool->Take(true));
    };

    // Large files with no conflict wait until every small file has gone to
    // the pool, then copy here while the pool works: one ISO does not hold
    // up thousands of small files, and they do not chop it up.
    std::vector<size_t> deferred_large;
    // Large files bypass the cache when it cannot absorb them (the copy
    // would stall flushing at the end) or the target is a share or a
    // removable drive. A same-volume copy that can block-clone keeps the
    // cached path, which clones.
    const TransferTarget target = DescribeTransferTarget(
        entries.empty() ? std::wstring{} : entries.front().source, req.dest_dir);
    uint64_t large_remaining = 0;
    for (const auto& candidate : entries)
        if (!candidate.directory && !candidate.reparse && !candidate.renamed &&
            candidate.bytes >= kDirectCopyMaxBytes) large_remaining += candidate.bytes;

    for (size_t step = 0; failure.empty() && !cancelled && step < entries.size() + deferred_large.size(); ++step) {
        const size_t index = step < entries.size() ? step : deferred_large[step - entries.size()];
        if (pool) apply_pooled(pool->Take(false));
        if (!failure.empty() || stop_requested()) break;
        auto& entry = entries[index];
        if (entry.renamed) {
            ++processed_items;
            publish_progress(entry, entry.bytes, entry.bytes, processed_items);
            committed_bytes += entry.bytes;
            continue;
        }
        if (transfer_cancel_.load()) { cancelled = true; break; }
        bool skipped = false;
        for (const auto& prefix : skipped_prefixes) {
            if (StartsWithPath(entry.source, prefix)) { skipped = true; break; }
        }
        if (skipped) {
            mark_skipped(entry);
            continue;
        }
        const std::wstring parent_dir = ParentOf(entry.destination);
        const std::wstring parent_key = dir_key(parent_dir);
        const bool fresh_parent = fresh_dirs.contains(parent_key);
        // Two sources with one target name (a multi-folder selection): the
        // first may still be copying; let it land so this one sees a conflict.
        if (pool && !fresh_parent && pooled_destinations.contains(dir_key(entry.destination))) {
            drain_pool();
            if (!failure.empty()) break;
        }
        bool destination_is_directory = false;
        bool destination_exists = !fresh_parent && PathExists(entry.destination, &destination_is_directory);
        // Taken now though the scan found it free (another program, or an
        // earlier root of this transfer with the same name): what is there
        // is not this root's to undo as a whole.
        if (destination_exists) entry.destination_preexisting = true;
        // Source and target can only be one object when the target exists.
        if (destination_exists && SameFileObject(entry.source, entry.destination)) {
            if (entry.directory) skipped_prefixes.push_back(entry.source);
            mark_skipped(entry);
            continue;
        }
        ConflictChoice choice = repeated;
        const bool conflict = destination_exists &&
            !(entry.directory && !entry.reparse && destination_is_directory);
        bool selected_apply_all = false;
        if (conflict && !apply_all) {
            ConflictItemInfo info;
            info.task_id = task_id;
            info.source = entry.source;
            info.destination = entry.destination;
            info.source_size = entry.bytes;
            info.source_modified = entry.modified;
            info.source_is_directory = entry.directory;
            TransferEntry destination_entry;
            if (ReadEntryMetadata(entry.destination, destination_entry)) {
                info.destination_size = destination_entry.bytes;
                info.destination_modified = destination_entry.modified;
                info.destination_is_directory = destination_entry.directory;
            }
            info.remaining = remaining_conflicts(index);
            info.ordinal = ++conflicts_asked;
            info.type = req.type;
            info.keep_both_name = FileName(UniqueCopyPath(entry.destination, entry.directory));
            {
                std::lock_guard<std::mutex> lock(transfer_control_mutex_);
                info.token = next_conflict_token_++;
                pending_conflict_ = info;
                resolved_conflict_token_ = 0;
            }
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::WaitingForConflict;
                st.current_item = FileName(entry.source);
                st.summary = l10n::Get(l10n::StringId::WaitingForFileConflictsToBeResolved).c_str();
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            const ULONGLONG held_from = GetTickCount64();
            std::unique_lock<std::mutex> lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] {
                return transfer_cancel_.load() || resolved_conflict_token_ == info.token;
            });
            held_ms += GetTickCount64() - held_from;
            if (transfer_cancel_.load()) {
                pending_conflict_.reset();
                cancelled = true;
                break;
            }
            choice = resolved_conflict_choice_;
            selected_apply_all = resolved_conflict_apply_all_;
            pending_conflict_.reset();
            lock.unlock();
            if (choice == ConflictChoice::Cancel) {
                cancelled = true;
                transfer_cancel_.store(true);
                break;
            }
            if (selected_apply_all) {
                apply_all = true;
                repeated = choice;
            }
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Running;
                st.summary = ProgressSummary(req.type, entries.size());
                st.bytes_per_second = rate.speed();
                st.eta_seconds = rate.eta_seconds();
            });
        }

        if (conflict && choice == ConflictChoice::Skip) {
            if (entry.directory) skipped_prefixes.push_back(entry.source);
            mark_skipped(entry);
            continue;
        }
        if (conflict && choice == ConflictChoice::KeepBoth) {
            const std::wstring old_destination = entry.destination;
            const std::wstring unique = UniqueCopyPath(entry.destination, entry.directory);
            entry.destination = unique;
            if (entry.directory) {
                for (size_t j = index + 1; j < entries.size(); ++j) {
                    if (StartsWithPath(entries[j].destination, old_destination))
                        entries[j].destination = unique + entries[j].destination.substr(old_destination.size());
                }
            }
            destination_exists = false;
            destination_is_directory = false;
        }
        if (conflict && choice == ConflictChoice::Replace &&
            (entry.directory != destination_is_directory || entry.reparse)) {
            if (req.type == OpType::Move) {
                if (!item_failed(entry, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                                 FailureReason(l10n::Get(l10n::StringId::CannotAtomicallyReplaceATargetOfADifferentTypeOrAReparsePoin).c_str()),
                                 entry.destination)) break;
                continue;
            }
            const std::wstring backup = UniqueTemporaryPath(
                entry.destination, L".pulse-backup-", task_id, index);
            if (!MoveFileExW(entry.destination.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
                const DWORD code = GetLastError();
                if (!item_failed(entry, HRESULT_FROM_WIN32(code),
                                 FailureReason(l10n::Get(l10n::StringId::CouldNotSafelyBackUpTheItemBeingReplaced).c_str(),
                                               TransferFailureText(HRESULT_FROM_WIN32(code))),
                                 entry.destination)) break;
                continue;
            }
            replacement_backups.push_back({ entry.destination, backup });
            destination_exists = false;
            destination_is_directory = false;
        }

        if (entry.reparse) {
            std::wstring error;
            if (!ensure_parent(parent_dir, error) || !CopyReparsePoint(entry, error)) {
                if (!item_failed(entry, E_FAIL, ErrorWithoutPath(error), entry.source)) break;
                continue;
            }
            if (req.type == OpType::Move) {
                const BOOL removed = entry.directory ? RemoveDirectoryW(entry.source.c_str())
                                                     : DeleteFileW(entry.source.c_str());
                if (!removed) {
                    const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
                    // The link now exists at the target too; nothing is lost.
                    if (!item_failed(entry, hr, FailureReason(l10n::Get(l10n::StringId::MoveCopiedSourceKept).c_str(),
                                                              TransferFailureText(hr)), entry.source)) break;
                    continue;
                }
                entry.source_removed = true;
            }
            failures_in_a_row = 0;
            entry.completed = true;
            ++processed_items;
            publish_progress(entry, entry.bytes, entry.bytes, processed_items);
            committed_bytes += entry.bytes;
            continue;
        }

        if (entry.directory) {
            const std::wstring key = dir_key(entry.destination);
            if (fresh_parent || ready_dirs.contains(parent_key)) {
                // The parent exists, so one CreateDirectory does it; an
                // existing folder of that name is merged into.
                if (CreateDirectoryW(entry.destination.c_str(), nullptr)) {
                    fresh_dirs.insert(key);
                } else {
                    const DWORD code = GetLastError();
                    bool is_directory = false;
                    if (code != ERROR_ALREADY_EXISTS || !PathExists(entry.destination, &is_directory) || !is_directory) {
                        // Everything inside is left out with it.
                        const HRESULT hr = HRESULT_FROM_WIN32(code);
                        if (!item_failed(entry, hr, FailureReason(l10n::Get(l10n::StringId::CouldNotCreateTheDestinationFolder).c_str(),
                                                                  TransferFailureText(hr)), entry.destination)) break;
                        continue;
                    }
                }
            } else {
                std::wstring error;
                if (!EnsureDirectories(entry.destination, error)) {
                    if (!item_failed(entry, E_FAIL, ErrorWithoutPath(error), entry.destination)) break;
                    continue;
                }
                if (!destination_exists) fresh_dirs.insert(key);
            }
            failures_in_a_row = 0;
            ready_dirs.insert(key);
            entry.directory_prepared = true;
            ++processed_items;
            SetProgressStatus([&](OpStatus& st) {
                st.current_item = FileName(entry.source);
                st.completed_items = processed_items;
                if (st.total_bytes == 0 && st.total_items > 0)
                    st.percent = static_cast<float>(st.completed_items * 100.0 / st.total_items);
            });
            continue;
        }

        if (std::wstring error; !fresh_parent && !ensure_parent(parent_dir, error)) {
            if (!item_failed(entry, E_FAIL, ErrorWithoutPath(error), entry.destination)) break;
            continue;
        }

        const bool direct = !destination_exists && entry.bytes < kDirectCopyMaxBytes;
        if (pool && direct && entry.bytes < kProgressCallbackMinBytes) {
            while (!pool->HasRoom() && failure.empty() && !stop_requested())
                apply_pooled(pool->Take(true));
            if (!failure.empty() || cancelled) break;
            if (!fresh_parent) pooled_destinations.insert(dir_key(entry.destination));
            pool->Submit({index, entry.source, entry.destination, entry.attributes,
                          req.type == OpType::Move});
            continue;
        }
        if (pool && !entry.deferred && !destination_exists) {
            entry.deferred = true;
            deferred_large.push_back(index);
            continue;
        }
        bool unbuffered = false;
        if (entry.bytes >= kDirectCopyMaxBytes && !target.block_clone) {
            MEMORYSTATUSEX memory{};
            memory.dwLength = sizeof(memory);
            // A spinning target drains the cache far slower than it fills;
            // buffered, the copy then stalls in the dirty-page throttle where
            // neither progress nor pause can get through.
            unbuffered = target.slow || target.spinning ||
                (GlobalMemoryStatusEx(&memory) && large_remaining > memory.ullAvailPhys / 2);
        }
        std::wstring copy_destination = direct ? entry.destination
            : UniqueTemporaryPath(entry.destination, L".pulse-copy-", task_id, index);
        const bool replacing = destination_exists && choice == ConflictChoice::Replace;

        uint64_t known_file_total = entry.bytes;
        CopyProgressContext context;
        context.cancel = &transfer_cancel_;
        context.pause = &transfer_pause_;
        context.report = [&](uint64_t done, uint64_t actual_total) {
            if (actual_total > 0 && actual_total != known_file_total) {
                if (actual_total > known_file_total) total_bytes += actual_total - known_file_total;
                else total_bytes -= (std::min)(total_bytes, known_file_total - actual_total);
                known_file_total = actual_total;
                entry.bytes = actual_total;
                SetProgressStatus([&](OpStatus& st) { st.total_bytes = total_bytes; });
            }
            // Small files finishing alongside count toward the same total.
            if (pool) apply_pooled(pool->Take(false));
            publish_progress(entry, done, actual_total, processed_items);
        };

        HRESULT copy_result = E_FAIL;
        // A direct copy that lost the name to another writer must not delete
        // that writer's file.
        auto discard_copy = [&] {
            if (direct && (copy_result == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) ||
                           copy_result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)))
                return;
            DeleteFileW(copy_destination.c_str());
        };
        bool resume = false;
        // Dropped together if the file system rejects either.
        DWORD extra_flags = COPY_FILE_COPY_SYMLINK | (unbuffered ? COPY_FILE_NO_BUFFERING : 0);
        for (;;) {
            context.pause_sent = false;
            COPYFILE2_EXTENDED_PARAMETERS parameters{};
            parameters.dwSize = sizeof(parameters);
            parameters.dwCopyFlags = extra_flags;
            if (!resume) parameters.dwCopyFlags |= COPY_FILE_FAIL_IF_EXISTS;
            if (resume) parameters.dwCopyFlags |= COPY_FILE_RESUME_FROM_PAUSE;
            parameters.pfCancel = nullptr;
            // Small files finish within one callback interval; the callback
            // only adds a round trip per file. Pause and cancel are checked
            // between files for them.
            if (entry.bytes >= kProgressCallbackMinBytes) {
                parameters.pProgressRoutine = CopyProgress;
                parameters.pvCallbackContext = &context;
            }
            copy_result = CopyFile2(entry.source.c_str(), copy_destination.c_str(), &parameters);
            if (copy_result == HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER) && extra_flags != 0 && !resume) {
                extra_flags = 0;
                continue;
            }
            if (copy_result != HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED)) break;

            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Paused;
                st.summary = std::wstring(OpVerb(req.type)) + l10n::Get(l10n::StringId::Paused).c_str();
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            const ULONGLONG held_from = GetTickCount64();
            std::unique_lock<std::mutex> lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] {
                return transfer_cancel_.load() || !transfer_pause_.load();
            });
            lock.unlock();
            held_ms += GetTickCount64() - held_from;
            if (transfer_cancel_.load()) {
                cancelled = true;
                break;
            }
            resume = true;
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Running;
                st.summary = ProgressSummary(req.type, entries.size());
                st.bytes_per_second = rate.speed();
                st.eta_seconds = rate.eta_seconds();
            });
        }
        if (cancelled || transfer_cancel_.load()) {
            cancelled = true;
            discard_copy();
            break;
        }
        if (FAILED(copy_result)) {
            discard_copy();
            if (!item_failed(entry, copy_result, TransferFailureText(copy_result), entry.source)) break;
            continue;
        }

        if (verify_copies_.load()) {
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Verifying;
                st.summary = l10n::Get(l10n::StringId::Verifying).c_str() + FileName(entry.source);
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            std::array<uint8_t, 32> source_hash{};
            std::array<uint8_t, 32> copied_hash{};
            auto wait_if_paused = [&] {
                if (!transfer_pause_.load() || transfer_cancel_.load()) return;
                SetStatus([&](OpStatus& st) {
                    st.phase = OpPhase::Paused;
                    st.summary = std::wstring(OpVerb(req.type)) + l10n::Get(l10n::StringId::Paused).c_str();
                });
                const ULONGLONG held_from = GetTickCount64();
                std::unique_lock<std::mutex> lock(transfer_control_mutex_);
                transfer_control_cv_.wait(lock, [&] {
                    return transfer_cancel_.load() || !transfer_pause_.load();
                });
                lock.unlock();
                held_ms += GetTickCount64() - held_from;
                if (!transfer_cancel_.load()) SetStatus([&](OpStatus& st) {
                    st.phase = OpPhase::Verifying;
                    st.summary = l10n::Get(l10n::StringId::Verifying).c_str() + FileName(entry.source);
                });
            };
            std::wstring hash_error;
            if (!Sha256File(entry.source, transfer_cancel_, wait_if_paused, source_hash, hash_error) ||
                !Sha256File(copy_destination, transfer_cancel_, wait_if_paused, copied_hash, hash_error)) {
                discard_copy();
                if (transfer_cancel_.load()) { cancelled = true; break; }
                if (!item_failed(entry, E_FAIL, ErrorWithoutPath(hash_error), entry.source)) break;
                continue;
            }
            if (source_hash != copied_hash) {
                discard_copy();
                if (!item_failed(entry, HRESULT_FROM_WIN32(ERROR_CRC),
                                 FailureReason(l10n::Get(l10n::StringId::SHA256VerificationFailed).c_str()),
                                 entry.source)) break;
                continue;
            }
            SetStatus([&](OpStatus& st) { st.phase = OpPhase::Running; });
        }

        if (!direct && SameFileObject(entry.source, entry.destination)) {
            DeleteFileW(copy_destination.c_str());
            mark_skipped(entry);
            continue;
        }
        if (replacing) {
            DWORD old_attributes = GetFileAttributesW(entry.destination.c_str());
            if (old_attributes != INVALID_FILE_ATTRIBUTES &&
                (old_attributes & FILE_ATTRIBUTE_READONLY))
                SetFileAttributesW(entry.destination.c_str(), old_attributes & ~FILE_ATTRIBUTE_READONLY);
            if (!ReplaceFileW(entry.destination.c_str(), copy_destination.c_str(), nullptr,
                              REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) &&
                !MoveFileExW(copy_destination.c_str(), entry.destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                const DWORD error = GetLastError();
                DeleteFileW(copy_destination.c_str());
                if (!item_failed(entry, HRESULT_FROM_WIN32(error), TransferFailureText(HRESULT_FROM_WIN32(error)),
                                 entry.destination)) break;
                continue;
            }
        } else if (!direct && !MoveFileExW(copy_destination.c_str(), entry.destination.c_str(),
                                           MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            DeleteFileW(copy_destination.c_str());
            if (!item_failed(entry, HRESULT_FROM_WIN32(error), TransferFailureText(HRESULT_FROM_WIN32(error)),
                             entry.destination)) break;
            continue;
        }

        if (req.type == OpType::Move) {
            // Written out now; the source goes with its batch (commit_moves).
            if (DWORD flush_error = 0; !FlushCopyToDisk(entry.destination, entry.attributes, flush_error, false)) {
                if (!flush_error) flush_error = ERROR_WRITE_FAULT;
                failure_hr = HRESULT_FROM_WIN32(flush_error);
                failure = l10n::Get(l10n::StringId::MoveKeptSourceNotFlushed).c_str() + entry.destination +
                          L" | " + Win32Message(flush_error);
                break;
            }
            if (!queue_move(index)) break;
        } else {
            entry.completed = true;
        }
        failures_in_a_row = 0;
        if (entry.bytes >= kDirectCopyMaxBytes)
            large_remaining -= (std::min)(large_remaining, entry.bytes);
        ++processed_items;
        publish_progress(entry, entry.bytes, entry.bytes, processed_items);
        committed_bytes += entry.bytes;
    }

    if (pool) {
        if (!failure.empty() || cancelled || transfer_cancel_.load()) pool->Stop();
        drain_pool();
        pool.reset();
    }
    // Copies already written out finish their move, on failure and cancel
    // too (as each finished move used to).
    commit_moves();
    for (const auto& done : entries) {
        if (done.source_removed) moved_sources_.push_back(done.source);
        if (!done.completed) continue;
        completed_sources.push_back(done.source);
        completed_destinations.push_back(done.destination);
    }

    if (failure.empty() && !stop_requested()) {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (stop_requested()) break;
            if (!it->directory_prepared) continue;
            if (req.type == OpType::Move) {
                if (RemoveDirectoryW(it->source.c_str())) {
                    moved_sources_.push_back(it->source);
                } else {
                    const DWORD error = GetLastError();
                    const bool intentionally_retained = error == ERROR_DIR_NOT_EMPTY &&
                        std::any_of(skipped_sources.begin(), skipped_sources.end(), [&](const auto& skipped) {
                            return StartsWithPath(skipped, it->source);
                        });
                    if (!intentionally_retained && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                        if (failure.empty()) failure_hr = HRESULT_FROM_WIN32(error);
                        if (!failure.empty()) failure += L"; ";
                        failure += Win32Message(error) + L" | " + it->source;
                    }
                }
            } else if (!it->reparse) {
                SetCopiedDirectoryMetadata(*it);
            }
        }
    }

    // Replacements that could not be put back, appended to the result text so
    // the kept ".pulse-backup-" copy can be recovered by hand.
    std::wstring rollback_note;
    if (failure.empty() && !cancelled) {
        for (const auto& backup : replacement_backups) {
            std::error_code ignored;
            fsys::remove_all(fsys::path(backup.backup), ignored);
        }
    } else {
        // The new destination is removed only so the backup can take its name
        // back. Both steps used to ignore their result, so a destination held
        // open by a scanner, or a backup that could not be renamed, left the
        // original stranded under its temporary name with nothing reported.
        for (auto it = replacement_backups.rbegin(); it != replacement_backups.rend(); ++it) {
            std::error_code removed;
            fsys::remove_all(fsys::path(it->original), removed);
            DWORD error = removed ? static_cast<DWORD>(removed.value()) : ERROR_SUCCESS;
            if (error == ERROR_SUCCESS &&
                !MoveFileExW(it->backup.c_str(), it->original.c_str(), MOVEFILE_WRITE_THROUGH))
                error = GetLastError();
            if (error != ERROR_SUCCESS) {
                rollback_note += l10n::Get(l10n::StringId::CouldNotRestore).c_str() + it->original +
                    l10n::Get(l10n::StringId::OriginalKeptAt).c_str() + it->backup + L" | " +
                    Win32Message(error);
            }
        }
        for (size_t i = completed_destinations.size(); i-- > 0;) {
            const bool restored = std::any_of(replacement_backups.begin(),
                replacement_backups.end(), [&](const ReplacementBackup& backup) {
                    return StartsWithPath(completed_destinations[i], backup.original);
                });
            if (restored) {
                const auto& source = completed_sources[i];
                std::erase_if(moved_sources_, [&](const auto& moved) {
                    return _wcsicmp(moved.c_str(), source.c_str()) == 0;
                });
                completed_destinations.erase(completed_destinations.begin() + i);
                completed_sources.erase(completed_sources.begin() + i);
            }
        }
    }

    if (failure.empty() && !cancelled && failed_items == 0) {
        OpRequest committed = req;
        committed.sources.clear();
        std::vector<std::wstring> committed_destinations;
        // Each root and what was scanned under it are one run of entries
        // (pre-order), up to the next root's entry.
        for (size_t root_index = 0; root_index < req.sources.size(); ++root_index) {
            const size_t first = root_entries[root_index];
            if (first == std::wstring::npos) continue;
            const TransferEntry& found = entries[first];
            const std::wstring original_destination = JoinPath(req.dest_dir,
                                                               root_name(req.sources[root_index]));
            const bool independent_root = !found.destination_preexisting
                || _wcsicmp(found.destination.c_str(), original_destination.c_str()) != 0;
            if (independent_root) {
                committed.sources.push_back(req.sources[root_index]);
                committed_destinations.push_back(found.destination);
                continue;
            }
            size_t end = entries.size();
            for (size_t next = root_index + 1; next < req.sources.size(); ++next)
                if (root_entries[next] != std::wstring::npos) { end = root_entries[next]; break; }
            for (size_t item = first; item < end; ++item) {
                if (!entries[item].completed) continue;
                committed.sources.push_back(entries[item].source);
                committed_destinations.push_back(entries[item].destination);
            }
        }
        if (!committed_destinations.empty()) PushUndo(committed, task_id, &committed_destinations);
    } else if (!completed_destinations.empty()) {
        OpRequest partial = req;
        partial.sources = completed_sources;
        PushUndo(partial, task_id, &completed_destinations);
    }

    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        pending_conflict_.reset();
    }
    const bool transfer_cancelled = cancelled || transfer_cancel_.load();
    diagnostics::runtime::Event("file_transfer_result", {{"task", task_id},
        {"hresult", static_cast<uint32_t>(failure_hr)}, {"has_error", !failure.empty()},
        {"cancelled", transfer_cancelled}, {"completed_sources", completed_sources.size()}});
    // Items held open by another program say by which (a bounded number:
    // each probe asks Restart Manager).
    if (!transfer_cancelled) {
        size_t probed = 0;
        for (auto& failed : item_failures) {
            if (probed >= 16) break;
            if (!IsLockLikeError(failed.hr)) continue;
            ++probed;
            const auto owners = FindLockOwners(LockProbeFiles(failed.source, 16), CurrentModuleDirectory());
            if (owners.empty()) continue;
            const LockOwner& owner = owners.front();
            const std::wstring name = owner.app_name.empty() ? owner.image_name : owner.app_name;
            if (name.empty()) continue;
            const std::wstring format = l10n::Get(l10n::StringId::TransferInUseBy).c_str();
            if (format.find(L"%s") == std::wstring::npos) continue;
            wchar_t text[512];
            swprintf_s(text, format.c_str(), name.c_str());
            failed.reason = text;
        }
    }
    // One failed item and it is in use: the file-in-use dialog, as when the
    // task stopped there; its retry covers just that item.
    std::optional<LockReport> single_lock;
    if (!transfer_cancelled && failure.empty() && failed_items == 1 && item_failures.size() == 1 &&
        IsLockLikeError(item_failures.front().hr)) {
        OpRequest narrowed = req;
        narrowed.sources = {item_failures.front().source};
        narrowed.dest_dir = ParentOf(item_failures.front().destination);
        narrowed.new_name.clear();
        if (_wcsicmp(FileName(item_failures.front().source).c_str(),
                     FileName(item_failures.front().destination).c_str()) != 0)
            narrowed.new_name = FileName(item_failures.front().destination);
        single_lock = ProbeLock(narrowed, task_id, item_failures.front().hr,
                                item_failures.front().reason + L" | " + item_failures.front().source);
    }
    const LockReport lock_report = single_lock ? *single_lock
        : !transfer_cancelled && !failure.empty()
        ? (move_lock_report ? *move_lock_report : ProbeLock(req, task_id, failure_hr, failure))
        : LockReport{};
    if (failed_items > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_retries_.push_back(FailedItemsRetry{task_id, req, item_failures});
        while (failed_retries_.size() > 8) failed_retries_.pop_front();
    }
    rollback_note = kept_backup_note + rollback_note;
    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.pause_requested = false;
        st.completed_ops++;
        st.bytes_per_second = 0.0;
        st.eta_seconds = 0;
        st.locked_path = lock_report.path;
        st.lock_owners = lock_report.owners;
        st.failures = item_failures;
        st.failed_items = failed_items;
        if (transfer_cancelled) {
            st.phase = OpPhase::Failed;
            st.last_error = l10n::Get(l10n::StringId::Canceled).c_str() + rollback_note;
            st.summary = std::wstring(OpVerb(req.type)) + l10n::Get(l10n::StringId::Canceled_2).c_str();
        } else if (!failure.empty()) {
            st.phase = OpPhase::Failed;
            st.last_error = failure + rollback_note;
            st.summary = std::wstring(OpVerb(req.type)) + l10n::Get(l10n::StringId::Failed).c_str();
        } else if (failed_items > 0) {
            // The rest went through; the dialog lists what did not.
            st.phase = OpPhase::Failed;
            st.percent = 100.0f;
            st.transferred_bytes = st.total_bytes;
            st.completed_items = st.total_items;
            st.last_error = item_failures.front().reason + L" | " + item_failures.front().source + rollback_note;
            wchar_t text[128];
            swprintf_s(text, l10n::Get(l10n::StringId::TransferItemsFailed).c_str(),
                       static_cast<int>((std::min<uint64_t>)(failed_items, INT_MAX)));
            st.summary = text;
        } else {
            st.phase = OpPhase::Completed;
            st.percent = 100.0f;
            st.transferred_bytes = st.total_bytes;
            st.completed_items = st.total_items;
            st.summary = Describe(req) + l10n::Get(l10n::StringId::Completed).c_str();
            st.last_error.clear();
        }
    });
    transfer_active_.store(false);
}

void OpsManager::RunShellOp(const OpRequest& req, uint64_t task_id) {
    if ((req.type == OpType::CreateFolder || req.type == OpType::CreateTextFile ||
         req.type == OpType::RealDelete) && IsShellTransferRequest(req)) {
        RunShellItemOp(req, task_id);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        shell_cancel_requested_ = active_item_ && active_item_->seq == task_id && active_item_->cancel_requested;
    }
    shell_activity_tick_ = GetTickCount64();
    // Status: active.
    SetStatus([&](OpStatus& st) {
        st.active = true;
        st.can_pause = true;
        st.authorization = AuthorizationState::None;
        st.can_skip_authorization = false;
        st.type = req.type;
        st.task_id = task_id;
        st.phase = OpPhase::Running;
        st.percent = 0.0f;
        st.summary = Describe(req);
        st.last_error.clear();
        st.failures.clear();
        st.failed_items = 0;
        st.locked_path.clear();
        st.lock_owners.clear();
        st.source_label = req.sources.empty() ? L"" : FileName(req.sources.front());
        st.destination_label.clear();
        st.current_item = req.type == OpType::EmptyRecycle
            ? OpVerb(OpType::EmptyRecycle) : st.source_label;
        st.total_bytes = st.transferred_bytes = 0;
        st.total_items = req.type == OpType::EmptyRecycle ? 0
            : (req.sources.empty() ? 1 : req.sources.size());
        st.completed_items = 0;
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.items_per_second = st.peak_items_per_second = 0.0;
        st.speed_basis = SpeedBasisFor(req.type);
        st.eta_seconds = 0;
        if (req.type == OpType::EmptyRecycle) st.percent = -1.0f;
    });

    DuplicateCleanupGuard duplicate_guard;
    if (req.duplicate_cleanup) {
        SetStatus([](OpStatus& st) { st.phase = OpPhase::Verifying; st.percent = -1.0f; });
        const auto cancelled = [&] { return stopping_.load() || shell_cancel_requested_.load(); };
        if (req.type != OpType::RecycleDelete ||
            !duplicate_guard.Validate(req.duplicate_groups, req.sources, cancelled)) {
            SetStatus([&](OpStatus& st) {
                st.active = false;
                st.phase = OpPhase::Failed;
                ++st.completed_ops;
                st.last_error = cancelled() ? l10n::Get(l10n::StringId::Canceled).c_str() :
                    l10n::Get(l10n::StringId::DuplicateFilesChangedOrCouldNotBeVerifiedScanAgainBeforeClea).c_str();
            });
            return;
        }
        SetStatus([](OpStatus& st) { st.phase = OpPhase::Running; st.percent = 0.0f; });
    }

#ifndef PULSE_ELEVATED_HOST
    if (req.type == OpType::RecycleDelete || req.type == OpType::RealDelete) {
        RunAuthorizedDelete(req, task_id);
        return;
    }
#endif
    if (req.type == OpType::EmptyRecycle) {
        // Delete the payloads directly (recycle_empty.h has the numbers). The
        // Shell only gets what is left, e.g. bins of folders redirected to a
        // network share, and is told once at the end so Explorer refreshes.
        SetStatus([](OpStatus& st) {
            st.can_pause = false;
            st.current_item = OpVerb(OpType::EmptyRecycle);
            st.summary = st.current_item;
            st.percent = -1.0f;
        });
        TransferRateEstimator recycle_rate;
        recycle_rate.Reset(GetTickCount64(), 0);
        const ULONGLONG started = GetTickCount64();
        const auto cancelled = [this] { return stopping_.load() || shell_cancel_requested_.load(); };
        const auto progress = [&](const recycle_empty::Progress& p) {
            SetStatus([&](OpStatus& st) {
                if (!st.active || st.type != OpType::EmptyRecycle) return;
                st.total_items = p.total_items;
                st.completed_items = p.removed_items;
                st.total_bytes = (std::max)(p.total_bytes, p.deleted_bytes);
                st.transferred_bytes = p.deleted_bytes;
                st.percent = st.total_bytes ? 100.0f * static_cast<float>(st.transferred_bytes) / static_cast<float>(st.total_bytes)
                    : p.total_items ? 100.0f * static_cast<float>(p.removed_items) / static_cast<float>(p.total_items) : -1.0f;
                st.current_item = p.current.empty() ? OpVerb(OpType::EmptyRecycle) : p.current;
                recycle_rate.Observe(GetTickCount64(), st.transferred_bytes, st.total_bytes);
                st.bytes_per_second = recycle_rate.speed();
                st.peak_bytes_per_second = (std::max)(st.peak_bytes_per_second, st.bytes_per_second);
                st.eta_seconds = recycle_rate.eta_seconds();
            });
        };
        const recycle_empty::Result result = recycle_empty::EmptyFolders(
            recycle_empty::CurrentUserBinFolders(), cancelled, progress);
        HRESULT hr = S_OK;
        uint64_t shell_items = 0;
        if (!result.cancelled) {
            SHQUERYRBINFO left{};
            left.cbSize = sizeof(left);
            if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &left)) && left.i64NumItems > 0) {
                shell_items = static_cast<uint64_t>(left.i64NumItems);
                hr = SHEmptyRecycleBinW(nullptr, nullptr,
                    SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
            }
        }
        recycle_empty::NotifyShell();
        diagnostics::runtime::Event("recycle_empty_result", {{"task", task_id},
            {"items", result.removed_items}, {"files", result.deleted_files},
            {"bytes", result.deleted_bytes}, {"failed", result.failed_entries},
            {"error", static_cast<uint32_t>(result.first_error)}, {"cancelled", result.cancelled},
            {"shell_items", shell_items}, {"shell_hresult", static_cast<uint32_t>(hr)},
            {"elapsed_ms", GetTickCount64() - started}});

        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.percent = -1.0f;
            st.completed_ops++;
            st.bytes_per_second = 0;
            st.eta_seconds = 0;
            if (result.cancelled) {
                st.phase = OpPhase::Failed;
                st.last_error = l10n::Get(l10n::StringId::Canceled).c_str();
                st.summary = st.last_error;
            } else if (FAILED(hr) || result.failed_entries) {
                st.phase = OpPhase::Failed;
                st.last_error = l10n::Get(l10n::StringId::CouldNotEmptyTheRecycleBin).c_str();
            } else {
                st.phase = OpPhase::Completed;
                st.completed_items = st.total_items;
                st.summary = Describe(req) + l10n::Get(l10n::StringId::Completed).c_str();
            }
        });
        return;
    }

    auto& client = ipc::ShellClient::Instance();

    // A drive takes a label, not a file name: "Label (C:)" is not one.
    std::wstring drive_root;
    if (req.type == OpType::Rename && req.sources.size() == 1 && DriveRootOf(req.sources.front(), drive_root)) {
        std::wstring error;
        const bool renamed = !shell_cancel_requested_.load() && !stopping_.load() &&
            RenameDriveLabel(drive_root, req.new_name, ui_hwnd_.load(), &error);
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.percent = -1.0f;
            ++st.completed_ops;
            if (renamed) {
                OpRequest shown = req;  // the label set, not the typed " (Z:)"
                shown.new_name = DriveLabelFromRename(req.new_name, drive_root.front());
                st.completed_items = st.total_items;
                st.phase = OpPhase::Completed;
                st.summary = Describe(shown) + l10n::Get(l10n::StringId::Completed).c_str();
            } else {
                st.phase = OpPhase::Failed;
                st.last_error = error.empty() ? l10n::Get(l10n::StringId::Canceled).c_str() : error;
            }
        });
        return;
    }

    if (req.type == OpType::Rename || req.type == OpType::BatchRename) {
        bool valid = !req.sources.empty() && (req.type != OpType::Rename || req.sources.size() == 1);
        std::wstring invalid_reason;
        for (size_t i = 0; i < req.sources.size(); ++i) {
            const auto& name = req.type == OpType::BatchRename && i < req.new_names.size() ? req.new_names[i] : req.new_name;
            if (valid && !req.sources[i].empty() && !IsRenameComponent(name)) invalid_reason = RenameNameText(name);
            valid = valid && !req.sources[i].empty() && IsRenameComponent(name);
        }
        if (!valid || shell_cancel_requested_.load() || stopping_.load()) {
            SetStatus([&](OpStatus& st) {
                st.active = false; st.phase = OpPhase::Failed; st.percent = -1.0f;
                ++st.completed_ops;
                st.last_error = valid ? l10n::Get(l10n::StringId::Canceled).c_str()
                    : !invalid_reason.empty() ? invalid_reason : l10n::Get(l10n::StringId::InvalidName_2).c_str();
            });
            return;
        }
    }

    if (req.type == OpType::BatchRename) {
        std::vector<std::wstring> ok_sources;
        std::vector<std::wstring> ok_names;
        std::vector<std::wstring> ok_destinations;
        std::wstring last_error;
        bool cancelled = false;
        for (size_t i = 0; i < req.sources.size(); ++i) {
            if (stopping_.load() || shell_cancel_requested_.load()) { cancelled = true; break; }
            const std::wstring& name = i < req.new_names.size() ? req.new_names[i] : req.new_name;
            SetStatus([&](OpStatus& st) {
                st.current_item = FileName(req.sources[i]);
                st.completed_items = i;
                if (!req.sources.empty())
                    st.percent = 100.0f * static_cast<float>(i) / static_cast<float>(req.sources.size());
            });
            if (stopping_.load() || shell_cancel_requested_.load()) { cancelled = true; break; }
            // Each item uses the same no-overwrite filesystem rename.
            std::wstring local_error;
            const auto local = RenameInProcess(req.sources[i], name, &local_error);
            if (local == RenameResult::Rejected) { last_error = local_error; continue; }
            if (local == RenameResult::Completed) {
                ok_sources.push_back(req.sources[i]);
                ok_names.push_back(name);
                ok_destinations.push_back(JoinPath(ParentOf(req.sources[i]), name));
                continue;
            }

        }
        current_req_id_.store(0);
        shell_cancel_requested_ = false;
        if (!ok_sources.empty()) {
            OpRequest recorded = req;
            recorded.sources = ok_sources;
            recorded.new_names = ok_names;
            PushUndo(recorded, task_id, &ok_destinations);
        }
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.percent = -1.0f;
            st.completed_ops++;
            st.completed_items = ok_sources.size();
            if (cancelled) {
                st.phase = OpPhase::Failed;
                st.last_error = l10n::Get(l10n::StringId::Canceled).c_str();
            } else if (ok_sources.empty()) {
                st.phase = OpPhase::Failed;
                st.last_error = last_error.empty() ? l10n::Get(l10n::StringId::OpFailed).c_str() : last_error;
            } else {
                st.phase = OpPhase::Completed;
                st.summary = Describe(req) + l10n::Get(l10n::StringId::Completed).c_str();
                if (ok_sources.size() != req.sources.size())
                    st.last_error = last_error;
            }
        });
        return;
    }

    // A single rename shares the batch path's validation and no-overwrite rules.
    if (req.type == OpType::Rename && req.sources.size() == 1) {
        std::wstring local_error;
        DWORD local_code = ERROR_SUCCESS;
        const auto local = RenameInProcess(req.sources.front(), req.new_name, &local_error, &local_code);
        if (local == RenameResult::Rejected) {
            // A handle held elsewhere: name the program and offer the retry,
            // like delete and move (the folder case probes files inside it).
            const LockReport lock_report = local_code != ERROR_SUCCESS
                ? ProbeLock(req, task_id, HRESULT_FROM_WIN32(local_code), local_error) : LockReport{};
            SetStatus([&](OpStatus& st) {
                st.active = false; st.phase = OpPhase::Failed; st.percent = -1.0f;
                ++st.completed_ops; st.last_error = local_error;
                st.locked_path = lock_report.path;
                st.lock_owners = lock_report.owners;
            });
            return;
        }
        if (local == RenameResult::Completed) {
            PushUndo(req, task_id);
            SetStatus([&](OpStatus& st) {
                st.active = false;
                st.percent = -1.0f;
                st.completed_ops++;
                st.completed_items = st.total_items;
                st.phase = OpPhase::Completed;
                st.summary = Describe(req) + l10n::Get(l10n::StringId::Completed).c_str();
            });
            return;
        }
    }

    if (req.type == OpType::CreateFolder || req.type == OpType::CreateTextFile) {
        std::wstring local_error;
        const bool created = CreateInProcess(req, &local_error);
        if (created) PushUndo(req, task_id);
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.percent = -1.0f;
            st.completed_ops++;
            if (created) {
                st.completed_items = st.total_items;
                st.phase = OpPhase::Completed;
                st.summary = Describe(req) + l10n::Get(l10n::StringId::Completed).c_str();
            } else {
                st.phase = OpPhase::Failed;
                st.last_error = local_error;
            }
        });
        return;
    }

    const auto accepted = [this](uint32_t request) { RegisterShellRequest(request); };
    uint32_t id = 0;
    switch (req.type) {
    case OpType::Copy:
    case OpType::Move:
    case OpType::EmptyRecycle:
    case OpType::BatchRename:
        break;
    case OpType::RecycleDelete: id = client.DeleteRecycle(req.sources, accepted); break;
    case OpType::RealDelete: id = client.RealDelete(req.sources, accepted); break;
    case OpType::Rename: // Handled above; never send rename to the Shell host.
        break;
    case OpType::CreateFolder:   // Handled above, in process.
    case OpType::CreateTextFile:
        break;
    case OpType::RestoreRecycle:
        id = client.RestoreRecycle(req.sources, accepted);
        break;
    }
    if (shell_cancel_requested_.load() && id != 0) client.Cancel(id);

    if (id == 0) {
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.phase = OpPhase::Failed;
            st.percent = -1.0f;
            st.last_error = l10n::Get(l10n::StringId::TheFileOperationServiceIsNotRunning).c_str();
            st.completed_ops++;
        });
        return;
    }

    uint32_t hr = 0;
    bool cancelled = false;
    std::wstring error;
    if (!WaitShellDone(id, hr, cancelled, error)) return;
    diagnostics::runtime::Event("file_operation_shell_result", {{"task", task_id},
        {"request", id}, {"hresult", hr}, {"cancelled", cancelled}});

    const bool ok = SUCCEEDED((HRESULT)hr) && !cancelled;
    if (ok) PushUndo(req, task_id);
    const LockReport lock_report = !cancelled && FAILED((HRESULT)hr)
        ? ProbeLock(req, task_id, static_cast<HRESULT>(hr), error) : LockReport{};

    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.percent = -1.0f;
        st.completed_ops++;
        st.locked_path = lock_report.path;
        st.lock_owners = lock_report.owners;
        if (cancelled) {
            st.phase = OpPhase::Failed;
            st.last_error = l10n::Get(l10n::StringId::Canceled).c_str();
        } else if (FAILED((HRESULT)hr)) {
            st.phase = OpPhase::Failed;
            st.last_error = error.empty() ? l10n::Get(l10n::StringId::OpFailed).c_str() : error;
        } else {
            st.phase = OpPhase::Completed;
            st.completed_items = st.total_items;
            st.summary = Describe(req) + l10n::Get(l10n::StringId::Completed).c_str();
        }
    });
}

void OpsManager::RegisterShellRequest(uint32_t id) {
    if (!id) return;
    std::lock_guard<std::mutex> lock(done_mutex_);
    shell_request_ids_.insert(id);
    current_req_id_.store(id);
}

bool OpsManager::WaitShellDone(uint32_t id, uint32_t& hr, bool& cancelled, std::wstring& error) {
    auto& client = ipc::ShellClient::Instance();
    constexpr ULONGLONG kShellInactivityTimeoutMs = 10ull * 60ull * 1000ull;
    bool interrupted = false;
    {
        std::unique_lock<std::mutex> lock(done_mutex_);
        while (!done_results_.contains(id) && !stopping_.load()) {
            done_cv_.wait_for(lock, std::chrono::seconds(5));
            if (done_results_.contains(id)) break;
            const ULONGLONG activity = shell_activity_tick_.load();
            if (GetTickCount64() - activity < kShellInactivityTimeoutMs) continue;
            lock.unlock();
            client.Abort(id);
            lock.lock();
        }
        const auto result = done_results_.find(id);
        interrupted = result == done_results_.end();
        if (!interrupted) {
            hr = result->second.hr;
            cancelled = result->second.cancelled;
            // Preserve raw host error text; the UI localizes known messages.
            error = std::move(result->second.error);
            done_results_.erase(result);
        }
        shell_request_ids_.erase(id);
    }
    uint32_t expected = id;
    current_req_id_.compare_exchange_strong(expected, 0);
    shell_cancel_requested_ = false;
    if (interrupted) {
        SetStatus([](OpStatus& st) {
            st.active = false;
            st.phase = OpPhase::Failed;
            st.last_error = l10n::Get(l10n::StringId::OperationStopped).c_str();
            st.summary = l10n::Get(l10n::StringId::OperationStopped).c_str();
            st.completed_ops++;
        });
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Undo stack JSON persistence (minimal parser, same style as StagingTray).
// ---------------------------------------------------------------------------
std::wstring OpsManager::UndoToJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstring out = L"[\n";
    for (size_t i = 0; i < undo_.size(); ++i) {
        const auto& e = undo_[i];
        wchar_t head[64];
        swprintf_s(head, L"  {\"type\":%d,\"sup\":%s,", (int)e.type, e.supported ? L"true" : L"false");
        out += head;
        out += L"\"dest\":\"";
        pulse::json::Escape(e.dest_dir, out);
        out += L"\",\"name\":\"";
        pulse::json::Escape(e.new_name, out);
        out += L"\",\"src\":[";
        for (size_t j = 0; j < e.sources.size(); ++j) {
            out += L"\"";
            pulse::json::Escape(e.sources[j], out);
            out += L"\"";
            if (j + 1 < e.sources.size()) out += L",";
        }
        out += L"],\"dst\":[";
        for (size_t j = 0; j < e.destinations.size(); ++j) {
            out += L"\"";
            pulse::json::Escape(e.destinations[j], out);
            out += L"\"";
            if (j + 1 < e.destinations.size()) out += L",";
        }
        out += L"]}";
        if (i + 1 < undo_.size()) out += L",";
        out += L"\n";
    }
    out += L"]";
    return out;
}

bool OpsManager::UndoFromJson(const std::wstring& in) {
    std::deque<UndoEntry> parsed;
    size_t i = in.find(L'[');
    if (i == std::wstring::npos) return false;
    ++i;
    auto skipWs = [&] {
        while (i < in.size() && (in[i] == L' ' || in[i] == L'\n' || in[i] == L'\r' || in[i] == L'\t' || in[i] == L',')) ++i;
    };
    auto skipSpace = [&] {
        while (i < in.size() && (in[i] == L' ' || in[i] == L'\n' ||
               in[i] == L'\r' || in[i] == L'\t')) ++i;
    };
    auto readString = [&](std::wstring& out) -> bool {
        skipWs();
        if (i >= in.size() || in[i] != L'"') return false;
        ++i;
        out.clear();
        while (i < in.size() && in[i] != L'"') {
            // Plain runs in one append: the history holds thousands of paths.
            size_t run = i;
            while (run < in.size() && in[run] != L'"' && in[run] != L'\\') ++run;
            if (run > i) {
                out.append(in, i, run - i);
                i = run;
                continue;
            }
            if (in[i] == L'\\' && i + 1 < in.size()) {
                ++i;
                if (in[i] == L'n') out += L'\n';
                else if (in[i] == L'r') out += L'\r';
                else if (in[i] == L't') out += L'\t';
                else out += in[i];
            } else {
                out += in[i];
            }
            ++i;
        }
        if (i < in.size()) ++i;
        return true;
    };
    auto readValue = [&](const std::wstring& key, std::wstring& val) -> bool {
        skipWs();
        if (i >= in.size() || in[i] != L'"') return false;
        std::wstring k;
        if (!readString(k)) return false;
        skipWs();
        if (i >= in.size() || in[i] != L':') return false;
        ++i;
        skipWs();
        if (k != key) return false;
        if (i < in.size() && in[i] == L'"') return readString(val);
        size_t start = i;
        while (i < in.size() && in[i] != L',' && in[i] != L'}' && in[i] != L']') ++i;
        val = in.substr(start, i - start);
        while (!val.empty() && (val.back() == L' ')) val.pop_back();
        return true;
    };

    while (true) {
        skipWs();
        if (i >= in.size() || in[i] == L']') break;
        if (in[i] != L'{') return false;
        ++i;
        UndoEntry e;
        std::wstring v;
        if (!readValue(L"type", v)) return false;
        e.type = (OpType)_wtoi(v.c_str());
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        if (!readValue(L"sup", v)) return false;
        e.supported = (v == L"true");
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        if (!readValue(L"dest", e.dest_dir)) return false;
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        if (!readValue(L"name", e.new_name)) return false;
        skipWs();
        if (i < in.size() && in[i] == L',') ++i;
        // src array
        skipWs();
        if (i >= in.size() || in[i] != L'"') return false;
        std::wstring k;
        if (!readString(k) || k != L"src") return false;
        skipWs();
        if (i >= in.size() || in[i] != L':') return false;
        ++i;
        skipWs();
        if (i >= in.size() || in[i] != L'[') return false;
        ++i;
        while (true) {
            skipWs();
            if (i >= in.size()) return false;
            if (in[i] == L']') { ++i; break; }
            std::wstring s;
            if (!readString(s)) return false;
            e.sources.push_back(std::move(s));
        }
        skipSpace();
        // Version 2 adds actual committed destination paths. Version 1 ended
        // the object after src, so this field must remain optional.
        if (i < in.size() && in[i] == L',') {
            ++i;
            skipSpace();
            std::wstring destination_key;
            if (!readString(destination_key) || destination_key != L"dst") return false;
            skipSpace();
            if (i >= in.size() || in[i] != L':') return false;
            ++i;
            skipSpace();
            if (i >= in.size() || in[i] != L'[') return false;
            ++i;
            while (true) {
                skipWs();
                if (i >= in.size()) return false;
                if (in[i] == L']') { ++i; break; }
                std::wstring destination;
                if (!readString(destination)) return false;
                e.destinations.push_back(std::move(destination));
            }
            skipSpace();
        }
        if (i < in.size() && in[i] == L'}') ++i;
        parsed.push_back(std::move(e));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    undo_ = std::move(parsed);
    TrimUndoHistory(undo_);
    return true;
}

} // namespace pulse::ops
