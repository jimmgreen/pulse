#include "../app/extensions/transfer_preparation.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

using namespace pulse::extensions;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
}
int main() {
    namespace fs = std::filesystem;
    using namespace transfer_preparation;
    using Error = transfer_plan::Error;
    const auto root = fs::absolute(fs::path("bench_data") /
        ("transfer-preparation-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    try {
        fs::create_directories(root / "source");
        fs::create_directory(root / "target");
        const auto source = root / "source" / "example.txt";
        std::ofstream(source) << "frozen selection";
        authorization::Registry registry;
        authorization::PackageIdentity package{0, "test.transfer", std::string(64, 'a'), 1};
        Check(registry.SyncPackages({{package, true}}), "register package");
        const auto owner = registry.RegisterOwner(123);
        const auto principal = owner ? registry.CaptureBinding(package, *owner) : std::nullopt;
        if (!principal) return 1;
        transfer_plan::Store store(registry);
        std::atomic<bool> cancelled{false};
        Request request;
        request.operation = transfer_plan::Operation::Copy;
        request.sources = {source.wstring()};
        request.target = (root / "target").wstring();
        auto prepared = Prepare(request, *principal, registry, store, cancelled);
        Check(static_cast<bool>(prepared), "real local selection prepares pending plan");
        if (!prepared) return 1;
        auto frozen = store.Read(*prepared.plan_id, *principal);
        Check(frozen && (*frozen.value)->transfer.sources.size() == 1 &&
            (*frozen.value)->transfer.sources[0].identity.size == 16, "plan contains captured source identity and size");
        Check(!fs::exists(root / "target" / "example.txt") && fs::exists(source), "preparation never transfers files");
        Check(!store.Consume(*prepared.plan_id, *principal), "plan id is not a confirmed receipt");
        request.sources[0] = (root / "missing.txt").wstring();
        Check(frozen && (*frozen.value)->transfer.sources[0].path == source.wstring(), "later selection changes cannot modify frozen plan");
        auto missing = Prepare(request, *principal, registry, store, cancelled);
        Check(!missing && missing.filesystem_error != ERROR_SUCCESS, "missing source returns filesystem error without plan");
        request.sources = {source.wstring(), source.wstring()};
        Check(!Prepare(request, *principal, registry, store, cancelled), "duplicate source rejected before plan publication");
        request.sources = {source.wstring()};
        request.target = (root / "source").wstring();
        auto same_directory = Prepare(request, *principal, registry, store, cancelled);
        Check(static_cast<bool>(same_directory), "snapshot preparation does not pretend to approve destination conflicts");
        request.target = (root / "target").wstring();
        request.operation = transfer_plan::Operation::Move;
        auto moving = Prepare(request, *principal, registry, store, cancelled);
        auto moving_plan = moving ? store.Read(*moving.plan_id, *principal) : transfer_plan::Result<transfer_plan::Store::FrozenPlan>{};
        Check(moving_plan && (*moving_plan.value)->transfer.operation == transfer_plan::Operation::Move,
            "move intent retained without executing move");
        request.sources.clear();
        Check(Prepare(request, *principal, registry, store, cancelled).error == Error::InvalidDraft, "empty selection rejected");
        request.sources.assign(65, source.wstring());
        Check(Prepare(request, *principal, registry, store, cancelled).error == Error::InvalidDraft, "oversized selection bounded before I/O");
        request.sources = {std::wstring(32769, L'x')};
        Check(Prepare(request, *principal, registry, store, cancelled).error == Error::InvalidDraft, "oversized path rejected before I/O");
        request.sources = {(root / "source").wstring()};
        Check(!Prepare(request, *principal, registry, store, cancelled), "directory source unsupported");
        const auto tree_root = root / "tree";
        std::filesystem::create_directories(tree_root / "nested");
        std::filesystem::create_directory(tree_root / "empty");
        std::ofstream(tree_root / "alpha.txt") << "alpha";
        std::ofstream(tree_root / "nested" / "beta.txt") << "beta";
        WIN32_FILE_ATTRIBUTE_DATA tree_attributes{};
        GetFileAttributesExW(tree_root.c_str(), GetFileExInfoStandard, &tree_attributes);
        const uint64_t tree_modified = (static_cast<uint64_t>(tree_attributes.ftLastWriteTime.dwHighDateTime) << 32) |
            tree_attributes.ftLastWriteTime.dwLowDateTime;
        Request tree_request;
        tree_request.directory_tree = true;
        tree_request.tree_source = tree_root.wstring();
        tree_request.tree_source_modified = tree_modified;
        tree_request.target = (root / "target").wstring();
        const auto tree_prepared = Prepare(tree_request, *principal, registry, store, cancelled);
        auto tree_plan = tree_prepared ? store.Read(*tree_prepared.plan_id, *principal)
                                       : transfer_plan::Result<transfer_plan::Store::FrozenPlan>{};
        Check(tree_plan && (*tree_plan.value)->transfer.directory_tree,
              "directory tree preparation publishes a frozen tree plan");
        Check(tree_plan && (*tree_plan.value)->transfer.tree_directories.size() == 3,
              "directory tree preparation freezes root, nested and empty directories");
        Check(tree_plan && (*tree_plan.value)->transfer.sources.size() == 2 &&
              (*tree_plan.value)->transfer.sources[1].relative_destination == L"nested\\beta.txt",
              "directory tree preparation freezes relative file destinations in stable order");
        Check(!fs::exists(root / "target" / "tree") && fs::exists(tree_root / "nested" / "beta.txt"),
              "tree preparation performs no destination mutation");
        tree_request.tree_source_modified++;
        const auto stale_tree = Prepare(tree_request, *principal, registry, store, cancelled);
        Check(!stale_tree && stale_tree.filesystem_error == ERROR_FILE_INVALID,
              "directory tree root modified after the snapshot is rejected");
        request.sources = {source.wstring()};
        cancelled = true;
        Check(Prepare(request, *principal, registry, store, cancelled).error == Error::Cancelled, "cancelled preparation publishes no plan");
        cancelled = false;
        registry.UnregisterOwner(*owner);
        Check(Prepare(request, *principal, registry, store, cancelled).error == Error::Revoked, "closed owner cannot prepare plan");
        Check(!store.Read(*prepared.plan_id, *principal), "closed owner revokes previously prepared plan");
        Check(fs::exists(source) && fs::is_empty(root / "target"), "all preparation cases leave source and destination untouched");
        // root is generated beneath this test's bench_data directory only.
        fs::remove_all(root);
    } catch (const std::exception& error) {
        std::printf("[FAIL] %s\n", error.what());
        return 1;
    }
    return failures ? 1 : 0;
}
