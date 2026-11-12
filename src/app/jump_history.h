// jump_history.h - where the user goes, so frequent places can come first.
//
// Every jump made through the dialog jump bar or quick search is recorded
// against the folder: a visit count that fades with a two-week half-life
// (frecency), and the same per program, so a dialog of the photo editor ranks
// the photo folders first. Kept in a small file in Pulse's data directory and
// nowhere else. Shared by the UI thread and the jump bar's thread.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::app {
class JumpHistory {
public:
    // Loads `file` once; an empty name keeps the history in memory only.
    void Open(const std::wstring& file);
    bool IsOpen() const;
    // Off: nothing is recorded and every score is zero; what is stored stays.
    void SetEnabled(bool enabled);
    bool Enabled() const;

    // `app`: the program the jump was made in (exe name), may be empty.
    void Record(const std::wstring& path, const std::wstring& app, int64_t now = Now());
    // Decayed visits, plus the visits made from `app` once more.
    double Score(const std::wstring& path, const std::wstring& app, int64_t now = Now()) const;
    // Highest score first.
    std::vector<std::wstring> Top(const std::wstring& app, size_t limit, int64_t now = Now()) const;
    int64_t LastUsed(const std::wstring& path) const;
    // Zero means an unknown lifetime total (legacy records), never a decayed score.
    uint64_t VisitCount(const std::wstring& path) const;
    size_t Size() const;
    void Clear();

    static int64_t Now();   // seconds since 1970
    static constexpr double kHalfLife = 14.0 * 24 * 3600;
    static constexpr size_t kMaxEntries = 300;
    static constexpr size_t kMaxApps = 4;

    // The file format; Parse also detaches from the file (tests).
    std::wstring Serialize() const;
    void Parse(const std::wstring& text);

private:
    struct Visits { std::wstring app; double score = 0; int64_t last = 0; };
    struct Entry { std::wstring path, key; double score = 0; int64_t last = 0; std::vector<Visits> apps; uint64_t count = 0; };
    static double Decayed(double score, int64_t last, int64_t now);
    double ScoreLocked(const Entry& e, const std::wstring& app, int64_t now) const;
    Entry* FindLocked(const std::wstring& path);
    const Entry* FindLocked(const std::wstring& path) const;
    void ParseLocked(const std::wstring& text);
    std::wstring SerializeLocked() const;
    void SaveLocked() const;

    mutable std::mutex mu_;
    std::vector<Entry> entries_;
    std::wstring file_;
    bool open_ = false;
    bool enabled_ = true;
};

// Lower-case file name of the program behind `pid` ("photoshop.exe").
std::wstring ProcessExeName(unsigned long pid);
}
