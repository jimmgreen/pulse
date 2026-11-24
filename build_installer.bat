@echo off
rem Build Release binaries, then pack dist\PulseSetup-<version>.exe
rem (Pulse setup; see scripts\pack_setup.bat).
rem The installer always carries the x86 dialog companion: an installation that has it refuses an
rem x64-only update. The companion is built in build-x86 (override with PULSE_DIALOG_BUILD_X86);
rem /x64only packs without it, for machines that never had the companion.
setlocal
cd /d "%~dp0"

set "PULSE_SKIP_BUILD="
set "PULSE_X64_ONLY="
for %%A in (%*) do (
    if /i "%%~A"=="/skipbuild" set "PULSE_SKIP_BUILD=1"
    if /i "%%~A"=="/x64only" set "PULSE_X64_ONLY=1"
)
if not defined PULSE_DIALOG_BUILD_X86 set "PULSE_DIALOG_BUILD_X86=build-x86"

if defined PULSE_SKIP_BUILD goto after_release_build
call "%~dp0build_release.bat" || exit /b 1
if defined PULSE_X64_ONLY goto after_release_build
call "%~dp0scripts\build_dialog_x86.bat" "%PULSE_DIALOG_BUILD_X86%" || exit /b 1
:after_release_build

if defined PULSE_X64_ONLY (
    call "%~dp0scripts\pack_setup.bat" build windows
) else (
    call "%~dp0scripts\pack_setup.bat" build windows "%PULSE_DIALOG_BUILD_X86%"
)
exit /b %errorlevel%
