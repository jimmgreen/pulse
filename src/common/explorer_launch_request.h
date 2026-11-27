#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include "browse_request.h"
namespace pulse::explorer_launch {
struct Request { bool select = false; std::wstring path; };
std::wstring Quote(std::wstring_view value);
std::wstring SystemExplorer();
// Only an explicit system image establishes identity. Unsupported denotes an
// Explorer browsing request outside this protocol, not permission to fall back.
// Relative paths require an explicit absolute local working_directory.
browse::Match Classify(const wchar_t* application, const wchar_t* command, DWORD flags,
                       const wchar_t* working_directory, browse::Request& out);
bool Resolve(const wchar_t* application, const wchar_t* command, DWORD flags, Request& out);
}
