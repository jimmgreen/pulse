# Folder sizes in tile, icon and content views

Tiles, medium/large/extra-large icons and Content show the sum of logical file lengths beneath each filesystem directory.
This is not allocated disk space: sparse/compressed files and hard links follow
per-entry logical-size semantics. Small icons, List and Details retain their existing labels.

`FolderSizes` owns one low-priority worker. The UI supplies visible source rows;
the worker runs up to 4 resumable scans in 12 ms / 1,024-entry slices, with a 4 ms
pause after about 48 ms of work (it runs in background CPU/I/O mode), always giving the next slice to the scan that has done the least work, so
small folders finish within milliseconds while a large one continues. A long
automatic scan (over 250 ms of work) is parked, with its progress kept, whenever
other visible rows are waiting to start, and resumes once a slot is free.
Explicit calculations receive every other slice (fair across manual requests),
with up to 8,192 entries per slice under the same 12 ms deadline. These are scheduling budgets, not guarantees on individual filesystem
calls. Directory I/O and persistent-cache I/O never run on the window thread.
Directories are listed in 64 KiB batches through one directory handle
(`FileFullDirectoryInfo`, with a `FindFirstFileEx` fallback); the handle that is
opened is checked to be the directory itself, never a link target.

Automatic scans finish on their own: only a tree beyond 3,000,000 entries or 60 s of
walking waits for a click (the old limit of 20,000 entries / 500 ms left most
real folders at "—"). Rows wait up to 150 ms for the index service before a walk
starts, and not at all when the index is off or has just failed to answer.
A change inside a folder while it is walked no longer restarts the walk (busy
folders such as browser profiles never finished). The walk completes, its total
is shown as an unverified snapshot, and a refresh follows one second later that
reuses every watched subtree that did not change.

Removing visible requests cancels their generation and requests cancellation of
pending synchronous worker I/O. Scans cannot publish into a replacement request.

Local fixed disks scan automatically. UNC paths, mapped network drives and removable
media require clicking the size label. Network/driver latency can still delay a
worker operation; first-time enumeration of very large trees is not instantaneous.

The cache retains up to 4,096 directory totals in `folder_sizes.json` in Pulse's
data directory. On restart, cached totals are explicitly stale until revalidated.
Recursive local watches invalidate affected ancestors; updates are debounced by
200 ms. Watched complete results are reused, with a 60-second fallback freshness
window when no active watcher covers them. Dropping a watch invalidates its cache.
Cache writes are ignored by the cache's own watcher to prevent a feedback loop.

An empty folder is `0 B`. Unreadable roots never become a false zero. Unreadable,
offline or skipped link subtrees produce a partial result. Name-surrogate reparse
points (junctions, symbolic links, mount points) are never traversed; other reparse
directories, such as OneDrive / Cloud Files sync folders, are the folder itself and
are counted (they were previously skipped, undercounting synced folders). Automatic
scans do not enumerate offline or recall-on-open directories, whose listing is not
on this device. An explicit
click retries offline directory metadata enumeration, which may contact its storage
provider; file contents are never opened. Links/junctions and unreadable directories
still remain partial rather than claiming a false exact total. Administrator rights
are not requested automatically. There is no byte-size cap or artificial 128-level
cutoff. A failed refresh retains any old value with an explicit cached label.
Manual, cached, partial and unavailable labels can be clicked to retry.

While a long scan runs, visited bytes are published at most once per 200 ms per
job, explicitly marked as a lower bound plus Calculating. In-flight frames are
counted once; completed scans replace progress with their final complete/partial
state. Stale epochs cannot publish progress. An available index estimate remains
visible during an automatic scan, until its completed result replaces it.
Size-sort updates are deferred while scrolling or dragging the scrollbar.

The first choice for local indexed NTFS folders is the index service's MFT/USN
backend. It builds directory aggregates from existing metadata once (linear in
index entries), retains directory totals only, and propagates subsequent USN size
changes along the old/new ancestor chains. Moving a directory transfers its cached
subtree total without enumerating descendants. Snapshot replacement resets the
aggregates; ordinary volume visibility polling does not. No MFT parsing or volume
access is added to the UI process.

Indexed totals are explicitly labeled **Index estimate / 索引估算** because the
filename index can exclude entries and records one indexed name per MFT record
(hard-link aliases need not match directory-enumeration totals). Clicking an estimate
requests the existing exact directory scan, whose result is retained for that visible
scope. Both methods measure logical lengths, not allocated space.

Requests use versioned, bounded batches over a cancellable dedicated pipe, off the
window thread. Visible estimates refresh about once a second. Missing/older services,
uncovered paths, unfinished snapshots, offline volumes, uncaught-up journals and
journal gaps fall back to background enumeration. Index recovery restores the fast
path. Late replies cannot overwrite a replacement scope or a manual exact scan.
Pipe failures have a 10-second retry backoff; an unresponsive request times out in
approximately 750 ms and cancellation does not wait for that timeout.

Targeted scanner regression: build `pulse_folder_sizes_refactor_test`, then run
`pulse_folder_sizes_refactor_test.exe --manual-only` (offline manual retry, progressive
lower bounds, a 5 GiB sparse file's logical length, and a 140-level extended path).
`--watch-only` checks watched invalidation and verified subtree reuse separately.

Targeted validation: build Pulse with `PULSE_WITH_SELFTEST=ON`, set
`PULSE_SELFTEST_CASE=folder-sizes`, and run `pulse.exe --selftest`, waiting for exit.
The test uses isolated files, exercises real recursive watches and cache reloads,
checks view-model wiring and size-label hit tests, and saves wide/narrow screenshots
at 100%, 125%, 150% and 200% in both themes under `bench_data/folder-sizes`.
This case also exercises the real pipe client against an isolated controlled server:
index updates, unavailability/recovery, exact-scan override and stalled cancellation.
Run `pulse_index_engine_test.exe --folder-sizes-only` for MFT metadata aggregation,
production USN application with real fixture-file sizes, moves/deletion, coverage,
gap handling, overflow/cycle rejection and a 200,000-file / 10,000-update benchmark.