#include "context_working_directory.h"
#include "host_log_batch.h"
#include "recycle_restore_index.h"
#include "../common/recycle_restore.h"
#include "../common/windows_compat.h"
// main.cpp — pulse_shell.exe: windowless STA+COM shell-operation proxy.
//
// Pipe server on \\.\pipe\pulse_shell_<ui-pid> (ui-pid passed as argv[1],
// defaults to own pid for standalone runs). Requests are read on a dedicated
// thread and marshalled to the STA main thread (message-only window), where
// IFileOperation executes synchronously with an IFileOperationProgressSink
// reporting RSP_PROGRESS / RSP_DONE back over the pipe.
//
// The process is a stateless proxy: if it dies the UI side restarts it and
// retries the in-flight request once (see src/ipc/shell_client.cpp).
#include "../ipc/protocol.h"
#include "../ipc/ctx_menu_util.h"
#include "ctx_handlers.h"
#include "isolated_start_pin.h"
#include "start_pin_client.h"
#include "packaged_ctx_handlers.h"
#include "../common/current_user_security.h"
#include "../common/path_utils.h"
#include "../common/shell_id.h"
#include "../common/recycle_index.h"
#include "../common/crash_reporter.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sherrors.h>
#include <algorithm>
#include <cstdio>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cwctype>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

using namespace pulse::ipc;

namespace {

constexpr UINT WM_EXEC_REQUEST = WM_APP + 1;
constexpr UINT WM_QUIT_HOST = WM_APP + 2;
constexpr UINT WM_RECYCLE_CHECK = WM_APP + 3; // see MaybeRecycleHost
// Context-menu session thread messages (defined below with the session code).
constexpr UINT WM_CTX_INVOKE = WM_APP + 10;  // lParam = CtxInvokeMsg*
constexpr UINT WM_CTX_CLOSE = WM_APP + 11;

struct CtxInvokeMsg {
    uint32_t invoke_req_id = 0;
    uint32_t item_id = 0;
    std::wstring verb;
    std::wstring text;
};

void StartCtxSession(uint32_t session_id, const uint8_t* payload, size_t size);
void PostCtxMessage(uint32_t session_id, UINT message, WPARAM wParam, LPARAM lParam);

struct Request {
    uint32_t type = 0;
    uint32_t id = 0;
    std::wstring new_name;  // REQ_SHELL_VERB: the verb
    std::vector<std::wstring> sources;
    uint32_t owner = 0;     // REQ_SHELL_VERB: owner window for the shell's dialogs
    uint32_t flags = 0;     // REQ_SHELL_VERB: SVF_*; REQ_SHELL_TRANSFER: STF_*
    uint32_t op = 0;        // REQ_SHELL_TRANSFER: STO_*
    std::wstring dest;      // REQ_SHELL_TRANSFER: target folder
    std::vector<std::wstring> names;  // REQ_SHELL_TRANSFER: new leaf names
};

struct HostState {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::mutex write_mutex;      // reader thread (PONG) vs main thread (progress/done)
    HWND hwnd_msg = nullptr;
    std::atomic<uint32_t> cancel_id{0};
    std::atomic<bool> running{true};
    std::atomic<int> requests_in_flight{0}; // posted WM_EXEC_REQUEST not finished yet
} g;

// Crash-only diagnostics (plan §11: crashes are a normal design case).
// Log under %LOCALAPPDATA%\Pulse: an installed copy lives in Program Files,
// where the exe directory is not writable for a non-admin user.
// The path is resolved once; past 1 MiB the log moves to .log.1 (one
// generation kept, like pulse-index-timing.jsonl).
constexpr LONGLONG kHostLogMaxBytes = 1024 * 1024;
const std::wstring& HostLogPath() {
    static const std::wstring path = [] {
        wchar_t dir[32768]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, ARRAYSIZE(dir));
        if ((!n || n >= ARRAYSIZE(dir)) &&
            FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir)))
            return std::wstring();
        std::wstring p = std::wstring(dir) + L"\\Pulse";
        CreateDirectoryW(p.c_str(), nullptr);
        return p + L"\\pulse_shell_host.log";
    }();
    return path;
}

std::mutex& HostLogMutex() {
    static std::mutex mutex;
    return mutex;
}

void HostLog(const wchar_t* msg) {
    std::lock_guard<std::mutex> lock(HostLogMutex());
    const std::wstring& path = HostLogPath();
    if (path.empty()) return;
    const auto open_log = [&path] {
        return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    HANDLE f = open_log();
    LARGE_INTEGER size{};
    if (f != INVALID_HANDLE_VALUE && GetFileSizeEx(f, &size) && size.QuadPart > kHostLogMaxBytes) {
        CloseHandle(f);
        // Another host holding the file makes the move fail: keep appending.
        MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
        f = open_log();
    }
    if (f != INVALID_HANDLE_VALUE) {
        // Bound each entry and encode explicitly: the C locale can reject
        // non-ASCII menu labels when printf converts a wide string.
        size_t length = 0;
        while (msg && msg[length] && length < 2048) ++length;
        if (length && msg[length - 1] >= 0xD800 && msg[length - 1] <= 0xDBFF) --length;
        std::wstring line = L"[" + std::to_wstring(GetCurrentProcessId()) + L"] ";
        if (length) line.append(msg, length);
        line += L"\n";
        char buf[8192];
        const int n = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                                          buf, static_cast<int>(sizeof(buf)), nullptr, nullptr);
        if (n > 0) {
            DWORD written = 0;
            WriteFile(f, buf, static_cast<DWORD>(n), &written, nullptr);
        }
        CloseHandle(f);
    }
}

bool ContextDiagnosticsEnabled() {
    static const bool enabled = [] {
        wchar_t exe[32768]{};
        const DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        if (!n || n >= ARRAYSIZE(exe)) return false;
        std::wstring path(exe, n);
        const auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return false;
        path.resize(slash + 1);
        path += L"pulse-context-diagnostics.flag";
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }();
    return enabled;
}

std::wstring DiagnosticText(std::wstring value) {
    if (value.size() > 96) value.resize(96);
    for (auto& c : value) if (c < L' ' || c == 0x7f) c = L' ';
    return value;
}

void ContextDiagnostic(uint32_t sid, const std::wstring& message) {
    if (!ContextDiagnosticsEnabled()) return;
    HostLog((L"ctxdiag sid=" + std::to_wstring(sid) + L" " + message).c_str());
}

void DiagnosticRawRows(uint32_t sid, HMENU menu, const std::wstring& source, UINT depth = 0) {
    if (!ContextDiagnosticsEnabled() || !menu || depth > 8) return;
    const int count = (std::min)(GetMenuItemCount(menu), 256);
    for (int i = 0; i < count; ++i) {
        wchar_t label[97]{};
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_STRING | MIIM_SUBMENU;
        info.dwTypeData = label;
        info.cch = ARRAYSIZE(label);
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) continue;
        ContextDiagnostic(sid, L"raw source=" + DiagnosticText(source) + L" depth=" + std::to_wstring(depth) +
            L" id=" + std::to_wstring(info.wID) + L" state=" + std::to_wstring(info.fState) +
            L" type=" + std::to_wstring(info.fType) + L" text=" + DiagnosticText(label));
        DiagnosticRawRows(sid, info.hSubMenu, source, depth + 1);
    }
}

bool SendMsg(uint32_t type, uint32_t request_id, const std::vector<uint8_t>& payload) {
    MsgHeader h;
    h.type = type;
    h.request_id = request_id;
    h.payload_size = (uint32_t)payload.size();
    // Header + payload under one lock: ctx session threads write concurrently
    // with the main STA, an interleaved frame would desync the whole pipe.
    std::lock_guard<std::mutex> lock(g.write_mutex);
    if (!PipeWrite(g.pipe, reinterpret_cast<const uint8_t*>(&h), sizeof(h))) return false;
    if (!payload.empty())
        return PipeWrite(g.pipe, payload.data(), (DWORD)payload.size());
    return true;
}

void SendProgress(uint32_t id, float percent, const std::wstring& item,
                  size_t items_done, size_t total_items) {
    PayloadWriter w;
    w.PutF32(percent);
    w.PutString(item);
    w.PutU32(static_cast<uint32_t>((std::min)(items_done,
        static_cast<size_t>(UINT32_MAX))));
    w.PutU32(static_cast<uint32_t>((std::min)(total_items,
        static_cast<size_t>(UINT32_MAX))));
    SendMsg(RSP_PROGRESS, id, w.data());
}

void SendDone(uint32_t id, HRESULT hr, bool cancelled, const std::wstring& error) {
    PayloadWriter w;
    w.PutU32((uint32_t)hr);
    w.PutU32(cancelled ? 1 : 0);
    w.PutString(error);
    SendMsg(RSP_DONE, id, w.data());
}

// ---------------------------------------------------------------------------
// Progress sink: lives on the STA main thread, drives RSP_PROGRESS + cancel.
// ---------------------------------------------------------------------------
class ProgressSink : public IFileOperationProgressSink {
public:
    ProgressSink(uint32_t req_id, size_t total_items)
        : req_id_(req_id), total_items_(total_items ? total_items : 1) {}

    // IUnknown — stack-allocated, no real refcounting.
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) {
            *out = static_cast<IFileOperationProgressSink*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP StartOperations() override { return CheckCancel(); }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }

    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem* psi, LPCWSTR new_name) override {
        if (new_name) RememberName(new_name);
        else RememberItem(psi);
        return CheckCancel();
    }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT hr, IShellItem*) override {
        NoteItemResult(hr);
        ++items_done_;
        MaybeReport(items_done_ == total_items_);
        return CheckCancel();
    }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem* psi, IShellItem*, LPCWSTR) override {
        RememberItem(psi);
        return CheckCancel();
    }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem*) override {
        NoteItemResult(hr);
        ++items_done_;
        MaybeReport(items_done_ == total_items_);
        return CheckCancel();
    }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem* psi, IShellItem*, LPCWSTR) override {
        RememberItem(psi);
        return CheckCancel();
    }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem*) override {
        NoteItemResult(hr);
        ++items_done_;
        MaybeReport(items_done_ == total_items_);
        return CheckCancel();
    }
    IFACEMETHODIMP PreDeleteItem(DWORD flags, IShellItem* psi) override {
        RememberItem(psi);
        // A recycle the Shell cannot put in the bin (bin off, item too large,
        // no bin on the drive) arrives without this flag and would be deleted
        // for good under FOF_NOCONFIRMATION. Refuse it; nothing is touched.
        constexpr DWORD kRecycleIfPossible = 0x80;  // TSF_DELETE_RECYCLE_IF_POSSIBLE
        if (recycle_only_ && !(flags & kRecycleIfPossible)) {
            NoteItemResult(COPYENGINE_E_RECYCLE_BIN_NOT_FOUND);
            return COPYENGINE_E_RECYCLE_BIN_NOT_FOUND;
        }
        return CheckCancel();
    }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT hr, IShellItem*) override {
        NoteItemResult(hr);
        ++items_done_;
        MaybeReport(items_done_ == total_items_);
        return CheckCancel();
    }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return CheckCancel(); }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT hr, IShellItem*) override {
        NoteItemResult(hr);
        return CheckCancel();
    }

    IFACEMETHODIMP UpdateProgress(UINT work_total, UINT work_done) override {
        if (work_total > 0) {
            byte_percent_ = (float)((double)work_done * 100.0 / (double)work_total);
            has_byte_progress_ = true;
        }
        MaybeReport();
        return CheckCancel();
    }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }

    const std::wstring& last_failed_item() const { return last_failed_item_; }
    HRESULT item_failure() const { return item_failure_; }
    void NoteSetupFailure(const std::wstring& src) { last_failed_item_ = src; }
    void set_recycle_only(bool value) { recycle_only_ = value; }
    // A transfer reports the whole operation's work (UpdateProgress), which
    // covers the files inside folders too; item counts then grow past the
    // top-level count instead of being capped by it.
    void set_work_progress(bool value) { work_progress_ = value; }

