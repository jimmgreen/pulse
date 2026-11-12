"""Development CLI for the public Pulse extension contract (Python stdlib only)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
import zlib
if os.name == "nt":
    import ctypes
    from ctypes import wintypes

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import package_store
import native_archive
import template_catalog
import contract_compat
import project_creator
import msvc_env
import offline_manifest

ROOT = Path(__file__).resolve().parents[2]
HOST_TIMEOUT = 30
TEMPLATES = template_catalog.names()
BUILD_CONFIGS = ("Debug", "RelWithDebInfo", "Release")
_ORIGINAL_SUBPROCESS_RUN = subprocess.run


class Failure(Exception):
    def __init__(self, code, message, exit_code=3, **details):
        super().__init__(message)
        self.error = {"code": code, "message": message, **details}
        self.exit_code = exit_code


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise Failure("usage", message, 2)

    def exit(self, status=0, message=None):
        if status:
            raise Failure("usage", message or "Invalid arguments", 2)
        raise Failure("help", self.format_help(), 0)

    def print_help(self, file=None):
        pass  # main emits help once, including in machine-readable mode.


def strict_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON member")
        result[key] = value
    return result


def valid_structured_details(api):
    required = "structuredCapabilityDetails" in api.get("developerFeatures", [])
    if not required and "capabilityDetails" not in api and "bindingDetails" not in api:
        return True
    for key, identifiers in (("capabilityDetails", "capabilities"), ("bindingDetails", "bindings")):
        entries = api.get(key)
        if not isinstance(entries, list) or len(entries) != len(api[identifiers]):
            return False
        seen = set()
        for entry in entries:
            if not isinstance(entry, dict):
                return False
            identifier = entry.get("id")
            if not isinstance(identifier, str) or identifier not in api[identifiers] or identifier in seen:
                return False
            seen.add(identifier)
            if not isinstance(entry.get("summary"), str) or not entry["summary"].strip():
                return False
            if key == "bindingDetails":
                if entry.get("displayOnly") is not True:
                    return False
                continue
            if any(not isinstance(entry.get(name), str) or not entry[name].strip() for name in ("effect", "target")):
                return False
            if any(type(entry.get(name)) is not bool for name in
                   ("modifiesFiles", "persistsPreferences", "supportsCancellation", "argsRequired")):
                return False
            if "argsSchema" not in entry:
                return False
            schema = entry["argsSchema"]
            if schema is None:
                if entry["argsRequired"]:
                    return False
            elif not isinstance(schema, dict) or schema.get("type") != "object":
                return False
    return True


def host_call(pulse, flag, path=None):
    command = [str(pulse), flag]
    if path is not None:
        command.append(str(path))
    # Earlier developer hosts reject this missing-path check before opening UI.
    command.append("--extension-check")
    try:
        completed = subprocess.run(command, shell=False, capture_output=True, timeout=HOST_TIMEOUT)
    except FileNotFoundError as exc:
        raise Failure("host_not_found", "Pulse executable was not found; supply --pulse EXE.") from exc
    except subprocess.TimeoutExpired as exc:
        raise Failure("host_timeout", "Pulse did not reply within 30 seconds; use a host supporting the extension CLI.") from exc
    except OSError as exc:
        raise Failure("host_launch_failed", str(exc)) from exc
    try:
        output = json.loads(completed.stdout.decode("utf-8"), object_pairs_hook=strict_object,
                            parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    except (UnicodeError, ValueError) as exc:
        raise Failure("host_protocol_error", "Pulse returned invalid UTF-8 JSON; this host may be too old.",
                      host_exit_code=completed.returncode) from exc
    if not isinstance(output, dict):
        raise Failure("host_protocol_error", "Pulse response must be a JSON object.")
    if flag == "--extension-api":
        valid = (type(output.get("apiVersion")) is int and output["apiVersion"] == 1
                 and isinstance(output.get("manifestSchema"), dict) and output["manifestSchema"].get("type") == "object"
                 and isinstance(output.get("limits"), dict)
                 and all(type(output["limits"].get(key)) is int and output["limits"][key] > 0
                         for key in ("manifestBytes", "idBytes", "labelBytes"))
                 and all(isinstance(output.get(key), list) and
                         all(isinstance(item, str) and item for item in output[key])
                         for key in ("capabilities", "bindings", "developmentNotes")))
        for key in ("viewModes", "workspaceLayouts", "developerFeatures", "panelLayouts", "panelDensities", "panelPlacements", "panelTitleTones"):
            if key in output:
                valid = valid and isinstance(output[key], list) and all(isinstance(value, str) and value for value in output[key])
        if valid:
            valid = valid_structured_details(output)
        if valid and ("templates" in output or "declarativeTemplates" in output.get("developerFeatures", [])):
            names = output.get("templates")
            allowed = set("abcdefghijklmnopqrstuvwxyz0123456789-_")
            valid = (isinstance(names, list) and 1 <= len(names) <= 32 and
                     all(isinstance(name, str) and 1 <= len(name) <= 64 and set(name) <= allowed for name in names) and
                     len(set(names)) == len(names))
        if completed.returncode != 0 or not valid:
            raise Failure("host_protocol_error", "Pulse returned an unsupported or inconsistent SDK response.")
    else:
        valid = (type(output.get("valid")) is bool and
                 all(isinstance(output.get(key), str) for key in ("error", "code", "path")))
        if not valid:
            raise Failure("host_protocol_error", "Pulse returned a malformed validation response.")
        if output["valid"]:
            valid = completed.returncode == 0 and output["error"] == "" and output["code"] == ""
        else:
            valid = (completed.returncode == 2 and bool(output["error"]) and output["code"] in
                     ("invalid_args", "invalid_path", "read_failed", "too_large", "invalid_manifest"))
        if not valid or output["path"] != str(path):
            raise Failure("host_protocol_error", "Pulse validation result conflicts with its exit code or requested path.")
    return output


def validated(pulse, path):
    if pulse is None:
        result = offline_manifest.validate_manifest_file(path)
    else:
        result = host_call(pulse, "--extension-validate-json", path)
    if not result["valid"]:
        raise Failure(result["code"], result["error"], 2, host=result)
    return result


def initialize(pulse, destination, identifier, name, template):
    destination = Path(os.path.abspath(destination))
    if os.path.lexists(destination):
        raise Failure("destination_exists", "Destination already exists; choose a new directory.", 2, path=str(destination))
    try:
        manifest = template_catalog.load(template)
        manifest["id"] = identifier
        manifest["name"] = name
        text = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    except (OSError, ValueError) as exc:
        raise Failure("template_failed", str(exc)) from exc
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", suffix=".json", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(text)
        validated(pulse, temporary)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    created = []
    identity = None
    try:
        destination.mkdir(exist_ok=False)
        info = destination.stat()
        identity = (info.st_dev, info.st_ino)
        cli = Path(__file__).resolve()
        host = Path(os.path.abspath(pulse)) if pulse is not None else (ROOT / "build/pulse.exe")
        quote = lambda value: "'" + str(value).replace("'", "''") + "'"
        manifest_path = destination / "manifest.json"
        readme = ("# Pulse extension\n\nThis folder has not been installed or executed.\n\n"
                  "Validate before loading (PowerShell):\n\n```powershell\n"
                  f"python {quote(cli)} --pulse {quote(host)} validate {quote(manifest_path)}\n```\n\n"
                  "Load it when ready (PowerShell):\n\n```powershell\n"
                  f"& {quote(host)} --extension-manifest {quote(manifest_path)}\n```\n\n"
                  "Build the native DLL and run native-manifest to attach its hash before packing and installing.\n")
        for filename, contents in (("manifest.json", text), ("README.md", readme)):
            info = destination.stat()
            if (info.st_dev, info.st_ino) != identity or destination.is_symlink():
                raise OSError("Destination changed during initialization")
            path = destination / filename
            with path.open("x", encoding="utf-8", newline="\n") as stream:
                file_info = os.fstat(stream.fileno())
                created.append((path, (file_info.st_dev, file_info.st_ino)))
                stream.write(contents)
    except (OSError, UnicodeError) as exc:
        # Never recurse or remove a competitor's file/directory.
        for path, file_identity in reversed(created):
            try:
                info = path.lstat()
                if (info.st_dev, info.st_ino) == file_identity:
                    path.unlink()
            except OSError:
                pass
        if identity is not None:
            try:
                info = destination.lstat()
                if (info.st_dev, info.st_ino) == identity:
                    destination.rmdir()
            except OSError:
                pass
        code = "destination_exists" if isinstance(exc, FileExistsError) else "write_failed"
        raise Failure(code, str(exc), 2, path=str(destination)) from exc
    return {"path": str(destination), "manifest": str(destination / "manifest.json"), "template": template}


def checked_snapshot(pulse, path):
    validated(pulse, path)
    with path.open("rb") as stream:
        data = stream.read(65537)
    if len(data) > 65536:
        raise Failure("too_large", "Manifest grew beyond the 64 KiB host limit.", 2)
    validate_bytes(pulse, data)
    return data, decode_json(data)


def decode_json(data):
    # The native strict parser also rejects UTF-8 BOM and non-JSON NaN/Infinity.
    if data.startswith(b"\xef\xbb\xbf"):
        raise ValueError("UTF-8 BOM is not permitted")
    return json.loads(data.decode("utf-8"), object_pairs_hook=strict_object,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))


def validate_bytes(pulse, data):
    if pulse is None:
        result = offline_manifest.validate_manifest_bytes(data, "<memory>")
        if not result["valid"]:
            raise Failure(result["code"], result["error"], 2, host=result)
        return
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        validated(pulse, temporary)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _resolve_sdk_include(sdk_root=None):
    base = Path(os.path.abspath(sdk_root or ROOT))
    for candidate in (base / "sdk/native/include", base / "include"):
        if (candidate / "pulse/abi.h").is_file():
            return candidate
    raise Failure("invalid_sdk", "SDK root is missing include/pulse/abi.h.", 2, sdkRoot=str(base))


def build_project(project_dir, config="Release", build_dir=None, sdk_root=None):
    project_dir = Path(os.path.abspath(project_dir))
    for ancestor in (project_dir, *project_dir.parents):
        package_store.ordinary(ancestor)
    if not project_dir.is_dir() or not (project_dir / "CMakeLists.txt").is_file():
        raise Failure("invalid_project", "Project directory must contain CMakeLists.txt.", 2, path=str(project_dir))
    if config not in BUILD_CONFIGS:
        raise Failure("usage", f"Invalid build config '{config}'; choose from {', '.join(BUILD_CONFIGS)}.", 2)

    sdk_include = _resolve_sdk_include(sdk_root)
    resolved = msvc_env.resolve_build_environment(auto_discover=True)
    cmake_exe = resolved["cmake_executable"]
    ninja_exe = resolved["ninja_executable"]
    cl_exe = resolved["cl_executable"]
    if not cmake_exe or not ninja_exe or not cl_exe:
        tc = msvc_env.inspect_toolchain(auto_discover=True)
        raise Failure(
            "toolchain_not_ready",
            "x64 C++ toolchain is incomplete: " + "; ".join(tc.get("missingReasons", [])),
            2,
            toolchain=tc,
        )

    default_subdir = "build" if config == "Release" else f"build-{config.lower()}"
    target_build = Path(os.path.abspath(build_dir)) if build_dir else (project_dir / default_subdir)
    for ancestor in (target_build, *target_build.parents):
        if ancestor.exists():
            package_store.ordinary(ancestor)

    configure_cmd = [
        cmake_exe,
        "-S", str(project_dir),
        "-B", str(target_build),
        "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={config}",
        f"-DCMAKE_MAKE_PROGRAM={ninja_exe}",
        f"-DPULSE_SDK_INCLUDE={sdk_include.as_posix()}",
    ]
    build_cmd = [cmake_exe, "--build", str(target_build), "--parallel", "2"]

    for step_name, cmd in (("configure", configure_cmd), ("build", build_cmd)):
        try:
            completed = _ORIGINAL_SUBPROCESS_RUN(
                cmd,
                shell=False,
                capture_output=True,
                env=resolved["env"],
                cwd=str(project_dir),
                timeout=300,
            )
        except subprocess.TimeoutExpired as exc:
            raise Failure("build_timeout", f"CMake {step_name} timed out after 300 seconds.", 3) from exc
        except OSError as exc:
            raise Failure("build_launch_failed", str(exc), 3) from exc
        if completed.returncode != 0:
            out = (completed.stdout.decode("utf-8", errors="replace") + "\n" +
                   completed.stderr.decode("utf-8", errors="replace")).strip()
            raise Failure(
                "build_failed",
                f"CMake {step_name} failed with exit code {completed.returncode}.",
                2,
                step=step_name,
                exitCode=completed.returncode,
                diagnostics=out[-16384:],
            )

    libraries = [str(p) for p in sorted(target_build.glob("*.dll")) if p.is_file()]
    executables = [str(p) for p in sorted(target_build.glob("*.exe")) if p.is_file()]
    symbols = [str(p) for p in sorted(target_build.glob("*.pdb")) if p.is_file()]
    binary_inspections = {}
    for dll_str in libraries:
        try:
            binary_inspections[Path(dll_str).name] = offline_manifest.inspect_pe64_binary(
                package_store.read_bytes(Path(dll_str), package_store.MAX_NATIVE_BYTES)
            )
        except package_store.StoreError:
            pass
    return {
        "project": str(project_dir),
        "buildDir": str(target_build),
        "config": config,
        "autoDiscoveredToolchain": bool(resolved["auto_discovered"]),
        "libraries": libraries,
        "executables": executables,
        "symbols": symbols,
        "binaryInspection": binary_inspections,
    }


def _resolve_harness_binary(harness=None, pulse=None):
    if harness is not None:
        candidate = Path(os.path.abspath(harness))
        if not candidate.is_file():
            raise Failure("harness_not_found", "Specified --harness executable was not found.", 2, path=str(candidate))
        return candidate
    env_harness = os.environ.get("PULSE_PLUGIN_HARNESS", "").strip()
    if env_harness:
        candidate = Path(os.path.abspath(env_harness))
        if candidate.is_file():
            return candidate
    candidates = []
    if pulse is not None:
        candidates.append(Path(os.path.abspath(pulse)).parent / "pulse_plugin_harness.exe")
    candidates.extend([
        ROOT / "build/pulse_plugin_harness.exe",
        Path(__file__).resolve().parent / "pulse_plugin_harness.exe",
        ROOT / "bin/pulse_plugin_harness.exe",
    ])
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def _run_plugin_harness(
    harness_exe,
    manifest_path=None,
    library_path=None,
    package_dir=None,
    invoke_commands=(),
    smoke_commands=False,
    selections=(),
    directory=None,
    omit_experimental_ui=False,
    strict_resources=False,
    timeout_ms=5000,
):
    cmd = [str(harness_exe)]
    if package_dir is not None:
        cmd.extend(["--package", str(package_dir)])
    else:
        cmd.extend(["--manifest", str(manifest_path)])
        if library_path is not None:
            cmd.extend(["--library", str(library_path)])
    for cid in invoke_commands:
        cmd.extend(["--invoke-command", str(cid)])
    if smoke_commands:
        cmd.append("--smoke-commands")
    for sel in selections:
        cmd.extend(["--selection", str(Path(os.path.abspath(sel)))])
    if directory is not None:
        cmd.extend(["--directory", str(Path(os.path.abspath(directory)))])
    if omit_experimental_ui:
        cmd.append("--omit-experimental-ui")
    if strict_resources:
        cmd.append("--strict-resources")
    cmd.extend(["--timeout-ms", str(int(timeout_ms))])

    timeout_sec = max(10.0, (int(timeout_ms) / 1000.0) + 10.0)
    try:
        completed = _ORIGINAL_SUBPROCESS_RUN(cmd, shell=False, capture_output=True, timeout=timeout_sec)
    except subprocess.TimeoutExpired as exc:
        raise Failure("harness_timeout", "Headless plugin harness timed out.", 3) from exc
    except OSError as exc:
        raise Failure("harness_launch_failed", str(exc), 3) from exc

    raw_out = completed.stdout.decode("utf-8", errors="replace").strip()
    try:
        report = json.loads(raw_out)
    except ValueError as exc:
        raise Failure(
            "harness_protocol_error",
            "Headless plugin harness did not emit valid JSON (plugin may have crashed during execution).",
            2,
            exitCode=completed.returncode,
            stdout=raw_out[-4096:],
            stderr=completed.stderr.decode("utf-8", errors="replace")[-4096:],
        ) from exc
    return completed.returncode, report


def _discover_project_library(root_dir, manifest):
    native = manifest.get("native") if isinstance(manifest, dict) else None
    if isinstance(native, dict) and isinstance(native.get("library"), str):
        candidate = root_dir / native["library"]
        if candidate.is_file():
            return candidate
    for subdir in ("", "build", "build-debug", "build-relwithdebinfo"):
        folder = root_dir / subdir if subdir else root_dir
        if folder.is_dir():
            dlls = [p for p in sorted(folder.glob("*.dll")) if p.is_file()]
            if len(dlls) == 1:
                return dlls[0]
            if len(dlls) > 1 and isinstance(manifest.get("id"), str):
                stem = project_creator.library_stem_for_id(manifest["id"]).lower()
                for item in dlls:
                    if stem in item.stem.lower():
                        return item
                return dlls[0]
    return None


def test_package(
    pulse,
    path,
    library=None,
    harness=None,
    invoke_commands=(),
    smoke_commands=False,
    selections=(),
    directory=None,
    omit_experimental_ui=False,
    strict_resources=False,
    timeout_ms=5000,
):
    path = Path(os.path.abspath(path))
    has_runtime_opts = bool(library or harness or invoke_commands or smoke_commands or selections or omit_experimental_ui or strict_resources)

    # Preserve exact legacy contract-only check when given a manifest.json with host and no binary/runtime flags.
    if path.is_file() and path.suffix.lower() == ".json" and not has_runtime_opts:
        _, manifest = checked_snapshot(pulse, path)
        api = host_call(pulse, "--extension-api") if pulse is not None else offline_manifest.offline_api_contract(ROOT)
        if manifest.get("resources"):
            declared = api.get("manifestSchema", {}).get("properties", {}).get("resources", {}).get("items", {}).get("enum", [])
            if any(resource not in declared for resource in manifest["resources"]):
                raise Failure("contract_mismatch", "Host does not advertise the declared native capability.", 2)
        native_decl = manifest.get("native") if isinstance(manifest.get("native"), dict) else None
        inferred_dll = (path.parent / native_decl["library"]) if (native_decl and isinstance(native_decl.get("library"), str)) else None
        if pulse is not None and (inferred_dll is None or not inferred_dll.is_file()):
            return {
                "valid": True,
                "contractCompatible": True,
                "commandCount": 0,
                "panelPresent": False,
                "commandsExecuted": 0,
                "functionalExecutionTested": False,
                "checks": ["hostValidation", "snapshotValidation", "nativeCapabilities"],
            }
        if inferred_dll is None or not inferred_dll.is_file():
            contributed = manifest.get("contributes", {}).get("commands", []) if isinstance(manifest.get("contributes"), dict) else []
            return {
                "valid": True,
                "contractCompatible": True,
                "offlineValidated": pulse is None,
                "commandCount": len(contributed) if isinstance(contributed, list) else 0,
                "panelPresent": False,
                "commandsExecuted": 0,
                "functionalExecutionTested": False,
                "checks": ["offlineValidation" if pulse is None else "hostValidation", "snapshotValidation", "nativeCapabilities"],
            }
        library = inferred_dll

    checks = []
    temp_stage_parent = ROOT / "bench_data" if (ROOT / "bench_data").is_dir() else None
    extracted_temp = None
    try:
        package_dir_for_harness = None
        if path.is_file() and path.suffix.lower() in (".pulse-ext", ".zip"):
            try:
                snapshot = read_package_snapshot(path)
                manifest_bytes = snapshot.manifest_bytes
            except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile, NotImplementedError, zlib.error, package_store.StoreError) as exc:
                raise Failure("invalid_archive", str(exc), 2) from exc
            validate_bytes(pulse, manifest_bytes)
            manifest = decode_json(manifest_bytes)
            checks.extend(["archiveIntegrity", "offlineValidation" if pulse is None else "hostValidation"])
            if snapshot.files:
                extracted_temp = Path(tempfile.mkdtemp(prefix="tmp_ext_test_", dir=str(temp_stage_parent) if temp_stage_parent else None))
                manifest_path = extracted_temp / "manifest.json"
                manifest_path.write_bytes(manifest_bytes)
                for fname, fbytes in snapshot.files.items():
                    (extracted_temp / fname).write_bytes(fbytes)
                library_path = extracted_temp / manifest["native"]["library"]
                package_dir_for_harness = extracted_temp
            else:
                manifest_path = None
                library_path = None
        elif path.is_dir():
            manifest_path = path / "manifest.json"
            if not manifest_path.is_file():
                raise Failure("manifest_not_found", "Directory does not contain manifest.json.", 2, path=str(path))
            _, manifest = checked_snapshot(pulse, manifest_path)
            checks.extend(["offlineValidation" if pulse is None else "hostValidation", "snapshotValidation"])
            library_path = Path(os.path.abspath(library)) if library else _discover_project_library(path, manifest)
            if library_path and library_path.parent == path and isinstance(manifest.get("native"), dict):
                package_dir_for_harness = path
        elif path.is_file() and path.suffix.lower() == ".json":
            manifest_path = path
            _, manifest = checked_snapshot(pulse, manifest_path)
            checks.extend(["offlineValidation" if pulse is None else "hostValidation", "snapshotValidation"])
            library_path = Path(os.path.abspath(library)) if library else _discover_project_library(path.parent, manifest)
        else:
            raise Failure("invalid_path", "Target path must be a .pulse-ext archive, directory, or manifest.json.", 2, path=str(path))

        static_binary = None
        if library_path is not None:
            if not library_path.is_file():
                raise Failure("library_not_found", "Plugin DLL was not found.", 2, path=str(library_path))
            dll_bytes = package_store.read_bytes(library_path, package_store.MAX_NATIVE_BYTES)
            static_binary = offline_manifest.inspect_pe64_binary(dll_bytes)
            checks.append("pe64Exports")
            if not static_binary["isDll"] or not static_binary["hasEntryV1"] or not static_binary["hasSdkPreview"]:
                raise Failure(
                    "abi_exports_missing",
                    "Plugin DLL must be a Windows x64 DLL exporting PulsePluginEntryV1 and PulsePluginSdkPreview.",
                    2,
                    staticBinary=static_binary,
                )

        harness_exe = _resolve_harness_binary(harness=harness, pulse=pulse)
        harness_report = None
        commands_executed = 0
        command_count = len(manifest.get("contributes", {}).get("commands", [])) if isinstance(manifest.get("contributes"), dict) else 0
        if library_path is not None and harness_exe is not None:
            rc, harness_report = _run_plugin_harness(
                harness_exe,
                manifest_path=manifest_path,
                library_path=library_path,
                package_dir=package_dir_for_harness,
                invoke_commands=invoke_commands,
                smoke_commands=smoke_commands,
                selections=selections,
                directory=directory,
                omit_experimental_ui=omit_experimental_ui,
                strict_resources=strict_resources,
                timeout_ms=timeout_ms,
            )
            checks.append("headlessHarnessLifecycle")
            command_count = len(harness_report.get("registeredCommands", []))
            invocations = harness_report.get("commandInvocations", [])
            commands_executed = sum(1 for item in invocations if isinstance(item, dict) and item.get("invoked"))
            if rc != 0 or not harness_report.get("ok"):
                raise Failure(
                    "plugin_test_failed",
                    "; ".join(harness_report.get("errors", [])) or "Headless plugin smoke test failed.",
                    2,
                    harness=harness_report,
                    staticBinary=static_binary,
                )
        elif invoke_commands or smoke_commands:
            raise Failure(
                "harness_not_found",
                "Command smoke execution requires pulse_plugin_harness.exe (pass --harness or set PULSE_PLUGIN_HARNESS).",
                2,
            )

        return {
            "valid": True,
            "contractCompatible": True,
            "offlineValidated": pulse is None,
            "commandCount": command_count,
            "panelPresent": bool(harness_report and harness_report.get("openPanelsAtDeactivate", 0) > 0),
            "commandsExecuted": commands_executed,
            "functionalExecutionTested": harness_report is not None,
            "harnessExecuted": harness_report is not None,
            "unloadClean": harness_report.get("unloadClean") if harness_report is not None else None,
            "staticBinary": static_binary,
            "harness": harness_report,
            "checks": checks,
        }
    finally:
        if extracted_temp is not None and extracted_temp.exists():
            shutil.rmtree(extracted_temp, ignore_errors=True)


def read_package(path):
    """Verify the local archive structure and manifest digest; does not install it."""
    snapshot = read_package_snapshot(path)
    return snapshot.metadata, snapshot.manifest_bytes


def read_package_snapshot(path):
    return native_archive.read(path)


def verify_package(path):
    return read_package(path)[0]


def verify_archive(pulse, path, signature_path=None, policy_path=None):
    try:
        snapshot = read_package_snapshot(path)
        metadata, data = snapshot.metadata, snapshot.manifest_bytes
    except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile, NotImplementedError, zlib.error, package_store.StoreError) as exc:
        raise Failure("invalid_archive", str(exc), 2) from exc
    validate_bytes(pulse, data)
    manifest = json.loads(data)
    command_capabilities = sorted({command["capability"] for command in manifest.get("commands", [])})
    result = {"path": str(path), "package": metadata, "integrityVerified": True,
              "hostValidated": pulse is not None, "offlineValidated": True, "installed": False,
              "signature": {"signed": False, "trusted": False},
              "authorizationReview": {"name": manifest["name"],
                  "commandCapabilities": command_capabilities,
                  "resourcePermissions": sorted(manifest.get("resources", [])),
                  "workflowCount": len(manifest.get("workflows", [])),
                  "grantsAuthority": False,
                  "nativeCode": manifest.get("native"),
                  "nativeExecutionConsentRequired": snapshot.binary_bytes is not None,
                  "nativeFileConfirmationRequired": any(value in command_capabilities for value in
                      ("files.copySelectionToOtherPane", "files.moveSelectionToOtherPane", "workflow.run"))}}
    if snapshot.binary_bytes is not None:
        pe_info = offline_manifest.inspect_pe64_binary(snapshot.binary_bytes)
        result["nativeBinary"] = {
            "hasEntryV1": pe_info["hasEntryV1"],
            "hasSdkPreview": pe_info["hasSdkPreview"],
            "guiImports": pe_info["guiImports"],
        }
    if signature_path or policy_path:
        import package_signing
        result["signature"] = package_signing.verify(data,
            package_signing.parse(package_store.read_bytes(signature_path, 16384)) if signature_path else None,
            package_signing.parse(package_store.read_bytes(policy_path, 65536)) if policy_path else None)
    return result


def pack_package(pulse, path, output):
    if os.path.lexists(output):
        raise Failure("destination_exists", "Archive output already exists.", 2)
    data, manifest = checked_snapshot(pulse, path)
    native = package_store.native_declaration(manifest)
    files = package_store.read_native_files(path.parent, native) if native else None
    binary = files[native["library"]] if native else None
    metadata = native_archive.metadata_for(data, manifest)
    snapshot = native_archive.PackageSnapshot(metadata, data, binary, files)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            native_archive.write(stream, snapshot)
            stream.flush()
            os.fsync(stream.fileno())
        verify_package(temporary)
        # Publish the complete file atomically; hard-link creation never replaces an existing output.
        os.link(temporary, output)
    except FileExistsError as exc:
        raise Failure("destination_exists", "Archive output appeared while packing; it was preserved.", 2) from exc
    except OSError as exc:
        raise Failure("pack_write_failed", str(exc)) from exc
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return {"path": str(output), "package": metadata, "verified": True,
            "installed": False, "hostInstallSupported": False, "localInstallSupported": True}


def create_new_project(pulse, destination, identifier, name, template, sdk_root=None, validate_with_host=False):
    sdk_root = Path(os.path.abspath(sdk_root or ROOT))
    if validate_with_host and pulse is not None:
        validator = lambda data: validate_bytes(pulse, data)
    else:
        validator = lambda data: validate_bytes(None, data)
    try:
        created = project_creator.create_project(
            sdk_root,
            destination,
            identifier,
            name,
            template,
            pre_create_validate=validator,
        )
    except project_creator.ProjectCreationError as exc:
        raise Failure(exc.code, str(exc), 2, **exc.details) from exc
    created["hostValidated"] = bool(validate_with_host and pulse is not None)
    created["offlineValidated"] = True
    return created


def _probe_tool(command, timeout=10):
    try:
        completed = subprocess.run(command, shell=False, capture_output=True, timeout=timeout)
        stdout = completed.stdout.decode("utf-8", errors="replace")
        stderr = completed.stderr.decode("utf-8", errors="replace")
        return completed.returncode, stdout, stderr
    except (OSError, subprocess.SubprocessError):
        return None, "", ""


def inspect_toolchain(auto_discover=True):
    return msvc_env.inspect_toolchain(auto_discover=auto_discover)


def _inspect_manifest_against_host(pulse, manifest_path, api, host_arch="windows-x64"):
    abs_path = Path(os.path.abspath(manifest_path))
    try:
        raw = package_store.read_bytes(abs_path, 65536)
        manifest = decode_json(raw)
    except package_store.StoreError as exc:
        return {"valid": False, "hostValid": False, "code": exc.code,
                "reason": "Manifest file could not be read safely"}
    except (OSError, UnicodeError, ValueError):
        return {"valid": False, "hostValid": False, "code": "invalid_manifest",
                "reason": "Manifest is not strict UTF-8 JSON"}
    if not isinstance(manifest, dict):
        return {"valid": False, "hostValid": False, "code": "invalid_manifest",
                "reason": "Manifest root must be a JSON object"}

    native = manifest.get("native")
    native_arch = native.get("architecture") if isinstance(native, dict) else None
    arch_compatible = native_arch in (None, host_arch)

    resources = manifest.get("resources", [])
    if not isinstance(resources, list) or any(not isinstance(item, str) for item in resources):
        return {"valid": False, "hostValid": False, "code": "invalid_manifest",
                "reason": "Manifest resources must be an array of strings"}
    host_resources = (
        api.get("manifestSchema", {})
        .get("properties", {})
        .get("resources", {})
        .get("items", {})
        .get("enum", [])
    )
    unsupported_resources = [item for item in resources if host_resources and item not in host_resources]

    if pulse is None:
        validation = offline_manifest.validate_manifest_file(abs_path)
    else:
        validation = host_call(pulse, "--extension-validate-json", abs_path)
    host_valid = bool(validation.get("valid"))
    valid = host_valid and arch_compatible and not unsupported_resources
    result = {
        "valid": valid,
        "hostValid": host_valid if pulse is not None else False,
        "offlineValid": host_valid if pulse is None else True,
        "architecture": native_arch or "declarative",
        "architectureCompatible": arch_compatible,
        "resourceCount": len(resources),
        "unsupportedResources": unsupported_resources,
        "contributedCommandCount": len(manifest.get("contributes", {}).get("commands", []))
        if isinstance(manifest.get("contributes"), dict) and isinstance(manifest.get("contributes", {}).get("commands"), list)
        else 0,
    }
    if not valid:
        if not arch_compatible:
            result["code"] = "architecture_mismatch"
            result["reason"] = f"Package architecture '{native_arch}' does not match host '{host_arch}'"
        elif unsupported_resources:
            result["code"] = "contract_mismatch"
            result["reason"] = "Host does not advertise declared resources: " + ", ".join(unsupported_resources)
        else:
            result["code"] = validation.get("code") or "invalid_manifest"
            result["reason"] = "Manifest validation failed" if pulse is None else "Host rejected manifest validation"
    return result


def doctor(pulse, check_toolchain=False, manifest_path=None, sdk_root=None):
    checks = {"python": {"compatible": sys.version_info >= (3, 8),
                         "version": ".".join(str(part) for part in sys.version_info[:3])}}
    if pulse is None:
        api = offline_manifest.offline_api_contract(sdk_root or ROOT)
        sdk_catalog = api.get("nativeSdk", {}).get("catalog") if isinstance(api.get("nativeSdk"), dict) else None
        sdk_preview = sdk_catalog.get("sdkPreview") if isinstance(sdk_catalog, dict) else offline_manifest.SDK_PREVIEW_VERSION
        checks["host"] = {
            "reachable": False,
            "offlineMode": True,
            "contractCompatible": True,
            "apiVersion": api["apiVersion"],
            "sdkPreview": sdk_preview,
            "architecture": "windows-x64",
        }
    else:
        try:
            api = host_call(pulse, "--extension-api")
            sdk_catalog = api.get("nativeSdk", {}).get("catalog") if isinstance(api.get("nativeSdk"), dict) else None
            sdk_preview = sdk_catalog.get("sdkPreview") if isinstance(sdk_catalog, dict) else None
            checks["host"] = {
                "reachable": True,
                "contractCompatible": True,
                "apiVersion": api["apiVersion"],
                "sdkPreview": sdk_preview,
                "architecture": "windows-x64",
                "capabilityCount": len(api["capabilities"]),
                "bindingCount": len(api["bindings"]),
            }
        except Failure as exc:
            checks["host"] = {"reachable": False, "contractCompatible": False, "code": exc.error["code"]}
            raise Failure("doctor_failed", "Host compatibility check failed; diagnostic details contain no local paths.",
                          3, checks=checks) from exc

    healthy = checks["python"]["compatible"]
    if check_toolchain:
        checks["toolchain"] = inspect_toolchain()
        healthy = healthy and checks["toolchain"]["ready"]
    if manifest_path is not None:
        checks["manifest"] = _inspect_manifest_against_host(pulse, manifest_path, api)
        healthy = healthy and checks["manifest"]["valid"]
    return {"healthy": healthy, "checks": checks,
            "pathsIncluded": False, "environmentIncluded": False}


def develop(pulse, path, profile):
    validated(pulse, path)
    api = host_call(pulse, "--extension-api")
    if "isolatedDevReload" not in api.get("developerFeatures", []):
        raise Failure("dev_unsupported", "This host does not advertise isolated development reload.", 3)
    try:
        profile.mkdir(exist_ok=False)
    except FileExistsError as exc:
        raise Failure("destination_exists", "Development profile must be a new directory.", 2) from exc
    except OSError as exc:
        raise Failure("profile_failed", str(exc)) from exc
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PULSE_")}
    environment["PULSE_TEST_DATA_DIR"] = str(profile)
    command = [str(pulse), "--test-instance", "--extension-dev", str(path)]
    # Keep child output out of the CLI JSON stream; diagnostics remain in this new profile.
    process = None
    interrupted = False
    try:
        with (profile / "extension-dev-process.log").open("xb") as log:
            process = subprocess.Popen(command, shell=False, env=environment, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                exit_code = process.wait()
            except KeyboardInterrupt:
                interrupted = True
                process.terminate()
                try:
                    exit_code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    exit_code = process.wait()
    except OSError as exc:
        raise Failure("dev_launch_failed", str(exc), profile=str(profile)) from exc
    result = {"profile": str(profile), "diagnostic": str(profile / "extension-dev-status.json"),
              "processLog": str(profile / "extension-dev-process.log"), "exitCode": exit_code,
              "stoppedByUser": interrupted, "isolated": True, "reloadVerified": False}
    if exit_code != 0 and not interrupted:
        raise Failure("dev_process_failed", "Development host exited unsuccessfully; inspect the isolated diagnostics.",
                      3, dev=result)
    return result


def install_archive(pulse, archive, store, accept_capabilities, signature_path=None, allow_native=False,
                    rotation_path=None, allow_publisher_change=False):
    signature_bytes = package_store.read_bytes(signature_path, 16384) if signature_path else None
    rotation_bytes = package_store.read_bytes(rotation_path, 16384) if rotation_path else None
    if archive.suffix.lower() == ".json":
        data, _ = checked_snapshot(pulse, archive)
        return package_store.install(
            store, data, accept_capabilities, signature_bytes,
            source_dir=archive.parent, allow_native=allow_native,
            rotation=rotation_bytes, allow_publisher_change=allow_publisher_change,
        )
    try:
        snapshot = read_package_snapshot(archive)
        data = snapshot.manifest_bytes
    except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile, NotImplementedError, zlib.error, package_store.StoreError) as exc:
        raise Failure("invalid_archive", str(exc), 2) from exc
    validate_bytes(pulse, data)
    return package_store.install(
        store, data, accept_capabilities, signature_bytes,
        allow_native=allow_native, binary_data=snapshot.binary_bytes, file_data=snapshot.files,
        rotation=rotation_bytes, allow_publisher_change=allow_publisher_change,
    )


def author_native_manifest(pulse, manifest_path, library, output, files=()):
    if os.path.lexists(output):
        raise Failure("destination_exists", "Manifest output already exists.", 2)
    if library.parent != output.parent:
        raise Failure("native_library_location", "The output manifest must share its directory with the DLL.", 2)
    for ancestor in (library.parent, *library.parent.parents):
        package_store.ordinary(ancestor)
    manifest = package_store.strict_json(package_store.read_bytes(manifest_path, 65536))
    data = package_store.read_bytes(library, package_store.MAX_NATIVE_BYTES)
    sha = package_store.validate_native_binary(data)
    manifest["native"] = {"version": 1, "architecture": "windows-x64", "library": library.name, "sha256": sha}
    if files:
        manifest["native"]["files"] = []
        for path in files:
            path = Path(os.path.abspath(path))
            if path.parent != library.parent:
                raise Failure("native_library_location", "Additional files must share the DLL directory.", 2)
            content = package_store.read_bytes(path, package_store.MAX_NATIVE_BYTES)
            manifest["native"]["files"].append({"path": path.name, "sha256": hashlib.sha256(content).hexdigest()})
    package_store.native_declaration(manifest)
    package_store.read_native_files(library.parent, manifest["native"])
    serialized = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if len(serialized) > 65536:
        raise Failure("too_large", "Manifest exceeds 64 KiB.", 2)
    validate_bytes(pulse, serialized)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=".native-manifest-", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(serialized)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return {"path": str(output), "library": str(library), "binarySha256": sha,
            "manifestSha256": hashlib.sha256(serialized).hexdigest(), "architecture": "windows-x64",
            "hostValidated": pulse is not None, "offlineValidated": True,
            "installed": False, "executed": False, "grantsExecution": False}


def _win32_process_start_time_and_image(handle):
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    creation = wintypes.FILETIME()
    exit_time = wintypes.FILETIME()
    kernel_time = wintypes.FILETIME()
    user_time = wintypes.FILETIME()
    start_ft = None
    if kernel32.GetProcessTimes(wintypes.HANDLE(handle), ctypes.byref(creation), ctypes.byref(exit_time),
                                ctypes.byref(kernel_time), ctypes.byref(user_time)):
        start_ft = (int(creation.dwHighDateTime) << 32) | int(creation.dwLowDateTime)
    buf_len = wintypes.DWORD(32768)
    buf = ctypes.create_unicode_buffer(32768)
    image = None
    if kernel32.QueryFullProcessImageNameW(wintypes.HANDLE(handle), 0, buf, ctypes.byref(buf_len)):
        image = buf.value
    return start_ft, image


def _process_start_filetime(pid):
    if os.name != "nt" or type(pid) is not int or pid <= 0:
        return None
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return None
    try:
        start_ft, _ = _win32_process_start_time_and_image(handle)
        return start_ft
    finally:
        kernel32.CloseHandle(handle)


def _query_process_command_line(pid):
    if os.name != "nt" or type(pid) is not int or pid <= 0:
        return None
    pwsh = shutil.which("pwsh") or shutil.which("powershell")
    if not pwsh:
        return None
    script = f"$p = Get-CimInstance Win32_Process -Filter 'ProcessId = {int(pid)}' -ErrorAction SilentlyContinue; if ($p) {{ $p.CommandLine }}"
    code, stdout, _ = _probe_tool([pwsh, "-NoProfile", "-NonInteractive", "-Command", script], timeout=10)
    if code != 0:
        return None
    return (stdout or "").strip() or None


def _post_close_to_pid_windows(pid):
    if os.name != "nt":
        return 0
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    signaled = []

    @WNDENUMPROC
    def callback(hwnd, _lparam):
        owner_pid = wintypes.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner_pid))
        if int(owner_pid.value) == pid:
            if user32.PostMessageW(hwnd, 0x0010, 0, 0):  # WM_CLOSE
                signaled.append(int(hwnd))
        return True

    user32.EnumWindows(callback, 0)
    return len(signaled)


def stop_host(pulse, pid, profile=None, timeout=10.0):
    if type(pid) is not int or isinstance(pid, bool) or pid <= 0:
        raise Failure("invalid_pid", "Process ID must be a positive integer.", 2)
    if timeout <= 0 or timeout > 120:
        raise Failure("invalid_timeout", "Timeout must be between 0 and 120 seconds.", 2)

    expected_host = Path(os.path.abspath(pulse))
    expected_start_ft = None
    record_path = None
    verified_by = "command_line"

    if profile is not None:
        profile = Path(os.path.abspath(profile))
        for ancestor in (profile, *profile.parents):
            package_store.ordinary(ancestor)
        record_path = profile / "extension-host-instance.json"
        package_store.ordinary(record_path)
        if not record_path.is_file():
            raise Failure("instance_not_found", "Profile has no recorded extension host instance.", 2, profile=str(profile))
        record = package_store.strict_json(package_store.read_bytes(record_path, 16384))
        if (not isinstance(record, dict) or record.get("formatVersion") != 1 or
                type(record.get("processId")) is not int):
            raise Failure("invalid_instance_record", "Recorded host instance metadata is malformed.", 2, profile=str(profile))
        if record["processId"] != pid:
            raise Failure("pid_mismatch", "Requested PID does not match the recorded profile host instance.", 2,
                          pid=pid, expectedPid=record["processId"])
        if isinstance(record.get("hostPath"), str) and record["hostPath"]:
            expected_host = Path(os.path.abspath(record["hostPath"]))
        if type(record.get("processStartFileTime")) is int:
            expected_start_ft = record["processStartFileTime"]
        verified_by = "profile"

    if os.name != "nt":
        raise Failure("unsupported_platform", "Cooperative host shutdown requires Windows.", 3)

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.GetExitCodeProcess.restype = wintypes.BOOL
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.WaitForSingleObject.restype = wintypes.DWORD

    # SYNCHRONIZE (0x00100000) | PROCESS_QUERY_LIMITED_INFORMATION (0x00001000) — never PROCESS_TERMINATE!
    handle = kernel32.OpenProcess(0x00101000, False, pid)
    if not handle:
        err = ctypes.get_last_error()
        if err == 87:  # ERROR_INVALID_PARAMETER -> PID does not exist
            if profile is not None:
                return {"stopped": True, "alreadyExited": True, "processId": pid,
                        "cooperative": True, "forceKilled": False, "verifiedBy": verified_by}
            raise Failure("process_not_found", "Target process is not running.", 2, pid=pid)
        raise Failure("access_denied", "Cannot inspect target process.", 3, pid=pid, winerror=err)

    try:
        exit_code = wintypes.DWORD(0)
        if kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)) and int(exit_code.value) != 259:
            if profile is not None:
                return {"stopped": True, "alreadyExited": True, "processId": pid,
                        "exitCode": int(exit_code.value), "cooperative": True,
                        "forceKilled": False, "verifiedBy": verified_by}
            raise Failure("process_not_found", "Target process has already exited.", 2, pid=pid)

        actual_start_ft, actual_image = _win32_process_start_time_and_image(handle)
        if expected_start_ft is not None and actual_start_ft is not None and actual_start_ft != expected_start_ft:
            raise Failure("pid_reused",
                          "Process start time does not match the recorded host instance; refusing to signal recycled PID.",
                          2, pid=pid)

        if profile is None or expected_start_ft is None:
            cmdline = _query_process_command_line(pid) or ""
            test_flags = ("--test-instance", "--extension-store", "--extension-dev")
            has_test_flag = any(flag in cmdline for flag in test_flags)
            image_matches = False
            if actual_image:
                actual_path = Path(actual_image)
                if actual_path.name.lower() == expected_host.name.lower():
                    image_matches = True
                elif expected_host.suffix.lower() in (".cmd", ".bat", ".py", ".ps1") and str(expected_host).lower() in cmdline.lower():
                    image_matches = True
            if profile is None and (not image_matches or not has_test_flag):
                raise Failure("not_test_host",
                              "Target process is not an isolated Pulse extension test/store host; refusing to close.",
                              2, pid=pid)
            if profile is not None and not image_matches and not has_test_flag:
                raise Failure("not_test_host",
                              "Recorded PID does not match the configured Pulse host image or flags; refusing to close.",
                              2, pid=pid)

        if profile is not None:
            try:
                signal_path = profile / "extension-host-stop.signal"
                package_store.ordinary(signal_path)
                signal_path.write_bytes(b"stop\n")
            except OSError:
                pass

        deadline = time.monotonic() + timeout
        windows_signaled = 0
        while True:
            windows_signaled += _post_close_to_pid_windows(pid)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            slice_ms = max(1, min(250, int(remaining * 1000)))
            wait_res = int(kernel32.WaitForSingleObject(handle, slice_ms))
            if wait_res == 0:  # WAIT_OBJECT_0
                final_exit = wintypes.DWORD(0)
                kernel32.GetExitCodeProcess(handle, ctypes.byref(final_exit))
                return {
                    "stopped": True,
                    "alreadyExited": False,
                    "processId": pid,
                    "exitCode": int(final_exit.value),
                    "windowsSignaled": windows_signaled,
                    "cooperative": True,
                    "forceKilled": False,
                    "verifiedBy": verified_by,
                }

        raise Failure(
            "stop_timeout",
            "Host process did not exit cooperatively within timeout; unrelated or unresponsive processes are never force-killed.",
            3,
            pid=pid,
            windowsSignaled=windows_signaled,
        )
    finally:
        kernel32.CloseHandle(handle)


def run_store(pulse, store, profile=None):
    registry = package_store.listing(store)
    if not (store / "registry.json").is_file():
        raise Failure("invalid_store", "Store has no registry; install a package before launching it.", 2)
    api = host_call(pulse, "--extension-api")
    if "nativePackages" not in api.get("developerFeatures", []):
        raise Failure("store_unsupported", "Host does not advertise native packages.")
    for entry in registry["packages"]:
        if not entry["enabled"]:
            continue
        data, manifest = package_store.revision(store, entry["activeRevision"])
        if manifest["id"] != entry["id"] or manifest["name"] != entry["name"]:
            raise package_store.StoreError("invalid_store", "Active revision identity does not match registry")
        package_store.native_binary(store, manifest, entry["activeRevision"])
        validate_bytes(pulse, data)
    command = [str(pulse), "--extension-store", str(store)]
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PULSE_")}
    if profile is not None:
        try:
            profile.mkdir(exist_ok=False)
        except FileExistsError as exc:
            raise Failure("destination_exists", "Run profile must be a new directory.", 2) from exc
        environment["PULSE_TEST_DATA_DIR"] = str(profile)
        command.append("--test-instance")
    log_path = None
    try:
        log_stream = ((profile / "extension-store-process.log").open("xb") if profile else
                      tempfile.NamedTemporaryFile(prefix="pulse-extension-run-", suffix=".log", delete=False))
        with log_stream as log:
            log_path = str(Path(log.name))
            process = subprocess.Popen(command, shell=False, env=environment, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT)
    except OSError as exc:
        raise Failure("store_launch_failed", str(exc), processLog=log_path) from exc
    result = {"launched": True, "processId": process.pid, "store": str(store),
              "enabledPackages": sum(item["enabled"] for item in registry["packages"]),
              "isolated": profile is not None, "loadVerified": False, "processLog": log_path}
    if profile is not None:
        instance_record = profile / "extension-host-instance.json"
        record_payload = {
            "formatVersion": 1,
            "processId": process.pid,
            "hostPath": str(Path(os.path.abspath(pulse))),
            "store": str(store),
            "profile": str(profile),
            "processStartFileTime": _process_start_filetime(process.pid),
        }
        with instance_record.open("xb") as record_stream:
            record_stream.write((json.dumps(record_payload, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
        result["profile"] = str(profile)
        result["instanceRecord"] = str(instance_record)
    return result


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    machine = "--json" in argv
    explicit_pulse = any(item == "--pulse" or item.startswith("--pulse=") for item in argv)
    command = next((item for item in argv if item in (
        "inspect-sdk", "compare-sdk", "validate", "init", "new", "build", "test", "pack", "verify",
        "doctor", "dev", "install", "list", "enable", "disable", "rollback", "uninstall",
        "gc", "run", "stop-host", "native-manifest"
    )), None)
    parser = Parser(description=__doc__)
    parser.add_argument("--pulse", type=Path, default=ROOT / "build/pulse.exe")
    parser.add_argument("--offline", action="store_true", help="Validate manifests and stage packages offline without launching pulse.exe")
    parser.add_argument("--json", action="store_true")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("inspect-sdk")
    compare_sdk = commands.add_parser("compare-sdk")
    compare_sdk.add_argument("baseline", type=Path)
    compare_sdk.add_argument("--require-identical", action="store_true")
    doctor_cmd = commands.add_parser("doctor")
    doctor_cmd.add_argument("--toolchain", action="store_true")
    doctor_cmd.add_argument("--manifest", type=Path)
    doctor_cmd.add_argument("--sdk-root", type=Path, default=ROOT)
    native_manifest = commands.add_parser("native-manifest")
    native_manifest.add_argument("path")
    native_manifest.add_argument("--library", required=True)
    native_manifest.add_argument("--output", required=True)
    native_manifest.add_argument("--file", action="append", default=[], help="Additional same-directory file; repeat for each dependency or resource")
    install = commands.add_parser("install")
    install.add_argument("path")
    install.add_argument("--store")
    install.add_argument("--accept-capabilities", action="store_true")
    install.add_argument("--allow-native", action="store_true")
    install.add_argument("--signature", type=Path)
    install.add_argument("--rotation", type=Path)
    install.add_argument("--allow-publisher-change", action="store_true")
    installed = commands.add_parser("list")
    installed.add_argument("--store")
    for operation in ("enable", "disable", "rollback", "uninstall"):
        item = commands.add_parser(operation)
        item.add_argument("id")
        item.add_argument("--store")
        if operation in ("enable", "rollback"):
            item.add_argument("--allow-native", action="store_true")
        if operation == "rollback":
            item.add_argument("--accept-capabilities", action="store_true")
            item.add_argument("--rotation", type=Path)
            item.add_argument("--allow-publisher-change", action="store_true")
        if operation == "uninstall":
            item.add_argument("--purge-data", action="store_true")
    gc_cmd = commands.add_parser("gc")
    gc_cmd.add_argument("--store")
    run = commands.add_parser("run")
    run.add_argument("--store")
    run.add_argument("--profile")
    stop_cmd = commands.add_parser("stop-host")
    stop_cmd.add_argument("--pid", type=int, required=True)
    stop_cmd.add_argument("--profile", type=Path)
    stop_cmd.add_argument("--timeout", type=float, default=10.0)
    build_cmd = commands.add_parser("build")
    build_cmd.add_argument("project")
    build_cmd.add_argument("--config", choices=BUILD_CONFIGS, default="Release")
    build_cmd.add_argument("--build-dir", type=Path)
    build_cmd.add_argument("--sdk-root", type=Path, default=ROOT)
    test = commands.add_parser("test")
    test.add_argument("path")
    test.add_argument("--library", type=Path)
    test.add_argument("--harness", type=Path)
    test.add_argument("--invoke-command", action="append", default=[])
    test.add_argument("--smoke-commands", action="store_true")
    test.add_argument("--selection", action="append", default=[])
    test.add_argument("--directory", type=Path)
    test.add_argument("--omit-experimental-ui", action="store_true")
    test.add_argument("--strict-resources", action="store_true")
    test.add_argument("--timeout-ms", type=int, default=5000)
    pack = commands.add_parser("pack")
    pack.add_argument("path")
    pack.add_argument("--output", required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("path")
    verify.add_argument("--signature", type=Path)
    verify.add_argument("--policy", type=Path)
    dev = commands.add_parser("dev")
    dev.add_argument("path")
    dev.add_argument("--profile", required=True)
    validate = commands.add_parser("validate")
    validate.add_argument("path")
    init = commands.add_parser("init")
    init.add_argument("destination")
    init.add_argument("--id", required=True)
    init.add_argument("--name", required=True)
    init.add_argument("--template", choices=TEMPLATES, default="inspector")
    new_cmd = commands.add_parser("new")
    new_cmd.add_argument("destination")
    new_cmd.add_argument("--id", required=True)
    new_cmd.add_argument("--name", required=True)
    new_cmd.add_argument("--template", default="rename-panel")
    new_cmd.add_argument("--sdk-root", type=Path, default=ROOT)
    try:
        args = parser.parse_args(argv)
        command = args.command
        if explicit_pulse and args.offline:
            raise Failure("usage", "--pulse and --offline cannot be combined.", 2)
        use_host = (explicit_pulse and not args.offline) or (
            not explicit_pulse and not args.offline and subprocess.run is not _ORIGINAL_SUBPROCESS_RUN
        )
        validation_host = args.pulse if use_host else None
        if hasattr(args, "store"):
            args.store = args.store or str(package_store.default_root())
        if command == "inspect-sdk":
            payload = {"api": host_call(args.pulse, "--extension-api")}
        elif command == "compare-sdk":
            try:
                baseline = contract_compat.load(package_store.read_bytes(args.baseline.absolute(), contract_compat.MAX_BYTES))
                comparison = contract_compat.compare(baseline, host_call(args.pulse, "--extension-api"))
            except ValueError as exc:
                raise Failure("invalid_contract", str(exc), 2) from exc
            if args.require_identical and not comparison["identical"]:
                raise Failure("contract_changed", "Discovery changed; review the differences and validate manifests with the target host.", 2, comparison=comparison)
            payload = {"comparison": comparison}
        elif command == "validate":
            payload = {"validation": validated(validation_host, Path(os.path.abspath(args.path)))}
        elif command == "build":
            payload = {"build": build_project(
                Path(os.path.abspath(args.project)),
                config=args.config,
                build_dir=Path(os.path.abspath(args.build_dir)) if args.build_dir else None,
                sdk_root=args.sdk_root,
            )}
        elif command == "test":
            payload = {"test": test_package(
                validation_host,
                Path(os.path.abspath(args.path)),
                library=args.library,
                harness=args.harness,
                invoke_commands=args.invoke_command,
                smoke_commands=args.smoke_commands,
                selections=args.selection,
                directory=args.directory,
                omit_experimental_ui=args.omit_experimental_ui,
                strict_resources=args.strict_resources,
                timeout_ms=args.timeout_ms,
            )}
        elif command == "pack":
            payload = {"archive": pack_package(validation_host, Path(os.path.abspath(args.path)), Path(os.path.abspath(args.output)))}
        elif command == "verify":
            payload = {"verification": verify_archive(validation_host, Path(os.path.abspath(args.path)), args.signature, args.policy)}
        elif command == "doctor":
            payload = {"doctor": doctor(
                validation_host,
                check_toolchain=args.toolchain,
                manifest_path=args.manifest,
                sdk_root=args.sdk_root,
            )}
        elif command == "dev":
            payload = {"dev": develop(args.pulse, Path(os.path.abspath(args.path)), Path(os.path.abspath(args.profile)))}
        elif command == "install":
            payload = {"installation": install_archive(
                validation_host,
                Path(os.path.abspath(args.path)),
                Path(os.path.abspath(args.store)),
                args.accept_capabilities,
                Path(os.path.abspath(args.signature)) if args.signature else None,
                args.allow_native,
                rotation_path=Path(os.path.abspath(args.rotation)) if args.rotation else None,
                allow_publisher_change=args.allow_publisher_change,
            )}
        elif command == "native-manifest":
            payload = {"nativeManifest": author_native_manifest(validation_host, Path(os.path.abspath(args.path)),
                Path(os.path.abspath(args.library)), Path(os.path.abspath(args.output)), args.file)}
        elif command == "list":
            payload = {"registry": package_store.listing(Path(os.path.abspath(args.store)))}
        elif command == "gc":
            payload = {"gc": package_store.gc(Path(os.path.abspath(args.store)))}
        elif command in ("enable", "disable", "rollback", "uninstall"):
            rotation_bytes = (
                package_store.read_bytes(Path(os.path.abspath(args.rotation)), 16384)
                if getattr(args, "rotation", None) else None
            )
            payload = {"update": package_store.update(
                Path(os.path.abspath(args.store)),
                args.id,
                command,
                validate=lambda data: validate_bytes(validation_host, data),
                allow_native=getattr(args, "allow_native", False),
                accept_capabilities=getattr(args, "accept_capabilities", False),
                rotation=rotation_bytes,
                allow_publisher_change=getattr(args, "allow_publisher_change", False),
                purge_data=getattr(args, "purge_data", False),
            )}
        elif command == "run":
            payload = {"run": run_store(args.pulse, Path(os.path.abspath(args.store)),
                        Path(os.path.abspath(args.profile)) if args.profile else None)}
        elif command == "stop-host":
            payload = {"stop": stop_host(
                args.pulse,
                args.pid,
                Path(os.path.abspath(args.profile)) if args.profile else None,
                timeout=args.timeout,
            )}
        elif command == "new":
            created = create_new_project(
                validation_host,
                args.destination,
                args.id,
                args.name,
                args.template,
                sdk_root=args.sdk_root,
                validate_with_host=use_host,
            )
            payload = {"project": created, **created}
        else:
            payload = initialize(validation_host, args.destination, args.id, args.name, args.template)
        result, exit_code = {"ok": True, "command": command, **payload}, 0
    except package_store.StoreError as exc:
        result, exit_code = {"ok": False, "command": command, "error": {"code": exc.code, "message": str(exc)}}, 2
    except Failure as exc:
        result, exit_code = {"ok": False, "command": command, "error": exc.error}, exc.exit_code
        if exc.error["code"] == "help":
            result = {"ok": True, "command": command, "help": str(exc)}
    except (OSError, ValueError) as exc:
        result, exit_code = {"ok": False, "command": command,
                             "error": {"code": "io_error", "message": str(exc)}}, 3
    if machine:
        print(json.dumps(result, ensure_ascii=True))
    elif "help" in result:
        print(result["help"], end="")
    elif result["ok"]:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print(f"{result['error']['code']}: {result['error']['message']}", file=sys.stderr)
    return exit_code


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    raise SystemExit(main())
