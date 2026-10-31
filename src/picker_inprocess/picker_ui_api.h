#pragma once
#include <windows.h>
#include <stdint.h>

// Controlled proof ABI only. Not a production activation or installation API.
// All structures/buffers remain caller-owned. Strings use counted UTF-16 units;
// callback data is borrowed only until the callback returns. No CRT allocations
// cross this boundary. Initialize/Show/GetWindow/Close/Shutdown run on one STA.
#define PULSE_PICKER_UI_ABI 1u
#define PULSE_PICKER_UI_MAX_UNITS 32767u
#define PULSE_PICKER_UI_DARK 1u
#define PULSE_PICKER_UI_READY 1u
#define PULSE_PICKER_UI_FILE_OK 2u
#define PULSE_PICKER_UI_ACCEPTED 1u
#define PULSE_PICKER_UI_CANCELLED 2u
#define PULSE_PICKER_UI_FAILED 3u

typedef struct PulsePickerUiConfig {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    HWND owner; // Null or owned by this process's calling STA in this proof.
    const wchar_t* directory;
    uint32_t directory_units;
    const wchar_t* filename;
    uint32_t filename_units;
} PulsePickerUiConfig;

typedef struct PulsePickerUiEvent {
    uint32_t size;
    uint32_t kind;
    HWND window; // Actual Pulse picker HWND on the Show STA.
    const wchar_t* path; // FILE_OK candidate only; borrowed, not yet committed.
    uint32_t path_units;
    uint32_t filter_index; // Zero-based.
} PulsePickerUiEvent;

typedef HRESULT (WINAPI* PulsePickerUiCallback)(void* context, const PulsePickerUiEvent* event);
typedef struct PulsePickerUiCallbacks {
    uint32_t size;
    uint32_t reserved;
    PulsePickerUiCallback callback;
    void* context;
} PulsePickerUiCallbacks;

typedef struct PulsePickerUiResult {
    uint32_t size;
    uint32_t status;
    uint32_t win32_error;
    uint32_t path_units; // Excludes trailing NUL. Zero on cancellation/failure.
    wchar_t* path;
    uint32_t path_capacity; // At least MAX_UNITS+1, caller allocation.
} PulsePickerUiResult;

typedef struct PulsePickerUiApi {
    uint32_t size;
    uint32_t version;
    // Pins the DLL for process lifetime (WndProc/worker safety), balances its
    // own OleInitialize with Shutdown. No DllMain work or process DPI mutation.
    HRESULT (WINAPI* Initialize)(uint32_t flags); // Reserved: must be zero; theme is per Show.
    // S_OK/S_FALSE from FILE_OK accept/veto; veto leaves the same window alive.
    // Other callback failures abort with failure; no callback crosses threads.
    HRESULT (WINAPI* Show)(const PulsePickerUiConfig*, const PulsePickerUiCallbacks*, PulsePickerUiResult*);
    HRESULT (WINAPI* GetWindow)(HWND* window); // Only during Show; no fake HWND.
    HRESULT (WINAPI* Close)(void); // Cancel active Show, including from callback.
    HRESULT (WINAPI* Shutdown)(void); // Rejects active Show; does not unpin DLL.
} PulsePickerUiApi;
#ifdef __cplusplus
extern "C" {
#endif
HRESULT WINAPI PulsePickerUiGetApi(uint32_t version, uint32_t size, PulsePickerUiApi* api);
#ifdef __cplusplus
}
#endif
