#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../app/extensions/file_event_hub.h"
#include "../../app/extensions/native/fileevents_adapter.h"
#include "../../app/extensions/native/diagnostics_adapter.h"

#include <pulse/client.hpp>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace pulse::extensions::native;
using pulse::extensions::fileevents::Hub;
using pulse::sdk::FileEvent;
using pulse::sdk::FileEventsClient;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (ok) ++g_passed;
    else ++g_failed;
}

struct MockHost {
    FileEventsAdapter* adapter = nullptr;

    static PulseStatus PULSE_CALL QueryInterface(void* context, PulseStringView name, uint32_t version,
                                                 void* out_table, uint32_t out_size) noexcept {
        auto* self = static_cast<MockHost*>(context);
        if (!self || !out_table) return PULSE_ERROR_ARGUMENT;
        if (std::string_view(name.data ? name.data : "", name.length) == PULSE_FILEEVENTS_INTERFACE_NAME)
            return self->adapter->Query(version, out_table, out_size);
        return PULSE_ERROR_UNAVAILABLE;
    }
    PulseHostV1 Host() noexcept { return PulseHostV1{sizeof(PulseHostV1), PULSE_ABI_VERSION_1, this, &QueryInterface}; }
};

std::vector<std::wstring> W(std::initializer_list<const wchar_t*> list) {
    std::vector<std::wstring> out;
    for (const auto* item : list) out.emplace_back(item);
    return out;
}

} // namespace

