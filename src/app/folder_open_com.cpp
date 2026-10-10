// folder_open_com.cpp — the folder-open delegate.
//
// On Windows 11, HKLM registers CLSID_ExecuteFolder as
// Folder\shell\open\command's DelegateExecute, so a folder open activates that
// COM class instead of running the (Default) command line. A program's "open
// file location" (WeChat, QQ, a launcher) therefore never reaches a registry
// verb Pulse could write; the only way onto that path is to become the delegate.
//
// Measured behaviour of the shell's activation (Windows 11 26300), which is what
// this server is shaped after:
//
//   ClassFactory::CreateInstance
//   IExecuteCommand::SetShowWindow / SetDirectory / SetNoShowUI
//   IObjectWithSelection::SetSelection   <-- the FOLDER, never the file
//   IInitializeCommand::Initialize       <-- verb "open", property bag empty
//   IExecuteCommand::Execute
//
// The file the caller wanted revealed is not delivered here at all; the shell
// pushes it to the opened view through IShellView::SelectItem instead, which
// Pulse already implements (shell_window_registry.cpp). Recognising that is why
// `target` is normally empty and the window resolves the entry itself.
// Standard headers first: folder_open_com.h pulls in the MIDL interfaces, and
// the macros they define break the STL if it is parsed afterwards.
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "folder_open_com.h"

namespace pulse::app::folder_open {

namespace {

// Pulse's own folder-open delegate. The same value is written to
// Folder\shell\open\command\DelegateExecute by shell_integration_registry.cpp.
const CLSID class_id{0x6e2f1a44, 0x9c3b, 0x4d7e, {0x8a, 0x51, 0x2b, 0x0c, 0x7d, 0x9e, 0x4f, 0x13}};
constexpr size_t kMaxRequests = 64;

// Minimal owning COM pointer. Deliberately local: reaching for wrl/client.h
// from this header's namespace chain breaks MIDL parsing (see the header).
template <class T>
class Owned {
public:
    Owned() = default;
    ~Owned() { Reset(); }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;

    T* Get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T* operator->() const { return value_; }

