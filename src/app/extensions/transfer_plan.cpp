#include "transfer_plan.h"
#include "package_revision.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <limits>

namespace pulse::extensions::transfer_plan {
namespace {
constexpr std::string_view kDigestDomain = "Pulse.TransferPlan.v2";
uint64_t Now() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void Number(std::string& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (i * 8)) & 255));
}
void Path(std::string& out, const std::wstring& value) {
    Number(out, value.size());
    for (const auto c : value) Number(out, static_cast<uint64_t>(c));
}
void Identity(std::string& out, const FileIdentity& value) {
    Number(out, value.volume);
    out.append(reinterpret_cast<const char*>(value.file_id.data()), value.file_id.size());
    Number(out, value.size); Number(out, value.last_write);
}
bool SafeComponent(std::wstring_view value) {
    if (value.empty() || value == L"." || value == L".." || value.size() > 255 ||
        value.back() == L'.' || value.back() == L' ') return false;
    for (const auto c : value)
        if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) return false;
    std::wstring base(value.substr(0, value.find(L'.')));
    std::transform(base.begin(), base.end(), base.begin(), towupper);
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
        base == L"CONIN$" || base == L"CONOUT$") return false;
    return !(base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")) &&
        ((base[3] >= L'0' && base[3] <= L'9') || base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3'));
}
bool SafeRelative(std::wstring_view value, bool allow_empty = false) {
    if (value.empty()) return allow_empty;
    size_t start = 0;
    while (start < value.size()) {
        const size_t end = value.find(L'\\', start);
        if (!SafeComponent(value.substr(start, end == std::wstring_view::npos ? end : end - start))) return false;
        if (end == std::wstring_view::npos) return true;
        start = end + 1;
    }
    return false;
}
bool SamePathOrdinal(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index)
        if (towupper(left[index]) != towupper(right[index])) return false;
    return true;
}
std::string Digest(const Plan& plan) {
    std::string bytes(kDigestDomain);
    Number(bytes, static_cast<uint64_t>(plan.transfer.operation));
    Number(bytes, plan.transfer.directory_tree ? 1 : 0);
    Path(bytes, plan.transfer.tree_root_name);
    Number(bytes, plan.created_ms); Number(bytes, plan.expires_ms); Number(bytes, plan.transfer.sources.size());
    for (const auto& source : plan.transfer.sources) {
        Path(bytes, source.path); Path(bytes, source.relative_destination); Identity(bytes, source.identity);
        Number(bytes, source.parent.volume);
        bytes.append(reinterpret_cast<const char*>(source.parent.file_id.data()), source.parent.file_id.size());
    }
    Number(bytes, plan.transfer.tree_directories.size());
    for (const auto& directory : plan.transfer.tree_directories) {
        Path(bytes, directory.path); Path(bytes, directory.relative_destination);
        Identity(bytes, directory.identity);
    }
    Path(bytes, plan.transfer.target.path); Identity(bytes, plan.transfer.target.identity);
    return PackageRevision(bytes);
}
template<class T> Result<T> Failed(Error error) { return {error, std::nullopt}; }
}

struct ExecutionPermit::SharedState {
    std::shared_ptr<const transfer_plan::Plan> plan;
    authorization::PrincipalBinding principal;
    authorization::Registry::Clock clock;
    std::mutex mutex;
    bool started = false;
    SharedState(std::shared_ptr<const transfer_plan::Plan> frozen,
                const authorization::PrincipalBinding& binding, authorization::Registry::Clock time)
        : plan(std::move(frozen)), principal(binding), clock(std::move(time)) {}
};

ExecutionPermit::ExecutionPermit(std::shared_ptr<const transfer_plan::Plan> plan,
                               const authorization::PrincipalBinding& principal,
                               authorization::Registry::Clock clock)
    : state_(std::make_shared<SharedState>(std::move(plan), principal, std::move(clock))) {}

