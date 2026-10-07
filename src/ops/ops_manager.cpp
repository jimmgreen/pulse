// ops_manager.cpp — See ops_manager.h for the contract.
#include "ops_manager.h"
#include "../app/shell_namespace_cache.h"
#include "../common/runtime_log.h"
#include "operation_presentation.h"
#include "shell_command.h"
#include "elevated_transfer.h"
#include "../ipc/shell_client.h"
#include "../common/json_utils.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/rename_filename.h"
#include "../common/utf8_file.h"
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
#include <thread>
#include <vector>
#include <winioctl.h>

namespace pulse::ops {

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
            if (SUCCEEDED(SHParseDisplayName(shell_path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
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

void ReplaceAll(std::wstring& hay, std::wstring_view from, const std::wstring& to) {
    if (from.empty()) return;
    size_t i = 0;
    while ((i = hay.find(from, i)) != std::wstring::npos) {
        hay.replace(i, from.size(), to);
        i += to.size();
    }
}

std::wstring ExpandShellCommand(std::wstring command, const std::wstring& path) {
    const std::wstring file = pulse::path::StripExtendedPathPrefix(path);
    std::wstring quoted = L"\"";
    quoted += file;
    quoted += L'"';
    ReplaceAll(command, L"\"%1\"", quoted);
    ReplaceAll(command, L"\"%L\"", quoted);
    ReplaceAll(command, L"\"%l\"", quoted);
    ReplaceAll(command, L"\"%V\"", quoted);
    ReplaceAll(command, L"\"%v\"", quoted);
    ReplaceAll(command, L"%1", quoted);
    ReplaceAll(command, L"%L", quoted);
    ReplaceAll(command, L"%l", quoted);
    ReplaceAll(command, L"%V", quoted);
    ReplaceAll(command, L"%v", quoted);
    ReplaceAll(command, L"%*", quoted);
    return command;
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

std::wstring DisplayPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

const wchar_t* OpVerb(OpType t) {
    switch (t) {
    case OpType::Copy: return l10n::Pick(L"复制", L"Copy");
    case OpType::Move: return l10n::Pick(L"移动", L"Move");
    case OpType::RecycleDelete: return l10n::Pick(L"删除", L"Delete");
    case OpType::RealDelete: return l10n::Pick(L"永久删除", L"Permanently delete");
    case OpType::Rename: return l10n::Pick(L"重命名", L"Rename");
    case OpType::CreateFolder: return l10n::Pick(L"新建文件夹", L"New folder");
    case OpType::CreateTextFile: return l10n::Pick(L"新建文本文档", L"New text document");
    case OpType::RestoreRecycle: return l10n::Pick(L"还原", L"Restore");
    case OpType::EmptyRecycle: return l10n::Pick(L"清空回收站", L"Empty Recycle Bin");
    case OpType::BatchRename: return l10n::Pick(L"批量重命名", L"Batch rename");
    }
    return l10n::Pick(L"操作", L"Operation");
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
    if (r.type == OpType::EmptyRecycle) return s;
    s += L" ";
    if (!r.sources.empty()) s += FileName(r.sources.front());
    if (r.sources.size() > 1) {
        wchar_t buf[32];
        swprintf_s(buf, l10n::Pick(L" 等 %zu 项", L" (%zu items)"), r.sources.size());
        s += buf;
    }
    if (r.type == OpType::Copy || r.type == OpType::Move) {
        s += L" → " + DisplayPath(r.dest_dir);
    } else if (r.type == OpType::Rename) {
        s += L" → " + r.new_name;
    } else if (r.type == OpType::BatchRename && r.sources.size() > 1) {
        wchar_t buf[32];
        swprintf_s(buf, l10n::Pick(L" %zu 项", L" · %zu items"), r.sources.size());
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

enum class RenameResult { Completed, Rejected };
RenameResult RenameInProcess(const std::wstring& source, const std::wstring& new_name,
                     std::wstring* error) {
    if (source.empty() || !IsRenameComponent(new_name)) {
        if (error) *error = l10n::Pick(L"名称无效", L"Invalid name");
        return RenameResult::Rejected;
    }
    const std::wstring target = JoinPath(ParentOf(source), new_name);
    if (target.empty()) {
        if (error) *error = l10n::Pick(L"名称无效", L"Invalid name");
        return RenameResult::Rejected;
    }
    // The current shell host suppresses confirmation UI, so sending an existing
    // target there could silently overwrite it. Reject that conflict here.
    if (CompareStringOrdinal(source.c_str(), -1, target.c_str(), -1, TRUE) != CSTR_EQUAL &&
        GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (error) *error = l10n::Pick(L"目标名称已存在", L"The target name already exists");
        return RenameResult::Rejected;
    }
    if (!MoveFileW(source.c_str(), target.c_str())) {
        const DWORD code = GetLastError();
        if (error) *error = l10n::Pick(L"重命名失败（错误 ", L"Rename failed (error ") + std::to_wstring(code) + l10n::Pick(L"）", L")");
        return RenameResult::Rejected;
    }
    return RenameResult::Completed;
}

struct TransferEntry {
    std::wstring source;
    std::wstring destination;
    bool directory = false;
    bool reparse = false;
    bool destination_preexisting = false;
    bool directory_prepared = false;
    bool renamed = false;  // moved by a same-volume rename while scanning
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

class TransferRateEstimator {
public:
    void Reset(ULONGLONG tick, uint64_t bytes) {
        samples_.clear();
        samples_.push_back({ tick, bytes });
        smoothed_speed_ = 0.0;
        eta_seconds_ = 0;
        last_rate_tick_ = tick;
        last_eta_tick_ = tick;
    }

    void Observe(ULONGLONG tick, uint64_t bytes, uint64_t total_bytes) {
        if (samples_.empty() || bytes < samples_.back().bytes) {
            Reset(tick, bytes);
            return;
        }

        constexpr ULONGLONG kMinimumSampleMs = 100;
        constexpr ULONGLONG kWindowMs = 4000;
        constexpr ULONGLONG kWarmupMs = 750;
        if (tick - samples_.back().tick < kMinimumSampleMs && bytes < total_bytes)
            return;

        if (tick == samples_.back().tick) {
            samples_.back().bytes = bytes;
        } else {
            samples_.push_back({ tick, bytes });
        }
        while (samples_.size() > 2 && samples_[1].tick + kWindowMs <= tick)
            samples_.pop_front();

        const ULONGLONG span = tick - samples_.front().tick;
        const uint64_t byte_delta = bytes - samples_.front().bytes;
        if (span < kWarmupMs || byte_delta == 0) return;

        const double window_speed = static_cast<double>(byte_delta) * 1000.0
            / static_cast<double>(span);
        if (smoothed_speed_ <= 0.0) {
            smoothed_speed_ = window_speed;
        } else {
            const double seconds = static_cast<double>(tick - last_rate_tick_) / 1000.0;
            const double alpha = 1.0 - std::exp(-seconds);
            smoothed_speed_ += alpha * (window_speed - smoothed_speed_);
        }
        last_rate_tick_ = tick;

        if (bytes >= total_bytes || smoothed_speed_ <= 1.0) {
            eta_seconds_ = 0;
            return;
        }
        if (eta_seconds_ != 0 && tick - last_eta_tick_ < 1000) return;

        const double raw_eta = static_cast<double>(total_bytes - bytes) / smoothed_speed_;
        if (eta_seconds_ == 0) {
            eta_seconds_ = (std::max)(uint64_t{ 1 },
                static_cast<uint64_t>(std::ceil(raw_eta)));
        } else {
            // React faster to a slowdown than to a transient speed-up.
            const double alpha = raw_eta > static_cast<double>(eta_seconds_) ? 0.45 : 0.20;
            const double blended = static_cast<double>(eta_seconds_)
                + alpha * (raw_eta - static_cast<double>(eta_seconds_));
            eta_seconds_ = (std::max)(uint64_t{ 1 },
                static_cast<uint64_t>(std::llround(blended)));
        }
        last_eta_tick_ = tick;
    }

    double speed() const { return smoothed_speed_; }
    uint64_t eta_seconds() const { return eta_seconds_; }

private:
    struct Sample {
        ULONGLONG tick = 0;
        uint64_t bytes = 0;
    };
    std::deque<Sample> samples_;
    double smoothed_speed_ = 0.0;
    uint64_t eta_seconds_ = 0;
    ULONGLONG last_rate_tick_ = 0;
    ULONGLONG last_eta_tick_ = 0;
};

COPYFILE2_MESSAGE_ACTION CALLBACK CopyProgress(const COPYFILE2_MESSAGE* message,
                                               void* raw) {
    auto* ctx = static_cast<CopyProgressContext*>(raw);
    if (!ctx || !message) return COPYFILE2_PROGRESS_CANCEL;
    uint64_t transferred = 0;
    uint64_t total = 0;
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
        break;
    }
    const ULONGLONG now = GetTickCount64();
    if (ctx->report && (transferred == total || now - ctx->last_report >= 100)) {
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
    std::wstring text = raw ? raw : l10n::Pick(L"文件操作失败", L"File operation failed");
    if (raw) LocalFree(raw);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    return text;
}

bool EnsureDirectories(const std::wstring& dir, std::wstring& failure) {
    if (dir.empty() || IsVolumeRoot(dir)) return true;
    bool is_directory = false;
    if (PathExists(dir, &is_directory)) {
        if (is_directory) return true;
        failure = l10n::Pick(L"无法创建目标目录：", L"Could not create the destination folder: ") + dir + l10n::Pick(L" | 目标已存在且不是文件夹", L" | The target exists and is not a folder");
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(dir), error);
    if (error) {
        failure = l10n::Pick(L"无法创建目标目录：", L"Could not create the destination folder: ") + dir + L" | " + Win32Message(error.value());
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
        std::wstring name = stem + l10n::Pick(L" - 副本", L" - Copy");
        if (index > 1) name += L" (" + std::to_wstring(index) + L")";
        std::wstring candidate = JoinPath(parent, name + extension);
        if (!PathExists(candidate)) return candidate;
    }
    return JoinPath(parent, stem + l10n::Pick(L" - 副本 ", L" - Copy ") + std::to_wstring(GetTickCount64()) + extension);
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
        error = l10n::Pick(L"无法初始化 SHA-256 校验", L"Could not initialize SHA-256 verification");
        return false;
    }
    std::vector<uint8_t> object(object_bytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_bytes,
                         nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = l10n::Pick(L"无法初始化 SHA-256 校验", L"Could not initialize SHA-256 verification");
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

} // namespace

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
            L",\"names\":" + StringArrayJson(item.req.new_names) + L"}";
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

void OpsManager::Start(std::function<void()> notify) {
    if (running_) return;
    notify_ = std::move(notify);
    stopping_ = false;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(menu_mutex_);
        menu_running_ = true;
    }
    thread_ = std::thread([this] { WorkerThread(); });
#ifndef PULSE_ELEVATED_HOST
    menu_thread_ = std::thread([this] { MenuThread(); });
    {
        std::lock_guard<std::mutex> lock(open_mutex_);
        open_running_ = true;
    }
    open_thread_ = std::thread([this] { OpenThread(); });
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
    PersistJournal();
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
    item.open_verb = L"open";
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
    item.open_verb = verb.empty() ? L"open" : verb;
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

void OpsManager::OpenTerminal(const std::wstring& dir) {
    QueueItem item;
    item.open_path = dir;
    item.open_file = L"wt.exe";
    item.open_verb = L"__terminal";
    item.open_args = TerminalCommandLine(dir);
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

void OpsManager::CancelCurrent() {
    if (transfer_active_.load()) {
        transfer_cancel_.store(true);
        transfer_pause_.store(false);
        transfer_control_cv_.notify_all();
        SetStatus([](OpStatus& status) {
            if (status.active) status.phase = OpPhase::Cancelling;
        });
    }
    shell_cancel_requested_ = true;
    uint32_t id = current_req_id_.load();
    if (id != 0) ipc::ShellClient::Instance().Cancel(id);
}

void OpsManager::PauseCurrent() {
    if (!transfer_active_.load() || transfer_cancel_.load()) return;
    transfer_pause_.store(true);
}

void OpsManager::ResumeCurrent() {
    if (!transfer_active_.load()) return;
    transfer_pause_.store(false);
    transfer_control_cv_.notify_all();
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
        || req.type == OpType::RecycleDelete || req.type == OpType::RealDelete;
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
    lock_retry_ = std::move(retry);
    return report;
}

bool OpsManager::RetryLockedOperation(uint64_t task_id, bool close_owners) {
    OpRequest req;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !lock_retry_ || lock_retry_->task_id != task_id) return false;
        req = std::move(lock_retry_->req);
        req.lock_retry = true;
        if (close_owners) {
            for (const auto& owner : lock_retry_->owners)
                if (owner.closable) req.close_first.push_back(owner);
        }
        lock_retry_.reset();
    }
    Submit(std::move(req));
    return true;
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
            st.summary = l10n::Pick(L"正在结束占用文件的进程…", L"Ending the processes that are using the files…");
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
        failure = l10n::Pick(L"无法结束进程 ", L"Could not end process ") + DescribeLockOwner(owner);
        if (result == CloseOwnerResult::TimedOut) failure += l10n::Pick(L" | 进程没有及时退出", L" | The process did not exit in time");
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
        st.bytes_per_second = 0.0;
        st.eta_seconds = 0;
        st.completed_ops++;
        if (nothing_left) {
            st.phase = OpPhase::Completed;
            st.percent = 100.0f;
            st.last_error.clear();
            st.summary = summary + l10n::Pick(L" 完成", L" completed");
        } else {
            st.phase = OpPhase::Failed;
            st.percent = -1.0f;
            st.last_error = failure;
            st.summary = std::wstring(OpVerb(req.type)) + l10n::Pick(L"失败", L" failed");
        }
    });
    return false;
}

OpStatus OpsManager::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return PresentOperationStatus(status_);
}

void OpsManager::SetStatus(const std::function<void(OpStatus&)>& fn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn(status_);
    }
    if (notify_) notify_();
}

bool OpsManager::CanUndo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !undo_.empty() && undo_.back().supported;
}

std::wstring OpsManager::UndoLabel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (undo_.empty()) return L"";
    const UndoEntry& e = undo_.back();
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

void OpsManager::PushUndo(const OpRequest& req,
                          const std::vector<std::wstring>* actual_destinations) {
    CompletedOperation completed;
    completed.type = req.type;
    completed.sources = req.sources;
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
            completed.destinations.push_back(JoinPath(req.dest_dir, FileName(source)));
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
    if (req.type == OpType::RealDelete) return;              // never undoable, not recorded
    if (req.type == OpType::RestoreRecycle) return;
    if (req.type == OpType::EmptyRecycle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    undo_.push_back(std::move(e));
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

void OpsManager::Undo() {
    UndoEntry e;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (update_preparing_ || undo_.empty()) return;
        e = undo_.back();
        if (!e.supported) {
            // Leave the entry; report why it cannot be undone.
            status_.last_error = l10n::Pick(L"回收站删除暂不支持撤销（1B-2 恢复方案：枚举 $Recycle.Bin 还原）", L"Undo is not supported for items moved to the Recycle Bin");
        } else {
            undo_.pop_back();
        }
    }
    if (!e.supported) {
        if (notify_) notify_();
        return;
    }

    switch (e.type) {
    case OpType::Move: {
        // Move each dest copy back to its original parent directory.
        for (size_t i = 0; i < e.sources.size(); ++i) {
            const auto& src = e.sources[i];
            OpRequest inv;
            inv.type = OpType::Move;
            inv.is_undo = true;
            inv.sources.push_back(i < e.destinations.size()
                ? e.destinations[i] : JoinPath(e.dest_dir, FileName(src)));
            inv.dest_dir = ParentOf(src);
            Submit(std::move(inv));
        }
        break;
    }
    case OpType::Rename: {
        OpRequest inv;
        inv.type = OpType::Rename;
        inv.is_undo = true;
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
        inv.sources = e.sources;
        Submit(std::move(inv));
        break;
    }
    case OpType::RecycleDelete: {
        OpRequest inv;
        inv.type = OpType::RestoreRecycle;
        inv.is_undo = true;
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
    }
    open_cv_.notify_one();
}

void OpsManager::OpenThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
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

        if (_wcsicmp(item.open_verb.c_str(), L"__terminal") == 0) {
            const auto started = GetTickCount64();
            const auto result = LaunchTerminal(item.open_file, item.open_args, item.open_path, dialog_owner);
            diagnostics::runtime::Event("terminal_launch_result", {{"task", item.seq},
                {"open_error", result.open_error}, {"error", result.error},
                {"cancelled", result.error == ERROR_CANCELLED},
                {"elevation_requested", result.elevation_requested}, {"elapsed_ms", GetTickCount64() - started}});
            if (result.error && result.error != ERROR_CANCELLED) {
                wchar_t detail[512]{};
                FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                    result.error, 0, detail, ARRAYSIZE(detail), nullptr);
                const auto message = std::wstring(l10n::Pick(L"无法打开终端。", L"Could not open the terminal.")) +
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
                const auto message = std::wstring(l10n::Pick(L"无法启动此菜单命令。", L"Could not launch this menu command.")) +
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

        // Dedicated open thread: never wait behind transfers. Omit
        // SEE_MASK_NOASYNC so association handoff does not block this worker.
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.hwnd = dialog_owner;
        sei.lpVerb = item.open_verb.empty() ? L"open" : item.open_verb.c_str();
        // Like openas / properties above: the shell and association handlers
        // reject \\?\ paths and answer "Windows cannot find" (#55).
        const std::wstring shell_path = pulse::path::StripExtendedPathPrefix(item.open_path);
        sei.lpFile = item.open_file.empty() ? shell_path.c_str() : item.open_file.c_str();
        sei.lpParameters = item.open_args.empty() ? nullptr : item.open_args.c_str();
        // Like Explorer, start an opened item in its own folder: batch files
        // and many tools resolve relative paths against the working directory.
        const std::wstring item_dir =
            item.open_file.empty() ? OpenItemWorkingDirectory(item.open_path) : std::wstring();
        sei.lpDirectory = !item.open_file.empty() ? shell_path.c_str()
                          : item_dir.empty()      ? nullptr
                                                  : item_dir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        sei.fMask = SEE_MASK_FLAG_NO_UI;
        ShellExecuteExW(&sei); // best effort; errors surface via the OS association UI
    }
    CoUninitialize();
}

void OpsManager::WorkerThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

#ifndef PULSE_ELEVATED_HOST
    ipc::ShellClient::Callbacks cb;
    cb.progress = [this](uint32_t, float pct, std::wstring item,
                         uint32_t items_done, uint32_t total_items) {
        shell_activity_tick_ = GetTickCount64();
        SetStatus([&](OpStatus& st) {
            st.percent = pct;
            st.current_item = item;
            if (total_items > 0) st.total_items = total_items;
            st.completed_items = (std::min)(static_cast<uint64_t>(items_done),
                                             st.total_items);
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
    // Done results land here (reader thread) and RunShellOp waits on them.
    // Context-menu invoke completions share RSP_DONE; route them away first so
    // they can never overwrite the file-op completion a RunShellOp is waiting on.
    cb.done = [this](uint32_t id, uint32_t hr, bool cancelled, std::wstring error) {
        shell_activity_tick_ = GetTickCount64();
        if (ConsumeCtxInvokeDone(id)) {
            ctx_invoke_done_.fetch_add(1, std::memory_order_acq_rel);
            if (notify_) notify_();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done_id_ = id;
            done_hr_ = hr;
            done_cancelled_ = cancelled;
            done_error_ = std::move(error);
            done_ready_ = true;
        }
        done_cv_.notify_one();
    };
    cb.ctx_items = [this](uint32_t id, std::vector<ipc::CtxMenuItem> items, bool partial,
                          std::vector<std::wstring> slow_clsids) {
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
            out.push_back(std::move(m));
        }
        OnCtxItems(id, std::move(out), partial, std::move(slow_clsids));
    };
    // Shell namespace answers feed the This PC cache; the sidebar picks them up
    // on its next rebuild rather than being woken from this thread.
    cb.shell_items = [](uint32_t id, std::vector<ipc::ShellItem> items, bool ok) {
        app::DispatchShellItems(id, std::move(items), ok);
    };
    ipc::ShellClient::Instance().Start(cb);
#endif

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

        const bool run = (!item.req.lock_retry && item.req.close_first.empty()) ||
                         PrepareLockRetry(item.req, item.seq);
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
        diagnostics::runtime::Event("file_operation_end", {{"task", item.seq},
            {"phase", static_cast<uint64_t>(diagnostic_status.phase)}, {"items", diagnostic_status.completed_items},
            {"bytes", diagnostic_status.transferred_bytes}, {"has_error", !diagnostic_status.last_error.empty()},
            {"elapsed_ms", GetTickCount64() - diagnostic_started}});
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_item_.reset();
        }
        PersistJournal();
    }

#ifndef PULSE_ELEVATED_HOST
    ipc::ShellClient::Instance().Stop();
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

bool OpsManager::ConsumeCtxInvokeDone(uint32_t id) {
    std::lock_guard<std::mutex> lock(menu_mutex_);
    return ctx_invoke_ids_.erase(id) != 0;
}

bool OpsManager::TakeCtxInvokeDone() {
    return ctx_invoke_done_.exchange(0, std::memory_order_acq_rel) != 0;
}

void OpsManager::OnCtxItems(uint32_t client_id, std::vector<ShellMenuItem> items,
                            bool partial, std::vector<std::wstring> slow_clsids) {
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
    if (cb) cb(token, std::move(items), partial, std::move(slow_clsids));
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
        auto& client = ipc::ShellClient::Instance();
        switch (job.kind) {
        case MenuJob::Kind::Query: {
            const uint32_t id = client.QueryContextMenu(
                job.paths, job.owner_hwnd, job.background, job.extended,
                job.disabled_clsids);
            std::lock_guard<std::mutex> lock(menu_mutex_);
            if (id != 0) {
                menu_session_by_token_[job.token] = id;
                menu_token_by_session_[id] = job.token;
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
                                                         job.verb, job.text);
            if (id != 0) {
                std::lock_guard<std::mutex> lock(menu_mutex_);
                ctx_invoke_ids_.insert(id);
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
        }
        {
            std::lock_guard<std::mutex> lock(menu_mutex_);
            menu_dispatching_ = false;
        }
    }
}

void OpsManager::RunTransfer(const OpRequest& req, uint64_t task_id) {
    transfer_active_.store(true);
    transfer_cancel_.store(false);
    transfer_pause_.store(false);
    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        pending_conflict_.reset();
        resolved_conflict_token_ = 0;
    }

    SetStatus([&](OpStatus& st) {
        st.active = true;
        st.can_pause = true;
        st.authorization = AuthorizationState::None;
        st.can_skip_authorization = false;
        st.type = req.type;
        st.task_id = task_id;
        st.phase = OpPhase::Scanning;
        st.percent = -1.0f;
        st.summary = l10n::IsChinese()
            ? std::wstring(l10n::Cn(L"正在准备")) + OpVerb(req.type) + L"…"
            : std::wstring(OpGerund(req.type)) + L" \u2014 preparing\u2026";
        st.last_error.clear();
        st.locked_path.clear();
        st.lock_owners.clear();
        st.source_label = req.sources.empty() ? L"" : FileName(req.sources.front());
        st.destination_label = FileName(req.dest_dir);
        if (st.destination_label.empty()) st.destination_label = req.dest_dir;
        st.current_item.clear();
        st.total_bytes = st.transferred_bytes = 0;
        st.total_items = st.completed_items = 0;
        st.bytes_per_second = st.peak_bytes_per_second = 0.0;
        st.eta_seconds = 0;
    });

    // Decide before the fast move mutates any source. A whole-request retry
    // after partial completion could duplicate copies or move items twice.
#ifndef PULSE_ELEVATED_HOST
    if (!req.sources.empty() && !req.dest_dir.empty() &&
        NeedsShellTransfer(req.sources, req.dest_dir, req.type == OpType::Move)) {
        RunAuthorizedTransfer(req, task_id);
        return;
    }
#endif

    std::wstring failure;
    HRESULT failure_hr = S_OK;   // lets a lock-style failure be traced to its owner
    bool cancelled = false;
    std::vector<TransferEntry> entries;
    std::vector<std::wstring> completed_sources;
    std::vector<std::wstring> completed_destinations;
    struct ReplacementBackup {
        std::wstring original;
        std::wstring backup;
    };
    std::vector<ReplacementBackup> replacement_backups;

    if (req.sources.empty() || req.dest_dir.empty()) {
        failure = l10n::Pick(L"复制或移动请求缺少来源/目标", L"The copy or move request is missing a source or destination");
    }
    if (failure.empty() && req.is_undo && req.type == OpType::Move) {
        // A merged move can remove the now-empty source directories. Its
        // per-file undo must recreate them before attempting the fast rename.
        EnsureDirectories(req.dest_dir, failure);
    }

    // A single unobstructed same-volume move is an atomic rename and should
    // not be expanded into per-file work.
    if (failure.empty() && req.type == OpType::Move && req.sources.size() == 1) {
        const std::wstring destination = JoinPath(req.dest_dir, FileName(req.sources.front()));
        if (!PathExists(destination)) {
            if (MoveFileExW(req.sources.front().c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
                completed_sources = req.sources;
                completed_destinations.push_back(destination);
                PushUndo(req, &completed_destinations);
                SetStatus([&](OpStatus& st) {
                    st.active = false;
                    st.phase = OpPhase::Completed;
                    st.percent = 100.0f;
                    st.total_items = st.completed_items = 1;
                    st.summary = Describe(req) + l10n::Pick(L" 完成", L" completed");
                    st.completed_ops++;
                });
                transfer_active_.store(false);
                return;
            }
            const DWORD move_error = GetLastError();
#ifndef PULSE_ELEVATED_HOST
            if (move_error == ERROR_ACCESS_DENIED || move_error == ERROR_PRIVILEGE_NOT_HELD ||
                move_error == ERROR_ELEVATION_REQUIRED) {
                RunAuthorizedTransfer(req, task_id);
                return;
            }
#endif
            if (move_error != ERROR_NOT_SAME_DEVICE) {
                failure_hr = HRESULT_FROM_WIN32(move_error);
                failure = Win32Message(move_error) + L" | " + req.sources.front();
            }
        }
    }

    namespace fsys = std::filesystem;
    if (failure.empty()) {
        for (const auto& source : req.sources) {
            TransferEntry root;
            root.source = source;
            root.destination = JoinPath(req.dest_dir, FileName(source));
            if (!ReadEntryMetadata(source, root)) {
                {
                    const DWORD code = GetLastError();
                    failure_hr = HRESULT_FROM_WIN32(code);
                    failure = Win32Message(code) + L" | " + source;
                }
                break;
            }
            const bool same_location =
                _wcsicmp(root.source.c_str(), root.destination.c_str()) == 0 ||
                SameFileObject(root.source, root.destination);
            if (same_location && req.type == OpType::Move) {
                // Dropping an item onto its current folder is a no-op, like Explorer.
                continue;
            }
            if (same_location && req.type == OpType::Copy) {
                root.destination = UniqueCopyPath(root.destination, root.directory);
            }
            if (root.directory && StartsWithPath(req.dest_dir, root.source)) {
                failure = l10n::Pick(L"不能将目录复制或移动到其自身内部：", L"Cannot copy or move a folder into itself: ") + source;
                break;
            }
            root.destination_preexisting = PathExists(root.destination);
            // On one volume a move is a rename however many items are dropped
            // (#60). Recorded as completed at once so a later failure still
            // offers undo for it; anything a rename cannot do (another volume,
            // a lock) takes the per-file path below.
            if (req.type == OpType::Move && !root.destination_preexisting &&
                MoveFileExW(source.c_str(), root.destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
                root.renamed = true;
                completed_sources.push_back(root.source);
                completed_destinations.push_back(root.destination);
                entries.push_back(root);
                continue;
            }
            entries.push_back(root);
            if (!root.directory || root.reparse) continue;

            std::error_code ec;
            fsys::recursive_directory_iterator it(fsys::path(source),
                fsys::directory_options::skip_permission_denied, ec);
            fsys::recursive_directory_iterator end;
            if (ec) {
                failure = l10n::Pick(L"无法读取来源目录：", L"Could not read the source folder: ") + source + L" | " + Win32Message(ec.value());
                break;
            }
            for (; it != end; it.increment(ec)) {
                if (ec) {
                    failure = l10n::Pick(L"扫描来源目录失败：", L"Could not scan the source folder: ") + source + L" | " + Win32Message(ec.value());
                    break;
                }
                TransferEntry child;
                child.source = it->path().wstring();
                child.destination = (fsys::path(root.destination) /
                    it->path().lexically_relative(fsys::path(source))).wstring();
                if (!ReadEntryMetadata(child.source, child)) {
                    {
                        const DWORD code = GetLastError();
                        failure_hr = HRESULT_FROM_WIN32(code);
                        failure = Win32Message(code) + L" | " + child.source;
                    }
                    break;
                }
                entries.push_back(std::move(child));
                if (entries.back().reparse && entries.back().directory) it.disable_recursion_pending();
            }
            if (!failure.empty()) break;
        }
    }

    // Accidental same-folder drops leave zero work. Do not flash the status bar
    // with "移动 foo → dest 完成" for a no-op Explorer already ignores.
    if (failure.empty() && entries.empty()) {
        SetStatus([&](OpStatus& st) {
            st.completed_ops++;
            st.active = false;
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
        if (failure.empty()) {
            st.phase = OpPhase::Running;
            st.percent = 0.0f;
            st.summary = ProgressSummary(req.type, entries.size());
        }
    });

    bool apply_all = req.collision_policy != CollisionPolicy::System;
    ConflictChoice repeated = req.collision_policy == CollisionPolicy::KeepBoth
        ? ConflictChoice::KeepBoth : ConflictChoice::Replace;
    std::vector<std::wstring> skipped_prefixes;
    uint64_t committed_bytes = 0;
    uint64_t published_bytes = 0;
    uint64_t processed_items = 0;
    TransferRateEstimator rate;
    rate.Reset(GetTickCount64(), 0);

    auto publish_progress = [&](const TransferEntry& entry, uint64_t file_done,
                                uint64_t file_total, uint64_t completed_items) {
        const uint64_t bounded_file_done = file_total > 0
            ? (std::min)(file_done, file_total) : file_done;
        uint64_t overall = committed_bytes + bounded_file_done;
        if (overall < committed_bytes) overall = UINT64_MAX;
        overall = (std::max)(published_bytes, overall);
        if (total_bytes > 0) overall = (std::min)(overall, total_bytes);
        published_bytes = overall;
        const ULONGLONG now = GetTickCount64();
        rate.Observe(now, overall, total_bytes);
        SetStatus([&](OpStatus& st) {
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

    auto reset_rate = [&] {
        rate.Reset(GetTickCount64(), published_bytes);
        SetStatus([&](OpStatus& st) {
            st.bytes_per_second = 0.0;
            st.eta_seconds = 0;
        });
    };

    auto mark_skipped = [&](const TransferEntry& entry) {
        total_bytes -= (std::min)(total_bytes, entry.bytes);
        ++processed_items;
        rate.Observe(GetTickCount64(), published_bytes, total_bytes);
        SetStatus([&](OpStatus& st) {
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

    for (size_t index = 0; failure.empty() && index < entries.size(); ++index) {
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
        if (SameFileObject(entry.source, entry.destination)) {
            if (entry.directory) skipped_prefixes.push_back(entry.source);
            mark_skipped(entry);
            continue;
        }

        bool destination_is_directory = false;
        bool destination_exists = PathExists(entry.destination, &destination_is_directory);
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
            {
                std::lock_guard<std::mutex> lock(transfer_control_mutex_);
                info.token = next_conflict_token_++;
                pending_conflict_ = info;
                resolved_conflict_token_ = 0;
            }
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::WaitingForConflict;
                st.current_item = FileName(entry.source);
                st.summary = l10n::Pick(L"正在等待处理文件冲突", L"Waiting for file conflicts to be resolved");
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            std::unique_lock<std::mutex> lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] {
                return transfer_cancel_.load() || resolved_conflict_token_ == info.token;
            });
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
            rate.Reset(GetTickCount64(), published_bytes);
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Running;
                st.summary = ProgressSummary(req.type, entries.size());
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
                failure = l10n::Pick(L"移动时无法原子替换不同类型或重解析目标：", L"Cannot atomically replace a target of a different type or a reparse point while moving: ") + entry.destination;
                break;
            }
            const std::wstring backup = UniqueTemporaryPath(
                entry.destination, L".pulse-backup-", task_id, index);
            if (!MoveFileExW(entry.destination.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
                failure = l10n::Pick(L"无法安全备份要替换的目标项目：", L"Could not safely back up the item being replaced: ") + entry.destination
                    + L" | " + Win32Message(GetLastError());
                break;
            }
            replacement_backups.push_back({ entry.destination, backup });
            destination_exists = false;
            destination_is_directory = false;
        }

        if (entry.reparse) {
            if (!EnsureDirectories(ParentOf(entry.destination), failure)) break;
            if (!CopyReparsePoint(entry, failure)) break;
            if (req.type == OpType::Move) {
                const BOOL removed = entry.directory ? RemoveDirectoryW(entry.source.c_str())
                                                     : DeleteFileW(entry.source.c_str());
                if (!removed) {
                    {
                        const DWORD code = GetLastError();
                        failure_hr = HRESULT_FROM_WIN32(code);
                        failure = Win32Message(code) + L" | " + entry.source;
                    }
                    break;
                }
            }
            completed_sources.push_back(entry.source);
            completed_destinations.push_back(entry.destination);
            ++processed_items;
            publish_progress(entry, entry.bytes, entry.bytes, processed_items);
            committed_bytes += entry.bytes;
            continue;
        }

        if (entry.directory) {
            if (!EnsureDirectories(entry.destination, failure)) break;
            entry.directory_prepared = true;
            ++processed_items;
            SetStatus([&](OpStatus& st) {
                st.current_item = FileName(entry.source);
                st.completed_items = processed_items;
                if (st.total_bytes == 0 && st.total_items > 0)
                    st.percent = static_cast<float>(st.completed_items * 100.0 / st.total_items);
            });
            continue;
        }

        if (!EnsureDirectories(ParentOf(entry.destination), failure)) break;

        const bool direct = !destination_exists && entry.bytes < kDirectCopyMaxBytes;
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
                SetStatus([&](OpStatus& st) { st.total_bytes = total_bytes; });
            }
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
        DWORD extra_flags = COPY_FILE_COPY_SYMLINK;
        for (;;) {
            context.pause_sent = false;
            COPYFILE2_EXTENDED_PARAMETERS parameters{};
            parameters.dwSize = sizeof(parameters);
            parameters.dwCopyFlags = extra_flags;
            if (!resume) parameters.dwCopyFlags |= COPY_FILE_FAIL_IF_EXISTS;
            if (resume) parameters.dwCopyFlags |= COPY_FILE_RESUME_FROM_PAUSE;
            parameters.pfCancel = nullptr;
            parameters.pProgressRoutine = CopyProgress;
            parameters.pvCallbackContext = &context;
            copy_result = CopyFile2(entry.source.c_str(), copy_destination.c_str(), &parameters);
            if (copy_result == HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER) && extra_flags != 0 && !resume) {
                extra_flags = 0;
                continue;
            }
            if (copy_result != HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED)) break;

            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Paused;
                st.summary = std::wstring(OpVerb(req.type)) + l10n::Pick(L"已暂停", L" paused");
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            std::unique_lock<std::mutex> lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] {
                return transfer_cancel_.load() || !transfer_pause_.load();
            });
            if (transfer_cancel_.load()) {
                cancelled = true;
                break;
            }
            resume = true;
            reset_rate();
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Running;
                st.summary = ProgressSummary(req.type, entries.size());
            });
        }
        if (cancelled || transfer_cancel_.load()) {
            cancelled = true;
            discard_copy();
            break;
        }
        if (FAILED(copy_result)) {
            discard_copy();
            failure_hr = copy_result;
            failure = Win32Message(HRESULT_CODE(copy_result)) + L" | " + entry.source;
            break;
        }

        if (verify_copies_.load()) {
            SetStatus([&](OpStatus& st) {
                st.phase = OpPhase::Verifying;
                st.summary = l10n::Pick(L"正在校验 ", L"Verifying ") + FileName(entry.source);
                st.bytes_per_second = 0.0;
                st.eta_seconds = 0;
            });
            std::array<uint8_t, 32> source_hash{};
            std::array<uint8_t, 32> copied_hash{};
            auto wait_if_paused = [&] {
                if (!transfer_pause_.load() || transfer_cancel_.load()) return;
                SetStatus([&](OpStatus& st) {
                    st.phase = OpPhase::Paused;
                    st.summary = std::wstring(OpVerb(req.type)) + l10n::Pick(L"已暂停", L" paused");
                });
                std::unique_lock<std::mutex> lock(transfer_control_mutex_);
                transfer_control_cv_.wait(lock, [&] {
                    return transfer_cancel_.load() || !transfer_pause_.load();
                });
                lock.unlock();
                if (!transfer_cancel_.load()) SetStatus([&](OpStatus& st) {
                    st.phase = OpPhase::Verifying;
                    st.summary = l10n::Pick(L"正在校验 ", L"Verifying ") + FileName(entry.source);
                });
            };
            if (!Sha256File(entry.source, transfer_cancel_, wait_if_paused, source_hash, failure) ||
                !Sha256File(copy_destination, transfer_cancel_, wait_if_paused, copied_hash, failure)) {
                discard_copy();
                if (transfer_cancel_.load()) cancelled = true;
                break;
            }
            if (source_hash != copied_hash) {
                discard_copy();
                failure = l10n::Pick(L"SHA-256 校验失败 | ", L"SHA-256 verification failed | ") + entry.source;
                break;
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
                failure_hr = HRESULT_FROM_WIN32(error);
                failure = Win32Message(error) + L" | " + entry.destination;
                break;
            }
        } else if (!direct && !MoveFileExW(copy_destination.c_str(), entry.destination.c_str(),
                                           MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            DeleteFileW(copy_destination.c_str());
            failure_hr = HRESULT_FROM_WIN32(error);
            failure = Win32Message(error) + L" | " + entry.destination;
            break;
        }

        if (req.type == OpType::Move) {
            DWORD source_attributes = GetFileAttributesW(entry.source.c_str());
            if (source_attributes != INVALID_FILE_ATTRIBUTES &&
                (source_attributes & FILE_ATTRIBUTE_READONLY))
                SetFileAttributesW(entry.source.c_str(), source_attributes & ~FILE_ATTRIBUTE_READONLY);
            if (!DeleteFileW(entry.source.c_str())) {
                {
                    const DWORD code = GetLastError();
                    failure_hr = HRESULT_FROM_WIN32(code);
                    failure = Win32Message(code) + L" | " + entry.source;
                }
                break;
            }
        }
        completed_sources.push_back(entry.source);
        completed_destinations.push_back(entry.destination);
        ++processed_items;
        publish_progress(entry, entry.bytes, entry.bytes, processed_items);
        committed_bytes += entry.bytes;
    }

    if (failure.empty() && !cancelled) {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (!it->directory_prepared) continue;
            if (req.type == OpType::Move) {
                RemoveDirectoryW(it->source.c_str());
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
                rollback_note += l10n::Pick(L"\uFF1B\u65E0\u6CD5\u8FD8\u539F ",
                                            L"; could not restore ") + it->original +
                    l10n::Pick(L"\uFF0C\u539F\u6587\u4EF6\u4FDD\u7559\u5728 ",
                               L", original kept at ") + it->backup + L" | " +
                    Win32Message(error);
            }
        }
        for (size_t i = completed_destinations.size(); i-- > 0;) {
            const bool restored = std::any_of(replacement_backups.begin(),
                replacement_backups.end(), [&](const ReplacementBackup& backup) {
                    return StartsWithPath(completed_destinations[i], backup.original);
                });
            if (restored) {
                completed_destinations.erase(completed_destinations.begin() + i);
                completed_sources.erase(completed_sources.begin() + i);
            }
        }
    }

    if (failure.empty() && !cancelled) {
        OpRequest committed = req;
        committed.sources.clear();
        std::vector<std::wstring> committed_destinations;
        for (size_t root_index = 0; root_index < req.sources.size(); ++root_index) {
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const TransferEntry& entry) {
                return _wcsicmp(entry.source.c_str(), req.sources[root_index].c_str()) == 0;
            });
            if (found == entries.end()) continue;
            const std::wstring original_destination = JoinPath(req.dest_dir,
                                                               FileName(req.sources[root_index]));
            const bool independent_root = !found->destination_preexisting
                || _wcsicmp(found->destination.c_str(), original_destination.c_str()) != 0;
            if (independent_root) {
                committed.sources.push_back(req.sources[root_index]);
                committed_destinations.push_back(found->destination);
                continue;
            }
            for (size_t item = 0; item < completed_sources.size(); ++item) {
                if (StartsWithPath(completed_sources[item], req.sources[root_index])) {
                    committed.sources.push_back(completed_sources[item]);
                    committed_destinations.push_back(completed_destinations[item]);
                }
            }
        }
        if (!committed_destinations.empty()) PushUndo(committed, &committed_destinations);
    } else if (!completed_destinations.empty()) {
        OpRequest partial = req;
        partial.sources = completed_sources;
        PushUndo(partial, &completed_destinations);
    }

