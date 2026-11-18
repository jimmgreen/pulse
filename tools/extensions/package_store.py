"""Explicit local declaration store with optional pinned signatures and permission upgrade checks."""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import shutil
import tempfile
import time
if os.name == "nt":
    import ctypes
    import msvcrt
else:
    import fcntl


class StoreError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def fail(message, code="invalid_store"):
    raise StoreError(code, message)


def ordinary(path):
    if path.is_symlink():
        fail("Store paths must not be symbolic links")
    try:
        info = path.lstat()
        if getattr(info, "st_file_attributes", 0) & 0x400:
            fail("Store paths must not be reparse points")
    except FileNotFoundError:
        pass


def local_root(root):
    if not root.is_absolute():
        fail("Store must be an absolute local directory")
    if os.name == "nt":
        if root.drive.startswith("\\") or ctypes.windll.kernel32.GetDriveTypeW(str(root.anchor)) not in (2, 3, 6):
            fail("Store must be on a local drive")
    for ancestor in (root, *root.parents):
        ordinary(ancestor)


def strict_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                fail("Duplicate JSON key")
            result[key] = value
        return result
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=pairs,
                          parse_constant=lambda value: fail("Invalid JSON constant"))
    except (ValueError, UnicodeError) as exc:
        fail("Invalid store JSON: " + str(exc))


def digest(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


MAX_NATIVE_BYTES = 64 * 1024 * 1024
MAX_PACKAGE_BYTES = 256 * 1024 * 1024


def default_root():
    local = os.environ.get("LOCALAPPDATA")
    if not local:
        fail("LOCALAPPDATA is not set; supply --store", "store_not_found")
    return Path(local) / "Pulse" / "extensions"


def native_declaration(manifest):
    if "runtime" in manifest or any(isinstance(command, dict) and command.get("capability") == "runtime.invoke"
                                    for command in manifest.get("commands", [])):
        fail("runtime and runtime.invoke are removed; executable packages require a native declaration", "invalid_manifest")
    if "native" not in manifest:
        return None
    native = manifest["native"]
    if (not isinstance(native, dict) or (not {"version", "architecture", "library", "sha256"} <= set(native) or set(native) - {"version", "architecture", "library", "sha256", "files"}) or
            type(native["version"]) is not int or native["version"] != 1 or
            native["architecture"] != "windows-x64" or
            not isinstance(native["library"], str) or
            not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,123}\.dll", native["library"]) or
            native["library"].split(".", 1)[0].upper() in
                ({"CON", "PRN", "AUX", "NUL"} | {prefix + str(index) for prefix in ("COM", "LPT") for index in range(1, 10)}) or
            not digest(native["sha256"])):
        fail("Native declaration requires version 1, windows-x64, a single .dll filename and lowercase SHA-256", "invalid_native")
    if "files" in native:
        files = native["files"]
        if not isinstance(files, list) or not 1 <= len(files) <= 63:
            fail("native.files requires 1-63 sidecars", "invalid_native")
        seen = {native["library"].lower()}
        for item in files:
            if not isinstance(item, dict) or set(item) != {"path", "sha256"} or not file_name(item["path"]) or not digest(item["sha256"]):
                fail("Invalid native sidecar path or SHA-256", "invalid_native")
            if item["path"].lower() in seen:
                fail("Duplicate native file name", "invalid_native")
            seen.add(item["path"].lower())
    return native


def file_name(value):
    return (isinstance(value, str) and 1 <= len(value) <= 128 and
            re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", value) is not None and not value.endswith(".") and
            value.lower() not in {"manifest.json", "package.json"} and
            value.split(".", 1)[0].upper() not in
            ({"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"} |
             {prefix + str(i) for prefix in ("COM", "LPT") for i in range(1, 10)}))


def native_files(native):
    return {native["library"]: native["sha256"], **{f["path"]: f["sha256"] for f in native.get("files", [])}}


def validate_native_files(native, files):
    expected = native_files(native)
    if not isinstance(files, dict) or set(files) != set(expected):
        fail("Native package file set differs from manifest", "invalid_native_binary")
    total = 0
    for name, sha in expected.items():
        data = files[name]
        if not isinstance(data, bytes) or len(data) > MAX_NATIVE_BYTES:
            fail("Native file exceeds 64 MiB or is not immutable bytes", "invalid_native_binary")
        total += len(data)
        if total > MAX_PACKAGE_BYTES:
            fail("Native package exceeds 256 MiB", "invalid_native_binary")
        if hashlib.sha256(data).hexdigest() != sha:
            fail("Native file content does not match its SHA-256: " + name, "native_hash_mismatch")
        if name.lower().endswith(".dll"):
            validate_native_binary(data, sha)
    return files


