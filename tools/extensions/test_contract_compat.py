import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import contract_compat as contract
import generate_sdk
import pulse_ext


class ContractComparisonTests(unittest.TestCase):
    def setUp(self):
        self.api = generate_sdk.describe(generate_sdk.read_json(generate_sdk.SCHEMA),
                                         generate_sdk.read_json(generate_sdk.METADATA))

    def changed(self):
        result = copy.deepcopy(self.api)
        result.pop('contractIdentity')
        return result

    def test_generated_identity_validates_and_legacy_without_identity_matches(self):
        contract.validate(self.api)
        result = contract.compare(self.changed(), self.api)
        self.assertTrue(result['identical'])
        self.assertEqual(result['changes'], [])
        self.assertFalse(result['runtimeCompatibilityCertified'])
        self.assertFalse(result['grantsAuthority'])
        self.assertTrue(result['nativeManifestValidationRequired'])

    def test_dictionary_order_does_not_change_identity(self):
        reordered = dict(reversed(list(self.api.items())))
        self.assertEqual(contract.fingerprint(self.api), contract.fingerprint(reordered))

    def test_same_version_with_removed_capability_requires_review(self):
        modified = self.changed()
        modified['capabilities'].remove('workflow.run')
        result = contract.compare(self.api, modified)
        self.assertEqual(result['status'], 'review-required')
        self.assertEqual(result['capabilities']['removed'], ['workflow.run'])
        self.assertEqual(result['baselineApiVersion'], result['currentApiVersion'])

    def test_changed_resource_result_and_budget_are_not_hidden_by_same_version(self):
        modified = self.changed()
        modified['resourceApi']['limits']['auditOwnRecords'] = 1
        modified['resourceApi']['responseSchema']['additionalProperties'] = False
        paths = {item['path'] for item in contract.compare(self.api, modified)['changes']}
        self.assertIn('/resourceApi/limits/auditOwnRecords', paths)
        self.assertIn('/resourceApi/responseSchema/additionalProperties', paths)

    def test_identity_is_not_trusted_if_content_tampered(self):
        self.api['limits']['commands'] += 1
        with self.assertRaisesRegex(ValueError, 'identity'):
            contract.validate(self.api)

    def test_added_and_removed_properties_report_json_pointer_without_values(self):
        modified = self.changed()
        modified['x/private~field'] = 'do not echo request contents'
        del modified['developmentNotes']
        result = contract.compare(self.api, modified)
        self.assertIn({'path': '/x~1private~0field', 'kind': 'added'}, result['changes'])
        self.assertIn({'path': '/developmentNotes', 'kind': 'removed'}, result['changes'])
        self.assertNotIn('do not echo', json.dumps(result))

    def test_duplicate_nonfinite_wrong_envelope_and_bad_utf8_rejected(self):
        for data in (b'{"a":1,"a":2}', b'{"a":NaN}', b'{"api":{}}', b'\xff'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                contract.load(data)

    def test_byte_and_depth_budgets(self):
        with self.assertRaises(ValueError):
            contract.load(b' ' * (contract.MAX_BYTES + 1))
        modified = self.changed()
        nested = {}
        modified['nested'] = nested
        for _ in range(65):
            nested['next'] = {}
            nested = nested['next']
        with self.assertRaisesRegex(ValueError, 'structural'):
            contract.validate(modified)

    def test_change_budget_is_explicit_and_not_same(self):
        modified = self.changed()
        modified.update({str(i): i for i in range(300)})
        result = contract.compare(self.api, modified)
        self.assertEqual(len(result['changes']), contract.MAX_CHANGES)
        self.assertTrue(result['changesTruncated'])
        self.assertFalse(result['identical'])

    def test_cli_analysis_is_read_only_and_require_identical_fails_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            baseline = Path(folder) / 'host-api.json'
            data = json.dumps(self.api).encode()
            baseline.write_bytes(data)
            for changed, expected in ((False, 0), (True, 2)):
                current = self.changed()
                if changed:
                    current['capabilities'].pop()
                output = io.StringIO()
                with patch.object(pulse_ext, 'host_call', return_value=current) as host, contextlib.redirect_stdout(output):
                    code = pulse_ext.main(['--json', 'compare-sdk', str(baseline), '--require-identical'])
                self.assertEqual(code, expected)
                self.assertEqual(host.call_count, 1)
                self.assertEqual(host.call_args.args[1], '--extension-api')
                self.assertEqual(baseline.read_bytes(), data)
                result = json.loads(output.getvalue())
                comparison = result['comparison'] if not changed else result['error']['comparison']
                self.assertEqual(comparison['commandsExecuted'], 0)


if __name__ == '__main__':
    unittest.main()
