#pragma once
#include "transfer_ui.h"
namespace pulse::extensions {
bool BeginRecycleDeleteSelection(AppState& state, const CallerBinding& principal, uint64_t* request_id);
TransferRequestSnapshot GetDeleteRequestSnapshot(AppState& state, const CallerBinding& principal, uint64_t request);
bool CancelDeleteRequest(AppState& state, const CallerBinding& principal, uint64_t request);
void ReleaseDeleteRequests(AppState& state, const CallerBinding& principal);
void CancelDeleteUi(HWND owner);
}
