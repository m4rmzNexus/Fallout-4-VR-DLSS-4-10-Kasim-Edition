@echo off
echo ========================================
echo F4SEVR DLSS4 - Direct Compile
echo ========================================
echo.

:: Set compiler path
set COMPILER="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe"

:: Check if compiler exists
if not exist %COMPILER% (
    echo [ERROR] Visual Studio compiler not found
    echo Please adjust the compiler path in this script
    pause
    exit /b 1
)

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

:: Set library paths
set LIBPATHS=/LIBPATH:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\um\x64" /LIBPATH:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\ucrt\x64" /LIBPATH:"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\lib\x64"

:: Set libraries
set LIBS=user32.lib kernel32.lib d3d11.lib dxgi.lib uuid.lib d3dcompiler.lib dwmapi.lib gdi32.lib

:: Compile flags
set FLAGS=/LD /MD /O2 /EHsc /DWIN32 /D_WINDOWS /D_USRDLL /DNOMINMAX /DF4SEVR_DLSS_EXPORTS

:: Source files
set SOURCES=src\main.cpp src\F4SEVR_Upscaler.cpp src\ImGui_Menu.cpp dlss_config.cpp dlss_hooks.cpp dlss_manager.cpp third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\imgui_demo.cpp third_party\imgui\backends\imgui_impl_dx11.cpp third_party\imgui\backends\imgui_impl_win32.cpp

echo Compiling F4SEVR_DLSS.dll...
echo.

:: Compile and link
%COMPILER% %FLAGS% %INCLUDES% %SOURCES% /Fe:F4SEVR_DLSS.dll /link %LIBPATHS% %LIBS% /DEF:exports.def

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Build failed!
    pause
    exit /b 1
)

echo.
echo ========================================
echo Build Successful!
echo Output: F4SEVR_DLSS.dll
echo ========================================
echo.
pause
exit /b 0


