#include "ctx_handlers.h"
#include "packaged_ctx_handlers.h"

#include "../common/path_utils.h"
#include "../ipc/ctx_menu_util.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <cwctype>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

using pulse::ipc::CleanMenuText;
using pulse::ipc::MenuMnemonic;
using pulse::ipc::IsBuiltinContextVerb;
using pulse::ipc::IsDisabledHandler;
using pulse::ipc::IsDefaultMenuExtraVerb;
using pulse::ipc::IsOpenWithSubmenuVerb;
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

// Packaged (MSIX / sparse) verbs whose manifest item types match the target.
void AddPackagedHandlers(bool background, const std::wstring& path,
                         std::vector<CtxHandlerDesc>& out,
                         std::unordered_set<std::wstring>& seen,
                         const std::vector<std::wstring>& disabled) {
    const std::vector<std::wstring> types = PackagedItemTypesFor(background, path);
    for (const auto& verb : PackagedContextMenuVerbs()) {
        if (!PackagedVerbMatches(verb, types)) continue;
        if (!seen.insert(verb.clsid_text).second) continue;
        if (IsDisabledHandler(verb.clsid_text, disabled)) continue;
        CtxHandlerDesc desc;
        desc.clsid = verb.clsid;
        desc.clsid_text = verb.clsid_text;
        desc.name = verb.name;
        desc.explorer_command = true;
        out.push_back(std::move(desc));
    }
}

std::wstring ExtensionOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return ToLowerVerb(name.substr(dot));
}

// The ProgID Explorer itself resolves for the type. Reading HKCR\.ext (or
// UserChoice) by hand diverges from it: on the reporting machine .dwg had an
// empty UserChoice and HKCR\.dwg = AutoCAD.Drawing.26, yet the shell (and its
// menu) used CADFile, so Pulse loaded the wrong verbs and handlers.
std::wstring ProgIdForExt(const std::wstring& ext) {
    if (ext.empty()) return {};
    wchar_t progid[256]{};
    DWORD cch = ARRAYSIZE(progid);
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_PROGID, ext.c_str(), nullptr,
                                    progid, &cch)) && progid[0] &&
        _wcsicmp(progid, ext.c_str()) != 0)
        return progid;
    DWORD bytes = sizeof(progid);
    progid[0] = 0;
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

// Visible row text; the raw label's access key ("&X") goes to *mnemonic.
std::wstring MenuItemText(HMENU menu, UINT pos, wchar_t* mnemonic = nullptr) {
    if (mnemonic) *mnemonic = 0;
    wchar_t buf[512]{};
    MENUITEMINFOW mii{ sizeof(mii) };
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = buf;
    mii.cch = ARRAYSIZE(buf) - 1;
    if (!GetMenuItemInfoW(menu, pos, TRUE, &mii)) return {};
    if (mnemonic) *mnemonic = MenuMnemonic(buf);
    return CleanMenuText(buf);
}

constexpr ULONGLONG kNestedFlyoutBudgetMs = 80;
constexpr CLSID kSendToHandler = {0x7BA4C740, 0x9E81, 0x11CF,
                                 {0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}};

void InitMenuPopup(IContextMenu2* menu2, IContextMenu3* menu3, HMENU submenu, UINT pos) {
    if (!submenu) return;
    __try {
        if (menu3) {
            LRESULT ignored = 0;
            menu3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu),
                                  MAKELPARAM(pos, FALSE), &ignored);
        } else if (menu2) {
            menu2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu),
                                 MAKELPARAM(pos, FALSE));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 发送到 has been seen to fill in 310-440 ms on a cold host (95-160 ms warm);
// Explorer's own rows can arrive 300+ ms apart, so a quiet gap below that
// would cut the list short.
constexpr ULONGLONG kSendToFillMs = 1500;
constexpr ULONGLONG kSendToQuietMs = 500;
// 包含到库中 replaces 正在检索库... in ~110 ms cold, ~16 ms warm.
constexpr ULONGLONG kPlaceholderFillMs = 300;

