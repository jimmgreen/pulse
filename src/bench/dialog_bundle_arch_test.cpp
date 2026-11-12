#include "../common/dialog_bundle.h"
#include "../common/json_value.h"
#include <filesystem>
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
namespace {
namespace fs=std::filesystem;
namespace bundle=pulse::dialog_bundle;
int checks=0,failures=0;
void Check(bool ok,const char* name,DWORD error=0) {
    ++checks;if(!ok)++failures;
    std::cout<<(ok?"[PASS] ":"[FAIL] ")<<name<<" error="<<error<<'\n';
}
std::string Read(const fs::path& p) {std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void Write(const fs::path& p,const std::string& s) {std::ofstream f(p,std::ios::binary|std::ios::trunc);f<<s;if(!f)throw std::runtime_error("write");}
std::string Hash(const std::string& bytes) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::array<unsigned char,32> result{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("hash algorithm");
    const bool ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0 &&
        BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),static_cast<ULONG>(bytes.size()),0)>=0 &&
        BCryptFinishHash(hash,result.data(),static_cast<ULONG>(result.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    if(!ok)throw std::runtime_error("hash");
    std::string text;for(const auto value:result){text+="0123456789abcdef"[value>>4];text+="0123456789abcdef"[value&15];}return text;
}
DWORD Resolve(const fs::path& app,unsigned machine,bundle::BundleLease& out) {
#ifdef PULSE_BUNDLE_SINGLE_BASELINE
    // Original production resolver had only the primary pointer; the adapter
    // adds the caller's requested-machine check, not companion support.
    out=bundle::BundleLease{};
    if(machine!=IMAGE_FILE_MACHINE_I386 && machine!=IMAGE_FILE_MACHINE_AMD64) return ERROR_INVALID_PARAMETER;
    bundle::BundleLease primary;
    const DWORD error=bundle::ResolveInstalledBundle(app.wstring(),primary);
    if(error) return error;
    if(primary.Machine()!=machine) return ERROR_EXE_MACHINE_TYPE_MISMATCH;
    out=std::move(primary);return ERROR_SUCCESS;
#else
    return bundle::ResolveInstalledBundleForMachine(app.wstring(),machine,out);
#endif
}
}
int wmain(int argc,wchar_t** argv) {
    std::cout<<std::unitbuf;
    if(argc!=3) return 2;
    try {
        const fs::path source(argv[1]),app(argv[2]);
        if(!source.is_absolute() || !app.is_absolute() || fs::exists(app)) return 2;
        fs::copy(source,app,fs::copy_options::recursive);
        const auto main=Read(app/L"dialog-current.json"),x86=Read(app/L"dialog-x86-current.json");
        Check(!main.empty()&&!x86.empty(),"dual fixture pointers exist");
        auto expect=[&](unsigned machine,bool success,const char* label,DWORD expected=ERROR_SUCCESS) {
            bundle::BundleLease lease;
            const DWORD error=Resolve(app,machine,lease);
            Check(success ? !error && lease && lease.Machine()==machine : error && !lease && (!expected || error==expected),label,error);
            if(success && !error) Check(!bundle::RecheckDirectory(lease),"selected closure remains leased");
        };
        expect(IMAGE_FILE_MACHINE_AMD64,true,"primary x64 resolves");
        expect(IMAGE_FILE_MACHINE_I386,true,"companion x86 resolves independently");
        fs::rename(app/L"dialog-x86-current.json",app/L"saved-x86-pointer");
        expect(IMAGE_FILE_MACHINE_AMD64,true,"legacy single-architecture pointer remains supported");
        expect(IMAGE_FILE_MACHINE_I386,false,"missing x86 companion never uses x64",0);
        fs::rename(app/L"saved-x86-pointer",app/L"dialog-x86-current.json");
        Write(app/L"dialog-x86-current.json",main);
        expect(IMAGE_FILE_MACHINE_I386,false,"companion architecture mismatch rejects",ERROR_EXE_MACHINE_TYPE_MISMATCH);
        Write(app/L"dialog-x86-current.json",x86);
        Write(app/L"dialog-current.json","{}");
        expect(IMAGE_FILE_MACHINE_I386,false,"invalid primary blocks even valid companion",0);
        Write(app/L"dialog-current.json",main);
        pulse::json::Value invalid; if(!pulse::json::Parse(x86,invalid)) throw std::runtime_error("pointer parse");
        invalid.set("directory","DialogBridge/../outside");
        Write(app/L"dialog-x86-current.json",pulse::json::Serialize(invalid));
        expect(IMAGE_FILE_MACHINE_I386,false,"companion escape rejects",0);
        Write(app/L"dialog-x86-current.json","{\"schema\":1,"+x86.substr(1));
        expect(IMAGE_FILE_MACHINE_I386,false,"duplicate companion pointer keys reject",0);
        Write(app/L"dialog-x86-current.json",x86);
        {
            pulse::json::Value pointer,manifest;
            if(!pulse::json::Parse(x86,pointer))throw std::runtime_error("x86 pointer");
            const auto previous=app/pulse::json::Wide(pointer.find("directory")->as_string());
            if(!pulse::json::Parse(Read(previous/L"manifest.json"),manifest))throw std::runtime_error("x86 manifest");
            manifest.set("version","mismatched-valid-v1");const auto raw=pulse::json::Serialize(manifest),digest=Hash(raw);
            const auto other=app/L"DialogBridge"/pulse::json::Wide(digest);
            fs::copy(previous,other,fs::copy_options::recursive);
            Write(other/L"manifest.json",raw);
            auto marker=pulse::json::Value::Object();marker.set("manifest_sha256",digest);marker.set("integrity_only",true);
            Write(other/L"VERIFIED.json",pulse::json::Serialize(marker));
            {bundle::BundleLease valid;Check(!bundle::VerifyBundle(other.wstring(),digest,valid),"other-version x86 closure is independently valid");}
            pointer.set("directory","DialogBridge/"+digest);pointer.set("manifest_sha256",digest);
            Write(app/L"dialog-x86-current.json",pulse::json::Serialize(pointer));
            expect(IMAGE_FILE_MACHINE_I386,false,"mixed primary and companion versions reject",ERROR_REVISION_MISMATCH);
            Write(app/L"dialog-x86-current.json",x86);
        }
        expect(IMAGE_FILE_MACHINE_ARM64,false,"unknown machine rejects",ERROR_INVALID_PARAMETER);
        {
            bundle::BundleLease output;
            const DWORD first=Resolve(app,IMAGE_FILE_MACHINE_AMD64,output);
            Write(app/L"dialog-x86-current.json","{}");
            const DWORD second=Resolve(app,IMAGE_FILE_MACHINE_I386,output);
            Check(!first&&second&&!output,"failure clears earlier architecture lease",second);
        }
        Write(app/L"dialog-x86-current.json",x86);
        Check(Read(source/L"dialog-current.json")==main&&Read(source/L"dialog-x86-current.json")==x86,"source immutable pointers unchanged");
        std::cout<<"Bundle architecture checks="<<checks<<" failures="<<failures<<'\n';
        return failures?1:0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 2;}
}

