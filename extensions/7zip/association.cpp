#include "association.h"
#include "takeover_record.h"
#include "text.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace association {
namespace {
using T = text::Id;
struct Extension { const wchar_t* name; int group; int icon; const char* label; };
// Resource ids follow plugin.rc (icons/*.ico in the 「文档」 style).
constexpr Extension kExtensions[] = {
    {L".zip", 0, 101, "ZIP"}, {L".zipx", 1, 102, "ZIPX"}, {L".7z", 2, 103, "7Z"}, {L".rar", 3, 104, "RAR"},
    {L".tar", 4, 105, "TAR"}, {L".gz", 5, 106, "GZ"}, {L".tgz", 5, 106, "TGZ"}, {L".xz", 6, 107, "XZ"},
    {L".txz", 6, 107, "TXZ"}, {L".bz2", 7, 108, "BZ2"}, {L".tbz2", 7, 108, "TBZ2"}, {L".tbz", 7, 108, "TBZ"},
    {L".lzma", 8, 109, "LZMA"}, {L".z", 9, 110, "Z"}, {L".cab", 10, 111, "CAB"}, {L".lzh", 11, 112, "LZH"},
    {L".lha", 11, 113, "LHA"}, {L".arj", 11, 114, "ARJ"}, {L".cpio", 11, 115, "CPIO"},
};
struct Group { const char* label; const char* extensions; };
constexpr Group kGroups[kGroupCount] = {
    {"ZIP", ".zip"}, {"ZIPX", ".zipx"}, {"7Z", ".7z"}, {"RAR", ".rar"}, {"TAR", ".tar"}, {"GZ", ".gz .tgz"},
    {"XZ", ".xz .txz"}, {"BZ2", ".bz2 .tbz2 .tbz"}, {"LZMA", ".lzma"}, {"Z", ".z"}, {"CAB", ".cab"},
    {"LZH \xC2\xB7 ARJ \xC2\xB7 CPIO", ".lzh .lha .arj .cpio"},
};

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}
std::wstring ModulePath(HMODULE module) {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n && n < path.size() ? n : 0);
    return path;
}
HMODULE ThisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModule), &module);
    return module;
}
bool SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
bool Same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool Missing(HRESULT result) {
    return result == HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION) || result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
// The extension's effective default ProgID, as Windows resolves it (UserChoice included).
HRESULT CurrentDefault(const wchar_t* extension, std::wstring& progid) {
    progid.clear();
    Microsoft::WRL::ComPtr<IApplicationAssociationRegistration> registration;
    HRESULT result = CoCreateInstance(__uuidof(ApplicationAssociationRegistration), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(registration.GetAddressOf()));
    if (FAILED(result)) return result;
    PWSTR value = nullptr;
    result = registration->QueryCurrentDefault(extension, AT_FILEEXTENSION, AL_EFFECTIVE, &value);
    if (SUCCEEDED(result) && value) progid = value;
    CoTaskMemFree(value);
    return result;
}
std::wstring FriendlyApp(const wchar_t* extension, const std::wstring& progid) {
    wchar_t name[260]{};
    DWORD length = ARRAYSIZE(name);
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYAPPNAME, extension, nullptr, name, &length)) && name[0])
        return name;
    return progid;
}
bool WindowsViewer(const std::wstring& progid) {
    return Same(progid, L"CompressedFolder") || Same(progid, L"CABFolder") || Same(progid, L"ArchiveFolder");
}
// The user picked a default in Windows (始终 in the chooser, or Default apps): only they may change it.
bool UserChose(const wchar_t* extension) {
    const std::wstring key = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\") + extension + L"\\UserChoice";
    return !takeover::ReadString(HKEY_CURRENT_USER, key, L"ProgId").empty();
}
struct Com {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    ~Com() { if (SUCCEEDED(result)) CoUninitialize(); }
};
std::wstring TypeName(const Extension& extension) {
    if (extension.group == 10) return Wide(text::Get(T::TypeCab));
    return Wide(text::Format(extension.group == 4 ? T::TypeBundle : T::TypeArchive, extension.label));
}
} // namespace

const char* GroupLabel(int group) { return group >= 0 && group < kGroupCount ? kGroups[group].label : ""; }
const char* GroupExtensions(int group) { return group >= 0 && group < kGroupCount ? kGroups[group].extensions : ""; }
int GroupIcon(int group) {
    for (const auto& extension : kExtensions)
        if (extension.group == group) return extension.icon;
    return 0;
}
std::wstring IconLibrary() { return ModulePath(ThisModule()); }

