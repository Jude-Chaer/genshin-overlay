; Per-user installer, no admin. Built by release.yml:
;   ISCC /DAppVersion=v0.3.0 /DSourceDir=<package folder> installer\PocketEsoptron.iss
; The settings, logins and map data stay in %LOCALAPPDATA%\PocketEsoptron,
; the uninstaller asks whether to delete them.

#ifndef AppVersion
  #define AppVersion "dev"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\Release"
#endif
#if Copy(AppVersion, 1, 1) == "v"
  #define ShownVersion Copy(AppVersion, 2)
#else
  #define ShownVersion AppVersion
#endif

[Setup]
AppId={{1BDF16A5-386D-4DA4-9C3F-F7378043FB03}
AppName=Pocket Esoptron
AppVersion={#ShownVersion}
AppPublisher=PocketEsoptron
AppPublisherURL=https://github.com/PocketEsoptron/PocketEsoptron
AppSupportURL=https://github.com/PocketEsoptron/PocketEsoptron/issues
AppUpdatesURL=https://github.com/PocketEsoptron/PocketEsoptron/releases
; %LOCALAPPDATA%\Programs\PocketEsoptron with PrivilegesRequired=lowest
DefaultDirName={autopf}\PocketEsoptron
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=output
OutputBaseFilename=PocketEsoptron-Setup-{#AppVersion}
SetupIconFile=..\resources\icon.ico
UninstallDisplayIcon={app}\PocketEsoptron.exe
UninstallDisplayName=Pocket Esoptron
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; the running copy is asked to quit in PrepareToInstall
CloseApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "portable,*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Pocket Esoptron"; Filename: "{app}\PocketEsoptron.exe"
Name: "{autodesktop}\Pocket Esoptron"; Filename: "{app}\PocketEsoptron.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PocketEsoptron.exe"; Description: "{cm:LaunchProgram,Pocket Esoptron}"; Flags: nowait postinstall skipifsilent
; the app's own update runs this with /VERYSILENT /UPDATE=1
Filename: "{app}\PocketEsoptron.exe"; Flags: nowait; Check: StartAfterUpdate

[UninstallDelete]
; extensions write into their own folders
Type: filesandordirs; Name: "{app}\extensions"
Type: filesandordirs; Name: "{app}\packets"
Type: files; Name: "{app}\*.old"
Type: dirifempty; Name: "{app}"

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  ApprovedKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run';
  RunName = 'PocketEsoptron';

var
  DeleteData: Boolean;

function StartAfterUpdate: Boolean;
begin
  Result := ExpandConstant('{param:UPDATE|0}') = '1';
end;

procedure QuitRunningCopy;
var
  Exe: String;
  ResultCode: Integer;
begin
  Exe := ExpandConstant('{app}\PocketEsoptron.exe');
  if FileExists(Exe) then
    Exec(Exe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  QuitRunningCopy;
  if CheckForMutexes('Local\PocketEsoptron') then
    Result := 'Pocket Esoptron is running. Quit it from its tray icon, then try again.'
  else
    Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  QuitRunningCopy;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Command: String;
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    if not UninstallSilent then
      DeleteData := MsgBox('Also delete your settings, saved logins and downloaded map data?' + #13#10#13#10 +
        'Keep them to use them again if you install Pocket Esoptron later.',
        mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
    if DeleteData then
      Exec(ExpandConstant('{app}\PocketEsoptron.exe'), '--forget-logins', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    // only a Run value that starts this copy
    if RegQueryStringValue(HKCU, RunKey, RunName, Command) and
      (Pos(Lowercase(ExpandConstant('{app}\PocketEsoptron.exe')), Lowercase(Command)) > 0) then
    begin
      RegDeleteValue(HKCU, RunKey, RunName);
      RegDeleteValue(HKCU, ApprovedKey, RunName);
    end;
  end;
  if (CurUninstallStep = usPostUninstall) and DeleteData then
    DelTree(ExpandConstant('{localappdata}\PocketEsoptron'), True, True, True);
end;