private:
    HRESULT CheckCancel() {
        if (g.cancel_id.load() == req_id_)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return S_OK;
    }
    void RememberItem(IShellItem* psi) {
        if (!psi) return;
        PWSTR name = nullptr;
        if (SUCCEEDED(psi->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)) && name) {
            current_item_ = name;
            CoTaskMemFree(name);
        }
    }
    void RememberName(LPCWSTR name) { current_item_ = name ? name : L""; }
    bool recycle_only_ = false;
    bool work_progress_ = false;
    void NoteItemResult(HRESULT hr) {
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            if (SUCCEEDED(item_failure_)) item_failure_ = hr;
            last_failed_item_ = current_item_;
        }
    }
    void MaybeReport(bool force = false) {
        // Throttle to ~20 updates/sec to keep the pipe quiet.
        auto now = GetTickCount64();
        if (!force && now - last_send_ < 50) return;
        last_send_ = now;
        float pct;
        if (work_progress_) {
            pct = has_byte_progress_ ? byte_percent_ : 0.0f;
            SendProgress(req_id_, pct, current_item_, items_done_, (std::max)(items_done_, total_items_));
            return;
        }
        if (has_byte_progress_ && total_items_ == 1) {
            pct = byte_percent_;
        } else {
            pct = (float)((double)items_done_ * 100.0 / (double)total_items_);
        }
        SendProgress(req_id_, pct, current_item_, items_done_, total_items_);
    }

    uint32_t req_id_ = 0;
    size_t total_items_ = 1;
    size_t items_done_ = 0;
    float byte_percent_ = 0.0f;
    bool has_byte_progress_ = false;
    std::wstring current_item_;
    std::wstring last_failed_item_;
    HRESULT item_failure_ = S_OK;
    ULONGLONG last_send_ = 0;
};

// ---------------------------------------------------------------------------
// IFileOperation execution (STA main thread only).
// ---------------------------------------------------------------------------
std::wstring ToParsingPath(std::wstring path) {
    // IFileOperation / SHCreateItemFromParsingName reject \\?\ prefixes
    // (ERROR_INVALID_PARAMETER) even for short paths.
    return pulse::path::StripExtendedPathPrefix(path);
}

// "::{CLSID}\..." parsing names: shell items with no filesystem path (a
// phone's storage and files). They have no file type for the handler
// enumeration and no directory to run in.
bool IsShellNamespacePath(const std::wstring& path) {
    return path.size() > 2 && path[0] == L':' && path[1] == L':';
}

HRESULT MakeItem(const std::wstring& path, IShellItem** out) {
    if (pulse::shell_id::IsPidlId(path)) return pulse::shell_id::CreateItem(path, IID_PPV_ARGS(out));
    const std::wstring parsed = ToParsingPath(path);
    HRESULT hr = SHCreateItemFromParsingName(parsed.c_str(), nullptr, IID_PPV_ARGS(out));
    if (FAILED(hr) && parsed != path)
        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(out));
    return hr;
}

bool PathExists(const std::wstring& path) {
    const std::wstring parsed = ToParsingPath(path);
    if (GetFileAttributesW(parsed.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return parsed != path && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring RenameDestination(const std::wstring& source, const std::wstring& new_name) {
    std::wstring path = ToParsingPath(source);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return new_name;
    path.resize(slash + 1);
    path += new_name;
    return path;
}

bool OperationPostconditionSatisfied(const Request& req) {
    if ((req.type == REQ_DELETE_RECYCLE || req.type == REQ_REALDELETE) &&
        !req.sources.empty()) {
        return std::all_of(req.sources.begin(), req.sources.end(), [](const auto& source) {
            return !PathExists(source);
        });
    }
    if (req.type == REQ_RENAME && req.sources.size() == 1 && !req.new_name.empty()) {
        return !PathExists(req.sources.front()) &&
               PathExists(RenameDestination(req.sources.front(), req.new_name));
    }
    return false;
}

std::wstring DescribeCreateError(HRESULT hr) {
    const DWORD code = HRESULT_FACILITY(hr) == FACILITY_WIN32
        ? HRESULT_CODE(hr) : static_cast<DWORD>(hr);
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
    case ERROR_PRIVILEGE_NOT_HELD:
        return L"没有权限在此位置新建";
    case ERROR_PATH_NOT_FOUND:
    case ERROR_FILE_NOT_FOUND:
        return L"目标文件夹不存在";
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return L"已存在同名项目";
    case ERROR_INVALID_NAME:
        return L"名称无效";
    default:
        break;
    }
    LPWSTR msg = nullptr;
    std::wstring text;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr) && msg) {
        text = msg;
        LocalFree(msg);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
            text.pop_back();
    }
    return text.empty() ? L"新建失败" : text;
}

// REQ_NEW_FOLDER / REQ_NEW_FILE: plain Win32 creation (no conflict UI, the UI
// side already picked a unique name). Runs on the STA thread like other ops.
HRESULT ExecuteCreate(const uint32_t type, const std::wstring& path) {
    if (path.empty()) return E_INVALIDARG;
    const std::wstring parsed = ToParsingPath(path);
    const wchar_t* create_path = parsed.empty() ? path.c_str() : parsed.c_str();
    if (GetFileAttributesW(create_path) != INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    if (type == REQ_NEW_FOLDER) {
        return CreateDirectoryW(create_path, nullptr)
            ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HANDLE f = CreateFileW(create_path, GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(f);
    return S_OK;
}

using pulse::recycle::CanonPath;
using pulse::recycle::ReadRecycleOriginal;
using pulse::recycle::ExtendedPath;
using pulse::recycle::ExtendedRootLength;
using pulse::recycle::EnsureParentFolder;
using pulse::recycle::FreeSiblingName;
using pulse::recycle::RestoreSelectedRecyclePayload;
using pulse::recycle::RestoreOneFromRecycle;

HRESULT ExecuteRestore(const std::vector<std::wstring>& paths, std::wstring& error) {
    if (paths.empty()) return E_INVALIDARG;
    size_t ok = 0;
    pulse::shell::RecycleRestoreIndex lookup;
    for (const auto& src : paths) {
        std::wstring one_error;
        if (RestoreOneFromRecycle(src, one_error, lookup)) {
            ++ok;
        } else if (error.empty()) {
            error = one_error;
        }
    }
    if (ok == paths.size()) return S_OK;
    if (ok == 0) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (error.empty()) error = L"部分项目未能还原";
    return HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY);
}

// ---------------------------------------------------------------------------
// REQ_SHELL_VERB: one of Explorer's own verbs, run the way Explorer runs it.
// Main STA thread: a copy / cut leaves its data object on the clipboard, and
// this thread (message loop, alive for the session) keeps serving it.
// ---------------------------------------------------------------------------
std::wstring DescribeHresult(HRESULT hr) {
    std::wstring text;
    LPWSTR msg = nullptr;
    const DWORD win_error = HRESULT_FACILITY(hr) == FACILITY_WIN32
        ? HRESULT_CODE(hr) : static_cast<DWORD>(hr);
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                       nullptr, win_error, 0, (LPWSTR)&msg, 0, nullptr) && msg) {
        text = msg;
        LocalFree(msg);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
            text.pop_back();
    }
    return text;
}

HRESULT ShellVerbMenu(const std::vector<std::wstring>& paths, bool background, HWND owner,
                      IContextMenu** out) {
    *out = nullptr;
    if (paths.empty()) return E_INVALIDARG;
    HRESULT hr = S_OK;
    if (background) {
        IShellItem* folder = nullptr;
        IShellFolder* sf = nullptr;
        hr = MakeItem(paths.front(), &folder);
        if (SUCCEEDED(hr)) hr = folder->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&sf));
        if (SUCCEEDED(hr))
            hr = sf->CreateViewObject(owner, IID_IContextMenu, reinterpret_cast<void**>(out));
        if (sf) sf->Release();
        if (folder) folder->Release();
        return SUCCEEDED(hr) && !*out ? E_FAIL : hr;
    }
    // Every item or none: a verb never runs on part of the selection.
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = pulse::shell_id::ParseId(ToParsingPath(p), &pidl);
        if (FAILED(hr) || !pidl) {
            if (SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            break;
        }
        pidls.push_back(pidl);
    }
    if (SUCCEEDED(hr)) {
        IShellItemArray* array = nullptr;
        hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
            const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array);
        if (SUCCEEDED(hr)) {
            hr = array->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(out));
            array->Release();
        }
    }
    for (auto pidl : pidls) CoTaskMemFree(pidl);
    return SUCCEEDED(hr) && !*out ? E_FAIL : hr;
}

