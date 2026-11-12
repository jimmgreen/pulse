// jump_history.cpp - frecency of folders reached through the jump features.
#include "jump_history.h"

#include "../common/utf8_file.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <ctime>

namespace pulse::app {
namespace {
std::wstring Lower(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}
// Without the long-path prefix and trailing separators.
std::wstring Plain(const std::wstring& path) {
    std::wstring plain = path;
    if (plain.rfind(L"\\\\?\\UNC\\", 0) == 0) plain = L"\\\\" + plain.substr(8);
    else if (plain.rfind(L"\\\\?\\", 0) == 0) plain = plain.substr(4);
    while (plain.size() > 3 && (plain.back() == L'\\' || plain.back() == L'/')) plain.pop_back();
    return plain;
}
// Same folder however it was typed.
std::wstring Key(const std::wstring& path) { return Lower(Plain(path)); }
std::vector<std::wstring> Split(const std::wstring& line, wchar_t sep) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        const size_t end = line.find(sep, start);
        parts.push_back(line.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return parts;
}
}

int64_t JumpHistory::Now() { return static_cast<int64_t>(std::time(nullptr)); }

double JumpHistory::Decayed(double score, int64_t last, int64_t now) {
    const double age = static_cast<double>(std::max<int64_t>(0, now - last));
    return score * std::exp2(-age / kHalfLife);
}

void JumpHistory::ParseLocked(const std::wstring& text) {
    entries_.clear();
    for (const auto& raw : Split(text, L'\n')) {
        std::wstring line = raw;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        const auto f = Split(line, L'\t');
        if (f.size() < 3 || f[0].empty()) continue;
        if (f[0] == L"#count-v1") {
            if (auto* entry = FindLocked(f[1])) {
                wchar_t* end = nullptr;
                const auto count = std::wcstoull(f[2].c_str(), &end, 10);
                if (!f[2].empty() && f[2][0] != L'-' && end && !*end) entry->count = count;
            }
            continue;
        }
        Entry e;
        e.path = Plain(f[0]);
        e.key = Key(e.path);
        e.score = std::wcstod(f[1].c_str(), nullptr);
        e.last = std::wcstoll(f[2].c_str(), nullptr, 10);
        for (size_t i = 3; i + 2 < f.size() && e.apps.size() < kMaxApps; i += 3)
            e.apps.push_back({f[i], std::wcstod(f[i + 1].c_str(), nullptr), std::wcstoll(f[i + 2].c_str(), nullptr, 10)});
        if (e.score > 0 && entries_.size() < kMaxEntries && !FindLocked(e.path)) entries_.push_back(std::move(e));
    }
}

void JumpHistory::Open(const std::wstring& file) {
    std::wstring text;
    const bool read = !file.empty() && ReadUtf8File(file, text);
    std::lock_guard lock(mu_);
    if (open_) return;
    open_ = true;
    file_ = file;
    if (read) ParseLocked(text);
}

void JumpHistory::Parse(const std::wstring& text) {
    std::lock_guard lock(mu_);
    open_ = true;
    file_.clear();
    ParseLocked(text);
}

bool JumpHistory::IsOpen() const { std::lock_guard lock(mu_); return open_; }
void JumpHistory::SetEnabled(bool enabled) { std::lock_guard lock(mu_); enabled_ = enabled; }
bool JumpHistory::Enabled() const { std::lock_guard lock(mu_); return enabled_; }
size_t JumpHistory::Size() const { std::lock_guard lock(mu_); return entries_.size(); }

JumpHistory::Entry* JumpHistory::FindLocked(const std::wstring& path) {
    const std::wstring key = Key(path);
    for (auto& e : entries_) if (e.key == key) return &e;
    return nullptr;
}
const JumpHistory::Entry* JumpHistory::FindLocked(const std::wstring& path) const {
    return const_cast<JumpHistory*>(this)->FindLocked(path);
}

void JumpHistory::Record(const std::wstring& path, const std::wstring& app, int64_t now) {
    if (path.empty()) return;
    std::lock_guard lock(mu_);
    if (!enabled_) return;
    Entry* e = FindLocked(path);
    if (!e) {
        entries_.push_back({});
        e = &entries_.back();
        e->count = 1;
    }
    else if (e->count && e->count != UINT64_MAX) ++e->count;
    e->path = Plain(path);
    e->key = Key(e->path);
    e->score = Decayed(e->score, e->last, now) + 1.0;
    e->last = now;
    const std::wstring name = Lower(app);
    if (!name.empty()) {
        auto it = std::find_if(e->apps.begin(), e->apps.end(), [&](const Visits& v) { return v.app == name; });
        if (it == e->apps.end()) {
            if (e->apps.size() >= kMaxApps) {
                // Make room by dropping the program with the least left.
                it = std::min_element(e->apps.begin(), e->apps.end(), [&](const Visits& a, const Visits& b) {
                    return Decayed(a.score, a.last, now) < Decayed(b.score, b.last, now);
                });
                *it = Visits{name, 0, now};
            } else {
                it = e->apps.insert(e->apps.end(), Visits{name, 0, now});
            }
        }
        it->score = Decayed(it->score, it->last, now) + 1.0;
        it->last = now;
    }
    if (entries_.size() > kMaxEntries) {
        auto weakest = std::min_element(entries_.begin(), entries_.end(), [&](const Entry& a, const Entry& b) {
            return Decayed(a.score, a.last, now) < Decayed(b.score, b.last, now);
        });
        entries_.erase(weakest);
    }
    SaveLocked();
}

double JumpHistory::ScoreLocked(const Entry& e, const std::wstring& app, int64_t now) const {
    double score = Decayed(e.score, e.last, now);
    if (!app.empty())
        for (const auto& v : e.apps)
            if (v.app == app) score += Decayed(v.score, v.last, now);
    return score;
}

double JumpHistory::Score(const std::wstring& path, const std::wstring& app, int64_t now) const {
    std::lock_guard lock(mu_);
    if (!enabled_) return 0;
    const Entry* e = FindLocked(path);
    return e ? ScoreLocked(*e, Lower(app), now) : 0;
}

std::vector<std::wstring> JumpHistory::Top(const std::wstring& app, size_t limit, int64_t now) const {
    std::vector<std::wstring> paths;
    std::lock_guard lock(mu_);
    if (!enabled_) return paths;
    const std::wstring name = Lower(app);
    std::vector<std::pair<double, size_t>> order;
    order.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) order.push_back({ScoreLocked(entries_[i], name, now), i});
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [score, i] : order) {
        if (paths.size() >= limit) break;
        paths.push_back(entries_[i].path);
    }
    return paths;
}

