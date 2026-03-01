@echo off
rem Build Release configuration.  Run build_configure.bat first.

set CMAKE="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

cd /d %~dp0

%CMAKE% --build build\release --config Release

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [FAILED] Build failed.
    exit /b %ERRORLEVEL%
)
echo.
echo [OK] Build complete.  Run: .\build\release\Release\TerrainViewer.exe
