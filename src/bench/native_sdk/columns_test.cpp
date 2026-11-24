#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../app/extensions/column_registry.h"
#include "../../app/extensions/native/columns_adapter.h"
#include "../../app/extensions/native/diagnostics_adapter.h"

#include <pulse/client.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace pulse::extensions::native;
using namespace pulse::extensions::columns;
using pulse::sdk::BadgeIcon;
using pulse::sdk::ColumnItem;
using pulse::sdk::ColumnsClient;
using pulse::sdk::ColumnSpec;
using pulse::sdk::ColumnText;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (ok) ++g_passed;
    else ++g_failed;
}

struct MockHost {
    ColumnsAdapter* adapter = nullptr;

    static PulseStatus PULSE_CALL QueryInterface(void* context, PulseStringView name, uint32_t version,
                                                 void* out_table, uint32_t out_size) noexcept {
        auto* self = static_cast<MockHost*>(context);
        if (!self || !out_table) return PULSE_ERROR_ARGUMENT;
        if (std::string_view(name.data ? name.data : "", name.length) == PULSE_COLUMNS_INTERFACE_NAME)
            return self->adapter->Query(version, out_table, out_size);
        return PULSE_ERROR_UNAVAILABLE;
    }
    PulseHostV1 Host() noexcept { return PulseHostV1{sizeof(PulseHostV1), PULSE_ABI_VERSION_1, this, &QueryInterface}; }
};

Item File(std::wstring path, uint64_t size = 10, uint64_t mtime = 1000) {
    Item item;
    item.path = std::move(path);
    item.size = size;
    item.mtime = mtime;
    return item;
}

PulseStringView V(std::string_view text) { return PulseStringView{text.data(), static_cast<uint32_t>(text.size())}; }

} // namespace

