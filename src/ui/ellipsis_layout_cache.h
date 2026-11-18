#pragma once
#include "ui_compositor.h"
#include "typography.h"
#include <list>
#include <unordered_map>
#include <string>

namespace pulse::ui {
// Immutable, device-independent layouts. Position and brush are not part of
// shaping; hover/progress frames can reuse them. Keep formats alive to prevent
// recycled COM addresses from aliasing an old cache entry.
struct EllipsisLayoutKey {
    IDWriteTextFormat* format;
    std::wstring text;
    float width, height, font_size;
    DWRITE_TEXT_ALIGNMENT alignment;
    DWRITE_PARAGRAPH_ALIGNMENT paragraph;
    bool operator==(const EllipsisLayoutKey&) const = default;
};
struct EllipsisLayoutHash {
    size_t operator()(const EllipsisLayoutKey& k) const {
        size_t h=std::hash<std::wstring>{}(k.text);
        const auto mix=[&](size_t v){h^=v+0x9e3779b9+(h<<6)+(h>>2);};
        mix(std::hash<void*>{}(k.format));mix(std::hash<float>{}(k.width));mix(std::hash<float>{}(k.height));
        mix(std::hash<float>{}(k.font_size));mix(k.alignment);mix(k.paragraph);return h;
    }
};
struct EllipsisLayoutCache {
    struct Entry { EllipsisLayoutKey key; ComPtr<IDWriteTextFormat> format; ComPtr<IDWriteTextLayout> layout; };
    std::list<Entry> lru;
    std::unordered_map<EllipsisLayoutKey,std::list<Entry>::iterator,EllipsisLayoutHash> entries;
    ComPtr<IDWriteFactory> factory;
    uint64_t generation=0, builds=0, hits=0;
};
inline EllipsisLayoutCache& EllipsisLayouts() { thread_local EllipsisLayoutCache cache;return cache; }
inline IDWriteTextLayout* CachedEllipsisLayout(IDWriteFactory* factory,IDWriteTextFormat* format,
        const std::wstring& text,float width,float height) {
    if(!factory||!format||text.empty()||width<=0||height<=0)return nullptr;
    auto& cache=EllipsisLayouts();
    const auto generation=typography::Generation();
    if(cache.factory.get()!=factory||cache.generation!=generation) {
        cache.entries.clear();cache.lru.clear();cache.factory.reset();factory->AddRef();cache.factory.p=factory;cache.generation=generation;
    }
    EllipsisLayoutKey key{format,text,width,height,format->GetFontSize(),format->GetTextAlignment(),format->GetParagraphAlignment()};
    if(auto it=cache.entries.find(key);it!=cache.entries.end()) {
        ++cache.hits;cache.lru.splice(cache.lru.begin(),cache.lru,it->second);return it->second->layout.get();
    }
    ComPtr<IDWriteTextLayout> layout;
    if(FAILED(factory->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),format,width,height,&layout)))return nullptr;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};ComPtr<IDWriteInlineObject> sign;
    if(SUCCEEDED(factory->CreateEllipsisTrimmingSign(layout.get(),&sign)))layout->SetTrimming(&trimming,sign.get());
    ++cache.builds;
    if(cache.entries.size()>=512){cache.entries.erase(cache.lru.back().key);cache.lru.pop_back();}
    EllipsisLayoutCache::Entry entry;entry.key=key;format->AddRef();entry.format.p=format;entry.layout=std::move(layout);
    cache.lru.push_front(std::move(entry));cache.entries.emplace(std::move(key),cache.lru.begin());return cache.lru.front().layout.get();
}
}
