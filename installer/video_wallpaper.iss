; Video Wallpaper - Inno Setup installer script
; Requires Inno Setup 6+ (https://jrsoftware.org/isinfo.php)

#define MyAppName "Video Wallpaper"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Video Wallpaper"
#define MyAppURL "https://github.com/abk056904/Videowallpaperplayer"
#define MyAppExeName "VideoWallpaper.exe"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=installer\output
OutputBaseFilename=VideoWallpaper-{#MyAppVersion}-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoVersion={#MyAppVersion}.0
VersionInfoDescription={#MyAppName} Installer
VersionInfoCompany={#MyAppPublisher}
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startupicon"; Description: "Start with Windows"; GroupDescription: "Startup:"; Flags: checked

[Files]
; Main executable
Source: "build\release\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; FFmpeg DLLs
Source: "build\release\Release\avcodec-61.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\release\Release\avformat-61.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\release\Release\avutil-59.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\release\Release\swscale-8.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\release\Release\swresample-5.dll"; DestDir: "{app}"; Flags: ignoreversion

; MinGW runtime DLLs
Source: "build\release\Release\libgcc_s_seh-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\release\Release\libwinpthread-1.dll"; DestDir: "{app}"; Flags: ignoreversion

; Documentation
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Start with Windows (HKCU Run key)
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "{#MyAppName}"; ValueData: """{app}\{#MyAppExeName}"" --minimized"; \
    Flags: uninsdeletevalue; Tasks: startupicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
