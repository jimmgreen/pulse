import unittest
from catalog import inventory


class InventoryTests(unittest.TestCase):
    def test_internal_risk_is_not_public_permission(self):
        result = inventory('inline constexpr ActionEntry kActions[] = {\n'
                           '{"nav.open", ActionRisk::View, true},\n};')
        entry = result['capabilities'][0]
        self.assertTrue(entry['riskDependsOnTarget'])
        self.assertEqual(entry['status'], 'internal')
        self.assertIsNone(entry['publicId'])
        self.assertIsNone(result['publicApiVersion'])

    def test_duplicate_ids_rejected(self):
        with self.assertRaises(ValueError):
            inventory('kActions[] = {\n{"x", ActionRisk::Read},\n'
                      '{"x", ActionRisk::View},\n};')

    def test_unknown_syntax_cannot_silently_drop_actions(self):
        with self.assertRaises(ValueError):
            inventory('kActions[] = {\n{"x", ActionRisk::Read},\n'
                      '{"y", ActionRisk::View, DynamicFlag},\n};')

    def test_missing_inventory_rejected(self):
        with self.assertRaises(ValueError):
            inventory('')


if __name__ == '__main__':
    unittest.main()
