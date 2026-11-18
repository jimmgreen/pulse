#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <memory>
#include <mutex>

namespace pulse::shell_bridge::detail {
// Weak owner control, shared by proxy and wrapper. The final wrapper Release
// must clear dialog under mutex before destroying anything. Callback acquisition
// uses the same mutex when AddRef'ing dialog; no strong owner cycle is created.
struct FileDialogEventOwner {
    std::mutex mutex;
    IFileDialog* dialog = nullptr;
    DWORD thread = GetCurrentThreadId();
};
HRESULT CreateFileDialogEventProxy(std::shared_ptr<FileDialogEventOwner> owner,
                                  IFileDialogEvents* sink, IFileDialogEvents** output) noexcept;
}
