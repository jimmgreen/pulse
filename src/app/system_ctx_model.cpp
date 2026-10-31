#include "system_ctx_model.h"
#include "system_ctx_packaged.h"

#include "../common/localization.h"
#include "../ipc/ctx_menu_util.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <unordered_map>

namespace pulse::app {
namespace {

using sysctx::Action;
using sysctx::Item;
using sysctx::Kind;
using sysctx::Op;
using sysctx::OpResult;
using sysctx::Root;

const wchar_t* KindGlyph(Kind kind) {
    switch (kind) {
    case Kind::ShellEx: return L"\xEA86";   // puzzle: shell extension DLL
    case Kind::DragDrop: return L"\xE8B0";
    case Kind::ShellNew: return L"\xE710";
    case Kind::SendTo: return L"\xE724";
    case Kind::OpenWith: return L"\xE7AC";
    default: return L"\xE756";
    }
}

bool Toggleable(const Item& item) {
    return sysctx::SystemItemToggleable(item);
}

// 删除残留项: the program behind the entry is gone and its key is one Pulse
// may write (the switch's own rules).
bool Deletable(const Item& item) { return item.missing && Toggleable(item); }

std::wstring Expand(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    wchar_t buffer[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), buffer, ARRAYSIZE(buffer));
    return n > 0 && n <= ARRAYSIZE(buffer) ? std::wstring(buffer) : s;
}

bool HasExtension(const std::wstring& path, std::wstring_view ext) {
    if (path.size() <= ext.size()) return false;
    return _wcsicmp(path.c_str() + path.size() - ext.size(), std::wstring(ext).c_str()) == 0;
}

// An .exe/.ico the shell icon cache can draw: the Icon value first ("x.exe,0"),
// then the program itself. DLL resources keep the kind glyph.
std::wstring IconFile(const Item& item) {
    auto usable = [](std::wstring path) -> std::wstring {
        path = Expand(path);
        if (!(HasExtension(path, L".exe") || HasExtension(path, L".ico"))) return {};
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY) ? path : std::wstring();
    };
    if (!item.icon.empty()) {
        std::wstring icon = item.icon;
        if (!icon.empty() && icon[0] == L'"') {
            const size_t close = icon.find(L'"', 1);
            icon = icon.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
        } else if (const size_t comma = icon.rfind(L','); comma != std::wstring::npos) {
            icon = icon.substr(0, comma);
        }
        if (auto path = usable(icon); !path.empty()) return path;
    }
    if (item.kind == Kind::SendTo) return {};
    return item.file.empty() ? std::wstring() : usable(item.file);
}

std::wstring Timestamp(bool file_name) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t buffer[40];
    swprintf_s(buffer, file_name ? L"%04u%02u%02u-%02u%02u%02u" : L"%04u-%02u-%02uT%02u:%02u:%02u", t.wYear,
               t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buffer;
}

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool WriteBytes(const std::wstring& path, const void* data, size_t size, bool append) {
    HANDLE f = CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = size == 0 || (WriteFile(f, data, static_cast<DWORD>(size), &written, nullptr) && written == size);
    CloseHandle(f);
    return ok;
}

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    for (const wchar_t c : s) {
        if (c == L'"' || c == L'\\') { out += L'\\'; out += c; }
        else if (c < 0x20) { wchar_t b[8]; swprintf_s(b, L"\\u%04x", static_cast<unsigned>(c)); out += b; }
        else out += c;
    }
    return out;
}

std::wstring HelperPath() {
    wchar_t module[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    if (n == 0 || n >= ARRAYSIZE(module)) return {};
    std::wstring path(module, n);
    const size_t slash = path.rfind(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1) + L"pulse_integration.exe";
}


// "CompanyName" / "FileDescription" from the version resource.
std::wstring VersionString(const std::vector<BYTE>& block, const wchar_t* name) {
    struct LangCodePage { WORD language; WORD codepage; };
    LangCodePage* translate = nullptr;
    UINT bytes = 0;
    std::vector<std::wstring> queries;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translate), &bytes) &&
        translate && bytes >= sizeof(LangCodePage)) {
        wchar_t q[96];
        swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\%s", translate[0].language, translate[0].codepage, name);
        queries.emplace_back(q);
    }
    queries.push_back(std::wstring(L"\\StringFileInfo\\040904b0\\") + name);
    queries.push_back(std::wstring(L"\\StringFileInfo\\080404b0\\") + name);
    for (const auto& q : queries) {
        wchar_t* value = nullptr;
        UINT len = 0;
        if (VerQueryValueW(block.data(), q.c_str(), reinterpret_cast<void**>(&value), &len) && value && len > 1 &&
            value[0]) {
            std::wstring s(value);
            while (!s.empty() && (s.back() == L' ' || s.back() == L'\0')) s.pop_back();
            return s;
        }
    }
    return {};
}

void ReadVersionInfo(const std::wstring& path, std::wstring& company, std::wstring& description,
                     std::wstring& product) {
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size == 0) return;
    std::vector<BYTE> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return;
    company = VersionString(block, L"CompanyName");
    description = VersionString(block, L"FileDescription");
    product = VersionString(block, L"ProductName");
}

bool ContainsNoCase(const std::wstring& text, std::wstring_view needle) {
    if (needle.empty() || text.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= text.size(); ++i)
        if (_wcsnicmp(text.c_str() + i, needle.data(), needle.size()) == 0) return true;
    return false;
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool IsWindowsFile(const std::wstring& path) {
    static const std::wstring windows = [] {
        wchar_t dir[MAX_PATH];
        const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::wstring(dir, n) + L"\\" : std::wstring(L"C:\\Windows\\");
    }();
    return path.size() > windows.size() && _wcsnicmp(path.c_str(), windows.c_str(), windows.size()) == 0;
}

bool IsMicrosoft(const std::wstring& publisher, const std::wstring& file) {
    return ContainsNoCase(publisher, L"Microsoft") || (publisher.empty() && !file.empty() && IsWindowsFile(file));
}

const wchar_t* KindLabel(Kind kind) {
    switch (kind) {
    case Kind::ShellEx: return l10n::Get(l10n::StringId::ShellExtension).c_str();
    case Kind::DragDrop: return l10n::Get(l10n::StringId::DragAndDropExtension).c_str();
    case Kind::ShellNew: return l10n::Get(l10n::StringId::NewItemTemplate).c_str();
    case Kind::SendTo: return l10n::Get(l10n::StringId::SendToTarget).c_str();
    case Kind::OpenWith: return l10n::Get(l10n::StringId::OpenWithProgram).c_str();
    default: return l10n::Get(l10n::StringId::MenuCommand).c_str();
    }
}

struct KnownName {
    const wchar_t* key;
    const wchar_t* zh;
    const wchar_t* en;
};

// Windows' own verbs carry no menu text in the registry (Explorer names them
// from shell32), so the key name ("removeproperties") is all a scan finds.
constexpr KnownName kVerbNames[] = {
    {L"open", L"打开", L"Open"},
    {L"opennew", L"打开", L"Open"},
    {L"edit", L"编辑", L"Edit"},
    {L"print", L"打印", L"Print"},
    {L"printto", L"打印到", L"Print to"},
    {L"runas", L"以管理员身份运行", L"Run as administrator"},
    {L"runasuser", L"以其他用户身份运行", L"Run as a different user"},
    {L"opennewwindow", L"在新窗口中打开", L"Open in new window"},
    {L"opennewprocess", L"在新进程中打开", L"Open in new process"},
    {L"opennewtab", L"在新标签页中打开", L"Open in new tab"},
    {L"explore", L"在资源管理器中浏览", L"Explore"},
    {L"find", L"搜索", L"Search"},
    {L"cmd", L"在此处打开命令窗口", L"Open command window here"},
    {L"powershell", L"在此处打开 PowerShell 窗口", L"Open PowerShell window here"},
    {L"pintohome", L"固定到快速访问", L"Pin to Quick access"},
    {L"pintostartscreen", L"固定到“开始”屏幕", L"Pin to Start"},
    {L"removeproperties", L"删除属性和个人信息", L"Remove properties and personal information"},
    {L"properties", L"属性", L"Properties"},
    {L"play", L"播放", L"Play"},
    {L"enqueue", L"添加到播放列表", L"Add to playlist"},
    {L"preview", L"预览", L"Preview"},
    {L"setdesktopwallpaper", L"设置为桌面背景", L"Set as desktop background"},
    {L"rotateleft", L"向左旋转", L"Rotate left"},
    {L"rotateright", L"向右旋转", L"Rotate right"},
    {L"openas", L"打开方式", L"Open with"},
    {L"install", L"安装", L"Install"},
    {L"mount", L"装载", L"Mount"},
    {L"burn", L"刻录光盘映像", L"Burn disc image"},
    {L"3d edit", L"使用画图 3D 进行编辑", L"Edit with Paint 3D"},
    {L"3d print", L"3D 打印", L"3D print"},
    {L"wsl", L"在此处打开 Linux shell", L"Open Linux shell here"},
};

// Windows' own shell extensions, by handler key name: the menu rows they add.
constexpr KnownName kHandlerNames[] = {
    {L"EPP", L"使用 Microsoft Defender 扫描", L"Scan with Microsoft Defender"},
    {L"Sharing", L"授予访问权限", L"Give access to"},
    {L"ModernSharing", L"共享", L"Share"},
    {L"Open With", L"打开方式", L"Open with"},
    {L"Open With EncryptionMenu", L"打开方式（加密文件）", L"Open with (encrypted files)"},
    {L"OpenWith", L"打开方式", L"Open with"},
    {L"PintoStartScreen", L"固定到“开始”屏幕", L"Pin to Start"},
    {L"Library Location", L"包含到库中", L"Include in library"},
    {L"SendTo", L"发送到", L"Send to"},
    {L"Offline Files", L"始终脱机可用", L"Always available offline"},
    {L"WorkFolders", L"工作文件夹", L"Work Folders"},
    {L"Compatibility", L"兼容性疑难解答", L"Troubleshoot compatibility"},
    {L"CopyAsPathMenu", L"复制文件地址", L"Copy as path"},
    {L"{596AB062-B4D2-4215-9F74-E9109B0A8153}", L"还原以前的版本", L"Restore previous versions"},
    {L"{90AA3A4E-1CBA-4233-B8BB-535773D48449}", L"固定到任务栏", L"Pin to taskbar"},
    {L"{a2a9545d-a0c2-42b4-9708-a0b2badd77c8}", L"固定到“开始”菜单", L"Pin to Start menu"},
    {L"{7AD84985-87B4-4a16-BE58-8B72A5B390F7}", L"投送到设备", L"Cast to Device"},
};

const wchar_t* LookUpName(const KnownName* names, size_t count, const std::wstring& key) {
    if (key.empty()) return nullptr;
    for (size_t i = 0; i < count; ++i)
        if (_wcsicmp(names[i].key, key.c_str()) == 0) return l10n::Pick(names[i].zh, names[i].en);
    return nullptr;
}

std::wstring UpperClsid(const std::wstring& clsid) {
    std::wstring out = clsid;
    for (auto& c : out) c = static_cast<wchar_t>(std::towupper(c));
    return out;
}

// Menu text as a title: no "&", no trailing access key ("全部解压缩(&T)..."
// -> "全部解压缩..."; "Extract(E)" -> "Extract").
std::wstring StripAccessKey(std::wstring text) {
    text.erase(std::remove(text.begin(), text.end(), L'&'), text.end());
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == L'.' || text[end - 1] == L'\u2026')) --end;
    if (end >= 3 && text[end - 1] == L')' && text[end - 3] == L'(' && std::iswalnum(text[end - 2]) && text[end - 2] < 0x80) {
        size_t cut = end - 3;
        while (cut > 0 && text[cut - 1] == L' ') --cut;
        if (cut > 0) text.erase(cut, 3 + (end - 3 - cut));
    }
    return text;
}

