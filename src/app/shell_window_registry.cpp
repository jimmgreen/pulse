// shell_window_registry.cpp — see shell_window_registry.h.
#include "shell_window_registry.h"

// shlobj.h first: it brings in objbase (`interface`) under WIN32_LEAN_AND_MEAN.
#include <shlobj.h>
#include <exdisp.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <type_traits>
#include <algorithm>
#ifdef PULSE_SHELL_WINDOW_REGISTRY_TEST
#include "shell_window_registry_test.h"
#endif

namespace pulse::app {

using Microsoft::WRL::ComPtr;

struct ShellWindowRegistry::Shared {
    ShellWindowRegistry::CreateWindows create = nullptr;
    HWND window = nullptr;
    DWORD window_thread = 0;   // owns `window`; Register pairs with RegisterPending by it
    UINT select_message = 0;
    std::mutex mutex;
    std::vector<ShellWindowEntry> desired;
    uint64_t revision = 1;
    HANDLE wake = nullptr;
    ~Shared() { if (wake) CloseHandle(wake); }
    std::atomic<bool> stopping{false};
};

namespace {

constexpr DWORD kRetryFirstMs = 250;
constexpr DWORD kRetryMaxMs = 5000;
bool Disconnected(HRESULT hr) {
    return hr == RPC_E_DISCONNECTED || hr == CO_E_OBJNOTCONNECTED ||
        hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || hr == RPC_E_SERVER_DIED ||
        hr == RPC_E_SERVER_DIED_DNE;
}

} // namespace

void TraceShellWindows(const wchar_t* format, ...) {
#ifdef PULSE_WITH_SELFTEST
    static const std::wstring path = [] {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_SHELL_WINDOWS_LOG", buffer, MAX_PATH);
        return length > 0 && length < MAX_PATH ? std::wstring(buffer) : std::wstring();
    }();
    if (path.empty()) return;
    wchar_t line[1024]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line, _TRUNCATE, format, args);
    va_end(args);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char text[1200]{};
    const int prefix = sprintf_s(text, "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond,
                                 now.wMilliseconds);
    const int body = WideCharToMultiByte(CP_UTF8, 0, line, -1, text + prefix,
                                         static_cast<int>(sizeof(text)) - prefix - 2, nullptr, nullptr);
    if (body <= 0) return;
    const size_t end = static_cast<size_t>(prefix + body - 1);
    text[end] = '\n';
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(end + 1), &written, nullptr);
    CloseHandle(file);
#else
    (void)format;
#endif
}

namespace {


struct PidlDeleter {
    void operator()(std::remove_pointer_t<PIDLIST_ABSOLUTE>* pidl) const noexcept { CoTaskMemFree(pidl); }
};
using UniquePidl = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, PidlDeleter>;

UniquePidl FolderPidl(const std::wstring& path) {
#ifdef PULSE_SHELL_WINDOW_REGISTRY_TEST
    return UniquePidl(ShellRegistryTestFolder(path));
#else
    PIDLIST_ABSOLUTE pidl = nullptr;
    const HRESULT hr = path.empty()
        ? SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &pidl)
        : SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
    return SUCCEEDED(hr) ? UniquePidl(pidl) : UniquePidl();
#endif
}

std::wstring PidlName(PCIDLIST_ABSOLUTE pidl, SIGDN kind) {
    PWSTR name = nullptr;
    if (!pidl || FAILED(SHGetNameFromIDList(pidl, kind, &name)) || !name) return {};
    std::wstring result = name;
    CoTaskMemFree(name);
    return result;
}

HRESULT ReturnBstr(const std::wstring& text, BSTR* out) {
    if (!out) return E_POINTER;
    *out = SysAllocString(text.c_str());
    return *out ? S_OK : E_OUTOFMEMORY;
}

