#pragma once

#include "../ipc/protocol.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <vector>

namespace pulse::shell {

struct CtxHandlerDesc {
    CLSID clsid{};
    std::wstring clsid_text;
    std::wstring name;
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

std::vector<CtxHandlerDesc> EnumerateCtxHandlers(
    bool background, const std::wstring& path,
    const std::vector<std::wstring>& disabled_clsids);

bool BindCtxSelection(const std::vector<std::wstring>& paths, bool background,
                      CtxBind& out);

HRESULT QueryOneHandler(const CtxHandlerDesc& handler, const CtxBind& bind,
                        HWND owner, UINT id_first, UINT id_last, UINT qcm_flags,
                        CtxHandlerSlot& slot);

void CollectHandlerItems(const CtxHandlerSlot& slot, bool background,
                         std::vector<CtxItemOut>& out);

uint32_t FindItemId(const std::vector<CtxItemOut>& items, uint32_t id,
                    const std::wstring& verb, const std::wstring& text);

} // namespace pulse::shell
