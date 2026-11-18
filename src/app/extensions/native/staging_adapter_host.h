#pragma once
#include "staging_adapter.h"
#include "../runtime.h"
namespace pulse::extensions::native {
StagingBackend MakeStagingBackend(HWND owner, CallerBinding caller);
void SetStagingConfirmObserver(std::function<void(HWND)> observer);
void SetStagingPickerObserver(std::function<void(HWND)> observer);
}
