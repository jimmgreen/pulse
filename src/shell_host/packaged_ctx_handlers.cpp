#include "packaged_ctx_handlers.h"

#include "../common/path_utils.h"

#include <appmodel.h>
#include <ocidl.h>
#include <oleauto.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <xmllite.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <new>

#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "xmllite.lib")

namespace pulse::shell {
namespace {

constexpr wchar_t kContextMenuCategory[] = L"windows.fileExplorerContextMenus";
constexpr int kMaxSubCommandDepth = 16;
constexpr int kMaxSubCommands = 64;

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

void SetSiteOf(IUnknown* object, IUnknown* site) {
    IObjectWithSite* with_site = nullptr;
    if (object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&with_site))) && with_site) {
        with_site->SetSite(site);
        with_site->Release();
    }
}

void SetSelectionOf(IUnknown* object, IShellItemArray* items) {
    if (!object || !items) return;
    IObjectWithSelection* with_sel = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&with_sel))) && with_sel) {
        with_sel->SetSelection(items);
        with_sel->Release();
    }
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
    ExplorerCommandMenu(std::vector<IExplorerCommand*> roots, IShellItemArray* items, IUnknown* site)
        : roots_(std::move(roots)), items_(items), site_(site) {
        if (items_) items_->AddRef();
        if (site_) site_->AddRef();
        for (auto* root : roots_) {
            root->AddRef();
            if (site_) SetSiteOf(root, site_);
            if (items_) SetSelectionOf(root, items_);
        }
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
        for (auto* root : roots_) AddCommand(menu, index, root, last, next, 0);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, static_cast<USHORT>(next - first));
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* info) override {
        if (!info || !IS_INTRESOURCE(info->lpVerb)) return E_INVALIDARG;
        const size_t offset = LOWORD(reinterpret_cast<UINT_PTR>(info->lpVerb));
        if (offset >= commands_.size() || !commands_[offset]) return E_INVALIDARG;
        return commands_[offset]->Invoke(items_, nullptr);
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override {
        return E_NOTIMPL;
    }

private:
    ~ExplorerCommandMenu() {
        ReleaseCommands();
        for (auto* root : roots_) {
            if (site_) SetSiteOf(root, nullptr);
            root->Release();
        }
        if (site_) site_->Release();
        if (items_) items_->Release();
    }

    bool IsRootCommand(IExplorerCommand* command) const noexcept {
        return std::find(roots_.begin(), roots_.end(), command) != roots_.end();
    }

    void ReleaseCommands() {
        for (auto* command : commands_) {
            if (!command) continue;
            if (site_ && !IsRootCommand(command)) SetSiteOf(command, nullptr);
            command->Release();
        }
        commands_.clear();
        for (auto bitmap : bitmaps_) DeleteObject(bitmap);
        bitmaps_.clear();
    }

    HBITMAP CommandBitmap(IExplorerCommand* command) {
        LPWSTR reference = nullptr;
        const HRESULT hr = command->GetIcon(items_, &reference);
        if (FAILED(hr) || !reference || !*reference) {
            CoTaskMemFree(reference);
            return nullptr;
        }
        std::vector<wchar_t> path(reference, reference + wcslen(reference) + 1);
        CoTaskMemFree(reference);
        const int index = PathParseIconLocationW(path.data());
        PathUnquoteSpacesW(path.data());
        const DWORD needed = ExpandEnvironmentStringsW(path.data(), nullptr, 0);
        if (!needed || needed > 32768) return nullptr;
        std::vector<wchar_t> expanded(needed);
        if (!ExpandEnvironmentStringsW(path.data(), expanded.data(), needed)) return nullptr;
        HICON icon = nullptr;
        if (FAILED(SHDefExtractIconW(expanded.data(), index, 0, &icon, nullptr,
                                    MAKELONG(32, 32))) || !icon) return nullptr;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 32;
        bi.bmiHeader.biHeight = -32;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC dc = CreateCompatibleDC(nullptr);
        if (bitmap && dc) {
            auto previous = SelectObject(dc, bitmap);
            ZeroMemory(bits, 32 * 32 * 4);
            std::vector<unsigned char> mask(32 * 32 * 4);
            const BOOL masked = DrawIconEx(dc, 0, 0, icon, 32, 32, 0, nullptr, DI_MASK);
            GdiFlush();
            memcpy(mask.data(), bits, mask.size());
            ZeroMemory(bits, mask.size());
            const BOOL drawn = DrawIconEx(dc, 0, 0, icon, 32, 32, 0, nullptr, DI_NORMAL);
            GdiFlush();
            auto* pixels = static_cast<unsigned char*>(bits);
            bool alpha = false;
            for (size_t i = 3; i < mask.size(); i += 4) alpha |= pixels[i] != 0;
            if (drawn && masked && !alpha) {
                for (size_t i = 0; i < mask.size(); i += 4) {
                    pixels[i + 3] = mask[i] ? 0 : 255;
                    if (mask[i]) pixels[i] = pixels[i + 1] = pixels[i + 2] = 0;
                }
            }
            SelectObject(dc, previous);
            if (!drawn) { DeleteObject(bitmap); bitmap = nullptr; }
        } else if (bitmap) { DeleteObject(bitmap); bitmap = nullptr; }
        if (dc) DeleteDC(dc);
        DestroyIcon(icon);
        return bitmap;
    }

    static void InsertSeparator(HMENU menu, UINT& position) {
        InsertMenuW(menu, position++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }

    // Every inserted item takes the next id and one commands_ slot (nullptr
    // for a submenu parent), so InvokeCommand's offset indexes commands_.
    bool AddCommand(HMENU menu, UINT& position, IExplorerCommand* command, UINT last,
                    UINT& next, int depth) {
        auto clear_child_site = [&]() {
            if (depth > 0 && site_ && !IsRootCommand(command)) SetSiteOf(command, nullptr);
        };
        if (depth > 0) {
            if (site_) SetSiteOf(command, site_);
            if (items_) SetSelectionOf(command, items_);
        }
        EXPCMDSTATE state = ECS_ENABLED;
        if (FAILED(command->GetState(items_, TRUE, &state))) state = ECS_ENABLED;
        if (state & ECS_HIDDEN) {
            clear_child_site();
            return false;
        }
        EXPCMDFLAGS flags = ECF_DEFAULT;
        if (FAILED(command->GetFlags(&flags))) flags = ECF_DEFAULT;
        if (flags & ECF_ISSEPARATOR) {
            clear_child_site();
            if (depth > 0) InsertSeparator(menu, position);
            return depth > 0;
        }
        if (next > last) {
            clear_child_site();
            return false;
        }
        LPWSTR title = nullptr;
        if (FAILED(command->GetTitle(items_, &title)) || !title || !title[0]) {
            CoTaskMemFree(title);
            clear_child_site();
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
                for (size_t i = first_slot; i < commands_.size(); ++i) {
                    if (!commands_[i]) continue;
                    if (site_ && !IsRootCommand(commands_[i])) SetSiteOf(commands_[i], nullptr);
                    commands_[i]->Release();
                }
                if (site_ && !IsRootCommand(command)) SetSiteOf(command, nullptr);
                commands_.resize(first_slot);
                next = first_id;
                CoTaskMemFree(title);
                return false;
            }
            info.fMask |= MIIM_SUBMENU;
            info.hSubMenu = submenu;
        } else if (flags & ECF_HASSUBCOMMANDS) {
            clear_child_site();
            CoTaskMemFree(title);
            return false;
        } else {
            info.wID = next++;
            command->AddRef();
            commands_.push_back(command);
        }
        if ((flags & ECF_SEPARATORBEFORE) && depth > 0) InsertSeparator(menu, position);
        if (HBITMAP bitmap = CommandBitmap(command)) {
            info.fMask |= MIIM_BITMAP;
            info.hbmpItem = bitmap;
            bitmaps_.push_back(bitmap);
        }
        InsertMenuItemW(menu, position++, TRUE, &info);
        if ((flags & ECF_SEPARATORAFTER) && depth > 0) InsertSeparator(menu, position);
        CoTaskMemFree(title);
        return true;
    }

    LONG refs_ = 1;
    std::vector<IExplorerCommand*> roots_;
    IShellItemArray* items_ = nullptr;
    IUnknown* site_ = nullptr;
    std::vector<IExplorerCommand*> commands_;
    std::vector<HBITMAP> bitmaps_;
};

