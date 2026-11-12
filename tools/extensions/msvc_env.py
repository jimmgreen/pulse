"""Auto-discovery of MSVC (vcvars64.bat), CMake, Ninja, and Windows SDK (stdlib only)."""
from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess

_ENV_CACHE: dict[tuple[str, str], dict[str, str]] = {}
_UPPER_ENV_KEYS = frozenset({
    "PATH", "PATHEXT", "INCLUDE", "LIB", "LIBPATH", "SYSTEMROOT", "COMSPEC", "TEMP", "TMP",
})


def _env_get(env: dict[str, str], name: str, default: str = "") -> str:
    if name in env:
        return env[name]
    upper = name.upper()
    for k, v in env.items():
        if k.upper() == upper:
            return v
    return default


def _normalize_env(raw_env: dict[str, str]) -> dict[str, str]:
    normalized: dict[str, str] = {}
    seen_upper: dict[str, str] = {}
    for k, v in raw_env.items():
        if not k or k.startswith("="):
            continue
        upper = k.upper()
        canon = upper if upper in _UPPER_ENV_KEYS else k
        prev_key = seen_upper.get(upper)
        if prev_key is not None and prev_key != canon:
            del normalized[prev_key]
        seen_upper[upper] = canon
        normalized[canon] = v
    return normalized


def _probe_tool(command: list[str], env: dict[str, str] | None = None, timeout: int = 10) -> tuple[int | None, str, str]:
    try:
        completed = subprocess.run(
            command,
            shell=False,
            capture_output=True,
            env=env,
            timeout=timeout,
        )
        stdout = completed.stdout.decode("utf-8", errors="replace")
        stderr = completed.stderr.decode("utf-8", errors="replace")
        return completed.returncode, stdout, stderr
    except (OSError, subprocess.SubprocessError):
        return None, "", ""


def _candidate_vs_roots() -> list[Path]:
    roots: list[Path] = []
    seen: set[str] = set()

    def _add(p: Path) -> None:
        key = os.path.normcase(os.path.abspath(str(p)))
        if key not in seen and p.is_dir():
            seen.add(key)
            roots.append(p)

    if os.name != "nt":
        return roots

    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    pf = os.environ.get("ProgramFiles", r"C:\Program Files")
    for installer_base in (pf86, pf):
        vswhere = Path(installer_base) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if vswhere.is_file():
            code, stdout, _ = _probe_tool([
                str(vswhere),
                "-latest",
                "-products", "*",
                "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-property", "installationPath",
            ])
            if code == 0 and stdout.strip():
                for line in stdout.splitlines():
                    if line.strip():
                        _add(Path(line.strip()))

    for base in (pf, pf86, r"C:\Program Files", r"C:\Program Files (x86)"):
        if not base:
            continue
        vs_parent = Path(base) / "Microsoft Visual Studio"
        for ver in ("18", "2022", "2019"):
            for edition in ("Enterprise", "Professional", "Community", "BuildTools"):
                _add(vs_parent / ver / edition)

    return roots


def _version_tuple(text: str) -> tuple[int, ...]:
    parts = [int(part) for part in re.findall(r"\d+", text)]
    return tuple(parts) if parts else (0,)


