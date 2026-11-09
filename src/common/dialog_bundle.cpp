#include "dialog_bundle.h"
#include "dialog_bundle_pointer.h"
#include "json_value.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <vector>
#include <utility>
#include <stdexcept>

namespace pulse::dialog_bundle {
namespace {
namespace fs = std::filesystem;
using json::Value;
struct Failure { DWORD error; };
void Require(bool ok, DWORD error = ERROR_INVALID_DATA) { if (!ok) throw Failure{error}; }
DWORD Last() { const DWORD e = GetLastError(); return e ? e : ERROR_FUNCTION_FAILED; }
void Win(bool ok) { if (!ok) throw Failure{Last()}; }
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    File() = default;
    explicit File(HANDLE value) : h(value) {}
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    File(File&& f) noexcept : h(std::exchange(f.h, INVALID_HANDLE_VALUE)) {}
    File& operator=(File&& f) noexcept { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = std::exchange(f.h, INVALID_HANDLE_VALUE); return *this; }
    File(const File&) = delete;
};
std::string Lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return s; }
bool Hex(std::string_view s) { return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
bool Relative(std::string_view s) {
    if (s.empty() || s.size() > 1024 || s.front() == '/' || s.back() == '/') return false;
    size_t start = 0;
    while (start < s.size()) {
        const size_t end = s.find('/', start); auto part = s.substr(start, end == s.npos ? s.size() - start : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        for (char c : part) if ((static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) >= 127) || c == '\\' || c == ':' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|' || c == '"') return false;
        if (end == s.npos) break;
        start = end + 1;
    }
    return true;
}
void Keys(const Value& v, std::initializer_list<const char*> keys) {
    Require(v.is_object() && v.size() == keys.size());
    for (auto k : keys) Require(v.find(k) != nullptr);
}
const Value& At(const Value& v, const char* k) { const auto* p = v.find(k); Require(p != nullptr); return *p; }
std::string Text(const Value& v) { Require(v.is_string()); const auto& s = v.as_string(); Require(s.find('\0') == s.npos); return s; }
int64_t Int(const Value& v, int64_t max = 512LL * 1024 * 1024) { int64_t n = 0; Require(v.get_int(n, 0, max)); return n; }
std::set<std::string> Strings(const Value& v) {
    Require(v.is_array() && v.size() <= 512); std::set<std::string> result;
    std::string previous;
    for (const auto& x : v.items()) { auto s = Text(x); Require(s == Lower(s) && (previous.empty() || previous < s)); Require(result.insert(s).second); previous = s; }
    return result;
}
std::wstring Final(HANDLE h) {
    std::wstring result(32768, 0); const DWORD n = GetFinalPathNameByHandleW(h, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
    Require(n && n < result.size(), ERROR_BAD_PATHNAME); result.resize(n);
    if (result.starts_with(LR"(\\?\)")) result.erase(0, 4);
    return result;
}
File Open(const fs::path& path, bool directory, bool pointer_snapshot = false) {
    File f(pointer_snapshot ? detail::OpenPointerSnapshot(path.c_str()) : CreateFileW(path.c_str(), directory ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES : GENERIC_READ,
        directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    Win(f.h != INVALID_HANDLE_VALUE); BY_HANDLE_FILE_INFORMATION info{};
    Win(GetFileInformationByHandle(f.h, &info) != FALSE);
    Require(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), ERROR_NOT_SUPPORTED);
    Require(!!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory, ERROR_DIRECTORY);
    // The pointer's directory entry may already have been atomically replaced;
    // its checked non-reparse handle still names the original complete snapshot.
    if(!pointer_snapshot) Require(_wcsicmp(Final(f.h).c_str(), path.c_str()) == 0, ERROR_BAD_PATHNAME);
    return f;
}
std::vector<unsigned char> Read(HANDLE h, size_t limit) {
    LARGE_INTEGER size{}, zero{}; Win(GetFileSizeEx(h, &size) != FALSE);
    Require(size.QuadPart >= 0 && static_cast<unsigned long long>(size.QuadPart) <= limit, ERROR_FILE_TOO_LARGE);
    Win(SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) != FALSE);
    std::vector<unsigned char> b(static_cast<size_t>(size.QuadPart)); size_t at = 0;
    while (at < b.size()) { DWORD got = 0; DWORD count = static_cast<DWORD>(std::min<size_t>(b.size() - at, 1024 * 1024));
        Win(ReadFile(h, b.data() + at, count, &got, nullptr) != FALSE); Require(got != 0, ERROR_HANDLE_EOF); at += got; }
    return b;
}
std::string Digest(const std::vector<unsigned char>& b) {
    struct Hash { BCRYPT_ALG_HANDLE a = nullptr; BCRYPT_HASH_HANDLE h = nullptr; ~Hash() { if(h) BCryptDestroyHash(h); if(a) BCryptCloseAlgorithmProvider(a,0); } } hash;
    Require(BCryptOpenAlgorithmProvider(&hash.a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, ERROR_FUNCTION_FAILED);
    Require(BCryptCreateHash(hash.a, &hash.h, nullptr, 0, nullptr, 0, 0) >= 0, ERROR_FUNCTION_FAILED);
    for (size_t at = 0; at < b.size();) { const ULONG count = static_cast<ULONG>(std::min<size_t>(b.size() - at, 1024 * 1024));
        Require(BCryptHashData(hash.h, const_cast<PUCHAR>(b.data() + at), count, 0) >= 0, ERROR_FUNCTION_FAILED); at += count; }
    std::array<unsigned char,32> bytes{}; Require(BCryptFinishHash(hash.h, bytes.data(), static_cast<ULONG>(bytes.size()),0) >= 0, ERROR_FUNCTION_FAILED);
    std::string result; for (auto c : bytes) { result += "0123456789abcdef"[c >> 4]; result += "0123456789abcdef"[c & 15]; } return result;
}
Value Parse(const std::vector<unsigned char>& b) { Value v; Require(json::Parse(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), v, nullptr, {1024 * 1024, 16})); return v; }
bool System(const std::string& s) {
    static const std::string names = R"( kernel32.dll kernelbase.dll ntdll.dll user32.dll gdi32.dll gdi32full.dll advapi32.dll ole32.dll oleaut32.dll oleacc.dll shell32.dll shlwapi.dll comdlg32.dll comctl32.dll uuid.dll rpcrt4.dll combase.dll imm32.dll winmm.dll version.dll uxtheme.dll dwmapi.dll shcore.dll d3d11.dll d2d1.dll dwrite.dll dxgi.dll dcomp.dll windowscodecs.dll gdiplus.dll bcrypt.dll bcryptprimitives.dll crypt32.dll cryptbase.dll secur32.dll sspicli.dll winhttp.dll wininet.dll ws2_32.dll dnsapi.dll iphlpapi.dll netapi32.dll mpr.dll propsys.dll cfgmgr32.dll setupapi.dll userenv.dll wtsapi32.dll powrprof.dll profapi.dll virtdisk.dll psapi.dll dbghelp.dll dbgcore.dll imagehlp.dll wintrust.dll normaliz.dll usp10.dll msvcrt.dll ucrtbase.dll mf.dll mfplat.dll mfreadwrite.dll mfuuid.dll mfplay.dll avrt.dll msacm32.dll msimg32.dll opengl32.dll glu32.dll winspool.drv hid.dll urlmon.dll cabinet.dll cryptui.dll d3d9.dll d3d12.dll dxcore.dll dinput8.dll wevtapi.dll wldap32.dll winusb.dll wlanapi.dll sensapi.dll authz.dll dwmapi.dll rstrtmgr.dll xmllite.dll )";
    if (names.find(" " + s + " ") != names.npos) return true;
    if (!(s.starts_with("api-ms-win-") || s.starts_with("ext-ms-win-")) || !s.ends_with(".dll")) return false;
    return std::all_of(s.begin(),s.end() - 4,[](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
// Bounded disk-image reader; never LoadLibrary or resolve imported code.
struct Pe {
    const std::vector<unsigned char>& b;
    struct Section { uint64_t va, size, raw; }; std::vector<Section> sections;
    uint64_t headers = 0, base = 0; unsigned machine = 0;
    std::vector<std::pair<uint64_t,uint64_t>> dirs;
    uint64_t Num(uint64_t at, unsigned n) const {
        Require(at <= b.size() && n <= b.size() - at, ERROR_BAD_EXE_FORMAT);
        uint64_t value = 0; for(unsigned i=0;i<n;++i) value |= uint64_t(b[static_cast<size_t>(at+i)]) << (i*8); return value;
    }
    uint64_t Offset(uint64_t rva, uint64_t count) const {
        if (rva < headers && count <= headers-rva) return rva;
        uint64_t result = 0; unsigned matches = 0;
        for(const auto& s:sections) if(rva >= s.va && rva-s.va <= s.size && count <= s.size-(rva-s.va)) { result=s.raw+rva-s.va; ++matches; }
        Require(matches==1,ERROR_BAD_EXE_FORMAT); return result;
    }
    explicit Pe(const std::vector<unsigned char>& bytes) : b(bytes) {
        Require(Num(0,2)==0x5a4d,ERROR_BAD_EXE_FORMAT); auto p=Num(0x3c,4); Require(Num(p,4)==0x4550,ERROR_BAD_EXE_FORMAT);
        machine=static_cast<unsigned>(Num(p+4,2)); const auto count=Num(p+6,2), size=Num(p+20,2), opt=p+24, magic=Num(opt,2);
        const bool x64=machine==0x8664 && magic==0x20b; Require(x64 || (machine==0x14c && magic==0x10b),ERROR_BAD_EXE_FORMAT);
        const uint64_t offset=x64?112:96; Require(count>=1 && count<=96 && size>=offset && opt<=b.size() && size<=b.size()-opt,ERROR_BAD_EXE_FORMAT);
        base=Num(opt+(x64?24:28),x64?8:4); headers=Num(opt+60,4);
        Require(headers && headers<=b.size() && opt+size+count*40<=headers,ERROR_BAD_EXE_FORMAT);
        auto n=Num(opt+offset-4,4); Require(n<=16 && offset+n*8<=size,ERROR_BAD_EXE_FORMAT);
        for(uint64_t i=0;i<n;++i) dirs.emplace_back(Num(opt+offset+i*8,4),Num(opt+offset+i*8+4,4));
        for(uint64_t i=0;i<count;++i) { auto s=opt+size+i*40; Section section{Num(s+12,4),Num(s+16,4),Num(s+20,4)};
            Require(section.raw<=b.size() && section.size<=b.size()-section.raw,ERROR_BAD_EXE_FORMAT);
            if(section.size) { Require(section.raw>=headers,ERROR_BAD_EXE_FORMAT); for(auto old:sections) Require(!(section.va<old.va+old.size && old.va<section.va+section.size),ERROR_BAD_EXE_FORMAT); }
            sections.push_back(section); }
    }
    std::set<std::string> Imports(bool delay) const {
        const size_t index=delay?13:1; std::set<std::string> names; if(index>=dirs.size()) return names;
        const auto [rva,size]=dirs[index]; if(!rva && !size) return names; const uint64_t stride=delay?32:20;
        Require(rva && size>=stride && size<=1024*1024,ERROR_BAD_EXE_FORMAT);
        for(uint64_t step=0;step+stride<=size;step+=stride) {
            auto at=Offset(rva+step,stride); bool any=false; for(unsigned i=0;i<stride;i+=4) any |= Num(at+i,4)!=0; if(!any) return names;
            auto name=Num(at+(delay?4:12),4); if(delay) { auto attr=Num(at,4); Require(attr<=1,ERROR_BAD_EXE_FORMAT); if(!attr) { Require(name>=base,ERROR_BAD_EXE_FORMAT); name-=base; } }
            Require(name!=0,ERROR_BAD_EXE_FORMAT); std::string s; bool terminated=false;
            for(unsigned i=0;i<=260;++i) { char c=static_cast<char>(Num(Offset(name+i,1),1)); if(!c) {terminated=true;break;} Require((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-',ERROR_BAD_EXE_FORMAT); s+=c; }
            s=Lower(s); Require(terminated && (s.ends_with(".dll")||s.ends_with(".drv")),ERROR_BAD_EXE_FORMAT); names.insert(s);
        } throw Failure{ERROR_BAD_EXE_FORMAT};
    }
};
}
struct BundleLease::State {
    std::wstring directory,launcher,host,runtime,ui;
    std::string digest; unsigned machine=0;
    std::vector<File> directories,files;
    std::vector<std::pair<std::wstring,BY_HANDLE_FILE_INFORMATION>> identities;
    std::set<std::string> expected;
    bool installed=false;
};
namespace {
void Keep(BundleLease::State& s, const fs::path& path, File f) {
    BY_HANDLE_FILE_INFORMATION id{}; Win(GetFileInformationByHandle(f.h,&id)!=FALSE);
    s.identities.emplace_back(path.wstring(),id); s.files.push_back(std::move(f));
}
void Enumerate(const BundleLease::State& s) {
    std::set<std::string> found; size_t count=0;
    for(auto it=fs::recursive_directory_iterator(s.directory);it!=fs::recursive_directory_iterator();++it) {
        const auto& item=*it;
        Require(++count<=2048,ERROR_FILE_TOO_LARGE); auto attr=GetFileAttributesW(item.path().c_str()); Win(attr!=INVALID_FILE_ATTRIBUTES);
        Require(!(attr&FILE_ATTRIBUTE_REPARSE_POINT),ERROR_NOT_SUPPORTED);
        const auto relative=Lower(json::Utf8(item.path().lexically_relative(s.directory).generic_wstring()));
        // Installer transaction backups and unowned legacy data are never component
        // search paths. Keep them outside the installed component identity set.
        if(s.installed && (attr&FILE_ATTRIBUTE_DIRECTORY) && (relative==".pulse-setup" || relative=="dialogbridge")) {
            it.disable_recursion_pending(); continue;
        }
        if(attr&FILE_ATTRIBUTE_DIRECTORY) { Require(relative=="licenses" || relative.starts_with("licenses/")); continue; }
        if(s.installed && (relative=="uninstall.exe" || relative=="setup-defaults.ini" || relative==".pulse-files")) continue;
        Require(found.insert(relative).second && s.expected.contains(relative));
    } Require(found==s.expected);
    for(const auto& [path,old]:s.identities) { auto f=Open(path,false); BY_HANDLE_FILE_INFORMATION now{}; Win(GetFileInformationByHandle(f.h,&now)!=FALSE);
        Require(now.dwVolumeSerialNumber==old.dwVolumeSerialNumber && now.nFileIndexHigh==old.nFileIndexHigh && now.nFileIndexLow==old.nFileIndexLow && now.nFileSizeHigh==old.nFileSizeHigh && now.nFileSizeLow==old.nFileSizeLow,ERROR_FILE_INVALID); }
}
}
BundleLease::BundleLease() noexcept = default;
BundleLease::~BundleLease() = default;
BundleLease::BundleLease(BundleLease&&) noexcept = default;
BundleLease& BundleLease::operator=(BundleLease&&) noexcept = default;
BundleLease::operator bool() const noexcept { return state_!=nullptr; }
#define ACCESSOR(name,field) const std::wstring& BundleLease::name() const noexcept { static const std::wstring empty; return state_?state_->field:empty; }
ACCESSOR(Directory,directory) ACCESSOR(Launcher,launcher) ACCESSOR(Host,host) ACCESSOR(Runtime,runtime) ACCESSOR(UiProvider,ui)
#undef ACCESSOR
const std::string& BundleLease::ManifestDigest() const noexcept { static const std::string empty; return state_?state_->digest:empty; }
unsigned BundleLease::Machine() const noexcept { return state_?state_->machine:0; }
DWORD RecheckDirectory(const BundleLease& lease) noexcept {
    try { Require(!!lease,ERROR_INVALID_HANDLE); Enumerate(*lease.state_); return ERROR_SUCCESS; }
    catch(const Failure& f) {return f.error;} catch(const fs::filesystem_error& e) {return static_cast<DWORD>(e.code().value());} catch(...) {return ERROR_NOT_ENOUGH_MEMORY;}
}
DWORD VerifyBundle(const std::wstring& directory, std::string_view expected_digest, BundleLease& output) noexcept {
    output=BundleLease{};
    try {
        Require(expected_digest.empty()||Hex(expected_digest));
        Require(directory.size()>=3 && directory[1]==L':' && directory[2]==L'\\' && directory.find(L'/',0)==directory.npos && directory.find(L':',2)==directory.npos && directory.find(L'\0')==directory.npos,ERROR_BAD_PATHNAME);
        fs::path root(directory); Require(root.is_absolute() && root.lexically_normal()==root,ERROR_BAD_PATHNAME);
        auto s=std::make_unique<BundleLease::State>(); auto path=root.root_path();
        s->directories.push_back(Open(path,true));
        for(const auto& part:root.relative_path()) { Require(part!=L"." && part!=L"..",ERROR_BAD_PATHNAME); path/=part; s->directories.push_back(Open(path,true)); }
        s->directory=Final(s->directories.back().h);
        auto mf=Open(root/L"manifest.json",false); auto raw=Read(mf.h,1024*1024); s->digest=Digest(raw);
        Require(expected_digest.empty()||s->digest==expected_digest,ERROR_REVISION_MISMATCH);
        auto manifest=Parse(raw); Keys(manifest,{"schema","version","architecture","contract","dynamic_helpers","system_dependencies","files"});
        const auto schema=Int(At(manifest,"schema")); Require(schema==1 || schema==2,ERROR_REVISION_MISMATCH); s->installed=schema==2;
        const auto version=Text(At(manifest,"version")); Require(!version.empty()&&version.size()<=64);
        Require(std::all_of(version.begin(),version.end(),[](char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-';}));
        Require(version[0]!='_'&&version[0]!='.'&&version[0]!='-');
        const auto arch=Text(At(manifest,"architecture")); Require(arch=="x64"||arch=="x86"); s->machine=arch=="x64"?0x8664:0x14c;
        const auto& contract=At(manifest,"contract"); Keys(contract,{"manifest","picker_ui_abi","runtime_session","open_policy","save_policy","experimental","capability_certification"});
        Require(Int(At(contract,"manifest"))==1 && Int(At(contract,"picker_ui_abi"))==2 && Int(At(contract,"runtime_session"))==1 && Text(At(contract,"open_policy"))=="LocalOpenV1" && Text(At(contract,"save_policy"))=="LocalSaveV1",ERROR_REVISION_MISMATCH);
        Require(At(contract,"experimental").is_bool()&&At(contract,"experimental").as_bool()&&At(contract,"capability_certification").is_bool()&&!At(contract,"capability_certification").as_bool());
        const auto& dynamic=At(manifest,"dynamic_helpers"); Keys(dynamic,{"Pulse.Preview.exe","pdfium.dll","pulse_elevated.exe"});
        Require(Text(At(dynamic,"Pulse.Preview.exe"))=="src/ui/thumbnail_cache.cpp: module-relative preview worker" && Text(At(dynamic,"pdfium.dll"))=="src/preview_host/pdf_raster.cpp: dynamic PDF renderer" && Text(At(dynamic,"pulse_elevated.exe"))=="src/ops/elevated_transfer_client.cpp: explicit elevated operation helper");
        Keep(*s,root/L"manifest.json",std::move(mf)); s->expected.insert("manifest.json");
        auto marker=Open(root/L"VERIFIED.json",false); auto mark=Parse(Read(marker.h,1024)); Keys(mark,{"manifest_sha256","integrity_only"});
        Require(Text(At(mark,"manifest_sha256"))==s->digest && At(mark,"integrity_only").is_bool()&&At(mark,"integrity_only").as_bool()); Keep(*s,root/L"VERIFIED.json",std::move(marker)); s->expected.insert("verified.json");
        const auto& entries=At(manifest,"files"); Require(entries.is_array()&&entries.size()>=1&&entries.size()<=512);
        std::set<std::string> binaries,dependencies,systems;
        for(const auto& item:entries.items()) {
            auto relative=Text(At(item,"path")), role=Text(At(item,"role")); Require(Relative(relative)); const bool license=role=="license";
            if(license) Keys(item,{"path","role","size","sha256"}); else Keys(item,{"path","role","size","sha256","machine","imports","delay_imports"});
            Require(license||role=="entry"||role=="import"||role=="dynamic-helper"); Require(license?relative.starts_with("licenses/"):relative.find('/')==relative.npos);
            Require(s->expected.insert(Lower(relative)).second); auto wide=json::Wide(relative); Require(!wide.empty()); auto full=root/fs::path(wide);
            // Manifest separators are '/', while Win32 final paths use '\'.
            // Preserve the validated components, changing only separator spelling
            // before comparing the opened handle's canonical path.
            full.make_preferred();
            if(license) { auto parent=root; for(const auto& part:fs::path(wide).parent_path()) {parent/=part;s->directories.push_back(Open(parent,true));} }
            auto f=Open(full,false); auto bytes=Read(f.h,512*1024*1024); Require(Int(At(item,"size"))==static_cast<int64_t>(bytes.size()));
            auto hash=Text(At(item,"sha256")); Require(Hex(hash)&&hash==Digest(bytes),ERROR_CRC);
            if(!license) { Pe pe(bytes); Require(pe.machine==s->machine && Int(At(item,"machine"))==pe.machine,ERROR_EXE_MACHINE_TYPE_MISMATCH);
                auto normal=pe.Imports(false), delay=pe.Imports(true); Require(normal==Strings(At(item,"imports"))&&delay==Strings(At(item,"delay_imports")));
                dependencies.insert(normal.begin(),normal.end()); dependencies.insert(delay.begin(),delay.end()); binaries.insert(Lower(relative)); }
            Keep(*s,full,std::move(f));
        }
        const std::string automatic=arch=="x64"?"pulsedialogauto64.dll":"pulsedialogauto32.dll";
        const std::string wrong_automatic=arch=="x64"?"pulsedialogauto32.dll":"pulsedialogauto64.dll";
        Require(!binaries.contains(wrong_automatic));
        Require(binaries.contains(automatic)==binaries.contains("pulsedialogautocontrol.exe"));
        const std::string runtime=arch=="x64"?"PulseFileDialogRuntimeUiProof64R8.dll":"PulseFileDialogRuntimeUiProof32R8.dll";
        for(const auto& name:std::vector<std::string>{"PulseDialogLauncher.exe",runtime,"pulse.exe","PulsePickerUiProof.dll","lumatext.dll","pulse_shell.exe","Pulse.Preview.exe","pdfium.dll","pulse_elevated.exe"}) Require(binaries.contains(Lower(name)),ERROR_MOD_NOT_FOUND);
        for(const auto& dep:dependencies) { if(System(dep)) {Require(!binaries.contains(dep));systems.insert(dep);} else Require(binaries.contains(dep),ERROR_MOD_NOT_FOUND); }
        Require(systems==Strings(At(manifest,"system_dependencies")));
        for(auto name:{"licenses/pulse/license","licenses/detours/license.md","licenses/ib-pinyin/license.txt","licenses/md4c/license.md","licenses/sqlite/readme.md","licenses/lzma/readme.pulse.md"}) Require(s->expected.contains(name),ERROR_FILE_NOT_FOUND);
        for(auto group:{"licenses/lumatext/","licenses/pdfium/"}) Require(std::any_of(s->expected.begin(),s->expected.end(),[&](const auto& x){return x.starts_with(group);}),ERROR_FILE_NOT_FOUND);
        s->launcher=(root/L"PulseDialogLauncher.exe").wstring(); s->host=(root/L"pulse.exe").wstring(); s->runtime=(root/json::Wide(runtime)).wstring(); s->ui=(root/L"PulsePickerUiProof.dll").wstring();
        Enumerate(*s); output.state_=std::move(s); return ERROR_SUCCESS;
    } catch(const Failure& f) {return f.error;} catch(const fs::filesystem_error& e) {return static_cast<DWORD>(e.code().value());} catch(...) {return ERROR_NOT_ENOUGH_MEMORY;}
}
DWORD ResolveInstalledBundle(const std::wstring& directory, BundleLease& output) noexcept {
    output=BundleLease{};
    try {
        Require(directory.size()>=3 && directory[1]==L':' && directory[2]==L'\\' &&
            directory.find(L'/')==directory.npos && directory.find(L':',2)==directory.npos &&
            directory.find(L'\0')==directory.npos,ERROR_BAD_PATHNAME);
        const fs::path root(directory);
        Require(root.is_absolute() && root.lexically_normal()==root,ERROR_BAD_PATHNAME);
        std::vector<File> ancestors;
        auto path=root.root_path(); ancestors.push_back(Open(path,true));
        for(const auto& part:root.relative_path()) {path/=part;ancestors.push_back(Open(path,true));}
        std::string digest, relative;
        {
            auto pointer=Open(root/L"dialog-current.json",false,true);
            const auto value=Parse(Read(pointer.h,1024));
            Keys(value,{"schema","directory","manifest_sha256"});
            Require(Int(At(value,"schema"))==1,ERROR_REVISION_MISMATCH);
            digest=Text(At(value,"manifest_sha256"));
            relative=Text(At(value,"directory"));
            Require(Hex(digest) && relative=="DialogBridge/"+digest,ERROR_BAD_PATHNAME);
        }
        auto selected=root/json::Wide(relative); selected.make_preferred();
        BundleLease lease;
        const DWORD error=VerifyBundle(selected.wstring(),digest,lease);
        if(error) return error;
        Require(!lease.state_->installed,ERROR_REVISION_MISMATCH);
        output=std::move(lease);
        return ERROR_SUCCESS;
    } catch(const Failure& f) {return f.error;} catch(const fs::filesystem_error& e) {return static_cast<DWORD>(e.code().value());} catch(...) {return ERROR_NOT_ENOUGH_MEMORY;}
}
}
