#pragma once
#include "../fs/fs_enum.h"
#include "../ui/ui_renderer.h"

namespace pulse::app {

bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir);

} // namespace pulse::app
