#include "../ui/extension_data_controller.h"
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using pulse::extensions::UiDataRow;
using namespace pulse::ui;
int failures = 0;
void Check(bool condition, const char *message) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << message << '\n';
    failures += !condition;
}
UiDataRow Row(std::string id, std::string text = "Row") {
    UiDataRow row;
    row.id = std::move(id);
    row.text = std::move(text);
    return row;
}
UiDataPage Snapshot(std::string revision = "r1") {
    UiDataPage page;
    page.revision = std::move(revision);
    page.rows = {Row("a"), Row("b")};
    page.next_cursor = "next";
    return page;
}
struct Fixture {
    std::vector<UiDataCompletion> completions;
    UiDataPage last;
    int applies = 0, cancels = 0, wakes = 0;
    ExtensionDataController controller{
        [this](const UiDataRequest &, UiDataCompletion completion) {
            completions.push_back(std::move(completion));
            return [this] { ++cancels; };
        },
        [this] { ++wakes; },
        [this](const UiDataPage &page) { last = page; ++applies; }};
    Fixture() { controller.Request({}); }
    void Publish(UiDataPage page) {
        completions.back()(std::move(page));
        controller.Drain();
    }
};
}
int main() {
    {
        Fixture f;
        f.Publish(Snapshot());
        f.controller.Request({}, true);
        auto next = Snapshot();
        next.rows = {Row("b", "Updated"), Row("c")};
        f.Publish(next);
        Check(f.last.status == "ready" && f.last.rows.size() == 3 && f.last.rows[1].text == "Updated",
              "same-revision snapshot pagination upserts stable top-level IDs");
        next.revision = "r2";
        f.Publish(next);
        Check(f.last.status == "error" && f.last.error == "revision_changed" && f.last.rows.size() == 3 &&
                  f.controller.NextCursor().empty(), "snapshot pagination rejects a changed revision and retains rows");
    }
    {
        Fixture f;
        f.Publish(Snapshot());
        f.controller.Request({});
        UiDataPage error;
        error.status = "error";
        error.error = "provider_failed";
        error.rows = {Row("unexpected")};
        error.next_cursor = "unexpected";
        f.Publish(error);
        Check(f.last.status == "error" && f.last.rows.size() == 2 && f.last.rows.front().id == "a" &&
                  f.controller.NextCursor().empty(), "refresh errors retain displayed rows and ignore supplied data");
    }
    {
        Fixture f;
        f.Publish(Snapshot());
        auto excessive = Snapshot("r2");
        excessive.rows = {Row("root")};
        for (int index = 0; index < 4096; ++index)
            excessive.rows.front().children.push_back(Row("child" + std::to_string(index)));
        f.Publish(excessive);
        Check(f.last.status == "error" && f.last.error == "row_limit_exceeded" && f.last.rows.size() == 2 &&
                  f.last.rows.front().id == "a", "recursive row budget rejects 4097 rows without changing displayed snapshot");
    }
    {
        Fixture f;
        auto duplicate = Snapshot();
        duplicate.rows.front().children.push_back(Row("b"));
        f.Publish(duplicate);
        Check(f.last.status == "error" && f.last.error == "duplicate_row_id" && f.last.rows.empty(),
              "nested row IDs must be unique across the entire snapshot");
    }
    {
        Fixture f;
        auto exact = Snapshot();
        exact.rows = {Row("root")};
        exact.next_cursor.clear();
        for (int index = 0; index < 4095; ++index)
            exact.rows.front().children.push_back(Row("child" + std::to_string(index)));
        f.Publish(exact);
        Check(f.last.status == "ready" && f.last.rows.front().children.size() == 4095,
              "recursive row budget accepts exactly 4096 rows");
        f.controller.Request({}, true);
        UiDataPage additional;
        additional.revision = "r1";
        additional.rows = {Row("additional")};
        f.Publish(additional);
        Check(f.last.status == "error" && f.last.error == "row_limit_exceeded" && f.last.rows.size() == 1 &&
                  f.last.rows.front().children.size() == 4095,
              "snapshot pagination validates its accumulated recursive row budget");
    }
    {
        Fixture f;
        f.Publish(Snapshot());
        const auto completion = f.completions.back();
        std::thread worker([&] {
            for (int index = 0; index < 66; ++index)
                completion(Snapshot("overflow" + std::to_string(index)));
        });
        worker.join();
        f.controller.Drain();
        Check(f.last.status == "error" && f.last.error == "page_queue_full" && f.last.rows.front().id == "a" &&
                  f.controller.NextCursor().empty(), "inbox overflow reports an error and retains the established snapshot");
        const int applies = f.applies;
        completion(Snapshot("late"));
        f.controller.Drain();
        Check(f.applies == applies, "overflow rejects later pages from the failed generation");
        const size_t requests = f.completions.size();
        f.controller.Request({}, true);
        Check(f.completions.size() == requests, "overflow refuses next-page requests until refresh");
        f.controller.Request({});
        f.Publish(Snapshot("recovered"));
        Check(f.last.status == "ready" && f.last.revision == "recovered", "refresh recovers a failed inbox stream");
    }
    {
        Fixture f;
        f.Publish(Snapshot());
        for (int index = 0; index < 65; ++index)
            f.completions.back()(Snapshot());
        f.controller.Request({}, true);
        f.controller.Drain();
        Check(f.last.status == "error" && f.last.error == "page_queue_full" && f.last.rows.size() == 2,
              "next-page requests cannot hide an overflow error before its UI drain");
    }
#ifndef PULSE_CONTROLLER_BASELINE
    {
        Fixture f;
        UiDataPage delta;
        delta.delta = true;
        delta.base_revision = "r1";
        delta.revision = "r2";
        delta.rows = {Row("b", "Updated"), Row("c")};
        delta.removed_ids = {"a"};
        f.Publish(delta);
        Check(f.last.status == "error" && f.last.error == "stale_revision" && f.last.rows.empty(),
              "delta requires an established matching snapshot");
        f.Publish(Snapshot());
        f.Publish(delta);
        Check(f.last.status == "ready" && f.last.rows.size() == 2 && f.last.rows.front().id == "b" &&
                  f.last.rows.front().text == "Updated" && f.last.rows.back().id == "c" &&
                  f.last.revision == "r2" && f.controller.NextCursor().empty(),
              "delta removes top-level rows, upserts stable IDs, and clears pagination");
        f.Publish(delta);
        Check(f.last.status == "error" && f.last.error == "stale_revision" && f.last.rows.size() == 2 &&
                  f.last.rows.back().id == "c", "stale delta revision preserves the last successful delta");
        f.controller.Request({});
        f.Publish(Snapshot());
        f.controller.Request({}, true);
        f.Publish(delta);
        Check(f.last.status == "error" && f.last.error == "invalid_delta" && f.last.rows.size() == 2 &&
                  f.last.rows.front().id == "a", "next-page streams reject delta pages");
    }
    {
        Fixture f;
        auto tree = Snapshot();
        tree.rows.front().children.push_back(Row("descendant"));
        f.Publish(tree);
        UiDataPage delta;
        delta.delta = true;
        delta.base_revision = "r1";
        delta.revision = "r2";
        delta.removed_ids = {"descendant"};
        f.Publish(delta);
        Check(f.last.status == "ready" && f.last.rows.front().children.size() == 1,
              "delta removals only address top-level row IDs");
        delta.base_revision = "r2";
        delta.revision = "r3";
        delta.rows = {Row("replacement")};
        delta.rows.front().children.push_back(Row("descendant"));
        f.Publish(delta);
        Check(f.last.status == "error" && f.last.error == "duplicate_row_id" && f.last.rows.size() == 2,
              "delta rejects nested IDs colliding with retained rows");
        delta.rows = {Row("a", "Replacement")};
        delta.rows.front().children.push_back(Row("new_descendant"));
        f.Publish(delta);
        Check(f.last.status == "ready" && f.last.rows.front().children.front().id == "new_descendant" &&
                  f.last.rows.front().text == "Replacement", "delta replaces an ancestor and its complete descendant tree");
    }
    {
        Fixture f;
        auto tree = Snapshot();
        tree.rows = {Row("root")};
        tree.next_cursor.clear();
        for (int index = 0; index < 4095; ++index)
            tree.rows.front().children.push_back(Row("child" + std::to_string(index)));
        f.Publish(tree);
        UiDataPage delta;
        delta.delta = true;
        delta.base_revision = "r1";
        delta.revision = "r2";
        delta.rows = {Row("additional")};
        f.Publish(delta);
        Check(f.last.status == "error" && f.last.error == "row_limit_exceeded" && f.last.rows.size() == 1 &&
                  f.last.rows.front().children.size() == 4095,
              "delta validates the combined recursive row budget before committing");
        for (int index = 0; index < 65; ++index)
            f.completions.back()(delta);
        f.controller.Drain();
        Check(f.last.status == "error" && f.last.error == "page_queue_full" && f.last.rows.size() == 1,
              "delta queue overflow fails the stream without silently dropping an update");
    }
#endif
    {
        Fixture f;
        const auto cancelled = f.completions.back();
        f.controller.Request({});
        cancelled(Snapshot("stale"));
        f.controller.Drain();
        Check(f.applies == 0 && f.cancels == 1, "refresh cancels the old generation before late completions");
        f.Publish(Snapshot());
        const auto explicit_cancel = f.completions.back();
        f.controller.Cancel();
        explicit_cancel(Snapshot("cancelled"));
        f.controller.Drain();
        Check(f.applies == 1 && f.cancels == 2, "explicit cancellation invalidates late completions");
        f.controller.Request({});
        f.Publish(Snapshot());
        const auto closed = f.completions.back();
        const int applies = f.applies;
        f.controller.Close();
        closed(Snapshot("closed"));
        f.controller.Drain();
        Check(f.applies == applies && f.cancels == 3, "close cancels the provider and discards later completions");
    }
    std::cout << "Failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
