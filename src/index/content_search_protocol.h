#pragma once

#include "../ipc/protocol.h"

namespace pulse::index::content {

inline constexpr uint32_t kMagic = 0x43535050; // 'PPSC'
inline constexpr size_t kMaximumPayload = 8 * 1024 * 1024;

enum Message : uint32_t {
    REQ_SEARCH = 1,
    RSP_BATCH = 101,
};

inline std::wstring PipeName(std::wstring_view token) {
    return L"\\\\.\\pipe\\PulseContentSearch." + std::wstring(token);
}

inline ipc::MsgHeader Header(uint32_t type, uint32_t size) {
    ipc::MsgHeader header;
    header.magic = kMagic;
    header.type = type;
    header.payload_size = size;
    return header;
}

} // namespace pulse::index::content
