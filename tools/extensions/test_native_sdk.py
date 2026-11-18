"""Validate a native source SDK, compile author DLLs and run wrapper lifecycle tests."""
import argparse
import hashlib
import json
import re
from pathlib import Path, PurePosixPath
import subprocess
import zipfile


def extract(archive, destination):
    destination = Path(destination)
    if destination.exists():
        raise ValueError("Extraction destination must not exist")
    with zipfile.ZipFile(archive) as source:
        infos = source.infolist()
        names = [entry.filename for entry in infos]
        if len(names) != len(set(names)) or sum(entry.file_size for entry in infos) > 4 * 1024 * 1024:
            raise ValueError("Duplicate names or excessive SDK size")
        for entry in infos:
            name = PurePosixPath(entry.filename)
            if name.is_absolute() or ".." in name.parts or "\\" in entry.filename or ":" in entry.filename or entry.is_dir() or (entry.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError("Unsafe archive entry")
        manifest = json.loads(source.read("native-sdk-manifest.json"))
        if set(names) != set(manifest["files"]) | {"native-sdk-manifest.json"}:
            raise ValueError("Unexpected SDK contents")
        for name, digest in manifest["files"].items():
            if hashlib.sha256(source.read(name)).hexdigest() != digest:
                raise ValueError("SDK content hash mismatch")
        destination.mkdir(parents=True)
        source.extractall(destination)
    return destination


def validate(root):
    root = Path(root).resolve()
    manifest = json.loads((root / "native-sdk-manifest.json").read_text(encoding="utf-8"))
    if manifest["delivery"] != "source-only" or manifest["hostIncluded"] is not False:
        raise ValueError("Expected source SDK without host")
    allowed = ("sdk/native/", "examples/native/", "tools/extensions/")
    authoring_files = {"docs/extensions-sdk-quickstart.md", "docs/native-packages.md", "sdk/native/README.md", "sdk/native/README.md", "sdk/native/README.md", "sdk/native/README.md", "sdk/native/README.md", "sdk/native/README.md", "sdk/native/README.md", "schemas/extensions/manifest.schema.json", "examples/native/catalog.json"}
    for name, digest in manifest["files"].items():
        if (not name.startswith(allowed) and name not in authoring_files) or PurePosixPath(name).suffix.lower() in (".dll", ".exe", ".lib"):
            raise ValueError("Private or binary content in source SDK")
        path = (root / name).resolve()
        try:
            path.relative_to(root)
        except ValueError as error:
            raise ValueError("SDK path escapes its root") from error
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError("SDK content mismatch")
    catalog = json.loads((root / "sdk/native/api-catalog.json").read_text(encoding="utf-8"))
    if any(item["name"] == "pulse.ui" for item in catalog["interfaces"]):
        raise ValueError("Unexpected native interface contract")
    abi = (root / "sdk/native/include/pulse/abi.h").read_text(encoding="utf-8")
    preview = int(re.search(r"#define PULSE_SDK_PREVIEW (\d+)u?", abi).group(1))
    if catalog.get("sdkPreview") != preview:
        raise ValueError("Native SDK preview discovery/header mismatch")
    model = next(item for item in catalog["interfaces"] if item["name"] == "pulse.ui.model")
    header = (root / "sdk/native/include/pulse/ui_model.h").read_bytes()
    if hashlib.sha256(header).hexdigest() != model["headerContract"]["sha256"]:
        raise ValueError("Native model discovery/header mismatch")
    data = next(item for item in catalog["interfaces"] if item["name"] == "pulse.ui.data")
    data_header = (root / "sdk/native/include/pulse/ui_data.h").read_bytes()
    if (hashlib.sha256(data_header).hexdigest() != data["headerContract"]["sha256"] or
            data_header.decode("utf-8") != data["headerContract"]["text"]):
        raise ValueError("Native data discovery/header mismatch")
    if not {"sdk/native/include/pulse/ui_data.h", "sdk/native/README.md"}.issubset(manifest["files"]):
        raise ValueError("Missing native data authoring files")
    for name, filename in (("pulse.directories", "directories.h"), ("pulse.context", "context.h"),
                           ("pulse.fileops", "fileops.h"), ("pulse.ui.mount", "ui_mount.h"), ("pulse.commands", "commands.h"), ("pulse.preview", "preview.h"), ("pulse.tasks", "tasks.h"), ("pulse.staging", "staging.h"), ("pulse.settings.pages", "settings_pages.h")):
        interface = next(item for item in catalog["interfaces"] if item["name"] == name)
        header_path = "sdk/native/include/pulse/" + filename
        if header_path not in manifest["files"]:
            raise ValueError("Missing native directory/context authoring header")
        header = (root / header_path).read_bytes()
        if (hashlib.sha256(header).hexdigest() != interface["headerContract"]["sha256"] or
                header.decode("utf-8") != interface["headerContract"]["text"]):
            raise ValueError("Native interface discovery/header mismatch")
    if "sdk/native/README.md" not in manifest["files"]:
        raise ValueError("Missing native directory authoring guide")
    if "sdk/native/README.md" not in manifest["files"]:
        raise ValueError("Missing native file operation authoring guide")
    if not {"examples/native/directory-transfer/" + name for name in
            ("CMakeLists.txt", "plugin.cpp", "manifest.json", "README.md")}.issubset(manifest["files"]):
        raise ValueError("Missing native directory transfer authoring files")
    permissions = json.loads((root / "schemas/extensions/manifest.schema.json").read_text(encoding="utf-8"))["properties"]["resources"]
    if catalog["resourceDeclarations"] != permissions:
        raise ValueError("Native resource declarations/schema mismatch")
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--sdk-root", type=Path)
    source.add_argument("--archive", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = validate(extract(args.archive, args.work / "sdk") if args.archive else args.sdk_root)
    report = {"validated": True, "hostIncluded": False, "dllsCompiled": False, "hostUiTested": False}
    if not args.validate_only:
        with (args.work / "compile.log").open("w", encoding="utf-8") as log:
            abi = args.work / "abi-check"
            abi.mkdir()
            (abi / "main.cpp").write_text('''#include <cstddef>
#pragma pack(push, 1)
#include <pulse/abi.h>
#include <pulse/ui.h>
#include <pulse/files.h>
#include <pulse/directories.h>
#include <pulse/context.h>
#include <pulse/commands.h>
#include <pulse/preview.h>
#include <pulse/tasks.h>
#include <pulse/staging.h>
#include <pulse/settings_pages.h>
#include <pulse/fileops.h>
#include <pulse/ui_model.h>
#include <pulse/ui_data.h>
#include <pulse/resources.h>
#include <pulse/ui_events.h>
#include <pulse/ui_mount.h>
#include <pulse/dispatch.h>
struct RestoredPacking { char c; uint64_t value; };
#pragma pack(pop)
static_assert(offsetof(RestoredPacking, value) == 1);
static_assert(alignof(PulseHostV1) == 8);
static_assert(alignof(PulsePreviewV1) == 8);
static_assert(alignof(PulseStagingV1) == 8);
static_assert(alignof(PulseSettingsPagesV1) == 8);
static_assert(sizeof(PulseSettingsPagesV1) == 40);
static_assert(offsetof(PulseSettingsPagesV1, create) == 16);
static_assert(offsetof(PulseSettingsPagesV1, query) == 24);
static_assert(offsetof(PulseSettingsPagesV1, remove) == 32);
static_assert(alignof(PulseStageSnapshot) == 8);
static_assert(offsetof(PulseStageSnapshot, task_id) == 16);
static_assert(offsetof(PulseStagingV1, choose_destination) == 56);
static_assert(alignof(PulseUiModelV1) == 8);
static_assert(alignof(PulseFilesV1) == 8);
static_assert(alignof(PulseDirectoriesV1) == 8);
static_assert(alignof(PulseDirectoryRow) == 8);
static_assert(alignof(PulseDirectoryReply) == 8);
static_assert(alignof(PulseDirectoryChange) == 8);
static_assert(alignof(PulseContextV1) == 8);
static_assert(alignof(PulseFileOpsV1) == 8);
static_assert(alignof(PulseFileOpsV2) == 8);
static_assert(alignof(PulseFileOpsV3) == 8);
static_assert(alignof(PulseFileOpsV4) == 8);
static_assert(offsetof(PulseFileOpsV2, submit) == offsetof(PulseFileOpsV1, submit));
static_assert(offsetof(PulseFileOpsV2, get) == offsetof(PulseFileOpsV1, get));
static_assert(offsetof(PulseFileOpsV2, cancel) == offsetof(PulseFileOpsV1, cancel));
static_assert(offsetof(PulseFileOpsV2, submit_to_directory) == sizeof(PulseFileOpsV1));
static_assert(offsetof(PulseFileOpsV3, submit) == offsetof(PulseFileOpsV1, submit));
static_assert(offsetof(PulseFileOpsV3, get) == offsetof(PulseFileOpsV1, get));
static_assert(offsetof(PulseFileOpsV3, cancel) == offsetof(PulseFileOpsV1, cancel));
static_assert(offsetof(PulseFileOpsV3, submit_to_directory) == offsetof(PulseFileOpsV2, submit_to_directory));
static_assert(offsetof(PulseFileOpsV3, submit_entries) == sizeof(PulseFileOpsV2));
static_assert(offsetof(PulseFileOpsV4, submit_directory_tree) == sizeof(PulseFileOpsV3));
static_assert(offsetof(PulseFileOpsV4, get_item) == sizeof(PulseFileOpsV3) + sizeof(void*));
static_assert(alignof(PulseContextSnapshot) == 8);
static_assert(alignof(PulseUiModelV1) == 8);
static_assert(alignof(PulseUiDataV1) == 8);
static_assert(alignof(PulseUiDataRequest) == 8);
static_assert(alignof(PulseUiDataBinding) == 8);
static_assert(alignof(PulseUiDataPage) == 8);
static_assert(alignof(PulseResourcesV1) == 8);
static_assert(alignof(PulseUiEventsV1) == 8);
static_assert(alignof(PulseUiMountV1) == 8);
static_assert(sizeof(PulseUiMountInfo) == 40);
static_assert(offsetof(PulseUiMountV1, set) == 16);
static_assert(alignof(PulseDispatchV1) == 8);
static_assert(offsetof(PulseUiModelString, value) == 8);
int main() { return 0; }
''', encoding="utf-8")
            (abi / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.25)\nproject(NativeAbiCheck LANGUAGES CXX)\nadd_executable(abi_check main.cpp)\ntarget_include_directories(abi_check PRIVATE "${PULSE_SDK_INCLUDE}")\n', encoding="utf-8")
            for command in (["cmake", "-S", str(abi.resolve()), "-B", str((abi / "build").resolve()), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPULSE_SDK_INCLUDE=" + str(root / "sdk/native/include")], ["cmake", "--build", str((abi / "build").resolve())]):
                subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
            for example in ("basic", "rename-panel", "inspector", "reactive-panel", "live-data", "directory-browser", "file-operations", "directory-transfer", "directory-workbench"):
                build = (args.work / ("build-" + example)).resolve()
                for command in (["cmake", "-S", str(root / "examples/native" / example), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPULSE_SDK_INCLUDE=" + str(root / "sdk/native/include")], ["cmake", "--build", str(build)]):
                    subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
            for name in ("pulse_rename_lower.dll", "pulse_rename_upper.dll"):
                if not (args.work / "build-rename-panel" / name).is_file():
                    raise ValueError("Missing author DLL " + name)
            for directory, name in (("basic", "pulse_native_example.dll"), ("inspector", "pulse_inspector.dll"), ("reactive-panel", "pulse_reactive.dll"), ("live-data", "pulse_live_data.dll"), ("directory-browser", "pulse_directory_browser.dll"), ("file-operations", "pulse_file_operations.dll"), ("directory-transfer", "pulse_directory_transfer.dll"), ("directory-workbench", "pulse_directory_workbench.dll")):
                if not (args.work / ("build-" + directory) / name).is_file():
                    raise ValueError("Missing author DLL " + name)
            with (args.work / "wrapper-test.log").open("w", encoding="utf-8") as test_log:
                subprocess.run([str((args.work / "build-reactive-panel/pulse_client_wrapper_test.exe").resolve())], check=True, stdout=test_log, stderr=subprocess.STDOUT)
            with (args.work / "data-provider-test.log").open("w", encoding="utf-8") as test_log:
                subprocess.run([str((args.work / "build-live-data/pulse_live_data_test.exe").resolve())], check=True, stdout=test_log, stderr=subprocess.STDOUT)
            with (args.work / "directory-provider-test.log").open("w", encoding="utf-8") as test_log:
                subprocess.run([str((args.work / "build-directory-browser/pulse_directory_browser_test.exe").resolve())], check=True, stdout=test_log, stderr=subprocess.STDOUT)
        report["dllsCompiled"] = True
        report["abiPackingCompiled"] = True
        report["dllCount"] = 10
        report["directoryProviderTestsPassed"] = True
        report["dataProviderTestsPassed"] = True
        report["wrapperTestsPassed"] = True
    (args.work / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