const transfer_plan::Plan& ExecutionPermit::Plan() const noexcept { return *state_->plan; }
const authorization::PrincipalBinding& ExecutionPermit::Principal() const noexcept { return state_->principal; }

Error ExecutionPermit::TryStart(authorization::Registry& registry,
                              const authorization::PrincipalBinding& expected) const {
    std::lock_guard lock(state_->mutex);
    if (state_->principal.Package() != expected.Package() || state_->principal.Owner() != expected.Owner())
        return Error::NotFound;
    if (state_->started) return Error::AlreadyStarted;
    if (!registry.ValidateBinding(state_->principal) || !registry.ValidateBinding(expected)) return Error::Revoked;
    const auto now = state_->clock();
    if (now < state_->plan->created_ms || now >= state_->plan->expires_ms) return Error::Expired;
    state_->started = true;
    return Error::None;
}

Store::Store(authorization::Registry& registry, Limits limits, Clock clock, Random random)
    : registry_(registry), limits_(limits), clock_(clock ? std::move(clock) : Clock{Now}),
      random_(random ? std::move(random) : Random{authorization::SecureRandomBytes}) {}

void Store::Cleanup(uint64_t now) {
    for (auto it = plans_.begin(); it != plans_.end();) {
        if (now < it->second.plan->created_ms || now >= it->second.plan->expires_ms || !registry_.ValidateBinding(it->second.principal))
            it = plans_.erase(it);
        else ++it;
    }
}

Error Store::Check(const Record& record, const Binding& expected, uint64_t now) {
    if (record.principal.Package() != expected.Package() || record.principal.Owner() != expected.Owner()) return Error::NotFound;
    if (!registry_.ValidateBinding(record.principal) || !registry_.ValidateBinding(expected)) return Error::Revoked;
    if (now < record.plan->created_ms || now >= record.plan->expires_ms) return Error::Expired;
    if (record.state == State::Cancelled) return Error::Cancelled;
    if (record.state == State::Consumed) return Error::AlreadyConsumed;
    return Error::None;
}

std::optional<std::string> Store::Token() {
    if (serial_ == (std::numeric_limits<uint64_t>::max)()) return std::nullopt;
    std::array<uint8_t, 32> nonce{};
    try { if (!random_(nonce)) return std::nullopt; } catch (...) { return std::nullopt; }
    if (last_nonce_ && nonce == *last_nonce_) return std::nullopt;
    constexpr char hex[] = "0123456789abcdef";
    std::string token; token.reserve(80);
    for (const auto byte : nonce) { token.push_back(hex[byte >> 4]); token.push_back(hex[byte & 15]); }
    for (const auto& [id, record] : plans_) {
        if (id.compare(0, 64, token) == 0 || (!record.receipt.empty() && record.receipt.compare(0, 64, token) == 0)) return std::nullopt;
    }
    const auto serial = ++serial_;
    for (int shift = 60; shift >= 0; shift -= 4) token.push_back(hex[(serial >> shift) & 15]);
    last_nonce_ = nonce;
    return token;
}

