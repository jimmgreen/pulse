#pragma once
#include <windows.h>
#include <stdint.h>

// Experimental v2 contract. Runtime integration remains disabled pending tests.
// Independent of v1; publishing this declaration does not enable a runtime gate.
// C POD, natural platform ABI alignment, WINAPI, same-bitness caller and DLL.
// Never pass STL/CRT ownership across this boundary. All input UTF-16 is counted,
// well-formed, excludes NUL, and is copied before Show pumps messages. A zero
// count permits NULL. Output counts exclude NUL; capacities include its space.
// Read each structure's size first; reject wrong size before reading other fields.
// All reserved input fields must be zero; reject unknown flags/options/capabilities.
#define PULSE_PICKER_UI_V2_VERSION 2u
#define PULSE_PICKER_UI_V2_MAX_UNITS 32767u
#define PULSE_PICKER_UI_V2_MAX_FILTERS 64u
#define PULSE_PICKER_UI_V2_MAX_INPUT_UNITS 65536u
#define PULSE_PICKER_UI_V2_CAP_OPEN_LOCAL_V1 0x1u
#define PULSE_PICKER_UI_V2_CAP_STANDARD_EVENTS 0x2u
#define PULSE_PICKER_UI_V2_CAP_STATE_QUERY 0x4u
#define PULSE_PICKER_UI_V2_CAP_CUSTOM_BUTTONS 0x8u
#define PULSE_PICKER_UI_V2_CAP_SAVE_LOCAL_V1 0x10u
#define PULSE_PICKER_UI_V2_CAP_FOLDER_V1 0x20u
#define PULSE_PICKER_UI_V2_FEATURE_PROPERTIES 0x2000u
#define PULSE_PICKER_UI_V2_FEATURE_SAVE_SKIP_ACCESS 0x4000u
#define PULSE_PICKER_UI_V2_FEATURE_NAVIGATE 0x8000u
#define PULSE_PICKER_UI_V2_FEATURE_SAVE_NATIVE_DEFAULTS 0x10000u
#define PULSE_PICKER_UI_V2_FEATURE_OPEN_MISSING 0x20000u
#define PULSE_PICKER_UI_V2_FEATURE_SAVE_NO_TEST_CREATE 0x40000u
#define PULSE_PICKER_UI_V2_EVENT_PROPERTY_CHANGED 13u
#define PULSE_PICKER_UI_V2_EVENT_NAVIGATION_COMPLETE 14u
#define PULSE_PICKER_UI_V2_PROPERTY_SCALAR 1u
#define PULSE_PICKER_UI_V2_PROPERTY_LIST 2u
#define PULSE_PICKER_UI_V2_MAX_PROPERTIES 16u
#define PULSE_PICKER_UI_V2_MAX_PROPERTY_VALUES 16u
#define PULSE_PICKER_UI_V2_MAX_CONTROLS 16u
#define PULSE_PICKER_UI_V2_MAX_CONTROL_ITEMS 32u
#define PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS 0x1u
#define PULSE_PICKER_UI_V2_FEATURE_CUSTOM_COMBOBOX 0x2u
#define PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT 0x4u
#define PULSE_PICKER_UI_V2_FEATURE_OPEN_DEFAULT_EXTENSION 0x8u
#define PULSE_PICKER_UI_V2_FEATURE_SAVE_UNENCRYPTED 0x10u
#define PULSE_PICKER_UI_V2_FEATURE_FILESYSTEM_PLACES 0x80u
#define PULSE_PICKER_UI_V2_MAX_PLACES 64u
#define PULSE_PICKER_UI_V2_PLACE_TOP 1u
#define PULSE_PICKER_UI_V2_PLACE_BOTTOM 0u
#define PULSE_PICKER_UI_V2_MAX_PATHS 1024u
#define PULSE_PICKER_UI_V2_MAX_PACKED_UNITS 524288u
#define PULSE_PICKER_UI_V2_LABEL_TITLE 0x1u
#define PULSE_PICKER_UI_V2_LABEL_OK 0x2u
#define PULSE_PICKER_UI_V2_LABEL_FILE_NAME 0x4u
#define PULSE_PICKER_UI_V2_CONTROL_GROUP 1u
#define PULSE_PICKER_UI_V2_CONTROL_BUTTON 2u
#define PULSE_PICKER_UI_V2_CONTROL_COMBO 3u
#define PULSE_PICKER_UI_V2_CONTROL_MENU 4u
#define PULSE_PICKER_UI_V2_CONTROL_OPEN_DROPDOWN 5u
#define PULSE_PICKER_UI_V2_CONTROL_RADIO 6u
#define PULSE_PICKER_UI_V2_CONTROL_CHECK 7u
#define PULSE_PICKER_UI_V2_CONTROL_EDIT 8u
#define PULSE_PICKER_UI_V2_FEATURE_CUSTOM_RADIO 0x400u
#define PULSE_PICKER_UI_V2_FEATURE_CUSTOM_CHECK 0x800u
#define PULSE_PICKER_UI_V2_FEATURE_CUSTOM_EDIT 0x1000u
#define PULSE_PICKER_UI_V2_FEATURE_CUSTOM_MENU 0x20u
#define PULSE_PICKER_UI_V2_FEATURE_OPEN_DROPDOWN 0x40u
#define PULSE_PICKER_UI_V2_CONTROL_ENABLED 0x1u
#define PULSE_PICKER_UI_V2_CONTROL_VISIBLE 0x2u
#define PULSE_PICKER_UI_V2_CONTROL_PROMINENT 0x4u
#define PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_GROUP 0x80000u
#define PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_CONTROL 0x100000u
#define PULSE_PICKER_UI_V2_FEATURE_NATIVE_OPTIONS 0x200000u
#define PULSE_PICKER_UI_V2_NATIVE_OPTIONS_MASK 0xF237FBEEu
#define PULSE_PICKER_UI_V2_DARK 0x1u
// Deliberately distinct from FILEOPENDIALOGOPTIONS. The wrapper must map and
// validate every native setting; no flags are silently discarded. Local file
// Open, filesystem folder selection, and restricted LocalSaveV1 have distinct
// access policies. Multiselect requires its explicit option and feature.
// LocalOpenV1 filters are advisory for a typed file: the listing uses the selected
// filter, but worker validation may accept an existing ordinary file outside it.
// FEATURE_NATIVE_OPTIONS carries STRICTFILETYPES for Save; Open keeps native
// advisory typed-filter semantics even when the caller supplies that flag.
// A wire adapter must require the paired AdvisoryFileFilters/AdvisoryFilters
// contract when mapping nonempty filters; do not silently reinterpret strict.
#define PULSE_PICKER_UI_V2_OPEN_SHOW_HIDDEN 0x1u
#define PULSE_PICKER_UI_V2_OPEN_NO_DEREFERENCE_LINKS 0x2u
#define PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT 0x4u
#define PULSE_PICKER_UI_V2_OPEN_USE_DEFAULT_EXTENSION 0x8u
#define PULSE_PICKER_UI_V2_OPEN_SKIP_ACCESS_VALIDATION 0x10u
#define PULSE_PICKER_UI_V2_OPEN_ALLOW_MISSING 0x20u
#define PULSE_PICKER_UI_V2_OPEN_REQUIRE_EXISTING_PARENT 0x40u
#define PULSE_PICKER_UI_V2_FEATURE_OPEN_SKIP_ACCESS 0x100u
#define PULSE_PICKER_UI_V2_FEATURE_COMPUTER_PLACE 0x200u
#define PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1 1u
#define PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1 2u
#define PULSE_PICKER_UI_V2_ACCESS_FOLDER_V1 3u
#define PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT 0x1u
#define PULSE_PICKER_UI_V2_SAVE_REQUIRE_UNENCRYPTED 0x2u
#define PULSE_PICKER_UI_V2_SAVE_SKIP_ACCESS_VALIDATION 0x4u
#define PULSE_PICKER_UI_V2_SAVE_NATIVE_DEFAULTS 0x8u
#define PULSE_PICKER_UI_V2_SAVE_NO_TEST_CREATE 0x10u
#define PULSE_PICKER_UI_V2_SAVE_REQUIRE_EXISTING_PARENT 0x20u
#define PULSE_PICKER_UI_V2_OVERWRITE_DEFAULT 0u
#define PULSE_PICKER_UI_V2_OVERWRITE_ACCEPT 1u
#define PULSE_PICKER_UI_V2_OVERWRITE_REFUSE 2u

