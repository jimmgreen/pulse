"""Development CLI for the public Pulse extension contract (Python stdlib only)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile
import zlib
import package_store
import native_archive
import template_catalog
import contract_compat

ROOT = Path(__file__).resolve().parents[2]
HOST_TIMEOUT = 30
TEMPLATES = template_catalog.names()


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
        host = Path(os.path.abspath(pulse))
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
    return json.loads(data.decode("utf-8"), object_pairs_hook=strict_object,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))


def validate_bytes(pulse, data):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        validated(pulse, temporary)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def test_package(pulse, path):
    _, manifest = checked_snapshot(pulse, path)
    api = host_call(pulse, "--extension-api")
    if manifest.get("resources"):
        declared = api.get("manifestSchema", {}).get("properties", {}).get("resources", {}).get("items", {}).get("enum", [])
        if any(resource not in declared for resource in manifest["resources"]):
            raise Failure("contract_mismatch", "Host does not advertise the declared native capability.", 2)
    return {"valid": True, "contractCompatible": True, "commandCount": 0,
            "panelPresent": False, "commandsExecuted": 0, "functionalExecutionTested": False,
            "checks": ["hostValidation", "snapshotValidation", "nativeCapabilities"]}


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
              "hostValidated": True, "installed": False,
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


def doctor(pulse):
    checks = {"python": {"compatible": sys.version_info >= (3, 8),
                         "version": ".".join(str(part) for part in sys.version_info[:3])}}
    try:
        api = host_call(pulse, "--extension-api")
        checks["host"] = {"reachable": True, "contractCompatible": True, "apiVersion": api["apiVersion"],
                          "capabilityCount": len(api["capabilities"]), "bindingCount": len(api["bindings"])}
    except Failure as exc:
        checks["host"] = {"reachable": False, "contractCompatible": False, "code": exc.error["code"]}
        raise Failure("doctor_failed", "Host compatibility check failed; diagnostic details contain no local paths.",
                      3, checks=checks) from exc
    return {"healthy": checks["python"]["compatible"], "checks": checks,
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


def install_archive(pulse, archive, store, accept_capabilities, signature_path=None, allow_native=False):
    if archive.suffix.lower() == ".json":
        data, _ = checked_snapshot(pulse, archive)
        return package_store.install(store, data, accept_capabilities,
            package_store.read_bytes(signature_path, 16384) if signature_path else None,
            source_dir=archive.parent, allow_native=allow_native)
    try:
        snapshot = read_package_snapshot(archive)
        data = snapshot.manifest_bytes
    except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile, NotImplementedError, zlib.error, package_store.StoreError) as exc:
        raise Failure("invalid_archive", str(exc), 2) from exc
    validate_bytes(pulse, data)
    return package_store.install(store, data, accept_capabilities, package_store.read_bytes(signature_path, 16384) if signature_path else None,
                                 allow_native=allow_native, binary_data=snapshot.binary_bytes, file_data=snapshot.files)


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
            "hostValidated": True, "installed": False, "executed": False, "grantsExecution": False}


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
    return {"launched": True, "processId": process.pid, "store": str(store),
            "enabledPackages": sum(item["enabled"] for item in registry["packages"]),
            "isolated": profile is not None, "loadVerified": False, "processLog": log_path}


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    machine = "--json" in argv
    command = next((item for item in argv if item in ("inspect-sdk", "compare-sdk", "validate", "init", "test", "pack", "verify", "doctor", "dev", "install", "list", "enable", "disable", "rollback", "uninstall", "run", "native-manifest")), None)
    parser = Parser(description=__doc__)
    parser.add_argument("--pulse", type=Path, default=ROOT / "build/pulse.exe")
    parser.add_argument("--json", action="store_true")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("inspect-sdk")
    compare_sdk = commands.add_parser("compare-sdk")
    compare_sdk.add_argument("baseline", type=Path)
    compare_sdk.add_argument("--require-identical", action="store_true")
    commands.add_parser("doctor")
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
    installed = commands.add_parser("list")
    installed.add_argument("--store")
    for operation in ("enable", "disable", "rollback", "uninstall"):
        item = commands.add_parser(operation)
        item.add_argument("id")
        item.add_argument("--store")
        if operation in ("enable", "rollback"):
            item.add_argument("--allow-native", action="store_true")
    run = commands.add_parser("run")
    run.add_argument("--store")
    run.add_argument("--profile")
    test = commands.add_parser("test")
    test.add_argument("path")
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
    try:
        args = parser.parse_args(argv)
        command = args.command
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
            payload = {"validation": validated(args.pulse, Path(os.path.abspath(args.path)))}
        elif command == "test":
            payload = {"test": test_package(args.pulse, Path(os.path.abspath(args.path)))}
        elif command == "pack":
            payload = {"archive": pack_package(args.pulse, Path(os.path.abspath(args.path)), Path(os.path.abspath(args.output)))}
        elif command == "verify":
            payload = {"verification": verify_archive(args.pulse, Path(os.path.abspath(args.path)), args.signature, args.policy)}
        elif command == "doctor":
            payload = {"doctor": doctor(args.pulse)}
        elif command == "dev":
            payload = {"dev": develop(args.pulse, Path(os.path.abspath(args.path)), Path(os.path.abspath(args.profile)))}
        elif command == "install":
            payload = {"installation": install_archive(args.pulse, Path(os.path.abspath(args.path)),
                        Path(os.path.abspath(args.store)), args.accept_capabilities, args.signature, args.allow_native)}
        elif command == "native-manifest":
            payload = {"nativeManifest": author_native_manifest(args.pulse, Path(os.path.abspath(args.path)),
                Path(os.path.abspath(args.library)), Path(os.path.abspath(args.output)), args.file)}
        elif command == "list":
            payload = {"registry": package_store.listing(Path(os.path.abspath(args.store)))}
        elif command in ("enable", "disable", "rollback", "uninstall"):
            payload = {"update": package_store.update(Path(os.path.abspath(args.store)), args.id, command,
                        validate=lambda data: validate_bytes(args.pulse, data), allow_native=getattr(args, "allow_native", False))}
        elif command == "run":
            payload = {"run": run_store(args.pulse, Path(os.path.abspath(args.store)),
                        Path(os.path.abspath(args.profile)) if args.profile else None)}
        else:
            payload = initialize(args.pulse, args.destination, args.id, args.name, args.template)
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
