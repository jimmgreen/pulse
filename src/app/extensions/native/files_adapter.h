#pragma once
#include "../../../../sdk/native/include/pulse/files.h"
#include "../runtime.h"
#include <memory>
namespace pulse {
struct AppState;
}
namespace pulse::extensions::native {
// Internal seam: current production backend delegates to ResourceService and
// its JSON model. JSON never appears in the DLL ABI. All dispatch occurs on UI.
struct FilesBackend {
    using Completion = std::function<void(json::Value)>;
    using Cancel = std::function<void()>;
    std::function<bool(const CallerBinding &)> live;
    std::function<Cancel(const CallerBinding &, const json::Value &, Completion)> dispatch;
};
class FilesAdapter final {
  public:
    FilesAdapter(AppState &owner, CallerBinding caller);
    FilesAdapter(CallerBinding caller, FilesBackend backend);
    ~FilesAdapter();
    FilesAdapter(const FilesAdapter &) = delete;
    FilesAdapter &operator=(const FilesAdapter &) = delete;
    PulseFilesV1 Table() const noexcept;
    PulseStatus Query(uint32_t version, void *out_table, uint32_t out_size) const noexcept;
    // Suppresses callbacks and cancels pending requests. STATE means a callback
    // is still on the stack: loader must defer FreeLibrary until it returns.
    PulseStatus Close() noexcept;
    bool CallbackActive() const noexcept;
    struct Impl;

  private:
    std::shared_ptr<Impl> impl_;
};
} // namespace pulse::extensions::native
