#pragma once
#include "../common/picker_save_request.h"
#include "../common/runtime_log.h"
#include <atomic>
#include <functional>
#include <memory>

namespace pulse::ui {
namespace picker_save_delivery {
inline std::atomic<unsigned> pending = 0;
struct Work { picker_places::Snapshot endpoint; picker_save::Request request; HMODULE module = nullptr; };
inline void CALLBACK Deliver(PTP_CALLBACK_INSTANCE instance, void* context) {
    std::unique_ptr<Work> work(static_cast<Work*>(context));
    FreeLibraryWhenCallbackReturns(instance, work->module);
    bool accepted = false;
    try { accepted = picker_save::Send(work->endpoint, work->request); } catch (...) {}
    --pending;
    diagnostics::runtime::Event("picker_save_watch_requested", {{"accepted", accepted}, {"request", work->request.request_id}});
}
}
inline std::function<void()> PickerSaveDelivery(picker_places::Snapshot endpoint, picker_save::Request request) {
    auto sent = std::make_shared<std::atomic<bool>>(false);
    return [endpoint = std::move(endpoint), request = std::move(request), sent] {
        if (sent->exchange(true)) return;
        if (picker_save_delivery::pending.fetch_add(1) >= 32) { --picker_save_delivery::pending; return; }
        try {
            auto work = std::make_unique<picker_save_delivery::Work>();
            work->endpoint = endpoint; work->request = request;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(&picker_save_delivery::Deliver), &work->module)) {
                --picker_save_delivery::pending; return;
            }
            if (!TrySubmitThreadpoolCallback(picker_save_delivery::Deliver, work.get(), nullptr)) {
                FreeLibrary(work->module); --picker_save_delivery::pending; return;
            }
            work.release();
        } catch (...) { --picker_save_delivery::pending; }
    };
}
}