bool HasNonAscii(const std::wstring& text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= 0x80; });
}

void TrimSpaces(std::wstring& text) {
    const auto blank = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\u3000'; };
    while (!text.empty() && blank(text.back())) text.pop_back();
    size_t lead = 0;
    while (lead < text.size() && blank(text[lead])) ++lead;
    text.erase(0, lead);
}

// ComHandlerLabels shows the item's name as "…" (压缩为 "… (2).zip"). A
// trailing "…" is the menu's own ellipsis (添加到压缩文件…); any other one
// stands for the file name.
bool TemplatedLabel(const std::wstring& text) {
    const size_t at = text.find(L'\u2026');
    return at != std::wstring::npos && at + 1 < text.size();
}

// The command without the file name it was built for: 压缩为 "… (2).zip" ->
// 压缩为…, 解压到 …\ -> 解压到…
std::wstring UntemplateLabel(std::wstring text) {
    if (!TemplatedLabel(text)) return text;
    static constexpr std::pair<wchar_t, wchar_t> kQuotes[] = {
        {L'"', L'"'}, {L'\u201C', L'\u201D'}, {L'\u300C', L'\u300D'}, {L'\'', L'\''}, {L'\u2018', L'\u2019'}};
    for (const auto& [open, close] : kQuotes) {
        size_t start = 0;
        while ((start = text.find(open, start)) != std::wstring::npos) {
            const size_t end = text.find(close, start + 1);
            if (end == std::wstring::npos) break;
            const size_t mark = text.find(L'\u2026', start);
            if (mark < end) text.erase(start, end - start + 1);
            else start = end + 1;
        }
    }
    // Unquoted: the word holding the name goes from the placeholder on.
    const size_t mark = text.find(L'\u2026');
    if (mark != std::wstring::npos) {
        size_t word = text.find_last_of(L' ', mark);
        word = word == std::wstring::npos ? 0 : word + 1;
        text.erase(word == mark ? word : mark);
    }
    TrimSpaces(text);
    while (!text.empty() && (text.back() == L':' || text.back() == L'\uFF1A' || text.back() == L'(' ||
                             text.back() == L'\uFF08' || text.back() == L'-'))
        text.pop_back();
    TrimSpaces(text);
    if (!text.empty()) text += L'\u2026';
    return text;
}

// "新建压缩包 (Bandizip)...." / "Add to archive..." -> one "…".
std::wstring TidyEllipsis(std::wstring text) {
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == L'.' || text[end - 1] == L'\u2026')) --end;
    if (end < text.size() && end > 0) {
        text.erase(end);
        TrimSpaces(text);
        text += L'\u2026';
    }
    return text;
}

// Programs whose English names say little to a Chinese reader.
struct Brand {
    const wchar_t* match;  // case-insensitive, inside the program's names
    const wchar_t* zh;
};
constexpr Brand kBrands[] = {
    {L"Quark", L"夸克"},           {L"Fantai", L"泛泰快传"},    {L"BaiduNetdisk", L"百度网盘"},
    {L"Baidu Netdisk", L"百度网盘"}, {L"Huorong", L"火绒"},       {L"WeChat", L"微信"},
    {L"WeiXin", L"微信"},          {L"TencentDocs", L"腾讯文档"}, {L"Xunlei", L"迅雷"},
    {L"Youdao", L"有道"},          {L"AliyunDrive", L"阿里云盘"}, {L"DingTalk", L"钉钉"},
    {L"Feishu", L"飞书"},          {L"Sogou", L"搜狗"},
    // ProductName "BdzShell": the DLL's name, not the program's.
    {L"Bdz", L"Bandizip"},         {L"HaoZip", L"好压"},        {L"360zip", L"360压缩"},
    {L"KuaiZip", L"快压"},
};

// "Bandizip shell menu dll" / "QuarkAI.ContextMenu" / "IconExtract Module":
// the program name with the plumbing words taken out.
std::wstring StripPlumbing(std::wstring text) {
    static constexpr const wchar_t* kWords[] = {
        L"context menu handler", L"contextmenuhandler", L"context menu", L"contextmenu", L"shell extension handler",
        L"shell extensions", L"shell extension", L"shellextension", L"shell menu", L"shellmenu", L"shell ext",
        L"shellext", L"extension", L"handler", L"module", L"library", L"dll", L"com server", L"x64", L"64-bit",
        L"32-bit"};
    for (const wchar_t* word : kWords) {
        const size_t n = wcslen(word);
        for (size_t at = 0; at + n <= text.size();) {
            if (_wcsnicmp(text.c_str() + at, word, n) == 0) text.erase(at, n);
            else ++at;
        }
    }
    for (auto& c : text)
        if (c == L'.' || c == L'_') c = L' ';
    std::wstring out;
    for (wchar_t c : text)
        if (!(c == L' ' && (out.empty() || out.back() == L' '))) out += c;
    TrimSpaces(out);
    while (!out.empty() && (out.back() == L'-' || out.back() == L',')) out.pop_back();
    TrimSpaces(out);
    return out;
}

// Which program a third-party handler belongs to, for "<程序> 的右键菜单":
// a known Chinese name, else ProductName, else the DLL's description, the
// CLSID's name, the handler key or the file, each without the plumbing words.
// "夸克的右键菜单", "Bandizip 的右键菜单": a space only after a Latin name.
std::wstring Possessive(const std::wstring& program, const wchar_t* zh_tail, const wchar_t* en_tail) {
    const bool chinese = std::wstring_view(l10n::Get(l10n::StringId::En).c_str()) == L"zh";
    if (!chinese) return program + L" " + en_tail;
    return program + (!program.empty() && program.back() >= 0x2E80 ? L"" : L" ") + zh_tail;
}

std::wstring ProgramName(const std::wstring& product, const std::wstring& description, const std::wstring& display,
                         const std::wstring& key_name, const std::wstring& file) {
    const std::wstring stem = [&] {
        std::wstring name = FileName(file);
        const size_t dot = name.find_last_of(L'.');
        return dot == std::wstring::npos ? name : name.substr(0, dot);
    }();
    for (const Brand& brand : kBrands)
        for (const std::wstring* name : {&product, &description, &display, &key_name, &stem})
            if (ContainsNoCase(*name, brand.match)) return brand.zh;
    // ProductName is sometimes just the DLL's own name ("BdzShell"): a name
    // that repeats the handler key or the file stem waits for the second pass.
    const auto internal = [&](const std::wstring& text) {
        return _wcsicmp(text.c_str(), key_name.c_str()) == 0 || _wcsicmp(text.c_str(), stem.c_str()) == 0;
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (const std::wstring* name : {&product, &description, &display, &key_name, &stem}) {
            if (name->empty() || name->front() == L'{') continue;
            if (pass == 0 && internal(*name)) continue;
            std::wstring text = StripPlumbing(*name);
            if (text.size() >= 2) return text;
        }
    }
    return {};
}