    {
        std::lock_guard<std::mutex> lock(transfer_control_mutex_);
        pending_conflict_.reset();
    }
    const bool transfer_cancelled = cancelled || transfer_cancel_.load();
    diagnostics::runtime::Event("file_transfer_result", {{"task", task_id},
        {"hresult", static_cast<uint32_t>(failure_hr)}, {"has_error", !failure.empty()},
        {"cancelled", transfer_cancelled}, {"completed_sources", completed_sources.size()}});
    const LockReport lock_report = !transfer_cancelled && !failure.empty()
        ? ProbeLock(req, task_id, failure_hr, failure) : LockReport{};
    SetStatus([&](OpStatus& st) {
        st.active = false;
        st.completed_ops++;
        st.bytes_per_second = 0.0;
        st.eta_seconds = 0;
        st.locked_path = lock_report.path;
        st.lock_owners = lock_report.owners;
        if (transfer_cancelled) {
            st.phase = OpPhase::Failed;
            st.last_error = l10n::Pick(L"已取消", L"Canceled") + rollback_note;
            st.summary = std::wstring(OpVerb(req.type)) + l10n::Pick(L"已取消", L" canceled");
        } else if (!failure.empty()) {
            st.phase = OpPhase::Failed;
            st.last_error = failure + rollback_note;
            st.summary = std::wstring(OpVerb(req.type)) + l10n::Pick(L"失败", L" failed");
        } else {
            st.phase = OpPhase::Completed;
            st.percent = 100.0f;
            st.transferred_bytes = st.total_bytes;
            st.completed_items = st.total_items;
            st.summary = Describe(req) + l10n::Pick(L" 完成", L" completed");
            st.last_error.clear();
        }
    });
    transfer_active_.store(false);
}