void SafeDispatch(const MSG* msg) {
    __try {
        TranslateMessage(msg);
        DispatchMessageW(msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Some flyouts fill from messages posted to their own hidden windows, so the
// worker gives them its queue until `done` or `deadline`. Thread messages (the
// host's worker commands) are set aside and posted back afterwards.
template <typename Done>
void PumpUntil(Done done, ULONGLONG deadline) {
    std::vector<MSG> held;
    bool quit = false;
    WPARAM quit_code = 0;
    while (!done()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const ULONGLONG left = deadline - now;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(left < 10 ? left : 10),
                                  QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                quit_code = msg.wParam;
            } else if (!msg.hwnd) {
                held.push_back(msg);
            } else {
                SafeDispatch(&msg);
            }
        }
    }
    for (const MSG& msg : held)
        PostThreadMessageW(GetCurrentThreadId(), msg.message, msg.wParam, msg.lParam);
    if (quit) PostQuitMessage(static_cast<int>(quit_code));
}

// A flyout showing only greyed rows (正在检索库..., 正在搜索设备...) is still
// being filled.
bool OnlyPlaceholderRows(HMENU menu) {
    const int count = GetMenuItemCount(menu);
    int rows = 0;
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{ sizeof(mii) };
        mii.fMask = MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii)) return false;
        if (mii.fType & MFT_SEPARATOR) continue;
        if (mii.hSubMenu || !(mii.fState & (MFS_DISABLED | MFS_GRAYED))) return false;
        ++rows;
    }
    return rows > 0;
}

// True when the flyout was given time to fill.
bool WaitForFlyoutFill(HMENU submenu, int fill_target) {
    if (fill_target > 0) {
        int last = GetMenuItemCount(submenu);
        ULONGLONG changed = GetTickCount64();
        PumpUntil([&] {
            const int count = GetMenuItemCount(submenu);
            const ULONGLONG now = GetTickCount64();
            if (count != last) {
                last = count;
                changed = now;
            }
            return count >= fill_target || now - changed >= kSendToQuietMs;
        }, GetTickCount64() + kSendToFillMs);
        return true;
    }
    if (OnlyPlaceholderRows(submenu)) {
        const int before = GetMenuItemCount(submenu);
        PumpUntil([&] {
            return GetMenuItemCount(submenu) != before || !OnlyPlaceholderRows(submenu);
        }, GetTickCount64() + kPlaceholderFillMs);
        return true;
    }
    return false;
}

// Entries Explorer lists under 发送到 (hidden files such as desktop.ini are not).
int CountSendToTargets() {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_SendTo, 0, nullptr, &pidl)) || !pidl) return 0;
    int count = 0;
    IShellFolder* folder = nullptr;
    if (SUCCEEDED(SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder))) && folder) {
        IEnumIDList* items = nullptr;
        if (folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &items) == S_OK &&
            items) {
            PITEMID_CHILD child = nullptr;
            while (items->Next(1, &child, nullptr) == S_OK) {
                ++count;
                CoTaskMemFree(child);
            }
            items->Release();
        }
        folder->Release();
    }
    CoTaskMemFree(pidl);
    return count;
}

bool ContainsVerb(const std::vector<std::wstring>& verbs, const std::wstring& verb) {
    if (verb.empty()) return false;
    for (const auto& v : verbs)
        if (_wcsicmp(v.c_str(), verb.c_str()) == 0) return true;
    return false;
}

