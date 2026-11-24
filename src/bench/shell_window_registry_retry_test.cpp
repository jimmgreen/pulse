#include "../app/shell_window_registry.h"
#include "../app/shell_window_registry_test.h"
#include "../app/shell_select_delivery.h"
#include <atomic>
#include <mutex>
#include <set>
#include <cstdio>
#include <functional>
#include <cstring>

namespace {
struct State {
    DWORD ui = GetCurrentThreadId();
    std::atomic<int> start_fail{1}, init_fail{1}, create_fail{1}, pending_fail{1}, bind_fail{1}, nav_fail{1}, revoke_fail{0};
    std::atomic<bool> block_nav{false}, disconnect_nav{false}, wrong_thread{false};
    std::atomic<int> starts{0}, inits{0}, creates{0}, pendings{0}, binds{0}, navigates{0}, revokes{0};
    std::mutex mutex;
    std::set<long> cookies;
    std::multiset<long> register_windows;  // Register's HWND argument
    std::vector<pulse::app::ShellWindowEntry> acknowledged;
    std::wstring last_navigate;
    std::multiset<std::wstring> navigated;   // every location OnNavigate received
    long next_cookie = 1;
    void Thread() { if (GetCurrentThreadId() == ui) wrong_thread = true; }
} state;
bool Fail(std::atomic<int>& remaining) {
    int count = remaining.load();
    while (count > 0) if (remaining.compare_exchange_weak(count, count - 1)) return true;
    return false;
}
int failures = 0;
std::unique_ptr<pulse::app::ShellSelectRequest> delivered;
unsigned delivery_calls = 0;
BOOL WINAPI AcceptSelection(HWND, UINT, WPARAM, LPARAM value) {
    ++delivery_calls;
    delivered.reset(reinterpret_cast<pulse::app::ShellSelectRequest*>(value));
    return TRUE;
}
BOOL WINAPI RejectSelection(HWND, UINT, WPARAM, LPARAM) {
    ++delivery_calls;
    SetLastError(ERROR_NOT_ENOUGH_QUOTA);
    return FALSE;
}
BOOL WINAPI RejectSelectionWithoutError(HWND, UINT, WPARAM, LPARAM) {
    ++delivery_calls;
    return FALSE;
}
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
}
bool Wait(const std::function<bool()>& ready, pulse::app::ShellWindowRegistry* registry = nullptr) {
    const auto deadline = GetTickCount64() + 18000;
    while (GetTickCount64() < deadline) {
        if (registry) registry->EnsureRunning();
        if (ready()) return true;
        Sleep(10);
    }
    return false;
}
bool Ack(const wchar_t* path) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.acknowledged.size() == 1 && state.acknowledged.front().key == 1 && state.acknowledged.front().path == path;
}
size_t Cookies() { std::lock_guard<std::mutex> lock(state.mutex); return state.cookies.size(); }
class FakeWindows final : public IShellWindows {
    std::atomic<ULONG> references_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDispatch && iid != IID_IShellWindows) return E_NOINTERFACE;
        *out = static_cast<IShellWindows*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* value) override { if (value) *value = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_Count(long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Item(VARIANT, IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE _NewEnum(IUnknown**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Register(IDispatch*, long window, int, long* cookie) override {
        state.Thread(); ++state.binds;
        if (Fail(state.bind_fail)) return E_FAIL;
        std::lock_guard<std::mutex> lock(state.mutex);
        state.register_windows.insert(window);
        *cookie = state.next_cookie++; state.cookies.insert(*cookie); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RegisterPending(long, VARIANT*, VARIANT*, int, long* cookie) override {
        state.Thread(); ++state.pendings;
        if (Fail(state.pending_fail)) return E_FAIL;
        std::lock_guard<std::mutex> lock(state.mutex);
        *cookie = state.next_cookie++; state.cookies.insert(*cookie); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Revoke(long cookie) override {
        state.Thread(); ++state.revokes;
        if (Fail(state.revoke_fail)) return E_FAIL;
        std::lock_guard<std::mutex> lock(state.mutex);
        state.cookies.erase(cookie); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnNavigate(long, VARIANT* location) override {
        state.Thread(); ++state.navigates;
        if (state.disconnect_nav.exchange(false)) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.cookies.clear(); return RPC_E_DISCONNECTED;
        }
        if (state.block_nav || Fail(state.nav_fail)) return E_FAIL;
        if (!location || location->vt != (VT_ARRAY | VT_UI1)) return E_INVALIDARG;
        void* data = nullptr;
        HRESULT hr = SafeArrayAccessData(location->parray, &data);
        if (FAILED(hr)) return hr;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.last_navigate = reinterpret_cast<const wchar_t*>(static_cast<const BYTE*>(data) + sizeof(USHORT));
            state.navigated.insert(state.last_navigate);
        }
        SafeArrayUnaccessData(location->parray);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnActivated(long, VARIANT_BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FindWindowSW(VARIANT*, VARIANT*, int, long*, int, IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnCreated(long, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ProcessAttachDetach(VARIANT_BOOL) override { return S_OK; }
};
}
namespace pulse::app {
HRESULT ShellRegistryTestCreate(IShellWindows** out) {
    state.Thread(); ++state.creates; *out = nullptr;
    if (Fail(state.create_fail)) return E_FAIL;
    *out = new FakeWindows(); return S_OK;
}
HRESULT ShellRegistryTestInitialize() {
    state.Thread(); ++state.inits;
    if (Fail(state.init_fail)) return E_FAIL;
    return CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
}
HANDLE ShellRegistryTestStart(LPTHREAD_START_ROUTINE start, void* param, DWORD* id) {
    ++state.starts;
    if (Fail(state.start_fail)) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    return CreateThread(nullptr, 0, start, param, 0, id);
}
PIDLIST_ABSOLUTE ShellRegistryTestFolder(const std::wstring& path) {
    // Private synthetic PIDL: no filesystem, namespace provider or Explorer access.
    const auto item_size = sizeof(USHORT) + (path.size() + 1) * sizeof(wchar_t);
    auto* bytes = static_cast<BYTE*>(CoTaskMemAlloc(item_size + sizeof(USHORT)));
    if (!bytes) return nullptr;
    const auto cb = static_cast<USHORT>(item_size);
    memcpy(bytes, &cb, sizeof(cb));
    memcpy(bytes + sizeof(cb), path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    memset(bytes + item_size, 0, sizeof(USHORT));
    return reinterpret_cast<PIDLIST_ABSOLUTE>(bytes);
}
PIDLIST_ABSOLUTE ShellRegistryTestFolderAlias(const std::wstring& path) {
    // C:\known\…: a second form that differs; C:\same\…: one byte-identical to the first.
    if (path.rfind(L"C:\\known\\", 0) == 0) return ShellRegistryTestFolder(path + L"#known-folder");
    if (path.rfind(L"C:\\same\\", 0) == 0) return ShellRegistryTestFolder(path);
    return nullptr;
}
void ShellRegistryTestApplied(const std::vector<ShellWindowEntry>& acknowledged) {
    std::lock_guard<std::mutex> lock(state.mutex); state.acknowledged = acknowledged;
}
}
int main() {
    using namespace pulse::app;
    setvbuf(stdout, nullptr, _IONBF, 0);
    const auto selection = [] {
        auto request = std::make_unique<ShellSelectRequest>();
        request->key = 42;
        request->path = L"C:\\fixture\\selected.txt";
        request->flags = SVSI_SELECT | SVSI_ENSUREVISIBLE;
        return request;
    };
    const HWND receiver = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(1));
    Check(PostShellSelect(nullptr, WM_APP + 17, selection(), AcceptSelection) ==
        HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE) && delivery_calls == 0,
        "reveal delivery: missing receiver rejects rather than posting an unowned thread message");
    Check(PostShellSelect(receiver, WM_APP + 17, selection(), RejectSelection) ==
        HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA) && !delivered,
        "reveal delivery: queue rejection preserves failing HRESULT");
    SetLastError(ERROR_ACCESS_DENIED);
    Check(PostShellSelect(receiver, WM_APP + 17, selection(), RejectSelectionWithoutError) ==
        HRESULT_FROM_WIN32(ERROR_GEN_FAILURE),
        "reveal delivery: failure without last-error never becomes S_OK or a stale error");
    Check(PostShellSelect(receiver, WM_APP + 17, selection(), AcceptSelection) == S_OK &&
        delivered && delivered->key == 42 && delivered->path == L"C:\\fixture\\selected.txt" &&
        delivered->flags == (SVSI_SELECT | SVSI_ENSUREVISIBLE),
        "reveal delivery: accepted queue owns the unchanged request");
    delivered.reset();
    ShellWindowRegistry registry(nullptr, WM_APP + 17);
    registry.Publish({{1, L"C:\\fake-A"}});
    Check(Wait([] { return Ack(L"C:\\fake-A"); }, &registry), "one Publish survives thread/init/COM/pending/register/navigate transient failures");
    Check(state.starts == 2 && state.inits == 2 && state.creates == 2, "thread and COM startup recover without publishing a changed wanted set");
    Check(state.pendings == 2 && state.binds == 2 && state.navigates == 2 && Cookies() == 2,
        "partial registration retries only failed phases without duplicate live cookies");
    const int settled_calls = state.navigates;
    for (int i = 0; i < 100; ++i) registry.Publish({{1, L"C:\\fake-A"}});
    Sleep(350);
    Check(state.navigates == settled_calls, "acknowledged identical wanted is idle");
    state.block_nav = true;
    registry.Publish({{1, L"C:\\fake-B"}});
    Check(Wait([&] { return state.navigates > settled_calls; }), "changed wanted attempts navigation on worker");
    Sleep(30);
    Check(Ack(L"C:\\fake-A"), "failed navigation leaves previous acknowledged path intact");
    const int failed_calls = state.navigates;
    for (int i = 0; i < 100; ++i) registry.Publish({{1, L"C:\\fake-B"}});
    Sleep(100);
    Check(state.navigates == failed_calls, "identical wanted does not defeat failure backoff");
    state.block_nav = false;
    Check(Wait([] { return Ack(L"C:\\fake-B"); }), "navigation recovers automatically without another Publish");
    Check(state.pendings == 2 && state.binds == 2, "successful navigation reuses existing registration");

    state.block_nav = true;
    const int before_replace = state.navigates;
    registry.Publish({{1, L"C:\\obsolete"}});
    Check(Wait([&] { return state.navigates > before_replace; }), "obsolete desired reaches injected failure");
    state.block_nav = false;
    registry.Publish({{1, L"C:\\latest"}});
    Check(Wait([] { return Ack(L"C:\\latest"); }), "new desired supersedes failed navigation during backoff");
    state.disconnect_nav = true;
    registry.Publish({{1, L"C:\\reconnected"}});
    Check(Wait([] { return Ack(L"C:\\reconnected"); }), "simulated disconnected broker is recreated and desired registered again");
    Check(state.creates == 3 && Cookies() == 2, "disconnect rebuild has one registration pair");

    state.revoke_fail = 1;
    registry.Publish({});
    Check(Wait([] { return Cookies() == 0; }), "failed revoke is retried until both cookies are released");
    state.block_nav = true;
    const int before_stop = state.navigates;
    registry.Publish({{1, L"C:\\stop-during-retry"}});
    Check(Wait([&] { return state.navigates > before_stop; }), "stop scenario reaches pending retry");
    const auto start = GetTickCount64();
    registry.Stop();
    Check(GetTickCount64() - start < 1000 && Cookies() == 0, "Stop cancels backoff promptly and releases partial registration");
    const int stopped_calls = state.navigates;
    Sleep(350); registry.EnsureRunning();
    Check(state.navigates == stopped_calls, "stopped registry performs no delayed retry or restart");

    // Multi-window M4b: each entry registers with the window showing its pane;
    // an entry without one falls back to the registry's (host) window.
    {
        const auto fake = [](uintptr_t value) { return reinterpret_cast<HWND>(value); };
        const HWND host = fake(0x1110), first = fake(0x2220), second = fake(0x3330);
        state.block_nav = false;  // left set by the stop scenario above
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.register_windows.clear();
            state.acknowledged.clear();
        }
        const std::vector<ShellWindowEntry> wanted{
            {11, L"C:\\window-1", first}, {12, L"C:\\window-2", second}, {13, L"C:\\no-window", nullptr}};
        ShellWindowRegistry multi(host, WM_APP + 17);
        multi.Publish(wanted);
        const bool acked = Wait([&] {
            std::lock_guard<std::mutex> lock(state.mutex);
            return state.acknowledged == wanted;
        }, &multi);
        std::multiset<long> windows;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            windows = state.register_windows;
        }
        Check(acked, "multi-window: entries of two windows are acknowledged with their windows");
        Check(windows == std::multiset<long>{HandleToLong(first), HandleToLong(second), HandleToLong(host)},
              "multi-window: Register gets each pane's window, the host only for an entry without one");
        multi.Publish({});
        Check(Wait([] { return Cookies() == 0; }, &multi), "multi-window: revoking every window's entries releases all cookies");
        multi.Stop();
    }
    // Reveal from other programs: FindWindowSW compares ID lists byte for byte,
    // and File Explorer's path form differs from SHParseDisplayName's
    // known-folder form below Desktop, Downloads…; a pane registers under both.
    {
        const auto fake = [](uintptr_t value) { return reinterpret_cast<HWND>(value); };
        const HWND window = fake(0x4440);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.register_windows.clear();
            state.navigated.clear();
            state.acknowledged.clear();
        }
        const auto acked = [](const std::vector<ShellWindowEntry>& want) {
            std::lock_guard<std::mutex> lock(state.mutex);
            return state.acknowledged == want;
        };
        ShellWindowRegistry forms(fake(0x1110), WM_APP + 17);
        const std::vector<ShellWindowEntry> known{{21, L"C:\\known\\Downloads", window}};
        forms.Publish(known);
        const bool known_acked = Wait([&] { return acked(known) && Cookies() == 4; }, &forms);
        bool both_forms = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            both_forms = state.register_windows == std::multiset<long>{HandleToLong(window), HandleToLong(window)} &&
                state.navigated.count(L"C:\\known\\Downloads") == 1 &&
                state.navigated.count(L"C:\\known\\Downloads#known-folder") == 1;
        }
        Check(known_acked && both_forms,
              "reveal: a folder below a known folder registers its pane under both forms, with the pane's window");
        const std::vector<ShellWindowEntry> plain{{21, L"C:\\plain", window}};
        forms.Publish(plain);
        Check(Wait([&] { return acked(plain) && Cookies() == 2; }, &forms),
              "reveal: navigating to a folder with one form revokes the second registration");
        const std::vector<ShellWindowEntry> same{{21, L"C:\\same\\x", window}};
        forms.Publish(same);
        Check(Wait([&] { return acked(same) && Cookies() == 2; }, &forms),
              "reveal: byte-identical forms register once");
        const std::vector<ShellWindowEntry> back{{21, L"C:\\known\\Desktop", window}};
        forms.Publish(back);
        Check(Wait([&] { return acked(back) && Cookies() == 4; }, &forms),
              "reveal: navigating back below a known folder registers the second form again");
        // Another pane opening re-applies the set: the settled pane with both
        // forms must not navigate again (only the new pane's one form does).
        const int before_other = state.navigates;
        const std::vector<ShellWindowEntry> two{back.front(), {22, L"C:\\other", window}};
        forms.Publish(two);
        Check(Wait([&] { return acked(two) && Cookies() == 6; }, &forms) && state.navigates == before_other + 1,
              "reveal: a pane settled under both forms is left alone when another pane registers");
        forms.Publish({});
        Check(Wait([] { return Cookies() == 0; }, &forms), "reveal: closing the pane releases both forms' cookies");
        forms.Stop();
    }
    Check(!state.wrong_thread, "all injected COM calls occur off the UI thread");
    return failures ? 1 : 0;
}