    void Reset() {
        if (value_) value_->Release();
        value_ = nullptr;
    }
    void Attach(T* value) {
        Reset();
        value_ = value;
    }

private:
    T* value_ = nullptr;
};

struct ServerState {
    std::mutex mutex;
    bool accepting = true;
    std::vector<Request> pending;
    std::atomic<unsigned> commands{0};
};

std::shared_ptr<ServerState> current;
DWORD registration = 0;

// The folder argument arrives as a property name in some shell paths and as
// SetDirectory in others; keep whichever names a real directory.
bool LooksLikeDirectory(const std::wstring& value) {
    if (value.empty()) return false;
    // Reject verb/flag-looking values and shell:::{GUID} parsing names.
    if (value[0] == L'/' || value[0] == L'-') return false;
    if (value.rfind(L"shell:", 0) == 0 || value.rfind(L"::{", 0) == 0) return false;
    const DWORD attrs = GetFileAttributesW(value.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

class Command final : public IExecuteCommand, public IObjectWithSelection, public IInitializeCommand {
public:
    explicit Command(std::shared_ptr<ServerState> state) : state_(std::move(state)) { ++state_->commands; }
    ~Command() { --state_->commands; }

    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IExecuteCommand) *out = static_cast<IExecuteCommand*>(this);
        else if (iid == IID_IObjectWithSelection) *out = static_cast<IObjectWithSelection*>(this);
        else if (iid == IID_IInitializeCommand) *out = static_cast<IInitializeCommand*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }

    IFACEMETHODIMP SetKeyState(DWORD) override { return S_OK; }
    IFACEMETHODIMP SetParameters(PCWSTR) override { return S_OK; }
    IFACEMETHODIMP SetPosition(POINT) override { return S_OK; }
    IFACEMETHODIMP SetShowWindow(int) override { return S_OK; }
    IFACEMETHODIMP SetNoShowUI(BOOL) override { return S_OK; }
    IFACEMETHODIMP SetDirectory(PCWSTR directory) override {
        if (executed_ || !directory) return E_INVALIDARG;
        try {
            const std::wstring value(directory);
            if (LooksLikeDirectory(value)) folder_ = value;
            return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }

    IFACEMETHODIMP Initialize(PCWSTR verb, IPropertyBag* bag) override {
        if (executed_) return E_INVALIDARG;
        try {
            if (verb) verb_ = verb;
            if (!bag) return S_OK;
            // The shell has been observed leaving every value empty, but a
            // caller that does describe its target would appear here.
            for (const wchar_t* name : {L"Directory", L"Path", L"File", L"Item", L"Selection",
                                        L"Target", L"FocusedItem"}) {
                VARIANT value;
                VariantInit(&value);
                if (SUCCEEDED(bag->Read(name, &value, nullptr))) {
                    if (value.vt == VT_BSTR && value.bstrVal) {
                        const std::wstring text(value.bstrVal);
                        if (LooksLikeDirectory(text)) folder_ = text;
                        else if (!text.empty()) target_ = text;
                    }
                    VariantClear(&value);
                }
            }
            return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }

    IFACEMETHODIMP SetSelection(IShellItemArray* items) override {
        if (executed_ || !items) return E_INVALIDARG;
        DWORD count = 0;
        if (FAILED(items->GetCount(&count))) return S_OK;
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* item = nullptr;
            if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
            PWSTR path = nullptr;
            const HRESULT named = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            item->Release();
            if (FAILED(named) || !path) continue;
            const std::wstring value(path);
            CoTaskMemFree(path);
            // The shell hands the delegate the folder itself; a child item here
            // would be the entry the caller wanted focused.
            if (LooksLikeDirectory(value)) folder_ = value;
            else if (!value.empty()) target_ = value;
        }
        return S_OK;
    }

    IFACEMETHODIMP GetSelection(REFIID, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP Execute() override {
        if (executed_) return S_OK;
        if (folder_.empty()) return E_INVALIDARG;
        try {
            Request request;
            request.folder = folder_;
            request.target = target_;
            request.verb = verb_;
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->accepting || state_->pending.size() >= kMaxRequests)
                return HRESULT_FROM_WIN32(ERROR_BUSY);
            state_->pending.push_back(std::move(request));
            executed_ = true;
            return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<ServerState> state_;
    std::wstring folder_, target_, verb_;
    bool executed_ = false;
};

class Factory final : public IClassFactory {
public:
    explicit Factory(std::shared_ptr<ServerState> state) : state_(std::move(state)) {}
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *out = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->accepting) return CO_E_SERVER_STOPPING;
        Command* command = new (std::nothrow) Command(state_);
        if (!command) return E_OUTOFMEMORY;
        const HRESULT hr = command->QueryInterface(iid, out);
        command->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL) override { return S_OK; }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<ServerState> state_;
};

} // namespace

HRESULT RegisterCommandServer(const CLSID* class_override) {
    if (registration) return S_OK;
    try {
        auto state = std::make_shared<ServerState>();
        Owned<IClassFactory> factory;
        factory.Attach(new (std::nothrow) Factory(state));
        if (!factory) return E_OUTOFMEMORY;
        const HRESULT hr = CoRegisterClassObject(class_override ? *class_override : class_id,
                                                factory.Get(), CLSCTX_LOCAL_SERVER,
                                                REGCLS_MULTIPLEUSE, &registration);
        if (SUCCEEDED(hr)) current = std::move(state);
        return hr;
    } catch (...) { return E_OUTOFMEMORY; }
}

void RevokeCommandServer() {
    if (current) {
        std::lock_guard<std::mutex> lock(current->mutex);
        current->accepting = false;
        current->pending.clear();
    }
    if (registration) {
        CoRevokeClassObject(registration);
        registration = 0;
    }
    current.reset();
}

std::vector<Request> TakeRequests() {
    if (!current) return {};
    std::lock_guard<std::mutex> lock(current->mutex);
    return std::exchange(current->pending, {});
}

bool CommandServerBusy() {
    if (!current) return false;
    std::lock_guard<std::mutex> lock(current->mutex);
    return current->commands != 0 || !current->pending.empty();
}

} // namespace pulse::app::folder_open
