#pragma once

#include "../ipc/protocol.h"

namespace pulse::index::agent {

inline constexpr uint32_t kMagic = 0x544E5050; // 'PPNT'
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\PulseNetworkIndex";
inline constexpr size_t kMaxPayload = 16 * 1024 * 1024;

enum Message : uint32_t {
    REQ_STATUS = 1,
    REQ_SEARCH = 2,
    REQ_ROOTS = 3,
    REQ_ADD_ROOT = 4,
    REQ_REMOVE_ROOT = 5,
    REQ_REBUILD = 6,
    RSP_STATUS = 101,
    RSP_SEARCH = 102,
    RSP_ROOTS = 103,
    RSP_RESULT = 104,
};

inline ipc::MsgHeader MakeHeader(uint32_t type, uint32_t id, uint32_t size) {
    ipc::MsgHeader header;
    header.magic = kMagic;
    header.type = type;
    header.request_id = id;
    header.payload_size = size;
    return header;
}

} // namespace pulse::index::agent
