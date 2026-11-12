"""Official extension release packaging and signed catalog pipeline (Windows CNG)."""
import argparse
import base64
import ctypes
from datetime import datetime, timedelta, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import sys

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import check_docs_and_contracts
import native_archive
import package_signing
import package_store

ROOT = TOOLS_DIR.parents[1]

CATALOG_SIGNATURE_PREFIX = b"Pulse.Extension.Catalog.Signature\0v1\0"
MANIFEST_SIGNATURE_PREFIX = package_signing.PREFIX
ALGORITHM = package_signing.ALGORITHM

OFFICIAL_PUBLIC_KEY_HEX = (
    "52534131000c000003000000800100000000000000000000010001"
    "c3cd365f6d023010f91a712db7787536854a746e74be888a6e03bdb8b9d401ed"
    "b046493f77464fa40b19edde03f52e9025f7709dbb4eb16c7961f21dd6bbb863"
    "7d0d5fb232b028a8b700f9a78934a2c3f2c470798dcef2d4f47445772c766ca6"
    "b391efd5fee6e71abdfff48fc01e8da7a53ec30653f0054eefe73dbfd8fed483"
    "32803306cc3ffca83306939de5b5f37361a989d46ee1c3bb89f0909e8f77188d"
    "6a852015f260d55803b71d9fc9a1d5ceaef5c9c45c5711426727e6f5bebecd39"
    "18617847bd138222fb79ca53d3f642493e72d211721740ce8c57f269fa2c68b5"
    "21578f7def24be9c10e18944efd7d9488ce7e892db0f64a1d0d6a359e22eb744"
    "843a815fb8e69f9f55272ad8d04a6394cdb458c86173f53ee5a9487597c20467"
    "abe912f92b4772d9d9707d93fbeeebc25ec9f691bdce9e62f46d4021e92cccf9"
    "e52dfc48b7503c5ff1e4e44996619f99644a6ba8592a31b8cf377665ca58d52d"
    "d69687ac869ccdf1c3ebb0387f87632a790eda7a5c551702c590c2c5a961980d"
)
OFFICIAL_PUBLISHER_ID = "86f9a10e981d16ac677eb11b9c45e82bf38a7e00c0d3d271e84ef9e0d7a4762b"

MAX_CATALOG_BYTES = 4 * 1024 * 1024
MAX_CATALOG_SIGNATURE_BYTES = 16384
MAX_CATALOG_PUBLISHERS = 512
MAX_CATALOG_EXTENSIONS = 2048
MAX_CATALOG_RELEASES = 8192
MAX_CATALOG_SEQUENCE = 9007199254740991  # 2^53 - 1
MAX_PACKAGE_ASSET_BYTES = 256 * 1024 * 1024 + 256 * 1024

ALLOWED_CATEGORIES = frozenset({"fileops", "preview", "search", "panel", "tools"})
ALLOWED_GLYPHS = frozenset({
    "archive", "film", "image", "doc", "find", "ocr",
    "git", "rename", "duplicate", "compress", "cloud", "puzzle",
})
ALLOWED_RESOURCES = tuple(check_docs_and_contracts.EXPECTED_RESOURCES)

_UNICODE_WHITESPACE = frozenset({
    ord(" "), ord("\n"), 0x85, 0xA0, 0x1680,
    *range(0x2000, 0x200B), 0x2028, 0x2029, 0x202F, 0x205F, 0x3000,
})
_EXTENSION_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,30}(\.[a-z0-9][a-z0-9-]{0,30}){1,3}$")
_SEMVER_RE = re.compile(r"^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$")
_MIN_PULSE_RE = re.compile(r"^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,4}$")
_COLOR_RE = re.compile(r"^#[0-9A-F]{6}$")
_HTTPS_RE = re.compile(r"^https://[A-Za-z0-9.-]+(/[A-Za-z0-9._~%/+-]*)?$")
_GITHUB_ASSET_RE = re.compile(
    r"^https://github\.com/[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}"
    r"/releases/download/[A-Za-z0-9._-]{1,100}/[A-Za-z0-9._-]{1,128}$"
)


class ReleasePipelineError(ValueError):
    def __init__(self, code, message):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.detail = message


def _fail(code, message):
    raise ReleasePipelineError(code, message)


def _is_strict_int(value):
    return type(value) is int and not isinstance(value, bool)


def _require_int(value, low, high, field_name):
    if not _is_strict_int(value) or not (low <= value <= high):
        _fail("invalid_integer", f"Field '{field_name}' must be an integer in [{low}, {high}]")
    return value


def _require_hash(value, field_name="sha256"):
    if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
        _fail("invalid_hash", f"Field '{field_name}' must be 64 lowercase hex characters")
    return value


def _strict_integer_tokens(raw_bytes):
    try:
        text = raw_bytes.decode("utf-8")
    except UnicodeDecodeError:
        return False
    if text.startswith("\ufeff"):
        return False
    quoted = False
    idx = 0
    length = len(text)
    while idx < length:
        ch = text[idx]
        if quoted:
            if ch == "\\":
                idx += 1
            elif ch == '"':
                quoted = False
        elif ch == '"':
            quoted = True
        elif ch == "-" or ("0" <= ch <= "9"):
            magnitude = (ord(ch) - ord("0")) if ("0" <= ch <= "9") else 0
            has_digit = "0" <= ch <= "9"
            while idx + 1 < length:
                nxt = text[idx + 1]
                if nxt in (".", "e", "E"):
                    return False
                if not ("0" <= nxt <= "9"):
                    break
                digit = ord(nxt) - ord("0")
                if magnitude > ((1 << 53) - digit) // 10:
                    return False
                magnitude = magnitude * 10 + digit
                has_digit = True
                idx += 1
            if not has_digit:
                return False
        idx += 1
    return True


def _parse_strict_json(raw_bytes, max_bytes, error_code="invalid_json"):
    if not isinstance(raw_bytes, (bytes, bytearray)):
        _fail(error_code, "Expected raw UTF-8 bytes")
    raw_bytes = bytes(raw_bytes)
    if len(raw_bytes) > max_bytes:
        _fail("size_limit", f"JSON exceeds limit of {max_bytes} bytes")
    if not _strict_integer_tokens(raw_bytes):
        _fail(error_code, "JSON contains non-integer numeric tokens, UTF-8 BOM, or overflow")

    def unique_pairs(items):
        obj = {}
        for key, val in items:
            if key in obj:
                _fail(error_code, f"Duplicate JSON key: {key}")
            obj[key] = val
        return obj

    try:
        return json.loads(
            raw_bytes.decode("utf-8"),
            object_pairs_hook=unique_pairs,
            parse_constant=lambda c: _fail(error_code, f"Invalid JSON constant: {c}"),
        )
    except UnicodeDecodeError as exc:
        _fail(error_code, f"Invalid UTF-8: {exc}")
    except json.JSONDecodeError as exc:
        _fail(error_code, f"Malformed JSON: {exc}")


def _exact_fields(obj, required, optional=(), context="object"):
    if not isinstance(obj, dict):
        _fail("invalid_format", f"{context} must be a JSON object")
    req_set = set(required)
    opt_set = set(optional)
    missing = req_set - set(obj.keys())
    if missing:
        _fail("invalid_format", f"{context} missing required fields: {sorted(missing)}")
    extra = set(obj.keys()) - (req_set | opt_set)
    if extra:
        _fail("invalid_format", f"{context} has unknown fields: {sorted(extra)}")


def _validate_text(value, limit, field_name="text"):
    if not isinstance(value, str):
        _fail("invalid_text", f"{field_name} must be a string")
    if len(value) == 0 or len(value) > limit:
        _fail("invalid_text", f"{field_name} length must be 1..{limit} Unicode scalars")
    nonspace = False
    for ch in value:
        cp = ord(ch)
        if cp < 32 and ch != "\n":
            _fail("invalid_text", f"{field_name} contains disallowed control character U+{cp:04X}")
        if cp not in _UNICODE_WHITESPACE:
            nonspace = True
    if not nonspace:
        _fail("invalid_text", f"{field_name} must not be whitespace-only")
    return value


def _validate_localized(value, limit, field_name="localized"):
    _exact_fields(value, ("zh", "en"), ("tw",), context=field_name)
    out = {
        "zh": _validate_text(value["zh"], limit, f"{field_name}.zh"),
        "en": _validate_text(value["en"], limit, f"{field_name}.en"),
    }
    if "tw" in value:
        out["tw"] = _validate_text(value["tw"], limit, f"{field_name}.tw")
    return out


def _coerce_localized(value, limit, field_name="localized"):
    if isinstance(value, str):
        text = _validate_text(value, limit, field_name)
        return {"zh": text, "en": text}
    return _validate_localized(value, limit, field_name)


def _validate_date(text, field_name="date"):
    if not isinstance(text, str) or len(text) != 10 or not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", text):
        _fail("invalid_date", f"{field_name} must match YYYY-MM-DD")
    year = int(text[0:4])
    month = int(text[5:7])
    day = int(text[8:10])
    if year == 0 or not (1 <= month <= 12):
        _fail("invalid_date", f"{field_name} has out-of-range year or month: {text}")
    days = (31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)
    leap = (year % 4 == 0) and (year % 100 != 0 or year % 400 == 0)
    max_day = days[month - 1] + (1 if (month == 2 and leap) else 0)
    if not (1 <= day <= max_day):
        _fail("invalid_date", f"{field_name} has invalid calendar day: {text}")
    return text


