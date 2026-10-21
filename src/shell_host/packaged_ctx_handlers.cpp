#include "packaged_ctx_handlers.h"

#include "../common/path_utils.h"

#include <appmodel.h>
#include <shlwapi.h>
#include <xmllite.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <new>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "xmllite.lib")

namespace pulse::shell {
namespace {

constexpr wchar_t kContextMenuCategory[] = L"windows.fileExplorerContextMenus";
constexpr int kMaxSubCommandDepth = 3;
constexpr int kMaxSubCommands = 64;

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

bool Equals(const wchar_t* a, const wchar_t* b) {
    return a && b && _wcsicmp(a, b) == 0;
}

std::wstring Attribute(IXmlReader* reader, const wchar_t* name) {
    std::wstring value;
    if (reader->MoveToAttributeByName(name, nullptr) == S_OK) {
        const wchar_t* text = nullptr;
        if (SUCCEEDED(reader->GetValue(&text, nullptr)) && text) value = text;
    }
    reader->MoveToElement();
    return value;
}

std::wstring ClsidText(const CLSID& clsid) {
    wchar_t buf[64]{};
    if (StringFromGUID2(clsid, buf, ARRAYSIZE(buf)) <= 0) return {};
    return Lower(buf);
}

// "ms-resource:AppName" and friends -> the string in the user's language.
std::wstring ResolveDisplayName(const std::wstring& full_name, const std::wstring& identity,
                                const std::wstring& raw) {
    if (raw.empty()) return identity;
    constexpr wchar_t kResource[] = L"ms-resource:";
    constexpr size_t kResourceLength = ARRAYSIZE(kResource) - 1;
    if (_wcsnicmp(raw.c_str(), kResource, kResourceLength) != 0) return raw;
    const std::wstring key = raw.substr(kResourceLength);
    std::wstring uri;
    if (key.starts_with(L"//")) uri = raw;
    else if (key.starts_with(L"/")) uri = L"ms-resource://" + identity + key;
    else uri = L"ms-resource://" + identity + L"/resources/" + key;
    const std::wstring indirect = L"@{" + full_name + L"?" + uri + L"}";
    wchar_t buf[256]{};
    if (SUCCEEDED(SHLoadIndirectString(indirect.c_str(), buf, ARRAYSIZE(buf), nullptr)) && buf[0])
        return buf;
    return identity;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 8 * 1024 * 1024;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
             read == out.size();
    }
    CloseHandle(file);
    return ok;
}

std::wstring PackagePath(const std::wstring& full_name) {
    UINT32 length = 0;
    if (GetPackagePathByFullName(full_name.c_str(), &length, nullptr) != ERROR_INSUFFICIENT_BUFFER ||
        length == 0)
        return {};
    std::wstring path(length, L'\0');
    if (GetPackagePathByFullName(full_name.c_str(), &length, path.data()) != ERROR_SUCCESS) return {};
    path.resize(wcsnlen(path.c_str(), path.size()));
    return path;
}

std::vector<PackagedCtxVerb> ScanPackages() {
    std::vector<PackagedCtxVerb> out;
    HKEY packages = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"PackagedCom\\Package", 0, KEY_READ, &packages) !=
        ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t full_name[256]{};
        DWORD full_cch = ARRAYSIZE(full_name);
        const LSTATUS status = RegEnumKeyExW(packages, i, full_name, &full_cch, nullptr, nullptr,
                                             nullptr, nullptr);
        if (status == ERROR_MORE_DATA) continue;
        if (status != ERROR_SUCCESS) break;
        const std::wstring root = PackagePath(full_name);
        if (root.empty()) continue;
        std::string bytes;
        if (!ReadFileBytes(root + L"\\AppxManifest.xml", bytes)) continue;
        // Most packages declare no context menu: skip them without an XML parse.
        if (bytes.find("fileExplorerContextMenus") == std::string::npos) continue;
        IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()),
                                            static_cast<UINT>(bytes.size()));
        if (!stream) continue;
        std::wstring identity, display;
        auto verbs = ParsePackagedContextMenus(stream, &identity, &display);
        stream->Release();
        if (verbs.empty()) continue;
        const std::wstring name = ResolveDisplayName(full_name, identity, display);
        for (auto& verb : verbs) {
            if (std::any_of(out.begin(), out.end(), [&](const PackagedCtxVerb& seen) {
                    return seen.clsid_text == verb.clsid_text;
                }))
                continue;
            verb.name = name.empty() ? verb.verb_id : name;
            out.push_back(std::move(verb));
        }
    }
    RegCloseKey(packages);
    return out;
}

