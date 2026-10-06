[Setup]
AppId={{8F1C7A52-3B6E-4D0A-9C41-5A2E7D91B3F6}
AppName=PikViewer
AppVersion=1.0.0
AppPublisher=PikViewer
DefaultDirName={autopf}\PikViewer
DefaultGroupName=PikViewer
DisableProgramGroupPage=yes
OutputDir=Salida
OutputBaseFilename=PikViewer-Setup
SetupIconFile=Assets\PikViewer.ico
UninstallDisplayName=PikViewer
UninstallDisplayIcon={app}\PikViewer.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
ChangesAssociations=yes
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableWelcomePage=yes
MinVersion=10.0.17763

[Languages]
#if FileExists(CompilerPath + "Languages\Spanish.isl")
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
#else
Name: "english"; MessagesFile: "compiler:Default.isl"
#endif

[Tasks]
Name: "assoc"; Description: "Registrar PikViewer como visor de imágenes (después podrás elegirlo como predeterminado)"
Name: "desktopicon"; Description: "Crear un acceso directo en el escritorio"; Flags: unchecked

[Files]
Source: "Salida\PikViewer.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\PikViewer"; Filename: "{app}\PikViewer.exe"
Name: "{autodesktop}\PikViewer"; Filename: "{app}\PikViewer.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PikViewer.exe"; Description: "Abrir PikViewer"; Flags: nowait postinstall skipifsilent
Filename: "ms-settings:defaultapps"; Description: "Abrir Ajustes para elegir PikViewer como visor predeterminado"; Flags: shellexec postinstall skipifsilent unchecked

[Registry]
; Ajustes de la app (se borran al desinstalar)
Root: HKCU; Subkey: "Software\PikViewer"; Flags: uninsdeletekey
; Tipos de archivo: cada extensión tiene su propio ProgID para que el
; Explorador de archivos muestre el tipo real (PNG, JPEG, JXR, etc.) en vez de
; "Imagen (PikViewer)". PikViewer sigue apareciendo como aplicación asociada.
Root: HKA; Subkey: "Software\Classes\PikViewer.PNG"; ValueType: string; ValueData: "PNG"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.PNG\DefaultIcon"; ValueType: string; ValueData: "{app}\PikViewer.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.PNG\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.JPEG"; ValueType: string; ValueData: "JPEG"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.JPEG\DefaultIcon"; ValueType: string; ValueData: "{app}\PikViewer.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.JPEG\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.JXR"; ValueType: string; ValueData: "JPEG XR"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.JXR\DefaultIcon"; ValueType: string; ValueData: "{app}\PikViewer.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.JXR\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.WDP"; ValueType: string; ValueData: "WDP"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.WDP\DefaultIcon"; ValueType: string; ValueData: "{app}\PikViewer.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.WDP\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.BMP"; ValueType: string; ValueData: "BMP"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.BMP\DefaultIcon"; ValueType: string; ValueData: "{app}\PikViewer.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.BMP\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.TIFF"; ValueType: string; ValueData: "TIFF"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.TIFF\DefaultIcon"; ValueType: string; ValueData: "{app}\PikViewer.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.TIFF\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.GIF"; ValueType: string; ValueData: "GIF"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.GIF\DefaultIcon"; ValueType: string; ValueData: "{app}\PikViewer.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.GIF\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.HEIF"; ValueType: string; ValueData: "HEIF"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.HEIF\DefaultIcon"; ValueType: string; ValueData: "{app}\PikViewer.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.HEIF\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.AVIF"; ValueType: string; ValueData: "AVIF"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.AVIF\DefaultIcon"; ValueType: string; ValueData: "{app}\PikViewer.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.AVIF\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.WEBP"; ValueType: string; ValueData: "WebP"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.WEBP\DefaultIcon"; ValueType: string; ValueData: "{app}\PikViewer.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\PikViewer.WEBP\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc

; Aplicacion
Root: HKA; Subkey: "Software\Classes\Applications\PikViewer.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "PikViewer"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\Applications\PikViewer.exe\shell\open\command"; ValueType: string; ValueData: """{app}\PikViewer.exe"" ""%1"""; Tasks: assoc
; Capabilities (aparece en Ajustes > Aplicaciones predeterminadas)
Root: HKA; Subkey: "Software\PikViewer\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "PikViewer"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Visor y editor minimalista de imágenes HDR"; Tasks: assoc
Root: HKA; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "PikViewer"; ValueData: "Software\PikViewer\Capabilities"; Flags: uninsdeletevalue; Tasks: assoc


Root: HKA; Subkey: "Software\Classes\.jxr\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.JXR"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jxr"; ValueData: "PikViewer.JXR"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.wdp\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.WDP"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wdp"; ValueData: "PikViewer.WDP"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.png\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.PNG"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".png"; ValueData: "PikViewer.PNG"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.jpg\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.JPEG"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpg"; ValueData: "PikViewer.JPEG"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.jpeg\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.JPEG"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpeg"; ValueData: "PikViewer.JPEG"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.jfif\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.JPEG"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jfif"; ValueData: "PikViewer.JPEG"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.bmp\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.BMP"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".bmp"; ValueData: "PikViewer.BMP"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.tif\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.TIFF"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tif"; ValueData: "PikViewer.TIFF"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.tiff\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.TIFF"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tiff"; ValueData: "PikViewer.TIFF"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.gif\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.GIF"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".gif"; ValueData: "PikViewer.GIF"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.heic\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.HEIF"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heic"; ValueData: "PikViewer.HEIF"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.heif\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.HEIF"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heif"; ValueData: "PikViewer.HEIF"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.avif\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.AVIF"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avif"; ValueData: "PikViewer.AVIF"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.webp\OpenWithProgids"; ValueType: none; ValueName: "PikViewer.WEBP"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\PikViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webp"; ValueData: "PikViewer.WEBP"; Tasks: assoc

[Code]
var
  MaintenancePage: TInputOptionWizardPage;
  ExistingInstall: Boolean;

function IsPikViewerInstalled(): Boolean;
var
  UninstallString: String;
begin
  Result := False;
  if RegQueryStringValue(HKLM64, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{8F1C7A52-3B6E-4D0A-9C41-5A2E7D91B3F6}_is1', 'UninstallString', UninstallString) then
    Result := True;
  if not Result then
    Result := FileExists(ExpandConstant('{autopf}\PikViewer\PikViewer.exe'));
end;

procedure MigrateLegacyAssociation(const Ext, NewProgID: String);
var
  CurrentProgID: String;
begin
  CurrentProgID := '';
  if RegQueryStringValue(HKA, 'Software\Classes\' + Ext, '', CurrentProgID) then
  begin
    if CompareText(CurrentProgID, 'PikViewer.Image') = 0 then
      RegWriteStringValue(HKA, 'Software\Classes\' + Ext, '', NewProgID);
  end;
end;

procedure CleanupLegacyAssociation;
begin
  MigrateLegacyAssociation('.png', 'PikViewer.PNG');
  MigrateLegacyAssociation('.jpg', 'PikViewer.JPEG');
  MigrateLegacyAssociation('.jpeg', 'PikViewer.JPEG');
  MigrateLegacyAssociation('.jfif', 'PikViewer.JPEG');
  MigrateLegacyAssociation('.jxr', 'PikViewer.JXR');
  MigrateLegacyAssociation('.wdp', 'PikViewer.WDP');
  MigrateLegacyAssociation('.bmp', 'PikViewer.BMP');
  MigrateLegacyAssociation('.tif', 'PikViewer.TIFF');
  MigrateLegacyAssociation('.tiff', 'PikViewer.TIFF');
  MigrateLegacyAssociation('.gif', 'PikViewer.GIF');
  MigrateLegacyAssociation('.heic', 'PikViewer.HEIF');
  MigrateLegacyAssociation('.heif', 'PikViewer.HEIF');
  MigrateLegacyAssociation('.avif', 'PikViewer.AVIF');
  MigrateLegacyAssociation('.webp', 'PikViewer.WEBP');
  RegDeleteKeyIncludingSubkeys(HKA, 'Software\Classes\PikViewer.Image');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    CleanupLegacyAssociation;
end;

procedure InitializeWizard;
begin
  ExistingInstall := IsPikViewerInstalled();

  MaintenancePage := CreateInputOptionPage(
    wpWelcome,
    'PikViewer ya está instalado',
    '¿Qué quieres hacer?',
    'Selecciona una opción y pulsa Siguiente.',
    True,
    False);
  MaintenancePage.Add('Reparar PikViewer (volver a instalar los archivos)');
  MaintenancePage.Add('Desinstalar PikViewer');
  MaintenancePage.SelectedValueIndex := 0;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = MaintenancePage.ID) and (not ExistingInstall);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ResultCode: Integer;
  Uninstaller: String;
begin
  Result := True;

  if (CurPageID = MaintenancePage.ID) and (MaintenancePage.SelectedValueIndex = 1) then
  begin
    Uninstaller := ExpandConstant('{autopf}\PikViewer\unins000.exe');
    if not FileExists(Uninstaller) then
      Uninstaller := ExpandConstant('{uninstallexe}');

    if Exec(Uninstaller, '', '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) then
    begin
      MsgBox('PikViewer se ha desinstalado.', mbInformation, MB_OK);
      WizardForm.Close;
    end
    else
      MsgBox('No se pudo iniciar la desinstalación.', mbError, MB_OK);

    Result := False;
  end;
end;

