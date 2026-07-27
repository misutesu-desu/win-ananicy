#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\artifacts\app"
#endif
#ifndef OutputDir
  #define OutputDir "..\artifacts"
#endif

#define AppName "WinAnanicy"
#define AppPublisher "Abdullah Çafer (misutesu-desu)"
#define AppUrl "https://github.com/misutesu-desu/win-ananicy"
#define AppExeName "WinAnanicy.exe"
#define EngineExeName "win-ananicy.exe"

[Setup]
AppId={{7E6CF81C-1246-4D66-88DA-64559A306676}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription=WinAnanicy bilingual installer
VersionInfoProductName={#AppName}
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=WinAnanicy-{#AppVersion}-Setup
SetupIconFile=..\assets\win-ananicy.ico
UninstallDisplayIcon={app}\{#AppExeName}
LicenseFile=..\LICENSE
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
DisableWelcomePage=no
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
ChangesEnvironment=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"

[CustomMessages]
english.StartWithWindows=Start the optimization engine automatically with Windows
turkish.StartWithWindows=Optimizasyon motorunu Windows ile otomatik başlat
english.DesktopIcon=Create a &desktop shortcut
turkish.DesktopIcon=&Masaüstü kısayolu oluştur
english.LaunchApp=Launch WinAnanicy
turkish.LaunchApp=WinAnanicy'yi çalıştır
english.SetupDescription=Smart process optimization without administrator access
turkish.SetupDescription=Yönetici izni gerektirmeyen akıllı süreç optimizasyonu
english.AdditionalTasks=Additional tasks:
turkish.AdditionalTasks=Ek görevler:

[Tasks]
Name: "startup"; Description: "{cm:StartWithWindows}"; GroupDescription: "{cm:AdditionalTasks}"; Flags: checkedonce
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; GroupDescription: "{cm:AdditionalTasks}"; Flags: unchecked

[Dirs]
Name: "{localappdata}\WinAnanicy"
Name: "{localappdata}\WinAnanicy\logs"
Name: "{localappdata}\WinAnanicy\backups"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\data\rules.json"; DestDir: "{localappdata}\WinAnanicy"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#SourceDir}\data\settings.json"; DestDir: "{localappdata}\WinAnanicy"; Flags: onlyifdoesntexist uninsneveruninstall

[Icons]
Name: "{group}\WinAnanicy"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"
Name: "{group}\Uninstall WinAnanicy"; Filename: "{uninstallexe}"
Name: "{autodesktop}\WinAnanicy"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "WinAnanicyEngine"; ValueData: """{app}\{#EngineExeName}"" --background --config ""{localappdata}\WinAnanicy\rules.json"" --settings ""{localappdata}\WinAnanicy\settings.json"""; Flags: uninsdeletevalue; Tasks: startup

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchApp}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\{#EngineExeName}"; Parameters: "--stop"; Flags: runhidden waituntilterminated skipifdoesntexist; RunOnceId: "StopWinAnanicyEngine"

[Code]
const
  EVENT_MODIFY_STATE = $0002;

function OpenEvent(dwDesiredAccess: LongWord; bInheritHandle: Boolean;
  lpName: String): THandle;
  external 'OpenEventW@kernel32.dll stdcall';
function SetEvent(hEvent: THandle): Boolean;
  external 'SetEvent@kernel32.dll stdcall';
function CloseHandle(hObject: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

procedure SignalUiExit;
var
  ExitEvent: THandle;
begin
  ExitEvent := OpenEvent(EVENT_MODIFY_STATE, False, 'Local\WinAnanicy.UI.Exit');
  if ExitEvent <> 0 then
  begin
    SetEvent(ExitEvent);
    CloseHandle(ExitEvent);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  EnginePath: String;
begin
  Result := '';
  SignalUiExit;
  EnginePath := ExpandConstant('{app}\{#EngineExeName}');
  if FileExists(EnginePath) then
  begin
    Exec(EnginePath, '--stop', ExpandConstant('{app}'), SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Sleep(1500);
  end;
  Sleep(1000);
end;

function InitializeUninstall(): Boolean;
begin
  SignalUiExit;
  Sleep(1000);
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  PreferencesPath: String;
  LanguageCode: String;
  StartupValue: String;
  Contents: String;
begin
  if CurStep <> ssPostInstall then
    Exit;

  PreferencesPath := ExpandConstant('{localappdata}\WinAnanicy\ui-settings.json');
  if FileExists(PreferencesPath) then
    Exit;

  if ActiveLanguage = 'turkish' then
    LanguageCode := 'tr'
  else
    LanguageCode := 'en';

  if WizardIsTaskSelected('startup') then
    StartupValue := 'true'
  else
    StartupValue := 'false';

  Contents :=
    '{' + #13#10 +
    '  "Language": "' + LanguageCode + '",' + #13#10 +
    '  "StartWithWindows": ' + StartupValue + ',' + #13#10 +
    '  "MinimizeToTray": true' + #13#10 +
    '}';
  SaveStringToFile(PreferencesPath, Contents, False);
end;
