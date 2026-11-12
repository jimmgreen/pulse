"""Unit and targeted MSVC mutation tests for Native SDK ABI freeze invariants."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import check_native_abi as abi


class NativeAbiFreezeInvariantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.baseline = json.loads(abi.BASELINE.read_text(encoding='utf-8'))

    def clone_baseline(self):
        return copy.deepcopy(self.baseline)

    def test_baseline_passes_freeze_invariants(self):
        self.assertTrue(abi.verify_freeze_invariants(self.clone_baseline()))

    def test_abi_major_invariants(self):
        mutated = self.clone_baseline()
        mutated['macros']['PULSE_ABI_VERSION_1'] = ['2u']
        with self.assertRaisesRegex(ValueError, 'PULSE_ABI_VERSION_1'):
            abi.verify_freeze_invariants(mutated)

        mutated = self.clone_baseline()
        mutated['macros']['PULSE_ABI_MAJOR'] = ['2']
        with self.assertRaisesRegex(ValueError, 'PULSE_ABI_MAJOR'):
            abi.verify_freeze_invariants(mutated)

        mutated = self.clone_baseline()
        mutated['macros'].pop('PULSE_ABI_VERSION_1', None)
        mutated['macros'].pop('PULSE_ABI_MAJOR', None)
        with self.assertRaisesRegex(ValueError, 'missing PULSE_ABI_MAJOR'):
            abi.verify_freeze_invariants(mutated)

        valid_with_major = self.clone_baseline()
        valid_with_major['macros']['PULSE_ABI_MAJOR'] = ['1u']
        self.assertTrue(abi.verify_freeze_invariants(valid_with_major))

    def test_core_entry_host_and_plugin_header_invariants(self):
        for struct_name in ('PulseHostV1', 'PulsePluginV1'):
            with self.subTest(struct=struct_name, case='missing'):
                mutated = self.clone_baseline()
                del mutated['types'][struct_name]
                with self.assertRaisesRegex(ValueError, struct_name):
                    abi.verify_freeze_invariants(mutated)

            with self.subTest(struct=struct_name, case='reordered-header-fields'):
                mutated = self.clone_baseline()
                fields = mutated['types'][struct_name]['fields']
                fields[0], fields[1] = fields[1], fields[0]
                with self.assertRaisesRegex(ValueError, struct_name):
                    abi.verify_freeze_invariants(mutated)

            with self.subTest(struct=struct_name, case='size-offset-drift'):
                mutated = self.clone_baseline()
                mutated['measured'][f'field:{struct_name}.size'][0] = '4'
                with self.assertRaisesRegex(ValueError, f'{struct_name}\\.size'):
                    abi.verify_freeze_invariants(mutated)

            with self.subTest(struct=struct_name, case='version-offset-drift'):
                mutated = self.clone_baseline()
                mutated['measured'][f'field:{struct_name}.version'][0] = '8'
                with self.assertRaisesRegex(ValueError, f'{struct_name}\\.version'):
                    abi.verify_freeze_invariants(mutated)

            with self.subTest(struct=struct_name, case='signed-size-type'):
                mutated = self.clone_baseline()
                mutated['measured'][f'field:{struct_name}.size'][3] = 'const char *__cdecl TypeName<int>(void)'
                with self.assertRaisesRegex(ValueError, f'{struct_name}\\.size'):
                    abi.verify_freeze_invariants(mutated)

    def test_string_view_layout_invariants(self):
        mutated = self.clone_baseline()
        mutated['measured']['type:PulseStringView'][0] = '24'
        with self.assertRaisesRegex(ValueError, 'PulseStringView size'):
            abi.verify_freeze_invariants(mutated)

        mutated = self.clone_baseline()
        mutated['measured']['field:PulseStringView.data'][0] = '8'
        with self.assertRaisesRegex(ValueError, 'PulseStringView field layout'):
            abi.verify_freeze_invariants(mutated)

        mutated = self.clone_baseline()
        mutated['measured']['field:PulseStringView.length'][0] = '12'
        with self.assertRaisesRegex(ValueError, 'PulseStringView field layout'):
            abi.verify_freeze_invariants(mutated)

    def test_fileops_v1_to_v4_prefix_compatibility_invariants(self):
        mutated = self.clone_baseline()
        mutated['measured']['type:PulseFileOpsV2'][0] = mutated['measured']['type:PulseFileOpsV1'][0]
        with self.assertRaisesRegex(ValueError, 'PulseFileOps V1 size'):
            abi.verify_freeze_invariants(mutated)

        mutated = self.clone_baseline()
        fields = mutated['types']['PulseFileOpsV3']['fields']
        fields[3], fields[4] = fields[4], fields[3]
        with self.assertRaisesRegex(ValueError, 'not a strict prefix'):
            abi.verify_freeze_invariants(mutated)

        mutated = self.clone_baseline()
        mutated['measured']['field:PulseFileOpsV4.submit_to_directory'][0] = '48'
        with self.assertRaisesRegex(ValueError, 'submit_to_directory'):
            abi.verify_freeze_invariants(mutated)

        mutated = self.clone_baseline()
        mutated['measured']['field:PulseFileOpsV2.submit'][3] = 'const char *__cdecl TypeName<void *>(void)'
        with self.assertRaisesRegex(ValueError, 'field submit layout/type changed'):
            abi.verify_freeze_invariants(mutated)

        mutated = self.clone_baseline()
        del mutated['types']['PulseFileOpsV4']
        with self.assertRaisesRegex(ValueError, 'expected PulseFileOps V1\\.\\.V4'):
            abi.verify_freeze_invariants(mutated)

    def test_generic_multi_version_family_prefix_enforcement(self):
        valid = self.clone_baseline()
        valid['types']['PulseSampleV1'] = {'fields': ['size', 'version', 'context', 'op_a'], 'declarations': []}
        valid['types']['PulseSampleV2'] = {'fields': ['size', 'version', 'context', 'op_a', 'op_b'], 'declarations': []}
        valid['measured']['type:PulseSampleV1'] = ['24', '8', 'struct PulseSampleV1']
        valid['measured']['type:PulseSampleV2'] = ['32', '8', 'struct PulseSampleV2']
        for name in ('PulseSampleV1', 'PulseSampleV2'):
            valid['measured'][f'field:{name}.size'] = ['0', '4', '4', 'unsigned int']
            valid['measured'][f'field:{name}.version'] = ['4', '4', '4', 'unsigned int']
            valid['measured'][f'field:{name}.context'] = ['8', '8', '8', 'void *']
            valid['measured'][f'field:{name}.op_a'] = ['16', '8', '8', 'int (__cdecl *)(void *)']
        valid['measured']['field:PulseSampleV2.op_b'] = ['24', '8', '8', 'int (__cdecl *)(void *, int)']
        self.assertTrue(abi.verify_freeze_invariants(valid))

        broken = copy.deepcopy(valid)
        broken['measured']['field:PulseSampleV2.op_a'] = ['16', '8', '8', 'int (__cdecl *)(const void *)']
        with self.assertRaisesRegex(ValueError, 'PulseSampleV1.*PulseSampleV2'):
            abi.verify_freeze_invariants(broken)

    def test_msvc_header_mutation_probes(self):
        if shutil.which('cl') is None:
            self.skipTest('MSVC cl.exe is not on PATH; skipping live compiler probe mutations')

        scratch_root = abi.ROOT / 'bench_data' / f'tmp_abi_mut_{uuid.uuid4().hex[:12]}'
        scratch_root.mkdir(parents=True, exist_ok=True)
        try:
            cases = [
                ('baseline', 'abi.h', None, None),
                ('field-reorder', 'abi.h', 'uint32_t size;\n    uint32_t version;', 'uint32_t version;\n    uint32_t size;'),
                ('same-size-type', 'abi.h', 'uint32_t length;', 'int32_t length;'),
                ('tail-field', 'abi.h', 'uint32_t length; }', 'uint32_t length; uint32_t tail; }'),
                ('callback-signature', 'abi.h', '*activate)(void* instance)', '*activate)(const void* instance)'),
                ('calling-convention', 'abi.h', '#define PULSE_CALL __cdecl', '#define PULSE_CALL __stdcall'),
                ('enum-value', 'abi.h', 'PULSE_ERROR_ARGUMENT = 1', 'PULSE_ERROR_ARGUMENT = 101'),
                ('new-type', 'abi.h', '#pragma pack(pop)', 'typedef struct PulseAdded { uint32_t value; } PulseAdded;\n#pragma pack(pop)'),
                ('unsupported-union', 'abi.h', '#pragma pack(pop)', 'typedef union PulseAdded { uint32_t value; } PulseAdded;\n#pragma pack(pop)'),
                ('unmeasured-forward', 'abi.h', '#pragma pack(pop)', 'typedef struct PulseAdded PulseAdded;\n#pragma pack(pop)'),
                ('fileops-prefix-break', 'fileops.h',
                 'typedef struct PulseFileOpsV2 {\n    uint32_t size;\n    uint32_t version;\n    void *context;\n    PulseStatus(PULSE_CALL *submit)',
                 'typedef struct PulseFileOpsV2 {\n    uint32_t size;\n    uint32_t version;\n    void *context;\n    void *inserted_breaking_slot;\n    PulseStatus(PULSE_CALL *submit)'),
            ]
            for name, filename, before, after in cases:
                with self.subTest(case=name), tempfile.TemporaryDirectory(prefix=name + '-', dir=scratch_root) as directory:
                    root = Path(directory)
                    include = root / 'include'
                    shutil.copytree(abi.ROOT / 'sdk/native/include', include)
                    header = include / 'pulse' / filename
                    if before:
                        original = header.read_text(encoding='utf-8')
                        self.assertIn(before, original, name)
                        header.write_text(original.replace(before, after, 1), encoding='utf-8')
                    completed = subprocess.run(
                        [sys.executable, str(Path(abi.__file__)), 'check', '--include', str(include), '--build-dir', str(root / 'probe')],
                        capture_output=True,
                        text=True,
                    )
                    output = completed.stdout + completed.stderr
                    if name == 'baseline':
                        self.assertEqual(completed.returncode, 0, output)
                    elif name == 'unsupported-union':
                        self.assertNotEqual(completed.returncode, 0)
                        self.assertIn('Unsupported/duplicate typedef', output)
                    elif name == 'unmeasured-forward':
                        self.assertNotEqual(completed.returncode, 0)
                        self.assertIn('Unmeasured forward declarations', output)
                    elif name in ('field-reorder', 'tail-field', 'fileops-prefix-break'):
                        self.assertNotEqual(completed.returncode, 0)
                        self.assertIn('Freeze invariant violated', output)
                    else:
                        self.assertNotEqual(completed.returncode, 0)
                        self.assertTrue(
                            'ABI snapshot changed' in output or 'Freeze invariant violated' in output,
                            output,
                        )
        finally:
            shutil.rmtree(scratch_root, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