// Paste = a simulated drop of the clipboard on the folder's drop target, as
// Explorer's paste does (works for a phone folder and a local one alike).
// Drops `data` on the folder `dir` the way Explorer would: the folder's own
// drop target runs the transfer (an MTP phone copies over WPD). `wanted` is
// DROPEFFECT_COPY or DROPEFFECT_MOVE.
HRESULT DropDataInto(IDataObject* data, const std::wstring& dir, DWORD wanted) {
    IShellItem* folder = nullptr;
    IDropTarget* drop = nullptr;
    HRESULT hr = MakeItem(dir, &folder);
    if (SUCCEEDED(hr)) hr = folder->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&drop));
    if (folder) folder->Release();
    if (SUCCEEDED(hr) && drop) {
        POINTL pt{ 0, 0 };
        DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        // A copy is asked for as Ctrl+drag: items dropped on their own
        // folder are then copied as "a - 副本" (Explorer's paste there), not
        // refused as a move onto themselves.
        const DWORD keys = wanted == DROPEFFECT_MOVE ? MK_LBUTTON | MK_SHIFT
                         : wanted == DROPEFFECT_COPY ? MK_LBUTTON | MK_CONTROL : MK_LBUTTON;
        hr = drop->DragEnter(data, keys, pt, &effect);
        // A target that cannot move (some phones) still takes a copy: the
        // source stays, nothing is lost.
        if (SUCCEEDED(hr) && !(effect & wanted)) wanted = effect & DROPEFFECT_COPY;
        if (SUCCEEDED(hr) && wanted) {
            effect = wanted;
            hr = drop->Drop(data, wanted == DROPEFFECT_MOVE ? MK_LBUTTON | MK_SHIFT : MK_LBUTTON | MK_CONTROL,
                            pt, &effect);
        } else {
            drop->DragLeave();
            if (SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
    } else if (SUCCEEDED(hr)) {
        hr = E_NOINTERFACE;
    }
    if (drop) drop->Release();
    return hr;
}

HRESULT PasteClipboardInto(const std::wstring& dir) {
    IDataObject* data = nullptr;
    HRESULT hr = OleGetClipboard(&data);
    if (FAILED(hr) || !data) return FAILED(hr) ? hr : E_FAIL;
    DWORD wanted = DROPEFFECT_COPY;  // a cut moves, anything else copies
    FORMATETC fmt{ static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)),
                   nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (SUCCEEDED(data->GetData(&fmt, &medium))) {
        if (const DWORD* preferred = static_cast<const DWORD*>(GlobalLock(medium.hGlobal))) {
            if (*preferred & DROPEFFECT_MOVE) wanted = DROPEFFECT_MOVE;
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    hr = DropDataInto(data, dir, wanted);
    data->Release();
    return hr;
}

// A drag released in Pulse that the shell must carry out: files dropped on a
// phone folder, or a phone's items dropped on a PC folder. `items` are paths
// or "::{CLSID}\..." parsing names.
HRESULT DropItemsInto(const std::wstring& dir, const std::vector<std::wstring>& items, bool move) {
    if (items.empty()) return E_INVALIDARG;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    HRESULT hr = S_OK;
    for (const auto& item : items) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = pulse::shell_id::ParseId(ToParsingPath(item), &pidl);
        if (FAILED(hr) || !pidl) {
            if (SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            break;
        }
        pidls.push_back(pidl);
    }
    IShellItemArray* array = nullptr;
    IDataObject* data = nullptr;
    if (SUCCEEDED(hr))
        hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                               const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array);
    for (auto* pidl : pidls) CoTaskMemFree(pidl);
    if (SUCCEEDED(hr) && array) hr = array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    if (array) array->Release();
    if (SUCCEEDED(hr) && data) hr = DropDataInto(data, dir, move ? DROPEFFECT_MOVE : DROPEFFECT_COPY);
    else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
    if (data) data->Release();
    return hr;
}

void ExecuteShellVerb(Request* req) {
    // Copy / cut / paste go through the OLE clipboard, which needs OLE (not
    // just COM) on this thread. Kept for the process: the clipboard data a
    // copy leaves must outlive the request.
    static const HRESULT ole = OleInitialize(nullptr);
    const bool background = (req->flags & SVF_BACKGROUND) != 0;
    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(req->owner));
    const std::wstring& verb = req->new_name;
    HRESULT hr = FAILED(ole) ? ole : S_OK;
    if (SUCCEEDED(hr) && req->sources.empty()) hr = E_INVALIDARG;
    const bool drop_copy = _wcsicmp(verb.c_str(), L"drop:copy") == 0;
    const bool drop_move = _wcsicmp(verb.c_str(), L"drop:move") == 0;
    if (SUCCEEDED(hr) && background && _wcsicmp(verb.c_str(), L"paste") == 0) {
        hr = PasteClipboardInto(req->sources.front());
    } else if (SUCCEEDED(hr) && background && (drop_copy || drop_move)) {
        // sources: the target folder, then the dropped items.
        hr = DropItemsInto(req->sources.front(),
                           std::vector<std::wstring>(req->sources.begin() + 1, req->sources.end()),
                           drop_move);
    } else if (SUCCEEDED(hr)) {
        IContextMenu* menu = nullptr;
        hr = ShellVerbMenu(req->sources, background, owner, &menu);
        HMENU hmenu = SUCCEEDED(hr) ? CreatePopupMenu() : nullptr;
        if (SUCCEEDED(hr) && !hmenu) hr = HRESULT_FROM_WIN32(GetLastError());
        constexpr UINT kFirst = 1;
        constexpr UINT kLast = 0x7FFF;
        if (SUCCEEDED(hr)) {
            UINT flags = verb.empty() ? CMF_DEFAULTONLY : CMF_NORMAL;
            if (!background) flags |= CMF_ITEMMENU;
            hr = menu->QueryContextMenu(hmenu, 0, kFirst, kLast, flags);
        }
        std::string verb_a;
        UINT default_id = 0;
        if (SUCCEEDED(hr) && verb.empty()) {
            const UINT id = GetMenuDefaultItem(hmenu, FALSE, 0);
            if (id == static_cast<UINT>(-1) || id < kFirst || id > kLast)
                hr = HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION);
            else default_id = id - kFirst;
        } else if (SUCCEEDED(hr)) {
            for (wchar_t c : verb) verb_a.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        if (SUCCEEDED(hr)) {
            const auto dir = pulse::shell::ContextWorkingDirectory(ToParsingPath(req->sources.front()), background);
            CMINVOKECOMMANDINFOEX info{};
            info.cbSize = sizeof(info);
            info.fMask = CMIC_MASK_UNICODE;
            info.hwnd = owner;
            info.lpDirectoryW = dir.empty() ? nullptr : dir.c_str();
            if (verb.empty()) {
                info.lpVerb = MAKEINTRESOURCEA(default_id);
                info.lpVerbW = MAKEINTRESOURCEW(default_id);
            } else {
                info.lpVerb = verb_a.c_str();
                info.lpVerbW = verb.c_str();
            }
            info.nShow = SW_SHOWNORMAL;
            hr = menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
        }
        if (hmenu) DestroyMenu(hmenu);
        if (menu) menu->Release();
    }
    const bool cancelled = hr == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (FAILED(hr) && !cancelled) {
        wchar_t line[160];
        _snwprintf_s(line, _TRUNCATE, L"shell verb '%.64ls' failed hr=0x%08lX",
                     verb.empty() ? L"(default)" : verb.c_str(), static_cast<unsigned long>(hr));
        HostLog(line);
    }
    SendDone(req->id, cancelled ? S_OK : hr, cancelled, FAILED(hr) && !cancelled ? DescribeHresult(hr) : L"");
    delete req;
}

void ExecuteRequest(Request* req) {
    if (req->type == REQ_SHELL_VERB) {
        ExecuteShellVerb(req);
        return;
    }
    if (req->type == REQ_NEW_FOLDER || req->type == REQ_NEW_FILE) {
        HRESULT hr = req->sources.empty() ? E_INVALIDARG
                                          : ExecuteCreate(req->type, req->sources.front());
        SendDone(req->id, hr, false, FAILED(hr) ? DescribeCreateError(hr) : L"");
        delete req;
        return;
    }
    if (req->type == REQ_RESTORE_RECYCLE) {
        std::wstring error;
        HRESULT hr = ExecuteRestore(req->sources, error);
        SendDone(req->id, hr, false, FAILED(hr) ? error : L"");
        delete req;
        return;
    }

    HRESULT hr = S_OK;
    IFileOperation* op = nullptr;
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&op));
    }

    ProgressSink sink(req->id, req->sources.size());
    sink.set_recycle_only(req->type == REQ_DELETE_RECYCLE);
    DWORD sink_cookie = 0;
    if (SUCCEEDED(hr)) {
#ifndef FOFX_DONTDISPLAYUI
        constexpr DWORD kDontDisplayUi = 0x00004000;
#else
        constexpr DWORD kDontDisplayUi = FOFX_DONTDISPLAYUI;
#endif
        DWORD flags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | kDontDisplayUi |
            FOFX_SHOWELEVATIONPROMPT;
        if (req->type != REQ_REALDELETE) flags |= FOF_ALLOWUNDO;
        if (req->type == REQ_DELETE_RECYCLE) flags |= FOFX_RECYCLEONDELETE;
        op->SetOperationFlags(flags);
        op->Advise(&sink, &sink_cookie);
    }

    bool setup_failed = false;
    if (SUCCEEDED(hr)) {
        for (const auto& src : req->sources) {
            IShellItem* item = nullptr;
            HRESULT ihr = MakeItem(src, &item);
            if (FAILED(ihr)) {
                hr = ihr;
                setup_failed = true;
                sink.NoteSetupFailure(src);
                break;
            }
            switch (req->type) {
            case REQ_DELETE_RECYCLE:
            case REQ_REALDELETE: ihr = op->DeleteItem(item, nullptr); break;
            case REQ_RENAME: ihr = op->RenameItem(item, req->new_name.c_str(), nullptr); break;
            default: ihr = E_INVALIDARG; break;
            }
            item->Release();
            if (FAILED(ihr)) { hr = ihr; setup_failed = true; break; }
        }
    }

    bool cancelled = false;
    if (SUCCEEDED(hr)) {
        hr = op->PerformOperations();
        BOOL aborted = FALSE;
        op->GetAnyOperationsAborted(&aborted);
        cancelled = (g.cancel_id.load() == req->id) || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (SUCCEEDED(hr) && FAILED(sink.item_failure())) hr = sink.item_failure();
        if (aborted && !cancelled && SUCCEEDED(hr)) {
            // User declined a system conflict/confirm dialog.
            cancelled = true;
        }
    }
    if (op) {
        if (sink_cookie) op->Unadvise(sink_cookie);
        op->Release();
    }
    // Some shell providers finish the filesystem mutation and then return a
    // failure from final bookkeeping. The filesystem state is authoritative;
    // never tell the UI that a completed delete/rename failed. Setup failures
    // are excluded so a source that was already missing cannot become success.
    if (FAILED(hr) && !cancelled && !setup_failed && OperationPostconditionSatisfied(*req))
        hr = S_OK;

    std::wstring error;
    if (FAILED(hr) && !cancelled) {
        LPWSTR msg = nullptr;
        const DWORD win_error = HRESULT_FACILITY(hr) == FACILITY_WIN32
            ? HRESULT_CODE(hr) : static_cast<DWORD>(hr);
        if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                           nullptr, win_error, 0, (LPWSTR)&msg, 0, nullptr) && msg) {
            error = msg;
            LocalFree(msg);
            while (!error.empty() && (error.back() == L'\r' || error.back() == L'\n'))
                error.pop_back();
        }
        if (!sink.last_failed_item().empty()) {
            if (!error.empty()) error += L" | ";
            error += sink.last_failed_item();
        }
    }

    if (g.cancel_id.load() == req->id) g.cancel_id.store(0);
    SendDone(req->id, hr, cancelled, error);
    delete req;
}

// ---------------------------------------------------------------------------
// Pipe reader thread: parses frames, posts requests to the STA thread.
// ---------------------------------------------------------------------------
static DWORD WINAPI ReaderThreadImpl();

bool ReadRaw(void* out, DWORD size) {
    return PipeRead(g.pipe, static_cast<uint8_t*>(out), size);
}

// Expected pipe peer, captured before the pipe is created. ui_pid is the pid
// the UI passed as argv[1] and the one embedded in the pipe name, so a peer
// reporting a different pid is not the process we were started by.
struct ExpectedPeer {
    DWORD pid = 0;
    std::wstring executable;   // lower-case, empty when it could not be read
};
ExpectedPeer g_expected_peer;

std::wstring LowerPath(std::wstring path) {
    for (auto& c : path) c = static_cast<wchar_t>(std::towlower(c));
    return path;
}

// Full path of a process image, or empty when it cannot be queried (a protected
// or already-exiting process is not necessarily hostile, so callers must treat
// "unknown" separately from "different").
std::wstring ProcessImagePath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t buffer[MAX_PATH * 2]{};
    DWORD size = ARRAYSIZE(buffer);
    const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer, &size);
    CloseHandle(process);
    return ok && size ? std::wstring(buffer, size) : std::wstring();
}

// True when the connected pipe peer is the UI process that spawned this host.
bool ClientIsExpected() {
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(g.pipe, &pid)) {
        HostLog(L"Rejected pipe peer: cannot read client pid");
        return false;
    }
    if (pid != g_expected_peer.pid) {
        wchar_t buf[160];
        swprintf_s(buf, L"Rejected pipe peer: pid %lu, expected %lu", pid, g_expected_peer.pid);
        HostLog(buf);
        return false;
    }
    if (g_expected_peer.executable.empty()) return true;   // could not read our own path
    const std::wstring peer = LowerPath(ProcessImagePath(pid));
    if (peer.empty()) return true;   // peer already exiting; pid already matched
    if (peer != g_expected_peer.executable) {
        wchar_t buf[160];
        swprintf_s(buf, L"Rejected pipe peer: image '%ls' is not the UI", peer.c_str());
        HostLog(buf);
        return false;
    }
    return true;
}

