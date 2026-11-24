#pragma once
#include "file_dialog_inprocess_session.h"
#include <cstdint>
namespace pulse::shell_bridge {
struct RemoteProviderTimings {
    uint64_t spawn_ms = 0;
    uint64_t accept_ms = 0;
    uint64_t hello_ms = 0;
    uint64_t total_ms = 0;
    uint32_t host_pid = 0;
    bool warm_connected = false;
};
HRESULT LoadFileDialogRemoteProvider(const std::wstring&, std::shared_ptr<const FileDialogInprocessProvider>&, bool allow_unverified, RemoteProviderTimings* timings = nullptr) noexcept;
}
