"""Unified full-project generator for packable C++ native Pulse extensions."""
import json
import os
from pathlib import Path
import re
import package_store
import template_catalog

RESERVED_DEVICES = (
    {"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"}
    | {prefix + str(index) for prefix in ("COM", "LPT") for index in range(1, 10)}
)

TEMPLATE_CMAKELISTS_STEMS = {
    "archive-dialog": "pulse_archive_dialog",
    "directory-browser": "pulse_directory_browser",
    "directory-transfer": "pulse_directory_transfer",
    "directory-workbench": "pulse_directory_workbench",
    "file-operations": "pulse_file_operations",
    "inspector": "pulse_inspector",
    "live-data": "pulse_live_data",
    "reactive-panel": "pulse_reactive",
    "rename-panel": "pulse_rename",
}


class ProjectCreationError(Exception):
    def __init__(self, code, message, **details):
        super().__init__(message)
        self.code = code
        self.details = details


def fail(code, message, **details):
    raise ProjectCreationError(code, message, **details)


def templates(sdk_root):
    sdk_root = Path(sdk_root).resolve()
    examples_dir = sdk_root / "examples/native"
    if not (examples_dir / "catalog.json").is_file():
        fail("invalid_sdk", "SDK root is missing examples/native/catalog.json", sdkRoot=str(sdk_root))
    return template_catalog.names(examples_dir)


def abi_samples(sdk_root):
    sdk_root = Path(sdk_root).resolve()
    examples_dir = sdk_root / "examples/native"
    if not examples_dir.is_dir():
        return ()
    packable = set(templates(sdk_root))
    return tuple(sorted(
        item.name for item in examples_dir.iterdir()
        if item.is_dir() and (item / "CMakeLists.txt").is_file() and item.name not in packable
    ))


def reserved_ids(examples_dir):
    ids = {"pulse.native-sdk"}
    try:
        for manifest in template_catalog.all_templates(examples_dir).values():
            if isinstance(manifest.get("id"), str):
                ids.add(manifest["id"])
    except (OSError, ValueError):
        pass
    return ids


def validate_package_id(package_id, examples_dir=None, allow_reserved_id=False):
    if (not isinstance(package_id, str) or not 1 <= len(package_id) <= 64 or
            re.fullmatch(r"[A-Za-z0-9._-]+", package_id) is None):
        fail("invalid_id", "Package ID must be 1..64 ASCII characters matching [A-Za-z0-9._-]+")
    stem = package_id.split(".", 1)[0].upper()
    if stem in RESERVED_DEVICES:
        fail("invalid_id", "Package ID must not start with a Windows reserved device name")
    if not allow_reserved_id:
        lowered = package_id.lower()
        blocked = {item.lower() for item in reserved_ids(examples_dir)} if examples_dir else {"pulse.native-sdk"}
        if lowered.startswith("sample.") or lowered in blocked:
            fail("reserved_id", "Package ID uses a reserved SDK sample identifier; supply a unique reverse-DNS or local.* ID")
    return package_id


def validate_display_name(display_name):
    if not package_store.label(display_name):
        fail("invalid_name", "Display name must be 1..128 UTF-8 bytes of visible non-control characters")
    return display_name


def derive_package_id(project_name, prefix="local"):
    if not isinstance(project_name, str) or re.fullmatch(r"[a-z][a-z0-9-]{0,63}", project_name) is None:
        fail("invalid_project_name", "Project name must match [a-z][a-z0-9-]{0,63}")
    max_suffix = 64 - len(prefix) - 1
    suffix = project_name[:max_suffix].rstrip("-")
    if not suffix:
        fail("invalid_project_name", "Project name cannot derive a valid package ID")
    return f"{prefix}.{suffix}"


def derive_display_name(project_name):
    if not isinstance(project_name, str) or not project_name.strip():
        fail("invalid_project_name", "Project name must be non-empty")
    words = [part for part in re.split(r"[-_.]+", project_name.strip()) if part]
    text = " ".join(word.capitalize() for word in words) or project_name.strip()
    encoded = text.encode("utf-8")[:128]
    return encoded.decode("utf-8", errors="ignore").strip() or "Pulse Extension"


def library_stem_for_id(package_id):
    cleaned = re.sub(r"[^a-z0-9]+", "_", package_id.lower()).strip("_")
    if not cleaned or not cleaned[0].isalpha():
        cleaned = "ext_" + cleaned
    if cleaned.upper() in RESERVED_DEVICES:
        cleaned = "ext_" + cleaned
    return cleaned[:56].rstrip("_") or "pulse_ext_plugin"


def cmake_project_identifier(package_id):
    parts = [part for part in re.split(r"[^A-Za-z0-9]+", package_id) if part]
    title = "".join(part[:1].upper() + part[1:] for part in parts)
    if not title or not title[0].isalpha():
        title = "PulseExt" + title
    return title[:64]


