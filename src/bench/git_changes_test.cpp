// git_changes_test: the Source Control window's git layer (git_changes.cpp).
// Parser and quoting cases are pure; with a fixture folder argument the test
// also drives a real repository there (stage, unstage, discard, commit, blob
// reads, odd file names). It never touches the Recycle Bin or global config.
//
//   pulse_git_changes_test [fixture_dir]
#include "../app/git_changes.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pulse::app;

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
        std::printf("PASS %s\n", what);
    } else {
        ++g_fail;
        std::printf("FAIL %s\n", what);
    }
}

std::string Z(std::initializer_list<const char*> recs) {
    std::string out;
    for (const char* r : recs) {
        out += r;
        out.push_back('\0');
    }
    return out;
}

void ParserCases() {
    GitChangeSet set;
    const std::string out = Z({
        "# branch.oid 1234567890abcdef1234567890abcdef12345678",
        "# branch.head main",
        "# branch.upstream origin/main",
        "# branch.ab +2 -1",
        "1 M. N... 100644 100644 100644 aaa bbb src/staged.cpp",
        "1 .M N... 100644 100644 100644 aaa bbb src/work.cpp",
        "1 MM N... 100644 100644 100644 aaa bbb both.txt",
        "1 A. N... 000000 100644 100644 000 bbb new file.txt",
        "1 .D N... 100644 100644 000000 aaa aaa gone.txt",
        "2 R. N... 100644 100644 100644 aaa aaa R100 docs/new name.md",
        "docs/old name.md",
        "1 .M S.M. 160000 160000 160000 aaa aaa vendor/lib",
        "u UU N... 100644 100644 100644 100644 a b c conflict.txt",
        "? \xE4\xB8\xAD\xE6\x96\x87.txt",
        "? dir/untracked.bin",
        "! ignored.log",
    });
    Check(ParseGitChanges(out, set), "parse: header seen");
    Check(set.branch == L"main" && set.head == L"1234567" && set.upstream == L"origin/main", "parse: branch fields");
    Check(set.ahead == 2 && set.behind == 1 && set.has_upstream_ab, "parse: ahead/behind");
    Check(set.staged.size() == 4, "parse: 4 staged (M., MM, A., R.)");
    Check(set.unstaged.size() == 6, "parse: 6 unstaged (.M, MM, .D, submodule, 2 untracked)");
    Check(set.conflicts.size() == 1 && set.conflicts[0].path == L"conflict.txt", "parse: conflict");
    bool rename_ok = false, space_ok = false, sub_ok = false;
    for (const auto& c : set.staged) {
        if (c.kind == GitChangeKind::Renamed && c.path == L"docs/new name.md" && c.orig_path == L"docs/old name.md")
            rename_ok = true;
        if (c.kind == GitChangeKind::Added && c.path == L"new file.txt") space_ok = true;
    }
    for (const auto& c : set.unstaged)
        if (c.path == L"vendor/lib" && c.submodule) sub_ok = true;
    Check(rename_ok, "parse: rename keeps source path");
    Check(space_ok, "parse: path with a space");
    Check(sub_ok, "parse: submodule flag");
    Check(set.unstaged.size() >= 2 && set.unstaged[set.unstaged.size() - 2].kind == GitChangeKind::Untracked &&
              set.unstaged[set.unstaged.size() - 2].path == L"\u4E2D\u6587.txt",
          "parse: untracked last, UTF-8 name");
    Check(set.unstaged[1].kind == GitChangeKind::Modified && set.unstaged[1].path == L"both.txt",
          "parse: MM listed on both sides");
    GitChangeSet initial;
    Check(ParseGitChanges(Z({"# branch.oid (initial)", "# branch.head main"}), initial) && !initial.HasHead(),
          "parse: initial branch has no HEAD");
    GitChangeSet junk;
    Check(!ParseGitChanges("fatal: not a git repository", junk), "parse: non-status text rejected");
}