Result<std::string> Store::Create(const Binding& principal, const Draft& draft) {
    std::lock_guard lock(mutex_);
    const auto now = clock_(); Cleanup(now);
    if (!registry_.ValidateBinding(principal)) return Failed<std::string>(Error::Revoked);
    if (plans_.size() >= limits_.plans) return Failed<std::string>(Error::Quota);
    const size_t source_limit = draft.directory_tree ? limits_.tree_files : limits_.sources;
    if ((draft.operation != Operation::Copy && draft.operation != Operation::Move) ||
        (draft.directory_tree && draft.operation != Operation::Copy) ||
        (!draft.directory_tree && draft.sources.empty()) ||
        draft.sources.size() > source_limit || !limits_.lifetime_ms ||
        (draft.directory_tree && (draft.tree_directories.empty() || draft.tree_directories.size() > limits_.tree_directories ||
            !SafeComponent(draft.tree_root_name))) ||
        (!draft.directory_tree && (!draft.tree_root_name.empty() || !draft.tree_directories.empty())) ||
        now > (std::numeric_limits<uint64_t>::max)() - limits_.lifetime_ms) return Failed<std::string>(Error::InvalidDraft);
    size_t remaining = limits_.plan_bytes;
    constexpr size_t header_bytes = kDigestDomain.size() + 8 * 8;
    constexpr size_t entry_bytes = 8 + 8 + 16 + 8 + 8 + 8;
    if (remaining < header_bytes + entry_bytes ||
        draft.sources.size() > (remaining - header_bytes - entry_bytes) / (entry_bytes + 24))
        return Failed<std::string>(Error::InvalidDraft);
    remaining -= header_bytes + entry_bytes + draft.sources.size() * (entry_bytes + 24);
    constexpr size_t directory_bytes = 8 + 16 + 16;
    if (draft.tree_directories.size() > remaining / directory_bytes) return Failed<std::string>(Error::InvalidDraft);
    remaining -= draft.tree_directories.size() * directory_bytes;
    const auto path_ok = [&](const std::wstring& path) {
        if (path.empty() || path.size() > limits_.path_chars || path.size() > remaining / 8) return false;
        remaining -= path.size() * 8;
        return path.find(L'\0') == std::wstring::npos;
    };
    if (!path_ok(draft.target.path) ||
        (draft.directory_tree && !path_ok(draft.tree_root_name))) return Failed<std::string>(Error::InvalidDraft);
    std::vector<std::wstring> destinations;
    destinations.reserve(draft.sources.size() + draft.tree_directories.size());
    for (const auto& directory : draft.tree_directories) {
        if (!draft.directory_tree || !SafeRelative(directory.relative_destination, true) ||
            !path_ok(directory.path) || (!directory.relative_destination.empty() && !path_ok(directory.relative_destination)))
            return Failed<std::string>(Error::InvalidDraft);
        if (std::any_of(destinations.begin(), destinations.end(), [&](const auto& value) {
                return SamePathOrdinal(value, directory.relative_destination);
            }))
            return Failed<std::string>(Error::InvalidDraft);
        destinations.push_back(directory.relative_destination);
    }
    for (size_t i = 0; i < draft.sources.size(); ++i) {
        if (!path_ok(draft.sources[i].path) ||
            (draft.directory_tree && (!path_ok(draft.sources[i].relative_destination) ||
                                      !SafeRelative(draft.sources[i].relative_destination))) ||
            (!draft.directory_tree && !draft.sources[i].relative_destination.empty()))
            return Failed<std::string>(Error::InvalidDraft);
        if (draft.directory_tree) {
            if (std::any_of(destinations.begin(), destinations.end(), [&](const auto& value) {
                    return SamePathOrdinal(value, draft.sources[i].relative_destination);
                }))
                return Failed<std::string>(Error::InvalidDraft);
            destinations.push_back(draft.sources[i].relative_destination);
        }
        for (size_t j = 0; j < i; ++j)
            if (draft.sources[i].path == draft.sources[j].path) return Failed<std::string>(Error::InvalidDraft);
    }
    if (draft.directory_tree) {
        if (std::find(destinations.begin(), destinations.end(), L"") == destinations.end())
            return Failed<std::string>(Error::InvalidDraft);
        for (const auto& destination : draft.sources) {
            const auto split = destination.relative_destination.find_last_of(L'\\');
            const auto parent = split == std::wstring::npos ? std::wstring{} : destination.relative_destination.substr(0, split);
            if (std::none_of(draft.tree_directories.begin(), draft.tree_directories.end(), [&](const auto& directory) {
                    return SamePathOrdinal(directory.relative_destination, parent);
                })) return Failed<std::string>(Error::InvalidDraft);
        }
        for (const auto& directory : draft.tree_directories) {
            if (directory.relative_destination.empty()) continue;
            const auto split = directory.relative_destination.find_last_of(L'\\');
            const auto parent = split == std::wstring::npos ? std::wstring{} : directory.relative_destination.substr(0, split);
            if (std::none_of(draft.tree_directories.begin(), draft.tree_directories.end(), [&](const auto& candidate) {
                    return SamePathOrdinal(candidate.relative_destination, parent);
                })) return Failed<std::string>(Error::InvalidDraft);
        }
    }
    Plan plan{draft, now, now + limits_.lifetime_ms, {}};
    plan.digest = Digest(plan);
    if (plan.digest.empty()) return Failed<std::string>(Error::HashFailed);
    const auto token = Token();
    if (!token) return Failed<std::string>(Error::RandomFailed);
    plans_.emplace(*token, Record{principal, std::make_shared<const Plan>(std::move(plan))});
    return {Error::None, *token};
}

