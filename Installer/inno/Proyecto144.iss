; Proyecto 1.44 - instalador de la compilacion de main.
; No compila las ramas PR experimentales ni promete UI multiplayer terminada.
; Game exe and VERSION.txt are created by Installer/scripts/build_installer.ps1.
#ifndef P144_BUILD_ID
  #error "Pass /DP144_BUILD_ID=<short SHA>"
#endif
[Setup]
AppId={{9DB0C299-86D1-47D2-A1DC-14440000B001}
AppName=Proyecto 1.44
AppVersion=0.4.0+{#P144_BUILD_ID}
AppVerName=Proyecto 1.44 - Bones 0.4.0 ({#P144_BUILD_ID})
AppPublisher=Proyecto 1.44
DefaultDirName={localappdata}\Programs\Proyecto144
DefaultGroupName=Proyecto 1.44
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
WizardStyle=modern
Compression=lzma
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=Proyecto144_Instalador_{#P144_BUILD_ID}
UninstallDisplayName=Proyecto 1.44
UninstallDisplayIcon={app}\Proyecto144.exe
ArchitecturesAllowed=x64compatible
SetupLogging=yes
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "Crear acceso directo en el escritorio"; GroupDescription: "Accesos directos:"; Flags: unchecked

[Files]
Source: "..\..\BONES\build\Proyecto144_Bones_ModKit.exe"; DestDir: "{app}"; DestName: "Proyecto144.exe"; Flags: ignoreversion
Source: "..\dist\VERSION.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Proyecto 1.44"; Filename: "{app}\Proyecto144.exe"
Name: "{autodesktop}\Proyecto 1.44"; Filename: "{app}\Proyecto144.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Proyecto144.exe"; Description: "Iniciar Proyecto 1.44"; Flags: nowait postinstall skipifsilent
