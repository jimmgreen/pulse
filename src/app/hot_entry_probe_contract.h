#pragma once
#include <windows.h>
#include <array>
#include <string>
namespace pulse::app::hot_entry_probe {
inline constexpr DWORD kMagic = 0x48545031;
inline constexpr unsigned kSenders = 6;
struct Sender {
    DWORD pid = 0;
    std::array<unsigned char, 16> attempted_id{};
    LONG forwarded = 0;
    volatile LONG published = 0;
    volatile LONG presented = 0;
    LONG presented_windows = 0, presented_tabs = 0;
};
struct Shared {
    DWORD magic = kMagic, size = sizeof(Shared);
    wchar_t nonce[40]{};
    DWORD primary_pid = 0;
    volatile LONG ready = 0, stop = 0, failed = 0;
    volatile LONG windows = 0, tabs = 0, max_windows = 0, max_tabs = 0;
    Sender senders[kSenders];
};
inline std::wstring MappingName(const wchar_t* nonce) { return L"Local\\PulseHotEntry.Mapping-" + std::wstring(nonce); }
inline std::wstring MutexName(const wchar_t* nonce) { return L"Local\\PulseHotEntry.Singleton-" + std::wstring(nonce); }
inline LONG Read(volatile LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
}
