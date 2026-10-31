#pragma once

// C ABI for the explicitly named, non-shipping PulsePickerBridgeProbe DLL.
// The caller must use the DLL matching its own process architecture.
#include <windows.h>
#include <stdint.h>

#define PULSE_BRIDGE_PROBE_ABI_VERSION 1u
#define PULSE_BRIDGE_PROBE_ENABLE 0x00000001u
#define PULSE_BRIDGE_PROBE_INSTALLED 0x00000001u
#define PULSE_BRIDGE_PROBE_PINNED 0x00000002u
#define PULSE_BRIDGE_PROBE_PROTECTION_RESTORED 0x00000004u
#define PULSE_BRIDGE_PROBE_ENABLED 0x00000008u
#define PULSE_BRIDGE_PROBE_MAX_PATH_UNITS 32767u

#pragma pack(push, 8)
typedef struct PulseBridgeProbeConfig {
    uint32_t size;
    uint32_t version;
    uint32_t machine; // IMAGE_FILE_MACHINE_I386 or IMAGE_FILE_MACHINE_AMD64.
    uint32_t flags; // Only ENABLE; zero installs an original-call bypass.
    HMODULE target_exe; // Must equal GetModuleHandleW(NULL), never a DLL.
    const WCHAR* host_path; // Optional bounded UTF-16; no embedded NUL.
    uint32_t host_path_units; // Excludes terminator; copied before Install returns.
    uint32_t reserved[5]; // All zero.
} PulseBridgeProbeConfig;

typedef struct PulseBridgeProbeState {
    uint32_t size; // Caller initializes size/version/machine/reserved.
    uint32_t version;
    uint32_t machine;
    uint32_t flags;
    HRESULT last_status; // Install/Deactivate snapshot; concurrent calls may replace it.
    DWORD last_probe_error; // Last barrier SetEvent/Wait failure, zero initially.
    uint64_t native_show_calls;
    uint64_t active_native_shows;
    uint32_t reserved[4];
} PulseBridgeProbeState;

typedef HRESULT (WINAPI* PulseBridgeProbeInstallFn)(const PulseBridgeProbeConfig* config);
typedef HRESULT (WINAPI* PulseBridgeProbeDeactivateFn)(void);
typedef HRESULT (WINAPI* PulseBridgeProbeGetStateFn)(PulseBridgeProbeState* state);
// required includes NUL. Insufficient capacity returns ERROR_INSUFFICIENT_BUFFER
// and required, without partially writing buffer. Last successful config remains
// available after Deactivate. No DLL-allocated memory crosses this boundary.
typedef HRESULT (WINAPI* PulseBridgeProbeCopyHostPathFn)(WCHAR* buffer, uint32_t capacity, uint32_t* required);
// Probe-only: nonempty barriers require confirmed PIN (otherwise E_UNEXPECTED,
// no handle duplication). An empty clear succeeds even before installation.
// Duplicate both event handles into DLL ownership. Native Show is
// replaced by a fixed ERROR_CANCELLED response, optionally after signaling
// entered and waiting for resume (1..30000 ms). NULL,NULL,0 clears the barrier.
// A running Show retains its previous barrier. This never opens validation gate.
typedef HRESULT (WINAPI* PulseBridgeProbeSetShowBarrierFn)(HANDLE entered, HANDLE resume, uint32_t timeout_ms);

typedef struct PulseBridgeProbeApi {
    uint32_t size;
    uint32_t version;
    uint32_t machine;
    uint32_t reserved;
    PulseBridgeProbeInstallFn install;
    PulseBridgeProbeDeactivateFn deactivate;
    PulseBridgeProbeGetStateFn get_state;
    PulseBridgeProbeCopyHostPathFn copy_host_path;
    PulseBridgeProbeSetShowBarrierFn set_show_barrier;
} PulseBridgeProbeApi;
#pragma pack(pop)

// The caller retains its LoadLibrary reference until calls finish or state
// confirms PINNED; concurrent FreeLibrary before pin is not supported.
// Stable undecorated export name on both architectures. Unknown version/size is
// rejected without writes. No work is performed by DllMain. Pinning is permanent
// until process exit; Deactivate restores routing, not DLL unloadability and not
// a drain barrier. Before PIN, queries/rejections allocate no persistent state;
// Deactivate returns S_FALSE and CopyHostPath reports an empty string.
// This API does not implement third-party injection or upgrade.
typedef HRESULT (WINAPI* PulseBridgeProbeGetApiFn)(uint32_t version, uint32_t api_size, PulseBridgeProbeApi* api);