#define PULSE_PICKER_UI_V2_EVENT_READY 1u
#define PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING 2u
#define PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED 3u
#define PULSE_PICKER_UI_V2_EVENT_SELECTION_CHANGED 4u
#define PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED 5u
#define PULSE_PICKER_UI_V2_EVENT_FILE_OK 6u
#define PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED 7u
#define PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED 8u
#define PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING 9u
#define PULSE_PICKER_UI_V2_EVENT_OPEN_DROPDOWN_SELECTED 10u
#define PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED 11u
#define PULSE_PICKER_UI_V2_EVENT_EDIT_CHANGED 12u

#define PULSE_PICKER_UI_V2_ACCEPTED 1u
#define PULSE_PICKER_UI_V2_CANCELLED 2u
#define PULSE_PICKER_UI_V2_FAILED 3u
#define PULSE_PICKER_UI_V2_CLOSED 4u
#define PULSE_PICKER_UI_V2_UI_SHOWN 0x1u
#define PULSE_PICKER_UI_V2_STATE_VALIDATING 0x1u
#define PULSE_PICKER_UI_V2_STATE_CANDIDATE_READY 0x2u
#define PULSE_PICKER_UI_V2_STATE_CLOSING 0x4u

typedef uint64_t PulsePickerUiV2Session; // Opaque nonzero, non-reused token; never dereferenced.
typedef struct PulsePickerUiV2Text {
    const wchar_t* data;
    uint32_t units;
} PulsePickerUiV2Text;
typedef struct PulsePickerUiV2Buffer {
    wchar_t* data;
    uint32_t capacity;
    uint32_t units; // Required length even on ERROR_INSUFFICIENT_BUFFER.
} PulsePickerUiV2Buffer;
typedef struct PulsePickerUiV2Filter {
    uint32_t size;
    uint32_t reserved;
    PulsePickerUiV2Text label;
    PulsePickerUiV2Text pattern; // Conservative semicolon wildcard grammar; advisory file filters.
} PulsePickerUiV2Filter;
typedef struct PulsePickerUiV2Initialize {
    uint32_t size;
    uint32_t version;
    uint32_t reserved[2];
} PulsePickerUiV2Initialize;
// Ordered controls; a group precedes its buttons. parent_id is meaningful only
// with has_parent=1, so control ID zero remains valid. State uses the flags above.
typedef struct PulsePickerUiV2ControlItem {
    uint32_t size;
    uint32_t id;
    uint32_t state;
    uint32_t reserved;
    PulsePickerUiV2Text label;
} PulsePickerUiV2ControlItem;

