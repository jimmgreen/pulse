#include <windows.h>
#include <objbase.h>
#include <rpc.h>
#include <rpcasync.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <wrl/client.h>

namespace {
using Microsoft::WRL::ComPtr;
void Observe(const char* stage) {
    ComPtr<IServerSecurity> security;
    const HRESULT context = CoGetCallContext(IID_PPV_ARGS(&security));
    DWORD service = 0, level = 0, capabilities = 0;
    const HRESULT blanket = CoQueryClientBlanket(&service, nullptr, nullptr, &level, nullptr, nullptr, &capabilities);
    std::printf("OBSERVE stage=%s pid=%lu tid=%lu context_hr=0x%08lX context_present=%u blanket_hr=0x%08lX authn_svc=%lu authn_level=%lu capabilities=%lu impersonating=%u\n",
        stage, GetCurrentProcessId(), GetCurrentThreadId(), static_cast<unsigned long>(context), security ? 1u : 0u,
        static_cast<unsigned long>(blanket), service, level, capabilities, security && security->IsImpersonating() ? 1u : 0u);
    std::fflush(stdout);
    RPC_CALL_ATTRIBUTES_V2_W attributes{};
    attributes.Version = 2; attributes.Flags = RPC_QUERY_CLIENT_PID;
    const RPC_STATUS rpc = RpcServerInqCallAttributesW(nullptr, &attributes);
    std::printf("RPC_OBSERVE stage=%s rpc_status=%ld client_pid_known=%u client_pid=%llu protocol=%lu\n", stage, rpc,
        rpc == RPC_S_OK ? 1u : 0u, rpc == RPC_S_OK ? static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(attributes.ClientPID)) : 0,
        rpc == RPC_S_OK ? attributes.ProtocolSequence : 0);
    std::fflush(stdout);
}
class Factory final : public IClassFactory {
    std::atomic<ULONG> references_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *output = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown*, REFIID, void** output) override {
        if (output) *output = nullptr;
        Observe("incoming_cross_apartment");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { std::printf("[FAIL] initialize hr=0x%08lX\n", static_cast<unsigned long>(initialized)); return 1; }
    Observe("ordinary_sta");
    ComPtr<IClassFactory> factory; factory.Attach(new Factory());
    IStream* stream = nullptr;
    const HRESULT marshaled = CoMarshalInterThreadInterfaceInStream(IID_IClassFactory, factory.Get(), &stream);
    if (FAILED(marshaled)) { CoUninitialize(); return 2; }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) { stream->Release(); CoUninitialize(); return 2; }
    std::atomic<HRESULT> response{E_PENDING};
    std::thread client([&] {
        const HRESULT ready = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IClassFactory> proxy;
        const HRESULT loaded = SUCCEEDED(ready) ? CoGetInterfaceAndReleaseStream(stream, IID_PPV_ARGS(&proxy)) : ready;
        if (FAILED(ready)) stream->Release();
        void* result = nullptr;
        response = SUCCEEDED(loaded) ? proxy->CreateInstance(nullptr, IID_IUnknown, &result) : loaded;
        proxy.Reset();
        if (SUCCEEDED(ready)) CoUninitialize();
        SetEvent(done);
    });
    const ULONGLONG started = GetTickCount64();
    while (WaitForSingleObject(done, 0) != WAIT_OBJECT_0) {
        if (GetTickCount64() - started >= 10000) {
            std::printf("[FAIL] COM context probe watchdog=10000\n"); std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 124);
        }
        MsgWaitForMultipleObjectsEx(1, &done, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    client.join(); CloseHandle(done);
    const bool okay = response == E_NOTIMPL;
    factory.Reset(); CoUninitialize();
    std::printf("[%s] unregistered cross-apartment COM call completed; context is evidence, not a broker identity\n", okay ? "PASS" : "FAIL");
    return okay ? 0 : 1;
}
