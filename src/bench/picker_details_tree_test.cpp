#include "../ui/picker_details_tree.h"
#include <cstdio>
#include <cstdlib>

using namespace pulse::ui;
namespace fs = pulse::fs;
namespace {
int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::printf("[FAIL] line=%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
PickerEntry Entry(std::wstring path, bool folder = false) {
    PickerEntry entry;
    entry.path = std::move(path);
    entry.name = entry.path.substr(entry.path.find_last_of(L'\\') + 1);
    entry.kind = folder ? PickerEntryKind::Folder : PickerEntryKind::File;
    entry.source = 99;
    return entry;
}
PickerListing Listing(std::wstring path, std::vector<PickerEntry> entries, uint64_t token = 0) {
    PickerListing listing;
    listing.path = std::move(path);
    listing.generation = token;
    listing.entries = std::move(entries);
    auto snapshot = std::make_shared<std::vector<fs::DirEntry>>();
    for (size_t i = 0; i < listing.entries.size(); ++i) {
        fs::DirEntry row;
        row.name = listing.entries[i].name;
        row.is_dir = listing.entries[i].kind == PickerEntryKind::Folder;
        row.size = 100 + i;
        snapshot->push_back(std::move(row));
    }
    listing.snapshot = std::move(snapshot);
    return listing;
}
bool Complete(PickerDetailsTree& tree, PickerListing listing) {
    return tree.Complete(std::make_unique<PickerListing>(std::move(listing)));
}
void Aligned(const PickerDetailsTree::View& view) {
    CHECK(view.entries.size() == view.snapshot->size());
    CHECK(view.entries.size() == view.rows->rows.size());
    for (size_t i = 0; i < view.entries.size(); ++i) {
        CHECK(view.entries[i].source == i);
        CHECK(view.rows->ViewOf(static_cast<int>(i)) == static_cast<int>(i));
        CHECK(!(*view.snapshot)[i].full_path.empty());
    }
}
}
int main() {
    PickerDetailsTree tree;
    const auto root = Listing(L"C:\\root", {Entry(L"C:\\root\\B", true), Entry(L"C:\\root\\A", true), Entry(L"C:\\root\\last.txt")});
    tree.Reset(root);
    auto view = tree.Build();
    CHECK(view.entries[0].name == L"B");
    CHECK(!tree.Toggle(L"C:\\root\\last.txt"));
    CHECK(tree.Toggle(L"c:/root/B/"));
    CHECK(tree.NeedsLoad(L"C:\\root\\B"));
    tree.BeginLoad(L"C:\\root\\B", 1, 123);
    CHECK(!tree.NeedsLoad(L"C:\\root\\B"));
    view = tree.Build();
    CHECK(view.rows->rows[1].kind == TreeRowKind::Loading);
    CHECK(view.rows->rows[1].since_ms == 123);
    CHECK(view.rows->ParentRow(1) == 0);
    CHECK(!Complete(tree, Listing(L"C:\\root\\B", {}, 9)));
    CHECK(Complete(tree, Listing(L"C:\\root\\B", {Entry(L"C:\\root\\B\\sub", true), Entry(L"C:\\root\\B\\same.txt")}, 1)));
    CHECK(tree.Toggle(L"C:\\root\\B\\sub"));
    tree.BeginLoad(L"C:\\root\\B\\sub", 2);
    CHECK(Complete(tree, Listing(L"C:\\root\\B\\sub", {Entry(L"C:\\root\\B\\sub\\deep.txt")}, 2)));
    CHECK(tree.Toggle(L"C:\\root\\A"));
    tree.BeginLoad(L"C:\\root\\A", 3);
    auto a = Listing(L"C:\\root\\A", {Entry(L"C:\\root\\A\\same.txt")}, 3);
    auto a_snapshot = std::make_shared<std::vector<fs::DirEntry>>(*a.snapshot);
    (*a_snapshot)[0].full_path = L"\\\\?\\C:\\root\\A\\same.txt";
    a.snapshot = a_snapshot;
    CHECK(Complete(tree, std::move(a)));
    view = tree.Build();
    Aligned(view);
    CHECK(view.entries.size() == 7);
    CHECK(view.entries[2].name == L"deep.txt" && view.rows->rows[2].depth == 2);
    CHECK(view.entries[3].name == view.entries[5].name);
    CHECK((*view.snapshot)[3].full_path == L"C:\\root\\B\\same.txt");
    CHECK((*view.snapshot)[5].full_path == L"\\\\?\\C:\\root\\A\\same.txt");
    CHECK((*view.snapshot)[3].size == 101);
    CHECK(view.entries[6].name == L"last.txt");
    CHECK(tree.Toggle(L"C:\\root\\B"));
    CHECK(!tree.Expanded(L"C:\\root\\B\\sub"));
    CHECK(tree.Build().entries.size() == 4);
    CHECK(tree.Toggle(L"C:\\root\\B"));
    CHECK(tree.NeedsLoad(L"C:\\root\\B"));
    tree.BeginLoad(L"C:\\root\\B", 4);
    CHECK(tree.Toggle(L"C:\\root\\B"));
    CHECK(!Complete(tree, Listing(L"C:\\root\\B", {}, 4)));
    CHECK(tree.Toggle(L"C:\\root\\B"));
    tree.BeginLoad(L"C:\\root\\B", 5);
    CHECK(!Complete(tree, Listing(L"C:\\root\\B", {}, 4)));
    tree.Reset(root);
    CHECK(!Complete(tree, Listing(L"C:\\root\\B", {}, 5)));
    CHECK(tree.Toggle(L"C:\\root\\B"));
    tree.BeginLoad(L"C:\\root\\B", 5);
    CHECK(tree.NeedsLoad(L"C:\\root\\B"));
    tree.BeginLoad(L"C:\\root\\B", 6);
    CHECK(Complete(tree, Listing(L"C:\\root\\B", {}, 6)));
    CHECK(tree.Build().rows->rows[1].kind == TreeRowKind::Empty);
    tree.BeginLoad(L"C:\\root\\B", 7);
    auto failed = Listing(L"C:\\root\\B", {}, 7);
    failed.error = ERROR_ACCESS_DENIED;
    CHECK(Complete(tree, std::move(failed)));
    CHECK(tree.Build().rows->rows[1].kind == TreeRowKind::Failed);
    std::vector<PickerEntry> many;
    for (int i = 0; i < 1001; ++i) many.push_back(Entry(L"C:\\root\\B\\" + std::to_wstring(i)));
    tree.BeginLoad(L"C:\\root\\B", 8);
    CHECK(Complete(tree, Listing(L"C:\\root\\B", many, 8)));
    view = tree.Build();
    CHECK(view.entries.size() == 504);
    CHECK(view.rows->rows[501].kind == TreeRowKind::More);
    CHECK(view.rows->rows[501].shown == 500 && view.rows->rows[501].total == 1001);
    tree.ShowMore(L"C:\\root\\B");
    CHECK(tree.Build().rows->rows[1001].kind == TreeRowKind::More);
    tree.ShowMore(L"C:\\root\\B");
    view = tree.Build();
    CHECK(view.entries.size() == 1004);
    CHECK(view.entries[1001].name == L"1000");
    Aligned(view);
    auto archive = Entry(L"C:\\root\\outer.zip", true);
    archive.archive = true;
    tree.Reset(Listing(L"", {Entry(L"shell:::test", true), archive, Entry(L"\\\\server\\share\\dir", true)}));
    CHECK(!tree.Toggle(L"shell:::test"));
    CHECK(!tree.Toggle(archive.path));
    CHECK(tree.Toggle(L"\\\\server\\share\\dir"));
    tree.Reset(root, false);
    CHECK(!tree.Toggle(L"C:\\root\\B"));
    CHECK(!tree.Build().rows->rows[0].expandable);
    tree.Clear();
    CHECK(tree.Build().entries.empty());
    CHECK(!Complete(tree, Listing(L"C:\\root\\B", {}, 8)));
    tree.Reset(root);
    CHECK(tree.Toggle(L"C:\\root\\B"));
    many.clear();
    for (int i = 0; i < 11000; ++i) many.push_back(Entry(L"C:\\root\\B\\" + std::to_wstring(i)));
    tree.BeginLoad(L"C:\\root\\B", 9);
    CHECK(Complete(tree, Listing(L"C:\\root\\B", many, 9)));
    tree.ShowMore(L"C:\\root\\B", true);
    view = tree.Build();
    CHECK(view.entries.size() == 10004);
    CHECK(view.rows->rows[10001].kind == TreeRowKind::More);
    CHECK(view.rows->rows[10001].shown == 10000);
    tree.ShowMore(L"C:\\root\\B");
    CHECK(tree.Build().rows->rows[10001].total == 11000);
    CHECK(view.entries.back().name == L"last.txt");
    tree.Reset(Listing(L"C:\\deep", {Entry(L"C:\\deep\\d", true)}));
    std::wstring deep = L"C:\\deep\\d";
    for (uint64_t i = 0; i < 64; ++i) {
        CHECK(tree.Toggle(deep));
        tree.BeginLoad(deep, 10 + i);
        CHECK(Complete(tree, Listing(deep, {Entry(deep + L"\\d", true)}, 10 + i)));
        deep += L"\\d";
    }
    view = tree.Build();
    CHECK(view.entries.size() == 65);
    CHECK(view.rows->rows.back().depth == 64);
    CHECK(!view.rows->rows.back().expandable);
    CHECK(!tree.Toggle(deep));
    CHECK(tree.Toggle(L"C:\\deep\\d"));
    CHECK(tree.Build().entries.size() == 1);
    CHECK(!tree.Expanded(deep));
    tree.Reset(root);
    CHECK(tree.Toggle(L"C:\\root\\B"));
    tree.BeginLoad(L"C:\\root\\B", 74);
    CHECK(Complete(tree, Listing(L"C:\\root\\B", {Entry(L"C:\\root\\B\\sub", true)}, 74)));
    CHECK(tree.Toggle(L"C:\\root\\B\\sub"));
    tree.BeginLoad(L"C:\\root\\B\\sub", 75);
    CHECK(tree.Toggle(L"C:\\root\\A"));
    tree.BeginLoad(L"C:\\root\\A", 76);
    const auto before_collapse = tree.Build().rows->revision;
    tree.CollapseAll();
    view = tree.Build();
    Aligned(view);
    CHECK(view.rows->revision > before_collapse);
    CHECK(view.entries.size() == root.entries.size());
    CHECK(view.entries[0].name == L"B" && view.entries[1].name == L"A");
    CHECK(!tree.Expanded(L"C:\\root\\B") && !tree.Expanded(L"C:\\root\\A"));
    CHECK(!tree.Expanded(L"C:\\root\\B\\sub"));
    CHECK(!Complete(tree, Listing(L"C:\\root\\B\\sub", {}, 75)));
    CHECK(!Complete(tree, Listing(L"C:\\root\\A", {}, 76)));
    CHECK(tree.Toggle(L"C:\\root\\A"));
    tree.BeginLoad(L"C:\\root\\A", 76);
    CHECK(tree.NeedsLoad(L"C:\\root\\A"));
    tree.BeginLoad(L"C:\\root\\A", 77);
    CHECK(Complete(tree, Listing(L"C:\\root\\A", {}, 77)));
    std::printf("[PASS] picker_details_tree nested siblings full_path source stale collapse collapse_all reset loading empty failed pagination namespace depth budget checks=%d\n", checks);
    return 0;
}