// REQ_SHELL_TRANSFER: one IFileOperation on its own STA thread, so a long
// copy off (or onto) a phone never holds the main thread's requests (open,
// properties, rename). Explorer's own engine: the same WPD path, the same
// speed. Conflicts were settled by Pulse before the request (its dialog);
// what still collides is overwritten, or renamed with STF_RENAME_ON_COLLISION.
void ExecuteShellTransfer(const Request& req) {
    const bool create = req.op == STO_NEW_FOLDER || req.op == STO_NEW_FILE;
    const bool remove = req.op == STO_DELETE;
    HRESULT hr = S_OK;
    IShellItem* dest = nullptr;
    if (!remove) hr = req.dest.empty() ? E_INVALIDARG : MakeItem(req.dest, &dest);
    if (SUCCEEDED(hr) && !create && req.sources.empty()) hr = E_INVALIDARG;
    if (SUCCEEDED(hr) && create && (req.names.empty() || req.names.front().empty())) hr = E_INVALIDARG;
    IFileOperation* op = nullptr;
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    ProgressSink sink(req.id, create ? 1 : req.sources.size());
    sink.set_work_progress(true);
    DWORD sink_cookie = 0;
    if (SUCCEEDED(hr)) {
        // No UI of the shell's: Pulse asked about conflicts and deletes, and
        // shows the progress and the errors itself.
        DWORD flags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
        if (req.flags & STF_RENAME_ON_COLLISION) flags |= FOF_RENAMEONCOLLISION;
        hr = op->SetOperationFlags(flags);
        if (SUCCEEDED(hr)) op->Advise(&sink, &sink_cookie);
    }
    bool setup_failed = FAILED(hr);
    if (SUCCEEDED(hr) && create) {
        hr = op->NewItem(dest, req.op == STO_NEW_FOLDER ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                         req.names.front().c_str(), nullptr, nullptr);
        setup_failed = FAILED(hr);
    } else if (SUCCEEDED(hr)) {
        for (size_t i = 0; i < req.sources.size(); ++i) {
            IShellItem* item = nullptr;
            hr = MakeItem(req.sources[i], &item);
            if (FAILED(hr)) {
                sink.NoteSetupFailure(req.sources[i]);
                break;
            }
            const wchar_t* name = i < req.names.size() && !req.names[i].empty() ? req.names[i].c_str() : nullptr;
            switch (req.op) {
            case STO_COPY: hr = op->CopyItem(item, dest, name, nullptr); break;
            case STO_MOVE: hr = op->MoveItem(item, dest, name, nullptr); break;
            case STO_DELETE: hr = op->DeleteItem(item, nullptr); break;
            default: hr = E_INVALIDARG; break;
            }
            item->Release();
            if (FAILED(hr)) break;
        }
        setup_failed = FAILED(hr);
    }
    bool cancelled = false;
    if (SUCCEEDED(hr)) {
        hr = op->PerformOperations();
        BOOL aborted = FALSE;
        op->GetAnyOperationsAborted(&aborted);
        cancelled = g.cancel_id.load() == req.id || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
                    (aborted && SUCCEEDED(hr) && SUCCEEDED(sink.item_failure()));
        if (SUCCEEDED(hr) && FAILED(sink.item_failure())) hr = sink.item_failure();
    }
    if (op) {
        if (sink_cookie) op->Unadvise(sink_cookie);
        op->Release();
    }
    if (dest) dest->Release();
    std::wstring error;
    if (FAILED(hr) && !cancelled) {
        error = DescribeHresult(hr);
        if (!sink.last_failed_item().empty()) {
            if (!error.empty()) error += L" | ";
            error += sink.last_failed_item();
        }
        wchar_t line[160];
        swprintf_s(line, L"shell transfer op=%u items=%zu failed hr=0x%08lX setup=%d", req.op,
                   req.sources.size(), static_cast<unsigned long>(hr), setup_failed ? 1 : 0);
        HostLog(line);
    }
    SendDone(req.id, cancelled ? S_OK : hr, cancelled, error);
}

DWORD WINAPI ShellTransferThread(LPVOID param) {
    std::unique_ptr<Request> req(static_cast<Request*>(param));
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ExecuteShellTransfer(*req);
    if (SUCCEEDED(co)) CoUninitialize();
    g.requests_in_flight.fetch_sub(1);
    PostMessageW(g.hwnd_msg, WM_RECYCLE_CHECK, 0, 0);
    return 0;
}

void StartShellTransfer(uint32_t id, const uint8_t* payload, size_t size) {
    auto req = std::make_unique<Request>();
    req->type = REQ_SHELL_TRANSFER;
    req->id = id;
    PayloadReader r(payload, size);
    if (!r.GetU32(req->op) || !r.GetU32(req->flags) || !r.GetString(req->dest) ||
        !r.GetStringArray(req->sources) || !r.GetStringArray(req->names) || req->op > STO_DELETE) {
        SendDone(id, E_INVALIDARG, false, L"malformed transfer request");
        return;
    }
    g.requests_in_flight.fetch_add(1);
    HANDLE thread = CreateThread(nullptr, 0, ShellTransferThread, req.get(), 0, nullptr);
    if (!thread) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        g.requests_in_flight.fetch_sub(1);
        SendDone(id, hr, false, DescribeHresult(hr));
        return;
    }
    req.release();  // the thread owns it now
    CloseHandle(thread);
}

DWORD WINAPI ReaderThread(LPVOID) {
    __try {
        return ReaderThreadImpl();
    } __except (pulse::crash::ReportFatal(GetExceptionInformation(), "shell-reader")) {
        TerminateProcess(GetCurrentProcess(), GetExceptionCode());
        return 0;
    }
}

DWORD WINAPI ReaderThreadImpl() {
    while (g.running.load()) {
        // (Re)connect — overlapped handle requires an explicit OVERLAPPED here.
        OVERLAPPED ol{};
        ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ol.hEvent) break;
        BOOL connected = ConnectNamedPipe(g.pipe, &ol);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD ignored = 0;
                connected = GetOverlappedResult(g.pipe, &ol, &ignored, TRUE);
            } else if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
        }
        CloseHandle(ol.hEvent);
        if (!connected) {
            if (!g.running.load()) break;
            Sleep(200);
            continue;
        }
        // The pipe DACL only proves "same user", which on a shared machine is
        // every process that user runs. The protocol can delete arbitrary paths
        // (REQ_REALDELETE), so confirm the peer is the UI process that spawned
        // us: the pid passed on the command line, running the same executable.
        if (!ClientIsExpected()) {
            HostLog(L"Rejected pipe peer: not the spawning UI process");
            DisconnectNamedPipe(g.pipe);
            continue;
        }
        // Frame loop.
        for (;;) {
            MsgHeader h{};
            if (!ReadRaw(&h, sizeof(h))) break;
            if (h.magic != kMagic || h.payload_size > kMaxPayload) break;
            std::vector<uint8_t> payload(h.payload_size);
            if (h.payload_size && !ReadRaw(payload.data(), h.payload_size)) break;

            switch (h.type) {
            case REQ_PING: {
                SendMsg(RSP_PONG, h.request_id, {});
                break;
            }
            case REQ_CANCEL: {
                g.cancel_id.store(h.request_id);
                break;
            }
            case REQ_SHUTDOWN: {
                PostMessageW(g.hwnd_msg, WM_QUIT_HOST, 0, 0);
                break;
            }
            case REQ_CTX_QUERY: {
                StartCtxSession(h.request_id, payload.data(), payload.size());
                break;
            }
            case REQ_SHELL_TRANSFER: {
                StartShellTransfer(h.request_id, payload.data(), payload.size());
                break;
            }
            case REQ_CTX_INVOKE: {
                PayloadReader r(payload.data(), payload.size());
                uint32_t session = 0, item = 0;
                std::wstring verb, text;
                if (r.GetU32(session) && r.GetU32(item)) {
                    r.GetString(verb);
                    r.GetString(text);
                    auto* inv = new CtxInvokeMsg{ h.request_id, item, std::move(verb),
                                                  std::move(text) };
                    PostCtxMessage(session, WM_CTX_INVOKE, 0,
                                   reinterpret_cast<LPARAM>(inv));
                } else {
                    SendDone(h.request_id, E_INVALIDARG, false, L"malformed ctx invoke");
                }
                break;
            }
            case REQ_CTX_CLOSE: {
                PayloadReader r(payload.data(), payload.size());
                uint32_t session = 0;
                if (r.GetU32(session))
                    PostCtxMessage(session, WM_CTX_CLOSE, 0, 0);
                break;
            }
            case REQ_DELETE_RECYCLE:
            case REQ_REALDELETE:
            case REQ_RENAME:
            case REQ_NEW_FOLDER:
            case REQ_NEW_FILE:
            case REQ_RESTORE_RECYCLE:
            case REQ_SHELL_VERB: {
                auto* req = new Request();
                req->type = h.type;
                req->id = h.request_id;
                PayloadReader r(payload.data(), payload.size());
                bool ok = true;
                if (req->type == REQ_SHELL_VERB) {
                    ok = r.GetU32(req->owner) && r.GetU32(req->flags) &&
                         r.GetString(req->new_name) && r.GetStringArray(req->sources) &&
                         !req->sources.empty();
                } else if (req->type == REQ_RENAME) {
                    std::wstring path;
                    ok = r.GetString(path) && r.GetString(req->new_name);
                    req->sources.push_back(std::move(path));
                } else if (req->type == REQ_NEW_FOLDER || req->type == REQ_NEW_FILE) {
                    std::wstring path;
                    ok = r.GetString(path);
                    req->sources.push_back(std::move(path));
                } else {
                    ok = r.GetStringArray(req->sources);
                }
                if (!ok) {
                    SendDone(req->id, E_INVALIDARG, false, L"malformed request payload");
                    delete req;
                    break;
                }
                g.requests_in_flight.fetch_add(1);
                if (!PostMessageW(g.hwnd_msg, WM_EXEC_REQUEST, 0,
                                  reinterpret_cast<LPARAM>(req))) {
                    g.requests_in_flight.fetch_sub(1);
                    SendDone(req->id, HRESULT_FROM_WIN32(GetLastError()), false,
                             L"host message queue unavailable");
                    delete req;
                }
                break;
            }
            default:
                break;
            }
        }
        // Client disconnected: clean up and listen again.
        DisconnectNamedPipe(g.pipe);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Context-menu sessions (Explorer verbs). One STA thread per session so a hung
// third-party extension only wedges its own thread: file operations on the
// main STA and the next right-click are unaffected (优化.md §7.2).
// ---------------------------------------------------------------------------
constexpr UINT kCtxIdFirst = 1;
constexpr UINT kCtxIdLast = 0x7FFF;
constexpr DWORD kCtxSessionExpireMs = 120 * 1000;

struct CtxSessionData {
    uint32_t session_id = 0;   // == REQ_CTX_QUERY request id
    HWND owner = nullptr;
    bool extended = false;
    bool background = false;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> disabled_clsids;
};

struct CtxSlot {
    DWORD thread_id = 0;       // 0 until the session thread has a message queue
    bool close_requested = false;
};

std::mutex g_ctx_mutex;
std::map<uint32_t, CtxSlot> g_ctx_sessions;
// Guarded by g_ctx_mutex (#65): session threads still running (a session
// leaves g_ctx_sessions before it joins its handler threads), and handler
// threads given up on because QueryContextMenu never returned.
int g_ctx_threads = 0;
std::vector<HANDLE> g_abandoned_threads;

// Test hook (#65): PULSE_SHELL_TEST_STUCK_HANDLER=<file name> adds, to menus
// for that file only, a handler whose QueryContextMenu never returns, like a
// third-party extension that hangs.
constexpr wchar_t kTestStuckHandler[] = L"{50554C53-4500-4D41-4E47-000000000065}";

bool TestStuckHandlerFor(const std::wstring& path) {
    wchar_t name[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"PULSE_SHELL_TEST_STUCK_HANDLER", name,
                                                 ARRAYSIZE(name));
    if (length == 0 || length >= ARRAYSIZE(name) || path.size() <= length) return false;
    return path[path.size() - length - 1] == L'\\' &&
           _wcsicmp(path.c_str() + path.size() - length, name) == 0;
}

using pulse::shell::CtxItemOut;

void SendCtxItems(uint32_t session_id, const std::vector<CtxItemOut>& items,
                  uint32_t flags = 0,
                  const std::vector<std::wstring>& slow_clsids = {},
                  const std::vector<pulse::ipc::CtxHandlerStatus>& handlers = {}) {
    PayloadWriter w;
    w.PutU32(session_id);
    w.PutU32(flags);
    w.PutU32((uint32_t)items.size());
    for (const auto& it : items) {
        w.PutU32(it.id);
        uint32_t item_flags = 0;
        if (it.enabled) item_flags |= CTX_ITEM_ENABLED;
        if (it.separator_after) item_flags |= CTX_ITEM_SEPARATOR_AFTER;
        if (it.has_children) item_flags |= CTX_ITEM_HAS_CHILDREN;
        if (it.child) item_flags |= CTX_ITEM_CHILD;
        item_flags |= PackCtxItemDepth(it.depth);
        item_flags |= pulse::ipc::PackCtxItemMnemonic(it.mnemonic);
        w.PutU32(item_flags);
        w.PutString(it.verb);
        w.PutString(it.text);
        w.PutString(it.clsid);
        w.PutString(it.handler);
    }
    w.PutStringArray(slow_clsids);
    pulse::ipc::PutCtxHandlerStatuses(w, handlers);
    pulse::ipc::PutCtxItemIcons(w, items);
    SendMsg(RSP_CTX_ITEMS, session_id, w.data());
}

