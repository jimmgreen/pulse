#include "../app/system_ctx_registry.h"
#include "../app/system_ctx_packaged.h"
#include <cstdio>
#include <set>

using namespace pulse::app::sysctx;
namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
struct Fixture {
    std::wstring base = L"Software\\PulseTest\\SystemCtxTree_" + std::to_wstring(GetCurrentProcessId());
    Roots roots;
    Fixture() {
        roots.user = roots.machine = HKEY_CURRENT_USER;
        roots.user_classes = base + L"\\User";
        roots.machine_classes = base + L"\\Machine";
        roots.user_command_store = base + L"\\UserStore";
        roots.machine_command_store = base + L"\\MachineStore";
        roots.resolve_strings = roots.check_owner = false;
        roots.file_exts.clear();
    }
    ~Fixture() { RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str()); }
    void Value(const std::wstring& path, const wchar_t* name, const wchar_t* value) {
        HKEY key = nullptr;
        const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, (base + L"\\" + path).c_str(),
            0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS) {
            std::printf("[ERROR] fixture RegCreateKeyEx status=%ld\n", status);
            ++failures; return;
        }
        if (RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
            static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) ++failures;
        RegCloseKey(key);
    }
};
const Item* Find(const std::vector<Item>& items, const wchar_t* display) {
    for (const auto& item : items) if (item.display == display) return &item;
    return nullptr;
}
}
int main() {
    Fixture f;
    f.Value(L"User\\Tree\\shell\\Inline", L"MUIVerb", L"Inline");
    f.Value(L"User\\Tree\\shell\\Inline", L"SubCommands", L"");
    f.Value(L"Machine\\Tree\\shell\\Inline\\shell\\One", nullptr, L"Machine child");
    f.Value(L"User\\Tree\\shell\\Inline\\shell\\One", nullptr, L"User child");
    f.Value(L"User\\Tree\\shell\\Inline\\shell\\One\\command", nullptr, L"one.exe %1");
    f.Value(L"User\\Tree\\shell\\Inline\\shell\\Nested", nullptr, L"Nested");
    f.Value(L"User\\Tree\\shell\\Inline\\shell\\Nested\\shell\\Deep", nullptr, L"Deep leaf");
    f.Value(L"User\\Tree\\shell\\Inline\\shell\\Nested\\shell\\Deep\\command", nullptr, L"deep.exe %1");
    for (const auto* parent : {L"Shared A", L"Shared B"}) {
        const std::wstring key = L"User\\Tree\\shell\\" + std::wstring(parent);
        f.Value(key, nullptr, parent); f.Value(key, L"SubCommands", L"shared; shared;missing");
    }
    f.Value(L"MachineStore\\shared", nullptr, L"Shared command");
    f.Value(L"MachineStore\\shared\\command", nullptr, L"shared.exe %1");
    f.Value(L"UserStore\\shared", nullptr, L"User store shadow");
    f.Value(L"User\\Tree\\shell\\Extended", nullptr, L"Extended");
    f.Value(L"User\\Tree\\shell\\Extended", L"ExtendedSubCommandsKey", L"Referenced\\shell");
    f.Value(L"Machine\\Referenced\\shell\\Action", nullptr, L"Referenced action");
    f.Value(L"Machine\\Referenced\\shell\\Action\\command", nullptr, L"ref.exe %1");
    f.Value(L"User\\Tree\\shell\\Loop", nullptr, L"Loop");
    f.Value(L"User\\Tree\\shell\\Loop", L"ExtendedSubCommandsKey", L"Cycle");
    f.Value(L"User\\Cycle\\shell\\Again", nullptr, L"Again");
    f.Value(L"User\\Cycle\\shell\\Again", L"ExtendedSubCommandsKey", L"Cycle");
    const Scene scene{L"test-tree", {L"Tree"}, SceneMode::Menu};
    const auto rows = Enumerate(f.roots, scene);
    const auto* parent = Find(rows, L"Inline");
    const auto* child = Find(rows, L"User child");
    const auto* deep = Find(rows, L"Deep leaf");
    Check(parent && child && child->parent_id == parent->tree_id && child->depth == 1 &&
          parent->has_children && !Find(rows, L"Machine child"),
          "inline cascade reads merged user-over-machine children with parent identity");
    Check(deep && deep->depth == 2 && deep->command == L"deep.exe %1",
          "embedded shell without SubCommands keeps nested descendants");
    size_t shared = 0; std::set<std::wstring> shared_parents, identities;
    bool unique = true, readonly = true;
    for (const auto& row : rows) {
        unique = unique && identities.insert(row.tree_id).second;
        if (row.display == L"Shared command") {
            ++shared; shared_parents.insert(row.parent_id);
            readonly = readonly && row.root == Root::Machine && row.registry_read_only &&
                row.registry_path == f.roots.machine_command_store + L"\\shared" &&
                !SystemItemToggleable(row) &&
                Apply(f.roots, MakeOp(row, Action::Disable)) == OpResult::Unsupported;
        }
    }
    Check(shared == 2 && shared_parents.size() == 2 && unique && !Find(rows, L"User store shadow"),
          "shared CommandStore reference keeps two occurrences but deduplicates same-parent tokens");
    Check(shared == 2 && readonly,
          "shared registry children are inspect-only and mutation API rejects their operations");
    const auto* referenced = Find(rows, L"Referenced action");
    Check(referenced && referenced->depth == 1 && referenced->root == Root::Machine &&
          referenced->registry_read_only && !referenced->parent_id.empty(),
          "ExtendedSubCommandsKey shell suffix resolves merged source and parent");
    const auto* again = Find(rows, L"Again");
    const auto* a = Find(rows, L"Shared A");
    Check(again && again->children_incomplete && a && a->children_incomplete && rows.size() < 30,
          "cycles and unresolved CommandStore references terminate with explicit incompleteness");
    bool batch_rejected = child != nullptr;
    if (child) {
        std::vector<Op> parsed;
        batch_rejected = !ParseBatch(SerializeBatch({MakeOp(*child, Action::Disable)}), parsed);
    }
    Check(batch_rejected, "readonly inline descendant cannot become a writable serialized batch");
    const auto repeat = Enumerate(f.roots, scene);
    bool stable = rows.size() == repeat.size();
    for (size_t i = 0; i < rows.size() && i < repeat.size(); ++i)
        stable = stable && rows[i].tree_id == repeat[i].tree_id && rows[i].parent_id == repeat[i].parent_id;
    Check(stable, "unchanged registry scan retains every occurrence and parent identity");
    std::wstring chain = L"User\\Limit\\shell\\First";
    for (int i = 0; i < 24; ++i) {
        f.Value(chain, nullptr, (L"Depth " + std::to_wstring(i)).c_str());
        chain += L"\\shell\\Next";
    }
    const auto limited = Enumerate(f.roots, Scene{L"depth-limit", {L"Limit"}, SceneMode::Menu});
    std::printf("depth fixture rows=%zu last_depth=%u incomplete=%d\n", limited.size(),
                limited.empty() ? 0 : limited.back().depth,
                !limited.empty() && limited.back().children_incomplete);
    Check(limited.size() == 17 && limited.back().depth == 16 && limited.back().children_incomplete,
          "deep registry trees stop at sixteen descendant levels and report truncation");
    f.Value(L"User\\Wide\\shell\\Group", nullptr, L"Wide group");
    for (int i = 0; i < 260; ++i)
        f.Value(L"User\\Wide\\shell\\Group\\shell\\Item" + std::to_wstring(i), nullptr, L"Wide child");
    const auto wide = Enumerate(f.roots, Scene{L"wide-limit", {L"Wide"}, SceneMode::Menu});
    Check(wide.size() == 257 && wide[0].children_incomplete && wide[0].has_children,
          "wide registry branch is bounded and explicitly reports incomplete children");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
