import json
import re
import unittest
from generate_sdk import generate, describe, read_json, SCHEMA, HEADER, TYPES, API_HEADER, TEMPLATE_HEADER
import contract_compat
import template_catalog


class GenerationTests(unittest.TestCase):
    def test_native_manifest_contract(self):
        schema = read_json(SCHEMA)
        self.assertEqual(set(schema["properties"]), {"apiVersion", "id", "name", "resources", "native"})
        self.assertEqual(schema["required"], ["apiVersion", "id", "name"])
        self.assertFalse(schema["additionalProperties"])
        outputs = generate(schema)
        for field in ("commands", "toolbar", "palette", "workflows", "runtime", "panel"):
            self.assertNotIn(field + "?:", outputs[TYPES])
        self.assertIn("native?: NativePackage", outputs[TYPES])
        # Native header contracts are embedded for host/author compatibility checks.
        # Bound the discovery envelope separately from those public contracts.
        chunks = re.findall(r'R"pulse_api\((.*?)\)pulse_api"', outputs[API_HEADER], re.DOTALL)
        self.assertTrue(chunks)
        self.assertTrue(all(len(chunk) <= 8000 for chunk in chunks))
        embedded = json.loads(''.join(chunks))
        from generate_sdk import ROOT
        self.assertEqual(embedded['nativeSdk'].pop('catalog'),
                         read_json(ROOT / 'sdk/native/api-catalog.json'))
        self.assertLess(len(json.dumps(embedded)), 20000)
        for path, text in outputs.items():
            self.assertEqual(path.read_text(encoding="utf-8"), text)

    def test_discovery_is_native_and_read_only(self):
        api = describe(read_json(SCHEMA))
        self.assertEqual(contract_compat.validate(api), api)
        self.assertEqual(api["capabilities"], [])
        self.assertEqual(api["bindings"], [])
        self.assertEqual(api["nativeSdk"]["catalogPath"], "sdk/native/api-catalog.json")
        self.assertEqual(api["nativeSdk"]["catalog"]["sdkPreview"], 3)
        self.assertTrue(any(item["name"] == "pulse.commands" for item in api["nativeSdk"]["catalog"]["interfaces"]))
        self.assertEqual(api["templates"], list(template_catalog.names()))

    def test_resource_declarations_are_installation_disclosure(self):
        from generate_sdk import ROOT
        catalog = read_json(ROOT / "sdk/native/api-catalog.json")
        self.assertNotIn("resourcePermissions", catalog)
        self.assertEqual(catalog["resourceDeclarations"], read_json(SCHEMA)["properties"]["resources"])
        self.assertIn("Installation disclosure only", catalog["resourceDeclarationPolicy"])
        for interface in catalog["interfaces"]:
            self.assertNotIn("permissions", interface)
            for operation in interface.get("operations", []):
                if isinstance(operation, dict):
                    self.assertNotIn("permission", operation)
        metadata = read_json(ROOT / "schemas/extensions/api_metadata.json")
        operations = metadata["resourceApi"]["operations"]
        requests = read_json(ROOT / "schemas/extensions/resource-request.schema.json")
        self.assertEqual([entry["op"] for entry in operations],
                         [entry["properties"]["op"]["const"] for entry in requests["oneOf"]])
        self.assertTrue(all("resourceDeclaration" in entry and "permission" not in entry for entry in operations))
        types = generate(read_json(SCHEMA))[TYPES]
        self.assertIn("ResourceDeclaration", types)
        self.assertNotIn("ResourcePermission", types)

    def test_native_templates_have_no_legacy_fields(self):
        allowed = set(read_json(SCHEMA)["properties"])
        templates = template_catalog.all_templates()
        self.assertEqual(len(templates), 8)
        for value in templates.values():
            self.assertFalse(set(value) - allowed)

    def test_rejects_reintroduced_legacy_schema(self):
        schema = read_json(SCHEMA)
        schema["properties"]["commands"] = {}
        with self.assertRaises(ValueError):
            generate(schema)


if __name__ == "__main__":
    unittest.main()