// IShellView over one pane: only SelectItem does anything. The shell asks
// for it through the document's IServiceProvider (IID_IFolderView service).
class FolderView final : public IShellView {
public:
    FolderView(uint64_t key, HWND window, UINT message) : key_(key), window_(window), message_(message) {}
    void SetFolder(UniquePidl folder) { folder_ = std::move(folder); }
    PCIDLIST_ABSOLUTE Folder() const { return folder_.get(); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IOleWindow || riid == IID_IShellView) {
            *ppv = static_cast<IShellView*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const long refs = --refs_;
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    IFACEMETHODIMP GetWindow(HWND* hwnd) override {
        if (!hwnd) return E_POINTER;
        *hwnd = window_;
        return S_OK;
    }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

    IFACEMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }
    IFACEMETHODIMP EnableModeless(BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP UIActivate(UINT) override { return E_NOTIMPL; }
    IFACEMETHODIMP Refresh() override { return E_NOTIMPL; }
    IFACEMETHODIMP CreateViewWindow(IShellView*, LPCFOLDERSETTINGS, IShellBrowser*, RECT*, HWND*) override {
        return E_NOTIMPL;
    }
    IFACEMETHODIMP DestroyViewWindow() override { return E_NOTIMPL; }
    IFACEMETHODIMP GetCurrentInfo(LPFOLDERSETTINGS) override { return E_NOTIMPL; }
    IFACEMETHODIMP AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override { return E_NOTIMPL; }
    IFACEMETHODIMP SaveViewState() override { return E_NOTIMPL; }
    IFACEMETHODIMP SelectItem(PCUITEMID_CHILD item, SVSIF flags) override {
        TraceShellWindows(L"SelectItem key=%llx flags=0x%x", static_cast<unsigned long long>(key_), flags);
        if (!item) return E_INVALIDARG;
        // SVSI_DESELECT (0) alone: nothing to show.
        if ((flags & (SVSI_SELECT | SVSI_EDIT | SVSI_FOCUSED | SVSI_ENSUREVISIBLE)) == 0) return S_OK;
        if (!folder_) return E_FAIL;
        UniquePidl full(ILCombine(folder_.get(), item));
        if (!full) return E_OUTOFMEMORY;
        auto request = std::make_unique<ShellSelectRequest>();
        request->key = key_;
        request->path = PidlName(full.get(), SIGDN_DESKTOPABSOLUTEPARSING);
        request->flags = flags;
        if (request->path.empty()) return E_FAIL;
        if (PostMessageW(window_, message_, 0, reinterpret_cast<LPARAM>(request.get()))) request.release();
        return S_OK;
    }
    IFACEMETHODIMP GetItemObject(UINT, REFIID, void** ppv) override {
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }

private:
    ~FolderView() = default;
    std::atomic<long> refs_{1};
    const uint64_t key_;
    const HWND window_;
    const UINT message_;
    UniquePidl folder_;
};

// IWebBrowserApp::get_Document result. The shell only queries it for
// IServiceProvider; the IDispatch side has no members.
class FolderDocument final : public IDispatch, public IServiceProvider {
public:
    explicit FolderDocument(FolderView* view) : view_(view) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch) *ppv = static_cast<IDispatch*>(this);
        else if (riid == IID_IServiceProvider) *ppv = static_cast<IServiceProvider*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const long refs = --refs_;
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    IFACEMETHODIMP GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        return S_OK;
    }
    IFACEMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** info) override {
        if (info) *info = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
    IFACEMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        return DISP_E_MEMBERNOTFOUND;
    }

    IFACEMETHODIMP QueryService(REFGUID service, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        wchar_t guid[40]{};
        StringFromGUID2(service, guid, ARRAYSIZE(guid));
        TraceShellWindows(L"QueryService %s", guid);
        if (service == IID_IFolderView || service == IID_IShellView) return view_->QueryInterface(riid, ppv);
        return E_NOINTERFACE;
    }

private:
    ~FolderDocument() = default;
    std::atomic<long> refs_{1};
    ComPtr<FolderView> view_;
};

