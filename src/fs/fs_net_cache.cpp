// fs_net_cache.cpp — Disk snapshots for UNC folders and a timed connectivity probe.
#include "fs_net_cache.h"
#include <shlobj.h>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

namespace pulse::fs {

namespace {

std::wstring CacheDir() {
    wchar_t path[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) return L"";
    std::wstring dir = std::wstring(path) + L"\\Pulse";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\netcache";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

uint64_t HashPath(const std::wstring& p) {
    uint64_t h = 14695981039346656037ull;
    for (wchar_t c : p) {
        h ^= static_cast<uint16_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring CacheFile(const std::wstring& path) {
    std::wstring dir = CacheDir();
    if (dir.empty()) return L"";
    wchar_t name[32];
    swprintf_s(name, L"%016llX.bin", HashPath(NormalizePath(path)));
    return dir + L"\\" + name;
}

uint64_t NowUnix() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

bool SaveNetSnapshot(const std::wstring& path, const SnapshotPtr& snapshot) {
    if (!snapshot || !IsUncPath(path)) return false;
    const std::wstring file = CacheFile(path);
    if (file.empty()) return false;
    const std::wstring tmp = file + L".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write("PNCH", 4);
    uint32_t ver = 1;
    uint64_t ts = NowUnix();
    uint32_t count = static_cast<uint32_t>(snapshot->size());
    f.write(reinterpret_cast<const char*>(&ver), 4);
    f.write(reinterpret_cast<const char*>(&ts), 8);
    f.write(reinterpret_cast<const char*>(&count), 4);
    for (const auto& e : *snapshot) {
        uint8_t flags = (e.is_dir ? 1 : 0) | (e.is_reparse ? 2 : 0);
        uint64_t mtime = (static_cast<uint64_t>(e.mtime.dwHighDateTime) << 32) | e.mtime.dwLowDateTime;
        uint32_t nlen = static_cast<uint32_t>(e.name.size());
        f.write(reinterpret_cast<const char*>(&flags), 1);
        f.write(reinterpret_cast<const char*>(&e.attrs), 4);
        f.write(reinterpret_cast<const char*>(&e.size), 8);
        f.write(reinterpret_cast<const char*>(&mtime), 8);
        f.write(reinterpret_cast<const char*>(&nlen), 4);
        f.write(reinterpret_cast<const char*>(e.name.data()), nlen * sizeof(wchar_t));
    }
    f.close();
    if (!f) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), file.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

SnapshotPtr LoadNetSnapshot(const std::wstring& path, uint64_t* unix_sec) {
    if (!IsUncPath(path)) return nullptr;
    const std::wstring file = CacheFile(path);
    if (file.empty()) return nullptr;
    std::ifstream f(file, std::ios::binary);
    if (!f) return nullptr;
    char magic[4]{};
    f.read(magic, 4);
    if (std::string(magic, 4) != "PNCH") return nullptr;
    uint32_t ver = 0, count = 0;
    uint64_t ts = 0;
    f.read(reinterpret_cast<char*>(&ver), 4);
    f.read(reinterpret_cast<char*>(&ts), 8);
    f.read(reinterpret_cast<char*>(&count), 4);
    if (ver != 1 || count > 500000) return nullptr;
    auto entries = std::make_shared<std::vector<DirEntry>>();
    entries->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t flags = 0;
        uint32_t attrs = 0, nlen = 0;
        uint64_t size = 0, mtime = 0;
        f.read(reinterpret_cast<char*>(&flags), 1);
        f.read(reinterpret_cast<char*>(&attrs), 4);
        f.read(reinterpret_cast<char*>(&size), 8);
        f.read(reinterpret_cast<char*>(&mtime), 8);
        f.read(reinterpret_cast<char*>(&nlen), 4);
        if (!f || nlen > 1024) return nullptr;
        DirEntry e;
        e.name.assign(nlen, L'\0');
        f.read(reinterpret_cast<char*>(e.name.data()), nlen * sizeof(wchar_t));
        e.attrs = attrs;
        e.size = size;
        e.mtime.dwLowDateTime = static_cast<DWORD>(mtime);
        e.mtime.dwHighDateTime = static_cast<DWORD>(mtime >> 32);
        e.is_dir = (flags & 1) != 0;
        e.is_reparse = (flags & 2) != 0;
        entries->push_back(std::move(e));
    }
    if (unix_sec) *unix_sec = ts;
    return entries;
}

std::wstring FormatCacheAge(uint64_t unix_sec) {
    if (unix_sec == 0) return L"刚才";
    const uint64_t now = NowUnix();
    if (now <= unix_sec) return L"刚才";
    const uint64_t sec = now - unix_sec;
    if (sec < 60) return std::to_wstring(sec) + L" 秒前";
    if (sec < 3600) return std::to_wstring(sec / 60) + L" 分钟前";
    if (sec < 86400) return std::to_wstring(sec / 3600) + L" 小时前";
    return std::to_wstring(sec / 86400) + L" 天前";
}

namespace {

struct ProbeJob {
    std::wstring unc;
    HWND hwnd = nullptr;
    UINT msg = 0;
    HANDLE done = nullptr;
    BOOL ok = FALSE;
    DWORD rtt_ms = 0;
};

DWORD WINAPI ProbeInner(LPVOID param) {
    auto* j = static_cast<ProbeJob*>(param);
    const ULONGLONG t0 = GetTickCount64();
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    j->ok = GetFileAttributesExW(j->unc.c_str(), GetFileExInfoStandard, &fad);
    j->rtt_ms = static_cast<DWORD>(GetTickCount64() - t0);
    SetEvent(j->done);
    return 0;
}

} // namespace

void StartUncProbe(HWND hwnd, UINT msg, std::wstring unc) {
    if (!hwnd || unc.empty()) return;
    auto* job = new ProbeJob{};
    job->unc = std::move(unc);
    job->hwnd = hwnd;
    job->msg = msg;
    job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, ProbeInner, job, 0, nullptr);
    if (!thread) {
        CloseHandle(job->done);
        delete job;
        return;
    }
    std::thread([job, thread]() {
        const DWORD wait = WaitForSingleObject(job->done, 1500);
        auto* result = new UncProbeResult{};
        result->unc = job->unc;
        result->rtt_ms = job->rtt_ms;
        if (wait != WAIT_OBJECT_0) result->status = NetStatus::Offline;
        else if (!job->ok) result->status = NetStatus::Offline;
        else result->status = job->rtt_ms > 800 ? NetStatus::Slow : NetStatus::Online;
        if (!PostMessageW(job->hwnd, job->msg, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        CloseHandle(job->done);
        delete job;
    }).detach();
}

} // namespace pulse::fs
