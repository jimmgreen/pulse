#include "../shell_host/isolated_start_pin.h"
#include <cstdio>
#include <set>
#include <thread>
#include <tlhelp32.h>
namespace {
std::set<DWORD> Children() {
    std::set<DWORD> children;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return children;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
        do {
            if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                _wcsicmp(entry.szExeFile, L"pulse_shell.exe") == 0)
                children.insert(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return children;
}
class FakeMenu final : public IContextMenu {
  public:
    unsigned calls = 0;
    UINT offset = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO *info) override {
        ++calls;
        offset = LOWORD(reinterpret_cast<ULONG_PTR>(info->lpVerb));
        return E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR, UINT, UINT *, LPSTR, UINT) override {
        return E_NOTIMPL;
    }
};
bool VerifyFakeInvoke() {
    FakeMenu menu;
    pulse::shell::CtxHandlerSlot slot;
    slot.menu = &menu;
    slot.id_first = 100;
    slot.id_last = 200;
    pulse::shell::CtxItemOut item;
    item.id = 107;
    std::vector<pulse::shell::CtxItemOut> items{item};
    using pulse::shell::InvokeIsolatedStartPinItem;
    if (InvokeIsolatedStartPinItem(slot, items, nullptr, 107) != E_ACCESSDENIED || menu.offset != 7 ||
        menu.calls != 1)
        return false;
    if (InvokeIsolatedStartPinItem(slot, items, nullptr, 108) != E_INVALIDARG ||
        InvokeIsolatedStartPinItem(slot, items, nullptr, 99) != E_INVALIDARG)
        return false;
    items[0].enabled = false;
    if (InvokeIsolatedStartPinItem(slot, items, nullptr, 107) != E_INVALIDARG)
        return false;
    items[0].enabled = true;
    items[0].has_children = true;
    return InvokeIsolatedStartPinItem(slot, items, nullptr, 107) == E_INVALIDARG && menu.calls == 1;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"Usage: isolated_start_pin_test <existing folder>\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool fake_pass = VerifyFakeInvoke();
    std::printf("[%s] fake invoke offset, HRESULT and invalid IDs\n", fake_pass ? "PASS" : "FAIL");
    int failed = fake_pass ? 0 : 1;
    pulse::shell::IsolatedStartPinSession::Prewarm();
    std::set<DWORD> previous = Children();
    bool prewarmed = previous.size() == 1;
    std::printf("[%s] prewarm launches one idle child\n", prewarmed ? "PASS" : "FAIL");
    if (!prewarmed)
        ++failed;
    for (int i = 0; i < 3; ++i) {
        pulse::shell::IsolatedStartPinSession session;
        std::vector<pulse::shell::CtxItemOut> items;
        HRESULT hr = session.Query({argv[1]}, nullptr, 1, 0x7fff, CMF_NORMAL, nullptr, items);
        bool has_pin = false;
        for (const auto& item : items)
            has_pin |= _wcsicmp(item.verb.c_str(), L"pintostartscreen") == 0 ||
                       _wcsicmp(item.verb.c_str(), L"unpinfromstartscreen") == 0;
        std::printf("[%s] isolated query %d hr=0x%08lx rows=%zu\n", SUCCEEDED(hr) && has_pin ? "PASS" : "FAIL", i,
                    static_cast<unsigned long>(hr), items.size());
        if (FAILED(hr) || !has_pin)
            ++failed;
        auto children = Children();
        bool reused = children.size() == 1 && (children == previous);
        std::printf("[%s] single idle child reused\n", reused ? "PASS" : "FAIL");
        if (!reused)
            ++failed;
        previous = std::move(children);
        for (const auto &item : items)
            if (item.id && (item.id > 0x7fff || (item.icon && !item.icon->Valid())))
                ++failed;
        if (i == 0 && SUCCEEDED(hr)) {
            const bool rejected = session.Invoke(UINT_MAX) == E_INVALIDARG;
            std::printf("[%s] child protocol rejects invalid invocation without pinning\n",
                        rejected ? "PASS" : "FAIL");
            if (!rejected) ++failed;
        }
    }
    HANDLE cancel = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!cancel)
        ++failed;
    else {
        pulse::shell::IsolatedStartPinSession session;
        std::vector<pulse::shell::CtxItemOut> items;
        HRESULT hr = session.Query({argv[1]}, nullptr, 1, 0x7fff, CMF_NORMAL, cancel, items);
        bool pass = hr == HRESULT_FROM_WIN32(ERROR_CANCELLED);
        std::printf("[%s] pre-cancel\n", pass ? "PASS" : "FAIL");
        if (!pass)
            ++failed;
        CloseHandle(cancel);
    }
    {
        pulse::shell::IsolatedStartPinSession first, second;
        std::vector<pulse::shell::CtxItemOut> items;
        HRESULT one = first.Query({argv[1]}, nullptr, 1, 0x7fff, CMF_NORMAL, nullptr, items);
        HRESULT two = second.Query({argv[1]}, nullptr, 1, 0x7fff, CMF_NORMAL, nullptr, items);
        bool independent = SUCCEEDED(one) && SUCCEEDED(two) && Children().size() == 2;
        std::printf("[%s] simultaneous sessions own separate children\n", independent ? "PASS" : "FAIL");
        if (!independent)
            ++failed;
    }
    {
        const auto pooled = Children();
        bool one_idle = pooled.size() == 1;
        std::printf("[%s] surplus child retired\n", one_idle ? "PASS" : "FAIL");
        if (!one_idle)
            ++failed;
        // Kill only our own isolated test child to simulate a broken cached transport.
        for (DWORD pid : pooled) {
            HANDLE child = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            if (!child) {
                ++failed;
                continue;
            }
            TerminateProcess(child, 99);
            WaitForSingleObject(child, 5000);
            CloseHandle(child);
        }
        pulse::shell::IsolatedStartPinSession recovered;
        std::vector<pulse::shell::CtxItemOut> items;
        HRESULT hr = recovered.Query({argv[1]}, nullptr, 1, 0x7fff, CMF_NORMAL, nullptr, items);
        bool replaced = SUCCEEDED(hr) && Children().size() == 1 && Children() != pooled;
        std::printf("[%s] dead pooled child replaced\n", replaced ? "PASS" : "FAIL");
        if (!replaced)
            ++failed;
    }
    {
        const auto pooled = Children();
        std::vector<HANDLE> paused;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (pooled.size() == 1 && snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (Thread32First(snapshot, &entry))
                do {
                    if (entry.th32OwnerProcessID != *pooled.begin())
                        continue;
                    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                    if (!thread)
                        continue;
                    if (SuspendThread(thread) == static_cast<DWORD>(-1))
                        CloseHandle(thread);
                    else
                        paused.push_back(thread);
                } while (Thread32Next(snapshot, &entry));
        }
        if (snapshot != INVALID_HANDLE_VALUE)
            CloseHandle(snapshot);
        HANDLE inflight_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool cancelled = false;
        if (inflight_cancel && !paused.empty()) {
            std::thread signal([inflight_cancel] {
                Sleep(100);
                SetEvent(inflight_cancel);
            });
            pulse::shell::IsolatedStartPinSession session;
            std::vector<pulse::shell::CtxItemOut> items;
            HRESULT hr = session.Query({argv[1]}, nullptr, 1, 0x7fff, CMF_NORMAL, inflight_cancel, items);
            signal.join();
            cancelled = hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) && Children().empty();
        }
        for (HANDLE thread : paused) {
            ResumeThread(thread);
            CloseHandle(thread);
        }
        if (inflight_cancel)
            CloseHandle(inflight_cancel);
        std::printf("[%s] in-flight cancel destroys paused test child\n", cancelled ? "PASS" : "FAIL");
        if (!cancelled)
            ++failed;
        pulse::shell::IsolatedStartPinSession recovered;
        std::vector<pulse::shell::CtxItemOut> items;
        HRESULT hr = recovered.Query({argv[1]}, nullptr, 1, 0x7fff, CMF_NORMAL, nullptr, items);
        bool pass = SUCCEEDED(hr) && Children().size() == 1 && Children() != pooled;
        std::printf("[%s] cancelled child never reused\n", pass ? "PASS" : "FAIL");
        if (!pass)
            ++failed;
    }
    CoUninitialize();
    return failed ? 1 : 0;
}