void CollectSubmenuLeaves(IContextMenu* menu, IContextMenu2* menu2, IContextMenu3* menu3,
                          HMENU submenu, UINT pos, bool background, UINT id_first,
                          bool parent_enabled, int depth, ULONGLONG deadline,
                          std::vector<CtxItemOut>& kids, bool open_with = false,
                          int fill_target = 0) {
    if (GetTickCount64() < deadline) {
        InitMenuPopup(menu2, menu3, submenu, pos);
        // Off the first-paint path: these workers stream in a later partial.
        // Rows that arrived late get the usual budget for their own flyouts.
        if (depth == 0 && WaitForFlyoutFill(submenu, fill_target))
            deadline = GetTickCount64() + kNestedFlyoutBudgetMs;
    }
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
                                 kids, open_with);
            continue;
        }
        wchar_t child_mnemonic = 0;
        const std::wstring child_text =
            MenuItemText(submenu, static_cast<UINT>(j), &child_mnemonic);
        if (child_text.empty()) continue;
        CtxItemOut item;
        item.id = sub.wID;
        item.enabled = parent_enabled && !(sub.fState & (MFS_DISABLED | MFS_GRAYED));
        item.child = true;
        item.verb = CtxVerbOf(menu, sub.wID, id_first);
        // 选择其他应用 carries the builtin "openas" verb but belongs to the flyout.
        if (!open_with && IsBuiltinContextVerb(item.verb, background)) continue;
        item.text = child_text;
        item.mnemonic = child_mnemonic;
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

std::vector<std::wstring> CtxHandlerKeysForFile(const std::wstring& progid,
                                               const std::wstring& ext,
                                               const std::wstring& perceived) {
    // Explorer's order: the type's ProgID, its SystemFileAssociations entries,
    // then every file (*) and every file-system object.
    constexpr wchar_t kHandlers[] = L"\\shellex\\ContextMenuHandlers";
    std::vector<std::wstring> keys;
    if (!progid.empty()) keys.push_back(progid + kHandlers);
    if (!ext.empty()) keys.push_back(L"SystemFileAssociations\\" + ext + kHandlers);
    if (!perceived.empty())
        keys.push_back(L"SystemFileAssociations\\" + perceived + kHandlers);
    keys.push_back(std::wstring(L"*") + kHandlers);
    keys.push_back(std::wstring(L"AllFilesystemObjects") + kHandlers);
    return keys;
}

std::vector<std::wstring> CtxHandlerKeysForLocation(bool drive_root) {
    constexpr wchar_t kHandlers[] = L"\\shellex\\ContextMenuHandlers";
    if (drive_root)
        return { std::wstring(L"Drive") + kHandlers, std::wstring(L"Folder") + kHandlers };
    return { std::wstring(L"Directory") + kHandlers, std::wstring(L"Folder") + kHandlers,
             std::wstring(L"AllFilesystemObjects") + kHandlers };
}

std::vector<std::wstring> ConditionalShellVerbs(const std::vector<std::wstring>& classes) {
    std::vector<std::wstring> verbs;
    for (const auto& cls : classes) {
        HKEY shell = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, (cls + L"\\shell").c_str(), 0, KEY_READ, &shell) !=
            ERROR_SUCCESS)
            continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[256]{};
            DWORD cch = ARRAYSIZE(name);
            if (RegEnumKeyExW(shell, i, name, &cch, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS)
                break;
            if (RegGetValueW(shell, name, L"AppliesTo", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                             nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
                !ContainsVerb(verbs, name))
                verbs.emplace_back(name);
        }
        RegCloseKey(shell);
    }
    return verbs;
}

HRESULT InitContextMenuExtension(IUnknown* extension, const CtxBind& bind, IContextMenu** menu) {
    if (!menu) return E_POINTER;
    *menu = nullptr;
    if (!extension) return E_INVALIDARG;
    IShellExtInit* init = nullptr;
    if (SUCCEEDED(extension->QueryInterface(IID_PPV_ARGS(&init))) && init) {
        // Called anyway, WorkFolders (E_INVALIDARG), Portable Devices and
        // Library Location (E_FAIL) still add 立即同步 / 以便携式设备方式打开 /
        // 包含到库中 that Explorer never shows; Open With fails on a drive.
        const HRESULT hr = init->Initialize(bind.folder, bind.data, bind.assoc);
        init->Release();
        if (FAILED(hr)) return hr;
    }
    const HRESULT hr = extension->QueryInterface(IID_PPV_ARGS(menu));
    return SUCCEEDED(hr) && !*menu ? E_NOINTERFACE : hr;
}

