// git_extended_test: tests for git_branches, git_history, and git_conflict.
// Pure parsing / graph / conflict-composition tests run standalone; pass a
// fixture directory to also run end-to-end tests against real git repositories.
//
//   pulse_git_extended_test [fixture_dir]
#include "../app/git_branches.h"
#include "../app/git_conflict.h"
#include "../app/git_history.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
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

bool WriteBytes(const std::wstring& path, std::string_view bytes) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
    CloseHandle(f);
    return ok != FALSE;
}

std::string ReadBytes(const std::wstring& path) {
    std::string out;
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(f);
    return out;
}

void PureUnitCases() {
    // 1. Branches & Stash parsing
    const std::string br_out =
        "*\x1f" "refs/heads/main\x1fmain\x1f" "origin/main\x1f" "Alice\x1f" "1700000000\x1f" "init\n"
        " \x1f" "refs/heads/feature\x1f" "feature\x1f\x1f" "Bob\x1f" "1700000100\x1f" "wip feat\n"
        " \x1f" "refs/remotes/origin/HEAD\x1f" "origin/HEAD\x1f\x1f" "Alice\x1f" "1700000000\x1f" "init\n"
        " \x1f" "refs/remotes/origin/main\x1f" "origin/main\x1f\x1f" "Alice\x1f" "1700000000\x1f" "init\n"
        " \x1f" "refs/remotes/origin/remote-only\x1f" "origin/remote-only\x1f\x1f" "Lin\x1f" "1700000200\x1f" "remote work\n";
    std::vector<GitBranchInfo> local, remote;
    ParseBranchesOutput(br_out, local, remote);
    Check(local.size() == 2 && local[0].name == L"main" && local[0].is_current && local[0].upstream == L"origin/main",
          "branches: local parsed with current first");
    Check(remote.size() == 1 && remote[0].name == L"origin/remote-only" && remote[0].short_name == L"remote-only",
          "branches: remote filters HEAD and already-tracked branches");

    const std::string stash_out =
        "__PULSE_STASH__stash@{0}\x1f" "1700000500\x1fOn main: font test\n"
        "a.txt\n"
        "b/c.md\n"
        "__PULSE_STASH__stash@{1}\x1f" "1700000100\x1fOn feat: backup\n"
        "only.png\n";
    std::vector<GitStashInfo> stashes;
    ParseStashListOutput(stash_out, stashes);
    Check(stashes.size() == 2 && stashes[0].ref == L"stash@{0}" && stashes[0].file_count == 2 &&
              stashes[1].file_count == 1,
          "stash: list and file counts parsed");

    // 2. History log parsing & graph lane assignment
    const std::string log_out =
        "\x1e" "c444444444444444444444444444444444444444\x1f" "b222222222222222222222222222222222222222 c333333333333333333333333333333333333333\x1fHEAD -> refs/heads/main\x1f" "Alice\x1f" "a@x\x1f" "1700000400\x1fmerge feat\x1f\n"
        "\x1e" "c333333333333333333333333333333333333333\x1f" "a111111111111111111111111111111111111111\x1frefs/heads/feat\x1f" "Bob\x1f" "b@x\x1f" "1700000300\x1f" "feat commit\x1f\n"
        "\x1e" "b222222222222222222222222222222222222222\x1f" "a111111111111111111111111111111111111111\x1frefs/remotes/origin/main\x1f" "Alice\x1f" "a@x\x1f" "1700000200\x1fmain commit\x1f\n"
        "\x1e" "a111111111111111111111111111111111111111\x1f\x1ftag: refs/tags/v1.0\x1f" "Alice\x1f" "a@x\x1f" "1700000100\x1finit\x1f" "body line\n";
    std::vector<GitCommitItem> items;
    std::unordered_set<std::wstring> unpushed{L"c444444444444444444444444444444444444444"};
    std::unordered_set<std::wstring> remote_only;
    ParseGitLogOutput(log_out, unpushed, remote_only, items);
    ComputeCommitGraph(items);
    Check(items.size() == 4 && items[0].is_merge && items[0].is_unpushed && items[0].lane == 0,
          "history: merge and unpushed flags parsed");
    Check(items[1].lane != items[2].lane && items[0].edges.size() == 2 && items[0].edges[1].is_merge,
          "history: parallel branch gets separate lane and merge edge");
    Check(!items[3].refs.empty() && items[3].refs[0].kind == GitRefTag::Kind::Tag && items[3].refs[0].name == L"v1.0",
          "history: tag decoration parsed");

    // 3. Conflict marker parsing & lossless composition (2-way + 3-way)
    const std::string cf_text =
        "header\n"
        "<<<<<<< HEAD\n"
        "my line\n"
        "||||||| base\n"
        "base line\n"
        "=======\n"
        "their line\n"
        ">>>>>>> feature\n"
        "footer\n";
    std::vector<GitConflictHunk> hunks;
    std::vector<std::string> segs;
    Check(ParseConflictText(cf_text, hunks, segs) && hunks.size() == 1 && segs.size() == 2,
          "conflict: 3-way markers parsed");
    Check(hunks[0].ours_text == L"my line" && hunks[0].base_text == L"base line" && hunks[0].theirs_text == L"their line",
          "conflict: ours/base/theirs text extracted");
    hunks[0].choice = GitConflictChoice::Both;
    Check(ComposeResolvedText(segs, hunks) == "header\nmy line\ntheir line\nfooter\n",
          "conflict: KeepBoth preserves surrounding segments");
    hunks[0].choice = GitConflictChoice::Custom;
    hunks[0].custom_utf8 = "custom line\n";
    Check(ComposeResolvedText(segs, hunks) == "header\ncustom line\nfooter\n",
          "conflict: Custom choice composes cleanly");
}

