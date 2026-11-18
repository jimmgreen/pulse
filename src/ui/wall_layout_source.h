#pragma once
#include "wall_aspects.h"
#include "../fs/fs_enum.h"
#include <memory>
#include <unordered_map>

namespace pulse::ui {
// Owned by one layout worker. UI requests retain only immutable snapshot handles.
class WallLayoutSource {
public:
    static std::shared_ptr<WallLayoutSource> Create(std::shared_ptr<const std::vector<fs::DirEntry>> snapshot,
        std::shared_ptr<const std::vector<int>> order, std::wstring folder, bool virtual_path) {
        return std::shared_ptr<WallLayoutSource>(new WallLayoutSource(std::move(snapshot), std::move(order),
            std::move(folder), virtual_path), [](WallLayoutSource* source) {
                if (!TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, void* context) {
                    delete static_cast<WallLayoutSource*>(context);
                }, source, nullptr)) delete source;
            });
    }
    WallLayoutSource(std::shared_ptr<const std::vector<fs::DirEntry>> snapshot,
                     std::shared_ptr<const std::vector<int>> order, std::wstring folder, bool virtual_path)
        : snapshot_(std::move(snapshot)), order_(std::move(order)), folder_(std::move(folder)), virtual_(virtual_path) {}

    const std::vector<float>* Prepare(const std::atomic<uint64_t>& generation, uint64_t expected, bool force) {
        auto& store = WallAspects::Instance();
        bool& changed = dirty_;
        if (!initialized_) {
            const size_t count = order_ ? order_->size() : snapshot_->size();
            values_.assign(count, 0.85f);
            keys_.assign(count, 0);
            indices_.clear();
            version_ = store.Version();
            for (size_t row = 0; row < count; ++row) {
                if ((row & 255) == 0 && generation.load() != expected) return nullptr;
                const int src = order_ ? (*order_)[row] : static_cast<int>(row);
                if (src < 0 || static_cast<size_t>(src) >= snapshot_->size()) continue;
                const auto& entry = (*snapshot_)[static_cast<size_t>(src)];
                if (!IsWallPicture(entry.name, entry.is_dir)) continue;
                const auto path = Path(entry, folder_, virtual_);
                if (path.empty()) continue;
                const uint64_t modified = (static_cast<uint64_t>(entry.mtime.dwHighDateTime) << 32) | entry.mtime.dwLowDateTime;
                const uint64_t key = WallAspects::ContentHash(path, modified, entry.size);
                keys_[row] = key;
                values_[row] = 0;
                indices_[key].push_back(row);
            }
            store.Lookup(keys_.data(), values_.data(), keys_.size());
            initialized_ = true;
            changed = true;
        }
        if (version_ != store.Version()) {
            std::vector<WallAspects::AspectChange> changes;
            if (!store.ChangesSince(version_, changes)) {
                changed = store.Lookup(keys_.data(), values_.data(), keys_.size()) || changed;
            } else {
                for (const auto& change : changes) {
                    const auto found = indices_.find(change.hash);
                    if (found == indices_.end()) continue;
                    for (size_t index : found->second) {
                        if (values_[index] != change.aspect) {
                            values_[index] = change.aspect;
                            changed = true;
                        }
                    }
                }
            }
        }
        return changed || force ? &values_ : nullptr;
    }
    void Committed() noexcept { dirty_ = false; }
    static std::wstring Path(const fs::DirEntry& entry, const std::wstring& folder, bool virtual_path) {
        if (!entry.full_path.empty()) return entry.full_path;
        if (virtual_path) return {};
        return folder + (folder.empty() || folder.back() == L'\\' || folder.back() == L'/' ? L"" : L"\\") + entry.name;
    }
private:
    std::shared_ptr<const std::vector<fs::DirEntry>> snapshot_;
    std::shared_ptr<const std::vector<int>> order_;
    std::wstring folder_;
    bool virtual_ = false;
    bool initialized_ = false;
    bool dirty_ = false;
    uint64_t version_ = 0;
    std::vector<uint64_t> keys_;
    std::vector<float> values_;
    std::unordered_map<uint64_t, std::vector<size_t>> indices_;
};
}
