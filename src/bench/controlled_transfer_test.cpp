#include "../ops/controlled_transfer.h"
#include "../app/extensions/transfer_snapshot.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <utility>
#include <winioctl.h>
#include <winternl.h>
#include <cstring>
using namespace pulse;
namespace {
int failures = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; }
bool RootJunctionProbe(const std::filesystem::path& target, const std::filesystem::path& outside,
    HANDLE directory) {
    HANDLE edit = CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (edit == INVALID_HANDLE_VALUE) return false;
    struct Data { DWORD tag; WORD length, reserved, offset, size, print_offset, print_size; wchar_t path[2048]; } data{};
    const auto name = L"\\??\\" + outside.wstring();
    data.tag = IO_REPARSE_TAG_MOUNT_POINT; data.size = static_cast<WORD>(name.size()*sizeof(wchar_t));
    data.print_offset = data.size + sizeof(wchar_t); data.length = static_cast<WORD>(8 + data.print_offset + sizeof(wchar_t));
    std::memcpy(data.path, name.c_str(), data.size + sizeof(wchar_t));
    DWORD count=0;
    const bool changed=DeviceIoControl(edit,FSCTL_SET_REPARSE_POINT,&data,data.length+8,nullptr,0,&count,nullptr)!=FALSE;
    Check(changed,"adversarial fixture changes held target into junction without replacing identity");
    HANDLE child=INVALID_HANDLE_VALUE;
    NTSTATUS status=-1;
    if(changed) {
        using Open=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
        auto open=reinterpret_cast<Open>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCreateFile"));
        wchar_t leaf[]=L"probe.txt";UNICODE_STRING text{};text.Buffer=leaf;text.Length=sizeof(leaf)-sizeof(wchar_t);text.MaximumLength=text.Length;
        OBJECT_ATTRIBUTES attributes{};attributes.Length=sizeof(attributes);attributes.RootDirectory=directory;attributes.ObjectName=&text;attributes.Attributes=0x40;
        IO_STATUS_BLOCK io{};
        status=open(&child,FILE_WRITE_DATA|DELETE|SYNCHRONIZE,&attributes,&io,nullptr,FILE_ATTRIBUTE_NORMAL,0,2,0x20|0x40|0x00200000,nullptr,0);
        if(child!=INVALID_HANDLE_VALUE)CloseHandle(child);
        struct Remove {DWORD tag;WORD length,reserved;} remove{IO_REPARSE_TAG_MOUNT_POINT,0,0};
        Check(DeviceIoControl(edit,FSCTL_DELETE_REPARSE_POINT,&remove,sizeof(remove),nullptr,0,&count,nullptr)!=FALSE,"fixture removes junction through its retained edit handle");
    }
    CloseHandle(edit);
    std::printf("relative create under mutated root NTSTATUS=%08lx\n",static_cast<unsigned long>(status));
    return changed && !std::filesystem::exists(outside/L"probe.txt") &&
        (status<0 || std::filesystem::exists(target/L"probe.txt"));
}
struct Fixture {
    std::filesystem::path root;
    extensions::authorization::Registry registry;
    extensions::authorization::PackageIdentity package{0, "fixture", "revision", 1};
    extensions::authorization::OwnerHandle owner;
    extensions::authorization::PrincipalBinding binding;
    extensions::transfer_plan::Store store;
    std::atomic<bool> cancelled{false};
    extensions::authorization::OwnerHandle Setup() { registry.SyncPackages({{package,true}}); return *registry.RegisterOwner(1); }
    Fixture() : owner(Setup()), binding(*registry.CaptureBinding(package, owner)), store(registry) {
        root = std::filesystem::absolute(std::filesystem::path("bench_data") /
            ("controlled-transfer-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("unique fixture failed");
        std::filesystem::create_directory(root / "from"); std::filesystem::create_directory(root / "to");
    }
    ~Fixture() {
        if (root.parent_path() == std::filesystem::absolute("bench_data") && root.filename().wstring().starts_with(L"controlled-transfer-")) {
            std::error_code error; std::filesystem::remove_all(root, error);
        }
    }
    std::filesystem::path File(const std::string& name, size_t bytes = 600000) {
        auto path = root / "from" / name; std::ofstream out(path, std::ios::binary); out << std::string(bytes, 'x'); return path;
    }
    extensions::transfer_plan::ExecutionPermit Permit(const std::vector<std::filesystem::path>& paths, bool move = false) {
        extensions::transfer_plan::Draft draft;
        draft.operation = move ? extensions::transfer_plan::Operation::Move : extensions::transfer_plan::Operation::Copy;
        for (const auto& path : paths) {
            auto source = extensions::transfer_snapshot::CaptureSource(path.wstring(), cancelled);
            if (!source) throw std::runtime_error("capture source failed");
            draft.sources.push_back(source.value->snapshot);
        }
        auto target = extensions::transfer_snapshot::CaptureTarget((root / "to").wstring(), cancelled);
        if (!target) throw std::runtime_error("capture target failed");
        draft.target = target.value->snapshot;
        const auto plan = store.Create(binding, draft);
        if (!plan) throw std::runtime_error("plan create failed");
        const auto receipt = store.Confirm(*plan.value, binding);
        if (!receipt) throw std::runtime_error("confirm failed");
        const auto permit = store.Consume(*receipt.value, binding);
        if (!permit) throw std::runtime_error("consume failed");
        return *permit.value;
    }
    extensions::transfer_plan::ExecutionPermit TreePermit(const std::filesystem::path& tree) {
        extensions::transfer_plan::Draft draft;
        draft.operation = extensions::transfer_plan::Operation::Copy;
        draft.directory_tree = true;
        draft.tree_root_name = tree.filename().wstring();
        for (const auto& [source_path, relative] : std::vector<std::pair<std::filesystem::path, std::wstring>>{
                 {tree, L""}, {tree / "nested", L"nested"}, {tree / "empty", L"empty"}}) {
            auto directory = extensions::transfer_snapshot::CaptureTarget(source_path.wstring(), cancelled);
            if (!directory) throw std::runtime_error("capture tree directory failed");
            draft.tree_directories.push_back({source_path.wstring(), relative, directory.value->snapshot.identity});
        }
        for (const auto& [source_path, relative] : std::vector<std::pair<std::filesystem::path, std::wstring>>{
                 {tree / "alpha.bin", L"alpha.bin"}, {tree / "nested" / "beta.bin", L"nested\\beta.bin"}}) {
            auto source = extensions::transfer_snapshot::CaptureSource(source_path.wstring(), cancelled);
            if (!source) throw std::runtime_error("capture tree file failed");
            auto snapshot = source.value->snapshot;
            snapshot.relative_destination = relative;
            draft.sources.push_back(std::move(snapshot));
        }
        auto target = extensions::transfer_snapshot::CaptureTarget((root / "to").wstring(), cancelled);
        if (!target) throw std::runtime_error("capture tree destination failed");
        draft.target = target.value->snapshot;
        const auto plan = store.Create(binding, draft);
        if (!plan) throw std::runtime_error("tree plan create failed");
        const auto receipt = store.Confirm(*plan.value, binding);
        const auto permit = receipt ? store.Consume(*receipt.value, binding)
                                    : extensions::transfer_plan::Result<extensions::transfer_plan::ExecutionPermit>{};
        if (!permit) throw std::runtime_error("tree permit consume failed");
        return *permit.value;
    }
    bool NoStaging() const {
        for (const auto& item : std::filesystem::directory_iterator(root / "to"))
            if (item.path().filename().wstring().starts_with(L".pulse-stage-")) return false;
        return true;
    }
};
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        using ops::ControlledTransferState;
        { Fixture f; std::filesystem::create_directory(f.root/"outside");
          auto snapshot=extensions::transfer_snapshot::CaptureTarget((f.root/"to").wstring(),f.cancelled);
          const auto expected=snapshot.value->snapshot;snapshot.value.reset();
          auto lease=extensions::transfer_snapshot::AcquireTransferTarget(expected,f.cancelled);
          Check(lease && RootJunctionProbe(f.root/"to",f.root/"outside",lease.value->handles.Leaf()),
              "same directory handle relative create does not follow newly installed root junction"); }

        { Fixture f; auto file=f.File("copy.txt"); auto p=f.Permit({file});
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled);
          std::printf("copy error=%lu cleanup=%lu bytes=%llu\n",r.items[0].error,r.items[0].cleanup_error,static_cast<unsigned long long>(r.items[0].bytes));
          Check(r.complete && r.items[0].state==ControlledTransferState::Copied && std::filesystem::file_size(f.root/"to"/"copy.txt")==600000 && std::filesystem::exists(file),"copy commits complete destination and keeps source");
          std::ifstream copied(f.root/"to"/"copy.txt",std::ios::binary);
          const std::string content((std::istreambuf_iterator<char>(copied)),std::istreambuf_iterator<char>());
          Check(content==std::string(600000,'x'),"copy output bytes exactly match captured fixture content");
          Check(std::filesystem::last_write_time(file)==std::filesystem::last_write_time(f.root/"to"/"copy.txt"),"copy preserves source last write timestamp");
          Check(ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled).permit_error==extensions::transfer_plan::Error::AlreadyStarted,"same execution permit cannot run twice"); Check(f.NoStaging(),"successful copy leaves no staging"); }
        { Fixture f; auto file=f.File("move.txt"); auto p=f.Permit({file},true); auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled);
          Check(r.complete && r.items[0].state==ControlledTransferState::Moved && !std::filesystem::exists(file) && std::filesystem::exists(f.root/"to"/"move.txt"),"move commits destination then deletes verified source handle"); }
        { Fixture f; auto file=f.File("conflict.txt"); auto p=f.Permit({file}); std::ofstream(f.root/"to"/"conflict.txt")<<"keep";
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled); Check(!r.complete && std::filesystem::file_size(f.root/"to"/"conflict.txt")==4 && f.NoStaging(),"existing destination is never overwritten"); }
        { Fixture f; auto file=f.File("same.txt"); std::filesystem::create_directory(f.root/"other"); auto other=f.root/"other"/"SAME.txt";std::ofstream(other)<<"second";auto p=f.Permit({file,other});
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled); Check(!r.complete && r.items[0].state==ControlledTransferState::NotStarted && std::filesystem::is_empty(f.root/"to"),"case insensitive duplicate leaf names reject entire batch before writing"); }
        { Fixture f; auto file=f.File("cancel.txt"); auto p=f.Permit({file}); auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled,[&](const auto&){f.cancelled=true;});
          Check(!r.complete && r.items[0].state==ControlledTransferState::Cancelled && f.NoStaging() && !std::filesystem::exists(f.root/"to"/"cancel.txt"),"chunk cancellation cleans only owned staging and preserves source"); }
        { Fixture f; auto file=f.File("revoke.txt"); auto p=f.Permit({file}); auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled,[&](const auto&){f.registry.SyncPackages({{f.package,false}});});
          Check(!r.complete && r.items[0].error==ERROR_ACCESS_DENIED && f.NoStaging(),"caller revocation during transfer blocks commit and cleans staging"); }
        { Fixture f; auto file=f.File("partial.txt"); auto p=f.Permit({file},true); auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled,[&](const auto& progress){if(progress.committed)f.cancelled=true;});
          Check(!r.complete && r.items[0].state==ControlledTransferState::DestinationCommittedSourceRetained && std::filesystem::exists(file) && std::filesystem::file_size(f.root/"to"/"partial.txt")==600000,"cancel after commit reports destination plus retained source without rollback"); }
        { Fixture f; auto file=f.File("replaced.txt"); auto p=f.Permit({file}); std::filesystem::rename(file,f.root/"original.txt"); std::ofstream(file)<<"replacement";
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled); Check(!r.complete && r.items[0].error==ERROR_FILE_INVALID && f.NoStaging(),"source replacement identity is rejected before staging"); }
        { Fixture f; auto file=f.File("target.txt"); auto p=f.Permit({file}); std::filesystem::rename(f.root/"to",f.root/"old-to");std::filesystem::create_directory(f.root/"to");
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled);Check(!r.complete&&r.items[0].error==ERROR_FILE_INVALID&&std::filesystem::is_empty(f.root/"to"),"target replacement identity is rejected before writing"); }
        { Fixture f; auto file=f.File("download.txt"); std::ofstream(std::filesystem::path(file.wstring()+L":Zone.Identifier"))<<"[ZoneTransfer]\nZoneId=3";auto p=f.Permit({file});
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled);Check(!r.complete&&r.items[0].error==ERROR_NOT_SUPPORTED&&std::filesystem::is_empty(f.root/"to"),"Zone.Identifier is rejected rather than silently dropped"); }
        { Fixture f; auto first=f.File("first.txt"), second=f.File("second.txt");auto p=f.Permit({first,second});
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled,[&](const auto& progress){if(progress.committed)f.cancelled=true;});
          Check(!r.complete&&r.items[0].state==ControlledTransferState::Copied&&r.items[1].state!=ControlledTransferState::Copied&&std::filesystem::exists(f.root/"to"/"first.txt"),"later cancellation preserves completed earlier batch outcome"); }
        { Fixture f; auto file=f.File("throw.txt");auto p=f.Permit({file});
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled,[](const auto&){throw std::bad_alloc();});
          Check(!r.complete&&r.items[0].error==ERROR_UNHANDLED_EXCEPTION&&f.NoStaging(),"throwing progress cannot escape worker and owned staging is cleaned"); }
        { Fixture f; auto file=f.File("readonly.txt");SetFileAttributesW(file.c_str(),FILE_ATTRIBUTE_READONLY);auto p=f.Permit({file},true);
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled);
          Check(!r.complete&&r.items[0].state==ControlledTransferState::DestinationCommittedSourceRetained&&std::filesystem::exists(file)&&std::filesystem::exists(f.root/"to"/"readonly.txt"),"readonly move delete failure reports actual destination and retained source");
          SetFileAttributesW(file.c_str(),FILE_ATTRIBUTE_NORMAL);SetFileAttributesW((f.root/"to"/"readonly.txt").c_str(),FILE_ATTRIBUTE_NORMAL); }
        { Fixture f; auto file=f.File("offline.txt");SetFileAttributesW(file.c_str(),FILE_ATTRIBUTE_OFFLINE);auto p=f.Permit({file});
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled);Check(!r.complete&&r.items[0].error==ERROR_NOT_SUPPORTED&&std::filesystem::is_empty(f.root/"to"),"unsupported offline attribute rejected without producing output");SetFileAttributesW(file.c_str(),FILE_ATTRIBUTE_NORMAL); }
        { Fixture f; auto file=f.File("empty.txt",0);auto p=f.Permit({file});auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled);
          Check(r.complete&&std::filesystem::file_size(f.root/"to"/"empty.txt")==0,"zero byte source completes within captured size bound"); }
        { Fixture f; auto first=f.File("first.txt"), second=f.File("second.txt");auto p=f.Permit({first,second});std::ofstream(f.root/"to"/"second.txt")<<"preserve";
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled);Check(!r.complete&&!std::filesystem::exists(f.root/"to"/"first.txt")&&std::filesystem::file_size(f.root/"to"/"second.txt")==8,"later existing conflict rejects batch before first output"); }
        { Fixture f; const auto tree=f.root/"from"/"tree"; std::filesystem::create_directories(tree/"nested"); std::filesystem::create_directory(tree/"empty");
          std::ofstream(tree/"alpha.bin",std::ios::binary)<<"alpha"; std::ofstream(tree/"nested"/"beta.bin",std::ios::binary)<<"nested-data";
          auto p=f.TreePermit(tree); auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled);
          std::ifstream alpha(f.root/"to"/"tree"/"alpha.bin",std::ios::binary), beta(f.root/"to"/"tree"/"nested"/"beta.bin",std::ios::binary);
          const std::string alpha_bytes((std::istreambuf_iterator<char>(alpha)),{}), beta_bytes((std::istreambuf_iterator<char>(beta)),{});
          Check(r.complete&&r.items.size()==2&&alpha_bytes=="alpha"&&beta_bytes=="nested-data"&&
              std::filesystem::is_empty(f.root/"to"/"tree"/"empty")&&std::filesystem::exists(tree/"alpha.bin"),
              "directory tree copy preserves nested file bytes, empty directories and all sources"); }
        { Fixture f; const auto tree=f.root/"from"/"tree"; std::filesystem::create_directories(tree/"nested"); std::filesystem::create_directory(tree/"empty");
          std::ofstream(tree/"alpha.bin")<<"alpha"; std::ofstream(tree/"nested"/"beta.bin")<<"nested-data";
          auto p=f.TreePermit(tree); std::filesystem::create_directory(f.root/"to"/"tree"); std::ofstream(f.root/"to"/"tree"/"sentinel")<<"keep";
          auto r=ops::ExecuteControlledTransfer(p,f.registry,f.binding,f.cancelled);
          Check(!r.complete&&!r.items.empty()&&r.items[0].error==ERROR_FILE_EXISTS&&std::filesystem::exists(f.root/"to"/"tree"/"sentinel")&&
              !std::filesystem::exists(f.root/"to"/"tree"/"alpha.bin"),
              "existing tree root rejects the request before changing existing target data"); }
        { Fixture f; const auto tree=f.root/"from"/"tree"; std::filesystem::create_directory(tree);
          auto target=extensions::transfer_snapshot::CaptureTarget((f.root/"to").wstring(),f.cancelled);
          auto directory=extensions::transfer_snapshot::CaptureTarget(tree.wstring(),f.cancelled);
          extensions::transfer_plan::Draft draft; draft.directory_tree=true; draft.tree_root_name=L"tree"; draft.target=target.value->snapshot;
          draft.tree_directories.push_back({tree.wstring(),L"",directory.value->snapshot.identity});
          extensions::transfer_plan::SourceFile escape; escape.path=(tree/"ignored").wstring(); escape.relative_destination=L"..\\outside"; draft.sources.push_back(escape);
          Check(f.store.Create(f.binding,draft).error==extensions::transfer_plan::Error::InvalidDraft,
              "directory tree plan rejects parent traversal in relative output paths"); }
    } catch(const std::exception& error) { std::printf("[FAIL] fixture %s\n",error.what());return 1; }
    return failures?1:0;
}
