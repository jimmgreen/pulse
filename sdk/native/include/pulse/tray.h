#ifndef PULSE_NATIVE_TRAY_H
#define PULSE_NATIVE_TRAY_H
#include "abi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PULSE_TRAY_INTERFACE_NAME "pulse.tray"
#define PULSE_TRAY_VERSION_1 1u
#pragma pack(push, 8)
typedef uint64_t PulseTraySubscription;

enum {
    PULSE_TRAY_ITEM_NONE = 0u,
    PULSE_TRAY_ITEM_FOLDER = 1u,
    PULSE_TRAY_ITEM_MOVE_INTENT = 2u,
    PULSE_TRAY_ITEM_MISSING = 4u,
    PULSE_TRAY_ITEM_IN_FLIGHT = 8u
};

enum {
    PULSE_TRAY_RELEASE_BATCH_DEFAULT = 0u,
    PULSE_TRAY_RELEASE_COPY = 1u,
    PULSE_TRAY_RELEASE_MOVE = 2u
};

typedef struct PulseTrayEntry {
    uint64_t item_id;
    uint64_t batch_id;
    uint64_t size_bytes;
    uint32_t flags;
    uint32_t reserved;
    PulseStringView path;
    PulseStringView name;
} PulseTrayEntry;

typedef struct PulseTraySnapshot {
    uint32_t size;
    uint32_t entry_count;
    uint64_t revision;
    uint64_t total_bytes;
    const PulseTrayEntry* entries;
} PulseTraySnapshot;

typedef void(PULSE_CALL *PulseTraySnapshotFn)(void* user, const PulseTraySnapshot* snapshot);

/* UI-thread bidirectional bridge to Pulse's shared StagingTray (AppCore::tray).
   snapshot invokes callback synchronously; subscribe delivers deferred initial and
   coalesced change snapshots at the session tick. Borrowed strings and entry arrays
   expire when callback returns. */
typedef struct PulseTrayV1 {
    uint32_t size, version;
    void* context;
    PulseStatus(PULSE_CALL *snapshot)(void* context, PulseTraySnapshotFn callback, void* user);
    PulseStatus(PULSE_CALL *collect)(void* context, const PulseStringView* paths, uint32_t path_count, uint32_t move_intent);
    PulseStatus(PULSE_CALL *remove_item)(void* context, uint64_t item_id);
    PulseStatus(PULSE_CALL *clear)(void* context);
    PulseStatus(PULSE_CALL *release_to_directory)(void* context, PulseStringView destination_dir, uint32_t release_mode);
    PulseStatus(PULSE_CALL *subscribe)(void* context, PulseTraySnapshotFn callback, void* user,
        PulseTraySubscription* out_subscription);
    PulseStatus(PULSE_CALL *unsubscribe)(void* context, PulseTraySubscription subscription);
} PulseTrayV1;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
#endif
