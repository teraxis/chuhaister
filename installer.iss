; Inno Setup script for claude-swap.
;
; Per-user install (no admin prompt): everything lands in %LOCALAPPDATA%, the
; account store stays in %USERPROFILE%\.cswap and is deliberately NOT removed
; on uninstall — those are the user's credentials, not our data.
;
; Build:  "D:\Tools\InnoSetup\ISCC.exe" installer.iss   (see build.ps1 installer)

#define AppName      "chuhAIster"
#define AppVersion   "0.1.1"
#define AppPublisher "Bilyk Ihor"
#define AppURL       "https://github.com/teraxis/chuhaister"
#define TrayExe      "chuhaister-tray.exe"
#define CoreExe      "chuhaister-core.exe"

[Setup]
AppId={{8F3A2C71-5D4E-4B9A-9C2E-7A1B6D8E4F52}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={localappdata}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
OutputDir=dist
OutputBaseFilename=chuhaister-setup-{#AppVersion}
SetupIconFile=chuhaister.ico
UninstallDisplayIcon={app}\{#TrayExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; The tray is a background app; closing it is enough, no reboot ever needed.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Tasks]
Name: "autostart"; Description: "{cm:AutoStartTask}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#TrayExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#TrayExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#TrayExe}"; Tasks: desktopicon

[Registry]
; Start with Windows — same HKCU Run value the tray's own toggle manages, so
; the two never disagree.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "chuhAIster"; ValueData: """{app}\{#TrayExe}"""; \
    Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#TrayExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    Flags: nowait postinstall skipifsilent

[UninstallRun]
; Stop the tray before files are removed so the exe isn't locked.
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im {#TrayExe}"; Flags: runhidden; RunOnceId: "StopTray"

[CustomMessages]
english.AutoStartTask=Start claude-swap when I sign in to Windows
ukrainian.AutoStartTask=Запускати claude-swap під час входу у Windows

[Code]
// The account store holds the user's Claude credentials. Uninstalling the app
// must never destroy them without an explicit, interactive Yes.
//
// A silent uninstall (as an upgrade/reinstall runs) MUST leave the store alone:
// under /SUPPRESSMSGBOXES a Yes/No box returns Yes, so a prompt here would wipe
// the credentials on every reinstall. Bail out before asking when silent.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  VaultDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    if UninstallSilent then Exit;
    VaultDir := ExpandConstant('{%USERPROFILE}\.cswap');
    if DirExists(VaultDir) then
    begin
      if MsgBox('Also delete your saved Claude accounts?' + #13#10 + #13#10 +
                VaultDir + #13#10 + #13#10 +
                'Choose No to keep them for a future reinstall.',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(VaultDir, True, True, True);
    end;
  end;
end;