// ProductName says which program a DLL belongs to, unless it is Windows itself.
bool GenericProduct(const std::wstring& product) {
    return product.empty() || ContainsNoCase(product, L"Operating System") || ContainsNoCase(product, L"操作系统") ||
           product.front() == L'{';
}

std::wstring MethodText(const Item& item, bool toggleable) {
    if (item.source == sysctx::Source::Packaged) return l10n::Get(l10n::StringId::PackagedMenuReadOnly);
    if (item.trusted_installer)
        return l10n::Get(l10n::StringId::WindowsManagesThisEntryItselfTrustedInstallerPulseLeavesItAl).c_str();
    if (item.is_protected)
        return l10n::Get(l10n::StringId::ABasicWindowsCommandSwitchingItOffBreaksFileExplorerSoThereI).c_str();
    if (!toggleable)
        return l10n::Get(l10n::StringId::SwitchesForNewSendToAndOpenWithComeInALaterVersionForNowYouC).c_str();
    std::wstring text = item.kind == Kind::Verb
        ? l10n::Get(l10n::StringId::SwitchedOffTheCommandLeavesTheContextMenuPulseOnlyAddsHideMa).c_str()
        : l10n::Get(l10n::StringId::SwitchedOffWindowsStopsLoadingTheExtensionAndItsRowsLeaveThe).c_str();
    if (Deletable(item))
        text += l10n::Get(l10n::StringId::ItsProgramIsGoneUsuallyLeftBehindByAnUninstallerSoTheEntryCa).c_str();
    text += l10n::Get(l10n::StringId::NothingIsWrittenUntilYouChooseApplyChanges).c_str();
    return text;
}

std::wstring SceneKeys(const sysctx::Scene& scene) {
    switch (scene.mode) {
    case sysctx::SceneMode::ShellNew: return L"HKCR\\.ext\\[ProgID\\]ShellNew";
    case sysctx::SceneMode::SendTo: return L"%APPDATA%\\Microsoft\\Windows\\SendTo";
    case sysctx::SceneMode::OpenWith: return L"HKCR\\Applications\\*.exe";
    case sysctx::SceneMode::DragDrop: return L"HKCR\\…\\shellex\\DragDropHandlers";
    default: break;
    }
    std::wstring out;
    for (const auto& key : scene.class_keys) {
        if (!out.empty()) out += L" · ";
        out += L"HKCR\\" + key;
    }
    return out;
}

// The key ContextMenuPrefs files the entry under in Pulse's own menu: shell
// extensions by CLSID, static verbs by their menu text (as Pulse sees it).
std::wstring PulseKey(const Item& item) {
    if (item.kind == Kind::ShellEx) return item.clsid.empty() ? std::wstring() : ipc::HandlerCatalogKey(item.clsid);
    if (item.kind == Kind::Verb && !item.display.empty()) return ipc::CatalogKey(item.display, false);
    return {};
}

} // namespace

SystemCtxModel::SystemCtxModel() {
    // Flat scene ids; the scene list (nav_) groups them like the prototype.
    scene_ids_ = {L"file", L"allobj", L"folder", L"dirbg", L"desk", L"drive", L"pc", L"bin", L"lnk", L"exe",
                  L"shellnew", L"sendto", L"openwith", L"dragdrop"};
    static const wchar_t* const kExts[] = {L".txt", L".png", L".jpg", L".mp4", L".pdf", L".docx", L".zip"};
    static const wchar_t* const kPerceived[] = {L"image", L"video", L"audio", L"text"};
    const int first_ext = static_cast<int>(scene_ids_.size());
    for (const auto* ext : kExts) scene_ids_.push_back(std::wstring(L"ext:") + ext);
    const int first_perc = static_cast<int>(scene_ids_.size());
    for (const auto* type : kPerceived) scene_ids_.push_back(std::wstring(L"perc:") + type);

    nav_ = {
        {L"文件", L"Files", L"\xE7C3", 0, {0}, {}},
        {L"所有对象", L"All objects", L"\xE8C8", 0, {1}, {}},
        {L"文件夹", L"Folders", L"\xE838", 0, {2}, {}},
        {L"目录背景", L"Folder background", L"\xE8B7", 0, {3}, {}},
        {L"桌面背景", L"Desktop", L"\xE7F4", 0, {4}, {}},
        {L"磁盘", L"Drives", L"\xEDA2", 0, {5}, {}},
        {L"此电脑", L"This PC", L"\xE977", 0, {6}, {}},
        {L"回收站", L"Recycle Bin", L"\xE74D", 0, {7}, {}},
        {L"快捷方式 .lnk", L"Shortcuts .lnk", L"\xE71B", 1, {8}, {}},
        {L"程序 .exe", L"Programs .exe", L"\xE756", 1, {9}, {}},
        {L"按扩展名", L"By extension", L"\xE8EC", 1, {}, {}},
        {L"按感知类型", L"By perceived type", L"\xE890", 1, {}, {}},
        {L"新建", L"New", L"\xE710", 2, {10}, {}},
        {L"发送到", L"Send to", L"\xE724", 2, {11}, {}},
        {L"打开方式", L"Open with", L"\xE7AC", 2, {12}, {}},
        {L"拖放菜单", L"Right-drag", L"\xE8B0", 2, {13}, {}},
    };
    for (int i = 0; i < static_cast<int>(std::size(kExts)); ++i) {
        nav_[10].scenes.push_back(first_ext + i);
        nav_[10].subs.emplace_back(kExts[i]);
    }
    static const wchar_t* const kPercZh[] = {L"图片", L"视频", L"音频", L"文本"};
    static const wchar_t* const kPercEn[] = {L"Images", L"Videos", L"Audio", L"Text"};
    for (int i = 0; i < static_cast<int>(std::size(kPerceived)); ++i) {
        nav_[11].scenes.push_back(first_perc + i);
        nav_[11].subs.emplace_back(l10n::Pick(kPercZh[i], kPercEn[i]));
    }
    sub_index_.assign(nav_.size(), 0);

    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local)
        data_dir_ = std::wstring(local) + L"\\Pulse\\SystemContextMenu";
    CoTaskMemFree(local);
}

SystemCtxModel::~SystemCtxModel() {
    if (worker_.joinable()) worker_.join();
}

void SystemCtxModel::SetNotifyWindow(HWND hwnd) {
    std::lock_guard lock(mutex_);
    notify_ = hwnd;
}

void SystemCtxModel::SetDataDirectoryForTesting(std::wstring directory) {
    std::lock_guard lock(mutex_);
    data_dir_ = std::move(directory);
}

std::wstring SystemCtxModel::DataDirectory() const {
    std::lock_guard lock(mutex_);
    return data_dir_;
}

std::wstring SystemCtxModel::BackupDirectory() const {
    std::lock_guard lock(mutex_);
    return data_dir_.empty() ? std::wstring() : data_dir_ + L"\\Backups";
}

std::wstring SystemCtxModel::OpKey(Root root, Kind kind, const std::wstring& key_path) {
    std::wstring key = std::to_wstring(static_cast<int>(root)) + L"|" + std::to_wstring(static_cast<int>(kind)) + L"|";
    for (const wchar_t c : key_path) key += static_cast<wchar_t>(std::towlower(c));
    return key;
}

std::wstring SystemCtxModel::PendingKey(const Op& op) {
    std::wstring key = OpKey(op.root, op.kind, op.key_path);
    if (op.action == Action::Position) key += L"|pos";
    else if (op.action == Action::Delete || op.action == Action::Restore) key += L"|del";
    return key;
}

void SystemCtxModel::EnsureLoaded() {
    {
        std::lock_guard lock(mutex_);
        if (loaded_ || state_ != ui::SysCtxView::State::Idle) return;
        loaded_ = true;
    }
    StartJob(ui::SysCtxView::State::Loading, {}, 0, false);
}

void SystemCtxModel::Refresh() {
    StartJob(ui::SysCtxView::State::Loading, {}, 0, false);
}

void SystemCtxModel::StartJob(ui::SysCtxView::State state, std::vector<Op> ops, size_t pop_undo, bool record_undo) {
    std::lock_guard lock(mutex_);
    if (state_ == ui::SysCtxView::State::Loading || state_ == ui::SysCtxView::State::Applying) return;
    if (worker_.joinable()) worker_.join();  // finished: it cleared the state before exiting
    state_ = state;
    dirty_ = true;
    worker_ = std::thread([this, ops = std::move(ops), pop_undo, record_undo]() mutable {
        RunJob(std::move(ops), pop_undo, record_undo);
    });
}

