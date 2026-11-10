"""Bundle bounded declarative metadata references and preview tools; never include a script engine or Pulse host."""
import argparse
import hashlib
import io
import json
import os
import posixpath
import re
import sys
from pathlib import Path
import stat
import struct
import tempfile
import unicodedata
import zipfile

import generate_sdk
import pulse_ext
import template_catalog

ROOT = Path(__file__).resolve().parents[2]
MAX_FILE = 32 * 1024 * 1024
MAX_TOTAL = 64 * 1024 * 1024
MAX_FILES = 80
FIXED_FILES = (
    'LICENSE',
    'sdk/native/README.md',
    'docs/extensions-sdk-quickstart.md',
    'schemas/extensions/store.schema.json',
    'schemas/extensions/action-exposure-policy.json',
    'docs/native-packages.md',
    'sdk/native/api-catalog.json',
    'sdk/native/samples/archive/plugin.cpp',
    'sdk/native/ui-model.schema.json',
    'tools/extensions/package_signing.py',
    'tools/extensions/pulse_ext.py',
    'tools/extensions/package_store.py',
    'tools/extensions/contract_compat.py',
    'tools/extensions/native_archive.py',
    'tools/extensions/template_catalog.py',
    'tools/extensions/README.md',
    'schemas/extensions/resource-response.schema.json',
    'schemas/extensions/resource-request.schema.json',
    'schemas/extensions/manifest.schema.json',
    'sdk/typescript/index.d.ts',
    'sdk/typescript/manifest.generated.d.ts',
    'sdk/typescript/api.generated.d.ts',
    'examples/native/catalog.json',
)
LAUNCHER = b'@echo off\r\npython "%~dp0tools\\extensions\\pulse_ext.py" --preview-host "%~dp0bin\\Pulse.ExtensionPreview.exe" %*\r\nexit /b %errorlevel%\r\n'


def regular_bytes(path, limit=MAX_FILE):
    """Reject link/reparse inputs; read a bounded snapshot of a regular file."""
    path = Path(path).absolute()
    for current in (path,) + tuple(path.parents):
        info = current.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Linked SDK input is not supported: ' + str(path))
    if not stat.S_ISREG(path.stat().st_mode):
        raise ValueError('SDK input is not a regular file: ' + str(path))
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError('SDK input exceeds its size limit: ' + str(path))
    return data


def safe_name(name):
    if not isinstance(name, str) or not name or len(name.encode('utf-8')) > 512:
        return False
    forbidden = set('<>:"\\|?*')
    reserved = {'con', 'prn', 'aux', 'nul', 'conin$', 'conout$'} | {
        prefix + str(number) for prefix in ('com', 'lpt') for number in range(1, 10)}
    return all(part and part not in ('.', '..') and not part.endswith(('.', ' ')) and
               unicodedata.normalize('NFKC', part.split('.')[0]).casefold() not in reserved and
               not any(ord(char) < 32 or ord(char) == 127 or char in forbidden for char in part)
               for part in name.split('/'))


def archive_bytes(files, metadata):
    if len(files) > MAX_FILES or sum(len(data) for data in files.values()) > MAX_TOTAL:
        raise ValueError('SDK archive exceeds its budget')
    folded = set()
    for name, data in files.items():
        if not safe_name(name) or name.casefold() in folded or len(data) > MAX_FILE:
            raise ValueError('Invalid or colliding SDK member: ' + name)
        folded.add(name.casefold())
    if 'sdk-release.json' in folded:
        raise ValueError('Reserved SDK metadata name')
    release = dict(metadata)
    release['files'] = [{'path': name, 'bytes': len(files[name]),
                         'sha256': hashlib.sha256(files[name]).hexdigest()} for name in sorted(files)]
    payload = dict(files)
    payload['sdk-release.json'] = (json.dumps(release, ensure_ascii=False, sort_keys=True, indent=2) + '\n').encode('utf-8')
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(payload):
            entry = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, payload[name], compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    return buffer.getvalue(), release


