@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer was not found. Install Visual Studio 2022 C++ Build Tools.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -all -products * -property installationPath`) do if not defined VSROOT if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VSROOT=%%i"
if not defined VSROOT (
    echo Visual Studio C++ Build Tools with vcvars64.bat were not found.
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-game.ps1" %*
exit /b %errorlevel%