void QuoteCases() {
    Check(GitQuoteArg(L"a b") == L"\"a b\"", "quote: space");
    Check(GitQuoteArg(L"C:\\dir\\") == L"\"C:\\dir\\\\\"", "quote: trailing backslash doubled");
    Check(GitQuoteArg(L"say \"hi\"") == L"\"say \\\"hi\\\"\"", "quote: embedded quotes");
    Check(GitQuoteArg(L"a\\\"b") == L"\"a\\\\\\\"b\"", "quote: backslash before quote");
    Check(GitJoinPath(L"C:\\repo\\", L"a/b c.txt") == L"C:\\repo\\a\\b c.txt", "join: separators");
    Check(GitIsImagePath(L"x/Logo.PNG") && !GitIsImagePath(L"readme.md"), "image extensions");
    Check(GitChangeLetter(GitChangeKind::Untracked) == L'U' && GitChangeLetter(GitChangeKind::Renamed) == L'R',
          "letters");
}

bool WriteText(const std::wstring& path, const char* text) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, text, static_cast<DWORD>(strlen(text)), &wrote, nullptr);
    CloseHandle(f);
    return ok != FALSE;
}

std::string ReadAll(const std::wstring& path) {
    std::string out;
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(f);
    return out;
}

const GitChange* Find(const std::vector<GitChange>& list, const std::wstring& path) {
    for (const auto& c : list)
        if (c.path == path) return &c;
    return nullptr;
}