def verify_archive(path):
    raw = regular_bytes(path, MAX_TOTAL + 1024 * 1024)
    # Bound the central directory before ZipFile allocates one object per entry.
    end = raw.rfind(b'PK\x05\x06', max(0, len(raw) - 65557))
    if end < 0 or end + 22 > len(raw):
        raise ValueError('Missing SDK ZIP directory')
    _, disk, directory_disk, disk_count, count, size, offset, comment = struct.unpack_from('<4s4H2LH', raw, end)
    if (disk or directory_disk or disk_count != count or count > MAX_FILES + 1 or
            size > (MAX_FILES + 1) * 2048 or offset + size != end or end + 22 + comment != len(raw)):
        raise ValueError('Unsupported or oversized SDK ZIP directory')
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        entries = archive.infolist()
        if len(entries) != count or not entries:
            raise ValueError('Inconsistent SDK ZIP entry count')
        folded = set()
        total = 0
        for entry in entries:
            name = entry.filename
            mode = entry.external_attr >> 16
            if (not safe_name(name) or len(name.encode('utf-8')) > 512 or name.casefold() in folded or
                    entry.is_dir() or entry.flag_bits & 1 or stat.S_IFMT(mode) not in (0, stat.S_IFREG) or
                    entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) or
                    entry.file_size > MAX_FILE):
                raise ValueError('Unsupported SDK ZIP member')
            folded.add(name.casefold())
            total += entry.file_size
        if total > MAX_TOTAL + 65536:
            raise ValueError('SDK decompression budget exceeded')
        if 'sdk-release.json' not in archive.namelist() or archive.getinfo('sdk-release.json').file_size > 65536:
            raise ValueError('Missing or oversized SDK release manifest')
        release = json.loads(archive.read('sdk-release.json').decode('utf-8'), object_pairs_hook=pulse_ext.strict_object)
        if (not isinstance(release, dict) or type(release.get('formatVersion')) is not int or
                release['formatVersion'] != 1 or not isinstance(release.get('files'), list) or
                len(release['files']) != len(entries) - 1):
            raise ValueError('Invalid SDK release manifest')
        payload = {}
        for item in release['files']:
            if not isinstance(item, dict) or set(item) != {'path', 'bytes', 'sha256'}:
                raise ValueError('Invalid SDK file record')
            name = item['path']
            if (not isinstance(name, str) or not safe_name(name) or name in payload or
                    name == 'sdk-release.json' or name not in archive.namelist() or
                    type(item['bytes']) is not int or not 0 <= item['bytes'] <= MAX_FILE):
                raise ValueError('Invalid SDK file identity')
            data = archive.read(name)
            if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
                raise ValueError('SDK file digest mismatch: ' + name)
            payload[name] = data
        if set(payload) | {'sdk-release.json'} != set(archive.namelist()):
            raise ValueError('SDK inventory differs from archive')
        return release, payload


def portable_documents(files):
    for name in list(files):
        if not name.endswith('.md'):
            continue
        def link(match):
            label, destination = match.groups()
            if '://' in destination or destination.startswith('#'):
                return match.group(0)
            target = posixpath.normpath(posixpath.join(posixpath.dirname(name), destination.split('#')[0]))
            if target in files:
                return match.group(0)
            if target.startswith(('src/', 'bench_data/', 'build-extensions/', 'examples/native/', 'sdk/native/include/', 'tools/extensions/bundle_native_', 'cmake/', 'docs/archive/')):
                return label + ' (source repository: `' + target + '`)'
            raise ValueError('Missing offline documentation link: ' + name + ' -> ' + destination)
        text = files[name].decode('utf-8-sig')
        files[name] = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', link, text).encode('utf-8')


def root_readme(quickstart):
    def relocate(match):
        label, destination = match.groups()
        if '://' in destination or destination.startswith('#'):
            return match.group(0)
        path, separator, fragment = destination.partition('#')
        target = posixpath.normpath(posixpath.join('docs', path))
        return '[' + label + '](' + target + separator + fragment + ')'
    introduction = ('# Pulse declarative metadata reference\n\n'
                    'This bundle contains host JSON/TypeScript contract references and static preview tools. '
                    'Executable plugins use the C++ native SDK. No script runtime is included.\n\n'
                    'See the [metadata reference](sdk/native/README.md) and '
                    '[API maintenance contract](sdk/native/README.md).\n\n')
    return (introduction + re.sub(r'\[([^\]]+)\]\(([^)]+)\)', relocate, quickstart.decode('utf-8-sig'))).encode('utf-8')


def collect(root, preview):
    files = {name: regular_bytes(root / name) for name in FIXED_FILES}
    for name in template_catalog.names(root / 'examples/native'):
        relative = 'examples/native/' + name + '/manifest.json'
        files[relative] = regular_bytes(root / relative, 65536)
    files['pulse-ext.cmd'] = LAUNCHER
    files['README.md'] = root_readme(files['docs/extensions-sdk-quickstart.md'])
    files['bin/Pulse.ExtensionPreview.exe'] = regular_bytes(preview)
    # This build's renderer uses a delayed native dependency; include its actual
    # binary and all notices, rather than relying on a developer's PATH.
    files['bin/lumatext.dll'] = regular_bytes(preview.parent / 'lumatext.dll')
    for name in ('bin/Pulse.ExtensionPreview.exe', 'bin/lumatext.dll'):
        image = files[name]
        if len(image) < 64 or image[:2] != b'MZ':
            raise ValueError('SDK binary is not a PE image')
        offset = struct.unpack_from('<L', image, 60)[0]
        if offset + 6 > len(image) or image[offset:offset + 4] != b'PE\x00\x00' or struct.unpack_from('<H', image, offset + 4)[0] != 0x8664:
            raise ValueError('SDK requires x64 native binaries')
    licenses = preview.parent / 'licenses/LumaText'
    pending = [licenses]
    count = 0
    visited = 0
    while pending:
        directory = pending.pop()
        info = directory.lstat()
        if directory.is_symlink() or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Linked notice directory is not supported')
        with os.scandir(directory) as entries:
            for entry in entries:
                visited += 1
                if visited > 32:
                    raise ValueError('Too many renderer notices')
                path = Path(entry.path)
                info = path.lstat()
                if path.is_symlink() or getattr(info, 'st_file_attributes', 0) & 0x400:
                    raise ValueError('Linked notice entry is not supported')
                if stat.S_ISDIR(info.st_mode):
                    pending.append(path)
                    continue
                name = 'bin/licenses/LumaText/' + path.relative_to(licenses).as_posix()
                files[name] = regular_bytes(path, 2 * 1024 * 1024)
                count += 1
    if not count:
        raise ValueError('Renderer notices are missing')
    portable_documents(files)
    return files