SystemCtxModel::ScanResult SystemCtxModel::Scan() const {
    const sysctx::Roots roots;
    const auto packaged = shell::PackagedContextMenuVerbs();
    ScanResult out;
    out.entries.resize(scene_ids_.size());
    out.keys.resize(scene_ids_.size());
    struct VersionInfo {
        std::wstring company, description, product;
    };
    std::unordered_map<std::wstring, VersionInfo> versions;  // file -> its version strings
    for (size_t i = 0; i < scene_ids_.size(); ++i) {
        const std::wstring& id = scene_ids_[i];
        sysctx::Scene scene;
        if (id.rfind(L"ext:", 0) == 0) {
            scene = sysctx::ExtensionScene(roots, std::wstring_view(id).substr(4));
        } else if (id.rfind(L"perc:", 0) == 0) {
            scene = sysctx::PerceivedScene(std::wstring_view(id).substr(5));
        } else {
            for (const auto& builtin : sysctx::BuiltinScenes())
                if (builtin.id == id) scene = builtin;
        }
        out.keys[i] = SceneKeys(scene);
        if (scene.id.empty()) continue;
        auto items = sysctx::Enumerate(roots, scene);
        sysctx::AppendPackagedItems(scene, packaged, items);
        for (auto& item : items) {
            Entry e;
            e.icon = IconFile(item);
            if (!item.file.empty() && !item.missing) {
                auto it = versions.find(item.file);
                if (it == versions.end()) {
                    VersionInfo v;
                    ReadVersionInfo(item.file, v.company, v.description, v.product);
                    if (GenericProduct(v.product)) v.product.clear();
                    it = versions.emplace(item.file, std::move(v)).first;
                }
                e.publisher = it->second.company;
                e.description = it->second.description;
                e.product = it->second.product;
            }
            e.item = std::move(item);
            out.entries[i].push_back(std::move(e));
        }
    }
    return out;
}
std::wstring SystemCtxModel::WriteBackup(const std::vector<Op>& ops) const {
    const std::wstring dir = BackupDirectory();
    if (dir.empty() || SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) > ERROR_ALREADY_EXISTS) return {};
    // regedit reads REGEDIT5 files as UTF-16LE with a BOM.
    const std::wstring text = L"\xFEFF" + sysctx::ExportReg(sysctx::Roots{}, ops);
    std::wstring path = dir + L"\\" + Timestamp(true) + L".reg";
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; ++n)
        path = dir + L"\\" + Timestamp(true) + L"-" + std::to_wstring(n) + L".reg";
    return WriteBytes(path, text.data(), text.size() * sizeof(wchar_t), false) ? FileName(path) : std::wstring();
}

void SystemCtxModel::AppendLog(const std::vector<Op>& ops, const std::vector<OpResult>& results) const {
    std::wstring dir;
    {
        std::lock_guard lock(mutex_);
        dir = data_dir_;
    }
    if (dir.empty() || SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) > ERROR_ALREADY_EXISTS) return;
    std::wstring lines;
    const std::wstring time = Timestamp(false);
    static constexpr const wchar_t* kKinds[] = {L"verb", L"shellex", L"dragdrop", L"shellnew", L"sendto", L"openwith"};
    for (size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        lines += L"{\"time\":\"" + time + L"\",\"action\":\"" +
                 (op.action == Action::Disable  ? L"disable"
                  : op.action == Action::Enable ? L"enable"
                  : op.action == Action::Delete ? L"delete"
                  : op.action == Action::Restore ? L"restore"
                  : op.place == sysctx::Place::Top    ? L"position-top"
                  : op.place == sysctx::Place::Bottom ? L"position-bottom"
                                                      : L"position-default") +
                 L"\",\"kind\":\"" +
                 kKinds[static_cast<int>(op.kind)] + L"\",\"root\":\"" + (op.root == Root::Machine ? L"HKLM" : L"HKCU") +
                 L"\",\"key\":\"" + JsonEscape(op.key_path) + L"\",\"result\":\"" +
                 (i < results.size() ? sysctx::ResultName(results[i]) : L"skipped") + L"\"}\n";
    }
    const std::string bytes = Utf8(lines);
    WriteBytes(dir + L"\\ops.jsonl", bytes.data(), bytes.size(), true);
}

bool SystemCtxModel::RunElevated(const std::vector<Op>& ops, std::vector<OpResult>& results,
                                 std::wstring& error) const {
    results.assign(ops.size(), OpResult::Failed);
    const std::wstring helper = HelperPath();
    if (helper.empty() || GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = l10n::Get(l10n::StringId::PulseIntegrationExeIsMissing).c_str();
        return false;
    }
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return false;
    const std::wstring stem = std::wstring(temp) + L"pulse_sysctx_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(GetTickCount64());
    const std::wstring batch = stem + L".txt", result = stem + L".result";
    const std::string bytes = Utf8(sysctx::SerializeBatch(ops));
    if (!WriteBytes(batch, bytes.data(), bytes.size(), false)) return false;
    DeleteFileW(result.c_str());
    const std::wstring params = L"--system-ctx-apply \"" + batch + L"\" --result \"" + result + L"\"";
    HWND owner = nullptr;
    {
        std::lock_guard lock(mutex_);
        owner = notify_;
    }
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = helper.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        const DWORD e = GetLastError();
        DeleteFileW(batch.c_str());
        error = e == ERROR_CANCELLED ? l10n::Get(l10n::StringId::CancelledNoAdministratorPermissionNothingChanged).c_str()
                                     : l10n::Get(l10n::StringId::CouldnTStartTheAdministratorHelper).c_str();
        return false;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    CloseHandle(info.hProcess);
    std::wstring text;
    std::vector<OpResult> parsed;
    {
        HANDLE f = CreateFileW(result.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buffer[16384];
            DWORD read = 0;
            if (ReadFile(f, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
                const int n = MultiByteToWideChar(CP_UTF8, 0, buffer, static_cast<int>(read), nullptr, 0);
                text.resize(static_cast<size_t>(n));
                MultiByteToWideChar(CP_UTF8, 0, buffer, static_cast<int>(read), text.data(), n);
            }
            CloseHandle(f);
        }
    }
    DeleteFileW(batch.c_str());
    DeleteFileW(result.c_str());
    if (!sysctx::ParseResults(text, parsed) || parsed.size() != ops.size()) {
        error = l10n::Get(l10n::StringId::TheAdministratorHelperReturnedNoResult).c_str();
        return false;
    }
    results = std::move(parsed);
    return true;
}

void SystemCtxModel::RunJob(std::vector<Op> ops, size_t pop_undo, bool record_undo) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring message;
    bool error = false;
    std::vector<Op> done;
    std::wstring backup;  // 概览 · 最近的改动 names the .reg file
    bool cancelled = false;
    std::wstring title;   // named from the scan before the batch: a deleted entry leaves the next one
    if (record_undo && !ops.empty()) {
        std::lock_guard lock(mutex_);
        title = ChangeTitleLocked(ops);
    }
    if (!ops.empty()) {
        const sysctx::Roots roots;
        // Undo steps arrive planned (Inverse keeps the recorded markers);
        // re-planning them would lose e.g. a cleared CommandFlags bit.
        for (Op& op : ops)
            if (!op.planned) sysctx::PlanOp(roots, op);
        backup = WriteBackup(ops);
        std::vector<Op> user, machine;
        for (const Op& op : ops) (op.root == Root::Machine ? machine : user).push_back(op);
        std::vector<OpResult> user_results, machine_results;
        for (const Op& op : user) user_results.push_back(sysctx::Apply(roots, op));
        if (!user.empty()) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        std::wstring elevated_error;
        if (!machine.empty() && !RunElevated(machine, machine_results, elevated_error)) {
            cancelled = true;
            message = elevated_error;
            error = true;
        }
        AppendLog(user, user_results);
        if (!cancelled) AppendLog(machine, machine_results);
        int failed = 0;
        auto collect = [&](const std::vector<Op>& list, const std::vector<OpResult>& results) {
            for (size_t i = 0; i < list.size() && i < results.size(); ++i) {
                if (results[i] == OpResult::Ok) done.push_back(list[i]);
                else ++failed;
            }
        };
        collect(user, user_results);
        if (!cancelled) collect(machine, machine_results);
        if (!cancelled || !done.empty()) {
            if (failed > 0) {
                message = l10n::Get(l10n::StringId::Text2971).c_str() + std::to_wstring(failed) +
                          l10n::Get(l10n::StringId::ChangeSFailedProtectedOrChangedByAnotherProgramTheRestAreDon).c_str();
                error = true;
            } else if (!done.empty() && message.empty()) {
                message = (record_undo ? l10n::Get(l10n::StringId::Changed).c_str() : l10n::Get(l10n::StringId::Undid).c_str()) +
                          std::to_wstring(done.size()) +
                          l10n::Get(l10n::StringId::EntrIesTheRegistryWasBackedUpFirst).c_str();
            }
        }
    }
    ScanResult scanned = Scan();
    HWND notify = nullptr;
    {
        std::lock_guard lock(mutex_);
        entries_ = std::move(scanned.entries);
        scene_keys_ = std::move(scanned.keys);
        if (!ops.empty()) {
            // Queued entries that went through (or failed for good) leave the
            // queue; a cancelled UAC prompt keeps them for another try.
            for (const Op& op : ops) {
                if (op.root != Root::Machine || !cancelled) pending_.erase(PendingKey(op));
            }
            if (record_undo && !done.empty()) {
                undo_.push_back({done});
                const std::wstring time = Timestamp(false);  // yyyy-mm-ddThh:mm:ss
                Change change;
                change.title = done.size() == ops.size() && !title.empty() ? title : ChangeTitleLocked(done);
                change.caption = time.size() >= 16 ? time.substr(11, 5) : time;
                if (!backup.empty()) change.caption += L" · " + backup;
                changes_.push_back(std::move(change));
            }
            // Undo keeps its step until the reversal actually ran.
            if (!record_undo && !cancelled) {
                for (size_t n = 0; n < pop_undo && !undo_.empty(); ++n) {
                    undo_.pop_back();
                    // 概览 · 最近的改动: the newest change still standing is the one reversed.
                    const auto reversed = std::find_if(changes_.rbegin(), changes_.rend(),
                                                       [](const Change& c) { return !c.undone; });
                    if (reversed != changes_.rend()) reversed->undone = true;
                }
            }
            message_ = message;
            message_error_ = error;
        }
        state_ = ui::SysCtxView::State::Ready;
        dirty_ = true;
        notify = notify_;
    }
    if (SUCCEEDED(com)) CoUninitialize();
    // No lock from here on: StartJob joins this thread while holding it.
    if (notify) InvalidateRect(notify, nullptr, FALSE);
}

