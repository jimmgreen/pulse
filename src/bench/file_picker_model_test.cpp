#include "../common/picker_open_filter.h"
#include "../common/picker_shell_namespace.h"
#include "../common/windows_compat.h"
#include "../ui/folder_picker_loader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <shobjidl.h>
#include <map>
#include <aclapi.h>
#include <cstring>
#include <wrl/client.h>

using namespace pulse::ui;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!ok) ++failures;
}
bool CreateShortcut(const std::filesystem::path& path, const std::filesystem::path& target) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return false;
    bool saved = false;
    {
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> file;
        saved = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
            SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(path.c_str(), TRUE));
    }
    CoUninitialize();
    return saved;
}
constexpr UINT kListing = WM_APP + 73;
constexpr UINT kValidation = WM_APP + 74;
std::unique_ptr<PickerValidation> WaitValidation(HWND window) {
    const auto end = GetTickCount64() + 5000;
    MSG message{};
    while (GetTickCount64() < end) {
        if (PeekMessageW(&message, window, kValidation, kValidation, PM_REMOVE)) return PickerLoader::TakeValidation(message.lParam);
        Sleep(1);
    }
    return {};
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--createprompt-open") {
        const auto base = std::filesystem::current_path() / L"bench_data" /
            (L"picker-createprompt-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!CreateDirectoryW(base.c_str(), nullptr)) return 2;
        const auto file = base / L"existing.txt";
        HANDLE fixture = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(fixture != INVALID_HANDLE_VALUE, "create isolated CREATEPROMPT existing-file fixture");
        if (fixture != INVALID_HANDLE_VALUE) CloseHandle(fixture);
        HWND window = CreateWindowExW(0, L"STATIC", L"Create prompt validation", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        Check(window != nullptr, "private CREATEPROMPT result window");
        if (window) {
            {
                PickerLoader loader(window, kListing, kValidation);
                PickerOptions options; options.native_options = FOS_CREATEPROMPT;
                options.allow_missing_open = true; options.require_existing_open_parent = true;
                options.validate_local_open_access_v1 = true;
                uint64_t generation = 1;
                auto validate = [&](std::vector<std::wstring> names) {
                    loader.Validate(generation++, base.wstring(), std::move(names), PickerMode::File, options);
                    return WaitValidation(window);
                };
                auto result = validate({L"new.txt"});
                Check(result && !result->error && result->paths == std::vector<std::wstring>{(base / L"new.txt").wstring()} &&
                    result->missing_paths == result->paths, "CREATEPROMPT missing leaf reaches UI confirmation without skip-access policy");
                result = validate({L"existing.txt"});
                Check(result && !result->error && result->paths == std::vector<std::wstring>{file.wstring()} && result->missing_paths.empty(),
                    "CREATEPROMPT preserves existing-file access validation and missing metadata");
                HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                Check(lock != INVALID_HANDLE_VALUE, "hold owned exclusive CREATEPROMPT file");
                result = validate({L"new.txt", L"existing.txt"});
                Check(result && result->error == ERROR_SHARING_VIOLATION && result->paths.empty() && result->missing_paths.empty(),
                    "CREATEPROMPT cannot bypass an existing selected file sharing denial");
                if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
                result = validate({L"absent\\new.txt"});
                Check(result && result->error && result->paths.empty(), "CREATEPROMPT still enforces existing-parent policy");
                options.native_options = 0;
                result = validate({L"new.txt"});
                Check(result && result->error == ERROR_NOT_SUPPORTED, "missing Open without explicit create policy retains legacy rejection");
            }
            DestroyWindow(window);
        }
        Check(GetFileAttributesW((base / L"new.txt").c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW((base / L"absent").c_str()) == INVALID_FILE_ATTRIBUTES, "CREATEPROMPT validation creates no file or directory");
        Check(DeleteFileW(file.c_str()) && RemoveDirectoryW(base.c_str()), "owned CREATEPROMPT fixtures removed");
        std::cout << "failures=" << failures << '\n'; return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--shell-native-options") {
        constexpr wchar_t recycle[] = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
        PickerOptions options; options.native_options = FOS_ALLNONSTORAGEITEMS;
        Check(NormalizePickerInput(recycle) == recycle && PickerParent(recycle).empty(), "Shell parsing name and namespace root parent remain intact");
        const auto listing = ReadPickerListing(recycle, PickerMode::File, options);
        Check(!listing.error && listing.path == recycle && listing.snapshot && listing.snapshot->size() == listing.entries.size(), "virtual folder enumerates real Shell items on loader path");
        const auto blocked = ReadPickerListing(recycle, PickerMode::File);
        Check(blocked.error != ERROR_SUCCESS, "filesystem-only loader still rejects virtual directory");
        const auto base = std::filesystem::current_path() / L"bench_data" /
            (L"picker-shell-filter-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        const bool created = CreateDirectoryW(base.c_str(), nullptr) != FALSE;
        Check(created, "create isolated Shell filesystem filter fixture");
        if (created) {
            for (const auto* name : {L"visible.txt", L"hidden.txt", L"excluded.jpg"}) {
                HANDLE file = CreateFileW((base / name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                Check(file != INVALID_HANDLE_VALUE, "create owned Shell filter file");
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            }
            Check(SetFileAttributesW((base / L"hidden.txt").c_str(), FILE_ATTRIBUTE_HIDDEN) != FALSE, "set owned Shell hidden fixture");
            options.filters = {{L"Text", L"*.txt"}};
            auto filtered = ReadPickerListing(base.wstring(), PickerMode::File, options);
            Check(!filtered.error && filtered.entries.size() == 1 && filtered.entries.front().name == L"visible.txt", "Shell listing filters actual filename extension and hides hidden items");
            options.show_hidden = true;
            filtered = ReadPickerListing(base.wstring(), PickerMode::File, options);
            Check(!filtered.error && filtered.entries.size() == 2, "Shell enumeration honors explicit hidden-items option");
            options.search = L"visible";
            filtered = ReadPickerListing(base.wstring(), PickerMode::File, options);
            Check(!filtered.error && filtered.entries.size() == 1 && filtered.entries.front().name == L"visible.txt", "Shell listing preserves search filter");
            for (const auto* name : {L"visible.txt", L"hidden.txt", L"excluded.jpg"}) Check(DeleteFileW((base / name).c_str()) != FALSE, "delete owned Shell filter file");
            Check(RemoveDirectoryW(base.c_str()) != FALSE, "remove empty owned Shell fixture");
            options.filters.clear(); options.search.clear(); options.show_hidden = false;
        }
        HWND window = CreateWindowExW(0, L"STATIC", L"Shell validation", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        Check(window != nullptr, "private Shell validation result window");
        if (window) {
            {
                PickerLoader loader(window, kListing, kValidation);
                loader.Validate(1, L"", {recycle}, PickerMode::Folder, options);
                auto result = WaitValidation(window);
                Check(result && !result->error && result->paths.size() == 1 && pulse::picker_shell::IsParsingName(result->paths.front()), "worker accepts virtual folder with Shell identity");
                loader.Validate(2, L"", {recycle}, PickerMode::File, options);
                result = WaitValidation(window);
                Check(result && !result->error && result->navigate == recycle && result->paths.empty(), "virtual folder in Open mode navigates instead of becoming file");
                for (uint64_t generation = 3; generation < 30; ++generation) loader.Load(generation, recycle, PickerMode::File, options);
            }
            MSG message{};
            Check(!PeekMessageW(&message, window, kListing, kValidation, PM_REMOVE), "cancelled Shell enumeration drains posted payloads");
            DestroyWindow(window);
        }
        std::cout << "failures=" << failures << '\n'; return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--open-filter-separators") {
        for (const auto* pattern : {L"*.zip;", L"*.zip;;", L";*.zip", L"*.zip;;*.rar;"}) {
            PickerOptions options; options.filters = {{L"Generic",pattern}};
            Check(pulse::picker_filter::OpenPattern(std::wstring_view(pattern)), "generic empty-segment Open mask admitted");
            Check(PickerMatchesFilter(L"allowed.zip", PickerMode::File, options) && !PickerMatchesFilter(L"blocked.txt", PickerMode::File, options), "empty segment does not turn filter into match-all");
            Check(pulse::picker_filter::FirstLiteralExtension(std::wstring_view(pattern)) == L"zip", "first nonempty literal extension retained");
        }
        Check(!pulse::picker_filter::OpenPattern(std::wstring_view(L";;;")), "empty-only filter rejected");
        std::cout << "failures=" << failures << '\n'; return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--open-missing") {
        const auto base=std::filesystem::current_path()/L"bench_data"/(L"picker-open-missing-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        if(!CreateDirectoryW(base.c_str(),nullptr))return 2;
        HANDLE file=CreateFileW((base/L"exists.txt").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return 2;CloseHandle(file);
        HWND window=CreateWindowExW(0,L"STATIC",L"missing Open",0,0,0,0,0,HWND_MESSAGE,nullptr,nullptr,nullptr);
        Check(window!=nullptr,"private missing-result window");
        if(window){
            {
                PickerLoader loader(window,kListing,kValidation);PickerOptions options;
                options.allow_missing_open=true;options.skip_access_validation=true;options.advisory_file_filters=true;
                uint64_t generation=1;
                auto validate=[&](std::vector<std::wstring> names,PickerMode mode=PickerMode::File){loader.Validate(generation++,base.wstring(),std::move(names),mode,options);return WaitValidation(window);};
                auto result=validate({L"exists.txt"});
                Check(result&&!result->error&&result->paths==std::vector<std::wstring>{(base/L"exists.txt").wstring()},"missing policy preserves existing exact file");
                result=validate({L"missing.txt"});
                Check(result&&!result->error&&result->paths==std::vector<std::wstring>{(base/L"missing.txt").wstring()},"explicit missing policy returns absent file path");
                result=validate({L"absent\\missing.txt"});
                Check(result&&!result->error&&result->paths==std::vector<std::wstring>{(base/L"absent"/L"missing.txt").wstring()},"absent parent accepted only in explicit policy");
                result=validate({L"exists.txt",L"missing.txt",L"missing.lnk"});
                Check(result&&!result->error&&result->paths==std::vector<std::wstring>{(base/L"exists.txt").wstring(),(base/L"missing.txt").wstring(),(base/L"missing.lnk").wstring()},"mixed multi stays ordered and missing shortcut is not resolved");
                result=validate({base.wstring()});
                Check(result&&!result->error&&result->navigate==base.wstring()&&result->paths.empty(),"existing directory remains navigation");
                options.require_existing_open_parent=true;result=validate({L"absent\\missing.txt"});
                Check(result&&result->error&&result->paths.empty(),"require-parent rejects absent parent");
                result=validate({L"missing.txt"});Check(result&&!result->error,"require-parent accepts absent leaf with existing parent");
                result=validate({L"exists.txt\\missing.txt"});Check(result&&result->error&&result->paths.empty(),"require-parent rejects file parent");
                options.require_existing_open_parent=false;options.allow_missing_open=false;result=validate({L"missing.txt"});
                Check(result&&result->error==ERROR_FILE_NOT_FOUND&&result->paths.empty(),"ordinary Open still rejects absent leaf");
                options.allow_missing_open=true;options.skip_access_validation=false;result=validate({L"missing.txt"});
                Check(result&&result->error==ERROR_NOT_SUPPORTED,"missing requires explicit skip access policy");
                options.skip_access_validation=true;result=validate({L"missing.txt"},PickerMode::Folder);
                Check(result&&result->error==ERROR_NOT_SUPPORTED,"folder cannot use missing Open policy");
                for(const auto* invalid:{L"bad?.txt",L"\\\\.\\pipe\\pulse-missing"}){result=validate({invalid});Check(result&&result->error&&result->paths.empty(),"invalid or device path cannot become a missing result");}
                loader.Validate(generation++,base.wstring(),std::vector<std::wstring>(1024,L"missing.txt"),PickerMode::File,options);
                const uint64_t newest=generation++;loader.Validate(newest,base.wstring(),{L"exists.txt"},PickerMode::File,options);
                result=WaitValidation(window);if(result&&result->generation!=newest)result=WaitValidation(window);
                Check(result&&result->generation==newest&&!result->error&&result->paths==std::vector<std::wstring>{(base/L"exists.txt").wstring()},"latest request supersedes missing validation work");
            }
            MSG message{};while(PeekMessageW(&message,window,kValidation,kValidation,PM_REMOVE))PickerLoader::TakeValidation(message.lParam);
            DestroyWindow(window);
        }
        Check(GetFileAttributesW((base/L"missing.txt").c_str())==INVALID_FILE_ATTRIBUTES&&GetFileAttributesW((base/L"absent").c_str())==INVALID_FILE_ATTRIBUTES,"validation creates neither file nor parent");
        Check(DeleteFileW((base/L"exists.txt").c_str())&&RemoveDirectoryW(base.c_str()),"remove only owned missing Open fixture");
        return failures?1:0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--computer-place") {
        const auto base = std::filesystem::current_path() / L"bench_data" /
            (L"picker-computer-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        Check(CreateDirectoryW(base.c_str(), nullptr) != FALSE, "exclusive Computer navigation fixture");
        const auto file = base / L"ordinary.txt";
        HANDLE fixture = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(fixture != INVALID_HANDLE_VALUE, "Computer navigation ordinary file fixture");
        if (fixture != INVALID_HANDLE_VALUE) CloseHandle(fixture);
        const auto computer = ReadPickerListing(L"pulse:computer", PickerMode::File);
        Check(computer.error == ERROR_SUCCESS && !computer.entries.empty(), "Computer token enumerates drive rows");
        const auto drive = base.root_path().wstring();
        bool found = false;
        for (const auto& row : computer.entries) {
            Check(row.kind == PickerEntryKind::Drive && !row.path.empty(), "Computer rows are real drive destinations");
            if (_wcsicmp(row.path.c_str(), drive.c_str()) == 0) found = true;
        }
        Check(found, "Computer includes fixture drive");
        const auto drive_listing = ReadPickerListing(drive, PickerMode::File);
        Check(drive_listing.error == ERROR_SUCCESS, "Computer drive destination opens filesystem listing");
        Check(PickerParent(drive).empty(), "drive Up returns internal Computer root");
        HWND window = CreateWindowExW(0, L"STATIC", L"Computer proof", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        Check(window != nullptr, "Computer validation result window");
        if (window) {
            {
                PickerLoader loader(window, kListing, kValidation);
                PickerOptions options; options.validate_local_open_access_v1 = true;
                loader.Validate(1, base.wstring(), {L"ordinary.txt"}, PickerMode::File, options);
                auto accepted = WaitValidation(window);
                Check(accepted && !accepted->error && accepted->paths.size() == 1 && accepted->paths[0] == file.wstring(), "local file remains selectable after Computer navigation");
                loader.Validate(2, base.wstring(), {L"pulse:computer"}, PickerMode::File, options);
                auto rejected = WaitValidation(window);
                Check(rejected && rejected->error && rejected->paths.empty(), "Computer token cannot become an accepted file");
            }
            DestroyWindow(window);
        }
        Check(DeleteFileW(file.c_str()) != FALSE && RemoveDirectoryW(base.c_str()) != FALSE, "Computer fixture cleanup");
        return failures ? 1 : 0;
    }
    if (argc == 2 && (std::string_view(argv[1]) == "--open-extension" || std::string_view(argv[1]) == "--extension-masks")) {
        const auto base = std::filesystem::current_path() / L"bench_data" /
            (L"picker-open-extension-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        Check(CreateDirectoryW(base.c_str(),nullptr)!=FALSE,"exclusive extension fixture");
        if(failures)return 1;
        const std::vector<std::wstring> files{L"stem",L"stem.txt",L"fallback.txt",L"explicit.md",L"first.txt",L"second.txt",L"fallback.md",L"literal.td;bt",L"literal..td",L"literal.tar.gz"};
        for(const auto& name:files){
            HANDLE file=CreateFileW((base/name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            Check(file!=INVALID_HANDLE_VALUE,"CREATE_NEW extension fixture file");if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        }
        HWND window=CreateWindowExW(0,L"STATIC",L"typed extension",0,0,0,0,0,HWND_MESSAGE,nullptr,nullptr,nullptr);
        Check(window!=nullptr,"private worker result window");
        if(window){
            PickerLoader loader(window,kListing,kValidation);
            PickerOptions options;options.validate_local_open_access_v1=true;options.typed_names=true;
            options.use_default_extension=true;options.default_extension=L"txt";options.advisory_file_filters=true;
            uint64_t generation=1;
            auto validate=[&](std::vector<std::wstring> names){
                loader.Validate(generation++,base.wstring(),std::move(names),PickerMode::File,options);return WaitValidation(window);
            };
            auto result=validate({L"stem"});
            Check(result && !result->error && result->paths==std::vector<std::wstring>{(base/L"stem").wstring()},"typed exact extensionless file wins over existing suffixed candidate");
            result=validate({L"fallback"});
            Check(result && !result->error && result->paths==std::vector<std::wstring>{(base/L"fallback.txt").wstring()},"typed missing exact name resolves existing default-extension file");
            result=validate({L"explicit.md"});
            Check(result && !result->error && result->paths==std::vector<std::wstring>{(base/L"explicit.md").wstring()},"explicit extension remains unchanged");
            result=validate({L"first",L"second"});
            Check(result && !result->error && result->paths==std::vector<std::wstring>{(base/L"first.txt").wstring(),(base/L"second.txt").wstring()},"typed multi candidates resolve independently in order");
            result=validate({L"first",L"missing"});
            Check(result && result->error==ERROR_FILE_NOT_FOUND && result->paths.empty(),"unresolved later extension candidate rejects whole batch");
            HANDLE denied=CreateFileW((base/L"stem").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
            Check(denied!=INVALID_HANDLE_VALUE,"hold exact extensionless file exclusively");
            result=validate({L"stem"});
            Check(result && result->error==ERROR_SHARING_VIOLATION && result->paths.empty(),"exact sharing denial cannot redirect to readable suffixed file");
            if(denied!=INVALID_HANDLE_VALUE)CloseHandle(denied);
            options.typed_names=false;result=validate({L"fallback"});
            Check(result && result->error==ERROR_FILE_NOT_FOUND && result->paths.empty(),"selected path never substitutes extension candidate");
            result=validate({L"stem"});
            Check(result && !result->error && result->paths==std::vector<std::wstring>{(base/L"stem").wstring()},"selected extensionless row retains actual path");
            options.typed_names=true;options.use_default_extension=false;result=validate({L"fallback"});
            Check(result && result->error==ERROR_FILE_NOT_FOUND,"absent setter does not infer an extension");
            options.use_default_extension=true;options.default_extension.clear();result=validate({L"fallback"});
            Check(result && result->error==ERROR_FILE_NOT_FOUND,"explicit empty default does not invent suffix");
            for(const auto* invalid:{L"a/b",L"a\tb",L"td.",L"td "}){
                options.default_extension=invalid;result=validate({L"stem"});
                Check(result && result->error==ERROR_INVALID_NAME && result->paths.empty(),"invalid default extension rejected before file acceptance");
            }
            for (const auto* complex : {L"td;bt", L".td", L"tar.gz", L"*.td;*.bt", L"*", L"a b", L"txt||"}) {
                options.default_extension = complex;
                result = validate({L"stem", L"explicit.md"});
                Check(result && !result->error && result->paths == std::vector<std::wstring>{(base/L"stem").wstring(), (base/L"explicit.md").wstring()}, "complex default preserves exact and multiple existing names");
            }
            for (const auto* literal : {L"td;bt", L".td", L"tar.gz"}) {
                options.default_extension = literal;
                result = validate({L"literal"});
                Check(result && !result->error && result->paths == std::vector<std::wstring>{(base/(std::wstring(L"literal.") + literal)).wstring()}, "missing name appends entire literal default without splitting or stripping");
                result = validate({L"absent"});
                Check(result && result->error && result->paths.empty(), "missing literal candidate remains an error");
            }
            options.default_extension = L"txt||";
            result = validate({L"fallback"});
            Check(result && result->error && result->paths.empty(), "pipe default without filters is not truncated into existing txt file");
            options.filters = {{L"All", L"*.*"}};
            result = validate({L"fallback"});
            Check(result && result->error && result->paths.empty(), "all-files filter does not truncate pipe default");
            options.filters = {{L"Drawing shape", L"*.txt;*.md||"}};
            result = validate({L"fallback"});
            Check(result && !result->error && result->paths == std::vector<std::wstring>{(base/L"fallback.txt").wstring()},
                "initial first literal mask supplies missing-name suffix even with pipe default");
            options.filters[0].pattern = L"*.md||;*.txt";
            result = validate({L"fallback"});
            Check(result && result->error && result->paths.empty(), "pipe first mask is not truncated and later masks are not searched");
            result = validate({L"explicit.md", L"stem"});
            Check(result && !result->error && result->paths == std::vector<std::wstring>{(base/L"explicit.md").wstring(), (base/L"stem").wstring()},
                "pipe first mask preserves exact outside-filter names and exact extensionless names");
            options.filters.clear();
            options.default_extension = L"*.txt";
            result = validate({L"fallback"});
            Check(result && result->error && result->paths.empty(), "wildcard suffix does not choose a matching file");
            options.default_extension=L"txt";
            options.filters={{L"Text",L"*.txt"},{L"Markdown",L"*.md"},{L"All",L"*.*"}};
            options.filter_index=1;result=validate({L"fallback"});
            Check(result && !result->error && result->paths==std::vector<std::wstring>{(base/L"fallback.md").wstring()},"programmatic initial type index uses first literal mask as native Open does");
            options.extension_filter_changed=true;result=validate({L"fallback"});
            Check(result && !result->error && result->paths==std::vector<std::wstring>{(base/L"fallback.md").wstring()},"type change updates missing-name extension candidate");
            options.filters.push_back({L"Generic extension mask",L"*.xl*;*.xlsx"});
            options.filter_index=3;result=validate({L"fallback"});
            Check(result && !result->error && result->paths==std::vector<std::wstring>{(base/L"fallback.txt").wstring()},
                "wildcard first pattern restores original default after literal change without guessing later xlsx");
            options.filter_index=2;result=validate({L"fallback"});
            Check(result && !result->error && result->paths==std::vector<std::wstring>{(base/L"fallback.txt").wstring()},"all-files type restores original default extension");
        }
        PickerOptions masks;masks.filters={{L"Generic",L"*.xl*;*.xlsx"}};
        PickerOptions pipe_masks; pipe_masks.filters = {{L"Drawing shape", L"*.txt;*.md||"}};
        const auto pipe_listing = ReadPickerListing(base.wstring(), PickerMode::File, pipe_masks);
        Check(!pipe_listing.error && !pipe_listing.entries.empty(), "literal pipe mask listing succeeds");
        for (const auto& entry : pipe_listing.entries)
            Check(entry.name.ends_with(L".txt"), "pipe masks never broaden into the stripped extension");
        for(const auto* name:{L"book.xls",L"BOOK.XLSX",L"book.xl",L"book.xlsm"})
            Check(PickerMatchesFilter(name,PickerMode::File,masks),"extension mask matches generic star suffix case-insensitively");
        for(const auto* name:{L"book.txt",L"book.doc",L"bookxlsx",L"LICENSE"})
            Check(!PickerMatchesFilter(name,PickerMode::File,masks),"extension mask excludes unrelated suffixes and extensionless names");
        masks.filters[0].pattern=L"*.m*";
        const auto filtered=ReadPickerListing(base.wstring(),PickerMode::File,masks);
        Check(filtered.error==ERROR_SUCCESS && filtered.entries.size()==2,"real listing retains only two matching mask rows");
        for(const auto& entry:filtered.entries)Check(entry.name==L"explicit.md" || entry.name==L"fallback.md","real listing never broadens mask to txt files");
        if(window)DestroyWindow(window);
        if(!failures){for(const auto& name:files)Check(DeleteFileW((base/name).c_str())!=FALSE,"delete exact owned extension fixture");Check(RemoveDirectoryW(base.c_str())!=FALSE,"remove empty extension fixture");}
        return failures?1:0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--local-open") {
        const auto base = std::filesystem::current_path() / L"bench_data" /
            (L"picker-local-open-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        const bool created = CreateDirectoryW(base.c_str(), nullptr) != FALSE;
        Check(created, "create exclusive local-policy fixture directory");
        if (!created) return 1;
        const auto file = base / L"ordinary.txt";
        HANDLE fixture = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(fixture != INVALID_HANDLE_VALUE, "CREATE_NEW ordinary fixture");
        if (fixture != INVALID_HANDLE_VALUE) CloseHandle(fixture);
        const auto second_file = base / L"second.txt";
        HANDLE second_fixture = CreateFileW(second_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(second_fixture != INVALID_HANDLE_VALUE, "CREATE_NEW second local-policy fixture");
        if (second_fixture != INVALID_HANDLE_VALUE) CloseHandle(second_fixture);
        HWND window = CreateWindowExW(0, L"STATIC", L"local Open", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        Check(window != nullptr, "validation result window");
        bool acl_recovered = true;
        if (window) {
            PickerLoader loader(window, kListing, kValidation);
            PickerOptions options; options.validate_local_open_access_v1 = true;
            uint64_t generation = 1;
            auto validate = [&](const std::vector<std::wstring>& names) {
                loader.Validate(generation++, base.wstring(), names, PickerMode::File, options);
                return WaitValidation(window);
            };
            auto result = validate({L"ordinary.txt"});
            Check(result && !result->error && result->paths == std::vector<std::wstring>{file.wstring()}, "local ordinary access accepted");
            Check(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "set owned readonly fixture");
            result = validate({L"ordinary.txt"});
            Check(result && !result->error && result->paths.size() == 1, "readonly Open remains readable");
            Check(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "restore owned attributes");
            HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            Check(lock != INVALID_HANDLE_VALUE, "hold exclusive owned file");
            result = validate({L"ordinary.txt"});
            Check(result && result->error == ERROR_SHARING_VIOLATION && result->paths.empty(), "worker preserves sharing error");
            result = validate({L"second.txt", L"ordinary.txt"});
            Check(result && result->error == ERROR_SHARING_VIOLATION && result->paths.empty() && result->navigate.empty(),
                "sharing denial after a readable prefix rejects entire batch");
            options.native_options = FOS_SHAREAWARE;
            result = validate({L"second.txt", L"ordinary.txt"});
            Check(result && result->error == ERROR_SHARING_VIOLATION &&
                result->paths == std::vector<std::wstring>{second_file.wstring(), file.wstring()} && result->failed_path == file.wstring(),
                "share-aware policy retains every validated candidate for caller decision");
            options.native_options |= FOS_ALLNONSTORAGEITEMS;
            result = validate({L"second.txt", L"ordinary.txt"});
            Check(result && result->error == ERROR_SHARING_VIOLATION && result->paths.size() == 2,
                "Shell namespace mode retains filesystem access and sharing validation");
            result = validate({L"ordinary.txt", L"missing.txt"});
            Check(result && result->error == ERROR_FILE_NOT_FOUND && result->paths.empty(),
                "share-aware candidate retention cannot hide another selection error");
            SaveTargetRequest save_request; save_request.directory = base.wstring(); save_request.filename = L"ordinary.txt";
            const auto save_captured = CaptureSaveConfirmation(save_request);
            Check(save_captured.snapshot.has_value(), "exclusive content lock still permits Save metadata capture");
            if (save_captured.snapshot) {
                loader.CheckSaveAccess(generation++, *save_captured.snapshot, false, 0, false, false, FOS_SHAREAWARE);
                result = WaitValidation(window);
                Check(result && result->error == ERROR_SHARING_VIOLATION && result->save_access_checked && result->save_snapshot &&
                    result->paths == std::vector<std::wstring>{file.wstring()}, "share-aware Save retains checked snapshot and candidate for caller decision");
            }
            options.native_options = 0;
            if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
            result = validate({L"missing.txt"});
            Check(result && result->error == ERROR_FILE_NOT_FOUND && result->paths.empty(), "missing retains exact error");
            result = validate({L"absent-parent\\missing.txt"});
            Check(result && (result->error == ERROR_FILE_NOT_FOUND || result->error == ERROR_PATH_NOT_FOUND) &&
                result->paths.empty() && result->navigate.empty(), "missing parent cannot produce an accepted Open result");
            result = validate({L"second.txt", L"absent-parent\\missing.txt"});
            Check(result && result->error && result->paths.empty() && result->navigate.empty(),
                "missing parent after a valid item rejects the entire Open batch");
            result = validate({L"second.txt", L"ordinary.txt"});
            Check(result && !result->error && result->paths == std::vector<std::wstring>{second_file.wstring(),file.wstring()},
                "local multi-file access returns every validated path in caller order");
            result = validate({L"second.txt", L"missing.txt"});
            Check(result && result->error == ERROR_FILE_NOT_FOUND && result->paths.empty() && result->navigate.empty(),
                "missing second item rejects whole local batch without successful prefix");
            result = validate({L"second.txt", L"\\\\.\\pipe\\unopened.txt"});
            Check(result && result->error == ERROR_NOT_SUPPORTED && result->paths.empty(),
                "unsupported device namespace second item rejects whole batch");
            result = validate(std::vector<std::wstring>(1025, L"ordinary.txt"));
            Check(result && result->error == ERROR_NOT_SUPPORTED && result->paths.empty(),
                "local batch rejects more than the existing 1024-path protocol bound");
            // Owned CREATE_NEW fixture only. Preserve the original descriptor on
            // disk before applying a minimal READ_EA deny; never edit user ACLs.
            PACL original_acl = nullptr;
            PSECURITY_DESCRIPTOR original_sd = nullptr;
            const DWORD captured = GetNamedSecurityInfoW(const_cast<PWSTR>(file.c_str()), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION, nullptr, nullptr, &original_acl, nullptr, &original_sd);
            SECURITY_DESCRIPTOR_CONTROL original_control{}; DWORD revision = 0;
            const bool captured_control = captured == ERROR_SUCCESS &&
                GetSecurityDescriptorControl(original_sd, &original_control, &revision);
            const auto journal = base / L"acl-original.bin";
            bool journaled = false;
            if (captured_control) {
                std::ofstream recovery(journal, std::ios::binary);
                recovery.write(static_cast<const char*>(original_sd), GetSecurityDescriptorLength(original_sd));
                recovery.close(); journaled = !recovery.fail();
            }
            BYTE everyone[SECURITY_MAX_SID_SIZE]{}; DWORD sid_size = sizeof(everyone);
            EXPLICIT_ACCESSW deny{}; PACL restricted = nullptr;
            deny.grfAccessPermissions = FILE_READ_EA; deny.grfAccessMode = DENY_ACCESS;
            deny.Trustee.TrusteeForm = TRUSTEE_IS_SID; deny.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
            deny.Trustee.ptstrName = reinterpret_cast<PWSTR>(everyone);
            const bool prepared_acl = journaled && CreateWellKnownSid(WinWorldSid, nullptr, everyone, &sid_size) &&
                SetEntriesInAclW(1, &deny, original_acl, &restricted) == ERROR_SUCCESS;
            DWORD installed = ERROR_INVALID_DATA;
            if (prepared_acl) installed = SetNamedSecurityInfoW(const_cast<PWSTR>(file.c_str()), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, restricted, nullptr);
            std::cout << "[FACT] ACL capture=" << captured << " install=" << installed << " journaled=" << journaled << '\n';
            Check(installed == ERROR_SUCCESS, "owned READ_EA deny fixture with recovery journal installed");
            if (installed == ERROR_SUCCESS) {
                acl_recovered = false;
                auto access_error = [&](DWORD access) {
                    HANDLE probe = CreateFileW(file.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
                    if (probe == INVALID_HANDLE_VALUE) return GetLastError();
                    CloseHandle(probe); return DWORD{ERROR_SUCCESS};
                };
                Check(access_error(FILE_READ_DATA) == ERROR_SUCCESS && access_error(GENERIC_READ) == ERROR_ACCESS_DENIED,
                    "fixture proves data-only success versus generic-read denial");
                result = validate({L"ordinary.txt"});
                Check(result && result->error == ERROR_ACCESS_DENIED && result->paths.empty(), "worker preserves generic-read ACL failure");
                result = validate({L"second.txt", L"ordinary.txt"});
                Check(result && result->error == ERROR_ACCESS_DENIED && result->paths.empty(),
                    "second-item ACL denial discards already validated prefix");
                const auto protection = (original_control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
                const DWORD restored = SetNamedSecurityInfoW(const_cast<PWSTR>(file.c_str()), SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION | protection, nullptr, nullptr, original_acl, nullptr);
                PACL restored_acl = nullptr; PSECURITY_DESCRIPTOR restored_sd = nullptr;
                SECURITY_DESCRIPTOR_CONTROL restored_control{};
                const DWORD queried = GetNamedSecurityInfoW(const_cast<PWSTR>(file.c_str()), SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION, nullptr, nullptr, &restored_acl, nullptr, &restored_sd);
                acl_recovered = restored == ERROR_SUCCESS && queried == ERROR_SUCCESS &&
                    GetSecurityDescriptorControl(restored_sd, &restored_control, &revision) &&
                    (original_control & SE_DACL_PROTECTED) == (restored_control & SE_DACL_PROTECTED) &&
                    ((original_acl == nullptr && restored_acl == nullptr) ||
                     (original_acl && restored_acl && original_acl->AclSize == restored_acl->AclSize &&
                      std::memcmp(original_acl, restored_acl, original_acl->AclSize) == 0));
                std::cout << "[FACT] ACL restore=" << restored << " query=" << queried << " exact=" << acl_recovered << '\n';
                Check(acl_recovered, "original DACL bytes and protection restored exactly");
                if (restored_sd) LocalFree(restored_sd);
            }
            if (restricted) LocalFree(restricted);
            if (original_sd) LocalFree(original_sd);
            if (acl_recovered && journaled) Check(DeleteFileW(journal.c_str()) != FALSE, "restored ACL journal removed");
            if (!acl_recovered) std::wcerr << L"ACL recovery required: " << file << L" journal=" << journal << L'\n';
            options.skip_access_validation = true;
            result = validate({L"ordinary.txt"});
            Check(result && result->error == ERROR_NOT_SUPPORTED && result->paths.empty(), "contradictory policy rejected");
        }
        if (window) DestroyWindow(window);
        HANDLE exclusive = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(exclusive != INVALID_HANDLE_VALUE, "worker leaves no file handle");
        if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
        Check(acl_recovered && DeleteFileW(file.c_str()) != FALSE && DeleteFileW(second_file.c_str()) != FALSE &&
            RemoveDirectoryW(base.c_str()) != FALSE, "owned fixtures removed nonrecursively");
        return failures ? 1 : 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--advisory-filters") {
        const auto base = std::filesystem::absolute(L"bench_data") /
            (L"picker-advisory-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!CreateDirectoryW(base.c_str(), nullptr)) return 2;
        std::ofstream(base / L"manual.bin") << "unchanged";
        Check(CreateShortcut(base / L"manual.lnk", base / L"manual.bin"), "create advisory shortcut fixture");
        HWND window = CreateWindowExW(0, L"STATIC", L"advisory validation", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) return 2;
        {
            PickerLoader loader(window, kListing, kValidation);
            PickerOptions options;
            options.filters = {{L"Text", L"*.txt"}, {L"Images", L"*.png"}};
            options.filter_index = 1;
            uint64_t generation = 1;
            loader.Validate(generation++, base.wstring(), {L"manual.bin"}, PickerMode::File, options);
            auto result = WaitValidation(window);
            Check(result && result->error == ERROR_UNSUPPORTED_TYPE && result->paths.empty(), "default product acceptance remains strict");
            options.advisory_file_filters = true;
            loader.Validate(generation++, base.wstring(), {L"manual.bin"}, PickerMode::File, options);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_SUCCESS && result->paths == std::vector<std::wstring>{(base / L"manual.bin").wstring()}, "advisory accepts existing typed file outside filter");
            const auto listing = ReadPickerListing(base.wstring(), PickerMode::File, options);
            bool visible = false;
            for (const auto& entry : listing.entries) if (entry.name == L"manual.bin") visible = true;
            Check(listing.error == ERROR_SUCCESS && !visible && options.filter_index == 1, "advisory keeps listing filter and selected index");
            loader.Validate(generation++, base.wstring(), {L"missing.bin"}, PickerMode::File, options);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_FILE_NOT_FOUND && result->paths.empty(), "advisory does not bypass existence validation");
            Check(!options.skip_access_validation, "product loader options default to normal access contract");
            options.skip_access_validation = true;
            loader.Validate(generation++, base.wstring(), {L"missing.bin"}, PickerMode::File, options);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_FILE_NOT_FOUND && result->paths.empty() && result->navigate.empty(),
                "explicit skip-access contract still rejects missing file in background validation");
            options.advisory_file_filters = false;
            loader.Validate(generation++, base.wstring(), {L"manual.bin"}, PickerMode::File, options);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_UNSUPPORTED_TYPE && result->paths.empty(),
                "skip-access alone does not change strict filter acceptance");
            options.advisory_file_filters = true;
            options.skip_access_validation = false;
            loader.Validate(generation++, base.wstring(), {L"manual.bin"}, PickerMode::Image, options);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_UNSUPPORTED_TYPE && result->paths.empty(), "advisory cannot bypass Image format safety");
            options.no_dereference_links = true;
            loader.Validate(generation++, base.wstring(), {L"manual.lnk"}, PickerMode::File, options);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_SUCCESS && result->paths == std::vector<std::wstring>{(base / L"manual.lnk").wstring()}, "advisory combines with shortcut object preservation");
            Check(!PickerShowsEntry(FILE_ATTRIBUTE_NORMAL, L"manual.lnk", PickerMode::File, options), "advisory shortcut acceptance does not broaden listing");
        }
        DestroyWindow(window);
        bool cleaned = DeleteFileW((base / L"manual.lnk").c_str()) != FALSE;
        cleaned = DeleteFileW((base / L"manual.bin").c_str()) != FALSE && cleaned;
        cleaned = RemoveDirectoryW(base.c_str()) != FALSE && cleaned;
        Check(cleaned, "advisory fixtures cleaned up");
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--links") {
        const auto base = std::filesystem::absolute(L"bench_data") /
            (L"picker-links-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!CreateDirectoryW(base.c_str(), nullptr)) return 2;
        if (!CreateDirectoryW((base / L"folder").c_str(), nullptr)) return 2;
        std::ofstream(base / L"target.txt") << "target";
        Check(CreateShortcut(base / L"file.lnk", base / L"target.txt"), "create live file shortcut fixture");
        Check(CreateShortcut(base / L"dead.lnk", base / L"missing.txt"), "create broken shortcut fixture");
        Check(CreateShortcut(base / L"folder.lnk", base / L"folder"), "create directory shortcut fixture");
        HWND window = CreateWindowExW(0, L"STATIC", L"link validation", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) return 2;
        {
            PickerLoader loader(window, kListing, kValidation);
            PickerOptions preserve;
            preserve.no_dereference_links = true;
            preserve.filters = {{L"All", L"*.*"}};
            uint64_t generation = 1;
            for (const auto* name : {L"file.lnk", L"dead.lnk", L"folder.lnk", L"target.txt"}) {
                loader.Validate(generation++, base.wstring(), {name}, PickerMode::Folder, preserve);
                const auto result = WaitValidation(window);
                Check(result && result->error == ERROR_UNSUPPORTED_TYPE && result->paths.empty() && result->navigate.empty(),
                    "folder no-dereference rejects shortcut files and ordinary files without resolving targets");
                Check(!PickerShowsEntry(FILE_ATTRIBUTE_NORMAL, name, PickerMode::Folder, preserve),
                    "folder no-dereference listing excludes non-directory objects");
            }
            loader.Validate(generation++, base.wstring(), {L"folder"}, PickerMode::Folder, preserve);
            const auto directory = WaitValidation(window);
            Check(directory && directory->error == ERROR_SUCCESS && directory->paths == std::vector<std::wstring>{(base / L"folder").wstring()},
                "folder no-dereference accepts an existing directory");
            loader.Validate(generation++, base.wstring(), {L"missing"}, PickerMode::Folder, preserve);
            const auto missing = WaitValidation(window);
            Check(missing && missing->error == ERROR_FILE_NOT_FOUND && missing->paths.empty(),
                "folder no-dereference retains existence validation");
            for (const auto* name : {L"file.lnk", L"dead.lnk", L"folder.lnk"}) {
                loader.Validate(generation++, base.wstring(), {name}, PickerMode::File, preserve);
                auto result = WaitValidation(window);
                Check(result && result->error == ERROR_SUCCESS && result->navigate.empty() &&
                    result->paths == std::vector<std::wstring>{(base / name).wstring()}, "no-dereference returns shortcut object without navigating its target");
            }
            loader.Validate(generation++, base.wstring(), {L"file.lnk"}, PickerMode::File);
            auto result = WaitValidation(window);
            Check(result && result->error == ERROR_SUCCESS && result->paths == std::vector<std::wstring>{(base / L"target.txt").wstring()}, "default shortcut resolution still returns file target");
            loader.Validate(generation++, base.wstring(), {L"dead.lnk"}, PickerMode::File);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_FILE_NOT_FOUND && result->paths.empty(), "default broken shortcut still fails");
            loader.Validate(generation++, base.wstring(), {L"folder.lnk"}, PickerMode::File);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_SUCCESS && result->navigate == (base / L"folder").wstring(), "default directory shortcut still navigates");
            loader.Validate(generation++, base.wstring(), {L"folder.lnk"}, PickerMode::Folder, preserve);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_UNSUPPORTED_TYPE && result->paths.empty() && result->navigate.empty(), "folder mode does not reinterpret shortcut file as directory");
            preserve.filters = {{L"Text", L"*.txt"}};
            loader.Validate(generation++, base.wstring(), {L"file.lnk"}, PickerMode::File, preserve);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_UNSUPPORTED_TYPE, "shortcut object still obeys file filter");
            Check(!PickerShowsEntry(FILE_ATTRIBUTE_NORMAL, L"file.lnk", PickerMode::File, preserve), "no-dereference listing does not bypass filter for shortcuts");
            const auto listing = ReadPickerListing(base.wstring(), PickerMode::File, preserve);
            bool contains_link = false;
            for (const auto& entry : listing.entries) if (entry.name.ends_with(L".lnk")) contains_link = true;
            Check(listing.error == ERROR_SUCCESS && !contains_link, "real enumeration applies shortcut-object filter");
            preserve.filters = {{L"Links", L"*.lnk"}};
            const auto links = ReadPickerListing(base.wstring(), PickerMode::File, preserve);
            size_t file_links = 0;
            for (const auto& entry : links.entries) if (entry.name.ends_with(L".lnk") && entry.kind == PickerEntryKind::File) ++file_links;
            Check(file_links == 3, "link rows retain file kind used by double-click confirmation");
        }
        DestroyWindow(window);
        bool cleaned = true;
        for (const auto* name : {L"file.lnk", L"dead.lnk", L"folder.lnk", L"target.txt"})
            cleaned = DeleteFileW((base / name).c_str()) != FALSE && cleaned;
        cleaned = RemoveDirectoryW((base / L"folder").c_str()) != FALSE && cleaned;
        cleaned = RemoveDirectoryW(base.c_str()) != FALSE && cleaned;
        Check(cleaned, "link fixtures cleaned up");
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--save") {
        const auto base = std::filesystem::absolute(L"bench_data") /
            (L"save-loader-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!CreateDirectoryW(base.c_str(), nullptr)) return 2;
        std::ofstream(base / L"existing.txt") << "keep";
        HWND window = CreateWindowExW(0, L"STATIC", L"save validation", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) return 2;
        {
            PickerLoader loader(window, kListing, kValidation);
            SaveTargetRequest request;
            request.directory = base.wstring();
            request.filename = L"new";
            request.default_extension = L"txt";
            request.filters = {{L"Text", L"*.txt"}, {L"Image", L"*.png;*.jpg"}};
            loader.ValidateSave(1, request);
            auto result = WaitValidation(window);
            Check(result && result->generation == 1 && result->error == ERROR_SUCCESS &&
                result->paths == std::vector<std::wstring>{(base / L"new.txt").wstring()} &&
                !result->overwrite_required && !std::filesystem::exists(base / L"new.txt"), "save worker returns nonexistent target without creating it");
            if (result && result->save_snapshot) {
                const auto fresh_target = *result->save_snapshot;
                loader.CheckSaveAccess(400, fresh_target, false, 0);
                result = WaitValidation(window);
                Check(result && !result->error && result->save_access_checked && !result->save_file_ok_checked &&
                    result->save_access_diagnostic && result->save_access_diagnostic->cleanup == SaveAccessCleanup::Confirmed &&
                    !std::filesystem::exists(base / L"new.txt"), "LocalSave worker new-target probe confirms cleanup before FileOk");
                loader.CheckSaveAccess(401, fresh_target, true, 123);
                result = WaitValidation(window);
                Check(result && !result->error && result->save_file_ok_checked && result->save_ui_generation == 123 &&
                    !std::filesystem::exists(base / L"new.txt"), "LocalSave post-FileOk worker preserves authorization generation and cleans probe");
            }
            request.filter_index = 1;
            request.filter_changed = true;
            loader.ValidateSave(2, request);
            result = WaitValidation(window);
            Check(result && result->paths == std::vector<std::wstring>{(base / L"new.png").wstring()}, "save worker applies selected filter after type change");
            request.filename = L"existing.txt";
            loader.ValidateSave(3, request);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_SUCCESS && result->overwrite_required &&
                std::filesystem::file_size(base / L"existing.txt") == 4, "save worker requests overwrite without truncation");
            Check(result && result->save_snapshot.has_value(), "save worker returns confirmation identity snapshot");
            if (result && result->save_snapshot) {
                const auto captured = *result->save_snapshot;
                loader.RevalidateSave(300, captured);
                result = WaitValidation(window);
                Check(result && result->generation == 300 && !result->error && !result->overwrite_required &&
                    result->save_snapshot && result->paths == std::vector<std::wstring>{(base / L"existing.txt").wstring()},
                    "confirmed stable identity returns through background revalidation");
                loader.CheckSaveAccess(402, captured, true, 124);
                result = WaitValidation(window);
                Check(result && !result->error && result->save_access_checked && result->save_file_ok_checked &&
                    result->save_ui_generation == 124 && result->save_snapshot &&
                    result->save_snapshot->TargetIdentity() == captured.TargetIdentity() &&
                    std::filesystem::file_size(base / L"existing.txt") == 4,
                    "LocalSave existing post-FileOk probe keeps confirmed identity and file length");
                const auto retained = base / L"retained.txt";
                const bool moved = MoveFileW((base / L"existing.txt").c_str(), retained.c_str()) != FALSE;
                Check(moved, "worker replacement fixture retains original identity");
                if (moved) {
                    { std::ofstream replacement_file(base / L"existing.txt"); replacement_file << "test"; }
                    loader.RevalidateSave(301, captured);
                    result = WaitValidation(window);
                    Check(result && result->generation == 301 && !result->error && result->overwrite_required &&
                        result->save_snapshot, "worker target replacement asks for new confirmation");
                    loader.CheckSaveAccess(403, captured, true, 125);
                    result = WaitValidation(window);
                    Check(result && !result->error && result->overwrite_required && result->save_snapshot &&
                        !result->save_file_ok_checked && !result->save_access_checked && result->save_ui_generation == 125,
                        "LocalSave post-FileOk replacement loses consent and requests new overwrite");
                    Check(DeleteFileW((base / L"existing.txt").c_str()) &&
                        MoveFileW(retained.c_str(), (base / L"existing.txt").c_str()), "worker replacement fixture restored");
                }
                SetFileAttributesW((base / L"existing.txt").c_str(), FILE_ATTRIBUTE_READONLY);
                loader.RevalidateSave(302, captured);
                result = WaitValidation(window);
                Check(result && result->generation == 302 && result->error == ERROR_ACCESS_DENIED &&
                    result->paths.empty() && !result->save_snapshot && result->navigate.empty(),
                    "worker readonly transition fails without output or navigation");
                SetFileAttributesW((base / L"existing.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
                loader.RevalidateSave(303, captured);
                loader.ValidateSave(304, request);
                result = WaitValidation(window);
                // A reply posted before the second request may already be in the
                // window queue; the UI ignores that older generation as well.
                if (result && result->generation == 303) result = WaitValidation(window);
                Check(result && result->generation == 304 && result->overwrite_required && result->save_snapshot,
                    "new validation supersedes revalidation after discarding any already-posted stale reply");
            }
            SetFileAttributesW((base / L"existing.txt").c_str(), FILE_ATTRIBUTE_READONLY);
            loader.ValidateSave(4, request);
            result = WaitValidation(window);
            Check(result && result->error == ERROR_ACCESS_DENIED && result->paths.empty(), "save worker rejects read-only target");
            SetFileAttributesW((base / L"existing.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
            for (uint64_t generation = 5; generation < 100; ++generation)
                loader.Load(generation, base.wstring(), PickerMode::Save);
            request.filename = L"latest";
            loader.ValidateSave(100, request);
            result = WaitValidation(window);
            Check(result && result->generation == 100 && result->paths ==
                std::vector<std::wstring>{(base / L"latest.png").wstring()}, "latest save validation survives stale navigation jobs");
            loader.ValidateSave(101, request);
        }
        MSG message{};
        const bool late = PeekMessageW(&message, window, kValidation, kValidation, PM_REMOVE) != FALSE;
        if (late) PickerLoader::TakeValidation(message.lParam);
        Check(!late, "closing save loader drains pending validation replies");
        DestroyWindow(window);
        const bool cleaned = DeleteFileW((base / L"existing.txt").c_str()) && RemoveDirectoryW(base.c_str());
        Check(cleaned, "save loader fixtures removed");
        return failures ? 1 : 0;
    }
    PickerOptions filter;
    filter.filters = {{L"Pictures", L"*.PNG;*.jpg"}};
    Check(PickerMatchesFilter(L"A.PnG", PickerMode::File, filter) && !PickerMatchesFilter(L"a.png.exe", PickerMode::File, filter), "case insensitive exact suffix filter");
    filter.filters = {{L"All", L"*.*"}};
    Check(PickerMatchesFilter(L"LICENSE", PickerMode::File, filter) && PickerMatchesFilter(L"a.unknown", PickerMode::File, filter), "all files includes unknown and absent extensions");
    Check(!PickerMatchesFilter(L"a.svg", PickerMode::Image, filter), "image mode cannot broaden its supported formats");
    Check(!PickerShowsEntry(FILE_ATTRIBUTE_HIDDEN, L"a.txt", PickerMode::File), "hidden defaults off");
    filter.show_hidden = true;
    Check(PickerShowsEntry(FILE_ATTRIBUTE_HIDDEN, L"a.txt", PickerMode::File, filter), "hidden option");
    filter.search = L"DOC";
    Check(PickerShowsEntry(FILE_ATTRIBUTE_DIRECTORY, L"Documents", PickerMode::File, filter) && !PickerShowsEntry(FILE_ATTRIBUTE_DIRECTORY, L"Pictures", PickerMode::File, filter), "search filters folders case insensitively");
    std::vector<PickerEntry> entries{{L"file2", L"", PickerEntryKind::File}, {L"folder", L"", PickerEntryKind::Folder}, {L"file10", L"", PickerEntryKind::File}};
    SortPickerEntries(entries);
    Check(entries[0].name == L"folder" && entries[1].name == L"file2", "folder first and natural name order");
    PickerOptions descending; descending.descending = true;
    SortPickerEntries(entries, descending);
    Check(entries[0].name == L"folder" && entries[1].name == L"file10", "descending keeps folders first");
    entries[1].size = 1; entries[2].size = 10; descending.sort = PickerSort::Size;
    SortPickerEntries(entries, descending);
    Check(entries[1].size == 10, "size sort");
    entries[1].modified.dwLowDateTime = 1; entries[2].modified.dwLowDateTime = 10;
    descending.sort = PickerSort::Modified;
    SortPickerEntries(entries, descending);
    Check(entries[1].modified.dwLowDateTime == 10, "modified sort");
    entries[1].name = L"a.txt"; entries[2].name = L"z.png";
    descending.sort = PickerSort::Type; descending.descending = false;
    SortPickerEntries(entries, descending);
    Check(entries[1].name == L"z.png", "extension type sort");
    PickerHistory history;
    history.Navigate(L"C:\\one");
    Check(history.Back(L"C:\\two") == L"C:\\one" && history.CanGoForward(), "history back stores forward");
    Check(history.Forward(L"C:\\one") == L"C:\\two", "history forward");
    history.Back(L"C:\\two"); history.Navigate(L"C:\\one");
    Check(!history.CanGoForward(), "navigation invalidates forward");
    std::vector<std::wstring> names;
    Check(ParsePickerNames(L"  a file.txt  ", names) && names.size() == 1 && names[0] == L"a file.txt", "unquoted spaces are one filename");
    Check(ParsePickerNames(L"\"a file.txt\" \"b.txt\"", names) && names.size() == 2, "quoted multiple filenames");
    Check(!ParsePickerNames(L"\"unfinished", names) && names.empty(), "unmatched quote rejected");
    Check(NormalizePickerInput(L"\\\\?\\UNC\\server\\share\\file") == L"\\\\server\\share\\file", "UNC extended prefix normalization");
    Check(PickerParent(L"\\\\server\\share") == L"", "UNC share root parent");

    const auto base = std::filesystem::temp_directory_path() / (L"pulse-picker-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(base / L"folder");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{std::filesystem::path(L"\\\\?\\" + base.wstring())};
    std::ofstream(base / L"a file.txt") << "hello";
    std::ofstream(base / L"b.PNG") << "image";
    std::ofstream(base / L"LICENSE") << "license";
    HWND window = CreateWindowExW(0, L"STATIC", L"picker test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "test result window");
    if (!window) return 1;
    {
        PickerLoader loader(window, kListing, kValidation);
        loader.Validate(1, base.wstring(), {L"a file.txt", L"LICENSE"}, PickerMode::File);
        auto result = WaitValidation(window);
        Check(result && result->error == ERROR_SUCCESS && result->paths.size() == 2 && result->paths[0] == (base / L"a file.txt").wstring(), "relative multi-file validation");
        loader.Validate(201, base.wstring(), {L"folder"}, PickerMode::Folder);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1 && result->navigate.empty(), "folder mode confirms directory");
        SetEnvironmentVariableW(L"PULSE_PICKER_TEST_ROOT", base.c_str());
        loader.Validate(202, L"", {L"%PULSE_PICKER_TEST_ROOT%\\LICENSE"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1, "environment path expansion");
        SetEnvironmentVariableW(L"PULSE_PICKER_TEST_ROOT", nullptr);
        auto long_path = base;
        for (int part = 0; part < 6; ++part) {
            long_path /= std::wstring(48, L'x') + std::to_wstring(part);
            const std::wstring native = L"\\\\?\\" + long_path.wstring();
            Check(CreateDirectoryW(native.c_str(), nullptr) != FALSE, "long fixture directory");
        }
        const auto long_file = long_path / L"long.unknown";
        const auto native_file = L"\\\\?\\" + long_file.wstring();
        HANDLE file = CreateFileW(native_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "long fixture file");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        loader.Validate(203, L"", {native_file}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1 && result->paths[0] == long_file.wstring(), "existing extended long file validation");
        loader.Validate(2, base.wstring(), {L"folder"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->navigate == (base / L"folder").wstring(), "directory input navigates");
        loader.Validate(3, base.wstring(), {L"folder", L"LICENSE"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_DIRECTORY && result->paths.empty(), "mixed file and folder rejected atomically");
        loader.Validate(4, base.wstring(), {L"missing"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_FILE_NOT_FOUND, "missing file error");
        loader.Validate(5, base.wstring(), {L"LICENSE"}, PickerMode::Image);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_UNSUPPORTED_TYPE, "unsupported type error");
        loader.CreateFolder(6, base.wstring(), L"created");
        result = WaitValidation(window);
        Check(result && result->error == ERROR_SUCCESS && std::filesystem::is_directory(base / L"created"), "create folder");
        loader.CreateFolder(7, base.wstring(), L"created");
        result = WaitValidation(window);
        Check(result && result->error == ERROR_ALREADY_EXISTS, "create collision never overwrites");
        loader.CreateFolder(8, base.wstring(), L"..\\escape");
        result = WaitValidation(window);
        Check(result && result->error == ERROR_INVALID_NAME, "folder traversal rejected");
        for (uint64_t generation = 10; generation < 200; ++generation) loader.Load(generation, base.wstring(), PickerMode::File);
        loader.Validate(200, base.wstring(), {L"LICENSE"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->generation == 200 && result->paths.size() == 1, "latest request survives rapid navigation");
        Check(CreateShortcut(base / L"file.lnk", base / L"LICENSE"), "file shortcut fixture");
        Check(CreateShortcut(base / L"folder.lnk", base / L"folder"), "directory shortcut fixture");
        Check(CreateShortcut(base / L"image.lnk", base / L"b.PNG"), "image shortcut fixture");
        Check(CreateShortcut(base / L"dead.lnk", base / L"missing-target"), "dead shortcut fixture");
        loader.Validate(300, base.wstring(), {L"file.lnk"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1 && result->paths[0] == (base / L"LICENSE").wstring(), "file shortcut returns actual target");
        loader.Validate(301, base.wstring(), {L"folder.lnk"}, PickerMode::Image);
        result = WaitValidation(window);
        Check(result && result->navigate == (base / L"folder").wstring(), "directory shortcut navigates in image mode");
        loader.Validate(310, base.wstring(), {L"folder.lnk"}, PickerMode::Folder);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1 && result->paths[0] == (base / L"folder").wstring(), "folder shortcut confirms actual directory");
        loader.Validate(311, base.wstring(), {L"file.lnk"}, PickerMode::Folder);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_UNSUPPORTED_TYPE, "folder mode rejects shortcut to file");
        PickerEntry folder_shortcut{L"folder.lnk", (base / L"folder.lnk").wstring(), PickerEntryKind::File};
        Check(PickerChosenPath(PickerMode::Folder, base.wstring(), &folder_shortcut) == folder_shortcut.path, "folder primary validates shortcut rather than selecting current directory");
        Check(PickerShowsEntry(FILE_ATTRIBUTE_NORMAL, L"folder.lnk", PickerMode::Folder), "folder mode shows shortcuts for validation");
        loader.Validate(302, base.wstring(), {L"image.lnk"}, PickerMode::Image);
        result = WaitValidation(window);
        Check(result && result->paths.size() == 1 && result->paths[0] == (base / L"b.PNG").wstring(), "image shortcut validates destination type");
        loader.Validate(303, base.wstring(), {L"file.lnk"}, PickerMode::Image);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_UNSUPPORTED_TYPE, "shortcut cannot bypass image restriction");
        loader.Validate(304, base.wstring(), {L"dead.lnk"}, PickerMode::File);
        result = WaitValidation(window);
        Check(result && result->error == ERROR_FILE_NOT_FOUND, "dead shortcut fails without target tracking");
        // Shell refuses cyclic/deep shortcut fixtures on this system. Exercise
        // our actual traversal with deterministic targets, retaining COM-backed
        // file, directory, image and dead-link integration cases above.
        std::map<std::wstring, std::wstring> graph{{L"a.lnk", L"b.lnk"}, {L"b.lnk", L"a.lnk"}};
        auto read_next = [&](std::wstring& current, bool& followed) -> DWORD {
            const auto found = graph.find(current);
            followed = found != graph.end();
            if (followed) current = found->second;
            return ERROR_SUCCESS;
        };
        std::wstring chain_path = L"a.lnk";
        Check(FollowPickerShortcutChain(chain_path, read_next) == ERROR_CANT_RESOLVE_FILENAME, "shortcut traversal rejects deterministic cycle");
        graph.clear();
        for (int hop = 0; hop < 16; ++hop) graph[std::to_wstring(hop)] = std::to_wstring(hop + 1);
        chain_path = L"0";
        Check(FollowPickerShortcutChain(chain_path, read_next) == ERROR_SUCCESS && chain_path == L"16", "shortcut traversal accepts sixteen hops");
        graph[L"16"] = L"17";
        chain_path = L"0";
        Check(FollowPickerShortcutChain(chain_path, read_next) == ERROR_CANT_RESOLVE_FILENAME, "shortcut traversal rejects seventeenth hop");
        chain_path = L"0";
        Check(FollowPickerShortcutChain(chain_path, read_next, [] { return true; }) == ERROR_CANCELLED, "shortcut traversal cancels before reader");
        const auto listing = ReadPickerListing(base.wstring(), PickerMode::Image);
        bool only_images = listing.error == ERROR_SUCCESS;
        for (const auto& entry : listing.entries) if (entry.kind != PickerEntryKind::Folder && entry.name != L"b.PNG" && entry.name.find(L".lnk") == std::wstring::npos) only_images = false;
        Check(only_images, "image listing keeps navigable shortcuts and filters unrelated files");
    }
    MSG leftover{};
    Check(!PeekMessageW(&leftover, window, kListing, kValidation, PM_REMOVE), "close drains posted payloads");
    const auto before_close = GetTickCount64();
    {
        PickerLoader closing(window, kListing, kValidation);
        for (uint64_t generation = 0; generation < 100; ++generation) closing.Load(generation, base.wstring(), PickerMode::File);
    }
    Check(GetTickCount64() - before_close < 1000, "close never joins background worker");
    DestroyWindow(window);
    std::error_code cleanup_error;
    std::filesystem::remove_all(cleanup.path, cleanup_error);
    Check(!cleanup_error && !std::filesystem::exists(base), "isolated fixture cleanup");
    std::cout << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
