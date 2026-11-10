; Inno Setup script for the FxmeNet installer.
; Built by CI:  iscc /DAppVersion=0.1.0 /DStageDir=<dir> /DOutDir=<dir> fxmenet.iss
; StageDir holds: fxmenet-server.exe, fxmenet-top.exe, FxmeNet-manual.pdf, FxmeNet.vst3\ (bundle)

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "stage"
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
AppId={{6E0C1B52-6B6A-4C0D-9D58-3A1F5C0E7F21}
AppName=FxmeNet
AppVersion={#AppVersion}
AppPublisher=FX-Mechanics
AppPublisherURL=https://www.fx-mechanics.com
DefaultDirName={autopf}\FxmeNet
DefaultGroupName=FxmeNet
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir={#OutDir}
OutputBaseFilename=FxmeNet-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ChangesEnvironment=yes
UninstallDisplayName=FxmeNet

[Components]
Name: "plugin"; Description: "FxmeNet plugin (VST3), for the computer that runs the DAW"; Types: full client custom
Name: "server"; Description: "FxmeNet server and fxmenet-top, for the computer that runs the effects"; Types: full server custom

[Types]
Name: "full"; Description: "Plugin and server"
Name: "client"; Description: "Plugin only (DAW computer)"
Name: "server"; Description: "Server only (effects computer)"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Tasks]
Name: "addpath"; Description: "Add the FxmeNet folder to the PATH (to type fxmenet-server and fxmenet-top in a terminal)"; Components: server
Name: "firewall"; Description: "Allow the server through Windows Firewall (UDP 54320, 54321, TCP 54322)"; Components: server

[Files]
Source: "{#StageDir}\fxmenet-server.exe"; DestDir: "{app}"; Components: server; Flags: ignoreversion
Source: "{#StageDir}\fxmenet-top.exe"; DestDir: "{app}"; Components: server; Flags: ignoreversion
Source: "{#StageDir}\FxmeNet.vst3\*"; DestDir: "{commoncf64}\VST3\FxmeNet.vst3"; Components: plugin; Flags: ignoreversion recursesubdirs createallsubdirs

Source: "{#StageDir}\FxmeNet-manual.pdf"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\FxmeNet manual"; Filename: "{app}\FxmeNet-manual.pdf"
Name: "{group}\FxmeNet top"; Filename: "{app}\fxmenet-top.exe"; Components: server
Name: "{group}\Uninstall FxmeNet"; Filename: "{uninstallexe}"

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""FxmeNet server (UDP)"" dir=in action=allow protocol=UDP localport=54320-54321 program=""{app}\fxmenet-server.exe"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""FxmeNet server (TCP)"" dir=in action=allow protocol=TCP localport=54322 program=""{app}\fxmenet-server.exe"""; Flags: runhidden; Tasks: firewall

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""FxmeNet server (UDP)"""; Flags: runhidden; RunOnceId: "fwudp"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""FxmeNet server (TCP)"""; Flags: runhidden; RunOnceId: "fwtcp"

[Registry]
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Tasks: addpath; Check: NeedsPath(ExpandConstant('{app}'))

[Code]
function NeedsPath(Param: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;
