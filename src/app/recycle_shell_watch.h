#pragma once
#include <windows.h>
#include <shlobj.h>
#include <atomic>

namespace pulse::app {
// Registration is prepared on the I/O worker; cancellation does not join that worker.
class RecycleShellWatch {
public:
    const HWND owner;
    explicit RecycleShellWatch(HWND hwnd) : owner(hwnd) {}
    ~RecycleShellWatch() { Cancel(); }
    void Cancel() {
        cancelled_ = true;
        if (const ULONG id = registration_.exchange(0)) SHChangeNotifyDeregister(id);
    }
    bool Active() const { return registration_.load() != 0; }
    void Register(UINT message, PCIDLIST_ABSOLUTE root = nullptr) {
        if (cancelled_ || Active()) return;
        PIDLIST_ABSOLUTE recycle = nullptr;
        if (!root) {
            if (FAILED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &recycle))) return;
            root = recycle;
        }
        SHChangeNotifyEntry entry{root, TRUE};
        const ULONG id = SHChangeNotifyRegister(owner, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
            SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEITEM |
                SHCNE_RENAMEFOLDER | SHCNE_UPDATEDIR | SHCNE_UPDATEITEM,
            message, 1, &entry);
        if (recycle) CoTaskMemFree(recycle);
        registration_ = id;
        if (cancelled_) Cancel();
    }
private:
    std::atomic<ULONG> registration_{0};
    std::atomic<bool> cancelled_{false};
};
} // namespace pulse::app