int SystemCtxModel::CurrentSceneLocked() const {
    const Nav& nav = nav_[static_cast<size_t>(nav_index_)];
    if (nav.scenes.empty()) return -1;
    const int sub = std::clamp(sub_index_[static_cast<size_t>(nav_index_)], 0, static_cast<int>(nav.scenes.size()) - 1);
    return nav.scenes[static_cast<size_t>(sub)];
}

std::shared_ptr<const ui::SysCtxView> SystemCtxModel::View() {
    std::lock_guard lock(mutex_);
    if (!dirty_ && view_) return view_;
    auto view = std::make_shared<ui::SysCtxView>();
    view->state = state_;
    view->scene = nav_index_;
    view->filter = filter_;
    view->side_tab = side_tab_;
    view->undo_steps = static_cast<int>(undo_.size());
    view->message = message_;
    view->message_error = message_error_;
    for (const auto& [key, op] : pending_) {
        ++view->pending;
        if (op.root == Root::Machine) ++view->pending_admin;
    }
    view->pending_pulse = static_cast<int>(pending_pulse_.size() + pending_skip_.size());
    view->pending += view->pending_pulse;
    auto pending_op = [&](const Item& item) -> const Op* {
        const auto it = pending_.find(OpKey(item.root, item.kind, item.key_path));
        return it == pending_.end() ? nullptr : &it->second;
    };
    auto pending_delete = [&](const Item& item) {
        return pending_.count(OpKey(item.root, item.kind, item.key_path) + L"|del") != 0;
    };
    auto pending_pos = [&](const Item& item) -> const Op* {
        const auto it = pending_.find(OpKey(item.root, item.kind, item.key_path) + L"|pos");
        return it == pending_.end() ? nullptr : &it->second;
    };
    auto effective_on = [&](const Item& item) {
        if (pending_delete(item)) return false;
        const Op* op = pending_op(item);
        return op ? op->action == Action::Enable : item.enabled;
    };
    auto zone_of = [](sysctx::Place place) {
        return place == sysctx::Place::Top ? ui::kSysCtxZoneTop
             : place == sysctx::Place::Bottom ? ui::kSysCtxZoneBottom : ui::kSysCtxZoneDefault;
    };
    auto effective_zone = [&](const Item& item) {
        if (item.kind != Kind::Verb) return static_cast<int>(ui::kSysCtxZoneDefault);
        const Op* op = pending_pos(item);
        return static_cast<int>(zone_of(op ? op->place : item.position));
    };
    auto default_hidden = [&](const Entry& e) {
        return !e.item.enabled && !e.item.dash_marker && e.item.root == Root::Machine &&
               (e.item.kind == Kind::Verb) && IsMicrosoft(e.publisher, e.item.file.empty() ? L"C:\\Windows\\x" : e.item.file);
    };
    auto third_party = [&](const Entry& e) {
        return !e.publisher.empty() ? !ContainsNoCase(e.publisher, L"Microsoft")
                                    : (!e.item.file.empty() && !IsWindowsFile(e.item.file)) || e.item.missing;
    };

    // Scene list: counts over every sub scene.
    std::vector<bool> scene_pending(scene_ids_.size(), false);
    for (size_t s = 0; s < entries_.size(); ++s)
        for (const Entry& e : entries_[s])
            if (pending_op(e.item) || pending_pos(e.item) || pending_delete(e.item)) scene_pending[s] = true;
    for (const Nav& nav : nav_) {
        ui::SysCtxSceneView scene;
        scene.label = l10n::Pick(nav.zh, nav.en);
        scene.glyph = nav.glyph;
        scene.group = nav.group;
        for (const int s : nav.scenes) {
            if (s >= static_cast<int>(entries_.size())) continue;
            for (const Entry& e : entries_[static_cast<size_t>(s)]) {
                ++scene.count;
                if (!e.item.enabled) ++scene.disabled;  // applied state: the queue shows on the rows
            }
            scene.pending = scene.pending || scene_pending[static_cast<size_t>(s)];
        }
        view->scenes.push_back(std::move(scene));
    }
    // Totals: each scene id once.
    for (const auto& list : entries_) {
        for (const Entry& e : list) {
            ++view->total;
            view->disabled += e.item.enabled ? 0 : 1;
            view->missing += e.item.missing ? 1 : 0;
        }
    }

    const Nav& nav = nav_[static_cast<size_t>(nav_index_)];
    view->scene_title = l10n::Pick(nav.zh, nav.en);
    view->subs = nav.subs;
    view->sub = sub_index_[static_cast<size_t>(nav_index_)];
    const int s = CurrentSceneLocked();
    if (s >= 0 && s < static_cast<int>(scene_keys_.size())) view->scene_keys = scene_keys_[static_cast<size_t>(s)];

    row_map_.clear();
    preview_map_.clear();
    selected_row_ = -1;
    view->drag_item = drag_item_;
    view->drag_live = drag_live_;
    view->drag_y = drag_y_;
    view->drag_grab = drag_grab_;
    if (s >= 0 && s < static_cast<int>(entries_.size())) {
        const auto& list = entries_[static_cast<size_t>(s)];
        for (size_t i = 0; i < list.size(); ++i) {
            const Entry& e = list[i];
            const Item& item = e.item;
            const Op* toggle = pending_op(item);
            const Op* moving = pending_pos(item);
            const Op* pending = toggle ? toggle : moving;
            const bool deleting = pending_delete(item);
            if (Deletable(item) && !deleting) ++view->leftovers;
            const bool on = effective_on(item);
            const int zone = effective_zone(item);
            const bool hidden_by_default = default_hidden(e);
            const bool third = third_party(e);
            // 已禁用 counts what Windows has now: a queued switch must not look
            // applied, and the row stays in the filter it was found under.
            const bool matches[ui::kSysCtxFilterCount] = {true, !item.enabled, item.root == Root::Machine, third,
                                                          item.missing, hidden_by_default};
            for (int f = 0; f < ui::kSysCtxFilterCount; ++f) view->filter_counts[static_cast<size_t>(f)] += matches[f] ? 1 : 0;
            const std::wstring key = OpKey(item.root, item.kind, item.key_path);

            std::wstring title = TitleLocked(e);

            // Live preview: the menu after the queued changes; what is off
            // sits on the 已隐藏 shelf under it.
            {
                ui::SysCtxPreviewItem p;
                p.title = title;
                p.icon_path = e.icon;
                p.glyph = KindGlyph(item.kind);
                p.change = deleting ? 1 : toggle ? (on ? 2 : 1) : 0;
                p.extended = item.extended;
                p.selected = key == selected_;
                p.hidden = !on;
                p.zone = zone;
                p.rank = item.kind != Kind::Verb ? 2 : zone == ui::kSysCtxZoneTop ? 0 : zone == ui::kSysCtxZoneBottom ? 3 : 1;
                p.moved = moving ? 1 + zone : 0;
                p.draggable = Toggleable(item) && !deleting && state_ == ui::SysCtxView::State::Ready;
                p.movable = p.draggable && item.kind == Kind::Verb;
                view->preview.push_back(std::move(p));
                preview_map_.emplace_back(s, i);
            }
            if (!matches[filter_]) continue;

            ui::SysCtxRowView row;
            row.title = std::move(title);
            const std::wstring target = !item.file.empty() ? item.file : !item.command.empty() ? item.command : item.clsid;
            const bool handler = item.kind == Kind::ShellEx || item.kind == Kind::DragDrop;
            if (item.missing) {
                row.subtitle = l10n::Get(l10n::StringId::ProgramGoneProbablyUninstalled).c_str() +
                               (!item.command.empty() ? item.command : target);
            } else if (handler) {
                // Named by its menu rows: "Bandizip 的右键扩展 · Bandisoft"; named
                // by its program: "右键扩展 · Bandisoft". The DLL is in 详情.
                const bool by_labels = labels_.count(UpperClsid(item.clsid)) != 0;
                const std::wstring program =
                    by_labels && !IsMicrosoft(e.publisher, item.file)
                        ? ProgramName(e.product, e.description, item.display, item.key_name, item.file)
                        : std::wstring();
                row.subtitle = program.empty() ? std::wstring(KindLabel(item.kind))
                               : item.kind == Kind::DragDrop ? Possessive(program, L"的拖放扩展", L"drag-and-drop extension")
                                                             : Possessive(program, L"的右键扩展", L"shell extension");
                const std::wstring who = !e.publisher.empty() ? e.publisher : e.product;
                if (!who.empty() && who != program) row.subtitle += L" · " + who;
                else if (who.empty() && !item.file.empty() && program.empty()) row.subtitle += L" · " + FileName(item.file);
            } else if (!e.publisher.empty() && !item.file.empty()) {
                row.subtitle = e.publisher + L" · " + FileName(item.file);
            } else if (!item.file.empty()) {
                row.subtitle = FileName(item.file);
            } else if (item.kind == Kind::SendTo) {
                row.subtitle = FileName(item.key_path);
            } else if (item.kind == Kind::Verb) {
                row.subtitle = item.command.empty() ? std::wstring(l10n::Get(l10n::StringId::BuiltIntoWindows).c_str())
                                                    : l10n::Get(l10n::StringId::Runs).c_str() + item.command;
            } else {
                row.subtitle = KindLabel(item.kind);
            }
            row.icon_path = e.icon;
            row.glyph = KindGlyph(item.kind);
            row.on = on;
            row.pending = pending != nullptr || deleting;
            row.deletable = Deletable(item);
            row.deleting = deleting;
            row.machine = item.root == Root::Machine;
            row.missing = item.missing;
            row.locked = item.is_protected || item.trusted_installer;
            row.system_owned = item.trusted_installer;
            row.read_only = !row.locked && !Toggleable(item);
            row.extended = item.extended;
            row.default_hidden = hidden_by_default;
            row.third_party = third;
            row.selected = key == selected_;
            row.place = zone;
            if (const std::wstring pulse_key = PulseKey(item); !pulse_key.empty()) {
                const auto queued = pending_pulse_.find(pulse_key);
                const bool hidden = queued != pending_pulse_.end()
                                        ? queued->second
                                        : std::binary_search(pulse_hidden_.begin(), pulse_hidden_.end(), pulse_key);
                row.pulse = hidden ? 2 : 1;
                if (queued != pending_pulse_.end()) row.pending = true;
            }
            row.kind_label = std::wstring(KindLabel(item.kind)) + L" · " +
                             (item.root == Root::File ? l10n::Get(l10n::StringId::YourSendToFolder).c_str()
                              : row.machine          ? l10n::Get(l10n::StringId::AllUsers).c_str()
                                                     : l10n::Get(l10n::StringId::CurrentUser).c_str());
            row.registry_copyable = item.source == sysctx::Source::Registry;
            row.registry = item.source == sysctx::Source::Packaged ? l10n::Get(l10n::StringId::PackagedMenuSource)
                           : item.root == Root::File ? item.key_path
                           : (row.machine ? L"HKLM\\Software\\Classes\\" : L"HKCU\\Software\\Classes\\") + item.key_path;
            row.file = !item.file.empty() ? item.file : !item.command.empty() ? item.command : item.clsid;
            row.publisher = e.publisher;
            row.method = MethodText(item, Toggleable(item));
            if (item.source == sysctx::Source::Packaged)
                row.kind_label = l10n::Get(l10n::StringId::PackagedMenuSource);
            if (handler && item.source == sysctx::Source::Registry) {
                // What the extension adds, when Pulse's own menu has seen it.
                const auto labels = labels_.find(UpperClsid(item.clsid));
                if (labels != labels_.end() && !labels->second.empty()) {
                    std::wstring added;
                    for (const auto& label : labels->second) added += (added.empty() ? L"" : l10n::Get(l10n::StringId::Text2776).c_str()) + label;
                    row.method = l10n::Get(l10n::StringId::ItAdds).c_str() + added + L"\n" + row.method;
                }
            }
            if (row.selected) {
                selected_row_ = static_cast<int>(view->rows.size());
                view->selected = selected_row_;
            }
            view->rows.push_back(std::move(row));
            row_map_.emplace_back(s, i);
        }
    }

    // 概览: ContextMenuManager marks, Pulse's handler timings, this session's changes.
    auto entry_title = [this](const Entry& e) { return TitleLocked(e); };
    ui::SysCtxOverview& ov = view->overview;
    ov.shown = overview_;
    mark_map_.clear();
    slow_map_.clear();
    for (size_t sc = 0; sc < entries_.size(); ++sc) {
        for (size_t i = 0; i < entries_[sc].size(); ++i) {
            const Entry& e = entries_[sc][i];
            if (!e.item.dash_marker || effective_on(e.item)) continue;
            ++ov.marks;
            if (ov.mark_rows.size() >= 12) continue;  // the scenes list the rest
            ui::SysCtxMarkRow mark;
            mark.title = entry_title(e);
            mark.registry = (e.item.root == Root::Machine ? L"HKLM\\Software\\Classes\\" : L"HKCU\\Software\\Classes\\") +
                            e.item.key_path;
            mark.icon_path = e.icon;
            mark.glyph = KindGlyph(e.item.kind);
            ov.mark_rows.push_back(std::move(mark));
            mark_map_.emplace_back(static_cast<int>(sc), i);
        }
    }
    // Each measured CLSID once; the first scene registering it names it.
    struct Ranked {
        ui::SysCtxSlowRow row;
        std::pair<int, size_t> entry{-1, 0};
    };
    std::vector<Ranked> ranked;
    for (const auto& timing : timings_) {
        if (timing.clsid.empty()) continue;
        Ranked r;
        r.row.clsid = timing.clsid;
        r.row.title = timing.clsid;
        r.row.glyph = KindGlyph(Kind::ShellEx);
        r.row.ms = timing.ms;
        const bool skip_queued = std::find(pending_skip_.begin(), pending_skip_.end(), UpperClsid(timing.clsid)) !=
                                 pending_skip_.end();
        r.row.pulse_skipped = timing.pulse_skipped || skip_queued;
        for (size_t sc = 0; sc < entries_.size() && r.entry.first < 0; ++sc) {
            for (size_t i = 0; i < entries_[sc].size(); ++i) {
                const Entry& e = entries_[sc][i];
                if (e.item.clsid.size() != timing.clsid.size() || !ContainsNoCase(e.item.clsid, timing.clsid)) continue;
                r.entry = {static_cast<int>(sc), i};
                r.row.title = entry_title(e);
                r.row.publisher = !e.publisher.empty() ? e.publisher : FileName(e.item.file);
                r.row.icon_path = e.icon;
                r.row.glyph = KindGlyph(e.item.kind);
                r.row.on = effective_on(e.item);
                r.row.pending = pending_op(e.item) != nullptr || skip_queued;
                r.row.cmm = e.item.dash_marker;
                r.row.switchable = Toggleable(e.item) && state_ == ui::SysCtxView::State::Ready;
                break;
            }
        }
        if (r.entry.first < 0) r.row.publisher = l10n::Get(l10n::StringId::NotInAScannedScene).c_str();
        if (r.row.ms >= ui::kSysCtxSlowMs) ++ov.slow;
        ranked.push_back(std::move(r));
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Ranked& a, const Ranked& b) { return a.row.ms > b.row.ms; });
    if (ranked.size() > 6) ranked.resize(6);
    for (Ranked& r : ranked) {
        ov.slow_rows.push_back(std::move(r.row));
        slow_map_.push_back(r.entry);
    }
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
        if (!it->undone) ++ov.changes;
        if (ov.recent.size() < 4) ov.recent.push_back({it->title, it->caption, it->undone});
    }

    view_ = std::move(view);
    dirty_ = false;
    return view_;
}