typedef struct PulsePickerUiV2Control {
    uint32_t size;
    uint32_t kind;
    uint32_t id;
    uint32_t parent_id;
    uint32_t has_parent;
    uint32_t state;
    PulsePickerUiV2Text label;
    uint32_t reserved[2];
    const PulsePickerUiV2ControlItem* items; // COMBO only; copied, at most MAX_CONTROL_ITEMS.
    uint32_t item_count;
    uint32_t has_selected; // Distinguishes no selection from item ID zero.
    uint32_t selected_item_id;
    uint32_t checked;
    PulsePickerUiV2Text text;
} PulsePickerUiV2Control;

typedef struct PulsePickerUiV2Place {
    uint32_t size, location;
    PulsePickerUiV2Text label, path;
} PulsePickerUiV2Place;
typedef struct PulsePickerUiV2Property {
    uint32_t size, index, kind;
    PulsePickerUiV2Text label;
    const PulsePickerUiV2Text* values;
    uint32_t value_count;
} PulsePickerUiV2Property;
typedef struct PulsePickerUiV2Config {
    uint32_t size;
    uint32_t version;
    uint32_t required_capabilities;
    uint32_t appearance;
    uint32_t open_options;
    uint32_t access_policy; // Explicit LocalOpenV1, never NativeValidationParity.
    HWND owner;
    PulsePickerUiV2Text initial_folder; // Resolved filesystem folder, not a setter trace.
    PulsePickerUiV2Text filename;       // Optional leaf, not a Shell parsing expression.
    PulsePickerUiV2Text title;
    const PulsePickerUiV2Filter* filters;
    uint32_t filter_count;
    uint32_t filter_index; // Zero based; zero if no filters. COM conversion is outside ABI.
    const PulsePickerUiV2Control* controls;
    uint32_t control_count;
    uint32_t reserved[2];
    PulsePickerUiV2Text default_extension; // Extension without leading dot; Open requires OPEN_USE_DEFAULT_EXTENSION, including explicit empty.
    uint32_t save_options; // LocalSaveV1 requires OVERWRITE_PROMPT; Open must be zero.
    uint32_t label_flags; // Presence, including explicit empty. Legacy nonempty title still works.
    PulsePickerUiV2Text ok_button_label;
    PulsePickerUiV2Text file_name_label;
    const PulsePickerUiV2Place* places;
    uint32_t place_count;
    const PulsePickerUiV2Property* properties;
    uint32_t property_count;
    uint32_t native_options; // Original FILEOPENDIALOGOPTIONS; requires FEATURE_NATIVE_OPTIONS.
} PulsePickerUiV2Config;

