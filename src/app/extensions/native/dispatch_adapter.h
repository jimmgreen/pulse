#pragma once
#include "../../../../sdk/native/include/pulse/dispatch.h"
#include <functional>
#include <memory>
namespace pulse::extensions::native {
class DispatchAdapter {
  public:
    explicit DispatchAdapter(std::function<bool()> alive);
    ~DispatchAdapter();
    DispatchAdapter(const DispatchAdapter &) = delete;
    DispatchAdapter &operator=(const DispatchAdapter &) = delete;
    PulseStatus Query(uint32_t version, void *output, uint32_t size);
    void Close();
    bool CallbackActive() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace pulse::extensions::native
