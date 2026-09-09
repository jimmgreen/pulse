#include "ctx_handlers.h"

#include "../common/path_utils.h"
#include "../ipc/ctx_menu_util.h"

#include <shlwapi.h>
#include <shobjidl.h>

#include <cwctype>
#include <unordered_set>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

using pulse::ipc::CleanMenuText;
using pulse::ipc::IsBuiltinContextVerb;
using pulse::ipc::IsDisabledHandler;
using pulse::ipc::IsDroppedContextSubmenu;
using pulse::ipc::KeepFlyoutParentWithoutLeaves;
using pulse::ipc::kMaxSubmenuChildren;
using pulse::ipc::ToLowerVerb;

namespace pulse::shell {
namespace {

std::wstring ToParsingPath(std::wstring path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

std::wstring ClsidText(const CLSID& clsid) {
    wchar_t buf[64]{};
    if (StringFromGUID2(clsid, buf, ARRAYSIZE(buf)) <= 0) return {};
    return ToLowerVerb(buf);
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) return {};
    std::vector<uint8_t> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return {};
    struct LANGANDCODEPAGE { WORD language; WORD codepage; };
    LANGANDCODEPAGE* translate = nullptr;
    UINT translate_bytes = 0;
    if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translate), &translate_bytes) ||
        !translate || translate_bytes < sizeof(LANGANDCODEPAGE))
        return {};
    wchar_t query[64]{};
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
               translate[0].language, translate[0].codepage);
    wchar_t* desc = nullptr;
    UINT desc_bytes = 0;
    if (!VerQueryValueW(block.data(), query, reinterpret_cast<void**>(&desc), &desc_bytes) ||
        !desc || desc[0] == 0)
        return {};
    return desc;
}

std::wstring HandlerName(HKEY handlers, const wchar_t* subkey, const CLSID& clsid) {
    if (subkey && subkey[0] && subkey[0] != L'{') return subkey;
    wchar_t server[MAX_PATH]{};
    DWORD server_bytes = sizeof(server);
    std::wstring inproc = L"CLSID\\";
    inproc += ClsidText(clsid);
    inproc += L"\\InprocServer32";
    if (RegGetValueW(HKEY_CLASSES_ROOT, inproc.c_str(), nullptr,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, server,
                     &server_bytes) == ERROR_SUCCESS && server[0]) {
        wchar_t expanded[MAX_PATH]{};
        if (ExpandEnvironmentStringsW(server, expanded, ARRAYSIZE(expanded))) {
            auto desc = FileDescription(expanded);
            if (!desc.empty()) return desc;
        }
    }
    wchar_t named[256]{};
    DWORD named_bytes = sizeof(named);
    if (handlers && subkey &&
        RegGetValueW(handlers, subkey, nullptr, RRF_RT_REG_SZ, nullptr, named,
                     &named_bytes) == ERROR_SUCCESS && named[0] && named[0] != L'{')
        return named;
    return subkey && subkey[0] ? std::wstring(subkey) : ClsidText(clsid);
}

void AddHandlersFromKey(HKEY root, const wchar_t* relative,
                        std::vector<CtxHandlerDesc>& out,
                        std::unordered_set<std::wstring>& seen,
                        const std::vector<std::wstring>& disabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, relative, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{};
        DWORD name_cch = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &name_cch, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS)
            break;
        wchar_t value[128]{};
        DWORD value_bytes = sizeof(value);
        const wchar_t* guid_text = name;
        if (RegGetValueW(key, name, nullptr, RRF_RT_REG_SZ, nullptr, value,
                         &value_bytes) == ERROR_SUCCESS && value[0] == L'{')
            guid_text = value;
        CLSID clsid{};
        if (FAILED(CLSIDFromString(guid_text, &clsid))) continue;
        const std::wstring text = ClsidText(clsid);
        if (text.empty() || !seen.insert(text).second) continue;
        if (IsDisabledHandler(text, disabled)) continue;
        CtxHandlerDesc desc;
        desc.clsid = clsid;
        desc.clsid_text = text;
        desc.name = HandlerName(key, name, clsid);
        out.push_back(std::move(desc));
    }
    RegCloseKey(key);
}

std::wstring ExtensionOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return ToLowerVerb(name.substr(dot));
}

std::wstring ProgIdForExt(const std::wstring& ext) {
    if (ext.empty()) return {};
    wchar_t progid[256]{};
    DWORD bytes = sizeof(progid);
    if (RegGetValueW(HKEY_CLASSES_ROOT, ext.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                     progid, &bytes) != ERROR_SUCCESS || !progid[0])
        return {};
    return progid;
}