def render_cmakelists(source_text, template, package_id, sdk_root):
    stem = library_stem_for_id(package_id)
    cmake_project = cmake_project_identifier(package_id)
    rendered = re.sub(
        r"project\s*\(\s*[A-Za-z0-9_]+\s+LANGUAGES\s+CXX\s*\)",
        f"project({cmake_project} LANGUAGES CXX)",
        source_text,
        count=1,
    )
    sdk_include = (Path(sdk_root).resolve() / "sdk/native/include").as_posix()
    rendered = re.sub(
        r'set\(PULSE_SDK_INCLUDE\s+"[^"]*"\s+CACHE\s+PATH',
        f'set(PULSE_SDK_INCLUDE "{sdk_include}" CACHE PATH',
        rendered,
        count=1,
    )
    original_stem = TEMPLATE_CMAKELISTS_STEMS.get(template)
    if original_stem:
        if template == "rename-panel":
            rendered = rendered.replace("pulse_rename_", stem + "_")
        else:
            rendered = rendered.replace(original_stem, stem)
    if template == "reactive-panel":
        rendered = rendered.replace("pulse_client_wrapper_test", stem + "_wrapper_test")
    targets = [stem + "_lower", stem + "_upper"] if template == "rename-panel" else [stem]
    primary_dll = (targets[0] + ".dll")
    return rendered, stem, targets, primary_dll


def render_readme(package_id, display_name, template, primary_dll, sdk_root):
    sdk_root = Path(sdk_root).resolve()
    cli_path = sdk_root / "tools/extensions/pulse_ext.py"
    include_dir = sdk_root / "sdk/native/include"
    quote = lambda value: "'" + str(value).replace("'", "''") + "'"
    return (
        f"# {display_name} (`{package_id}`)\n\n"
        f"Created from the Pulse Native SDK `{template}` template (`PULSE_SDK_PREVIEW = 3`, `windows-x64`).\n\n"
        "## Build (x64 Visual Studio Developer Prompt)\n\n"
        "```powershell\n"
        f"cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULSE_SDK_INCLUDE={quote(include_dir)}\n"
        "cmake --build build\n"
        "```\n\n"
        "For source-level debugging with PDB symbols, configure `-B build-debug -DCMAKE_BUILD_TYPE=RelWithDebInfo` (or `Debug`).\n\n"
        "## Stage, Pack, Verify, and Install\n\n"
        "Replace `C:/Pulse/pulse.exe` with the target Pulse host matching `sdkPreview = 3`:\n\n"
        "```powershell\n"
        f"python {quote(cli_path)} --pulse 'C:/Pulse/pulse.exe' --json native-manifest manifest.json --library 'build/{primary_dll}' --output 'build/manifest.json'\n"
        f"python {quote(cli_path)} --pulse 'C:/Pulse/pulse.exe' --json pack 'build/manifest.json' --output 'build/{package_id}.pulse-ext'\n"
        f"python {quote(cli_path)} --pulse 'C:/Pulse/pulse.exe' --json verify 'build/{package_id}.pulse-ext'\n"
        f"python {quote(cli_path)} --pulse 'C:/Pulse/pulse.exe' --json install 'build/{package_id}.pulse-ext' --store 'C:/PulseAuthor/extensions' --allow-native --accept-capabilities\n"
        f"python {quote(cli_path)} --pulse 'C:/Pulse/pulse.exe' --json enable {quote(package_id)} --store 'C:/PulseAuthor/extensions' --allow-native\n"
        f"python {quote(cli_path)} --pulse 'C:/Pulse/pulse.exe' --json run --store 'C:/PulseAuthor/extensions' --profile 'C:/PulseAuthor/run-1'\n"
        "```\n\n"
        "## Cooperative Restart & Debug Loop\n\n"
        "Before rebuilding or reinstalling an active DLL, cooperatively close the isolated test host using the returned PID:\n\n"
        "```powershell\n"
        f"python {quote(cli_path)} --pulse 'C:/Pulse/pulse.exe' --json stop-host --pid <PID> --profile 'C:/PulseAuthor/run-1'\n"
        "```\n"
    )


def ensure_no_symlinks(path):
    try:
        package_store.ordinary(path)
    except package_store.StoreError as exc:
        fail("symlink_rejected", str(exc), path=str(path))


