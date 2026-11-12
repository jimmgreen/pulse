#pragma once
#include "../ipc/picker_protocol.h"
#include <windows.h>
#include <objbase.h>
#include <functional>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>

namespace pulse::shell_bridge::winrt_picker {
// Desktop-only path-backed adapter. This does not mint a brokered storage grant.
// The transport runs on a background MTA and must own its modal owner guard.
struct Services {
    std::function<ipc::picker::Message(const ipc::picker::Message&, HWND, HANDLE)> run;
};
using File = winrt::Windows::Storage::StorageFile;
using Folder = winrt::Windows::Storage::StorageFolder;
using Files = winrt::Windows::Foundation::Collections::IVectorView<File>;
using winrt::Windows::Foundation::IAsyncOperation;
HRESULT DesktopEligibility(HWND owner) noexcept;
IAsyncOperation<File> PickFileAsync(ipc::picker::Request request, HWND owner, Services services);
IAsyncOperation<Files> PickFilesAsync(ipc::picker::Request request, HWND owner, Services services);
IAsyncOperation<Folder> PickFolderAsync(ipc::picker::Request request, HWND owner, Services services);
IAsyncOperation<File> PickSaveAsync(ipc::picker::Request request, HWND owner, Services services);
}
