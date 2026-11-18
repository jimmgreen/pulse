#include "package_signature.h"
#include "package_revision.h"
#include "../../common/json_value.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <set>
#include <vector>
#pragma comment(lib, "bcrypt.lib")
namespace pulse::extensions {
namespace {
struct Algorithm { BCRYPT_ALG_HANDLE value{}; ~Algorithm(){if(value)BCryptCloseAlgorithmProvider(value,0);} };
struct Key { BCRYPT_KEY_HANDLE value{}; ~Key(){if(value)BCryptDestroyKey(value);} };
bool Only(const json::Value& value,std::initializer_list<std::string_view> fields){
    if(!value.is_object()||value.size()!=fields.size())return false;
    for(const auto& key:value.keys())if(std::find(fields.begin(),fields.end(),key)==fields.end())return false;
    return true;
}
bool Hex(std::string_view text,std::vector<UCHAR>& bytes){
    if(text.empty()||text.size()%2||text.size()>8192)return false;
    auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    for(size_t i=0;i<text.size();i+=2){int a=digit(text[i]),b=digit(text[i+1]);if(a<0||b<0)return false;bytes.push_back(static_cast<UCHAR>((a<<4)|b));}
    return true;
}
bool PublicKey(const std::vector<UCHAR>& bytes){
    if(bytes.size()<sizeof(BCRYPT_RSAKEY_BLOB))return false;
    BCRYPT_RSAKEY_BLOB header{};std::memcpy(&header,bytes.data(),sizeof(header));
    return header.Magic==BCRYPT_RSAPUBLIC_MAGIC&&header.BitLength>=2048&&header.BitLength<=4096&&
        header.BitLength%8==0&&header.cbModulus==header.BitLength/8&&header.cbPublicExp>=1&&header.cbPublicExp<=8&&
        header.cbPrime1==0&&header.cbPrime2==0&&bytes.size()==sizeof(header)+header.cbPublicExp+header.cbModulus;
}
}
bool VerifyPackageSignature(std::string_view manifest,std::string_view signature,std::string_view policy,std::string& error){
    auto fail=[&](const char* code){error=code;return false;};error.clear();
    if(manifest.size()>65536||signature.size()>16384||policy.size()>65536)return fail("signature_size_limit");
    bool required=false;std::vector<UCHAR> public_key;json::Value signed_value;
    std::string publisher;
    if(!signature.empty()){
        if(!json::Parse(signature,signed_value,nullptr,{16384,4})||!Only(signed_value,{"formatVersion","algorithm","publisher","signature"}))return fail("invalid_signature_format");
        int64_t version=0;const auto* v=signed_value.find("formatVersion");
        if(!v||!v->get_int(version,1,1)||signed_value.get_string("algorithm")!="RSA-PKCS1-SHA256")return fail("unsupported_signature_format");
        publisher=signed_value.get_string("publisher");
    }
    if(!policy.empty()){
        json::Value rules;if(!json::Parse(policy,rules,nullptr,{65536,5})||!Only(rules,{"formatVersion","requireSigned","publishers"}))return fail("invalid_signature_policy");
        int64_t version=0;const auto* v=rules.find("formatVersion");const auto* require=rules.find("requireSigned");const auto* publishers=rules.find("publishers");
        if(!v||!v->get_int(version,1,1)||!require||!require->is_bool()||!publishers||!publishers->is_array()||publishers->size()>32)return fail("invalid_signature_policy");
        required=require->as_bool();std::set<std::string> pins;
        for(const auto& entry:publishers->items()){
            std::vector<UCHAR> blob;
            if(!Only(entry,{"id","publicKey"})||!Hex(entry.get_string("publicKey"),blob)||!PublicKey(blob))return fail("invalid_publisher_key");
            const auto pin=PackageRevision(std::string_view(reinterpret_cast<const char*>(blob.data()),blob.size()));
            if(pin.size()!=64||entry.get_string("id")!=pin||!pins.insert(pin).second)return fail("invalid_publisher_pin");
            if(pin==publisher)public_key=std::move(blob);
        }
    }
    if(signature.empty())return required?fail("signature_required"):true;
    if(public_key.empty())return fail("unknown_publisher");
    std::vector<UCHAR> sig;if(!Hex(signed_value.get_string("signature"),sig))return fail("invalid_signature_format");
    constexpr char prefix[]="Pulse.Extension.Manifest.Signature\0v1\0";
    std::string payload(prefix,sizeof(prefix)-1);payload.append(manifest);
    std::vector<UCHAR> hash;if(!Hex(PackageRevision(payload),hash)||hash.size()!=32)return fail("signature_hash_failed");
    Algorithm algorithm;Key key;
    if(BCryptOpenAlgorithmProvider(&algorithm.value,BCRYPT_RSA_ALGORITHM,nullptr,0)<0||
       BCryptImportKeyPair(algorithm.value,nullptr,BCRYPT_RSAPUBLIC_BLOB,&key.value,public_key.data(),static_cast<ULONG>(public_key.size()),0)<0)return fail("invalid_publisher_key");
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    if(BCryptVerifySignature(key.value,&padding,hash.data(),static_cast<ULONG>(hash.size()),sig.data(),static_cast<ULONG>(sig.size()),BCRYPT_PAD_PKCS1)<0)return fail("signature_invalid");
    return true;
}
}
