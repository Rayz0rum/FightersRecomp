@echo off
rem Regenerates the path tracing shader bytecode headers (DXIL, shader model
rem 6.5 for inline ray queries). Needs dxc.exe (and dxil.dll next to it for
rem signing) from the Windows SDK or the DirectX Shader Compiler releases on
rem PATH, or DXC set to its path.
setlocal
if "%DXC%"=="" set DXC=dxc.exe
set OUT=%~dp0..\bytecode\d3d12_6_5
if not exist "%OUT%" mkdir "%OUT%"
for %%S in (pt_convert pt_primary pt_lighting pt_temporal pt_denoise pt_resolve pt_bloom pt_composite) do (
  "%DXC%" -nologo -T cs_6_5 -E main -O3 -Qstrip_debug -Qstrip_reflect ^
      -Vn %%S_cs -Fh "%OUT%\%%S_cs.h" "%~dp0%%S.cs.hlsl" || exit /b 1
)