void OpsManager::RunShellOp(const OpRequest& req, uint64_t task_id) {
    shell_cancel_requested_ = false;
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
                st.last_error = cancelled() ? l10n::Pick(L"已取消", L"Canceled") :
                    l10n::Pick(L"重复文件已变化或无法验证。请重新扫描后再清理。",
                               L"Duplicate files changed or could not be verified. Scan again before cleaning up.");
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
        SHQUERYRBINFO start{};
        start.cbSize = sizeof(start);
        const bool have_start = SUCCEEDED(SHQueryRecycleBinW(nullptr, &start))
            && start.i64NumItems >= 0;
        const int64_t start_items = have_start ? start.i64NumItems : 0;
        const int64_t start_bytes = have_start ? start.i64Size : 0;
        SetStatus([&](OpStatus& st) {
            st.current_item = OpVerb(OpType::EmptyRecycle);
            st.summary = st.current_item;
            st.total_items = start_items > 0 ? static_cast<uint64_t>(start_items) : 0;
            st.total_bytes = start_bytes > 0 ? static_cast<uint64_t>(start_bytes) : 0;
            st.percent = have_start && start_items > 0 ? 0.0f : -1.0f;
        });

        std::atomic<bool> emptying{true};
        std::thread poller([&] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            while (emptying.load(std::memory_order_relaxed) && !stopping_.load()) {
                SHQUERYRBINFO now{};
                now.cbSize = sizeof(now);
                if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &now)) && have_start && start_items > 0) {
                    const int64_t left = (std::max)(int64_t{0}, now.i64NumItems);
                    const int64_t done_items = (std::max)(int64_t{0}, start_items - left);
                    const int64_t left_bytes = (std::max)(int64_t{0}, now.i64Size);
                    const int64_t done_bytes = (std::max)(int64_t{0}, start_bytes - left_bytes);
                    SetStatus([&](OpStatus& st) {
                        if (!st.active || st.type != OpType::EmptyRecycle) return;
                        st.completed_items = static_cast<uint64_t>(done_items);
                        st.total_items = static_cast<uint64_t>(start_items);
                        st.transferred_bytes = static_cast<uint64_t>(done_bytes);
                        st.total_bytes = static_cast<uint64_t>((std::max)(start_bytes, int64_t{0}));
                        st.percent = 100.0f * static_cast<float>(done_items)
                            / static_cast<float>(start_items);
                        st.current_item = OpVerb(OpType::EmptyRecycle);
                    });
                }
                for (int i = 0; i < 8 && emptying.load(std::memory_order_relaxed); ++i)
                    Sleep(50);
            }
            CoUninitialize();
        });

        const HRESULT hr = SHEmptyRecycleBinW(nullptr, nullptr,
            SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        emptying.store(false, std::memory_order_relaxed);
        if (poller.joinable()) poller.join();

        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.percent = -1.0f;
            st.completed_ops++;
            if (FAILED(hr)) {
                st.phase = OpPhase::Failed;
                st.last_error = l10n::Pick(L"无法清空回收站", L"Could not empty the Recycle Bin");
            } else {
                st.phase = OpPhase::Completed;
                st.completed_items = st.total_items;
                st.summary = Describe(req) + l10n::Pick(L" 完成", L" completed");
            }
        });
        return;
    }

    auto& client = ipc::ShellClient::Instance();

    if (req.type == OpType::Rename || req.type == OpType::BatchRename) {
        bool valid = !req.sources.empty() && (req.type != OpType::Rename || req.sources.size() == 1);
        for (size_t i = 0; i < req.sources.size(); ++i) {
            const auto& name = req.type == OpType::BatchRename && i < req.new_names.size() ? req.new_names[i] : req.new_name;
            valid = valid && !req.sources[i].empty() && IsRenameComponent(name);
        }
        if (!valid || shell_cancel_requested_.load() || stopping_.load()) {
            SetStatus([&](OpStatus& st) {
                st.active = false; st.phase = OpPhase::Failed; st.percent = -1.0f;
                ++st.completed_ops; st.last_error = valid ? l10n::Pick(L"已取消", L"Canceled") : l10n::Pick(L"名称无效", L"Invalid name");
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
            PushUndo(recorded, &ok_destinations);
        }
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.percent = -1.0f;
            st.completed_ops++;
            st.completed_items = ok_sources.size();
            if (cancelled) {
                st.phase = OpPhase::Failed;
                st.last_error = l10n::Pick(L"已取消", L"Canceled");
            } else if (ok_sources.empty()) {
                st.phase = OpPhase::Failed;
                st.last_error = last_error.empty() ? l10n::Pick(L"操作失败", L"Operation failed") : last_error;
            } else {
                st.phase = OpPhase::Completed;
                st.summary = Describe(req) + l10n::Pick(L" 完成", L" completed");
                if (ok_sources.size() != req.sources.size())
                    st.last_error = last_error;
            }
        });
        return;
    }

    // A single rename shares the batch path's validation and no-overwrite rules.
    if (req.type == OpType::Rename && req.sources.size() == 1) {
        std::wstring local_error;
        const auto local = RenameInProcess(req.sources.front(), req.new_name, &local_error);
        if (local == RenameResult::Rejected) {
            SetStatus([&](OpStatus& st) {
                st.active = false; st.phase = OpPhase::Failed; st.percent = -1.0f;
                ++st.completed_ops; st.last_error = local_error;
            });
            return;
        }
        if (local == RenameResult::Completed) {
            PushUndo(req);
            SetStatus([&](OpStatus& st) {
                st.active = false;
                st.percent = -1.0f;
                st.completed_ops++;
                st.completed_items = st.total_items;
                st.phase = OpPhase::Completed;
                st.summary = Describe(req) + l10n::Pick(L" 完成", L" completed");
            });
            return;
        }
    }

    uint32_t id = 0;
    switch (req.type) {
    case OpType::Copy:
    case OpType::Move:
    case OpType::EmptyRecycle:
    case OpType::BatchRename:
        break;
    case OpType::RecycleDelete: id = client.DeleteRecycle(req.sources); break;
    case OpType::RealDelete: id = client.RealDelete(req.sources); break;
    case OpType::Rename: // Handled above; never send rename to the Shell host.
        break;
    case OpType::CreateFolder:
        if (!req.sources.empty()) id = client.CreateFolder(req.sources.front());
        break;
    case OpType::CreateTextFile:
        if (!req.sources.empty()) id = client.CreateNewFile(req.sources.front());
        break;
    case OpType::RestoreRecycle:
        id = client.RestoreRecycle(req.sources);
        break;
    }
    current_req_id_.store(id);
    if (shell_cancel_requested_.load() && id != 0) client.Cancel(id);

    if (id == 0) {
        SetStatus([&](OpStatus& st) {
            st.active = false;
            st.phase = OpPhase::Failed;
            st.percent = -1.0f;
            st.last_error = l10n::Pick(L"操作层未启动", L"The file operation service is not running");
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
    if (ok) PushUndo(req);
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
            st.last_error = l10n::Pick(L"已取消", L"Canceled");
        } else if (FAILED((HRESULT)hr)) {
            st.phase = OpPhase::Failed;
            st.last_error = error.empty() ? l10n::Pick(L"操作失败", L"Operation failed") : error;
        } else {
            st.phase = OpPhase::Completed;
            st.completed_items = st.total_items;
            st.summary = Describe(req) + l10n::Pick(L" 完成", L" completed");
        }
    });
}

