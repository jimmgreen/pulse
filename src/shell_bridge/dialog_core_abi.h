#pragma once

#include <windows.h>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Stable C ABI between the fixed-path thin Stub DLL (PulseDialogStub64/32.dll)
// and hot-swappable versioned Core DLLs (v\<sha256>\PulseDialogCore64/32.dll).
#define PULSE_DIALOG_CORE_ABI_VERSION 1u
#define PULSE_DIALOG_CORE_MAGIC 0x50444331u /* 'PDC1' */

#define PULSE_STUB_FLAG_COM_ONLY            0x00000001u
#define PULSE_STUB_FLAG_INSTALL_SINK_HOOKS  0x00000002u
#define PULSE_STUB_FLAG_WINRT_ENABLED       0x00000004u
#define PULSE_STUB_FLAG_DIRECT_REVEAL       0x00000008u

#pragma pack(push, 8)
typedef struct PulseStubConfigV1 {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t generation;
    const wchar_t* bridge_root;
    const wchar_t* core_dir;
    const wchar_t* host_exe;
} PulseStubConfigV1;

typedef struct PulseDialogCoreV1 {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t size;
    uint32_t reserved;
    uint64_t core_build_id;
    HRESULT (WINAPI* initialize)(const PulseStubConfigV1* config);
    void    (WINAPI* shutdown)(void);
    HRESULT (WINAPI* dll_get_class_object)(REFCLSID clsid, REFIID iid, void** out);
    DWORD   (WINAPI* activate_process)(const PulseStubConfigV1* config);
    DWORD   (WINAPI* initialize_direct_reveal)(void* diagnostic_out);
    uint32_t (WINAPI* get_active_hook_mask)(void);
} PulseDialogCoreV1;

typedef struct PulseStubStatusV1 {
    uint32_t size;
    uint32_t version;
    uint32_t core_loaded;
    uint32_t active_hook_mask;
    uint64_t generation;
    uint64_t core_build_id;
    DWORD last_error;
    uint32_t reserved;
} PulseStubStatusV1;
#pragma pack(pop)

// Core DLL exported entry point queried by PulseDialogStub.
typedef HRESULT (WINAPI* PulseDialogCoreQueryV1Fn)(PulseDialogCoreV1* out_table);

#ifdef __cplusplus
}
#endif