void RepoCases(const std::wstring& base) {
    const std::wstring root = base + L"\\repo-" + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(root.c_str(), nullptr)) {
        Check(false, "repo: create fixture folder");
        return;
    }
    auto git = [&](const wchar_t* args) { return RunGit(root, args, 60000, true); };
    const GitRunResult init = git(L"init -q -b main");
    Check(init.started && init.exit_code == 0, "repo: git init");
    if (init.exit_code != 0) std::printf("  started=%d exit=%lu out=%s\n", init.started ? 1 : 0, init.exit_code, init.out.c_str());
    git(L"config user.name \"Pulse Test\"");
    git(L"config user.email test@pulse.invalid");
    git(L"config commit.gpgsign false");
    git(L"config core.autocrlf false");

    const std::wstring odd = L"\u4E2D\u6587 odd [1] (x).txt";  // spaces, glob brackets, CJK
    Check(WriteText(GitJoinPath(root, L"a.txt"), "one\ntwo\nthree\n"), "repo: write a.txt");
    Check(WriteText(GitJoinPath(root, odd), "odd\n"), "repo: write odd name");

    GitChangeSet set;
    GitOpResult r = GitReadChanges(root, set);
    Check(r.ok && !set.HasHead() && set.unstaged.size() == 2 && set.staged.empty(), "repo: 2 untracked before first commit");

    r = GitStagePaths(root, {L"a.txt", odd});
    Check(r.ok, "repo: stage both (odd name literal)");
    GitReadChanges(root, set);
    Check(set.staged.size() == 2 && set.unstaged.empty(), "repo: both staged");

    r = GitUnstagePaths(root, {odd}, set.HasHead());
    Check(r.ok, "repo: unstage before first commit (rm --cached)");
    GitReadChanges(root, set);
    Check(set.staged.size() == 1 && Find(set.unstaged, odd) && Find(set.unstaged, odd)->kind == GitChangeKind::Untracked,
          "repo: odd name back to untracked");

    r = GitCommit(root, L"\u9996\u6B21\u63D0\u4EA4 first commit\r\n\r\nbody line", false);
    Check(r.ok, "repo: commit with CJK message");
    Check(GitLastCommitMessage(root) == L"\u9996\u6B21\u63D0\u4EA4 first commit\n\nbody line", "repo: message round-trips");
    GitReadChanges(root, set);
    Check(set.HasHead() && set.branch == L"main" && set.staged.empty(), "repo: HEAD exists, index clean");

    WriteText(GitJoinPath(root, L"a.txt"), "one\nTWO\nthree\nfour\n");
    GitReadChanges(root, set);
    const GitChange* a = Find(set.unstaged, L"a.txt");
    Check(a && a->kind == GitChangeKind::Modified, "repo: a.txt modified");

    std::string blob;
    Check(GitReadBlob(root, GitBlobSource::Head, L"a.txt", blob, 1 << 20) && blob == "one\ntwo\nthree\n", "repo: HEAD blob");
    Check(GitReadBlob(root, GitBlobSource::Index, L"a.txt", blob, 1 << 20) && blob == "one\ntwo\nthree\n", "repo: index blob");
    Check(!GitReadBlob(root, GitBlobSource::Head, odd, blob, 1 << 20), "repo: untracked has no HEAD blob");
    bool too_large = false;
    Check(!GitReadBlob(root, GitBlobSource::Head, L"a.txt", blob, 4, &too_large) && too_large, "repo: size cap respected");

    GitStagePaths(root, {L"a.txt"});
    WriteText(GitJoinPath(root, L"a.txt"), "one\nTWO\nthree\nfour\nfive\n");
    GitReadChanges(root, set);
    Check(Find(set.staged, L"a.txt") && Find(set.unstaged, L"a.txt"), "repo: MM after stage + edit");
    Check(GitReadBlob(root, GitBlobSource::Index, L"a.txt", blob, 1 << 20) && blob == "one\nTWO\nthree\nfour\n",
          "repo: index blob is the staged version");

    r = GitDiscardPaths(root, {L"a.txt"});
    Check(r.ok && ReadAll(GitJoinPath(root, L"a.txt")) == "one\nTWO\nthree\nfour\n", "repo: discard restores index version");
    GitReadChanges(root, set);
    Check(Find(set.staged, L"a.txt") && !Find(set.unstaged, L"a.txt"), "repo: only the staged change left");

    r = GitUnstagePaths(root, {L"a.txt"}, true);
    GitReadChanges(root, set);
    Check(r.ok && !Find(set.staged, L"a.txt") && Find(set.unstaged, L"a.txt"), "repo: unstage with HEAD (restore --staged)");

    DeleteFileW(GitJoinPath(root, L"a.txt").c_str());
    GitReadChanges(root, set);
    a = Find(set.unstaged, L"a.txt");
    Check(a && a->kind == GitChangeKind::Deleted, "repo: deletion seen");
    r = GitStagePaths(root, {L"a.txt"});
    GitReadChanges(root, set);
    a = Find(set.staged, L"a.txt");
    Check(r.ok && a && a->kind == GitChangeKind::Deleted, "repo: staging a deletion");
    GitUnstagePaths(root, {L"a.txt"}, true);
    r = GitDiscardPaths(root, {L"a.txt"});
    Check(r.ok && ReadAll(GitJoinPath(root, L"a.txt")) == "one\ntwo\nthree\n", "repo: discard brings a deleted file back");

    r = GitCommit(root, L"   ", false);
    Check(!r.ok && !r.message.empty(), "repo: empty message refused");
    r = GitCommit(root, L"nothing staged", false);
    Check(!r.ok && !r.message.empty(), "repo: commit with nothing staged fails with git's words");
    r = GitCommit(root, L"", true);
    Check(r.ok && GitLastCommitMessage(root).starts_with(L"\u9996\u6B21"), "repo: amend --no-edit keeps message");

    const std::wstring before = GitHeadId(root);
    WriteText(GitJoinPath(root, L"b.txt"), "b\n");
    GitStagePaths(root, {L"b.txt"});
    Check(GitCommit(root, L"second", false).ok, "repo: second commit");
    const std::wstring second = GitHeadId(root);
    Check(!second.empty() && second != before, "repo: head id moved");
    Check(!GitUndoCommit(root, before).ok, "repo: undo refuses a stale head");
    r = GitUndoCommit(root, second);
    GitReadChanges(root, set);
    Check(r.ok && GitHeadId(root) == before && Find(set.staged, L"b.txt"), "repo: undo keeps the change staged");

    // First-commit undo in a fresh repository.
    const std::wstring root2 = root + L"-first";
    CreateDirectoryW(root2.c_str(), nullptr);
    RunGit(root2, L"init -q -b main", 60000, true);
    RunGit(root2, L"config user.name \"Pulse Test\"", 60000, true);
    RunGit(root2, L"config user.email test@pulse.invalid", 60000, true);
    RunGit(root2, L"config commit.gpgsign false", 60000, true);
    WriteText(GitJoinPath(root2, L"x.txt"), "x\n");
    GitStagePaths(root2, {L"x.txt"});
    GitCommit(root2, L"only", false);
    r = GitUndoCommit(root2, GitHeadId(root2));
    GitChangeSet set2;
    GitReadChanges(root2, set2);
    Check(r.ok && !set2.HasHead() && Find(set2.staged, L"x.txt"), "repo: undo of the first commit leaves it staged");

    const std::wstring tmp = GitWriteTempFile("x", L"bad:name?.txt");
    Check(!tmp.empty() && ReadAll(tmp) == "x", "temp file with unsafe hint");
    if (!tmp.empty()) DeleteFileW(tmp.c_str());

    // Identity, hunk patch, blame, and file summary on a real repo.
    const GitIdentity id = GitReadIdentity(root);
    Check(id.name == L"Pulse Test" && id.email == L"test@pulse.invalid", "repo: read git identity");

    WriteText(GitJoinPath(root, L"hunk.txt"), "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10\nL11\nL12\n");
    GitStagePaths(root, {L"hunk.txt"});
    GitCommit(root, L"add hunk.txt", false);

    // Modify lines 2 and 11 -> two separate hunks.
    WriteText(GitJoinPath(root, L"hunk.txt"), "L1\nL2-edit\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10\nL11-edit\nL12\n");
    std::vector<GitDiffHunkPatch> hunks;
    Check(GitReadFileHunks(root, L"hunk.txt", false, hunks).ok && hunks.size() == 2,
          "repo: read 2 separate hunks");
    if (hunks.size() == 2) {
        Check(GitApplyHunkPatch(root, hunks[0].patch_bytes, true, false).ok, "repo: stage first hunk only");
        std::vector<GitDiffHunkPatch> staged_hunks, unstaged_hunks;
        GitReadFileHunks(root, L"hunk.txt", true, staged_hunks);
        GitReadFileHunks(root, L"hunk.txt", false, unstaged_hunks);
        Check(staged_hunks.size() == 1 && unstaged_hunks.size() == 1,
              "repo: 1 staged hunk and 1 unstaged hunk left");
        if (!unstaged_hunks.empty()) {
            Check(GitApplyHunkPatch(root, unstaged_hunks[0].patch_bytes, false, true).ok,
                  "repo: revert second hunk in worktree");
        }
        Check(ReadAll(GitJoinPath(root, L"hunk.txt")) == "L1\nL2-edit\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10\nL11\nL12\n",
              "repo: hunk revert kept first hunk edit");
    }

    std::vector<GitBlameLine> blame_lines;
    Check(GitReadBlame(root, L"hunk.txt", blame_lines) && blame_lines.size() == 12 &&
              blame_lines[1].uncommitted && !blame_lines[0].uncommitted && blame_lines[0].author == L"Pulse Test",
          "repo: blame marks uncommitted line 2 and committed author");

    const GitFileSummary summary = GitReadFileSummary(root, L"hunk.txt");
    Check(summary.valid && summary.staged && summary.added == 1 && summary.deleted == 1 &&
              !summary.preview_lines.empty() && !summary.history.empty(),
          "repo: file summary reads numstat, preview lines and history");

    std::printf("fixture %ls\n", root.c_str());
}

