"""Bounded immutable snapshots of declarative v1 and multi-file native v2 packages."""
import hashlib
import io
import json
from pathlib import Path
import stat
import struct
from typing import NamedTuple, Optional
import zipfile

import package_store

MAX_MANIFEST_BYTES = 65536
MAX_METADATA_BYTES = 16384
MAX_V1_ARCHIVE_BYTES = 256 * 1024
MAX_ARCHIVE_BYTES = package_store.MAX_PACKAGE_BYTES + MAX_V1_ARCHIVE_BYTES


class PackageSnapshot(NamedTuple):
    metadata: dict
    manifest_bytes: bytes
    binary_bytes: Optional[bytes]
    files: Optional[dict] = None


def metadata_for(data, manifest):
    native = package_store.native_declaration(manifest)
    metadata = {"format": "pulse-extension-local", "formatVersion": 2 if native else 1,
                "apiVersion": manifest["apiVersion"], "id": manifest["id"],
                "manifestSha256": hashlib.sha256(data).hexdigest()}
    if native:
        metadata["native"] = {"library": native["library"], "sha256": native["sha256"]}
    if native and native.get("files"):
        metadata["native"]["files"] = native["files"]
    return metadata


def bounded_zip_directory(snapshot):
    # Bound the entry count before ZipFile allocates objects from the directory.
    end = snapshot.rfind(b"PK\x05\x06", max(0, len(snapshot) - 65557))
    if end < 0 or end + 22 > len(snapshot) or snapshot[:4] != b"PK\x03\x04":
        raise ValueError("Package has no supported ZIP directory")
    _, disk, directory_disk, disk_count, count, size, offset, comment = struct.unpack_from("<4s4H2IH", snapshot, end)
    if (disk or directory_disk or not 2 <= count <= 66 or disk_count != count or
            size > 16384 or offset + size != end or end + 22 + comment != len(snapshot)):
        raise ValueError("Package requires a bounded single-disk, non-ZIP64 directory with 2-66 members")


def read_member(archive, info, limit):
    if info.file_size > limit or info.compress_size > MAX_ARCHIVE_BYTES:
        raise ValueError("Package entry exceeds its size limit")
    with archive.open(info) as stream:
        data = stream.read(limit + 1)
    if len(data) > limit or len(data) != info.file_size:
        raise ValueError("Decompressed package entry exceeds its size limit or declared length")
    return data


def parse(snapshot):
    if len(snapshot) > MAX_ARCHIVE_BYTES:
        raise ValueError("Package archive exceeds the native archive limit")
    bounded_zip_directory(snapshot)
    with zipfile.ZipFile(io.BytesIO(snapshot)) as archive:
        infos = archive.infolist()
        names = [item.filename for item in infos]
        if not 2 <= len(infos) <= 66 or len({name.lower() for name in names}) != len(names):
            raise ValueError("Package has duplicate or excessive members")
        for info in infos:
            kind = stat.S_IFMT(info.external_attr >> 16)
            if (info.is_dir() or kind not in (0, stat.S_IFREG) or info.external_attr & 0x10 or
                    info.flag_bits & (1 | 0x40) or info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) or
                    "/" in info.filename or "\\" in info.filename or ":" in info.filename or
                    info.filename != info.orig_filename or len(info.filename) > 128):
                raise ValueError("Package members must be unencrypted regular files with single filenames")
        if "manifest.json" not in names or "package.json" not in names:
            raise ValueError("Package must contain manifest.json and package.json")
        data = read_member(archive, archive.getinfo("manifest.json"), MAX_MANIFEST_BYTES)
        manifest = package_store.strict_json(data)
        metadata = package_store.strict_json(read_member(archive, archive.getinfo("package.json"), MAX_METADATA_BYTES))
        if (not isinstance(manifest, dict) or type(manifest.get("apiVersion")) is not int or
                not isinstance(manifest.get("id"), str) or not isinstance(metadata, dict) or
                type(metadata.get("formatVersion")) is not int or type(metadata.get("apiVersion")) is not int):
            raise ValueError("Invalid package identity metadata")
        native = package_store.native_declaration(manifest)
        expected_names = {"manifest.json", "package.json"} | (set(package_store.native_files(native)) if native else set())
        if set(names) != expected_names or metadata != metadata_for(data, manifest):
            raise ValueError("Package metadata, manifest digest or exact member set does not match")
        if native is None and len(snapshot) > MAX_V1_ARCHIVE_BYTES:
            raise ValueError("Declarative package archive exceeds the 256 KiB limit")
        files = None
        binary = None
        if native:
            files = {}
            total = 0
            for name in package_store.native_files(native):
                files[name] = read_member(archive, archive.getinfo(name), min(package_store.MAX_NATIVE_BYTES, package_store.MAX_PACKAGE_BYTES - total))
                total += len(files[name])
            package_store.validate_native_files(native, files)
            binary = files[native["library"]]
        return PackageSnapshot(metadata, data, binary, files)



def read(path):
    # A single bounded read is shared by verification and later installation.
    return parse(package_store.read_bytes(Path(path), MAX_ARCHIVE_BYTES))


def write(stream, snapshot):
    manifest = package_store.strict_json(snapshot.manifest_bytes)
    native = package_store.native_declaration(manifest)
    if snapshot.metadata != metadata_for(snapshot.manifest_bytes, manifest):
        raise ValueError("Package metadata does not match its snapshot")
    files = snapshot.files
    if native:
        if files is None:
            files = {native["library"]: snapshot.binary_bytes}
        package_store.validate_native_files(native, files)
    elif snapshot.binary_bytes is not None or files:
        raise ValueError("Metadata-only package must not carry files")
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=False) as archive:
        archive.writestr("manifest.json", snapshot.manifest_bytes)
        archive.writestr("package.json", json.dumps(snapshot.metadata, ensure_ascii=False, indent=2) + "\n")
        for name, data in (files or {}).items():
            archive.writestr(name, data)