void JumpHistory::Clear() {
    std::lock_guard lock(mu_);
    entries_.clear();
    SaveLocked();
}

int64_t JumpHistory::LastUsed(const std::wstring& path) const {
    std::lock_guard lock(mu_);
    const auto* entry = enabled_ ? FindLocked(path) : nullptr;
    return entry ? entry->last : 0;
}

uint64_t JumpHistory::VisitCount(const std::wstring& path) const {
    std::lock_guard lock(mu_);
    const auto* entry = enabled_ ? FindLocked(path) : nullptr;
    return entry ? entry->count : 0;
}

std::wstring JumpHistory::SerializeLocked() const {
    std::wstring text;
    wchar_t number[64];
    for (const auto& e : entries_) {
        if (e.path.find_first_of(L"\t\r\n") != std::wstring::npos) continue;
        text += e.path;
        swprintf_s(number, L"\t%.4f\t%lld", e.score, static_cast<long long>(e.last));
        text += number;
        for (const auto& v : e.apps) {
            if (v.app.find_first_of(L"\t\r\n") != std::wstring::npos) continue;
            text += L'\t';
            text += v.app;
            swprintf_s(number, L"\t%.4f\t%lld", v.score, static_cast<long long>(v.last));
            text += number;
        }
        text += L'\n';
        // Older readers ignore this zero-score metadata row instead of consuming an app slot.
        if (e.count) text += L"#count-v1\t" + e.path + L"\t" + std::to_wstring(e.count) + L"\n";
    }
    return text;
}

std::wstring JumpHistory::Serialize() const {
    std::lock_guard lock(mu_);
    return SerializeLocked();
}

void JumpHistory::SaveLocked() const {
    if (file_.empty()) return;
    if (entries_.empty()) {
        DeleteFileW(file_.c_str());
        return;
    }
    WriteUtf8FileAtomic(file_, SerializeLocked());
}

std::wstring ProcessExeName(unsigned long pid) {
    std::wstring name;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return name;
    wchar_t path[MAX_PATH * 2]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const wchar_t* leaf = wcsrchr(path, L'\\');
        name = Lower(leaf ? leaf + 1 : path);
    }
    CloseHandle(process);
    return name;
}
}