// --- read-only registry property bag ---------------------------------------

class RegPropertyBag final : public IPropertyBag {
public:
    explicit RegPropertyBag(HKEY key) : key_(key) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPropertyBag) {
            *ppv = static_cast<IPropertyBag*>(this);
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

    IFACEMETHODIMP Read(LPCOLESTR prop_name, VARIANT* var, IErrorLog*) override {
        if (!var) return E_POINTER;
        if (!prop_name || !key_) return E_INVALIDARG;
        const VARTYPE requested = var->vt;
        DWORD type = 0;
        DWORD bytes = 0;
        if (RegQueryValueExW(key_, prop_name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS)
            return E_INVALIDARG;
        VARIANT raw{};
        VariantInit(&raw);
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            const size_t cch = (bytes / sizeof(wchar_t)) + 1;
            std::vector<wchar_t> buf(cch, L'\0');
            if (RegQueryValueExW(key_, prop_name, nullptr, &type,
                                 reinterpret_cast<LPBYTE>(buf.data()), &bytes) != ERROR_SUCCESS)
                return E_FAIL;
            buf.back() = L'\0';
            std::wstring value(buf.data());
            if (type == REG_EXPAND_SZ && !value.empty()) {
                const DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
                if (needed > 0 && needed <= 32768) {
                    std::wstring expanded(needed, L'\0');
                    const DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed);
                    if (written > 0 && written <= needed) {
                        expanded.resize(wcsnlen(expanded.c_str(), expanded.size()));
                        value = std::move(expanded);
                    }
                }
            }
            raw.vt = VT_BSTR;
            raw.bstrVal = SysAllocStringLen(value.c_str(), static_cast<UINT>(value.size()));
            if (!raw.bstrVal) return E_OUTOFMEMORY;
        } else if (type == REG_DWORD && bytes == sizeof(DWORD)) {
            DWORD value = 0;
            if (RegQueryValueExW(key_, prop_name, nullptr, &type,
                                 reinterpret_cast<LPBYTE>(&value), &bytes) != ERROR_SUCCESS)
                return E_FAIL;
            if (requested == VT_I4) {
                raw.vt = VT_I4;
                raw.lVal = static_cast<LONG>(value);
            } else if (requested == VT_BOOL) {
                raw.vt = VT_BOOL;
                raw.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
            } else {
                raw.vt = VT_UI4;
                raw.ulVal = value;
            }
        } else {
            return E_INVALIDARG;
        }

        if (requested == VT_EMPTY || requested == raw.vt) {
            *var = raw;
            return S_OK;
        }
        VARIANT converted{};
        VariantInit(&converted);
        const HRESULT hr = VariantChangeType(&converted, &raw, 0, requested);
        VariantClear(&raw);
        if (SUCCEEDED(hr)) *var = converted;
        return hr;
    }

    IFACEMETHODIMP Write(LPCOLESTR, VARIANT*) override { return E_ACCESSDENIED; }