def _discover_direct_vs_x64_env(vs_root: Path, base_env: dict[str, str]) -> dict[str, str] | None:
    vc_tools_base = vs_root / "VC" / "Tools" / "MSVC"
    if not vc_tools_base.is_dir():
        return None

    vc_tools_dir: Path | None = None
    default_ver_file = vs_root / "VC" / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt"
    if default_ver_file.is_file():
        ver_str = default_ver_file.read_text(encoding="utf-8", errors="replace").strip()
        if ver_str and (vc_tools_base / ver_str / "bin" / "Hostx64" / "x64" / "cl.exe").is_file():
            vc_tools_dir = vc_tools_base / ver_str
    if vc_tools_dir is None:
        candidates = [
            entry for entry in vc_tools_base.iterdir()
            if entry.is_dir() and (entry / "bin" / "Hostx64" / "x64" / "cl.exe").is_file()
        ]
        if not candidates:
            return None
        candidates.sort(key=lambda p: _version_tuple(p.name))
        vc_tools_dir = candidates[-1]

    cl_bin = vc_tools_dir / "bin" / "Hostx64" / "x64"
    vc_inc = vc_tools_dir / "include"
    vc_lib = vc_tools_dir / "lib" / "x64"
    if not vc_inc.is_dir() or not vc_lib.is_dir():
        return None

    pf86 = _env_get(base_env, "ProgramFiles(x86)") or r"C:\Program Files (x86)"
    pf = _env_get(base_env, "ProgramFiles") or r"C:\Program Files"
    kit_roots: list[Path] = []
    env_sdk = _env_get(base_env, "WindowsSdkDir").strip()
    if env_sdk:
        kit_roots.append(Path(env_sdk))
    for base in (pf86, pf, r"C:\Program Files (x86)", r"C:\Program Files"):
        if base:
            kit_roots.append(Path(base) / "Windows Kits" / "10")

    chosen_kit: Path | None = None
    chosen_sdk_ver: str | None = None
    for kit in kit_roots:
        inc_root = kit / "Include"
        lib_root = kit / "Lib"
        if not inc_root.is_dir() or not lib_root.is_dir():
            continue
        sdk_vers = []
        for entry in inc_root.iterdir():
            if not entry.is_dir() or not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", entry.name):
                continue
            ver = entry.name
            if (
                (entry / "um" / "Windows.h").is_file()
                and (entry / "ucrt").is_dir()
                and (lib_root / ver / "um" / "x64" / "kernel32.lib").is_file()
                and (lib_root / ver / "ucrt" / "x64").is_dir()
            ):
                sdk_vers.append(ver)
        if sdk_vers:
            sdk_vers.sort(key=_version_tuple)
            chosen_kit = kit
            chosen_sdk_ver = sdk_vers[-1]
            break

    if chosen_kit is None or chosen_sdk_ver is None:
        return None

    env = _normalize_env(base_env)
    inc_dirs = [str(vc_inc)]
    atlmfc_inc = vc_tools_dir / "ATLMFC" / "include"
    if atlmfc_inc.is_dir():
        inc_dirs.append(str(atlmfc_inc))
    for sub in ("ucrt", "shared", "um", "winrt", "cppwinrt"):
        cand = chosen_kit / "Include" / chosen_sdk_ver / sub
        if cand.is_dir():
            inc_dirs.append(str(cand))

    lib_dirs = []
    atlmfc_lib = vc_tools_dir / "ATLMFC" / "lib" / "x64"
    if atlmfc_lib.is_dir():
        lib_dirs.append(str(atlmfc_lib))
    lib_dirs.extend([
        str(vc_lib),
        str(chosen_kit / "Lib" / chosen_sdk_ver / "ucrt" / "x64"),
        str(chosen_kit / "Lib" / chosen_sdk_ver / "um" / "x64"),
    ])

    prepend_paths = [str(cl_bin)]
    sdk_bin_x64 = chosen_kit / "bin" / chosen_sdk_ver / "x64"
    if sdk_bin_x64.is_dir():
        prepend_paths.append(str(sdk_bin_x64))
    for extra_bin in (
        vs_root / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin",
        vs_root / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja",
        vs_root / "MSBuild" / "Current" / "Bin" / "amd64",
    ):
        if extra_bin.is_dir():
            prepend_paths.append(str(extra_bin))

    existing_inc = _env_get(env, "INCLUDE")
    existing_lib = _env_get(env, "LIB")
    existing_path = _env_get(env, "PATH")
    env["INCLUDE"] = os.pathsep.join([*inc_dirs, existing_inc]) if existing_inc else os.pathsep.join(inc_dirs)
    env["LIB"] = os.pathsep.join([*lib_dirs, existing_lib]) if existing_lib else os.pathsep.join(lib_dirs)
    env["PATH"] = os.pathsep.join([*prepend_paths, existing_path]) if existing_path else os.pathsep.join(prepend_paths)
    env["VSINSTALLDIR"] = str(vs_root) + "\\"
    env["VCINSTALLDIR"] = str(vs_root / "VC") + "\\"
    env["VCToolsInstallDir"] = str(vc_tools_dir) + "\\"
    env["WindowsSdkDir"] = str(chosen_kit) + "\\"
    env["WindowsSDKVersion"] = chosen_sdk_ver + "\\"
    env["VSCMD_ARG_TGT_ARCH"] = "x64"
    env["VSCMD_ARG_HOST_ARCH"] = "x64"
    env["VSLANG"] = "1033"
    return env


