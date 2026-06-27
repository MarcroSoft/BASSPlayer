; Inno Setup script for BASS PlAIer.
; Built in CI; expects the payload staged in the "release\" folder.
; Version is passed on the command line: ISCC /DMyAppVersion=1.2.3 installer.iss

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

[Setup]
AppId={{B9F6C0A1-5E3D-4A2B-9C7E-2F1A6D8B4C30}
AppName=BASS PlAIer
AppVersion={#MyAppVersion}
AppPublisher=MarcroSoft
DefaultDirName={autopf}\BASS PlAIer
DisableProgramGroupPage=yes
LicenseFile=LICENSE
OutputDir=.
OutputBaseFilename=BASSPlAIer-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\BASSPlAIer.exe
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "release\BASSPlAIer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "release\bass.dll";       DestDir: "{app}"; Flags: ignoreversion
Source: "release\bass_fx.dll";    DestDir: "{app}"; Flags: ignoreversion
Source: "release\bassenc.dll";    DestDir: "{app}"; Flags: ignoreversion
Source: "release\README.html";    DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\BASS PlAIer"; Filename: "{app}\BASSPlAIer.exe"
Name: "{autodesktop}\BASS PlAIer";  Filename: "{app}\BASSPlAIer.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\BASSPlAIer.exe"; Description: "{cm:LaunchProgram,BASS PlAIer}"; Flags: nowait postinstall skipifsilent
