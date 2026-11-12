"""Explicit local declaration store with pinned signatures, publisher continuity, rotation, and GC."""
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
        lower = name.lower()
        if lower.endswith(".dll"):
            validate_native_binary(data, sha)
        elif lower.endswith(".exe"):
            validate_native_executable(data, sha)
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


def _validate_x64_pe(data, expect_dll, expected_sha=None):
    kind = "Native DLL" if expect_dll else "Native executable sidecar"
    if len(data) > MAX_NATIVE_BYTES:
        fail(f"{kind} exceeds 64 MiB", "invalid_native_binary")
    sha = hashlib.sha256(data).hexdigest()
    if expected_sha is not None and sha != expected_sha:
        fail(f"{kind} content does not match its SHA-256", "native_hash_mismatch")
    if len(data) < 64 or data[:2] != b"MZ":
        fail(f"{kind} must be an x64 PE32+ {'DLL' if expect_dll else 'executable'}", "invalid_native_binary")
    offset = struct.unpack_from("<I", data, 0x3c)[0]
    if offset < 64 or offset + 26 > len(data) or data[offset:offset + 4] != b"PE\0\0":
        fail(f"{kind} must be an x64 PE32+ {'DLL' if expect_dll else 'executable'}", "invalid_native_binary")
    machine = struct.unpack_from("<H", data, offset + 4)[0]
    opt_size = struct.unpack_from("<H", data, offset + 20)[0]
    characteristics = struct.unpack_from("<H", data, offset + 22)[0]
    is_dll = bool(characteristics & 0x2000)
    is_exec = bool(characteristics & 0x0002)
    role_ok = is_dll if expect_dll else (is_exec and not is_dll)
    if (machine != 0x8664 or not role_ok or opt_size < 2 or offset + 24 + opt_size > len(data) or
            struct.unpack_from("<H", data, offset + 24)[0] != 0x20b):
        fail(f"{kind} must be an x64 PE32+ {'DLL' if expect_dll else 'executable'}", "invalid_native_binary")
    return sha


def validate_native_binary(data, expected_sha=None):
    return _validate_x64_pe(data, True, expected_sha)


def validate_native_executable(data, expected_sha=None):
    return _validate_x64_pe(data, False, expected_sha)


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
    contributed = set()
    if "contributes" in manifest:
        contributes = manifest["contributes"]
        if not isinstance(contributes, dict):
            fail("Revision contributes declaration is malformed")
        static_commands = contributes.get("commands", [])
        if not isinstance(static_commands, list) or any(not isinstance(item, dict) or not isinstance(item.get("id"), str) for item in static_commands):
            fail("Revision contributed commands are malformed")
        contributed = {"contributes.command:" + item["id"] for item in static_commands}
    return {item["capability"] for item in commands} | {"resource:" + item for item in resources} | contributed


def _load_policy_keys(root):
    import package_signing
    policy_path = root / "signature-policy.json"
    ordinary(policy_path)
    if not policy_path.exists():
        return None, {}
    try:
        policy = package_signing.parse(read_bytes(policy_path, 65536))
        keys = package_signing.policy_keys(policy)
        return policy, keys
    except (ValueError, TypeError, KeyError) as error:
        fail(str(error), "signature_invalid")


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
        return package_signing.verify(data, package_signing.parse(signature) if signature is not None else None, policy)
    except (ValueError, TypeError, KeyError) as error:
        fail(str(error), "signature_invalid")


def _read_stored_signature_doc(root, sha):
    import package_signing
    sig_path = root / "versions" / (sha + ".sig.json")
    ordinary(sig_path)
    if not sig_path.exists():
        return None
    try:
        doc = package_signing.parse(read_bytes(sig_path, 16384))
        package_signing.exact(doc, ("formatVersion", "algorithm", "publisher", "signature"))
        package_signing.fingerprint(doc["publisher"])
        return doc
    except (ValueError, TypeError, KeyError) as error:
        fail("Invalid stored signature metadata: " + str(error), "signature_invalid")


