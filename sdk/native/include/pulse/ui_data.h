#ifndef PULSE_NATIVE_UI_DATA_H
#define PULSE_NATIVE_UI_DATA_H
#include "ui_model.h"
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
#define PULSE_UI_DATA_INTERFACE "pulse.ui.data"
#define PULSE_UI_DATA_VERSION 1u
enum { PULSE_UI_DATA_REFRESH = 0, PULSE_UI_DATA_NEXT_PAGE = 1, PULSE_UI_DATA_CANCEL = 2 };
enum { PULSE_UI_DATA_LOADING = 0, PULSE_UI_DATA_READY = 1, PULSE_UI_DATA_ERROR = 2 };
enum { PULSE_UI_DATA_SNAPSHOT = 0, PULSE_UI_DATA_DELTA = 1 };
typedef struct PulseUiDataRequest {
    uint32_t size, page_size;
    uint64_t request, provider, panel;
    PulseStringView node, filter, sort, cursor;
} PulseUiDataRequest;
typedef void (PULSE_CALL *PulseUiDataRequestFn)(void* user, const PulseUiDataRequest* request);
typedef void (PULSE_CALL *PulseUiDataCancelFn)(void* user, uint64_t request);
typedef struct PulseUiDataBinding {
    uint32_t size, page_size; /* 1..256; <=16 bindings and <=16 providers per instance. */
    PulseStringView filter_state, sort_state, status_state, error_state, progress_state;
} PulseUiDataBinding;
typedef struct PulseUiDataPage {
    uint32_t size, status, mode, progress; /* progress 0..100; see enums above. */
    PulseStringView revision, base_revision, next_cursor, error;
    const PulseUiModelRow* rows;
    uint32_t row_count;
    const PulseStringView* removed_ids;
    uint32_t removed_count;
} PulseUiDataPage;
/* Request/cancel callbacks and all operations except publish run on the UI thread.
   Callbacks must return promptly; compute on plugin-owned workers. Data control
   operations cannot reenter a request/cancel callback; use dispatch to defer them.
   publish and pulse.ui.close remain available during callbacks. No exceptions
   cross the ABI. Request views last only through the callback. publish copies all
   borrowed data synchronously, may run on any thread, and never invokes plugin code.
   Cancel invalidates the request BEFORE the cancel callback. Later publish returns
   STATE. cancel callback only signals work; destroy joins all plugin-owned workers.
   Host context remains valid through plugin destroy, not after it.

   bind targets an existing empty virtualList/tree/dataTable in pulse.ui.model.
   It attaches the shared renderer's real data controller, not a second widget.
   Selection state must initially be empty. Config state keys must exist, have
   distinct roles, and not overlap the selection key. sort starts nameAsc/nameDesc.
   bind does not request data; request REFRESH starts a live stream. Filter/sort
   state changes automatically cancel/restart it. NEXT_PAGE cancels the previous
   stream and requests its cursor; the revision must match. Rebind after model
   update: successful update, close, stop, unregister and unbind cancel requests.
   Unbind clears displayed rows and selection. Providers remain until unregister
   or stop. Handles belong to one plugin instance and cannot be reused elsewhere.

   SNAPSHOT publishes <=page_size rows (<=4096 recursive rows), depth<=8, <=16
   cells/row, total UTF-8<=65536 bytes/page; IDs unique within a page, <=64 bytes.
   Rows have the same semantics as PulseUiModelRow; row commands must be declared
   native commands. READY requires a nonempty revision (<=128 bytes). Cursors<=256
   bytes, errors<=1024. LOADING/ERROR carry no rows/cursor/delta; ERROR needs error.
   DELTA is READY with base_revision matching displayed revision; it upserts stable
   row IDs and removes top-level IDs (<=256). It requires an established snapshot,
   is not allowed for a next-page stream, and clears pagination. Tree changes to
   descendants replace their top-level ancestor. Total displayed rows<=4096.
   Initial empty/error/loading, refresh, pagination and deltas are observable via
   configured state keys. Stale revisions/overflow become error states, not success.
   The controller inbox is bounded (64 pages); overflow fails the stream until
   refresh. PULSE_OK on publish means copied/queued, not yet displayed. */
typedef struct PulseUiDataV1 {
    uint32_t size, version;
    void* context;
    PulseStatus (PULSE_CALL *register_provider)(void*, PulseUiDataRequestFn, PulseUiDataCancelFn, void* user, uint64_t* provider);
    PulseStatus (PULSE_CALL *unregister_provider)(void*, uint64_t provider);
    PulseStatus (PULSE_CALL *bind)(void*, uint64_t panel, PulseStringView node, uint64_t provider, const PulseUiDataBinding*);
    PulseStatus (PULSE_CALL *unbind)(void*, uint64_t panel, PulseStringView node);
    PulseStatus (PULSE_CALL *request)(void*, uint64_t panel, PulseStringView node, uint32_t operation);
    PulseStatus (PULSE_CALL *publish)(void*, uint64_t request, const PulseUiDataPage*);
} PulseUiDataV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif
