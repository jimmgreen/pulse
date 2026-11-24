#pragma once
#include "search_adapter.h"
#include "../../../index/index_types.h"

namespace pulse {
struct AppState;
}

namespace pulse::extensions::native {

SearchBackend MakeSearchBackend(HWND owner);
bool SearchAdapterHostDeliver(uint32_t raw_id, index::SearchResult&& result);
bool ClaimSearchAdapterIndexResult(AppState& s, uint32_t raw_id);

} // namespace pulse::extensions::native