def _parse_rotation_input(rotation):
    import package_signing
    if rotation is None:
        return None
    if isinstance(rotation, dict):
        return rotation
    if isinstance(rotation, (bytes, bytearray)):
        if len(rotation) > 16384:
            fail("Rotation proof exceeds byte limit", "installed_publisher_mismatch")
        try:
            return package_signing.parse(bytes(rotation))
        except (ValueError, UnicodeDecodeError) as error:
            fail("Malformed rotation proof: " + str(error), "installed_publisher_mismatch")
    fail("Unsupported rotation proof input", "installed_publisher_mismatch")


def _verify_publisher_continuity(root, package_id, old_sha, old_bytes, new_sig_result,
                                 policy_keys, rotation=None, allow_publisher_change=False):
    import package_signing
    old_sig_doc = _read_stored_signature_doc(root, old_sha)
    if old_sig_doc is None:
        return {"rotated": False, "publisherChangedWithoutRotation": False}

    old_publisher = old_sig_doc["publisher"]
    if old_publisher in policy_keys:
        try:
            package_signing.verify_with_public_key(old_bytes, old_sig_doc, policy_keys[old_publisher])
        except (ValueError, TypeError, KeyError) as error:
            fail("Stored active revision signature failed verification: " + str(error), "signature_invalid")

    if not new_sig_result.get("signed"):
        fail("Installed package is signed by publisher " + old_publisher + "; unsigned upgrade is rejected",
             "installed_publisher_mismatch")

    new_publisher = new_sig_result["publisher"]
    if old_publisher == new_publisher:
        return {"rotated": False, "publisherChangedWithoutRotation": False, "publisher": new_publisher}

    if rotation is not None:
        rotation_doc = _parse_rotation_input(rotation)
        if old_publisher in policy_keys:
            old_pub_hex = policy_keys[old_publisher].hex()
        elif isinstance(rotation_doc, dict) and isinstance(rotation_doc.get("oldPublicKey"), str):
            old_pub_hex = rotation_doc["oldPublicKey"]
        else:
            fail("Old publisher public key is missing from policy and rotation proof", "installed_publisher_mismatch")
        if new_publisher not in policy_keys:
            fail("New publisher is not trusted in store signature policy", "installed_publisher_mismatch")
        try:
            old_pub_blob = package_signing.public_blob(package_signing.hex_bytes(old_pub_hex))
            package_signing.verify_with_public_key(old_bytes, old_sig_doc, old_pub_blob)
            package_signing.verify_rotation(package_id, old_pub_hex, policy_keys[new_publisher].hex(), rotation_doc)
        except (ValueError, TypeError, KeyError) as error:
            fail("Publisher rotation verification failed: " + str(error), "installed_publisher_mismatch")
        return {
            "rotated": True,
            "publisherChangedWithoutRotation": False,
            "oldPublisher": old_publisher,
            "publisher": new_publisher,
            "rotationDoc": rotation_doc,
        }

    if allow_publisher_change:
        return {
            "rotated": False,
            "publisherChangedWithoutRotation": True,
            "oldPublisher": old_publisher,
            "publisher": new_publisher,
        }

    fail("Installed package publisher mismatch (" + old_publisher + " != " + new_publisher + "); supply --rotation or --allow-publisher-change",
         "installed_publisher_mismatch")


