// snapshot_store_test.cpp - SnapshotStore keeps "changed while listed" marks
// across eviction (#58-12: new files in a big folder needed F5).
#include "../fs/fs_enum.h"
#include "../fs/fs_snapshot.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

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
void TestMemoryAccounting() {
    using namespace pulse::fs;
    auto short_entries = std::make_shared<std::vector<DirEntry>>(100000);
    for (auto& e : *short_entries) e.name = L"x";
    const size_t short_bytes = sizeof(std::vector<DirEntry>) + short_entries->capacity() * sizeof(DirEntry);
    SnapshotStore measure(16, 0);
    measure.Update(L"C:\\measure-short", 1, short_entries);
    std::cout << "MEMORY short_actual=" << short_bytes << " counted=" << measure.ResidentBytes() << '\n';
    Check(measure.ResidentBytes() == short_bytes, "SNAP-MEM inline names and absent cold records add no heap allocation");
    auto other_short = std::make_shared<std::vector<DirEntry>>(*short_entries);
    SnapshotStore two;
    two.Update(L"C:\\short-a", 1, short_entries);
    two.Update(L"C:\\short-b", 1, other_short);
    uint64_t generation = 0;
    Check(two.GetOrStart(L"C:\\short-a", generation) == short_entries && two.EntryCount() == 2,
        "SNAP-MEM two 100k short-name folders fit the unchanged 24 MiB budget without re-listing");

    auto cold = std::make_shared<std::vector<DirEntry>>(1);
    auto& e = cold->front();
    e.name = std::wstring(120, L'n');
    auto& extra = e.extra_.Mutable();
    std::wstring* fields[] = {&extra.full_path, &extra.recycle_path, &extra.change_type_text,
        &extra.change_old_path, &extra.link_target, &extra.link_destination, &extra.shell_id};
    size_t expected = sizeof(std::vector<DirEntry>) + cold->capacity() * sizeof(DirEntry) +
        sizeof(DirEntryExtra) + (e.name.capacity() + 1) * sizeof(wchar_t);
    for (auto field : fields) {
        field->reserve(256);
        *field = L"long cold value";
        expected += (field->capacity() + 1) * sizeof(wchar_t);
    }
    extra.recycle_path.clear(); // Empty does not imply its allocation was released.
    SnapshotStore cold_store(16, 0);
    cold_store.Update(L"C:\\cold", 1, cold);
    std::cout << "MEMORY cold_actual=" << expected << " counted=" << cold_store.ResidentBytes() << '\n';
    Check(cold_store.ResidentBytes() == expected, "SNAP-MEM cold object plus all seven external strings and terminators counted");
    auto large_cold = std::make_shared<std::vector<DirEntry>>(6000, e);
    const auto& copy = large_cold->front();
    const auto& copy_extra = copy.extra_.Get();
    const std::wstring* copied_fields[] = {&copy_extra.full_path, &copy_extra.recycle_path, &copy_extra.change_type_text,
        &copy_extra.change_old_path, &copy_extra.link_target, &copy_extra.link_destination, &copy_extra.shell_id};
    size_t per_entry = sizeof(DirEntry) + sizeof(DirEntryExtra) + (copy.name.capacity()+1)*sizeof(wchar_t);
    for(auto field : copied_fields) if(field->capacity()>std::wstring().capacity()) per_entry+=(field->capacity()+1)*sizeof(wchar_t);
    const size_t copied_bytes = sizeof(std::vector<DirEntry>) + 6000 * per_entry;
    SnapshotStore bounded;
    for (int i=0;i<12;++i) bounded.Update(L"C:\\cold-"+std::to_wstring(i),1,std::make_shared<std::vector<DirEntry>>(*large_cold));
    Check(bounded.ResidentBytes() == bounded.EntryCount()*copied_bytes && bounded.ResidentBytes() <= 24ull*1024*1024,
        "SNAP-MEM long-lived cold listings respect actual payload budget");

    const auto begin=std::chrono::steady_clock::now();
    for(int i=0;i<100;++i) measure.Update(L"C:\\measure-short",2+i,short_entries);
    for(int i=0;i<100;++i) measure.Put(L"C:\\measure-short",short_entries);
    const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    std::cout << "PERF same_snapshot_200_updates_ms=" << elapsed << '\n';
    measure.MarkDirty(L"C:\\measure-short");
    const DirectoryIdentity identity{7, 8, 9};
    measure.Update(L"C:\\measure-short", 150, short_entries, identity, 500);
    Check(!measure.IsDirty(L"C:\\measure-short") && measure.ListedSince(L"C:\\measure-short", 500) &&
        SameDirectoryIdentity(measure.Identity(L"C:\\measure-short"), identity),
        "SNAP-MEM same-pointer Update still publishes freshness and directory identity");
    measure.MarkDirty(L"C:\\measure-short");
    Check(measure.Put(L"C:\\measure-short", short_entries) == 150 &&
        !measure.IsDirty(L"C:\\measure-short") && measure.GetOrStart(L"C:\\measure-short", generation) == short_entries && generation == 150,
        "SNAP-MEM same-pointer Put clears dirtiness without changing worker generation");
    auto replacement=Listing(2);
    measure.Update(L"C:\\measure-short", 200, replacement);
    const auto replacement_bytes=measure.ResidentBytes();
    measure.Update(L"C:\\measure-short", 100, short_entries);
    Check(measure.Peek(L"C:\\measure-short")==replacement && measure.ResidentBytes()==replacement_bytes,
        "SNAP-MEM new immutable snapshot is recounted and stale generations cannot replace it");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--memory") {
        TestMemoryAccounting();
        return failures ? 1 : 0;
    }
    TestNeverListed();
    TestBigFolderEvicted();
    TestCountEviction();
    TestCleared();
    TestBounded();
    std::cout << (failures ? "FAILED" : "ALL PASS") << " (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