// All four strings are one coherent UI snapshot; empty means no current item.
// Selection is a single selected filesystem path (independent of typed filename).
// Candidate exists only after worker validation for this exact generation.
// Query never runs Shell/filesystem validation synchronously on the Show thread.
typedef struct PulsePickerUiV2Snapshot {
    uint32_t size;
    uint32_t flags;
    uint64_t show_id;
    uint64_t generation;
    HWND window;
    uint32_t filter_index;
    uint32_t reserved;
    PulsePickerUiV2Buffer folder;
    PulsePickerUiV2Buffer filename;
    PulsePickerUiV2Buffer selection;
    PulsePickerUiV2Buffer candidate;
    // Packed paths: each nonempty path ends in NUL, units includes these NULs,
    // capacity must exceed units for one extra trailing NUL. Empty => count/units 0.
    // Multi-select requires both buffers; single-select callers may omit both.
    PulsePickerUiV2Buffer selection_paths;
    PulsePickerUiV2Buffer candidate_paths;
    uint32_t selection_count;
    uint32_t candidate_count;
} PulsePickerUiV2Snapshot;
typedef struct PulsePickerUiV2Event {
    uint32_t size;
    uint32_t kind;
    uint64_t show_id;
    uint64_t generation;
    HWND window; // Real picker HWND on Show STA, never a proxy.
    PulsePickerUiV2Text proposed_folder; // Only FOLDER_CHANGING; borrowed for callback.
    uint32_t control_id; // Only BUTTON_CLICKED; ID is not a window/control handle.
    uint32_t reserved[2];
    uint32_t item_id; // ITEM_SELECTED only; zero is a valid item ID.
    PulsePickerUiV2Text control_text; // EDIT_CHANGED, borrowed for callback.
    const PulsePickerUiV2Text* property_values; // PROPERTY_CHANGED: complete borrowed value list.
    uint32_t property_value_count;
    HRESULT navigation_result; // NAVIGATION_COMPLETE only; proposed_folder identifies the queued request.
} PulsePickerUiV2Event;
typedef HRESULT (WINAPI* PulsePickerUiV2Callback)(void*, const PulsePickerUiV2Event*);
typedef struct PulsePickerUiV2OverwriteEvent {
    uint32_t size;
    uint32_t reserved;
    uint64_t show_id;
    uint64_t generation;
    HWND window;
    PulsePickerUiV2Text target; // Borrowed normalized existing target, not an accepted result.
} PulsePickerUiV2OverwriteEvent;
typedef HRESULT (WINAPI* PulsePickerUiV2OverwriteCallback)(void*, const PulsePickerUiV2OverwriteEvent*, uint32_t* response);
// Unless explicit SAVE_NO_TEST_CREATE or SAVE_SKIP_ACCESS_VALIDATION is selected,
// LocalSaveV1 performs worker access probes: a new target is briefly CREATE_NEW /
// DELETE_ON_CLOSE. This is not side-effect-free or NativeValidationParity.
// Overwrite DEFAULT asks through Pulse UI, ACCEPT still revalidates, REFUSE stays.
// Callback Close/cancellation/identity changes take precedence over responses.
typedef struct PulsePickerUiV2Callbacks {
    uint32_t size;
    uint32_t reserved;
    PulsePickerUiV2Callback callback;
    void* context; // Caller keeps valid until synchronous Show returns.
    PulsePickerUiV2OverwriteCallback overwrite; // Optional: absent/E_NOTIMPL means DEFAULT.
    PulsePickerUiV2OverwriteCallback share_violation; // FDESVR_DEFAULT/ACCEPT/REFUSE (0/1/2); identity rules match overwrite.
} PulsePickerUiV2Callbacks;
typedef struct PulsePickerUiV2Result {
    uint32_t size;
    uint32_t status;
    uint32_t flags;
    uint32_t win32_error; // Win32 only; do not cast a DXGI/COM HRESULT here.
    HRESULT terminal_hr; // Exactly Show return, including explicit Close(hr).
    uint32_t filter_index;
    uint64_t show_id;
    uint64_t generation;
    PulsePickerUiV2Buffer path; // Caller provides MAX_UNITS+1 before Show.
    PulsePickerUiV2Buffer paths; // Same packed encoding; required for multi-select.
    uint32_t path_count; // Accepted only; path equals the first element.
} PulsePickerUiV2Result;

