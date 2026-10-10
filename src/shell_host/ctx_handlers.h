#pragma once

#include "../ipc/protocol.h"

#include <windows.h>
#include <atomic>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <vector>

namespace pulse::shell {

struct CtxHandlerDesc {
    CLSID clsid{};
    std::wstring clsid_text;
    std::wstring name;
    bool explorer_command = false; // packaged IExplorerCommand verb (#48)
    // SendTo slot only. Registry verbs with an AppliesTo condition: the
    // default Shell menu evaluates those itself (启用 BitLocker on a drive), so
    // they are taken from it like 创建快捷方式.
    std::vector<std::wstring> default_menu_verbs;
    // Drive roots: Explorer's drive menu has no 发送到, but the default menu
    // still supplies 创建快捷方式 / 格式化 / BitLocker.
    bool default_menu_only = false;
};

struct CtxItemOut {
    uint32_t id = 0;
    bool enabled = true;
    bool separator_after = false;
    bool has_children = false;
    bool child = false;
    std::wstring verb;
    std::wstring text;
    std::wstring clsid;
    std::wstring handler;
    wchar_t mnemonic = 0;   // access key from the raw label ("&X"), 0 = none
};

struct CtxHandlerSlot {
    IContextMenu* menu = nullptr;
    IContextMenu2* menu2 = nullptr;
    IContextMenu3* menu3 = nullptr;
    HMENU hmenu = nullptr;
    UINT id_first = 0;
    UINT id_last = 0;
    std::wstring clsid;
    std::wstring name;
    // Localized title obtained from the SendTo CLSID, used to isolate its
    // submenu in the default Shell context menu (which has no canonical verb).
    std::wstring send_to_title;
    std::vector<std::wstring> default_menu_verbs; // see CtxHandlerDesc
    bool default_menu_only = false;
    // Destinations in the SendTo folder. Explorer's 发送到 lists them from
    // posted messages, so collection pumps until the flyout holds them all.
    int send_to_expected = 0;
};

struct CtxBind {
    IDataObject* data = nullptr;
    PIDLIST_ABSOLUTE folder = nullptr;
    HKEY assoc = nullptr;
    ~CtxBind();
    CtxBind() = default;
    CtxBind(const CtxBind&) = delete;
    CtxBind& operator=(const CtxBind&) = delete;
};

void ReleaseHandlerSlot(CtxHandlerSlot& slot) noexcept;

// shellex keys (relative to HKCR) for a plain file, in Explorer's order.
// Folder / Directory / Drive extensions never apply to a file.
std::vector<std::wstring> CtxHandlerKeysForFile(const std::wstring& progid,
                                               const std::wstring& ext,
                                               const std::wstring& perceived);

// Handler keys of a file system folder or a drive root, in Explorer's order.
// A drive's menu reads Drive and Folder only; a folder's also Directory and
// AllFilesystemObjects. Neither reads * (files only).
std::vector<std::wstring> CtxHandlerKeysForLocation(bool drive_root);

// Verbs under HKCR\<class>\shell carrying an AppliesTo condition.
std::vector<std::wstring> ConditionalShellVerbs(const std::vector<std::wstring>& classes);

// IShellExtInit + IContextMenu for an extension that is already created.
// Like the default Shell menu, an extension whose Initialize fails is dropped.
HRESULT InitContextMenuExtension(IUnknown* extension, const CtxBind& bind, IContextMenu** menu);

std::vector<CtxHandlerDesc> EnumerateCtxHandlers(
    bool background, const std::wstring& path,
    const std::vector<std::wstring>& disabled_clsids);

bool BindCtxSelection(const std::vector<std::wstring>& paths, bool background,
                      CtxBind& out);

// `phase`, when given, is set to 1 once the extension is created and
// initialized, right before its own QueryContextMenu runs.
HRESULT QueryOneHandler(const CtxHandlerDesc& handler, const CtxBind& bind,
                        HWND owner, UINT id_first, UINT id_last, UINT qcm_flags,
                        CtxHandlerSlot& slot, std::atomic<int>* phase = nullptr);

void CollectHandlerItems(const CtxHandlerSlot& slot, bool background,
                         std::vector<CtxItemOut>& out);

uint32_t FindItemId(const std::vector<CtxItemOut>& items, uint32_t id,
                    const std::wstring& verb, const std::wstring& text);

} // namespace pulse::shell