int main() {
    // 1. Pure helpers.
    {
        std::vector<std::wstring> ext;
        Check(ParseExtensions(L".JPG; png ;.tif;;", ext) && ext.size() == 3 && ext[0] == L".jpg" &&
                  ext[1] == L".png" && ext[2] == L".tif",
              "ParseExtensions lowers, adds dots, trims and skips empties");
        Check(!ParseExtensions(L".a/b", ext) && !ParseExtensions(L".x.y", ext) && !ParseExtensions(L"*", ext),
              "ParseExtensions rejects separators, inner dots and wildcards");
        ProviderInfo info;
        info.extensions = {L".jpg"};
        info.applies_to = PULSE_COLUMN_APPLIES_FILES;
        Item dir = File(L"C:\\x\\photos.jpg");
        dir.is_dir = true;
        Check(Applies(info, File(L"C:\\x\\A.JPG")) && !Applies(info, File(L"C:\\x\\a.png")) &&
                  !Applies(info, File(L"C:\\x\\.jpg")) && !Applies(info, dir),
              "Applies honours extensions (case-insensitive) and the files-only flag");
        info.package_id = "pulse.demo";
        info.id = "hash";
        Check(info.Key() == L"pulse.demo/hash", "ProviderInfo::Key is <package>/<id>");
    }

    // 2. Adapter validation, client round trip, cache, invalidate, limits, drain.
    {
        Registry registry;
        auto sink = std::make_shared<DiagnosticsSink>();
        std::atomic<int> wakes{0};
        ColumnsAdapter adapter("pulse.demo", [] { return true; }, sink, [&] { wakes.fetch_add(1); }, &registry);
        MockHost mock{&adapter};
        const PulseHostV1 raw_host = mock.Host();
        const pulse::sdk::Host host(raw_host);
        auto client = ColumnsClient::TryQuery(host);
        Check(client.has_value() && client->Available(), "ColumnsClient attaches through the adapter");

        PulseColumnsV1 table{};
        Check(adapter.Query(2, &table, sizeof(table)) == PULSE_ERROR_ABI &&
                  adapter.Query(PULSE_COLUMNS_VERSION_1, &table, sizeof(table) - 1) == PULSE_ERROR_ARGUMENT,
              "Query rejects unknown versions and short tables");

        // Raw descriptor validation.
        (void)adapter.Query(PULSE_COLUMNS_VERSION_1, &table, sizeof(table));
        const auto noop = [](void*, const PulseColumnBatch*) noexcept -> PulseStatus { return PULSE_OK; };
        PulseColumnDesc desc{};
        desc.size = sizeof(desc);
        desc.id = V("bad id");
        desc.title = V("T");
        desc.query = noop;
        PulseColumnHandle h = 0;
        Check(table.register_column(table.context, &desc, &h) == PULSE_ERROR_ARGUMENT && h == 0 &&
                  sink->Read().field == "id",
              "register_column rejects an invalid id with a diagnostic");
        desc.id = V("ok");
        desc.width_dip = 20;
        Check(table.register_column(table.context, &desc, &h) == PULSE_ERROR_ARGUMENT && sink->Read().field == "width_dip",
              "register_column rejects width_dip outside 48..480");
        desc.width_dip = 0;
        desc.extensions = V(".a/b");
        Check(table.register_column(table.context, &desc, &h) == PULSE_ERROR_ARGUMENT && sink->Read().field == "extensions",
              "register_column rejects a malformed extension list");
        desc.extensions = {};
        desc.title = {};
        Check(table.register_column(table.context, &desc, &h) == PULSE_ERROR_ARGUMENT && sink->Read().field == "title",
              "register_column requires a title");
        desc.title = V("T");
        desc.applies_to = 8;
        Check(table.register_column(table.context, &desc, &h) == PULSE_ERROR_ARGUMENT && sink->Read().field == "applies_to",
              "register_column rejects undocumented applies_to bits");

        // Client column: text = name length, tone accent for .txt.
        std::atomic<int> calls{0};
        ColumnsClient::Registration col;
        ColumnSpec spec;
        spec.id = "len";
        spec.title = "Length";
        spec.align = PULSE_COLUMN_ALIGN_END;
        spec.applies_to = PULSE_COLUMN_APPLIES_FILES;
        const PulseStatus st = client->AddColumn(spec, [&](const ColumnItem& item) -> std::optional<ColumnText> {
            calls.fetch_add(1);
            if (item.path.ends_with(".skip")) return std::nullopt;
            ColumnText t;
            t.text = std::to_string(item.size_bytes) + " B";
            t.tone = item.path.ends_with(".txt") ? PULSE_COLUMN_TONE_ACCENT : PULSE_COLUMN_TONE_DEFAULT;
            return t;
        }, col);
        Check(st == PULSE_OK && col.Active(), "AddColumn registers a column");
        const auto listed = registry.List(Kind::Column);
        Check(listed.size() == 1 && listed[0].title == L"Length" && listed[0].align_end &&
                  listed[0].width_dip == 120.0f,
              "Registry lists the column with defaults (120 dip) and alignment");
        ColumnsClient::Registration dup;
        Check(client->AddColumn(spec, [](const ColumnItem&) { return std::optional<ColumnText>{}; }, dup) ==
                  PULSE_ERROR_STATE,
              "Duplicate id in one package is refused");

        const uint64_t handle = col.Raw();
        CellValue cell;
        const uint64_t rev0 = registry.Revision();
        Item a = File(L"C:\\d\\a.txt", 42);
        Item b = File(L"C:\\d\\b.bin", 7);
        Item s = File(L"C:\\d\\c.skip", 1);
        Item folder = File(L"C:\\d\\sub", 0);
        folder.is_dir = true;
        Check(registry.Cell(handle, a, cell) == Lookup::Pending && registry.Cell(handle, b, cell) == Lookup::Pending &&
                  registry.Cell(handle, s, cell) == Lookup::Pending,
              "First lookups are pending and never block");
        Check(registry.Cell(handle, folder, cell) == Lookup::NotApplicable, "Files-only column skips folders");
        Check(registry.WaitUntilIdle(handle, 3000), "Worker drains the queue");
        Check(registry.Revision() > rev0, "Revision moves when values arrive");
        Check(registry.Cell(handle, a, cell) == Lookup::Ready && cell.text == L"42 B" &&
                  cell.tone == PULSE_COLUMN_TONE_ACCENT,
              "Cell returns the plugin's text and tone");
        Check(registry.Cell(handle, s, cell) == Lookup::Ready && cell.text.empty(), "Unset items are cached blank");
        const int after_first = calls.load();
        (void)registry.Cell(handle, a, cell);
        Check(calls.load() == after_first, "Cached values are not asked again");
        Item a2 = a;
        a2.mtime = 2000;
        a2.size = 43;
        Check(registry.Cell(handle, a2, cell) == Lookup::Pending, "A changed file version is asked again");
        registry.WaitUntilIdle(handle, 3000);
        Check(registry.Cell(handle, a2, cell) == Lookup::Ready && cell.text == L"43 B", "New version gets a new value");

        Check(col.Invalidate("c:\\D\\B.BIN") == PULSE_OK && registry.Cell(handle, b, cell) == Lookup::Pending,
              "Invalidate(path) drops that item, ignoring letter case");
        registry.WaitUntilIdle(handle, 3000);
        Check(col.Invalidate() == PULSE_OK && registry.Cell(handle, a2, cell) == Lookup::Pending,
              "Invalidate() drops every value");
        registry.WaitUntilIdle(handle, 3000);
        Check(col.SetTitle("Größe") == PULSE_OK && registry.List(Kind::Column)[0].title == L"Größe",
              "SetTitle retitles the column");
        Check(wakes.load() > 0, "Finished batches wake the host");

        // Batches: 100 items arrive in at most 64 per call.
        std::atomic<uint32_t> max_batch{0};
        PulseColumnDesc big{};
        big.size = sizeof(big);
        big.id = V("batch");
        big.title = V("Batch");
        big.user = &max_batch;
        big.query = [](void* user, const PulseColumnBatch* batch) noexcept -> PulseStatus {
            auto* m = static_cast<std::atomic<uint32_t>*>(user);
            if (batch->count > m->load()) m->store(batch->count);
            for (uint32_t i = 0; i < batch->count; ++i) {
                const PulseColumnValue v{sizeof(PulseColumnValue), 9, PulseStringView{"x", 1}};
                if (batch->set_text(batch->context, i, &v) != PULSE_ERROR_ARGUMENT) return PULSE_ERROR_STATE;
                const PulseColumnValue ok{sizeof(PulseColumnValue), 0, PulseStringView{"x", 1}};
                if (batch->set_text(batch->context, i, &ok) != PULSE_OK) return PULSE_ERROR_STATE;
                if (batch->set_text(batch->context, i, &ok) != PULSE_ERROR_ARGUMENT) return PULSE_ERROR_STATE;
                const PulseBadgeValue bv{sizeof(PulseBadgeValue), 0, PulseStringView{"lock", 4}};
                if (batch->set_badge(batch->context, i, &bv) != PULSE_ERROR_STATE) return PULSE_ERROR_STATE;
            }
            return PULSE_OK;
        };
        PulseColumnHandle big_handle = 0;
        Check(table.register_column(table.context, &big, &big_handle) == PULSE_OK, "Raw register_column works");
        std::vector<Item> many;
        for (int i = 0; i < 100; ++i) many.push_back(File(L"C:\\m\\f" + std::to_wstring(i) + L".dat"));
        // Queue all before the worker wakes up as far as possible.
        for (const auto& item : many) (void)registry.Cell(big_handle, item, cell);
        registry.WaitUntilIdle(big_handle, 3000);
        bool all = true;
        for (const auto& item : many) all = all && registry.Cell(big_handle, item, cell) == Lookup::Ready && cell.text == L"x";
        Check(all && max_batch.load() >= 1 && max_batch.load() <= 64,
              "Batches hold at most 64 items; bad tone, double set and wrong-kind set are refused");

        // Limits: 4 columns per package.
        PulseColumnHandle extra = 0;
        big.id = V("c3");
        Check(table.register_column(table.context, &big, &extra) == PULSE_OK, "Third column registers");
        big.id = V("c4");
        Check(table.register_column(table.context, &big, &extra) == PULSE_OK, "Fourth column registers");
        big.id = V("c5");
        Check(table.register_column(table.context, &big, &extra) == PULSE_ERROR_STATE, "Fifth column is refused");

        // Badges.
        ColumnsClient::Registration badge;
        ColumnSpec bspec;
        bspec.id = "lock";
        bspec.title = "Locked";
        bspec.extensions = ".key";
        Check(client->AddBadge(bspec, [](const ColumnItem& item) -> std::optional<BadgeIcon> {
                  if (item.path.find("open") != std::string_view::npos) return std::nullopt;
                  return BadgeIcon{item.path.find("weird") != std::string_view::npos ? "no-such-icon" : "lock",
                                   PULSE_COLUMN_TONE_WARNING};
              }, badge) == PULSE_OK,
              "AddBadge registers a badge provider");
        BadgeValue bv;
        Item k1 = File(L"C:\\k\\a.key"), k2 = File(L"C:\\k\\open.key"), k3 = File(L"C:\\k\\weird.key"),
             k4 = File(L"C:\\k\\a.txt");
        (void)registry.Badge(badge.Raw(), k1, bv);
        (void)registry.Badge(badge.Raw(), k2, bv);
        (void)registry.Badge(badge.Raw(), k3, bv);
        Check(registry.Badge(badge.Raw(), k4, bv) == Lookup::NotApplicable, "Badge extension filter applies");
        registry.WaitUntilIdle(badge.Raw(), 3000);
        Check(registry.Badge(badge.Raw(), k1, bv) == Lookup::Ready && bv.shown && bv.icon == PULSE_ICON_LOCK &&
                  bv.tone == PULSE_COLUMN_TONE_WARNING,
              "Badge resolves built-in icon names (lock) with a tone");
        Check(registry.Badge(badge.Raw(), k2, bv) == Lookup::Ready && !bv.shown, "No badge when the plugin skips");
        Check(registry.Badge(badge.Raw(), k3, bv) == Lookup::Ready && !bv.shown, "Unknown icon names draw nothing");
        Check(registry.Cell(badge.Raw(), k1, cell) == Lookup::NotApplicable, "Cell on a badge handle is not applicable");

        // Unregister waits for a running callback, which sees cancellation.
        struct Slow {
            std::atomic<bool> inside{false};
            std::atomic<bool> saw_cancel{false};
        } slow;
        PulseColumnDesc sd{};
        sd.size = sizeof(sd);
        sd.id = V("slow");
        sd.title = V("Slow");
        sd.user = &slow;
        sd.query = [](void* user, const PulseColumnBatch* batch) noexcept -> PulseStatus {
            auto* s2 = static_cast<Slow*>(user);
            s2->inside = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                if (batch->is_cancelled(batch->context)) {
                    s2->saw_cancel = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return PULSE_OK;
        };
        ColumnsAdapter adapter2("pulse.slow", [] { return true; }, sink, nullptr, &registry);
        PulseColumnsV1 t2{};
        (void)adapter2.Query(PULSE_COLUMNS_VERSION_1, &t2, sizeof(t2));
        PulseColumnHandle slow_h = 0;
        Check(t2.register_column(t2.context, &sd, &slow_h) == PULSE_OK, "Second package registers its own column");
        Check(!registry.Owns(slow_h, "pulse.demo") && registry.Owns(slow_h, "pulse.slow"), "Owns checks the package");
        Check(table.unregister(table.context, slow_h) == PULSE_ERROR_ARGUMENT, "A package cannot unregister another's column");
        (void)registry.Cell(slow_h, File(L"C:\\s\\x"), cell);
        for (int i = 0; i < 500 && !slow.inside; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Check(adapter2.CallbackActive(), "CallbackActive while the query runs");
        const auto t0 = std::chrono::steady_clock::now();
        Check(t2.unregister(t2.context, slow_h) == PULSE_OK, "unregister succeeds");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        Check(slow.saw_cancel && !adapter2.CallbackActive() && ms < 1000,
              "unregister returns after the running query saw is_cancelled");
        Check(registry.Cell(slow_h, File(L"C:\\s\\x"), cell) == Lookup::NotApplicable, "Unregistered handles look up nothing");

        // Thread rule: calls from another thread are refused.
        PulseStatus off_thread = PULSE_OK;
        std::thread([&] { off_thread = table.invalidate(table.context, handle, {}); }).join();
        Check(off_thread == PULSE_ERROR_STATE, "pulse.columns refuses calls off the owner thread");

        // Close removes everything of the session.
        adapter.Close();
        Check(registry.List(Kind::Column).empty() && registry.List(Kind::Badge).empty(),
              "Close unregisters every provider of the session");
        Check(table.register_column(table.context, &desc, &h) == PULSE_ERROR_STATE, "Calls after Close are refused");
        col.Reset();
        badge.Reset();
    }

    std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
