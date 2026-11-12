import argparse, difflib, hashlib, json, shutil
from pathlib import Path

GUARD = 'if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)\n  message(FATAL_ERROR "LumaText 0.1 supports Windows x64 only")\nendif()'
PORT = 'if(NOT WIN32 OR NOT (CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_SIZEOF_VOID_P EQUAL 4))\n  message(FATAL_ERROR "LumaText experimental adapter requires Windows x64 or x86")\nendif()'
PARTS = ('CMakeLists.txt', 'LICENSE', 'LICENSES', 'cmake', 'include', 'src')
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def prepare(source, output):
    source, output = Path(source).resolve(strict=True), Path(output).absolute()
    if output.exists() or output.is_relative_to(source):
        raise ValueError('NEW output outside external source required')
    raw_original = (source / 'CMakeLists.txt').read_bytes()
    original = raw_original.decode('utf-8').replace('\r\n', '\n')
    if original.count(GUARD) != 1:
        raise ValueError('upstream architecture guard changed; review required')
    output.mkdir(parents=True)
    provenance = {'source': str(source), 'experimental_architecture': 'x86',
                  'changes': 'CMake architecture admission only; no renderer code change', 'files': []}
    for part in PARTS:
        src, dst = source / part, output / part
        if src.is_dir(): shutil.copytree(src, dst)
        else: shutil.copy2(src, dst)
    shutil.copy2(source / 'CMakeLists.txt', output / 'x86-CMakeLists.original.txt')
    adapted = original.replace(GUARD, PORT)
    newline = '\r\n' if b'\r\n' in raw_original else '\n'
    (output / 'CMakeLists.txt').write_bytes(adapted.replace('\n', newline).encode('utf-8'))
    for part in PARTS:
        src = source / part
        for path in sorted(src.rglob('*')) if src.is_dir() else [src]:
            if path.is_file():
                relative = path.relative_to(source)
                provenance['files'].append({'path': relative.as_posix(), 'source_sha256': digest(path),
                                            'copy_sha256': digest(output / relative)})
    (output / 'x86-source.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True), adapted.splitlines(True),
                           fromfile='a/CMakeLists.txt', tofile='b/CMakeLists.txt')), encoding='utf-8')
    (output / 'x86-source.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
    for item in provenance['files']:
        if digest(source / item['path']) != item['source_sha256']:
            raise ValueError('external source changed during snapshot')
        if item['path'] != 'CMakeLists.txt' and item['source_sha256'] != item['copy_sha256']:
            raise ValueError('unexpected source adaptation')
    print(json.dumps({'result': 'ok', 'copied_files': len(provenance['files']),
                      'changed_files': ['CMakeLists.txt'], 'output': str(output)}))
def rollback(output):
    output = Path(output).resolve(strict=True)
    manifest = json.loads((output / 'x86-source.json').read_text(encoding='utf-8'))
    entry = next(item for item in manifest['files'] if item['path'] == 'CMakeLists.txt')
    original = output / 'x86-CMakeLists.original.txt'
    if digest(original) != entry['source_sha256']:
        raise ValueError('original source hash mismatch')
    current = digest(output / 'CMakeLists.txt')
    if current not in (entry['source_sha256'], entry['copy_sha256']):
        raise ValueError('adapted source changed after preparation')
    shutil.copyfile(original, output / 'CMakeLists.txt')
    if digest(output / 'CMakeLists.txt') != entry['source_sha256']:
        raise ValueError('rollback hash mismatch')
    print(json.dumps({'result': 'restored', 'changed_files': ['CMakeLists.txt'], 'sha256': entry['source_sha256']}))
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Copy real LumaText sources; admit an explicitly experimental x86 build without editing the source checkout.')
    parser.add_argument('--source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rollback', action='store_true')
    args = parser.parse_args()
    if args.rollback: rollback(args.output)
    elif args.source: prepare(args.source, args.output)
    else: parser.error('--source required unless --rollback')
