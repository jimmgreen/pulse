// index_protocol.h — pulse.exe <-> Pulse.Index.exe named-pipe protocol.
#pragma once
#include "../ipc/protocol.h"
#include "index_engine.h"

namespace pulse::index {

inline constexpr uint32_t kIndexMagic = 0x58444950; // 'PIDX'
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\PulseIndex";
inline constexpr wchar_t kServiceName[] = L"PulseIndex";
inline constexpr wchar_t kMutexName[] = L"Local\\Pulse.Index.Singleton";
inline constexpr size_t kIndexMaxRequestPayload = 256 * 1024;
inline constexpr size_t kIndexMaxPayload = 48 * 1024 * 1024;

enum IndexMsg : uint32_t {
    REQ_IDX_STATUS = 1,
    REQ_IDX_SEARCH = 2,
    REQ_IDX_VOLUMES = 3,
    REQ_IDX_TEST_SHUTDOWN = 0x7fff0001,
    RSP_IDX_STATUS = 101,
    RSP_IDX_SEARCH = 102,
    RSP_IDX_VOLUMES = 103,
};

inline ipc::MsgHeader MakeIndexHdr(uint32_t type, uint32_t id, uint32_t size) {
    ipc::MsgHeader h;
    h.magic = kIndexMagic;
    h.type = type;
    h.request_id = id;
    h.payload_size = size;
    return h;
}

} // namespace pulse::index
