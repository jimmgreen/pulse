#pragma once
#include <cstdint>
#include <string>

namespace pulse::ui {
// The view owns the text; a revision changes whenever its searchable body changes.
class PreviewTextIdentity {
public:
    void Reset() noexcept { owner_ = nullptr; }
    bool Changed(const void* owner, uint64_t revision, const std::wstring& text,
                 const std::wstring& previous, bool source = false) {
        if (owner_ == owner && revision_ == revision && source_ == source) return false;
        owner_ = owner;
        revision_ = revision;
        source_ = source;
        return text != previous;
    }
private:
    const void* owner_ = nullptr;
    uint64_t revision_ = 0;
    bool source_ = false;
};
}
