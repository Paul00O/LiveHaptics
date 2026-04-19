@echo off
setlocal EnableDelayedExpansion

title Live Haptics - Build

taskkill /IM "LiveHaptics.exe" /F

:: ── Locate vcvars64 ──────────────────────────────────────────────────────────
set "VS_VCVARS="
for %%e in (Community Professional Enterprise BuildTools) do (
    set "_c=C:\Program Files\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat"
    if exist "!_c!" ( set "VS_VCVARS=!_c!" & goto :found_vs )
)
echo [ERROR] Visual Studio 2022 not found.
pause & exit /b 1
:found_vs

:: ── Locate cmake ─────────────────────────────────────────────────────────────
set "CMAKE="
for %%e in (Community Professional Enterprise BuildTools) do (
    set "_cm=C:\Program Files\Microsoft Visual Studio\2022\%%e\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if exist "!_cm!" ( set "CMAKE=!_cm!" & goto :found_cmake )
)
where cmake >nul 2>&1 && set "CMAKE=cmake" && goto :found_cmake
echo [ERROR] cmake.exe not found.
pause & exit /b 1
:found_cmake

:: ── cd to project root then use relative paths (avoids trailing-\ quoting bug)
call "%VS_VCVARS%" >nul 2>&1
cd /d "%~dp0"
if not exist build mkdir build

echo.
echo  [1/2] Configuring...
"%CMAKE%" -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 ( echo. & echo [ERROR] Configure failed. & pause & exit /b 1 )

echo.
echo  [2/2] Building...
"%CMAKE%" --build build --target LiveHaptics
if errorlevel 1 ( echo. & echo [ERROR] Build failed. & pause & exit /b 1 )

echo.
echo  ============================================
echo   OK  ^>  build\LiveHaptics.exe
echo  ============================================
echo.

start "" "build\LiveHaptics.exe"

pause
