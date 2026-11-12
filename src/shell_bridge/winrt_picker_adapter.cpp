#include "winrt_picker_adapter.h"
#include <memory>
#include <filesystem>

namespace pulse::shell_bridge::winrt_picker {
namespace {
namespace wire = ipc::picker;
using Paths = winrt::Windows::Foundation::Collections::IVectorView<winrt::hstring>;
struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Event() { if (handle) CloseHandle(handle); }
};
IAsyncOperation<Paths> Select(wire::Request request, HWND owner, Services services) {
    winrt::check_hresult(DesktopEligibility(owner));
    if (!services.run) winrt::throw_hresult(E_INVALIDARG);
    const DWORD owner_thread = GetWindowThreadProcessId(owner, nullptr);
    auto cancel = co_await winrt::get_cancellation_token();
    cancel.enable_propagation();
    const auto event = std::make_shared<Event>();
    if (!event->handle) winrt::throw_last_error();
    cancel.callback([event] { SetEvent(event->handle); });
    wire::Message message;
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    static_assert(sizeof(id) == sizeof(message.request_id));
    memcpy(message.request_id.data(), &id, sizeof(id));
    message.body = std::move(request);
    std::vector<uint8_t> encoded;
    if (wire::Encode(message, encoded) != wire::CodecError::None) winrt::throw_hresult(E_INVALIDARG);
    co_await winrt::resume_background();
    if (cancel()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    const auto reply = services.run(message, owner, event->handle);
    if (cancel()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    winrt::check_hresult(DesktopEligibility(owner));
    if (GetWindowThreadProcessId(owner, nullptr) != owner_thread) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
    if (!wire::ValidateResponse(message, reply)) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    const auto& response = std::get<wire::Response>(reply.body);
    auto paths = winrt::single_threaded_vector<winrt::hstring>();
    if (response.status == wire::Status::Cancelled) co_return paths.GetView();
    if (response.status != wire::Status::Accepted) {
        const HRESULT error = response.error_source == wire::ErrorSource::HResult ? static_cast<HRESULT>(response.error_code) :
            response.error_source == wire::ErrorSource::Win32 ? HRESULT_FROM_WIN32(response.error_code) : E_FAIL;
        winrt::throw_hresult(FAILED(error) ? error : E_FAIL);
    }
    for (const auto& path : response.paths) paths.Append(winrt::hstring(reinterpret_cast<const wchar_t*>(path.data()), static_cast<uint32_t>(path.size())));
    co_return paths.GetView();
}
void Require(const wire::Request& request, wire::Operation operation, bool multiple = false) {
    if (request.operation != operation || ((request.options & wire::AllowMultiSelect) != 0) != multiple)
        winrt::throw_hresult(E_INVALIDARG);
}
}
HRESULT DesktopEligibility(HWND owner) noexcept {
    DWORD pid = 0;
    if (!owner || !IsWindow(owner) || !GetWindowThreadProcessId(owner, &pid) || pid != GetCurrentProcessId())
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return HRESULT_FROM_WIN32(GetLastError());
    DWORD appcontainer = 0, size = 0;
    const BOOL read = GetTokenInformation(token, TokenIsAppContainer, &appcontainer, sizeof(appcontainer), &size);
    const DWORD error = read ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (error) return HRESULT_FROM_WIN32(error);
    return appcontainer ? E_ACCESSDENIED : S_OK;
}
IAsyncOperation<File> PickFileAsync(wire::Request request, HWND owner, Services services) {
    Require(request, wire::Operation::OpenFiles);
    auto cancel = co_await winrt::get_cancellation_token(); cancel.enable_propagation();
    const auto paths = co_await Select(std::move(request), owner, std::move(services));
    if (paths.Size() == 0) co_return nullptr;
    co_return co_await File::GetFileFromPathAsync(paths.GetAt(0));
}
IAsyncOperation<Files> PickFilesAsync(wire::Request request, HWND owner, Services services) {
    Require(request, wire::Operation::OpenFiles, true);
    auto cancel = co_await winrt::get_cancellation_token(); cancel.enable_propagation();
    const auto paths = co_await Select(std::move(request), owner, std::move(services));
    auto files = winrt::single_threaded_vector<File>();
    for (const auto& path : paths) files.Append(co_await File::GetFileFromPathAsync(path));
    co_return files.GetView();
}
IAsyncOperation<Folder> PickFolderAsync(wire::Request request, HWND owner, Services services) {
    Require(request, wire::Operation::PickFolder);
    auto cancel = co_await winrt::get_cancellation_token(); cancel.enable_propagation();
    const auto paths = co_await Select(std::move(request), owner, std::move(services));
    if (paths.Size() == 0) co_return nullptr;
    co_return co_await Folder::GetFolderFromPathAsync(paths.GetAt(0));
}
IAsyncOperation<File> PickSaveAsync(wire::Request request, HWND owner, Services services) {
    Require(request, wire::Operation::SaveFile);
    auto cancel = co_await winrt::get_cancellation_token(); cancel.enable_propagation();
    const auto paths = co_await Select(std::move(request), owner, std::move(services));
    if (paths.Size() == 0) co_return nullptr;
    const std::filesystem::path path(paths.GetAt(0).c_str());
    const auto folder = co_await Folder::GetFolderFromPathAsync(path.parent_path().wstring());
    if (cancel()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    // Selection does not truncate an existing file; the host controls its writes.
    co_return co_await folder.CreateFileAsync(path.filename().wstring(), winrt::Windows::Storage::CreationCollisionOption::OpenIfExists);
}
}
