#pragma once
#include "../common/direct_reveal_client.h"
#include "../common/explorer_launch_request.h"
namespace pulse::explorer_bridge {
using Sender = DirectRevealResult (*)(const explorer_launch::Request&) noexcept;
using BrowseSender = DirectRevealResult (*)(const browse::Request&) noexcept;
using NativeFallback = DWORD (*)(const browse::Request&) noexcept;
DWORD Dispatch(int argc, const wchar_t* const* argv, Sender sender) noexcept;
DWORD Dispatch(int argc, const wchar_t* const* argv, BrowseSender sender) noexcept;
DWORD Dispatch(int argc, const wchar_t* const* argv, Sender sender, NativeFallback fallback) noexcept;
DWORD Dispatch(int argc, const wchar_t* const* argv, BrowseSender sender, NativeFallback fallback) noexcept;
DirectRevealResult Send(const browse::Request& request) noexcept;
DWORD LaunchNativeExplorer(const browse::Request& request) noexcept;
}