namespace {

// HKCR class names behind handler keys ("Drive\\shellex\\..." -> "Drive").
std::vector<std::wstring> ClassesOf(const std::vector<std::wstring>& handler_keys) {
    std::vector<std::wstring> classes;
    for (const auto& key : handler_keys) {
        const size_t at = key.find(L"\\shellex\\");
        if (at != std::wstring::npos) classes.push_back(key.substr(0, at));
    }
    return classes;
}

void AttachDefaultMenuVerbs(std::vector<CtxHandlerDesc>& handlers,
                            const std::vector<std::wstring>& handler_keys) {
    for (auto& desc : handlers) {
        if (desc.clsid != kSendToHandler) continue;
        desc.default_menu_verbs = ConditionalShellVerbs(ClassesOf(handler_keys));
        return;
    }
}

} // namespace

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
        AddPackagedHandlers(true, path, out, seen, disabled_clsids);
        return out;
    }
    const std::wstring ext = ExtensionOf(path);
    const DWORD attr = GetFileAttributesW(ToParsingPath(path).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // A plain file: only the keys Explorer's association array holds for
        // it. Folder / Directory / Drive extensions do not apply; loading them
        // cost a worker each (PintoStartScreen ~125 ms) and showed rows such as
        // 包含到库中 or 以便携式设备方式打开 that Explorer never offers on a file.
        const auto keys = CtxHandlerKeysForFile(ProgIdForExt(ext), ext, PerceivedType(ext));
        for (const auto& key : keys) add(key.c_str());
        AttachDefaultMenuVerbs(out, keys);
        AddPackagedHandlers(false, path, out, seen, disabled_clsids);
        return out;
    }
    if (attr != INVALID_FILE_ATTRIBUTES) {
        // A folder or a drive root: Explorer's keys for it, never * (files
        // only) — on a drive root those added VS / Git / ToDesk / 内网通 / 泛泰快传 rows
        // Explorer does not show.
        const std::wstring parsing = ToParsingPath(path);
        const bool drive_root = PathIsRootW(parsing.c_str()) && !PathIsUNCW(parsing.c_str());
        const auto keys = CtxHandlerKeysForLocation(drive_root);
        for (const auto& key : keys) add(key.c_str());
        if (drive_root) {
            // No 发送到 on a drive, but its default-menu slot still supplies
            // 创建快捷方式 / 格式化 / BitLocker.
            std::vector<CtxHandlerDesc> objects;
            std::unordered_set<std::wstring> objects_seen = seen;
            AddHandlersFromKey(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shellex\\ContextMenuHandlers",
                               objects, objects_seen, disabled_clsids);
            for (auto& desc : objects) {
                if (desc.clsid != kSendToHandler) continue;
                desc.default_menu_only = true;
                seen.insert(desc.clsid_text);
                out.push_back(std::move(desc));
            }
        }
        AttachDefaultMenuVerbs(out, keys);
        AddPackagedHandlers(false, path, out, seen, disabled_clsids);
        return out;
    }
    add(L"*\\shellex\\ContextMenuHandlers");
    add(L"AllFilesystemObjects\\shellex\\ContextMenuHandlers");
    add(L"Folder\\shellex\\ContextMenuHandlers");
    add(L"Directory\\shellex\\ContextMenuHandlers");
    add(L"Drive\\shellex\\ContextMenuHandlers");
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
    AddPackagedHandlers(false, path, out, seen, disabled_clsids);
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
                        CtxHandlerSlot& slot, std::atomic<int>* phase) {
    slot = {};
    slot.id_first = id_first;
    slot.id_last = id_last;
    slot.clsid = handler.clsid_text;
    slot.name = handler.name;
    slot.default_menu_verbs = handler.default_menu_verbs;
    slot.default_menu_only = handler.default_menu_only;

    HRESULT hr = E_FAIL;
    if (handler.explorer_command) {
        // Packaged verbs are IExplorerCommand servers, usually out of process
        // (dllhost surrogate); the selection is the bound data object, which is
        // the folder itself for a background click.
        IExplorerCommand* command = nullptr;
        hr = CoCreateInstance(handler.clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                              IID_PPV_ARGS(&command));
        if (FAILED(hr) || !command) return FAILED(hr) ? hr : E_FAIL;
        IShellItemArray* items = nullptr;
        if (bind.data)
            SHCreateShellItemArrayFromDataObject(bind.data, IID_PPV_ARGS(&items));
        if (phase) phase->store(1);
        hr = items ? CreateExplorerCommandMenu(command, items, &slot.menu) : E_FAIL;
        if (items) items->Release();
        command->Release();
    } else {
        IUnknown* unk = nullptr;
        hr = CoCreateInstance(handler.clsid, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IUnknown, reinterpret_cast<void**>(&unk));
        if (FAILED(hr) || !unk) return FAILED(hr) ? hr : E_FAIL;

        hr = InitContextMenuExtension(unk, bind, &slot.menu);
        unk->Release();
    }
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
    if (phase) phase->store(1);
    hr = SafeQueryContextMenu(slot.menu, slot.hmenu, 0, id_first, id_last, qcm_flags);
    if (FAILED(hr)) {
        ReleaseHandlerSlot(slot);
        return hr;
    }
    if (handler.clsid == kSendToHandler && bind.data) {
        // The standalone SendTo extension exposes only a placeholder on modern
        // Windows. The default Shell menu supplies the services that populate
        // and invoke its destinations. Keep the CLSID-provided localized title
        // to isolate that submenu without hardcoding any display language.
        // Prefer the placeholder flyout. Some Windows builds add the
        // placeholder as a plain row instead; it used to slip through as an
        // inert "Send to" that did nothing when clicked (#77).
        int plain_row = -1;
        for (int i = 0; i < GetMenuItemCount(slot.hmenu); ++i) {
            if (GetSubMenu(slot.hmenu, i)) {
                slot.send_to_title = MenuItemText(slot.hmenu, static_cast<UINT>(i));
                break;
            }
            MENUITEMINFOW row{ sizeof(row) };
            row.fMask = MIIM_FTYPE;
            if (plain_row < 0 && GetMenuItemInfoW(slot.hmenu, i, TRUE, &row) &&
                !(row.fType & MFT_SEPARATOR) &&
                !MenuItemText(slot.hmenu, static_cast<UINT>(i)).empty())
                plain_row = i;
        }
        if (slot.send_to_title.empty() && plain_row >= 0)
            slot.send_to_title = MenuItemText(slot.hmenu, static_cast<UINT>(plain_row));
        if (!slot.send_to_title.empty()) {
            if (!slot.default_menu_only) slot.send_to_expected = CountSendToTargets();
            IShellItemArray* selection = nullptr;
            IContextMenu* native = nullptr;
            HRESULT native_hr = SHCreateShellItemArrayFromDataObject(bind.data, IID_PPV_ARGS(&selection));
            if (SUCCEEDED(native_hr)) {
                native_hr = selection->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&native));
                selection->Release();
            }
            HMENU native_popup = native ? CreatePopupMenu() : nullptr;
            if (native_popup) {
                // No CMF_SYNCCASCADEMENU: synchronously initializing every
                // cascade turns this worker into a serial shadow query of the
                // whole default menu, making it structurally the slowest
                // worker on machines with legacy extensions (#77 regression).
                // CollectHandlerItems fills the Send to flyout through
                // InitMenuPopup with its own budget instead.
                native_hr = SafeQueryContextMenu(native, native_popup, 0, id_first, id_last,
                                                 qcm_flags);
                if (SUCCEEDED(native_hr)) {
                    ReleaseHandlerSlot(slot);
                    slot.menu = native;
                    slot.hmenu = native_popup;
                    native = nullptr;
                    native_popup = nullptr;
                }
            }
            if (native_popup) DestroyMenu(native_popup);
            if (native) native->Release();
        }
    }
    slot.menu->QueryInterface(IID_PPV_ARGS(&slot.menu3));
    slot.menu->QueryInterface(IID_PPV_ARGS(&slot.menu2));
    return S_OK;
}

