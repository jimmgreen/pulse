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


def _require_measured_type(measured, type_name):
    entry = measured.get(f'type:{type_name}')
    if not isinstance(entry, list) or len(entry) != 3:
        raise ValueError(f'Freeze invariant violated: missing measured type {type_name}')
    return int(entry[0]), int(entry[1]), entry[2]


def _require_measured_field(measured, type_name, field_name):
    entry = measured.get(f'field:{type_name}.{field_name}')
    if not isinstance(entry, list) or len(entry) != 4:
        raise ValueError(f'Freeze invariant violated: missing measured field {type_name}.{field_name}')
    return int(entry[0]), int(entry[1]), int(entry[2]), entry[3]


def _is_abi_major_one(raw):
    if isinstance(raw, list):
        return len(raw) == 1 and raw[0] in ('1', '1u', '1U')
    if isinstance(raw, str):
        return raw in ('1', '1u', '1U')
    return False


def verify_freeze_invariants(data):
    if not isinstance(data, dict):
        raise ValueError('Freeze invariant violated: snapshot must be a JSON object')
    types = data.get('types')
    measured = data.get('measured')
    macros = data.get('macros', {})
    constants = data.get('constants', {})
    if not isinstance(types, dict) or not isinstance(measured, dict):
        raise ValueError('Freeze invariant violated: snapshot missing types or measured layout')

    abi_keys_found = 0
    for key in ('PULSE_ABI_MAJOR', 'PULSE_ABI_VERSION_1'):
        if key in macros:
            abi_keys_found += 1
            if not _is_abi_major_one(macros[key]):
                raise ValueError(f'Freeze invariant violated: macro {key} must equal 1, got {macros[key]!r}')
        if key in constants:
            abi_keys_found += 1
            if not _is_abi_major_one(constants[key]):
                raise ValueError(f'Freeze invariant violated: constant {key} must equal 1, got {constants[key]!r}')
    if abi_keys_found == 0:
        raise ValueError('Freeze invariant violated: missing PULSE_ABI_MAJOR / PULSE_ABI_VERSION_1 == 1')

    for core_name, min_size, expected_prefix in (
        ('PulseHostV1', 24, ['size', 'version', 'context', 'query_interface']),
        ('PulsePluginV1', 40, ['size', 'version', 'instance', 'activate', 'deactivate', 'destroy']),
    ):
        type_info = types.get(core_name)
        if not isinstance(type_info, dict) or not isinstance(type_info.get('fields'), list):
            raise ValueError(f'Freeze invariant violated: missing struct {core_name}')
        fields = type_info['fields']
        if fields[:len(expected_prefix)] != expected_prefix:
            raise ValueError(
                f'Freeze invariant violated: {core_name} fields {fields} do not start with {expected_prefix}'
            )
        total_size, align, _ = _require_measured_type(measured, core_name)
        if total_size < min_size or align != 8:
            raise ValueError(
                f'Freeze invariant violated: {core_name} layout size={total_size} align={align} (expected >={min_size}, align=8)'
            )
        size_off, size_bytes, _, size_type = _require_measured_field(measured, core_name, 'size')
        ver_off, ver_bytes, _, ver_type = _require_measured_field(measured, core_name, 'version')
        if (size_off, size_bytes) != (0, 4) or 'unsigned int' not in size_type:
            raise ValueError(f'Freeze invariant violated: {core_name}.size must be uint32 at offset 0')
        if (ver_off, ver_bytes) != (4, 4) or 'unsigned int' not in ver_type:
            raise ValueError(f'Freeze invariant violated: {core_name}.version must be uint32 at offset 4')

    sv_info = types.get('PulseStringView')
    if not isinstance(sv_info, dict) or sv_info.get('fields') != ['data', 'length']:
        raise ValueError('Freeze invariant violated: PulseStringView must have fields [data, length]')
    sv_size, sv_align, _ = _require_measured_type(measured, 'PulseStringView')
    if (sv_size, sv_align) != (16, 8):
        raise ValueError(f'Freeze invariant violated: PulseStringView size={sv_size} align={sv_align} (expected 16, 8)')
    data_off, data_bytes, _, _ = _require_measured_field(measured, 'PulseStringView', 'data')
    len_off, len_bytes, _, _ = _require_measured_field(measured, 'PulseStringView', 'length')
    if (data_off, data_bytes) != (0, 8) or (len_off, len_bytes) != (8, 4):
        raise ValueError(
            f'Freeze invariant violated: PulseStringView field layout data=({data_off},{data_bytes}) length=({len_off},{len_bytes})'
        )

    families = {}
    for type_name, type_info in types.items():
        if not isinstance(type_info, dict) or 'fields' not in type_info:
            continue
        match = re.fullmatch(r'(Pulse\w+)V(\d+)', type_name)
        if match:
            families.setdefault(match.group(1), []).append((int(match.group(2)), type_name))

    fileops_versions = sorted(families.get('PulseFileOps', []))
    if [ver for ver, _ in fileops_versions] != [1, 2, 3, 4]:
        raise ValueError(
            f'Freeze invariant violated: expected PulseFileOps V1..V4 family, found {fileops_versions}'
        )

    for family_name, members in sorted(families.items()):
        if len(members) < 2:
            continue
        ordered = sorted(members, key=lambda item: item[0])
        for ver_num, struct_name in ordered:
            fields = types[struct_name]['fields']
            if len(fields) < 2 or fields[0] != 'size' or fields[1] != 'version':
                raise ValueError(f'Freeze invariant violated: {struct_name} must start with size and version')
            s_off, s_sz, _, _ = _require_measured_field(measured, struct_name, 'size')
            v_off, v_sz, _, _ = _require_measured_field(measured, struct_name, 'version')
            if (s_off, s_sz) != (0, 4) or (v_off, v_sz) != (4, 4):
                raise ValueError(f'Freeze invariant violated: {struct_name} size/version header layout mismatch')
        for idx in range(len(ordered) - 1):
            prev_ver, prev_name = ordered[idx]
            next_ver, next_name = ordered[idx + 1]
            prev_size, prev_align, _ = _require_measured_type(measured, prev_name)
            next_size, next_align, _ = _require_measured_type(measured, next_name)
            if not (prev_size < next_size):
                raise ValueError(
                    f'Freeze invariant violated: {family_name} V{prev_ver} size ({prev_size}) must be strictly < V{next_ver} size ({next_size})'
                )
            if prev_align != next_align:
                raise ValueError(
                    f'Freeze invariant violated: {family_name} alignment changed between V{prev_ver} ({prev_align}) and V{next_ver} ({next_align})'
                )
            prev_fields = types[prev_name]['fields']
            next_fields = types[next_name]['fields']
            if len(prev_fields) >= len(next_fields) or next_fields[:len(prev_fields)] != prev_fields:
                raise ValueError(
                    f'Freeze invariant violated: {prev_name} fields {prev_fields} are not a strict prefix of {next_name} fields {next_fields}'
                )
            for field_name in prev_fields:
                prev_m = _require_measured_field(measured, prev_name, field_name)
                next_m = _require_measured_field(measured, next_name, field_name)
                if prev_m != next_m:
                    raise ValueError(
                        f'Freeze invariant violated: field {field_name} layout/type changed from {prev_name} {prev_m} to {next_name} {next_m}'
                    )
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['check', 'generate'])
    parser.add_argument('--include', type=Path, default=ROOT / 'sdk/native/include')
    parser.add_argument('--baseline', type=Path, default=BASELINE)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-extensions/p32-abi')
    args = parser.parse_args()
    try:
        data = snapshot(args.include.resolve(), args.build_dir.resolve())
        verify_freeze_invariants(data)
        rendered = json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + '\n'
        if args.action == 'generate':
            args.baseline.parent.mkdir(parents=True, exist_ok=True)
            args.baseline.write_text(rendered, encoding='utf-8')
            print('[PASS] Explicitly generated preview ABI snapshot:', args.baseline)
        else:
            expected = args.baseline.read_text(encoding='utf-8')
            expected_data = json.loads(expected)
            verify_freeze_invariants(expected_data)
            if expected_data != data:
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