def _verify_rollback_continuity(root, package_id, curr_sha, curr_bytes, prev_sha, prev_sig_result,
                                policy_keys, rotation=None, allow_publisher_change=False):
    import package_signing
    curr_sig_doc = _read_stored_signature_doc(root, curr_sha)
    if curr_sig_doc is None:
        return {"publisherChangedWithoutRotation": False}

    curr_publisher = curr_sig_doc["publisher"]
    if curr_bytes is not None and curr_publisher in policy_keys:
        try:
            package_signing.verify_with_public_key(curr_bytes, curr_sig_doc, policy_keys[curr_publisher])
        except (ValueError, TypeError, KeyError) as error:
            fail("Active revision signature is invalid: " + str(error), "signature_invalid")

    if not prev_sig_result.get("signed"):
        fail("Cannot roll back signed package to an unsigned revision", "installed_publisher_mismatch")

    prev_publisher = prev_sig_result["publisher"]
    if curr_publisher == prev_publisher:
        return {"publisherChangedWithoutRotation": False}

    # Check explicit rotation proof or stored rotation proof from the forward upgrade.
    candidate_doc = _parse_rotation_input(rotation)
    if candidate_doc is None:
        stored_rot_path = root / "versions" / (curr_sha + ".rotation.json")
        ordinary(stored_rot_path)
        if stored_rot_path.exists():
            try:
                candidate_doc = package_signing.parse(read_bytes(stored_rot_path, 16384))
            except (ValueError, UnicodeDecodeError):
                candidate_doc = None

    if candidate_doc is not None and prev_publisher in policy_keys:
        curr_pub_hex = (
            policy_keys[curr_publisher].hex() if curr_publisher in policy_keys
            else candidate_doc.get("newPublicKey") if candidate_doc.get("newPublisher") == curr_publisher
            else candidate_doc.get("oldPublicKey")
        )
        if isinstance(curr_pub_hex, str):
            try:
                if candidate_doc.get("oldPublisher") == prev_publisher and candidate_doc.get("newPublisher") == curr_publisher:
                    package_signing.verify_rotation(package_id, policy_keys[prev_publisher].hex(), curr_pub_hex, candidate_doc)
                    return {"publisherChangedWithoutRotation": False, "rotated": True}
                if candidate_doc.get("oldPublisher") == curr_publisher and candidate_doc.get("newPublisher") == prev_publisher:
                    package_signing.verify_rotation(package_id, curr_pub_hex, policy_keys[prev_publisher].hex(), candidate_doc)
                    return {"publisherChangedWithoutRotation": False, "rotated": True}
            except (ValueError, TypeError, KeyError) as error:
                if rotation is not None:
                    fail("Rollback rotation proof failed verification: " + str(error), "installed_publisher_mismatch")

    if rotation is not None:
        fail("Rollback rotation proof does not match installed/previous publishers", "installed_publisher_mismatch")

    if allow_publisher_change:
        return {"publisherChangedWithoutRotation": True}

    fail("Rollback crosses distinct publishers (" + curr_publisher + " -> " + prev_publisher + "); supply --rotation or --allow-publisher-change",
         "installed_publisher_mismatch")


def install(root, data, accept_capabilities=False, signature=None, source_dir=None,
            allow_native=False, binary_data=None, file_data=None,
            rotation=None, allow_publisher_change=False):
    manifest = strict_json(data)
    native = require_native_consent(manifest, allow_native)
    if native is None and (binary_data is not None or file_data is not None):
        fail("A DLL snapshot requires a native declaration", "invalid_native_binary")
    sha = hashlib.sha256(data).hexdigest()
    with locked(root, create=True) as root:
        new_sig = check_signature(root, data, sha, signature)
        _, policy_keys = _load_policy_keys(root)
        registry = read_registry(root)
        entry = next((item for item in registry["packages"] if item["id"] == manifest["id"]), None)
        continuity = {"rotated": False, "publisherChangedWithoutRotation": False}
        if entry:
            old_bytes, old = revision(root, entry["activeRevision"])
            if old["id"] != entry["id"] or old["name"] != entry["name"]:
                fail("Active revision identity does not match registry")
            continuity = _verify_publisher_continuity(
                root, manifest["id"], entry["activeRevision"], old_bytes, new_sig, policy_keys,
                rotation=rotation, allow_publisher_change=allow_publisher_change,
            )
            if entry["activeRevision"] == sha:
                native_binary(root, manifest, sha)
                if signature is not None:
                    import package_signing
                    package_signing.atomic_write(root / "versions" / (sha + ".sig.json"), signature)
                if continuity.get("rotated") and continuity.get("rotationDoc"):
                    import package_signing
                    package_signing.atomic_write(
                        root / "versions" / (sha + ".rotation.json"),
                        (json.dumps(continuity["rotationDoc"], indent=2) + "\n").encode("utf-8"),
                    )
                if continuity.get("publisherChangedWithoutRotation") and entry["enabled"]:
                    entry["enabled"] = False
                    save(root, registry)
                    return {"changed": True, "package": entry, "publisherChanged": True}
                return {"changed": False, "package": entry}
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
        if continuity.get("rotated") and continuity.get("rotationDoc"):
            import package_signing
            package_signing.atomic_write(
                versions / (sha + ".rotation.json"),
                (json.dumps(continuity["rotationDoc"], indent=2) + "\n").encode("utf-8"),
            )
        inherited_enabled = False
        if entry and not continuity.get("publisherChangedWithoutRotation"):
            inherited_enabled = entry["enabled"]
        replacement = {"id": manifest["id"], "name": manifest["name"], "enabled": inherited_enabled,
                       "activeRevision": sha}
        if entry:
            replacement["previousRevision"] = entry["activeRevision"]
            registry["packages"][registry["packages"].index(entry)] = replacement
        else:
            registry["packages"].append(replacement)
        save(root, registry)
        result = {"changed": True, "package": replacement}
        if continuity.get("rotated"):
            result["publisherRotated"] = True
        if continuity.get("publisherChangedWithoutRotation"):
            result["publisherChanged"] = True
        return result


