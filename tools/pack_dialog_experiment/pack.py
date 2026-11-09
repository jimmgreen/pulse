"""Create a NEW, explicitly experimental, architecture-specific dialog bundle.
No installation, registry mutation, component execution, overwrite, or cleanup.
"""
import shutil
import tempfile
import time
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
from pe_dependencies import PE, PEError

class PackageError(ValueError):
    pass

MACHINES = {'x64': 0x8664, 'x86': 0x14c}
CONTRACT = {'manifest': 1, 'picker_ui_abi': 2, 'runtime_session': 1,
            'open_policy': 'LocalOpenV1', 'save_policy': 'LocalSaveV1',
            'experimental': True, 'capability_certification': False}
# Explicit Windows runtime requirements, not copied from PATH/System32. Unknown
# names fail unless a matching local dependency exists. No OS availability claim.
SYSTEM = set('''kernel32.dll kernelbase.dll ntdll.dll user32.dll gdi32.dll gdi32full.dll
advapi32.dll ole32.dll oleaut32.dll oleacc.dll shell32.dll shlwapi.dll comdlg32.dll comctl32.dll
uuid.dll rpcrt4.dll combase.dll imm32.dll winmm.dll version.dll uxtheme.dll dwmapi.dll
shcore.dll d3d11.dll d2d1.dll dwrite.dll dxgi.dll dcomp.dll windowscodecs.dll gdiplus.dll
bcrypt.dll bcryptprimitives.dll crypt32.dll cryptbase.dll secur32.dll sspicli.dll
winhttp.dll wininet.dll ws2_32.dll dnsapi.dll iphlpapi.dll netapi32.dll mpr.dll
propsys.dll cfgmgr32.dll setupapi.dll userenv.dll wtsapi32.dll powrprof.dll profapi.dll virtdisk.dll
psapi.dll dbghelp.dll dbgcore.dll imagehlp.dll wintrust.dll normaliz.dll usp10.dll
msvcrt.dll ucrtbase.dll mf.dll mfplat.dll mfreadwrite.dll mfuuid.dll mfplay.dll
avrt.dll msacm32.dll msimg32.dll opengl32.dll glu32.dll winspool.drv hid.dll
urlmon.dll cabinet.dll cryptui.dll d3d9.dll d3d12.dll dxcore.dll dinput8.dll
wevtapi.dll wldap32.dll winusb.dll wlanapi.dll sensapi.dll authz.dll dwmapi.dll rstrtmgr.dll xmllite.dll
'''.split())
API_SET = re.compile(r'(?:api|ext)-ms-win-[a-z0-9-]+\.dll')
# Reviewed runtime loading edges, separate from PE imports. Expand only with a
# source reference and a targeted dependency test; never glob every build binary.
DYNAMIC = {
    'Pulse.Preview.exe': 'src/ui/thumbnail_cache.cpp: module-relative preview worker',
    'pdfium.dll': 'src/preview_host/pdf_raster.cpp: dynamic PDF renderer',
    'pulse_elevated.exe': 'src/ops/elevated_transfer_client.cpp: explicit elevated operation helper',
}
REPO_LICENSES = {
    'LICENSE': 'licenses/Pulse/LICENSE',
    'third_party/detours/LICENSE.md': 'licenses/Detours/LICENSE.md',
    'third_party/ib-pinyin-cpp/LICENSE.txt': 'licenses/ib-pinyin/LICENSE.txt',
    'third_party/md4c/LICENSE.md': 'licenses/md4c/LICENSE.md',
    'third_party/sqlite/README.md': 'licenses/SQLite/README.md',
    'third_party/lzma/README.pulse.md': 'licenses/LZMA/README.pulse.md',
}

def roots(arch, automatic=False):
    return ['PulseDialogLauncher.exe', f'PulseFileDialogRuntimeUiProof{32 if arch == "x86" else 64}R8.dll',
            'pulse.exe', 'PulsePickerUiProof.dll', 'lumatext.dll', 'pulse_shell.exe'] + ([f'PulseDialogAuto{64 if arch == "x64" else 32}.dll', 'PulseDialogAutoControl.exe'] if automatic else [])

