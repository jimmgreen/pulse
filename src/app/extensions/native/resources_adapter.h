#pragma once
#include "../../../../sdk/native/include/pulse/resources.h"
#include "files_adapter.h"
namespace pulse::extensions::native {
// Internal ResourceService JSON conversion only; the public ABI is typed.
class ResourcesAdapter final {
  public:
    ResourcesAdapter(AppState &owner, CallerBinding caller);
    ResourcesAdapter(CallerBinding caller, FilesBackend backend);
    ~ResourcesAdapter();
    ResourcesAdapter(const ResourcesAdapter &) = delete;
    ResourcesAdapter &operator=(const ResourcesAdapter &) = delete;
    PulseResourcesV1 Table() const noexcept;
    PulseStatus Query(uint32_t version, void *out_table, uint32_t out_size) const noexcept;
    // STATE means a DLL callback is on the stack; defer unloading until it returns.
    PulseStatus Close() noexcept;
    bool CallbackActive() const noexcept;
    struct Impl;

  private:
    std::shared_ptr<Impl> impl_;
};
} // namespace pulse::extensions::native