def _capture_vcvars64(vcvars_bat: Path, base_env: dict[str, str]) -> dict[str, str] | None:
    cache_key = (os.path.normcase(str(vcvars_bat)), _env_get(base_env, "PATH"))
    cached = _ENV_CACHE.get(cache_key)
    if cached is not None:
        return dict(cached)

    comspec = _env_get(base_env, "ComSpec") or os.environ.get("ComSpec") or r"C:\Windows\System32\cmd.exe"
    cmdline = f'"{comspec}" /d /s /c "set VSLANG=1033&& set VSCMD_SKIP_SENDTELEMETRY=1&& call "{vcvars_bat}" >nul && set"'
    try:
        completed = subprocess.run(
            cmdline,
            shell=False,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            env=_normalize_env(base_env),
            timeout=90,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None

    captured = _normalize_env(base_env)
    text = completed.stdout.decode("utf-8", errors="replace")
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if not key or key.startswith("="):
            continue
        upper = key.upper()
        canon = upper if upper in _UPPER_ENV_KEYS else key
        for existing in list(captured.keys()):
            if existing.upper() == upper and existing != canon:
                del captured[existing]
        captured[canon] = value

    _ENV_CACHE[cache_key] = dict(captured)
    return captured


def _is_x64_cl_ready(env: dict[str, str]) -> bool:
    cl_path = shutil.which("cl", path=_env_get(env, "PATH") or None)
    if not cl_path:
        return False
    code, stdout, stderr = _probe_tool([cl_path], env=env)
    banner = ((stderr or "") + "\n" + (stdout or "")).strip()
    arch_match = re.search(r"\bfor\s+(x64|x86|ARM64|ARM)\b", banner, re.IGNORECASE)
    if code is None or not arch_match or arch_match.group(1).lower() != "x64":
        return False
    inc, libs, _, _ = _check_windows_sdk_env(env)
    return inc and libs


def _check_windows_sdk_env(env: dict[str, str]) -> tuple[bool, bool, str | None, str]:
    include_entries = [entry.strip() for entry in _env_get(env, "INCLUDE").split(os.pathsep) if entry.strip()]
    lib_entries = [entry.strip() for entry in _env_get(env, "LIB").split(os.pathsep) if entry.strip()]
    sdk_dir = _env_get(env, "WindowsSdkDir").strip()
    sdk_ver_raw = _env_get(env, "WindowsSDKVersion").strip().strip("\\/")
    sdk_version = sdk_ver_raw if re.fullmatch(r"[0-9.]+", sdk_ver_raw) else None

    headers_found = any(
        (Path(item) / "Windows.h").is_file() or (Path(item) / "windows.h").is_file()
        for item in include_entries
    )
    if not headers_found and sdk_dir and sdk_version:
        headers_found = (Path(sdk_dir) / "Include" / sdk_version / "um" / "Windows.h").is_file()

    x64_libs_found = False
    for item in lib_entries:
        candidate = Path(item) / "kernel32.lib"
        if candidate.is_file():
            parts_lower = {part.lower() for part in Path(item).parts}
            if "x86" not in parts_lower and "arm" not in parts_lower and "arm64" not in parts_lower:
                x64_libs_found = True
                break
    if not x64_libs_found and sdk_dir and sdk_version:
        x64_libs_found = (Path(sdk_dir) / "Lib" / sdk_version / "um" / "x64" / "kernel32.lib").is_file()

    reason = ""
    if not (headers_found and x64_libs_found):
        if not headers_found and not x64_libs_found:
            reason = "Windows SDK headers (Windows.h) and x64 libraries (kernel32.lib) were not found in INCLUDE/LIB"
        elif not headers_found:
            reason = "Windows SDK headers (Windows.h) were not found in INCLUDE"
        else:
            reason = "Windows SDK x64 libraries (kernel32.lib) were not found in LIB"
    return headers_found, x64_libs_found, sdk_version, reason


def _prepend_vs_bundled_tools(env: dict[str, str], vs_roots: list[Path]) -> bool:
    added = False
    current_path = _env_get(env, "PATH")
    extra_dirs: list[str] = []

    if shutil.which("cmake", path=current_path or None) is None:
        for root in vs_roots:
            cmake_bin = root / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin"
            if (cmake_bin / "cmake.exe").is_file():
                extra_dirs.append(str(cmake_bin))
                added = True
                break

    combined_check = os.pathsep.join([*extra_dirs, current_path]) if current_path else os.pathsep.join(extra_dirs)
    if shutil.which("ninja", path=combined_check or None) is None:
        for root in vs_roots:
            ninja_bin = root / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja"
            if (ninja_bin / "ninja.exe").is_file():
                extra_dirs.append(str(ninja_bin))
                added = True
                break

    if extra_dirs:
        for existing in list(env.keys()):
            if existing.upper() == "PATH" and existing != "PATH":
                del env[existing]
        env["PATH"] = os.pathsep.join([*extra_dirs, current_path]) if current_path else os.pathsep.join(extra_dirs)
    return added


def resolve_build_environment(
    base_env: dict[str, str] | None = None,
    auto_discover: bool = True,
) -> dict:
    env = _normalize_env(dict(os.environ if base_env is None else base_env))
    auto_discovered = False
    vcvars_used = False

    if auto_discover and os.name == "nt":
        vs_roots: list[Path] | None = None
        if not _is_x64_cl_ready(env):
            vs_roots = _candidate_vs_roots()
            for root in vs_roots:
                direct_env = _discover_direct_vs_x64_env(root, env)
                if direct_env is not None:
                    env = direct_env
                    auto_discovered = True
                    vcvars_used = True
                    break
            if not vcvars_used:
                for root in vs_roots:
                    vcvars = root / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
                    if vcvars.is_file():
                        captured = _capture_vcvars64(vcvars, env)
                        if captured is not None:
                            env = captured
                            auto_discovered = True
                            vcvars_used = True
                            break
        cur_path = _env_get(env, "PATH") or None
        if shutil.which("cmake", path=cur_path) is None or shutil.which("ninja", path=cur_path) is None:
            if vs_roots is None:
                vs_roots = _candidate_vs_roots()
            if _prepend_vs_bundled_tools(env, vs_roots):
                auto_discovered = True

    path_var = _env_get(env, "PATH") or None
    return {
        "env": env,
        "auto_discovered": auto_discovered,
        "vcvars_used": vcvars_used,
        "cmake_executable": shutil.which("cmake", path=path_var),
        "ninja_executable": shutil.which("ninja", path=path_var),
        "cl_executable": shutil.which("cl", path=path_var),
    }


def inspect_toolchain(auto_discover: bool = True, base_env: dict[str, str] | None = None) -> dict:
    resolved = resolve_build_environment(base_env=base_env, auto_discover=auto_discover)
    env = resolved["env"]
    missing_reasons: list[str] = []

    # 1. CMake >= 3.25
    cmake_info: dict = {"available": False, "compatible": False, "version": None}
    cmake_exe = resolved["cmake_executable"]
    if cmake_exe is None:
        cmake_info["reason"] = "cmake was not found on PATH or Visual Studio CMake bundle"
        missing_reasons.append(cmake_info["reason"])
    else:
        code, stdout, _ = _probe_tool([cmake_exe, "--version"], env=env)
        match = re.search(r"cmake version\s+(\d+)\.(\d+)(?:\.(\d+))?", stdout or "", re.IGNORECASE)
        if code == 0 and match:
            major, minor = int(match.group(1)), int(match.group(2))
            patch_ver = int(match.group(3) or 0)
            cmake_info["available"] = True
            cmake_info["version"] = f"{major}.{minor}.{patch_ver}"
            cmake_info["compatible"] = (major, minor) >= (3, 25)
            if not cmake_info["compatible"]:
                cmake_info["reason"] = f"cmake {cmake_info['version']} is older than required 3.25"
                missing_reasons.append(cmake_info["reason"])
        else:
            cmake_info["reason"] = "cmake --version failed to report a version"
            missing_reasons.append(cmake_info["reason"])

    # 2. Ninja
    ninja_info: dict = {"available": False, "compatible": False, "version": None}
    ninja_exe = resolved["ninja_executable"]
    if ninja_exe is None:
        ninja_info["reason"] = "ninja was not found on PATH or Visual Studio Ninja bundle"
        missing_reasons.append(ninja_info["reason"])
    else:
        code, stdout, _ = _probe_tool([ninja_exe, "--version"], env=env)
        version = (stdout or "").strip().splitlines()[0].strip() if stdout else ""
        if code == 0 and re.fullmatch(r"[0-9][0-9A-Za-z._+-]{0,31}", version):
            ninja_info["available"] = True
            ninja_info["compatible"] = True
            ninja_info["version"] = version
        else:
            ninja_info["reason"] = "ninja --version failed"
            missing_reasons.append(ninja_info["reason"])

    # 3. MSVC cl.exe targeting x64
    msvc_info: dict = {"available": False, "compatible": False, "version": None, "architecture": None}
    cl_exe = resolved["cl_executable"]
    if cl_exe is None:
        msvc_info["reason"] = "MSVC cl.exe was not found via PATH or vswhere/vcvars64.bat"
        missing_reasons.append(msvc_info["reason"])
    else:
        code, stdout, stderr = _probe_tool([cl_exe], env=env)
        banner = ((stderr or "") + "\n" + (stdout or "")).strip()
        ver_match = re.search(r"(\d+\.\d+\.\d+(?:\.\d+)?)", banner)
        arch_match = re.search(r"\bfor\s+(x64|x86|ARM64|ARM)\b", banner, re.IGNORECASE)
        if ver_match:
            msvc_info["version"] = ver_match.group(1)
        if arch_match:
            msvc_info["architecture"] = arch_match.group(1).lower()
        if code is not None and (ver_match or "Microsoft" in banner):
            msvc_info["available"] = True
            if msvc_info["architecture"] == "x64":
                msvc_info["compatible"] = True
            else:
                detected = msvc_info["architecture"] or "unknown"
                msvc_info["reason"] = f"MSVC cl.exe targets {detected} instead of x64; initialize vcvars64.bat"
                missing_reasons.append(msvc_info["reason"])
        else:
            msvc_info["reason"] = "MSVC cl.exe failed to run"
            missing_reasons.append(msvc_info["reason"])

    # 4. Windows SDK headers & x64 import libraries (no local paths exposed)
    headers_found, x64_libs_found, sdk_version, sdk_reason = _check_windows_sdk_env(env)
    winsdk_info: dict = {
        "available": headers_found and x64_libs_found,
        "compatible": headers_found and x64_libs_found,
        "headersFound": headers_found,
        "x64LibrariesFound": x64_libs_found,
        "version": sdk_version,
    }
    if not winsdk_info["compatible"]:
        winsdk_info["reason"] = sdk_reason
        missing_reasons.append(sdk_reason)

    ready = all(section["compatible"] for section in (cmake_info, ninja_info, msvc_info, winsdk_info))
    return {
        "ready": ready,
        "autoDiscovered": bool(resolved["auto_discovered"]),
        "targetArchitecture": "windows-x64",
        "cmake": cmake_info,
        "ninja": ninja_info,
        "msvc": msvc_info,
        "windowsSdk": winsdk_info,
        "missingReasons": missing_reasons,
    }
