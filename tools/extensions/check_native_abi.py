"""Fail-closed public C ABI snapshot checker for Windows x64 Microsoft MSVC."""
import argparse
import difflib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BASELINE = ROOT / 'sdk/native/abi/windows-x64-msvc-preview3.json'


def normalized(text):
    return ' '.join(re.findall(r'\w+|[^\w\s]', text))


def inventory(include):
    data = {'headers': [], 'types': {}, 'constants': {}, 'macros': {}}
    forwards = set()
    for path in sorted((include / 'pulse').glob('*.h')):
        data['headers'].append(path.name)
        source = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(encoding='utf-8'), flags=re.S)
        source = source.replace('\\\n', '')
        for line in source.splitlines():
            match = re.match(r'\s*#define\s+(PULSE_\w+)(.*)', line)
            if match and match[2].strip():
                data['macros'].setdefault(match[1], []).append(normalized(match[2]))
        source = re.sub(r'^\s*#.*$', '', source, flags=re.M)
        source = source.replace('extern "C" {', '')
        source = re.sub(r'^\s*}\s*$', '', source, flags=re.M)

        def structure(match):
            name, body, alias = match.groups()
            if alias and alias != name:
                raise ValueError('Unsupported struct alias: ' + name)
            fields, declarations = [], []
            for declaration in body.split(';'):
                declaration = declaration.strip()
                if not declaration:
                    continue
                declarations.append(normalized(declaration))
                callback = re.fullmatch(r'.+?\(\s*PULSE_CALL\s*\*\s*(\w+)\s*\)\s*\([^;]*\)', declaration, re.S)
                if callback:
                    fields.append(callback[1])
                else:
                    simple = re.fullmatch(r'(?:const\s+)?(?:struct\s+)?\w+(?=\s|\*)\s*(.+)', declaration, re.S)
                    if not simple:
                        raise ValueError('Unsupported field: ' + declaration)
                    for field in simple[1].split(','):
                        member = re.fullmatch(r'\s*\**\s*(\w+)\s*', field)
                        if not member:
                            raise ValueError('Unsupported field: ' + declaration)
                        fields.append(member[1])
            if name in data['types'] or len(fields) != len(set(fields)):
                raise ValueError('Duplicate structure/field: ' + name)
            data['types'][name] = {'fields': fields, 'declarations': declarations}
            return ''

        source = re.sub(r'(?:typedef\s+)?struct\s+(Pulse\w+)\s*\{([^{}]*)\}\s*(Pulse\w+)?\s*;', structure, source, flags=re.S)
        forward_pattern = r'typedef\s+struct\s+(Pulse\w+)\s+\1\s*;'
        forwards.update(re.findall(forward_pattern, source))
        source = re.sub(forward_pattern, '', source)

        def enumeration(match):
            for item in match[1].split(','):
                if not item.strip():
                    continue
                constant = re.fullmatch(r'\s*(PULSE_\w+)\s*=\s*(.+?)\s*', item, re.S)
                if not constant or constant[1] in data['constants']:
                    raise ValueError('Unsupported/duplicate enum: ' + item)
                data['constants'][constant[1]] = normalized(constant[2])
            return ''

        source = re.sub(r'enum\s*\{([^{}]*)\}\s*;', enumeration, source, flags=re.S)

        def alias(match):
            declaration = match[1].strip()
            callback = re.search(r'\(\s*PULSE_CALL\s*\*\s*(Pulse\w+)\s*\)', declaration)
            simple = re.fullmatch(r'\w+\s+(Pulse\w+)', declaration)
            name = callback[1] if callback else simple[1] if simple else None
            if not name or name in data['types']:
                raise ValueError('Unsupported/duplicate typedef: ' + declaration)
            data['types'][name] = {'declaration': normalized(declaration)}
            return ''

        source = re.sub(r'typedef\s+([^;]+);', alias, source, flags=re.S)
        if source.strip():
            raise ValueError(f'Uninventoried public declaration in {path.name}: {source.strip()[:160]}')
    if not data['types']:
        raise ValueError('No public C types discovered')
    if forwards - data['types'].keys():
        raise ValueError('Unmeasured forward declarations: ' + ', '.join(sorted(forwards - data['types'].keys())))
    return data


