#define MyAppName "Jubi"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "ITCYBER TECHNOLOGIES PVT LTD"
#define MyAppURL "https://github.com/kautukade/JUBI"
#define MyAppExeName "Jubi.exe"

[Setup]
AppId={{8AF94329-2DB2-46E3-B227-98D5619E01E4}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases/tag/continuous
DefaultDirName={autopf}\Jubi
DefaultGroupName=Jubi
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist-installer
OutputBaseFilename=Jubi-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
RestartIfNeededByRun=no
CloseApplications=yes
RestartApplications=no
DirExistsWarning=no
UsePreviousAppDir=yes
UninstallDisplayName=Jubi Local AI Agent Platform
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoVersion=0.1.0.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=Jubi One-Click Windows Installer
VersionInfoProductName=Jubi
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCopyright=Copyright (c) 2026 ITCYBER TECHNOLOGIES PVT LTD

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: ".git\*,dist-installer\*,.sarus-venv\*,logs\*,data\*,*.pyc"

[Icons]
Name: "{autoprograms}\Jubi"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autoprograms}\Jubi README"; Filename: "{app}\README.md"
Name: "{autodesktop}\Jubi"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: checkedonce

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ""{app}\installer\UNINSTALL-SARUS.ps1"""; WorkingDir: "{app}"; Flags: runhidden waituntilterminated

[Code]
function IsUpdateMode: Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
  begin
    if CompareText(ParamStr(I), '/UPDATE') = 0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  BootstrapScript: String;
  BootstrapArgs: String;
  LogPath: String;
begin
  if CurStep = ssPostInstall then
  begin
    BootstrapScript := ExpandConstant('{app}\installer\EXE-INSTALL.ps1');
    LogPath := ExpandConstant('{app}\logs\exe-install.log');
    if IsUpdateMode then
      WizardForm.StatusLabel.Caption := 'Updating Jubi, repairing requirements and restarting background services...'
    else
      WizardForm.StatusLabel.Caption := 'Installing Jubi, requirements, AI models and background services...';

    if not FileExists(BootstrapScript) then
    begin
      MsgBox('Jubi installer payload is incomplete: EXE-INSTALL.ps1 is missing.', mbError, MB_OK);
      Abort;
    end;

    BootstrapArgs := '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "' + BootstrapScript + '"';
    if IsUpdateMode then
      BootstrapArgs := BootstrapArgs + ' -UpdateMode';

    if not Exec(
      ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      BootstrapArgs,
      ExpandConstant('{app}'),
      SW_SHOW,
      ewWaitUntilTerminated,
      ResultCode) then
    begin
      MsgBox('Could not start the Jubi installation engine.', mbError, MB_OK);
      Abort;
    end;

    if ResultCode <> 0 then
    begin
      MsgBox('Jubi installation engine failed. Exit code: ' + IntToStr(ResultCode) + #13#10 +
        'See the installer log at:' + #13#10 + LogPath, mbError, MB_OK);
      Abort;
    end;
  end;
end;
