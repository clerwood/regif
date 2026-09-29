; Inno Setup script for regif. Built by .github/workflows/build.yml; to build locally:
;   iscc /DAppVersion=0.1.0 /DSourceDir=..\dist\regif installer\regif.iss

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\regif"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
#ifndef Platform
  #define Platform "x64"
#endif

[Setup]
; Keep AppId stable across versions so upgrades replace the previous install.
AppId={{C5E1A8B2-7D34-4F0A-9B6E-2A8D4C1F7E93}
AppName=regif
AppVersion={#AppVersion}
AppVerName=regif {#AppVersion}
AppPublisher=regif contributors
DefaultDirName={autopf}\regif
DefaultGroupName=regif
DisableProgramGroupPage=yes
LicenseFile={#SourceDir}\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=regif-{#AppVersion}-{#Platform}-setup
SetupIconFile=..\src\Regif\Assets\regif.ico
UninstallDisplayIcon={app}\Regif.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
MinVersion=10.0.17763
; Installs per user by default (no admin prompt); the dialog offers an all-users install.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
#if Platform == "ARM64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\regif"; Filename: "{app}\Regif.exe"
Name: "{autodesktop}\regif"; Filename: "{app}\Regif.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Regif.exe"; Description: "{cm:LaunchProgram,regif}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Working files from editing sessions. Originals and exported files are never stored here.
Type: filesandordirs; Name: "{localappdata}\regif\sessions"
