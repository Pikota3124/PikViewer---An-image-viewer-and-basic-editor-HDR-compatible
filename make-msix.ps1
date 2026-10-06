# Crea Salida\PikViewer.msix (firmado) y Salida\PikViewer.cer. No instala nada en este equipo.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$out = Join-Path $PSScriptRoot 'Salida'
$exe = Join-Path $out 'PikViewer.exe'
if (-not (Test-Path $exe)) { throw "Falta Salida\PikViewer.exe. Ejecuta Construir.bat." }

$bin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter makeappx.exe |
       Where-Object FullName -like '*\x64\*' | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $bin) { throw "No encuentro makeappx.exe. Instala el Windows SDK (viene con Visual Studio)." }
$makeappx = $bin.FullName
$signtool = Join-Path $bin.DirectoryName 'signtool.exe'

# Carpeta temporal de empaquetado
$stage = Join-Path $env:TEMP 'PikViewer-pkg'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item $stage -ItemType Directory | Out-Null
Copy-Item $exe, .\AppxManifest.xml $stage
Copy-Item .\Assets (Join-Path $stage 'Assets') -Recurse

$msix = Join-Path $out 'PikViewer.msix'
& $makeappx pack /d $stage /p $msix /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makeappx fallo." }
Remove-Item $stage -Recurse -Force

# Certificado autofirmado, guardado solo en TU almacen de certificados (no se exporta ninguna clave privada)
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq 'CN=PikViewer' | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=PikViewer' -KeyUsage DigitalSignature `
        -FriendlyName 'PikViewer' -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(10) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
}
& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /s My $msix | Out-Null
if ($LASTEXITCODE -ne 0) { throw "signtool fallo al firmar el MSIX." }

# Parte publica del certificado + script para quien quiera instalar el MSIX
Export-Certificate -Cert $cert -FilePath (Join-Path $out 'PikViewer.cer') | Out-Null
Copy-Item .\Instalar-MSIX.bat $out -Force
Write-Host "MSIX creado: $msix"