UINT QueryContextMenuFlags(const CtxSessionData& d) {
    UINT flags = CMF_NORMAL;
    if (d.extended) flags |= CMF_EXTENDEDVERBS;
    if (!d.background) {
        // An item menu, as Explorer's view asks for it. Some extensions only
        // add their rows then: 播放到设备 (PlayTo) returns nothing for
        // CMF_NORMAL alone, though Explorer shows it on every image.
        flags |= CMF_ITEMMENU;
        bool all_dirs = !d.paths.empty();
        for (const auto& p : d.paths) {
            const std::wstring path = ToParsingPath(p);
            const DWORD attr = GetFileAttributesW(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                all_dirs = false;
                break;
            }
        }
        if (all_dirs) flags |= CMF_EXPLORE;
    }
    return flags;
}

// Walks the populated HMENU via the same nested-flyout rules as per-handler
// collection so fallback and worker paths cannot drift.
void CollectCtxItems(IContextMenu* menu, IContextMenu2* menu2, HMENU hmenu,
                     bool background, std::vector<CtxItemOut>& out,
                     bool shell_namespace = false) {
    pulse::shell::CtxHandlerSlot slot;
    slot.shell_namespace = shell_namespace;
    slot.menu = menu;
    slot.menu2 = menu2;
    slot.hmenu = hmenu;
    slot.id_first = kCtxIdFirst;
    slot.id_last = kCtxIdLast;
    if (menu) menu->QueryInterface(IID_PPV_ARGS(&slot.menu3));
    pulse::shell::CollectHandlerItems(slot, background, out);
    if (slot.menu3) slot.menu3->Release();
    slot.menu = nullptr;
    slot.menu2 = nullptr;
    slot.menu3 = nullptr;
    slot.hmenu = nullptr;
}

// A phone item's menu, as the shell gave it (before Pulse drops the rows it
// draws itself): one host log line per row, so a user's log tells whether a
// row Explorer shows never came or was lost on the way to the app.
void LogShellMenuRows(IContextMenu* menu, HMENU hmenu) {
    pulse::shell::HostLogBatch log(HostLogPath(), nullptr, 64, kHostLogMaxBytes, &HostLogMutex());
    const int count = GetMenuItemCount(hmenu);
    wchar_t head[64];
    swprintf_s(head, L"phone ctx: %d native rows", count);
    HostLog(head);
    for (int i = 0; i < count; ++i) {
        wchar_t text[128]{};
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask = MIIM_ID | MIIM_FTYPE | MIIM_SUBMENU | MIIM_STRING | MIIM_STATE;
        mii.dwTypeData = text;
        mii.cch = ARRAYSIZE(text);
        if (!GetMenuItemInfoW(hmenu, static_cast<UINT>(i), TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) {
            log.Add(L"phone ctx:   ----");
            continue;
        }
        wchar_t verb[64]{};
        if (!mii.hSubMenu && mii.wID >= kCtxIdFirst && mii.wID <= kCtxIdLast &&
            FAILED(menu->GetCommandString(mii.wID - kCtxIdFirst, GCS_VERBW, nullptr,
                                          reinterpret_cast<LPSTR>(verb), ARRAYSIZE(verb))))
            verb[0] = L'\0';
        verb[ARRAYSIZE(verb) - 1] = L'\0';
        std::wstring line = L"phone ctx:   [";
        line += verb;
        line += L"] ";
        line += text;
        if (mii.hSubMenu) line += L" >";
        if (mii.fState & MFS_DISABLED) line += L" (disabled)";
        log.Add(line.c_str());
    }
}

HRESULT BuildCtxMenu(const CtxSessionData& d, IContextMenu** out_menu, HMENU* out_hmenu,
                     std::vector<CtxItemOut>& items) {
    *out_menu = nullptr;
    *out_hmenu = nullptr;
    if (d.paths.empty()) return E_INVALIDARG;

    IContextMenu* menu = nullptr;
    HRESULT hr = S_OK;
    if (d.background) {
        IShellItem* folder = nullptr;
        hr = MakeItem(d.paths.front(), &folder);
        IShellFolder* sf = nullptr;
        if (SUCCEEDED(hr))
            hr = folder->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&sf));
        if (SUCCEEDED(hr))
            hr = sf->CreateViewObject(d.owner, IID_IContextMenu,
                                      reinterpret_cast<void**>(&menu));
        if (sf) sf->Release();
        if (folder) folder->Release();
    } else {
        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (const auto& p : d.paths) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(pulse::shell_id::ParseId(ToParsingPath(p), &pidl)))
                pidls.push_back(pidl);
        }
        if (pidls.empty()) return E_INVALIDARG;
        IShellItemArray* array = nullptr;
        hr = SHCreateShellItemArrayFromIDLists((UINT)pidls.size(),
            const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array);
        if (SUCCEEDED(hr)) {
            hr = array->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu));
            array->Release();
        }
        for (auto pidl : pidls) CoTaskMemFree(pidl);
    }
    if (FAILED(hr) || !menu) return FAILED(hr) ? hr : E_FAIL;

    HMENU hmenu = CreatePopupMenu();
    UINT flags = QueryContextMenuFlags(d);
    hr = menu->QueryContextMenu(hmenu, 0, kCtxIdFirst, kCtxIdLast, flags);
    if (FAILED(hr)) {
        DestroyMenu(hmenu);
        menu->Release();
        return hr;
    }
    DiagnosticRawRows(d.session_id, hmenu, L"fallback");
    const bool phone_items = !d.background && IsShellNamespacePath(ToParsingPath(d.paths.front()));
    if (phone_items) LogShellMenuRows(menu, hmenu);
    IContextMenu2* menu2 = nullptr;
    menu->QueryInterface(IID_PPV_ARGS(&menu2));
    CollectCtxItems(menu, menu2, hmenu, d.background, items, phone_items);
    if (phone_items) {
        wchar_t kept[64];
        swprintf_s(kept, L"phone ctx: %zu rows passed to the app", items.size());
        HostLog(kept);
    }
    if (menu2) menu2->Release();
    *out_menu = menu;
    *out_hmenu = hmenu;
    return S_OK;
}

HRESULT InvokeStartPinPaths(const std::vector<std::wstring>& paths, bool pin) {
    if (paths.empty()) return E_INVALIDARG;
    for (const auto& path : paths) {
        const HRESULT hr = pulse::start_pin::Invoke(ToParsingPath(path), pin);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

void CtxInvoke(const CtxSessionData& d, IContextMenu* menu, UINT id_first,
               uint32_t invoke_req_id, uint32_t item_id) {
    if (!menu || item_id < id_first) {
        SendDone(invoke_req_id, E_INVALIDARG, false, L"context menu invoke failed");
        return;
    }
    // 固定到任务栏: CTaskbandPin's own InvokeCommand refuses a non-Explorer
    // caller, so the pin goes to TaskbarPinManager directly. Where that
    // manager is unavailable the verb is invoked normally below.
    wchar_t verb_buf[64]{};
    if (!d.background &&
        SUCCEEDED(menu->GetCommandString(item_id - id_first, GCS_VERBW, nullptr,
                                         reinterpret_cast<LPSTR>(verb_buf),
                                         ARRAYSIZE(verb_buf)))) {
        verb_buf[ARRAYSIZE(verb_buf) - 1] = L'\0';  // some handlers fill the buffer unterminated
        if (!_wcsicmp(verb_buf, L"PinToStartScreen") || !_wcsicmp(verb_buf, L"UnpinFromStartScreen")) {
            const HRESULT hr = InvokeStartPinPaths(d.paths, !_wcsicmp(verb_buf, L"PinToStartScreen"));
            SendDone(invoke_req_id, hr, false, FAILED(hr) ? L"context menu invoke failed" : L"");
            return;
        }
        HRESULT pin_hr = E_FAIL;
        if (_wcsicmp(verb_buf, L"taskbarpin") == 0) {
            if (pulse::shell::InvokeTaskbandPin(d.paths, pin_hr)) {
                if (FAILED(pin_hr)) {
                    wchar_t line[96];
                    swprintf_s(line, L"taskbarpin via TaskbarPinManager failed hr=0x%08lX",
                               static_cast<unsigned long>(pin_hr));
                    HostLog(line);
                }
                SendDone(invoke_req_id, pin_hr, false,
                         FAILED(pin_hr) ? L"context menu invoke failed" : L"");
                return;
            }
            HostLog(L"taskbarpin: TaskbarPinManager unavailable, using InvokeCommand");
        }
    }
    const auto dir = pulse::shell::ContextWorkingDirectory(ToParsingPath(d.paths.front()), d.background);
    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE;
    info.hwnd = d.owner;
    info.lpVerb = MAKEINTRESOURCEA(item_id - id_first);
    info.lpVerbW = MAKEINTRESOURCEW(item_id - id_first);
    info.lpDirectoryW = dir.empty() ? nullptr : dir.c_str();
    info.nShow = SW_SHOWNORMAL;
    const HRESULT hr = menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
    SendDone(invoke_req_id, hr, false,
             FAILED(hr) ? L"context menu invoke failed" : L"");
}

bool SessionCloseRequested(uint32_t sid) {
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    auto it = g_ctx_sessions.find(sid);
    return it == g_ctx_sessions.end() || it->second.close_requested;
}

struct HandlerWorker {
    HandlerWorker() = default;
    HandlerWorker(const HandlerWorker&) = delete;
    HandlerWorker& operator=(const HandlerWorker&) = delete;
    // Owners either join first or deliberately release a stuck worker. Never
    // destroy a worker while its thread can still access its data/events.
    ~HandlerWorker() {
        if (done_event) CloseHandle(done_event);
        if (exit_event) CloseHandle(exit_event);
        if (thread) CloseHandle(thread);
    }
    uint32_t session_id = 0;
    pulse::shell::CtxHandlerDesc desc;
    std::vector<std::wstring> paths;
    bool background = false;
    HWND owner = nullptr;
    UINT id_first = 0;
    UINT id_last = 0;
    UINT qcm_flags = 0;
    pulse::shell::CtxHandlerSlot slot;
    std::unique_ptr<pulse::shell::IsolatedStartPinSession> isolated_start_pin;
    std::vector<CtxItemOut> items;
    std::vector<pulse::shell::SendToTarget> send_to;   // native 发送到 only
    HANDLE done_event = nullptr;
    HANDLE exit_event = nullptr;
    HANDLE thread = nullptr;
    DWORD thread_id = 0;
    uint32_t elapsed_ms = 0;
    // 0 = binding / loading the extension, 1 = inside the extension's own menu
    // code (QueryContextMenu / flyout init). Only a worker stuck in its own code
    // can be blamed while another thread may hold the loader lock.
    std::atomic<int> phase{0};
};

// A native 发送到 row (C5): child k of the flyout is send_to[k].
void InvokeNativeSendTo(const HandlerWorker& w, uint32_t invoke_req_id, uint32_t item_id) {
    HRESULT hr = E_INVALIDARG;
    if (item_id > w.id_first && item_id - w.id_first - 1 < w.send_to.size()) {
        pulse::shell::CtxBind bind;
        hr = pulse::shell::BindCtxSelection(w.paths, false, bind) && bind.data
            ? pulse::shell::DropOnSendToTarget(w.send_to[item_id - w.id_first - 1].pidl, bind.data)
            : E_FAIL;
    }
    SendDone(invoke_req_id, hr, false, FAILED(hr) ? L"send to failed" : L"");
}

DWORD HandlerWorkerThreadImpl(LPVOID param) {
    auto* w = static_cast<HandlerWorker*>(param);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        if (w->done_event) SetEvent(w->done_event);
        return 0;
    }
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    w->thread_id = GetCurrentThreadId();

    const ULONGLONG t0 = GetTickCount64();
    if (w->desc.clsid_text == kTestStuckHandler) Sleep(INFINITE);
    pulse::shell::CtxBind bind;
    if (IsNativeSendToClsid(w->desc.clsid_text)) {
        w->phase = 1;
        w->send_to = pulse::shell::EnumerateSendToTargets();
        wchar_t mnemonic = 0;
        const std::wstring title = pulse::shell::NativeSendToTitle(&mnemonic);
        pulse::shell::NativeSendToItems(w->send_to, title, mnemonic, w->id_first, w->id_last,
                                        w->items);
    } else if (_wcsicmp(w->desc.clsid_text.c_str(),
                       L"{470c0ebd-5d73-4d58-9ced-e91e22e23282}") == 0) {
        // Keep native menu enumeration isolated. Invocation is delegated to
        // the Explorer extension because Windows checks the caller identity.
        w->isolated_start_pin = std::make_unique<pulse::shell::IsolatedStartPinSession>();
        const HRESULT hr = w->isolated_start_pin->Query(w->paths, w->owner,
            w->id_first, w->id_last, w->qcm_flags, w->exit_event, w->items);
        w->phase = 1;
        ContextDiagnostic(w->session_id, L"isolated handler=" + DiagnosticText(w->desc.clsid_text) +
            L" hr=" + std::to_wstring(static_cast<unsigned long>(hr)));
        if (FAILED(hr)) {
            w->items.clear();
            w->isolated_start_pin.reset();
        }
    } else if (pulse::shell::BindCtxSelection(w->paths, w->background, bind)) {
        const HRESULT hr = pulse::shell::QueryOneHandler(
            w->desc, bind, w->owner, w->id_first, w->id_last, w->qcm_flags, w->slot, &w->phase);
        ContextDiagnostic(w->session_id, L"handler=" + DiagnosticText(w->desc.clsid_text) +
            L" hr=" + std::to_wstring(static_cast<unsigned long>(hr)));
        DiagnosticRawRows(w->session_id, w->slot.hmenu, w->desc.clsid_text);
        if (SUCCEEDED(hr) && w->slot.menu)
            pulse::shell::CollectHandlerItems(w->slot, w->background, w->items);
        else
            pulse::shell::ReleaseHandlerSlot(w->slot);
    }
    ContextDiagnostic(w->session_id, L"worker completed handler=" + DiagnosticText(w->desc.clsid_text) +
        L" phase=" + std::to_wstring(w->phase.load()) + L" rows=" + std::to_wstring(w->items.size()));
    w->elapsed_ms = static_cast<uint32_t>(GetTickCount64() - t0);
    if (w->done_event) SetEvent(w->done_event);

    for (;;) {
        HANDLE waits[1] = { w->exit_event };
        const DWORD n = w->exit_event ? 1 : 0;
        MsgWaitForMultipleObjects(n, n ? waits : nullptr, FALSE, INFINITE, QS_ALLINPUT);
        const bool exiting = w->exit_event &&
            WaitForSingleObject(w->exit_event, 0) == WAIT_OBJECT_0;
        bool invoked = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_CTX_INVOKE) {
                std::unique_ptr<CtxInvokeMsg> inv(
                    reinterpret_cast<CtxInvokeMsg*>(msg.lParam));
                CtxSessionData d;
                d.owner = w->owner;
                d.paths = w->paths;
                d.background = w->background;
                if (IsNativeSendToClsid(w->desc.clsid_text))
                    InvokeNativeSendTo(*w, inv->invoke_req_id, inv->item_id);
                else if (w->isolated_start_pin) {
                    HRESULT hr = E_INVALIDARG;
                    for (const auto& item : w->items) {
                        if (item.id != inv->item_id || !item.enabled || item.has_children) continue;
                        if (!_wcsicmp(item.verb.c_str(), L"PinToStartScreen") ||
                            !_wcsicmp(item.verb.c_str(), L"UnpinFromStartScreen"))
                            hr = InvokeStartPinPaths(w->paths, !_wcsicmp(item.verb.c_str(), L"PinToStartScreen"));
                        break;
                    }
                    SendDone(inv->invoke_req_id, hr, false,
                             FAILED(hr) ? L"context menu invoke failed" : L"");
                } else if (w->slot.menu && inv->item_id >= w->id_first)
                    CtxInvoke(d, w->slot.menu, w->id_first, inv->invoke_req_id, inv->item_id);
                else
                    SendDone(inv->invoke_req_id, E_INVALIDARG, false,
                             L"context menu invoke failed");
                invoked = true;
            } else if (msg.message == WM_CTX_CLOSE) {
                // coordinator uses exit_event; ignore stray close
            } else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (invoked || exiting) break;
    }
    pulse::shell::ReleaseHandlerSlot(w->slot);
    w->isolated_start_pin.reset();
    pulse::shell::FreeSendToTargets(w->send_to);
    CoUninitialize();
    return 0;
}

