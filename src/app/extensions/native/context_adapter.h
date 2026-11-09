#pragma once
#include "../../../../sdk/native/include/pulse/context.h"
#include "../runtime.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
struct ContextObservation {
    std::string directory, selection_name, view_mode;
    uint64_t selection_count = 0, selection_epoch = 0;
    uintptr_t tab_key = 0, snapshot_key = 0;
    bool loading = false;
};
struct ContextSelectionEntry {
    std::string path, name;
    bool directory = false;
};
struct ContextSelection {
    std::string directory;
    std::vector<ContextSelectionEntry> entries;
};
struct ContextBackend {
    std::function<bool(const CallerBinding &)> live;
    std::function<ContextObservation()> capture;
    std::function<PulseStatus(ContextSelection &)> capture_selection;
};
class ContextAdapter {
  public:
    ContextAdapter(AppState &owner, CallerBinding caller);
    ContextAdapter(CallerBinding caller, ContextBackend backend);
    ~ContextAdapter();
    ContextAdapter(const ContextAdapter &) = delete;
    ContextAdapter &operator=(const ContextAdapter &) = delete;
    PulseContextV1 Table() const noexcept;
    PulseStatus Query(uint32_t version, void *table, uint32_t size) const noexcept;
    void Poll() noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;
  private:
    struct State;
    std::shared_ptr<State> state_;
};
}