def system(name):
    return name in SYSTEM or bool(API_SET.fullmatch(name))

def safe_relative(text):
    if not isinstance(text, str) or not text or '\\' in text or ':' in text or '\0' in text:
        raise PackageError(f'unsafe relative path: {text!r}')
    path = PurePosixPath(text)
    if path.is_absolute() or str(path) != text or any(p in ('.', '..') for p in path.parts):
        raise PackageError(f'unsafe relative path: {text!r}')
    return path

def no_links(path):
    path = Path(path).absolute()
    for current in [*reversed(path.parents), path]:
        info = current.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise PackageError(f'reparse/symlink path rejected: {current}')
    return path

def digest(path):
    sha = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''): sha.update(block)
    return sha.hexdigest()

def entries(directory):
    result = {}
    for child in directory.iterdir():
        key = child.name.lower()
        if key in result: raise PackageError(f'case-colliding source: {child.name}')
        result[key] = child
    return result

def record(path, relative, role, arch=None):
    no_links(path)
    if not path.is_file(): raise PackageError(f'not a regular file: {path}')
    safe_relative(relative)
    item = {'path': relative, 'role': role, 'size': path.stat().st_size, 'sha256': digest(path)}
    if arch:
        pe = PE(path)
        if pe.machine != MACHINES[arch]: raise PackageError(f'wrong architecture: {relative}: {pe.machine:#x}, expected {arch}')
        item.update(machine=pe.machine, imports=pe.normal, delay_imports=pe.delay)
    return item

def plan(build, version, arch, repository, automatic=False, runtime_items=()):
    if arch not in MACHINES or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', version):
        raise PackageError('invalid architecture or version label')
    if not Path(build).is_absolute() or not Path(repository).is_absolute(): raise PackageError('absolute build/repository paths required')
    build, repository = no_links(build), no_links(repository)
    local = entries(build)
    pending = [(name, 'entry') for name in roots(arch, automatic)] + [(name, 'dynamic-helper') for name in DYNAMIC]
    for source, relative in runtime_items:
        safe_relative(relative)
        if not relative.startswith('licenses/'):
            if '/' in relative or Path(source).resolve() != (build / relative).resolve():
                raise PackageError('runtime binary must be a build direct child')
            pending.append((relative, 'entry'))
    seen, sources, files, errors, os_dependencies = set(), {}, [], [], set()
    while pending:
        name, role = pending.pop(0)
        key = name.lower()
        if key in seen: continue
        seen.add(key)
        path = local.get(key)
        if path is None:
            errors.append(f'missing dependency: {name}'); continue
        try:
            item = record(path, path.name, role, arch)
            files.append(item); sources[item['path']] = path
            for dependency in item['imports'] + item['delay_imports']:
                if system(dependency):
                    if dependency in local: errors.append(f'local file shadows system dependency: {dependency}')
                    os_dependencies.add(dependency)
                else: pending.append((dependency, 'import'))
        except (OSError, ValueError) as error: errors.append(f'{name}: {error}')
    for group in ('LumaText', 'PDFium'):
        folder = build / 'licenses' / group
        if not folder.is_dir(): errors.append(f'missing license directory: {folder}'); continue
        no_links(folder)
        candidates = sorted(folder.rglob('*'))
        if not any(p.is_file() for p in candidates): errors.append(f'empty license directory: {group}')
        for path in candidates:
            no_links(path)
            if not path.is_file(): continue
            relative = path.relative_to(build).as_posix()
            files.append(record(path, relative, 'license')); sources[relative] = path
    for source, relative in REPO_LICENSES.items():
        path = repository / source
        try: files.append(record(path, relative, 'license')); sources[relative] = path
        except (OSError, ValueError) as error: errors.append(f'license {source}: {error}')
    for source, relative in runtime_items:
        if not relative.startswith('licenses/'): continue
        existing = next((item for item in files if item['path'].lower() == relative.lower()), None)
        if existing:
            if digest(source) != existing['sha256']: errors.append(f'conflicting runtime license: {relative}')
        else:
            files.append(record(source, relative, 'license')); sources[relative] = source
    if errors: raise PackageError('\n'.join(errors))
    if len(files) > 512 or len({f['path'].lower() for f in files}) != len(files): raise PackageError('manifest size/case collision')
    manifest = {'schema': 1, 'version': version, 'architecture': arch, 'contract': CONTRACT,
                'dynamic_helpers': DYNAMIC, 'system_dependencies': sorted(os_dependencies),
                'files': sorted(files, key=lambda f: f['path'].lower())}
    return manifest, sources