// THREAD/STATE CONTRACT
// Initialize/ValidateConfig/Show/queries/Close/Shutdown use the initializing STA;
// wrong thread => RPC_E_WRONG_THREAD, no mutation. One active Show per session;
// nested Show/Shutdown from a callback is rejected, never recursively destroys UI.
// Session handles are DLL-owned and released only by Shutdown; stale handles are
// invalid. DLL code must remain loaded/pinned while HWNDs/workers/handles can live.
// No DllMain initialization/waits, process DPI changes, or global DLL-search changes.
// Initial implementation rejects concurrent initialized sessions explicitly.
// Experimental builds advertise capabilities=0 until verified; a controlled test
// may call with required_capabilities=0, which is not a production admission policy.
// Owner support must be capability-checked before creating UI: same-STA managed
// owner or the separately verified caller-managed disabled external owner policy.
// Never enable an owner originally disabled; restore only verified owned changes.
//
// UI runs with scoped module activation context/DPI/language. Before invoking user
// code restore the caller's captured activation/DPI/language state; restore the
// UI's scoped state afterward, even on callback failure. Do not restore stale
// caller state over intentional callback changes: capture its outgoing state for
// subsequent callbacks/Show return for DPI/language. Activation is overlaid using
// a retained caller context; callbacks must balance their own activation frames.
// Never pop an outer UI frame across nested window-dispatch frames.
// No artificial keyboard messages, input-queue
// attachment, or message interception that bypasses the caller's actual hook.
// The caller remains responsible for COM sink lifetime and native HRESULT mapping.
//
// EVENT/REENTRY CONTRACT
// Callbacks run synchronously on Show STA while the actual HWND is alive. Query,
// GetWindow and Close are allowed inside callbacks; no nested event dispatch from
// those APIs. No ABI exceptions may escape. Avoid holding locks across user code.
// FOLDER_CHANGING and FILE_OK: S_OK proceeds, S_FALSE vetoes and retains the same
// HWND/state; FAILED(hr) terminates with failure, other success codes are invalid.
// Notification callbacks require S_OK; failures terminate without accepting.
// Folder-changing veto retains the committed folder/selection; proposed_folder
// is separate. Folder-changed fires only after committed navigation. Selection/
// type notifications observe already-updated snapshot data. No initial notification
// order or cross-event ordering is claimed until native-oracle tests establish it.
// A close during any callback wins over its return value; latch the first Close hr.
// Callbacks may pump messages: if generation changed, the validated candidate is
// stale and must not commit, even when FILE_OK returned S_OK. Revalidate first;
// never replay acceptance against a newly selected path without another FILE_OK.
// On FILE_OK veto, clear candidate but preserve filename/selection and dialog.
// State generation increases on folder/name/selection/filter changes and candidate
// invalidation. show_id is never reused during a session; no HWND-only identity.
// Ready requires usable input controls and successful first presentation; FileOk
// acceptance requires successful post-callback presentation and cancellation check.
// Further UI notifications while inside a callback are deferred, not recursively
// delivered. If a queued transition generation is no longer the current snapshot,
// this experimental implementation fails closed with ERROR_RETRY rather than
// delivering mismatched state or rolling Query backward. Full reentrant event
// compatibility and notification ordering remain gated on oracle tests.

