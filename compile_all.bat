@echo off
setlocal enableextensions enabledelayedexpansion
echo ========================================
echo F4SEVR DLSS4 - Direct Compile
echo ========================================
echo.

:: Ensure Visual Studio build environment is initialized
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if exist "%VCVARS%" (
    call "%VCVARS%" x64 >nul
) else (
    echo [ERROR] Unable to find vcvarsall.bat at "%VCVARS%"
    echo Please update compile_all.bat with the correct Visual Studio path.
    exit /b 1
)

:: Set compiler path (after vcvars to keep toolset consistent)
set COMPILER="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe"

:: Check if compiler exists
if not exist %COMPILER% (
    echo [ERROR] Visual Studio compiler not found at %COMPILER%
    echo Please adjust the compiler path in this script
    exit /b 1
)

:: Allow non-interactive runs to skip pause prompts
if "%DLSS_NOPAUSE%"=="" (
    set "DLSS_NOPAUSE=0"
)
set "LIBS_EXTRA="
set "NGX_LIB_DIR="

:: Set include paths
if not defined NGX_SDK_PATH (
    if exist "%~dp0DLSS-310.4.0\include\nvsdk_ngx.h" (
        echo [INFO] Defaulting NGX_SDK_PATH to %~dp0DLSS-310.4.0
        set "NGX_SDK_PATH=%~dp0DLSS-310.4.0"
    )
)
set INCLUDES=/I"src" /I"include" /I"common" /I"third_party\imgui" /I"third_party\imgui\backends" /I"." /I"C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\um" /I"C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\shared" /I"C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\ucrt"
if defined NGX_SDK_PATH (
    echo Using NGX_SDK_PATH=%NGX_SDK_PATH%
    set INCLUDES=%INCLUDES% /I"%NGX_SDK_PATH%\include"
) else (
    echo [WARN] NGX_SDK_PATH environment variable not set. Ensure NVIDIA NGX SDK headers are available in the include path.
)

if not defined SL_SDK_PATH (
    if exist "%~dp0streamline-sdk-v2.9.0\include\sl.h" (
        echo [INFO] Defaulting SL_SDK_PATH to %~dp0streamline-sdk-v2.9.0
        set "SL_SDK_PATH=%~dp0streamline-sdk-v2.9.0"
    )
)
if defined SL_SDK_PATH (
    echo Using SL_SDK_PATH=%SL_SDK_PATH%
    set INCLUDES=%INCLUDES% /I"%SL_SDK_PATH%\include"
) else (
    echo [WARN] SL_SDK_PATH environment variable not set. Streamline backend will be disabled.
)

:: Set library paths
set LIBPATHS=/LIBPATH:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\um\x64" /LIBPATH:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\ucrt\x64" /LIBPATH:"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\lib\x64"
if defined SL_SDK_PATH (
    set LIBPATHS=%LIBPATHS% /LIBPATH:"%SL_SDK_PATH%\lib\x64"
)
if defined NGX_SDK_PATH (
    set "NGX_LIB_DIR=%NGX_SDK_PATH%\lib\Windows_x86_64\x64"
)
if defined NGX_LIB_DIR (
    if exist "%NGX_LIB_DIR%" (
        set LIBPATHS=%LIBPATHS% /LIBPATH:"%NGX_LIB_DIR%"
        if exist "%NGX_LIB_DIR%\nvsdk_ngx_d.lib" (
            echo [INFO] Linking NGX dynamic stub from %NGX_LIB_DIR%\nvsdk_ngx_d.lib
            set LIBS_EXTRA=nvsdk_ngx_d.lib
        ) else if exist "%NGX_LIB_DIR%\nvsdk_ngx_s.lib" (
            echo [INFO] Linking NGX static stub from %NGX_LIB_DIR%\nvsdk_ngx_s.lib
            set LIBS_EXTRA=nvsdk_ngx_s.lib
        ) else (
            echo [WARN] nvsdk_ngx_*.lib not found under %NGX_LIB_DIR%
        )
    ) else (
        echo [WARN] NGX lib directory missing: %NGX_LIB_DIR%
    )
)

:: Set libraries
set LIBS=user32.lib kernel32.lib d3d11.lib dxgi.lib uuid.lib d3dcompiler.lib dwmapi.lib gdi32.lib advapi32.lib shell32.lib crypt32.lib wintrust.lib
if defined SL_SDK_PATH (
    if exist "%SL_SDK_PATH%\lib\x64\sl.interposer.lib" (
        set LIBS=%LIBS% sl.interposer.lib
    ) else (
        echo [WARN] sl.interposer.lib not found in %SL_SDK_PATH%\lib\x64. Streamline libs will not be linked.
    )
)
if defined LIBS_EXTRA (
    set LIBS=%LIBS% %LIBS_EXTRA%
)

:: Compile flags
set FLAGS=/LD /MD /O2 /EHsc /std:c++17 /DWIN32 /D_WINDOWS /D_USRDLL /DNOMINMAX /DF4SEVR_DLSS_EXPORTS
if defined SL_SDK_PATH (
    set FLAGS=%FLAGS% /DUSE_STREAMLINE=1
)

:: Source files
set SOURCES=src\main.cpp src\F4SEVR_Upscaler.cpp src\ImGui_Menu.cpp dlss_config.cpp dlss_hooks.cpp dlss_manager.cpp src\backends\SLBackend.cpp third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\imgui_demo.cpp third_party\imgui\backends\imgui_impl_dx11.cpp third_party\imgui\backends\imgui_impl_win32.cpp

echo Compiling F4SEVR_DLSS.dll...
echo.

:: Compile and link
%COMPILER% %FLAGS% %INCLUDES% %SOURCES% /Fe:F4SEVR_DLSS.dll /link %LIBPATHS% %LIBS% /DEF:exports.def

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Build failed!
    if not "%DLSS_NOPAUSE%"=="1" pause
    endlocal
    exit /b 1
)

echo.
echo ========================================
echo Build Successful!
echo Output: F4SEVR_DLSS.dll
echo ========================================
echo.
if not "%DLSS_NOPAUSE%"=="1" pause
endlocal
exit /b 0
