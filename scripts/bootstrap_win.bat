@echo off
REM SPDX-License-Identifier: MIT
REM Copyright (c) 2026 Rajesh D'Monte
REM Configure + build windows-debug from scratch: locate VS via vswhere, enter
REM the dev environment, ensure vcpkg is available, then configure + build.
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [bootstrap] vswhere.exe not found. Install Visual Studio with the C++ workload.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    -property installationPath`) do set "VSDEV_DIR=%%i\VC\Auxiliary\Build"

if not exist "%VSDEV_DIR%\vcvars64.bat" (
    echo [bootstrap] vcvars64.bat missing under %VSDEV_DIR%
    exit /b 1
)
call "%VSDEV_DIR%\vcvars64.bat" >nul

REM Ensure vcpkg. Honor an existing VCPKG_ROOT; else clone+bootstrap to %USERPROFILE%\vcpkg.
if not defined VCPKG_ROOT (
    if exist "%USERPROFILE%\vcpkg\vcpkg.exe" (
        set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
    ) else (
        echo [bootstrap] Cloning vcpkg to %USERPROFILE%\vcpkg ...
        git clone https://github.com/microsoft/vcpkg "%USERPROFILE%\vcpkg" || exit /b 1
        call "%USERPROFILE%\vcpkg\bootstrap-vcpkg.bat" || exit /b 1
        set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
    )
)
echo [bootstrap] VCPKG_ROOT=%VCPKG_ROOT%

cmake --preset windows-debug || exit /b %errorlevel%
cmake --build build\windows-debug --parallel || exit /b %errorlevel%

echo [bootstrap] done. Run: build\windows-debug\game\voxhammer.exe --version