typedef struct PulsePickerUiV2Api {
    uint32_t size;
    uint32_t version;
    uint32_t capabilities; // Advertise only implemented AND verified contracts.
    uint32_t reserved;
    HRESULT (WINAPI* Initialize)(const PulsePickerUiV2Initialize*, PulsePickerUiV2Session*);
    // Structural/support validation only; unknown contract => ERROR_NOT_SUPPORTED.
    // No UI or filesystem work. Show rechecks and owns a complete config copy.
    HRESULT (WINAPI* ValidateConfig)(PulsePickerUiV2Session, const PulsePickerUiV2Config*);
    HRESULT (WINAPI* Show)(PulsePickerUiV2Session, const PulsePickerUiV2Config*,
        const PulsePickerUiV2Callbacks*, PulsePickerUiV2Result*);
    HRESULT (WINAPI* GetWindow)(PulsePickerUiV2Session, uint64_t show_id, HWND*);
    // expected_generation==0 means current. Stale show/generation => ERROR_RETRY.
    // Size query: NULL/zero buffers permitted; reports lengths+metadata and
    // ERROR_INSUFFICIENT_BUFFER. Never partially copies strings; on insufficient
    // capacity all string buffers stay untouched. Requery with returned generation.
    // Output strings must not overlap each other or the snapshot structure.
    HRESULT (WINAPI* Query)(PulsePickerUiV2Session, uint64_t show_id,
        uint64_t expected_generation, PulsePickerUiV2Snapshot*);
    // Exact HRESULT, including S_OK, is preserved. CLOSED always has no result
    // path, so Close(S_OK) cannot fabricate an accepted selection. No active Show
    // => ERROR_INVALID_STATE. Signals closure; destroys HWND only after callback/
    // render stacks unwind. Ordinary user cancellation instead reports CANCELLED.
    HRESULT (WINAPI* Close)(PulsePickerUiV2Session, uint64_t show_id, HRESULT);
    HRESULT (WINAPI* Shutdown)(PulsePickerUiV2Session);
    // Active Show only, same STA. Changes label/state of an existing control.
    // Identity/kind/parent cannot change. Copies text before returning.
    HRESULT (WINAPI* UpdateControl)(PulsePickerUiV2Session, uint64_t show_id, const PulsePickerUiV2Control*);
    // Structural UI features, independent of verified-contract capabilities.
    // Exact API/config sizes are required: an older provider must be rejected,
    // never silently ignore appended caller settings.
    uint32_t implemented_features;
    // Same Show STA. Copies the path; veto/failure retains committed state.
    HRESULT (WINAPI* Navigate)(PulsePickerUiV2Session, uint64_t show_id, PulsePickerUiV2Text folder);
} PulsePickerUiV2Api;

// Show clears output status/length/path before work. ACCEPTED alone carries a
// path. CANCELLED => HRESULT_FROM_WIN32(ERROR_CANCELLED); FAILED => actual failure;
// CLOSED => exact Close HRESULT. UI_SHOWN prevents erroneous replay via native Show
// after Pulse displayed. Unsupported before display must leave UI_SHOWN clear.
// Recent-doc updates and native directory persistence are wrapper responsibilities;
// this ABI neither edits native dialog history nor claims full native equivalence.
#ifdef __cplusplus
extern "C" {
#endif
// Independent export; build/DEF integration is explicit, never substitutes v1.
HRESULT WINAPI PulsePickerUiGetApiV2(uint32_t version, uint32_t size, PulsePickerUiV2Api* api);
#ifdef __cplusplus
}
#endif

