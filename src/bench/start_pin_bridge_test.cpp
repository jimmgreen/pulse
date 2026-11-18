#include "../ipc/shell_client.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || wcscmp(argv[1], L"--live") != 0) return 2;
    std::mutex mutex;
    std::condition_variable changed;
    std::map<uint32_t, std::vector<pulse::ipc::CtxMenuItem>> menus;
    std::map<uint32_t, uint32_t> results;
    pulse::ipc::ShellClient::Callbacks callbacks;
    callbacks.ctx_items = [&](uint32_t id, std::vector<pulse::ipc::CtxMenuItem> items, bool partial,
                              pulse::ipc::CtxItemsExtra) {
        if (partial) return;
        std::lock_guard lock(mutex); menus[id] = std::move(items); changed.notify_all();
    };
    callbacks.done = [&](uint32_t id, uint32_t hr, bool, std::wstring) {
        std::lock_guard lock(mutex); results[id] = hr; changed.notify_all();
    };
    auto& client = pulse::ipc::ShellClient::Instance(); client.Start(std::move(callbacks));
    auto query = [&](const wchar_t* verb, bool invoke) {
        const auto session = client.QueryContextMenu({argv[2]}, 0, false, false);
        pulse::ipc::CtxMenuItem found;
        {
            std::unique_lock lock(mutex);
            if (!session || !changed.wait_for(lock, std::chrono::seconds(20), [&] { return menus.count(session) != 0; })) return false;
            for (const auto& item : menus[session]) if (item.enabled && !item.has_children && !_wcsicmp(item.verb.c_str(), verb)) found = item;
        }
        if (!found.id || !invoke) { client.CloseContextMenu(session); return found.id != 0; }
        const auto id = client.InvokeContextMenu(session, found.id, found.verb, found.text);
        std::unique_lock lock(mutex);
        if (!id || !changed.wait_for(lock, std::chrono::seconds(25), [&] { return results.count(id) != 0; })) return false;
        printf("invoke %ls hr=%08x\n", verb, results[id]);
        return results[id] == S_OK;
    };
    // Caller must provide its isolated, initially unpinned fixture.
    const bool initial = query(L"PinToStartScreen", false);
    const bool pinned = initial && query(L"PinToStartScreen", true);
    const bool confirmed = pinned && query(L"UnpinFromStartScreen", false);
    const bool unpinned = initial && query(L"UnpinFromStartScreen", true);
    const bool restored = unpinned && query(L"PinToStartScreen", false);
    printf("[%s] menu pin / verify / unpin / verify (initial=%d pinned=%d confirmed=%d unpinned=%d restored=%d)\n",
           initial && pinned && confirmed && unpinned && restored ? "PASS" : "FAIL", initial, pinned, confirmed, unpinned, restored);
    client.Stop();
    return initial && pinned && confirmed && unpinned && restored ? 0 : 1;
}
