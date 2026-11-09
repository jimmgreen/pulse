#pragma once
#include <d2d1.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace pulse::ui {
struct ItemArtworkKey {
    uint64_t generation = 0;
    int source = -1;
    int mode = 0;
    std::wstring path, name;
    uint64_t modified = 0, size = 0;
    D2D1_RECT_F cell{};
    bool operator==(const ItemArtworkKey& b) const {
        return generation == b.generation && source == b.source && mode == b.mode &&
            path == b.path && name == b.name && modified == b.modified && size == b.size &&
            cell.left == b.cell.left && cell.top == b.cell.top && cell.right == b.cell.right && cell.bottom == b.cell.bottom;
    }
};

// Keep the geometry produced by painting, including decoded alpha bounds and
// aspect fitting. An unpainted or changed layout has no image drag target.
class ItemArtworkHitCache {
public:
    void Remember(ItemArtworkKey key, D2D1_RECT_F artwork) {
        if (auto it=items_.find(key); it!=items_.end()) { it->second=artwork; return; }
        if (items_.size() >= 4096) items_.erase(items_.begin());
        items_.emplace(std::move(key), artwork);
    }
    bool Contains(const ItemArtworkKey& key, float x, float y) const {
        if (auto it=items_.find(key); it!=items_.end()) {
            const auto& r=it->second;
            return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
        }
        return false;
    }
private:
    struct Hash {
        size_t operator()(const ItemArtworkKey& key) const {
            size_t value=std::hash<std::wstring>{}(key.path);
            const auto mix=[&](size_t next){value^=next+0x9e3779b9+(value<<6)+(value>>2);};
            mix(std::hash<uint64_t>{}(key.generation)); mix(std::hash<int>{}(key.source));
            mix(std::hash<int>{}(key.mode));
            for (float coordinate : {key.cell.left,key.cell.top,key.cell.right,key.cell.bottom}) mix(std::hash<float>{}(coordinate));
            return value;
        }
    };
    std::unordered_map<ItemArtworkKey,D2D1_RECT_F,Hash> items_;
};
}
