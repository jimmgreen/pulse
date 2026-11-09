#include "../common/dialog_bundle.h"
#include "../common/dialog_bundle_pointer.h"
#include "../common/json_value.h"
#include "../dialog_launcher/managed_launch_profile.h"
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <functional>
#include <array>
#include <stdexcept>
namespace {
namespace fs = std::filesystem;
namespace bundle = pulse::dialog_bundle;
using pulse::json::Value;
int checks=0, failures=0;
void Check(bool ok,const char* name,DWORD error=0) {
    ++checks; if(!ok) ++failures;
    std::cout<<(ok?"[PASS] ":"[FAIL] ")<<name<<" error="<<error<<'\n';
}
void Need(bool ok) { if(!ok) throw std::runtime_error("fixture setup failed"); }
std::wstring Self() { std::wstring s(32768,0); DWORD n=GetModuleFileNameW(nullptr,s.data(),static_cast<DWORD>(s.size())); Need(n&&n<s.size()); s.resize(n); return s; }
void Put(std::vector<unsigned char>& b,size_t at,uint64_t value,unsigned count) { for(unsigned i=0;i<count;++i)b.at(at+i)=static_cast<unsigned char>(value>>(8*i)); }
constexpr unsigned machine = sizeof(void*)==8?0x8664:0x14c;
std::vector<unsigned char> Image(std::string normal="kernel32.dll",std::string delay="kernel32.dll",unsigned m=machine) {
    std::vector<unsigned char> b(0x1200); Put(b,0,0x5a4d,2);Put(b,0x3c,0x80,4);Put(b,0x80,0x4550,4);
    const bool x64=m==0x8664; const size_t opt=0x98,sz=x64?240:224,off=x64?112:96;
    Put(b,0x84,m,2);Put(b,0x86,1,2);Put(b,0x94,sz,2);Put(b,0x96,IMAGE_FILE_EXECUTABLE_IMAGE|IMAGE_FILE_DLL,2);
    Put(b,opt,x64?0x20b:0x10b,2);Put(b,opt+(x64?24:28),x64?0x140000000ULL:0x400000,x64?8:4);
    Put(b,opt+60,0x200,4);Put(b,opt+off-4,16,4);
    const size_t section=opt+sz; Put(b,section+12,0x1000,4);Put(b,section+16,0x1000,4);Put(b,section+20,0x200,4);
    for(bool delayed:{false,true}) { const auto& name=delayed?delay:normal; if(name.empty())continue;
        const size_t raw=delayed?0x400:0x200,index=delayed?13:1,stride=delayed?32:20, text=delayed?0x700:0x600;
        Put(b,opt+off+index*8,raw+0xe00,4);Put(b,opt+off+index*8+4,stride*2,4);
        if(delayed)Put(b,raw,1,4);Put(b,raw+(delayed?4:12),text+0xe00,4);
        for(size_t i=0;i<name.size();++i)b.at(text+i)=static_cast<unsigned char>(name[i]);
    } return b;
}
std::string Hash(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE a=nullptr; BCRYPT_HASH_HANDLE h=nullptr; std::array<unsigned char,32> digest{};
    Need(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0);
    const bool ok=BCryptCreateHash(a,&h,nullptr,0,nullptr,0,0)>=0 && BCryptHashData(h,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0)>=0 && BCryptFinishHash(h,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
    if(h)BCryptDestroyHash(h);BCryptCloseAlgorithmProvider(a,0);Need(ok);
    std::string s;for(auto c:digest){s+="0123456789abcdef"[c>>4];s+="0123456789abcdef"[c&15];}return s;
}
void Write(const fs::path& path,const std::vector<unsigned char>& bytes) {
    fs::create_directories(path.parent_path());std::ofstream f(path,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));Need(!!f);
}
void WriteText(const fs::path& p,const std::string& text){Write(p,{text.begin(),text.end()});}
Value Strings(std::string s){auto a=Value::Array();if(!s.empty())a.push(Value(s));return a;}
struct Fixture {
    fs::path dir; Value manifest=Value::Object();
    std::vector<std::string> names;
    explicit Fixture(fs::path p):dir(std::move(p)) {
        Need(CreateDirectoryW(dir.c_str(),nullptr)!=FALSE);
        const std::string runtime=machine==0x8664?"PulseFileDialogRuntimeUiProof64R8.dll":"PulseFileDialogRuntimeUiProof32R8.dll";
        names={"PulseDialogLauncher.exe",runtime,"pulse.exe","PulsePickerUiProof.dll","lumatext.dll","pulse_shell.exe","Pulse.Preview.exe","pdfium.dll","pulse_elevated.exe"};
        manifest.set("schema",1);manifest.set("version","synthetic-1");manifest.set("architecture",machine==0x8664?"x64":"x86");
        auto c=Value::Object();c.set("manifest",1);c.set("picker_ui_abi",2);c.set("runtime_session",1);c.set("open_policy","LocalOpenV1");c.set("save_policy","LocalSaveV1");c.set("experimental",true);c.set("capability_certification",false);manifest.set("contract",std::move(c));
        auto d=Value::Object();d.set("Pulse.Preview.exe","src/ui/thumbnail_cache.cpp: module-relative preview worker");d.set("pdfium.dll","src/preview_host/pdf_raster.cpp: dynamic PDF renderer");d.set("pulse_elevated.exe","src/ops/elevated_transfer_client.cpp: explicit elevated operation helper");manifest.set("dynamic_helpers",std::move(d));
        manifest.set("system_dependencies",Strings("kernel32.dll"));manifest.set("files",Value::Array());
        for(const auto& name:names)Add(name,Image(),false);
        for(auto name:{"licenses/Pulse/LICENSE","licenses/Detours/LICENSE.md","licenses/ib-pinyin/LICENSE.txt","licenses/md4c/LICENSE.md","licenses/SQLite/README.md","licenses/LZMA/README.pulse.md","licenses/LumaText/LICENSE","licenses/PDFium/LICENSE"})Add(name,{'t','e','s','t'},true);
        Commit();
    }
    void Add(const std::string& name,std::vector<unsigned char> bytes,bool license){
        if(!license&&fs::path(name).extension()==L".exe")Put(bytes,0x96,IMAGE_FILE_EXECUTABLE_IMAGE,2);
        Write(dir/pulse::json::Wide(name),bytes);
        auto e=Value::Object();e.set("path",name);e.set("role",license?"license":"entry");e.set("size",static_cast<uint64_t>(bytes.size()));e.set("sha256",Hash(bytes));
        if(!license){e.set("machine",static_cast<int>(machine));e.set("imports",Strings("kernel32.dll"));e.set("delay_imports",Strings("kernel32.dll"));}
        auto files=*manifest.find("files");files.push(std::move(e));manifest.set("files",std::move(files));
    }
    void Change(size_t i,const std::function<void(Value&)>& fn){auto files=Value::Array();size_t n=0;for(auto e:manifest.find("files")->items()){if(n++==i)fn(e);files.push(std::move(e));}manifest.set("files",std::move(files));}
    void Commit(){const auto text=pulse::json::Serialize(manifest);WriteText(dir/L"manifest.json",text);auto marker=Value::Object();marker.set("manifest_sha256",Hash({text.begin(),text.end()}));marker.set("integrity_only",true);WriteText(dir/L"VERIFIED.json",pulse::json::Serialize(marker));}
    ~Fixture(){std::error_code error;fs::remove_all(dir,error);if(error){++failures;std::cerr<<"[FAIL] synthetic fixture cleanup "<<error.value()<<'\n';}}
};
}
int wmain(int argc,wchar_t** argv){
    std::cout<<std::unitbuf;
    if(argc==3&&std::wstring_view(argv[1])==L"--resolve-installed") {bundle::BundleLease lease;DWORD e=bundle::ResolveInstalledBundle(argv[2],lease);Check(e==0,"read-only installed pointer verification",e);if(!e)Check(bundle::RecheckDirectory(lease)==0,"retained installed snapshot recheck");return failures?1:0;}
    if(argc==3&&std::wstring_view(argv[1])==L"--verify") {bundle::BundleLease lease;DWORD e=bundle::VerifyBundle(argv[2],{},lease);Check(e==0,"read-only supplied bundle verification",e);if(!e)Check(bundle::RecheckDirectory(lease)==0,"retained supplied bundle directory recheck");return failures?1:0;}
    const bool installed_only=argc==2 && std::wstring_view(argv[1])==L"--installed-pointer";
    if(argc!=1 && !installed_only)return 2;
    try {
        const auto bench=fs::path(Self()).parent_path().parent_path()/L"bench_data";fs::create_directories(bench);
        const auto root=bench/(L"dialog-bundle-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));Need(CreateDirectoryW(root.c_str(),nullptr)!=FALSE);
        unsigned next=0;
        if(installed_only) {
            const auto app=root/L"app";fs::create_directories(app/L"DialogBridge");
            Fixture source(root/L"source");
            const auto manifest=pulse::json::Serialize(source.manifest);
            const auto digest=Hash({manifest.begin(),manifest.end()});
            const auto selected=app/L"DialogBridge"/pulse::json::Wide(digest);
            fs::rename(source.dir,selected);
            auto pointer=Value::Object();pointer.set("schema",1);pointer.set("directory","DialogBridge/"+digest);pointer.set("manifest_sha256",digest);
            auto publish=[&](const Value& value){WriteText(app/L"dialog-current.json",pulse::json::Serialize(value));};
            bundle::BundleLease lease;
            auto reject=[&](const char* label){const auto e=bundle::ResolveInstalledBundle(app.wstring(),lease);Check(e!=0&&!lease,label,e);};
            reject("missing pointer never falls back to root");
            publish(pointer);
            {
                const auto pointer_path=app/L"dialog-current.json";
                const HANDLE snapshot=bundle::detail::OpenPointerSnapshot(pointer_path.c_str());
                Need(snapshot!=INVALID_HANDLE_VALUE);
                const HANDLE writer=CreateFileW(pointer_path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
                Check(writer==INVALID_HANDLE_VALUE,"pointer snapshot forbids in-place writes");
                if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
                WriteText(app/L"next-pointer.json","{}");
                const auto backup=app/L"previous-pointer.json";
                const BOOL retired=MoveFileExW(pointer_path.c_str(),backup.c_str(),MOVEFILE_WRITE_THROUGH);
                const DWORD retire_error=retired ? ERROR_SUCCESS : GetLastError();
                Check(retired!=FALSE,"installer retires pointer to backup during open snapshot",retire_error);
                const BOOL published=retired && MoveFileExW((app/L"next-pointer.json").c_str(),pointer_path.c_str(),MOVEFILE_WRITE_THROUGH);
                const DWORD publish_error=published ? ERROR_SUCCESS : GetLastError();
                Check(published!=FALSE,"installer publishes next pointer during open snapshot",publish_error);
                std::array<char,1024> bytes{};DWORD read=0;
                const BOOL read_ok=ReadFile(snapshot,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr);
                Check(read_ok && std::string(bytes.data(),read)==pulse::json::Serialize(pointer),"open pointer snapshot reads original complete bytes after publication");
                CloseHandle(snapshot);
                publish(pointer);
            }
            auto error=bundle::ResolveInstalledBundle(app.wstring(),lease);
            Check(error==0&&lease.Directory()==selected.wstring(),"pointer resolves digest-bound private bundle",error);
            Check(lease.Host()==(selected/L"pulse.exe").wstring(),"host belongs to private directory");
            WriteText(app/L"pulse.exe","main application replaceable");
            Check(DeleteFileW((app/L"pulse.exe").c_str())!=FALSE,"lease does not pin root executable");
            Check(DeleteFileW((app/L"dialog-current.json").c_str())!=FALSE,"pointer can be replaced while lease lives");
            publish(pointer);
            Check(bundle::RecheckDirectory(lease)==0,"selected snapshot survives pointer replacement");
            lease=bundle::BundleLease{};
            auto bad=pointer;bad.set("directory","DialogBridge/../"+digest);publish(bad);reject("pointer path traversal rejected");
            bad=pointer;bad.set("directory","DialogBridge\\"+digest);publish(bad);reject("pointer backslash path rejected");
            bad=pointer;bad.set("manifest_sha256",std::string(64,'A'));publish(bad);reject("nonlowercase digest rejected");
            bad=pointer;bad.set("unexpected",true);publish(bad);reject("unknown pointer key rejected");
            bad=pointer;bad.set("schema",2);publish(bad);reject("unknown pointer schema rejected");
            WriteText(app/L"dialog-current.json",std::string(1025,' '));reject("oversized pointer rejected");
            WriteText(app/L"dialog-current.json","{broken");reject("malformed pointer rejected");
            WriteText(app/L"dialog-current.json","{\"schema\":1,\"schema\":1,\"directory\":\"DialogBridge/"+digest+"\",\"manifest_sha256\":\""+digest+"\"}");reject("duplicate pointer keys rejected");
            publish(pointer);WriteText(selected/L"manifest.json",manifest+" ");reject("manifest raw byte digest mismatch rejected");
            WriteText(selected/L"manifest.json",manifest);
            Fixture legacy(root/L"legacy");legacy.manifest.set("schema",2);legacy.Commit();
            error=bundle::VerifyBundle(legacy.dir.wstring(),{},lease);
            Check(error==0,"explicit legacy schema2 remains readable",error);lease=bundle::BundleLease{};
            error=bundle::ResolveInstalledBundle(legacy.dir.wstring(),lease);
            Check(error!=0&&!lease,"installed resolver does not fall back to legacy root",error);
            // A pointer file itself must not redirect reads to another directory.
            Need(DeleteFileW((app/L"dialog-current.json").c_str())!=FALSE);
            WriteText(root/L"pointer-target.json",pulse::json::Serialize(pointer));
            if(CreateSymbolicLinkW((app/L"dialog-current.json").c_str(),(root/L"pointer-target.json").c_str(),0x2))
                reject("reparse pointer rejected");
            else std::cout<<"[SKIP] reparse pointer fixture requires symlink permission error="<<GetLastError()<<'\n';
            std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
        }
        const auto run=[&](const char* name,const std::function<void(Fixture&)>& alter,DWORD expected=ERROR_INVALID_DATA){Fixture f(root/std::to_wstring(++next));alter(f);bundle::BundleLease lease;const DWORD e=bundle::VerifyBundle(f.dir.wstring(),{},lease);Check(e==expected,name,e);Check(!lease,"failure returns empty lease");};
        {
            Fixture f(root/std::to_wstring(++next));bundle::BundleLease lease;DWORD e=bundle::VerifyBundle(f.dir.wstring(),{},lease);Check(!e&&!!lease,"valid synthetic full bundle",e);
            const auto digest=lease.ManifestDigest();Check(digest.size()==64&&lease.Machine()==machine,"lease publishes digest and machine");
            HANDLE writer=CreateFileW((f.dir/L"PulsePickerUiProof.dll").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);const DWORD write_error=GetLastError();Check(writer==INVALID_HANDLE_VALUE&&write_error==ERROR_SHARING_VIOLATION,"lease blocks component writes",write_error);if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
            const BOOL removed=DeleteFileW((f.dir/L"PulsePickerUiProof.dll").c_str());const DWORD remove_error=GetLastError();Check(!removed&&remove_error==ERROR_SHARING_VIOLATION,"lease blocks component removal",remove_error);
            WriteText(f.dir/L"extra.dll","x");Check(bundle::RecheckDirectory(lease)!=0,"recheck detects extra entry");Need(DeleteFileW((f.dir/L"extra.dll").c_str())!=FALSE);
            Check(bundle::RecheckDirectory(lease)==0,"recheck accepts unchanged identity set");
            pulse::dialog_launcher::ManagedLaunchProfile p;p.format_version=2;p.bundle_root=f.dir.wstring();p.manifest_digest=digest;p.host=lease.Host();p.runtime=lease.Runtime();p.target=lease.Host();p.cwd=root.wstring();p.control_root=root.wstring();
            e=pulse::dialog_launcher::CaptureProfile(p,lease.Launcher());Check(!e,"v2 captures full bundle binding",e);
            std::vector<unsigned char> encoded;pulse::dialog_launcher::ManagedLaunchProfile decoded;
            Check(pulse::dialog_launcher::EncodeProfile(p,encoded)==0&&pulse::dialog_launcher::DecodeProfile(encoded,decoded)==0&&decoded.format_version==2&&decoded.manifest_digest==digest,"v2 roundtrip retains manifest binding");
            Check(pulse::dialog_launcher::ValidateProfile(decoded,lease.Launcher())==0,"v2 validates complete bundle");
            lease=bundle::BundleLease{};WriteText(f.dir/L"PulsePickerUiProof.dll","tampered");e=pulse::dialog_launcher::ValidateProfile(decoded,p.launcher);Check(e!=0,"v2 rejects changed UI beyond three old identities",e);
        }
        {
            Fixture f(root/std::to_wstring(++next));
            f.Add(machine==0x8664?"PulseDialogAuto64.dll":"PulseDialogAuto32.dll",Image(),false);
            f.Add("PulseDialogAutoControl.exe",Image(),false);f.Commit();
            bundle::BundleLease lease;const DWORD e=bundle::VerifyBundle(f.dir.wstring(),{},lease);
            Check(e==0,"architecture-matched automatic bundle accepted",e);
        }
        run("wrong automatic DLL name rejected",[](auto& f){f.Add(machine==0x8664?"PulseDialogAuto32.dll":"PulseDialogAuto64.dll",Image(),false);f.Add("PulseDialogAutoControl.exe",Image(),false);f.Commit();});
        run("incomplete automatic pair rejected",[](auto& f){f.Add("PulseDialogAutoControl.exe",Image(),false);f.Commit();});
        run("missing UI rejected",[](auto& f){Need(DeleteFileW((f.dir/L"PulsePickerUiProof.dll").c_str())!=FALSE);},ERROR_FILE_NOT_FOUND);
        run("unlisted file rejected",[](auto& f){WriteText(f.dir/L"extra.dll","x");});
        run("byte tamper rejected",[](auto& f){std::fstream s(f.dir/L"lumatext.dll",std::ios::in|std::ios::out|std::ios::binary);s.put('X');Need(!!s);},ERROR_CRC);
        run("license tamper rejected",[](auto& f){WriteText(f.dir/L"licenses/Pulse/LICENSE","best");},ERROR_CRC);
        run("parent path escape rejected",[](auto& f){f.Change(0,[](auto& e){e.set("path","../outside.exe");});f.Commit();});
        run("absolute path rejected",[](auto& f){f.Change(0,[](auto& e){e.set("path","C:/outside.exe");});f.Commit();});
        run("ADS rejected",[](auto& f){f.Change(0,[](auto& e){e.set("path","pulse.exe:payload");});f.Commit();});
        run("case collision rejected",[](auto& f){f.Change(1,[](auto& e){e.set("path","PULSEDIALOGLAUNCHER.EXE");});f.Commit();});
        run("unknown schema rejected",[](auto& f){f.manifest.set("schema",3);f.Commit();},ERROR_REVISION_MISMATCH);
        {
            Fixture f(root/std::to_wstring(++next));f.manifest.set("schema",2);f.Commit();
            WriteText(f.dir/L"uninstall.exe","installer generated");
            WriteText(f.dir/L"setup-defaults.ini","settings");WriteText(f.dir/L".pulse-files","owned files");
            WriteText(f.dir/L".pulse-setup/backup/pulse.exe","previous version");
            WriteText(f.dir/L"DialogBridge/user-note.txt","unowned legacy data");
            bundle::BundleLease lease;const DWORD e=bundle::VerifyBundle(f.dir.wstring(),{},lease);
            Check(e==0,"installed shared root accepts exact setup metadata and excluded backup trees",e);
            if(!e) {
                WriteText(f.dir/L"rogue.dll","unexpected code");
                Check(bundle::RecheckDirectory(lease)!=0,"installed root still rejects undeclared DLL");
                Need(DeleteFileW((f.dir/L"rogue.dll").c_str())!=FALSE);
                Check(bundle::RecheckDirectory(lease)==0,"installed shared root retains identity checks");
            }
        }
        run("portable bundle rejects installer exceptions",[](auto& f){WriteText(f.dir/L".pulse-files","metadata");});
        run("installed bundle rejects unknown directories",[](auto& f){f.manifest.set("schema",2);f.Commit();WriteText(f.dir/L"plugins/rogue.dll","unexpected code");});
        run("installed bundle rejects byte tampering",[](auto& f){f.manifest.set("schema",2);f.Commit();WriteText(f.dir/L"pulse.exe","tampered");});
        run("unknown contract rejected",[](auto& f){auto c=*f.manifest.find("contract");c.set("picker_ui_abi",3);f.manifest.set("contract",c);f.Commit();},ERROR_REVISION_MISMATCH);
        run("wrong scalar type rejected",[](auto& f){f.Change(0,[](auto& e){e.set("size",true);});f.Commit();});
        run("fractional file size rejected",[](auto& f){f.Change(0,[](auto& e){e.set("size",1.5);});f.Commit();});
        run("missing required file metadata",[](auto& f){auto files=Value::Array();auto e=Value::Object();e.set("path","pulse.exe");files.push(e);f.manifest.set("files",files);f.Commit();});
        run("marker mismatch rejected",[](auto& f){WriteText(f.dir/L"VERIFIED.json",R"({"manifest_sha256":"bad","integrity_only":true})");});
        run("duplicate JSON keys rejected",[](auto& f){WriteText(f.dir/L"manifest.json",R"({"schema":1,"schema":1})");});
        run("manifest input bound",[](auto& f){WriteText(f.dir/L"manifest.json",std::string(1024*1024+1,' '));},ERROR_FILE_TOO_LARGE);
        run("unknown file field rejected",[](auto& f){f.Change(0,[](auto& e){e.set("extra",true);});f.Commit();});
        run("other architecture image rejected",[](auto& f){auto b=Image("kernel32.dll","kernel32.dll",machine==0x8664?0x14c:0x8664);Write(f.dir/pulse::json::Wide(f.names[1]),b);f.Change(1,[&](auto& e){e.set("sha256",Hash(b));});f.Commit();},ERROR_EXE_MACHINE_TYPE_MISMATCH);
        run("inconsistent PE magic rejected",[](auto& f){auto b=Image();Put(b,0x98,machine==0x8664?0x10b:0x20b,2);Write(f.dir/pulse::json::Wide(f.names[1]),b);f.Change(1,[&](auto& e){e.set("sha256",Hash(b));});f.Commit();},ERROR_BAD_EXE_FORMAT);
        run("unresolved delay dependency rejected",[](auto& f){auto b=Image("kernel32.dll","absent.dll");Write(f.dir/pulse::json::Wide(f.names[1]),b);f.Change(1,[&](auto& e){e.set("sha256",Hash(b));e.set("delay_imports",Strings("absent.dll"));});f.Commit();},ERROR_MOD_NOT_FOUND);
        run("unresolved ordinary dependency rejected",[](auto& f){auto b=Image("absent.dll","kernel32.dll");Write(f.dir/pulse::json::Wide(f.names[1]),b);f.Change(1,[&](auto& e){e.set("sha256",Hash(b));e.set("imports",Strings("absent.dll"));});f.Commit();},ERROR_MOD_NOT_FOUND);
        run("delay directory must contain its terminator",[](auto& f){auto b=Image();Put(b,0x98+(machine==0x8664?112:96)+13*8+4,32,4);Write(f.dir/pulse::json::Wide(f.names[1]),b);f.Change(1,[&](auto& e){e.set("sha256",Hash(b));});f.Commit();},ERROR_BAD_EXE_FORMAT);
        run("actual import differs from manifest",[](auto& f){f.Change(1,[](auto& e){e.set("imports",Strings("user32.dll"));});f.Commit();});
        run("truncated PE despite matching hash rejected",[](auto& f){std::vector<unsigned char> b{'M','Z'};Write(f.dir/pulse::json::Wide(f.names[1]),b);f.Change(1,[&](auto& e){e.set("sha256",Hash(b));e.set("size",2);});f.Commit();},ERROR_BAD_EXE_FORMAT);
        {
            Fixture f(root/std::to_wstring(++next));bundle::BundleLease lease;Need(bundle::VerifyBundle(f.dir.wstring(),{},lease)==0);auto old=lease.ManifestDigest();lease=bundle::BundleLease{};
            f.manifest.set("version","changed-package");f.Commit();DWORD e=bundle::VerifyBundle(f.dir.wstring(),old,lease);Check(e==ERROR_REVISION_MISMATCH&&!lease,"saved digest rejects self-consistent replacement manifest",e);
        }
        Check(RemoveDirectoryW(root.c_str())!=FALSE,"remove only unique owned test root");
    }catch(const std::exception& e){Check(false,e.what());}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
