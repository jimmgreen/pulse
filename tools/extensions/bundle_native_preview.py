"""Package a matching, isolated developer host beside an immutable native source SDK."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def pe64(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError(f'Not a PE binary: {path}')
    offset = struct.unpack_from('<I', data, 60)[0]
    if offset + 6 > len(data) or data[offset:offset + 4] != b'PE\0\0' or struct.unpack_from('<H', data, offset + 4)[0] != 0x8664:
        raise ValueError(f'Not an x64 PE binary: {path}')
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-sdk', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    payload = {}
    with zipfile.ZipFile(args.source_sdk) as source:
        manifest = json.loads(source.read('native-sdk-manifest.json'))
        for name, expected in manifest['files'].items():
            if name.startswith('/') or '..' in Path(name).parts or '\\' in name:
                raise ValueError('Invalid SDK archive member')
            data = source.read(name)
            if sha(data) != expected:
                raise ValueError(f'SDK hash mismatch: {name}')
            payload[name] = data
        payload['native-sdk-manifest.json'] = source.read('native-sdk-manifest.json')
    payload['bin/Pulse.NativePreview.exe'] = pe64(args.host)
    payload['bin/pulse_shell.exe'] = pe64(args.host.parent / 'pulse_shell.exe')
    payload['bin/lumatext.dll'] = pe64(args.host.parent / 'lumatext.dll')
    notices = args.host.parent / 'licenses/LumaText'
    for path in notices.iterdir():
        if path.is_symlink() or not path.is_file():
            raise ValueError('Unexpected renderer notice entry')
        payload[f'bin/licenses/LumaText/{path.name}'] = path.read_bytes()
    payload['LICENSE'] = (root / 'LICENSE').read_bytes()
    payload['README-native-preview.md'] = (root / 'sdk/native/README.md').read_bytes()
    for name in ('test_native_runtime_ui.py', 'test_native_inspector_ui.py', 'test_native_reactive_ui.py', 'test_native_packages_ui.py', 'test_native_data_ui.py', 'test_native_directory_ui.py', 'test_native_fileops_ui.py', 'test_native_target_ui.py', 'test_native_workbench_ui.py'):
        payload[f'tools/extensions/{name}'] = (root / 'tools/extensions' / name).read_bytes()
    report = dict(formatVersion=1, delivery='trusted-native-developer-preview',
                  productionInstaller=False, experimentalScriptHostIncluded=False,
                  sourceSdkSha256=sha(args.source_sdk.read_bytes()),
                  files={name: sha(data) for name, data in payload.items()})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in payload.items():
            archive.writestr(name, data)
        archive.writestr('native-preview-manifest.json', json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(path=str(args.output), sha256=sha(args.output.read_bytes()), files=len(payload))))


if __name__ == '__main__':
    main()