def read_native_files(source_dir, native):
    source_dir = Path(source_dir).absolute()
    for ancestor in (source_dir, *source_dir.parents):
        ordinary(ancestor)
    files = {}
    total = 0
    for name in native_files(native):
        files[name] = read_bytes(source_dir / name, min(MAX_NATIVE_BYTES, MAX_PACKAGE_BYTES - total))
        total += len(files[name])
    return validate_native_files(native, files)


def require_native_consent(manifest, allow_native):
    native = native_declaration(manifest)
    if native and not allow_native:
        fail("Native packages execute trusted code inside Pulse; explicitly supply --allow-native", "native_confirmation_required")
    return native


def validate_native_binary(data, expected_sha=None):
    if len(data) > MAX_NATIVE_BYTES:
        fail("Native DLL exceeds 64 MiB", "invalid_native_binary")
    sha = hashlib.sha256(data).hexdigest()
    if expected_sha is not None and sha != expected_sha:
        fail("Native DLL content does not match its SHA-256", "native_hash_mismatch")
    if len(data) < 64 or data[:2] != b"MZ":
        fail("Native library must be an x64 PE DLL", "invalid_native_binary")
    offset = struct.unpack_from("<I", data, 0x3c)[0]
    if (offset < 64 or offset + 26 > len(data) or data[offset:offset + 4] != b"PE\0\0" or
            struct.unpack_from("<H", data, offset + 4)[0] != 0x8664 or
            not struct.unpack_from("<H", data, offset + 22)[0] & 0x2000 or
            struct.unpack_from("<H", data, offset + 20)[0] < 2 or
            offset + 24 + struct.unpack_from("<H", data, offset + 20)[0] > len(data) or
            struct.unpack_from("<H", data, offset + 24)[0] != 0x20b):
        fail("Native library must be an x64 PE DLL", "invalid_native_binary")
    return sha


def native_binary(root, manifest, revision_sha=None):
    native = native_declaration(manifest)
    if native is None:
        return None
    if native.get("files"):
        if not revision_sha:
            fail("Multi-file package requires its manifest revision")
        directory = root / "packages" / revision_sha
        ordinary(root / "packages")
        ordinary(directory)
        if {p.name for p in directory.iterdir()} != set(native_files(native)):
            fail("Stored package contains missing or extra files", "invalid_native_binary")
        read_native_files(directory, native)
        return directory / native["library"]
    ordinary(root / "binaries")
    path = root / "binaries" / (native["sha256"] + ".dll")
    validate_native_binary(read_bytes(path, MAX_NATIVE_BYTES), native["sha256"])
    return path


def read_native_source(source_dir, native):
    source_dir = Path(source_dir).absolute()
    for ancestor in (source_dir, *source_dir.parents):
        ordinary(ancestor)
    data = read_bytes(source_dir / native["library"], MAX_NATIVE_BYTES)
    validate_native_binary(data, native["sha256"])
    return data


def publish_native(root, manifest, source_dir, binary_data=None, file_data=None, revision_sha=None):
    native = native_declaration(manifest)
    if native is None:
        return
    if native.get("files"):
        files = read_native_files(source_dir, native) if file_data is None and source_dir is not None else validate_native_files(native, file_data)
        directory = root / "packages"
        ordinary(directory)
        directory.mkdir(exist_ok=True)
        target = directory / revision_sha
        if target.exists():
            native_binary(root, manifest, revision_sha)
            return
        temporary = Path(tempfile.mkdtemp(dir=directory, prefix=".package-"))
        try:
            for name, data in files.items():
                with (temporary / name).open("xb") as stream:
                    stream.write(data)
                    stream.flush()
                    os.fsync(stream.fileno())
            read_native_files(temporary, native)
            os.rename(temporary, target)
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)
        return
    if source_dir is None and binary_data is None:
        fail("Native install requires the manifest directory containing its DLL", "native_source_required")
    data = read_native_source(source_dir, native) if binary_data is None else binary_data
    if not isinstance(data, bytes):
        fail("Native DLL snapshot must be immutable bytes", "invalid_native_binary")
    validate_native_binary(data, native["sha256"])
    directory = root / "binaries"
    ordinary(directory)
    directory.mkdir(exist_ok=True)
    target = directory / (native["sha256"] + ".dll")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=directory, prefix=".binary-", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        try:
            os.link(temporary, target)
        except FileExistsError:
            native_binary(root, manifest)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def label(value):
    try:
        return (isinstance(value, str) and bool(value.strip(" ")) and len(value.encode("utf-8")) <= 128 and
                not any(ord(c) < 32 or ord(c) == 127 for c in value))
    except UnicodeError:
        return False


