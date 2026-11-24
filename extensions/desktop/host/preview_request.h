#pragma once
#include "ipc/desktop_ipc.h"
#include <algorithm>

namespace pulse::desktop {
inline std::string PulseFilePreviewRequest(std::wstring_view path, const RECT* anchor = nullptr) {
    const auto separator=[](wchar_t c){return c==L'\\'||c==L'/';};
    const bool drive=path.size()>=3 && ((path[0]>=L'A'&&path[0]<=L'Z')||(path[0]>=L'a'&&path[0]<=L'z')) &&
        path[1]==L':' && separator(path[2]);
    const bool unc=path.size()>=5 && separator(path[0])&&separator(path[1])&&!separator(path[2]);
    if ((!drive&&!unc)||std::any_of(path.begin(),path.end(),[](wchar_t c){return c<32||c==L'"';})) return {};
    std::string request = "{\"mode\":\"preview\",\"path\":" + ipc::JsonQuoteWide(path);
    if (anchor) request += ",\"rect_x\":" + std::to_string(anchor->left) +
        ",\"rect_y\":" + std::to_string(anchor->top) +
        ",\"rect_w\":" + std::to_string(std::max<LONG>(0, anchor->right-anchor->left)) +
        ",\"rect_h\":" + std::to_string(std::max<LONG>(0, anchor->bottom-anchor->top));
    return request + "}";
}
}