def create_project(sdk_root, destination, package_id, display_name, template,
                   allow_reserved_id=False, pre_create_validate=None):
    if destination is None or not str(destination).strip():
        fail("invalid_destination", "Destination directory is required")
    raw_destination = Path(os.path.abspath(destination))
    if os.path.lexists(raw_destination):
        fail("destination_exists", "Destination already exists; choose a new directory.", path=str(raw_destination))
    for ancestor in raw_destination.parents:
        ensure_no_symlinks(ancestor)

    raw_sdk = Path(os.path.abspath(sdk_root))
    for ancestor in (raw_sdk, *raw_sdk.parents):
        ensure_no_symlinks(ancestor)
    sdk_root = raw_sdk.resolve()
    examples_dir = sdk_root / "examples/native"
    try:
        allowed_templates = templates(sdk_root)
    except (OSError, ValueError) as exc:
        fail("invalid_sdk", str(exc), sdkRoot=str(sdk_root))

    if not isinstance(template, str) or template not in allowed_templates:
        if template == "basic":
            fail("template_not_packable",
                 "basic is a minimal ABI sample without manifest.json; choose one of the packable templates: "
                 + ", ".join(allowed_templates))
        fail("unknown_template", "Unknown template; expected one of: " + ", ".join(allowed_templates))

    validate_package_id(package_id, examples_dir=examples_dir, allow_reserved_id=allow_reserved_id)
    validate_display_name(display_name)

    source_dir = examples_dir / template
    ensure_no_symlinks(source_dir)
    if not source_dir.is_dir() or not (source_dir / "CMakeLists.txt").is_file() or not (source_dir / "manifest.json").is_file():
        fail("template_incomplete", f"Template '{template}' is missing CMakeLists.txt or manifest.json")

    source_entries = sorted(source_dir.rglob("*"))
    for entry in source_entries:
        ensure_no_symlinks(entry)
        if entry.is_dir():
            fail("template_unexpected_entry", "Template must contain flat regular files only")

    try:
        manifest = template_catalog.load(template, examples_dir)
    except (OSError, ValueError) as exc:
        fail("template_failed", str(exc))
    manifest["id"] = package_id
    manifest["name"] = display_name
    if "contributes" not in manifest:
        manifest["contributes"] = {"commands": []}
    manifest_text = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    manifest_bytes = manifest_text.encode("utf-8")
    if len(manifest_bytes) > 65536:
        fail("too_large", "Rendered manifest exceeds 64 KiB")

    cmake_source = (source_dir / "CMakeLists.txt").read_text(encoding="utf-8")
    cmake_text, stem, targets, primary_dll = render_cmakelists(cmake_source, template, package_id, sdk_root)
    readme_text = render_readme(package_id, display_name, template, primary_dll, sdk_root)

    file_plan = {}
    for entry in source_entries:
        if entry.name in ("manifest.json", "CMakeLists.txt", "README.md"):
            continue
        file_plan[entry.name] = entry.read_bytes()
    file_plan["manifest.json"] = manifest_bytes
    file_plan["CMakeLists.txt"] = cmake_text.encode("utf-8")
    file_plan["README.md"] = readme_text.encode("utf-8")

    if pre_create_validate is not None:
        pre_create_validate(manifest_bytes)

    created_files = []
    dir_identity = None
    try:
        raw_destination.parent.mkdir(parents=True, exist_ok=True)
        for ancestor in raw_destination.parents:
            ensure_no_symlinks(ancestor)
        raw_destination.mkdir(exist_ok=False)
        stat_info = raw_destination.lstat()
        dir_identity = (stat_info.st_dev, stat_info.st_ino)
        for filename, content_bytes in file_plan.items():
            check_info = raw_destination.lstat()
            if (check_info.st_dev, check_info.st_ino) != dir_identity or raw_destination.is_symlink():
                raise OSError("Destination directory changed during project creation")
            target_file = raw_destination / filename
            with target_file.open("xb") as stream:
                f_info = os.fstat(stream.fileno())
                created_files.append((target_file, (f_info.st_dev, f_info.st_ino)))
                stream.write(content_bytes)
    except OSError as exc:
        for path_created, f_identity in reversed(created_files):
            try:
                info = path_created.lstat()
                if (info.st_dev, info.st_ino) == f_identity:
                    path_created.unlink()
            except OSError:
                pass
        if dir_identity is not None:
            try:
                info = raw_destination.lstat()
                if (info.st_dev, info.st_ino) == dir_identity:
                    raw_destination.rmdir()
            except OSError:
                pass
        code = "destination_exists" if isinstance(exc, FileExistsError) else "write_failed"
        fail(code, str(exc), path=str(raw_destination))

    return {
        "path": str(raw_destination),
        "manifest": str(raw_destination / "manifest.json"),
        "cmakeLists": str(raw_destination / "CMakeLists.txt"),
        "readme": str(raw_destination / "README.md"),
        "packageId": package_id,
        "displayName": display_name,
        "template": template,
        "libraryStem": stem,
        "targets": targets,
        "primaryLibrary": primary_dll,
        "files": sorted(file_plan.keys()),
    }
