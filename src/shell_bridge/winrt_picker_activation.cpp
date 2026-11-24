#include "winrt_picker_activation.h"
#include <shobjidl.h>
#include <winstring.h>
#include <shlobj.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace pulse::shell_bridge::winrt_picker {
namespace {
namespace pick = winrt::Windows::Storage::Pickers;
namespace foundation = winrt::Windows::Foundation;
namespace wire = ipc::picker;
using winrt::hstring;
std::u16string Text(const hstring& value) { return {reinterpret_cast<const char16_t*>(value.c_str()), value.size()}; }
uint64_t HashText(std::wstring_view text) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : text) {
        const uint16_t unit = static_cast<uint16_t>(ch);
        hash = (hash ^ (unit & 0xffu)) * 1099511628211ull;
        hash = (hash ^ (unit >> 8)) * 1099511628211ull;
    }
    return hash;
}
std::wstring WinRtSettingsValueName(std::wstring_view settings_id) {
    wchar_t module_path[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, module_path, ARRAYSIZE(module_path));
    std::wstring_view exe(module_path, len && len < ARRAYSIZE(module_path) ? len : 0);
    const auto slash = exe.find_last_of(L"\\/");
    if (slash != exe.npos) exe.remove_prefix(slash + 1);
    std::wstring lowered(exe);
    for (auto& ch : lowered) if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    wchar_t name[64]{};
    std::swprintf(name, ARRAYSIZE(name), L"%016llx-%016llx",
        static_cast<unsigned long long>(HashText(lowered)),
        static_cast<unsigned long long>(HashText(settings_id)));
    return name;
}
std::optional<std::u16string> ReadRememberedDirectory(std::wstring_view settings_id) noexcept {
    if (settings_id.empty()) return std::nullopt;
    try {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Pulse\\WinRtPickerState", 0, KEY_READ, &key) != ERROR_SUCCESS)
            return std::nullopt;
        struct Close { HKEY h; ~Close() { RegCloseKey(h); } } close{key};
        const auto value_name = WinRtSettingsValueName(settings_id);
        DWORD type = 0, bytes = 0;
        if (RegQueryValueExW(key, value_name.c_str(), nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
            type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 65536)
            return std::nullopt;
        std::wstring folder(bytes / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, value_name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(folder.data()), &bytes) != ERROR_SUCCESS)
            return std::nullopt;
        while (!folder.empty() && folder.back() == L'\0') folder.pop_back();
        if (folder.empty()) return std::nullopt;
        const DWORD attrs = GetFileAttributesW(folder.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return std::nullopt;
        return std::u16string(folder.begin(), folder.end());
    } catch (...) { return std::nullopt; }
}
void WriteRememberedDirectory(std::wstring_view settings_id, const std::wstring& directory) noexcept {
    if (settings_id.empty() || directory.empty()) return;
    try {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Pulse\\WinRtPickerState", 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return;
        struct Close { HKEY h; ~Close() { RegCloseKey(h); } } close{key};
        const auto value_name = WinRtSettingsValueName(settings_id);
        const DWORD bytes = static_cast<DWORD>((directory.size() + 1) * sizeof(wchar_t));
        RegSetValueExW(key, value_name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(directory.c_str()), bytes);
    } catch (...) {}
}
Services WrapWithSettingsState(const Services& services, const hstring& settings_id, bool folder_result) {
    if (settings_id.empty() || !services.run) return services;
    std::wstring key(settings_id.c_str(), settings_id.size());
    Services wrapped;
    wrapped.run = [run = services.run, key = std::move(key), folder_result](const wire::Message& req, HWND hwnd, HANDLE cancel) {
        auto reply = run(req, hwnd, cancel);
        if (const auto* body = std::get_if<wire::Response>(&reply.body);
            body && body->status == wire::Status::Accepted && !body->paths.empty()) {
            const auto& first = body->paths.front();
            std::wstring path(first.begin(), first.end());
            const std::wstring dir = folder_result ? path : std::filesystem::path(path).parent_path().wstring();
            WriteRememberedDirectory(key, dir);
        }
        return reply;
    };
    return wrapped;
}
std::optional<std::u16string> Start(pick::PickerLocationId location, wire::Operation operation) {
    if (location == pick::PickerLocationId::ComputerFolder) {
        if (operation != wire::Operation::SaveFile) return std::u16string(u"pulse:computer");
    }
    const KNOWNFOLDERID* folder = nullptr;
    switch (location) {
    case pick::PickerLocationId::DocumentsLibrary:
    case pick::PickerLocationId::Unspecified:
    case pick::PickerLocationId::HomeGroup:
        folder = &FOLDERID_Documents; break;
    case pick::PickerLocationId::Desktop: folder = &FOLDERID_Desktop; break;
    case pick::PickerLocationId::Downloads: folder = &FOLDERID_Downloads; break;
    case pick::PickerLocationId::MusicLibrary: folder = &FOLDERID_Music; break;
    case pick::PickerLocationId::PicturesLibrary: folder = &FOLDERID_Pictures; break;
    case pick::PickerLocationId::VideosLibrary: folder = &FOLDERID_Videos; break;
    case pick::PickerLocationId::ComputerFolder: folder = &FOLDERID_Profile; break;
    case pick::PickerLocationId::Objects3D: folder = &FOLDERID_Objects3D; break;
    default: return std::nullopt;
    }
    PWSTR path = nullptr;
    HRESULT hr = SHGetKnownFolderPath(*folder, KF_FLAG_DONT_VERIFY, nullptr, &path);
    if (FAILED(hr) && folder == &FOLDERID_Objects3D)
        hr = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DONT_VERIFY, nullptr, &path);
    if (FAILED(hr) || !path) return std::nullopt;
    std::u16string result(reinterpret_cast<const char16_t*>(path));
    CoTaskMemFree(path); return result;
}
template<class Native> std::optional<wire::Request> Base(const Native& native, HWND owner, bool native_only, wire::Operation operation) {
    if (native_only || FAILED(DesktopEligibility(owner))) return std::nullopt;
    const auto settings_id = native.SettingsIdentifier();
    auto start = ReadRememberedDirectory(std::wstring_view(settings_id.c_str(), settings_id.size()));
    if (!start) start = Start(native.SuggestedStartLocation(), operation);
    if (!start) return std::nullopt;
    wire::Request request; request.operation = operation;
    request.required_capabilities = operation == wire::Operation::PickFolder ? wire::Folder : operation == wire::Operation::SaveFile ? wire::Save : wire::Open;
    request.initial_directory = std::move(*start);
    if (!native.CommitButtonText().empty()) {
        request.required_capabilities |= wire::DialogLabels;
        request.label_flags |= wire::LabelOkButton;
        request.ok_button_label = Text(native.CommitButtonText());
    }
    return request;
}
template<class Vector> std::optional<std::u16string> Patterns(const Vector& values) {
    if (!values.Size()) return std::nullopt;
    std::u16string pattern;
    for (const auto& extension : values) {
        if (!pattern.empty()) pattern += u";";
        if (extension == L"*") pattern += u"*.*";
        else if (extension.size() > 1 && extension[0] == L'.') { pattern += u"*"; pattern += Text(extension); }
        else return std::nullopt;
    }
    return pattern;
}
struct Open : winrt::implements<Open, pick::IFileOpenPicker, pick::IFileOpenPicker2, pick::IFileOpenPicker3, pick::IFileOpenPickerWithOperationId, ::IInitializeWithWindow, winrt::non_agile> {
    pick::FileOpenPicker native{nullptr};
    Services services;
    HWND owner = nullptr;
    bool native_only = false;
    Open(pick::FileOpenPicker value, Services bridge) : native(std::move(value)), services(std::move(bridge)) {}
    int32_t __stdcall QueryInterface(const winrt::guid& iid, void** output) noexcept {
        if (!output) return E_POINTER;
        if (iid == winrt::guid_of<pick::IFileOpenPicker2>() && !native.try_as<pick::IFileOpenPicker2>()) { *output = nullptr; return E_NOINTERFACE; }
        if (iid == winrt::guid_of<pick::IFileOpenPicker3>() && !native.try_as<pick::IFileOpenPicker3>()) { *output = nullptr; return E_NOINTERFACE; }
        if (iid == winrt::guid_of<pick::IFileOpenPickerWithOperationId>() && !native.try_as<pick::IFileOpenPickerWithOperationId>()) { *output = nullptr; return E_NOINTERFACE; }
        return implements::QueryInterface(iid, output);
    }
    hstring GetRuntimeClassName() const { return L"Windows.Storage.Pickers.FileOpenPicker"; }
    HRESULT __stdcall Initialize(HWND window) noexcept override {
        try {
            const HRESULT hr = native.as<::IInitializeWithWindow>()->Initialize(window);
            if (SUCCEEDED(hr)) owner = window;
            return hr;
        } catch (...) { return winrt::to_hresult(); }
    }
    auto SettingsIdentifier() { return native.SettingsIdentifier(); }
    void SettingsIdentifier(const hstring& value) { native.SettingsIdentifier(value); }
    auto SuggestedStartLocation() { return native.SuggestedStartLocation(); }
    void SuggestedStartLocation(const pick::PickerLocationId& value) { native.SuggestedStartLocation(value); }
    auto CommitButtonText() { return native.CommitButtonText(); }
    void CommitButtonText(const hstring& value) { native.CommitButtonText(value); }
    auto ViewMode() { return native.ViewMode(); }
    void ViewMode(const pick::PickerViewMode& value) { native.ViewMode(value); }
    auto User() { return native.User(); }
    auto ContinuationData() { native_only = true; return native.ContinuationData(); }
    auto FileTypeFilter() { return native.FileTypeFilter(); }
    void PickSingleFileAndContinue() { native_only = true; native.PickSingleFileAndContinue(); }
    void PickMultipleFilesAndContinue() { native_only = true; native.PickMultipleFilesAndContinue(); }
    auto PickSingleFileAsync(const hstring& id) { native_only = true; return native.as<pick::IFileOpenPickerWithOperationId>().PickSingleFileAsync(id); }
    std::optional<wire::Request> Request(bool multiple) {
        auto request = Base(native, owner, native_only, wire::Operation::OpenFiles);
        const auto patterns = Patterns(native.FileTypeFilter());
        if (!request || !patterns || !services.run) return std::nullopt;
        request->required_capabilities |= wire::Filters;
        request->filters.push_back({*patterns, *patterns});
        if (multiple) { request->required_capabilities |= wire::MultiSelect; request->options |= wire::AllowMultiSelect; }
        return request;
    }
    IAsyncOperation<File> PickSingleFileAsync() {
        auto request = Request(false);
        return request ? PickFileAsync(std::move(*request), owner, WrapWithSettingsState(services, native.SettingsIdentifier(), false)) : native.PickSingleFileAsync();
    }
    IAsyncOperation<Files> PickMultipleFilesAsync() {
        auto request = Request(true);
        return request ? PickFilesAsync(std::move(*request), owner, WrapWithSettingsState(services, native.SettingsIdentifier(), false)) : native.PickMultipleFilesAsync();
    }
};
using Extensions = foundation::Collections::IVector<hstring>;
using ChoicesMap = foundation::Collections::IMap<hstring, Extensions>;
using Choice = foundation::Collections::IKeyValuePair<hstring, Extensions>;
// Native FileTypeChoices supports mutation/lookup but can return E_NOINTERFACE
// for iteration and E_NOTIMPL for GetView. Track only keys inserted by this host;
// native Insert/Remove still validate every mutation before our mirror changes.
struct Choices : winrt::implements<Choices, ChoicesMap, foundation::Collections::IIterable<Choice>, winrt::non_agile> {
    ChoicesMap native{nullptr};
    ChoicesMap keys = winrt::single_threaded_map<hstring, Extensions>();
    explicit Choices(ChoicesMap value) : native(std::move(value)) {}
    uint32_t Size() { return native.Size(); }
    bool HasKey(const hstring& key) { return native.HasKey(key); }
    Extensions Lookup(const hstring& key) { return native.Lookup(key); }
    bool Insert(const hstring& key, const Extensions& value) {
        const bool replaced = native.Insert(key, value); keys.Insert(key, value); return replaced;
    }
    void Remove(const hstring& key) { native.Remove(key); keys.Remove(key); }
    void Clear() { native.Clear(); keys.Clear(); }
    auto GetView() {
        auto snapshot = winrt::single_threaded_map<hstring, Extensions>();
        for (const auto& key : keys) snapshot.Insert(key.Key(), native.Lookup(key.Key()));
        return snapshot.GetView();
    }
    auto First() { return GetView().First(); }
};
struct Save : winrt::implements<Save, pick::IFileSavePicker, pick::IFileSavePicker2, pick::IFileSavePicker3, pick::IFileSavePicker4, ::IInitializeWithWindow, winrt::non_agile> {
    pick::FileSavePicker native{nullptr};
    Services services;
    HWND owner = nullptr;
    bool native_only = false;
    ChoicesMap choices{nullptr};
    Save(pick::FileSavePicker value, Services bridge) : native(std::move(value)), services(std::move(bridge)), choices(winrt::make<Choices>(native.FileTypeChoices())) {}
    int32_t __stdcall QueryInterface(const winrt::guid& iid, void** output) noexcept {
        if (!output) return E_POINTER;
        if (iid == winrt::guid_of<pick::IFileSavePicker2>() && !native.try_as<pick::IFileSavePicker2>()) { *output = nullptr; return E_NOINTERFACE; }
        if (iid == winrt::guid_of<pick::IFileSavePicker3>() && !native.try_as<pick::IFileSavePicker3>()) { *output = nullptr; return E_NOINTERFACE; }
        if (iid == winrt::guid_of<pick::IFileSavePicker4>() && !native.try_as<pick::IFileSavePicker4>()) { *output = nullptr; return E_NOINTERFACE; }
        return implements::QueryInterface(iid, output);
    }
    hstring GetRuntimeClassName() const { return L"Windows.Storage.Pickers.FileSavePicker"; }
    HRESULT __stdcall Initialize(HWND window) noexcept override {
        try {
            const HRESULT hr = native.as<::IInitializeWithWindow>()->Initialize(window);
            if (SUCCEEDED(hr)) owner = window;
            return hr;
        } catch (...) { return winrt::to_hresult(); }
    }
    auto SettingsIdentifier() { return native.SettingsIdentifier(); }
    void SettingsIdentifier(const hstring& value) { native.SettingsIdentifier(value); }
    auto SuggestedStartLocation() { return native.SuggestedStartLocation(); }
    void SuggestedStartLocation(const pick::PickerLocationId& value) { native.SuggestedStartLocation(value); }
    auto CommitButtonText() { return native.CommitButtonText(); }
    void CommitButtonText(const hstring& value) { native.CommitButtonText(value); }
    auto DefaultFileExtension() { return native.DefaultFileExtension(); }
    void DefaultFileExtension(const hstring& value) { native.DefaultFileExtension(value); }
    auto SuggestedSaveFile() { return native.SuggestedSaveFile(); }
    void SuggestedSaveFile(const File& value) { native.SuggestedSaveFile(value); if (value) native_only = true; }
    auto SuggestedFileName() { return native.SuggestedFileName(); }
    void SuggestedFileName(const hstring& value) { native.SuggestedFileName(value); }
    auto EnterpriseId() { return native.EnterpriseId(); }
    void EnterpriseId(const hstring& value) { native.EnterpriseId(value); if (!value.empty()) native_only = true; }
    auto User() { return native.User(); }
    auto ContinuationData() { native_only = true; return native.ContinuationData(); }
    auto FileTypeChoices() { return choices; }
    void PickSaveFileAndContinue() { native_only = true; native.PickSaveFileAndContinue(); }
    IAsyncOperation<File> PickSaveFileAsync() {
        auto request = Base(native, owner, native_only, wire::Operation::SaveFile);
        if (!request || !services.run || !native.FileTypeChoices().Size())
            return native.PickSaveFileAsync();
        for (const auto& choice : choices.GetView()) {
            const auto pattern = Patterns(choice.Value());
            if (!pattern) return native.PickSaveFileAsync();
            request->filters.push_back({Text(choice.Key()), *pattern});
        }
        request->required_capabilities |= wire::Filters;
        request->options = wire::OverwritePrompt;
        request->filename = Text(native.SuggestedFileName());
        auto extension = Text(native.DefaultFileExtension());
        if (!extension.empty() && extension.front() == u'.') extension.erase(0, 1);
        request->default_extension = std::move(extension);
        return PickSaveAsync(std::move(*request), owner, WrapWithSettingsState(services, native.SettingsIdentifier(), false));
    }
};
struct FolderProxy : winrt::implements<FolderProxy, pick::IFolderPicker, pick::IFolderPicker2, pick::IFolderPicker3, ::IInitializeWithWindow, winrt::non_agile> {
    pick::FolderPicker native{nullptr};
    Services services;
    HWND owner = nullptr;
    bool native_only = false;
    FolderProxy(pick::FolderPicker value, Services bridge) : native(std::move(value)), services(std::move(bridge)) {}
    int32_t __stdcall QueryInterface(const winrt::guid& iid, void** output) noexcept {
        if (!output) return E_POINTER;
        if (iid == winrt::guid_of<pick::IFolderPicker2>() && !native.try_as<pick::IFolderPicker2>()) { *output = nullptr; return E_NOINTERFACE; }
        if (iid == winrt::guid_of<pick::IFolderPicker3>() && !native.try_as<pick::IFolderPicker3>()) { *output = nullptr; return E_NOINTERFACE; }
        return implements::QueryInterface(iid, output);
    }
    hstring GetRuntimeClassName() const { return L"Windows.Storage.Pickers.FolderPicker"; }
    HRESULT __stdcall Initialize(HWND window) noexcept override {
        try {
            const HRESULT hr = native.as<::IInitializeWithWindow>()->Initialize(window);
            if (SUCCEEDED(hr)) owner = window;
            return hr;
        } catch (...) { return winrt::to_hresult(); }
    }
    auto SettingsIdentifier() { return native.SettingsIdentifier(); }
    void SettingsIdentifier(const hstring& value) { native.SettingsIdentifier(value); }
    auto SuggestedStartLocation() { return native.SuggestedStartLocation(); }
    void SuggestedStartLocation(const pick::PickerLocationId& value) { native.SuggestedStartLocation(value); }
    auto CommitButtonText() { return native.CommitButtonText(); }
    void CommitButtonText(const hstring& value) { native.CommitButtonText(value); }
    auto ViewMode() { return native.ViewMode(); }
    void ViewMode(const pick::PickerViewMode& value) { native.ViewMode(value); }
    auto User() { return native.User(); }
    auto ContinuationData() { native_only = true; return native.ContinuationData(); }
    auto FileTypeFilter() { return native.FileTypeFilter(); }
    void PickFolderAndContinue() { native_only = true; native.PickFolderAndContinue(); }
    IAsyncOperation<Folder> PickSingleFolderAsync() {
        auto request = Base(native, owner, native_only, wire::Operation::PickFolder);
        if (!request || !services.run || !native.FileTypeFilter().Size()) return native.PickSingleFolderAsync();
        return PickFolderAsync(std::move(*request), owner, WrapWithSettingsState(services, native.SettingsIdentifier(), true));
    }
};
struct Factory : winrt::implements<Factory, foundation::IActivationFactory> {
    foundation::IActivationFactory native{nullptr};
    hstring name;
    Services services;
    Factory() = default;
    foundation::IInspectable ActivateInstance() {
        auto value = native.ActivateInstance<foundation::IInspectable>();
        if (name == L"Windows.Storage.Pickers.FileOpenPicker") return winrt::make<Open>(value.as<pick::FileOpenPicker>(), services);
        if (name == L"Windows.Storage.Pickers.FileSavePicker") return winrt::make<Save>(value.as<pick::FileSavePicker>(), services);
        return winrt::make<FolderProxy>(value.as<pick::FolderPicker>(), services);
    }
};
}
HRESULT GetActivationFactory(HSTRING name, REFIID iid, void** output, const Services& services, GetActivationFactoryFunction original) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!original) return E_INVALIDARG;
    const HRESULT hr = original(name, iid, output);
    if (FAILED(hr) || !*output || !IsEqualIID(iid, winrt::guid_of<foundation::IActivationFactory>()) || !services.run) return hr;
    try {
        UINT32 length = 0;
        const wchar_t* text = WindowsGetStringRawBuffer(name, &length);
        const std::wstring_view type(text, length);
        if (type != L"Windows.Storage.Pickers.FileOpenPicker" && type != L"Windows.Storage.Pickers.FileSavePicker" && type != L"Windows.Storage.Pickers.FolderPicker") return hr;
        foundation::IActivationFactory native{nullptr};
        winrt::copy_from_abi(native, *output);
        auto implementation = winrt::make_self<Factory>();
        implementation->native = std::move(native);
        implementation->name = hstring(text, length);
        implementation->services = services;
        auto factory = implementation.as<foundation::IActivationFactory>();
        static_cast<::IUnknown*>(*output)->Release();
        *output = winrt::detach_abi(factory);
        return S_OK;
    } catch (...) { return hr; } // Original activation factory remains the fallback.
}
HRESULT ActivateInstance(HSTRING name, IInspectable** output, const Services& services, ActivateInstanceFunction original) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!original) return E_INVALIDARG;
    const HRESULT hr = original(name, output);
    if (FAILED(hr) || !*output || !services.run) return hr;
    try {
        UINT32 length = 0;
        const wchar_t* text = WindowsGetStringRawBuffer(name, &length);
        const std::wstring_view type(text, length);
        if (type != L"Windows.Storage.Pickers.FileOpenPicker" &&
            type != L"Windows.Storage.Pickers.FileSavePicker" &&
            type != L"Windows.Storage.Pickers.FolderPicker") {
            return hr;
        }
        foundation::IInspectable native{nullptr};
        winrt::copy_from_abi(native, *output);
        foundation::IInspectable wrapped{nullptr};
        if (type == L"Windows.Storage.Pickers.FileOpenPicker") {
            wrapped = winrt::make<Open>(native.as<pick::FileOpenPicker>(), services);
        } else if (type == L"Windows.Storage.Pickers.FileSavePicker") {
            wrapped = winrt::make<Save>(native.as<pick::FileSavePicker>(), services);
        } else {
            wrapped = winrt::make<FolderProxy>(native.as<pick::FolderPicker>(), services);
        }
        (*output)->Release();
        *output = static_cast<IInspectable*>(winrt::detach_abi(wrapped));
        return S_OK;
    } catch (...) { return hr; } // Original instance remains the fallback.
}
}
