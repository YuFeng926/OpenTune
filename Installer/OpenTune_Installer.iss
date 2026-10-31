; OpenTune Installer for Windows
; Inno Setup 6 Script
; Build: ISCC.exe OpenTune_Installer.iss

#define MyAppName "OpenTune"
#define MyAppShortName "OpenTune"
#ifndef MyAppVersion
  #define MyAppVersion "1.8.2"
#endif
#define MyAppPublisher "DAYA"
#define MyAppURL "https://github.com/YuFeng926/OpenTune"
#define MyAppExeName "OpenTune.exe"
#ifndef MyBuildConfig
  #define MyBuildConfig "build-ara-ninja"
#endif

#define StandaloneBuildDir "..\" + MyBuildConfig + "\OpenTuneStandalone_artefacts\Release"
#define VST3BuildDir "..\" + MyBuildConfig + "\OpenTune_artefacts\Release"

[Setup]
AppId={{B8F7C3D2-9E1A-4F6B-8C5D-7E3F2A1B4C9D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppCopyright=Copyright (C) DAYA and contributors
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} Installer
DefaultDirName={commonpf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
PrivilegesRequired=admin
LicenseFile=Licenses\LICENSE-COMBINED.txt
SetupIconFile=..\Resources\AppIcon.ico
WizardImageFile=WizardImage.bmp
WizardSmallImageFile=WizardSmallImage.bmp
WizardStyle=modern
DisableProgramGroupPage=yes
DisableWelcomePage=no
OutputDir=..\dist
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-Windows-x64-Setup
Compression=lzma2/ultra64
SolidCompression=yes
InternalCompressLevel=ultra64
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.0
ShowLanguageDialog=yes
LanguageDetectionMethod=uilanguage
UninstallDisplayIcon={app}\OpenTune.exe
UninstallDisplayName={#MyAppName}
CloseApplications=force
SetupLogging=yes
DisableDirPage=auto
UsePreviousAppDir=no

[Messages]
; 向导内置消息（WelcomeLabel1/2 等）只能在 [Messages] 段覆盖；[CustomMessages] 段仅用于 {cm:} 引用的自定义消息。
; 换行使用 %n。
en.WelcomeLabel1=Welcome to OpenTune Setup
en.WelcomeLabel2=This software is permanently free and open source. If you paid to purchase this software, please contact the purchase platform for a refund. The only official release channel is the GitHub page (github.com/YuFeng926/OpenTune).%n%nSelling the OpenVPI vocoder weights (including the model files distributed together with this software), or charging any disguised fees (such as installation fees, service fees, or membership fees) for them, violates the CC BY-NC-SA 4.0 license. Distributing this software in violation of the AGPLv3 license constitutes copyright infringement. DAYA STUDIO and the OpenVPI Team will pursue legal remedies against such violations.
zh.WelcomeLabel1=欢迎使用 OpenTune 安装程序
zh.WelcomeLabel2=本软件永久开源免费。如果您付费购买了此软件，请联系购买平台退款。唯一官方发布渠道为 GitHub 页面（github.com/YuFeng926/OpenTune）。%n%n售卖 OpenVPI 声码器权重（包括随本软件一起分发的模型文件）、或以收取安装费、服务费、会员费等任何名义变相收费的行为，均属违反 CC BY-NC-SA 4.0 许可。任何违反 AGPLv3 协议而分发此软件的行为将构成著作权侵权。DAYA STUDIO 与 OpenVPI Team 将依法追究其法律责任。
ja.WelcomeLabel1=OpenTune セットアップへようこそ
ja.WelcomeLabel2=本ソフトウェアは永久に無料のオープンソースです。本ソフトウェアを有料で購入された場合は、購入元のプラットフォームに返金をご請求ください。唯一の公式配布チャンネルは GitHub ページ（github.com/YuFeng926/OpenTune）です。%n%nOpenVPI のボコーダー重み（本ソフトウェアと共に配布されるモデルファイルを含む）の販売、またはインストール費・サービス費・会員費等の名目によるいかなる変則的な課金も、CC BY-NC-SA 4.0 ライセンスに違反します。AGPLv3 ライセンスに違反して本ソフトウェアを配布する行為は著作権侵害を構成します。DAYA STUDIO および OpenVPI Team は法的措置を講じます。
ru.WelcomeLabel1=Добро пожаловать в установку OpenTune
ru.WelcomeLabel2=Эта программа навсегда бесплатна и распространяется как открытое ПО. Если вы заплатили за её приобретение, обратитесь на площадку покупки для возврата средств. Единственный официальный канал распространения — страница GitHub (github.com/YuFeng926/OpenTune).%n%nПродажа весов вокодера OpenVPI (включая файлы моделей, распространяемые вместе с этим программным обеспечением) или взимание любых скрытых платежей (таких как плата за установку, обслуживание или членство) нарушает лицензию CC BY-NC-SA 4.0. Распространение этого программного обеспечения с нарушением лицензии AGPLv3 является нарушением авторских прав. DAYA STUDIO и OpenVPI Team примут юридические меры.
es.WelcomeLabel1=Bienvenido al instalador de OpenTune
es.WelcomeLabel2=Este software es gratuito y de código abierto de forma permanente. Si pagó por adquirirlo, solicite un reembolso a la plataforma de compra. El único canal oficial de distribución es la página de GitHub (github.com/YuFeng926/OpenTune).%n%nVender los pesos del vocoder de OpenVPI (incluidos los archivos de modelo distribuidos junto con este software), o cobrar cualquier tarifa encubierta (como tarifas de instalación, servicio o membresía) por ellos, infringe la licencia CC BY-NC-SA 4.0. Distribuir este software en violación de la licencia AGPLv3 constituye una infracción de derechos de autor. DAYA STUDIO y OpenVPI Team emprenderán acciones legales.

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "zh"; MessagesFile: "Languages\ChineseSimplified.isl"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"

[Types]
Name: "full"; Description: "{cm:TypeFull}"
Name: "standalone_type"; Description: "{cm:TypeStandalone}"
Name: "vst3_type"; Description: "{cm:TypeVST3}"
Name: "custom"; Description: "{cm:TypeCustom}"

[Components]
Name: "standalone"; Description: "{cm:CompStandalone}"; Types: full standalone_type custom; Flags: dontinheritcheck
Name: "vst3plugin"; Description: "{cm:CompVST3}"; Types: full vst3_type custom; Flags: dontinheritcheck

[Tasks]
Name: "desktopicon"; Description: "{cm:TaskDesktopIcon}"; Components: standalone; GroupDescription: "{cm:TaskGroup}"

[Files]
; Standalone - main executable
Source: "{#StandaloneBuildDir}\Standalone\{#MyAppExeName}"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

; Standalone - inference runtime DLLs
Source: "{#StandaloneBuildDir}\Standalone\DirectML.dll"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#StandaloneBuildDir}\Standalone\OpenTuneOnnxRuntime_1_24_4.dll"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

; Standalone - D3D12 Agility SDK
Source: "{#StandaloneBuildDir}\Standalone\D3D12\D3D12Core.dll"; DestDir: "{app}\D3D12"; Components: standalone; Flags: ignoreversion
Source: "{#StandaloneBuildDir}\Standalone\D3D12\D3D12SDKLayers.dll"; DestDir: "{app}\D3D12"; Components: standalone; Flags: ignoreversion

; AI models - single shared copy, installed for either component (Inno treats the
; component list as OR). Both the Standalone and the VST3 bundle resolve it from
; {commonappdata}\OpenTune\Models; only ZIP/portable and development builds keep
; a copy next to the executable.
Source: "{#StandaloneBuildDir}\Standalone\models\fcpe.onnx"; DestDir: "{commonappdata}\OpenTune\Models"; Components: standalone vst3plugin; Flags: ignoreversion
Source: "{#StandaloneBuildDir}\Standalone\models\hifigan.onnx"; DestDir: "{commonappdata}\OpenTune\Models"; Components: standalone vst3plugin; Flags: ignoreversion
; Standalone - documents
Source: "{#StandaloneBuildDir}\Standalone\docs\*"; DestDir: "{app}\docs"; Components: standalone; Flags: ignoreversion recursesubdirs

; Standalone - license notices
Source: "..\NOTICE.txt"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "..\NOTICE.zh-CN.txt"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "..\STATEMENTS.txt"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "..\LICENSES\soundtouch.LGPL-2.1.txt"; DestDir: "{app}\licenses"; Components: standalone; Flags: ignoreversion
Source: "..\LICENSES\cursors.LGPL-3.0.txt"; DestDir: "{app}\licenses"; Components: standalone; Flags: ignoreversion

; VST3 plugin - entire bundle (含 Contents\x86_64-win 下的推理运行时 DLL)
; models 从 bundle 排除（Excludes）：模型在安装包内仅存一份，安装在
; {commonappdata}\OpenTune\Models，由 Standalone 与 VST3 共用（ModelPathResolver 依次
; 尝试模块目录 > Resources > Program Files > ProgramData 命中），bundle 不携带模型副本
Source: "{#VST3BuildDir}\VST3\OpenTune.vst3\*"; DestDir: "{code:GetVST3Dir}\OpenTune.vst3"; Components: vst3plugin; Excludes: "Contents\x86_64-win\models"; Flags: ignoreversion recursesubdirs createallsubdirs

; VST3 ARA bundle - same complete bundle installed to the system-wide ARA directory.
; VST3 scanners do not search ARA paths; this enables ARA host discovery on the
; standard common CLAP/ARA location while keeping the user-chosen VST3 dir above.
Source: "{#VST3BuildDir}\VST3\OpenTune.vst3\*"; DestDir: "{commoncf}\ARA\OpenTune.vst3"; Components: vst3plugin; Excludes: "Contents\x86_64-win\models"; Flags: ignoreversion recursesubdirs createallsubdirs

; Microsoft Visual C++ v14 x64 Redistributable prerequisite.
; Official source: https://aka.ms/vc14/vc_redist.x64.exe
; Extracted and executed by PrepareToInstall before OpenTune files are installed.
Source: "redist\VC_redist.x64.exe"; Flags: dontcopy

[InstallDelete]
Type: files; Name: "{app}\onnxruntime.dll"
Type: files; Name: "{app}\onnxruntime_providers_shared.dll"
; Standalone: the runtime DLL name carries the ONNX Runtime version, so upgrades must not
; leave a stale copy behind (wildcard cleans every previous version in one entry).
Type: files; Name: "{app}\OpenTuneOnnxRuntime_*.dll"; Components: standalone
Type: files; Name: "{code:GetVST3Dir}\OpenTune.vst3\Contents\x86_64-win\onnxruntime.dll"
Type: files; Name: "{code:GetVST3Dir}\OpenTune.vst3\Contents\x86_64-win\onnxruntime_providers_shared.dll"
; VST3 bundle: remove old versioned runtime DLL and stale bundled models from upgrades
Type: files; Name: "{code:GetVST3Dir}\OpenTune.vst3\Contents\x86_64-win\OpenTuneOnnxRuntime_*.dll"; Components: vst3plugin
Type: filesandordirs; Name: "{code:GetVST3Dir}\OpenTune.vst3\Contents\x86_64-win\models"

; ARA bundle: clean up old bare ORT, old versioned runtime, and stale models
; (parallel to VST3 above; only cleans inside the OpenTune ARA bundle dir)
Type: files; Name: "{commoncf}\ARA\OpenTune.vst3\Contents\x86_64-win\onnxruntime.dll"
Type: files; Name: "{commoncf}\ARA\OpenTune.vst3\Contents\x86_64-win\onnxruntime_providers_shared.dll"
Type: files; Name: "{commoncf}\ARA\OpenTune.vst3\Contents\x86_64-win\OpenTuneOnnxRuntime_*.dll"; Components: vst3plugin
Type: filesandordirs; Name: "{commoncf}\ARA\OpenTune.vst3\Contents\x86_64-win\models"

; AI models: single shared location, always replaced with the current set
Type: filesandordirs; Name: "{commonappdata}\OpenTune\Models"
; Legacy layouts kept models next to the executable (chosen or default install dir).
; A stale copy there would shadow the shared one (module directory is searched first),
; so both locations are cleaned before the new files are copied.
Type: filesandordirs; Name: "{app}\models"
Type: filesandordirs; Name: "{commonpf}\OpenTune\models"
; Clean install: drop previous shortcuts before recreating them.
Type: files; Name: "{commondesktop}\{#MyAppShortName}.lnk"; Components: standalone
Type: filesandordirs; Name: "{commonprograms}\{#MyAppShortName}"; Components: standalone

[Dirs]
Name: "{app}"; Components: standalone
Name: "{app}\D3D12"; Components: standalone
Name: "{app}\docs"; Components: standalone
Name: "{app}\licenses"; Components: standalone
Name: "{commonappdata}\OpenTune"; Components: standalone vst3plugin
Name: "{commonappdata}\OpenTune\Models"; Components: standalone vst3plugin
Name: "{commoncf}\ARA"; Components: vst3plugin

[Icons]
Name: "{group}\{#MyAppShortName}"; Filename: "{app}\{#MyAppExeName}"; Components: standalone; Comment: "{cm:IconComment}"
Name: "{group}\{cm:IconUserGuide}"; Filename: "{app}\docs\UserGuide_en.html"; Components: standalone
Name: "{group}\{cm:IconUninstall}"; Filename: "{uninstallexe}"; Components: standalone
Name: "{commondesktop}\{#MyAppShortName}"; Filename: "{app}\{#MyAppExeName}"; Components: standalone; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram}"; Components: standalone; Check: ShouldLaunchStandalone; Flags: postinstall nowait skipifsilent unchecked

[Code]
var
  VST3DirPage: TInputDirWizardPage;
  VCRedistNeedsRestart: Boolean;

function VersionMatchesExpected(const ActualVersion, ExpectedVersion: String): Boolean;
begin
  Result := (CompareText(ActualVersion, ExpectedVersion) = 0) or
    ((Length(ActualVersion) > Length(ExpectedVersion)) and
     (Copy(ActualVersion, 1, Length(ExpectedVersion)) = ExpectedVersion) and
     (ActualVersion[Length(ExpectedVersion) + 1] = '.'));
end;

function ValidateSharedModels(var FailureDetails: String): Boolean;
var
  RequiredFiles: array[0..1] of String;
  I: Integer;
  FilePath: String;
begin
  Result := True;
  RequiredFiles[0] := '{commonappdata}\OpenTune\Models\fcpe.onnx';
  RequiredFiles[1] := '{commonappdata}\OpenTune\Models\hifigan.onnx';

  for I := 0 to 1 do
  begin
    FilePath := ExpandConstant(RequiredFiles[I]);
    if not FileExists(FilePath) then
    begin
      Result := False;
      FailureDetails := FailureDetails + #13#10 + 'Missing: ' + FilePath;
      Log('Install validation: missing ' + FilePath);
    end;
  end;
end;

function ValidateInstalledStandalone(var FailureDetails: String): Boolean;
var
  RequiredFiles: array[0..4] of String;
  I: Integer;
  FilePath: String;
  InstalledVersion: String;
begin
  Result := True;

  RequiredFiles[0] := '{app}\OpenTune.exe';
  RequiredFiles[1] := '{app}\OpenTuneOnnxRuntime_1_24_4.dll';
  RequiredFiles[2] := '{app}\DirectML.dll';
  RequiredFiles[3] := '{app}\D3D12\D3D12Core.dll';
  RequiredFiles[4] := '{app}\D3D12\D3D12SDKLayers.dll';

  for I := 0 to 4 do
  begin
    FilePath := ExpandConstant(RequiredFiles[I]);
    if not FileExists(FilePath) then
    begin
      Result := False;
      FailureDetails := FailureDetails + #13#10 + 'Missing: ' + FilePath;
      Log('Standalone install validation: missing ' + FilePath);
    end;
  end;

  FilePath := ExpandConstant('{app}\OpenTune.exe');
  if FileExists(FilePath) then
  begin
    if not GetVersionNumbersString(FilePath, InstalledVersion) then
    begin
      Result := False;
      FailureDetails := FailureDetails + #13#10 + 'Cannot read version: ' + FilePath;
      Log('Standalone install validation: cannot read version from ' + FilePath);
    end
    else if not VersionMatchesExpected(InstalledVersion, '{#MyAppVersion}') then
    begin
      Result := False;
      FailureDetails := FailureDetails + #13#10 +
        'Version mismatch: expected {#MyAppVersion}, found ' + InstalledVersion;
      Log('Standalone install validation: expected version {#MyAppVersion}, found ' + InstalledVersion);
    end
    else
      Log('Standalone install validation: OpenTune.exe version ' + InstalledVersion);
  end;
end;

procedure ValidatePostInstall;
var
  FailureDetails: String;
begin
  if not (WizardIsComponentSelected('standalone') or WizardIsComponentSelected('vst3plugin')) then
    Exit;

  FailureDetails := '';
  if not ValidateSharedModels(FailureDetails) then
  begin
    Log('Shared model validation failed:' + FailureDetails);
    MsgBox(ExpandConstant('{cm:InstallValidationFailed}') + FailureDetails,
      mbError, MB_OK);
    RaiseException('OpenTune shared model validation failed.');
  end;

  if WizardIsComponentSelected('standalone') then
  begin
    FailureDetails := '';
    if not ValidateInstalledStandalone(FailureDetails) then
    begin
      Log('Standalone installation validation failed:' + FailureDetails);
      MsgBox(ExpandConstant('{cm:InstallValidationFailed}') + FailureDetails,
        mbError, MB_OK);
      RaiseException('OpenTune Standalone installation validation failed.');
    end;

    Log('Standalone installation validation passed.');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    ValidatePostInstall;
end;

function ShouldLaunchStandalone: Boolean;
begin
  Result := not VCRedistNeedsRestart;
end;

procedure InitializeWizard;
begin
  VST3DirPage := CreateInputDirPage(
    wpSelectComponents,
    ExpandConstant('{cm:VST3DirTitle}'),
    ExpandConstant('{cm:VST3DirDesc}'),
    ExpandConstant('{cm:VST3DirLabel}'),
    False, 'New Folder');
  VST3DirPage.Add('');
  VST3DirPage.Values[0] := ExpandConstant('{commoncf}\VST3');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = VST3DirPage.ID then
    Result := not WizardIsComponentSelected('vst3plugin');
end;

function IsProcessRunning(const AName: String): Boolean;
var
  wmi: Variant;
  items: Variant;
begin
  Result := False;
  try
    wmi := CreateOleObject('winmgmts:{impersonationLevel=impersonate}!\\.\root\cimv2');
    items := wmi.ExecQuery('SELECT Name FROM Win32_Process WHERE Name = "' + AName + '"');
    Result := items.Count > 0;
  except
    Result := False;
  end;
end;

function IsAnyDawRunning: Boolean;
var
  daws: array[0..14] of String;
  i: Integer;
begin
  daws[0]  := 'FL64.exe';
  daws[1]  := 'reaper.exe';
  daws[2]  := 'CubasePro.exe';
  daws[3]  := 'Cubase.exe';
  daws[4]  := 'Live.exe';
  daws[5]  := 'Studio One.exe';
  daws[6]  := 'StudioOne.exe';
  daws[7]  := 'Cakewalk.exe';
  daws[8]  := 'BitwigStudio.exe';
  daws[9]  := 'Nuendo.exe';
  daws[10] := 'Protools.exe';
  daws[11] := 'Waveform.exe';
  daws[12] := 'Samplitude.exe';
  daws[13] := 'Mixcraft.exe';
  daws[14] := 'Sonar.exe';
  Result := False;
  for i := 0 to 14 do
  begin
    if IsProcessRunning(daws[i]) then
    begin
      Result := True;
      Break;
    end;
  end;
end;

function VCRedistInstalled: Boolean;
var
  major: Cardinal;
  minor: Cardinal;
begin
  Result := False;
  if RegQueryDwordValue(HKEY_LOCAL_MACHINE,
       'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Major', major) and
     RegQueryDwordValue(HKEY_LOCAL_MACHINE,
       'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Minor', minor) then
    Result := (major = 14) and (minor >= 30);
end;

function CleanPreviousInstall(var FailureDetails: String): Boolean;
var
  PreviousDir: String;
begin
  Result := True;

  if not RegQueryStringValue(
       HKEY_LOCAL_MACHINE_64,
       'Software\Microsoft\Windows\CurrentVersion\Uninstall\{B8F7C3D2-9E1A-4F6B-8C5D-7E3F2A1B4C9D}_is1',
       'InstallLocation', PreviousDir) then
  begin
    Log('Clean install: no previous install location found');
    Exit;
  end;

  PreviousDir := RemoveBackslashUnlessRoot(PreviousDir);
  if (PreviousDir = '') or not DirExists(PreviousDir) then
  begin
    Log('Clean install: previous install directory not present: ' + PreviousDir);
    Exit;
  end;

  if CompareText(PreviousDir, RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0 then
  begin
    Log('Clean install: previous install directory is the target directory, skipping removal: ' + PreviousDir);
    Exit;
  end;

  if not (FileExists(PreviousDir + '\OpenTune.exe') and
          FileExists(PreviousDir + '\unins000.exe')) then
  begin
    Log('Clean install: previous install directory does not look like an OpenTune install, skipping: ' + PreviousDir);
    Exit;
  end;

  Log('Clean install: removing previous install directory: ' + PreviousDir);
  if not DelTree(PreviousDir, True, True, True) then
  begin
    FailureDetails := PreviousDir;
    Result := False;
    Log('Clean install: failed to remove previous install directory: ' + PreviousDir);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  resCode: Integer;
  FailureDetails: String;
begin
  Result := '';

  if IsProcessRunning('OpenTune.exe') then
  begin
    if MsgBox(ExpandConstant('{cm:AppRunningWarning}'), mbConfirmation, MB_YESNO) = IDYES then
    begin
      Exec('taskkill', '/f /im OpenTune.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
    end
    else
    begin
      Result := ExpandConstant('{cm:AppRunningBlock}');
      Exit;
    end;
  end;

  if WizardIsComponentSelected('vst3plugin') and IsAnyDawRunning then
  begin
    if MsgBox(ExpandConstant('{cm:DawRunningWarning}'), mbConfirmation, MB_YESNO) = IDYES then
    begin
      Exec('taskkill', '/f /im FL64.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im reaper.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im CubasePro.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im Cubase.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im Live.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im "Studio One.exe"', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im StudioOne.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im Cakewalk.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im BitwigStudio.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im Nuendo.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im Protools.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im Waveform.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im Samplitude.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im Mixcraft.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
      Exec('taskkill', '/f /im Sonar.exe', '', SW_HIDE, ewWaitUntilTerminated, resCode);
    end
    else
    begin
      Result := ExpandConstant('{cm:DawRunningBlock}');
      Exit;
    end;
  end;

  if WizardIsComponentSelected('standalone') or WizardIsComponentSelected('vst3plugin') then
  begin
    if VCRedistInstalled then
      Log('Microsoft Visual C++ x64 Redistributable v14.30+ already installed, skipping')
    else if CheckForMutexes('Global\_MSIExecute') then
    begin
      Log('Another MSI installation is in progress, skipping VC++ Redistributable to avoid deadlock');
    end
    else
    begin
      Log('Installing Microsoft Visual C++ Redistributable x64 prerequisite');
      ExtractTemporaryFile('VC_redist.x64.exe');
      if not Exec(
        ExpandConstant('{tmp}\VC_redist.x64.exe'),
        '/install /passive /norestart /log "' + ExpandConstant('{tmp}\OpenTune-VC-redist.log') + '"',
        '', SW_SHOW, ewWaitUntilTerminated, resCode) then
      begin
        Log('Failed to launch VC_redist.x64.exe: ' + SysErrorMessage(resCode));
        Result := ExpandConstant('{cm:VCRedistInstallFailed}');
        Exit;
      end;

      if resCode = 0 then
        Log('Microsoft Visual C++ Redistributable x64 prerequisite installed')
      else if resCode = 1638 then
        Log('A newer Microsoft Visual C++ Redistributable x64 is already installed')
      else if resCode = 3010 then
      begin
        Log('Microsoft Visual C++ Redistributable x64 installed; restart required');
        VCRedistNeedsRestart := True;
        NeedsRestart := True;
      end
      else
      begin
        Log('Microsoft Visual C++ Redistributable x64 failed with exit code ' + IntToStr(resCode));
        Result := ExpandConstant('{cm:VCRedistInstallFailed}');
        Exit;
      end;
    end;
  end;

  if WizardIsComponentSelected('standalone') then
  begin
    if not CleanPreviousInstall(FailureDetails) then
    begin
      Result := 'Could not remove the previous OpenTune installation directory:' + #13#10 +
        FailureDetails + #13#10 +
        'Close any program using it and run the installer again.';
      Exit;
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  modelsDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // Shared models are used by both components, so remove them only when no
    // OpenTune binary remains on disk (an out-of-band copy may still need them).
    if FileExists(ExpandConstant('{app}\OpenTune.exe'))
       or FileExists(ExpandConstant('{commoncf}\ARA\OpenTune.vst3\Contents\x86_64-win\OpenTune.vst3')) then
      Exit;

    modelsDir := ExpandConstant('{commonappdata}\OpenTune\Models');
    if DirExists(modelsDir) then
      DelTree(modelsDir, True, True, False);
  end;
end;

function GetVST3Dir(Param: String): String;
begin
  Result := VST3DirPage.Values[0];
end;

[CustomMessages]
; =====================================================
; English
; =====================================================
en.TypeFull=Full installation (Standalone + VST3 plugin)
en.TypeStandalone=Standalone application only
en.TypeVST3=VST3 plugin only
en.TypeCustom=Custom installation
en.CompStandalone=OpenTune Standalone Application
en.CompVST3=OpenTune VST3 Plugin (ARA2)
en.CompVST3Desc=Install the VST3 plugin for use in DAWs (e.g. FL Studio, Cubase, REAPER)
en.TaskDesktopIcon=Create a desktop shortcut
en.TaskGroup=Additional shortcuts:
en.LaunchProgram=Launch OpenTune
en.IconComment=AI-powered vocal pitch correction
en.IconUserGuide=User Guide
en.IconUninstall=Uninstall OpenTune
en.VST3SystemWide=Install for all users (recommended)
en.VST3PerUser=Install for current user only
en.AppRunningWarning=OpenTune is currently running. Close it now before installing? (Recommended)
en.AppRunningBlock=Please close OpenTune before running the installer.
en.DawRunningWarning=A digital audio workstation (DAW) is currently running which may have the OpenTune VST3 plugin loaded. Close it now to proceed with installation? (Recommended)
en.DawRunningBlock=Please close your DAW before installing the VST3 plugin.
en.ErrorStandaloneMissing=OpenTune executable not found after installation. The installation may be corrupted.
en.InstallValidationFailed=OpenTune was installed, but the installation verification failed. Please review the listed files and run the installer again.
en.VCRedistInstallFailed=Microsoft Visual C++ Redistributable x64 could not be installed. OpenTune cannot continue because its audio analysis runtime is unavailable.
zh.AppRunningWarning=OpenTune 正在运行。是否立即关闭后再安装？（推荐）
zh.AppRunningBlock=请先关闭 OpenTune 后再运行安装程序。
zh.DawRunningWarning=检测到宿主（DAW）正在运行，可能已加载 OpenTune VST3 插件。是否立即关闭后再安装？（推荐）
zh.DawRunningBlock=请先关闭宿主软件后再安装 VST3 插件。
ja.AppRunningWarning=OpenTune が実行中です。インストール前に閉じますか？（推奨）
ja.AppRunningBlock=OpenTune を閉じてからインストーラーを実行してください。
ja.DawRunningWarning=DAW（デジタルオーディオワークステーション）が実行中で、OpenTune VST3 プラグインが読み込まれている可能性があります。インストールを続行するために閉じますか？（推奨）
ja.DawRunningBlock=VST3 プラグインをインストールする前に DAW を閉じてください。
ru.AppRunningWarning=OpenTune в настоящее время работает. Закрыть его перед установкой? (Рекомендуется)
ru.AppRunningBlock=Пожалуйста, закройте OpenTune перед запуском установщика.
ru.DawRunningWarning=Обнаружена работающая DAW, в которой может быть загружен плагин OpenTune VST3. Закрыть её для продолжения установки? (Рекомендуется)
ru.DawRunningBlock=Пожалуйста, закройте DAW перед установкой плагина VST3.
es.AppRunningWarning=OpenTune está en ejecución. ¿Cerrarlo antes de instalar? (Recomendado)
es.AppRunningBlock=Por favor, cierre OpenTune antes de ejecutar el instalador.
es.DawRunningWarning=Se detectó un DAW en ejecución que puede tener cargado el plugin OpenTune VST3. ¿Cerrarlo para continuar con la instalación? (Recomendado)
es.DawRunningBlock=Por favor, cierre su DAW antes de instalar el plugin VST3.
en.SelectComponents=Select Components
en.SelectComponentsDesc=Please select which components you wish to install.
en.SelectComponentsDesc2=Select the components to install:
en.VST3DirTitle=VST3 Plugin Install Location
en.VST3DirDesc=Where should the VST3 plugin be installed?
en.VST3DirLabel=Select the folder where the VST3 plugin will be installed, then click Next.
; 注：en.WelcomeLabel1/2 已移至 [Messages] 段（内置消息只能在 [Messages] 覆盖）。

; =====================================================
; Chinese Simplified
; =====================================================
zh.TypeFull=完整安装（独立版 + VST3 插件）
zh.TypeStandalone=仅安装独立版
zh.TypeVST3=仅安装 VST3 插件
zh.TypeCustom=自定义安装
zh.CompStandalone=OpenTune 独立应用程序
zh.CompVST3=OpenTune VST3 插件（支持 ARA2）
zh.CompVST3Desc=安装 VST3 插件以在 DAW（如 FL Studio、Cubase、REAPER）中使用
zh.TaskDesktopIcon=创建桌面快捷方式
zh.TaskGroup=附加快捷方式：
zh.LaunchProgram=启动 OpenTune
zh.IconComment=AI 智能人声修音软件
zh.IconUserGuide=用户指南
zh.IconUninstall=卸载 OpenTune
zh.VST3SystemWide=为所有用户安装（推荐）
zh.VST3PerUser=仅为当前用户安装
zh.ErrorStandaloneMissing=安装后未找到 OpenTune 可执行文件，安装可能已损坏。
zh.InstallValidationFailed=OpenTune 已安装，但安装结果校验失败。请检查列出的文件后重新运行安装程序。
zh.VCRedistInstallFailed=Microsoft Visual C++ x64 运行库安装失败。由于 OpenTune 的音频分析运行时不可用，安装无法继续。
zh.SelectComponents=选择组件
zh.SelectComponentsDesc=请选择您要安装的组件。
zh.SelectComponentsDesc2=选择要安装的组件：
zh.VST3DirTitle=VST3 插件安装位置
zh.VST3DirDesc=VST3 插件应该安装在哪个目录？
zh.VST3DirLabel=选择 VST3 插件的安装目录，然后点击"下一步"。
; 注：zh.WelcomeLabel1/2 已移至 [Messages] 段（内置消息只能在 [Messages] 覆盖）。

; =====================================================
; Japanese
; =====================================================
ja.TypeFull=フルインストール（スタンドアロン + VST3 プラグイン）
ja.TypeStandalone=スタンドアロンのみ
ja.TypeVST3=VST3 プラグインのみ
ja.TypeCustom=カスタムインストール
ja.CompStandalone=OpenTune スタンドアロンアプリケーション
ja.CompVST3=OpenTune VST3 プラグイン（ARA2 対応）
ja.CompVST3Desc=VST3 プラグインをインストールして DAW（FL Studio、Cubase、REAPER など）で使用
ja.TaskDesktopIcon=デスクトップにショートカットを作成
ja.TaskGroup=追加のショートカット：
ja.LaunchProgram=OpenTune を起動
ja.IconComment=AI ボーカルピッチ補正ソフトウェア
ja.IconUserGuide=ユーザーガイド
ja.IconUninstall=OpenTune をアンインストール
ja.VST3SystemWide=すべてのユーザーにインストール（推奨）
ja.VST3PerUser=現在のユーザーのみにインストール
ja.ErrorStandaloneMissing=インストール後に OpenTune の実行ファイルが見つかりません。インストールが破損している可能性があります。
ja.InstallValidationFailed=OpenTune はインストールされましたが、インストール結果の検証に失敗しました。表示されたファイルを確認して、インストーラーを再実行してください。
ja.VCRedistInstallFailed=Microsoft Visual C++ x64 再頒布可能パッケージをインストールできませんでした。OpenTune のオーディオ解析ランタイムを利用できないため、インストールを続行できません。
ja.SelectComponents=コンポーネントを選択
ja.SelectComponentsDesc=インストールするコンポーネントを選択してください。
ja.SelectComponentsDesc2=インストールするコンポーネント：
ja.VST3DirTitle=VST3 プラグインのインストール場所
ja.VST3DirDesc=VST3 プラグインをどこにインストールしますか？
ja.VST3DirLabel=VST3 プラグインのインストール先フォルダを選択し、[次へ] をクリックしてください。
; 注：ja.WelcomeLabel1/2 已移至 [Messages] 段（内置消息只能在 [Messages] 覆盖）。

; =====================================================
; Russian
; =====================================================
ru.TypeFull=Полная установка (Standalone + VST3 плагин)
ru.TypeStandalone=Только Standalone
ru.TypeVST3=Только VST3 плагин
ru.TypeCustom=Выборочная установка
ru.CompStandalone=Приложение OpenTune Standalone
ru.CompVST3=Плагин OpenTune VST3 (ARA2)
ru.CompVST3Desc=Установить VST3 плагин для использования в DAW (FL Studio, Cubase, REAPER и др.)
ru.TaskDesktopIcon=Создать ярлык на рабочем столе
ru.TaskGroup=Дополнительные ярлыки:
ru.LaunchProgram=Запустить OpenTune
ru.IconComment=AI-коррекция высоты тона вокала
ru.IconUserGuide=Руководство пользователя
ru.IconUninstall=Удалить OpenTune
ru.VST3SystemWide=Установить для всех пользователей (рекомендуется)
ru.VST3PerUser=Установить только для текущего пользователя
ru.ErrorStandaloneMissing=Исполняемый файл OpenTune не найден после установки. Возможно, установка повреждена.
ru.InstallValidationFailed=OpenTune установлен, но проверка установки завершилась ошибкой. Проверьте перечисленные файлы и запустите установщик снова.
ru.VCRedistInstallFailed=Не удалось установить распространяемый пакет Microsoft Visual C++ x64. Установка OpenTune не может продолжиться, поскольку недоступна среда анализа аудио.
ru.SelectComponents=Выбор компонентов
ru.SelectComponentsDesc=Пожалуйста, выберите компоненты для установки.
ru.SelectComponentsDesc2=Выберите компоненты для установки:
ru.VST3DirTitle=Место установки плагина VST3
ru.VST3DirDesc=Куда установить плагин VST3?
ru.VST3DirLabel=Выберите папку для установки плагина VST3, затем нажмите «Далее».
; 注：ru.WelcomeLabel1/2 已移至 [Messages] 段（内置消息只能在 [Messages] 覆盖）。

; =====================================================
; Spanish
; =====================================================
es.TypeFull=Instalación completa (Standalone + plugin VST3)
es.TypeStandalone=Solo aplicación Standalone
es.TypeVST3=Solo plugin VST3
es.TypeCustom=Instalación personalizada
es.CompStandalone=Aplicación OpenTune Standalone
es.CompVST3=Plugin VST3 OpenTune (ARA2)
es.CompVST3Desc=Instalar el plugin VST3 para usar en DAWs (FL Studio, Cubase, REAPER, etc.)
es.TaskDesktopIcon=Crear acceso directo en el escritorio
es.TaskGroup=Accesos directos adicionales:
es.LaunchProgram=Iniciar OpenTune
es.IconComment=Corrección de tono vocal con IA
es.IconUserGuide=Guía de usuario
es.IconUninstall=Desinstalar OpenTune
es.VST3SystemWide=Instalar para todos los usuarios (recomendado)
es.VST3PerUser=Instalar solo para el usuario actual
es.ErrorStandaloneMissing=El ejecutable de OpenTune no se encontró después de la instalación. La instalación puede estar dañada.
es.InstallValidationFailed=OpenTune se instaló, pero la verificación de la instalación falló. Revise los archivos indicados y vuelva a ejecutar el instalador.
es.VCRedistInstallFailed=No se pudo instalar Microsoft Visual C++ Redistributable x64. OpenTune no puede continuar porque el runtime de análisis de audio no está disponible.
es.SelectComponents=Seleccionar componentes
es.SelectComponentsDesc=Por favor seleccione los componentes que desea instalar.
es.SelectComponentsDesc2=Seleccione los componentes a instalar:
es.VST3DirTitle=Ubicación de instalación del plugin VST3
es.VST3DirDesc=¿Dónde debería instalarse el plugin VST3?
es.VST3DirLabel=Seleccione la carpeta donde se instalará el plugin VST3, luego haga clic en Siguiente.
; 注：es.WelcomeLabel1/2 已移至 [Messages] 段（内置消息只能在 [Messages] 覆盖）。
