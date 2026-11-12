#include "../app/extensions/store/catalog_service.h"
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {
using namespace pulse::extensions::store;
using namespace std::chrono_literals;
int passed = 0, failed = 0;
void Check(const char* label, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    ok ? ++passed : ++failed;
}
struct Step {
    CatalogFetchStatus status = CatalogFetchStatus::Online;
    std::string marker = "online";
    bool empty = false, late_success = false, throws = false, unknown_exception = false;
};
class Gate {
public:
    explicit Gate(std::vector<Step> steps) : steps_(std::move(steps)) {}
    CatalogService::FetchOperation Operation() {
        return [this](const std::filesystem::path& path, const std::atomic<bool>& cancelled,
            CatalogSnapshot& out, std::string& error) {
            std::unique_lock lock(mutex_);
            const auto index = records_.size();
            records_.push_back({path, &cancelled});
            ++active_;
            if (active_ > maximum_active_) maximum_active_ = active_;
            changed_.notify_all();
            const bool released = changed_.wait_for(lock, 5s, [&] { return all_released_ || released_ > index; });
            const bool was_cancelled = cancelled.load();
            records_[index].observed_cancelled = was_cancelled;
            records_[index].cancelled = nullptr;
            --active_;
            changed_.notify_all();
            if (!released || index >= steps_.size()) { error = "unexpected_fetch_or_gate_timeout"; return CatalogFetchStatus::Failed; }
            const auto step = steps_[index];
            lock.unlock();
            if (was_cancelled && !step.late_success) return CatalogFetchStatus::Cancelled;
            if (step.throws) throw std::runtime_error("synthetic fetch exception");
            if (step.unknown_exception) throw 7;
            out.catalog.generated = step.marker;
            out.offline = step.status == CatalogFetchStatus::Offline;
            if (!step.empty) { Extension extension; extension.id = step.marker; out.catalog.extensions.push_back(std::move(extension)); }
            if (step.status == CatalogFetchStatus::Failed) error = step.marker;
            return step.status;
        };
    }
    bool WaitCalls(size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] { return records_.size() >= count; });
    }
    void Release(size_t count) {
        { std::lock_guard lock(mutex_); released_ = count; }
        changed_.notify_all();
    }
    void ReleaseAll() {
        { std::lock_guard lock(mutex_); all_released_ = true; }
        changed_.notify_all();
    }
    bool Cancelled(size_t index) {
        std::lock_guard lock(mutex_);
        return index < records_.size() && (records_[index].cancelled ? records_[index].cancelled->load() : records_[index].observed_cancelled);
    }
    size_t Calls() { std::lock_guard lock(mutex_); return records_.size(); }
    size_t MaximumActive() { std::lock_guard lock(mutex_); return maximum_active_; }
    bool Path(size_t index, const std::filesystem::path& path) {
        std::lock_guard lock(mutex_); return index < records_.size() && records_[index].path == path;
    }
private:
    struct Record {
        std::filesystem::path path;
        const std::atomic<bool>* cancelled;
        bool observed_cancelled = false;
    };
    std::vector<Step> steps_;
    std::vector<Record> records_;
    size_t released_ = 0, active_ = 0, maximum_active_ = 0;
    bool all_released_ = false;
    std::mutex mutex_;
    std::condition_variable changed_;
};
bool Finish(CatalogService& service, uint64_t generation, CatalogServiceState state) {
    if (!service.WaitForGeneration(generation, 5s)) return false;
    const auto snapshot = service.GetSnapshot();
    return snapshot.generation == generation && snapshot.state == state;
}
} // namespace

