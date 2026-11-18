#include "../common/localization.h"
#include "../common/rename_identity.h"
#include "../ops/ops_manager.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
using namespace pulse::ops;
int main() {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    auto check = [&](bool ok, const char *label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const auto root = std::filesystem::absolute(
        std::filesystem::path(L"bench_data") /
        (L"extension-rename-ops-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root.parent_path());
    if (!std::filesystem::create_directory(root))
        return 2;
    auto write = [&](const wchar_t *name) { std::ofstream(root / name) << "isolated caller-bound rename"; };
    std::mutex result_mutex;
    std::optional<ControlledTransferResult> last_result;
    auto request = [&](const wchar_t *a, const wchar_t *b, const wchar_t *x, const wchar_t *y) {
        OpRequest result;
        result.type = OpType::BatchRename;
        result.sources = {(root / a).wstring(), (root / b).wstring()};
        result.new_names = {x, y};
        for (const auto &path : result.sources) {
            RenameIdentity identity;
            check(CaptureRenameIdentity(path, identity) == ERROR_SUCCESS,
                  "capture per-item source and parent identity");
            result.authorized_rename_identities.push_back(identity);
        }
        result.rename_authorized = [] { return true; };
        result.rename_completed = [&](ControlledTransferResult outcome) {
            std::lock_guard lock(result_mutex);
            last_result = std::move(outcome);
        };
        return result;
    };
    OpsManager ops;
    ops.Start([] {});
    auto wait = [&](uint64_t count) {
        const auto end = GetTickCount64() + 15000;
        while (GetTickCount64() < end) {
            const auto status = ops.Status();
            if (!status.active && status.completed_ops > count)
                return true;
            Sleep(5);
        }
        return false;
    };
    auto run = [&](OpRequest value) {
        {
            std::lock_guard lock(result_mutex);
            last_result.reset();
        }
        const auto count = ops.Status().completed_ops;
        if (!ops.Submit(std::move(value)) || !wait(count))
            return false;
        const auto end = GetTickCount64() + 15000;
        while (GetTickCount64() < end) {
            {
                std::lock_guard lock(result_mutex);
                if (last_result)
                    return true;
            }
            Sleep(5);
        }
        return false;
    };
    write(L"a");
    write(L"b");
    check(run(request(L"a", L"b", L"x", L"y")) && std::filesystem::exists(root / L"x") &&
              std::filesystem::exists(root / L"y"),
          "distinct confirmed identities rename both matching objects");
    const auto before_undo = ops.Status().completed_ops;
    ops.Undo();
    check(wait(before_undo) && std::filesystem::exists(root / L"a") && std::filesystem::exists(root / L"b"),
          "normal user undo is not coupled to retired extension authority");
    auto cancelled = request(L"a", L"b", L"x", L"y");
    cancelled.rename_authorized = [] { return false; };
    check(run(std::move(cancelled)) && std::filesystem::exists(root / L"a") && !std::filesystem::exists(root / L"x"),
          "revoked queued request does not mutate files");
    auto replaced = request(L"a", L"b", L"x", L"y");
    std::filesystem::rename(root / L"a", root / L"old-a");
    write(L"a");
    check(run(std::move(replaced)) && std::filesystem::exists(root / L"a") && std::filesystem::exists(root / L"b") &&
              !std::filesystem::exists(root / L"x") && !std::filesystem::exists(root / L"y"),
          "replacement identity fails and stops later batch items");
    auto missing = request(L"a", L"b", L"x", L"y");
    missing.authorized_rename_identities.pop_back();
    check(run(std::move(missing)) && std::filesystem::exists(root / L"a") && !std::filesystem::exists(root / L"x"),
          "incomplete identity vector fails closed before execution");
    auto partial = request(L"a", L"b", L"x", L"y");
    std::atomic<unsigned> checks = 0;
    partial.rename_authorized = [&] { return ++checks <= 2; };
    check(run(std::move(partial)) && std::filesystem::exists(root / L"x") && std::filesystem::exists(root / L"b") &&
              !std::filesystem::exists(root / L"y"),
          "authority loss between items preserves completed work and cancels remaining work");
    {
        std::lock_guard lock(result_mutex);
        check(last_result && !last_result->complete && last_result->items.size() == 2 &&
                  last_result->items[0].state == ControlledTransferState::Moved &&
                  last_result->items[1].state == ControlledTransferState::Cancelled,
              "task completion preserves exact committed and cancelled item outcomes");
    }
    ops.Stop();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    check(!error && !std::filesystem::exists(root), "remove isolated operation fixture");
    return failures ? 1 : 0;
}
