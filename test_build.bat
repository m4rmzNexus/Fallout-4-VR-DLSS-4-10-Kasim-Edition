@echo off
echo ==============================================
echo F4SEVR DLSS Plugin - Test Build Script
echo ==============================================
echo.

REM Check if Visual Studio 2022 is available
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x64
) else (
    echo ERROR: Visual Studio 2022 not found!
    pause
    exit /b 1
)

echo Building with MSBuild...
msbuild F4SEVR_DLSS.vcxproj /p:Configuration=Release /p:Platform=x64

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ERROR: Build failed!
    pause
    exit /b 1
)

echo.
echo Build completed successfully!
echo Output file: build\F4SEVR_DLSS.dll
echo.
echo To install:
echo 1. Copy build\F4SEVR_DLSS.dll to "Fallout 4 VR\Data\F4SE\Plugins\"
echo 2. Copy build\F4SEVR_DLSS.ini to "Fallout 4 VR\Data\F4SE\Plugins\"
echo 3. Ensure nvngx_dlss.dll is in the game root directory
echo.
pause