def build(pulse, preview, output):
    pulse, preview, output = pulse.absolute(), preview.absolute(), output.absolute()
    if output.exists():
        raise ValueError('Output directory must not exist')
    expected = generate_sdk.generate(generate_sdk.read_json(generate_sdk.SCHEMA))
    for path, content in expected.items():
        if regular_bytes(path).decode('utf-8').replace('\r\n', '\n') != content:
            raise ValueError('Generated contract is stale: ' + str(path))
    host_hash = hashlib.sha256(regular_bytes(pulse)).hexdigest()
    files = collect(ROOT, preview)
    for path, content in expected.items():
        name = path.relative_to(ROOT).as_posix()
        if name in files and files[name].decode("utf-8").replace("\r\n", "\n") != content:
            raise ValueError("SDK snapshot contains stale generated types")
    schema = json.loads(files['schemas/extensions/manifest.schema.json'].decode('utf-8'))
    api = pulse_ext.host_call(pulse, '--extension-api')
    if api != generate_sdk.describe(schema):
        raise ValueError('Selected host contract differs from this SDK')
    names = json.loads(files['examples/native/catalog.json'].decode('utf-8'))['templates']
    if api.get('templates') != names:
        raise ValueError('Host template catalog differs from this SDK')
    with tempfile.TemporaryDirectory(prefix='pulse-sdk-validate-') as temporary:
        for name in names:
            path = Path(temporary) / (name + '.json')
            path.write_bytes(files['examples/native/' + name + '/manifest.json'])
            result = pulse_ext.host_call(pulse, '--extension-validate-json', path)
            if not result['valid']:
                raise ValueError('Host rejected template ' + name + ': ' + result['error'])
    files['schemas/extensions/host-api.json'] = (json.dumps(api, ensure_ascii=False, sort_keys=True, indent=2) + '\n').encode('utf-8')
    if hashlib.sha256(regular_bytes(pulse)).hexdigest() != host_hash:
        raise ValueError("Host changed during SDK validation")
    data, release = archive_bytes(files, {
        'formatVersion': 1, 'maturity': 'development-preview', 'apiVersion': api['apiVersion'],
        'platform': 'windows-x64', 'pythonMinimum': '3.8', 'templates': names,
        'validatedTemplateCount': len(names), 'testedHostSha256': host_hash,
        'discoverySha256': api['contractIdentity']['sha256'],
        'hostIncluded': False, 'scriptRuntimeIncluded': False, 'purpose': 'declarative-metadata-reference', 'signed': False,
    })
    # The output is new and owned by this invocation. Publish only a completed
    # ZIP, with a no-replacement hard link, matching the package CLI's policy.
    output.mkdir()
    archive_path = output / 'Pulse-Extension-SDK-v1-preview.zip'
    with tempfile.NamedTemporaryFile(dir=output, prefix='.sdk-', delete=False) as stream:
        temporary_archive = Path(stream.name)
        try:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        except BaseException:
            stream.close()
            temporary_archive.unlink()
            raise
    try:
        os.link(temporary_archive, archive_path)
    finally:
        temporary_archive.unlink()
    digest = hashlib.sha256(data).hexdigest()
    with (output / 'SHA256SUMS.txt').open('x', encoding='ascii') as stream:
        stream.write(digest + '  ' + archive_path.name + '\n')
    with (output / 'sdk-release.json').open('x', encoding='utf-8') as stream:
        stream.write(json.dumps(release, ensure_ascii=False, indent=2) + '\n')
    return {'archive': str(archive_path), 'sha256': digest, 'files': len(files) + 1,
            'templatesValidated': len(names), 'maturity': 'development-preview'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--preview-host', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(build(args.pulse, args.preview_host, args.output), ensure_ascii=False))
    except (OSError, ValueError, pulse_ext.Failure) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    main()
