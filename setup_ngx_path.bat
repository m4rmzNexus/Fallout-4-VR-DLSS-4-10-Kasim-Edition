@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "NGX_FOLDER=DLSS-310.4.0"
set "TARGET_PATH=%SCRIPT_DIR%%NGX_FOLDER%"

if not exist "%TARGET_PATH%" (
    echo [ERROR] Expected NGX SDK directory not found at "%TARGET_PATH%".
    echo Place the DLSS-310.4.0 package next to this script or edit NGX_FOLDER accordingly.
    exit /b 1
)

REM Persist NGX_SDK_PATH for the current user
setx NGX_SDK_PATH "%TARGET_PATH%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to set NGX_SDK_PATH environment variable.
    exit /b 1
)

echo NGX_SDK_PATH set to "%TARGET_PATH%".
echo Restart open terminals or IDEs to pick up the new value.
exit /b 0