// Independent probe-only extension. The v1 table above remains unchanged.
#define PULSE_BRIDGE_PROBE_FIXTURE_VERSION 1u
#define PULSE_BRIDGE_PROBE_FIXTURE_PRESENT 0x00000001u
#define PULSE_BRIDGE_PROBE_MAX_FIXTURE_BYTES (1024u * 1024u)
#pragma pack(push, 8)
typedef struct PulseBridgeProbeFixture {
    uint32_t size;
    uint32_t version;
    uint32_t machine;
    uint32_t flags; // Must be zero.
    const uint8_t* bytes; // Complete existing picker wire Message, copied.
    uint32_t byte_count;
    uint32_t reserved[3]; // All zero.
} PulseBridgeProbeFixture;

typedef struct PulseBridgeProbeFixtureState {
    uint32_t size; // Caller initializes size/version/machine/reserved.
    uint32_t version;
    uint32_t machine;
    uint32_t flags; // FIXTURE_PRESENT or zero.
    uint64_t generation; // Current selected profile, not necessarily installed.
    uint64_t validation_calls;
    uint64_t validation_matches;
    uint64_t bridge_calls; // Actual RunPickerClient invocations.
    uint64_t active_bridge_calls;
    uint64_t accepted; // Terminal counts below cover actual client invocations.
    uint64_t cancelled;
    uint64_t unsupported;
    uint64_t failed;
    uint64_t rejected_dispatches; // Defensive body mismatch, no client launched.
    uint32_t last_terminal_status; // picker Status; zero before a terminal result.
    uint32_t last_error_source; // picker ErrorSource.
    uint32_t last_error_code;
    uint32_t reserved0;
    uint8_t fixture_request_id[16]; // Decoded registration ID, not part of match.
    uint8_t last_request_id[16]; // ID associated with the last terminal details.
    uint32_t reserved[4];
} PulseBridgeProbeFixtureState;

// Only after PIN: bounded strict Decode, OpenFiles/SaveFile Request only.
// Pre-PIN returns E_UNEXPECTED; bad bytes/semantics return E_INVALIDARG, wire
// version mismatch ERROR_REVISION_MISMATCH, unsupported capability/body/operation
// ERROR_NOT_SUPPORTED (each Win32 code wrapped as HRESULT).
// Failures preserve the selected profile and its counters. Selection affects a
// later successful Install, never existing routing or previously made wrappers.
typedef HRESULT (WINAPI* PulseBridgeProbeSetFixtureFn)(const PulseBridgeProbeFixture* fixture);
// No profile (including pre-PIN) returns S_FALSE without allocation.
typedef HRESULT (WINAPI* PulseBridgeProbeClearFixtureFn)(void);
// Queries only the current selected generation; old wrappers keep private
// shared snapshots, and their calls are NOT included in a replacement profile.
// Concurrent counters are sampled; terminal status/error/request ID are one
// coherent completed-call record. No selection yields zero flags/counts/IDs.
typedef HRESULT (WINAPI* PulseBridgeProbeGetFixtureStateFn)(PulseBridgeProbeFixtureState* state);
typedef struct PulseBridgeProbeFixtureApi {
    uint32_t size;
    uint32_t version;
    uint32_t machine;
    uint32_t reserved;
    PulseBridgeProbeSetFixtureFn set_fixture;
    PulseBridgeProbeClearFixtureFn clear_fixture;
    PulseBridgeProbeGetFixtureStateFn get_state;
} PulseBridgeProbeFixtureApi;
#pragma pack(pop)
// Stable export PulseBridgeProbeGetFixtureApi, independent version negotiation.
// Install with ENABLE and a selected fixture requires a nonempty absolute host
// executable path. Match compares the complete Request body, ignoring only ID;
// no match/no fixture still uses the probe's fixed native cancellation seam.
typedef HRESULT (WINAPI* PulseBridgeProbeGetFixtureApiFn)(uint32_t version, uint32_t api_size, PulseBridgeProbeFixtureApi* api);
