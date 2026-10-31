#include "../ops/transfer_engine.h"
#include "../ops/owned_transfer_output.h"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <set>
#include <objbase.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>
using namespace pulse::ops;
namespace fs = std::filesystem;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
struct AclRestore {
    std::wstring path;
    PSECURITY_DESCRIPTOR original = nullptr;
    PACL old_acl = nullptr;
    DWORD protection = 0;
    bool changed = false;
    explicit AclRestore(const fs::path& p) : path(p.wstring()) {
        if (GetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &old_acl, nullptr, &original)) throw std::runtime_error("ACL snapshot");
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
        if (!GetSecurityDescriptorControl(original, &control, &revision)) {
            LocalFree(original); original = nullptr; throw std::runtime_error("ACL control snapshot");
        }
        protection = control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
    }
    ~AclRestore() {
        if (changed && !Restore()) { ++failures; fwprintf(stderr, L"[FAIL] ACL restore: %ls\n", path.c_str()); }
        if (original) LocalFree(original);
    }
    void Set(PSID sid, DWORD permissions, ACCESS_MODE mode, DWORD inherit, bool replace) {
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = permissions; entry.grfAccessMode = mode; entry.grfInheritance = inherit;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID; entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &entry, replace ? nullptr : old_acl, &acl)) throw std::runtime_error("construct fixture ACL");
        const DWORD result = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | (replace ? PROTECTED_DACL_SECURITY_INFORMATION : protection),
            nullptr, nullptr, acl, nullptr);
        LocalFree(acl);
        if (result) throw std::runtime_error("apply fixture ACL");
        changed = true;
    }
    bool Restore() {
        const DWORD result = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | protection,
            nullptr, nullptr, old_acl, nullptr);
        if (!result) changed = false;
        return result == ERROR_SUCCESS;
    }
};
std::wstring Security(const fs::path& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto name = path.wstring();
    if (GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &descriptor)) throw std::runtime_error("read owner/DACL");
    LPWSTR text = nullptr;
    const BOOL ok = ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &text, nullptr);
    LocalFree(descriptor);
    if (!ok) throw std::runtime_error("owner/DACL SDDL");
    std::wstring result(text); LocalFree(text); return result;
}
std::array<unsigned char,32> Sha(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    BCRYPT_ALG_HANDLE provider = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    struct Cleanup { HANDLE& f; BCRYPT_ALG_HANDLE& p; BCRYPT_HASH_HANDLE& h;
        ~Cleanup() { if (f != INVALID_HANDLE_VALUE) CloseHandle(f); if (h) BCryptDestroyHash(h); if (p) BCryptCloseAlgorithmProvider(p,0); }
    } cleanup{file,provider,hash};
    if (file == INVALID_HANDLE_VALUE || BCryptOpenAlgorithmProvider(&provider,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 ||
        BCryptCreateHash(provider,&hash,nullptr,0,nullptr,0,0)<0) throw std::runtime_error("SHA open");
    std::vector<unsigned char> buffer(1<<20);
    for (;;) { DWORD got=0; if (!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&got,nullptr)) throw std::runtime_error("SHA read");
        if (!got) break;
        if (BCryptHashData(hash,buffer.data(),got,0)<0) throw std::runtime_error("SHA update"); }
    std::array<unsigned char,32> digest{};
    if (BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw std::runtime_error("SHA finish");
    return digest;
}
SmallCopyResult Copy(const fs::path& source, const fs::path& target, bool verify) {
    std::atomic<bool> cancel{false},pause{false}; std::mutex mutex; std::condition_variable cv;
    SmallCopyPool pool(2,cancel,pause,mutex,cv);
    SmallCopyJob job{0,source.wstring(),target.wstring(),FILE_ATTRIBUTE_NORMAL,false,2ull<<20};
    job.verify=verify; job.staged=verify;
    pool.Submit(job);
    const auto deadline=GetTickCount64()+30000;
    std::vector<SmallCopyResult> results;
    while (pool.Outstanding() && GetTickCount64()<deadline) {
        auto batch=pool.Take(true); results.insert(results.end(),batch.begin(),batch.end());
    }
    if (pool.Outstanding()) { pool.Stop(); throw std::runtime_error("pool timeout"); }
    if (results.size()!=1 || !results.front().ran) throw std::runtime_error("missing copy result");
    return results.front();
}
using CertificateSet = std::set<std::array<unsigned char,32>>;
HCERTSTORE OpenPersonalStore() {
    return CertOpenStore(CERT_STORE_PROV_SYSTEM_W,0,0,
        CERT_SYSTEM_STORE_CURRENT_USER|CERT_STORE_READONLY_FLAG|CERT_STORE_OPEN_EXISTING_FLAG,L"My");
}
CertificateSet CertificateFingerprints() {
    HCERTSTORE store=OpenPersonalStore();
    if (!store) throw std::runtime_error("read-only certificate snapshot open");
    CertificateSet result;
    PCCERT_CONTEXT certificate=nullptr;
    bool ok=true;
    while ((certificate=CertEnumCertificatesInStore(store,certificate))!=nullptr) {
        std::array<unsigned char,32> fingerprint{}; DWORD bytes=static_cast<DWORD>(fingerprint.size());
        if (!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM,0,nullptr,certificate->pbCertEncoded,
            certificate->cbCertEncoded,fingerprint.data(),&bytes) || bytes!=fingerprint.size()) {ok=false; CertFreeCertificateContext(certificate); break;}
        result.insert(fingerprint);
    }
    if (ok && GetLastError()!=CRYPT_E_NOT_FOUND && GetLastError()!=ERROR_NO_MORE_FILES) ok=false;
    CertCloseStore(store,0);
    if (!ok) throw std::runtime_error("certificate fingerprint snapshot");
    return result;
}
std::vector<unsigned char> DefaultEfsHash(DWORD& error) {
    std::vector<unsigned char> hash(20); DWORD bytes=static_cast<DWORD>(hash.size());
    error=RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows NT\\CurrentVersion\\EFS\\CurrentKeys",
        L"CertificateHash",RRF_RT_REG_BINARY,nullptr,hash.data(),&bytes);
    if (!error && bytes!=hash.size()) error=ERROR_INVALID_DATA;
    if (error) hash.clear();
    return hash;
}
bool HasEfsUsage(PCCERT_CONTEXT certificate) {
    DWORD bytes=0;
    if (!CertGetEnhancedKeyUsage(certificate,0,nullptr,&bytes)) return false;
    std::vector<unsigned char> buffer(bytes);
    auto* usage=reinterpret_cast<PCERT_ENHKEY_USAGE>(buffer.data());
    if (!CertGetEnhancedKeyUsage(certificate,0,usage,&bytes)) return false;
    for (DWORD i=0;i<usage->cUsageIdentifier;++i)
        if (strcmp(usage->rgpszUsageIdentifier[i],"1.3.6.1.4.1.311.10.3.4")==0) return true;
    return false;
}
struct EfsStoreInvariant {
    CertificateSet before=CertificateFingerprints();
    DWORD default_error=0;
    std::vector<unsigned char> default_hash=DefaultEfsHash(default_error);
    ~EfsStoreInvariant() {
        try {
            DWORD after_error=0; const auto after_hash=DefaultEfsHash(after_error);
            Check(CertificateFingerprints()==before,"CurrentUser/My certificate fingerprint set unchanged");
            Check(after_error==default_error && after_hash==default_hash,"current default EFS certificate selection unchanged");
        } catch (const std::exception& error) {++failures; printf("[FAIL] EFS state snapshot: %s\n",error.what());}
    }
};
bool Encrypted(const fs::path& path) {
    const DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_ENCRYPTED)!=0;
}
void ReportEfsAndCloud(const fs::path& root, const fs::path& original) {
    wchar_t volume[32768]{}, filesystem[MAX_PATH]{}; DWORD flags=0;
    if (!GetVolumePathNameW(root.c_str(),volume,32768) ||
        !GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,&flags,filesystem,MAX_PATH)) throw std::runtime_error("volume capabilities");
    printf("[INFO] filesystem=%ls flags=0x%08lx encryption_capable=%d\n",filesystem,flags,(flags&FILE_SUPPORTS_ENCRYPTION)?1:0);
    printf("[SKIP] cloud placeholder: no connected-provider fixture supplied; no fake placeholder created\n");
    EfsStoreInvariant invariant;
    HCERTSTORE store=OpenPersonalStore();
    if (!store) throw std::runtime_error("personal store read-only open");
    PCCERT_CONTEXT certificate=nullptr;
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key=0; DWORD key_spec=0; BOOL release=FALSE;
    struct Release {
        HCERTSTORE& store; PCCERT_CONTEXT& cert; HCRYPTPROV_OR_NCRYPT_KEY_HANDLE& key; DWORD& spec; BOOL& owned;
        ~Release() {
            if (owned && key) {if (spec==CERT_NCRYPT_KEY_SPEC) NCryptFreeObject(key); else CryptReleaseContext(key,0);}
            if (cert) CertFreeCertificateContext(cert);
            if (store) CertCloseStore(store,0);
        }
    } cleanup{store,certificate,key,key_spec,release};
    unsigned efs_count=0;
    while ((certificate=CertEnumCertificatesInStore(store,certificate))!=nullptr) if (HasEfsUsage(certificate)) ++efs_count;
    printf("[INFO] current_user_my_efs_certificates=%u\n",efs_count);
    if (!(flags&FILE_SUPPORTS_ENCRYPTION)) {printf("[SKIP] EFS: destination volume lacks encryption capability\n");return;}
    if (invariant.default_error) {
        printf("[SKIP] EFS: existing current default certificate not confirmed; registry_error=%lu; EncryptFileW not called\n",invariant.default_error);return;
    }
    CRYPT_HASH_BLOB hash{static_cast<DWORD>(invariant.default_hash.size()),invariant.default_hash.data()};
    certificate=CertFindCertificateInStore(store,X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,0,CERT_FIND_SHA1_HASH,&hash,nullptr);
    if (!certificate || !HasEfsUsage(certificate) || CertVerifyTimeValidity(nullptr,certificate->pCertInfo)!=0) {
        printf("[SKIP] EFS: current default is missing, expired, or lacks explicit EFS EKU; no certificate selected/generated\n");return;
    }
    if (!CryptAcquireCertificatePrivateKey(certificate,CRYPT_ACQUIRE_SILENT_FLAG|CRYPT_ACQUIRE_NO_HEALING|CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG,
        nullptr,&key,&key_spec,&release)) {
        printf("[SKIP] EFS: current default private key unavailable silently; error=%lu; EncryptFileW not called\n",GetLastError());return;
    }
    // Never set the default, enroll/import a certificate, or request interactive key access.
    const auto source=root/L"efs-source.bin", destination=root/L"efs-target";
    if (!fs::create_directory(destination)) throw std::runtime_error("EFS fixture directory");
    COPYFILE2_EXTENDED_PARAMETERS parameters{}; parameters.dwSize=sizeof(parameters); parameters.dwCopyFlags=COPY_FILE_FAIL_IF_EXISTS|COPY_FILE_COPY_SYMLINK;
    const HRESULT fixture=CopyFile2(original.c_str(),source.c_str(),&parameters);
    if (FAILED(fixture)) {printf("[FAIL] EFS fixture copy hr=0x%08lx\n",static_cast<unsigned long>(fixture));++failures;return;}
    if (Encrypted(source)) throw std::runtime_error("EFS fixture must start as an ordinary unencrypted file");
    const auto expected=Sha(source);
    if (!EncryptFileW(source.c_str())) {printf("[FAIL] EncryptFileW existing-key fixture error=%lu\n",GetLastError());++failures;return;}
    Check(Encrypted(source) && Sha(source)==expected,"EFS source is encrypted and decrypts to unchanged full SHA256");
    const auto baseline=destination/L"native.bin";
    const HRESULT native=CopyFile2(source.c_str(),baseline.c_str(),&parameters);
    if (FAILED(native)) {printf("[FAIL] EFS native baseline hr=0x%08lx\n",static_cast<unsigned long>(native));++failures;return;}
    Check(Encrypted(baseline) && Sha(baseline)==expected,"native CopyFile2 retains encrypted state and decryptable SHA256");
    for (bool verify : {false,true}) {
        const auto stage=destination/(verify?L"verify-stage.bin":L"plain.bin");
        const auto final=verify?destination/L"verify-final.bin":stage;
        auto result=Copy(source,stage,verify);
        printf("[INFO] EFS pool verify=%d hr=0x%08lx\n",verify?1:0,static_cast<unsigned long>(result.hr));
        Check(SUCCEEDED(result.hr),"EFS pooled copy succeeds");
        if (FAILED(result.hr)) continue;
        if (verify) {
            DWORD error=0;
            const bool committed=result.verification && result.verification->Success() && result.owned_output &&
                CommitVerifiedOutput(stage.wstring(),final.wstring(),*result.owned_output,error);
            Check(committed,"EFS verified stage commits by owned identity");
            if (!committed) {printf("[INFO] EFS commit error=%lu\n",error);continue;}
        }
        Check(Encrypted(final)==Encrypted(baseline) && Encrypted(final) && Sha(final)==expected &&
              Encrypted(source) && Sha(source)==expected,"EFS final/native/source encrypted state and complete plaintext SHA256 agree");
    }
}

}
int wmain() {
    fs::path root;
    try {
        GUID id{}; wchar_t guid[40]{};
        if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("GUID");
        StringFromGUID2(id,guid,40);
        root=fs::absolute(fs::path(L"bench_data")/(std::wstring(L"copy_security_")+guid));
        if (!fs::create_directories(root)) throw std::runtime_error("unique fixture");
        const auto source=root/L"source.bin", parent=root/L"target";
        fs::create_directory(parent);
        HANDLE token=nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) throw std::runtime_error("token");
        DWORD bytes=0; GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
        std::vector<unsigned char> token_data(bytes);
        const BOOL token_ok=bytes && GetTokenInformation(token,TokenUser,token_data.data(),bytes,&bytes); CloseHandle(token);
        if (!token_ok) throw std::runtime_error("token SID");
        PSID sid=reinterpret_cast<TOKEN_USER*>(token_data.data())->User.Sid;
        HANDLE file=CreateFileW(source.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE) throw std::runtime_error("source fixture");
        std::vector<unsigned char> block(1<<20);
        for (size_t i=0;i<block.size();++i) block[i]=static_cast<unsigned char>((i*37+i/251)&255);
        bool wrote=true;
        for (int i=0;i<2;++i) {DWORD written=0; wrote=wrote && WriteFile(file,block.data(),static_cast<DWORD>(block.size()),&written,nullptr) && written==block.size();}
        CloseHandle(file); if (!wrote) throw std::runtime_error("source fixture write");
        AclRestore source_acl(source),parent_acl(parent);
        source_acl.Set(sid,FILE_ALL_ACCESS,SET_ACCESS,NO_INHERITANCE,true);
        parent_acl.Set(sid,FILE_ALL_ACCESS,GRANT_ACCESS,SUB_CONTAINERS_AND_OBJECTS_INHERIT,false);
        const auto source_security=Security(source);
        const auto source_hash=Sha(source);
        const auto baseline=parent/L"native.bin";
        COPYFILE2_EXTENDED_PARAMETERS parameters{}; parameters.dwSize=sizeof(parameters); parameters.dwCopyFlags=COPY_FILE_FAIL_IF_EXISTS|COPY_FILE_COPY_SYMLINK;
        if (FAILED(CopyFile2(source.c_str(),baseline.c_str(),&parameters))) throw std::runtime_error("native ACL baseline");
        const auto baseline_security=Security(baseline);
        for (bool verify : {false,true}) {
            const auto staged=parent/(verify?L"verified-stage.bin":L"plain.bin");
            const auto final=verify?parent/L"verified-final.bin":staged;
            auto result=Copy(source,staged,verify);
            Check(SUCCEEDED(result.hr),verify?"verified copy succeeds with protected source DACL":"plain copy succeeds with protected source DACL");
            if (FAILED(result.hr)) continue;
            if (verify) {
                DWORD error=0;
                Check(result.verification && result.verification->Success() && result.owned_output &&
                    CommitVerifiedOutput(staged.wstring(),final.wstring(),*result.owned_output,error),"verified output commits by owned identity");
                if (!fs::exists(final)) continue;
            }
            Check(Security(final)==baseline_security,"final owner and DACL match same-directory native CopyFile2 baseline");
            Check(Sha(final)==source_hash && Sha(baseline)==source_hash && Sha(source)==source_hash,"full source/target SHA256 equals native baseline");
            Check(Security(source)==source_security,"protected source DACL and owner unchanged");
        }
        for (bool deny_source : {true,false}) {
            AclRestore denied(deny_source?source:parent);
            denied.Set(sid,deny_source?FILE_READ_DATA:FILE_ADD_FILE,DENY_ACCESS,NO_INHERITANCE,false);
            for (bool verify : {false,true}) {
                const auto staged=parent/(std::wstring(deny_source?L"source-denied-":L"target-denied-")+(verify?L"verify.bin":L"plain.bin"));
                const auto final=parent/(staged.filename().wstring()+L".published");
                const auto native=parent/(staged.filename().wstring()+L".native");
                const HRESULT native_hr=CopyFile2(source.c_str(),native.c_str(),&parameters);
                auto result=Copy(source,staged,verify);
                Check(FAILED(native_hr) && FAILED(result.hr) && HRESULT_CODE(result.hr)==ERROR_ACCESS_DENIED,
                    deny_source?"real source read denial fails baseline and pool":"real target create denial fails baseline and pool");
                if (result.owned_output && fs::exists(staged)) {
                    DWORD error=0; Check(DeleteVerifiedOutput(staged.wstring(),*result.owned_output,error),"owned failed stage safely removed");
                }
                Check(!fs::exists(staged) && !fs::exists(final) && !fs::exists(native),"permission failure publishes no partial destination");
            }
            Check(denied.Restore(),"temporary denial DACL restored");
            Check(Sha(source)==source_hash && Security(source)==source_security,"permission failures preserve source content and protected DACL");
        }
        ReportEfsAndCloud(root,source);
        Check(parent_acl.Restore() && source_acl.Restore(),"original fixture DACLs restored");
        wprintf(L"[INFO] retained fixture: %ls\n",root.c_str());
    } catch (const std::exception& error) {
        ++failures; fprintf(stderr,"[FAIL] %s fixture=%ls\n",error.what(),root.c_str());
    }
    return failures?1:0;
}