DWORD WINAPI HandlerWorkerThread(LPVOID param) {
    auto* w = static_cast<HandlerWorker*>(param);
    __try {
        return HandlerWorkerThreadImpl(param);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (w && w->done_event) SetEvent(w->done_event);
        return 0;
    }
}

// Kept in one production entry point so acquisition failures can be exercised
// without registering or invoking any installed context-menu handler.
struct HandlerWorkerApi {
    decltype(&CreateEventW) create_event = ::CreateEventW;
    decltype(&CreateThread) create_thread = ::CreateThread;
};

bool StartHandlerWorker(HandlerWorker& worker, const HandlerWorkerApi& api = {}) {
    worker.done_event = api.create_event(nullptr, TRUE, FALSE, nullptr);
    if (!worker.done_event) return false;
    worker.exit_event = api.create_event(nullptr, TRUE, FALSE, nullptr);
    if (!worker.exit_event) return false;
    worker.thread = api.create_thread(nullptr, 0, HandlerWorkerThread, &worker, 0, nullptr);
    return worker.thread != nullptr;
}

// A handler still inside QueryContextMenu gets this long after the session
// ends. A thread cannot be stopped safely, so then it is given up on: its
// worker leaks with it, and the host recycles itself once idle (#65).
constexpr DWORD kHandlerJoinMs = 2000;

void JoinHandlerWorkers(std::vector<std::unique_ptr<HandlerWorker>>& workers) {
    for (auto& w : workers)
        if (w && w->exit_event) SetEvent(w->exit_event);
    // Handlers that never answered first. One that did may be running a
    // command the user picked (a dialog, say): that one is waited for.
    const ULONGLONG deadline = GetTickCount64() + kHandlerJoinMs;
    std::vector<HANDLE> abandoned;
    for (auto& w : workers) {
        if (!w || !w->thread || !w->done_event ||
            WaitForSingleObject(w->done_event, 0) == WAIT_OBJECT_0) continue;
        const ULONGLONG now = GetTickCount64();
        const DWORD wait = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        if (WaitForSingleObject(w->thread, wait) == WAIT_OBJECT_0) continue;
        wchar_t line[256];
        swprintf_s(line, L"context menu handler never returned: %ls",
                   w->desc.clsid_text.c_str());
        HostLog(line);
        abandoned.push_back(w->thread);
        (void)w.release(); // the thread still uses it, events included
    }
    if (!abandoned.empty()) {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_abandoned_threads.insert(g_abandoned_threads.end(), abandoned.begin(), abandoned.end());
    }
    for (auto& w : workers)
        if (w && w->thread) WaitForSingleObject(w->thread, INFINITE);
    for (auto& w : workers) {
        if (!w) continue;
        if (w->done_event) CloseHandle(w->done_event);
        if (w->exit_event) CloseHandle(w->exit_event);
        if (w->thread) CloseHandle(w->thread);
        w->done_event = w->exit_event = w->thread = nullptr;
    }
    workers.clear();
}

// The coordinator can be tested with one controlled worker, without loading
// installed providers or writing the user's host diagnostic log.
struct CtxSessionApi {
    decltype(&pulse::shell::EnumerateCtxHandlers) enumerate = pulse::shell::EnumerateCtxHandlers;
    decltype(&BuildCtxMenu) fallback = BuildCtxMenu;
    decltype(&HostLog) log = HostLog;
    HandlerWorkerApi worker;
};

