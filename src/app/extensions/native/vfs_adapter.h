#pragma once

#include "../../../../sdk/native/include/pulse/vfs.h"
#include "../vfs_registry.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pulse::extensions::native {

class DiagnosticsSink;

struct VfsBackend {
    std::function<bool()> valid_owner;
    std::function<void(std::string_view package_id,
                       std::string_view mount_id,
                       std::string_view rel_path_utf8)> invalidate_path;
    std::function<PulseStatus(const std::wstring& vfs_uri, uint32_t mode)> open_path;
};

class VfsAdapter {
public:
    VfsAdapter(std::string package_id,
               VfsBackend backend,
               std::function<bool()> alive,
               std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
               std::function<void()> wake = nullptr,
               vfs::VfsRegistry* registry = &vfs::VfsRegistry::Instance());
    ~VfsAdapter();
    VfsAdapter(const VfsAdapter&) = delete;
    VfsAdapter& operator=(const VfsAdapter&) = delete;

    void SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink);
    void SetWake(std::function<void()> wake);
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native
