#pragma once
#include "folder_picker_model.h"
#include "picker_shell_item_filter.h"
#include <new>
#include <numeric>
#include <type_traits>
namespace pulse::ui {
// A listing has two same-order publication channels: PickerEntry for interaction
// and immutable DirEntry rows for the shared renderer. Project them by the same
// current row positions, not PickerEntry::source (raw pre-sort enumeration IDs).
template<class Include, class Current>
HRESULT FilterPickerShellListing(PickerListing& listing, Include include, Current current) {
    if (!listing.snapshot || listing.snapshot->size() != listing.entries.size()) return E_UNEXPECTED;
    try {
        std::vector<size_t> accepted(listing.entries.size());
        std::iota(accepted.begin(), accepted.end(), size_t{});
        const HRESULT filtered = FilterShellItems(accepted,
            [&](size_t index) -> const std::wstring& { return listing.entries[index].path; },
            include, current, ShellFilterCandidateRole::EnumeratedView);
        if (filtered != S_OK || accepted.size() == listing.entries.size()) return filtered;
        auto rows = std::make_shared<std::vector<fs::DirEntry>>();
        rows->reserve(accepted.size());
        for (const auto index : accepted) rows->push_back((*listing.snapshot)[index]);
        std::vector<PickerEntry> entries;
        entries.reserve(accepted.size());
        if (!current()) return HRESULT_FROM_WIN32(ERROR_RETRY);
        // No callbacks, pumping or throwing allocations occur between the two
        // commits. Other readers keep the original immutable snapshot unchanged.
        static_assert(std::is_nothrow_move_constructible_v<PickerEntry>);
        for (const auto index : accepted) entries.push_back(std::move(listing.entries[index]));
        listing.entries.swap(entries);
        listing.snapshot = std::move(rows);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
}