void RepoCases(const std::wstring& base) {
    const std::wstring root = base + L"\\ext-repo-" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(root.c_str(), nullptr);
    auto git = [&](const std::wstring& args) { return RunGit(root, args, 30000, true); };

    Check(git(L"init -q -b main").exit_code == 0, "ext: git init");
    git(L"config user.name \"Pulse Tester\"");
    git(L"config user.email tester@pulse.invalid");
    git(L"config commit.gpgsign false");
    git(L"config core.autocrlf false");

    const std::wstring doc_rel = L"\u63D0\u6848\u8BF4\u660E.md";  // 提案说明.md
    const std::wstring img_rel = L"\u4E3B\u89C6\u89C9.png";       // 主视觉.png
    WriteBytes(GitJoinPath(root, doc_rel), "title\ncolor=old\nfooter\n");
    WriteBytes(GitJoinPath(root, img_rel), std::string("PNG\0BASE", 8));
    GitStagePaths(root, {doc_rel, img_rel});
    Check(GitCommit(root, L"initial version", false).ok, "ext: initial commit");
    const std::wstring init_hash = GitHeadId(root);

    // Create feature branch and make conflicting changes.
    Check(GitCreateBranch(root, L"feat").ok, "ext: create & switch to feat");
    WriteBytes(GitJoinPath(root, doc_rel), "title\ncolor=theirs\nfooter\n");
    WriteBytes(GitJoinPath(root, img_rel), std::string("PNG\0THEIRS_DATA", 15));
    GitStagePaths(root, {doc_rel, img_rel});
    Check(GitCommit(root, L"feat changes", false).ok, "ext: feat commit");

    // Dirty worktree on feat -> auto-stash switch back to main
    WriteBytes(GitJoinPath(root, L"scratch.txt"), "dirty\n");
    Check(GitSwitchBranch(root, L"main", true).ok, "ext: switch main with auto-stash");
    GitBranchOverview ov = GitReadBranchOverview(root);
    Check(ov.ok && ov.current_branch == L"main" && ov.stashes.size() == 1, "ext: overview sees main and 1 stash");
    if (!ov.stashes.empty()) {
        Check(GitStashDrop(root, ov.stashes[0].ref).ok, "ext: drop auto-stash");
    }

    // Commit different changes on main, then merge feat -> conflict!
    WriteBytes(GitJoinPath(root, doc_rel), "title\ncolor=ours\nfooter\n");
    WriteBytes(GitJoinPath(root, img_rel), std::string("PNG\0OURS_DATA", 13));
    GitStagePaths(root, {doc_rel, img_rel});
    Check(GitCommit(root, L"main changes", false).ok, "ext: main commit");

    const GitRemoteOpResult mr = GitMergeBranch(root, L"feat");
    Check(!mr.ok && mr.has_conflicts, "ext: merge reports conflicts");

    const GitConflictState cs = GitReadConflictState(root);
    Check(cs.ok && cs.merging && cs.conflict_files.size() == 2, "ext: conflict state lists 2 files");

    // Resolve text conflict
    std::string raw_doc = ReadBytes(GitJoinPath(root, doc_rel));
    std::vector<GitConflictHunk> hunks;
    std::vector<std::string> segs;
    Check(ParseConflictText(raw_doc, hunks, segs) && hunks.size() == 1, "ext: parse conflict markers in CJK file");
    std::string base_blob;
    Check(GitReadConflictStageBlob(root, 1, doc_rel, base_blob, 1 << 20) && base_blob.find("color=old") != std::string::npos,
          "ext: read stage 1 common ancestor blob");
    if (!hunks.empty()) {
        hunks[0].choice = GitConflictChoice::Ours;
        Check(GitWriteResolvedTextFile(root, doc_rel, ComposeResolvedText(segs, hunks)).ok,
              "ext: write resolved text and stage");
    }

    // Resolve binary conflict saving unchosen copy
    std::wstring copy_path;
    Check(GitResolveBinaryConflict(root, img_rel, true, true, L"\u5BF9\u65B9\u7248\u672C", &copy_path).ok,
          "ext: resolve binary conflict keeping ours and saving copy");
    Check(!copy_path.empty() && ReadBytes(copy_path) == std::string("PNG\0THEIRS_DATA", 15),
          "ext: saved unchosen binary copy matches theirs");
    Check(ReadBytes(GitJoinPath(root, img_rel)) == std::string("PNG\0OURS_DATA", 13),
          "ext: main binary file kept ours");

    Check(GitCompleteMerge(root).ok && !GitIsMerging(root), "ext: complete merge");

    // Trigger another conflict and abort it cleanly
    Check(GitCreateBranch(root, L"feat2", init_hash).ok, "ext: branch feat2 from init");
    WriteBytes(GitJoinPath(root, doc_rel), "title\ncolor=feat2\nfooter\n");
    GitStagePaths(root, {doc_rel});
    GitCommit(root, L"feat2 commit", false);
    GitSwitchBranch(root, L"main", false);
    Check(GitMergeBranch(root, L"feat2").has_conflicts, "ext: second conflict triggered");
    Check(GitAbortMerge(root).ok && !GitIsMerging(root), "ext: abort merge cleans MERGE_HEAD");

    // History, commit files, commit tree, single-file restore, soft reset
    std::vector<GitCommitItem> hist;
    int behind = 0;
    GitHistoryFilter filter;
    Check(GitReadHistory(root, filter, 50, hist, behind).ok && hist.size() >= 4, "ext: read history");

    std::vector<GitCommitFile> cfiles;
    Check(GitReadCommitFiles(root, init_hash, cfiles).ok && cfiles.size() == 2, "ext: read commit files for root commit");

    std::vector<GitTreeEntry> tree;
    Check(GitReadCommitTree(root, init_hash, tree).ok && tree.size() == 2, "ext: read commit tree");

    // Edit doc_rel, then restore from init_hash with auto-stash
    WriteBytes(GitJoinPath(root, doc_rel), "title\ncolor=unsaved\nfooter\n");
    Check(GitRestoreFileFromCommit(root, init_hash, doc_rel, true).ok, "ext: restore file with auto-stash");
    Check(ReadBytes(GitJoinPath(root, doc_rel)) == "title\ncolor=old\nfooter\n", "ext: file content restored to init");
    GitBranchOverview ov2 = GitReadBranchOverview(root);
    Check(ov2.stashes.size() == 1, "ext: restore auto-stashed unsaved edit");

    // Soft reset to init_hash
    Check(GitResetToCommit(root, init_hash, false).ok && GitHeadId(root) == init_hash,
          "ext: soft reset moves HEAD back to init");

    std::printf("fixture %ls\n", root.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    PureUnitCases();
    if (argc > 1) RepoCases(argv[1]);
    std::printf("git_extended_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