// --- IExplorerCommand -> IContextMenu --------------------------------------

class ExplorerCommandMenu final : public IContextMenu {
public:
    ExplorerCommandMenu(IExplorerCommand* command, IShellItemArray* items)
        : root_(command), items_(items) {
        root_->AddRef();
        if (items_) items_->AddRef();
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IContextMenu) {
            *ppv = static_cast<IContextMenu*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return refs;
    }

    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last,
                                    UINT flags) override {
        ReleaseCommands();
        if (!menu || (flags & CMF_DEFAULTONLY)) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        UINT next = first;
        AddCommand(menu, index, root_, last, next, 0);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, static_cast<USHORT>(next - first));
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* info) override {
        if (!info || !IS_INTRESOURCE(info->lpVerb)) return E_INVALIDARG;
        const size_t offset = LOWORD(reinterpret_cast<UINT_PTR>(info->lpVerb));
        if (offset >= commands_.size() || !commands_[offset]) return E_INVALIDARG;
        // Packaged commands can live in a COM surrogate beyond pulse_shell.
        // In-process commands need no transfer and may return E_NOINTERFACE.
        CoAllowSetForegroundWindow(commands_[offset], nullptr);
        return commands_[offset]->Invoke(items_, nullptr);
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override {
        return E_NOTIMPL;
    }

private:
    ~ExplorerCommandMenu() {
        ReleaseCommands();
        if (items_) items_->Release();
        root_->Release();
    }

    void ReleaseCommands() {
        for (auto* command : commands_)
            if (command) command->Release();
        commands_.clear();
    }

    static void InsertSeparator(HMENU menu, UINT& position) {
        InsertMenuW(menu, position++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }

    // Every inserted item takes the next id and one commands_ slot (nullptr
    // for a submenu parent), so InvokeCommand's offset indexes commands_.
    bool AddCommand(HMENU menu, UINT& position, IExplorerCommand* command, UINT last,
                    UINT& next, int depth) {
        EXPCMDSTATE state = ECS_ENABLED;
        if (FAILED(command->GetState(items_, TRUE, &state))) state = ECS_ENABLED;
        if (state & ECS_HIDDEN) return false;
        EXPCMDFLAGS flags = ECF_DEFAULT;
        if (FAILED(command->GetFlags(&flags))) flags = ECF_DEFAULT;
        if (flags & ECF_ISSEPARATOR) {
            if (depth > 0) InsertSeparator(menu, position);
            return depth > 0;
        }
        if (next > last) return false;
        LPWSTR title = nullptr;
        if (FAILED(command->GetTitle(items_, &title)) || !title || !title[0]) {
            CoTaskMemFree(title);
            return false;
        }
        MENUITEMINFOW info{ sizeof(info) };
        info.fMask = MIIM_STRING | MIIM_STATE | MIIM_ID;
        info.dwTypeData = title;
        info.fState = (state & ECS_DISABLED) ? MFS_DISABLED : MFS_ENABLED;
        if (state & ECS_CHECKED) info.fState |= MFS_CHECKED;
        HMENU submenu = nullptr;
        if ((flags & ECF_HASSUBCOMMANDS) && depth < kMaxSubCommandDepth) {
            submenu = CreatePopupMenu();
            const UINT first_id = next;
            const size_t first_slot = commands_.size();
            info.wID = next++;
            commands_.push_back(nullptr);
            IEnumExplorerCommand* children = nullptr;
            if (submenu && SUCCEEDED(command->EnumSubCommands(&children)) && children) {
                UINT child_position = 0;
                IExplorerCommand* child = nullptr;
                ULONG fetched = 0;
                for (int count = 0; count < kMaxSubCommands &&
                         children->Next(1, &child, &fetched) == S_OK && child; ++count) {
                    AddCommand(submenu, child_position, child, last, next, depth + 1);
                    child->Release();
                    child = nullptr;
                }
                children->Release();
            }
            if (!submenu || GetMenuItemCount(submenu) <= 0) {
                // A flyout without visible children is not shown by Explorer either;
                // give back its id so the returned count matches the menu.
                if (submenu) DestroyMenu(submenu);
                for (size_t i = first_slot; i < commands_.size(); ++i)
                    if (commands_[i]) commands_[i]->Release();
                commands_.resize(first_slot);
                next = first_id;
                CoTaskMemFree(title);
                return false;
            }
            info.fMask |= MIIM_SUBMENU;
            info.hSubMenu = submenu;
        } else if (flags & ECF_HASSUBCOMMANDS) {
            CoTaskMemFree(title);
            return false;
        } else {
            info.wID = next++;
            command->AddRef();
            commands_.push_back(command);
        }
        if ((flags & ECF_SEPARATORBEFORE) && depth > 0) InsertSeparator(menu, position);
        InsertMenuItemW(menu, position++, TRUE, &info);
        if ((flags & ECF_SEPARATORAFTER) && depth > 0) InsertSeparator(menu, position);
        CoTaskMemFree(title);
        return true;
    }

    LONG refs_ = 1;
    IExplorerCommand* root_ = nullptr;
    IShellItemArray* items_ = nullptr;
    std::vector<IExplorerCommand*> commands_;
};

} // namespace