int main() {
    // 1. Query, validation, limits.
    {
        Hub hub;
        auto sink = std::make_shared<DiagnosticsSink>();
        FileEventsAdapter adapter("pulse.demo", [] { return true; }, sink, &hub);
        PulseFileEventsV1 table{};
        Check(adapter.Query(2, &table, sizeof(table)) == PULSE_ERROR_ABI &&
                  adapter.Query(PULSE_FILEEVENTS_VERSION_1, &table, sizeof(table) - 1) == PULSE_ERROR_ARGUMENT,
              "Query rejects unknown versions and short tables");
        Check(adapter.Query(PULSE_FILEEVENTS_VERSION_1, &table, sizeof(table)) == PULSE_OK && table.subscribe &&
                  table.unsubscribe,
              "Query fills every function");
        const auto noop = [](void*, const PulseFileEvent*) noexcept {};
        PulseFileEventSubscription sub = 0;
        Check(table.subscribe(table.context, 0, nullptr, nullptr, &sub) == PULSE_ERROR_ARGUMENT && sub == 0 &&
                  sink->Read().field == "callback",
              "subscribe needs a callback");
        Check(table.subscribe(table.context, 1u, noop, nullptr, &sub) == PULSE_ERROR_ARGUMENT &&
                  sink->Read().field == "kinds",
              "subscribe rejects bits outside PULSE_FILE_EVENT_ALL_KINDS");
        std::vector<PulseFileEventSubscription> subs;
        for (int i = 0; i < 8; ++i) {
            PulseFileEventSubscription s = 0;
            if (table.subscribe(table.context, 0, noop, nullptr, &s) == PULSE_OK) subs.push_back(s);
        }
        PulseFileEventSubscription ninth = 0;
        Check(subs.size() == 8 && table.subscribe(table.context, 0, noop, nullptr, &ninth) == PULSE_ERROR_STATE && ninth == 0,
              "At most 8 subscriptions per session");
        Check(table.unsubscribe(table.context, subs[0]) == PULSE_OK &&
                  table.unsubscribe(table.context, subs[0]) == PULSE_ERROR_ARGUMENT,
              "unsubscribe works once, then reports an unknown subscription");
        PulseStatus from_thread = PULSE_OK;
        std::thread([&] {
            PulseFileEventSubscription t = 0;
            from_thread = table.subscribe(table.context, 0, noop, nullptr, &t);
        }).join();
        Check(from_thread == PULSE_ERROR_STATE && sink->Read().field == "thread", "Calls from another thread fail");
        adapter.Close();
        Check(!hub.Wants(PULSE_FILE_EVENT_COPY), "Close removes every subscription");
        Check(table.subscribe(table.context, 0, noop, nullptr, &sub) == PULSE_ERROR_STATE, "Calls after Close fail");
    }

    // 2. Client round trip, kinds filter, item shapes, truncation, self-unsubscribe.
    {
        Hub hub;
        auto sink = std::make_shared<DiagnosticsSink>();
        FileEventsAdapter adapter("pulse.demo", [] { return true; }, sink, &hub);
        MockHost mock{&adapter};
        const PulseHostV1 raw_host = mock.Host();
        const pulse::sdk::Host host(raw_host);
        auto client = FileEventsClient::TryQuery(host);
        Check(client.has_value() && client->Available(), "FileEventsClient attaches through the adapter");

        std::vector<FileEvent> all, deletes;
        auto sub_all = client->Subscribe(0, [&](const FileEvent& e) { all.push_back(e); });
        auto sub_del = client->Subscribe(PULSE_FILE_EVENT_MASK(PULSE_FILE_EVENT_RECYCLE) | PULSE_FILE_EVENT_MASK(PULSE_FILE_EVENT_DELETE),
                                         [&](const FileEvent& e) { deletes.push_back(e); });
        Check(sub_all.has_value() && sub_del.has_value(), "Subscribe returns active subscriptions");
        Check(!hub.Wants(0) && !hub.Wants(10) && hub.Wants(PULSE_FILE_EVENT_COPY), "Wants checks the kind range and subscribers");

        hub.Publish(PULSE_FILE_EVENT_COPY, W({L"C:\\a\\x.txt", L"C:\\a\\照片.jpg"}), W({L"D:\\b\\x.txt", L"D:\\b\\照片.jpg"}));
        Check(all.size() == 1 && deletes.empty() && all[0].kind == PULSE_FILE_EVENT_COPY && all[0].items.size() == 2 &&
                  all[0].items[1].source == "C:\\a\\照片.jpg" && all[0].items[1].destination == "D:\\b\\照片.jpg" &&
                  !all[0].Truncated(),
              "COPY carries both UTF-8 paths; the kinds filter skips other subscribers");
        hub.Publish(PULSE_FILE_EVENT_RECYCLE, W({L"C:\\a\\old.txt"}), W({L"C:\\$Recycle.Bin\\x"}));
        Check(all.size() == 2 && deletes.size() == 1 && deletes[0].items[0].source == "C:\\a\\old.txt" &&
                  deletes[0].items[0].destination.empty(),
              "RECYCLE reports the source only, to matching subscribers");
        hub.Publish(PULSE_FILE_EVENT_CREATE_FOLDER, W({L"C:\\a\\New folder"}), {});
        Check(all.size() == 3 && all[2].items[0].source.empty() && all[2].items[0].destination == "C:\\a\\New folder",
              "CREATE_* reports the new path as the destination");
        Check(all[1].sequence == all[0].sequence + 1 && all[2].sequence == all[1].sequence + 1, "Sequence increases by one per event");
        hub.Publish(PULSE_FILE_EVENT_MOVE, {}, {});
        Check(all.size() == 3, "Events without items are not delivered");
        hub.Publish(PULSE_FILE_EVENT_COPY, W({LR"(\\?\C:\a\x.txt)"}), W({LR"(\\?\UNC\srv\share\x.txt)"}));
        Check(all.size() == 4 && all[3].items[0].source == R"(C:\a\x.txt)" && all[3].items[0].destination == R"(\\srv\share\x.txt)",
              "Long-path prefixes are removed from short paths");
        const std::wstring deep = LR"(\\?\C:\)" + std::wstring(300, L'd');
        hub.Publish(PULSE_FILE_EVENT_MOVE, std::vector<std::wstring>{deep}, std::vector<std::wstring>{deep});
        Check(all.size() == 5 && all[4].items[0].source.size() == deep.size() && all[4].items[0].source.rfind(R"(\\?\)", 0) == 0,
              "Paths of MAX_PATH or more keep the long-path prefix");

        std::vector<std::wstring> many(Hub::kMaxItems + 5, L"C:\\many\\f");
        hub.Publish(PULSE_FILE_EVENT_DELETE, many, {});
        Check(deletes.size() == 2 && deletes[1].items.size() == Hub::kMaxItems &&
                  deletes[1].total_count == Hub::kMaxItems + 5 && deletes[1].Truncated(),
              "Large operations are truncated to 1024 items with the total kept");

        // A callback that drops its own subscription, and one that subscribes more.
        std::optional<FileEventsClient::Subscription> self_drop;
        int self_calls = 0;
        self_drop = client->Subscribe(0, [&](const FileEvent&) { ++self_calls; self_drop.reset(); });
        hub.Publish(PULSE_FILE_EVENT_RENAME, W({L"C:\\a"}), W({L"C:\\b"}));
        hub.Publish(PULSE_FILE_EVENT_RENAME, W({L"C:\\b"}), W({L"C:\\c"}));
        Check(self_calls == 1 && !self_drop.has_value(), "A callback may unsubscribe itself; it is not called again");

        sub_all.reset();
        const size_t before = all.size();
        hub.Publish(PULSE_FILE_EVENT_COPY, W({L"C:\\x"}), W({L"C:\\y"}));
        Check(all.size() == before, "A reset subscription receives nothing");

        // Another session's events are independent; closing one keeps the other.
        FileEventsAdapter other("pulse.other", [] { return true; }, sink, &hub);
        PulseFileEventsV1 ot{};
        (void)other.Query(PULSE_FILEEVENTS_VERSION_1, &ot, sizeof(ot));
        int other_calls = 0;
        PulseFileEventSubscription os = 0;
        (void)ot.subscribe(ot.context, 0, [](void* user, const PulseFileEvent*) noexcept { ++*static_cast<int*>(user); }, &other_calls, &os);
        adapter.Close();
        hub.Publish(PULSE_FILE_EVENT_DELETE, W({L"C:\\z"}), {});
        Check(other_calls == 1 && deletes.size() == 2, "Closing one session keeps another session's subscriptions");
        sub_del.reset();   // unsubscribe on a closed session must be harmless
    }

    std::printf("pulse_native_fileevents_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}