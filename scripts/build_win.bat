@echo off
REM SPDX-License-Identifier: MIT
REM Copyright (c) 2026 Rajesh D'Monte
REM Incremental build of an already-configured windows-debug tree.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    -property installationPath`) do set "VSDEV_DIR=%%i\VC\Auxiliary\Build"
call "%VSDEV_DIR%\vcvars64.bat" >nul
cmake --build build\windows-debug --parallel