DWORD CtxSessionThreadImpl(LPVOID param, const CtxSessionApi& api = {}) {
    std::unique_ptr<CtxSessionData> data(static_cast<CtxSessionData*>(param));
    const uint32_t sid = data->session_id;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        SendCtxItems(sid, {});
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_ctx_sessions.erase(sid);
        return 0;
    }
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    bool closed_early = false;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        auto it = g_ctx_sessions.find(sid);
        if (it == g_ctx_sessions.end() || it->second.close_requested) {
            closed_early = true;
            g_ctx_sessions.erase(sid);
        } else {
            it->second.thread_id = GetCurrentThreadId();
        }
    }
    if (closed_early) {
        CoUninitialize();
        return 0;
    }

    constexpr UINT kIdsPerHandler = 256;
    constexpr DWORD kFastBudgetMs = 80;
    // Handlers still busy by then are not responding: the menu settles
    // without them and they are reported as timed out (#65).
    constexpr DWORD kHandlerStuckMs = 5000;
    const ULONGLONG started = GetTickCount64();
    UINT qcm_flags = QueryContextMenuFlags(*data);
    if (ContextDiagnosticsEnabled()) {
        ContextDiagnostic(sid, L"request count=" + std::to_wstring(data->paths.size()) +
            L" background=" + std::to_wstring(data->background) + L" flags=" + std::to_wstring(qcm_flags));
        for (size_t i = 0; i < (std::min)(data->paths.size(), size_t{32}); ++i) {
            const auto& path = data->paths[i];
            const bool shell = IsShellNamespacePath(path);
            const DWORD attrs = shell ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
            const wchar_t* kind = shell ? L"namespace" : attrs == INVALID_FILE_ATTRIBUTES ? L"unknown" :
                (attrs & FILE_ATTRIBUTE_DIRECTORY) ? L"directory" : L"file";
            const auto dot = path.find_last_of(L'.');
            const auto slash = path.find_last_of(L"\\/");
            const std::wstring extension = !shell && dot != std::wstring::npos &&
                (slash == std::wstring::npos || dot > slash) ? path.substr(dot) : std::wstring();
            ContextDiagnostic(sid, L"object=" + std::to_wstring(i) + L" type=" + kind +
                L" extension=" + (shell ? std::wstring() : DiagnosticText(extension)));
        }
        for (const auto& clsid : data->disabled_clsids)
            ContextDiagnostic(sid, L"disabled=" + DiagnosticText(clsid));
    }

    std::vector<std::unique_ptr<HandlerWorker>> workers;
    std::vector<CtxItemOut> items;
    std::vector<pulse::ipc::CtxHandlerStatus> statuses;
    IContextMenu* fallback_menu = nullptr;
    HMENU fallback_hmenu = nullptr;
    std::unique_ptr<CtxInvokeMsg> pending_invoke;

    auto pump_session_messages = [&] {
        MSG m{};
        while (PeekMessageW(&m, nullptr, WM_CTX_CLOSE, WM_CTX_CLOSE, PM_REMOVE)) {
            std::lock_guard<std::mutex> lock(g_ctx_mutex);
            auto it = g_ctx_sessions.find(sid);
            if (it != g_ctx_sessions.end()) it->second.close_requested = true;
        }
        while (PeekMessageW(&m, nullptr, WM_CTX_INVOKE, WM_CTX_INVOKE, PM_REMOVE))
            pending_invoke.reset(reinterpret_cast<CtxInvokeMsg*>(m.lParam));
    };

    // A phone's items ("::{...}" parsing names) have no file type to read
    // handlers for: their menu is the shell's own, the fallback menu below,
    // exactly what Explorer shows for them.
    auto handlers = IsShellNamespacePath(data->paths.front())
        ? decltype(api.enumerate(data->background, data->paths.front(), data->disabled_clsids)){}
        : api.enumerate(data->background, data->paths.front(), data->disabled_clsids);
    // Enumeration touches the target (GetFileAttributes on a sleeping disk or
    // an offline share) and the package catalog; a user log had it take 22 s.
    // Logged on its own so such a stall is not mistaken for the extensions.
    const uint32_t enumerate_ms = static_cast<uint32_t>(GetTickCount64() - started);
    if (enumerate_ms >= 300) {
        wchar_t line[192];
        swprintf_s(line, L"enumerate handlers %ums count=%zu path=%ls", enumerate_ms,
                   handlers.size(), data->paths.front().c_str());
        api.log(line);
    }
    if (TestStuckHandlerFor(data->paths.front()) &&
        std::find(data->disabled_clsids.begin(), data->disabled_clsids.end(),
                  kTestStuckHandler) == data->disabled_clsids.end()) {
        pulse::shell::CtxHandlerDesc stuck;
        stuck.clsid_text = kTestStuckHandler;
        handlers.insert(handlers.begin(), std::move(stuck));
    }
    // Native 发送到 (C5) right before the SendTo slot, in its menu position.
    // The slot still runs for the default menu's own rows and as the check;
    // the user hiding 发送到 (the slot disabled) hides both.
    if (!data->background &&
        !IsDisabledHandler(kNativeSendToClsid, data->disabled_clsids)) {
        const auto slot = std::find_if(handlers.begin(), handlers.end(),
            [](const pulse::shell::CtxHandlerDesc& h) {
                return IsSendToHandlerClsid(h.clsid_text) && !h.default_menu_only;
            });
        if (slot != handlers.end()) {
            pulse::shell::CtxHandlerDesc native;
            native.clsid_text = kNativeSendToClsid;
            handlers.insert(slot, std::move(native));
        }
    }
    if (handlers.size() > MAXIMUM_WAIT_OBJECTS)
        handlers.resize(MAXIMUM_WAIT_OBJECTS);

    // Ownership transfer after starting a thread must not allocate/throw.
    workers.reserve(handlers.size());
    UINT next_id = kCtxIdFirst;
    for (const auto& handler : handlers) {
        if (next_id + kIdsPerHandler > kCtxIdLast) break;
        auto worker = std::make_unique<HandlerWorker>();
        worker->session_id = sid;
        worker->desc = handler;
        worker->paths = data->paths;
        worker->background = data->background;
        worker->owner = data->owner;
        worker->id_first = next_id;
        worker->id_last = next_id + kIdsPerHandler - 1;
        worker->qcm_flags = qcm_flags;
        next_id += kIdsPerHandler;
        if (!StartHandlerWorker(*worker, api.worker)) continue;
        workers.push_back(std::move(worker));
    }
    // The fast budget, the stuck limit and the unfinished workers' elapsed
    // time all count from here. Counting from `started` let a slow enumeration
    // eat the whole 5 s: the loop then broke on its first pass and sent an
    // empty menu with every worker "unfinished" (items=0 handlers=19 after
    // 22063 ms in a user's log), and none of them had been given any time.
    const ULONGLONG handlers_started = GetTickCount64();

    auto worker_done = [](const HandlerWorker& w) {
        return w.done_event && WaitForSingleObject(w.done_event, 0) == WAIT_OBJECT_0;
    };
    auto all_workers_done = [&] {
        if (workers.empty()) return true;
        for (const auto& w : workers)
            if (w && !worker_done(*w)) return false;
        return true;
    };
    auto invokable_texts = [](const std::vector<CtxItemOut>& items, std::vector<std::wstring>& out) {
        for (const auto& item : items)
            if (!item.has_children && !item.text.empty()) out.push_back(item.text);
    };
    // `statuses` gets every worker in menu order: the app keeps the previous
    // rows of the handlers that have not finished (C4b shard merge).
    // Logged once per session: where the native 发送到 and the slot's differ.
    bool send_to_checked = false;
    auto check_send_to = [&](const std::vector<std::wstring>& slot_only,
                             const std::vector<std::wstring>& native_only) {
        if (send_to_checked) return;
        send_to_checked = true;
        if (slot_only.empty() && native_only.empty()) return;
        std::wstring line = L"Native send-to differs; slot only:";
        for (const auto& t : slot_only) line += L" [" + t + L"]";
        line += L" native only:";
        for (const auto& t : native_only) line += L" [" + t + L"]";
        HostLog(line.c_str());
    };
    auto collect_items = [&](std::vector<pulse::ipc::CtxHandlerStatus>& statuses) {
        const HandlerWorker* native_send_to = nullptr;
        for (const auto& w : workers)
            if (w && IsNativeSendToClsid(w->desc.clsid_text) && worker_done(*w) &&
                !w->items.empty())
                native_send_to = w.get();
        std::vector<std::wstring> classic_rows;
        for (const auto& w : workers)
            if (w && worker_done(*w) && !w->desc.explorer_command)
                invokable_texts(w->items, classic_rows);
        const uint32_t so_far = static_cast<uint32_t>(GetTickCount64() - handlers_started);
        statuses.clear();
        std::vector<CtxItemOut> out;
        for (const auto& w : workers) {
            if (!w) continue;
            pulse::ipc::CtxHandlerStatus status;
            status.clsid = w->desc.clsid_text;
            const bool done = worker_done(*w);
            status.elapsed_ms = done ? w->elapsed_ms : so_far;
            if (done) {
                status.flags |= pulse::ipc::CTX_HANDLER_DONE;
                std::vector<std::wstring> rows;
                if (w->desc.explorer_command) invokable_texts(w->items, rows);
                if (w->desc.explorer_command &&
                    pulse::shell::PackagedRowsDuplicate(rows, classic_rows)) {
                    status.flags |= pulse::ipc::CTX_HANDLER_DUPLICATE;
                } else if (native_send_to && IsSendToHandlerClsid(w->desc.clsid_text)) {
                    std::vector<std::wstring> slot_only;
                    std::vector<std::wstring> native_only;
                    const auto merged = pulse::shell::MergeNativeSendTo(
                        native_send_to->items, w->items, &slot_only, &native_only);
                    check_send_to(slot_only, native_only);
                    out.insert(out.end(), merged.begin(), merged.end());
                    status.rows = static_cast<uint32_t>(merged.size());
                } else {
                    out.insert(out.end(), w->items.begin(), w->items.end());
                    status.rows = static_cast<uint32_t>(w->items.size());
                }
            }
            statuses.push_back(std::move(status));
        }
        return out;
    };
    // Auto-disable feeds on this list, so it names only a handler that is
    // provably hung on its own: still running at the stuck limit while every
    // other worker finished. Slow-but-finished handlers are never reported —
    // the menu does not wait for them (partials stream as workers finish).
    // Several unfinished workers at once is a systemic stall (cold host, an
    // antivirus scanning each DLL, the loader lock held by the default menu
    // SendTo builds); blaming all of them permanently hid 25 healthy
    // extensions (0-47 ms each) on a user's machine. SendTo itself is exempt
    // (#77); while it is still running, a worker still loading its DLL may
    // merely be waiting on the loader lock, so it is not blamed either.
    auto collect_hung = [&] {
        std::vector<std::wstring> hung;
        if (GetTickCount64() - handlers_started < kHandlerStuckMs) return hung;
        bool send_to_running = false;
        const HandlerWorker* stuck = nullptr;
        size_t stuck_count = 0;
        for (const auto& w : workers) {
            if (!w || worker_done(*w)) continue;
            if (IsSendToHandlerClsid(w->desc.clsid_text)) {
                send_to_running = true;
                continue;
            }
            stuck = w.get();
            ++stuck_count;
        }
        if (stuck_count != 1 || !stuck || stuck->desc.clsid_text.empty()) return hung;
        if (send_to_running && stuck->phase.load() < 1) return hung;
        hung.push_back(stuck->desc.clsid_text);
        return hung;
    };

    auto done_count = [&] {
        size_t n = 0;
        for (const auto& w : workers)
            if (w && worker_done(*w)) ++n;
        return n;
    };

    if (!workers.empty()) {
        // Throttled so a burst of finishing extensions repaints the open menu
        // a few times, not once per worker.
        constexpr ULONGLONG kPartialGapMs = 60;
        bool sent_partial = false;
        size_t partial_done = 0;
        ULONGLONG partial_at = 0;
        while (!all_workers_done() && !SessionCloseRequested(sid)) {
            pump_session_messages();
            if (SessionCloseRequested(sid)) break;
            const ULONGLONG elapsed = GetTickCount64() - handlers_started;
            if (elapsed >= kHandlerStuckMs) break;
            if (elapsed >= kFastBudgetMs) {
                // First snapshot at the fast budget, then one more whenever
                // further workers finished: a single hung extension must not
                // hold back every row that arrived after 80 ms until the 5 s
                // stuck limit (发送到 needs ~150 ms on a busy machine).
                const size_t done = done_count();
                if (!sent_partial ||
                    (done != partial_done && elapsed - partial_at >= kPartialGapMs)) {
                    items = collect_items(statuses);
                    SendCtxItems(sid, items, CTX_ITEMS_PARTIAL, {}, statuses);
                    sent_partial = true;
                    partial_done = done;
                    partial_at = elapsed;
                }
            }
            std::vector<HANDLE> waits;
            waits.reserve(workers.size());
            for (const auto& w : workers)
                if (w && w->done_event && !worker_done(*w)) waits.push_back(w->done_event);
            if (waits.empty()) break;
            DWORD timeout = 200;
            if (!sent_partial) {
                const ULONGLONG left = elapsed >= kFastBudgetMs ? 1 : (kFastBudgetMs - elapsed);
                timeout = left > 0xFFFFFFFFULL ? 200 : static_cast<DWORD>(left);
                if (timeout == 0) timeout = 1;
            } else if (done_count() != partial_done) {
                // Finished rows are waiting for the throttle gap: wake for it.
                const ULONGLONG since = GetTickCount64() - handlers_started - partial_at;
                timeout = since >= kPartialGapMs ? 1 : static_cast<DWORD>(kPartialGapMs - since);
            }
            MsgWaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(),
                                      FALSE, timeout, QS_ALLINPUT);
        }
        pump_session_messages();
        items = collect_items(statuses);
    } else {
        const HRESULT fallback_hr = api.fallback(*data, &fallback_menu, &fallback_hmenu, items);
        ContextDiagnostic(sid, L"fallback hr=" + std::to_wstring(static_cast<unsigned long>(fallback_hr)));
    }

    const uint32_t elapsed = static_cast<uint32_t>(GetTickCount64() - started);
    wchar_t timing[160];
    swprintf_s(timing, L"QueryContextMenu %ums items=%zu handlers=%zu background=%d",
               elapsed, items.size(), workers.size(), data->background ? 1 : 0);
    api.log(timing);
    if (elapsed >= 500) {
        wchar_t slow[192];
        swprintf_s(slow, L"slow handler %ums path=%ls", elapsed,
                   data->paths.empty() ? L"" : data->paths.front().c_str());
        api.log(slow);
    }
    for (const auto& status : statuses) {
        if (status.elapsed_ms < 300) continue;
        wchar_t line[160];
        swprintf_s(line, L"handler %ls %ums rows=%u%ls", status.clsid.c_str(), status.elapsed_ms,
                   status.rows, (status.flags & CTX_HANDLER_DONE) ? L"" : L" unfinished");
        api.log(line);
    }
    if (ContextDiagnosticsEnabled()) {
        for (const auto& status : statuses)
            ContextDiagnostic(sid, L"status=" + DiagnosticText(status.clsid) + L" flags=" +
                std::to_wstring(status.flags) + L" rows=" + std::to_wstring(status.rows) +
                L" ms=" + std::to_wstring(status.elapsed_ms));
        for (const auto& item : items)
            ContextDiagnostic(sid, L"output id=" + std::to_wstring(item.id) + L" enabled=" +
                std::to_wstring(item.enabled) + L" child=" + std::to_wstring(item.child) +
                L" verb=" + DiagnosticText(item.verb) + L" text=" + DiagnosticText(item.text));
    }
    SendCtxItems(sid, items, 0, collect_hung(), statuses);

    auto dispatch_invoke = [&](std::unique_ptr<CtxInvokeMsg> inv) {
        if (!inv) return;
        uint32_t live = pulse::shell::FindItemId(
            items, inv->item_id, inv->verb, inv->text);
        if (!workers.empty()) {
            HandlerWorker* target = nullptr;
            for (auto& w : workers) {
                if (w && live >= w->id_first && live <= w->id_last) {
                    target = w.get();
                    break;
                }
            }
            if (target && target->thread_id && live != 0) {
                CtxInvokeMsg* raw = inv.release();
                if (!PostThreadMessageW(target->thread_id, WM_CTX_INVOKE, 0,
                                        reinterpret_cast<LPARAM>(raw))) {
                    std::unique_ptr<CtxInvokeMsg> back(raw);
                    SendDone(back->invoke_req_id, HRESULT_FROM_WIN32(GetLastError()),
                             false, L"context menu session not ready");
                }
            } else {
                SendDone(inv->invoke_req_id, E_INVALIDARG, false,
                         L"context menu invoke failed");
            }
            return;
        }
        if (fallback_menu && live != 0)
            CtxInvoke(*data, fallback_menu, kCtxIdFirst, inv->invoke_req_id, live);
        else
            SendDone(inv->invoke_req_id, E_INVALIDARG, false,
                     L"context menu invoke failed");
    };

    // The query pump may already have consumed WM_CTX_CLOSE. Its durable state
    // must survive this phase transition, otherwise no message remains to wake
    // the 120-second wait below. Preserve an already accepted pending invoke.
    bool done = SessionCloseRequested(sid) || (workers.empty() && fallback_menu == nullptr);
    if (pending_invoke) {
        dispatch_invoke(std::move(pending_invoke));
        done = true;
    }
    const ULONGLONG deadline = GetTickCount64() + kCtxSessionExpireMs;
    while (!done) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const DWORD wait = MsgWaitForMultipleObjects(
            0, nullptr, FALSE, (DWORD)(deadline - now), QS_ALLINPUT);
        if (wait == WAIT_TIMEOUT) break;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_CTX_INVOKE) {
                dispatch_invoke(std::unique_ptr<CtxInvokeMsg>(
                    reinterpret_cast<CtxInvokeMsg*>(msg.lParam)));
                done = true;
                break;
            }
            if (msg.message == WM_CTX_CLOSE) {
                done = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_ctx_sessions.erase(sid);
    }
    JoinHandlerWorkers(workers);
    if (fallback_hmenu) DestroyMenu(fallback_hmenu);
    if (fallback_menu) fallback_menu->Release();
    CoUninitialize();
    return 0;
}

