#include "../shell_bridge/picker_shell_results.h"
#include "../common/path_utils.h"
#include <wrl/client.h>
#include <filesystem>
#include <cstdio>

namespace {
using Microsoft::WRL::ComPtr;
using pulse::ipc::picker::Operation;
using pulse::shell_bridge::CreatePickerShellItem;
using pulse::shell_bridge::CreatePickerShellItemArray;
int failures = 0;
void Check(bool condition, const char* label) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}
std::wstring Native(const std::filesystem::path& path) {
    const auto value = path.wstring();
    if (value.starts_with(L"\\\\?\\")) return value;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
bool FileIdentity(const std::filesystem::path& path, BY_HANDLE_FILE_INFORMATION& info) {
    HANDLE file = CreateFileW(Native(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = GetFileInformationByHandle(file, &info) != FALSE;
    CloseHandle(file);
    return ok;
}
bool SameObject(const std::filesystem::path& first, const std::filesystem::path& second) {
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    return FileIdentity(first, a) && FileIdentity(second, b) &&
        a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}
bool Missing(const std::filesystem::path& path) {
    return GetFileAttributesW(Native(path).c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
}
bool SamePath(IShellItem* item, const std::filesystem::path& expected, bool nonexistent_save = false) {
    if (!item) return false;
    PWSTR path = nullptr;
    const HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    bool same = SUCCEEDED(hr) && path && pulse::path::EqualInsensitive(
        pulse::path::StripExtendedPathPrefix(path), expected.wstring());
    // Real Shell objects may expose an 8.3 spelling for paths beyond MAX_PATH.
    // Establish object identity, not just a plausible-looking shortened string.
    if (!same && SUCCEEDED(hr) && path) {
        const std::filesystem::path actual(pulse::path::StripExtendedPathPrefix(path));
        if (nonexistent_save) {
            same = Missing(actual) && Missing(expected) &&
                pulse::path::EqualInsensitive(actual.filename().wstring(), expected.filename().wstring()) &&
                SameObject(actual.parent_path(), expected.parent_path());
        } else same = SameObject(actual, expected);
        if (same) std::printf("[INFO] Shell spelling differs: same %s identity verified (actual=%zu expected=%zu units).\n",
            nonexistent_save ? "parent and unchanged nonexistent leaf" : "volume/file ID", wcslen(path), expected.wstring().size());
    }
    if (!same) {
        std::printf("GetDisplayName(SIGDN_FILESYSPATH) HRESULT=0x%08lX actual_units=%zu expected_units=%zu\n",
            static_cast<unsigned long>(hr), path ? wcslen(path) : 0, expected.wstring().size());
        if (path) std::printf("actual fixture path: %ls\n", path);
        std::printf("expected fixture path: %ls\n", expected.c_str());
    }
    CoTaskMemFree(path);
    return same;
}
bool CreateFixture(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(Native(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, "untouched", 9, &written, nullptr) && written == 9;
    CloseHandle(file);
    return ok;
}
}

int main(int argc, char** argv) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    const auto bench = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / L"bench_data";
    const auto root = std::filesystem::absolute(bench / (L"shell-results-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64())));
    if (!CreateDirectoryW(Native(root).c_str(), nullptr)) { CoUninitialize(); return 2; }
    const auto first = root / L"first.txt";
    const auto second = root / L"\u6587\U0001f600.txt";
    Check(CreateFixture(first) && CreateFixture(second), "isolated files created under bench_data");
    Check(SameObject(first, first) && !SameObject(first, second), "identity check distinguishes separate files with equal contents");
    if (argc == 2 && std::string_view(argv[1]) == "--save-no-test-create") {
        const auto parent = root / L"absent", deep = parent / L"missing.pdf";
        for (const auto& path : {first, root / L"missing.pdf", deep}) {
            ComPtr<IShellItem> item;
            Check(SUCCEEDED(CreatePickerShellItem(Operation::SaveFile,path.wstring(),&item,true)) && SamePath(item.Get(),path), "explicit no-test Save returns exact existing or missing path");
            ComPtr<IShellItemArray> array;
            Check(SUCCEEDED(CreatePickerShellItemArray(Operation::SaveFile,{path.wstring()},&array,true)), "Save result array supports explicit missing parent");
        }
        ComPtr<IShellItem> strict;
        Check(FAILED(CreatePickerShellItem(Operation::SaveFile,deep.wstring(),&strict)) && !strict, "default Save still rejects missing parent");
        Check(!std::filesystem::exists(parent) && !std::filesystem::exists(root/L"missing.pdf"), "Save result construction creates no file or parent");
        Check(DeleteFileW(first.c_str()) && DeleteFileW(second.c_str()) && RemoveDirectoryW(root.c_str()), "owned Save result fixtures cleaned");
        CoUninitialize(); return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--allow-missing") {
        const auto leaf=root/L"missing.txt", parent=root/L"absent", deep=parent/L"missing.txt";
        for(const auto& path : {first,leaf,deep}) {
            ComPtr<IShellItem> item;
            const HRESULT hr=CreatePickerShellItem(Operation::OpenFiles,path.wstring(),&item,true);
            std::printf("allow-missing hr=%08lx\n",static_cast<unsigned long>(hr));
            Check(SUCCEEDED(hr)&&SamePath(item.Get(),path),"real Shell parser preserves existing or missing absolute path");
            SFGAOF attributes=0;
            const HRESULT attr=item?item->GetAttributes(SFGAO_FILESYSTEM|SFGAO_FOLDER,&attributes):E_FAIL;
            std::printf("attributes hr=%08lx value=%08lx\n",static_cast<unsigned long>(attr),static_cast<unsigned long>(attributes));
            Check(SUCCEEDED(attr)&&(attributes&SFGAO_FILESYSTEM)&&!(attributes&SFGAO_FOLDER),"Shell filesystem nonfolder attributes");
        }
        ComPtr<IShellItemArray> array;
        Check(SUCCEEDED(CreatePickerShellItemArray(Operation::OpenFiles,{first.wstring(),leaf.wstring(),deep.wstring()},&array,true)),"mixed existing and missing array");
        DWORD count=0;Check(array&&SUCCEEDED(array->GetCount(&count))&&count==3,"mixed result count");
        unsigned index=0;for(const auto& path : {first,leaf,deep}) {ComPtr<IShellItem> item;Check(array&&SUCCEEDED(array->GetItemAt(index++,&item))&&SamePath(item.Get(),path),"mixed order preserved");}
        for(const auto& path : {leaf,deep}) {ComPtr<IShellItem> item;Check(FAILED(CreatePickerShellItem(Operation::OpenFiles,path.wstring(),&item))&&!item,"default Open still rejects missing path");}
        Check(!std::filesystem::exists(leaf)&&!std::filesystem::exists(parent),"Shell parsing creates neither leaf nor parent");
        array.Reset();
        Check(DeleteFileW(first.c_str())&&DeleteFileW(second.c_str())&&RemoveDirectoryW(root.c_str()),"owned missing-result fixtures cleaned");
        CoUninitialize();return failures?1:0;
    }
    {
        ComPtr<IShellItem> item;
        HRESULT hr = CreatePickerShellItem(Operation::OpenFiles, first.wstring(), &item);
        Check(SUCCEEDED(hr) && SamePath(item.Get(), first), "open returns a real filesystem Shell item");
        ComPtr<IUnknown> identity, again;
        ComPtr<IShellItem> queried;
        Check(item && SUCCEEDED(item.As(&identity)) && SUCCEEDED(identity.As(&queried)) &&
            SUCCEEDED(queried.As(&again)) && identity.Get() == again.Get(), "Shell item QueryInterface preserves IUnknown identity");
        item.Reset();
        Check(SamePath(queried.Get(), first), "queried interface remains alive after original reference released");
        hr = CreatePickerShellItem(Operation::PickFolder, root.wstring(), &item);
        Check(SUCCEEDED(hr) && SamePath(item.Get(), root), "folder result preserves directory identity");
        item.Reset();
        hr = CreatePickerShellItem(Operation::PickFolder, first.wstring(), &item);
        Check(FAILED(hr) && !item, "folder mode rejects a file and clears output");
        IShellItem* poisoned = reinterpret_cast<IShellItem*>(static_cast<INT_PTR>(1));
        hr = CreatePickerShellItem(Operation::OpenFiles, L"relative.txt", &poisoned);
        Check(FAILED(hr) && !poisoned, "invalid relative input clears caller output");
        Check(CreatePickerShellItem(Operation::OpenFiles, first.wstring(), nullptr) == E_POINTER, "null output pointer rejected");
        hr = CreatePickerShellItem(Operation::OpenFiles, (root / L"missing.txt").wstring(), &item);
        Check(FAILED(hr) && !item, "open never manufactures a nonexistent target");
        ComPtr<IShellItemArray> array;
        hr = CreatePickerShellItemArray(Operation::OpenFiles, {second.wstring(), first.wstring()}, &array);
        DWORD count = 0;
        ComPtr<IShellItem> a, b;
        Check(SUCCEEDED(hr) && array && SUCCEEDED(array->GetCount(&count)) && count == 2 &&
            SUCCEEDED(array->GetItemAt(0, &a)) && SUCCEEDED(array->GetItemAt(1, &b)) &&
            SamePath(a.Get(), second) && SamePath(b.Get(), first), "real Shell array preserves selection order and Unicode");
        ComPtr<IUnknown> array_identity, array_again;
        ComPtr<IShellItemArray> array_queried;
        Check(array && SUCCEEDED(array.As(&array_identity)) && SUCCEEDED(array_identity.As(&array_queried)) &&
            SUCCEEDED(array_queried.As(&array_again)) && array_identity.Get() == array_again.Get(), "Shell array QueryInterface identity is stable");
        array.Reset();
        hr = CreatePickerShellItemArray(Operation::OpenFiles, {first.wstring(), (root / L"missing").wstring()}, &array);
        Check(FAILED(hr) && !array, "partially constructed array is never returned as success");
        hr = CreatePickerShellItemArray(Operation::SaveFile, {first.wstring(), second.wstring()}, &array);
        Check(FAILED(hr) && !array, "save rejects multiple result targets");
        const auto target = root / L"new-\u6587\U0001f600.txt";
        hr = CreatePickerShellItem(Operation::SaveFile, target.wstring(), &item);
        if (FAILED(hr)) std::printf("save bind HRESULT=0x%08lX\n", static_cast<unsigned long>(hr));
        Check(SUCCEEDED(hr) && SamePath(item.Get(), target, true), "save constructs real Shell item for nonexistent Unicode leaf");
        Check(GetFileAttributesW(Native(target).c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
            "save result conversion never creates the target");
        item.Reset();
        hr = CreatePickerShellItem(Operation::SaveFile, (root / L"absent" / L"new.txt").wstring(), &item);
        Check(FAILED(hr) && !item, "save cannot fabricate a nonexistent parent directory");
        hr = CreatePickerShellItem(Operation::SaveFile, first.wstring(), &item);
        WIN32_FILE_ATTRIBUTE_DATA existing{};
        Check(SUCCEEDED(hr) && SamePath(item.Get(), first) && GetFileAttributesExW(Native(first).c_str(),
            GetFileExInfoStandard, &existing) && existing.nFileSizeLow == 9 && existing.nFileSizeHigh == 0,
            "existing save result does not truncate file contents");
    }
    std::vector<std::filesystem::path> directories;
    auto long_parent = root;
    bool long_created = true;
    for (int i = 0; i < 3; ++i) {
        long_parent /= std::wstring(90, static_cast<wchar_t>(L'a' + i));
        if (!CreateDirectoryW(Native(long_parent).c_str(), nullptr)) { long_created = false; break; }
        directories.push_back(long_parent);
    }
    Check(long_created, "long-path directory fixture created");
    if (long_created) {
        const auto long_file = long_parent / L"long.txt";
        const bool made = CreateFixture(long_file);
        ComPtr<IShellItem> item;
        const HRESULT hr = CreatePickerShellItem(Operation::OpenFiles, Native(long_file), &item);
        if (FAILED(hr)) std::printf("long-path Shell HRESULT=0x%08lX\n", static_cast<unsigned long>(hr));
        Check(made && SUCCEEDED(hr) && SamePath(item.Get(), long_file), "extended long-path input returns its real Shell item");
        const auto new_long = long_parent / L"new.txt";
        item.Reset();
        const HRESULT save_hr = CreatePickerShellItem(Operation::SaveFile, Native(new_long), &item);
        if (FAILED(save_hr)) std::printf("long-path save HRESULT=0x%08lX\n", static_cast<unsigned long>(save_hr));
        Check(SUCCEEDED(save_hr) && SamePath(item.Get(), new_long, true) &&
            GetFileAttributesW(Native(new_long).c_str()) == INVALID_FILE_ATTRIBUTES, "long-path save result does not create target");
        item.Reset();
        Check(DeleteFileW(Native(long_file).c_str()) != FALSE, "long-path file fixture removed");
    }
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
        Check(RemoveDirectoryW(Native(*it).c_str()) != FALSE, "long-path directory fixture removed");
    std::puts("[SKIP] UNC Shell parsing: no isolated test share is configured; no network path was probed.");
    Check(DeleteFileW(Native(first).c_str()) && DeleteFileW(Native(second).c_str()) && RemoveDirectoryW(Native(root).c_str()),
        "all Shell result fixtures removed");
    CoUninitialize();
    return failures ? 1 : 0;
}
