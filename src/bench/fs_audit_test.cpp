#include <windows.h>
#include <atomic>
#include <thread>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <cwctype>
static std::atomic<bool> short_read{false}, fallback_block{false};
static HANDLE entered, release_io;
static BOOL WINAPI TestRead(HANDLE h, LPVOID b, DWORD n, LPDWORD done, LPOVERLAPPED o) {
    return ReadFile(h,b,short_read ? n / 2 : n,done,o);
}
#define ReadFile TestRead
#include "../fs/recycle_metadata.h"
#undef ReadFile
static HANDLE WINAPI TestFind(LPCWSTR p, FINDEX_INFO_LEVELS level, LPVOID data,
                              FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
    if (fallback_block) { SetEvent(entered); WaitForSingleObject(release_io,INFINITE); }
    return FindFirstFileExW(p,level,data,search,filter,flags);
}
// This PC media queries: one drive is held to stand in for a disconnected
// mapped drive or an empty card reader.
static std::atomic<wchar_t> stalled_drive{0};
static std::atomic<unsigned> stalled_queries{0};
static HANDLE release_drive;
static BOOL WINAPI TestVolume(LPCWSTR root, LPWSTR name, DWORD name_size, LPDWORD serial,
                              LPDWORD max_component, LPDWORD flags, LPWSTR fs_name, DWORD fs_size) {
    if (root && stalled_drive.load() != 0 && root[0] == stalled_drive.load()) {
        ++stalled_queries;
        WaitForSingleObject(release_drive, INFINITE);
    }
    return GetVolumeInformationW(root, name, name_size, serial, max_component, flags, fs_name, fs_size);
}
#define FindFirstFileExW TestFind
#define GetVolumeInformationW TestVolume
#include "../fs/fs_enum.cpp"
#undef GetVolumeInformationW
#undef FindFirstFileExW
#include "../fs/fs_recycle.cpp"
static std::atomic<DWORD> watch_error{0};
static std::atomic<unsigned> arms{0};
static BOOL WINAPI TestChanges(HANDLE h, LPVOID b, DWORD n, BOOL tree, DWORD filter,
                               LPDWORD bytes, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE cb) {
    ++arms;
    if (DWORD error=watch_error.load()) { SetLastError(error); return FALSE; }
    return ReadDirectoryChangesW(h,b,n,tree,filter,bytes,ov,cb);
}
#define ReadDirectoryChangesW TestChanges
#include "../fs/fs_watch.cpp"
#undef ReadDirectoryChangesW
#include "../app/app_worker.cpp"
// Isolate enumeration/worker lifetime from unrelated cache, link, sort and
// logging services. The production WorkerPool and enumeration run unchanged.
namespace pulse::diagnostics::runtime {
void Event(const char*, std::initializer_list<Field>, Level) noexcept {}
}
namespace pulse::app {
std::wstring FindGitRoot(const std::wstring&) { return {}; }
void ResolveLinksInPlace(const std::wstring&,std::vector<fs::DirEntry>&,const std::function<bool()>&) {}
ScopedEntryGrouping::ScopedEntryGrouping(int,const std::wstring&) {}
ScopedEntryGrouping::~ScopedEntryGrouping() = default;
bool EntryLess(const fs::DirEntry& a,const fs::DirEntry& b,ui::SortColumn,ui::SortDirection) {return a.name<b.name;}
void SortEntriesBySize(std::vector<fs::DirEntry>&,ui::SortDirection,const FolderSizeLookup&,const std::function<void()>&) {}
void SortEntries(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir, const std::function<void()>& tick) {
    std::sort(entries.begin(), entries.end(), [&](const fs::DirEntry& a, const fs::DirEntry& b) { if (tick) tick(); return EntryLess(a, b, col, dir); });
}
}
namespace pulse::fs {
NetSnapshotWriteTicket BeginNetSnapshotWrite(const std::wstring&) {return {};}
bool SaveNetSnapshot(const NetSnapshotWrite&,const SnapshotPtr&) {return true;}
bool QueryDirectoryIdentity(const std::wstring&,DirectoryIdentity&) {return false;}
}
namespace pulse::diagnostics::runtime { void Event(const char*,std::initializer_list<Field>) noexcept {} }
using namespace pulse::fs;
static NtCreateFile_t real_open;
static NtQueryDirectoryFile_t real_query;
static std::atomic<int> open_mode{0};
static NTSTATUS NTAPI TestOpen(PHANDLE h, ACCESS_MASK access, NtObjectAttributes* attr,
    NtIoStatusBlock* iosb, PLARGE_INTEGER size, ULONG flags, ULONG share, ULONG disposition,
    ULONG options, PVOID ea, ULONG ea_size) {
    const int mode=open_mode.load();
    if (mode==1) { SetEvent(entered); WaitForSingleObject(release_io,INFINITE); return static_cast<NTSTATUS>(0xc0000001); }
    if (mode==2) return static_cast<NTSTATUS>(0xc0000001);
    return real_open(h,access,attr,iosb,size,flags,share,disposition,options,ea,ea_size);
}
static std::atomic<bool> pending{false};
static NtIoStatusBlock* pending_status;
static HANDLE pending_event;
static NTSTATUS NTAPI TestQuery(HANDLE h,HANDLE event,NtPioApcRoutine apc,PVOID ctx,
    NtIoStatusBlock* iosb,PVOID buffer,ULONG length,NtFileInformationClass info,
    BOOLEAN one,NtUnicodeString* name,BOOLEAN restart) {
    if (pending) { pending_status=iosb;pending_event=event;SetEvent(entered);return STATUS_PENDING; }
    return real_query(h,event,apc,ctx,iosb,buffer,length,info,one,name,restart);
}
static int failures;
static void Check(bool ok,const char* label) { printf("[%s] %s\n",ok?"PASS":"FAIL",label);failures+=!ok; }
static bool Drain() {
    for(int i=0;i<300 && active_directory_requests.load();++i) Sleep(10);
    return active_directory_requests==0;
}
static std::vector<BYTE> Metadata(uint64_t version,const std::wstring& path) {
    uint32_t count=static_cast<uint32_t>(path.size()+1);
    std::vector<BYTE> data(version==2?28+count*2:24+260*2);
    memcpy(data.data(),&version,8);
    if(version==2) memcpy(data.data()+24,&count,4);
    memcpy(data.data()+(version==2?28:24),path.c_str(),count*2);
    return data;
}
int main() {
    const auto dir=std::filesystem::absolute(L"bench_data/fs-audit-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir / L"sub");
    const auto file=dir / L"中文-é-😀.txt";
    HANDLE h=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
    DWORD wrote=0;WriteFile(h,"test",4,&wrote,nullptr);CloseHandle(h);
    InitNtApi();real_open=g_NtCreateFile;real_query=g_NtQueryDirectoryFile;
    g_NtCreateFile=TestOpen;g_NtQueryDirectoryFile=TestQuery;
    entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);release_io=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::vector<DirEntry> out;
    EnumerationOptions opts;opts.timeout_ms=100;
    EnumerateDirectory(dir.wstring(),out,opts);
    Check(out.size()==2,"M03-001 actual local enumeration succeeds");
    for(int mode=0;mode<3;++mode) {
        ResetEvent(entered);ResetEvent(release_io);
        open_mode=mode==0?1:mode==2?2:0;pending=mode==1;fallback_block=mode==2;
        std::atomic<bool> cancel{false};std::atomic<unsigned> callback_calls{0};bool threw=false;
        opts.cancelled=[&]{++callback_calls;return cancel.load();};opts.timeout_ms=3000;
        std::thread caller([&]{try {EnumerateDirectory(dir.wstring(),out,opts);}catch(...){threw=true;}});
        Check(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0,"M03-001 injected provider entered");
        const auto start=GetTickCount64();cancel=true;caller.join();
        Check(threw && out.empty() && GetTickCount64()-start<300,"M03-001 cancel returns without partial snapshot within 300ms");
        const auto calls=callback_calls.load();opts.cancelled={};
        open_mode=0;fallback_block=false;pending=false;
        std::vector<DirEntry> fresh;EnumerateDirectory(dir.wstring(),fresh);
        Check(fresh.size()==2,"M03-001 new navigation works while old provider remains blocked");
        if(mode==1) {pending_status->Status=STATUS_NO_MORE_FILES;SetEvent(pending_event);}
        else SetEvent(release_io);
        Check(Drain() && callback_calls==calls,"M03-001 retired worker drains without touching caller callback");
    }
    for(int mode=0;mode<3;++mode) {
        ResetEvent(entered);ResetEvent(release_io);
        open_mode=mode==0?1:mode==2?2:0;pending=mode==1;fallback_block=mode==2;
        std::atomic<unsigned> callbacks{0};
        {
            pulse::app::WorkerPool pool;pool.Start([&](auto){++callbacks;});
            pool.Refresh(dir.wstring(),pulse::ui::SortColumn::Name,pulse::ui::SortDirection::Asc);
            Check(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0,"M03-001 production WorkerPool reaches injected block");
            const auto stopped=GetTickCount64();pool.Stop();
            const auto elapsed=GetTickCount64()-stopped;
            printf("[INFO] WorkerPool Stop mode=%d elapsed_ms=%llu\n",mode,elapsed);
            Check(elapsed<300 && callbacks==0,"M03-001 production Stop joins within 300ms without stale callback");
        }
        open_mode=0;fallback_block=false;pending=false;
        if(mode==1) {pending_status->Status=STATUS_NO_MORE_FILES;SetEvent(pending_event);}
        else SetEvent(release_io);
        Check(Drain() && callbacks==0,"M03-001 late completion after WorkerPool destruction is safe");
    }
    ResetEvent(entered);ResetEvent(release_io);open_mode=1;
    std::atomic<unsigned> published{0};
    {
        pulse::app::WorkerPool pool;pool.Start([&](auto){++published;});
        const auto generation=pool.Refresh(dir.wstring(),pulse::ui::SortColumn::Name,pulse::ui::SortDirection::Asc);
        Check(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0,"M03-001 replaced navigation reaches blocked open");
        pool.CancelGeneration(generation);open_mode=0;
        pool.Refresh(dir.wstring(),pulse::ui::SortColumn::Name,pulse::ui::SortDirection::Asc);
        for(int i=0;i<100 && !published.load();++i) Sleep(5);
        Check(published==1,"M03-001 replacement navigation publishes while old request is blocked");
        pool.Stop();
    }
    SetEvent(release_io);Check(Drain() && published==1,"M03-001 cancelled navigation never publishes its late snapshot");
    ResetEvent(release_io);open_mode=1;opts.timeout_ms=35;
    for(int i=0;i<16;++i) {try {EnumerateDirectory(L"\\\\?\\UNC\\test\\share",out,opts);}catch(...) {}}
    auto start=GetTickCount64();bool limited=false;
    try {EnumerateDirectory(L"\\\\?\\UNC\\test\\share",out,opts);}catch(...){limited=true;}
    Check(limited && active_remote_directory_requests==16 && GetTickCount64()-start<30,"M03-001 blocked remote providers have a fixed 16 worker bound");
    open_mode=0;EnumerateDirectory(dir.wstring(),out,opts);
    Check(out.size()==2,"M03-001 remote retirement limit reserves local capacity");
    SetEvent(release_io);Check(Drain(),"M03-001 all capped providers retire after late completion");
    watch_error=ERROR_INVALID_FUNCTION;arms=0;std::atomic<unsigned> notices{0};
    {
        DirWatch watch;watch.Start(dir.wstring(),[&](bool,auto){++notices;});Sleep(350);
        Check(arms==1 && notices==1,"M03-002 unsupported watcher does not spin or repeatedly notify");
        start=GetTickCount64();watch.Stop();Check(GetTickCount64()-start<200,"M03-002 stop interrupts unsupported backoff");
    }
    for(int i=0;i<300 && active_watchers.load();++i) Sleep(10);
    Check(active_watchers==0,"M03-002 watcher state released after backoff stop");
    watch_error=ERROR_ACCESS_DENIED;arms=0;notices=0;
    std::atomic<unsigned> changes{0};
    {
        DirWatch watch;watch.Start(dir.wstring(),[&](bool overflow,auto){if(overflow) ++notices;else ++changes;});
        for(int i=0;i<100 && !arms.load();++i) Sleep(5);
        Sleep(80);Check(arms==1,"M03-002 transient error backs off before retry");
        watch_error=0;
        for(int i=0;i<200 && !watch.Armed();++i) Sleep(10);
        Check(watch.Armed() && notices>=2,"M03-002 successful rearm reconciles changes missed during error");
        h=CreateFileW((dir/L"after-watch.txt").c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);CloseHandle(h);
        for(int i=0;i<200 && !changes.load();++i) Sleep(10);
        Check(changes>0,"M03-002 real file changes delivered after transient recovery");
        watch.Stop();
    }
    for(int i=0;i<300 && active_watchers.load();++i) Sleep(10);
    struct Notice { std::wstring path;bool overflow;std::vector<DirNotifyEvent> events;};
    std::vector<Notice> queue{{L"a",false,{}},{L"b",false,{}},{L"a",false,{}}};
    for(int i=0;i<1000;++i) QueueDirectoryNotification(queue,L"a",true,{});
    Check(queue.size()==2 && queue.back().overflow,"M03-002 overflow coalesces all pending details by path");
    RecycleMetadata parsed;
    for(uint64_t version:{1ull,2ull}) {
        auto bytes=Metadata(version,file.wstring());
        Check(ParseRecycleMetadata(bytes.data(),bytes.size(),parsed) && parsed.original_path==file.wstring(),"M03-003 valid v1/v2 metadata preserves exact path");
        bool rejected=true;
        for(size_t n=0;n<bytes.size();++n) rejected &= !ParseRecycleMetadata(bytes.data(),n,parsed);
        Check(rejected,"M03-003 every byte truncation rejected");
        const auto index=dir / L"$Ifixture";
        h=CreateFileW(index.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
        WriteFile(h,bytes.data(),static_cast<DWORD>(bytes.size()),&wrote,nullptr);CloseHandle(h);
        Check(ReadRecycleMetadata(index.wstring(),parsed),"M03-003 real isolated metadata file reads successfully");
        short_read=true;Check(!ReadRecycleMetadata(index.wstring(),parsed),"M03-003 short read rejected");short_read=false;
    }
    auto malformed=Metadata(2,L"C:\\plausible.txt");uint32_t over=32768;memcpy(malformed.data()+24,&over,4);
    Check(!ParseRecycleMetadata(malformed.data(),malformed.size(),parsed),"M03-003 declared count cannot be clamped to plausible truncated path");
    auto relative=Metadata(2,L"relative.txt");
    Check(!ParseRecycleMetadata(relative.data(),relative.size(),parsed),"M03-003 relative restore destination rejected");
    const auto index=dir/L"$Irestore", payload=dir/L"$Rrestore", dest=dir/L"restored.txt";
    auto write_file=[](const std::filesystem::path& path,const std::vector<BYTE>& bytes) {
        HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
        DWORD done=0;WriteFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&done,nullptr);CloseHandle(f);
    };
    auto valid=Metadata(2,dest.wstring());write_file(index,valid);
    RecycleItem item;
    Check(!ReadRecycleIndex(index.wstring(),item),"M03-003 orphan index is not a restorable item");
    write_file(payload,{1,2,3});write_file(index,malformed);
    Check(!ReadRecycleIndex(index.wstring(),item) && RestoreRecyclePayload(index.wstring(),payload.wstring())==RecycleRestoreResult::InvalidMetadata &&
        std::filesystem::exists(index) && std::filesystem::exists(payload) && !std::filesystem::exists(dest),
        "M03-003 display and actual restore reject corrupt metadata without moving or deleting files");
    write_file(index,valid);write_file(dest,{9});
    Check(RestoreRecyclePayload(index.wstring(),payload.wstring())==RecycleRestoreResult::MoveFailed &&
        std::filesystem::exists(index) && std::filesystem::file_size(dest)==1 && std::filesystem::file_size(payload)==3,
        "M03-003 existing destination is preserved with payload and index intact");
    DeleteFileW(dest.c_str());
    Check(ReadRecycleIndex(index.wstring(),item) && RestoreRecyclePayload(index.wstring(),payload.wstring())==RecycleRestoreResult::Restored &&
        std::filesystem::file_size(dest)==3 && !std::filesystem::exists(index) && !std::filesystem::exists(payload),
        "M03-003 valid actual restore moves exact payload then deletes index");
    const auto saved=std::filesystem::current_path();std::filesystem::current_path(dir);
    Check(NormalizePath(L"sub\\..\\"+file.filename().wstring())==NormalizePath(file.wstring()),"M03-004 relative dot segments resolve before extended prefix");
    Check(NormalizePath(dir.root_name().wstring()+L"sub\\..\\"+file.filename().wstring())==NormalizePath(file.wstring()),"M03-004 drive-relative input uses current directory");
    Check(NormalizePath(dir.root_name().wstring())==L"\\\\?\\"+dir.root_name().wstring()+L"\\","M03-004 bare drive preserves root convention");
    Check(NormalizePath(L"\\\\server\\share\\folder\\..\\file")==L"\\\\?\\UNC\\server\\share\\file","M03-004 ordinary UNC dot segments normalized lexically");
    const auto literal=L"\\\\?\\"+dir.wstring()+L"\\literal. ";
    Check(CreateDirectoryW(literal.c_str(),nullptr) && NormalizePath(literal)==literal && RemoveDirectoryW(literal.c_str()),"M03-004 extended literal trailing characters preserved on disk");
    std::filesystem::current_path(saved);
    alignas(NtFileFullDirInformation) BYTE record[128]{};
    auto* info=reinterpret_cast<NtFileFullDirInformation*>(record);info->EaSize=0xa0000003;
    const std::wstring name=L"中文-é";info->FileNameLength=static_cast<ULONG>(name.size()*2);memcpy(info->FileName,name.data(),name.size()*2);
    ValidateNtDirectoryRecord(info,sizeof(record));
    Check(std::wstring(info->FileName,info->FileNameLength/2)==name && info->EaSize==0xa0000003,"M03-008 shared NT ABI preserves nonzero EA/tag and Unicode filename");
    g_NtCreateFile=real_open;g_NtQueryDirectoryFile=real_query;
    {
        // One stalled drive used to time out the whole This PC listing
        // ("目录不可用") while every drive still opened on its own.
        wchar_t windows_dir[MAX_PATH]{};
        GetWindowsDirectoryW(windows_dir, MAX_PATH);
        const wchar_t letter = static_cast<wchar_t>(towupper(windows_dir[0]));
        const std::wstring root = NormalizePath(std::wstring{letter, L':', L'\\'});
        const std::wstring suffix = std::wstring(L" (") + letter + L":)";
        auto list = [](std::vector<DirEntry>& listed) {
            const ULONGLONG start = GetTickCount64();
            EnumerateDirectory(L"", listed, EnumerationOptions{});
            return GetTickCount64() - start;
        };
        auto find = [&](const std::vector<DirEntry>& listed) -> const DirEntry* {
            for (const auto& entry : listed) if (entry.full_path == root) return &entry;
            return nullptr;
        };
        release_drive = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stalled_drive = letter;
        std::vector<DirEntry> first, second, third;
        bool listed_ok = true;
        ULONGLONG first_ms = 0, second_ms = 0;
        try { first_ms = list(first); second_ms = list(second); } catch (...) { listed_ok = false; }
        const DirEntry* stalled = find(first);
        std::printf("this pc stalled drive: first=%llums second=%llums drives=%zu queries=%u\n",
                    first_ms, second_ms, first.size(), stalled_queries.load());
        Check(listed_ok && first_ms < 5000 && stalled && stalled->name.ends_with(suffix) &&
              stalled->drive_total == 0 && first.size() == second.size(),
              "This PC lists every drive while one drive's media query stalls");
        Check(listed_ok && second_ms < 5000 && find(second) && stalled_queries.load() == 1,
              "a drive whose earlier media query is still stuck is not probed again");
        stalled_drive = 0;
        SetEvent(release_drive);
        // The released query finishes on its own thread; poll briefly.
        const DirEntry* recovered = nullptr;
        for (int i = 0; i < 30 && listed_ok && !(recovered && recovered->drive_total > 0); ++i) {
            try { list(third); } catch (...) { listed_ok = false; }
            recovered = find(third);
            if (!(recovered && recovered->drive_total > 0)) Sleep(100);
        }
        Check(listed_ok && recovered && recovered->drive_total > 0,
              "the drive's label and capacity return once its media answers");
        CloseHandle(release_drive);
    }
    CloseHandle(entered);CloseHandle(release_io);std::filesystem::remove_all(dir);
    return failures?1:0;
}
