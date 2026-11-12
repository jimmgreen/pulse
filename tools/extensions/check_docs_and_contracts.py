"""Verify zero drift across Native SDK headers, catalogs, schemas, templates, and docs."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]

EXPECTED_RESOURCES = [
    "context.read",
    "files.readCurrentDirectory",
    "operations.own",
    "settings.own",
    "audit.own",
    "files.renameCurrentDirectory",
    "files.readDirectory",
    "files.watchDirectory",
    "files.open",
    "context.events",
    "files.transferCurrentSelection",
    "files.deleteCurrentSelection",
    "files.transferToDirectory",
    "files.transferFromDirectory",
    "files.transferDirectoryTree",
]

EXPECTED_TEMPLATES = (
    "directory-browser",
    "directory-transfer",
    "directory-workbench",
    "file-operations",
    "inspector",
    "live-data",
    "reactive-panel",
    "rename-panel",
)

CORE_DOCS = (
    "docs/extensions-sdk-quickstart.md",
    "docs/native-packages.md",
    "tools/extensions/README.md",
    "sdk/native/README.md",
)

LIFECYCLE_DOC = "docs/extensions-sdk-lifecycle-and-support.md"

LIFECYCLE_LINKING_DOCS = (
    "sdk/native/README.md",
    "docs/native-packages.md",
    "docs/extensions-sdk-quickstart.md",
)

REQUIRED_LIFECYCLE_SECTIONS = (
    "## 1. Stability Tiers & 1.0 Freeze Matrix",
    "## 2. Binary & Semantic Compatibility Contract",
    "## 3. Support Window & Deprecation Policy",
    "## 4. Migration Playbooks",
    "## 5. Rollback, Upgrade & Failure Recovery",
)

EXPECTED_FROZEN_INTERFACES = (
    "pulse.files",
    "pulse.resources",
    "pulse.dispatch",
    "pulse.directories",
    "pulse.context",
    "pulse.fileops",
    "pulse.commands",
    "pulse.preview",
    "pulse.tasks",
    "pulse.staging",
    "pulse.settings.pages",
    "pulse.dialogs",
)

EXPECTED_EXPERIMENTAL_INTERFACES = (
    "pulse.ui.model",
    "pulse.ui.events",
    "pulse.ui.data",
    "pulse.ui.mount",
    "pulse.icons",
    "pulse.diagnostics",
    "pulse.pickers",
    "pulse.workbench",
)

EXPECTED_STABILITY_TIERS = {
    **{name: "frozen" for name in EXPECTED_FROZEN_INTERFACES},
    **{name: "experimental" for name in EXPECTED_EXPERIMENTAL_INTERFACES},
}

REQUIRED_INTERFACES = (
    "pulse.ui.model",
    "pulse.ui.data",
    "pulse.ui.events",
    "pulse.ui.mount",
    "pulse.icons",
    "pulse.diagnostics",
    "pulse.pickers",
    "pulse.workbench",
    "pulse.files",
    "pulse.fileops",
    "pulse.directories",
    "pulse.context",
    "pulse.resources",
    "pulse.dispatch",
    "pulse.commands",
    "pulse.preview",
    "pulse.tasks",
    "pulse.staging",
    "pulse.settings.pages",
    "pulse.dialogs",
)


def _load_module_from(root, filename, module_name):
    tools_dir = str((root / "tools/extensions").resolve())
    added = False
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
        added = True
    try:
        spec = importlib.util.spec_from_file_location(module_name, root / "tools/extensions" / filename)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        if added and tools_dir in sys.path:
            sys.path.remove(tools_dir)


def _extract_markdown_links(text, doc_rel, errors):
    """Return all inline and reference-resolved Markdown link targets outside code spans/blocks."""
    prose = re.sub(r"```.*?```", "", text, flags=re.S)
    prose = re.sub(r"`[^`\n]+`", "", prose)

    ref_defs = {}
    for def_match in re.finditer(r"^\s*\[([^\]]+)\]:\s*(\S+)", prose, flags=re.M):
        key = def_match.group(1).strip().casefold()
        ref_defs[key] = def_match.group(2).strip()

    targets = [m.group(1).strip() for m in re.finditer(r"\[[^\]]+\]\(([^)]+)\)", prose)]
    for ref_use in re.finditer(r"\[([^\]]+)\]\[([^\]]*)\]", prose):
        label, explicit_ref = ref_use.group(1).strip(), ref_use.group(2).strip()
        ref_key = (explicit_ref or label).casefold()
        if ref_key not in ref_defs:
            errors.append(f"Undefined markdown reference link [{explicit_ref or label}] in {doc_rel}")
        else:
            targets.append(ref_defs[ref_key])
    return targets


def check_all(root=ROOT):
    root = Path(root).resolve()
    errors = []

    # 1. abi.h PULSE_SDK_PREVIEW and PULSE_ABI_VERSION_1
    abi_path = root / "sdk/native/include/pulse/abi.h"
    abi_preview = None
    if not abi_path.is_file():
        errors.append("Missing sdk/native/include/pulse/abi.h")
    else:
        abi_text = abi_path.read_text(encoding="utf-8")
        preview_match = re.search(r"#define\s+PULSE_SDK_PREVIEW\s+(\d+)u?", abi_text)
        abi_ver_match = re.search(r"#define\s+PULSE_ABI_VERSION_1\s+(\d+)u?", abi_text)
        if not preview_match:
            errors.append("abi.h is missing #define PULSE_SDK_PREVIEW")
        else:
            abi_preview = int(preview_match.group(1))
            if abi_preview != 3:
                errors.append(f"Expected PULSE_SDK_PREVIEW == 3 in abi.h, found {abi_preview}")
        if not abi_ver_match or int(abi_ver_match.group(1)) != 1:
            errors.append("Expected PULSE_ABI_VERSION_1 == 1 in abi.h")

    # 2. sdk/native/api-catalog.json (+ stabilityTier / stability / supportPolicy)
    catalog_path = root / "sdk/native/api-catalog.json"
    catalog = None
    interfaces_by_name = {}
    if not catalog_path.is_file():
        errors.append("Missing sdk/native/api-catalog.json")
    else:
        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        if abi_preview is not None and catalog.get("sdkPreview") != abi_preview:
            errors.append(f"api-catalog.json sdkPreview ({catalog.get('sdkPreview')}) != abi.h ({abi_preview})")
        if catalog.get("abiVersion") != 1:
            errors.append(f"api-catalog.json abiVersion ({catalog.get('abiVersion')}) != 1")
        interfaces_by_name = {
            item["name"]: item
            for item in catalog.get("interfaces", [])
            if isinstance(item, dict) and isinstance(item.get("name"), str)
        }
        for required_iface in REQUIRED_INTERFACES:
            if required_iface not in interfaces_by_name:
                errors.append(f"api-catalog.json missing required interface: {required_iface}")
        if set(interfaces_by_name) != set(EXPECTED_STABILITY_TIERS):
            errors.append(
                f"api-catalog.json interface set {sorted(interfaces_by_name)} != {sorted(EXPECTED_STABILITY_TIERS)}"
            )
        for iface_name, iface in interfaces_by_name.items():
            tier = iface.get("stabilityTier")
            if tier not in {"frozen", "experimental"}:
                errors.append(f"Interface {iface_name} has invalid stabilityTier: {tier!r}")
            elif iface_name in EXPECTED_STABILITY_TIERS and tier != EXPECTED_STABILITY_TIERS[iface_name]:
                errors.append(
                    f"Interface {iface_name} stabilityTier {tier!r} != expected {EXPECTED_STABILITY_TIERS[iface_name]!r}"
                )
            if "headerContract" in iface:
                rel_header = "sdk/native/" + iface["header"]
                header_file = root / rel_header
                if not header_file.is_file():
                    errors.append(f"Missing public header for {iface_name}: {rel_header}")
                else:
                    lf_bytes = header_file.read_bytes().replace(b"\r\n", b"\n")
                    actual_sha = hashlib.sha256(lf_bytes).hexdigest()
                    if actual_sha != iface["headerContract"].get("sha256"):
                        errors.append(f"Header SHA-256 drift for {iface_name} ({rel_header})")
                    if lf_bytes.decode("utf-8") != iface["headerContract"].get("text"):
                        errors.append(f"Header text drift for {iface_name} ({rel_header})")

        stability = catalog.get("stability")
        if not isinstance(stability, dict):
            errors.append("api-catalog.json is missing top-level 'stability' metadata block")
        else:
            core_entry = stability.get("coreEntryAbi", {})
            tiers_meta = stability.get("tiers", {})
            frozen_meta = tiers_meta.get("frozen", {})
            experimental_meta = tiers_meta.get("experimental", {})
            if stability.get("abiMajor") != 1:
                errors.append(f"api-catalog.json stability.abiMajor ({stability.get('abiMajor')}) != 1")
            if core_entry.get("stabilityTier") != "frozen":
                errors.append("api-catalog.json stability.coreEntryAbi.stabilityTier must be 'frozen'")
            if (
                frozen_meta.get("count") != len(EXPECTED_FROZEN_INTERFACES)
                or tuple(frozen_meta.get("interfaces", ())) != EXPECTED_FROZEN_INTERFACES
            ):
                errors.append("api-catalog.json stability.tiers.frozen does not match expected 12 frozen interfaces")
            if (
                experimental_meta.get("count") != len(EXPECTED_EXPERIMENTAL_INTERFACES)
                or tuple(experimental_meta.get("interfaces", ())) != EXPECTED_EXPERIMENTAL_INTERFACES
            ):
                errors.append(
                    "api-catalog.json stability.tiers.experimental does not match expected 8 experimental interfaces"
                )

        support_policy = catalog.get("supportPolicy")
        if not isinstance(support_policy, dict):
            errors.append("api-catalog.json is missing top-level 'supportPolicy' metadata block")
        else:
            dep_win = support_policy.get("deprecationWindow", {})
            revocation = support_policy.get("emergencyRevocation", {})
            if support_policy.get("abiMajor") != 1:
                errors.append(f"api-catalog.json supportPolicy.abiMajor ({support_policy.get('abiMajor')}) != 1")
            if dep_win.get("minMinorReleases", 0) < 2 or dep_win.get("minDays", 0) < 180:
                errors.append("api-catalog.json supportPolicy.deprecationWindow must require >=2 minor releases and >=180 days")
            if support_policy.get("policyDocument") != LIFECYCLE_DOC:
                errors.append(f"api-catalog.json supportPolicy.policyDocument must be {LIFECYCLE_DOC!r}")
            channels = set(revocation.get("channels", ()))
            if not {"revokedPackages", "revokedRevisions"}.issubset(channels):
                errors.append("api-catalog.json supportPolicy.emergencyRevocation.channels missing revocation lists")

        api_metadata_path = root / "schemas/extensions/api_metadata.json"
        if api_metadata_path.is_file():
            api_metadata = json.loads(api_metadata_path.read_text(encoding="utf-8"))
            if api_metadata.get("stability") != stability:
                errors.append("schemas/extensions/api_metadata.json stability differs from sdk/native/api-catalog.json")
            if api_metadata.get("supportPolicy") != support_policy:
                errors.append("schemas/extensions/api_metadata.json supportPolicy differs from sdk/native/api-catalog.json")

        if (root / "tools/extensions/native_author_mcp.py").is_file():
            mcp_mod = _load_module_from(root, "native_author_mcp.py", "_check_mcp")
            catalog_tools = catalog.get("authoring", {}).get("tools")
            if catalog_tools != mcp_mod.TOOLS:
                errors.append("api-catalog.json authoring.tools is out of sync with native_author_mcp.TOOLS")

    # 3. schemas/extensions/manifest.schema.json (15 resources + contributes.commands)
    schema_path = root / "schemas/extensions/manifest.schema.json"
    schema = None
    if not schema_path.is_file():
        errors.append("Missing schemas/extensions/manifest.schema.json")
    else:
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        resources_prop = schema.get("properties", {}).get("resources", {})
        resources_enum = resources_prop.get("items", {}).get("enum", [])
        if len(resources_enum) != 15 or resources_enum != EXPECTED_RESOURCES:
            errors.append(f"manifest.schema.json resources enum mismatch: found {len(resources_enum)} items {resources_enum}")
        if catalog is not None and catalog.get("resourceDeclarations") != resources_prop:
            errors.append("api-catalog.json resourceDeclarations differs from manifest.schema.json resources")
        contributes_prop = schema.get("properties", {}).get("contributes")
        contributions_def = schema.get("$defs", {}).get("contributions", {})
        has_commands_schema = isinstance(contributes_prop, dict) and (
            "commands" in contributes_prop.get("properties", {})
            or (
                contributes_prop.get("$ref") == "#/$defs/contributions"
                and "commands" in contributions_def.get("properties", {})
            )
        )
        if not has_commands_schema:
            errors.append("manifest.schema.json is missing contributes.commands definition")

    # 4. examples/native/catalog.json & project_creator alignment
    examples_catalog_path = root / "examples/native/catalog.json"
    packable_templates = ()
    if not examples_catalog_path.is_file():
        errors.append("Missing examples/native/catalog.json")
    else:
        examples_catalog = json.loads(examples_catalog_path.read_text(encoding="utf-8"))
        packable_templates = tuple(examples_catalog.get("templates", []))
        if packable_templates != EXPECTED_TEMPLATES:
            errors.append(f"examples/native/catalog.json templates {packable_templates} != {EXPECTED_TEMPLATES}")
        if "basic" in packable_templates or "dock-panel" in packable_templates:
            errors.append("examples/native/catalog.json must not include basic or removed dock-panel")
        for tpl in packable_templates:
            tpl_dir = root / "examples/native" / tpl
            for required_member in ("CMakeLists.txt", "manifest.json", "plugin.cpp", "README.md"):
                if not (tpl_dir / required_member).is_file():
                    errors.append(f"Template {tpl} is missing {required_member}")
            manifest_file = tpl_dir / "manifest.json"
            if manifest_file.is_file():
                tpl_manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
                unknown_res = [r for r in tpl_manifest.get("resources", []) if r not in EXPECTED_RESOURCES]
                if unknown_res:
                    errors.append(f"Template {tpl} declares unknown resources: {unknown_res}")

        if (root / "tools/extensions/project_creator.py").is_file():
            creator_mod = _load_module_from(root, "project_creator.py", "_check_creator")
            if creator_mod.templates(root) != packable_templates:
                errors.append("project_creator.templates() does not match examples/native/catalog.json")
            if creator_mod.abi_samples(root) != ("basic",):
                errors.append(f"project_creator.abi_samples() expected ('basic',), got {creator_mod.abi_samples(root)}")

    # 5. Optional native-sdk-manifest.json compatibility metadata check (inside extracted SDK bundles)
    sdk_manifest_path = root / "native-sdk-manifest.json"
    is_extracted_sdk_bundle = sdk_manifest_path.is_file()
    if is_extracted_sdk_bundle:
        sdk_manifest = json.loads(sdk_manifest_path.read_text(encoding="utf-8"))
        compat = sdk_manifest.get("compatibility")
        if not isinstance(compat, dict):
            errors.append("native-sdk-manifest.json is missing compatibility metadata")
        else:
            if compat.get("sdkPreview") != abi_preview:
                errors.append(f"native-sdk-manifest.json compatibility.sdkPreview ({compat.get('sdkPreview')}) != {abi_preview}")
            if compat.get("abiVersion") != 1:
                errors.append("native-sdk-manifest.json compatibility.abiVersion != 1")
            if compat.get("architecture") != "windows-x64":
                errors.append("native-sdk-manifest.json compatibility.architecture != 'windows-x64'")
            if compat.get("minWindowsBuild") != 10240:
                errors.append("native-sdk-manifest.json compatibility.minWindowsBuild != 10240")
            if not isinstance(compat.get("pulseVersion"), str) or not compat["pulseVersion"].strip():
                errors.append("native-sdk-manifest.json compatibility.pulseVersion is empty")
            if not isinstance(compat.get("toolchain"), dict):
                errors.append("native-sdk-manifest.json compatibility.toolchain is missing")

    # 6. Core documentation consistency, lifecycle policy & zero stale drift
    stale_preview_patterns = (
        re.compile(r"PULSE_SDK_PREVIEW[^.\n;。；]{0,24}(?:=\s*2\b|为\s*2\b|仍为\s*2\b|is\s+2\b)", re.IGNORECASE),
        re.compile(r"(?:当前|目前)[^。\n]{0,20}(?:预览号|PULSE_SDK_PREVIEW|sdkPreview)\s*(?:为|仍为|=)\s*2\b", re.IGNORECASE),
    )
    valid_template_set = set(packable_templates or EXPECTED_TEMPLATES)
    expected_lifecycle_path = (root / LIFECYCLE_DOC).resolve()

    combined_docs = []
    checked_docs = list(CORE_DOCS)
    for doc_rel in CORE_DOCS:
        doc_path = root / doc_rel
        if not doc_path.is_file():
            errors.append(f"Missing core documentation file: {doc_rel}")
            continue
        text = doc_path.read_text(encoding="utf-8")
        combined_docs.append(text)
        for pattern in stale_preview_patterns:
            match = pattern.search(text)
            if match:
                errors.append(f"Stale SDK preview 2 reference in {doc_rel}: {match.group(0)!r}")
        if re.search(r"--template\s+dock-panel\b", text):
            errors.append(f"Removed '--template dock-panel' reference found in {doc_rel}")
        for tpl_match in re.finditer(r"--template\s+([a-z0-9][a-z0-9_-]*)", text):
            referenced_tpl = tpl_match.group(1)
            if referenced_tpl not in valid_template_set:
                errors.append(f"Unknown template '--template {referenced_tpl}' referenced in {doc_rel}")
        if doc_rel == "tools/extensions/README.md":
            if "isolatedDevReload" in text or re.search(r"\bpulse_ext\.py\b[^\n]*\bdev\b[^\n]*--profile\b", text):
                errors.append("tools/extensions/README.md still recommends legacy isolatedDevReload / 'dev --profile'")
        if "LoadLibraryExW" not in text or "DllMain" not in text:
            errors.append(f"{doc_rel} must explicitly document the LoadLibraryExW / DllMain ABI-only trust boundary")

        raw_targets = _extract_markdown_links(text, doc_rel, errors)
        resolved_targets = set()
        for raw_target in raw_targets:
            if not raw_target or raw_target.startswith(("#", "http://", "https://", "mailto:", "file://")):
                continue
            target_no_frag = raw_target.split("#", 1)[0].strip()
            if not target_no_frag:
                continue
            resolved_link = (doc_path.parent / target_no_frag).resolve()
            resolved_targets.add(resolved_link)
            if not resolved_link.exists():
                if is_extracted_sdk_bundle and resolved_link == expected_lifecycle_path:
                    continue
                errors.append(f"Broken relative markdown link in {doc_rel}: {raw_target}")

        if doc_rel in LIFECYCLE_LINKING_DOCS and expected_lifecycle_path not in resolved_targets:
            errors.append(f"{doc_rel} must cross-link to {LIFECYCLE_DOC}")

    # 7. Canonical lifecycle, compatibility & support policy specification
    lifecycle_path = root / LIFECYCLE_DOC
    if not is_extracted_sdk_bundle or lifecycle_path.is_file():
        checked_docs.append(LIFECYCLE_DOC)
        if not lifecycle_path.is_file():
            errors.append(f"Missing lifecycle & support policy specification: {LIFECYCLE_DOC}")
        else:
            lifecycle_text = lifecycle_path.read_text(encoding="utf-8")
            for section in REQUIRED_LIFECYCLE_SECTIONS:
                if section not in lifecycle_text:
                    errors.append(f"{LIFECYCLE_DOC} is missing required section heading: {section!r}")
            for iface_name in sorted(interfaces_by_name or REQUIRED_INTERFACES):
                if iface_name not in lifecycle_text:
                    errors.append(f"{LIFECYCLE_DOC} does not document catalog interface: {iface_name}")
            for raw_target in _extract_markdown_links(lifecycle_text, LIFECYCLE_DOC, errors):
                if not raw_target or raw_target.startswith(("#", "http://", "https://", "mailto:", "file://")):
                    continue
                target_no_frag = raw_target.split("#", 1)[0].strip()
                if not target_no_frag:
                    continue
                resolved_link = (lifecycle_path.parent / target_no_frag).resolve()
                if not resolved_link.exists():
                    errors.append(f"Broken relative markdown link in {LIFECYCLE_DOC}: {raw_target}")

    joined = "\n".join(combined_docs)
    for required_topic in ("contributes.commands", "pulse.dialogs", "extensions/7zip", "rotate", "gc", "stop-host"):
        if required_topic not in joined:
            errors.append(f"Core docs are missing required topic '{required_topic}'")

    # 8. Agent-first SDK assets & Stage D sandboxed-worker reference sample
    enforce_agent_assets = (
        root == ROOT.resolve()
        or is_extracted_sdk_bundle
        or (root / "sdk/native/samples").is_dir()
    )
    agents_doc_rel = "sdk/native/AGENTS.md"
    agents_doc_path = root / agents_doc_rel
    if enforce_agent_assets or agents_doc_path.is_file():
        checked_docs.append(agents_doc_rel)
        if not agents_doc_path.is_file():
            errors.append(f"Missing agent playbook: {agents_doc_rel}")
        else:
            agents_text = agents_doc_path.read_text(encoding="utf-8")
            for pattern in stale_preview_patterns:
                match = pattern.search(agents_text)
                if match:
                    errors.append(f"Stale SDK preview 2 reference in {agents_doc_rel}: {match.group(0)!r}")
            for tpl_match in re.finditer(r"--template\s+([a-z0-9][a-z0-9_-]*)", agents_text):
                if tpl_match.group(1) not in valid_template_set:
                    errors.append(f"Unknown template '--template {tpl_match.group(1)}' referenced in {agents_doc_rel}")
            for raw_target in _extract_markdown_links(agents_text, agents_doc_rel, errors):
                if not raw_target or raw_target.startswith(("#", "http://", "https://", "mailto:", "file://")):
                    continue
                target_no_frag = raw_target.split("#", 1)[0].strip()
                if target_no_frag and not (agents_doc_path.parent / target_no_frag).resolve().exists():
                    errors.append(f"Broken relative markdown link in {agents_doc_rel}: {raw_target}")

    skill_doc_rel = "sdk/native/skills/pulse-plugin-author/SKILL.md"
    skill_doc_path = root / skill_doc_rel
    if enforce_agent_assets or skill_doc_path.is_file():
        checked_docs.append(skill_doc_rel)
        if not skill_doc_path.is_file():
            errors.append(f"Missing agent skill file: {skill_doc_rel}")
        else:
            skill_text = skill_doc_path.read_text(encoding="utf-8")
            fm_match = re.match(r"\A---\r?\n(.*?)\r?\n---\r?\n", skill_text, flags=re.S)
            if not fm_match:
                errors.append(f"{skill_doc_rel} must start with valid YAML frontmatter (--- ... ---)")
            else:
                fm_body = fm_match.group(1)
                if not re.search(r"^\s*name:\s*pulse-plugin-author\s*$", fm_body, flags=re.M):
                    errors.append(f"{skill_doc_rel} YAML frontmatter must declare 'name: pulse-plugin-author'")
                if not re.search(r"^\s*description:\s*\S+", fm_body, flags=re.M):
                    errors.append(f"{skill_doc_rel} YAML frontmatter must declare a non-empty 'description'")
            for raw_target in _extract_markdown_links(skill_text, skill_doc_rel, errors):
                if not raw_target or raw_target.startswith(("#", "http://", "https://", "mailto:", "file://")):
                    continue
                target_no_frag = raw_target.split("#", 1)[0].strip()
                if target_no_frag and not (skill_doc_path.parent / target_no_frag).resolve().exists():
                    errors.append(f"Broken relative markdown link in {skill_doc_rel}: {raw_target}")

    mcp_example_rel = "sdk/native/mcp.example.json"
    mcp_example_path = root / mcp_example_rel
    if enforce_agent_assets or mcp_example_path.is_file():
        if not mcp_example_path.is_file():
            errors.append(f"Missing MCP configuration template: {mcp_example_rel}")
        else:
            try:
                mcp_example = json.loads(mcp_example_path.read_text(encoding="utf-8"))
                server_cfg = (mcp_example.get("mcpServers") or {}).get("pulse-native-author")
                if not isinstance(server_cfg, dict) or not isinstance(server_cfg.get("args"), list):
                    errors.append(f"{mcp_example_rel} must configure mcpServers.pulse-native-author")
                elif not any("native_author_mcp.py" in str(arg) for arg in server_cfg["args"]):
                    errors.append(f"{mcp_example_rel} args must reference native_author_mcp.py")
            except ValueError as exc:
                errors.append(f"Invalid JSON in {mcp_example_rel}: {exc}")

    sandboxed_dir = root / "sdk/native/samples/sandboxed-worker"
    if enforce_agent_assets or sandboxed_dir.exists():
        for member in ("CMakeLists.txt", "plugin.cpp", "worker_main.cpp", "manifest.json", "README.md"):
            if not (sandboxed_dir / member).is_file():
                errors.append(f"sdk/native/samples/sandboxed-worker is missing {member}")
        sw_manifest_path = sandboxed_dir / "manifest.json"
        if sw_manifest_path.is_file():
            try:
                sw_manifest = json.loads(sw_manifest_path.read_text(encoding="utf-8"))
                unknown_sw_res = [r for r in sw_manifest.get("resources", []) if r not in EXPECTED_RESOURCES]
                if unknown_sw_res:
                    errors.append(f"sandboxed-worker manifest declares unknown resources: {unknown_sw_res}")
                if (root / "tools/extensions/offline_manifest.py").is_file():
                    offline_mod = _load_module_from(root, "offline_manifest.py", "_check_offline_manifest")
                    offline_res = offline_mod.validate_manifest_file(sw_manifest_path)
                    if not offline_res.get("valid"):
                        errors.append(f"sandboxed-worker manifest failed offline validation: {offline_res}")
            except ValueError as exc:
                errors.append(f"Invalid JSON in sandboxed-worker/manifest.json: {exc}")

    return {
        "ok": not errors,
        "root": str(root),
        "sdkPreview": abi_preview,
        "resourceCount": len(EXPECTED_RESOURCES),
        "templates": list(packable_templates),
        "checkedDocs": checked_docs,
        "errors": errors,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    report = check_all(args.root)
    if args.json:
        print(json.dumps(report, ensure_ascii=False))
    else:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
