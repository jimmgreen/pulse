#pragma once
#include "../common/menu_icon.h"
#include <shlobj.h>

namespace pulse::shell {

MenuIcon CollectMenuIcon(IContextMenu2* menu2, IContextMenu3* menu3,
                         HMENU menu, const MENUITEMINFOW& item);

} // namespace pulse::shell
