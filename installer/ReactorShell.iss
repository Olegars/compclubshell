; UTF-8
#define AppVersion "0.1.0"

[Setup]
AppId={{A4E8C1D2-7B31-4F6A-9E20-5C8D1A7B0451}
AppName=REACTOR Shell
AppVersion={#AppVersion}
AppPublisher=0451
DefaultDirName={autopf}\REACTOR Shell
DefaultGroupName=REACTOR Shell
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=ReactorShell-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayIcon={app}\appsector0451.exe
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Ярлык на рабочем столе"; GroupDescription: "Ярлыки:"; Flags: unchecked

[Files]
Source: "..\dist\stage\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Registry]
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Policies\System"; ValueType: dword; ValueName: "DisableTaskMgr"; ValueData: "1"
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Policies\System"; ValueType: dword; ValueName: "DisableLockWorkstation"; ValueData: "1"
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Policies\System"; ValueType: dword; ValueName: "DisableChangePassword"; ValueData: "1"
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Policies\System"; ValueType: dword; ValueName: "HideFastUserSwitching"; ValueData: "1"
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Policies\System"; ValueType: dword; ValueName: "DisableTaskMgr"; ValueData: "1"
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Policies\System"; ValueType: dword; ValueName: "DisableLockWorkstation"; ValueData: "1"
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Policies\System"; ValueType: dword; ValueName: "DisableChangePassword"; ValueData: "1"

[Icons]
Name: "{group}\REACTOR Shell"; Filename: "{app}\appsector0451.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\REACTOR Shell"; Filename: "{app}\appsector0451.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\appsector0451.exe"; Description: "Запустить шелл сейчас"; Flags: nowait postinstall unchecked skipifsilent

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  // Зависший шелл без окна не отвечает на мягкое закрытие Inno Setup.
  Exec('taskkill.exe', '/F /IM appsector0451.exe /T', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := '';
end;