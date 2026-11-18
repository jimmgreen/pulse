#include "../app/extensions/package_signature.h"
#include "../app/extensions/package_revision.h"
#include "../common/json_value.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <vector>
#pragma comment(lib,"bcrypt.lib")
namespace {
std::string Hex(const std::vector<UCHAR>& bytes){constexpr char digits[]="0123456789abcdef";std::string result;for(auto b:bytes){result+=digits[b>>4];result+=digits[b&15];}return result;}
struct Keys { BCRYPT_ALG_HANDLE algorithm{};BCRYPT_KEY_HANDLE key{};~Keys(){if(key)BCryptDestroyKey(key);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);} };
}
int main(){
    using namespace pulse;int failures=0;auto check=[&](bool ok,const char* name){std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;};
    Keys keys;check(BCryptOpenAlgorithmProvider(&keys.algorithm,BCRYPT_RSA_ALGORITHM,nullptr,0)>=0&&BCryptGenerateKeyPair(keys.algorithm,&keys.key,2048,0)>=0&&BCryptFinalizeKeyPair(keys.key,0)>=0,"ephemeral CNG RSA key pair");if(!keys.key)return 1;
    ULONG size=0;BCryptExportKey(keys.key,nullptr,BCRYPT_RSAPUBLIC_BLOB,nullptr,0,&size,0);std::vector<UCHAR> blob(size);
    check(BCryptExportKey(keys.key,nullptr,BCRYPT_RSAPUBLIC_BLOB,blob.data(),size,&size,0)>=0,"export temporary public key");
    const std::string manifest="{\"apiVersion\":1,\"id\":\"fixture\",\"name\":\"Fixture\",\"commands\":[]}";
    constexpr char prefix[]="Pulse.Extension.Manifest.Signature\0v1\0";std::string payload(prefix,sizeof(prefix)-1);payload+=manifest;
    const auto digest=extensions::PackageRevision(payload);std::vector<UCHAR> hash;
    for(size_t i=0;i<digest.size();i+=2)hash.push_back(static_cast<UCHAR>(std::stoul(digest.substr(i,2),nullptr,16)));
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};size=0;BCryptSignHash(keys.key,&padding,hash.data(),static_cast<ULONG>(hash.size()),nullptr,0,&size,BCRYPT_PAD_PKCS1);std::vector<UCHAR> signature(size);
    check(BCryptSignHash(keys.key,&padding,hash.data(),static_cast<ULONG>(hash.size()),signature.data(),size,&size,BCRYPT_PAD_PKCS1)>=0,"sign domain and raw manifest bytes with CNG");
    const auto pin=extensions::PackageRevision(std::string_view(reinterpret_cast<const char*>(blob.data()),blob.size()));
    auto signed_value=json::Value::Object();signed_value.set("formatVersion",1);signed_value.set("algorithm","RSA-PKCS1-SHA256");signed_value.set("publisher",pin);signed_value.set("signature",Hex(signature));
    auto policy=json::Value::Object();policy.set("formatVersion",1);policy.set("requireSigned",true);auto publishers=json::Value::Array();auto publisher=json::Value::Object();publisher.set("id",pin);publisher.set("publicKey",Hex(blob));publishers.push(publisher);policy.set("publishers",publishers);
    std::string error;const auto signed_json=json::Serialize(signed_value),policy_json=json::Serialize(policy);
    check(extensions::VerifyPackageSignature(manifest,signed_json,policy_json,error),"trusted valid signature accepted");
    check(!extensions::VerifyPackageSignature(manifest+" ",signed_json,policy_json,error),"raw-byte whitespace tamper rejected");
    check(!extensions::VerifyPackageSignature(manifest,{},policy_json,error)&&error=="signature_required","required unsigned rejected");
    check(extensions::VerifyPackageSignature(manifest,{},{},error),"unsigned development remains compatible");
    check(!extensions::VerifyPackageSignature(manifest,signed_json,{},error)&&error=="unknown_publisher","untrusted signed package rejected");
    signed_value.set("formatVersion",2);check(!extensions::VerifyPackageSignature(manifest,json::Serialize(signed_value),policy_json,error),"unsupported signature version rejected");
    check(!extensions::VerifyPackageSignature(manifest,signed_json,"{}",error),"malformed policy rejected");
    signed_value.set("formatVersion",1);signed_value.set("signature",std::string(512,'0'));check(!extensions::VerifyPackageSignature(manifest,json::Serialize(signed_value),policy_json,error),"invalid RSA signature rejected");
    return failures?1:0;
}