private:
    ~RegPropertyBag() {
        if (key_) RegCloseKey(key_);
    }
    LONG refs_ = 1;
    HKEY key_ = nullptr;
};

// --- minimal folder view site ------------------------------------------------

class FolderViewSite final : public IServiceProvider, public IFolderView, public IOleWindow {
public:
    FolderViewSite(PIDLIST_ABSOLUTE folder, HWND owner, IShellItemArray* selection)
        : folder_(folder), owner_(owner), selection_(selection) {
        if (selection_) selection_->AddRef();
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IServiceProvider) {
            *ppv = static_cast<IServiceProvider*>(this);
        } else if (riid == IID_IFolderView) {
            *ppv = static_cast<IFolderView*>(this);
        } else if (riid == IID_IOleWindow) {
            *ppv = static_cast<IOleWindow*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return refs;
    }

    // IServiceProvider
    IFACEMETHODIMP QueryService(REFGUID service, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (service == IID_IFolderView || service == SID_STopLevelBrowser ||
            service == IID_IShellBrowser || service == IID_IOleWindow)
            return QueryInterface(riid, ppv);
        return E_NOINTERFACE;
    }

    // IOleWindow
    IFACEMETHODIMP GetWindow(HWND* phwnd) override {
        if (!phwnd) return E_POINTER;
        *phwnd = owner_;
        return owner_ ? S_OK : E_FAIL;
    }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

