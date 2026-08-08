#define MyAppName "SARUS"
#define MyAppVersion "1.1.0"
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
VersionInfoVersion=1.1.0.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=SARUS Windows Installer
VersionInfoProductName=SARUS
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCopyright=Copyright (c) 2026 ITCYBER TECHNOLOGIES PVT LTD

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: ".git\*,dist-installer\*,.sarus-venv\*,logs\*,data\*,*.pyc"

[Icons]
Name: "{autoprograms}\SARUS"; Filename: "{app}\START_SARUS.bat"; WorkingDir: "{app}"
Name: "{autoprograms}\SARUS README"; Filename: "{app}\README.md"
Name: "{autodesktop}\SARUS"; Filename: "{app}\START_SARUS.bat"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: checkedonce

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\installer\EXE-INSTALL.ps1"""; WorkingDir: "{app}"; StatusMsg: "Installing SARUS runtime and dependencies..."; Flags: waituntilterminated

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\installer\UNINSTALL-SARUS.ps1"""; WorkingDir: "{app}"; Flags: runhidden waituntilterminated

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if not FileExists(ExpandConstant('{app}\installer\EXE-INSTALL.ps1')) then
    Result := ''
  else
    Result := '';
end;

function GetCustomSetupExitCode(): Integer;
begin
  Result := 0;
end;
