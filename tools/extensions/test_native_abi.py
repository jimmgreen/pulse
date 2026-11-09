"""Targeted mutation tests; run from an x64 MSVC developer environment."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import check_native_abi as abi


def main():
    build = abi.ROOT / 'build-extensions/p32-abi'
    build.mkdir(parents=True, exist_ok=True)
    cases = [
        ('baseline', None, None),
        ('field-reorder', 'uint32_t size;\n    uint32_t version;', 'uint32_t version;\n    uint32_t size;'),
        ('same-size-type', 'uint32_t length;', 'int32_t length;'),
        ('tail-field', 'uint32_t length; }', 'uint32_t length; uint32_t tail; }'),
        ('callback-signature', '*activate)(void* instance)', '*activate)(const void* instance)'),
        ('calling-convention', '#define PULSE_CALL __cdecl', '#define PULSE_CALL __stdcall'),
        ('enum-value', 'PULSE_ERROR_ARGUMENT = 1', 'PULSE_ERROR_ARGUMENT = 101'),
        ('new-type', '#pragma pack(pop)', 'typedef struct PulseAdded { uint32_t value; } PulseAdded;\n#pragma pack(pop)'),
        ('unsupported-union', '#pragma pack(pop)', 'typedef union PulseAdded { uint32_t value; } PulseAdded;\n#pragma pack(pop)'),
        ('unmeasured-forward', '#pragma pack(pop)', 'typedef struct PulseAdded PulseAdded;\n#pragma pack(pop)'),
    ]
    results = []
    for name, before, after in cases:
        with tempfile.TemporaryDirectory(prefix=name + '-', dir=build) as directory:
            root = Path(directory)
            include = root / 'include'
            shutil.copytree(abi.ROOT / 'sdk/native/include', include)
            header = include / 'pulse/abi.h'
            if before:
                original = header.read_text(encoding='utf-8')
                assert before in original, name
                header.write_text(original.replace(before, after, 1), encoding='utf-8')
            completed = subprocess.run([sys.executable, str(Path(abi.__file__)), 'check', '--include', str(include), '--build-dir', str(root / 'probe')], capture_output=True, text=True)
            output = completed.stdout + completed.stderr
            passed = completed.returncode == 0 if name == 'baseline' else completed.returncode != 0 and ('ABI snapshot changed' in output or (name == 'unsupported-union' and 'Unsupported/duplicate typedef' in output) or (name == 'unmeasured-forward' and 'Unmeasured forward declarations' in output))
            (build / (name + '.log')).write_text(output, encoding='utf-8')
            results.append({'case': name, 'passed': passed, 'exitCode': completed.returncode})
            print(('[PASS] ' if passed else '[FAIL] ') + name, flush=True)
    (build / 'mutation-report.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    return 0 if all(row['passed'] for row in results) else 1


if __name__ == '__main__':
    sys.exit(main())
