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
#include "packaged_ctx_handlers.h"
#include "../common/current_user_security.h"
#include "../common/path_utils.h"
#include "../common/recycle_index.h"
#include "../common/crash_reporter.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
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
    std::wstring new_name;
    std::vector<std::wstring> sources;
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
void HostLog(const wchar_t* msg) {
    wchar_t dir[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir))) return;
    std::wstring path = std::wstring(dir) + L"\\Pulse";
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\pulse_shell_host.log";
    if (HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); f != INVALID_HANDLE_VALUE) {
        char buf[512];
        int n = snprintf(buf, sizeof(buf), "[%lu] %ls\n", GetCurrentProcessId(), msg);
        if (n > 0) { DWORD w = 0; WriteFile(f, buf, (DWORD)n, &w, nullptr); }
        CloseHandle(f);
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
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem* psi) override {
        RememberItem(psi);
        return CheckCancel();
    }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT hr, IShellItem*) override {
        NoteItemResult(hr);
        ++items_done_;
        MaybeReport(items_done_ == total_items_);
        return CheckCancel();
    }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return CheckCancel(); }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return CheckCancel(); }

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

HRESULT MakeItem(const std::wstring& path, IShellItem** out) {
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

std::wstring CanonPath(std::wstring p) {
    p = ToParsingPath(std::move(p));
    for (auto& c : p) {
        if (c == L'/') c = L'\\';
        c = static_cast<wchar_t>(towupper(c));
    }
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

bool ReadRecycleOriginal(const std::wstring& i_path, std::wstring& original) {
    original.clear();
    pulse::recycle::IndexRecord record;
    if (!pulse::recycle::ReadIndex(i_path, record)) return false;
    original = std::move(record.original_path);
    return true;
}

// The production caller validates the current user's recycle-root identity
// before reaching this step. Tests use only their own isolated $I/$R fixtures.
bool RestoreSelectedRecyclePayload(const std::wstring& payload, std::wstring& error) {
    std::wstring index = payload;
    const size_t slash = index.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 2 >= index.size()) return false;
    index[slash + 2] = L'I';
    std::wstring original;
    if (!ReadRecycleOriginal(index, original)) {
        error = L"无法读取所选回收站条目";
        return false;
    }
    const std::wstring dest = ToParsingPath(original);
    if (!MoveFileExW(payload.c_str(), dest.c_str(), 0)) {
        error = L"还原失败（目标可能已存在）";
        return false;
    }
    DeleteFileW(index.c_str());
    return true;
}

bool RestoreOneFromRecycle(const std::wstring& payload, std::wstring& error) {
    const std::wstring wanted = CanonPath(payload);
    const std::wstring sid = pulse::CurrentUserSidString();
    if (wanted.size() < 3 || wanted[1] != L':' || sid.empty()) {
        error = L"无法确定当前用户的回收站";
        return false;
    }
    const std::wstring parent = wanted.substr(0, wanted.find_last_of(L'\\'));
    const std::wstring expected = CanonPath(wanted.substr(0, 2) + L"\\$Recycle.Bin\\" + sid);
    const std::wstring name = wanted.substr(wanted.find_last_of(L'\\') + 1);
    if (parent != expected || name.size() <= 2 || name.substr(0, 2) != L"$R") {
        // Legacy undo records contain only the original path. Never guess
        // between multiple versions of that path.
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((expected + L"\\$I*").c_str(), &data);
        std::wstring match;
        bool ambiguous = false;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                std::wstring original;
                const std::wstring index = expected + L"\\" + data.cFileName;
                if (!ReadRecycleOriginal(index, original) || CanonPath(original) != wanted) continue;
                std::wstring candidate = index;
                candidate[candidate.find_last_of(L'\\') + 2] = L'R';
                if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                if (!match.empty()) { ambiguous = true; break; }
                match = std::move(candidate);
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        if (!match.empty() && !ambiguous) return RestoreOneFromRecycle(match, error);
        error = L"无法唯一确定回收站版本，请在回收站中选择具体条目";
        return false;
    }
    return RestoreSelectedRecyclePayload(payload, error);
}

HRESULT ExecuteRestore(const std::vector<std::wstring>& paths, std::wstring& error) {
    if (paths.empty()) return E_INVALIDARG;
    size_t ok = 0;
    for (const auto& src : paths) {
        std::wstring one_error;
        if (RestoreOneFromRecycle(src, one_error)) {
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

void ExecuteRequest(Request* req) {
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
            case REQ_RESTORE_RECYCLE: {
                auto* req = new Request();
                req->type = h.type;
                req->id = h.request_id;
                PayloadReader r(payload.data(), payload.size());
                bool ok = true;
                if (req->type == REQ_RENAME) {
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
                  const std::vector<std::wstring>& slow_clsids = {}) {
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
        item_flags |= pulse::ipc::PackCtxItemMnemonic(it.mnemonic);
        w.PutU32(item_flags);
        w.PutString(it.verb);
        w.PutString(it.text);
        w.PutString(it.clsid);
        w.PutString(it.handler);
    }
    w.PutStringArray(slow_clsids);
    SendMsg(RSP_CTX_ITEMS, session_id, w.data());
}

UINT QueryContextMenuFlags(const CtxSessionData& d) {
    UINT flags = CMF_NORMAL;
    if (d.extended) flags |= CMF_EXTENDEDVERBS;
    if (!d.background) {
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
                     bool background, std::vector<CtxItemOut>& out) {
    pulse::shell::CtxHandlerSlot slot;
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
            if (SUCCEEDED(SHParseDisplayName(ToParsingPath(p).c_str(), nullptr, &pidl, 0, nullptr)))
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
    IContextMenu2* menu2 = nullptr;
    menu->QueryInterface(IID_PPV_ARGS(&menu2));
    CollectCtxItems(menu, menu2, hmenu, d.background, items);
    if (menu2) menu2->Release();
    *out_menu = menu;
    *out_hmenu = hmenu;
    return S_OK;
}

void CtxInvoke(const CtxSessionData& d, IContextMenu* menu, UINT id_first,
               uint32_t invoke_req_id, uint32_t item_id) {
    if (!menu || item_id < id_first) {
        SendDone(invoke_req_id, E_INVALIDARG, false, L"context menu invoke failed");
        return;
    }
    std::wstring dir = ToParsingPath(d.paths.front());
    if (!d.background) {
        const auto slash = dir.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 2) dir.resize(slash);
    }
    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE;
    info.hwnd = d.owner;
    info.lpVerb = MAKEINTRESOURCEA(item_id - id_first);
    info.lpVerbW = MAKEINTRESOURCEW(item_id - id_first);
    info.lpDirectoryW = dir.c_str();
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
    pulse::shell::CtxHandlerDesc desc;
    std::vector<std::wstring> paths;
    bool background = false;
    HWND owner = nullptr;
    UINT id_first = 0;
    UINT id_last = 0;
    UINT qcm_flags = 0;
    pulse::shell::CtxHandlerSlot slot;
    std::vector<CtxItemOut> items;
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
    if (pulse::shell::BindCtxSelection(w->paths, w->background, bind)) {
        const HRESULT hr = pulse::shell::QueryOneHandler(
            w->desc, bind, w->owner, w->id_first, w->id_last, w->qcm_flags, w->slot, &w->phase);
        if (SUCCEEDED(hr) && w->slot.menu)
            pulse::shell::CollectHandlerItems(w->slot, w->background, w->items);
        else
            pulse::shell::ReleaseHandlerSlot(w->slot);
    }
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
                if (w->slot.menu && inv->item_id >= w->id_first)
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

    std::vector<std::unique_ptr<HandlerWorker>> workers;
    std::vector<CtxItemOut> items;
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

    auto handlers = api.enumerate(
        data->background, data->paths.front(), data->disabled_clsids);
    if (TestStuckHandlerFor(data->paths.front()) &&
        std::find(data->disabled_clsids.begin(), data->disabled_clsids.end(),
                  kTestStuckHandler) == data->disabled_clsids.end()) {
        pulse::shell::CtxHandlerDesc stuck;
        stuck.clsid_text = kTestStuckHandler;
        handlers.insert(handlers.begin(), std::move(stuck));
    }
    if (handlers.size() > MAXIMUM_WAIT_OBJECTS)
        handlers.resize(MAXIMUM_WAIT_OBJECTS);

    // Ownership transfer after starting a thread must not allocate/throw.
    workers.reserve(handlers.size());
    UINT next_id = kCtxIdFirst;
    for (const auto& handler : handlers) {
        if (next_id + kIdsPerHandler > kCtxIdLast) break;
        auto worker = std::make_unique<HandlerWorker>();
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
    auto collect_items = [&] {
        std::vector<std::wstring> classic_rows;
        for (const auto& w : workers)
            if (w && worker_done(*w) && !w->desc.explorer_command)
                invokable_texts(w->items, classic_rows);
        std::vector<CtxItemOut> out;
        for (const auto& w : workers) {
            if (!w || !worker_done(*w)) continue;
            if (w->desc.explorer_command) {
                std::vector<std::wstring> rows;
                invokable_texts(w->items, rows);
                if (pulse::shell::PackagedRowsDuplicate(rows, classic_rows)) continue;
            }
            out.insert(out.end(), w->items.begin(), w->items.end());
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
        if (GetTickCount64() - started < kHandlerStuckMs) return hung;
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
            const ULONGLONG elapsed = GetTickCount64() - started;
            if (elapsed >= kHandlerStuckMs) break;
            if (elapsed >= kFastBudgetMs) {
                // First snapshot at the fast budget, then one more whenever
                // further workers finished: a single hung extension must not
                // hold back every row that arrived after 80 ms until the 5 s
                // stuck limit (发送到 needs ~150 ms on a busy machine).
                const size_t done = done_count();
                if (!sent_partial ||
                    (done != partial_done && elapsed - partial_at >= kPartialGapMs)) {
                    items = collect_items();
                    SendCtxItems(sid, items, CTX_ITEMS_PARTIAL);
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
                const ULONGLONG since = GetTickCount64() - started - partial_at;
                timeout = since >= kPartialGapMs ? 1 : static_cast<DWORD>(kPartialGapMs - since);
            }
            MsgWaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(),
                                      FALSE, timeout, QS_ALLINPUT);
        }
        pump_session_messages();
        items = collect_items();
    } else {
        api.fallback(*data, &fallback_menu, &fallback_hmenu, items);
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
    SendCtxItems(sid, items, 0, collect_hung());

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
