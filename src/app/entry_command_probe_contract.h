#pragma once
#include <windows.h>
#include <cstdint>

namespace pulse::app::entry_probe {
// Test-only file contract. It does not change the external-open wire protocol.
inline constexpr uint32_t kMagic = 0x45504331;
enum class Kind : uint32_t { Folder, ThisPc, RecycleBin, WinE };
struct Contract {
    uint32_t magic = kMagic;
    uint32_t size = sizeof(Contract);
    Kind kind = Kind::Folder;
    uint32_t reserved = 0;
    wchar_t nonce[40]{};
    wchar_t pulse_path[4096]{};
    wchar_t target_path[4096]{};
};
inline constexpr wchar_t kContractFile[] = L"entry-contract.bin";
inline constexpr wchar_t kPresentationReport[] = L"entry-presentation.tsv";
inline constexpr wchar_t kEnvironment[] = L"PULSE_TEST_ENTRY_COMMAND";
// Pulse must emit this nonce/kind-bound receipt only after actual presentation.
// A launcher exit or successfully parsed argument is never this acknowledgement.
}