int main() {
    try {
        {
            CatalogService production;
            const auto initial = production.GetSnapshot();
            Check("default production constructor stays idle without I/O", initial.state == CatalogServiceState::Idle && initial.generation == 0 && !initial.result && initial.error.empty());
            production.Stop();
            const auto stopped = production.GetSnapshot();
            Check("stop idle publishes cancelled and is idempotent", stopped.state == CatalogServiceState::Cancelled && stopped.generation == 1 && stopped.error == "catalog_service_stopped");
            production.Stop();
            Check("stopped service cannot refresh or cancel into new work", production.Refresh({}) == 1 && production.Cancel() == 1);
        }
        {
            Gate gate({{CatalogFetchStatus::Online, "2026-11-09T00:00:00Z"}, {CatalogFetchStatus::Offline, "2026-11-10T00:00:00Z"},
                {CatalogFetchStatus::Failed, "network_failure"}, {CatalogFetchStatus::Online, "empty", true},
                {CatalogFetchStatus::Cancelled, "cancelled"}, {CatalogFetchStatus::Online, "throws", false, false, true},
                {CatalogFetchStatus::Online, "recovered"}});
            CatalogService service(gate.Operation());
            const auto first = service.Refresh(L"C:\\not-created\\first.cache");
            Check("refresh immediately publishes loading generation", first == 1 && service.GetSnapshot().state == CatalogServiceState::Loading && !service.GetSnapshot().result);
            Check("worker starts first request with unchanged caller path", gate.WaitCalls(1) && gate.Path(0, L"C:\\not-created\\first.cache"));
            // Gate is held inside the operation: these state calls must complete
            // without waiting for file/network work or its mutex.
            Check("snapshot stays loading while worker is blocked", service.GetSnapshot().generation == first && service.GetSnapshot().state == CatalogServiceState::Loading);
            gate.Release(1);
            Check("online result published", Finish(service, first, CatalogServiceState::Online));
            const auto retained = service.GetSnapshot();
            Check("online result is immutable shared snapshot", retained.result && retained.result->catalog.generated == "2026-11-09T00:00:00Z" && retained.result->catalog.extensions.size() == 1);
            const auto offline = service.Refresh({});
            Check("refresh keeps the previous verified catalog while network is blocked",service.GetSnapshot().state==CatalogServiceState::Loading && service.GetSnapshot().result==retained.result);
            gate.WaitCalls(2); gate.Release(2);
            Check("offline status and cache marker published", Finish(service, offline, CatalogServiceState::Offline) && service.GetSnapshot().result && service.GetSnapshot().result->offline);
            const auto failed_generation = service.Refresh({}); gate.WaitCalls(3); gate.Release(3);
            Check("failed refresh retains verified old data and diagnostic", Finish(service, failed_generation, CatalogServiceState::Failed) && service.GetSnapshot().result && service.GetSnapshot().result->catalog.generated=="2026-11-10T00:00:00Z" && service.GetSnapshot().error == "network_failure");
            const auto empty = service.Refresh({}); gate.WaitCalls(4); gate.Release(4);
            Check("empty verified catalog is successful online data", Finish(service, empty, CatalogServiceState::Online) && service.GetSnapshot().result && service.GetSnapshot().result->catalog.extensions.empty());
            const auto cancelled = service.Refresh({}); gate.WaitCalls(5); gate.Release(5);
            Check("operation cancellation preserves last verified catalog", Finish(service, cancelled, CatalogServiceState::Cancelled) && service.GetSnapshot().result && service.GetSnapshot().result->catalog.extensions.empty() && service.GetSnapshot().error == "cancelled");
            const auto exception = service.Refresh({}); gate.WaitCalls(6); gate.Release(6);
            Check("fetch exception contained as failure", Finish(service, exception, CatalogServiceState::Failed) && service.GetSnapshot().error.find("synthetic fetch exception") != std::string::npos);
            const auto recovered = service.Refresh({}); gate.WaitCalls(7); gate.Release(7);
            Check("worker survives exception and later refresh succeeds", Finish(service, recovered, CatalogServiceState::Online) && service.GetSnapshot().result->catalog.generated == "recovered");
            Check("previous shared snapshot survives replacement", retained.result && retained.result->catalog.generated == "2026-11-09T00:00:00Z" && retained.generation == first);
            Check("all sequential fetches run on one serial worker", gate.Calls() == 7 && gate.MaximumActive() == 1);
        }
        {
            Gate gate({{CatalogFetchStatus::Online, "stale", false, true}, {CatalogFetchStatus::Online, "latest"}});
            CatalogService service(gate.Operation());
            const auto old = service.Refresh(L"old"); Check("old generation starts before rapid refresh", gate.WaitCalls(1));
            const auto middle = service.Refresh(L"middle");
            const auto latest = service.Refresh(L"latest");
            Check("rapid refresh advances generations without waiting", old + 1 == middle && middle + 1 == latest && service.GetSnapshot().generation == latest && service.GetSnapshot().state == CatalogServiceState::Loading);
            Check("refresh atomically cancels active request before returning", gate.Cancelled(0));
            Check("pending requests do not run concurrently with old fetch", gate.Calls() == 1);
            Check("test wait recognizes superseded generations", service.WaitForGeneration(old, 0ms));
            gate.Release(1);
            Check("only latest pending path runs after stale late success", gate.WaitCalls(2) && gate.Path(1, L"latest"));
            const auto pending_snapshot = service.GetSnapshot();
            Check("late success cannot overwrite latest loading generation", pending_snapshot.generation == latest && pending_snapshot.state == CatalogServiceState::Loading && !pending_snapshot.result);
            gate.Release(2);
            Check("latest pending result wins", Finish(service, latest, CatalogServiceState::Online) && service.GetSnapshot().result->catalog.generated == "latest");
            Check("pending coalescing executes exactly old and latest", gate.Calls() == 2 && gate.MaximumActive() == 1);
        }
        {
            Gate gate({{CatalogFetchStatus::Online, "old_exception", false, true, true},
                {CatalogFetchStatus::Online, "unknown_exception", false, false, false, true},
                {CatalogFetchStatus::Online, "after_exception"}});
            CatalogService service(gate.Operation());
            service.Refresh(L"old_exception"); gate.WaitCalls(1);
            const auto latest = service.Refresh(L"latest_exception");
            gate.Release(1);
            Check("old-generation std exception cannot replace newest loading", gate.WaitCalls(2) && service.GetSnapshot().generation == latest && service.GetSnapshot().state == CatalogServiceState::Loading && service.GetSnapshot().error.empty());
            gate.Release(2);
            Check("unknown exception contained as newest failed state", Finish(service, latest, CatalogServiceState::Failed) && service.GetSnapshot().error == "catalog_fetch_exception" && !service.GetSnapshot().result);
            const auto recovered = service.Refresh(L"after_exception"); gate.WaitCalls(3); gate.Release(3);
            Check("worker remains usable after old and unknown exceptions", Finish(service, recovered, CatalogServiceState::Online) && service.GetSnapshot().result->catalog.generated == "after_exception");
        }
        {
            Gate gate({{CatalogFetchStatus::Online, "old", false, true}, {CatalogFetchStatus::Online, "after_cancel"}});
            CatalogService service(gate.Operation());
            const auto old = service.Refresh(L"old"); gate.WaitCalls(1);
            const auto pending = service.Refresh(L"never_run");
            const auto cancelled = service.Cancel();
            Check("cancel advances generation and immediately publishes cancelled", old < pending && cancelled == pending + 1 && Finish(service, cancelled, CatalogServiceState::Cancelled) && service.GetSnapshot().error == "cancelled");
            Check("cancel active fetch without waiting for gate", gate.Cancelled(0) && gate.Calls() == 1);
            gate.Release(1);
            const auto resumed = service.Refresh(L"after_cancel");
            Check("refresh after cancel starts only newest request", gate.WaitCalls(2) && gate.Path(1, L"after_cancel"));
            Check("old success cannot replace post-cancel loading", service.GetSnapshot().generation == resumed && service.GetSnapshot().state == CatalogServiceState::Loading);
            gate.Release(2);
            Check("refresh after cancel recovers", Finish(service, resumed, CatalogServiceState::Online) && service.GetSnapshot().result->catalog.generated == "after_cancel" && gate.Calls() == 2);
        }
        {
            Gate gate({{CatalogFetchStatus::Online, "late", false, true}});
            auto service = std::make_unique<CatalogService>(gate.Operation());
            const auto old = service->Refresh(L"active"); gate.WaitCalls(1);
            service->Refresh(L"pending");
            service->Stop();
            const auto stopped = service->GetSnapshot();
            Check("stop is non-joining while network gate is held", stopped.generation == old + 2 && stopped.state == CatalogServiceState::Cancelled && stopped.error == "catalog_service_stopped" && gate.Cancelled(0));
            Check("stop rejects future refresh without starting pending", service->Refresh(L"forbidden") == stopped.generation && service->Cancel() == stopped.generation && gate.Calls() == 1);
            std::mutex lifetime_mutex;
            std::condition_variable lifetime_changed;
            bool destroying = false, destroyed = false;
            std::thread destroyer([owned = std::move(service), &lifetime_mutex, &lifetime_changed, &destroying, &destroyed]() mutable {
                { std::lock_guard lock(lifetime_mutex); destroying = true; }
                lifetime_changed.notify_all();
                owned.reset(); // Deliberately off the UI/calling thread; joins worker.
                { std::lock_guard lock(lifetime_mutex); destroyed = true; }
                lifetime_changed.notify_all();
            });
            {
                std::unique_lock lock(lifetime_mutex);
                Check("off-thread destruction begins with blocked worker", lifetime_changed.wait_for(lock, 5s, [&] { return destroying; }));
            }
            gate.ReleaseAll();
            bool finished = false;
            {
                std::unique_lock lock(lifetime_mutex);
                finished = lifetime_changed.wait_for(lock, 5s, [&] { return destroyed; });
            }
            destroyer.join();
            Check("destructor safely joins cancelled late-returning worker", finished && destroyed && gate.Calls() == 1 && gate.Cancelled(0));
            Check("retained stop snapshot remains valid after destruction", stopped.state == CatalogServiceState::Cancelled && !stopped.result && stopped.error == "catalog_service_stopped");
        }
        {
            Gate gate({{CatalogFetchStatus::Online,"2026-11-11T00:00:00Z"},{CatalogFetchStatus::Offline,"2026-11-10T00:00:00Z"}});
            CatalogService service(gate.Operation());
            const auto first=service.Refresh({});gate.WaitCalls(1);gate.Release(1);
            Check("retained ordering fixture starts online",Finish(service,first,CatalogServiceState::Online));
            const auto retained=service.GetSnapshot().result;
            const auto next=service.Refresh({});gate.WaitCalls(2);gate.Release(2);
            Check("older final offline result cannot replace newer verified retained snapshot",Finish(service,next,CatalogServiceState::Offline) && service.GetSnapshot().result==retained && service.GetSnapshot().error=="catalog_rollback_rejected_against_retained");
        }
        {
            Gate gate({{CatalogFetchStatus::Failed,"slow_network_failure"}});
            auto network=gate.Operation();
            CatalogService service(CatalogService::ProgressiveFetchOperation([&](const std::filesystem::path& path,
                const std::atomic<bool>& cancelled,CatalogSnapshot& out,std::string& error,const CatalogCacheReady& ready) {
                CatalogSnapshot cached;cached.catalog.generated="2026-11-11T00:00:00Z";cached.offline=true;
                ready(cached);
                return network(path,cancelled,out,error);
            }));
            const auto generation=service.Refresh(L"verified-cache");
            Check("cache-first publication precedes blocked network completion",gate.WaitCalls(1));
            const auto cached=service.GetSnapshot();
            Check("loading publishes verified cache while network remains blocked",cached.generation==generation && cached.state==CatalogServiceState::Loading && cached.result && cached.result->catalog.generated=="2026-11-11T00:00:00Z");
            Check("intermediate cache publication advances displayed-index revision",cached.revision>=2);
            gate.Release(1);
            Check("slow failed refresh keeps the early verified snapshot",Finish(service,generation,CatalogServiceState::Failed) && service.GetSnapshot().result==cached.result && service.GetSnapshot().revision>cached.revision);
        }
        {
            std::mutex mutex;std::condition_variable changed;
            bool entered=false,released=false,late_finished=false;
            CatalogService service(CatalogService::ProgressiveFetchOperation([&](const std::filesystem::path&,
                const std::atomic<bool>&,CatalogSnapshot&,std::string&,const CatalogCacheReady& ready) {
                CatalogSnapshot cached;cached.catalog.generated="2026-11-11T00:00:00Z";ready(cached);
                {
                    std::unique_lock lock(mutex);entered=true;changed.notify_all();
                    changed.wait_for(lock,5s,[&]{return released;});
                }
                ready(cached); // deliberately non-cooperative, late intermediate publication
                {std::lock_guard lock(mutex);late_finished=true;} changed.notify_all();
                return CatalogFetchStatus::Cancelled;
            }));
            service.Refresh(L"cancel-cache");
            {std::unique_lock lock(mutex);Check("cache cancellation fixture enters worker",changed.wait_for(lock,5s,[&]{return entered;}));}
            const auto retained=service.GetSnapshot().result;
            const auto cancelled=service.Cancel();
            {std::lock_guard lock(mutex);released=true;} changed.notify_all();
            {std::unique_lock lock(mutex);Check("late cache callback completes after cancellation",changed.wait_for(lock,5s,[&]{return late_finished;}));}
            const auto snapshot=service.GetSnapshot();
            Check("cancelled generation rejects late cache publication",snapshot.generation==cancelled && snapshot.state==CatalogServiceState::Cancelled && snapshot.result==retained);
        }
        {
            bool rejected = false;
            try { CatalogService service(CatalogService::FetchOperation{}); }
            catch (const std::invalid_argument&) { rejected = true; }
            Check("test injection rejects empty operation before starting thread", rejected);
        }
    } catch (const std::exception& ex) {
        std::printf("[FAIL] unexpected exception: %s\n", ex.what()); ++failed;
    }
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
