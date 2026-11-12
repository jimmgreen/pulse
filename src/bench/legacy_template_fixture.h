#pragma once
#include <windows.h>
#include <vector>
#include <cstring>
namespace pulse::bench {
class LegacyTemplateFixture {
    HGLOBAL memory_ = nullptr;
    size_t bytes_ = 0;
public:
    LegacyTemplateFixture() {
        std::vector<WORD> words;
        const auto dword=[&](DWORD v){words.push_back(static_cast<WORD>(v));words.push_back(static_cast<WORD>(v>>16));};
        const auto text=[&](const wchar_t* value){do{words.push_back(static_cast<WORD>(*value));}while(*value++);};
        dword(WS_CHILD|DS_CONTROL); dword(WS_EX_CONTROLPARENT);
        words.insert(words.end(),{2,0,0,180,45,0,0,0});
        const auto item=[&](DWORD style,WORD y,WORD id,WORD cls,const wchar_t* title){
            if(words.size()%2)words.push_back(0);dword(style);dword(0);
            words.insert(words.end(),{5,y,160,14,id,0xffff,cls});text(title);words.push_back(0);
        };
        item(WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,4,1001,0x81,L"initial");
        item(WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,24,1002,0x80,L"Template button");
        bytes_=words.size()*sizeof(WORD);
        memory_=GlobalAlloc(GMEM_FIXED,bytes_);
        if(memory_){void* bytes=GlobalLock(memory_);if(bytes){std::memcpy(bytes,words.data(),words.size()*sizeof(WORD));GlobalUnlock(memory_);}else{GlobalFree(memory_);memory_=nullptr;}}
    }
    ~LegacyTemplateFixture(){if(memory_)GlobalFree(memory_);}
    size_t ByteSize() const{return bytes_;}
    HINSTANCE Handle() const{return reinterpret_cast<HINSTANCE>(memory_);}
    LegacyTemplateFixture(const LegacyTemplateFixture&)=delete;
    LegacyTemplateFixture& operator=(const LegacyTemplateFixture&)=delete;
};
}

