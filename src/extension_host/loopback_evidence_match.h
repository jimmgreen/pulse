#pragma once
// clang-format off
#include <windows.h>
#include <fwpmtypes.h>
// clang-format on
#include <cstring>

namespace pulse::extension_host {
// Pure correlation predicate shared by the callback and boundary regressions.
inline bool MatchesLoopbackCapabilityDrop(const FWPM_NET_EVENT2 *event, PSID sid, const FWP_BYTE_BLOB &application,
                                          UINT16 port, FILETIME started) {
    if (!event || event->type != FWPM_NET_EVENT_TYPE_CAPABILITY_DROP || !event->capabilityDrop ||
        !event->capabilityDrop->isLoopback || !sid || !IsValidSid(sid))
        return false;
    const auto &header = event->header;
    constexpr UINT32 required = FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET | FWPM_NET_EVENT_FLAG_APP_ID_SET |
                                FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET | FWPM_NET_EVENT_FLAG_IP_VERSION_SET |
                                FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET | FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET;
    return (header.flags & required) == required && header.ipVersion == FWP_IP_VERSION_V4 && header.ipProtocol == 6 &&
           header.remoteAddrV4 == 0x7f000001 && header.remotePort == port &&
           CompareFileTime(&header.timeStamp, &started) >= 0 && header.packageSid && IsValidSid(header.packageSid) &&
           EqualSid(header.packageSid, sid) && application.size != 0 && application.data &&
           header.appId.size == application.size && header.appId.data &&
           std::memcmp(header.appId.data, application.data, application.size) == 0;
}
} // namespace pulse::extension_host