def snapshot(include, build):
    data = inventory(include)
    build.mkdir(parents=True, exist_ok=True)
    source = ['#include <cstdio>', '#include <cstddef>', '#include <type_traits>',
              '#if !defined(_MSC_VER) || !defined(_M_X64) || defined(__clang__)',
              '#error This baseline requires Windows x64 Microsoft MSVC', '#endif',
              '#pragma pack(push, 1)']
    source += [f'#include <pulse/{header}>' for header in data['headers']]
    source += ['struct PackingSentinel { char byte; unsigned long long value; };',
               'static_assert(offsetof(PackingSentinel, value) == 1);', '#pragma pack(pop)',
               'template<class T> const char* TypeName() { return __FUNCSIG__; }', 'int main() {',
               'printf("compiler\\t%d\\n", _MSC_VER);']
    for name, entry in data['types'].items():
        source.append(f'printf("type:{name}\\t%zu\\t%zu\\t%s\\n", sizeof({name}), alignof({name}), TypeName<{name}>());')
        for field in entry.get('fields', []):
            source.append(f'static_assert(std::is_standard_layout_v<{name}>);')
            source.append(f'printf("field:{name}.{field}\\t%zu\\t%zu\\t%zu\\t%s\\n", offsetof({name}, {field}), sizeof(decltype({name}::{field})), alignof(decltype({name}::{field})), TypeName<decltype({name}::{field})>());')
    for name in data['constants']:
        source.append(f'printf("constant:{name}\\t%lld\\n", static_cast<long long>({name}));')
    source += ['}']
    cpp, exe = build / 'abi_probe.cpp', build / 'abi_probe.exe'
    cpp.write_text('\n'.join(source), encoding='utf-8')
    command = ['cl', '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/permissive-', '/utf-8', '/I' + str(include), str(cpp), '/Fe:' + str(exe), '/Fo:' + str(build / 'abi_probe.obj')]
    completed = subprocess.run(command, cwd=build, capture_output=True, text=True)
    (build / 'compile.log').write_text(completed.stdout + completed.stderr, encoding='utf-8')
    if completed.returncode:
        raise ValueError('MSVC probe failed; see ' + str(build / 'compile.log'))
    run = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    measured = {}
    for line in run.stdout.splitlines():
        key, *values = line.split('\t')
        measured[key] = values
    compiler = measured.pop('compiler')
    (build / 'compiler.json').write_text(json.dumps({'mscVer': compiler[0]}), encoding='utf-8')
    data.update(measured=measured, format=1, target='windows-x64-msvc-pack8', status='preview-snapshot-not-1.0-freeze')
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['check', 'generate'])
    parser.add_argument('--include', type=Path, default=ROOT / 'sdk/native/include')
    parser.add_argument('--baseline', type=Path, default=BASELINE)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-extensions/p32-abi')
    args = parser.parse_args()
    try:
        data = snapshot(args.include.resolve(), args.build_dir.resolve())
        rendered = json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + '\n'
        if args.action == 'generate':
            args.baseline.parent.mkdir(parents=True, exist_ok=True)
            args.baseline.write_text(rendered, encoding='utf-8')
            print('[PASS] Explicitly generated preview ABI snapshot:', args.baseline)
        else:
            expected = args.baseline.read_text(encoding='utf-8')
            if json.loads(expected) != data:
                diff = list(difflib.unified_diff(expected.splitlines(), rendered.splitlines(), fromfile='baseline', tofile='current', lineterm=''))
                (args.build_dir / 'abi.diff').write_text('\n'.join(diff), encoding='utf-8')
                print('\n'.join(diff[:100]))
                raise ValueError('ABI snapshot changed; review complete diff at ' + str(args.build_dir / 'abi.diff'))
            print(f'[PASS] ABI matches: {len(data["types"])} public types, {len(data["measured"])} measured entries')
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print('[FAIL]', error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
