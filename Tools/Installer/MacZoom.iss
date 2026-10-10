; Compiled by Tools\Build.cmd after MSBuild; the version comes from the built executable.

#define AppName "MacZoom"
#define AppExe "MacZoom.exe"
#define RootDir AddBackslash(SourcePath) + "..\.."
#define BuildDir RootDir + "\build\x64\Release"
#define AppVersion GetStringFileInfo(BuildDir + "\" + AppExe, "ProductVersion")

[Setup]
AppId={{C5C4832C-CFE8-4505-A79C-AFF9610DF7BB}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Hel Industries
AppPublisherURL=https://github.com/helindustries/mac-zoom
; MacZoom runs unelevated and needs nothing outside the user's profile, so neither does Setup.
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
LicenseFile={#RootDir}\LICENSE
OutputDir={#RootDir}\build
OutputBaseFilename={#AppName}-{#AppVersion}-windows-x64-setup
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no

[Tasks]
Name: autostart; Description: "Start {#AppName} when I sign in"

[Files]
Source: "{#BuildDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RootDir}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent

[Code]
// A running copy must be asked to quit, never killed: only its own teardown resets the
// magnification, and a killed copy leaves the desktop zoomed with nothing left to undo it.
// Its window is message-only, which FindWindow does not search, hence FindWindowExW with
// HWND_MESSAGE. The single-instance mutex goes last in that teardown, so its release marks
// the point where the executable can be replaced.
const
    HwndMessage = -3;
    WmClose = $0010;
    CloseTimeoutMs = 5000;
    PollMs = 100;
    WindowClass = 'MacZoomWnd';
    InstanceMutex = 'MacZoom_SingleInstance';

function FindWindowEx(Parent, ChildAfter: Longint; ClassName: String; WindowName: Longint): Longint;
    external 'FindWindowExW@user32.dll stdcall';
function PostMessageW(Window, Msg, WParam, LParam: Longint): Longint;
    external 'PostMessageW@user32.dll stdcall';

function CloseRunningCopy(): Boolean;
var
    Window: Longint;
    Waited: Integer;
begin
    Window := FindWindowEx(HwndMessage, 0, WindowClass, 0);
    if Window <> 0 then
        PostMessageW(Window, WmClose, 0, 0);
    Waited := 0;
    while CheckForMutexes(InstanceMutex) and (Waited < CloseTimeoutMs) do
    begin
        Sleep(PollMs);
        Waited := Waited + PollMs;
    end;
    Result := not CheckForMutexes(InstanceMutex);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
    if CloseRunningCopy() then
        Result := ''
    else
        Result := '{#AppName} is still running and did not quit when asked. Choose Exit from its tray icon, then run Setup again.';
end;

function InitializeUninstall(): Boolean;
begin
    Result := CloseRunningCopy();
    if not Result then
        MsgBox('{#AppName} is still running and did not quit when asked. Choose Exit from its tray icon, then uninstall again.', mbError, MB_OK);
end;
