@echo off
setlocal
cd /d "%~dp0"
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
echo Instalando PikViewer ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; Import-Certificate -FilePath '%~dp0PikViewer.cer' -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null; Get-AppxPackage PikViewer | Remove-AppxPackage -ErrorAction SilentlyContinue; Add-AppxPackage '%~dp0PikViewer.msix'"
if errorlevel 1 (
    echo.
    echo Algo ha fallado. Copia el mensaje de arriba.
    pause
    exit /b 1
)
echo.
echo Listo. PikViewer ya esta instalado (busca "PikViewer" en el menu Inicio).
pause
