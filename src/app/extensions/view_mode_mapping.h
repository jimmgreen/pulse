#pragma once
#include "../../ui/view_layout.h"
#include <string_view>
#include <optional>

namespace pulse::extensions {
inline std::optional<std::string_view> SdkViewModeName(ui::ViewMode mode) {
    switch (mode) {
    case ui::ViewMode::ExtraLargeIcons:
        return "extra_large_icons";
    case ui::ViewMode::LargeIcons:
        return "large_icons";
    case ui::ViewMode::MediumIcons:
        return "medium_icons";
    case ui::ViewMode::SmallIcons:
        return "small_icons";
    case ui::ViewMode::List:
        return "list";
    case ui::ViewMode::Details:
        return "details";
    case ui::ViewMode::Tiles:
        return "tiles";
    case ui::ViewMode::Content:
        return "content";
    }
    return std::nullopt;
}

} // namespace pulse::extensions
