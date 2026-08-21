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
#include "../common/path_utils.h"
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
// Context-menu session thread messages (defined below with the session code).
constexpr UINT WM_CTX_INVOKE = WM_APP + 10;  // wParam=invoke req id, lParam=item id
constexpr UINT WM_CTX_CLOSE = WM_APP + 11;

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
    HANDLE h = CreateFileW(i_path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 28 || sz.QuadPart > 64 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::vector<BYTE> buf(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || read < 28) return false;

    uint64_t ver = 0;
    memcpy(&ver, buf.data(), 8);
    if (ver == 2) {
        uint32_t nchars = 0;
        memcpy(&nchars, buf.data() + 24, 4);
        if (nchars == 0 || nchars > 32768) return false;
        const size_t need = 28ull + static_cast<size_t>(nchars) * 2ull;
        size_t bytes = static_cast<size_t>(nchars) * 2ull;
        if (need > buf.size()) {
            if (buf.size() <= 28) return false;
            bytes = buf.size() - 28;
            nchars = static_cast<uint32_t>(bytes / 2);
        }
        original.assign(reinterpret_cast<const wchar_t*>(buf.data() + 28), nchars);
        while (!original.empty() && original.back() == L'\0') original.pop_back();
        return !original.empty();
    }
    if (ver == 1) {
        const size_t maxn = (std::min)((buf.size() - 24) / 2, static_cast<size_t>(260));
        const wchar_t* p = reinterpret_cast<const wchar_t*>(buf.data() + 24);
        original.assign(p, wcsnlen(p, maxn));
        return !original.empty();
    }
    return false;
}