std::vector<PackagedCtxVerb> ParsePackagedContextMenus(IStream* manifest, std::wstring* identity,
                                                       std::wstring* display_name) {
    std::vector<PackagedCtxVerb> out;
    if (!manifest) return out;
    IXmlReader* reader = nullptr;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(&reader), nullptr)) ||
        !reader)
        return out;
    reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    if (FAILED(reader->SetInput(manifest))) {
        reader->Release();
        return out;
    }
    UINT extension_depth = 0;   // depth of the context-menu Extension, 0 = outside
    UINT properties_depth = 0;
    bool capture_display = false;
    std::wstring item_type;
    XmlNodeType node = XmlNodeType_None;
    while (reader->Read(&node) == S_OK) {
        const wchar_t* local = nullptr;
        UINT depth = 0;
        if (node == XmlNodeType_Element) {
            reader->GetLocalName(&local, nullptr);
            reader->GetDepth(&depth);
            const bool empty = reader->IsEmptyElement() != FALSE;
            if (Equals(local, L"Identity") && identity && identity->empty()) {
                *identity = Attribute(reader, L"Name");
            } else if (Equals(local, L"Properties") && !empty && properties_depth == 0) {
                properties_depth = depth;
            } else if (Equals(local, L"DisplayName") && properties_depth && depth == properties_depth + 1 &&
                       display_name && display_name->empty()) {
                capture_display = !empty;
            } else if (Equals(local, L"Extension") && !empty && extension_depth == 0 &&
                       Equals(Attribute(reader, L"Category").c_str(), kContextMenuCategory)) {
                extension_depth = depth;
            } else if (extension_depth && Equals(local, L"ItemType")) {
                item_type = empty ? std::wstring() : Lower(Attribute(reader, L"Type"));
            } else if (extension_depth && Equals(local, L"Verb") && !item_type.empty()) {
                std::wstring clsid_attr = Attribute(reader, L"Clsid");
                // Manifests usually write bare GUIDs; CLSIDFromString wants braces.
                if (!clsid_attr.empty() && clsid_attr.front() != L'{')
                    clsid_attr = L"{" + clsid_attr + L"}";
                CLSID clsid{};
                if (clsid_attr.empty() || FAILED(CLSIDFromString(clsid_attr.c_str(), &clsid))) continue;
                const std::wstring text = ClsidText(clsid);
                auto it = std::find_if(out.begin(), out.end(), [&](const PackagedCtxVerb& verb) {
                    return verb.clsid_text == text;
                });
                if (it == out.end()) {
                    PackagedCtxVerb verb;
                    verb.clsid = clsid;
                    verb.clsid_text = text;
                    verb.verb_id = Attribute(reader, L"Id");
                    out.push_back(std::move(verb));
                    it = out.end() - 1;
                }
                if (std::find(it->item_types.begin(), it->item_types.end(), item_type) ==
                    it->item_types.end())
                    it->item_types.push_back(item_type);
            }
        } else if (node == XmlNodeType_Text && capture_display) {
            const wchar_t* text = nullptr;
            if (SUCCEEDED(reader->GetValue(&text, nullptr)) && text) *display_name = text;
            capture_display = false;
        } else if (node == XmlNodeType_EndElement) {
            reader->GetLocalName(&local, nullptr);
            reader->GetDepth(&depth);
            if (Equals(local, L"Extension") && depth == extension_depth) extension_depth = 0;
            else if (Equals(local, L"ItemType")) item_type.clear();
            else if (Equals(local, L"Properties") && depth == properties_depth) properties_depth = 0;
            else if (Equals(local, L"DisplayName")) capture_display = false;
        }
    }
    reader->Release();
    return out;
}