void SystemCtxModel::SelectScene(int scene) {
    std::lock_guard lock(mutex_);
    if (scene < 0 || scene >= static_cast<int>(nav_.size())) return;
    if (scene != nav_index_) selected_.clear();
    nav_index_ = scene;
    overview_ = false;
    dirty_ = true;
}

void SystemCtxModel::SelectSub(int sub) {
    std::lock_guard lock(mutex_);
    const Nav& nav = nav_[static_cast<size_t>(nav_index_)];
    if (sub < 0 || sub >= static_cast<int>(nav.scenes.size())) return;
    sub_index_[static_cast<size_t>(nav_index_)] = sub;
    selected_.clear();
    dirty_ = true;
}

void SystemCtxModel::SetFilter(int filter) {
    std::lock_guard lock(mutex_);
    if (filter < 0 || filter >= ui::kSysCtxFilterCount) return;
    filter_ = filter;
    dirty_ = true;
}

void SystemCtxModel::SelectRow(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    if (!e) return;
    const std::wstring key = OpKey(e->item.root, e->item.kind, e->item.key_path);
    selected_ = key == selected_ ? std::wstring() : key;
    if (!selected_.empty()) side_tab_ = 1;
    dirty_ = true;
}

void SystemCtxModel::SetSideTab(int tab) {
    std::lock_guard lock(mutex_);
    side_tab_ = tab == 1 ? 1 : 0;
    dirty_ = true;
}