Result<Store::FrozenPlan> Store::Read(std::string_view plan_id, const Binding& expected) {
    std::lock_guard lock(mutex_);
    if (plan_id.size() != 80) return Failed<FrozenPlan>(Error::NotFound);
    const auto found = plans_.find(std::string(plan_id));
    if (found == plans_.end()) return Failed<FrozenPlan>(Error::NotFound);
    const auto error = Check(found->second, expected, clock_());
    if (error != Error::None) return Failed<FrozenPlan>(error);
    return {Error::None, found->second.plan};
}

Result<std::string> Store::Confirm(std::string_view plan_id, const Binding& expected) {
    std::lock_guard lock(mutex_);
    if (plan_id.size() != 80) return Failed<std::string>(Error::NotFound);
    const auto found = plans_.find(std::string(plan_id));
    if (found == plans_.end()) return Failed<std::string>(Error::NotFound);
    auto& record = found->second;
    const auto error = Check(record, expected, clock_());
    if (error != Error::None) return Failed<std::string>(error);
    if (record.state == State::Confirmed) return {Error::None, record.receipt};
    const auto digest = Digest(*record.plan);
    if (digest.empty()) return Failed<std::string>(Error::HashFailed);
    if (digest != record.plan->digest) return Failed<std::string>(Error::PlanMismatch);
    const auto receipt = Token();
    if (!receipt) return Failed<std::string>(Error::RandomFailed);
    record.receipt = *receipt; record.confirmed_digest = record.plan->digest; record.state = State::Confirmed;
    return {Error::None, *receipt};
}

Result<ExecutionPermit> Store::Consume(std::string_view receipt, const Binding& expected) {
    std::lock_guard lock(mutex_);
    if (receipt.size() != 80) return Failed<ExecutionPermit>(Error::NotFound);
    for (auto& [id, record] : plans_) {
        (void)id;
        if (record.receipt != receipt) continue;
        const auto error = Check(record, expected, clock_());
        if (error != Error::None) return Failed<ExecutionPermit>(error);
        const auto digest = Digest(*record.plan);
        if (digest.empty()) return Failed<ExecutionPermit>(Error::HashFailed);
        if (record.state != State::Confirmed || record.confirmed_digest != record.plan->digest || digest != record.confirmed_digest)
            return Failed<ExecutionPermit>(Error::PlanMismatch);
        ExecutionPermit permit(record.plan, record.principal, clock_);
        record.state = State::Consumed;
        return {Error::None, std::move(permit)};
    }
    return Failed<ExecutionPermit>(Error::NotFound);
}

Error Store::Cancel(std::string_view plan_id, const Binding& expected) {
    std::lock_guard lock(mutex_);
    if (plan_id.size() != 80) return Error::NotFound;
    const auto found = plans_.find(std::string(plan_id));
    if (found == plans_.end()) return Error::NotFound;
    const auto error = Check(found->second, expected, clock_());
    if (error != Error::None) return error;
    found->second.state = State::Cancelled;
    return Error::None;
}

}
