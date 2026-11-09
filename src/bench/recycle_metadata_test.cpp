// Compile the real host's private read/restore step, without calling wWinMain.
// Never call RestoreOneFromRecycle here: its legacy lookup scans the real bin.
#include "../shell_host/main.cpp"
#include "../fs/fs_recycle.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace pulse::review {
bool short_read = false;
DWORD RecycleReadAmountForReview(DWORD requested) {
    return short_read && requested > 2 ? requested - 2 : requested;
}
}

namespace {
namespace files = std::filesystem;
constexpr char kBody[] = "fixture-body";
int failures = 0;
void Check(bool ok, const char* text) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << '\n';
    if (!ok) ++failures;
}
void Put(std::vector<BYTE>& bytes, size_t offset, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i) bytes.at(offset + i) = static_cast<BYTE>(value >> (8 * i));
}
std::vector<BYTE> Record(unsigned version, const std::wstring& path) {
    static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 fixture required");
    if (version == 1 && path.size() >= 260) throw std::runtime_error("v1 fixture path too long");
    const size_t offset = version == 1 ? 24 : 28;
    std::vector<BYTE> bytes(offset + (version == 1 ? 260 : path.size() + 1) * 2, 0);
    Put(bytes, 0, version, 8); Put(bytes, 8, sizeof(kBody) - 1, 8);
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    Put(bytes, 16, ft.dwLowDateTime, 4); Put(bytes, 20, ft.dwHighDateTime, 4);
    if (version == 2) Put(bytes, 24, path.size() + 1, 4);
    for (size_t i = 0; i < path.size(); ++i) Put(bytes, offset + 2 * i, static_cast<uint16_t>(path[i]), 2);
    return bytes;
}
void WriteBytes(const files::path& path, const std::vector<BYTE>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!out) throw std::runtime_error("fixture write failed");
}
void WriteText(const files::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc); out << text;
    if (!out) throw std::runtime_error("fixture write failed");
}
std::string ReadText(const files::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
struct Pair {
    files::path index, content, target;
};
Pair MakePair(const files::path& root, const std::wstring& stem, unsigned version,
              const std::wstring& leaf, bool content = true) {
    Pair pair{root / (L"$I" + stem), root / (L"$R" + stem), root / L"dest" / leaf};
    WriteBytes(pair.index, Record(version, pair.target.wstring()));
    if (content) WriteText(pair.content, kBody);
    return pair;
}
void RunMalformedMatrix(const files::path& root) {
    const auto pair = MakePair(root, L"matrix", 2, L"matrix.txt");
    const auto v2 = Record(2, pair.target.wstring());
    const auto v1 = Record(1, pair.target.wstring());
    const auto rejected_by_both = [&](const std::vector<BYTE>& bytes) {
        WriteBytes(pair.index, bytes);
        pulse::fs::RecycleItem item; std::wstring path;
        const bool listing = pulse::fs::ReadRecycleIndex(pair.index.wstring(), item);
        const bool restore = ReadRecycleOriginal(pair.index.wstring(), path);
        return !listing && !restore;
    };
    for (const auto& bytes : {v1, v2}) {
        size_t accepted = 0;
        // Reads only: a short prefix can point outside the fixture, so never
        // pass these arbitrary prefixes to the actual restore/move function.
        for (size_t n = 0; n < bytes.size(); ++n) {
            std::vector<BYTE> prefix(bytes.begin(), bytes.begin() + n);
            if (!rejected_by_both(prefix)) ++accepted;
        }
        std::cout << "[INFO] truncated prefixes=" << bytes.size() << " accepted by at least one reader=" << accepted << '\n';
        Check(accepted == 0, "M03-003 every byte-prefix truncation of a healthy v1 or v2 record is rejected by both readers");
    }
    std::vector<std::pair<const char*, std::vector<BYTE>>> cases;
    auto add = [&](const char* label, std::vector<BYTE> bytes) { cases.emplace_back(label, std::move(bytes)); };
    auto bytes = v2; Put(bytes, 0, 99, 8); add("unknown version", bytes);
    bytes = v2; Put(bytes, 24, 0, 4); add("zero declared length", bytes);
    bytes = v2; Put(bytes, 24, 32769, 4); add("over-limit declared length", bytes);
    bytes = v2; Put(bytes, 24, pair.target.wstring().size() + 2, 4); add("declared length beyond actual bytes", bytes);
    bytes = v2; Put(bytes, 24, pair.target.wstring().size(), 4); add("declared length shorter than record", bytes);
    bytes = v2; bytes.push_back(0); add("odd extra byte", bytes);
    bytes = v2; bytes.push_back(0); bytes.push_back(0); add("undeclared trailing data", bytes);
    bytes = v2; Put(bytes, bytes.size() - 2, L'x', 2); add("missing v2 terminal NUL", bytes);
    bytes = v2; Put(bytes, 28 + 4 * 2, 0, 2); add("embedded NUL before declared v2 end", bytes);
    bytes = v1; for (size_t i = 3; i < 260; ++i) Put(bytes, 24 + i * 2, L'a', 2);
    add("unterminated v1 fixed field", bytes);
    bytes = v1; bytes.push_back(0); bytes.push_back(0); add("oversized v1 record", bytes);
    add("relative path", Record(2, L"notes.txt"));
    add("drive-relative path", Record(2, L"C:notes.txt"));
    add("root-relative path", Record(2, L"\\notes.txt"));
    add("drive root without an item", Record(2, L"C:\\"));
    add("device namespace", Record(2, L"\\\\.\\pipe\\review"));
    add("unknown extended namespace that would become relative", Record(2, L"\\\\?\\Volume{review}\\item"));
    add("empty path", Record(2, L""));
    for (const auto& [label, data] : cases) {
        const std::string text = std::string("M03-003 both readers reject ") + label;
        Check(rejected_by_both(data), text.c_str());
    }
    for (unsigned version : {1u, 2u}) {
        WriteBytes(pair.index, Record(version, pair.target.wstring()));
        pulse::review::short_read = true;
        pulse::fs::RecycleItem item; std::wstring path;
        const bool listing = pulse::fs::ReadRecycleIndex(pair.index.wstring(), item);
        const bool restore = ReadRecycleOriginal(pair.index.wstring(), path);
        pulse::review::short_read = false;
        Check(!listing && !restore, "M03-003 simulated short read cannot be repaired from unread zero-filled buffer bytes");
    }
    // Metadata parsing only, never connect to these UNC names or move to them.
    for (const auto& path : {std::wstring(L"\\\\server\\share\\folder\\名称.txt"),
                            std::wstring(L"\\\\?\\") + pair.target.wstring(),
                            std::wstring(L"\\\\?\\UNC\\server\\share\\folder\\name.txt")}) {
        WriteBytes(pair.index, Record(2, path));
        pulse::fs::RecycleItem item; std::wstring parsed;
        Check(pulse::fs::ReadRecycleIndex(pair.index.wstring(), item) && item.original_path == path &&
              ReadRecycleOriginal(pair.index.wstring(), parsed) && parsed == path,
              "M03-003 supported absolute and extended path data remain exact without network access");
    }
    Check(files::exists(pair.index) && ReadText(pair.content) == kBody && !files::exists(pair.target),
          "M03-003 malformed-record and short-read matrix never moves or deletes its payload");
}

void RunCases(const files::path& root) {
    files::create_directory(root / L"dest");
    const auto bad = MakePair(root, L"truncated", 2, L"photo.jpg");
    auto bytes = Record(2, bad.target.wstring());
    // Drop .jpg plus NUL, leaving another valid name in the same owned folder.
    bytes.resize(28 + (bad.target.wstring().size() - 4) * 2);
    WriteBytes(bad.index, bytes);
    pulse::fs::RecycleItem item;
    std::wstring original;
    Check(!pulse::fs::ReadRecycleIndex(bad.index.wstring(), item),
          "M03-003 listing reader rejects a v2 path shorter than its declared length");
    Check(!ReadRecycleOriginal(bad.index.wstring(), original),
          "M03-003 restore reader rejects the same truncated v2 metadata");
    Check(files::exists(bad.index) && ReadText(bad.content) == kBody,
          "M03-003 metadata reads alone preserve the isolated index and payload");
    std::wstring error;
    const bool restored_bad = RestoreSelectedRecyclePayload(bad.content.wstring(), error);
    const auto short_target = root / L"dest" / L"photo";
    std::cout << "[INFO] malformed restore accepted=" << restored_bad
              << " parsed_short_target=" << (original == short_target.wstring())
              << " wrong_target_exists=" << files::exists(short_target)
              << " wrong_target_body=" << (ReadText(short_target) == kBody)
              << " original_target_exists=" << files::exists(bad.target)
              << " index_left=" << files::exists(bad.index)
              << " payload_left=" << files::exists(bad.content) << '\n';
    Check(!restored_bad,
          "M03-003 real restore step refuses corrupted metadata rather than renaming the payload");
    Check(files::exists(bad.index) && ReadText(bad.content) == kBody,
          "M03-003 rejected restore preserves both index and original payload");
    Check(!files::exists(root / L"dest" / L"photo") && !files::exists(bad.target),
          "M03-003 rejected restore creates neither the truncated nor the original target");

    for (unsigned version : {1u, 2u}) {
        const auto good = MakePair(root, L"valid" + std::to_wstring(version), version,
                                   L"合法 文件" + std::to_wstring(version) + L".txt");
        item = {}; original.clear();
        Check(pulse::fs::ReadRecycleIndex(good.index.wstring(), item) && item.original_path == good.target.wstring() && item.size == sizeof(kBody) - 1,
              "M03-003 valid v1 or v2 listing preserves exact Unicode path and size");
        Check(ReadRecycleOriginal(good.index.wstring(), original) && original == good.target.wstring(),
              "M03-003 valid v1 or v2 restore reader preserves the same exact path");
        error.clear();
        Check(RestoreSelectedRecyclePayload(good.content.wstring(), error) && ReadText(good.target) == kBody &&
              !files::exists(good.index) && !files::exists(good.content),
              "M03-003 valid isolated restore moves exact content and retires only its own metadata");
    }
    const auto occupied = MakePair(root, L"occupied", 2, L"occupied.txt");
    const auto untouched = MakePair(root, L"untouched", 2, L"untouched.txt");
    const std::string untouched_index = ReadText(untouched.index);
    WriteText(occupied.target, "existing target must survive"); error.clear();
    const bool collision_restored = RestoreSelectedRecyclePayload(occupied.content.wstring(), error);
    const auto sibling = occupied.target.parent_path() / L"occupied (2).txt";
    Check(collision_restored && ReadText(occupied.target) == "existing target must survive",
          "M03-003 target collision never overwrites existing content");
    Check(ReadText(sibling) == kBody && !files::exists(occupied.index) && !files::exists(occupied.content),
          "M03-003 collision restores exact payload under next free name and consumes selected metadata");
    Check(ReadText(untouched.index) == untouched_index && ReadText(untouched.content) == kBody &&
          !files::exists(untouched.target), "M03-003 collision restore leaves unrelated index and payload untouched");
    RunMalformedMatrix(root);
    const auto orphan = MakePair(root, L"orphan", 2, L"orphan.txt", false);
    item = {}; error.clear();
    Check(!pulse::fs::ReadRecycleIndex(orphan.index.wstring(), item),
          "M03-003 orphan index is not listed as a restorable item");
    Check(!RestoreSelectedRecyclePayload(orphan.content.wstring(), error) && files::exists(orphan.index) && !files::exists(orphan.target),
          "M03-003 missing payload cannot remove its index or create a target");
}
}
int wmain() {
    std::cout << "[INFO] only privately created metadata, payloads and destinations; no real Recycle Bin access\n";
    const files::path root = files::current_path() / L"bench_data" /
        (L"review-recycle-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    const bool created = files::create_directory(root, ec);
    Check(created && !ec, "M03-003 unique owned fixture directory created");
    if (!created || ec) return 2;
    try { RunCases(root); }
    catch (const std::exception& error) { std::cout << "[FAIL] fixture exception: " << error.what() << '\n'; ++failures; }
    files::remove_all(root, ec);
    Check(!ec && !files::exists(root), "M03-003 only the owned fixture and its test destinations are cleaned up");
    return failures ? 1 : 0;
}
