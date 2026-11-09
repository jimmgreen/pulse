#include "../extension_host/loopback_evidence_match.h"
#include <cstdio>
#include <initializer_list>

int main() {
    using pulse::extension_host::MatchesLoopbackCapabilityDrop;
    int failures = 0;
    const auto check = [&](bool value, const char *name) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
        if (!value)
            ++failures;
    };
    unsigned char sid[SECURITY_MAX_SID_SIZE]{}, other[SECURITY_MAX_SID_SIZE]{};
    DWORD size = sizeof(sid), other_size = sizeof(other);
    if (!CreateWellKnownSid(WinWorldSid, nullptr, sid, &size) ||
        !CreateWellKnownSid(WinLocalSystemSid, nullptr, other, &other_size))
        return 2;
    unsigned char path[] = {1, 2, 3};
    FWP_BYTE_BLOB application{sizeof(path), path};
    FWPM_NET_EVENT_CAPABILITY_DROP0 drop{};
    drop.isLoopback = TRUE;
    FWPM_NET_EVENT2 event{};
    event.type = FWPM_NET_EVENT_TYPE_CAPABILITY_DROP;
    event.capabilityDrop = &drop;
    event.header.flags = FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET | FWPM_NET_EVENT_FLAG_APP_ID_SET |
                         FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET | FWPM_NET_EVENT_FLAG_IP_VERSION_SET |
                         FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET | FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET;
    event.header.ipVersion = FWP_IP_VERSION_V4;
    event.header.ipProtocol = 6;
    event.header.remoteAddrV4 = 0x7f000001;
    event.header.remotePort = 12345;
    event.header.packageSid = reinterpret_cast<SID *>(sid);
    event.header.appId = application;
    event.header.timeStamp = {100, 0};
    const FILETIME started{100, 0};
    const auto matches = [&](const FWPM_NET_EVENT2 &value) {
        return MatchesLoopbackCapabilityDrop(&value, sid, application, 12345, started);
    };
    check(matches(event), "exact capability drop matches SID application endpoint and time");
    auto changed = event;
    changed.header.packageSid = reinterpret_cast<SID *>(other);
    check(!matches(changed), "unrelated container rejected");
    changed = event;
    changed.header.remotePort++;
    check(!matches(changed), "other port rejected");
    changed = event;
    changed.header.remoteAddrV4 = 0x08080808;
    check(!matches(changed), "non-loopback endpoint rejected");
    changed = event;
    changed.header.timeStamp.dwLowDateTime--;
    check(!matches(changed), "historical event rejected");
    changed = event;
    changed.type = FWPM_NET_EVENT_TYPE_CLASSIFY_DROP;
    check(!matches(changed), "generic classify drop does not prove capability denial");
    changed = event;
    changed.header.appId.size--;
    check(!matches(changed), "different application rejected");
    changed = event;
    changed.header.ipProtocol = 17;
    check(!matches(changed), "non-TCP event rejected");
    for (UINT32 bit : {FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET, FWPM_NET_EVENT_FLAG_APP_ID_SET,
                       FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET, FWPM_NET_EVENT_FLAG_IP_VERSION_SET,
                       FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET, FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET}) {
        changed = event;
        changed.header.flags &= ~bit;
        check(!matches(changed), "missing required field flag rejected");
    }
    drop.isLoopback = FALSE;
    check(!matches(event), "capability drop must explicitly identify loopback");
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
