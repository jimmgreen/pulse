#include "collected_defaults_resolver.h"
#include <shlobj.h>
#include <propsys.h>
#include <propkey.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <atomic>
#include <memory>
#include <algorithm>

namespace pulse::shell_bridge {
namespace {
using Microsoft::WRL::ComPtr;
std::atomic<bool> busy{false};
#ifdef PULSE_DEFAULTS_RESOLVER_TESTING
std::atomic<DWORD> test_delay{0};
#endif
constexpr UINT kMaxDefaultProperties = 128;
class BindData final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileSystemBindData> {
public:
    explicit BindData(const std::wstring& leaf) {
        data_.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        wcscpy_s(data_.cFileName, leaf.c_str());
    }
    HRESULT STDMETHODCALLTYPE SetFindData(const WIN32_FIND_DATAW* data) override {
        if (!data) return E_POINTER;
        data_ = *data; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFindData(WIN32_FIND_DATAW* data) override {
        if (!data) return E_POINTER;
        *data = data_; return S_OK;
    }
private:
    WIN32_FIND_DATAW data_{};
};
struct Job {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HMODULE module = nullptr;
    std::wstring extension;
    std::vector<PROPERTYKEY> keys;
    HRESULT result = E_PENDING;
    ~Job() { if (ready) CloseHandle(ready); }
};
HRESULT Discover(Job& job) {
    wchar_t directory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return E_FAIL;
    const std::wstring leaf = L"__pulse_property_schema__" + job.extension;
    const std::wstring path = std::wstring(directory) + L"\\" + leaf;
    auto data = Microsoft::WRL::Make<BindData>(leaf);
    if (!data) return E_OUTOFMEMORY;
    ComPtr<IBindCtx> context;
    HRESULT hr = CreateBindCtx(0, &context);
    if (FAILED(hr)) return hr;
    hr = context->RegisterObjectParam(const_cast<wchar_t*>(STR_FILE_SYS_BIND_DATA), data.Get());
    if (FAILED(hr)) return hr;
    ComPtr<IShellItem2> item;
    hr = SHCreateItemFromParsingName(path.c_str(), context.Get(), IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    ComPtr<IPropertyDescriptionList> list;
    hr = item->GetPropertyDescriptionList(PKEY_PropList_PreviewDetails, IID_PPV_ARGS(&list));
    if (FAILED(hr)) return hr;
    UINT count = 0;
    hr = list->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (count > kMaxDefaultProperties) return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IPropertyDescription> description;
        hr = list->GetAt(index, IID_PPV_ARGS(&description));
        if (FAILED(hr)) return hr;
        PROPDESC_TYPE_FLAGS flags{};
        hr = description->GetTypeFlags(PDTF_MASK_ALL, &flags);
        if (FAILED(hr)) return hr;
        if ((flags & (PDTF_ISINNATE | PDTF_ISGROUP)) != 0) continue;
        PROPERTYKEY key{};
        hr = description->GetPropertyKey(&key);
        if (FAILED(hr)) return hr;
        if (std::none_of(job.keys.begin(), job.keys.end(), [&](const auto& old) { return IsEqualPropertyKey(old, key); }))
            job.keys.push_back(key);
    }
    return S_OK;
}
DWORD WINAPI Worker(void* raw) {
    auto job = *static_cast<std::shared_ptr<Job>*>(raw);
    delete static_cast<std::shared_ptr<Job>*>(raw);
    #ifdef PULSE_DEFAULTS_RESOLVER_TESTING
    Sleep(test_delay.load());
#endif
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try { job->result = SUCCEEDED(initialized) ? Discover(*job) : initialized; }
    catch (...) { job->result = E_OUTOFMEMORY; }
    if (SUCCEEDED(initialized)) CoUninitialize();
    HMODULE module = job->module;
    busy.store(false);
    SetEvent(job->ready);
    job.reset();
    // A timeout may outlive the wrapper; keep this DLL loaded until all worker code exits.
    FreeLibraryAndExitThread(module, 0);
}
}
#ifdef PULSE_DEFAULTS_RESOLVER_TESTING
void SetCollectedDefaultsTestDelay(DWORD milliseconds) noexcept { test_delay.store(milliseconds); }
#endif
HRESULT ResolveCollectedDefaults(const std::wstring& extension,
    std::vector<PROPERTYKEY>& keys, DWORD timeout_ms) noexcept try {
    if (extension.size() < 2 || extension.size() > 32 || extension.front() != L'.' || timeout_ms > 5000 ||
        extension.find_first_not_of(L".abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 1) != extension.npos ||
        extension.find(L'.', 1) != extension.npos) return E_INVALIDARG;
    auto job = std::make_shared<Job>();
    if (!job->ready) return HRESULT_FROM_WIN32(GetLastError());
    job->extension = extension;
    auto holder = std::make_unique<std::shared_ptr<Job>>(job);
    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&Worker), &job->module)) {
        busy.store(false); return HRESULT_FROM_WIN32(GetLastError());
    }
    HANDLE thread = CreateThread(nullptr, 0, Worker, holder.get(), 0, nullptr);
    if (!thread) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        FreeLibrary(job->module); busy.store(false); return hr;
    }
    holder.release(); CloseHandle(thread);
    DWORD index = 0;
    HRESULT hr = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
        timeout_ms, 1, &job->ready, &index);
    if (FAILED(hr)) return hr;
    if (FAILED(job->result)) return job->result;
    keys = std::move(job->keys);
    return S_OK;
} catch (...) { return E_OUTOFMEMORY; }
}
