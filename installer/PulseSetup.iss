; Pulse installer — single-file exe setup via Inno Setup 6.
; Builds dist\PulseSetup-<version>.exe from the Release build in build\.
;
; Install-time behaviour:
;   - Installs the 4 executables into Program Files\Pulse (admin required).
;   - "Index service" task (default on): runs `Pulse.Index.exe --install` and
;     starts the PulseIndex service, so full-disk MFT/USN indexing works out of
;     the box without a runtime UAC prompt.
;   - Uninstall stops and removes the service before deleting files.
; Uninstall offers a default-on cleanup option for Pulse settings, caches,
; logs, and local/network index data. A custom index directory is handled
; conservatively: only Pulse-owned index artifacts are removed.

#define AppVersion "1.0.0"

[Setup]
AppId={{A3F47C2E-9D1B-4E58-8C6A-2B5D0F9E1734}
AppName=Pulse
AppVersion={#AppVersion}
AppVerName=Pulse {#AppVersion}
DefaultDirName={autopf}\Pulse
DefaultGroupName=Pulse
; Reuse the existing installation directory and task selections when this is
; an upgrade. AppId is intentionally stable so the previous install can be
; located and removed before the new files are copied.
UsePreviousAppDir=yes
UsePreviousTasks=yes
UsePreviousGroup=yes
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
Name: "{group}\{cm:UninstallProgram,Pulse}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Pulse"; Filename: "{app}\pulse.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Same key the in-app preference manages (src/app/app_prefs.cpp).
; Note: with an elevated install this lands in the installing user's hive.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Pulse"; ValueData: """{app}\pulse.exe"""; Tasks: startup; Flags: uninsdeletevalue

[Run]
; The installer already runs elevated, so --install registers the service with
; no further UAC prompt. net-start failures are non-fatal (AUTO_START picks it
; up on the next boot).
Filename: "{app}\Pulse.Index.exe"; Parameters: "--set-index-path ""{code:GetIndexPath}"""; StatusMsg: "正在配置索引位置… / Configuring the index location…"; Flags: runhidden waituntilterminated; Tasks: indexservice
; Write the machine configuration before starting the service. This avoids
; launching an initial rebuild and then immediately launching a second rebuild
; when the index path command reloads an already-running service.
Filename: "{app}\Pulse.Index.exe"; Parameters: "--install"; StatusMsg: "正在安装全盘索引服务… / Installing the index service…"; Flags: runhidden waituntilterminated; Tasks: indexservice
Filename: "{cmd}"; Parameters: "/c net start PulseIndex >nul 2>&1 & exit /b 0"; Flags: runhidden waituntilterminated; Tasks: indexservice
Filename: "{app}\pulse.exe"; Parameters: "--seed-shell-verbs"; StatusMsg: "正在缓存右键菜单项… / Caching context-menu verbs…"; Flags: runhidden waituntilterminated
Filename: "{app}\pulse.exe"; Description: "{cm:LaunchProgram,Pulse}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Runs before files are deleted.
Filename: "{cmd}"; Parameters: "/c net stop PulseIndex >nul 2>&1 & exit /b 0"; Flags: runhidden waituntilterminated; RunOnceId: "StopPulseIndex"
Filename: "{app}\Pulse.Index.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemovePulseIndex"

[Code]
var
  IndexDirPage: TInputDirWizardPage;
  CleanupUserData: Boolean;
  UninstallIndexPath: String;

function IsChinese: Boolean;
begin
  Result := ActiveLanguage = 'chinesesimp';
end;

function ReadJsonString(const Json, Key: String): String;
var
  I, N: Integer;
  Marker: String;
  Ch: Char;
begin
  Result := '';
  Marker := '"' + Key + '"';
  I := Pos(Marker, Json);
  if I = 0 then
    Exit;
  I := I + Length(Marker);
  N := Length(Json);
  while (I <= N) and (Json[I] <> ':') do
    I := I + 1;
  if I > N then
    Exit;
  I := I + 1;
  while (I <= N) and ((Json[I] = ' ') or (Json[I] = #9) or
    (Json[I] = #10) or (Json[I] = #13)) do
    I := I + 1;
  if (I > N) or (Json[I] <> '"') then
    Exit;
  I := I + 1;
  while I <= N do
  begin
    Ch := Json[I];
    if Ch = '"' then
      Exit;
    if Ch = '\' then
    begin
      I := I + 1;
      if I > N then
      begin
        Result := '';
        Exit;
      end;
      Ch := Json[I];
      if Ch = 'n' then
        Result := Result + #10
      else if Ch = 'r' then
        Result := Result + #13
      else if Ch = 't' then
        Result := Result + #9
      else
        Result := Result + Ch;
    end
    else
      Result := Result + Ch;
    I := I + 1;
  end;
  Result := '';
end;

function ConfiguredIndexPath: String;
var
  Json: AnsiString;
  ConfigPath: String;
begin
  Result := ExpandConstant('{commonappdata}\Pulse\Index');
  ConfigPath := ExpandConstant('{commonappdata}\Pulse\Index\config.json');
  if LoadStringFromFile(ConfigPath, Json) then
  begin
    Result := ReadJsonString(String(Json), 'index_path');
    if Result = '' then
      Result := ExpandConstant('{commonappdata}\Pulse\Index');
  end;
end;

function EndsWith(const Text, Suffix: String): Boolean;
begin
  Result := (Length(Text) >= Length(Suffix)) and
    (CompareText(Copy(Text, Length(Text) - Length(Suffix) + 1,
      Length(Suffix)), Suffix) = 0);
end;

function IsPulseIndexArtifact(const Name: String): Boolean;
var
  Lower: String;
begin
  Lower := Lowercase(Name);
  Result :=
    (Lower = 'pulse-index.bin') or
    (Lower = 'pulse-index.bin.tmp') or
    (Lower = 'pulse-index.dlt') or
    ((Pos('pulse-index-', Lower) = 1) and EndsWith(Lower, '.dlt'));
end;

procedure DeletePulseIndexArtifacts(const Directory: String);
var
  FindRec: TFindRec;
  Path: String;
begin
  if (Directory = '') or not DirExists(Directory) then
    Exit;
  if FindFirst(AddBackslash(Directory) + '*', FindRec) then
  begin
    try
      repeat
        if ((FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) and
          IsPulseIndexArtifact(FindRec.Name) then
        begin
          Path := AddBackslash(Directory) + FindRec.Name;
          if not DeleteFile(Path) then
            Log('Could not delete Pulse index artifact: ' + Path);
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  { This succeeds only when the custom directory is empty. }
  RemoveDir(Directory);
end;

procedure CleanupPulseData;
var
  DefaultIndexPath: String;
begin
  DefaultIndexPath := ExpandConstant('{commonappdata}\Pulse\Index');
  if (UninstallIndexPath <> '') and
    (CompareText(RemoveBackslashUnlessRoot(UninstallIndexPath),
      RemoveBackslashUnlessRoot(DefaultIndexPath)) <> 0) then
    DeletePulseIndexArtifacts(UninstallIndexPath);

  DelTree(ExpandConstant('{localappdata}\Pulse'), True, True, True);
  DelTree(ExpandConstant('{commonappdata}\Pulse'), True, True, True);
end;

function InitializeUninstall: Boolean;
var
  Form: TSetupForm;
  HeadingLabel, DetailLabel: TNewStaticText;
  CleanupCheck: TNewCheckBox;
  ContinueButton, CancelButton: TNewButton;
begin
  { Silent removal is used by the upgrade path, so keep user data there. An
    interactive uninstall still defaults to removing Pulse-owned data. }
  CleanupUserData := not UninstallSilent;
  UninstallIndexPath := ConfiguredIndexPath;

  if UninstallSilent then
  begin
    Result := True;
    Exit;
  end;

  Form := CreateCustomForm(ScaleX(480), ScaleY(160), False, False);
  try
    Form.Caption := 'Pulse';
    Form.CenterOnShow := True;

    HeadingLabel := TNewStaticText.Create(Form);
    HeadingLabel.Parent := Form;
    HeadingLabel.Left := ScaleX(24);
    HeadingLabel.Top := ScaleY(20);
    HeadingLabel.Width := ScaleX(432);
    HeadingLabel.Font.Style := [fsBold];
    if IsChinese then
      HeadingLabel.Caption := '卸载 Pulse'
    else
      HeadingLabel.Caption := 'Uninstall Pulse';

    DetailLabel := TNewStaticText.Create(Form);
    DetailLabel.Parent := Form;
    DetailLabel.Left := ScaleX(24);
    DetailLabel.Top := ScaleY(52);
    DetailLabel.Width := ScaleX(432);
    DetailLabel.Height := ScaleY(34);
    DetailLabel.WordWrap := True;
    if IsChinese then
      DetailLabel.Caption := 'Pulse 将停止并移除索引服务。你也可以删除 Pulse 创建的设置、缓存、日志和索引数据。'
    else
      DetailLabel.Caption := 'Pulse will stop and remove its index service. You can also delete settings, caches, logs, and index data created by Pulse.';

    CleanupCheck := TNewCheckBox.Create(Form);
    CleanupCheck.Parent := Form;
    CleanupCheck.Left := ScaleX(24);
    CleanupCheck.Top := ScaleY(92);
    CleanupCheck.Width := ScaleX(432);
    CleanupCheck.Checked := True;
    if IsChinese then
      CleanupCheck.Caption := '同时删除 Pulse 设置、缓存和索引数据（推荐）'
    else
      CleanupCheck.Caption := 'Also delete Pulse settings, caches, and index data (recommended)';

    ContinueButton := TNewButton.Create(Form);
    ContinueButton.Parent := Form;
    ContinueButton.Width := ScaleX(104);
    ContinueButton.Height := ScaleY(30);
    ContinueButton.Left := Form.ClientWidth - ScaleX(232);
    ContinueButton.Top := Form.ClientHeight - ScaleY(38);
    ContinueButton.Default := True;
    ContinueButton.ModalResult := mrOk;
    if IsChinese then
      ContinueButton.Caption := '继续'
    else
      ContinueButton.Caption := 'Continue';

    CancelButton := TNewButton.Create(Form);
    CancelButton.Parent := Form;
    CancelButton.Width := ScaleX(104);
    CancelButton.Height := ScaleY(30);
    CancelButton.Left := Form.ClientWidth - ScaleX(116);
    CancelButton.Top := Form.ClientHeight - ScaleY(38);
    CancelButton.Cancel := True;
    CancelButton.ModalResult := mrCancel;
    if IsChinese then
      CancelButton.Caption := '取消'
    else
      CancelButton.Caption := 'Cancel';

    Form.ActiveControl := ContinueButton;
    Result := Form.ShowModal = mrOk;
    if Result then
      CleanupUserData := CleanupCheck.Checked;
  finally
    Form.Free;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    { The app can create this value after installation, so remove it even when
      the original installer task was not selected. }
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Pulse');
    if CleanupUserData then
      CleanupPulseData;
  end;
end;

procedure InitializeWizard;
begin
  IndexDirPage := CreateInputDirPage(wpSelectTasks,
    '搜索索引位置 / Search index location',
    '选择 Pulse 索引数据库的存储目录 / Choose where Pulse stores its index database',
    '默认覆盖全部本地 NTFS 固定盘和移动盘。服务器文件夹可稍后在 Pulse 设置中按当前 Windows 用户凭据添加。',
    False, '');
  IndexDirPage.Add('索引目录 / Index directory:');
  IndexDirPage.Values[0] := ExpandConstant('{commonappdata}\Pulse\Index');
end;

function GetIndexPath(Param: String): String;
begin
  Result := IndexDirPage.Values[0];
end;

const
  PulseUninstallKey =
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A3F47C2E-9D1B-4E58-8C6A-2B5D0F9E1734}_is1';

function ReadPreviousUninstallCommand(var CommandLine: String): Boolean;
begin
  CommandLine := '';
  Result := RegQueryStringValue(HKLM64, PulseUninstallKey,
    'UninstallString', CommandLine);
  if not Result then
    Result := RegQueryStringValue(HKLM, PulseUninstallKey,
      'UninstallString', CommandLine);
end;

function SplitCommandLine(const CommandLine: String; var FileName,
  Params: String): Boolean;
var
  S: String;
  I: Integer;
begin
  FileName := '';
  Params := '';
  S := Trim(CommandLine);
  if S = '' then
  begin
    Result := False;
    Exit;
  end;

  if S[1] = '"' then
  begin
    I := 2;
    while (I <= Length(S)) and (S[I] <> '"') do
      I := I + 1;
    if I > Length(S) then
    begin
      Result := False;
      Exit;
    end;
    FileName := Copy(S, 2, I - 2);
    Params := Trim(Copy(S, I + 1, Length(S)));
  end
  else
  begin
    I := Pos(' ', S);
    if I = 0 then
      FileName := S
    else
    begin
      FileName := Copy(S, 1, I - 1);
      Params := Trim(Copy(S, I + 1, Length(S)));
    end;
  end;
  Result := FileName <> '';
end;

function UninstallPreviousVersion: String;
var
  CommandLine, FileName, Params: String;
  ResultCode: Integer;
begin
  Result := '';
  if not ReadPreviousUninstallCommand(CommandLine) then
    Exit;
  if not SplitCommandLine(CommandLine, FileName, Params) then
  begin
    Result := '无法识别旧版本卸载程序，安装已停止。 / Could not parse the previous uninstaller.';
    Exit;
  end;
  if not FileExists(FileName) then
  begin
    Result := '找不到旧版本卸载程序，安装已停止。 / The previous uninstaller could not be found.';
    Exit;
  end;

  { Silent upgrades keep user data; an interactive uninstall still lets the
    user choose cleanup through the checkbox above. }
  if not Exec(FileName,
    Trim(Params + ' /VERYSILENT /SUPPRESSMSGBOXES /NORESTART'),
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Result := '无法启动旧版本卸载程序，安装已停止。 / Could not start the previous uninstaller.'
  else if ResultCode <> 0 then
    Result := '旧版本卸载失败（错误码 ' + IntToStr(ResultCode) + '），安装已停止。 / The previous uninstall failed (exit code ' +
      IntToStr(ResultCode) + ').';
end;

// Stop the index service before replacing files on upgrade; Pulse.Index.exe
// is locked while the service runs. Failure is fine (service not installed).
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  PreviousUninstallError: String;
  ResultCode: Integer;
begin
  Exec('net.exe', 'stop PulseIndex', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  PreviousUninstallError := UninstallPreviousVersion;
  Result := PreviousUninstallError;
end;
