#pragma once
#include "explorer_open_context.h"
#include <windows.h>
#include <mutex>
namespace pulse::app {
class SingleInstanceCoordinator;
class ExplorerSubmissionPort {
public:
    // Success transfers a new submission to the queue (or identifies its retry).
    // False never mutates the context: the producer must cancel its NEW source.
    // Retrying an already dispatched source must not revoke its UI-owned lease.
    bool Submit(std::shared_ptr<const ExplorerOpenContext> context) noexcept;
    void Revoke() noexcept;
private:
    friend class SingleInstanceCoordinator;
    ExplorerSubmissionPort(SingleInstanceCoordinator* owner, HWND host, UINT wake) : owner_(owner), host_(host), wake_(wake) {}
    // Lock order: port -> coordinator queue. Revoke never holds queue mutex.
    std::mutex mutex_;
    SingleInstanceCoordinator* owner_ = nullptr;
    HWND host_ = nullptr;
    UINT wake_ = 0;
};
}