bool RestoreOneFromRecycle(const std::wstring& wanted_canon, std::wstring& error) {
    const wchar_t drive = wanted_canon.size() >= 2 && wanted_canon[1] == L':'
        ? wanted_canon[0] : L'\0';
    if (!drive) {
        error = L"无法确定回收站卷";
        return false;
    }

    wchar_t bin[32];
    swprintf_s(bin, L"%c:\\$Recycle.Bin", drive);
    WIN32_FIND_DATAW sidFd{};
    HANDLE sidFind = FindFirstFileW((std::wstring(bin) + L"\\*").c_str(), &sidFd);
    if (sidFind == INVALID_HANDLE_VALUE) {
        error = L"无法打开回收站";
        return false;
    }

    bool restored = false;
    do {
        if (!(sidFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (sidFd.cFileName[0] == L'.') continue;
        std::wstring sidDir = std::wstring(bin) + L"\\" + sidFd.cFileName;
        WIN32_FIND_DATAW iFd{};
        HANDLE iFind = FindFirstFileW((sidDir + L"\\$I*").c_str(), &iFd);
        if (iFind == INVALID_HANDLE_VALUE) continue;
        do {
            if (iFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring iPath = sidDir + L"\\" + iFd.cFileName;
            std::wstring original;
            if (!ReadRecycleOriginal(iPath, original)) continue;
            if (CanonPath(original) != wanted_canon) continue;

            std::wstring rName = iFd.cFileName;
            if (rName.size() >= 2) rName[1] = (rName[1] == L'I') ? L'R' : L'r';
            std::wstring rPath = sidDir + L"\\" + rName;
            if (GetFileAttributesW(rPath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

            std::wstring dest = ToParsingPath(original);
            if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
                error = L"还原目标已存在";
                FindClose(iFind);
                FindClose(sidFind);
                return false;
            }
            if (!MoveFileExW(rPath.c_str(), dest.c_str(), 0)) {
                error = L"还原失败";
                FindClose(iFind);
                FindClose(sidFind);
                return false;
            }
            DeleteFileW(iPath.c_str());
            restored = true;
            break;
        } while (FindNextFileW(iFind, &iFd));
        FindClose(iFind);
        if (restored) break;
    } while (FindNextFileW(sidFind, &sidFd));
    FindClose(sidFind);

    if (!restored) error = L"回收站中未找到该项";
    return restored;
}

HRESULT ExecuteRestore(const std::vector<std::wstring>& paths, std::wstring& error) {
    if (paths.empty()) return E_INVALIDARG;
    size_t ok = 0;
    for (const auto& src : paths) {
        std::wstring one_error;
        if (RestoreOneFromRecycle(CanonPath(src), one_error)) {
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
        DWORD flags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | kDontDisplayUi;
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

DWORD WINAPI ReaderThread(LPVOID) {
    __try {
        return ReaderThreadImpl();
    } __except (HostLog(L"ReaderThread CRASH"), EXCEPTION_EXECUTE_HANDLER) {
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
                if (r.GetU32(session) && r.GetU32(item))
                    PostCtxMessage(session, WM_CTX_INVOKE, h.request_id, item);
                else
                    SendDone(h.request_id, E_INVALIDARG, false, L"malformed ctx invoke");
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
                PostMessageW(g.hwnd_msg, WM_EXEC_REQUEST, 0, reinterpret_cast<LPARAM>(req));
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
};

struct CtxSlot {
    DWORD thread_id = 0;       // 0 until the session thread has a message queue
    bool close_requested = false;
};

std::mutex g_ctx_mutex;
std::map<uint32_t, CtxSlot> g_ctx_sessions;

struct CtxItemOut {
    uint32_t id = 0;
    bool enabled = true;
    bool separator_after = false;
    bool has_children = false;  // submenu header row (id not invokable)
    bool child = false;         // row inside the preceding header's flyout
    std::wstring verb;
    std::wstring text;
};

void SendCtxItems(uint32_t session_id, const std::vector<CtxItemOut>& items) {
    PayloadWriter w;
    w.PutU32(session_id);
    w.PutU32((uint32_t)items.size());
    for (const auto& it : items) {
        w.PutU32(it.id);
        uint32_t flags = 0;
        if (it.enabled) flags |= CTX_ITEM_ENABLED;
        if (it.separator_after) flags |= CTX_ITEM_SEPARATOR_AFTER;
        if (it.has_children) flags |= CTX_ITEM_HAS_CHILDREN;
        if (it.child) flags |= CTX_ITEM_CHILD;
        w.PutU32(flags);
        w.PutString(it.verb);
        w.PutString(it.text);
    }
    SendMsg(RSP_CTX_ITEMS, session_id, w.data());
}

// GetCommandString is the classic crash pit: handlers index internal tables
// with the raw offset, and ids that belong to dynamically populated submenus
// (Send To, New) routinely AV. Explorer guards this call with SEH; so do we.
bool SafeGetVerbW(IContextMenu* menu, UINT offset, wchar_t* buf, UINT cch) {
    __try {
        return SUCCEEDED(menu->GetCommandString(offset, GCS_VERBW, nullptr,
                                                reinterpret_cast<CHAR*>(buf), cch));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::wstring CtxVerbOf(IContextMenu* menu, UINT id) {
    if (id < kCtxIdFirst) return {};
    wchar_t buf[128]{};
    if (SafeGetVerbW(menu, id - kCtxIdFirst, buf, ARRAYSIZE(buf) - 1))
        return buf;
    return {};
}

std::wstring MenuItemText(HMENU menu, UINT pos) {
    wchar_t buf[512]{};
    MENUITEMINFOW mii{ sizeof(mii) };
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = buf;
    mii.cch = ARRAYSIZE(buf) - 1;
    if (!GetMenuItemInfoW(menu, pos, TRUE, &mii)) return {};
    return CleanMenuText(buf);
}

// Walks the populated HMENU: top-level verbs pass the built-in filter;
// software-owned submenus keep their hierarchy as a header row + child rows
// (one level; deeper nesting is cut). The UI shows them as a flyout.
void CollectCtxItems(IContextMenu* menu, IContextMenu2* menu2, HMENU hmenu,
                     bool background, std::vector<CtxItemOut>& out) {
    const int count = GetMenuItemCount(hmenu);
    bool pending_separator = false;
    auto push = [&](CtxItemOut item) {
        if (item.text.empty()) return;
        if (pending_separator && !out.empty()) {
            out.back().separator_after = true;
            pending_separator = false;
        }
        out.push_back(std::move(item));
    };
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{ sizeof(mii) };
        mii.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(hmenu, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) {
            if (!out.empty()) pending_separator = true;
            continue;
        }
        const bool enabled = !(mii.fState & (MFS_DISABLED | MFS_GRAYED));
        if (mii.hSubMenu) {
            const std::wstring parent_verb = CtxVerbOf(menu, mii.wID);
            if (IsDroppedContextSubmenu(parent_verb)) continue;
            const std::wstring parent_text = MenuItemText(hmenu, (UINT)i);
            if (parent_text.empty()) continue;
            // Dynamic submenus (Send To, New) populate on WM_INITMENUPOPUP.
            if (menu2) {
                menu2->HandleMenuMsg(WM_INITMENUPOPUP,
                                     reinterpret_cast<WPARAM>(mii.hSubMenu),
                                     MAKELPARAM(i, TRUE));
            }
            const int sub_count = GetMenuItemCount(mii.hSubMenu);
            std::vector<CtxItemOut> kids;
            for (int j = 0; j < sub_count && (int)kids.size() < kMaxSubmenuChildren; ++j) {
                MENUITEMINFOW sub{ sizeof(sub) };
                sub.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
                if (!GetMenuItemInfoW(mii.hSubMenu, j, TRUE, &sub)) continue;
                if ((sub.fType & MFT_SEPARATOR) || sub.hSubMenu) continue; // one level only
                const std::wstring child_text = MenuItemText(mii.hSubMenu, (UINT)j);
                if (child_text.empty()) continue;
                CtxItemOut item;
                item.id = sub.wID;
                item.enabled = enabled && !(sub.fState & (MFS_DISABLED | MFS_GRAYED));
                item.child = true;
                item.verb = CtxVerbOf(menu, sub.wID);
                item.text = child_text;
                kids.push_back(std::move(item));
            }
            if (kids.empty()) continue;
            CtxItemOut header;
            header.id = 0; // never invoked; children carry the command ids
            header.enabled = enabled;
            header.has_children = true;
            header.verb = parent_verb;
            header.text = parent_text;
            push(std::move(header));
            for (auto& k : kids) out.push_back(std::move(k));
            continue;
        }
        if (mii.wID < kCtxIdFirst || mii.wID > kCtxIdLast) continue;
        const std::wstring verb = CtxVerbOf(menu, mii.wID);
        if (IsBuiltinContextVerb(verb, background)) continue;
        CtxItemOut item;
        item.id = mii.wID;
        item.enabled = enabled;
        item.verb = verb;
        item.text = MenuItemText(hmenu, (UINT)i);
        push(std::move(item));
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
    UINT flags = CMF_NORMAL;
    if (d.extended) flags |= CMF_EXTENDEDVERBS;
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

void CtxInvoke(const CtxSessionData& d, IContextMenu* menu, uint32_t invoke_req_id,
               uint32_t item_id) {
    std::wstring dir = ToParsingPath(d.paths.front());
    if (!d.background) {
        const auto slash = dir.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 2) dir.resize(slash);
    }
    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE;
    info.hwnd = d.owner;
    info.lpVerb = MAKEINTRESOURCEA(item_id - kCtxIdFirst);
    info.lpVerbW = MAKEINTRESOURCEW(item_id - kCtxIdFirst);
    info.lpDirectoryW = dir.c_str();
    info.nShow = SW_SHOWNORMAL;
    const HRESULT hr = menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
    SendDone(invoke_req_id, hr, false,
             FAILED(hr) ? L"context menu invoke failed" : L"");
}

DWORD WINAPI CtxSessionThreadImpl(LPVOID param) {
    std::unique_ptr<CtxSessionData> data(static_cast<CtxSessionData*>(param));
    const uint32_t sid = data->session_id;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        SendCtxItems(sid, {});
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_ctx_sessions.erase(sid);
        return 0;
    }
    // Create the thread message queue, then publish the thread id so the
    // reader can PostThreadMessage invoke/close at us without racing.
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

    IContextMenu* menu = nullptr;
    HMENU hmenu = nullptr;
    std::vector<CtxItemOut> items;
    const ULONGLONG started = GetTickCount64();
    BuildCtxMenu(*data, &menu, &hmenu, items);
    const uint32_t elapsed = static_cast<uint32_t>(GetTickCount64() - started);
    wchar_t timing[160];
    swprintf_s(timing, L"QueryContextMenu %ums items=%zu background=%d",
               elapsed, items.size(), data->background ? 1 : 0);
    HostLog(timing);
    if (elapsed >= 500) {
        wchar_t slow[192];
        swprintf_s(slow, L"slow handler %ums path=%ls", elapsed,
                   data->paths.empty() ? L"" : data->paths.front().c_str());
        HostLog(slow);
    }
    SendCtxItems(sid, items);

    // Session loop: wait for invoke/close; auto-expire as a leak guard.
    const ULONGLONG deadline = GetTickCount64() + kCtxSessionExpireMs;
    bool done = (menu == nullptr);
    while (!done) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const DWORD wait = MsgWaitForMultipleObjects(
            0, nullptr, FALSE, (DWORD)(deadline - now), QS_ALLINPUT);
        if (wait == WAIT_TIMEOUT) break;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_CTX_INVOKE) {
                CtxInvoke(*data, menu, (uint32_t)msg.wParam, (uint32_t)msg.lParam);
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
    if (hmenu) DestroyMenu(hmenu);
    if (menu) menu->Release();
    CoUninitialize();
    return 0;
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
    __try {
        return CtxSessionThreadImpl(param);
    } __except (CtxCrashFilter(GetExceptionInformation())) {
        CtxCrashCleanup(sid);
        return 0;
    }
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
    data->owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(owner));
    data->extended = (flags & CTXF_EXTENDED) != 0;
    data->background = (flags & CTXF_BACKGROUND) != 0;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_ctx_sessions[session_id] = CtxSlot{};
    }
    HANDLE thread = CreateThread(nullptr, 0, CtxSessionThread, data.get(), 0, nullptr);
    if (!thread) {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
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
        if (message == WM_CTX_INVOKE)
            SendDone((uint32_t)wParam, HRESULT_FROM_WIN32(ERROR_NOT_FOUND), false,
                     L"context menu session expired");
        return;
    }
    if (it->second.thread_id == 0) {
        // Session thread has no queue yet (still building). Invoke cannot
        // legitimately arrive this early; treat both as an early close.
        it->second.close_requested = true;
        if (message == WM_CTX_INVOKE)
            SendDone((uint32_t)wParam, HRESULT_FROM_WIN32(ERROR_NOT_READY), false,
                     L"context menu session not ready");
        return;
    }
    PostThreadMessageW(it->second.thread_id, message, wParam, lParam);
}

DWORD WINAPI ParentWatchdog(LPVOID param) {
    DWORD pid = (DWORD)(uintptr_t)param;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return 0; // parent already gone or not ours; keep serving standalone
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    if (g.running.load()) ExitProcess(0);
    return 0;
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_EXEC_REQUEST) {
        ExecuteRequest(reinterpret_cast<Request*>(lParam));
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
    // IFileOperation creates its conflict/confirmation UI in this process.
    // Declare PMv2 before COM or any HWND exists so Windows does not bitmap-scale
    // those dialogs on high-DPI displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    DWORD ui_pid = GetCurrentProcessId();
    if (__argc > 1) {
        ui_pid = (DWORD)_wtoi(__wargv[1]);
        if (ui_pid == 0) ui_pid = GetCurrentProcessId();
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    std::wstring pipe_name = PipeNameFor(ui_pid);
    g.pipe = CreateNamedPipeW(pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, nullptr);
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
    DestroyWindow(g.hwnd_msg);
    CloseHandle(g.pipe);
    CoUninitialize();
    return 0;
}
