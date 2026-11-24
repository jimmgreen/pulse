#pragma once
#include "winrt_picker_adapter.h"
#include <roapi.h>
namespace pulse::shell_bridge::winrt_picker {
using GetActivationFactoryFunction = HRESULT (WINAPI*)(HSTRING, REFIID, void**);
using ActivateInstanceFunction = HRESULT (WINAPI*)(HSTRING, IInspectable**);
// Only the three Windows.Storage.Pickers default activation factories/instances are adapted.
// Static/CreateForUser and unrecognized factory interfaces remain native.
HRESULT GetActivationFactory(HSTRING name, REFIID iid, void** output,
    const Services& services, GetActivationFactoryFunction original = &::RoGetActivationFactory) noexcept;
HRESULT ActivateInstance(HSTRING name, IInspectable** output,
    const Services& services, ActivateInstanceFunction original = &::RoActivateInstance) noexcept;
}