void CtxSessionThreadEnded() {
    bool recheck = false;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        --g_ctx_threads;
        recheck = !g_abandoned_threads.empty();
    }
    if (recheck) PostMessageW(g.hwnd_msg, WM_RECYCLE_CHECK, 0, 0);
}

int CtxCrashFilter(EXCEPTION_POINTERS* ep) {
    wchar_t mod[MAX_PATH]{};
    HMODULE hm = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &hm);
    if (hm) GetModuleFileNameW(hm, mod, ARRAYSIZE(mod));
    wchar_t buf[640];
    swprintf_s(buf, L"CtxSessionThread CRASH code=%08X addr=%p mod=%s",
               ep->ExceptionRecord->ExceptionCode,
               ep->ExceptionRecord->ExceptionAddress, hm ? mod : L"?");
    HostLog(buf);
    pulse::crash::ReportRecoverable(ep, "shell-context-menu");
    return EXCEPTION_EXECUTE_HANDLER;
}

// Separate function: __except bodies cannot contain objects with unwinding.
void CtxCrashCleanup(uint32_t sid) {
    SendCtxItems(sid, {});
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    g_ctx_sessions.erase(sid);
}

DWORD WINAPI CtxSessionThread(LPVOID param) {
    // Grab the session id up front: on a crash we still answer the query with
    // an empty item list so the UI process is not left waiting for RSP_CTX_ITEMS.
    const uint32_t sid = static_cast<CtxSessionData*>(param)->session_id;
    DWORD result = 0;
    __try {
        result = CtxSessionThreadImpl(param);
    } __except (CtxCrashFilter(GetExceptionInformation())) {
        CtxCrashCleanup(sid);
    }
    CtxSessionThreadEnded();
    return result;
}

void StartCtxSession(uint32_t session_id, const uint8_t* payload, size_t size) {
    auto data = std::make_unique<CtxSessionData>();
    data->session_id = session_id;
    PayloadReader r(payload, size);
    uint32_t owner = 0, flags = 0;
    if (!r.GetU32(owner) || !r.GetU32(flags) || !r.GetStringArray(data->paths) ||
        data->paths.empty()) {
        SendCtxItems(session_id, {});
        return;
    }
    r.TryStringArray(data->disabled_clsids);
    data->owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(owner));
    data->extended = (flags & CTXF_EXTENDED) != 0;
    data->background = (flags & CTXF_BACKGROUND) != 0;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_ctx_sessions[session_id] = CtxSlot{};
        ++g_ctx_threads;
    }
    HANDLE thread = CreateThread(nullptr, 0, CtxSessionThread, data.get(), 0, nullptr);
    if (!thread) {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        --g_ctx_threads;
        g_ctx_sessions.erase(session_id);
        SendCtxItems(session_id, {});
        return;
    }
    data.release(); // owned by the thread now
    CloseHandle(thread);
}

// Route invoke/close from the reader thread to the owning session thread.
void PostCtxMessage(uint32_t session_id, UINT message, WPARAM wParam, LPARAM lParam) {
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    auto it = g_ctx_sessions.find(session_id);
    if (it == g_ctx_sessions.end()) {
        if (message == WM_CTX_INVOKE) {
            std::unique_ptr<CtxInvokeMsg> inv(reinterpret_cast<CtxInvokeMsg*>(lParam));
            SendDone(inv->invoke_req_id, HRESULT_FROM_WIN32(ERROR_NOT_FOUND), false,
                     L"context menu session expired");
        }
        return;
    }
    if (it->second.thread_id == 0) {
        it->second.close_requested = true;
        if (message == WM_CTX_INVOKE) {
            std::unique_ptr<CtxInvokeMsg> inv(reinterpret_cast<CtxInvokeMsg*>(lParam));
            SendDone(inv->invoke_req_id, HRESULT_FROM_WIN32(ERROR_NOT_READY), false,
                     L"context menu session not ready");
        }
        return;
    }
    if (!PostThreadMessageW(it->second.thread_id, message, wParam, lParam) &&
        message == WM_CTX_INVOKE) {
        std::unique_ptr<CtxInvokeMsg> inv(reinterpret_cast<CtxInvokeMsg*>(lParam));
        SendDone(inv->invoke_req_id, HRESULT_FROM_WIN32(GetLastError()), false,
                 L"context menu session not ready");
    }
}

DWORD WINAPI ParentWatchdog(LPVOID param) {
    DWORD pid = (DWORD)(uintptr_t)param;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0; // parent already gone or not ours; keep serving standalone

    // A recycled PID must not become an unrelated watchdog target. The real
    // UI necessarily started before this host, so a candidate created later
    // cannot be its parent even if the numeric PID matches.
    FILETIME parent_created{}, parent_exit{}, parent_kernel{}, parent_user{};
    FILETIME host_created{}, host_exit{}, host_kernel{}, host_user{};
    if (!GetProcessTimes(h, &parent_created, &parent_exit, &parent_kernel, &parent_user) ||
        !GetProcessTimes(GetCurrentProcess(), &host_created, &host_exit,
                         &host_kernel, &host_user) ||
        CompareFileTime(&parent_created, &host_created) > 0) {
        CloseHandle(h);
        return 0;
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    if (g.running.load()) ExitProcess(0);
    return 0;
}

// Scans packaged (MSIX) context-menu manifests once at startup so the first
// right-click does not pay for it on its session thread. The result is
// cached in PackagedContextMenuVerbs() behind its own lock.
DWORD WINAPI PackagedVerbsPrewarm(LPVOID) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    pulse::shell::IsolatedStartPinSession::Prewarm();
    const ULONGLONG start = GetTickCount64();
    const size_t count = pulse::shell::PackagedContextMenuVerbs().size();
    wchar_t buf[96];
    swprintf_s(buf, L"Packaged verbs prewarmed %llums count=%zu",
               GetTickCount64() - start, count);
    HostLog(buf);
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

// A handler thread that never returned cannot be stopped (#65). Once nothing
// else runs in the host, end the process: that reclaims those threads, and
// Pulse starts a fresh host on its next request.
void MaybeRecycleHost() {
    if (g.requests_in_flight.load() != 0) return;
    std::lock_guard<std::mutex> lock(g_ctx_mutex); // keeps new sessions out
    auto& stuck = g_abandoned_threads;
    stuck.erase(std::remove_if(stuck.begin(), stuck.end(), [](HANDLE thread) {
        if (WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) return false;
        CloseHandle(thread); // it returned after all
        return true;
    }), stuck.end());
    if (stuck.empty() || g_ctx_threads != 0 || !g_ctx_sessions.empty() ||
        g.requests_in_flight.load() != 0) return;
    wchar_t line[128];
    swprintf_s(line, L"Recycling host: %zu context menu handler thread(s) never returned",
               stuck.size());
    HostLog(line);
    // Not ExitProcess: DLL detach would run beside threads that may hold the
    // loader lock or a heap lock.
    TerminateProcess(GetCurrentProcess(), 0);
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_EXEC_REQUEST) {
        ExecuteRequest(reinterpret_cast<Request*>(lParam));
        g.requests_in_flight.fetch_sub(1);
        MaybeRecycleHost();
        return 0;
    }
    if (msg == WM_RECYCLE_CHECK) {
        MaybeRecycleHost();
        return 0;
    }
    if (msg == WM_QUIT_HOST) {
        g.running.store(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    pulse::crash::Initialize({pulse::crash::ProcessRole::Shell, false, {}});
    // IFileOperation creates its conflict/confirmation UI in this process.
    // Declare PMv2 before COM or any HWND exists so Windows does not bitmap-scale
    // those dialogs on high-DPI displays.
    pulse::compat::EnableDpiAwareness();
    if (__argc > 1 && wcscmp(__wargv[1], L"--isolated-start-pin") == 0)
        return pulse::shell::RunIsolatedStartPin(__argc, __wargv);

    DWORD ui_pid = GetCurrentProcessId();
    if (__argc > 1) {
        ui_pid = (DWORD)_wtoi(__wargv[1]);
        if (ui_pid == 0) ui_pid = GetCurrentProcessId();
    }
    // Record who is allowed to drive this host. Querying the image of the
    // process that passed the pid also covers standalone runs, where that
    // process is whatever launched us (a test harness, for example).
    g_expected_peer.pid = ui_pid;
    g_expected_peer.executable = LowerPath(ProcessImagePath(ui_pid));

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;
    if (__argc == 3 && (!_wcsicmp(__wargv[1], L"--start-pin") || !_wcsicmp(__wargv[1], L"--start-unpin"))) {
        hr = pulse::start_pin::Invoke(__wargv[2], !_wcsicmp(__wargv[1], L"--start-pin"));
        CoUninitialize();
        return static_cast<int>(hr);
    }

    std::wstring pipe_name = PipeNameFor(ui_pid);
    pulse::CurrentUserSecurityAttributes pipe_security;
    if (!pipe_security) {
        HostLog(L"Cannot create pipe security descriptor");
        CoUninitialize();
        return 2;
    }
    g.pipe = CreateNamedPipeW(pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, pipe_security.get());
    if (g.pipe == INVALID_HANDLE_VALUE) {
        HostLog(L"CreateNamedPipe failed");
        CoUninitialize();
        return 2;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"PulseShellHost";
    RegisterClassExW(&wc);
    g.hwnd_msg = CreateWindowExW(0, wc.lpszClassName, L"PulseShellHost", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!g.hwnd_msg) {
        CloseHandle(g.pipe);
        CoUninitialize();
        return 3;
    }

    HANDLE reader = CreateThread(nullptr, 0, ReaderThread, nullptr, 0, nullptr);
    HANDLE prewarm = CreateThread(nullptr, 0, PackagedVerbsPrewarm, nullptr, 0, nullptr);
    if (ui_pid != GetCurrentProcessId()) {
        HANDLE wd = CreateThread(nullptr, 0, ParentWatchdog,
            reinterpret_cast<LPVOID>((uintptr_t)ui_pid), 0, nullptr);
        if (wd) CloseHandle(wd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g.running.store(false);
    CancelIoEx(g.pipe, nullptr); // unblock reader thread's pending I/O
    if (reader) {
        WaitForSingleObject(reader, 2000);
        CloseHandle(reader);
    }
    if (prewarm) {
        WaitForSingleObject(prewarm, 2000);
        CloseHandle(prewarm);
    }
    DestroyWindow(g.hwnd_msg);
    CloseHandle(g.pipe);
    CoUninitialize();
    return 0;
}
