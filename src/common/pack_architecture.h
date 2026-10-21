#pragma once

#include <cstdint>

namespace pulse::packs {

enum class Architecture { X64, Arm64 };

// Pack selection follows the executable, not the OS hosting it under emulation.
inline constexpr Architecture ProcessArchitecture() {
#if defined(_M_ARM64EC)
#error Pulse preview packs support only x64 and ARM64 process targets, not ARM64EC
#elif defined(_M_ARM64) || defined(__aarch64__)
    return Architecture::Arm64;
#elif defined(_M_X64) || defined(__x86_64__)
    return Architecture::X64;
#else
#error Pulse preview packs support only x64 and ARM64 process targets
#endif
}

inline constexpr const wchar_t* ArchitectureName(Architecture architecture = ProcessArchitecture()) {
    switch (architecture) {
    case Architecture::X64: return L"x64";
    case Architecture::Arm64: return L"arm64";
    }
    return L"";
}

// PE COFF machine values, without requiring Windows headers in catalog code.
inline constexpr uint16_t ProcessMachine(Architecture architecture = ProcessArchitecture()) {
    switch (architecture) {
    case Architecture::X64: return 0x8664;
    case Architecture::Arm64: return 0xAA64;
    }
    return 0;
}

} // namespace pulse::packs
