"""Native plugin development loop: build, stage, launch an isolated Pulse, and reload on change.

One `pulse_ext.py dev PROJECT` call replaces the build -> native-manifest -> pack -> verify ->
install -> enable -> stop-host -> run sequence while iterating. The DLL is copied into a fresh
`.pulse-dev/stage-NNNN` folder for every launch, so the build output is never locked by the
running host and rebuilding does not need the host to stop first. The host test data folder is
reused between launches (plugin settings survive a reload) unless a fresh one is requested.
Packaging, signing and store installation remain separate release steps.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

import package_store
import project_creator

DEV_DIR = ".pulse-dev"
RECORD = "extension-host-instance.json"
HOST_LOG = "pulse-dev-host.log"
SKIPPED_DIRS = {".pulse-dev", ".git", ".vs", ".vscode", "out", "__pycache__"}
MAX_WATCHED_FILES = 20000
STARTUP_GRACE_SECONDS = 2.0


def _skipped(name):
    return name in SKIPPED_DIRS or name == "build" or name.startswith("build-")


def source_fingerprint(project):
    """Size/mtime of every project file outside build output and the dev folder."""
    entries = []
    for root, dirs, files in os.walk(project):
        dirs[:] = sorted(d for d in dirs if not _skipped(d))
        for name in sorted(files):
            path = Path(root) / name
            try:
                info = path.stat()
            except OSError:
                continue
            entries.append((str(path.relative_to(project)), info.st_size, info.st_mtime_ns))
            if len(entries) >= MAX_WATCHED_FILES:
                return tuple(entries)
    return tuple(entries)


def changed_files(before, after):
    old = {entry[0]: entry[1:] for entry in before}
    new = {entry[0]: entry[1:] for entry in after}
    return sorted(name for name in old.keys() | new.keys() if old.get(name) != new.get(name))


def _pick_library(build, package_id):
    libraries = [Path(item) for item in build["libraries"]]
    if len(libraries) == 1:
        return libraries[0]
    stem = project_creator.library_stem_for_id(package_id).lower()
    matches = [item for item in libraries if item.stem.lower() == stem] or \
              [item for item in libraries if stem in item.stem.lower()]
    if len(matches) == 1:
        return matches[0]
    return None


def _next_stage(dev_root):
    numbers = []
    for item in dev_root.glob("stage-*"):
        suffix = item.name[len("stage-"):]
        if item.is_dir() and suffix.isdigit():
            numbers.append(int(suffix))
    return dev_root / f"stage-{(max(numbers) + 1 if numbers else 1):04d}"


def _prune_stages(dev_root, keep):
    """Best effort: earlier stages whose DLL is no longer loaded are removed."""
    removed = []
    for item in sorted(dev_root.glob("stage-*")):
        if item.is_dir() and item.resolve() not in keep:
            try:
                shutil.rmtree(item)
                removed.append(item.name)
            except OSError:
                pass  # still loaded by a host that has not exited yet; the next reload retries
    return removed


def _child_processes(parent_pid):
    """(pid, exe name) of direct children, via a Toolhelp snapshot."""
    if os.name != "nt":
        return []
    import ctypes
    from ctypes import wintypes

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", wintypes.LONG), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", wintypes.WCHAR * 260)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if not snapshot or snapshot == wintypes.HANDLE(-1).value:
        return []
    children = []
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(ProcessEntry)
        more = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            if entry.th32ParentProcessID == parent_pid:
                children.append((int(entry.th32ProcessID), entry.szExeFile))
            more = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return children


def _resolve_app_process(cli, launcher_pid, host_name):
    """An installed Pulse starts the app from AppRuntime and waits on it; close that child, not the launcher."""
    launcher_start = cli._process_start_filetime(launcher_pid)
    for pid, name in _child_processes(launcher_pid):
        if name.lower() != host_name.lower():
            continue
        child_start = cli._process_start_filetime(pid)
        if launcher_start is None or child_start is None or child_start >= launcher_start:
            return pid
    return None


def _read_record(data_dir):
    path = data_dir / RECORD
    if not path.is_file():
        return None
    try:
        record = package_store.strict_json(package_store.read_bytes(path, 16384))
    except (package_store.StoreError, OSError, ValueError):
        return None
    return record if isinstance(record, dict) and type(record.get("processId")) is int else None


def _write_record(cli, pulse, data_dir, pid, manifest):
    payload = {
        "formatVersion": 1,
        "processId": pid,
        "hostPath": str(Path(os.path.abspath(pulse))),
        "manifest": str(manifest),
        "profile": str(data_dir),
        "processStartFileTime": cli._process_start_filetime(pid),
    }
    path = data_dir / RECORD
    temporary = data_dir / (RECORD + ".tmp")
    temporary.write_bytes((json.dumps(payload, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    os.replace(temporary, path)
    return path


def _log_tail(path, limit=4000):
    try:
        data = path.read_bytes()
    except OSError:
        return ""
    return data[-limit:].decode("utf-8", errors="replace")


class Session:
    def __init__(self, cli, pulse, project, config, data_dir, open_panel, files, sdk_root, validation_host):
        self.cli = cli
        self.pulse = Path(os.path.abspath(pulse))
        self.project = project
        self.config = config
        self.data_dir = data_dir
        self.open_panel = open_panel
        self.files = [Path(os.path.abspath(item)) for item in files]
        self.sdk_root = sdk_root
        self.validation_host = validation_host
        self.dev_root = project / DEV_DIR
        self.manifest_source = project / "manifest.json"
        self.process = None
        self.stage = None
        self.launches = 0

    def build_and_stage(self):
        source = package_store.strict_json(package_store.read_bytes(self.manifest_source, 65536))
        package_id = source.get("id") if isinstance(source, dict) else None
        if not isinstance(package_id, str):
            raise self.cli.Failure("invalid_manifest", "manifest.json must declare an id.", 2)
        build = self.cli.build_project(self.project, config=self.config, sdk_root=self.sdk_root)
        library = _pick_library(build, package_id)
        if library is None:
            raise self.cli.Failure("library_not_found", "Cannot tell which built DLL is the plugin entry.", 2,
                                   libraries=build["libraries"])
        stage = _next_stage(self.dev_root)
        stage.mkdir(parents=True)
        staged_library = stage / library.name
        shutil.copy2(library, staged_library)
        symbols = library.with_suffix(".pdb")
        if symbols.is_file():
            shutil.copy2(symbols, stage / symbols.name)
        staged_files = []
        for item in self.files:
            if not item.is_file():
                raise self.cli.Failure("file_not_found", f"Sidecar file not found: {item}", 2)
            shutil.copy2(item, stage / item.name)
            staged_files.append(stage / item.name)
        manifest = self.cli.author_native_manifest(self.validation_host, self.manifest_source, staged_library,
                                                   stage / "manifest.json", staged_files)
        return {"build": build, "stage": stage, "library": staged_library, "manifest": manifest}

    def running(self):
        return self.process is not None and self.process.poll() is None

    def stop_recorded(self):
        """Cooperatively close the host recorded in the data folder (this or an earlier dev run)."""
        record = _read_record(self.data_dir)
        if record is None:
            return None
        pid = record["processId"]
        # A record that names an installed launcher: close the app it is waiting on instead.
        child = _resolve_app_process(self.cli, pid, self.pulse.name)
        if child:
            pid = child
            _write_record(self.cli, Path(record.get("hostPath") or self.pulse), self.data_dir, pid,
                          record.get("manifest", ""))
        try:
            result = self.cli.stop_host(self.pulse, pid, self.data_dir, timeout=15.0)
        except self.cli.Failure as exc:
            if exc.error["code"] in ("process_not_found", "pid_reused"):
                result = {"stopped": True, "alreadyExited": True, "processId": pid}
            else:
                raise
        if self.process is not None:
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        return result

    def launch(self, staged):
        manifest_path = Path(staged["manifest"]["path"])
        command = [str(self.pulse), "--test-instance", "--extension-manifest", str(manifest_path)]
        if self.open_panel:
            command += ["--extension-native", str(staged["library"])]
        environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PULSE_")}
        environment["PULSE_TEST_DATA_DIR"] = str(self.data_dir)
        log_path = self.data_dir / HOST_LOG
        self.launches += 1
        with log_path.open("ab") as log:
            log.write(f"\n=== pulse dev launch {self.launches}: {staged['stage'].name} ===\n".encode("utf-8"))
            log.flush()
            try:
                self.process = subprocess.Popen(command, shell=False, env=environment, stdin=subprocess.DEVNULL,
                                                stdout=log, stderr=subprocess.STDOUT)
            except OSError as exc:
                raise self.cli.Failure("dev_launch_failed", str(exc), 3) from exc
        app_pid = None
        deadline = time.monotonic() + STARTUP_GRACE_SECONDS
        while time.monotonic() < deadline and self.process.poll() is None:
            app_pid = app_pid or _resolve_app_process(self.cli, self.process.pid, self.pulse.name)
            time.sleep(0.1)
        if self.process.poll() is not None:
            code = self.process.returncode
            raise self.cli.Failure("host_exited", f"Pulse exited during startup with code {code}; see the host log.",
                                   3, exitCode=code, hostLog=str(log_path), logTail=_log_tail(log_path))
        # stop-host closes the windows of the recorded process: the app itself, not an installed launcher.
        host_pid = app_pid or self.process.pid
        record = _write_record(self.cli, self.pulse, self.data_dir, host_pid, manifest_path)
        self.stage = staged["stage"]
        removed = _prune_stages(self.dev_root, {self.stage.resolve()})
        return {"processId": host_pid, "launcherProcessId": self.process.pid if app_pid else None,
                "command": command, "hostLog": str(log_path), "instanceRecord": str(record),
                "removedStages": removed}

    def reload(self):
        staged = self.build_and_stage()
        stopped = self.stop_recorded()
        launched = self.launch(staged)
        return {
            "project": str(self.project),
            "config": self.config,
            "library": str(staged["library"]),
            "stage": str(staged["stage"]),
            "manifest": staged["manifest"]["path"],
            "binarySha256": staged["manifest"]["binarySha256"],
            "dataDir": str(self.data_dir),
            "previousHost": stopped,
            "panelOpened": self.open_panel,
            "loadVerified": False,
            **launched,
        }


def resolve_data_dir(project, data_dir, fresh):
    dev_root = project / DEV_DIR
    if data_dir is not None:
        target = Path(os.path.abspath(data_dir))
    elif fresh:
        target = dev_root / time.strftime("data-%Y%m%d-%H%M%S")
    else:
        target = dev_root / "data"
    for ancestor in (target, *target.parents):
        if ancestor.exists():
            package_store.ordinary(ancestor)
    if fresh and target.exists() and any(target.iterdir()):
        raise package_store.StoreError("destination_exists", "A fresh data folder must be new or empty.")
    target.mkdir(parents=True, exist_ok=True)
    return target


def develop(cli, pulse, project, config="Debug", data_dir=None, fresh=False, watch=False, open_panel=True,
            files=(), sdk_root=None, interval=0.5, emit=None, max_reloads=None, validation_host=None):
    project = Path(os.path.abspath(project))
    if not (project / "CMakeLists.txt").is_file() or not (project / "manifest.json").is_file():
        raise cli.Failure("invalid_project", "Project directory must contain CMakeLists.txt and manifest.json.", 2,
                          path=str(project))
    pulse = Path(os.path.abspath(pulse))
    if not pulse.is_file():
        raise cli.Failure("host_not_found", "Pass --pulse with the absolute path of the Pulse host to test against.",
                          2, pulse=str(pulse))
    api = cli.host_call(pulse, "--extension-api")
    if "nativePackages" not in api.get("developerFeatures", []):
        raise cli.Failure("dev_unsupported", "This host does not load native packages.", 3)
    if interval <= 0 or interval > 10:
        raise cli.Failure("usage", "--interval must be between 0 and 10 seconds.", 2)
    target = resolve_data_dir(project, data_dir, fresh)
    session = Session(cli, pulse, project, config, target, open_panel, files, sdk_root, validation_host)
    emit = emit or (lambda event, **fields: None)
    first = session.reload()
    if not watch:
        first["watching"] = False
        first["reloadCommand"] = "Run the same dev command again to rebuild and restart this isolated instance."
        return first
    emit("launched", **first)
    reloads, failures = 0, 0
    fingerprint = source_fingerprint(project)
    host_exit_reported = False
    try:
        while max_reloads is None or reloads < max_reloads:
            time.sleep(interval)
            if not session.running() and not host_exit_reported:
                host_exit_reported = True
                emit("host_exited", exitCode=session.process.returncode if session.process else None,
                     message="Pulse was closed; the next source change rebuilds and starts it again.")
            current = source_fingerprint(project)
            if current == fingerprint:
                continue
            # Debounce: editors write several files in quick succession.
            while True:
                time.sleep(0.3)
                settled = source_fingerprint(project)
                if settled == current:
                    break
                current = settled
            emit("changed", files=changed_files(fingerprint, current)[:20])
            fingerprint = current
            try:
                result = session.reload()
            except cli.Failure as exc:
                failures += 1
                emit("reload_failed", error=exc.error,
                     message="The running instance keeps the previous build; fix the error and save again.")
                continue
            reloads += 1
            host_exit_reported = False
            emit("reloaded", **result)
    except KeyboardInterrupt:
        pass
    stopped = session.stop_recorded() if session.running() else None
    return {"project": str(project), "dataDir": str(target), "watching": True, "reloads": reloads,
            "failedReloads": failures, "stopped": stopped}
