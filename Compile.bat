@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if not exist Salida mkdir Salida
if exist Salida\PikViewer.exe del Salida\PikViewer.exe

echo.
echo [1/3] Compilando PikViewer.exe ...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS goto novs
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
rc /nologo PikViewer.rc
if errorlevel 1 goto fail
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE main.cpp PikViewer.res /Fe:PikViewer.exe /link /SUBSYSTEM:WINDOWS
if errorlevel 1 goto fail
move /y PikViewer.exe Salida\PikViewer.exe >nul
del main.obj PikViewer.res 2>nul

echo.
echo [2/3] Creando el instalador PikViewer-Setup.exe ...
call :findiscc
if defined ISCC goto haveiscc
echo Instalando Inno Setup (solo la primera vez) ...
winget install -e --id JRSoftware.InnoSetup --silent --accept-package-agreements --accept-source-agreements
call :findiscc
if not defined ISCC goto noinno
:haveiscc
"%ISCC%" /Qp PikViewer.iss
if errorlevel 1 goto fail
goto msix

:noinno
echo.
echo No se pudo instalar Inno Setup automaticamente. Instalalo desde https://jrsoftware.org/isdl.php
echo y vuelve a ejecutar este archivo. Se continua con el MSIX.

:msix
echo.
echo [3/3] Creando el paquete MSIX ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-msix.ps1"
if errorlevel 1 goto fail

echo.
echo Listo. Todo esta en la carpeta Salida:
dir /b Salida
explorer "%~dp0Salida"
pause
exit /b 0

:findiscc
set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
exit /b 0

:novs
echo No se encontro Visual Studio con las herramientas de C++.
echo Instala "Build Tools for Visual Studio" (carga de trabajo "Desarrollo para escritorio con C++")
echo y vuelve a hacer doble clic en este archivo.
pause
exit /b 1

:fail
echo.
echo Algo ha fallado. Copia el mensaje de arriba y pasamelo.
pause
exit /b 1
