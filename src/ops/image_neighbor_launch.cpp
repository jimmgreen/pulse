#include "image_neighbor_launch.h"
#include "photos_protocol_launch.h"
#include "../common/preview_extensions.h"
#include <shobjidl_core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.System.h>
#include <thread>

namespace pulse::ops {
namespace {
ImageNeighborLaunchResult Run(const std::wstring& path, HWND owner, bool launch) noexcept {
    ImageNeighborLaunchResult result;
    try {
        // C++/WinRT blocking waits require MTA. The caller's Shell STA remains
        // unchanged; all WinRT objects are created and released on this worker.
        std::thread worker([&] {
            bool initialized = false;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                initialized = true;
                using namespace winrt::Windows::Storage;
                using namespace winrt::Windows::Storage::Search;
                result.stage = ImageNeighborLaunchStage::File;
                const auto file = StorageFile::GetFileFromPathAsync(path).get();
                const auto folder = file.GetParentAsync().get();
                if (!folder) winrt::throw_hresult(E_INVALIDARG);
                result.stage = ImageNeighborLaunchStage::Query;
                const QueryOptions query_options(CommonFileQuery::OrderByName, {});
                for (const auto extension : preview::formats::kImage)
                    query_options.FileTypeFilter().Append(winrt::hstring(extension));
                query_options.FolderDepth(FolderDepth::Shallow);
                const auto query = folder.CreateFileQueryWithOptions(query_options);
                // Pass the lazy query itself: never enumerate neighboring files.
                const winrt::Windows::System::LauncherOptions options;
                options.NeighboringFilesQuery(query);
                result.stage = ImageNeighborLaunchStage::Owner;
                if (!owner || !IsWindow(owner)) winrt::throw_hresult(E_INVALIDARG);
                winrt::check_hresult(options.as<IInitializeWithWindow>()->Initialize(owner));
                result.query_prepared = true;
                result.shallow = query_options.FolderDepth() == FolderDepth::Shallow;
                const auto order = query_options.SortOrder();
                result.ordered_by_name = order.Size() != 0 && order.GetAt(0).PropertyName == L"System.ItemNameDisplay" && order.GetAt(0).AscendingOrder;
                result.filter_count = query_options.FileTypeFilter().Size();
                if (launch) {
                    result.stage = ImageNeighborLaunchStage::Launch;
                    result.launched = winrt::Windows::System::Launcher::LaunchFileAsync(file, options).get();
                    result.error = result.launched ? S_OK : E_FAIL;
                } else result.error = S_OK;
                result.stage = ImageNeighborLaunchStage::Complete;
            } catch (const winrt::hresult_error& error) { result.error = error.code(); }
              catch (...) { result.error = E_FAIL; }
            if (initialized) winrt::uninit_apartment();
        });
        worker.join();
    } catch (...) { result.error = E_OUTOFMEMORY; }
    return result;
}
}
ImageNeighborLaunchResult LaunchImageWithNeighbors(const std::wstring& path, HWND owner) noexcept {
    const auto photos = LaunchPhotosProtocolIfDefault(path, owner);
    if (photos.attempted && (photos.launched || photos.error == HRESULT_FROM_WIN32(ERROR_CANCELLED))) {
        ImageNeighborLaunchResult result;
        result.launched = photos.launched;
        result.error = photos.error;
        result.stage = ImageNeighborLaunchStage::PhotosProtocol;
        result.photos_protocol_attempted = true;
        result.photos_protocol_error = photos.error;
        return result;
    }
    auto result = Run(path, owner, true);
    result.photos_protocol_attempted = photos.attempted;
    result.photos_protocol_error = photos.error;
    return result;
}
ImageNeighborLaunchResult PrepareImageNeighborLaunch(const std::wstring& path, HWND owner) noexcept {
    return Run(path, owner, false);
}
}
