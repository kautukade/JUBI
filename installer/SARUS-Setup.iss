#define MyAppName "SARUS"
#define MyAppVersion "1.3.0"
#define MyAppPublisher "ITCYBER TECHNOLOGIES PVT LTD"
#define MyAppURL "https://github.com/kautukade/SARUS"
#define MyAppExeName "SARUS.exe"

[Setup]
AppId={{DFB4068A-8D99-42A5-A915-1940D16C0C6B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\SARUS
DefaultGroupName=SARUS
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist-installer
OutputBaseFilename=SARUS-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
RestartIfNeededByRun=no
CloseApplications=no
DirExistsWarning=no
UsePreviousAppDir=yes
UninstallDisplayName=SARUS Local Multi-Agent AI OS
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoVersion=1.3.0.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=SARUS One-Click Windows Installer
VersionInfoProductName=SARUS
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCopyright=Copyright (c) 2026 ITCYBER TECHNOLOGIES PVT LTD

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: ".git\*,dist-installer\*,.sarus-venv\*,logs\*,data\*,*.pyc"

[Icons]
Name: "{autoprograms}\SARUS"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autoprograms}\SARUS README"; Filename: "{app}\README.md"
Name: "{autodesktop}\SARUS"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: checkedonce

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ""{app}\installer\UNINSTALL-SARUS.ps1"""; WorkingDir: "{app}"; Flags: runhidden waituntilterminated

[Code]
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
    WizardForm.StatusLabel.Caption := 'Installing SARUS runtime, dependencies and integrations...';

    if not FileExists(BootstrapScript) then
    begin
      MsgBox('SARUS installer payload is incomplete: EXE-INSTALL.ps1 is missing.', mbError, MB_OK);
      Abort;
    end;

    BootstrapArgs := '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "' + BootstrapScript + '"';
    if not Exec(
      ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      BootstrapArgs,
      ExpandConstant('{app}'),
      SW_SHOW,
      ewWaitUntilTerminated,
      ResultCode) then
    begin
      MsgBox('Could not start the SARUS installation engine.', mbError, MB_OK);
      Abort;
    end;

    if ResultCode <> 0 then
    begin
      MsgBox('SARUS installation engine failed. Exit code: ' + IntToStr(ResultCode) + #13#10 +
        'See the installer log at:' + #13#10 + LogPath, mbError, MB_OK);
      Abort;
    end;
  end;
end;
