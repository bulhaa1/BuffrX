; BuffrX installer (Inno Setup 6). Built by tools/installer.ps1; never edited per release.
;
; Installs for the current user, by default to %LOCALAPPDATA%\Programs\BuffrX, without asking for elevation.
; The folder can be changed on the wizard's own page. Uninstall removes the program only - the data root
; (%LOCALAPPDATA%\BuffrX) and the user's clips (Videos\BuffrX) are never listed for deletion, so they stay
; when a different person installs or the program is removed.
;
; Defines passed on the ISCC command line: AppVersion, Src (the packaged folder), WebView2 (bootstrapper).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Src
  #define Src "..\dist\BuffrX-0.0.0"
#endif
#ifndef WebView2
  #define WebView2 "..\work\toolchain\files\MicrosoftEdgeWebView2Setup.exe"
#endif

#define AppName "BuffrX"
#define AppExe "BuffrX.exe"
#define AppURL "https://buffrx.app/"

[Setup]
AppId={{5F0E1C8A-3B7D-4C21-9E4A-7D2B6C1F8A93}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
LicenseFile={#Src}\licenses\BuffrX-GPL-2.0.txt
DefaultDirName={localappdata}\Programs\BuffrX
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
DisableDirPage=no
UsePreviousAppDir=yes
OutputBaseFilename=BuffrX-Setup
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
WizardStyle=modern dynamic
; Inno 6.6+ draws the wizard art in a 202x386 area at 100%, scaled by DPI (534x1022 at 250%), so two
; sizes cover the range and Stretch fills whatever the area is at any scaling. In `dynamic` style the
; dark appearance reads its own *DynamicDark directives, so the same art is named twice or Windows in
; dark mode shows Inno's picture instead.
WizardImageFile=installer-art\wizard-328x628.png,installer-art\wizard-492x942.png
WizardImageFileDynamicDark=installer-art\wizard-328x628.png,installer-art\wizard-492x942.png
WizardSmallImageFile=installer-art\wizard-small-110.png,installer-art\wizard-small-165.png
WizardSmallImageFileDynamicDark=installer-art\wizard-small-110.png,installer-art\wizard-small-165.png
WizardImageStretch=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
AllowNoIcons=yes
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
SetupIconFile=..\src-tauri\icons\icon.ico
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "startmenuicon"; Description: "Create a Start Menu shortcut"; GroupDescription: "Additional icons:"; Flags: checkedonce
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"; Flags: checkedonce

[Files]
; manifest.json and SHA256SUMS.txt verify a zip download; nothing reads them from an installed copy.
Source: "{#Src}\*"; DestDir: "{app}"; Excludes: "manifest.json,SHA256SUMS.txt"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#WebView2}"; Flags: dontcopy

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: startmenuicon
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall
; A silent update passes /relaunch=1 so BuffrX comes back; a quiet install from anywhere else leaves it closed.
Filename: "{app}\{#AppExe}"; Flags: nowait; Check: RelaunchAfterSilent

[Code]
function RelaunchAfterSilent: Boolean;
begin
  Result := ExpandConstant('{param:relaunch|0}') = '1';
end;

{ The editor and the clip preview are WebView2 all the way down, and Windows 10 does not always ship it.
  Three places to look, because a 32-bit or 64-bit registration and a per-user install all occur in the wild. }
function WebView2Missing: Boolean;
var
  Pv: String;
begin
  Result := True;
  if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Pv) and (Pv <> '') and (Pv <> '0.0.0.0') then
    Result := False
  else if RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Pv) and (Pv <> '') and (Pv <> '0.0.0.0') then
    Result := False
  else if RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Pv) and (Pv <> '') and (Pv <> '0.0.0.0') then
    Result := False;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode: Integer;
begin
  Result := '';
  if WebView2Missing then begin
    ExtractTemporaryFile('MicrosoftEdgeWebView2Setup.exe');
    if not Exec(ExpandConstant('{tmp}\MicrosoftEdgeWebView2Setup.exe'), '/silent /install', '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then
      Result := 'The WebView2 installer could not be started.'
    else if (ExitCode <> 0) or WebView2Missing then
      Result := 'WebView2 could not be installed. Connect to the internet and try again.';
  end;
end;
