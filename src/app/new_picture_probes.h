#pragma once
#include "snapshot_patch.h"
#include "../common/preview_extensions.h"
#include <algorithm>
#include <cwctype>

namespace pulse::app {
// Only new pictures actually observed at zero get probes. A genuine empty file
// uses four timed probes and at most one further foreground probe per arrival.
class NewPictureProbes {
    struct Item {
        std::wstring path, name;
        uint64_t due, expires;
        unsigned step = 0;
        bool foreground_pending = false, foreground_used = false;
    };
    std::vector<Item> items_;
    static constexpr uint64_t delays[] = {250, 1000, 4000, 15000};
public:
    void Observe(const std::wstring& path, const PreparedDirNotifyBatch& batch, uint64_t now) {
        for (const auto& event : batch.events) {
            const auto erase = [&](const std::wstring& name) {
                std::erase_if(items_, [&](const Item& i) {
                    return i.path == path && !_wcsicmp(i.name.c_str(), name.c_str());
                });
            };
            if (event.action == FILE_ACTION_RENAMED_NEW_NAME) erase(event.old_name);
            if (event.action == FILE_ACTION_REMOVED) { erase(event.name); continue; }
            const auto meta = batch.metadata.find(event.name);
            if (meta == batch.metadata.end()) continue;
            if (!meta->second || meta->second->is_dir || meta->second->size) { erase(event.name); continue; }
            if (event.action != FILE_ACTION_ADDED && event.action != FILE_ACTION_RENAMED_NEW_NAME) continue;
            const auto dot = event.name.rfind(L'.');
            if (dot == std::wstring::npos) continue;
            auto ext = event.name.substr(dot);
            for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
            if (!preview::IsImageExtension(ext) && !preview::IsPsdExtension(ext) &&
                !preview::IsFamilyExtension(preview::PreviewFamily::Raw, ext)) continue;
            erase(event.name);
            if (items_.size() == 256) items_.erase(items_.begin());
            items_.push_back({path, event.name, now + delays[0], now + 600000});
        }
    }
    bool OnForeground(const std::wstring& path, uint64_t now) {
        bool queued = false;
        for (auto& i : items_) {
            if (i.path != path || now >= i.expires || i.foreground_used || i.foreground_pending) continue;
            i.foreground_pending = true;
            queued = true;
        }
        return queued;
    }
    template<class Emit> void Tick(uint64_t now, Emit emit) {
        std::erase_if(items_, [&](Item& i) {
            if (now >= i.expires) return true;
            if (i.foreground_pending) {
                i.foreground_pending = false;
                i.foreground_used = true;
                if (i.due > now) {
                    emit(i.path, i.name);
                    return i.step == 4;
                }
            }
            if (i.due > now) return false;
            emit(i.path, i.name);
            ++i.step;
            i.due = i.step == 4 ? UINT64_MAX : now + delays[i.step];
            return i.step == 4 && i.foreground_used;
        });
    }
};
}
