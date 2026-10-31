#include "protocol.h"
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <new>

namespace pulse::start_pin {
namespace {
using Microsoft::WRL::ComPtr;
const CLSID kClsid = {0xb3c9ed45,0x7a51,0x48a2,{0x9d,0xf1,0xd4,0x6b,0xa1,0x87,0xa3,0x12}};
std::atomic<long> objects{0};
struct NativeMenu {
    ComPtr<IContextMenu> menu;
    HMENU popup = nullptr;
    UINT id = 0;
    bool pinned = false;
    ~NativeMenu() { if (popup) DestroyMenu(popup); }
    HRESULT Load(const std::wstring& path) {
        PIDLIST_ABSOLUTE full = nullptr;
        HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &full, 0, nullptr);
        ComPtr<IShellFolder> parent; PCUITEMID_CHILD child = nullptr;
        if (SUCCEEDED(hr)) hr = SHBindToParent(full, IID_PPV_ARGS(&parent), &child);
        ComPtr<IDataObject> data;
        if (SUCCEEDED(hr)) hr = parent->GetUIObjectOf(nullptr, 1, &child, IID_IDataObject, nullptr,
                                                     reinterpret_cast<void**>(data.GetAddressOf()));
        // The native Start handler, isolated from unrelated third-party menu handlers.
        CLSID clsid{}; CLSIDFromString(L"{470c0ebd-5d73-4d58-9ced-e91e22e23282}", &clsid);
        ComPtr<IShellExtInit> init;
        if (SUCCEEDED(hr)) hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&init));
        if (full) ILRemoveLastID(full);
        if (SUCCEEDED(hr)) hr = init->Initialize(full, data.Get(), nullptr);
        CoTaskMemFree(full);
        if (SUCCEEDED(hr)) hr = init.As(&menu);
        if (FAILED(hr)) return hr;
        popup = CreatePopupMenu(); if (!popup) return HRESULT_FROM_WIN32(GetLastError());
        hr = menu->QueryContextMenu(popup, 0, 1, 0x7fff, CMF_NORMAL);
        if (FAILED(hr)) return hr;
        for (int i = 0; i < GetMenuItemCount(popup); ++i) {
            const UINT candidate = GetMenuItemID(popup, i);
            if (!candidate || candidate == UINT(-1)) continue;
            wchar_t verb[128]{};
            if (FAILED(menu->GetCommandString(candidate - 1, GCS_VERBW, nullptr,
                                              reinterpret_cast<char*>(verb), 128))) continue;
            if (!_wcsicmp(verb, L"PinToStartScreen") || !_wcsicmp(verb, L"UnpinFromStartScreen")) {
                id = candidate; pinned = !_wcsicmp(verb, L"UnpinFromStartScreen"); return S_OK;
            }
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
};
HRESULT SetPinned(const std::wstring& path, bool desired) {
    NativeMenu menu; HRESULT hr = menu.Load(path);
    if (FAILED(hr) || menu.pinned == desired) return hr;
    CMINVOKECOMMANDINFOEX info{}; info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_NOASYNC;
    info.lpVerb = MAKEINTRESOURCEA(menu.id - 1); info.lpVerbW = MAKEINTRESOURCEW(menu.id - 1);
    info.nShow = SW_SHOWNORMAL;
    hr = menu.menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
    if (FAILED(hr)) return hr;
    const auto deadline = GetTickCount64() + 10000;
    do {
        Sleep(100);
        NativeMenu check; hr = check.Load(path);
        if (SUCCEEDED(hr) && check.pinned == desired) return S_OK;
    } while (GetTickCount64() < deadline);
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}
struct Work { HMODULE module = nullptr; std::wstring nonce; };
DWORD WINAPI Run(void* parameter) {
    std::unique_ptr<Work> work(static_cast<Work*>(parameter));
    const HMODULE module = work->module;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(initialized)) {
        Handle pipe(CreateFileW((std::wstring(kPipePrefix) + work->nonce).c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        wchar_t module_path[32768]{};
        const auto length = GetModuleFileNameW(module, module_path, 32768);
        std::wstring expected(module_path, length);
        const auto slash = expected.find_last_of(L'\\');
        if (slash != std::wstring::npos) expected.resize(slash + 1); else expected.clear();
        expected += L"pulse_shell.exe";
        if (pipe && PeerIs(pipe.value, false, expected)) {
            const auto deadline = GetTickCount64() + kTimeout;
            Request request{}; HRESULT hr = E_INVALIDARG;
            if (Io(pipe.value, &request, sizeof(request), false, deadline) && request.version == 1 &&
                request.pin <= 1 && request.characters > 0 && request.characters <= 32767) {
                std::wstring path(request.characters, L'\0');
                if (Io(pipe.value, path.data(), request.characters * sizeof(wchar_t), false, deadline) &&
                    path.find(L'\0') == std::wstring::npos) hr = SetPinned(path, request.pin != 0);
            }
            Io(pipe.value, &hr, sizeof(hr), true, deadline);
        }
        CoUninitialize();
    }
    work.reset();
    --objects;
    FreeLibraryAndExitThread(module, 0);
}
class Command final : public IExecuteCommand {
    std::atomic<ULONG> references{1};
    std::wstring nonce;
public:
    Command() { ++objects; }
    ~Command() { --objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IExecuteCommand) return E_NOINTERFACE;
        *output = static_cast<IExecuteCommand*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE SetKeyState(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetParameters(LPCWSTR value) override {
        if (!value || wcsnlen_s(value, 40) != 38 || !ValidNonce(value)) return E_INVALIDARG;
        nonce = value; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPosition(POINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetShowWindow(int) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetNoShowUI(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetDirectory(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Execute() override {
        if (!ValidNonce(nonce)) return E_INVALIDARG;
        wchar_t windows[32768]{}; GetWindowsDirectoryW(windows, 32768);
        if (_wcsicmp(ImagePath(GetCurrentProcess()).c_str(), (std::wstring(windows) + L"\\explorer.exe").c_str()))
            return E_ACCESSDENIED;
        auto work = std::make_unique<Work>(); work->nonce = nonce;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&Run), &work->module)) return HRESULT_FROM_WIN32(GetLastError());
        ++objects;
        HANDLE thread = CreateThread(nullptr, 0, Run, work.get(), 0, nullptr);
        if (!thread) { --objects; FreeLibrary(work->module); return HRESULT_FROM_WIN32(GetLastError()); }
        work.release(); CloseHandle(thread); return S_OK;
    }
};
class Factory final : public IClassFactory {
    std::atomic<ULONG> references{1};
public:
    Factory() { ++objects; }
    ~Factory() { --objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *output = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr; if (outer) return CLASS_E_NOAGGREGATION;
        auto command = new (std::nothrow) Command; if (!command) return E_OUTOFMEMORY;
        const auto hr = command->QueryInterface(iid, output); command->Release(); return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { if (lock) ++objects; else --objects; return S_OK; }
};
}
}
STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (clsid != pulse::start_pin::kClsid) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = new (std::nothrow) pulse::start_pin::Factory;
    if (!factory) return E_OUTOFMEMORY;
    const auto hr = factory->QueryInterface(iid, output); factory->Release(); return hr;
}
STDAPI DllCanUnloadNow() { return pulse::start_pin::objects == 0 ? S_OK : S_FALSE; }
