"""Standalone verifier for Pulse-NativeSDK-<version>-windows-x64.zip outside the repository tree."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import msvc_env
import package_store
import pulse_ext
import test_native_sdk


def _isolated_python_env(extracted_sdk):
    env = dict(os.environ)
    env["PYTHONPATH"] = str((extracted_sdk / "tools/extensions").resolve())
    return env


@contextlib.contextmanager
def _load_extracted_store_modules(extracted_sdk):
    """Dynamically load native_archive.py and package_store.py from the extracted SDK tree."""
    extracted_sdk = Path(extracted_sdk).resolve()
    sdk_tools = extracted_sdk / "tools/extensions"
    if not (sdk_tools / "native_archive.py").is_file():
        sdk_tools = extracted_sdk / "tools"
    store_file = sdk_tools / "package_store.py"
    archive_file = sdk_tools / "native_archive.py"
    signing_file = sdk_tools / "package_signing.py"
    if not store_file.is_file() or not archive_file.is_file():
        raise ValueError(f"Extracted SDK is missing package_store.py or native_archive.py under {sdk_tools}")

    saved_modules = {name: sys.modules.get(name) for name in ("package_signing", "package_store", "native_archive")}
    saved_path = list(sys.path)
    sys.path.insert(0, str(sdk_tools))
    try:
        loaded = {}
        for mod_name, mod_path in (
            ("package_signing", signing_file),
            ("package_store", store_file),
            ("native_archive", archive_file),
        ):
            if not mod_path.is_file():
                continue
            spec = importlib.util.spec_from_file_location(mod_name, mod_path)
            if spec is None or spec.loader is None:
                raise ValueError(f"Unable to load module spec for {mod_path}")
            module = importlib.util.module_from_spec(spec)
            sys.modules[mod_name] = module
            spec.loader.exec_module(module)
            loaded[mod_name] = module
        ext_store = loaded["package_store"]
        ext_archive = loaded["native_archive"]
        if not Path(ext_store.__file__).resolve().is_relative_to(extracted_sdk):
            raise ValueError("package_store did not resolve from extracted SDK")
        if not Path(ext_archive.__file__).resolve().is_relative_to(extracted_sdk):
            raise ValueError("native_archive did not resolve from extracted SDK")
        yield ext_archive, ext_store
    finally:
        sys.path[:] = saved_path
        for mod_name, previous in saved_modules.items():
            if previous is None:
                sys.modules.pop(mod_name, None)
            else:
                sys.modules[mod_name] = previous


def _verify_compiled_lifecycle(extracted_sdk, generated_projects, temp_root):
    """Verify .pulse-ext packing, verification, upgrade, enable/disable, rollback, uninstall, and GC."""
    projects_by_template = {item["template"]: item for item in generated_projects}
    # Representative templates for minimal, tool-panel, and archive/directory provider roles.
    required_roles = {
        "minimal": "inspector",
        "tool-panel": "reactive-panel",
        "archive-provider": "directory-browser",
    }
    for role_label, tpl_name in required_roles.items():
        if tpl_name not in projects_by_template:
            raise ValueError(f"Missing compiled template '{tpl_name}' for '{role_label}' lifecycle check")

    with _load_extracted_store_modules(extracted_sdk) as (ext_archive, ext_store):
        def pack_project_archive(proj, stage_dir, archive_out, *, version_tag=None):
            proj_dir = Path(proj["path"]).resolve()
            build_dir = proj_dir / "build"
            base_manifest = json.loads((proj_dir / "manifest.json").read_text(encoding="utf-8"))
            stage_dir.mkdir(parents=True, exist_ok=True)
            if version_tag is not None:
                authored_doc = dict(base_manifest, version=version_tag, name=f"{base_manifest['name']} ({version_tag})")
                (stage_dir / "pulse.extension.json").write_text(
                    json.dumps(authored_doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
                )
                manifest = dict(base_manifest, name=authored_doc["name"])
            else:
                manifest = dict(base_manifest)

            primary_dll_name = proj["primaryLibrary"]
            primary_path = build_dir / primary_dll_name
            primary_bytes = ext_store.read_bytes(primary_path, ext_store.MAX_NATIVE_BYTES)
            primary_sha = ext_store.validate_native_binary(primary_bytes)
            native_decl = {
                "version": 1,
                "architecture": "windows-x64",
                "library": primary_dll_name,
                "sha256": primary_sha,
            }
            files_map = {primary_dll_name: primary_bytes}
            extra_targets = [t + ".dll" for t in proj["targets"] if (t + ".dll") != primary_dll_name]
            if extra_targets:
                native_decl["files"] = []
                for sidecar_name in extra_targets:
                    sidecar_bytes = ext_store.read_bytes(build_dir / sidecar_name, ext_store.MAX_NATIVE_BYTES)
                    sidecar_sha = ext_store.validate_native_binary(sidecar_bytes)
                    native_decl["files"].append({"path": sidecar_name, "sha256": sidecar_sha})
                    files_map[sidecar_name] = sidecar_bytes

            manifest["native"] = native_decl
            ext_store.native_declaration(manifest)
            ext_store.validate_native_files(native_decl, files_map)
            manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
            (stage_dir / "manifest.json").write_bytes(manifest_bytes)

            metadata = ext_archive.metadata_for(manifest_bytes, manifest)
            snapshot = ext_archive.PackageSnapshot(metadata, manifest_bytes, primary_bytes, files_map)
            buf = io.BytesIO()
            ext_archive.write(buf, snapshot)
            archive_out.parent.mkdir(parents=True, exist_ok=True)
            archive_out.write_bytes(buf.getvalue())
            return archive_out

        def verify_archive_file(archive_file, expected_id):
            snapshot = ext_archive.read(archive_file)
            if snapshot.metadata.get("id") != expected_id:
                raise ValueError(f"Verified archive ID mismatch: {snapshot.metadata.get('id')} != {expected_id}")
            if snapshot.metadata.get("formatVersion") != 2 or snapshot.binary_bytes is None:
                raise ValueError(f"Expected native v2 archive for {expected_id}")
            if snapshot.metadata.get("manifestSha256") != hashlib.sha256(snapshot.manifest_bytes).hexdigest():
                raise ValueError(f"Manifest SHA-256 mismatch in {archive_file}")
            return snapshot

        def install_archive_into_store(store_root, archive_file, expected_id):
            snapshot = verify_archive_file(archive_file, expected_id)
            installed = ext_store.install(
                store_root,
                snapshot.manifest_bytes,
                accept_capabilities=True,
                allow_native=True,
                binary_data=snapshot.binary_bytes,
                file_data=snapshot.files,
            )
            # Fresh store installations start disabled until explicitly enabled; upgrades preserve enabled state.
            if "previousRevision" not in installed["package"] and not installed["package"]["enabled"]:
                installed = ext_store.update(store_root, expected_id, "enable", allow_native=True)
            return installed, snapshot

        # 1. Pack .pulse-ext and verify_archive for minimal (0.1.0), tool-panel, archive-provider (and all templates).
        packages_dir = temp_root / "lifecycle-packages"
        packed_archives = {}
        for proj in generated_projects:
            tpl = proj["template"]
            ver_tag = "0.1.0" if tpl == required_roles["minimal"] else None
            stage_dir = temp_root / "lifecycle-stage" / f"{tpl}-0.1.0"
            out_archive = packages_dir / f"{proj['packageId']}-0.1.0.pulse-ext"
            pack_project_archive(proj, stage_dir, out_archive, version_tag=ver_tag)
            verify_archive_file(out_archive, proj["packageId"])
            packed_archives[tpl] = out_archive

        # 2. Install minimal (0.1.0) into isolated store; assert enabled == True and activeRevision exists.
        minimal_proj = projects_by_template[required_roles["minimal"]]
        minimal_id = minimal_proj["packageId"]
        isolated_store = temp_root / "lifecycle-isolated-store"
        install_010, snap_010 = install_archive_into_store(
            isolated_store,
            packed_archives[required_roles["minimal"]],
            minimal_id,
        )
        pkg_010 = install_010["package"]
        rev_010 = pkg_010.get("activeRevision")
        if pkg_010.get("enabled") is not True or not rev_010:
            raise ValueError(f"Expected enabled minimal v0.1.0 with activeRevision, got: {pkg_010}")
        if rev_010 != hashlib.sha256(snap_010.manifest_bytes).hexdigest():
            raise ValueError("activeRevision does not match v0.1.0 manifest SHA-256")
        if not (isolated_store / "versions" / f"{rev_010}.json").is_file():
            raise ValueError("Stored v0.1.0 manifest revision file is missing")

        # 3. Bump minimal copy's pulse.extension.json version to 0.2.0, repack, upgrade-install.
        stage_020 = temp_root / "lifecycle-stage" / f"{required_roles['minimal']}-0.2.0"
        archive_020 = packages_dir / f"{minimal_id}-0.2.0.pulse-ext"
        pack_project_archive(minimal_proj, stage_020, archive_020, version_tag="0.2.0")
        authored_020 = json.loads((stage_020 / "pulse.extension.json").read_text(encoding="utf-8"))
        if authored_020.get("version") != "0.2.0":
            raise ValueError("Temporary copy pulse.extension.json version was not updated to 0.2.0")
        install_020, snap_020 = install_archive_into_store(isolated_store, archive_020, minimal_id)
        pkg_020 = install_020["package"]
        rev_020 = pkg_020.get("activeRevision")
        if pkg_020.get("enabled") is not True:
            raise ValueError(f"Upgrade install did not preserve enabled=True: {pkg_020}")
        if not rev_020 or rev_020 == rev_010 or rev_020 != hashlib.sha256(snap_020.manifest_bytes).hexdigest():
            raise ValueError(f"Expected updated activeRevision on v0.2.0 upgrade, got: {pkg_020}")
        if pkg_020.get("previousRevision") != rev_010:
            raise ValueError(f"Expected previousRevision == {rev_010}, got: {pkg_020}")

        # 4. disable -> enable -> rollback -> uninstall -> collect_garbage (gc).
        disabled_res = ext_store.update(isolated_store, minimal_id, "disable")
        if disabled_res["package"].get("enabled") is not False:
            raise ValueError(f"Disable did not set enabled=False: {disabled_res}")

        enabled_res = ext_store.update(isolated_store, minimal_id, "enable", allow_native=True)
        if enabled_res["package"].get("enabled") is not True:
            raise ValueError(f"Re-enable did not set enabled=True: {enabled_res}")

        rollback_res = ext_store.update(
            isolated_store,
            minimal_id,
            "rollback",
            allow_native=True,
            accept_capabilities=True,
        )
        if rollback_res["package"].get("activeRevision") != rev_010:
            raise ValueError(f"Rollback did not restore v0.1.0 activeRevision ({rev_010}): {rollback_res}")
        if rollback_res["package"].get("enabled") is not True:
            raise ValueError(f"Rollback did not keep package enabled: {rollback_res}")

        uninstall_res = ext_store.update(isolated_store, minimal_id, "uninstall")
        if not uninstall_res.get("changed"):
            raise ValueError(f"Uninstall reported no change: {uninstall_res}")
        if ext_store.listing(isolated_store).get("packages") != []:
            raise ValueError("Registry is not empty after uninstall")

        gc_res = ext_store.gc(isolated_store)
        if gc_res.get("reclaimedBytes", 0) <= 0 or gc_res.get("skipped"):
            raise ValueError(f"Garbage collection did not reclaim unreferenced store artifacts cleanly: {gc_res}")
        if ext_store.listing(isolated_store).get("packages") != []:
            raise ValueError("Registry is not empty after garbage collection")
        for store_subdir in ("versions", "binaries", "packages"):
            subdir_path = isolated_store / store_subdir
            if subdir_path.exists() and list(subdir_path.iterdir()):
                raise ValueError(f"Store {store_subdir}/ still contains leftover files after GC: {list(subdir_path.iterdir())}")

        return {
            "packedArchiveCount": len(packed_archives),
            "lifecyclePackageId": minimal_id,
            "initialRevision": rev_010,
            "upgradedRevision": rev_020,
            "reclaimedBytes": gc_res["reclaimedBytes"],
        }


def verify_bundle(archive_path, work_dir=None, compile_projects=False, require_compile=False):
    archive_path = Path(archive_path).resolve()
    if not archive_path.is_file():
        raise ValueError(f"SDK archive does not exist: {archive_path}")

    with contextlib.ExitStack() as stack:
        if work_dir is None:
            temp_root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="pulse-sdk-verify-"))).resolve()
        else:
            temp_root = Path(work_dir).resolve()
            if temp_root.exists():
                raise ValueError(f"Verification output directory must not already exist: {temp_root}")
            temp_root.mkdir(parents=True, exist_ok=False)

        extracted_sdk = test_native_sdk.extract(archive_path, temp_root / "extracted-sdk")
        test_native_sdk.validate(extracted_sdk)

        sdk_manifest = json.loads((extracted_sdk / "native-sdk-manifest.json").read_text(encoding="utf-8"))
        compatibility = sdk_manifest.get("compatibility")
        if not isinstance(compatibility, dict):
            raise ValueError("SDK manifest is missing compatibility metadata")
        for required_key in ("pulseVersion", "minHostVersion", "sdkPreview", "abiVersion",
                             "architecture", "minWindowsBuild", "entrypoint", "toolchain"):
            if required_key not in compatibility:
                raise ValueError(f"SDK compatibility metadata missing '{required_key}'")
        if compatibility["sdkPreview"] != 3 or compatibility["abiVersion"] != 1:
            raise ValueError(f"Unexpected SDK preview/ABI version: {compatibility}")
        if compatibility["architecture"] != "windows-x64" or compatibility["minWindowsBuild"] != 10240:
            raise ValueError(f"Unexpected SDK platform requirements: {compatibility}")

        py_env = _isolated_python_env(extracted_sdk)

        # 1. Run extracted check_docs_and_contracts.py detached from the repo source tree.
        checker_script = extracted_sdk / "tools/extensions/check_docs_and_contracts.py"
        if not checker_script.is_file():
            raise ValueError("Extracted SDK is missing tools/extensions/check_docs_and_contracts.py")
        contract_proc = subprocess.run(
            [sys.executable, str(checker_script), "--root", str(extracted_sdk), "--json"],
            cwd=temp_root,
            env=py_env,
            capture_output=True,
            timeout=60,
        )
        if contract_proc.returncode != 0:
            raise ValueError(
                "Extracted check_docs_and_contracts.py failed: "
                + contract_proc.stdout.decode("utf-8", errors="replace")
                + contract_proc.stderr.decode("utf-8", errors="replace")
            )
        contract_report = json.loads(contract_proc.stdout.decode("utf-8"))
        if not contract_report.get("ok"):
            raise ValueError(f"Contract check reported errors: {contract_report.get('errors')}")

        # 2. Scaffold independent projects for all 9 packable templates using extracted pulse_ext.py new.
        cli_script = extracted_sdk / "tools/extensions/pulse_ext.py"
        examples_catalog = json.loads((extracted_sdk / "examples/native/catalog.json").read_text(encoding="utf-8"))
        templates = list(examples_catalog["templates"])
        if len(templates) != 9:
            raise ValueError(f"Expected 9 packable templates in SDK, found {len(templates)}: {templates}")

        projects_root = temp_root / "generated-projects"
        projects_root.mkdir()
        generated_projects = []
        seen_ids = set()
        seen_primary_dlls = set()

        for template in templates:
            slug = template.replace("-", "_")
            package_id = f"org.pulse.sdkverify.{slug}"
            display_name = f"SDK Verify {template}"
            dest = projects_root / f"proj-{template}"
            create_proc = subprocess.run(
                [
                    sys.executable,
                    str(cli_script),
                    "--json",
                    "new",
                    str(dest),
                    "--id",
                    package_id,
                    "--name",
                    display_name,
                    "--template",
                    template,
                    "--sdk-root",
                    str(extracted_sdk),
                ],
                cwd=temp_root,
                env=py_env,
                capture_output=True,
                timeout=30,
            )
            if create_proc.returncode != 0:
                raise ValueError(
                    f"pulse_ext.py new failed for template '{template}': "
                    + create_proc.stdout.decode("utf-8", errors="replace")
                    + create_proc.stderr.decode("utf-8", errors="replace")
                )
            reply = json.loads(create_proc.stdout.decode("utf-8"))
            if not reply.get("ok") or reply.get("packageId") != package_id:
                raise ValueError(f"Unexpected new project payload for '{template}': {reply}")

            manifest_data = json.loads((dest / "manifest.json").read_text(encoding="utf-8"))
            if manifest_data.get("id") != package_id or manifest_data.get("name") != display_name:
                raise ValueError(f"Rendered manifest identity mismatch in {template}")
            if manifest_data.get("contributes") != {"commands": []}:
                raise ValueError(f"Rendered manifest missing default contributes.commands in {template}")

            cmake_text = (dest / "CMakeLists.txt").read_text(encoding="utf-8")
            if reply["libraryStem"] not in cmake_text:
                raise ValueError(f"Rendered CMakeLists.txt missing stem '{reply['libraryStem']}' in {template}")
            readme_text = (dest / "README.md").read_text(encoding="utf-8")
            if package_id not in readme_text or reply["primaryLibrary"] not in readme_text:
                raise ValueError(f"Rendered README.md missing identity or DLL in {template}")
            if not (dest / "plugin.cpp").is_file():
                raise ValueError(f"Generated project for {template} is missing plugin.cpp")

            if package_id in seen_ids or reply["primaryLibrary"] in seen_primary_dlls:
                raise ValueError(f"Duplicate package ID or primary DLL generated for {template}")
            seen_ids.add(package_id)
            seen_primary_dlls.add(reply["primaryLibrary"])
            generated_projects.append({
                "template": template,
                "packageId": package_id,
                "path": str(dest),
                "targets": reply["targets"],
                "primaryLibrary": reply["primaryLibrary"],
            })

        # Confirm ABI-only 'basic' sample is cleanly rejected as non-packable.
        basic_proc = subprocess.run(
            [
                sys.executable,
                str(cli_script),
                "--json",
                "new",
                str(projects_root / "proj-basic"),
                "--id",
                "org.pulse.sdkverify.basic",
                "--name",
                "Basic",
                "--template",
                "basic",
                "--sdk-root",
                str(extracted_sdk),
            ],
            cwd=temp_root,
            env=py_env,
            capture_output=True,
            timeout=30,
        )
        basic_reply = json.loads(basic_proc.stdout.decode("utf-8"))
        if basic_proc.returncode != 2 or basic_reply.get("error", {}).get("code") != "template_not_packable":
            raise ValueError(f"Expected template_not_packable for 'basic', got: {basic_reply}")

        # Verify agent-first assets, offline modules, and sandboxed-worker reference sample in extracted SDK.
        required_agent_files = (
            "sdk/native/AGENTS.md",
            "sdk/native/skills/pulse-plugin-author/SKILL.md",
            "sdk/native/mcp.example.json",
            "sdk/native/samples/sandboxed-worker/CMakeLists.txt",
            "sdk/native/samples/sandboxed-worker/plugin.cpp",
            "sdk/native/samples/sandboxed-worker/worker_main.cpp",
            "sdk/native/samples/sandboxed-worker/manifest.json",
            "sdk/native/samples/sandboxed-worker/README.md",
            "tools/extensions/msvc_env.py",
            "tools/extensions/offline_manifest.py",
        )
        for rel_file in required_agent_files:
            if rel_file not in sdk_manifest["files"] or not (extracted_sdk / rel_file).is_file():
                raise ValueError(f"Extracted SDK is missing required agent/reference file: {rel_file}")

        # Verify host-free offline CLI doctor + manifest/contract check inside extracted SDK.
        sample_proj_dir = Path(generated_projects[0]["path"])
        offline_doc_proc = subprocess.run(
            [
                sys.executable,
                str(cli_script),
                "--json",
                "doctor",
                "--manifest",
                str(sample_proj_dir / "manifest.json"),
            ],
            cwd=temp_root,
            env=py_env,
            capture_output=True,
            timeout=30,
        )
        if offline_doc_proc.returncode != 0:
            raise ValueError(
                "Offline pulse_ext.py doctor failed in extracted SDK: "
                + offline_doc_proc.stdout.decode("utf-8", errors="replace")
                + offline_doc_proc.stderr.decode("utf-8", errors="replace")
            )
        offline_doc_reply = json.loads(offline_doc_proc.stdout.decode("utf-8"))
        if not offline_doc_reply.get("ok") or not offline_doc_reply.get("doctor", {}).get("healthy"):
            raise ValueError(f"Unexpected offline doctor reply: {offline_doc_reply}")

        # 3. Optional real C++20 compilation when --compile is passed and x64 toolchain is ready.
        compiled = False
        compile_skipped = False
        compile_skipped_reasons = []
        compiled_dlls = []
        lifecycle_summary = None
        if compile_projects:
            toolchain = pulse_ext.inspect_toolchain()
            if not toolchain["ready"]:
                if require_compile:
                    raise ValueError("Toolchain is not ready for --require-compile: " + "; ".join(toolchain["missingReasons"]))
                compile_skipped = True
                compile_skipped_reasons = toolchain["missingReasons"]
            else:
                resolved_tc = msvc_env.resolve_build_environment(auto_discover=True)
                cmake_exe = resolved_tc["cmake_executable"] or "cmake"
                ninja_exe = resolved_tc["ninja_executable"]
                sdk_include = str((extracted_sdk / "sdk/native/include").resolve())
                for proj in generated_projects:
                    proj_dir = Path(proj["path"])
                    build_dir = proj_dir / "build"
                    cfg_cmd = [
                        cmake_exe,
                        "-S",
                        str(proj_dir),
                        "-B",
                        str(build_dir),
                        "-G",
                        "Ninja",
                        "-DCMAKE_BUILD_TYPE=Release",
                        "-DPULSE_SDK_INCLUDE=" + sdk_include,
                    ]
                    if ninja_exe:
                        cfg_cmd.append("-DCMAKE_MAKE_PROGRAM=" + ninja_exe)
                    for cmd in (
                        cfg_cmd,
                        [cmake_exe, "--build", str(build_dir), "--parallel", "2"],
                    ):
                        proc = subprocess.run(cmd, cwd=temp_root, env=resolved_tc["env"], capture_output=True, timeout=300)
                        if proc.returncode != 0:
                            raise ValueError(
                                f"Failed compiling generated project {proj['template']}: "
                                + proc.stdout.decode("utf-8", errors="replace")
                                + proc.stderr.decode("utf-8", errors="replace")
                            )
                    for target_name in proj["targets"]:
                        dll_file = build_dir / (target_name + ".dll")
                        if not dll_file.is_file():
                            raise ValueError(f"Expected compiled DLL missing: {dll_file}")
                        package_store.validate_native_binary(dll_file.read_bytes())
                        compiled_dlls.append(dll_file.name)
                lifecycle_summary = _verify_compiled_lifecycle(extracted_sdk, generated_projects, temp_root)
                compiled = True

        report = {
            "ok": True,
            "archive": str(archive_path),
            "archiveSha256": hashlib.sha256(archive_path.read_bytes()).hexdigest(),
            "fileCount": len(sdk_manifest["files"]),
            "compatibility": compatibility,
            "contractsVerified": True,
            "templatesVerified": len(generated_projects),
            "templates": [item["template"] for item in generated_projects],
            "basicSampleRejectedCleanly": True,
            "compileRequested": bool(compile_projects),
            "compiled": compiled,
            "compileSkipped": compile_skipped,
            "compileSkippedReasons": compile_skipped_reasons,
            "compiledDllCount": len(compiled_dlls),
            "lifecycleVerified": lifecycle_summary is not None,
        }
        if lifecycle_summary is not None:
            report["lifecycle"] = lifecycle_summary
        if work_dir is not None:
            (temp_root / "verification-report.json").write_text(
                json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
            )
        return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compile", action="store_true")
    parser.add_argument("--require-compile", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    report = verify_bundle(
        args.archive,
        work_dir=args.output,
        compile_projects=(args.compile or args.require_compile),
        require_compile=args.require_compile,
    )
    if args.json:
        print(json.dumps(report, ensure_ascii=False))
    else:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
