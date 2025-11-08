@echo off
setlocal enableextensions enabledelayedexpansion
echo ========================================
echo F4SEVR DLSS4 - Visual Studio 2022 Build
echo ========================================
echo.

:: Setup Visual Studio environment
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Failed to setup Visual Studio environment
    exit /b 1
)
echo Environment configured for x64
echo.

:: Create output directories
set "OUTDIR=build"
if not exist obj mkdir obj
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

:: Default NGX path if not provided
if not defined NGX_SDK_PATH (
    if exist "%~dp0DLSS-310.4.0\include\nvsdk_ngx.h" (
        echo [INFO] Defaulting NGX_SDK_PATH to %~dp0DLSS-310.4.0
        set "NGX_SDK_PATH=%~dp0DLSS-310.4.0"
    )
)

set "CL_COMPILE_FLAGS=/c /O2 /MD /EHsc /std:c++17 /DWIN32 /D_WINDOWS /D_USRDLL /DNOMINMAX"
set "INCLUDE_SWITCHES=/I""src"" /I""include"" /I""common"" /I""third_party\imgui"" /I""third_party\imgui\backends"" /I""."""

if defined NGX_SDK_PATH (
    echo Using NGX_SDK_PATH=%NGX_SDK_PATH%
    set "INCLUDE_SWITCHES=%INCLUDE_SWITCHES% /I""%NGX_SDK_PATH%\include"""
    set "INCLUDE=%INCLUDE%;%NGX_SDK_PATH%\include"
) else (
    echo [WARN] NGX_SDK_PATH not set; ensure NGX headers are available.
)

:: Detect Streamline SDK and configure outside the detection block to avoid % expansion pitfalls
if exist "%~dp0streamline-sdk-v2.9.0\include\sl.h" set "SL_SDK_PATH=%~dp0streamline-sdk-v2.9.0"

if defined SL_SDK_PATH (
    echo Using SL_SDK_PATH=%SL_SDK_PATH%
    set "INCLUDE_SWITCHES=%INCLUDE_SWITCHES% /I""!SL_SDK_PATH!\include"""
    set "CL_COMPILE_FLAGS=%CL_COMPILE_FLAGS% /DUSE_STREAMLINE=1"
    set "INCLUDE=%INCLUDE%;!SL_SDK_PATH!\include"
    set "SL_LINK=/LIBPATH:"!SL_SDK_PATH!\lib\x64""
    set "SL_LIB_FULL=!SL_SDK_PATH!\lib\x64\sl.interposer.lib"
)

echo Final INCLUDE_SWITCHES: %INCLUDE_SWITCHES%
echo Compiling source files...
set "SL_INC="
if defined SL_SDK_PATH set "SL_INC=/I""%SL_SDK_PATH%\include"""

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\main.obj" "src\main.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\F4SEVR_Upscaler.obj" "src\F4SEVR_Upscaler.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\ImGui_Menu.obj" "src\ImGui_Menu.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\dlss_config.obj" "dlss_config.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\dlss_hooks.obj" "dlss_hooks.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\dlss_manager.obj" "dlss_manager.cpp"
if %ERRORLEVEL% NEQ 0 goto error

if exist "src\backends\SLBackend.cpp" (
    cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\SLBackend.obj" "src\backends\SLBackend.cpp"
    if %ERRORLEVEL% NEQ 0 goto error
)

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\imgui.obj" "third_party\imgui\imgui.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\imgui_draw.obj" "third_party\imgui\imgui_draw.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\imgui_tables.obj" "third_party\imgui\imgui_tables.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\imgui_widgets.obj" "third_party\imgui\imgui_widgets.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\imgui_demo.obj" "third_party\imgui\imgui_demo.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\imgui_impl_dx11.obj" "third_party\imgui\backends\imgui_impl_dx11.cpp"
if %ERRORLEVEL% NEQ 0 goto error

cl.exe %CL_COMPILE_FLAGS% %INCLUDE_SWITCHES% %SL_INC% /Fo"obj\imgui_impl_win32.obj" "third_party\imgui\backends\imgui_impl_win32.cpp"
if %ERRORLEVEL% NEQ 0 goto error

echo.
echo Linking F4SEVR_DLSS.dll...
echo SL link flags: %SL_LINK%  %SL_LIB_FULL%

set "_NGX_LIBPATH="
set "_NGX_LIB="
if defined NGX_SDK_PATH (
    set "_NGX_LIBPATH=/LIBPATH:""%NGX_SDK_PATH%\lib\Windows_x86_64\x64"""
    if exist "%NGX_SDK_PATH%\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib" (
        set "_NGX_LIB=nvsdk_ngx_d.lib"
        echo [INFO] Linking NGX stub ^(dynamic CRT^) from %NGX_SDK_PATH%\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib
    ) else if exist "%NGX_SDK_PATH%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" (
        set "_NGX_LIB=nvsdk_ngx_s.lib"
        echo [INFO] Linking NGX static stub from %NGX_SDK_PATH%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib
    ) else (
        echo [WARN] nvsdk_ngx_*.lib not found under %NGX_SDK_PATH%\lib\Windows_x86_64\x64
    )
)

rem Delay-load Streamline interposer to avoid hard runtime dependency at plugin load time
set "_DELAY=/DELAYLOAD:sl.interposer.dll Delayimp.lib"

