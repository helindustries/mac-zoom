; Compiled by Tools\Build.cmd after MSBuild; the version comes from the built executable.

#define AppName "MacZoom"
#define AppExe "MacZoom.exe"
#define RootDir AddBackslash(SourcePath) + "..\.."
#define BuildDir RootDir + "\build\x64\Release"
#define AppVersion GetStringFileInfo(BuildDir + "\" + AppExe, "ProductVersion")
#define Ifeo "SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"

[Setup]
AppId={{C5C4832C-CFE8-4505-A79C-AFF9610DF7BB}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Hel Industries
AppPublisherURL=https://github.com/helindustries/mac-zoom
; MacZoom itself runs unelevated; Setup needs elevation for the priority settings below.
PrivilegesRequired=admin
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
Name: autostart; Description: "Start {#AppName} when someone signs in"

[Files]
Source: "{#BuildDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RootDir}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"

[Registry]
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"""; Tasks: autostart; Flags: uninsdeletevalue
; High CPU priority from the moment the process exists, whoever starts it, and normal memory
; priority, which Task Scheduler would otherwise lower. High is the ceiling Windows applies
; from here; a realtime class is not, and would starve input and the system besides.
Root: HKLM; Subkey: "{#Ifeo}\{#AppExe}"; Flags: uninsdeletekeyifempty
Root: HKLM; Subkey: "{#Ifeo}\{#AppExe}\PerfOptions"; ValueType: dword; ValueName: "CpuPriorityClass"; ValueData: 3; Flags: uninsdeletekey
Root: HKLM; Subkey: "{#Ifeo}\{#AppExe}\PerfOptions"; ValueType: dword; ValueName: "PagePriority"; ValueData: 5
; The zoom itself is composed by dwm.exe. Windows already runs it High; this pins that, and is
; only written, and only removed again, where nothing had set it before.
Root: HKLM; Subkey: "{#Ifeo}\dwm.exe"; Flags: uninsdeletekeyifempty; Check: DwmPriorityUnset
Root: HKLM; Subkey: "{#Ifeo}\dwm.exe\PerfOptions"; ValueType: dword; ValueName: "CpuPriorityClass"; ValueData: 3; Flags: uninsdeletevalue uninsdeletekeyifempty; Check: DwmPriorityUnset

[Run]
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
// A running copy must be asked to quit, never killed: only its own teardown resets the
// magnification, and a killed copy leaves the desktop zoomed with nothing left to undo it.
// Its hidden window takes WM_CLOSE through that teardown, and the single-instance mutex goes
// last in it, so the mutex's release marks the point where the executable can be replaced.
const
    WmClose = $0010;
    CloseTimeoutMs = 5000;
    PollMs = 100;
    WindowClass = 'MacZoomWnd';
    InstanceMutex = 'MacZoom_SingleInstance';

function DwmPriorityUnset(): Boolean;
begin
    Result := not RegValueExists(HKLM, '{#Ifeo}\dwm.exe\PerfOptions', 'CpuPriorityClass');
end;

function CloseRunningCopy(): Boolean;
var
    Window: HWND;
    Waited: Integer;
begin
    Window := FindWindowByClassName(WindowClass);
    if Window <> 0 then
        PostMessage(Window, WmClose, 0, 0);
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