std::wstring PerceivedType(const std::wstring& ext) {
    if (ext.empty()) return {};
    wchar_t type[128]{};
    DWORD bytes = sizeof(type);
    if (RegGetValueW(HKEY_CLASSES_ROOT, ext.c_str(), L"PerceivedType", RRF_RT_REG_SZ,
                     nullptr, type, &bytes) != ERROR_SUCCESS || !type[0])
        return {};
    return type;
}

bool SafeGetVerbW(IContextMenu* menu, UINT offset, wchar_t* buf, UINT cch) {
    __try {
        return SUCCEEDED(menu->GetCommandString(offset, GCS_VERBW, nullptr,
                                                reinterpret_cast<CHAR*>(buf), cch));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT SafeQueryContextMenu(IContextMenu* menu, HMENU hmenu, UINT index,
                             UINT id_first, UINT id_last, UINT flags) {
    __try {
        return menu->QueryContextMenu(hmenu, index, id_first, id_last, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

std::wstring CtxVerbOf(IContextMenu* menu, UINT id, UINT id_first) {
    if (id < id_first) return {};
    wchar_t buf[128]{};
    if (SafeGetVerbW(menu, id - id_first, buf, ARRAYSIZE(buf) - 1)) return buf;
    return {};
}

std::wstring MenuItemText(HMENU menu, UINT pos) {
    wchar_t buf[512]{};
    MENUITEMINFOW mii{ sizeof(mii) };
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = buf;
    mii.cch = ARRAYSIZE(buf) - 1;
    if (!GetMenuItemInfoW(menu, pos, TRUE, &mii)) return {};
    return CleanMenuText(buf);
}

constexpr ULONGLONG kNestedFlyoutBudgetMs = 80;

void InitMenuPopup(IContextMenu2* menu2, IContextMenu3* menu3, HMENU submenu, UINT pos) {
    if (!submenu) return;
    __try {
        if (menu3) {
            LRESULT ignored = 0;
            menu3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu),
                                  MAKELPARAM(pos, TRUE), &ignored);
        } else if (menu2) {
            menu2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu),
                                 MAKELPARAM(pos, TRUE));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void CollectSubmenuLeaves(IContextMenu* menu, IContextMenu2* menu2, IContextMenu3* menu3,
                          HMENU submenu, UINT pos, bool background, UINT id_first,
                          bool parent_enabled, int depth, ULONGLONG deadline,
                          std::vector<CtxItemOut>& kids) {
    if (GetTickCount64() < deadline) InitMenuPopup(menu2, menu3, submenu, pos);
    const int sub_count = GetMenuItemCount(submenu);
    for (int j = 0; j < sub_count && static_cast<int>(kids.size()) < kMaxSubmenuChildren; ++j) {
        MENUITEMINFOW sub{ sizeof(sub) };
        sub.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(submenu, j, TRUE, &sub)) continue;
        if (sub.fType & MFT_SEPARATOR) continue;
        if (sub.hSubMenu) {
            if (depth >= 1) continue;
            const bool nested_enabled =
                parent_enabled && !(sub.fState & (MFS_DISABLED | MFS_GRAYED));
            CollectSubmenuLeaves(menu, menu2, menu3, sub.hSubMenu, static_cast<UINT>(j),
                                 background, id_first, nested_enabled, depth + 1, deadline,
                                 kids);
            continue;
        }
        const std::wstring child_text = MenuItemText(submenu, static_cast<UINT>(j));
        if (child_text.empty()) continue;
        CtxItemOut item;
        item.id = sub.wID;
        item.enabled = parent_enabled && !(sub.fState & (MFS_DISABLED | MFS_GRAYED));
        item.child = true;
        item.verb = CtxVerbOf(menu, sub.wID, id_first);
        if (IsBuiltinContextVerb(item.verb, background)) continue;
        item.text = child_text;
        kids.push_back(std::move(item));
    }
}

} // namespace

CtxBind::~CtxBind() {
    if (data) data->Release();
    if (folder) CoTaskMemFree(folder);
    if (assoc) RegCloseKey(assoc);
}

void ReleaseHandlerSlot(CtxHandlerSlot& slot) noexcept {
    if (slot.hmenu) DestroyMenu(slot.hmenu);
    if (slot.menu3) slot.menu3->Release();
    if (slot.menu2) slot.menu2->Release();
    if (slot.menu) slot.menu->Release();
    slot.hmenu = nullptr;
    slot.menu3 = nullptr;
    slot.menu2 = nullptr;
    slot.menu = nullptr;
}

std::vector<CtxHandlerDesc> EnumerateCtxHandlers(
    bool background, const std::wstring& path,
    const std::vector<std::wstring>& disabled_clsids) {
    std::vector<CtxHandlerDesc> out;
    std::unordered_set<std::wstring> seen;
    auto add = [&](const wchar_t* relative) {
        AddHandlersFromKey(HKEY_CLASSES_ROOT, relative, out, seen, disabled_clsids);
    };
    if (background) {
        add(L"Directory\\Background\\shellex\\ContextMenuHandlers");
        add(L"Directory\\shellex\\ContextMenuHandlers");
        add(L"Folder\\shellex\\ContextMenuHandlers");
        add(L"LibraryFolder\\Background\\shellex\\ContextMenuHandlers");
        add(L"Drive\\shellex\\ContextMenuHandlers");
        add(L"*\\shellex\\ContextMenuHandlers");
        return out;
    }
    add(L"*\\shellex\\ContextMenuHandlers");
    add(L"AllFilesystemObjects\\shellex\\ContextMenuHandlers");
    add(L"Folder\\shellex\\ContextMenuHandlers");
    add(L"Directory\\shellex\\ContextMenuHandlers");
    add(L"Drive\\shellex\\ContextMenuHandlers");
    const std::wstring ext = ExtensionOf(path);
    if (!ext.empty()) {
        const std::wstring assoc = L"SystemFileAssociations\\" + ext +
            L"\\shellex\\ContextMenuHandlers";
        add(assoc.c_str());
        const std::wstring progid = ProgIdForExt(ext);
        if (!progid.empty()) {
            const std::wstring key = progid + L"\\shellex\\ContextMenuHandlers";
            add(key.c_str());
        }
        const std::wstring perceived = PerceivedType(ext);
        if (!perceived.empty()) {
            const std::wstring key = L"SystemFileAssociations\\" + perceived +
                L"\\shellex\\ContextMenuHandlers";
            add(key.c_str());
        }
    }
    return out;
}

bool BindCtxSelection(const std::vector<std::wstring>& paths, bool background,
                      CtxBind& out) {
    if (paths.empty()) return false;
    if (background) {
        if (FAILED(SHParseDisplayName(ToParsingPath(paths.front()).c_str(), nullptr,
                                      &out.folder, 0, nullptr)) || !out.folder)
            return false;
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromIDList(out.folder, IID_PPV_ARGS(&folder))) && folder) {
            folder->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&out.data));
            folder->Release();
        }
        RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Directory\\Background", 0, KEY_READ, &out.assoc);
        return true;
    }

    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(ToParsingPath(p).c_str(), nullptr, &pidl, 0,
                                         nullptr)) && pidl)
            pidls.push_back(pidl);
    }
    if (pidls.empty()) return false;
    IShellItemArray* array = nullptr;
    HRESULT hr = SHCreateShellItemArrayFromIDLists(
        static_cast<UINT>(pidls.size()),
        const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array);
    if (SUCCEEDED(hr) && array) {
        array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&out.data));
        IShellItem* first = nullptr;
        if (SUCCEEDED(array->GetItemAt(0, &first)) && first) {
            IShellItem* parent = nullptr;
            if (SUCCEEDED(first->GetParent(&parent)) && parent) {
                SHGetIDListFromObject(parent, &out.folder);
                parent->Release();
            }
            first->Release();
        }
        array->Release();
    }
    for (auto pidl : pidls) CoTaskMemFree(pidl);
    const std::wstring ext = ExtensionOf(paths.front());
    const std::wstring progid = ProgIdForExt(ext);
    if (!progid.empty())
        RegOpenKeyExW(HKEY_CLASSES_ROOT, progid.c_str(), 0, KEY_READ, &out.assoc);
    else if (!ext.empty())
        RegOpenKeyExW(HKEY_CLASSES_ROOT, ext.c_str(), 0, KEY_READ, &out.assoc);
    else
        RegOpenKeyExW(HKEY_CLASSES_ROOT, L"*", 0, KEY_READ, &out.assoc);
    return out.data != nullptr;
}