void SystemCtxModel::BeginPreviewDrag(int item, float y, float grab) {
    std::lock_guard lock(mutex_);
    if (item < 0 || item >= static_cast<int>(preview_map_.size())) return;
    drag_item_ = item;
    drag_live_ = false;
    drag_start_ = drag_y_ = y;
    drag_grab_ = grab;
}

bool SystemCtxModel::PreviewDragMove(float y) {
    std::lock_guard lock(mutex_);
    if (drag_item_ < 0) return false;
    if (!drag_live_ && std::fabs(y - drag_start_) < 3.0f) return false;
    drag_live_ = true;
    drag_y_ = y;
    dirty_ = true;
    return true;
}

int SystemCtxModel::PreviewDragItem() const {
    std::lock_guard lock(mutex_);
    return drag_item_;
}

bool SystemCtxModel::PreviewDragLive() const {
    std::lock_guard lock(mutex_);
    return drag_item_ >= 0 && drag_live_;
}

void SystemCtxModel::EndPreviewDrag() {
    std::lock_guard lock(mutex_);
    if (drag_item_ < 0) return;
    drag_item_ = -1;
    drag_live_ = false;
    dirty_ = true;
}

void SystemCtxModel::SelectPreview(int item) {
    std::lock_guard lock(mutex_);
    if (item < 0 || item >= static_cast<int>(preview_map_.size())) return;
    const auto [s, i] = preview_map_[static_cast<size_t>(item)];
    if (s < 0 || s >= static_cast<int>(entries_.size()) || i >= entries_[static_cast<size_t>(s)].size()) return;
    const Item& it = entries_[static_cast<size_t>(s)][i].item;
    const std::wstring key = OpKey(it.root, it.kind, it.key_path);
    selected_ = key == selected_ ? std::wstring() : key;
    dirty_ = true;
}

void SystemCtxModel::DropPreview(int index, int zone) {
    {
        std::lock_guard lock(mutex_);
        if (state_ != ui::SysCtxView::State::Ready) return;
        if (index < 0 || index >= static_cast<int>(preview_map_.size())) return;
        if (zone < ui::kSysCtxZoneTop || zone > ui::kSysCtxZoneHidden) return;
        const auto [s, i] = preview_map_[static_cast<size_t>(index)];
        if (s < 0 || s >= static_cast<int>(entries_.size()) || i >= entries_[static_cast<size_t>(s)].size()) return;
        const Item item = entries_[static_cast<size_t>(s)][i].item;
        if (!Toggleable(item)) return;
        const std::wstring key = OpKey(item.root, item.kind, item.key_path);
        pending_.erase(key + L"|del");  // dragged back into use: no longer deleted
        const auto toggle = pending_.find(key);
        const bool on = toggle != pending_.end() ? toggle->second.action == Action::Enable : item.enabled;
        // `restores`: the op only returns the entry to its scanned state, so
        // the queued op is dropped instead.
        auto queue = [&](const std::wstring& k, const Op& op, bool restores) {
            if (restores) pending_.erase(k);
            else pending_[k] = op;
        };
        if (zone == ui::kSysCtxZoneHidden) {
            if (on) queue(key, sysctx::MakeOp(item, Action::Disable), !item.enabled);
        } else {
            if (!on) queue(key, sysctx::MakeOp(item, Action::Enable), item.enabled);
            if (item.kind == Kind::Verb) {
                const sysctx::Place want = zone == ui::kSysCtxZoneTop      ? sysctx::Place::Top
                                         : zone == ui::kSysCtxZoneBottom   ? sysctx::Place::Bottom
                                                                           : sysctx::Place::Default;
                const auto moving = pending_.find(key + L"|pos");
                const sysctx::Place now = moving != pending_.end() ? moving->second.place : item.position;
                if (want != now) queue(key + L"|pos", sysctx::MakePositionOp(item, want), want == item.position);
            }
        }
        message_.clear();
        dirty_ = true;
    }
}

const SystemCtxModel::Entry* SystemCtxModel::RowEntryLocked(int row) const {
    if (row < 0) row = selected_row_;
    if (row < 0 || row >= static_cast<int>(row_map_.size())) return nullptr;
    const auto [s, i] = row_map_[static_cast<size_t>(row)];
    if (s < 0 || s >= static_cast<int>(entries_.size()) || i >= entries_[static_cast<size_t>(s)].size()) return nullptr;
    return &entries_[static_cast<size_t>(s)][i];
}

void SystemCtxModel::ToggleEntryLocked(const Entry& e) {
    if (state_ != ui::SysCtxView::State::Ready || !Toggleable(e.item)) return;
    const std::wstring key = OpKey(e.item.root, e.item.kind, e.item.key_path);
    message_.clear();
    dirty_ = true;
    if (pending_.erase(key + L"|del")) return;  // the switch takes a queued delete back first
    if (pending_.erase(key)) return;            // flipped back before it was applied
    pending_[key] = sysctx::MakeOp(e.item, e.item.enabled ? Action::Disable : Action::Enable);
}

void SystemCtxModel::DeleteEntryLocked(const Entry& e) {
    if (state_ != ui::SysCtxView::State::Ready || !Deletable(e.item)) return;
    const std::wstring key = OpKey(e.item.root, e.item.kind, e.item.key_path);
    message_.clear();
    dirty_ = true;
    if (pending_.erase(key + L"|del")) return;
    // The key goes as a whole: a queued switch or move would only fail after it.
    pending_.erase(key);
    pending_.erase(key + L"|pos");
    pending_[key + L"|del"] = sysctx::MakeOp(e.item, Action::Delete);
}

void SystemCtxModel::ToggleRow(int row) {
    std::lock_guard lock(mutex_);
    if (const Entry* e = RowEntryLocked(row)) ToggleEntryLocked(*e);
}

void SystemCtxModel::DeleteRow(int row) {
    std::lock_guard lock(mutex_);
    if (const Entry* e = RowEntryLocked(row)) DeleteEntryLocked(*e);
}

int SystemCtxModel::DeleteSceneLeftovers() {
    std::lock_guard lock(mutex_);
    if (state_ != ui::SysCtxView::State::Ready) return 0;
    const int s = CurrentSceneLocked();
    if (s < 0 || s >= static_cast<int>(entries_.size())) return 0;
    int queued = 0;
    for (const Entry& e : entries_[static_cast<size_t>(s)]) {
        if (!Deletable(e.item)) continue;
        const std::wstring key = OpKey(e.item.root, e.item.kind, e.item.key_path);
        if (pending_.count(key + L"|del")) continue;
        DeleteEntryLocked(e);
        ++queued;
    }
    return queued;
}

bool SystemCtxModel::RowDeletable(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    return e && Deletable(e->item) && state_ == ui::SysCtxView::State::Ready;
}

bool SystemCtxModel::RowDeleting(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    return e && pending_.count(OpKey(e->item.root, e->item.kind, e->item.key_path) + L"|del") != 0;
}

void SystemCtxModel::ToggleMapped(const std::vector<std::pair<int, size_t>>& map, int index) {
    std::lock_guard lock(mutex_);
    if (index < 0 || index >= static_cast<int>(map.size())) return;
    const auto [s, i] = map[static_cast<size_t>(index)];
    if (s < 0 || s >= static_cast<int>(entries_.size()) || i >= entries_[static_cast<size_t>(s)].size()) return;
    ToggleEntryLocked(entries_[static_cast<size_t>(s)][i]);
}

void SystemCtxModel::ToggleSlowRow(int index) { ToggleMapped(slow_map_, index); }

void SystemCtxModel::RestoreMarkRow(int index) { ToggleMapped(mark_map_, index); }

void SystemCtxModel::QueueSlowSkip(int index) {
    std::lock_guard lock(mutex_);
    if (!view_ || index < 0 || index >= static_cast<int>(view_->overview.slow_rows.size())) return;
    const std::wstring clsid = UpperClsid(view_->overview.slow_rows[static_cast<size_t>(index)].clsid);
    if (clsid.empty()) return;
    const auto found = std::find(pending_skip_.begin(), pending_skip_.end(), clsid);
    if (found != pending_skip_.end()) pending_skip_.erase(found);
    else pending_skip_.push_back(clsid);
    message_.clear();
    dirty_ = true;
}

void SystemCtxModel::TogglePulseRow(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    if (!e) return;
    const std::wstring key = PulseKey(e->item);
    if (key.empty()) return;
    // Flipped back before 应用: nothing left to do.
    if (pending_pulse_.erase(key) == 0)
        pending_pulse_[key] = !std::binary_search(pulse_hidden_.begin(), pulse_hidden_.end(), key);
    message_.clear();
    dirty_ = true;
}

bool SystemCtxModel::RowPulseHidden(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    if (!e) return false;
    const std::wstring key = PulseKey(e->item);
    const auto queued = pending_pulse_.find(key);
    if (queued != pending_pulse_.end()) return queued->second;
    return std::binary_search(pulse_hidden_.begin(), pulse_hidden_.end(), key);
}

SystemCtxModel::PulseChanges SystemCtxModel::TakePulseChanges() {
    std::lock_guard lock(mutex_);
    PulseChanges out;
    if (state_ != ui::SysCtxView::State::Ready) return out;
    for (auto& [key, hidden] : pending_pulse_) out.hidden.emplace_back(key, hidden);
    out.skipped = std::move(pending_skip_);
    pending_pulse_.clear();
    pending_skip_.clear();
    dirty_ = true;
    return out;
}

