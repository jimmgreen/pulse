#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cstdio>
#include <initializer_list>

// Native, no Show, no wrapper/runtime dependency and no persistent settings.
// The observations intentionally do not assert an undocumented normalization.
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::printf("SETUP CoInitializeEx=0x%08lX pid=%lu\n", static_cast<unsigned long>(initialized), GetCurrentProcessId());
    if (FAILED(initialized)) return 2;
    int result = 0;
    {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        const HRESULT created = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        std::printf("SETUP native_CoCreateInstance=0x%08lX object=%p\n", static_cast<unsigned long>(created), static_cast<void*>(dialog.Get()));
        if (FAILED(created) || !dialog) result = 2;
        else {
            const COMDLG_FILTERSPEC filter{L"All files", L"*.*"};
            const HRESULT types = dialog->SetFileTypes(1, &filter);
            std::printf("SETUP SetFileTypes(count=1,pattern=*.*)=0x%08lX\n", static_cast<unsigned long>(types));
            if (FAILED(types)) result = 2;
            else {
                for (const UINT requested : {0u, 1u}) {
                    const HRESULT set = dialog->SetFileTypeIndex(requested);
                    UINT observed = 0xFFFFFFFFu;
                    const HRESULT get = dialog->GetFileTypeIndex(&observed);
                    std::printf("OBSERVATION requested=%u SetFileTypeIndex=0x%08lX GetFileTypeIndex=0x%08lX observed=%u\n",
                        requested, static_cast<unsigned long>(set), static_cast<unsigned long>(get), observed);
                }
                std::printf("SAMPLING_COMPLETE observations=2 show_calls=0; no normalization assertion\n");
            }
        }
    }
    CoUninitialize();
    if (std::fflush(stdout) != 0) return 2;
    return result;
}