def listing(root):
    with locked(root) as root:
        return read_registry(root)


def _check_package_settings_paths(data_root, identifier):
    if not isinstance(identifier, str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", identifier):
        fail("Invalid package identifier", "invalid_argument")
    for ancestor in (data_root, *data_root.parents):
        ordinary(ancestor)
    settings_dir = data_root / "extension-settings"
    ordinary(settings_dir)
    if not settings_dir.exists():
        return None
    if not settings_dir.is_dir():
        fail("Settings directory must be an ordinary directory", "unsafe_path")
    stem = "pkg-" + identifier.encode("ascii").hex()
    target = settings_dir / (stem + ".json")
    lock_path = settings_dir / (stem + ".lock")
    for candidate in (target, lock_path):
        ordinary(candidate)
        if candidate.exists():
            info = candidate.lstat()
            if not stat.S_ISREG(info.st_mode) or getattr(info, "st_nlink", 1) != 1:
                fail("Settings file must be an ordinary single-link file", "unsafe_path")
    return target, lock_path


def _purge_package_settings(data_root, identifier):
    resolved = _check_package_settings_paths(data_root, identifier)
    if resolved is None:
        return False
    target, lock_path = resolved
    deleted = False
    ordinary(target)
    if target.exists():
        target.unlink()
        deleted = True
    ordinary(lock_path)
    if lock_path.exists():
        try:
            lock_path.unlink()
        except OSError:
            pass
    return deleted


def update(root, identifier, operation, validate=None, allow_native=False,
           accept_capabilities=False, rotation=None, allow_publisher_change=False,
           purge_data: bool = False):
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
            try:
                curr_bytes, curr_manifest = revision(root, entry["activeRevision"])
            except StoreError:
                curr_bytes, curr_manifest = None, None
            data, manifest = revision(root, entry["previousRevision"])
            require_native_consent(manifest, allow_native)
            native_binary(root, manifest, entry["previousRevision"])
            prev_sig = check_signature(root, data, entry["previousRevision"])
            _, policy_keys = _load_policy_keys(root)
            if manifest["id"] != entry["id"]:
                fail("Previous revision ID does not match registry")
            continuity = _verify_rollback_continuity(
                root, entry["id"], entry["activeRevision"], curr_bytes,
                entry["previousRevision"], prev_sig, policy_keys,
                rotation=rotation, allow_publisher_change=allow_publisher_change,
            )
            if curr_manifest is not None:
                added = sorted(capabilities(manifest) - capabilities(curr_manifest))
                if added and not accept_capabilities:
                    fail("Rollback restores capabilities requiring --accept-capabilities: " + ", ".join(added),
                         "capability_confirmation_required")
            if validate:
                validate(data)
            entry["activeRevision"], entry["previousRevision"] = entry["previousRevision"], entry["activeRevision"]
            entry["name"] = manifest["name"]
            if continuity.get("publisherChangedWithoutRotation"):
                entry["enabled"] = False
        elif operation == "uninstall":
            if purge_data:
                _check_package_settings_paths(root.parent, entry["id"])
            registry["packages"].remove(entry)
        else:
            fail("Unknown package operation")
        changed = operation == "uninstall" or entry != before
        if changed:
            save(root, registry)
        purged_data = False
        if operation == "uninstall" and purge_data and changed:
            purged_data = _purge_package_settings(root.parent, entry["id"])
        result = {"changed": changed, "package": entry, "immutableVersionsRetained": operation == "uninstall"}
        if operation == "uninstall":
            result["purgedData"] = bool(purged_data)
        return result


def uninstall_package(root, identifier, purge_data: bool = False):
    return update(Path(root), identifier, "uninstall", purge_data=purge_data)


def _version_revision_hash(filename):
    for suffix in (".sig.json", ".rotation.json", ".json"):
        if filename.endswith(suffix):
            candidate = filename[:-len(suffix)]
            if digest(candidate):
                return candidate
            return None
    return None


def gc(root):
    with locked(root) as root:
        registry = read_registry(root)
        ordinary(root / "registry.json")
        ordinary(root / "signature-policy.json")
        referenced_revisions = set()
        for entry in registry["packages"]:
            referenced_revisions.add(entry["activeRevision"])
            if "previousRevision" in entry:
                referenced_revisions.add(entry["previousRevision"])

        referenced_binaries = set()
        binaries_safe = True
        for rev_sha in sorted(referenced_revisions):
            try:
                _, manifest = revision(root, rev_sha)
                native = manifest.get("native")
                if isinstance(native, dict) and digest(native.get("sha256")):
                    referenced_binaries.add(native["sha256"])
            except StoreError:
                # A damaged disabled revision must not cause conservative GC to drop shared binaries.
                binaries_safe = False

        removed = {"versions": [], "binaries": [], "packages": []}
        skipped = []
        reclaimed_bytes = 0

        versions_dir = root / "versions"
        if versions_dir.exists():
            ordinary(versions_dir)
            for item in sorted(versions_dir.iterdir()):
                ordinary(item)
                rev_sha = _version_revision_hash(item.name)
                if rev_sha in referenced_revisions:
                    continue
                if item.is_dir():
                    skipped.append({"path": str(item), "reason": "unexpected directory in versions"})
                    continue
                try:
                    size = item.stat().st_size
                    item.unlink()
                    removed["versions"].append(str(item))
                    reclaimed_bytes += size
                except OSError as exc:
                    skipped.append({"path": str(item), "reason": str(exc)})

        binaries_dir = root / "binaries"
        if binaries_dir.exists():
            ordinary(binaries_dir)
            if not binaries_safe:
                skipped.append({"path": str(binaries_dir), "reason": "skipped binary cleanup because a referenced revision manifest is unreadable"})
            else:
                for item in sorted(binaries_dir.iterdir()):
                    ordinary(item)
                    if item.name.endswith(".dll") and digest(item.name[:-4]) and item.name[:-4] in referenced_binaries:
                        continue
                    if item.is_dir():
                        skipped.append({"path": str(item), "reason": "unexpected directory in binaries"})
                        continue
                    try:
                        size = item.stat().st_size
                        item.unlink()
                        removed["binaries"].append(str(item))
                        reclaimed_bytes += size
                    except OSError as exc:
                        skipped.append({"path": str(item), "reason": str(exc)})

        packages_dir = root / "packages"
        if packages_dir.exists():
            ordinary(packages_dir)
            for item in sorted(packages_dir.iterdir()):
                ordinary(item)
                if digest(item.name) and item.name in referenced_revisions:
                    continue
                if not item.is_dir():
                    try:
                        size = item.stat().st_size
                        item.unlink()
                        removed["packages"].append(str(item))
                        reclaimed_bytes += size
                    except OSError as exc:
                        skipped.append({"path": str(item), "reason": str(exc)})
                    continue
                dir_bytes = 0
                dir_blocked = False
                for child in sorted(item.iterdir()):
                    ordinary(child)
                    if child.is_dir():
                        dir_blocked = True
                        skipped.append({"path": str(child), "reason": "unexpected nested directory in packages"})
                        continue
                    try:
                        size = child.stat().st_size
                        child.unlink()
                        dir_bytes += size
                    except OSError as exc:
                        dir_blocked = True
                        skipped.append({"path": str(child), "reason": str(exc)})
                reclaimed_bytes += dir_bytes
                if not dir_blocked:
                    try:
                        item.rmdir()
                        removed["packages"].append(str(item))
                    except OSError as exc:
                        skipped.append({"path": str(item), "reason": str(exc)})

        return {
            "referencedRevisions": sorted(referenced_revisions),
            "referencedBinaries": sorted(referenced_binaries),
            "removed": removed,
            "removedCount": sum(len(items) for items in removed.values()),
            "reclaimedBytes": reclaimed_bytes,
            "skipped": skipped,
        }
