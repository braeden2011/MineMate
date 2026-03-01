@echo off
rem Configure the project with the Visual Studio 17 2022 generator.
rem Run from the repo root.  Follow with build_all.bat.

set CMAKE="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set VCPKG_TOOLCHAIN=C:\vcpkg\scripts\buildsystems\vcpkg.cmake

cd /d %~dp0

%CMAKE% -B build\release -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_TOOLCHAIN%

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [FAILED] cmake configure failed.
    exit /b %ERRORLEVEL%
)
echo.
echo [OK] Configure complete.  Run build_all.bat to build.
