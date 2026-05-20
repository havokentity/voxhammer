@echo off
REM SPDX-License-Identifier: MIT
REM Copyright (c) 2026 Rajesh D'Monte
REM No-CLion fallback: configure + build the windows-release (clang-cl) preset
REM from a plain shell. Locates VS via vswhere, enters its dev environment, puts
REM the VS-bundled CMake/Ninja + clang-cl on PATH (vcvars64 alone doesn't), and
REM uses VCPKG_ROOT (or %USERPROFILE%\vcpkg). Pass --run to launch the engine
REM afterwards:  scripts\build_win_release.bat --run YOURPASSWORD
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [build_release] vswhere.exe not found. Install Visual Studio + the C++ workload.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    -property installationPath`) do set "VSDIR=%%i"

if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [build_release] vcvars64.bat not found under %VSDIR%
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

REM vcvars64 sets up cl/clang headers+libs but not the VS CMake/Ninja or the
REM LLVM bin -- prepend them so `cmake`, `ninja`, and `clang-cl` resolve.
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"

if not defined VCPKG_ROOT (
    if exist "%USERPROFILE%\vcpkg\vcpkg.exe" (
        set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
    ) else (
        echo [build_release] VCPKG_ROOT is not set and %USERPROFILE%\vcpkg not found.
        echo                 Run scripts\bootstrap_win.bat once, or set VCPKG_ROOT.
        exit /b 1
    )
)
echo [build_release] VS=%VSDIR%
echo [build_release] VCPKG_ROOT=%VCPKG_ROOT%

cmake --preset windows-release
if errorlevel 1 exit /b %errorlevel%
cmake --build build\windows-release --parallel
if errorlevel 1 exit /b %errorlevel%

echo [build_release] OK -> build\windows-release\game\voxhammer.exe
if /i "%~1"=="--run" (
    echo [build_release] launching with the supplied password...
    "build\windows-release\game\voxhammer.exe" --remote-password=%~2
)
