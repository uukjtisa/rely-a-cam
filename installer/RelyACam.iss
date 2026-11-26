; Rely-A-Cam for OBS: the one-click PC installer (Inno Setup 6.4+ / 7).
; Built by scripts/package.ps1, which passes AppVersion, Stage and OutDir.
;
; Installs the plugin where OBS 28+ looks for third-party plugins on Windows
; (%ProgramData%\obs-studio\plugins\rely-a-cam), adds the two inbound firewall rules phones need
;, and removes both on uninstall. It never touches OBS's own folder or scenes.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Stage
  #define Stage "..\release\stage\rely-a-cam"
#endif
#ifndef OutDir
  #define OutDir "..\release"
#endif

#define RuleTcp "Rely-A-Cam (phones, TCP)"
#define RuleUdp "Rely-A-Cam (discovery, UDP)"

[Setup]
AppId={{A751DD27-5228-40E9-9083-F5BA0B4FAB96}
AppName=Rely-A-Cam for OBS
AppVersion={#AppVersion}
AppVerName=Rely-A-Cam {#AppVersion}
AppPublisher=Niccc2007
AppPublisherURL=https://github.com/uukjtisa/rely-a-cam
AppSupportURL=https://github.com/uukjtisa/rely-a-cam/issues
AppUpdatesURL=https://github.com/uukjtisa/rely-a-cam/releases/latest
DefaultDirName={commonappdata}\obs-studio\plugins\rely-a-cam
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=RelyACam-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; The plugin DLL is locked while OBS runs: Restart Manager offers to close OBS first.
CloseApplications=yes
RestartApplications=no
UninstallDisplayName=Rely-A-Cam for OBS
VersionInfoVersion={#AppVersion}

[Messages]
WelcomeLabel2=This installs Rely-A-Cam into OBS Studio, so your phone can be a camera with zoom synced on both screens.%n%nIt also lets phones through Windows Firewall, which is the most common reason a phone can't connect.

[Files]
Source: "{#Stage}\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#RuleTcp}"""; Flags: runhidden; StatusMsg: "Letting phones through Windows Firewall..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#RuleUdp}"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""{#RuleTcp}"" dir=in action=allow protocol=TCP localport=41127-41146 program=""{code:ObsExe}"" profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""{#RuleUdp}"" dir=in action=allow protocol=UDP localport=41126 program=""{code:ObsExe}"" profile=any"; Flags: runhidden
Filename: "{code:ObsExe}"; WorkingDir: "{code:ObsDir}"; Description: "Open OBS now (the Phone Camera dock shows a QR)"; Flags: postinstall nowait skipifsilent runasoriginaluser; Check: ObsFound

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#RuleTcp}"""; Flags: runhidden; RunOnceId: "fwtcp"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#RuleUdp}"""; Flags: runhidden; RunOnceId: "fwudp"

[Code]
function ObsRoot(): String;
begin
  if not RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', Result) then
    Result := ExpandConstant('{commonpf64}\obs-studio');
end;

function ObsDir(Param: String): String;
begin
  Result := ObsRoot() + '\bin\64bit';
end;

function ObsExe(Param: String): String;
begin
  Result := ObsDir('') + '\obs64.exe';
end;

function ObsFound(): Boolean;
begin
  Result := FileExists(ObsExe(''));
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not ObsFound() then
    Result := MsgBox('OBS Studio was not found on this PC.' + #13#10 + #13#10 +
      'Rely-A-Cam is a plugin for OBS. Install OBS from obsproject.com first, then run this again.' + #13#10 + #13#10 +
      'Install the plugin anyway?', mbConfirmation, MB_YESNO) = IDYES;
end;
