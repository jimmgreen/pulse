#pragma once
#include <string>

namespace pulse::setup {
bool RegisterStartPin(const std::wstring& app, std::wstring& error);
bool UnregisterStartPin(const std::wstring& app, std::wstring& error);
}