    // IFolderView
    IFACEMETHODIMP GetFolder(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!folder_) return E_FAIL;
        if (riid == IID_IShellItem || riid == IID_IShellItem2)
            return SHCreateItemFromIDList(folder_, riid, ppv);
        // IShellFolder and the like: the folder's own handler (the desktop's
        // for an empty ID list).
        IShellItem* item = nullptr;
        HRESULT hr = SHCreateItemFromIDList(folder_, IID_PPV_ARGS(&item));
        if (SUCCEEDED(hr) && item) {
            hr = item->BindToHandler(nullptr, BHID_SFObject, riid, ppv);
            item->Release();
        }
        return hr;
    }
    IFACEMETHODIMP GetCurrentViewMode(UINT* mode) override {
        if (!mode) return E_POINTER;
        *mode = FVM_DETAILS;
        return S_OK;
    }
    IFACEMETHODIMP SetCurrentViewMode(UINT) override { return E_NOTIMPL; }
    IFACEMETHODIMP Item(int, PITEMID_CHILD* pidl) override {
        if (pidl) *pidl = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP ItemCount(UINT flags, int* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        const UINT type = flags & SVGIO_TYPE_MASK;
        if (selection_ && (type == SVGIO_SELECTION || type == SVGIO_ALLVIEW)) {
            DWORD n = 0;
            if (SUCCEEDED(selection_->GetCount(&n))) {
                *count = static_cast<int>(n);
                return S_OK;
            }
        }
        return E_NOTIMPL;
    }
    IFACEMETHODIMP Items(UINT flags, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        const UINT type = flags & SVGIO_TYPE_MASK;
        if (selection_ && (type == SVGIO_SELECTION || type == SVGIO_ALLVIEW)) {
            if (riid == IID_IDataObject)
                return selection_->BindToHandler(nullptr, BHID_DataObject, riid, ppv);
            if (riid == IID_IEnumShellItems)
                return selection_->EnumItems(reinterpret_cast<IEnumShellItems**>(ppv));
            return selection_->QueryInterface(riid, ppv);
        }
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetSelectionMarkedItem(int* item) override {
        if (item) *item = -1;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetFocusedItem(int* item) override {
        if (item) *item = -1;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetItemPosition(PCUITEMID_CHILD, POINT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetSpacing(POINT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetDefaultSpacing(POINT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetAutoArrange() override { return S_FALSE; }
    IFACEMETHODIMP SelectItem(int, DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP SelectAndPositionItems(UINT, PCUITEMID_CHILD_ARRAY, POINT*, DWORD) override {
        return E_NOTIMPL;
    }

private:
    ~FolderViewSite() {
        if (folder_) ILFree(folder_);
        if (selection_) selection_->Release();
    }
    LONG refs_ = 1;
    PIDLIST_ABSOLUTE folder_ = nullptr;
    HWND owner_ = nullptr;
    IShellItemArray* selection_ = nullptr;
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
                                  IContextMenu** out, IUnknown* site) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!command) return E_INVALIDARG;
    *out = new (std::nothrow) ExplorerCommandMenu({command}, items, site);
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT CreateExplorerCommandMenuMulti(const std::vector<IExplorerCommand*>& commands,
                                       IShellItemArray* items, IContextMenu** out,
                                       IUnknown* site) {
    if (!out) return E_POINTER;
    *out = nullptr;
    std::vector<IExplorerCommand*> valid;
    valid.reserve(commands.size());
    for (auto* cmd : commands)
        if (cmd) valid.push_back(cmd);
    if (valid.empty()) return E_INVALIDARG;
    *out = new (std::nothrow) ExplorerCommandMenu(std::move(valid), items, site);
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT CreateRegistryPropertyBag(HKEY root, const std::wstring& subkey,
                                  IPropertyBag** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!root || subkey.empty()) return E_INVALIDARG;
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    auto* bag = new (std::nothrow) RegPropertyBag(key);
    if (!bag) {
        RegCloseKey(key);
        return E_OUTOFMEMORY;
    }
    *out = bag;
    return S_OK;
}

HRESULT CreateFolderViewSite(PCIDLIST_ABSOLUTE folder, IUnknown** out,
                             HWND owner, IShellItemArray* selection) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!folder && !owner && !selection) return E_INVALIDARG;
    PIDLIST_ABSOLUTE copy = nullptr;
    if (folder) {
        copy = ILCloneFull(folder);
        if (!copy) return E_OUTOFMEMORY;
    }
    auto* site = new (std::nothrow) FolderViewSite(copy, owner, selection);
    if (!site) {
        if (copy) ILFree(copy);
        return E_OUTOFMEMORY;
    }
    *out = static_cast<IServiceProvider*>(site);
    return S_OK;
}

} // namespace pulse::shell