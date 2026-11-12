#include "../ui/picker_navigation_root.h"
#include <cstdio>
#include <utility>
namespace pulse::ui {
std::vector<std::pair<uint64_t, std::wstring>> requests;
PickerLoader::PickerLoader(HWND, UINT, UINT, UINT, UINT) {}
PickerLoader::~PickerLoader() = default;
void PickerLoader::Load(uint64_t generation, std::wstring path, PickerMode, PickerOptions) { requests.emplace_back(generation, std::move(path)); }
std::unique_ptr<PickerListing> PickerLoader::Take(LPARAM value) { return std::unique_ptr<PickerListing>(reinterpret_cast<PickerListing*>(value)); }
bool SamePickerPath(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && (a.empty() || CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL);
}
}
using namespace pulse::ui;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* name) { ++checks; if (!value) ++failures; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); }
DWORD Complete(PickerNavigationRoot& tree, uint64_t generation, std::initializer_list<const wchar_t*> folders, DWORD error = 0) {
    auto listing = std::make_unique<PickerListing>(); listing->generation = generation; listing->error = error;
    for (const auto* path : folders) { PickerEntry entry; entry.path = path; entry.name = path; listing->entries.push_back(std::move(entry)); }
    return tree.Complete(reinterpret_cast<LPARAM>(listing.release()));
}
std::vector<SidebarItem> Rows(const PickerNavigationRoot& tree) { std::vector<SidebarGroup> groups; tree.Append(groups); return groups.front().items; }
bool Has(const PickerNavigationRoot& tree, const wchar_t* path) { for (const auto& row : Rows(tree)) if (row.path == path) return true; return false; }
}
int main() {
    PickerNavigationRoot tree;
    tree.Set(nullptr, WM_APP, L"C:\\root", {});
    Check(!tree.RootLoaded() && requests.size() == 1, "root loading remains explicit");
    Complete(tree, requests.back().first, {L"C:\\root\\a", L"C:\\root\\b"});
    Check(tree.RootLoaded() && Rows(tree).size() == 3, "root has one top node and two indented children");
    tree.Toggle(L"C:\\root\\a"); const auto a = requests.back().first; const auto count = requests.size();
    tree.Toggle(L"C:\\root\\b");
    Check(requests.size() == count, "rapid sibling expansion queues without replacing active enumeration");
    Complete(tree, a, {L"C:\\root\\a\\leaf"});
    Check(requests.size() == count + 1 && requests.back().second == L"C:\\root\\b", "completing first expansion starts queued sibling");
    Complete(tree, requests.back().first, {L"C:\\root\\b\\leaf"});
    Check(Has(tree, L"C:\\root\\a\\leaf") && Has(tree, L"C:\\root\\b\\leaf"), "both rapid expansions retain their children");
    tree.Set(nullptr, WM_APP, L"C:\\root", {});
    Complete(tree, requests.back().first, {L"C:\\root\\a", L"C:\\root\\b"});
    tree.Toggle(L"C:\\root\\a"); const auto stale = requests.back().first;
    tree.Toggle(L"C:\\root\\a"); tree.Toggle(L"C:\\root\\a"); const auto fresh = requests.back().first;
    Complete(tree, stale, {L"C:\\root\\a\\stale"});
    Check(!Has(tree, L"C:\\root\\a\\stale") && fresh != stale, "collapse and reexpand invalidate stale completion");
    Complete(tree, fresh, {});
    Check(Rows(tree).size() == 3 && !Rows(tree)[1].expandable, "empty expansion completes without a phantom child or spinner");
    tree.Toggle(L"C:\\root\\b");
    Check(Complete(tree, requests.back().first, {}, ERROR_ACCESS_DENIED) == ERROR_ACCESS_DENIED, "enumeration errors propagate without pretending empty success");
    tree.Toggle(L"C:\\root\\b"); tree.Toggle(L"C:\\root\\b");
    Check(Complete(tree, requests.back().first, {}) == ERROR_SUCCESS && !Rows(tree)[2].expandable, "failed node can collapse and retry successfully");
    tree.Set(nullptr, WM_APP, L"C:\\replacement", {}); const auto old = requests.back().first;
    tree.Set(nullptr, WM_APP, L"C:\\new", {});
    Complete(tree, old, {L"C:\\replacement\\wrong"});
    Check(!tree.RootLoaded() && Rows(tree).front().path == L"C:\\new", "root replacement ignores prior root completion");
    const auto closing = requests.back().first; tree.Close(); Complete(tree, closing, {L"C:\\new\\late"});
    Check(!tree.Active(), "completion after close does not resurrect state");
    std::printf("Root queue checks=%u failures=%u\n", checks, failures); return failures ? 1 : 0;
}
