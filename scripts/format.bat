@echo off
REM SPDX-License-Identifier: MIT
REM Copyright (c) 2026 Rajesh D'Monte
REM Apply clang-format in place across the C++ tree.
setlocal enabledelayedexpansion
for /r "%~dp0..\engine" %%f in (*.cpp *.h) do clang-format -i "%%f"
for /r "%~dp0..\game"   %%f in (*.cpp *.h) do clang-format -i "%%f"
for /r "%~dp0..\tools"  %%f in (*.cpp *.h) do clang-format -i "%%f"
for /r "%~dp0..\tests"  %%f in (*.cpp *.h) do clang-format -i "%%f"
echo [format] done.
