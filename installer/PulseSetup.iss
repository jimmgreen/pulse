; Pulse installer — single-file exe setup via Inno Setup 6.
; Builds dist\PulseSetup-<version>.exe from the Release build in build\.
;
; Install-time behaviour:
;   - Installs the 4 executables into Program Files\Pulse (admin required).
;   - "Index service" task (default on): runs `Pulse.Index.exe --install` and
;     starts the PulseIndex service, so full-disk MFT/USN indexing works out of
;     the box without a runtime UAC prompt.
;   - Uninstall stops and removes the service before deleting files.
; User data under %LOCALAPPDATA%\Pulse (settings, session, index) is kept on
; uninstall.

#define AppVersion "1.0.0"

[Setup]
AppId={{A3F47C2E-9D1B-4E58-8C6A-2B5D0F9E1734}
AppName=Pulse
AppVersion={#AppVersion}
AppVerName=Pulse {#AppVersion}
DefaultDirName={autopf}\Pulse
DefaultGroupName=Pulse
PrivilegesRequired=admin
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Sources are referenced relative to the project root (this script's parent).
SourceDir=..
OutputDir=dist
OutputBaseFilename=PulseSetup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Ask Windows to close running Pulse processes (and their helper hosts) so
; upgrades can replace locked files.
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\pulse.exe

[Languages]
; ChineseSimplified.isl is bundled in installer/Languages (community translation,
; https://github.com/kira-96/Inno-Setup-Chinese-Simplified-Translation).
Name: "chinesesimp"; MessagesFile: "installer\Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "indexservice"; Description: "启用全盘文件索引（安装 PulseIndex 后台服务，推荐） / Enable full-disk file index (installs the PulseIndex background service, recommended)"; GroupDescription: "搜索与索引 / Search && indexing:"
Name: "startup"; Description: "开机自动启动 Pulse / Launch Pulse at sign-in"; GroupDescription: "其他 / Other:"; Flags: unchecked
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "其他 / Other:"; Flags: unchecked

[Files]
Source: "build\pulse.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Pulse.Index.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Pulse.Preview.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\pulse_shell.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Pulse"; Filename: "{app}\pulse.exe"; WorkingDir: "{app}"
Name: "{group}\卸载 Pulse / Uninstall Pulse"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Pulse"; Filename: "{app}\pulse.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Same key the in-app preference manages (src/app/app_prefs.cpp).
; Note: with an elevated install this lands in the installing user's hive.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Pulse"; ValueData: """{app}\pulse.exe"""; Tasks: startup; Flags: uninsdeletevalue

[Run]
; The installer already runs elevated, so --install registers the service with
; no further UAC prompt. net-start failures are non-fatal (AUTO_START picks it
; up on the next boot).
Filename: "{app}\Pulse.Index.exe"; Parameters: "--install"; StatusMsg: "正在安装全盘索引服务… / Installing the index service…"; Flags: runhidden waituntilterminated; Tasks: indexservice
Filename: "{cmd}"; Parameters: "/c net start PulseIndex >nul 2>&1 & exit /b 0"; Flags: runhidden waituntilterminated; Tasks: indexservice
Filename: "{app}\pulse.exe"; Description: "{cm:LaunchProgram,Pulse}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Runs before files are deleted.
Filename: "{cmd}"; Parameters: "/c net stop PulseIndex >nul 2>&1 & exit /b 0"; Flags: runhidden waituntilterminated; RunOnceId: "StopPulseIndex"
Filename: "{app}\Pulse.Index.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemovePulseIndex"

[Code]
// Stop the index service before replacing files on upgrade; Pulse.Index.exe
// is locked while the service runs. Failure is fine (service not installed).
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Exec('net.exe', 'stop PulseIndex', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := '';
end;