def publish_directory(source, destination):
    # Windows can transiently refuse a freshly copied image directory while a
    # reader holds it without delete sharing. Never replace an existing target;
    # bounded retries do not waive the caller's post-publication verification.
    delays = (0.05, 0.1, 0.2, 0.4, 0.8)
    for attempt in range(len(delays) + 1):
        try:
            source.rename(destination)
            return
        except OSError as error:
            if getattr(error, 'winerror', None) not in (5, 32, 33) or attempt == len(delays):
                raise
            time.sleep(delays[attempt])


def package_addressed(build, bridge, version, arch, repository, runtime_items=()):
    """Publish a complete private schema-1 closure; never update a live bundle."""
    bridge = no_links(bridge)
    # Build in a new sibling directory, so only a fully verified bundle can
    # acquire its final name. Both paths are on the same filesystem.
    with tempfile.TemporaryDirectory(prefix='.dialog-staging-', dir=bridge.parent) as staging:
        candidate = Path(staging) / 'bundle'
        package(build, candidate, version, arch, repository, automatic=True, runtime_items=runtime_items)
        manifest_hash = digest(candidate / 'manifest.json')
        destination = bridge / manifest_hash
        if destination.exists() or destination.is_symlink():
            existing = verify(destination)
            if existing['schema'] != 1 or digest(destination / 'manifest.json') != manifest_hash:
                raise PackageError('existing content-addressed bundle mismatch')
        else:
            publish_directory(candidate, destination)
        verify(destination)
    return {'schema': 1, 'directory': f'DialogBridge/{manifest_hash}',
            'manifest_sha256': manifest_hash}


def verify_installed(directory):
    """Validate the installed pointer and its self-contained runtime bundle."""
    directory = no_links(directory)
    pointer_path = no_links(directory / 'dialog-current.json')
    if pointer_path.stat().st_size > 1024: raise PackageError('dialog pointer bound')
    pointer = json.loads(pointer_path.read_text(encoding='utf-8'), object_pairs_hook=unique_object)
    if not isinstance(pointer, dict) or set(pointer) != {'schema', 'directory', 'manifest_sha256'} or type(pointer['schema']) is not int or pointer['schema'] != 1:
        raise PackageError('dialog pointer schema')
    manifest_hash = pointer['manifest_sha256']
    if not isinstance(manifest_hash, str) or not re.fullmatch(r'[0-9a-f]{64}', manifest_hash):
        raise PackageError('dialog pointer hash')
    if pointer['directory'] != f'DialogBridge/{manifest_hash}':
        raise PackageError('dialog pointer directory/hash mismatch')
    bundle = no_links(directory / pointer['directory'])
    if digest(no_links(bundle / 'manifest.json')) != manifest_hash:
        raise PackageError('dialog pointer manifest hash mismatch')
    manifest = verify(bundle)
    if manifest['schema'] != 1: raise PackageError('installed dialog bundle requires schema 1')
    names = {item['path'].lower() for item in manifest['files']}
    if not all(name.lower() in names for name in roots(manifest['architecture'], automatic=True)):
        raise PackageError('installed dialog bundle requires automatic components')
    forbidden = {'manifest.json', 'verified.json'}
    for arch in MACHINES:
        forbidden.update(name.lower() for name in roots(arch, True)
                         if name.lower() not in {'pulse.exe', 'lumatext.dll', 'pulse_shell.exe'})
    if any(path.name.lower() in forbidden for path in directory.iterdir()):
        raise PackageError('flat dialog component in installed root')
    if any(path.suffix.lower() in ('.exe', '.dll') and path.name.lower() != 'pulse.exe'
           for path in directory.iterdir()):
        raise PackageError('duplicate runtime binary in installed root')
    launcher = record(directory / 'pulse.exe', 'pulse.exe', 'entry', manifest['architecture'])
    if any(not system(dep) for dep in launcher['imports'] + launcher['delay_imports']):
        raise PackageError('stable launcher requires non-system dependency')
    return pointer


