#pragma once

#include "../common/dialog_bridge_state.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::dialog_broker {

enum class AdmissionVerdict : uint32_t {
    Eligible = 0,
    SkipInvalidProcess,
    SkipSessionZero,
    SkipProtectedProcess,
    SkipCigSignedOnly,
    SkipAcgNoDynamicCode,
    SkipBuiltInExcluded,
    SkipUserExcluded,
    SkipConsoleOnlySubsystem,
    SkipUnsupportedArch,
    SkipAlreadyLoaded,
};

struct ProcessCharacteristics {
    DWORD pid = 0;
    DWORD session_id = 0;
    WORD machine = 0; // IMAGE_FILE_MACHINE_AMD64 or IMAGE_FILE_MACHINE_I386
    WORD pe_subsystem = 0;
    uint64_t creation_time = 0;      // FILETIME as uint64_t (0 if unknown)
    bool is_elevated = false;        // TokenElevation.TokenIsElevated != 0
    bool is_protected = false;       // PPL / Protected Process
    bool has_cig_ms_only = false;    // ProcessSignaturePolicy.MicrosoftSignedOnly
    bool has_acg_no_dyn_code = false;// ProcessDynamicCodePolicy.ProhibitDynamicCode
    bool has_gui_dll_loaded = false; // user32.dll / comdlg32.dll / shell32.dll mapped
    bool already_has_stub = false;
    std::wstring image_path;
    std::wstring basename;
};

struct AdmissionDecision {
    AdmissionVerdict verdict = AdmissionVerdict::SkipInvalidProcess;
    WORD target_machine = 0;
    const char* reason_text = "invalid";
};

// Pure fail-safe policy evaluator (100% deterministic and unit-testable without live PPL/CIG processes).
AdmissionDecision EvaluateProcessPolicy(const ProcessCharacteristics& info,
                                        const dialog_bridge_state::StateSnapshot* state = nullptr) noexcept;

// Queries a live process handle / PID safely (returns SkipInvalidProcess on access denied / exit).
ProcessCharacteristics InspectLiveProcess(DWORD pid) noexcept;

// Lightweight creation timestamp query used to distinguish PID reuse without reading PE/modules.
bool QueryProcessCreationTime(DWORD pid, uint64_t& creation_time_out) noexcept;

// Enumerates PIDs running in interactive sessions (SessionId != 0), excluding System/Idle and self.
std::vector<DWORD> EnumerateInteractiveProcessIds() noexcept;

// Returns true when `basename` matches critical OS, AV/EDR, anti-cheat, or Pulse self processes.
bool IsBuiltInExcludedProcess(std::wstring_view basename) noexcept;

// Reads PE machine & subsystem from an image file header without loading the binary.
bool ReadPeImageHeader(const std::wstring& image_path, WORD& machine_out, WORD& subsystem_out) noexcept;

} // namespace pulse::dialog_broker
