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

#ifdef GesturesSetup
; Rely-A-Cam-Gestures, bundled when package.ps1 is given its setup (-GesturesSetup or RACG_SETUP).
; It's its own program with its own uninstaller; this only runs its setup, silently, with its
; default outputs.
[Tasks]
Name: "gestures"; Description: "Also install Rely-A-Cam-Gestures: hand and face gestures and memes for your phone cameras (about 150 MB)"; Flags: unchecked

[Files]
Source: "{#GesturesSetup}"; DestDir: "{tmp}"; DestName: "RelyACamGestures-Setup.exe"; Flags: deleteafterinstall; Tasks: gestures

[Run]
Filename: "{tmp}\RelyACamGestures-Setup.exe"; Parameters: "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-"; StatusMsg: "Installing Rely-A-Cam-Gestures..."; Tasks: gestures
#endif

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

{ VB-Audio Virtual Cable: offered for removal only when the plugin installed it, which it records
  in HKCU\Software\Rely-A-Cam (src/audio/cable.cpp). A silent uninstall keeps it. }
const
  CableKey = 'Software\Rely-A-Cam';

{ The setup sits in a folder the user can write to and this runs as admin, so it runs only while
  it still carries VB-Audio's signature. }
function SignedByVbAudio(Path: String): Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -NonInteractive -Command "$s = Get-AuthenticodeSignature -LiteralPath ''' + Path + '''; ' +
    'if ($s.Status -eq ''Valid'' -and $s.SignerCertificate.Subject -match ''BUREL VINCENT|VB-Audio'') { exit 0 } else { exit 1 }"',
    '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

procedure ForgetCable();
begin
  RegDeleteValue(HKCU, CableKey, 'VBCable');
  RegDeleteValue(HKCU, CableKey, 'VBCableSetup');
  RegDeleteKeyIfEmpty(HKCU, CableKey);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Marked: Cardinal;
  Setup: String;
  Code: Integer;
begin
  if (CurUninstallStep <> usUninstall) or UninstallSilent() then
    Exit;
  if not RegQueryDWordValue(HKCU, CableKey, 'VBCable', Marked) or (Marked <> 1) then
    Exit;
  if MsgBox('Rely-A-Cam installed VB-Audio Virtual Cable for "Use in other apps". Remove it too?' + #13#10 + #13#10 +
    'Apps using "CABLE Output" as their mic lose it. A restart may be needed to finish.', mbConfirmation, MB_YESNO) <> IDYES then
    Exit;
  if not RegQueryStringValue(HKCU, CableKey, 'VBCableSetup', Setup) or not FileExists(Setup) or not SignedByVbAudio(Setup) then
  begin
    MsgBox('VB-Cable''s own installer isn''t where Rely-A-Cam left it (or isn''t signed by VB-Audio any more), so it was left installed.' + #13#10 + #13#10 +
      'Remove it from vb-audio.com/Cable: download the pack, then right-click VBCABLE_Setup_x64.exe > Run as administrator > Remove Driver.',
      mbInformation, MB_OK);
    ForgetCable();
    Exit;
  end;
  if Exec(Setup, '-u -h', '', SW_HIDE, ewWaitUntilTerminated, Code) then
    ForgetCable()
  else
    MsgBox('VB-Cable''s uninstaller didn''t start, so it was left installed.', mbInformation, MB_OK);
end;