def package_installed(build, output, version, arch, repository, items):
    """Root is only a stable launcher; all application and dialog files share one version."""
    output = Path(output)
    if not output.is_absolute(): raise PackageError('absolute NEW output required')
    no_links(output.parent)
    if output.exists() or output.is_symlink(): raise PackageError('output already exists; never overwrite or clean')
    if output.is_relative_to(Path(build).absolute()): raise PackageError('output must be outside build directory')
    launcher = Path(build) / 'pulse_launcher.exe'
    launcher_record = record(launcher, 'pulse.exe', 'entry', arch)
    output.mkdir()
    with launcher.open('rb') as src, (output / 'pulse.exe').open('xb') as dst:
        shutil.copyfileobj(src, dst)
    if digest(output / 'pulse.exe') != launcher_record['sha256']:
        raise PackageError('launcher changed during copy')
    bridge = output / 'DialogBridge'
    bridge.mkdir()
    pointer = package_addressed(build, bridge, version, arch, repository, items)
    with (output / 'dialog-current.json').open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(pointer, stream, separators=(',', ':')); stream.write('\n')
    return verify_installed(output)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise PackageError(f'duplicate JSON key: {key}')
        result[key] = value
    return result
def verify(directory, require_marker=True):
    directory = no_links(directory)
    manifest_path = directory / 'manifest.json'
    no_links(manifest_path)
    if manifest_path.stat().st_size > 1024 * 1024: raise PackageError('manifest bound')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'), object_pairs_hook=unique_object)
    if set(manifest) != {'schema', 'version', 'architecture', 'contract', 'dynamic_helpers', 'system_dependencies', 'files'} or manifest['schema'] not in (1, 2):
        raise PackageError('manifest schema')
    arch = manifest['architecture']
    if arch not in MACHINES or manifest['contract'] != CONTRACT or manifest['dynamic_helpers'] != DYNAMIC:
        raise PackageError('unknown component contract')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', manifest['version']): raise PackageError('version label')
    files = manifest['files']
    if not isinstance(files, list) or not 1 <= len(files) <= 512: raise PackageError('file count')
    by_name, expected, os_dependencies = {}, {'manifest.json'}, set()
    for item in files:
        relative = str(safe_relative(item['path']))
        if relative.lower() in expected: raise PackageError('duplicate file path')
        expected.add(relative.lower())
        role = item['role']
        if role not in ('entry', 'import', 'dynamic-helper', 'license'): raise PackageError('unknown file role')
        if role == 'license':
            if not relative.startswith('licenses/'): raise PackageError('license path')
        elif '/' in relative: raise PackageError('binary must be a direct bundle child')
        actual = record(directory / relative, relative, role, None if role == 'license' else arch)
        if actual != item: raise PackageError(f'file metadata/hash/import mismatch: {relative}')
        if role != 'license': by_name[relative.lower()] = item
    for name in roots(arch) + list(DYNAMIC):
        if name.lower() not in by_name: raise PackageError(f'missing mandatory component: {name}')
    wrong_auto = 'pulsedialogauto32.dll' if arch == 'x64' else 'pulsedialogauto64.dll'
    if wrong_auto in by_name: raise PackageError('wrong automatic component architecture name')
    auto_name = 'pulsedialogauto64.dll' if arch == 'x64' else 'pulsedialogauto32.dll'
    if (auto_name in by_name) != ('pulsedialogautocontrol.exe' in by_name):
        raise PackageError('incomplete automatic component pair')
    for item in by_name.values():
        for dependency in item['imports'] + item['delay_imports']:
            if system(dependency):
                if dependency in by_name: raise PackageError('packaged system dependency')
                os_dependencies.add(dependency)
            elif dependency not in by_name: raise PackageError(f'unresolved packaged dependency: {dependency}')
    if sorted(os_dependencies) != manifest['system_dependencies']: raise PackageError('system dependency list mismatch')
    for relative in REPO_LICENSES.values():
        if relative.lower() not in expected: raise PackageError(f'missing mandatory license: {relative}')
    for group in ('lumatext', 'pdfium'):
        if not any(p.startswith(f'licenses/{group}/') for p in expected): raise PackageError(f'missing license group: {group}')
    if require_marker:
        marker = directory / 'VERIFIED.json'; no_links(marker)
        if marker.stat().st_size > 1024: raise PackageError('verification marker bound')
        if json.loads(marker.read_text(encoding='utf-8'), object_pairs_hook=unique_object) != {'manifest_sha256': digest(manifest_path), 'integrity_only': True}:
            raise PackageError('verification marker mismatch')
        expected.add('verified.json')
    actual_files = set()
    for path in directory.rglob('*'):
        no_links(path)
        if path.is_file():
            relative = path.relative_to(directory).as_posix().lower()
            if relative in actual_files: raise PackageError('case-colliding destination')
            actual_files.add(relative)
    if actual_files != expected: raise PackageError('unlisted/missing bundle files')
    return manifest

