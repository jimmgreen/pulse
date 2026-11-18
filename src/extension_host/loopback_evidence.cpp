#include "loopback_evidence.h"
// clang-format off
// Define WFP GUIDs locally; all WFP functions are dynamically resolved.
#include <initguid.h>
#include <fwpmu.h>
// clang-format on
#include "loopback_evidence_match.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace pulse::extension_host {
struct LoopbackEvidence::Impl {
    HMODULE module = nullptr;
    HANDLE engine = nullptr, subscription = nullptr, signal = nullptr;
    decltype(&FwpmEngineOpen0) open = nullptr;
    decltype(&FwpmEngineClose0) close = nullptr;
    decltype(&FwpmEngineGetOption0) option = nullptr;
    decltype(&FwpmGetAppIdFromFileName0) app_id = nullptr;
    decltype(&FwpmFreeMemory0) free_memory = nullptr;
    decltype(&FwpmNetEventSubscribe1) subscribe = nullptr;
    decltype(&FwpmNetEventUnsubscribe0) unsubscribe = nullptr;
    FWP_BYTE_BLOB *application = nullptr;
    std::vector<unsigned char> sid;
    uint16_t port = 0;
    FILETIME started{};
    std::mutex mutex;
    LoopbackEvidenceResult result;

    template <class T> bool Load(T &target, const char *name) {
        const auto address = GetProcAddress(module, name);
        static_assert(sizeof(target) == sizeof(address));
        std::memcpy(&target, &address, sizeof(target));
        return target != nullptr;
    }
    static void CALLBACK Event(void *context, const FWPM_NET_EVENT2 *event) {
        auto &self = *static_cast<Impl *>(context);
        // No event payload is retained until all endpoint, application and SID checks pass.
        if (!MatchesLoopbackCapabilityDrop(event, self.sid.data(), *self.application, self.port, self.started))
            return;
        std::lock_guard lock(self.mutex);
        self.result.capability_drop = true;
        self.result.filter_id = event->capabilityDrop->filterId;
        self.result.capability = static_cast<uint32_t>(event->capabilityDrop->networkCapabilityId);
        SetEvent(self.signal);
    }
    ~Impl() {
        // Unsubscribe drains callbacks; never hold their mutex here.
        if (subscription)
            unsubscribe(engine, subscription);
        if (engine)
            close(engine);
        if (application)
            free_memory(reinterpret_cast<void **>(&application));
        if (signal)
            CloseHandle(signal);
        if (module)
            FreeLibrary(module);
    }
};
LoopbackEvidence::LoopbackEvidence() : impl_(std::make_unique<Impl>()) {}
LoopbackEvidence::~LoopbackEvidence() {
    Finish(0);
    // If both RPC cleanup operations fail, keep callback-owned state alive until process exit.
    // This probe creates at most one subscription; never unload code under a possible callback.
    if (impl_->subscription)
        static_cast<void>(impl_.release());
}
bool LoopbackEvidence::Start(const std::wstring &executable, PSID container, uint16_t port) {
    auto &self = *impl_;
    if (self.module || !container || !IsValidSid(container) || !port) {
        self.result.error = ERROR_INVALID_PARAMETER;
        return false;
    }
    self.module = LoadLibraryExW(L"fwpuclnt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!self.module) {
        self.result.error = GetLastError();
        return false;
    }
    if (!self.Load(self.open, "FwpmEngineOpen0") || !self.Load(self.close, "FwpmEngineClose0") ||
        !self.Load(self.option, "FwpmEngineGetOption0") || !self.Load(self.app_id, "FwpmGetAppIdFromFileName0") ||
        !self.Load(self.free_memory, "FwpmFreeMemory0") || !self.Load(self.subscribe, "FwpmNetEventSubscribe1") ||
        !self.Load(self.unsubscribe, "FwpmNetEventUnsubscribe0")) {
        self.result.error = ERROR_PROC_NOT_FOUND;
        return false;
    }
    self.result.error = self.open(nullptr, RPC_C_AUTHN_WINNT, nullptr, nullptr, &self.engine);
    if (self.result.error)
        return false;
    FWP_VALUE0 *collecting = nullptr;
    self.result.error = self.option(self.engine, FWPM_ENGINE_COLLECT_NET_EVENTS, &collecting);
    if (self.result.error)
        return false;
    const bool enabled = collecting && collecting->type == FWP_UINT32 && collecting->uint32 != 0;
    if (collecting)
        self.free_memory(reinterpret_cast<void **>(&collecting));
    if (!enabled) {
        self.result.error = ERROR_NOT_SUPPORTED;
        return false;
    }
    self.result.error = self.app_id(executable.c_str(), &self.application);
    if (self.result.error)
        return false;
    self.sid.resize(GetLengthSid(container));
    if (!CopySid(static_cast<DWORD>(self.sid.size()), self.sid.data(), container)) {
        self.result.error = GetLastError();
        return false;
    }
    self.port = port;
    self.signal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!self.signal) {
        self.result.error = GetLastError();
        return false;
    }
    std::array<FWPM_FILTER_CONDITION0, 5> conditions{};
    for (auto &condition : conditions)
        condition.matchType = FWP_MATCH_EQUAL;
    conditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
    conditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
    conditions[0].conditionValue.byteBlob = self.application;
    conditions[1].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
    conditions[1].conditionValue.type = FWP_UINT8;
    conditions[1].conditionValue.uint8 = 6;
    conditions[2].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
    conditions[2].conditionValue.type = FWP_UINT32;
    conditions[2].conditionValue.uint32 = 0x7f000001;
    conditions[3].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
    conditions[3].conditionValue.type = FWP_UINT16;
    conditions[3].conditionValue.uint16 = port;
    conditions[4].fieldKey = FWPM_CONDITION_NET_EVENT_TYPE;
    conditions[4].conditionValue.type = FWP_UINT32;
    conditions[4].conditionValue.uint32 = FWPM_NET_EVENT_TYPE_CAPABILITY_DROP;
    FWPM_NET_EVENT_ENUM_TEMPLATE0 filter{};
    GetSystemTimeAsFileTime(&self.started);
    filter.startTime = self.started;
    // Subscription lifetime is bounded by the probe; use a finite upper time bound as well.
    ULARGE_INTEGER end{};
    end.LowPart = self.started.dwLowDateTime;
    end.HighPart = self.started.dwHighDateTime;
    end.QuadPart += 30ull * 10000000;
    filter.endTime = {end.LowPart, end.HighPart};
    filter.numFilterConditions = static_cast<UINT32>(conditions.size());
    filter.filterCondition = conditions.data();
    FWPM_NET_EVENT_SUBSCRIPTION0 subscription{};
    subscription.enumTemplate = &filter;
    self.result.error = self.subscribe(self.engine, &subscription, Impl::Event, &self, &self.subscription);
    self.result.subscribed = self.result.error == ERROR_SUCCESS;
    return self.result.subscribed;
}
LoopbackEvidenceResult LoopbackEvidence::Finish(DWORD grace_ms) {
    auto &self = *impl_;
    if (self.subscription) {
        WaitForSingleObject(self.signal, (std::min)(grace_ms, DWORD{1000}));
        const auto error = self.unsubscribe(self.engine, self.subscription);
        if (error == ERROR_SUCCESS)
            self.subscription = nullptr;
        else {
            self.result.error = error;
            if (self.close(self.engine) == ERROR_SUCCESS) {
                self.engine = nullptr;
                self.subscription = nullptr;
            }
        }
    }
    std::lock_guard lock(self.mutex);
    return self.result;
}
} // namespace pulse::extension_host
