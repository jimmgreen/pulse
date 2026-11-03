// snapshot_store_test.cpp - SnapshotStore keeps "changed while listed" marks
// across eviction (#58-12: new files in a big folder needed F5).
#include "../fs/fs_enum.h"
#include "../fs/fs_snapshot.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!ok) ++failures;
}

pulse::fs::SnapshotPtr Listing(size_t count) {
    auto entries = std::make_shared<std::vector<pulse::fs::DirEntry>>();
    entries->reserve(count);
    for (size_t i = 0; i < count; ++i) {
        pulse::fs::DirEntry entry;
        entry.name = L"photo-" + std::to_wstring(i) + L".jpg";
        entries->push_back(std::move(entry));
    }
    return entries;
}

void TestNeverListed() {
    pulse::fs::SnapshotStore store;
    const std::wstring a = L"C:\\snap-never";
    Check(!store.IsDirty(a), "SNAP-01 an unknown folder is not dirty");
    store.MarkDirty(a);
    Check(store.IsDirty(a), "SNAP-01 a change to a folder with no entry is kept");
    store.BeginRefresh(a);
    Check(!store.IsDirty(a), "SNAP-01 a new listing covers it");
}

// The reported case: the folder on screen is bigger than the byte budget.
// A listing is in flight, files arrive (marked dirty), then any other folder
// is stored and evicts the big one. The listing's result must still see the
// mark, so it runs once more and shows the files copied meanwhile.
void TestBigFolderEvicted() {
    const auto big = Listing(4000);
    const auto small = Listing(4);
    pulse::fs::SnapshotStore measure(16, 0);
    measure.Update(L"C:\\measure", 1, big);
    pulse::fs::SnapshotStore store(16, measure.ResidentBytes() / 2);
    const std::wstring a = L"C:\\snap-big", b = L"C:\\snap-other";
    store.Update(a, 1, big);
    Check(store.Peek(a) != nullptr, "SNAP-02 the folder being stored stays while it is the newest");
    store.BeginRefresh(a);   // F5 / an operation finished: a listing is queued
    store.MarkDirty(a);      // files arrive while it runs
    store.Update(b, 2, small);
    Check(store.Peek(a) == nullptr, "SNAP-02 storing another folder evicts the oversized one");
    Check(store.IsDirty(a), "SNAP-02 the change made while it was listed survives the eviction");
    store.Update(a, 3, big);
    Check(!store.IsDirty(a), "SNAP-02 the follow-up listing clears it");
}

void TestCountEviction() {
    const auto listing = Listing(2);
    pulse::fs::SnapshotStore store(2, 0);
    const std::wstring a = L"C:\\snap-a", b = L"C:\\snap-b", c = L"C:\\snap-c";
    store.Update(a, 1, listing);
    store.MarkDirty(a);
    store.Update(b, 2, listing);
    store.Update(c, 3, listing);
    Check(store.Peek(a) == nullptr && store.IsDirty(a), "SNAP-03 a dirty folder evicted by count keeps its mark");
    pulse::fs::SnapshotStore clean(2, 0);
    clean.Update(a, 1, listing);
    clean.Update(b, 2, listing);
    clean.Update(c, 3, listing);
    Check(clean.Peek(a) == nullptr && !clean.IsDirty(a), "SNAP-04 a clean folder evicted leaves no mark");
    Check(clean.EntryCount() == 2, "SNAP-04 the entry count stays bounded");
}

void TestCleared() {
    const auto listing = Listing(3);
    pulse::fs::SnapshotStore store;
    const std::wstring x = L"C:\\snap-put", y = L"C:\\snap-start";
    store.MarkDirty(x);
    store.Put(x, listing);
    Check(!store.IsDirty(x), "SNAP-05 a live patch clears the mark like a dirty entry");
    store.MarkDirty(y);
    uint64_t generation = 99;
    Check(store.GetOrStart(y, generation) == nullptr && generation == 0, "SNAP-06 a marked folder is listed afresh");
    Check(!store.IsDirty(y), "SNAP-06 starting that listing clears the mark");
}

void TestBounded() {
    pulse::fs::SnapshotStore store;
    for (int i = 0; i < 5000; ++i) store.MarkDirty(L"C:\\snap-many\\" + std::to_wstring(i));
    Check(store.IsDirty(L"C:\\snap-many\\4999"), "SNAP-07 the newest mark is kept");
    Check(!store.IsDirty(L"C:\\snap-many\\0"), "SNAP-07 marks for folders with no entry are bounded");
    Check(store.EntryCount() == 0 && store.ResidentBytes() == 0, "SNAP-07 marks hold no listings");
}

void TestRevisionTracksFreshnessAndListing() {
    pulse::fs::SnapshotStore store;
    const std::wstring path = L"C:\\snap-revision";
    const auto first = Listing(1);
    const auto second = Listing(2);
    store.Update(path, 4, first);
    const auto saved = store.State(path);
    Check(saved.snapshot == first && saved.generation == 4 && saved.revision != 0 && !saved.dirty,
          "SNAP-08 state returns one coherent accepted listing");
    store.MarkDirty(path);
    const auto dirty = store.State(path);
    Check(dirty.dirty && dirty.revision > saved.revision,
          "SNAP-08 dirty transition advances the cache revision");
    store.BeginRefresh(path);
    const auto refreshing = store.State(path);
    Check(!refreshing.dirty && refreshing.revision > dirty.revision,
          "SNAP-08 refresh transition advances the cache revision");
    store.Update(path, 5, second);
    const auto replaced = store.State(path);
    Check(replaced.snapshot == second && replaced.generation == 5 && replaced.revision > refreshing.revision,
          "SNAP-08 accepted replacement advances the cache revision");
    store.Update(path, 3, first);
    const auto rejected = store.State(path);
    Check(rejected.snapshot == second && rejected.generation == 5 && rejected.revision == replaced.revision,
          "SNAP-09 rejected old result cannot advance or replace cache state");
}
} // namespace

int wmain() {
    TestNeverListed();
    TestBigFolderEvicted();
    TestCountEviction();
    TestCleared();
    TestBounded();
    TestRevisionTracksFreshnessAndListing();
    std::cout << (failures ? "FAILED" : "ALL PASS") << " (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
