@echo off
rem Regenerates the immediate drawer's effect shader bytecode headers (DXBC,
rem shader model 5.1). Needs fxc.exe from the Windows SDK on PATH, or FXC set
rem to its path.
setlocal
if "%FXC%"=="" set FXC=fxc.exe
set OUT=%~dp0bytecode\d3d12_5_1
"%FXC%" /nologo /T vs_5_1 /E main /O3 /Qstrip_debug /Qstrip_reflect /Vn immediate_effect_vs /Fh "%OUT%\immediate_effect_vs.h" "%~dp0immediate_effect.vs.hlsl" || exit /b 1
"%FXC%" /nologo /T ps_5_1 /E main /O3 /Qstrip_debug /Qstrip_reflect /Vn immediate_effect_ps /Fh "%OUT%\immediate_effect_ps.h" "%~dp0immediate_effect.ps.hlsl" || exit /b 1
