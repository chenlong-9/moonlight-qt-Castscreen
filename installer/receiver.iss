; 灞€鍩熺綉鎶曞睆 - 鎺ユ敹绔紙浼氳瀹ゅぇ灞忥級瀹夎鑴氭湰
; 浣跨敤锛欼SCC.exe receiver.iss
; 渚濊禆锛?.\artifacts\enterprise-V1.0.0\receiver\ 宸叉槸鏈€鏂版瀯寤?
#define AppName "灞€鍩熺綉鎶曞睆"
#define AppVersion "1.0.1"
#define SrcDir "..\artifacts\package\receiver"

[Setup]
AppId={{7F3C1E24-9D5A-4B77-9C1E-2A6B4F0D8E31}
AppName={#AppName} 鎺ユ敹绔?AppVersion={#AppVersion}
AppVerName={#AppName} 鎺ユ敹绔?{#AppVersion}
AppPublisher=LAN Projection
DefaultDirName={localappdata}\Programs\LANProjection\Receiver
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\artifacts\installers
OutputBaseFilename=灞€鍩熺綉鎶曞睆-鎺ユ敹绔?Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\Moonlight.exe
UninstallDisplayName={#AppName} 鎺ユ敹绔?SetupLogging=yes

[Languages]
Name: "cn"; MessagesFile: "ChineseSimplified.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "寮€鏈鸿嚜鍔ㄥ惎鍔ㄦ帴鏀剁锛堟帹鑽愶紝浼氳瀹ゅぇ灞忛渶瑕侊級"; GroupDescription: "闄勫姞浠诲姟锛?; Flags: checkedonce
Name: "desktopicon"; Description: "鍒涘缓妗岄潰蹇嵎鏂瑰紡"; GroupDescription: "闄勫姞浠诲姟锛?

[Files]
Source: "{#SrcDir}\*"; DestDir: "{app}"; Excludes: "*.log,receiver.conf,璁剧疆浼氳瀹ゅ悕绉?ps1"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\鍚姩鎺ユ敹绔?; Filename: "{app}\Moonlight.exe"; Parameters: "standby --ui"; WorkingDir: "{app}"; IconFilename: "{app}\Moonlight.exe"; Comment: "鍚姩浼氳瀹ゆ帴鏀剁"
Name: "{group}\璁剧疆浼氳瀹ゅ悕绉?; Filename: "{app}\Moonlight.exe"; Parameters: "--show-settings"; WorkingDir: "{app}"; IconFilename: "{app}\Moonlight.exe"
Name: "{group}\鏀捐闃茬伀澧?; Filename: "{app}\鏀捐闃茬伀澧?cmd"; WorkingDir: "{app}"; IconFilename: "{app}\Moonlight.exe"
Name: "{group}\璇婃柇"; Filename: "{app}\璇婃柇.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\Moonlight.exe"
Name: "{group}\鍗歌浇 {#AppName} 鎺ユ敹绔?; Filename: "{uninstallexe}"
Name: "{userdesktop}\鍚姩鎺ユ敹绔?; Filename: "{app}\Moonlight.exe"; Parameters: "standby --ui"; WorkingDir: "{app}"; IconFilename: "{app}\Moonlight.exe"; Tasks: desktopicon
Name: "{userstartup}\{#AppName} 鎺ユ敹绔?; Filename: "{app}\Moonlight.exe"; Parameters: "standby --ui"; WorkingDir: "{app}"; IconFilename: "{app}\Moonlight.exe"; Tasks: autostart

[Run]
Filename: "{app}\Moonlight.exe"; Parameters: "standby --ui"; WorkingDir: "{app}"; Description: "绔嬪嵆鍚姩鎺ユ敹绔?; Flags: postinstall nowait skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\receiver-service.log"