std::vector<PackagedCtxVerb> PackagedContextMenuVerbs() {
    static std::mutex mutex;
    static bool valid = false;
    static FILETIME stamp{};
    static DWORD subkeys = 0;
    static std::vector<PackagedCtxVerb> cache;

    FILETIME now_stamp{};
    DWORD now_subkeys = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"PackagedCom\\Package", 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &now_subkeys, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, &now_stamp);
        RegCloseKey(key);
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (!valid || now_subkeys != subkeys || CompareFileTime(&now_stamp, &stamp) != 0) {
        cache = ScanPackages();
        stamp = now_stamp;
        subkeys = now_subkeys;
        valid = true;
    }
    return cache;
}

std::vector<std::wstring> PackagedItemTypesFor(bool background, const std::wstring& path) {
    if (background) return { L"directory\\background" };
    const std::wstring plain = pulse::path::StripExtendedPathPrefix(path);
    const DWORD attrs = GetFileAttributesW(plain.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        const bool drive = plain.size() <= 3 && plain.size() >= 2 && plain[1] == L':';
        if (drive) return { L"drive", L"folder", L"allfilesystemobjects" };
        return { L"directory", L"folder", L"allfilesystemobjects" };
    }
    std::vector<std::wstring> types{ L"*", L"allfilesystemobjects" };
    const size_t slash = plain.find_last_of(L"\\/");
    const size_t dot = plain.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash + 1) &&
        dot + 1 < plain.size())
        types.push_back(Lower(plain.substr(dot)));
    return types;
}

bool PackagedVerbMatches(const PackagedCtxVerb& verb, const std::vector<std::wstring>& types) {
    return std::any_of(verb.item_types.begin(), verb.item_types.end(), [&](const std::wstring& type) {
        return std::find(types.begin(), types.end(), type) != types.end();
    });
}

bool PackagedRowsDuplicate(const std::vector<std::wstring>& packaged_rows,
                           const std::vector<std::wstring>& classic_rows) {
    auto normalize = [](const std::wstring& text) {
        std::wstring out;
        for (wchar_t c : text)
            if (c != L'&') out.push_back(static_cast<wchar_t>(std::towlower(c)));
        const size_t first = out.find_first_not_of(L" \t");
        if (first == std::wstring::npos) return std::wstring();
        return out.substr(first, out.find_last_not_of(L" \t") - first + 1);
    };
    std::vector<std::wstring> classic;
    for (const auto& row : classic_rows) {
        std::wstring text = normalize(row);
        if (!text.empty()) classic.push_back(std::move(text));
    }
    size_t rows = 0;
    size_t duplicates = 0;
    for (const auto& row : packaged_rows) {
        const std::wstring text = normalize(row);
        if (text.empty()) continue;
        ++rows;
        if (std::find(classic.begin(), classic.end(), text) != classic.end()) ++duplicates;
    }
    return rows > 0 && duplicates * 2 >= rows;
}

HRESULT CreateExplorerCommandMenu(IExplorerCommand* command, IShellItemArray* items,
                                  IContextMenu** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!command) return E_INVALIDARG;
    *out = new (std::nothrow) ExplorerCommandMenu(command, items);
    return *out ? S_OK : E_OUTOFMEMORY;
}

} // namespace pulse::shell