def _validate_utc_timestamp(text, field_name="timestamp"):
    if not isinstance(text, str) or len(text) != 20 or text[10] != "T":
        _fail("invalid_timestamp", f"{field_name} must match YYYY-MM-DDTHH:MM:SSZ")
    _validate_date(text[:10], field_name)
    if not re.fullmatch(r"([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]Z", text[11:]):
        _fail("invalid_timestamp", f"{field_name} time part must match HH:MM:SSZ")
    return text


def _utc_now_str():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _parse_utc_timestamp(text, field_name="timestamp"):
    _validate_utc_timestamp(text, field_name)
    return datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def _validate_https_url(url, field_name="url"):
    if not isinstance(url, str) or len(url) > 2048 or not _HTTPS_RE.fullmatch(url):
        _fail("invalid_url", f"{field_name} is not a valid HTTPS URL")
    slash_idx = url.find("/", 8)
    host = url[8:] if slash_idx < 0 else url[8:slash_idx]
    if len(host) > 253 or "." not in host or host.endswith("."):
        _fail("invalid_url", f"{field_name} has malformed host")
    labels = host.split(".")
    numeric = True
    for label in labels:
        if not label or len(label) > 63 or label.startswith("-") or label.endswith("-"):
            _fail("invalid_url", f"{field_name} has invalid DNS label")
        if any(not ("0" <= c <= "9") for c in label):
            numeric = False
    lower_host = host.lower()
    if numeric or lower_host.endswith(".localhost") or lower_host.endswith(".local"):
        _fail("invalid_url", f"{field_name} must not target numeric IPs or local hosts")
    if slash_idx >= 0:
        path = url[slash_idx:]
        if "/../" in path or "/./" in path or path.endswith("/..") or path.endswith("/."):
            _fail("invalid_url", f"{field_name} must not contain dot path segments")
        idx = 0
        while idx < len(path):
            if path[idx] == "%":
                if idx + 2 >= len(path) or not re.fullmatch(r"[0-9A-Fa-f]{2}", path[idx + 1:idx + 3]):
                    _fail("invalid_url", f"{field_name} has malformed percent-encoding")
                code = int(path[idx + 1:idx + 3], 16)
                if code <= 32 or code == 127 or chr(code) in ("/", "\\", ".", "%", ":", "@"):
                    _fail("invalid_url", f"{field_name} contains disallowed percent-encoded byte")
                idx += 2
            idx += 1
    return url


def _validate_github_asset_url(url, suffix, field_name="asset.url"):
    _validate_https_url(url, field_name)
    if not _GITHUB_ASSET_RE.fullmatch(url) or not url.endswith(suffix):
        _fail(
            "invalid_asset_url",
            f"{field_name} must be a canonical GitHub Release download URL ending with '{suffix}'",
        )
    return url


def parse_semver(version_text):
    if not isinstance(version_text, str):
        _fail("invalid_version", "Version must be a string")
    match = _SEMVER_RE.fullmatch(version_text)
    if not match:
        _fail("invalid_version", f"Invalid SemVer '{version_text}'; expected MAJOR.MINOR.PATCH without leading zeros")
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)))


def _decode_key_material(source, allow_private=False):
    """Accept raw CNG blob bytes, Path, JSON keyfile, lowercase hex, or base64."""
    if isinstance(source, (bytes, bytearray)):
        raw = bytes(source)
        if raw[:4] in (b"RSA1", b"RSA2", b"RSA3"):
            return raw
        try:
            text = raw.decode("utf-8").strip()
        except UnicodeDecodeError:
            return raw
        return _decode_key_string(text, allow_private=allow_private)

    if isinstance(source, Path):
        if not source.is_file():
            _fail("key_not_found", f"Key file does not exist: {source}")
        data = package_store.read_bytes(source.resolve(), 32768)
        return _decode_key_material(data, allow_private=allow_private)

    if isinstance(source, str):
        stripped = source.strip()
        # Check if source refers to an existing file path when short enough.
        if len(stripped) <= 1024 and "\n" not in stripped and "\r" not in stripped:
            try:
                candidate = Path(stripped)
                if candidate.is_file():
                    data = package_store.read_bytes(candidate.resolve(), 32768)
                    return _decode_key_material(data, allow_private=allow_private)
            except OSError:
                pass
        return _decode_key_string(stripped, allow_private=allow_private)

    if isinstance(source, dict):
        if allow_private:
            for field in ("privateKey", "privateKeyHex"):
                if isinstance(source.get(field), str):
                    return _decode_key_string(source[field].strip(), allow_private=True)
        else:
            for field in ("publicKeyHex", "publicKey"):
                if isinstance(source.get(field), str):
                    return _decode_key_string(source[field].strip(), allow_private=False)

    _fail("invalid_key", "Unsupported key input type")


def _decode_key_string(text, allow_private=False):
    if not text:
        _fail("invalid_key", "Empty key material")
    if text.startswith("{"):
        doc = _parse_strict_json(text.encode("utf-8"), 32768, error_code="invalid_key")
        return _decode_key_material(doc, allow_private=allow_private)
    if len(text) % 2 == 0 and all(c in "0123456789abcdef" for c in text):
        return bytes.fromhex(text)
    try:
        decoded = base64.b64decode(text, validate=True)
        if decoded[:4] in (b"RSA1", b"RSA2", b"RSA3"):
            return decoded
    except ValueError:
        pass
    _fail("invalid_key", "Key must be canonical lowercase hex, valid base64 CNG blob, or keyfile JSON")


def load_public_key_blob(source):
    blob = _decode_key_material(source, allow_private=False)
    try:
        validated = package_signing.public_blob(blob)
    except ValueError as exc:
        _fail("invalid_public_key", str(exc))
    cng = package_signing.Cng()
    try:
        cng.import_key(validated, False)
    except ValueError as exc:
        _fail("invalid_public_key", str(exc))
    finally:
        cng.close()
    pin = hashlib.sha256(validated).hexdigest()
    return validated, validated.hex(), pin


def _import_private_blob(cng, raw_blob):
    if len(raw_blob) < 24 or len(raw_blob) > 8192:
        _fail("invalid_private_key", "Private key blob size is out of bounds")
    magic = raw_blob[:4]
    if magic == b"RSA3":
        blob_kind = "RSAFULLPRIVATEBLOB"
    elif magic == b"RSA2":
        blob_kind = "RSAPRIVATEBLOB"
    else:
        _fail("invalid_private_key", "Expected CNG RSAPRIVATEBLOB (RSA2) or RSAFULLPRIVATEBLOB (RSA3)")
    _, bits, exp_cb, mod_cb, p1_cb, p2_cb = struct.unpack("<6I", raw_blob[:24])
    if not (2048 <= bits <= 4096) or bits % 8 != 0 or mod_cb != bits // 8 or not (1 <= exp_cb <= 8) or not p1_cb or not p2_cb:
        _fail("invalid_private_key", "Only RSA 2048..4096 CNG private blobs are accepted")
    if cng.key:
        cng.dll.BCryptDestroyKey(cng.key)
        cng.key = ctypes.c_void_p()
    buf = ctypes.create_string_buffer(raw_blob)
    try:
        cng.check(
            cng.dll.BCryptImportKeyPair(
                cng.algorithm, None, blob_kind, ctypes.byref(cng.key), buf, len(raw_blob), 0
            )
        )
    except ValueError as exc:
        _fail("invalid_private_key", str(exc))


def load_private_key_blobs(source):
    """Return (full_private_blob, rsa_private_blob, public_blob, publisher_id)."""
    raw_blob = _decode_key_material(source, allow_private=True)
    cng = package_signing.Cng()
    try:
        _import_private_blob(cng, raw_blob)
        pub_blob = package_signing.public_blob(cng.export(False))
        full_priv_blob = cng.export(True)
        size = ctypes.c_ulong()
        cng.check(cng.dll.BCryptExportKey(cng.key, None, "RSAPRIVATEBLOB", None, 0, ctypes.byref(size), 0))
        priv_buf = ctypes.create_string_buffer(size.value)
        cng.check(
            cng.dll.BCryptExportKey(cng.key, None, "RSAPRIVATEBLOB", priv_buf, size.value, ctypes.byref(size), 0)
        )
        rsa_priv_blob = priv_buf.raw[:size.value]
    finally:
        cng.close()
    pin = hashlib.sha256(pub_blob).hexdigest()
    return full_priv_blob, rsa_priv_blob, pub_blob, pin


def generate_keypair(bits=3072):
    if not _is_strict_int(bits) or not (2048 <= bits <= 4096) or bits % 8 != 0:
        _fail("invalid_bits", "RSA key length must be an integer multiple of 8 between 2048 and 4096")
    cng = package_signing.Cng()
    try:
        cng.check(cng.dll.BCryptGenerateKeyPair(cng.algorithm, ctypes.byref(cng.key), bits, 0))
        cng.check(cng.dll.BCryptFinalizeKeyPair(cng.key, 0))
        pub_blob = package_signing.public_blob(cng.export(False))
        size = ctypes.c_ulong()
        cng.check(cng.dll.BCryptExportKey(cng.key, None, "RSAPRIVATEBLOB", None, 0, ctypes.byref(size), 0))
        priv_buf = ctypes.create_string_buffer(size.value)
        cng.check(
            cng.dll.BCryptExportKey(cng.key, None, "RSAPRIVATEBLOB", priv_buf, size.value, ctypes.byref(size), 0)
        )
        rsa_priv_blob = priv_buf.raw[:size.value]
    finally:
        cng.close()
    publisher_id = hashlib.sha256(pub_blob).hexdigest()
    return {
        "publisherId": publisher_id,
        "bits": bits,
        "publicKey": base64.b64encode(pub_blob).decode("ascii"),
        "publicKeyHex": pub_blob.hex(),
        "privateKey": base64.b64encode(rsa_priv_blob).decode("ascii"),
    }


