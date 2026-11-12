"""Build or describe the source-only native SDK. No host execution or grants."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]
FILES = [
    'sdk/native/README.md',
    'sdk/native/AGENTS.md',
    'sdk/native/skills/pulse-plugin-author/SKILL.md',
    'sdk/native/mcp.example.json',
    'sdk/native/abi/windows-x64-msvc-preview3.json',
    'tools/extensions/check_native_abi.py',
    'tools/extensions/test_native_abi.py',
    'sdk/native/samples/archive/CMakeLists.txt',
    'sdk/native/samples/archive/plugin.cpp',
    'sdk/native/samples/archive/archive_settings.h',
    'sdk/native/samples/archive/archive_settings.cpp',
    'sdk/native/samples/archive/archive_tasks.h',
    'sdk/native/samples/archive/archive_tasks.cpp',
    'sdk/native/samples/archive/archive_reader.h',
    'sdk/native/samples/archive/archive_reader.cpp',
    'sdk/native/samples/archive/archive_reader_test.cpp',
    'sdk/native/samples/archive/archive_writer.h',
    'sdk/native/samples/archive/archive_writer.cpp',
    'sdk/native/samples/archive/archive_writer_test.cpp',
    'sdk/native/samples/archive/manifest.json',
    'sdk/native/samples/archive/7z.def',
    'sdk/native/samples/sandboxed-worker/CMakeLists.txt',
    'sdk/native/samples/sandboxed-worker/plugin.cpp',
    'sdk/native/samples/sandboxed-worker/worker_main.cpp',
    'sdk/native/samples/sandboxed-worker/manifest.json',
    'sdk/native/samples/sandboxed-worker/README.md',
    'sdk/native/api-catalog.json',
    'sdk/native/include/pulse/abi.h',
    'sdk/native/include/pulse/sdk.hpp',
    'sdk/native/include/pulse/ui.h',
    'sdk/native/include/pulse/files.h',
    'sdk/native/include/pulse/ui_mount.h',
    'tools/extensions/native_author_mcp.py',
    'sdk/native/include/pulse/directories.h',
    'sdk/native/include/pulse/context.h',
    'sdk/native/include/pulse/commands.h',
    'sdk/native/include/pulse/preview.h',
    'sdk/native/include/pulse/tasks.h',
    'sdk/native/include/pulse/staging.h',
    'sdk/native/include/pulse/settings_pages.h',
    'sdk/native/include/pulse/dialogs.h',
    'sdk/native/include/pulse/icons.h',
    'sdk/native/include/pulse/diagnostics.h',
    'sdk/native/include/pulse/pickers.h',
    'sdk/native/include/pulse/workbench.h',
    'sdk/native/include/pulse/fileops.h',
    'sdk/native/include/pulse/ui_model.h',
    'sdk/native/ui-model.schema.json',
    'sdk/native/include/pulse/ui_data.h',
    'sdk/native/include/pulse/resources.h',
    'sdk/native/include/pulse/dispatch.h',
    'sdk/native/include/pulse/ui_events.h',
    'sdk/native/include/pulse/client.hpp',
    'sdk/native/include/pulse/model_builder.hpp',
    'examples/native/README.md',
    'examples/native/basic/CMakeLists.txt',
    'examples/native/basic/plugin.cpp',
    'examples/native/rename-panel/CMakeLists.txt',
    'examples/native/rename-panel/plugin.cpp',
    'examples/native/rename-panel/manifest.json',
    'examples/native/rename-panel/README.md',
    'tools/extensions/bundle_native_sdk.py',
    'tools/extensions/test_native_sdk.py',
    'examples/native/inspector/CMakeLists.txt',
    'examples/native/inspector/plugin.cpp',
    'examples/native/inspector/manifest.json',
    'examples/native/inspector/README.md',
    'examples/native/reactive-panel/CMakeLists.txt',
    'examples/native/reactive-panel/plugin.cpp',
    'examples/native/reactive-panel/manifest.json',
    'examples/native/reactive-panel/README.md',
    'examples/native/reactive-panel/wrapper_test.cpp',
    'examples/native/live-data/CMakeLists.txt',
    'examples/native/live-data/plugin.cpp',
    'examples/native/live-data/provider_test.cpp',
    'examples/native/live-data/manifest.json',
    'examples/native/live-data/README.md',
    'examples/native/directory-browser/CMakeLists.txt',
    'examples/native/directory-browser/plugin.cpp',
    'examples/native/directory-browser/provider_test.cpp',
    'examples/native/directory-browser/manifest.json',
    'examples/native/directory-browser/README.md',
    'examples/native/file-operations/CMakeLists.txt',
    'examples/native/file-operations/plugin.cpp',
    'examples/native/file-operations/manifest.json',
    'examples/native/file-operations/README.md',
    'examples/native/directory-transfer/CMakeLists.txt',
    'examples/native/directory-transfer/plugin.cpp',
    'examples/native/directory-transfer/manifest.json',
    'examples/native/directory-transfer/README.md',
    'examples/native/directory-workbench/CMakeLists.txt',
    'examples/native/directory-workbench/plugin.cpp',
    'examples/native/directory-workbench/manifest.json',
    'examples/native/directory-workbench/README.md',
    'docs/native-packages.md',
    'tools/extensions/README.md',
    'tools/extensions/msvc_env.py',
    'tools/extensions/offline_manifest.py',
    'tools/extensions/pulse_ext.py',
    'tools/extensions/project_creator.py',
    'tools/extensions/package_store.py',
    'tools/extensions/native_archive.py',
    'tools/extensions/package_signing.py',
    'tools/extensions/template_catalog.py',
    'tools/extensions/contract_compat.py',
    'tools/extensions/check_docs_and_contracts.py',
    'tools/extensions/verify_sdk_bundle.py',
    'tools/extensions/test_project_creator.py',
    'tools/extensions/test_native_author_mcp.py',
    'tools/extensions/test_native_author_mcp_workflow.py',
    'examples/native/catalog.json',
    'schemas/extensions/manifest.schema.json',
    'docs/extensions-sdk-quickstart.md',
]


def catalog(root=ROOT):
    value = json.loads((root / "sdk/native/api-catalog.json").read_text(encoding="utf-8"))
    if value["abiVersion"] != 1 or value["delivery"] != "source-only":
        raise ValueError("Unsupported native SDK contract")
    return value


def read_pulse_version(root=ROOT):
    root = Path(root)
    for candidate in (root / "version.txt", ROOT / "version.txt"):
        if candidate.is_file():
            version = candidate.read_text(encoding="utf-8").strip()
            if version:
                return version
    sdk_manifest = root / "native-sdk-manifest.json"
    if sdk_manifest.is_file():
        existing = json.loads(sdk_manifest.read_text(encoding="utf-8"))
        version = existing.get("compatibility", {}).get("pulseVersion")
        if isinstance(version, str) and version.strip():
            return version.strip()
    raise ValueError("Unable to determine Pulse version from version.txt")


def compatibility_metadata(root=ROOT):
    cat = catalog(root)
    version = read_pulse_version(root)
    return {
        "pulseVersion": version,
        "minHostVersion": version,
        "sdkPreview": cat["sdkPreview"],
        "abiVersion": cat["abiVersion"],
        "architecture": "windows-x64",
        "minWindowsBuild": 10240,
        "entrypoint": cat.get("entrypoint", "PulsePluginEntryV1"),
        "previewExport": "PulsePluginSdkPreview",
        "toolchain": {
            "compiler": "MSVC x64 (Visual Studio 2022+ / C++20)",
            "cppStandard": "c++20",
            "cmakeMinVersion": "3.25",
            "generator": "Ninja",
            "pythonMinVersion": "3.8",
        },
    }


def source_bytes(root, name):
    data = (root / name).read_bytes()
    # Catalog header hashes describe LF source, not checkout-specific line endings.
    return data.replace(b"\r\n", b"\n") if name.startswith("sdk/native/include/pulse/") else data


def bundle(output, root=ROOT):
    compat = compatibility_metadata(root)
    payload = {name: source_bytes(root, name) for name in FILES}
    manifest = {
        "formatVersion": 1,
        "delivery": "source-only",
        "hostIncluded": False,
        "compatibility": compat,
        "files": {name: hashlib.sha256(data).hexdigest() for name, data in payload.items()},
    }
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation avoids silently replacing a previously reviewed SDK.
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in payload.items():
            archive.writestr(name, data)
        archive.writestr("native-sdk-manifest.json", json.dumps(manifest, indent=2) + "\n")
    return manifest


def compatibility_report(archive_path=None, root=ROOT):
    if archive_path is not None:
        archive_path = Path(archive_path).resolve()
        raw_archive = archive_path.read_bytes()
        with zipfile.ZipFile(archive_path) as zf:
            manifest = json.loads(zf.read("native-sdk-manifest.json"))
            cat = json.loads(zf.read("sdk/native/api-catalog.json"))
            examples_cat = json.loads(zf.read("examples/native/catalog.json"))
        return {
            "formatVersion": 1,
            "archive": archive_path.name,
            "archiveSha256": hashlib.sha256(raw_archive).hexdigest(),
            "archiveBytes": len(raw_archive),
            "delivery": manifest["delivery"],
            "hostIncluded": manifest["hostIncluded"],
            "compatibility": manifest["compatibility"],
            "interfaces": [
                {"name": item["name"], "version": item["version"], "header": item.get("header")}
                for item in cat.get("interfaces", [])
                if isinstance(item, dict) and "name" in item
            ],
            "templates": list(examples_cat.get("templates", [])),
        }
    cat = catalog(root)
    examples_cat = json.loads((Path(root) / "examples/native/catalog.json").read_text(encoding="utf-8"))
    return {
        "formatVersion": 1,
        "delivery": "source-only",
        "hostIncluded": False,
        "compatibility": compatibility_metadata(root),
        "interfaces": [
            {"name": item["name"], "version": item["version"], "header": item.get("header")}
            for item in cat.get("interfaces", [])
            if isinstance(item, dict) and "name" in item
        ],
        "templates": list(examples_cat.get("templates", [])),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("describe")
    pack = commands.add_parser("bundle")
    pack.add_argument("--output", type=Path, required=True)
    compat_cmd = commands.add_parser("compatibility")
    compat_cmd.add_argument("--archive", type=Path)
    compat_cmd.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.command == "describe":
        result = catalog()
    elif args.command == "bundle":
        result = bundle(args.output)
    else:
        result = compatibility_report(archive_path=args.archive)
        if args.output is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
