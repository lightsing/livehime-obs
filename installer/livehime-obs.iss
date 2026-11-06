; Build using scripts/build-installer.ps1. The payload is a verified plugin ZIP.
#ifndef PayloadRoot
  #error PayloadRoot must name the verified staging directory.
#endif
#ifndef OutputRoot
  #error OutputRoot must name the installer output directory.
#endif
#ifndef AppVersion
  #error AppVersion must come from CMakeLists.txt.
#endif

[Setup]
AppId={{D39A69EF-8533-4CB2-8A13-770CBCA96340}
AppName=Livehime OBS
AppVersion={#AppVersion}
AppVerName=Livehime OBS {#AppVersion}
AppPublisher=lightsing
AppPublisherURL=https://github.com/lightsing
AppSupportURL=https://github.com/lightsing/livehime-obs/issues
AppUpdatesURL=https://github.com/lightsing/livehime-obs/releases
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=Livehime OBS - Bilibili streaming plugin installer
DefaultDirName={code:DefaultObsDirectory}
AppendDefaultDirName=no
DisableDirPage=no
DisableWelcomePage=no
DisableProgramGroupPage=yes
DirExistsWarning=no
AllowRootDirectory=no
AllowUNCPath=no
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
UsePreviousAppDir=yes
UsePreviousPrivileges=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
WizardStyle=modern
LicenseFile={#PayloadRoot}\data\obs-plugins\livehime-obs\docs\LICENSE
OutputDir={#OutputRoot}
OutputBaseFilename=livehime-obs-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
SetupLogging=yes
SetupMutex=LivehimeObsPluginSetup
CloseApplications=no
RestartApplications=no
Uninstallable=yes
UninstallLogMode=append
UninstallFilesDir={app}\data\obs-plugins\livehime-obs\installer
UninstallDisplayName=Livehime OBS {#AppVersion}
UninstallDisplayIcon={app}\bin\64bit\obs64.exe

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimp.SelectObs=请选择已安装的 OBS Studio 33.0.1 目录
english.SelectObs=Select the existing OBS Studio 33.0.1 directory
chinesesimp.SelectObsDetails=安装目录应包含 bin\64bit\obs64.exe。此插件要求 Windows x64、OBS 33.0.1 和 Qt 6.11.1。
english.SelectObsDetails=The directory must contain bin\64bit\obs64.exe. This plugin requires Windows x64, OBS 33.0.1, and Qt 6.11.1.
chinesesimp.MissingObs=未找到 OBS：%1。请选择 OBS 安装目录，而不是 obs-plugins 子目录。
english.MissingObs=OBS was not found at %1. Select the OBS installation directory, not its obs-plugins subdirectory.
chinesesimp.WrongObs=所选目录必须是 OBS Studio 33.0.1 x64。请使用匹配版本的 OBS。
english.WrongObs=The selected directory must contain OBS Studio 33.0.1 x64. Install a matching OBS version first.
chinesesimp.WrongQt=所选 OBS 的 Qt 运行库不匹配。此安装包需要 Qt 6.11.1。
english.WrongQt=The selected OBS Qt runtime is incompatible. This installer requires Qt 6.11.1.
chinesesimp.ObsRunning=所选目录中的 OBS 正在运行。请先结束直播并退出 OBS，再重试。安装程序不会自动关闭 OBS。
english.ObsRunning=OBS in the selected directory is running. End your stream and close OBS, then retry. Setup will not close OBS automatically.
chinesesimp.ProcessCheckFailed=无法确认 OBS 是否已退出。请关闭 OBS 并确认 Windows Management Instrumentation 服务可用，再重试。
english.ProcessCheckFailed=Could not verify whether OBS is closed. Close OBS and ensure the Windows Management Instrumentation service is available, then retry.
chinesesimp.TargetChanged=此插件已安装在 %1。升级时请保留原目录；若要迁移，请先卸载原安装。
english.TargetChanged=This plugin is already installed in %1. Keep that directory when upgrading, or uninstall it before moving to another OBS installation.
chinesesimp.CurrentUserInstallExists=此插件已按“当前用户”模式安装。请使用 /CURRENTUSER 参数运行安装器以升级；若要更改模式，请先使用原安装的卸载程序卸载。
english.CurrentUserInstallExists=This plugin is already installed for the current user. Run Setup with /CURRENTUSER to upgrade. To change modes, first uninstall using that installation's original uninstaller.
chinesesimp.AllUsersInstallExists=此插件已按“所有用户”模式安装。请使用 /ALLUSERS 参数运行安装器以升级；若要更改模式，请先使用原安装的卸载程序卸载。
english.AllUsersInstallExists=This plugin is already installed for all users. Run Setup with /ALLUSERS to upgrade. To change modes, first uninstall using that installation's original uninstaller.
chinesesimp.InstallModesConflict=检测到“当前用户”和“所有用户”两份安装记录。请分别使用原安装的卸载程序处理这些安装，再以一种模式重新安装。
english.InstallModesConflict=Both current-user and all-users installation records exist. Use each installation's original uninstaller to remove them before reinstalling in one mode.
chinesesimp.DuplicatePlugin=发现另一份同名插件：%1。请先卸载或移走该副本，避免 OBS 加载错误版本。安装程序不会删除该副本。
english.DuplicatePlugin=Another copy of this plugin was found at %1. Uninstall or move that copy first to prevent OBS from loading the wrong version. Setup will not delete it.

[Files]
Source: "{#PayloadRoot}\obs-plugins\64bit\livehime-obs.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#PayloadRoot}\data\obs-plugins\livehime-obs\*"; DestDir: "{app}\data\obs-plugins\livehime-obs"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
const
  PluginUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{D39A69EF-8533-4CB2-8A13-770CBCA96340}_is1';

function GetBinaryTypeW(FileName: String; var BinaryType: Cardinal): Boolean;
  external 'GetBinaryTypeW@kernel32.dll stdcall';

function NormalizedPath(const Value: String): String;
begin
  Result := Lowercase(RemoveBackslashUnlessRoot(ExpandFileName(Value)));
end;

function InstallModeError(): String;
var
  UserInstalled, MachineInstalled: Boolean;
begin
  { UsePreviousPrivileges selects a mode before InitializeSetup. Explicit
    /CURRENTUSER or /ALLUSERS can override it, so check both registry hives. }
  UserInstalled := RegKeyExists(HKCU, PluginUninstallKey);
  MachineInstalled := RegKeyExists(HKLM64, PluginUninstallKey);
  Result := '';
  if UserInstalled and MachineInstalled then
    Result := CustomMessage('InstallModesConflict')
  else if IsAdminInstallMode and UserInstalled then
    Result := CustomMessage('CurrentUserInstallExists')
  else if (not IsAdminInstallMode) and MachineInstalled then
    Result := CustomMessage('AllUsersInstallExists');
end;

function DefaultObsDirectory(Param: String): String;
var
  Candidate: String;
begin
  Result := ExpandConstant('{autopf64}\obs-studio');
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Candidate) then
    if FileExists(AddBackslash(Candidate) + 'bin\64bit\obs64.exe') then begin
      Result := Candidate;
      Exit;
    end;
  if RegQueryStringValue(HKLM32, 'SOFTWARE\OBS Studio', '', Candidate) then
    if FileExists(AddBackslash(Candidate) + 'bin\64bit\obs64.exe') then Result := Candidate;
end;

function HostCompatibilityError(const Root: String): String;
var
  Exe: String;
  VersionMS, VersionLS, BinaryType: Cardinal;
begin
  Result := '';
  Exe := AddBackslash(Root) + 'bin\64bit\obs64.exe';
  if not FileExists(Exe) then begin
    Result := FmtMessage(CustomMessage('MissingObs'), [Exe]);
    Exit;
  end;
  if not GetBinaryTypeW(Exe, BinaryType) then begin
    Result := CustomMessage('WrongObs');
    Exit;
  end;
  if BinaryType <> 6 then begin
    Result := CustomMessage('WrongObs');
    Exit;
  end;
  if not GetVersionNumbers(Exe, VersionMS, VersionLS) then begin
    Result := CustomMessage('WrongObs');
    Exit;
  end;
  if (VersionMS <> (33 shl 16)) or ((VersionLS shr 16) <> 1) then begin
    Result := CustomMessage('WrongObs');
    Exit;
  end;
  if not GetVersionNumbers(AddBackslash(Root) + 'bin\64bit\Qt6Core.dll', VersionMS, VersionLS) then begin
    Result := CustomMessage('WrongQt');
    Exit;
  end;
  if (VersionMS <> ((6 shl 16) + 11)) or ((VersionLS shr 16) <> 1) then
    Result := CustomMessage('WrongQt');
end;

function RunningHostError(const Root: String): String;
var
  Locator, Services, Processes, Process, ExePath: Variant;
  I: Integer;
  Target: String;
begin
  Result := '';
  Target := NormalizedPath(AddBackslash(Root) + 'bin\64bit\obs64.exe');
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Services := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Services.ExecQuery('SELECT ExecutablePath FROM Win32_Process WHERE Name=''obs64.exe''');
    for I := 0 to Processes.Count - 1 do begin
      Process := Processes.ItemIndex(I);
      ExePath := Process.ExecutablePath;
      if VarIsNull(ExePath) or VarIsEmpty(ExePath) then begin
        Result := CustomMessage('ProcessCheckFailed');
        Exit;
      end;
      if NormalizedPath(ExePath) = Target then begin
        Result := CustomMessage('ObsRunning');
        Exit;
      end;
    end;
  except
    Result := CustomMessage('ProcessCheckFailed');
  end;
end;

function IsPortableRoot(const Root: String): Boolean;
var
  Prefix: String;
begin
  Prefix := AddBackslash(Root);
  Result := FileExists(Prefix + 'portable_mode') or
    FileExists(Prefix + 'obs_portable_mode') or
    FileExists(Prefix + 'portable_mode.txt') or
    FileExists(Prefix + 'obs_portable_mode.txt');
end;

function DuplicatePluginError(const Root: String): String;
var
  Candidate, BundleRoot: String;
begin
  Result := '';
  if IsPortableRoot(Root) then
    BundleRoot := AddBackslash(Root) + 'plugins\livehime-obs\'
  else
    BundleRoot := ExpandConstant('{commonappdata}\obs-studio\plugins\livehime-obs\');
  Candidate := BundleRoot + 'livehime-obs.dll';
  if not FileExists(Candidate) then Candidate := BundleRoot + 'bin\64bit\livehime-obs.dll';
  if FileExists(Candidate) then Result := FmtMessage(CustomMessage('DuplicatePlugin'), [Candidate]);
end;

function TargetError(const Root: String): String;
var
  PreviousRoot: String;
begin
  Result := InstallModeError();
  if Result <> '' then Exit;
  PreviousRoot := GetPreviousData('ObsRoot', '');
  if (PreviousRoot <> '') and (NormalizedPath(PreviousRoot) <> NormalizedPath(Root)) then begin
    Result := FmtMessage(CustomMessage('TargetChanged'), [PreviousRoot]);
    Exit;
  end;
  Result := HostCompatibilityError(Root);
  if Result = '' then Result := DuplicatePluginError(Root);
end;

function InitializeSetup(): Boolean;
var
  Error: String;
begin
  Error := InstallModeError();
  Result := Error = '';
  if not Result then begin
    Log(Error);
    if not WizardSilent then MsgBox(Error, mbError, MB_OK);
  end;
end;

procedure InitializeWizard();
begin
  WizardForm.SelectDirLabel.Caption := CustomMessage('SelectObs');
  WizardForm.SelectDirBrowseLabel.Caption := CustomMessage('SelectObsDetails');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Error: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    Error := TargetError(WizardDirValue);
    if Error <> '' then begin
      Log(Error);
      if not WizardSilent then MsgBox(Error, mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  NeedsRestart := False;
  Result := TargetError(ExpandConstant('{app}'));
  if Result = '' then Result := RunningHostError(ExpandConstant('{app}'));
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'ObsRoot', ExpandConstant('{app}'));
end;

function InitializeUninstall(): Boolean;
var
  Error: String;
begin
  Error := RunningHostError(ExpandConstant('{app}'));
  Result := Error = '';
  if not Result then begin
    Log(Error);
    if not UninstallSilent then MsgBox(Error, mbError, MB_OK);
  end;
end;