def package(build, output, version, arch, repository, automatic=False, runtime_items=()):
    output = Path(output)
    if not output.is_absolute(): raise PackageError('absolute NEW output required')
    no_links(output.parent)
    if output.exists() or output.is_symlink(): raise PackageError('output already exists; never overwrite or clean')
    manifest, sources = plan(build, version, arch, repository, automatic, runtime_items)
    if output.is_relative_to(Path(build).absolute()): raise PackageError('output must be outside build directory')
    output.mkdir()  # Exclusive creation; failure leaves existing data unchanged.
    # Any failure leaves an incomplete unverified directory for diagnosis.
    for item in manifest['files']:
        source, target = sources[item['path']], output / item['path']
        no_links(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        with source.open('rb') as src, target.open('xb') as dst:
            for block in iter(lambda: src.read(1024 * 1024), b''): dst.write(block)
        if digest(target) != item['sha256']: raise PackageError(f'source changed during copy: {source.name}')
    with (output / 'manifest.json').open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2); stream.write('\n')
    verify(output, False)
    with (output / 'VERIFIED.json').open('x', encoding='utf-8', newline='\n') as stream:
        json.dump({'manifest_sha256': digest(output / 'manifest.json'), 'integrity_only': True}, stream)
    return verify(output)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('plan', 'pack'):
        p = sub.add_parser(command)
        p.add_argument('--build', type=Path, required=True)
        p.add_argument('--version', required=True)
        p.add_argument('--arch', choices=MACHINES, required=True)
        p.add_argument('--auto', action='store_true', help='include current-user architecture-matched automatic COM entry')
        p.add_argument('--repository', type=Path, default=Path(__file__).resolve().parents[2])
        if command == 'pack': p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('verify'); p.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'plan': result, _ = plan(args.build, args.version, args.arch, args.repository, args.auto)
        elif args.command == 'pack': result = package(args.build, args.output, args.version, args.arch, args.repository, args.auto)
        else: result = verify(args.directory)
        print(json.dumps({'result': 'verified-files' if args.command != 'plan' else 'read-only-plan',
                          'architecture': result['architecture'], 'version': result['version'],
                          'file_count': len(result['files']), 'capability_certification': False}, ensure_ascii=False))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f'FAIL: {error}', file=sys.stderr); return 1

if __name__ == '__main__': sys.exit(main())
