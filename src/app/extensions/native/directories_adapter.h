#pragma once
#include "../../../../sdk/native/include/pulse/directories.h"
#include "../runtime.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulse { struct AppState; }
namespace pulse::extensions::native {
struct DirectoriesBackend {
    std::function<bool(const CallerBinding &)> live;
    std::function<PulseStatus(const CallerBinding &, const std::wstring &, bool)> open_item;
};
struct DirectoryTransferTarget {
    std::wstring path;
    std::function<bool()> valid;
};
struct DirectoryTransferSources {
    std::vector<std::wstring> paths;
    std::vector<uint64_t> sizes;
    std::vector<uint64_t> modified;
    std::function<bool()> valid;
};
struct DirectoryTreeTransferSource {
    std::wstring path;
    std::wstring leaf;
    uint64_t size = 0;
    uint64_t modified = 0;
    std::function<bool()> valid;
};
class DirectoriesAdapter final {
  public:
    DirectoriesAdapter(AppState &owner, CallerBinding caller);
    DirectoriesAdapter(CallerBinding caller, DirectoriesBackend backend);
    ~DirectoriesAdapter();
    DirectoriesAdapter(const DirectoriesAdapter &) = delete;
    DirectoriesAdapter &operator=(const DirectoriesAdapter &) = delete;
    PulseDirectoriesV1 Table() const noexcept;
    PulseStatus Query(uint32_t version, void *out_table, uint32_t out_size) const noexcept;
    PulseStatus Close() noexcept;
    std::optional<DirectoryTransferTarget> ResolveTransferTarget(PulseDirectoryId id) const noexcept;
    std::optional<DirectoryTransferSources> ResolveTransferSources(PulseDirectoryId id, uint64_t snapshot,
        const uint64_t* entries, uint32_t count) const noexcept;
    std::optional<DirectoryTreeTransferSource> ResolveTreeTransferSource(PulseDirectoryId id,
        uint64_t snapshot, uint64_t entry) const noexcept;
    bool CallbackActive() const noexcept;
    void Poll() noexcept;
    struct Impl;

  private:
    std::shared_ptr<Impl> impl_;
};
} // namespace pulse::extensions::native
