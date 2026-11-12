"""Stdio MCP author server: create, build, package, stage, run, and stop public C++ plugins."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import msvc_env
import offline_manifest
import project_creator
import pulse_ext

ROOT = Path(__file__).resolve().parents[2]
MAX_MESSAGE = 1024 * 1024
MAX_LOG = 32768
BUILD_CONFIGS = ('Debug', 'RelWithDebInfo', 'Release')


class AuthorError(Exception):
    pass


def schema(properties=None, required=()):
    return dict(type='object', properties=properties or {}, required=list(required), additionalProperties=False)


STRING = dict(type='string', minLength=1, maxLength=32768)
TOOLS = [
    dict(
        name='native_sdk_describe',
        description='Read SDK interface summaries, packable templates, ABI samples, and build requirements. Supply interface to retrieve one complete header contract.',
        inputSchema=schema(dict(interface=STRING)),
    ),
    dict(
        name='native_project_create',
        description='Create a new packable C++ plugin project in the author workspace from a public SDK template with a unique package identity. Existing files are never replaced.',
        inputSchema=schema(
            dict(
                name=dict(type='string', pattern='^[a-z][a-z0-9-]{0,63}$'),
                template=STRING,
                id=dict(type='string', minLength=1, maxLength=64),
                displayName=dict(type='string', minLength=1, maxLength=128),
            ),
            ('name', 'template'),
        ),
    ),
    dict(
        name='native_project_build',
        description='Configure and build a workspace C++ project with CMake/Ninja (Debug, RelWithDebInfo, or Release) and public SDK headers. Returns DLLs, PDB symbols, and debugger attach guidance; no Pulse private libraries.',
        inputSchema=schema(
            dict(
                project=STRING,
                config=dict(type='string', enum=list(BUILD_CONFIGS)),
            ),
            ('project',),
        ),
    ),
    dict(
        name='native_package_stage',
        description='Bind the compiled DLL hash, pack and verify a .pulse-ext package using the same Pulse author CLI.',
        inputSchema=schema(dict(project=STRING, library=STRING, files=dict(type='array', items=STRING, maxItems=63)), ('project', 'library')),
    ),
    dict(
        name='native_package_install',
        description='Install a verified native package in this author workspace test store. Native consent must be explicit. Enable applies on next host startup.',
        inputSchema=schema(dict(package=STRING, allowNative=dict(type='boolean'), enable=dict(type='boolean')), ('package', 'allowNative')),
    ),
    dict(
        name='native_host_diagnose',
        description='Compare this Pulse host native SDK catalog, sdkPreview, interface versions, and x64 build toolchain with the author SDK without loading a plugin.',
        inputSchema=schema(),
    ),
    dict(
        name='native_host_run',
        description='Launch the configured host with an isolated author profile and test store. Returns the process ID, profile path, and log path; launch acceptance does not certify plugin loading or UI behavior.',
        inputSchema=schema(dict(allowNative=dict(type='boolean')), ('allowNative',)),
    ),
    dict(
        name='native_host_stop',
        description='Cooperatively close an isolated test host previously launched in this author workspace so locked plugin DLLs can be rebuilt or reinstalled safely.',
        inputSchema=schema(dict(pid=dict(type='integer', minimum=1), profile=STRING), ('pid',)),
    ),
    dict(
        name='native_plugin_test',
        description='Run offline manifest/PE64 ABI static checks and headless lifecycle + command smoke tests (via pulse_plugin_harness.exe when available) without launching the GUI host.',
        inputSchema=schema(
            dict(
                target=STRING,
                library=STRING,
                invokeCommands=dict(type='array', items=STRING, maxItems=32),
                smokeCommands=dict(type='boolean'),
                selection=dict(type='array', items=STRING, maxItems=63),
                directory=STRING,
                omitExperimentalUi=dict(type='boolean'),
                strictResources=dict(type='boolean'),
                timeoutMs=dict(type='integer', minimum=100, maximum=60000),
            ),
            ('target',),
        ),
    ),
    dict(
        name='native_host_logs',
        description='Read structured startup status, persistent extension-audit records, and crash attribution logs from an isolated workspace profile and test store, with external paths and host environment redacted.',
        inputSchema=schema(
            dict(
                profile=STRING,
                packageId=dict(type='string', minLength=1, maxLength=64),
                maxEvents=dict(type='integer', minimum=1, maximum=500),
            ),
        ),
    ),
]


def validate(arguments, spec):
    if not isinstance(arguments, dict):
        raise AuthorError('arguments must be an object')
    fields = spec['inputSchema']['properties']
    if set(arguments) - set(fields) or set(spec['inputSchema']['required']) - set(arguments):
        raise AuthorError('unexpected or missing arguments')
    for key, value in arguments.items():
        field_spec = fields[key]
        expected = field_spec['type']
        max_items = field_spec.get('maxItems', 63)
        if expected == 'array' and (not isinstance(value, list) or len(value) > max_items or
                                   any(not isinstance(item, str) or not item or len(item) > 32768 or '\0' in item for item in value)):
            raise AuthorError(f'{key} must be an array of at most {max_items} nonempty path strings')
        if expected == 'boolean' and type(value) is not bool:
            raise AuthorError(key + ' must be boolean')
        if expected == 'integer' and (
            type(value) is not int
            or isinstance(value, bool)
            or value < field_spec.get('minimum', 1)
            or ('maximum' in field_spec and value > field_spec['maximum'])
        ):
            raise AuthorError(key + ' must be a positive integer within allowed bounds')
        if expected == 'string':
            max_len = field_spec.get('maxLength', 32768)
            if not isinstance(value, str) or not value or len(value) > max_len or '\0' in value:
                raise AuthorError(key + ' must be a nonempty path/identifier string')
            if 'enum' in field_spec and value not in field_spec['enum']:
                raise AuthorError(key + ' must be one of: ' + ', '.join(field_spec['enum']))


def _reference_samples(sdk_root):
    samples_dir = Path(sdk_root) / 'sdk/native/samples'
    if not samples_dir.is_dir() or samples_dir.is_symlink():
        return []
    items = []
    for entry in sorted(samples_dir.iterdir(), key=lambda p: p.name):
        if entry.is_symlink() or not entry.is_dir():
            continue
        if (entry / 'CMakeLists.txt').is_file() and (entry / 'plugin.cpp').is_file():
            items.append(dict(
                name=entry.name,
                path=entry.relative_to(sdk_root).as_posix(),
                hasManifest=(entry / 'manifest.json').is_file(),
                hasReadme=(entry / 'README.md').is_file(),
            ))
    return items


_ENV_LINE_RE = re.compile(
    r'(?im)^(?:SET\s+)?(?:PATH|PATHEXT|TEMP|TMP|USERPROFILE|APPDATA|LOCALAPPDATA|'
    r'USERNAME|USERDOMAIN|HOMEDRIVE|HOMEPATH|SYSTEMROOT|WINDIR|COMSPEC|'
    r'PROGRAMFILES(?:\(X86\))?|PROGRAMDATA|VSCMD_[A-Z0-9_]+|INCLUDE|LIB|LIBPATH)=.*$'
)
_EXTERNAL_PATH_RE = re.compile(
    r'(?:\\\\\?\\)?[A-Za-z]:[\\/][^\s"\'<>|*?\r\n,;)]+|\\\\[A-Za-z0-9._$-]+[\\/][^\s"\'<>|*?\r\n,;)]+'
)


def _redact_text(text, workspace):
    if not isinstance(text, str) or not text:
        return text
    ws_resolved = Path(workspace).resolve()
    ws_backslash = str(ws_resolved)
    ws_slash = ws_resolved.as_posix()
    redacted = _ENV_LINE_RE.sub('<redacted-env>', text)

    def _replace_path(match):
        raw = match.group(0)
        cleaned = raw[4:] if raw.startswith('\\\\?\\') else raw
        norm_clean = cleaned.replace('\\', '/').rstrip('/')
        norm_ws = ws_slash.rstrip('/')
        if norm_clean.lower() == norm_ws.lower():
            return '<workspace>'
        if norm_clean.lower().startswith(norm_ws.lower() + '/'):
            suffix = norm_clean[len(norm_ws) + 1:]
            return '<workspace>/' + suffix
        return '<redacted-path>'

    for prefix in (ws_backslash, ws_slash):
        if prefix:
            pattern = re.compile(re.escape(prefix), re.IGNORECASE)
            redacted = pattern.sub('<workspace>', redacted)
    redacted = redacted.replace('<workspace>\\', '<workspace>/')
    redacted = _EXTERNAL_PATH_RE.sub(_replace_path, redacted)
    # Normalize any nested backslashes following <workspace>/
    redacted = re.sub(
        r'<workspace>/([^\s"\'<>|*?\r\n,;)]+)',
        lambda m: '<workspace>/' + m.group(1).replace('\\', '/'),
        redacted,
    )
    return redacted


def _redact_data(value, workspace):
    if isinstance(value, str):
        return _redact_text(value, workspace)
    if isinstance(value, list):
        return [_redact_data(item, workspace) for item in value]
    if isinstance(value, dict):
        out = {}
        for k, v in value.items():
            key_str = str(k)
            if key_str.lower() in ('env', 'environ', 'environment', 'hostenvironment'):
                out[key_str] = '<redacted-env>'
            else:
                out[key_str] = _redact_data(v, workspace)
        return out
    return value


def _read_bounded_text(path, max_bytes=262144, tail=False):
    if path.is_symlink() or not path.is_file():
        return None
    try:
        with path.open('rb') as handle:
            if tail:
                handle.seek(0, 2)
                size = handle.tell()
                handle.seek(max(0, size - max_bytes))
                return handle.read(max_bytes).decode('utf-8', errors='replace')
            data = handle.read(max_bytes + 1)
            if len(data) > max_bytes:
                return None
            return data.decode('utf-8', errors='replace')
    except OSError:
        return None


def _matches_package(item, package_id):
    if not package_id:
        return True
    if isinstance(item, dict):
        for field in ('packageId', 'id', 'extensionId', 'package'):
            val = item.get(field)
            if val == package_id:
                return True
            if isinstance(val, dict) and val.get('id') == package_id:
                return True
        return False
    if isinstance(item, str):
        stripped = item.strip()
        return (
            stripped == package_id
            or stripped.startswith(package_id + '\t')
            or stripped.startswith(package_id + ' ')
            or stripped.startswith(package_id + ':')
            or f'"{package_id}"' in stripped
        )
    return False


def _compare_catalogs(expected, actual):
    sdk_preview = expected.get('sdkPreview') if isinstance(expected, dict) else None
    host_preview = actual.get('sdkPreview') if isinstance(actual, dict) else None
    sdk_interfaces = {
        item['name']: item
        for item in (expected or {}).get('interfaces', [])
        if isinstance(item, dict) and isinstance(item.get('name'), str)
    }
    host_interfaces = {
        item['name']: item
        for item in (actual or {}).get('interfaces', [])
        if isinstance(item, dict) and isinstance(item.get('name'), str)
    }
    missing_in_host = sorted(set(sdk_interfaces) - set(host_interfaces))
    extra_in_host = sorted(set(host_interfaces) - set(sdk_interfaces))
    version_mismatches = []
    header_hash_mismatches = []
    for iface_name in sorted(set(sdk_interfaces) & set(host_interfaces)):
        sdk_item = sdk_interfaces[iface_name]
        host_item = host_interfaces[iface_name]
        if sdk_item.get('version') != host_item.get('version'):
            version_mismatches.append(dict(
                name=iface_name,
                sdkVersion=sdk_item.get('version'),
                hostVersion=host_item.get('version'),
            ))
        sdk_sha = (sdk_item.get('headerContract') or {}).get('sha256') if isinstance(sdk_item.get('headerContract'), dict) else None
        host_sha = (host_item.get('headerContract') or {}).get('sha256') if isinstance(host_item.get('headerContract'), dict) else None
        if sdk_sha and host_sha and sdk_sha != host_sha:
            header_hash_mismatches.append(dict(
                name=iface_name,
                sdkSha256=sdk_sha,
                hostSha256=host_sha,
            ))
    preview_matches = sdk_preview is not None and sdk_preview == host_preview
    abi_compatible = (
        isinstance(actual, dict)
        and preview_matches
        and not missing_in_host
        and not version_mismatches
        and not header_hash_mismatches
    )
    return (
        dict(sdk=sdk_preview, host=host_preview, matches=preview_matches),
        dict(
            missingInHost=missing_in_host,
            extraInHost=extra_in_host,
            versionMismatches=version_mismatches,
            headerHashMismatches=header_hash_mismatches,
            compatible=abi_compatible,
        ),
    )


class Author:
    def __init__(self, workspace, sdk, pulse=None):
        self.workspace = Path(workspace).resolve()
        self.sdk = Path(sdk).resolve()
        self.pulse = Path(pulse).resolve() if pulse is not None else None
        self.workspace.mkdir(parents=True, exist_ok=True)
        self.catalog = self.sdk / 'sdk/native/api-catalog.json'
        if not self.catalog.is_file() or (self.pulse is not None and not self.pulse.is_file()):
            raise AuthorError('SDK catalog and Pulse host must exist')

    def inside(self, value, exists=True):
        raw = self.workspace / value
        path = raw.resolve()
        try:
            path.relative_to(self.workspace)
        except ValueError as error:
            raise AuthorError('path must remain inside the configured author workspace') from error
        if path == self.workspace:
            raise AuthorError('path must remain inside the configured author workspace')
        for candidate in (raw, *raw.parents):
            if candidate == self.workspace:
                break
            if candidate.is_symlink():
                raise AuthorError('workspace symlinks are not allowed')
        if exists and not path.exists():
            raise AuthorError('workspace path does not exist: ' + str(path))
        return path

    def cli(self, *arguments):
        if self.pulse is not None:
            return [sys.executable, str(self.sdk / 'tools/extensions/pulse_ext.py'), '--pulse', str(self.pulse), '--json', *map(str, arguments)]
        return [sys.executable, str(self.sdk / 'tools/extensions/pulse_ext.py'), '--offline', '--json', *map(str, arguments)]

    def run(self, command, cancelled, commands, timeout=300, json_reply=False):
        if cancelled.is_set():
            raise AuthorError('cancelled')
        popen_cmd = list(command)
        popen_env = None
        if popen_cmd and popen_cmd[0] in ('cmake', 'ninja', 'cl'):
            resolved = msvc_env.resolve_build_environment(auto_discover=True)
            popen_env = resolved['env']
            exe = shutil.which(popen_cmd[0], path=popen_env.get('PATH'))
            if exe:
                popen_cmd[0] = exe
            if (
                command[0] == 'cmake'
                and '-G' in command
                and 'Ninja' in command
                and resolved.get('ninja_executable')
                and not any(str(arg).startswith('-DCMAKE_MAKE_PROGRAM=') for arg in command)
            ):
                popen_cmd.append('-DCMAKE_MAKE_PROGRAM=' + resolved['ninja_executable'])
        started = time.monotonic()
        with tempfile.TemporaryFile() as log:
            process = subprocess.Popen(popen_cmd, cwd=self.workspace, env=popen_env, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT, shell=False)
            reason = None
            while process.poll() is None:
                if cancelled.wait(0.05) or time.monotonic() - started > timeout:
                    reason = 'cancelled' if cancelled.is_set() else 'timeout'
                    if sys.platform == 'win32':
                        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True, timeout=15)
                    else:
                        process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=10)
                    break
            log.seek(0, 2)
            length = log.tell()
            log.seek(0 if json_reply and length <= 2 * 1024 * 1024 else max(0, length - MAX_LOG))
            output = log.read().decode('utf-8', errors='replace')
        entry = dict(command=command, exit=process.returncode, output=output[-MAX_LOG:], outputTruncated=length > MAX_LOG,
                     elapsedMs=round((time.monotonic() - started) * 1000))
        commands.append(entry)
        if reason or process.returncode or (json_reply and length > 2 * 1024 * 1024):
            raise AuthorError(reason or 'command failed; inspect compiler/CLI diagnostics')
        return output

    def _resolve_harness(self):
        harness = pulse_ext._resolve_harness_binary(pulse=self.pulse)
        if harness is not None:
            return harness
        for extra in (self.sdk / 'bin/pulse_plugin_harness.exe', self.sdk / 'build/pulse_plugin_harness.exe'):
            if extra.is_file():
                return extra
        return None

    def _resolve_logs_profile(self, explicit_profile=None):
        profiles_root = self.inside('profiles', exists=False)
        if explicit_profile is not None:
            profile_dir = self.inside(explicit_profile)
            try:
                profile_dir.relative_to(profiles_root)
            except ValueError as error:
                raise AuthorError('profile must reside under workspace profiles/') from error
            if profile_dir.is_symlink() or not profile_dir.is_dir():
                raise AuthorError('profile must be a workspace profile directory')
            return profile_dir
        if not profiles_root.is_dir() or profiles_root.is_symlink():
            return None
        candidates = []
        for entry in profiles_root.iterdir():
            if entry.is_symlink() or not entry.is_dir():
                continue
            try:
                mtime = entry.stat().st_mtime_ns
            except OSError:
                continue
            candidates.append((mtime, entry.name, entry))
        if not candidates:
            return None
        candidates.sort(key=lambda item: (item[0], item[1]))
        return candidates[-1][2]

    def _collect_host_logs(self, explicit_profile=None, package_id=None, max_events=100):
        profile_dir = self._resolve_logs_profile(explicit_profile)
        store_root = self.inside('.pulse-author/extensions', exists=False)
        store_parent = store_root.parent

        # 1. Startup status JSON
        startup_status = None
        status_candidates = []
        if profile_dir is not None:
            status_candidates.extend([
                profile_dir / 'extension-startup-status.json',
                profile_dir / 'extension-dev-status.json',
            ])
        status_candidates.append(store_parent / 'extension-startup-status.json')
        for status_path in status_candidates:
            raw_text = _read_bounded_text(status_path, 262144)
            if raw_text is None:
                continue
            try:
                parsed = json.loads(raw_text)
            except ValueError:
                continue
            if isinstance(parsed, dict) and package_id:
                filtered = {}
                for k, v in parsed.items():
                    if isinstance(v, list) and v and all(isinstance(elem, (dict, str)) for elem in v):
                        filtered[k] = [elem for elem in v if _matches_package(elem, package_id)]
                    else:
                        filtered[k] = v
                parsed = filtered
            startup_status = _redact_data(parsed, self.workspace)
            break

        # 2. Persistent audit records under extension-audit/
        audit_status = None
        audit_events = []
        audit_dirs = []
        if profile_dir is not None:
            audit_dirs.append(profile_dir / 'extension-audit')
        audit_dirs.append(store_parent / 'extension-audit')
        for audit_dir in audit_dirs:
            if not audit_dir.is_dir() or audit_dir.is_symlink():
                continue
            status_file = audit_dir / 'audit-status.json'
            status_raw = _read_bounded_text(status_file, 65536)
            if status_raw is not None and audit_status is None:
                try:
                    audit_status = _redact_data(json.loads(status_raw), self.workspace)
                except ValueError:
                    pass
            for seg in sorted(audit_dir.iterdir(), key=lambda p: p.name):
                if seg.name == 'audit-status.json' or seg.is_symlink() or not seg.is_file():
                    continue
                if seg.suffix.lower() not in ('.jsonl', '.log', '.json'):
                    continue
                seg_text = _read_bounded_text(seg, 1048576, tail=True)
                if not seg_text:
                    continue
                stripped = seg_text.strip()
                if stripped.startswith('['):
                    try:
                        arr = json.loads(stripped)
                        if isinstance(arr, list):
                            for obj in arr:
                                if isinstance(obj, dict) and _matches_package(obj, package_id):
                                    audit_events.append(_redact_data(obj, self.workspace))
                            continue
                    except ValueError:
                        pass
                for line in seg_text.splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                    except ValueError:
                        continue
                    if isinstance(obj, dict) and _matches_package(obj, package_id):
                        audit_events.append(_redact_data(obj, self.workspace))
        if len(audit_events) > max_events:
            audit_events = audit_events[-max_events:]

        # 3. Crash guard markers & crash attribution logs
        crash_markers = {'activating': [], 'crashed': [], 'crashDisabled': []}
        marker_roots = [store_root]
        if profile_dir is not None:
            marker_roots.append(profile_dir / 'extensions')
        marker_map = (
            ('native-activating.txt', 'activating'),
            ('native-crashed.txt', 'crashed'),
            ('crash-disabled.txt', 'crashDisabled'),
        )
        for mroot in marker_roots:
            if not mroot.is_dir() or mroot.is_symlink():
                continue
            for filename, bucket in marker_map:
                mtext = _read_bounded_text(mroot / filename, 65536)
                if not mtext:
                    continue
                for line in mtext.splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    parsed_entry = line
                    if line.startswith('{'):
                        try:
                            parsed_entry = json.loads(line)
                        except ValueError:
                            parsed_entry = line
                    elif '\t' in line:
                        parts = line.split('\t')
                        parsed_entry = {'packageId': parts[0], 'details': parts[1:]}
                    if _matches_package(parsed_entry, package_id):
                        redacted_entry = _redact_data(parsed_entry, self.workspace)
                        if redacted_entry not in crash_markers[bucket]:
                            crash_markers[bucket].append(redacted_entry)

        crash_reports = []
        if profile_dir is not None:
            for crash_file in (profile_dir / 'pulse_crash.log',):
                ctext = _read_bounded_text(crash_file, 65536, tail=True)
                if ctext:
                    lines = [ln for ln in ctext.splitlines() if ln.strip() and _matches_package(ln, package_id)]
                    if lines:
                        crash_reports.append({
                            'source': _redact_text(str(crash_file), self.workspace),
                            'lines': [_redact_text(ln, self.workspace) for ln in lines[-50:]],
                        })
            crashes_dir = profile_dir / 'Diagnostics/Crashes'
            if crashes_dir.is_dir() and not crashes_dir.is_symlink():
                for item in sorted(crashes_dir.iterdir(), key=lambda p: p.name):
                    if item.is_symlink() or not item.is_file():
                        continue
                    if item.suffix.lower() in ('.json', '.log', '.txt'):
                        ctext = _read_bounded_text(item, 65536, tail=True)
                        if not ctext:
                            continue
                        if item.suffix.lower() == '.json':
                            try:
                                cobj = json.loads(ctext)
                                if _matches_package(cobj, package_id):
                                    crash_reports.append({
                                        'source': _redact_text(str(item), self.workspace),
                                        'report': _redact_data(cobj, self.workspace),
                                    })
                                continue
                            except ValueError:
                                pass
                        lines = [ln for ln in ctext.splitlines() if ln.strip() and _matches_package(ln, package_id)]
                        if lines:
                            crash_reports.append({
                                'source': _redact_text(str(item), self.workspace),
                                'lines': [_redact_text(ln, self.workspace) for ln in lines[-50:]],
                            })

        process_log_tail = None
        if profile_dir is not None:
            for log_name in ('extension-store-process.log', 'extension-dev-process.log'):
                ltext = _read_bounded_text(profile_dir / log_name, 16384, tail=True)
                if ltext is not None:
                    process_log_tail = _redact_text(ltext, self.workspace)
                    break

        return dict(
            profile=_redact_text(str(profile_dir), self.workspace) if profile_dir is not None else None,
            packageId=package_id,
            redacted=True,
            environmentIncluded=False,
            startupStatus=startup_status,
            auditStatus=audit_status,
            auditEvents=audit_events,
            crashAttribution=dict(**crash_markers, reports=crash_reports),
            processLogTail=process_log_tail,
        )

    def _resolve_stop_profile(self, pid, explicit_profile=None):
        profiles_root = self.inside('profiles', exists=False)
        if explicit_profile is not None:
            profile_dir = self.inside(explicit_profile)
            try:
                profile_dir.relative_to(profiles_root)
            except ValueError as error:
                raise AuthorError('profile must reside under workspace profiles/') from error
            record_file = profile_dir / 'extension-host-instance.json'
            if record_file.is_symlink() or not record_file.is_file():
                raise AuthorError('profile has no recorded extension-host-instance.json')
            record = json.loads(record_file.read_text(encoding='utf-8'))
            if record.get('processId') != pid:
                raise AuthorError(f'profile recorded PID {record.get("processId")} does not match requested PID {pid}')
            return profile_dir

        if not profiles_root.is_dir():
            raise AuthorError(f'no recorded isolated host profile in this workspace matches PID {pid}')
        matches = []
        for entry in sorted(profiles_root.iterdir()):
            if entry.is_symlink() or not entry.is_dir():
                continue
            record_file = entry / 'extension-host-instance.json'
            if record_file.is_symlink() or not record_file.is_file():
                continue
            try:
                record = json.loads(record_file.read_text(encoding='utf-8'))
            except (OSError, ValueError):
                continue
            if isinstance(record, dict) and record.get('processId') == pid:
                matches.append(entry)
        if not matches:
            raise AuthorError(f'no recorded isolated host profile in this workspace matches PID {pid}')
        return matches[-1]

    def execute(self, name, arguments, cancelled):
        commands = []
        try:
            spec = next((item for item in TOOLS if item['name'] == name), None)
            if not spec:
                raise AuthorError('unknown tool')
            validate(arguments, spec)
            if cancelled.is_set():
                raise AuthorError('cancelled')
            if name == 'native_sdk_describe':
                packable_templates = list(project_creator.templates(self.sdk))
                abi_samples = list(project_creator.abi_samples(self.sdk))
                ref_samples = _reference_samples(self.sdk)
                catalog = json.loads(self.catalog.read_text(encoding='utf-8'))
                if 'interface' in arguments:
                    contract = next((item for item in catalog['interfaces'] if item['name'] == arguments['interface']), None)
                    if contract is None:
                        raise AuthorError('unknown SDK interface')
                else:
                    contract = [dict(name=item['name'], version=item['version'], header=item.get('header'),
                                     documentation=item.get('documentation')) for item in catalog['interfaces']]
                value = dict(
                    interfaces=contract,
                    templates=packable_templates,
                    abiSamples=abi_samples,
                    referenceSamples=ref_samples,
                    sdkPreview=catalog.get('sdkPreview'),
                    workspace=str(self.workspace),
                    requirements='Windows x64 Visual Studio developer environment (auto-discovered via vswhere/vcvars64.bat), CMake 3.25+, Ninja, Python 3',
                    installStore=str(self.workspace / '.pulse-author/extensions'),
                )
            elif name == 'native_project_create':
                project_name, template = arguments['name'], arguments['template']
                if (len(project_name) > 64 or not project_name[0].isascii() or not project_name[0].islower() or
                        any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in project_name)):
                    raise AuthorError('name must match [a-z][a-z0-9-]{0,63}')
                destination = self.inside('projects/' + project_name, exists=False)
                if destination.exists():
                    raise AuthorError('project already exists; files were preserved')
                try:
                    package_id = arguments.get('id') or project_creator.derive_package_id(project_name)
                    display_name = arguments.get('displayName') or project_creator.derive_display_name(project_name)
                    created = project_creator.create_project(self.sdk, destination, package_id, display_name, template)
                except project_creator.ProjectCreationError as error:
                    raise AuthorError(f'{error.code}: {error}') from error
                value = dict(
                    project=created['path'],
                    template=created['template'],
                    packageId=created['packageId'],
                    displayName=created['displayName'],
                    manifest=created['manifest'],
                    cmakeLists=created['cmakeLists'],
                    readme=created['readme'],
                    libraryStem=created['libraryStem'],
                    targets=created['targets'],
                    primaryLibrary=created['primaryLibrary'],
                    files=created['files'],
                )
            elif name == 'native_project_build':
                project = self.inside(arguments['project'])
                if not (project / 'CMakeLists.txt').is_file():
                    raise AuthorError('project requires CMakeLists.txt')
                config = arguments.get('config', 'Release')
                build_subdir = 'build' if config == 'Release' else ('build-' + config.lower())
                build = self.inside(str(project.relative_to(self.workspace)) + '/' + build_subdir, exists=False)
                self.run(['cmake', '-S', str(project), '-B', str(build), '-G', 'Ninja',
                          '-DCMAKE_BUILD_TYPE=' + config,
                          '-DPULSE_SDK_INCLUDE=' + str(self.sdk / 'sdk/native/include')], cancelled, commands)
                self.run(['cmake', '--build', str(build), '--parallel', '2'], cancelled, commands)
                libraries = [str(p) for p in sorted(build.glob('*.dll'))]
                executables = [str(p) for p in sorted(build.glob('*.exe'))]
                symbols = [str(p) for p in sorted(build.glob('*.pdb'))]
                debugger_attach = dict(
                    hostExecutable=str(self.pulse) if self.pulse is not None else None,
                    symbolSearchPaths=[str(build)],
                    visualStudio='Debug > Attach to Process > select isolated pulse.exe PID returned by native_host_run (Native code)',
                    vsCodeLaunchConfig=dict(
                        name='Attach to Pulse Isolated Host',
                        type='cppvsdbg',
                        request='attach',
                        processId='${command:pickProcess}',
                        symbolSearchPath=str(build),
                    ),
                )
                value = dict(
                    project=str(project),
                    build=str(build),
                    config=config,
                    libraries=libraries,
                    executables=executables,
                    symbols=symbols,
                    debuggerAttach=debugger_attach,
                )
            elif name == 'native_package_stage':
                project, library = self.inside(arguments['project']), self.inside(arguments['library'])
                if not library.is_file() or library.suffix.lower() != '.dll':
                    raise AuthorError('library must be a compiled workspace DLL')
                manifest_template = project / 'manifest.json'
                if not manifest_template.is_file():
                    raise AuthorError('project requires manifest.json')
                output = self.inside('packages', exists=False)
                output.mkdir(exist_ok=True)
                # Each stage is distinct so callers cannot accidentally replace a reviewed package.
                stage = Path(tempfile.mkdtemp(prefix=project.name + '-', dir=output))
                staged_library = stage / library.name
                shutil.copyfile(library, staged_library)
                manifest, package = stage / 'manifest.json', stage / (project.name + '.pulse-ext')
                extra = []
                names = {library.name.lower(), 'manifest.json', 'package.json'}
                for item in arguments.get('files', []):
                    source = self.inside(item)
                    if not source.is_file() or source.name.lower() in names:
                        raise AuthorError('additional files must be distinct regular workspace files')
                    names.add(source.name.lower())
                    target = stage / source.name
                    shutil.copyfile(source, target)
                    extra.extend(['--file', str(target)])
                self.run(self.cli('native-manifest', manifest_template, '--library', staged_library, '--output', manifest, *extra), cancelled, commands, 40)
                self.run(self.cli('pack', manifest, '--output', package), cancelled, commands, 40)
                verified = json.loads(self.run(self.cli('verify', package), cancelled, commands, 40))
                value = dict(package=str(package), manifest=str(manifest), verified=verified)
            elif name == 'native_package_install':
                if arguments['allowNative'] is not True:
                    raise AuthorError('allowNative=true is required for native test installation')
                package = self.inside(arguments['package'])
                if not package.is_file() or package.suffix != '.pulse-ext':
                    raise AuthorError('package must be a workspace .pulse-ext file')
                store = self.inside('.pulse-author/extensions', exists=False)
                installed = json.loads(self.run(self.cli('install', package, '--store', store, '--allow-native', '--accept-capabilities'), cancelled, commands, 40))
                value = dict(store=str(store), installed=installed, enabled=False, activation='next host startup')
                if arguments.get('enable', False):
                    identity = installed.get('installation', {}).get('package', {}).get('id')
                    if not identity:
                        raise AuthorError('CLI install did not expose package identity; installed disabled')
                    value['enabledResult'] = json.loads(self.run(self.cli('enable', identity, '--store', store, '--allow-native'), cancelled, commands, 40))
                    value['enabled'] = True
            elif name == 'native_host_run':
                if self.pulse is None:
                    raise AuthorError('Pulse host executable (--pulse) is required to run an isolated host')
                if arguments['allowNative'] is not True:
                    raise AuthorError('allowNative=true is required to run installed native code')
                store = self.inside('.pulse-author/extensions')
                profiles = self.inside('profiles', exists=False)
                profiles.mkdir(exist_ok=True)
                profile = profiles / ('run-' + str(time.time_ns()))
                value = json.loads(self.run(self.cli('run', '--store', store, '--profile', profile), cancelled, commands))
                value['profile'] = str(profile)
            elif name == 'native_host_stop':
                if self.pulse is None:
                    raise AuthorError('Pulse host executable (--pulse) is required to stop an isolated host')
                matched_profile = self._resolve_stop_profile(arguments['pid'], arguments.get('profile'))
                stopped = json.loads(self.run(self.cli('stop-host', '--pid', arguments['pid'], '--profile', matched_profile), cancelled, commands, 30))
                value = dict(profile=str(matched_profile), **stopped)
            elif name == 'native_plugin_test':
                target = self.inside(arguments['target'])
                library_arg = self.inside(arguments['library']) if 'library' in arguments else None
                selection_args = [str(self.inside(item)) for item in arguments.get('selection', [])]
                directory_arg = str(self.inside(arguments['directory'])) if 'directory' in arguments else None
                harness_bin = self._resolve_harness()
                try:
                    test_report = pulse_ext.test_package(
                        None,
                        target,
                        library=library_arg,
                        harness=harness_bin,
                        invoke_commands=tuple(arguments.get('invokeCommands', ())),
                        smoke_commands=bool(arguments.get('smokeCommands', False)),
                        selections=tuple(selection_args),
                        directory=directory_arg,
                        omit_experimental_ui=bool(arguments.get('omitExperimentalUi', False)),
                        strict_resources=bool(arguments.get('strictResources', False)),
                        timeout_ms=int(arguments.get('timeoutMs', 5000)),
                    )
                except pulse_ext.Failure as failure:
                    return dict(
                        ok=False,
                        error=f"{failure.error.get('code', 'plugin_test_failed')}: {failure.error.get('message', str(failure))}",
                        diagnostics=failure.error,
                        commands=commands,
                    )
                value = dict(target=str(target), **test_report)
            elif name == 'native_host_logs':
                value = self._collect_host_logs(
                    explicit_profile=arguments.get('profile'),
                    package_id=arguments.get('packageId'),
                    max_events=int(arguments.get('maxEvents', 100)),
                )
            else:
                expected = json.loads(self.catalog.read_text(encoding='utf-8'))
                toolchain_status = pulse_ext.inspect_toolchain()
                if self.pulse is None:
                    value = dict(
                        host=None,
                        sdk=str(self.sdk),
                        offlineMode=True,
                        catalogMatches=None,
                        abiCompatible=None,
                        diagnosis='offline_toolchain_only',
                        sdkPreview=dict(sdk=expected.get('sdkPreview'), host=None, matches=False),
                        interfaceComparison=None,
                        toolchain=toolchain_status,
                        hostInterfaces=[],
                    )
                else:
                    result = json.loads(self.run([str(self.pulse), '--extension-api', '--extension-check'], cancelled, commands, 40, json_reply=True))
                    actual = result.get('nativeSdk', {}).get('catalog')
                    preview_status, iface_comparison = _compare_catalogs(expected, actual)
                    catalog_matches = actual == expected
                    if catalog_matches:
                        diagnosis_label = 'matched'
                    elif not preview_status['matches']:
                        diagnosis_label = 'sdk_preview_mismatch'
                    elif not iface_comparison['compatible']:
                        diagnosis_label = 'interface_version_mismatch'
                    else:
                        diagnosis_label = 'host_sdk_mismatch'
                    value = dict(
                        host=str(self.pulse),
                        sdk=str(self.sdk),
                        catalogMatches=catalog_matches,
                        abiCompatible=iface_comparison['compatible'],
                        diagnosis=diagnosis_label,
                        sdkPreview=preview_status,
                        interfaceComparison=iface_comparison,
                        toolchain=toolchain_status,
                        hostInterfaces=[dict(name=item['name'], version=item['version']) for item in (actual or {}).get('interfaces', [])],
                    )
            return dict(ok=True, result=value, commands=commands)
        except (AuthorError, OSError, ValueError, subprocess.SubprocessError) as error:
            return dict(ok=False, error=str(error), commands=commands)


def serve(author, input_stream=sys.stdin.buffer, output_stream=sys.stdout):
    lock = threading.Lock()
    pending = {}
    def send(value):
        with lock:
            output_stream.write(json.dumps(value, ensure_ascii=False) + '\n')
            output_stream.flush()
    def respond(identity, result):
        send(dict(jsonrpc='2.0', id=identity, result=result))
    def complete(identity, name, arguments, event):
        result = author.execute(name, arguments, event)
        respond(identity, dict(content=[dict(type='text', text=json.dumps(result, ensure_ascii=False))],
                               structuredContent=result, isError=not result['ok']))
        with lock:
            pending.pop(identity, None)
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        while True:
            line = input_stream.readline(MAX_MESSAGE + 1)
            if not line:
                break
            try:
                if len(line) > MAX_MESSAGE:
                    raise ValueError('message exceeds 1 MiB')
                request = json.loads(line)
                if not isinstance(request, dict) or request.get('jsonrpc') != '2.0':
                    raise ValueError('invalid JSON-RPC request')
                method, identity = request.get('method'), request.get('id')
                parameters = request.get('params', {})
                if not isinstance(parameters, dict):
                    raise ValueError('params must be an object')
                if method == 'notifications/cancelled':
                    with lock:
                        event = pending.get(parameters.get('requestId'))
                    if event:
                        event.set()
                    continue
                if 'id' not in request:
                    continue
                if type(identity) not in (int, str):
                    raise ValueError('id must be a string or integer')
                if method == 'initialize':
                    requested = parameters.get('protocolVersion')
                    version = requested if requested in ('2025-11-25', '2025-06-18', '2025-03-26', '2024-11-05') else '2025-11-25'
                    respond(identity, dict(protocolVersion=version, capabilities=dict(tools=dict(listChanged=False)),
                        serverInfo=dict(name='pulse-native-author', version='1'),
                        instructions='Author C++ plugins against the public Pulse SDK. Toolchain auto-discovers MSVC x64 via vswhere/vcvars64.bat. Smoke-test headlessly with native_plugin_test; install tools use only the configured author test store.'))
                elif method == 'ping':
                    respond(identity, {})
                elif method == 'tools/list':
                    respond(identity, dict(tools=TOOLS))
                elif method == 'tools/call':
                    with lock:
                        duplicate = identity in pending
                        if not duplicate:
                            event = threading.Event()
                            pending[identity] = event
                    if duplicate:
                        raise ValueError('request id is already pending')
                    pool.submit(complete, identity, parameters.get('name'), parameters.get('arguments', {}), event)
                else:
                    send(dict(jsonrpc='2.0', id=identity, error=dict(code=-32601, message='Method not found')))
            except (ValueError, TypeError, KeyError) as error:
                send(dict(jsonrpc='2.0', id=None, error=dict(code=-32600, message=str(error))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--sdk-root', type=Path, default=ROOT)
    parser.add_argument('--pulse', type=Path, default=None)
    args = parser.parse_args()
    pulse_path = args.pulse
    if pulse_path is None:
        for candidate in (args.sdk_root / 'build/pulse.exe', args.sdk_root / 'bin/Pulse.NativePreview.exe', ROOT / 'build/pulse.exe'):
            if candidate.is_file():
                pulse_path = candidate
                break
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    serve(Author(args.workspace, args.sdk_root, pulse_path))


if __name__ == '__main__':
    main()