def sign_catalog_bytes(catalog_bytes, private_key_source):
    if len(catalog_bytes) > MAX_CATALOG_BYTES:
        _fail("catalog_size_limit", "Catalog exceeds 4 MiB byte limit")
    full_priv_blob, _, pub_blob, publisher_id = load_private_key_blobs(private_key_source)
    cng = package_signing.Cng()
    try:
        cng.import_key(full_priv_blob, True)
        sig_raw = cng.signature(catalog_bytes, domain=CATALOG_SIGNATURE_PREFIX)
    finally:
        cng.close()
    return {
        "formatVersion": 1,
        "algorithm": ALGORITHM,
        "publisher": publisher_id,
        "signature": sig_raw.hex(),
    }


def _verify_catalog_signature_bytes(catalog_bytes, signature_bytes, trusted_pin, trusted_key_hex):
    if len(catalog_bytes) > MAX_CATALOG_BYTES or len(signature_bytes) > MAX_CATALOG_SIGNATURE_BYTES:
        _fail("catalog_size_limit", "Catalog or signature exceeds byte limit")
    sig_doc = _parse_strict_json(signature_bytes, MAX_CATALOG_SIGNATURE_BYTES, error_code="invalid_catalog_signature_format")
    _exact_fields(sig_doc, ("formatVersion", "algorithm", "publisher", "signature"), context="catalog.sig.json")
    _require_int(sig_doc["formatVersion"], 1, 1, "signature.formatVersion")
    if sig_doc["algorithm"] != ALGORITHM:
        _fail("invalid_catalog_signature_format", f"Unsupported signature algorithm: {sig_doc['algorithm']}")
    _require_hash(sig_doc["publisher"], "signature.publisher")
    if sig_doc["publisher"] != trusted_pin:
        _fail("unknown_catalog_signer", f"Catalog signer {sig_doc['publisher']} does not match trusted pin {trusted_pin}")

    pub_blob, actual_key_hex, actual_pin = load_public_key_blob(trusted_key_hex)
    if actual_pin != trusted_pin or actual_key_hex != trusted_key_hex:
        _fail("invalid_catalog_trust_key", "Trusted public key fingerprint does not match trusted pin")

    try:
        sig_raw = package_signing.hex_bytes(sig_doc["signature"])
    except ValueError as exc:
        _fail("invalid_catalog_signature_format", str(exc))
    if not (256 <= len(sig_raw) <= 512):
        _fail("invalid_catalog_signature_format", "RSA signature length must be 256..512 bytes")

    cng = package_signing.Cng()
    try:
        cng.import_key(pub_blob, False)
        cng.signature(catalog_bytes, sig_raw, domain=CATALOG_SIGNATURE_PREFIX)
    except ValueError as exc:
        _fail("catalog_signature_invalid", str(exc))
    finally:
        cng.close()
    return sig_doc


def _default_min_host_version():
    version_file = ROOT / "version.txt"
    if version_file.is_file():
        version = version_file.read_text(encoding="utf-8").strip()
        if _MIN_PULSE_RE.fullmatch(version):
            return version
    return "1.0.60"


def _resolve_manifest_source(source_path):
    source_path = Path(source_path).resolve()
    if source_path.is_dir():
        for candidate_name in ("manifest.json", "pulse.extension.json"):
            candidate = source_path / candidate_name
            if candidate.is_file():
                return candidate, source_path
        _fail("manifest_not_found", f"No manifest.json or pulse.extension.json found in {source_path}")
    if source_path.is_file():
        return source_path, source_path.parent
    _fail("manifest_not_found", f"Extension source path does not exist: {source_path}")


def _validate_manifest_for_packaging(manifest):
    if not isinstance(manifest, dict):
        _fail("invalid_manifest", "Manifest root must be a JSON object")
    allowed_keys = {"apiVersion", "id", "name", "resources", "native", "contributes"}
    extra_keys = set(manifest.keys()) - allowed_keys
    if extra_keys:
        _fail("invalid_manifest", f"Manifest contains unsupported fields: {sorted(extra_keys)}")
    _require_int(manifest.get("apiVersion"), 1, 1, "manifest.apiVersion")
    ext_id = manifest.get("id")
    if not isinstance(ext_id, str) or not _EXTENSION_ID_RE.fullmatch(ext_id):
        _fail("invalid_manifest", f"Manifest id '{ext_id}' must match catalog extension ID format")
    if not package_store.label(manifest.get("name")):
        _fail("invalid_manifest", "Manifest name must be 1..128 UTF-8 bytes of visible characters")
    resources = manifest.get("resources", [])
    if not isinstance(resources, list) or len(resources) > len(ALLOWED_RESOURCES) or len(set(resources)) != len(resources):
        _fail("invalid_manifest", "Manifest resources must be a unique list of at most 15 declarations")
    for res in resources:
        if not isinstance(res, str) or res not in ALLOWED_RESOURCES:
            _fail("invalid_manifest", f"Unknown manifest resource declaration: {res!r}")
    return manifest