bool OpsManager::WaitShellDone(uint32_t id, uint32_t& hr, bool& cancelled, std::wstring& error) {
    auto& client = ipc::ShellClient::Instance();
    constexpr ULONGLONG kShellInactivityTimeoutMs = 10ull * 60ull * 1000ull;
    {
        std::unique_lock<std::mutex> lock(done_mutex_);
        while (!(done_ready_ && done_id_ == id) && !stopping_.load()) {
            done_cv_.wait_for(lock, std::chrono::seconds(5));
            if (GetTickCount64() - shell_activity_tick_.load() < kShellInactivityTimeoutMs)
                continue;
            lock.unlock();
            client.Abort(id);
            lock.lock();
        }
        if (stopping_.load() && !(done_ready_ && done_id_ == id)) {
            current_req_id_.store(0);
            shell_cancel_requested_ = false;
            SetStatus([](OpStatus& st) {
                st.active = false;
                st.phase = OpPhase::Failed;
                st.last_error = l10n::Pick(L"操作已停止", L"Operation stopped");
                st.summary = l10n::Pick(L"操作已停止", L"Operation stopped");
                st.completed_ops++;
            });
            return false;
        }
    }
    hr = done_hr_;
    cancelled = done_cancelled_;
    // Shell host errors (Simplified text or a system message, often with the
    // failed item appended) stay as reported: error matching needs the raw text
    // and the UI localizes known messages with ServiceErrorText at display time.
    error = done_error_;
    done_error_.clear();
    done_ready_ = false;
    current_req_id_.store(0);
    shell_cancel_requested_ = false;
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
    return true;
}

} // namespace pulse::ops