// What IShellWindows holds for a pane. Location* also lets programs that list
// shell windows (Shell.Application.Windows()) see which folder a pane shows.
class BrowserApp final : public IWebBrowserApp {
public:
    BrowserApp(HWND window, FolderDocument* document, FolderView* view)
        : window_(window), document_(document), view_(view) {
        // SHDocVw's type library gives scripts (Shell.Application.Windows())
        // names for the properties below. {EAB22AC0-30C1-11CF-A7EB-0000C05BAE0B}
        static constexpr GUID kLibShDocVw = {0xEAB22AC0, 0x30C1, 0x11CF, {0xA7, 0xEB, 0x00, 0x00, 0xC0, 0x5B, 0xAE, 0x0B}};
        ComPtr<ITypeLib> library;
        if (SUCCEEDED(LoadRegTypeLib(kLibShDocVw, 1, 1, LOCALE_NEUTRAL, &library)))
            library->GetTypeInfoOfGuid(IID_IWebBrowserApp, &type_info_);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IWebBrowser ||
            riid == IID_IWebBrowserApp) {
            *ppv = static_cast<IWebBrowserApp*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const long refs = --refs_;
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    // IDispatch
    IFACEMETHODIMP GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = type_info_ ? 1 : 0;
        return S_OK;
    }
    IFACEMETHODIMP GetTypeInfo(UINT index, LCID, ITypeInfo** info) override {
        if (!info) return E_POINTER;
        *info = nullptr;
        if (index != 0 || !type_info_) return DISP_E_BADINDEX;
        return type_info_.CopyTo(info);
    }
    IFACEMETHODIMP GetIDsOfNames(REFIID, LPOLESTR* names, UINT count, LCID, DISPID* ids) override {
        if (!type_info_) return DISP_E_UNKNOWNNAME;
        return DispGetIDsOfNames(type_info_.Get(), names, count, ids);
    }
    IFACEMETHODIMP Invoke(DISPID id, REFIID, LCID, WORD flags, DISPPARAMS* params, VARIANT* result,
                          EXCEPINFO* exception, UINT* arg_error) override {
        if (!type_info_) return DISP_E_MEMBERNOTFOUND;
        return DispInvoke(static_cast<IWebBrowserApp*>(this), type_info_.Get(), id, flags, params, result,
                          exception, arg_error);
    }