def read_bytes(path, limit):
    ordinary(path)
    with path.open("rb") as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            fail("Store entries must be regular files")
        data = stream.read(limit + 1)
    if len(data) > limit:
        fail("Store entry exceeds its size limit")
    return data


def revision(root, sha):
    if not digest(sha):
        fail("Invalid revision hash")
    ordinary(root / "versions")
    data = read_bytes(root / "versions" / (sha + ".json"), 65536)
    if hashlib.sha256(data).hexdigest() != sha:
        fail("Revision content does not match its hash")
    manifest = strict_json(data)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("id"), str) or not isinstance(manifest.get("name"), str):
        fail("Revision has no manifest identity")
    return data, manifest


def read_registry(root):
    path = root / "registry.json"
    ordinary(path)
    if not path.exists():
        return {"formatVersion": 1, "packages": []}
    registry = strict_json(read_bytes(path, 65536))
    if (not isinstance(registry, dict) or set(registry) != {"formatVersion", "packages"} or
            type(registry["formatVersion"]) not in (int, float) or registry["formatVersion"] != 1 or
            not isinstance(registry["packages"], list) or len(registry["packages"]) > 32):
        fail("Unsupported registry format")
    ids = set()
    for entry in registry["packages"]:
        if (not isinstance(entry, dict) or not {"id", "name", "enabled", "activeRevision"} <= set(entry) or
                set(entry) - {"id", "name", "enabled", "activeRevision", "previousRevision"} or
                not isinstance(entry["id"], str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", entry["id"]) or entry["id"] in ids or
                not label(entry["name"]) or
                type(entry["enabled"]) is not bool):
            fail("Malformed or duplicate registry package")
        ids.add(entry["id"])
        for key in ("activeRevision", "previousRevision"):
            if key in entry:
                if not digest(entry[key]):
                    fail("Invalid revision hash")
    return registry


@contextlib.contextmanager
def locked(root, create=False):
    root = Path(root)
    local_root(root)
    if create:
        root.mkdir(parents=True, exist_ok=True)
    if not root.is_dir():
        fail("Store directory does not exist", "store_not_found")
    ordinary(root / "versions")
    lock = root / ".registry.lock"
    ordinary(lock)
    stream = lock.open("a+b")
    acquired = False
    try:
        if os.fstat(stream.fileno()).st_size == 0:
            stream.write(b"\0")
            stream.flush()
        stream.seek(0)
        try:
            if os.name == "nt":
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            acquired = True
        except OSError:
            fail("Store is locked by another operation; retry after it finishes", "store_busy")
        yield root
    finally:
        try:
            if acquired:
                stream.seek(0)
                if os.name == "nt":
                    msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
        finally:
            stream.close()


def save(root, registry):
    ordinary(root)
    ordinary(root / "registry.json")
    data = (json.dumps(registry, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if len(data) > 65536:
        fail("Registry exceeds the 64 KiB limit")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=root, prefix=".registry-", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        # A short-lived Windows reader can block atomic replacement. Keep the
        # previous registry until commit, matching signature-policy publication.
        for attempt in range(50):
            try:
                os.replace(temporary, root / "registry.json")
                break
            except OSError as error:
                if getattr(error, "winerror", None) not in (5, 32, 33) or attempt == 49:
                    raise
                time.sleep(.01)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def capabilities(manifest):
    commands = manifest.get("commands", [])
    if not isinstance(commands, list) or any(not isinstance(item, dict) or not isinstance(item.get("capability"), str) for item in commands):
        fail("Revision command declarations are malformed")
    resources = manifest.get("resources", [])
    if not isinstance(resources, list) or any(not isinstance(item, str) for item in resources):
        fail("Revision resource declarations are malformed")
    return {item["capability"] for item in commands} | {"resource:" + item for item in resources}


def check_signature(root, data, sha, signature=None):
    import package_signing
    policy_path = root / "signature-policy.json"
    signature_path = root / "versions" / (sha + ".sig.json")
    try:
        ordinary(policy_path)
        ordinary(root / "versions")
        ordinary(signature_path)
        policy = package_signing.parse(read_bytes(policy_path, 65536)) if policy_path.exists() else None
        if signature is None and signature_path.exists():
            signature = read_bytes(signature_path, 16384)
        if signature is not None and len(signature) > 16384:
            fail("Signature exceeds byte limit", "signature_invalid")
        package_signing.verify(data, package_signing.parse(signature) if signature is not None else None, policy)
    except (ValueError, TypeError, KeyError) as error:
        fail(str(error), "signature_invalid")


def install(root, data, accept_capabilities=False, signature=None, source_dir=None, allow_native=False, binary_data=None, file_data=None):
    manifest = strict_json(data)
    native = require_native_consent(manifest, allow_native)
    if native is None and (binary_data is not None or file_data is not None):
        fail("A DLL snapshot requires a native declaration", "invalid_native_binary")
    sha = hashlib.sha256(data).hexdigest()
    with locked(root, create=True) as root:
        check_signature(root, data, sha, signature)
        registry = read_registry(root)
        entry = next((item for item in registry["packages"] if item["id"] == manifest["id"]), None)
        if entry and entry["activeRevision"] == sha:
            revision(root, sha)
            native_binary(root, manifest, sha)
            if signature is not None:
                import package_signing
                package_signing.atomic_write(root / "versions" / (sha + ".sig.json"), signature)
            return {"changed": False, "package": entry}
        if entry:
            _, old = revision(root, entry["activeRevision"])
            if old["id"] != entry["id"] or old["name"] != entry["name"]:
                fail("Active revision identity does not match registry")
            added = sorted(capabilities(manifest) - capabilities(old))
            if added and not accept_capabilities:
                fail("Upgrade adds capabilities requiring --accept-capabilities: " + ", ".join(added), "capability_confirmation_required")
        elif len(registry["packages"]) >= 32:
            fail("Store already contains 32 packages", "store_full")
        publish_native(root, manifest, source_dir, binary_data, file_data, sha)
        versions = root / "versions"
        versions.mkdir(exist_ok=True)
        ordinary(versions)
        path = versions / (sha + ".json")
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=versions, prefix=".version-", delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            try:
                os.link(temporary, path)
            except FileExistsError:
                revision(root, sha)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        if signature is not None:
            import package_signing
            package_signing.atomic_write(versions / (sha + ".sig.json"), signature)
        replacement = {"id": manifest["id"], "name": manifest["name"], "enabled": entry["enabled"] if entry else False,
                       "activeRevision": sha}
        if entry:
            replacement["previousRevision"] = entry["activeRevision"]
            registry["packages"][registry["packages"].index(entry)] = replacement
        else:
            registry["packages"].append(replacement)
        save(root, registry)
        return {"changed": True, "package": replacement}


def listing(root):
    with locked(root) as root:
        return read_registry(root)


def update(root, identifier, operation, validate=None, allow_native=False):
    with locked(root) as root:
        registry = read_registry(root)
        entry = next((item for item in registry["packages"] if item["id"] == identifier), None)
        if entry is None:
            fail("Package ID is not installed", "package_not_found")
        before = dict(entry)
        if operation in ("enable", "disable"):
            if operation == "enable":
                data, manifest = revision(root, entry["activeRevision"])
                require_native_consent(manifest, allow_native)
                native_binary(root, manifest, entry["activeRevision"])
                check_signature(root, data, entry["activeRevision"])
                if manifest["id"] != entry["id"] or manifest["name"] != entry["name"]:
                    fail("Revision identity does not match registry")
                if validate:
                    validate(data)
            entry["enabled"] = operation == "enable"
        elif operation == "rollback":
            if "previousRevision" not in entry:
                fail("Package has no previous revision", "no_previous_revision")
            data, manifest = revision(root, entry["previousRevision"])
            require_native_consent(manifest, allow_native)
            native_binary(root, manifest, entry["previousRevision"])
            check_signature(root, data, entry["previousRevision"])
            if manifest["id"] != entry["id"]:
                fail("Previous revision ID does not match registry")
            if validate:
                validate(data)
            entry["activeRevision"], entry["previousRevision"] = entry["previousRevision"], entry["activeRevision"]
            entry["name"] = manifest["name"]
        elif operation == "uninstall":
            registry["packages"].remove(entry)
        else:
            fail("Unknown package operation")
        changed = operation == "uninstall" or entry != before
        if changed:
            save(root, registry)
        return {"changed": changed, "package": entry, "immutableVersionsRetained": operation == "uninstall"}