link.exe /DLL /OUT:"%OUTDIR%\F4SEVR_DLSS.dll" /DEF:"exports.def" %_NGX_LIBPATH% %SL_LINK% %_DELAY% ^
    "obj\main.obj" ^
    "obj\F4SEVR_Upscaler.obj" ^
    "obj\ImGui_Menu.obj" ^
    "obj\dlss_config.obj" ^
    "obj\dlss_hooks.obj" ^
    "obj\dlss_manager.obj" ^
    "obj\SLBackend.obj" ^
    "obj\imgui.obj" ^
    "obj\imgui_draw.obj" ^
    "obj\imgui_tables.obj" ^
    "obj\imgui_widgets.obj" ^
    "obj\imgui_demo.obj" ^
    "obj\imgui_impl_dx11.obj" ^
    "obj\imgui_impl_win32.obj" ^
    user32.lib kernel32.lib d3d11.lib dxgi.lib uuid.lib d3dcompiler.lib dwmapi.lib gdi32.lib advapi32.lib shell32.lib ^
    %_NGX_LIB% "%SL_LIB_FULL%"
if %ERRORLEVEL% NEQ 0 goto error

echo.
echo ========================================
echo Build Successful!
echo Output: %OUTDIR%\F4SEVR_DLSS.dll
echo ========================================
echo.
echo Copying INI alongside the DLL...
copy /Y "F4SEVR_DLSS.ini" "%OUTDIR%\F4SEVR_DLSS.ini" >nul 2>&1
echo.

:: Packaging for MO2 (plugin) and Root (runtime)
set "DIST=dist"
set "DIST_MO2=%DIST%\MO2\Data\F4SE\Plugins"
set "DIST_ROOT=%DIST%\Root"
if not exist "%DIST_MO2%" mkdir "%DIST_MO2%"
if not exist "%DIST_ROOT%" mkdir "%DIST_ROOT%"
copy /Y "%OUTDIR%\F4SEVR_DLSS.dll" "%DIST_MO2%\F4SEVR_DLSS.dll" >nul 2>&1
copy /Y "F4SEVR_DLSS.ini" "%DIST_MO2%\F4SEVR_DLSS.ini" >nul 2>&1
set "SL_BIN=%SL_SDK_PATH%\bin\x64"
if defined SL_SDK_PATH (
    if exist "%SL_BIN%\sl.interposer.dll" copy /Y "%SL_BIN%\sl.interposer.dll" "%DIST_ROOT%\sl.interposer.dll" >nul 2>&1
    if exist "%SL_BIN%\sl.common.dll" copy /Y "%SL_BIN%\sl.common.dll" "%DIST_ROOT%\sl.common.dll" >nul 2>&1
    if exist "%SL_BIN%\sl.dlss.dll" copy /Y "%SL_BIN%\sl.dlss.dll" "%DIST_ROOT%\sl.dlss.dll" >nul 2>&1
    if exist "%SL_BIN%\sl.pcl.dll" copy /Y "%SL_BIN%\sl.pcl.dll" "%DIST_ROOT%\sl.pcl.dll" >nul 2>&1
    if exist "%SL_BIN%\sl.reflex.dll" copy /Y "%SL_BIN%\sl.reflex.dll" "%DIST_ROOT%\sl.reflex.dll" >nul 2>&1
    if exist "%SL_BIN%\nvngx_dlss.dll" copy /Y "%SL_BIN%\nvngx_dlss.dll" "%DIST_ROOT%\nvngx_dlss.dll" >nul 2>&1
    rem NOTE: Frame Generation is explicitly disabled for VR; do NOT package nvngx_dlssg.dll
    rem if exist "%SL_BIN%\nvngx_dlssd.dll" copy /Y "%SL_BIN%\nvngx_dlssd.dll" "%DIST_ROOT%\nvngx_dlssd.dll" >nul 2>&1
)
> "%DIST%\README.txt" echo F4SEVR_DLSS Packaging
>> "%DIST%\README.txt" echo ----------------------
>> "%DIST%\README.txt" echo MO2: Copy 'MO2\Data\F4SE\Plugins\F4SEVR_DLSS.dll' and '.ini' into your MO2 mod ^(under Data\F4SE\Plugins^).
>> "%DIST%\README.txt" echo Root: Copy next to fallout4vr.exe:
>> "%DIST%\README.txt" echo   - Root\sl.interposer.dll
>> "%DIST%\README.txt" echo   - Root\sl.common.dll
>> "%DIST%\README.txt" echo   - Root\sl.dlss.dll
>> "%DIST%\README.txt" echo   - Root\sl.pcl.dll (optional)
>> "%DIST%\README.txt" echo   - Root\sl.reflex.dll (optional)
>> "%DIST%\README.txt" echo   - Root\nvngx_dlss.dll (required)
>> "%DIST%\README.txt" echo   - Frame Generation is disabled: do NOT place nvngx_dlssg.dll
>> "%DIST%\README.txt" echo Logs: %%USERPROFILE%%\Documents\My Games\Fallout4VR\F4SE\Plugins\SL\sl.log and F4SE\Plugins\F4SEVR_DLSS.log
echo Packaging complete. See: %DIST%
echo.
echo To install:
echo 1. Copy F4SEVR_DLSS.dll to "Fallout 4 VR\Data\F4SE\Plugins\"
echo 2. Copy F4SEVR_DLSS.ini to "Fallout 4 VR\Data\F4SE\Plugins\"
echo.
exit /b 0

:error
echo.
echo [ERROR] Build failed!
pause
exit /b 1

endlocal
