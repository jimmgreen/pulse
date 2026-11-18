#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::win_e_broker {
inline constexpr uint32_t kMagic = 0x45425750;
inline constexpr uint32_t kVersion = 1;
using Nonce = std::array<unsigned char, 16>;
enum class Owner : LONG { Medium = 0, Broker = 1 };
enum class SlotState : LONG { Empty = 0, Writing, Offered, Claimed, Admitted, Dispatched, Rejected };
struct alignas(8) Shared {
    uint32_t magic = kMagic, version = kVersion, bytes = sizeof(Shared), parent_pid = 0, session = 0, reserved = 0;
    uint64_t parent_creation = 0;
    volatile LONG owner = static_cast<LONG>(Owner::Medium), ready_pid = 0;
    volatile LONG64 sequence = 0;
    volatile LONG state = static_cast<LONG>(SlotState::Empty);
    volatile LONG chord_flags = 0; // bit 0: E held, bit 1: this cycle was swallowed
    volatile LONG64 parent_heartbeat = 0;
};
static_assert(sizeof(Shared) == 64);
inline void Initialize(Shared& value, DWORD pid, DWORD session, uint64_t creation) {
    value.magic = kMagic; value.version = kVersion; value.bytes = sizeof(Shared);
    value.parent_pid = pid; value.session = session; value.reserved = 0;
    value.parent_creation = creation; value.owner = static_cast<LONG>(Owner::Medium);
    value.ready_pid = 0; value.sequence = 0; value.state = static_cast<LONG>(SlotState::Empty);
    value.chord_flags = 0; value.parent_heartbeat = static_cast<LONG64>(GetTickCount64());
}
inline bool ValidHeader(const Shared& value, DWORD pid, DWORD session, uint64_t creation) {
    return value.magic == kMagic && value.version == kVersion && value.bytes == sizeof(Shared) &&
        value.parent_pid == pid && value.session == session && value.parent_creation == creation &&
        pid && creation && value.reserved == 0;
}
inline Owner LoadOwner(Shared& value) { return static_cast<Owner>(InterlockedCompareExchange(&value.owner, 0, 0)); }
inline SlotState LoadState(Shared& value) { return static_cast<SlotState>(InterlockedCompareExchange(&value.state, 0, 0)); }
inline uint64_t LoadSequence(Shared& value) { return static_cast<uint64_t>(InterlockedCompareExchange64(&value.sequence, 0, 0)); }
inline bool AllowedTransition(SlotState from, SlotState to) {
    switch (from) {
    case SlotState::Empty: return to == SlotState::Writing;
    case SlotState::Writing: return to == SlotState::Offered || to == SlotState::Empty;
    case SlotState::Offered: return to == SlotState::Claimed || to == SlotState::Rejected;
    case SlotState::Claimed: return to == SlotState::Admitted || to == SlotState::Rejected;
    case SlotState::Admitted: return to == SlotState::Dispatched || to == SlotState::Rejected;
    case SlotState::Dispatched: case SlotState::Rejected: return to == SlotState::Empty;
    default: return false;
    }
}
inline bool Transition(Shared& value, SlotState from, SlotState to) {
    return AllowedTransition(from, to) && InterlockedCompareExchange(&value.state,
        static_cast<LONG>(to), static_cast<LONG>(from)) == static_cast<LONG>(from);
}
inline bool ParseNonce(std::wstring_view text, Nonce& nonce) {
    if (text.size() != nonce.size() * 2) return false;
    Nonce parsed{};
    const auto digit = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < parsed.size(); ++i) {
        const int a = digit(text[i * 2]), b = digit(text[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        parsed[i] = static_cast<unsigned char>((a << 4) | b);
    }
    nonce = parsed; return true;
}
inline std::wstring NonceText(const Nonce& nonce) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result; result.reserve(32);
    for (const auto byte : nonce) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
inline std::wstring MappingName(DWORD pid, const Nonce& nonce) {
    return L"Local\\Pulse.WinE.Broker." + std::to_wstring(pid) + L"." + NonceText(nonce);
}
inline std::wstring StopEventName(DWORD pid, const Nonce& nonce) { return MappingName(pid, nonce) + L".Stop"; }
inline uint64_t CreationTime(HANDLE process) {
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exit, &kernel, &user)) return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
}