    // IWebBrowser
    IFACEMETHODIMP GoBack() override { return E_NOTIMPL; }
    IFACEMETHODIMP GoForward() override { return E_NOTIMPL; }
    IFACEMETHODIMP GoHome() override { return E_NOTIMPL; }
    IFACEMETHODIMP GoSearch() override { return E_NOTIMPL; }
    IFACEMETHODIMP Navigate(BSTR, VARIANT*, VARIANT*, VARIANT*, VARIANT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Refresh() override { return E_NOTIMPL; }
    IFACEMETHODIMP Refresh2(VARIANT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Stop() override { return E_NOTIMPL; }
    IFACEMETHODIMP get_Application(IDispatch** out) override { return NoDispatch(out); }
    IFACEMETHODIMP get_Parent(IDispatch** out) override { return NoDispatch(out); }
    IFACEMETHODIMP get_Container(IDispatch** out) override { return NoDispatch(out); }
    IFACEMETHODIMP get_Document(IDispatch** out) override {
        if (!out) return E_POINTER;
        *out = static_cast<IDispatch*>(document_.Get());
        (*out)->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP get_TopLevelContainer(VARIANT_BOOL* out) override { return Bool(out, true); }
    IFACEMETHODIMP get_Type(BSTR* out) override { return NoBstr(out); }
    IFACEMETHODIMP get_Left(long*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_Left(long) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_Top(long*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_Top(long) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_Width(long*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_Width(long) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_Height(long*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_Height(long) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_LocationName(BSTR* out) override {
        return ReturnBstr(PidlName(view_->Folder(), SIGDN_NORMALDISPLAY), out);
    }
    IFACEMETHODIMP get_LocationURL(BSTR* out) override {
        // Explorer reports "" for folders without a file system path (This PC).
        const std::wstring path = PidlName(view_->Folder(), SIGDN_FILESYSPATH);
        std::wstring url;
        if (!path.empty()) {
            wchar_t buffer[2084]{};   // INTERNET_MAX_URL_LENGTH (wininet.h)
            DWORD length = ARRAYSIZE(buffer);
            if (SUCCEEDED(UrlCreateFromPathW(path.c_str(), buffer, &length, 0))) url = buffer;
        }
        return ReturnBstr(url, out);
    }
    IFACEMETHODIMP get_Busy(VARIANT_BOOL* out) override { return Bool(out, false); }

    // IWebBrowserApp
    IFACEMETHODIMP Quit() override { return E_NOTIMPL; }
    IFACEMETHODIMP ClientToWindow(int*, int*) override { return E_NOTIMPL; }
    IFACEMETHODIMP PutProperty(BSTR, VARIANT) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetProperty(BSTR, VARIANT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_Name(BSTR* out) override { return ReturnBstr(L"Pulse", out); }
    IFACEMETHODIMP get_HWND(SHANDLE_PTR* out) override {
        if (!out) return E_POINTER;
        *out = reinterpret_cast<SHANDLE_PTR>(window_);
        return S_OK;
    }
    IFACEMETHODIMP get_FullName(BSTR* out) override { return NoBstr(out); }
    IFACEMETHODIMP get_Path(BSTR* out) override { return NoBstr(out); }
    IFACEMETHODIMP get_Visible(VARIANT_BOOL* out) override { return Bool(out, IsWindowVisible(window_) != FALSE); }
    IFACEMETHODIMP put_Visible(VARIANT_BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_StatusBar(VARIANT_BOOL*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_StatusBar(VARIANT_BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_StatusText(BSTR* out) override { return NoBstr(out); }
    IFACEMETHODIMP put_StatusText(BSTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_ToolBar(int*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_ToolBar(int) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_MenuBar(VARIANT_BOOL*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_MenuBar(VARIANT_BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP get_FullScreen(VARIANT_BOOL*) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_FullScreen(VARIANT_BOOL) override { return E_NOTIMPL; }

private:
    ~BrowserApp() = default;
    static HRESULT NoDispatch(IDispatch** out) {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
    static HRESULT NoBstr(BSTR* out) {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
    static HRESULT Bool(VARIANT_BOOL* out, bool value) {
        if (!out) return E_POINTER;
        *out = value ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }

    std::atomic<long> refs_{1};
    const HWND window_;
    ComPtr<ITypeInfo> type_info_;
    ComPtr<FolderDocument> document_;
    ComPtr<FolderView> view_;
};

// Lives on the registry thread; owns every IShellWindows call.
class Worker {
public:
    explicit Worker(ShellWindowRegistry::Shared& shared) : shared_(shared) {}
    ~Worker() {
        // Stop is best-effort cleanup, never an unbounded retry loop.
        while (!registered_.empty()) {
            const auto key = registered_.begin()->first;
            Revoke(key);
            registered_.erase(key);
        }
    }
    void Apply() {
        const auto now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            if (revision_ != shared_.revision) {
                revision_ = shared_.revision;
                wanted_ = shared_.desired;
                dirty_ = true;
                due_ = 0;
                retry_ms_ = kRetryFirstMs;
            }
        }
        if (!dirty_ || shared_.stopping || now < due_) return;
        HRESULT failure = S_OK;
        if (!windows_ && (!wanted_.empty() || !registered_.empty())) {
            if (shared_.create) failure = shared_.create(&windows_);
            else {
#ifdef PULSE_SHELL_WINDOW_REGISTRY_TEST
            failure = ShellRegistryTestCreate(&windows_);
#else
            failure = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows_));
#endif
            }
        }
        if (SUCCEEDED(failure)) {
            std::vector<uint64_t> removed;
            for (const auto& [key, entry] : registered_) {
                if (entry.revoking || std::none_of(wanted_.begin(), wanted_.end(),
                    [&](const auto& value) { return value.key == key; })) removed.push_back(key);
            }
            for (const auto key : removed) {
                if (shared_.stopping) return;
                const auto hr = Revoke(key);
                if (FAILED(hr)) failure = hr;
                if (Disconnected(hr)) break;
            }
            if (!Disconnected(failure)) for (const auto& wanted : wanted_) {
                if (shared_.stopping) return;
                const auto hr = Advance(wanted.key, wanted.path);
                if (FAILED(hr)) failure = hr;
                if (Disconnected(hr)) break;
            }
        }
        if (Disconnected(failure)) {
            for (auto& [key, entry] : registered_) {
                (void)key;
                if (entry.view) entry.view->SetFolder(UniquePidl());
                // Best effort: a transient RPC failure can leave Explorer
                // running, and stale entries would keep answering for us.
                if (windows_ && entry.pending) windows_->Revoke(entry.cookie);
                if (windows_ && entry.bound && entry.window_cookie != entry.cookie)
                    windows_->Revoke(entry.window_cookie);
            }
            registered_.clear();
            windows_.Reset();
        }
        dirty_ = FAILED(failure);
        if (dirty_) {
            const auto delay = retry_ms_;
            due_ = GetTickCount64() + delay;
            retry_ms_ = (std::min)(retry_ms_ * 2, kRetryMaxMs);
            TraceShellWindows(L"retry revision=%llu hr=0x%08x delay=%lu", revision_,
                static_cast<unsigned>(failure), delay);
        } else {
            due_ = 0;
            retry_ms_ = kRetryFirstMs;
        }
#ifdef PULSE_SHELL_WINDOW_REGISTRY_TEST
        std::vector<ShellWindowEntry> acknowledged;
        for (const auto& [key, entry] : registered_)
            if (entry.ready) acknowledged.push_back({key, entry.path});
        ShellRegistryTestApplied(acknowledged);
#endif
    }
    DWORD WaitMs() const {
        if (!dirty_) return INFINITE;
        const auto now = GetTickCount64();
        return now >= due_ ? 0 : static_cast<DWORD>((std::min)(due_ - now, ULONGLONG(kRetryMaxMs)));
    }
private:
    struct Registered {
        long cookie = 0, window_cookie = 0;
        bool pending = false, bound = false, ready = false, revoking = false;
        std::wstring path;
        ComPtr<FolderView> view;
        ComPtr<BrowserApp> app;
    };
    HRESULT Advance(uint64_t key, const std::wstring& path) {
        auto& entry = registered_[key];
        if (entry.revoking) return E_PENDING;
        if (entry.ready && _wcsicmp(entry.path.c_str(), path.c_str()) == 0) return S_OK;
        UniquePidl folder = FolderPidl(path);
        if (!folder) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        VARIANT location;
        VariantInit(&location);
        HRESULT hr = InitVariantFromBuffer(folder.get(), ILGetSize(folder.get()), &location);
        if (FAILED(hr)) return hr;
        if (!entry.pending && !shared_.stopping) {
            VARIANT root; VariantInit(&root);
            hr = windows_->RegisterPending(static_cast<long>(shared_.window_thread), &location,
                &root, SWC_BROWSER, &entry.cookie);
            entry.pending = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr) && !entry.bound && !shared_.stopping) {
            if (!entry.view) {
                entry.view.Attach(new FolderView(key, shared_.window, shared_.select_message));
                entry.view->SetFolder(UniquePidl(ILCloneFull(folder.get())));
                ComPtr<FolderDocument> document;
                document.Attach(new FolderDocument(entry.view.Get()));
                entry.app.Attach(new BrowserApp(shared_.window, document.Get(), entry.view.Get()));
            }
            hr = windows_->Register(entry.app.Get(), HandleToLong(shared_.window), SWC_BROWSER, &entry.window_cookie);
            entry.bound = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr) && !shared_.stopping) {
            hr = windows_->OnNavigate(entry.cookie, &location);
            if (SUCCEEDED(hr)) {
                entry.view->SetFolder(std::move(folder));
                entry.path = path;
                entry.ready = true;
            }
        }
        VariantClear(&location);
        return hr;
    }
    HRESULT Revoke(uint64_t key) {
        const auto found = registered_.find(key);
        if (found == registered_.end()) return S_OK;
        auto& entry = found->second;
        entry.revoking = true;
        entry.ready = false;
        if (entry.view) entry.view->SetFolder(UniquePidl());
        if (windows_ && entry.pending) {
            const auto hr = windows_->Revoke(entry.cookie);
            if (FAILED(hr)) return hr;
            entry.pending = false;
            if (entry.bound && entry.window_cookie == entry.cookie) entry.bound = false;
        }
        if (windows_ && entry.bound) {
            const auto hr = windows_->Revoke(entry.window_cookie);
            if (FAILED(hr)) return hr;
            entry.bound = false;
        }
        registered_.erase(found);
        return S_OK;
    }
    ShellWindowRegistry::Shared& shared_;
    ComPtr<IShellWindows> windows_;
    std::map<uint64_t, Registered> registered_;
    std::vector<ShellWindowEntry> wanted_;
    uint64_t revision_ = 0;
    bool dirty_ = false;
    ULONGLONG due_ = 0;
    DWORD retry_ms_ = kRetryFirstMs;
};

DWORD WINAPI RegistryThread(void* param) {
    std::shared_ptr<ShellWindowRegistry::Shared> shared =
        std::move(*static_cast<std::shared_ptr<ShellWindowRegistry::Shared>*>(param));
    delete static_cast<std::shared_ptr<ShellWindowRegistry::Shared>*>(param);
    HRESULT init = E_FAIL;
    DWORD init_delay = kRetryFirstMs;
    while (!shared->stopping) {
#ifdef PULSE_SHELL_WINDOW_REGISTRY_TEST
        init = ShellRegistryTestInitialize();
#else
        init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
#endif
        if (SUCCEEDED(init)) break;
        WaitForSingleObject(shared->wake, init_delay);
        init_delay = (std::min)(init_delay * 2, kRetryMaxMs);
    }
    if (FAILED(init)) return 1;
    {
        MSG msg{};
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        Worker worker(*shared);
        while (!shared->stopping) {
            worker.Apply();
            if (shared->stopping) break;
            const auto result = MsgWaitForMultipleObjectsEx(1, &shared->wake, worker.WaitMs(),
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_FAILED) break;
            while (!shared->stopping && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { shared->stopping = true; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    CoUninitialize();
    return 0;
}

} // namespace

ShellWindowRegistry::ShellWindowRegistry(HWND window, UINT select_message, CreateWindows create)
    : shared_(std::make_shared<Shared>()) {
    shared_->create = create;
    shared_->window = window;
    shared_->window_thread = GetWindowThreadProcessId(window, nullptr);
    shared_->select_message = select_message;
}

ShellWindowRegistry::~ShellWindowRegistry() { Stop(); }

bool ShellWindowRegistry::Running() const {
    return thread_ && WaitForSingleObject(thread_, 0) == WAIT_TIMEOUT;
}

void ShellWindowRegistry::EnsureRunning() {
    if (shared_->stopping) return;
    if (thread_) {
        if (WaitForSingleObject(thread_, 0) == WAIT_TIMEOUT) return;
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    const auto now = GetTickCount64();
    if (now < next_start_) return;
    next_start_ = now + start_retry_ms_;
    start_retry_ms_ = (std::min)(start_retry_ms_ * 2, kRetryMaxMs);
    if (!shared_->wake) shared_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!shared_->wake) return;
    auto* param = new std::shared_ptr<Shared>(shared_);
    DWORD thread_id = 0;
#ifdef PULSE_SHELL_WINDOW_REGISTRY_TEST
    thread_ = ShellRegistryTestStart(&RegistryThread, param, &thread_id);
#else
    thread_ = CreateThread(nullptr, 0, &RegistryThread, param, 0, &thread_id);
#endif
    if (!thread_) delete param;
    else start_retry_ms_ = kRetryFirstMs;
}

void ShellWindowRegistry::Publish(std::vector<ShellWindowEntry> wanted) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->desired != wanted) {
            shared_->desired = std::move(wanted);
            ++shared_->revision;
            changed = true;
        }
    }
    EnsureRunning();
    if (changed && shared_->wake) SetEvent(shared_->wake);
}

void ShellWindowRegistry::Stop(DWORD timeout_ms) {
    shared_->stopping = true;
    if (shared_->wake) SetEvent(shared_->wake);
    if (!thread_) return;
    WaitForSingleObject(thread_, timeout_ms);
    CloseHandle(thread_);
    thread_ = nullptr;
}

} // namespace pulse::app
