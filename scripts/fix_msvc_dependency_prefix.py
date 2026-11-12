"""Match Ninja's MSVC dependency prefix to the compiler's actual UTF-8 bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

def repair(build, targets=()):
    build = Path(build).resolve(strict=True)
    caches = list((build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    if len(caches) != 1:
        raise ValueError('one configured MSVC compiler cache required')
    compiler_cache = caches[0]
    match = re.search(r'^set\(CMAKE_CXX_COMPILER "([^"]+)"\)', compiler_cache.read_text(encoding='utf-8-sig'), re.M)
    if not match:
        raise ValueError('MSVC compiler path missing')
    probe = build / 'CMakeFiles' / 'pulse_showincludes_probe'
    probe.mkdir(exist_ok=True)
    header, source = probe / 'pulse_prefix_marker.h', probe / 'probe.cpp'
    header.write_text('#define PULSE_PREFIX_MARKER 1\n', encoding='utf-8')
    source.write_text('#include "pulse_prefix_marker.h"\nint pulse_prefix_probe = PULSE_PREFIX_MARKER;\n', encoding='utf-8')
    command = [match[1], '/nologo', '/c', '/showIncludes', '/std:c++20', '/utf-8',
               str(source), '/Fo' + str(probe / 'probe.obj')]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (probe / 'compiler-output.bin').write_bytes(result.stdout)
    if result.returncode:
        raise ValueError('MSVC prefix probe failed: ' + result.stdout.decode('utf-8', 'replace')[-2048:])
    lines = [line for line in result.stdout.splitlines() if b'pulse_prefix_marker.h' in line]
    if len(lines) != 1:
        raise ValueError('one actual showIncludes header line required')
    path = re.search(rb'(?:[A-Za-z]:[\\/]|\\\\)', lines[0])
    if not path:
        raise ValueError('showIncludes absolute header path missing')
    prefix_bytes = lines[0][:path.start()]
    if not 1 <= len(prefix_bytes) <= 128:
        raise ValueError('showIncludes prefix bound')
    prefix = prefix_bytes.decode('utf-8', 'strict')
    changed = []
    for language in ('C', 'CXX'):
        cache = compiler_cache.with_name('CMake' + language + 'Compiler.cmake')
        before = cache.read_bytes()
        text = before.decode('utf-8-sig')
        field = 'set(CMAKE_' + language + '_CL_SHOWINCLUDES_PREFIX '
        replacement = field + '"' + prefix.replace('\\', '\\\\').replace('"', '\\"') + '")'
        found = [line for line in text.splitlines() if line.startswith(field)]
        if len(found) != 1:
            raise ValueError('compiler dependency prefix field missing')
        if found[0] != replacement:
            (probe / (cache.name + '.' + hashlib.sha256(before).hexdigest()[:12] + '.original')).write_bytes(before)
            cache.write_text('\n'.join(replacement if line.startswith(field) else line for line in text.splitlines()) + '\n',
                             encoding='utf-8', newline='\n')
            changed.append(str(cache))
    rules = build / 'CMakeFiles' / 'rules.ninja'
    before = rules.read_bytes()
    text = before.decode('utf-8-sig')
    replacement = 'msvc_deps_prefix = ' + prefix
    found = [line for line in text.splitlines() if line.startswith('msvc_deps_prefix = ')]
    if len(found) != 1:
        raise ValueError('Ninja MSVC prefix field missing')
    if found[0] != replacement:
        (probe / ('rules.ninja.' + hashlib.sha256(before).hexdigest()[:12] + '.original')).write_bytes(before)
        rules.write_text('\n'.join(replacement if line.startswith('msvc_deps_prefix = ') else line for line in text.splitlines()) + '\n',
                         encoding='utf-8', newline='\n')
        changed.append(str(rules))
    invalidated = []
    if changed:
        for target in targets:
            if not re.fullmatch(r'[A-Za-z0-9_]+', target):
                raise ValueError('invalid target name')
            folder = build / 'CMakeFiles' / (target + '.dir')
            for item in folder.rglob('*'):
                if item.is_file() and item.suffix.lower() in ('.obj', '.res', '.pch'):
                    if not item.resolve().is_relative_to(build):
                        raise ValueError('derived object escaped build directory')
                    invalidated.append(str(item))
        if 'pulse' in targets and (build / 'pulse_app_state.pch').is_file():
            invalidated.append(str(build / 'pulse_app_state.pch'))
        for item in invalidated:
            Path(item).unlink()
    report = {'result': 'ok', 'prefix_utf8': prefix, 'prefix_hex': prefix_bytes.hex(),
              'compiler_exit': result.returncode, 'changed': changed, 'invalidated': invalidated}
    (probe / 'last-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: (len(value) if key == 'invalidated' else value) for key, value in report.items()}, ensure_ascii=False))
    return report

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--invalidate-target', action='append', default=[])
    args = parser.parse_args()
    repair(args.build, args.invalidate_target)
