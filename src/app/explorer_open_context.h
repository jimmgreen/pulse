#pragma once
#include "explorer_handoff.h"
#include <array>
namespace pulse::app {
// Immutable after construction. No window, tab or COM ownership crosses threads.
struct ExplorerOpenContext {
    std::array<unsigned char, 16> id{};
    std::wstring folder;
    std::vector<std::wstring> names;
    std::shared_ptr<ExplorerHandoff> handoff;
    bool selection_read = false;
};
// Failure leaves the producer responsible for cancelling its source.
std::shared_ptr<const ExplorerOpenContext> MakeExplorerOpenContext(const std::wstring& folder,
    const std::vector<std::wstring>& names, std::shared_ptr<ExplorerHandoff> handoff, bool selection_read) noexcept;
}