def pack_release(
    source_path,
    version,
    private_key_source,
    output_dir,
    *,
    binary_dir=None,
    library_path=None,
    extra_files=(),
    download_base_url=None,
    package_url=None,
    signature_url=None,
    min_host_version=None,
    sdk_preview=3,
    published=None,
    release_notes=None,
    archive_name=None,
):
    parse_semver(version)
    _require_int(sdk_preview, 1, 99, "sdkPreview")
    min_pulse = min_host_version or _default_min_host_version()
    if not isinstance(min_pulse, str) or not _MIN_PULSE_RE.fullmatch(min_pulse):
        _fail("invalid_min_host", f"Invalid minHostVersion: {min_pulse!r}")

    published_date = published or datetime.now(timezone.utc).strftime("%Y-%m-%d")
    _validate_date(published_date, "published")
    notes_obj = _coerce_localized(
        release_notes if release_notes is not None else f"Release {version}",
        600,
        "releaseNotes",
    )

    manifest_file, default_bin_dir = _resolve_manifest_source(source_path)
    raw_manifest_input = package_store.read_bytes(manifest_file, native_archive.MAX_MANIFEST_BYTES)
    manifest = _parse_strict_json(raw_manifest_input, native_archive.MAX_MANIFEST_BYTES, error_code="invalid_manifest")

    # Strip optional authoring-only version if someone supplied pulse.extension.json with it,
    # only when explicitly matching; otherwise validate strict host manifest schema.
    if "version" in manifest and manifest_file.name == "pulse.extension.json":
        authored_ver = manifest.pop("version")
        if authored_ver != version:
            _fail("version_mismatch", f"pulse.extension.json version '{authored_ver}' != '{version}'")
        raw_manifest_input = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")

    if library_path is not None:
        lib_file = Path(library_path).resolve()
        lib_bytes = package_store.read_bytes(lib_file, package_store.MAX_NATIVE_BYTES)
        lib_sha = package_store.validate_native_binary(lib_bytes)
        native_obj = {
            "version": 1,
            "architecture": "windows-x64",
            "library": lib_file.name,
            "sha256": lib_sha,
        }
        files_map = {lib_file.name: lib_bytes}
        if extra_files:
            native_obj["files"] = []
            for extra in extra_files:
                extra_path = Path(extra).resolve()
                extra_data = package_store.read_bytes(extra_path, package_store.MAX_NATIVE_BYTES)
                extra_sha = hashlib.sha256(extra_data).hexdigest()
                native_obj["files"].append({"path": extra_path.name, "sha256": extra_sha})
                files_map[extra_path.name] = extra_data
        manifest["native"] = native_obj
        package_store.native_declaration(manifest)
        package_store.validate_native_files(native_obj, files_map)
        manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    else:
        manifest_bytes = raw_manifest_input
        native_obj = package_store.native_declaration(manifest)
        if native_obj is not None:
            resolved_bin_dir = Path(binary_dir).resolve() if binary_dir is not None else default_bin_dir
            files_map = package_store.read_native_files(resolved_bin_dir, native_obj)
        else:
            files_map = None

    _validate_manifest_for_packaging(manifest)
    if len(manifest_bytes) > native_archive.MAX_MANIFEST_BYTES:
        _fail("manifest_too_large", "Manifest exceeds 64 KiB")

    full_priv_blob, _, pub_blob, publisher_id = load_private_key_blobs(private_key_source)

    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    ext_id = manifest["id"]
    pkg_filename = archive_name or f"{ext_id}-{version}.pulse-ext"
    if not pkg_filename.endswith(".pulse-ext"):
        _fail("invalid_archive_name", "Archive filename must end with .pulse-ext")
    sig_filename = f"{pkg_filename}.sig.json"
    sha_filename = f"{pkg_filename}.sha256"

    if download_base_url is None and (package_url is None or signature_url is None):
        tag = f"{ext_id}-{version}"
        download_base_url = f"https://github.com/jimmgreen/pulse-extensions/releases/download/{tag}"
    if download_base_url is not None:
        base = download_base_url.rstrip("/")
        resolved_pkg_url = package_url or f"{base}/{pkg_filename}"
        resolved_sig_url = signature_url or f"{base}/{sig_filename}"
    else:
        resolved_pkg_url = package_url
        resolved_sig_url = signature_url

    _validate_github_asset_url(resolved_pkg_url, ".pulse-ext", "url")
    _validate_github_asset_url(resolved_sig_url, ".sig.json", "signatureUrl")

    metadata = native_archive.metadata_for(manifest_bytes, manifest)
    binary_bytes = files_map[native_obj["library"]] if native_obj is not None else None
    snapshot = native_archive.PackageSnapshot(metadata, manifest_bytes, binary_bytes, files_map)

    buf = io.BytesIO()
    native_archive.write(buf, snapshot)
    archive_bytes = buf.getvalue()
    verified_snapshot = native_archive.parse(archive_bytes)
    if verified_snapshot.manifest_bytes != manifest_bytes:
        _fail("archive_mismatch", "Reparsed archive manifest bytes do not match input")

    sig_doc = package_signing.sign(verified_snapshot.manifest_bytes, full_priv_blob)
    package_signing.verify_with_public_key(verified_snapshot.manifest_bytes, sig_doc, pub_blob)
    if sig_doc["publisher"] != publisher_id:
        _fail("publisher_mismatch", "Derived signature publisher ID mismatch")

    sig_bytes = (json.dumps(sig_doc, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    archive_sha256 = hashlib.sha256(archive_bytes).hexdigest()
    sig_sha256 = hashlib.sha256(sig_bytes).hexdigest()
    manifest_sha256 = hashlib.sha256(verified_snapshot.manifest_bytes).hexdigest()

    archive_path = output_dir / pkg_filename
    sig_path = output_dir / sig_filename
    sha_path = output_dir / sha_filename
    entry_path = output_dir / "release-entry.json"

    archive_path.write_bytes(archive_bytes)
    sig_path.write_bytes(sig_bytes)
    sha_path.write_bytes(f"{archive_sha256}  {pkg_filename}\n".encode("utf-8"))

    entry = {
        "formatVersion": 1,
        "id": ext_id,
        "name": manifest["name"],
        "publisher": publisher_id,
        "version": version,
        "size": len(archive_bytes),
        "sha256": archive_sha256,
        "url": resolved_pkg_url,
        "signatureUrl": resolved_sig_url,
        "signatureSize": len(sig_bytes),
        "signatureSha256": sig_sha256,
        "manifestSha256": manifest_sha256,
        "minHostVersion": min_pulse,
        "sdkPreview": sdk_preview,
        "architecture": native_obj["architecture"] if native_obj is not None else "any",
        "resources": list(manifest.get("resources", [])),
        "nativeCode": native_obj is not None,
        "published": published_date,
        "releaseNotes": notes_obj,
        "archiveFile": pkg_filename,
        "signatureFile": sig_filename,
        "sha256File": sha_filename,
    }
    entry_path.write_bytes((json.dumps(entry, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return {
        "archivePath": str(archive_path),
        "signaturePath": str(sig_path),
        "sha256Path": str(sha_path),
        "releaseEntryPath": str(entry_path),
        "entry": entry,
    }


def install_release_archive(
    archive_path,
    store_root,
    *,
    signature_path=None,
    trusted_publishers=(),
    allow_native=True,
    accept_capabilities=True,
    enable=True,
):
    """Install a signed .pulse-ext + .sig.json into an isolated package_store with pinned publishers."""
    archive_path = Path(archive_path).resolve()
    store_root = Path(store_root).resolve()
    if signature_path is None:
        cand_a = Path(str(archive_path) + ".sig.json")
        cand_b = archive_path.with_suffix(".sig.json")
        signature_path = cand_a if cand_a.is_file() else cand_b
    else:
        signature_path = Path(signature_path).resolve()

    policy_entries = []
    seen_pins = set()
    for pub_item in trusted_publishers:
        _, pub_hex, pin = load_public_key_blob(pub_item)
        if pin not in seen_pins:
            seen_pins.add(pin)
            policy_entries.append({"id": pin, "publicKey": pub_hex})

    with package_store.locked(store_root, create=True) as locked_root:
        policy_doc = {
            "formatVersion": 1,
            "requireSigned": True,
            "publishers": policy_entries,
        }
        package_signing.policy_keys(policy_doc)
        package_signing.atomic_write(
            locked_root / "signature-policy.json",
            (json.dumps(policy_doc, indent=2) + "\n").encode("utf-8"),
        )

    snapshot = native_archive.read(archive_path)
    sig_bytes = package_store.read_bytes(signature_path, MAX_CATALOG_SIGNATURE_BYTES)
    installed = package_store.install(
        store_root,
        snapshot.manifest_bytes,
        accept_capabilities=accept_capabilities,
        signature=sig_bytes,
        allow_native=allow_native,
        binary_data=snapshot.binary_bytes,
        file_data=snapshot.files,
    )
    if enable and not installed["package"]["enabled"]:
        installed = package_store.update(
            store_root,
            snapshot.metadata["id"],
            "enable",
            allow_native=allow_native,
        )
    return installed


def _normalize_publisher_spec(spec):
    if not isinstance(spec, dict):
        _fail("invalid_publisher", "Publisher metadata must be an object")
    allowed = {"id", "name", "publicKey", "official", "url"}
    extra = set(spec.keys()) - allowed
    if extra:
        _fail("invalid_publisher", f"Unknown publisher fields: {sorted(extra)}")
    for req in ("name", "publicKey", "official"):
        if req not in spec:
            _fail("invalid_publisher", f"Publisher missing required field '{req}'")
    if type(spec["official"]) is not bool:
        _fail("invalid_publisher", "Publisher 'official' must be a boolean")
    _, pub_hex, derived_pin = load_public_key_blob(spec["publicKey"])
    if "id" in spec and spec["id"] != derived_pin:
        _fail("invalid_publisher", f"Publisher id {spec['id']} != public key fingerprint {derived_pin}")
    out = {
        "id": derived_pin,
        "name": _validate_text(spec["name"], 60, "publisher.name"),
        "publicKey": pub_hex,
        "official": spec["official"],
    }
    if "url" in spec and spec["url"] is not None:
        out["url"] = _validate_https_url(spec["url"], "publisher.url")
    return out


def _inspect_and_verify_local_archive(archive_path, sig_path, publishers_by_id):
    archive_path = Path(archive_path).resolve()
    if sig_path is None:
        candidate_a = Path(str(archive_path) + ".sig.json")
        candidate_b = archive_path.with_suffix(".sig.json")
        sig_path = candidate_a if candidate_a.is_file() else candidate_b
    else:
        sig_path = Path(sig_path).resolve()
    if not archive_path.is_file():
        _fail("missing_artifact", f"Package archive does not exist: {archive_path}")
    if not sig_path.is_file():
        _fail("missing_artifact", f"Package signature does not exist: {sig_path}")

    archive_bytes = package_store.read_bytes(archive_path, MAX_PACKAGE_ASSET_BYTES)
    sig_bytes = package_store.read_bytes(sig_path, MAX_CATALOG_SIGNATURE_BYTES)
    if len(archive_bytes) == 0 or len(sig_bytes) == 0:
        _fail("invalid_artifact", "Package or signature file is empty")

    try:
        snapshot = native_archive.parse(archive_bytes)
        sig_doc = package_signing.parse(sig_bytes)
    except (ValueError, package_store.StoreError) as exc:
        _fail("invalid_artifact", str(exc))

    manifest = _parse_strict_json(snapshot.manifest_bytes, native_archive.MAX_MANIFEST_BYTES, error_code="invalid_manifest")
    _validate_manifest_for_packaging(manifest)

    pub_id = sig_doc.get("publisher")
    if pub_id not in publishers_by_id:
        _fail("unknown_extension_publisher", f"Package signer {pub_id!r} is not in catalog publishers")
    pub_blob = bytes.fromhex(publishers_by_id[pub_id]["publicKey"])
    try:
        package_signing.verify_with_public_key(snapshot.manifest_bytes, sig_doc, pub_blob)
    except ValueError as exc:
        _fail("package_signature_invalid", f"Local package signature failed verification: {exc}")

    return {
        "id": manifest["id"],
        "publisher": pub_id,
        "size": len(archive_bytes),
        "sha256": hashlib.sha256(archive_bytes).hexdigest(),
        "signatureSize": len(sig_bytes),
        "signatureSha256": hashlib.sha256(sig_bytes).hexdigest(),
        "manifestSha256": hashlib.sha256(snapshot.manifest_bytes).hexdigest(),
        "resources": list(manifest.get("resources", [])),
        "archiveName": archive_path.name,
        "signatureName": sig_path.name,
    }


def _normalize_release_item(item, ext_id, ext_publisher, publishers_by_id, base_dir=None):
    """Build a strict catalog.cpp Release object from release-entry dict/path or local .pulse-ext."""
    entry_dir = base_dir
    if isinstance(item, (str, Path)):
        p = Path(item).resolve()
        if p.name.endswith(".pulse-ext"):
            item = {"archivePath": str(p)}
            entry_dir = p.parent
        else:
            raw = package_store.read_bytes(p, 262144)
            item = _parse_strict_json(raw, 262144, error_code="invalid_release_entry")
            entry_dir = p.parent
    elif not isinstance(item, dict):
        _fail("invalid_release_entry", "Release item must be a dict or path")
    else:
        item = dict(item)

    # Check if local .pulse-ext is referenced or adjacent to release-entry.json for live verification.
    local_archive = None
    local_sig = None
    if "archivePath" in item and item["archivePath"]:
        cand = Path(item["archivePath"])
        if not cand.is_absolute() and entry_dir is not None:
            cand = entry_dir / cand
        if cand.is_file():
            local_archive = cand
    elif "archiveFile" in item and entry_dir is not None:
        cand = entry_dir / item["archiveFile"]
        if cand.is_file():
            local_archive = cand

    if "signaturePath" in item and item["signaturePath"]:
        cand_s = Path(item["signaturePath"])
        if not cand_s.is_absolute() and entry_dir is not None:
            cand_s = entry_dir / cand_s
        if cand_s.is_file():
            local_sig = cand_s
    elif "signatureFile" in item and entry_dir is not None:
        cand_s = entry_dir / item["signatureFile"]
        if cand_s.is_file():
            local_sig = cand_s

    inspected = None
    if local_archive is not None:
        inspected = _inspect_and_verify_local_archive(local_archive, local_sig, publishers_by_id)
        if inspected["id"] != ext_id:
            _fail("manifest_catalog_identity_mismatch", f"Archive id '{inspected['id']}' != extension '{ext_id}'")
        if inspected["publisher"] != ext_publisher:
            _fail("package_publisher_mismatch", f"Archive signer '{inspected['publisher']}' != extension publisher '{ext_publisher}'")

    if "id" in item and item["id"] != ext_id:
        _fail("manifest_catalog_identity_mismatch", f"Release entry id '{item['id']}' != extension '{ext_id}'")
    if "publisher" in item and item["publisher"] != ext_publisher:
        _fail("package_publisher_mismatch", f"Release entry publisher '{item['publisher']}' != '{ext_publisher}'")

    version = item.get("version")
    if version is None and inspected is not None:
        arch_name = inspected["archiveName"]
        prefix = f"{ext_id}-"
        if arch_name.startswith(prefix) and arch_name.endswith(".pulse-ext"):
            version = arch_name[len(prefix):-len(".pulse-ext")]
    parse_semver(version)

    released = item.get("released") or item.get("published") or datetime.now(timezone.utc).strftime("%Y-%m-%d")
    _validate_date(released, "released")

    min_pulse = item.get("minPulse") or item.get("minHostVersion") or _default_min_host_version()
    if not isinstance(min_pulse, str) or not _MIN_PULSE_RE.fullmatch(min_pulse):
        _fail("invalid_min_pulse", f"Invalid minPulse '{min_pulse}'")

    sdk_preview = item.get("sdkPreview", 3)
    _require_int(sdk_preview, 1, 99, "sdkPreview")

    manifest_sha = item.get("manifestSha256") or (inspected["manifestSha256"] if inspected else None)
    _require_hash(manifest_sha, "manifestSha256")
    if inspected and manifest_sha != inspected["manifestSha256"]:
        _fail("manifest_hash_mismatch", "Release entry manifestSha256 differs from local .pulse-ext")

    resources = item.get("resources") if "resources" in item else (inspected["resources"] if inspected else [])
    if not isinstance(resources, list) or len(resources) > len(ALLOWED_RESOURCES) or len(set(resources)) != len(resources):
        _fail("invalid_resources", "Release resources must be a unique list of valid resource declarations")
    for res in resources:
        if not isinstance(res, str) or res not in ALLOWED_RESOURCES:
            _fail("invalid_resources", f"Unknown resource declaration: {res!r}")
    if inspected and sorted(resources) != sorted(inspected["resources"]):
        _fail("manifest_catalog_identity_mismatch", "Release resources differ from local .pulse-ext manifest")

    if isinstance(item.get("package"), dict):
        pkg_asset_in = item["package"]
        pkg_url = pkg_asset_in.get("url")
        pkg_size = pkg_asset_in.get("size")
        pkg_sha = pkg_asset_in.get("sha256")
    else:
        pkg_url = item.get("url")
        pkg_size = item.get("size") if "size" in item else (inspected["size"] if inspected else None)
        pkg_sha = item.get("sha256") if "sha256" in item else (inspected["sha256"] if inspected else None)
        if pkg_url is None and inspected and item.get("downloadBaseUrl"):
            pkg_url = item["downloadBaseUrl"].rstrip("/") + "/" + inspected["archiveName"]
        elif pkg_url is None and inspected:
            pkg_url = f"https://github.com/jimmgreen/pulse-extensions/releases/download/{ext_id}-{version}/{inspected['archiveName']}"

    if isinstance(item.get("signature"), dict):
        sig_asset_in = item["signature"]
        sig_url = sig_asset_in.get("url")
        sig_size = sig_asset_in.get("signatureSize", sig_asset_in.get("size"))
        sig_sha = sig_asset_in.get("sha256")
    else:
        sig_url = item.get("signatureUrl")
        sig_size = item.get("signatureSize") if "signatureSize" in item else (inspected["signatureSize"] if inspected else None)
        sig_sha = item.get("signatureSha256") if "signatureSha256" in item else (inspected["signatureSha256"] if inspected else None)
        if sig_url is None and inspected and item.get("downloadBaseUrl"):
            sig_url = item["downloadBaseUrl"].rstrip("/") + "/" + inspected["signatureName"]
        elif sig_url is None and inspected:
            sig_url = f"https://github.com/jimmgreen/pulse-extensions/releases/download/{ext_id}-{version}/{inspected['signatureName']}"

    _validate_github_asset_url(pkg_url, ".pulse-ext", "package.url")
    _require_int(pkg_size, 1, MAX_PACKAGE_ASSET_BYTES, "package.size")
    _require_hash(pkg_sha, "package.sha256")

    _validate_github_asset_url(sig_url, ".sig.json", "signature.url")
    _require_int(sig_size, 1, MAX_CATALOG_SIGNATURE_BYTES, "signature.size")
    _require_hash(sig_sha, "signature.sha256")

    if inspected:
        if pkg_size != inspected["size"] or pkg_sha != inspected["sha256"]:
            _fail("package_asset_mismatch", "Release package size/sha256 differs from local .pulse-ext")
        if sig_size != inspected["signatureSize"] or sig_sha != inspected["signatureSha256"]:
            _fail("signature_asset_mismatch", "Release signature size/sha256 differs from local .sig.json")

    raw_notes = item.get("notes") if "notes" in item else item.get("releaseNotes", f"Release {version}")
    notes = _coerce_localized(raw_notes, 600, "notes")

    return {
        "version": version,
        "released": released,
        "minPulse": min_pulse,
        "sdkPreview": sdk_preview,
        "manifestSha256": manifest_sha,
        "resources": list(resources),
        "package": {
            "url": pkg_url,
            "size": pkg_size,
            "sha256": pkg_sha,
        },
        "signature": {
            "url": sig_url,
            "size": sig_size,
            "sha256": sig_sha,
        },
        "notes": notes,
    }


def _normalize_extension_spec(spec, publishers_by_id, extra_entries=(), base_dir=None):
    if not isinstance(spec, dict):
        _fail("invalid_extension", "Extension metadata must be an object")
    allowed = {
        "formatVersion", "id", "publisher", "name", "summary", "description",
        "category", "icon", "license", "homepage", "source", "tags", "versions", "entries",
    }
    extra = set(spec.keys()) - allowed
    if extra:
        _fail("invalid_extension", f"Unknown extension fields: {sorted(extra)}")
    if "formatVersion" in spec:
        _require_int(spec["formatVersion"], 1, 1, "extension.formatVersion")

    ext_id = spec.get("id")
    if not isinstance(ext_id, str) or not _EXTENSION_ID_RE.fullmatch(ext_id):
        _fail("invalid_extension", f"Invalid extension id: {ext_id!r}")
    pub_id = _require_hash(spec.get("publisher"), "extension.publisher")
    if pub_id not in publishers_by_id:
        _fail("unknown_extension_publisher", f"Extension '{ext_id}' references unknown publisher {pub_id}")
    if ext_id.startswith("pulse.") and not publishers_by_id[pub_id]["official"]:
        _fail("reserved_extension_namespace", f"Reserved 'pulse.*' extension '{ext_id}' requires an official publisher")

    name = _coerce_localized(spec.get("name"), 40, "extension.name")
    summary = _coerce_localized(spec.get("summary"), 120, "extension.summary")
    description = _coerce_localized(spec.get("description"), 1200, "extension.description")

    category = spec.get("category")
    if not isinstance(category, str) or category not in ALLOWED_CATEGORIES:
        _fail("invalid_extension", f"Invalid category {category!r}; expected one of {sorted(ALLOWED_CATEGORIES)}")

    icon = spec.get("icon")
    _exact_fields(icon, ("glyph", "color"), context="extension.icon")
    if not isinstance(icon["glyph"], str) or icon["glyph"] not in ALLOWED_GLYPHS:
        _fail("invalid_extension", f"Invalid icon glyph {icon['glyph']!r}")
    if not isinstance(icon["color"], str) or not _COLOR_RE.fullmatch(icon["color"]):
        _fail("invalid_extension", f"Invalid icon color {icon['color']!r}; expected #RRGGBB uppercase hex")

    license_text = _validate_text(spec.get("license"), 80, "extension.license")

    raw_versions = list(spec.get("versions", [])) + list(spec.get("entries", [])) + list(extra_entries)
    if not (1 <= len(raw_versions) <= 20):
        _fail("invalid_extension", f"Extension '{ext_id}' must provide 1..20 versions, got {len(raw_versions)}")

    normalized_releases = []
    seen_versions = set()
    for rel_item in raw_versions:
        rel_obj = _normalize_release_item(rel_item, ext_id, pub_id, publishers_by_id, base_dir=base_dir)
        ver_tuple = parse_semver(rel_obj["version"])
        if ver_tuple in seen_versions:
            _fail("duplicate_version", f"Duplicate version {rel_obj['version']} in extension '{ext_id}'")
        seen_versions.add(ver_tuple)
        normalized_releases.append((ver_tuple, rel_obj))

    normalized_releases.sort(key=lambda pair: pair[0], reverse=True)

    ext_out = {
        "formatVersion": 1,
        "id": ext_id,
        "publisher": pub_id,
        "name": name,
        "summary": summary,
        "description": description,
        "category": category,
        "icon": {"glyph": icon["glyph"], "color": icon["color"]},
        "license": license_text,
    }
    if "homepage" in spec and spec["homepage"] is not None:
        ext_out["homepage"] = _validate_https_url(spec["homepage"], "extension.homepage")
    if "source" in spec and spec["source"] is not None:
        ext_out["source"] = _validate_https_url(spec["source"], "extension.source")
    if "tags" in spec and spec["tags"] is not None:
        tags = spec["tags"]
        if not isinstance(tags, list) or len(tags) > 8:
            _fail("invalid_extension", "extension.tags must be an array of at most 8 strings")
        ext_out["tags"] = [_validate_text(t, 20, "extension.tag") for t in tags]
    ext_out["versions"] = [rel for _, rel in normalized_releases]
    return ext_out


def build_catalog(
    publishers,
    extensions,
    output_path,
    *,
    release_entries=(),
    previous_catalog=None,
    sequence=None,
    generated=None,
    expires=None,
    expires_days=30,
    private_key_source=None,
    signature_output_path=None,
    base_dir=None,
):
    gen_str = generated or _utc_now_str()
    gen_dt = _parse_utc_timestamp(gen_str, "generated")

    if expires is not None:
        exp_str = _validate_utc_timestamp(expires, "expires")
        exp_dt = _parse_utc_timestamp(exp_str, "expires")
    else:
        if not _is_strict_int(expires_days) or not (1 <= expires_days <= 3650):
            _fail("invalid_expires_days", "expires_days must be an integer between 1 and 3650")
        exp_dt = gen_dt + timedelta(days=expires_days)
        exp_str = exp_dt.strftime("%Y-%m-%dT%H:%M:%SZ")

    if exp_dt <= gen_dt or exp_str <= gen_str:
        _fail("invalid_expires", f"Catalog expires ({exp_str}) must be strictly after generated ({gen_str})")

    prev_seq = None
    if previous_catalog is not None:
        if isinstance(previous_catalog, (str, Path)):
            prev_bytes = package_store.read_bytes(Path(previous_catalog).resolve(), MAX_CATALOG_BYTES)
            prev_doc = _parse_strict_json(prev_bytes, MAX_CATALOG_BYTES, error_code="invalid_previous_catalog")
        elif isinstance(previous_catalog, (bytes, bytearray)):
            prev_doc = _parse_strict_json(bytes(previous_catalog), MAX_CATALOG_BYTES, error_code="invalid_previous_catalog")
        elif isinstance(previous_catalog, dict):
            prev_doc = previous_catalog
        else:
            _fail("invalid_previous_catalog", "Unsupported previous_catalog input")
        if "sequence" not in prev_doc:
            prev_seq = 0
        else:
            prev_seq = _require_int(prev_doc["sequence"], 1, MAX_CATALOG_SEQUENCE, "previous_catalog.sequence")

    if sequence is None:
        resolved_seq = (prev_seq + 1) if prev_seq is not None else 1
    else:
        resolved_seq = _require_int(sequence, 1, MAX_CATALOG_SEQUENCE, "sequence")
        if prev_seq is not None and resolved_seq <= prev_seq:
            _fail(
                "sequence_rollback",
                f"Explicit sequence {resolved_seq} must be strictly greater than previous sequence {prev_seq}",
            )
    _require_int(resolved_seq, 1, MAX_CATALOG_SEQUENCE, "sequence")

    if not isinstance(publishers, list) or len(publishers) > MAX_CATALOG_PUBLISHERS:
        _fail("catalog_entry_limit", "Publishers must be a list of at most 512 entries")
    normalized_publishers = []
    publishers_by_id = {}
    for pub_spec in publishers:
        pub_obj = _normalize_publisher_spec(pub_spec)
        if pub_obj["id"] in publishers_by_id:
            _fail("duplicate_publisher", f"Duplicate publisher id: {pub_obj['id']}")
        publishers_by_id[pub_obj["id"]] = pub_obj
        normalized_publishers.append(pub_obj)

    # Group standalone release_entries by extension id if provided separately.
    grouped_entries = {}
    for raw_entry in release_entries:
        if isinstance(raw_entry, (str, Path)):
            p = Path(raw_entry).resolve()
            if p.name.endswith(".pulse-ext"):
                inspected = _inspect_and_verify_local_archive(p, None, publishers_by_id)
                grouped_entries.setdefault(inspected["id"], []).append(p)
            else:
                doc = _parse_strict_json(package_store.read_bytes(p, 262144), 262144, error_code="invalid_release_entry")
                ext_target = doc.get("id")
                if not isinstance(ext_target, str):
                    _fail("invalid_release_entry", f"Release entry {p} missing 'id'")
                grouped_entries.setdefault(ext_target, []).append(p)
        elif isinstance(raw_entry, dict):
            ext_target = raw_entry.get("id")
            if not isinstance(ext_target, str):
                _fail("invalid_release_entry", "Release entry dict missing 'id'")
            grouped_entries.setdefault(ext_target, []).append(raw_entry)
        else:
            _fail("invalid_release_entry", "Unsupported release entry item")

    if not isinstance(extensions, list) or len(extensions) > MAX_CATALOG_EXTENSIONS:
        _fail("catalog_entry_limit", "Extensions must be a list of at most 2048 entries")

    normalized_extensions = []
    seen_ext_ids = set()
    total_releases = 0
    for ext_spec in extensions:
        if not isinstance(ext_spec, dict) or not isinstance(ext_spec.get("id"), str):
            _fail("invalid_extension", "Each extension spec must be an object with string 'id'")
        ext_id = ext_spec["id"]
        if ext_id in seen_ext_ids:
            _fail("duplicate_extension", f"Duplicate extension id: {ext_id}")
        seen_ext_ids.add(ext_id)
        extra_for_ext = grouped_entries.pop(ext_id, [])
        ext_obj = _normalize_extension_spec(ext_spec, publishers_by_id, extra_entries=extra_for_ext, base_dir=base_dir)
        total_releases += len(ext_obj["versions"])
        if total_releases > MAX_CATALOG_RELEASES:
            _fail("catalog_entry_limit", "Total catalog releases exceed 8192")
        normalized_extensions.append(ext_obj)

    if grouped_entries:
        _fail("unknown_extension", f"Release entries reference undefined extensions: {sorted(grouped_entries.keys())}")

    catalog_doc = {
        "formatVersion": 1,
        "generated": gen_str,
        "expires": exp_str,
        "sequence": resolved_seq,
        "publishers": normalized_publishers,
        "extensions": normalized_extensions,
    }
    catalog_bytes = (json.dumps(catalog_doc, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if len(catalog_bytes) > MAX_CATALOG_BYTES:
        _fail("catalog_size_limit", "Serialized catalog.json exceeds 4 MiB limit")
    if not _strict_integer_tokens(catalog_bytes):
        _fail("invalid_integer", "Serialized catalog contains non-integer numeric tokens")

    output_path = Path(output_path).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(catalog_bytes)

    result = {
        "catalogPath": str(output_path),
        "catalogSha256": hashlib.sha256(catalog_bytes).hexdigest(),
        "catalogBytes": len(catalog_bytes),
        "sequence": resolved_seq,
        "generated": gen_str,
        "expires": exp_str,
        "publisherCount": len(normalized_publishers),
        "extensionCount": len(normalized_extensions),
        "releaseCount": total_releases,
    }

    if private_key_source is not None:
        sig_doc = sign_catalog_bytes(catalog_bytes, private_key_source)
        sig_bytes = (json.dumps(sig_doc, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        resolved_sig_path = (
            Path(signature_output_path).resolve()
            if signature_output_path is not None
            else output_path.parent / "catalog.sig.json"
        )
        resolved_sig_path.parent.mkdir(parents=True, exist_ok=True)
        resolved_sig_path.write_bytes(sig_bytes)
        result["signaturePath"] = str(resolved_sig_path)
        result["signerPublisherId"] = sig_doc["publisher"]
    return result


def verify_catalog(
    catalog_path,
    signature_path=None,
    *,
    trusted_key=None,
    trusted_pin=None,
    min_sequence=1,
    now=None,
    artifacts_dir=None,
):
    catalog_path = Path(catalog_path).resolve()
    if signature_path is None:
        cand_a = catalog_path.parent / "catalog.sig.json"
        cand_b = Path(str(catalog_path) + ".sig.json")
        signature_path = cand_a if cand_a.is_file() else cand_b
    else:
        signature_path = Path(signature_path).resolve()

    if not catalog_path.is_file():
        _fail("catalog_not_found", f"Catalog file does not exist: {catalog_path}")
    if not signature_path.is_file():
        _fail("signature_not_found", f"Catalog signature file does not exist: {signature_path}")

    if trusted_key is None:
        resolved_key_hex = OFFICIAL_PUBLIC_KEY_HEX
        resolved_pin = trusted_pin if trusted_pin is not None else OFFICIAL_PUBLISHER_ID
    else:
        _, resolved_key_hex, derived_pin = load_public_key_blob(trusted_key)
        resolved_pin = trusted_pin if trusted_pin is not None else derived_pin
    _require_hash(resolved_pin, "trusted_pin")
    _, check_hex, check_pin = load_public_key_blob(resolved_key_hex)
    if check_pin != resolved_pin:
        _fail("invalid_catalog_trust_key", f"Trusted key fingerprint {check_pin} != trusted pin {resolved_pin}")

    _require_int(min_sequence, 1, MAX_CATALOG_SEQUENCE, "min_sequence")
    now_str = _validate_utc_timestamp(now, "now") if now is not None else _utc_now_str()

    catalog_bytes = package_store.read_bytes(catalog_path, MAX_CATALOG_BYTES)
    signature_bytes = package_store.read_bytes(signature_path, MAX_CATALOG_SIGNATURE_BYTES)

    _verify_catalog_signature_bytes(catalog_bytes, signature_bytes, resolved_pin, check_hex)

    root = _parse_strict_json(catalog_bytes, MAX_CATALOG_BYTES, error_code="invalid_catalog_format")
    _exact_fields(
        root,
        ("formatVersion", "generated", "publishers", "extensions"),
        ("expires", "sequence"),
        context="catalog.json",
    )
    _require_int(root["formatVersion"], 1, 1, "formatVersion")
    generated = _validate_utc_timestamp(root["generated"], "generated")

    if "expires" not in root:
        _fail("missing_expires", "Catalog requires an explicit 'expires' timestamp")
    expires = root["expires"]
    if isinstance(expires, str) and len(expires) == 10:
        _validate_date(expires, "expires")
        if expires < generated[:10]:
            _fail("invalid_catalog_format", "Catalog expires before generated date")
        if expires <= now_str[:10]:
            _fail("catalog_expired", f"Catalog expired on {expires} (now={now_str})")
    else:
        _validate_utc_timestamp(expires, "expires")
        if expires <= generated:
            _fail("invalid_catalog_format", "Catalog expires must be after generated timestamp")
        if expires <= now_str:
            _fail("catalog_expired", f"Catalog expired at {expires} (now={now_str})")

    if "sequence" not in root:
        _fail("missing_sequence", "Catalog requires a monotonic 'sequence' integer")
    seq = _require_int(root["sequence"], 1, MAX_CATALOG_SEQUENCE, "sequence")
    if seq < min_sequence:
        _fail("catalog_replay_rejected", f"Catalog sequence {seq} < required min_sequence {min_sequence}")

    publishers = root["publishers"]
    extensions = root["extensions"]
    if not isinstance(publishers, list) or len(publishers) > MAX_CATALOG_PUBLISHERS:
        _fail("catalog_entry_limit_or_type", "Invalid publishers array")
    if not isinstance(extensions, list) or len(extensions) > MAX_CATALOG_EXTENSIONS:
        _fail("catalog_entry_limit_or_type", "Invalid extensions array")

    publishers_by_id = {}
    for pub in publishers:
        _exact_fields(pub, ("id", "name", "publicKey", "official"), ("url",), context="publisher")
        _validate_text(pub["name"], 60, "publisher.name")
        if "url" in pub:
            _validate_https_url(pub["url"], "publisher.url")
        if type(pub["official"]) is not bool:
            _fail("invalid_catalog_publisher", "publisher.official must be boolean")
        if not isinstance(pub["publicKey"], str) or any(c not in "0123456789abcdef" for c in pub["publicKey"]):
            _fail("invalid_catalog_publisher", "publisher.publicKey must be lowercase hex")
        _, pub_hex, pin = load_public_key_blob(pub["publicKey"])
        if pub["id"] != pin or pin in publishers_by_id:
            _fail("invalid_catalog_publisher", "Publisher id mismatch or duplicate")
        if pub["official"] and (pin != resolved_pin or pub_hex != check_hex):
            _fail("unknown_official_publisher", "Official publisher does not match pinned official trust key")
        publishers_by_id[pin] = pub

    seen_ext_ids = set()
    total_releases = 0
    verified_artifacts = 0
    artifacts_root = Path(artifacts_dir).resolve() if artifacts_dir is not None else None
    if artifacts_root is not None and not artifacts_root.is_dir():
        _fail("artifacts_not_found", f"Artifacts directory does not exist: {artifacts_root}")

    for ext in extensions:
        _exact_fields(
            ext,
            ("formatVersion", "id", "publisher", "name", "summary", "description", "category", "icon", "license", "versions"),
            ("homepage", "source", "tags"),
            context="extension",
        )
        _require_int(ext["formatVersion"], 1, 1, "extension.formatVersion")
        ext_id = ext["id"]
        if not isinstance(ext_id, str) or not _EXTENSION_ID_RE.fullmatch(ext_id) or ext_id in seen_ext_ids:
            _fail("invalid_catalog_extension", f"Invalid or duplicate extension id: {ext_id!r}")
        seen_ext_ids.add(ext_id)

        pub_id = _require_hash(ext["publisher"], "extension.publisher")
        if pub_id not in publishers_by_id:
            _fail("unknown_extension_publisher", f"Unknown publisher {pub_id} for {ext_id}")
        if ext_id.startswith("pulse.") and not publishers_by_id[pub_id]["official"]:
            _fail("reserved_extension_namespace", f"Reserved 'pulse.*' id '{ext_id}' requires official publisher")

        _validate_localized(ext["name"], 40, "extension.name")
        _validate_localized(ext["summary"], 120, "extension.summary")
        _validate_localized(ext["description"], 1200, "extension.description")

        if not isinstance(ext["category"], str) or ext["category"] not in ALLOWED_CATEGORIES:
            _fail("invalid_catalog_extension", f"Invalid category in {ext_id}")
        _exact_fields(ext["icon"], ("glyph", "color"), context="extension.icon")
        if ext["icon"]["glyph"] not in ALLOWED_GLYPHS or not isinstance(ext["icon"]["color"], str) or not _COLOR_RE.fullmatch(ext["icon"]["color"]):
            _fail("invalid_catalog_extension", f"Invalid icon in {ext_id}")
        _validate_text(ext["license"], 80, "extension.license")
        if "homepage" in ext:
            _validate_https_url(ext["homepage"], "extension.homepage")
        if "source" in ext:
            _validate_https_url(ext["source"], "extension.source")
        if "tags" in ext:
            if not isinstance(ext["tags"], list) or len(ext["tags"]) > 8:
                _fail("invalid_catalog_extension", f"Invalid tags in {ext_id}")
            for tag in ext["tags"]:
                _validate_text(tag, 20, "extension.tag")

        versions = ext["versions"]
        if not isinstance(versions, list) or not (1 <= len(versions) <= 20):
            _fail("invalid_catalog_extension", f"Invalid versions count in {ext_id}")
        total_releases += len(versions)
        if total_releases > MAX_CATALOG_RELEASES:
            _fail("catalog_entry_limit_or_type", "Catalog exceeds max releases")

        prev_ver = (100000, 0, 0)
        for rel in versions:
            _exact_fields(
                rel,
                ("version", "released", "minPulse", "sdkPreview", "manifestSha256", "resources", "package", "signature", "notes"),
                context="release",
            )
            curr_ver = parse_semver(rel["version"])
            if curr_ver >= prev_ver:
                _fail("invalid_version_order", f"Versions in '{ext_id}' must be strictly descending SemVer")
            prev_ver = curr_ver

            _validate_date(rel["released"], "release.released")
            if not isinstance(rel["minPulse"], str) or not _MIN_PULSE_RE.fullmatch(rel["minPulse"]):
                _fail("invalid_catalog_extension", f"Invalid minPulse in {ext_id}@{rel['version']}")
            _require_int(rel["sdkPreview"], 1, 99, "release.sdkPreview")
            _require_hash(rel["manifestSha256"], "release.manifestSha256")

            resources = rel["resources"]
            if not isinstance(resources, list) or len(resources) > len(ALLOWED_RESOURCES) or len(set(resources)) != len(resources):
                _fail("invalid_catalog_extension", f"Invalid resources in {ext_id}@{rel['version']}")
            for res in resources:
                if not isinstance(res, str) or res not in ALLOWED_RESOURCES:
                    _fail("invalid_catalog_extension", f"Unknown resource {res!r} in {ext_id}@{rel['version']}")

            pkg = rel["package"]
            _exact_fields(pkg, ("url", "size", "sha256"), context="release.package")
            _validate_github_asset_url(pkg["url"], ".pulse-ext", "release.package.url")
            _require_int(pkg["size"], 1, MAX_PACKAGE_ASSET_BYTES, "release.package.size")
            _require_hash(pkg["sha256"], "release.package.sha256")

            sig = rel["signature"]
            _exact_fields(sig, ("url", "size", "sha256"), context="release.signature")
            _validate_github_asset_url(sig["url"], ".sig.json", "release.signature.url")
            _require_int(sig["size"], 1, MAX_CATALOG_SIGNATURE_BYTES, "release.signature.size")
            _require_hash(sig["sha256"], "release.signature.sha256")

            _validate_localized(rel["notes"], 600, "release.notes")

            if artifacts_root is not None:
                pkg_filename = pkg["url"].rsplit("/", 1)[-1]
                sig_filename = sig["url"].rsplit("/", 1)[-1]
                pkg_file = _locate_artifact(artifacts_root, pkg_filename)
                sig_file = _locate_artifact(artifacts_root, sig_filename)
                inspected = _inspect_and_verify_local_archive(pkg_file, sig_file, publishers_by_id)
                if inspected["id"] != ext_id:
                    _fail("artifact_mismatch", f"Artifact {pkg_filename} id '{inspected['id']}' != '{ext_id}'")
                if inspected["size"] != pkg["size"] or inspected["sha256"] != pkg["sha256"]:
                    _fail("artifact_mismatch", f"Artifact {pkg_filename} size/sha256 mismatch against catalog")
                if inspected["signatureSize"] != sig["size"] or inspected["signatureSha256"] != sig["sha256"]:
                    _fail("artifact_mismatch", f"Signature {sig_filename} size/sha256 mismatch against catalog")
                if inspected["manifestSha256"] != rel["manifestSha256"]:
                    _fail("artifact_mismatch", f"Artifact {pkg_filename} manifestSha256 mismatch against catalog")
                if sorted(inspected["resources"]) != sorted(resources):
                    _fail("artifact_mismatch", f"Artifact {pkg_filename} resources mismatch against catalog")
                verified_artifacts += 1

    return {
        "ok": True,
        "trustedPublisher": resolved_pin,
        "sequence": seq,
        "generated": generated,
        "expires": expires,
        "publisherCount": len(publishers_by_id),
        "extensionCount": len(seen_ext_ids),
        "releaseCount": total_releases,
        "verifiedArtifacts": verified_artifacts,
    }


def _locate_artifact(artifacts_root, filename):
    direct = artifacts_root / filename
    if direct.is_file():
        return direct
    matches = [p for p in artifacts_root.rglob(filename) if p.is_file()]
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        _fail("ambiguous_artifact", f"Multiple files named {filename} found under {artifacts_root}")
    _fail("missing_artifact", f"Artifact {filename} not found under {artifacts_root}")


def _load_json_arg(path_or_inline):
    p = Path(path_or_inline)
    if p.is_file():
        raw = package_store.read_bytes(p.resolve(), MAX_CATALOG_BYTES)
        return _parse_strict_json(raw, MAX_CATALOG_BYTES)
    return _parse_strict_json(path_or_inline.encode("utf-8"), MAX_CATALOG_BYTES)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_keygen = sub.add_parser("keygen", help="Generate an RSA publisher/catalog signing keypair via Windows CNG")
    p_keygen.add_argument("--bits", type=int, default=3072)
    p_keygen.add_argument("--output", type=Path, help="Optional JSON file to save keypair")
    p_keygen.add_argument("--public-key-out", type=Path, help="Optional path to save raw RSAPUBLICBLOB")
    p_keygen.add_argument("--private-key-out", type=Path, help="Optional path to save raw RSAPRIVATEBLOB")

    p_pack = sub.add_parser("pack-release", help="Pack .pulse-ext, detached .sig.json, .sha256, and release-entry.json")
    p_pack.add_argument("--source", type=Path, required=True, help="Extension directory or manifest file")
    p_pack.add_argument("--version", required=True, help="SemVer release version (e.g. 1.0.0)")
    p_pack.add_argument("--private-key", required=True, help="Private key file, base64, hex, or keygen JSON")
    p_pack.add_argument("--output-dir", type=Path, required=True, help="Output directory for release artifacts")
    p_pack.add_argument("--binary-dir", type=Path, help="Directory containing compiled native DLLs")
    p_pack.add_argument("--library", type=Path, help="Optional primary x64 DLL to attach to manifest")
    p_pack.add_argument("--file", action="append", default=[], type=Path, help="Optional sidecar file for native.files")
    p_pack.add_argument("--download-base-url", help="GitHub Release download base URL")
    p_pack.add_argument("--url", help="Explicit .pulse-ext GitHub Release download URL")
    p_pack.add_argument("--signature-url", help="Explicit .sig.json GitHub Release download URL")
    p_pack.add_argument("--min-host-version", help="Minimum Pulse host version (e.g. 1.0.60)")
    p_pack.add_argument("--sdk-preview", type=int, default=3)
    p_pack.add_argument("--published", help="Release date YYYY-MM-DD (defaults to UTC today)")
    p_pack.add_argument("--release-notes", help="Release notes text or localized JSON object/file")
    p_pack.add_argument("--archive-name", help="Override output .pulse-ext filename")

    p_build = sub.add_parser("build-catalog", help="Build catalog.json and optional catalog.sig.json")
    p_build.add_argument("--config", type=Path, help="Combined JSON config containing publishers and extensions")
    p_build.add_argument("--publishers", help="JSON file or inline array of publisher definitions")
    p_build.add_argument("--extensions", help="JSON file or inline array of extension definitions")
    p_build.add_argument("--entry", action="append", default=[], help="Path to release-entry.json or .pulse-ext (repeatable)")
    p_build.add_argument("--output", type=Path, required=True, help="Output catalog.json path")
    p_build.add_argument("--signature-output", type=Path, help="Output catalog.sig.json path")
    p_build.add_argument("--private-key", help="Catalog signing private key (file, base64, hex, or keygen JSON)")
    p_build.add_argument("--previous-catalog", type=Path, help="Previous catalog.json for monotonic sequence enforcement")
    p_build.add_argument("--sequence", type=int, help="Explicit monotonic positive sequence integer")
    p_build.add_argument("--generated", help="Explicit UTC timestamp YYYY-MM-DDTHH:MM:SSZ")
    p_build.add_argument("--expires", help="Explicit UTC expiry timestamp YYYY-MM-DDTHH:MM:SSZ")
    p_build.add_argument("--expires-days", type=int, default=30, help="Expiry offset in days when --expires is omitted")

    p_verify = sub.add_parser("verify-catalog", help="Verify catalog.json + catalog.sig.json and optional local artifacts")
    p_verify.add_argument("--catalog", type=Path, required=True, help="Path to catalog.json")
    p_verify.add_argument("--signature", type=Path, help="Path to catalog.sig.json")
    p_verify.add_argument("--trusted-key", help="Trusted catalog public key (defaults to official built-in key)")
    p_verify.add_argument("--trusted-pin", help="Trusted 64-hex publisher pin (defaults to official built-in pin)")
    p_verify.add_argument("--min-sequence", type=int, default=1, help="Minimum acceptable sequence number")
    p_verify.add_argument("--now", help="Override current UTC timestamp YYYY-MM-DDTHH:MM:SSZ")
    p_verify.add_argument("--artifacts-dir", type=Path, help="Optional directory containing local .pulse-ext and .sig.json files")

    args = parser.parse_args(argv)
    try:
        if args.command == "keygen":
            kp = generate_keypair(args.bits)
            if args.public_key_out is not None:
                args.public_key_out.parent.mkdir(parents=True, exist_ok=True)
                args.public_key_out.write_bytes(base64.b64decode(kp["publicKey"]))
            if args.private_key_out is not None:
                args.private_key_out.parent.mkdir(parents=True, exist_ok=True)
                args.private_key_out.write_bytes(base64.b64decode(kp["privateKey"]))
            if args.output is not None:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(kp, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(kp, ensure_ascii=False, indent=2))
            return 0

        if args.command == "pack-release":
            notes_val = args.release_notes
            if notes_val and (notes_val.startswith("{") or Path(notes_val).is_file()):
                notes_val = _load_json_arg(notes_val)
            res = pack_release(
                args.source,
                args.version,
                args.private_key,
                args.output_dir,
                binary_dir=args.binary_dir,
                library_path=args.library,
                extra_files=args.file,
                download_base_url=args.download_base_url,
                package_url=args.url,
                signature_url=args.signature_url,
                min_host_version=args.min_host_version,
                sdk_preview=args.sdk_preview,
                published=args.published,
                release_notes=notes_val,
                archive_name=args.archive_name,
            )
            print(json.dumps(res, ensure_ascii=False, indent=2))
            return 0

        if args.command == "build-catalog":
            base_dir = None
            publishers_in = None
            extensions_in = None
            if args.config is not None:
                cfg = _load_json_arg(str(args.config))
                if not isinstance(cfg, dict):
                    _fail("invalid_config", "--config must be a JSON object")
                publishers_in = cfg.get("publishers")
                extensions_in = cfg.get("extensions")
                base_dir = args.config.resolve().parent
            if args.publishers is not None:
                publishers_in = _load_json_arg(args.publishers)
            if args.extensions is not None:
                extensions_in = _load_json_arg(args.extensions)
                if base_dir is None and Path(args.extensions).is_file():
                    base_dir = Path(args.extensions).resolve().parent
            if publishers_in is None or extensions_in is None:
                _fail("missing_inputs", "Supply --config or both --publishers and --extensions")
            res = build_catalog(
                publishers_in,
                extensions_in,
                args.output,
                release_entries=args.entry,
                previous_catalog=args.previous_catalog,
                sequence=args.sequence,
                generated=args.generated,
                expires=args.expires,
                expires_days=args.expires_days,
                private_key_source=args.private_key,
                signature_output_path=args.signature_output,
                base_dir=base_dir,
            )
            print(json.dumps(res, ensure_ascii=False, indent=2))
            return 0

        if args.command == "verify-catalog":
            res = verify_catalog(
                args.catalog,
                args.signature,
                trusted_key=args.trusted_key,
                trusted_pin=args.trusted_pin,
                min_sequence=args.min_sequence,
                now=args.now,
                artifacts_dir=args.artifacts_dir,
            )
            print(json.dumps(res, ensure_ascii=False, indent=2))
            return 0
    except ReleasePipelineError as exc:
        print(json.dumps({"ok": False, "error": {"code": exc.code, "message": exc.detail}}, ensure_ascii=False), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8")
    raise SystemExit(main())