HRESULT QueryOneHandler(const CtxHandlerDesc& handler, const CtxBind& bind,
                        HWND owner, UINT id_first, UINT id_last, UINT qcm_flags,
                        CtxHandlerSlot& slot) {
    slot = {};
    slot.id_first = id_first;
    slot.id_last = id_last;
    slot.clsid = handler.clsid_text;
    slot.name = handler.name;

    IUnknown* unk = nullptr;
    HRESULT hr = CoCreateInstance(handler.clsid, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUnknown, reinterpret_cast<void**>(&unk));
    if (FAILED(hr) || !unk) return FAILED(hr) ? hr : E_FAIL;

    IShellExtInit* init = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&init))) && init) {
        init->Initialize(bind.folder, bind.data, bind.assoc);
        init->Release();
    }
    hr = unk->QueryInterface(IID_PPV_ARGS(&slot.menu));
    unk->Release();
    if (FAILED(hr) || !slot.menu) {
        slot.menu = nullptr;
        return FAILED(hr) ? hr : E_FAIL;
    }

    slot.hmenu = CreatePopupMenu();
    if (!slot.hmenu) {
        slot.menu->Release();
        slot.menu = nullptr;
        return E_OUTOFMEMORY;
    }
    (void)owner;
    hr = SafeQueryContextMenu(slot.menu, slot.hmenu, 0, id_first, id_last, qcm_flags);
    if (FAILED(hr)) {
        ReleaseHandlerSlot(slot);
        return hr;
    }
    slot.menu->QueryInterface(IID_PPV_ARGS(&slot.menu3));
    slot.menu->QueryInterface(IID_PPV_ARGS(&slot.menu2));
    return S_OK;
}

