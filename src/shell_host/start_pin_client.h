#pragma once
#include <windows.h>
#include <string>
namespace pulse::start_pin {
HRESULT Invoke(const std::wstring& path, bool pin);
}