void CollectHandlerItems(const CtxHandlerSlot& slot, bool background,
                         std::vector<CtxItemOut>& out) {
    if (!slot.menu || !slot.hmenu) return;
    if (!slot.send_to_title.empty()) InitMenuPopup(slot.menu2, slot.menu3, slot.hmenu, 0);
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
            if (!slot.send_to_title.empty()) continue;
            if (!out.empty()) pending_separator = true;
            continue;
        }
        const bool enabled = !(mii.fState & (MFS_DISABLED | MFS_GRAYED));
        wchar_t title_mnemonic = 0;
        const std::wstring title =
            MenuItemText(slot.hmenu, static_cast<UINT>(i), &title_mnemonic);
        // The SendTo slot holds the whole default Shell menu; besides 发送到 it
        // also feeds the few rows only that menu produces (创建快捷方式).
        std::wstring extra_verb;
        if (!slot.send_to_title.empty() && !mii.hSubMenu)
            extra_verb = CtxVerbOf(slot.menu, mii.wID, slot.id_first);
        const bool default_extra = IsDefaultMenuExtraVerb(extra_verb) ||
                                   ContainsVerb(slot.default_menu_verbs, extra_verb);
        const bool send_to_row = !slot.send_to_title.empty() &&
            (title == slot.send_to_title ||
             (mii.hSubMenu &&
              pulse::ipc::ToLowerVerb(CtxVerbOf(slot.menu, mii.wID, slot.id_first)) == L"sendto"));
        if (!slot.send_to_title.empty() && !default_extra && !send_to_row) continue;
        // A drive's menu has no 发送到; its slot only feeds the default extras.
        if (slot.default_menu_only && !default_extra) continue;
        if (mii.hSubMenu) {
            const std::wstring parent_verb = slot.send_to_title.empty()
                ? CtxVerbOf(slot.menu, mii.wID, slot.id_first) : L"sendto";
            const bool open_with = slot.send_to_title.empty() &&
                                   IsOpenWithSubmenuVerb(parent_verb);
            if (!open_with && IsBuiltinContextVerb(parent_verb, background)) continue;
            const std::wstring& parent_text = title;
            if (parent_text.empty()) continue;
            // Send to fills itself on WM_INITMENUPOPUP and sits near the end of
            // the default menu, so slower flyouts above it could use up the
            // shared budget and leave it empty (#77). It gets its own, and so
            // does 打开方式, which also enumerates its apps on first open.
            const bool send_to = !slot.send_to_title.empty() ||
                                 pulse::ipc::ToLowerVerb(parent_verb) == L"sendto";
            std::vector<CtxItemOut> kids;
            CollectSubmenuLeaves(slot.menu, slot.menu2, slot.menu3, mii.hSubMenu,
                                 static_cast<UINT>(i), background, slot.id_first, enabled, 0,
                                 send_to || open_with ? GetTickCount64() + kNestedFlyoutBudgetMs
                                                      : deadline,
                                 kids, open_with, send_to ? slot.send_to_expected : 0);
            // An empty 打开方式 adds nothing over Pulse's own 打开方式… row.
            if (kids.empty() && open_with) continue;
            if (kids.empty()) {
                if (!KeepFlyoutParentWithoutLeaves(mii.wID, slot.id_first, slot.id_last))
                    continue;
                CtxItemOut item;
                item.id = mii.wID;
                // An empty Send to flyout is not an invokable destination.
                item.enabled = enabled && !send_to;
                item.verb = parent_verb;
                item.text = parent_text;
                item.mnemonic = title_mnemonic;
                push(std::move(item));
                continue;
            }
            CtxItemOut header;
            header.id = 0;
            header.enabled = enabled;
            header.has_children = true;
            header.verb = parent_verb;
            header.text = parent_text;
            header.mnemonic = title_mnemonic;
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
        // A Send to row without its flyout cannot send anywhere (#77).
        item.enabled = enabled && (slot.send_to_title.empty() || default_extra);
        item.verb = verb;
        item.text = MenuItemText(slot.hmenu, static_cast<UINT>(i), &item.mnemonic);
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