void CollectHandlerItems(const CtxHandlerSlot& slot, bool background,
                         std::vector<CtxItemOut>& out) {
    if (!slot.menu || !slot.hmenu) return;
    const int count = GetMenuItemCount(slot.hmenu);
    const ULONGLONG deadline = GetTickCount64() + kNestedFlyoutBudgetMs;
    bool pending_separator = false;
    auto push = [&](CtxItemOut item) {
        if (item.text.empty()) return;
        item.clsid = slot.clsid;
        item.handler = slot.name;
        if (pending_separator && !out.empty()) {
            out.back().separator_after = true;
            pending_separator = false;
        }
        out.push_back(std::move(item));
    };
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{ sizeof(mii) };
        mii.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(slot.hmenu, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) {
            if (!out.empty()) pending_separator = true;
            continue;
        }
        const bool enabled = !(mii.fState & (MFS_DISABLED | MFS_GRAYED));
        if (mii.hSubMenu) {
            const std::wstring parent_verb = CtxVerbOf(slot.menu, mii.wID, slot.id_first);
            if (IsBuiltinContextVerb(parent_verb, background) ||
                IsDroppedContextSubmenu(parent_verb)) continue;
            const std::wstring parent_text = MenuItemText(slot.hmenu, static_cast<UINT>(i));
            if (parent_text.empty()) continue;
            std::vector<CtxItemOut> kids;
            CollectSubmenuLeaves(slot.menu, slot.menu2, slot.menu3, mii.hSubMenu,
                                 static_cast<UINT>(i), background, slot.id_first, enabled, 0,
                                 deadline, kids);
            if (kids.empty()) {
                if (!KeepFlyoutParentWithoutLeaves(mii.wID, slot.id_first, slot.id_last))
                    continue;
                CtxItemOut item;
                item.id = mii.wID;
                item.enabled = enabled;
                item.verb = parent_verb;
                item.text = parent_text;
                push(std::move(item));
                continue;
            }
            CtxItemOut header;
            header.id = 0;
            header.enabled = enabled;
            header.has_children = true;
            header.verb = parent_verb;
            header.text = parent_text;
            push(std::move(header));
            for (auto& k : kids) {
                k.clsid = slot.clsid;
                k.handler = slot.name;
                out.push_back(std::move(k));
            }
            continue;
        }
        if (mii.wID < slot.id_first || mii.wID > slot.id_last) continue;
        const std::wstring verb = CtxVerbOf(slot.menu, mii.wID, slot.id_first);
        if (IsBuiltinContextVerb(verb, background)) continue;
        CtxItemOut item;
        item.id = mii.wID;
        item.enabled = enabled;
        item.verb = verb;
        item.text = MenuItemText(slot.hmenu, static_cast<UINT>(i));
        push(std::move(item));
    }
}

uint32_t FindItemId(const std::vector<CtxItemOut>& items, uint32_t id,
                    const std::wstring& verb, const std::wstring& text) {
    if (id != 0) {
        for (const auto& item : items)
            if (item.id == id) return item.id;
    }
    if (!verb.empty()) {
        for (const auto& item : items)
            if (!item.has_children && item.id != 0 && item.verb == verb) return item.id;
    }
    if (!text.empty()) {
        for (const auto& item : items)
            if (!item.has_children && item.id != 0 && item.text == text) return item.id;
    }
    return 0;
}

} // namespace pulse::shell
