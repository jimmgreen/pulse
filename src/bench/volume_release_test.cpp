#include "../app/folder_sizes.h"
#include "../fs/fs_watch.h"
#include <filesystem>
#include <iostream>
#include <chrono>
#include <thread>
#include <stdexcept>

int main() {
    using namespace std::chrono_literals;
    const auto base = std::filesystem::temp_directory_path() / (L"pulse-release-" + std::to_wstring(GetCurrentProcessId()));
    const auto target = base / L"target";
    const auto other = base / L"other";
    std::filesystem::create_directories(target / L"child");
    std::filesystem::create_directories(other);
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; if (!ok) ++failures; };
    const auto completed = [](const std::shared_future<void>& future) {
        if (!future.valid() || future.wait_for(5s) != std::future_status::ready) return false;
        try { future.get(); return true; }
        catch (...) { return false; }
    };
    std::promise<void> failed;
    auto failed_future = failed.get_future().share();
    failed.set_exception(std::make_exception_ptr(std::runtime_error("release failed")));
    check(!completed(failed_future), "ready exceptional release is rejected");
    const auto wait = [](auto predicate) {
        const auto end = GetTickCount64() + 5000;
        while (!predicate() && GetTickCount64() < end) std::this_thread::sleep_for(10ms);
        return predicate();
    };
    pulse::fs::DirWatchSet watches;
    auto sync = [&] { watches.Sync({target.wstring(), other.wstring()}, [](const auto&, bool, auto) {}); };
    sync();
    check(wait([&] { return watches.Armed(target.wstring()) && watches.Armed(other.wstring()); }), "both watches armed");
    auto released = watches.SuspendVolume(target.wstring());
    check(released.size() == 1 && completed(released[0]), "target handle closed");
    sync();
    check(!watches.Armed(target.wstring()) && watches.Armed(other.wstring()), "Sync preserves suspension and unrelated watch");
    watches.ResumeVolume(target.wstring()); sync();
    check(wait([&] { return watches.Armed(target.wstring()); }), "watch resumes");
    watches.Sync({other.wstring()}, [](const auto&, bool, auto) {});
    auto retired = watches.SuspendVolume(target.wstring());
    check(retired.size() == 1 && completed(retired[0]), "rapid navigation waits for retired target watch");
    check(watches.Armed(other.wstring()), "rapid navigation preserves unrelated watch");
    watches.ResumeVolume(target.wstring()); sync();
    pulse::app::FolderSizes sizes;
    sizes.SetIndexEnabled(false);
    sizes.Sync({{(target / L"child").wstring(), true, true}}, {target.wstring()});
    check(wait([&] { return sizes.ReadStats().active_watches == 1; }), "size watch starts");
    auto done = sizes.SuspendVolume(target.wstring());
    check(completed(done), "size resources released");
    sizes.Sync({{(target / L"child").wstring(), true, true}}, {target.wstring()});
    sizes.Calculate((target / L"child").wstring());
    std::this_thread::sleep_for(100ms);
    check(sizes.ReadStats().active_watches == 0, "size suspension survives requests");
    sizes.ResumeVolume(target.wstring());
    check(wait([&] { return sizes.ReadStats().active_watches == 1; }), "size watches resume");
    check(completed(sizes.SuspendVolume(base.wstring())), "size cleanup release completes");
    sizes.Stop();
    for (auto& future : watches.SuspendVolume(base.wstring())) check(completed(future), "watch cleanup release completes");
    std::error_code error;
    std::filesystem::remove_all(base, error);
    check(!error && !std::filesystem::exists(base), "isolated fixtures removed");
    return failures ? 1 : 0;
}