void SystemCtxModel::ShowOverview() {
    std::lock_guard lock(mutex_);
    overview_ = true;
    dirty_ = true;
}

void SystemCtxModel::SetHandlerTimings(std::vector<ui::SysCtxHandlerTiming> timings) {
    std::lock_guard lock(mutex_);
    if (timings.size() == timings_.size() &&
        std::equal(timings.begin(), timings.end(), timings_.begin(), [](const auto& a, const auto& b) {
            return a.ms == b.ms && a.pulse_skipped == b.pulse_skipped && a.clsid == b.clsid;
        }))
        return;
    timings_ = std::move(timings);
    dirty_ = true;
}

void SystemCtxModel::SetHandlerLabels(std::map<std::wstring, std::vector<std::wstring>> labels) {
    std::map<std::wstring, std::vector<std::wstring>> upper;
    for (auto& [clsid, rows] : labels) {
        auto& list = upper[UpperClsid(clsid)];
        // Rows made for one file name (压缩为 "… (2).zip") come after the
        // plain ones and lose the name: 压缩为…
        std::stable_partition(rows.begin(), rows.end(), [](const std::wstring& row) { return !TemplatedLabel(row); });
        for (auto& row : rows) {
            std::wstring text = TidyEllipsis(UntemplateLabel(StripAccessKey(std::move(row))));
            if (!text.empty() && std::find(list.begin(), list.end(), text) == list.end()) list.push_back(std::move(text));
        }
    }
    std::lock_guard lock(mutex_);
    // Merged, never dropped: a registry change empties Pulse's menu caches
    // until the next right-click, and the rows should keep their names.
    bool changed = false;
    for (auto& [clsid, rows] : upper) {
        if (rows.empty()) continue;
        auto& known = labels_[clsid];
        if (known == rows) continue;
        known = std::move(rows);
        changed = true;
    }
    if (changed) dirty_ = true;
}

std::wstring SystemCtxModel::TitleLocked(const Entry& e) const {
    const Item& item = e.item;
    std::wstring title = item.display.empty() ? item.key_name : item.display;
    if (item.kind == Kind::Verb) {
        // No menu text of its own: Windows' name for the verb.
        if (_wcsicmp(title.c_str(), item.key_name.c_str()) == 0)
            if (const wchar_t* known = LookUpName(kVerbNames, std::size(kVerbNames), item.key_name)) title = known;
    } else if (item.kind == Kind::ShellEx || item.kind == Kind::DragDrop) {
        // What it adds to the menu, as Pulse's own menu saw it; else Windows'
        // known handlers; else the program it belongs to. The CLSID's own
        // name ("Bandizip shell menu dll") is the last resort.
        const auto labels = labels_.find(UpperClsid(item.clsid));
        if (labels != labels_.end() && !labels->second.empty()) {
            const auto& rows = labels->second;
            title.clear();
            const bool chinese = std::wstring_view(l10n::Get(l10n::StringId::En).c_str()) == L"zh";
            // Two fit a row; with more, one name and a count keep the count visible:
            // 解压到当前文件夹 等 4 项 / Extract here + 3 more
            const size_t shown = rows.size() > 2 ? 1 : rows.size();
            for (size_t n = 0; n < shown; ++n) title += (n ? (chinese ? L"、" : L", ") : L"") + rows[n];
            if (rows.size() > shown)
                title += chinese ? L" 等 " + std::to_wstring(rows.size()) + L" 项"
                                 : L" + " + std::to_wstring(rows.size() - shown) + L" more";
        } else if (const wchar_t* known = LookUpName(kHandlerNames, std::size(kHandlerNames), item.key_name)) {
            title = known;
        } else if (const wchar_t* by_clsid = LookUpName(kHandlerNames, std::size(kHandlerNames), item.clsid)) {
            title = by_clsid;
        } else if (!IsMicrosoft(e.publisher, item.file)) {
            // "Bandizip shell menu dll" / "QuarkAI.ContextMenu" mean nothing to
            // most readers: name the program and what it is.
            const std::wstring program = ProgramName(e.product, e.description, item.display, item.key_name, item.file);
            if (!program.empty())
                title = item.kind == Kind::DragDrop ? Possessive(program, L"的拖放菜单", L"drag-and-drop menu")
                                                    : Possessive(program, L"的右键菜单", L"context menu");
        } else if (!e.description.empty() && (title == item.key_name || (!title.empty() && title[0] == L'{'))) {
            title = e.description;  // a bare handler key ("AABdzCtx") says less than the DLL's description
        }
    }
    // Keyboard accelerators ("在此处打开命令窗口(&W)") are noise here.
    return StripAccessKey(std::move(title));
}

void SystemCtxModel::SetPulseHidden(std::vector<std::wstring> keys) {
    std::lock_guard lock(mutex_);
    if (keys == pulse_hidden_) return;
    pulse_hidden_ = std::move(keys);
    dirty_ = true;
}

std::wstring SystemCtxModel::ChangeTitleLocked(const std::vector<Op>& ops) const {
    if (ops.empty()) return {};
    const Op& first = ops.front();
    // The scan after the batch lists the entry where the op left it.
    const std::wstring after = sysctx::KeyPathAfter(first);
    std::wstring name;
    for (const auto& list : entries_) {
        for (const Entry& e : list) {
            if (e.item.root != first.root || e.item.kind != first.kind ||
                (e.item.key_path != after && e.item.key_path != first.key_path))
                continue;
            name = TitleLocked(e);
            break;
        }
        if (!name.empty()) break;
    }
    if (name.empty()) name = FileName(first.key_path);
    name.erase(std::remove(name.begin(), name.end(), L'&'), name.end());
    std::wstring title = first.action == Action::Disable   ? l10n::Get(l10n::StringId::TurnedOff).c_str()
                         : first.action == Action::Enable  ? l10n::Get(l10n::StringId::TurnedOn).c_str()
                         : first.action == Action::Delete  ? l10n::Get(l10n::StringId::DeletedLeftover).c_str()
                         : first.action == Action::Restore ? l10n::Get(l10n::StringId::PutBack).c_str()
                                                           : l10n::Get(l10n::StringId::Moved).c_str();
    title += l10n::Get(l10n::StringId::Text2989).c_str() + name + l10n::Get(l10n::StringId::Text2990).c_str();
    if (ops.size() > 1) {
        const bool chinese = std::wstring_view(l10n::Get(l10n::StringId::En).c_str()) == L"zh";
        title += chinese ? L"等 " + std::to_wstring(ops.size()) + L" 项"
                         : L" + " + std::to_wstring(ops.size() - 1) + L" more";
    }
    return title;
}

void SystemCtxModel::ApplyPending() {
    std::vector<Op> ops;
    {
        std::lock_guard lock(mutex_);
        if (state_ != ui::SysCtxView::State::Ready) return;
        for (const auto& [key, op] : pending_) ops.push_back(op);
    }
    if (!ops.empty()) StartJob(ui::SysCtxView::State::Applying, std::move(ops), 0, true);
}

void SystemCtxModel::DiscardPending() {
    std::lock_guard lock(mutex_);
    pending_.clear();
    pending_pulse_.clear();
    pending_skip_.clear();
    dirty_ = true;
}

void SystemCtxModel::Undo() {
    std::vector<Op> ops;
    {
        std::lock_guard lock(mutex_);
        if (state_ != ui::SysCtxView::State::Ready || undo_.empty()) return;
        const auto& last = undo_.back().ops;
        for (auto it = last.rbegin(); it != last.rend(); ++it) ops.push_back(sysctx::Inverse(*it));
    }
    StartJob(ui::SysCtxView::State::Applying, std::move(ops), 1, false);
}

void SystemCtxModel::RestoreAll() {
    std::vector<Op> ops;
    size_t steps = 0;
    {
        std::lock_guard lock(mutex_);
        if (state_ != ui::SysCtxView::State::Ready || undo_.empty()) return;
        // Newest batch first, each in reverse, so every key ends where it began.
        for (auto batch = undo_.rbegin(); batch != undo_.rend(); ++batch)
            for (auto it = batch->ops.rbegin(); it != batch->ops.rend(); ++it) ops.push_back(sysctx::Inverse(*it));
        steps = undo_.size();
    }
    StartJob(ui::SysCtxView::State::Applying, std::move(ops), steps, false);
}

std::wstring SystemCtxModel::RowRegistryPath(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    if (!e || e->item.source != sysctx::Source::Registry || e->item.root == Root::File) return {};
    return (e->item.root == Root::Machine ? L"HKEY_LOCAL_MACHINE" : L"HKEY_CURRENT_USER") +
           std::wstring(L"\\Software\\Classes\\") + e->item.key_path;
}

std::wstring SystemCtxModel::RowFile(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    if (!e) return {};
    if (e->item.root == Root::File) return e->item.key_path;
    return e->item.file;
}

std::wstring SystemCtxModel::RowPulseKey(int row) {
    std::lock_guard lock(mutex_);
    const Entry* e = RowEntryLocked(row);
    return e ? PulseKey(e->item) : std::wstring();
}

} // namespace pulse::app
