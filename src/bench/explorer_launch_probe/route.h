#pragma once
#include <windows.h>
#include <string>
#include <string_view>
namespace explorer_probe {
struct Request { bool select = false; std::wstring path; };
std::wstring Quote(std::wstring_view value);
std::wstring SystemExplorer();
bool Resolve(const wchar_t* application, const wchar_t* command, DWORD flags, Request& out);
}