void ExtendedUnitCases() {
    // Dir counts in GitRepoStatus
    GitRepoStatus st;
    const std::string v2 =
        "# branch.oid 1111111111111111111111111111111111111111\0"
        "# branch.head main\0"
        "1 .M N... 100644 100644 100644 a b docs/a.md\0"
        "1 .M N... 100644 100644 100644 a b docs/sub/b.md\0"
        "? docs/new/\0";
    // Build with explicit size because of embedded NULs.
    const char raw_v2[] =
        "# branch.oid 1111111111111111111111111111111111111111\0"
        "# branch.head main\0"
        "1 .M N... 100644 100644 100644 a b docs/a.md\0"
        "1 .M N... 100644 100644 100644 a b docs/sub/b.md\0"
        "? docs/new/\0";
    Check(ParseGitStatusV2(std::string_view(raw_v2, sizeof(raw_v2) - 1), st), "status: dir_counts parsed");
    Check(GitDirChangeCount(st, L"docs") == 3 && GitDirChangeCount(st, L"docs\\sub") == 1 &&
              GitDirChangeCount(st, L"docs\\new") == 1,
          "status: recursive dir change counts match");
    (void)v2;

    // Suggest commit message
    std::vector<GitChange> staged;
    staged.push_back({L"docs/proposal.md", {}, GitChangeKind::Modified});
    Check(!GitSuggestCommitMessage(staged, true).empty(), "suggest: 1 file");
    staged.push_back({L"hero.png", {}, GitChangeKind::Added});
    staged.push_back({L"build.py", {}, GitChangeKind::Modified});
    Check(GitSuggestCommitMessage(staged, true).find(L"3") != std::wstring::npos, "suggest: 3 files includes count");

    // ParseUnifiedDiffHunks
    const char raw_diff[] =
        "diff --git a/f.txt b/f.txt\n"
        "index 1111111..2222222 100644\n"
        "--- a/f.txt\n"
        "+++ b/f.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n"
        "@@ -10,2 +10,3 @@\n"
        " x\n"
        "+y\n"
        " z\n";
    const auto parsed_hunks = ParseUnifiedDiffHunks(raw_diff);
    Check(parsed_hunks.size() == 2 && parsed_hunks[0].old_start == 1 && parsed_hunks[1].new_start == 10 &&
              parsed_hunks[1].patch_bytes.starts_with("diff --git "),
          "hunks: split unified diff into standalone patches");

    // ParseGitBlamePorcelain (repeated commit metadata cache + uncommitted)
    const char raw_blame[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1 1 2\n"
        "author Alice\n"
        "author-time 1700000000\n"
        "\tfirst line\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 2 2\n"
        "\tsecond line\n"
        "0000000000000000000000000000000000000000 3 3 1\n"
        "author Not Committed Yet\n"
        "author-time 1700000100\n"
        "\tedited line\n";
    std::vector<GitBlameLine> bl;
    Check(ParseGitBlamePorcelain(raw_blame, bl) && bl.size() == 3 &&
              bl[0].author == L"Alice" && bl[1].author == L"Alice" &&
              !bl[0].uncommitted && bl[2].uncommitted,
          "blame: porcelain parses repeated hashes and uncommitted line");

    // Dual-track GitMarkLabel & GitDirBadgeText formatting with real string resources
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const std::wstring zh_mod_plain = pulse::ui::GitMarkLabel(pulse::ui::GitMark::Modified, true);
    const std::wstring zh_mod_tech = pulse::ui::GitMarkLabel(pulse::ui::GitMark::Modified, false);
    const std::wstring zh_dir_plain = pulse::ui::GitDirBadgeText(pulse::ui::GitMark::Modified, 3, true);
    const std::wstring zh_dir_tech = pulse::ui::GitDirBadgeText(pulse::ui::GitMark::Modified, 3, false);
    const std::wstring zh_dir_conf = pulse::ui::GitDirBadgeText(pulse::ui::GitMark::Conflict, 2, true);
    Check(!zh_mod_plain.empty() && !zh_mod_tech.empty() && zh_mod_plain != zh_mod_tech &&
              zh_dir_plain.find(L"3") != std::wstring::npos &&
              zh_dir_tech.find(L"3") != std::wstring::npos &&
              zh_dir_tech.find(zh_mod_tech) != std::wstring::npos &&
              zh_dir_conf == pulse::ui::GitMarkLabel(pulse::ui::GitMark::Conflict, true) &&
              pulse::ui::GitMarkLabel(pulse::ui::GitMark::None, true).empty() &&
              pulse::ui::GitDirBadgeText(pulse::ui::GitMark::Modified, 0, true).empty(),
          "mark: zh-CN plain vs tech labels and directory summaries");

    pulse::l10n::SetLanguage(L"en-US");
    Check(pulse::ui::GitMarkLabel(pulse::ui::GitMark::Modified, true) == L"Modified" &&
              pulse::ui::GitMarkLabel(pulse::ui::GitMark::Modified, false) == L"M" &&
              pulse::ui::GitDirBadgeText(pulse::ui::GitMark::Modified, 3, true) == L"3 edits inside" &&
              pulse::ui::GitDirBadgeText(pulse::ui::GitMark::Modified, 3, false) == L"3 M",
          "mark: en-US plain vs tech labels and directory summaries");
    pulse::l10n::SetLanguage(L"zh-CN");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    ParserCases();
    QuoteCases();
    ExtendedUnitCases();
    if (argc > 1) RepoCases(argv[1]);
    std::printf("git_changes_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