std::vector<Status> Query(unsigned groups) {
    Com com;
    std::vector<Status> result(kGroupCount);
    std::vector<bool> free(kGroupCount, false), pulse(kGroupCount, false);
    for (const auto& extension : kExtensions) {
        auto& status = result[static_cast<size_t>(extension.group)];
        std::wstring progid;
        const HRESULT hr = CurrentDefault(extension.name, progid);
        if (SUCCEEDED(hr) && Same(progid, takeover::ProgId(extension.name))) pulse[static_cast<size_t>(extension.group)] = true;
        else if (FAILED(hr) || progid.empty()) free[static_cast<size_t>(extension.group)] = true;
        else if (status.who.empty()) {
            status.who = FriendlyApp(extension.name, progid);
            status.windows = WindowsViewer(progid);
        }
    }
    for (int group = 0; group < kGroupCount; ++group) {
        auto& status = result[static_cast<size_t>(group)];
        if (!((groups >> group) & 1)) status.owner = Owner::Off;
        else if (!status.who.empty()) status.owner = Owner::Other;
        else if (free[static_cast<size_t>(group)]) status.owner = Owner::Free;
        else status.owner = Owner::Pulse;
    }
    return result;
}

bool Apply(unsigned groups, bool extract) {
    groups &= kAllGroups;
    if (!groups) { Remove(); return true; }
    Com com;
    const auto exe = ModulePath(nullptr), dll = ModulePath(ThisModule());
    if (exe.empty() || dll.empty()) return false;
    const std::wstring command = L"\"" + exe + L"\" \"" + (extract ? takeover::kExtractLaunch : takeover::kPreviewLaunch) + L"%1\"";
    const auto verb = Wide(text::Get(extract ? T::CmdSmart : T::VerbPreview));
    const std::wstring capabilities = takeover::kCapabilitiesKey;
    bool ok = SetString(capabilities, L"ApplicationName", takeover::kAppName) &&
              SetString(capabilities, L"ApplicationDescription", Wide(text::Get(T::AppDescription))) &&
              SetString(capabilities, L"ApplicationIcon", exe + L",0") &&
              SetString(takeover::kRegisteredApps, takeover::kAppName, capabilities);
    for (const auto& extension : kExtensions) {
        if (!((groups >> extension.group) & 1)) {
            takeover::RemoveExtension(extension.name);
            continue;
        }
        const auto progid = takeover::ProgId(extension.name), key = takeover::ClassesKey(progid);
        const auto ext_key = takeover::ClassesKey(extension.name);
        const auto type = TypeName(extension);
        ok = SetString(key, nullptr, type) && SetString(key, L"FriendlyTypeName", type) &&
             SetString(key + L"\\DefaultIcon", nullptr, dll + L",-" + std::to_wstring(extension.icon)) &&
             SetString(key + L"\\shell", nullptr, L"open") && SetString(key + L"\\shell\\open", nullptr, verb) &&
             SetString(key + L"\\shell\\open\\command", nullptr, command) &&
             RegSetKeyValueW(HKEY_CURRENT_USER, (ext_key + L"\\OpenWithProgids").c_str(), progid.c_str(), REG_NONE, nullptr, 0) == ERROR_SUCCESS &&
             SetString(capabilities + L"\\FileAssociations", extension.name, progid) && ok;
        // Pulse becomes the default only where no archiver is: nothing opens the extension, or only Windows'
        // own archive folder does and the user never chose it. A program's or the user's choice is never replaced.
        DWORD claimed = 0;
        const auto user_default = takeover::ReadString(HKEY_CURRENT_USER, ext_key, nullptr);
        std::wstring current;
        const HRESULT found = CurrentDefault(extension.name, current);
        const bool unclaimed = Missing(found) || (SUCCEEDED(found) && WindowsViewer(current) && !UserChose(extension.name));
        if (Same(user_default, progid)) claimed = 1;
        else if (user_default.empty() && unclaimed && SetString(ext_key, nullptr, progid)) claimed = 1;
        ok = RegSetKeyValueW(HKEY_CURRENT_USER, takeover::kRecordKey, extension.name, REG_DWORD, &claimed, sizeof(claimed)) == ERROR_SUCCESS && ok;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    return ok;
}

void Remove() {
    for (const auto& extension : kExtensions) takeover::RemoveExtension(extension.name);  // also unrecorded leftovers
    if (!takeover::RemoveAll()) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
}

void Choose(HWND owner, int group) {
    for (const auto& extension : kExtensions) {
        if (extension.group != group) continue;
        // Windows asks how files of this type should open; 「始终」 makes it the user's own choice.
        const std::wstring sample = std::wstring(L"archive") + extension.name;
        OPENASINFO info{sample.c_str(), nullptr, OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT};
        SHOpenWithDialog(owner, &info);
        return;
    }
}

void OpenDefaultApps() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Pulse", nullptr, nullptr, SW_SHOWNORMAL);
}
} // namespace association
