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
#ifdef FixtureIdentity
  #define ProductAppId "{{7F302FCE-7BA4-4ABE-92CF-62D0937D87AD}"
  #define UninstallGuid "{7F302FCE-7BA4-4ABE-92CF-62D0937D87AD}"
  #define ArtifactSuffix "-fixture"
#else
  #define ProductAppId "{{D39A69EF-8533-4CB2-8A13-770CBCA96340}"
  #define UninstallGuid "{D39A69EF-8533-4CB2-8A13-770CBCA96340}"
  #define ArtifactSuffix ""
#endif

[Setup]
AppId={#ProductAppId}
AppName=Livehime OBS
AppVersion={#AppVersion}
AppVerName=Livehime OBS {#AppVersion}
AppPublisher=lightsing
AppPublisherURL=https://github.com/lightsing
AppSupportURL=https://github.com/lightsing/livehime-obs/issues
AppUpdatesURL=https://github.com/lightsing/livehime-obs/releases
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=Livehime OBS - Bilibili streaming plugin installer
DefaultDirName={code:DefaultInstallDirectory}
AppendDefaultDirName=no
DisableDirPage=no
DisableWelcomePage=no
DisableProgramGroupPage=yes
DirExistsWarning=no
AllowRootDirectory=no
AllowUNCPath=no
PrivilegesRequired=admin
; Explicit /CURRENTUSER plus a non-shared /DIR supports isolated fixture tests.
PrivilegesRequiredOverridesAllowed=dialog
UsePreviousAppDir=yes
UsePreviousPrivileges=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
WizardStyle=modern
LicenseFile={#PayloadRoot}\data\obs-plugins\livehime-obs\docs\LICENSE
OutputDir={#OutputRoot}
OutputBaseFilename=livehime-obs-{#AppVersion}-windows-x64-setup{#ArtifactSuffix}
Compression=lzma2/max
SolidCompression=yes
SetupLogging=yes
SetupMutex=LivehimeObsPluginSetup
CloseApplications=no
RestartApplications=no
Uninstallable=yes
UninstallLogMode=append
UninstallFilesDir={code:SelectedUninstallerRoot}
UninstallDisplayName=Livehime OBS {#AppVersion}
UninstallDisplayIcon={code:SelectedUninstallerRoot}\unins000.exe

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimp.SelectObs=选择 OBS Studio
english.SelectObs=Select OBS Studio
chinesesimp.SelectObsDescription=请选择已安装的 OBS Studio 33.0.1 目录。
english.SelectObsDescription=Select the existing OBS Studio 33.0.1 directory.
chinesesimp.SelectObsDetails=目录应包含 bin\64bit\obs64.exe。安装器在此核验 OBS 33.0.1 x64 和 Qt 6.11.1。升级会沿用原插件位置；新安装再选择独立插件目录。
english.SelectObsDetails=The directory must contain bin\64bit\obs64.exe. Setup checks OBS 33.0.1 x64 and Qt 6.11.1 here. Upgrades keep their existing plugin location; a new installation selects its bundle directory next.
chinesesimp.SelectBundle=选择插件目录（通常保留默认位置）
english.SelectBundle=Select the plugin bundle directory (normally keep the default)
chinesesimp.SelectBundleDetails=标准位置为 ProgramData\obs-studio\plugins\livehime-obs，所有用户共享。最后一级必须为 livehime-obs。便携版可使用其 OBS 目录下的 plugins\livehime-obs。
english.SelectBundleDetails=The standard shared location is ProgramData\obs-studio\plugins\livehime-obs. The final folder must be named livehime-obs. A portable OBS can use plugins\livehime-obs inside its OBS directory.
chinesesimp.MissingObs=未找到 OBS：%1。请在 OBS 目录页选择含 bin\64bit\obs64.exe 的安装目录。
english.MissingObs=OBS was not found at %1. On the OBS directory page, select the installation containing bin\64bit\obs64.exe.
chinesesimp.WrongObs=所选 OBS 必须是 OBS Studio 33.0.1 x64。请使用匹配版本的 OBS。
english.WrongObs=The selected OBS must be OBS Studio 33.0.1 x64. Install a matching OBS version first.
chinesesimp.WrongQt=所选 OBS 的 Qt 运行库不匹配。此安装包需要 Qt 6.11.1。
english.WrongQt=The selected OBS Qt runtime is incompatible. This installer requires Qt 6.11.1.
chinesesimp.WrongBundleName=插件目录的最后一级必须为 livehime-obs；请勿选择 OBS 安装根目录。
english.WrongBundleName=The plugin bundle folder must be named livehime-obs. Do not select the OBS installation root.
chinesesimp.AdminDirectory=此共享或受保护目录需要管理员安装：%1。请取消安装，正常重新运行安装器并允许管理员权限；不要使用 /CURRENTUSER 参数。
english.AdminDirectory=Administrative installation is required for this shared or protected directory: %1. Cancel and run Setup normally with administrator permission; do not use /CURRENTUSER.
chinesesimp.NoWriteAccess=无法写入插件目录的已有父目录：%1。请检查目录权限或使用标准共享位置，再重试。安装器不会更改目录权限。
english.NoWriteAccess=Setup cannot write to the existing parent of the plugin directory: %1. Check its permissions or use the standard shared location, then retry. Setup does not change directory permissions.
chinesesimp.ProbeCleanupFailed=无法清理权限检查临时文件：%1。安装尚未开始，请检查目录权限后重试。
english.ProbeCleanupFailed=Could not remove the permission-check temporary file: %1. Installation has not started; check directory permissions before retrying.
chinesesimp.ObsRunning=所选 OBS 正在运行。请先结束直播并退出 OBS，再重试。安装程序不会自动关闭 OBS。
english.ObsRunning=The selected OBS is running. End your stream and close OBS, then retry. Setup will not close OBS automatically.
chinesesimp.SharedObsRunning=共享插件可能被任意 OBS 实例使用。请先结束直播并退出所有 OBS，再重试。安装程序不会自动关闭 OBS。
english.SharedObsRunning=The shared plugin may be used by any OBS instance. End your stream and close all OBS instances, then retry. Setup will not close OBS automatically.
chinesesimp.ProcessCheckFailed=无法确认 OBS 是否已退出。请关闭 OBS 并确认 Windows Management Instrumentation 服务可用，再重试。
english.ProcessCheckFailed=Could not verify whether OBS is closed. Close OBS and ensure the Windows Management Instrumentation service is available, then retry.
chinesesimp.TargetChanged=此插件已安装在 %1。升级时请保留原插件目录；若要迁移，请先卸载原安装。
english.TargetChanged=This plugin is already installed in %1. Keep that bundle directory when upgrading, or uninstall it before moving the installation.
chinesesimp.HostChanged=此插件记录的 OBS 目录为 %1。升级时请保留原 OBS 目录；若要迁移，请先卸载原安装。
english.HostChanged=This installation is associated with OBS at %1. Keep that OBS directory when upgrading, or uninstall before changing it.
chinesesimp.CurrentUserInstallExists=此插件已按“当前用户”模式安装。请使用原安装范围升级（命令行可用 /CURRENTUSER）；仅在更改安装范围时才需先使用原卸载程序卸载。
english.CurrentUserInstallExists=A current-user installation is registered. Upgrade in its existing mode (use /CURRENTUSER on the command line). Uninstall first only when changing the installation mode.
chinesesimp.AllUsersInstallExists=已存在共享安装。请正常运行安装器，不要使用 /CURRENTUSER；若要迁移，请先使用原卸载程序卸载。
english.AllUsersInstallExists=A shared installation already exists. Run Setup normally without /CURRENTUSER. To move it, first use the original installation's uninstaller.
chinesesimp.InstallModesConflict=检测到两种模式的安装记录。请分别使用原卸载程序处理，再重新运行共享安装器。
english.InstallModesConflict=Installation records exist in both modes. Remove them with their original uninstallers before running the shared installer again.
chinesesimp.DuplicatePlugin=发现旧版或另一份活动的同名插件：%1。请先卸载旧版或移走该副本，再重试。安装器不会删除该副本。
english.DuplicatePlugin=An old or another active copy of this plugin was found at %1. Uninstall the old version or move that copy before retrying. Setup will not delete it.
chinesesimp.ZipChanged=原 ZIP 安装中的插件或更新助手已被移动。请退出安装器，在 OBS 中重新检查更新。
english.ZipChanged=The plugin or update helper from the original ZIP installation was moved. Exit Setup and check for updates again in OBS.
chinesesimp.InstallStateMissing=无法读取本安装的 OBS 目录记录。请先在同一插件目录重新运行安装器修复，再尝试卸载。
english.InstallStateMissing=This installation's OBS directory record could not be read. Repair it by running Setup in the same bundle directory before uninstalling.

[Files]
Source: "{#PayloadRoot}\obs-plugins\64bit\livehime-obs.dll"; DestDir: "{code:SelectedBinaryRoot}"; Flags: ignoreversion
Source: "{#PayloadRoot}\data\obs-plugins\livehime-obs\*"; DestDir: "{code:SelectedDataRoot}"; Excludes: "install-manifest.json"; Flags: ignoreversion recursesubdirs createallsubdirs; AfterInstall: EnsureUnicodeInstallState
Source: "{#PayloadRoot}\manifest-modern.json"; DestDir: "{code:SelectedDataRoot}"; DestName: "install-manifest.json"; Flags: ignoreversion; Check: IsModernLayout
Source: "{#PayloadRoot}\data\obs-plugins\livehime-obs\install-manifest.json"; DestDir: "{code:SelectedDataRoot}"; Flags: ignoreversion; Check: IsLegacyLayout

[INI]
Filename: "{code:SelectedDataRoot}\install-state.ini"; Section: "Install"; Key: "ObsRoot"; String: "{code:SelectedObsRoot}"
Filename: "{code:SelectedDataRoot}\install-state.ini"; Section: "Install"; Key: "SharedBundle"; String: "{code:SelectedSharedBundle}"
Filename: "{code:SelectedDataRoot}\install-state.ini"; Section: "Install"; Key: "Layout"; String: "{code:SelectedLayoutName}"

[UninstallDelete]
; This exact file is generated and owned by Setup. No directory deletes.
Type: files; Name: "{code:SelectedDataRoot}\install-state.ini"

[Code]
const
  PluginUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#UninstallGuid}_is1';
  ModernInstallLayout = 'modern-bundle-v1';
  LegacyInstallLayout = 'legacy-root-v1';

var
  ObsDirectoryPage: TInputDirWizardPage;
  InstallationLayout, RegisteredRoot, RegisteredHost: String;
  AdoptingLegacyZip: Boolean;

function GetBinaryTypeW(FileName: String; var BinaryType: Cardinal): Boolean;
  external 'GetBinaryTypeW@kernel32.dll stdcall';
function CreateFileW(FileName: String; DesiredAccess, ShareMode: Cardinal;
  SecurityAttributes: NativeInt; CreationDisposition, FlagsAndAttributes: Cardinal;
  TemplateFile: NativeInt): NativeInt;
  external 'CreateFileW@kernel32.dll stdcall';
function WriteFile(Handle: NativeInt; Buffer: String; BytesToWrite: Cardinal;
  var BytesWritten: Cardinal; Overlapped: NativeInt): Boolean;
  external 'WriteFile@kernel32.dll stdcall';
function CloseHandle(Handle: NativeInt): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function NormalizedPath(const Value: String): String;
begin
  Result := Lowercase(RemoveBackslashUnlessRoot(ExpandFileName(Value)));
end;

function RegisteredInstallRoot(): String;
var
  RootKey: Integer;
begin
  if IsAdminInstallMode then RootKey := HKLM64 else RootKey := HKCU;
  if not RegQueryStringValue(RootKey, PluginUninstallKey, 'Inno Setup: App Path', Result) then
    if not RegQueryStringValue(RootKey, PluginUninstallKey, 'InstallLocation', Result) then Result := '';
end;

function HasLegacyUpdateFiles(const Root: String): Boolean;
begin
  Result := FileExists(AddBackslash(Root) + 'obs-plugins\64bit\livehime-obs.dll') and
    FileExists(AddBackslash(Root) + 'data\obs-plugins\livehime-obs\livehime-update-helper.exe');
end;

function ConfigureExistingInstallation(): String;
var
  PreviousLayout, PreviousBundle, StatePath, StateHost, ExplicitDir, ExplicitHost: String;
  Registered: Boolean;
begin
  Result := '';
  RegisteredRoot := RegisteredInstallRoot();
  RegisteredHost := GetPreviousData('ObsRoot', '');
  InstallationLayout := ModernInstallLayout;
  if IsAdminInstallMode then Registered := RegKeyExists(HKLM64, PluginUninstallKey)
  else Registered := RegKeyExists(HKCU, PluginUninstallKey);
  if not Registered then begin
    ExplicitDir := ExpandConstant('{param:DIR|}');
    ExplicitHost := ExpandConstant('{param:OBSDIR|}');
    if (ExplicitDir <> '') and (ExplicitHost <> '') and
      (NormalizedPath(ExplicitDir) = NormalizedPath(ExplicitHost)) and HasLegacyUpdateFiles(ExplicitDir) then begin
      RegisteredRoot := RemoveBackslashUnlessRoot(ExpandFileName(ExplicitDir));
      InstallationLayout := LegacyInstallLayout;
      AdoptingLegacyZip := True;
    end;
    Exit;
  end;
  if RegisteredRoot = '' then begin Result := CustomMessage('InstallStateMissing'); Exit; end;
  PreviousLayout := GetPreviousData('InstallLayout', '');
  PreviousBundle := GetPreviousData('BundleRoot', '');
  if PreviousLayout = ModernInstallLayout then begin
    if (RegisteredHost = '') or (PreviousBundle = '') or
      (NormalizedPath(PreviousBundle) <> NormalizedPath(RegisteredRoot)) then
      Result := CustomMessage('InstallStateMissing');
    StatePath := AddBackslash(RegisteredRoot) + 'data\install-state.ini';
    if FileExists(StatePath) then begin
      StateHost := GetIniString('Install', 'ObsRoot', '', StatePath);
      if (GetIniString('Install', 'Layout', '', StatePath) <> ModernInstallLayout) or
        (StateHost = '') or (NormalizedPath(StateHost) <> NormalizedPath(RegisteredHost)) then
        Result := CustomMessage('InstallStateMissing');
    end;
  end else if ((PreviousLayout = '') or (PreviousLayout = LegacyInstallLayout)) and
    (RegisteredHost <> '') and
    (NormalizedPath(RegisteredHost) = NormalizedPath(RegisteredRoot)) and
    FileExists(AddBackslash(RegisteredRoot) + 'bin\64bit\obs64.exe') then
    InstallationLayout := LegacyInstallLayout
  else Result := CustomMessage('InstallStateMissing');
end;

function EffectiveLayout(): String;
begin
  Result := InstallationLayout;
  if Result <> '' then Exit;
  if IsUninstaller then begin
    Result := GetIniString('Install', 'Layout', '', ExpandConstant('{app}\data\install-state.ini'));
    if Result <> ModernInstallLayout then
      Result := GetIniString('Install', 'Layout', '',
        ExpandConstant('{app}\data\obs-plugins\livehime-obs\install-state.ini'));
  end;
end;

function IsModernLayout(): Boolean;
begin
  Result := EffectiveLayout() = ModernInstallLayout;
end;

function IsLegacyLayout(): Boolean;
begin
  Result := EffectiveLayout() = LegacyInstallLayout;
end;

function SelectedLayoutName(Param: String): String;
begin
  Result := EffectiveLayout();
end;

function SelectedBinaryRoot(Param: String): String;
begin
  Result := ExpandConstant('{app}');
  if IsLegacyLayout() then Result := AddBackslash(Result) + 'obs-plugins\64bit';
end;

function SelectedDataRoot(Param: String): String;
begin
  Result := ExpandConstant('{app}\data');
  if IsLegacyLayout() then Result := AddBackslash(Result) + 'obs-plugins\livehime-obs';
end;

function SelectedUninstallerRoot(Param: String): String;
begin
  if IsLegacyLayout() then Result := AddBackslash(SelectedDataRoot('')) + 'installer'
  else Result := ExpandConstant('{app}\installer');
end;

function DefaultInstallDirectory(Param: String): String;
begin
  Result := RegisteredInstallRoot();
  if Result = '' then Result := ExpandConstant('{commonappdata}\obs-studio\plugins\livehime-obs');
end;

function PathWithin(const Path, Parent: String): Boolean;
var
  NormalPath, NormalParent: String;
begin
  NormalPath := NormalizedPath(Path);
  NormalParent := NormalizedPath(Parent);
  Result := (NormalPath = NormalParent) or (Pos(AddBackslash(NormalParent), NormalPath) = 1);
end;

function SharedBundle(const BundleRoot: String): Boolean;
begin
  Result := PathWithin(BundleRoot, ExpandConstant('{commonappdata}'));
end;

function SelectedObsRoot(Param: String): String;
begin
  Result := RemoveBackslashUnlessRoot(ExpandFileName(ObsDirectoryPage.Values[0]));
end;

function SelectedSharedBundle(Param: String): String;
begin
  if IsModernLayout() and SharedBundle(ExpandConstant('{app}')) then Result := '1' else Result := '0';
end;

procedure EnsureUnicodeInstallState();
var
  StatePath: String;
  Handle: NativeInt;
  Written: Cardinal;
  Success: Boolean;
begin
  StatePath := AddBackslash(SelectedDataRoot('')) + 'install-state.ini';
  if FileExists(StatePath) then Exit;
  { Win32 INI APIs preserve Unicode only when an existing file has a UTF-16 BOM. }
  Handle := CreateFileW(StatePath, $40000000, 0, 0, 1, $80, 0);
  if Handle = -1 then RaiseException(CustomMessage('InstallStateMissing'));
  try
    Success := WriteFile(Handle, #$FEFF, 2, Written, 0);
  finally
    CloseHandle(Handle);
  end;
  if (not Success) or (Written <> 2) then RaiseException(CustomMessage('InstallStateMissing'));
end;

function InstallModeError(): String;
var
  UserInstalled, MachineInstalled: Boolean;
begin
  UserInstalled := RegKeyExists(HKCU, PluginUninstallKey);
  MachineInstalled := RegKeyExists(HKLM64, PluginUninstallKey);
  Result := '';
  if UserInstalled and MachineInstalled then Result := CustomMessage('InstallModesConflict')
  else if IsAdminInstallMode and UserInstalled then Result := CustomMessage('CurrentUserInstallExists')
  else if (not IsAdminInstallMode) and MachineInstalled then Result := CustomMessage('AllUsersInstallExists');
end;

function DefaultObsRoot(): String;
var
  Candidate: String;
begin
  Result := ExpandConstant('{param:OBSDIR|}');
  if Result <> '' then Exit;
  Result := GetPreviousData('ObsRoot', '');
  if Result <> '' then Exit;
  Result := ExpandConstant('{commonpf64}\obs-studio');
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

function RunningHostError(const ObsRoot, BundleRoot: String; ForceShared: Boolean): String;
var
  Locator, Services, Processes, Process, ExePath: Variant;
  I: Integer;
  Target: String;
begin
  Result := '';
  Target := NormalizedPath(AddBackslash(ObsRoot) + 'bin\64bit\obs64.exe');
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Services := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Services.ExecQuery('SELECT ExecutablePath FROM Win32_Process WHERE Name=''obs64.exe''');
    if (ForceShared or SharedBundle(BundleRoot)) and (Processes.Count > 0) then begin
      Result := CustomMessage('SharedObsRunning');
      Exit;
    end;
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
  Result := FileExists(Prefix + 'portable_mode') or FileExists(Prefix + 'obs_portable_mode') or
    FileExists(Prefix + 'portable_mode.txt') or FileExists(Prefix + 'obs_portable_mode.txt');
end;

function OtherBundleError(const CandidateRoot, BundleRoot: String): String;
var
  Candidate: String;
begin
  Result := '';
  if NormalizedPath(CandidateRoot) = NormalizedPath(BundleRoot) then Exit;
  Candidate := AddBackslash(CandidateRoot) + 'livehime-obs.dll';
  if not FileExists(Candidate) then Candidate := AddBackslash(CandidateRoot) + 'bin\64bit\livehime-obs.dll';
  if FileExists(Candidate) then Result := FmtMessage(CustomMessage('DuplicatePlugin'), [Candidate]);
end;

function DuplicatePluginError(const ObsRoot, BundleRoot: String): String;
var
  Candidate, ActiveBundle, UserPath: String;
begin
  Result := '';
  Candidate := AddBackslash(ObsRoot) + 'obs-plugins\64bit\livehime-obs.dll';
  if FileExists(Candidate) and not (IsLegacyLayout() and
    (NormalizedPath(ObsRoot) = NormalizedPath(BundleRoot))) then begin
    Result := FmtMessage(CustomMessage('DuplicatePlugin'), [Candidate]);
    Exit;
  end;
  if IsModernLayout() then begin
    Candidate := AddBackslash(BundleRoot) + 'bin\64bit\livehime-obs.dll';
    if FileExists(Candidate) then begin
      Result := FmtMessage(CustomMessage('DuplicatePlugin'), [Candidate]);
      Exit;
    end;
  end;
  if IsPortableRoot(ObsRoot) then ActiveBundle := AddBackslash(ObsRoot) + 'plugins\livehime-obs'
  else ActiveBundle := ExpandConstant('{commonappdata}\obs-studio\plugins\livehime-obs');
  Result := OtherBundleError(ActiveBundle, BundleRoot);
  if Result <> '' then Exit;
  UserPath := GetEnv('OBS_PLUGINS_PATH');
  if UserPath <> '' then Result := OtherBundleError(AddBackslash(UserPath) + 'livehime-obs', BundleRoot);
  if Result <> '' then Exit;
  UserPath := GetEnv('OBS_LEGACY_PLUGINS_PATH');
  if (UserPath <> '') and (GetEnv('OBS_LEGACY_PLUGINS_DATA_PATH') <> '') then begin
    Candidate := AddBackslash(UserPath) + 'livehime-obs.dll';
    if FileExists(Candidate) and (NormalizedPath(Candidate) <>
      NormalizedPath(AddBackslash(SelectedBinaryRoot('')) + 'livehime-obs.dll')) then
      Result := FmtMessage(CustomMessage('DuplicatePlugin'), [Candidate]);
  end;
end;

function BundleLocationError(const BundleRoot: String): String;
begin
  Result := '';
  if IsLegacyLayout() and not AdoptingLegacyZip then Exit;
  if IsModernLayout() and (Lowercase(ExtractFileName(RemoveBackslashUnlessRoot(BundleRoot))) <> 'livehime-obs') then begin
    Result := CustomMessage('WrongBundleName');
    Exit;
  end;
  if (not IsAdminInstallMode) and
    (SharedBundle(BundleRoot) or PathWithin(BundleRoot, ExpandConstant('{commonpf64}')) or
     PathWithin(BundleRoot, ExpandConstant('{commonpf32}'))) then
    Result := FmtMessage(CustomMessage('AdminDirectory'), [BundleRoot]);
end;

function ProbeExistingParent(const Destination: String): String;
var
  Parent, NextParent, ProbeName: String;
  Handle: NativeInt;
begin
  Result := '';
  Parent := RemoveBackslashUnlessRoot(ExpandFileName(Destination));
  while not DirExists(Parent) do begin
    NextParent := ExtractFileDir(Parent);
    if (NextParent = '') or (NextParent = Parent) then begin
      Result := FmtMessage(CustomMessage('NoWriteAccess'), [Parent]);
      Exit;
    end;
    Parent := NextParent;
  end;
  ProbeName := GenerateUniqueName(Parent, '.livehime-write-check');
  { CREATE_NEW prevents overwrites; DELETE_ON_CLOSE removes this empty probe. }
  Handle := CreateFileW(ProbeName, $40010000, 7, 0, 1, $04000100, 0);
  if Handle = -1 then begin
    Result := FmtMessage(CustomMessage('NoWriteAccess'), [Parent]);
    Exit;
  end;
  CloseHandle(Handle);
  if FileExists(ProbeName) then Result := FmtMessage(CustomMessage('ProbeCleanupFailed'), [ProbeName]);
end;

function BundleWriteError(const BundleRoot: String): String;
begin
  Result := ProbeExistingParent(SelectedBinaryRoot(''));
  if Result = '' then Result := ProbeExistingParent(SelectedDataRoot(''));
  if Result = '' then Result := ProbeExistingParent(AddBackslash(SelectedDataRoot('')) + 'locale');
  if Result = '' then Result := ProbeExistingParent(AddBackslash(SelectedDataRoot('')) + 'qt-plugins\tls');
  if Result = '' then Result := ProbeExistingParent(AddBackslash(SelectedDataRoot('')) + 'docs\deps');
  if Result = '' then Result := ProbeExistingParent(SelectedUninstallerRoot(''));
end;

function TargetError(const ObsRoot, BundleRoot: String): String;
var
  PreviousRoot: String;
begin
  Result := InstallModeError();
  if Result <> '' then Exit;
  PreviousRoot := RegisteredRoot;
  if (PreviousRoot <> '') and (NormalizedPath(PreviousRoot) <> NormalizedPath(BundleRoot)) then begin
    Result := FmtMessage(CustomMessage('TargetChanged'), [PreviousRoot]);
    Exit;
  end;
  PreviousRoot := GetPreviousData('ObsRoot', '');
  if (PreviousRoot <> '') and (NormalizedPath(PreviousRoot) <> NormalizedPath(ObsRoot)) then begin
    Result := FmtMessage(CustomMessage('HostChanged'), [PreviousRoot]);
    Exit;
  end;
  if IsLegacyLayout() and (NormalizedPath(ObsRoot) <> NormalizedPath(BundleRoot)) then begin
    Result := FmtMessage(CustomMessage('TargetChanged'), [RegisteredRoot]);
    Exit;
  end;
  if AdoptingLegacyZip and not HasLegacyUpdateFiles(BundleRoot) then begin
    Result := CustomMessage('ZipChanged');
    Exit;
  end;
  Result := HostCompatibilityError(ObsRoot);
  if Result = '' then Result := BundleLocationError(BundleRoot);
  if Result = '' then Result := DuplicatePluginError(ObsRoot, BundleRoot);
  if Result = '' then Result := BundleWriteError(BundleRoot);
end;

function InitializeSetup(): Boolean;
var
  Error: String;
begin
  Error := InstallModeError();
  if Error = '' then Error := ConfigureExistingInstallation();
  Result := Error = '';
  if not Result then begin
    Log(Error);
    if not WizardSilent then MsgBox(Error, mbError, MB_OK);
  end;
end;

procedure InitializeWizard();
begin
  ObsDirectoryPage := CreateInputDirPage(wpLicense, CustomMessage('SelectObs'),
    CustomMessage('SelectObsDescription'), CustomMessage('SelectObsDetails'), False, '');
  ObsDirectoryPage.Add('');
  ObsDirectoryPage.Values[0] := DefaultObsRoot();
  WizardForm.SelectDirLabel.Caption := CustomMessage('SelectBundle');
  WizardForm.SelectDirBrowseLabel.Caption := CustomMessage('SelectBundleDetails');
  if IsLegacyLayout() then begin
    WizardForm.SelectDirLabel.Caption := CustomMessage('SelectObs');
    WizardForm.SelectDirBrowseLabel.Caption := CustomMessage('SelectObsDescription');
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  { An upgrade keeps its physical installation directory. The separate OBS
    page remains visible so the host and plugin directory cannot be confused. }
  Result := (RegisteredRoot <> '') and (PageID = wpSelectDir);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Error: String;
begin
  Error := '';
  if CurPageID = ObsDirectoryPage.ID then Error := HostCompatibilityError(SelectedObsRoot(''))
  else if CurPageID = wpSelectDir then Error := TargetError(SelectedObsRoot(''), WizardDirValue);
  Result := Error = '';
  if not Result then begin
    Log(Error);
    if not WizardSilent then MsgBox(Error, mbError, MB_OK);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  NeedsRestart := False;
  Result := TargetError(SelectedObsRoot(''), ExpandConstant('{app}'));
  if Result = '' then Result := RunningHostError(SelectedObsRoot(''), ExpandConstant('{app}'), False);
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'InstallLayout', EffectiveLayout());
  SetPreviousData(PreviousDataKey, 'InstallRoot', ExpandConstant('{app}'));
  if IsModernLayout() then SetPreviousData(PreviousDataKey, 'BundleRoot', ExpandConstant('{app}'))
  else SetPreviousData(PreviousDataKey, 'BundleRoot', '');
  SetPreviousData(PreviousDataKey, 'ObsRoot', SelectedObsRoot(''));
end;

function InitializeUninstall(): Boolean;
var
  Error, ObsRoot, StatePath: String;
begin
  StatePath := AddBackslash(SelectedDataRoot('')) + 'install-state.ini';
  ObsRoot := GetIniString('Install', 'ObsRoot', '', StatePath);
  if (ObsRoot = '') or ((EffectiveLayout() <> ModernInstallLayout) and
    (EffectiveLayout() <> LegacyInstallLayout)) then
    Error := CustomMessage('InstallStateMissing')
  else Error := RunningHostError(ObsRoot, ExpandConstant('{app}'),
    GetIniBool('Install', 'SharedBundle', False, StatePath));
  Result := Error = '';
  if not Result then begin
    Log(Error);
    if not UninstallSilent then MsgBox(Error, mbError, MB_OK);
  end;
end;
