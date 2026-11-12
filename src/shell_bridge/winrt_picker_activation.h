#pragma once
#include "winrt_picker_adapter.h"
#include <roapi.h>
namespace pulse::shell_bridge::winrt_picker {
using GetActivationFactoryFunction = HRESULT (WINAPI*)(HSTRING, REFIID, void**);
// Only the three Windows.Storage.Pickers default activation factories are adapted.
// Static/CreateForUser and unrecognized factory interfaces remain native.
HRESULT GetActivationFactory(HSTRING name, REFIID iid, void** output,
    const Services& services, GetActivationFactoryFunction original = &::RoGetActivationFactory) noexcept;
}
