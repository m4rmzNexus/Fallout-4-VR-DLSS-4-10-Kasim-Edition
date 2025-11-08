// F4SE VR DLSS4 Plugin
// Fallout 4 VR Upscaler with DLSS4 Multi Frame Generation support

#include <windows.h>
#include <stdio.h>
#include <string>
#include <shlobj.h>

#include "F4SEVR_Upscaler.h"
#include "f4se/PluginAPI.h"
#include "f4se_common/f4se_version.h"
#include "dlss_hooks.h"

// Plugin handle
static PluginHandle g_pluginHandle = kPluginHandle_Invalid;

// Version info
#define PLUGIN_VERSION_MAJOR 1
#define PLUGIN_VERSION_MINOR 0
#define PLUGIN_VERSION_BUILD 0
#define MAKE_VERSION(major, minor, build) (((major & 0xFF) << 24) | ((minor & 0xFF) << 16) | (build & 0xFFFF))

static constexpr UInt32 kMinRuntimeVersion = RUNTIME_VR_VERSION_1_2_72;

// External hook installer
extern "C" bool InstallDLSSHooks();

// Log function
static std::string GetDocumentsLogPath() {
    char docs[MAX_PATH] = {0};
    if (!SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYDOCUMENTS, NULL, 0, docs))) {
        return std::string("F4SEVR_DLSS.log");
    }
    std::string base = docs;
    const std::string pNoSpace = base + "\\My Games\\Fallout4VR\\F4SE\\Plugins\\F4SEVR_DLSS.log";
    const std::string pWithSpace = base + "\\My Games\\Fallout 4 VR\\F4SE\\Plugins\\F4SEVR_DLSS.log";
    if (GetFileAttributesA(pNoSpace.c_str()) != INVALID_FILE_ATTRIBUTES) return pNoSpace;
    if (GetFileAttributesA(pWithSpace.c_str()) != INVALID_FILE_ATTRIBUTES) return pWithSpace;
    const std::string dNo = base + "\\My Games\\Fallout4VR\\F4SE\\Plugins\\";
    const std::string dWs = base + "\\My Games\\Fallout 4 VR\\F4SE\\Plugins\\";
    if (GetFileAttributesA(dNo.c_str()) != INVALID_FILE_ATTRIBUTES) return pNoSpace;
    if (GetFileAttributesA(dWs.c_str()) != INVALID_FILE_ATTRIBUTES) return pWithSpace;
    return pNoSpace;
}

static void Log(const char* format, ...) {
    const std::string logPath = GetDocumentsLogPath();
    // Ensure directory
    std::string dir = logPath;
    size_t pos = dir.find_last_of("/\\");
    if (pos != std::string::npos) {
        dir = dir.substr(0, pos);
        SHCreateDirectoryExA(NULL, dir.c_str(), NULL);
    }

    FILE* log = fopen(logPath.c_str(), "a");
    if (log) {
        va_list args;
        va_start(args, format);
        vfprintf(log, format, args);
        va_end(args);
        fprintf(log, "\n");
        fclose(log);
    }
}

static bool IsPathUnder(const std::wstring& path, const std::wstring& root) {
    if (path.size() < root.size()) return false;
    std::wstring p = path;
    std::wstring r = root;
    for (auto& ch : p) ch = towlower(ch);
    for (auto& ch : r) ch = towlower(ch);
    return p.rfind(r, 0) == 0; // starts with
}

static std::wstring GetModulePath(HMODULE h) {
    wchar_t buf[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(h, buf, MAX_PATH);
    if (len == 0) return L"";
    return std::wstring(buf, buf + len);
}

static std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0) return L"";
    std::wstring p(buf, buf + len);
    size_t pos = p.find_last_of(L"/\\");
    if (pos != std::wstring::npos) p.erase(pos + 1);
    return p;
}

static bool DetectExternalOverlays() {
    bool detected = false;
    const std::wstring gameDir = GetExeDir();

    auto check = [&](const wchar_t* mod) {
        HMODULE h = GetModuleHandleW(mod);
        if (!h) return false;
        std::wstring mpath = GetModulePath(h);
        if (mpath.empty()) return false;
        // If module path resides under game directory, it's likely a proxy (Reshade/ENB)
        if (IsPathUnder(mpath, gameDir)) {
            std::string mp(mpath.begin(), mpath.end());
            Log("[SAFE] Third-party overlay likely detected: %S", mpath.c_str());
            return true;
        }
        // Also heuristics: filename contains enb/reshade
        std::wstring low = mpath; for (auto& c : low) c = towlower(c);
        if (low.find(L"reshade") != std::wstring::npos || low.find(L"enb") != std::wstring::npos) {
            Log("[SAFE] Third-party overlay detected by name: %S", mpath.c_str());
            return true;
        }
        return false;
    };

    if (check(L"dxgi.dll") || check(L"d3d11.dll") || check(L"ReShade64.dll") || check(L"enbseries.dll")) {
        detected = true;
    }

    if (detected) {
        Log("[SAFE] Enabling overlay compatibility (no WndProc hook; recommend ReShade before upscaling)");
        SetOverlaySafeMode(true);
    }
    return detected;
}

// F4SE Plugin API functions
extern "C" {
    
__declspec(dllexport) bool F4SEPlugin_Query(const F4SEInterface* f4se, PluginInfo* info) {
    if (!f4se || !info) {
        return false;
    }

    // Clear log on startup
    const std::string logPath = GetDocumentsLogPath();
    // Ensure directory exists
    {
        std::string dir = logPath;
        size_t pos = dir.find_last_of("/\\");
        if (pos != std::string::npos) {
            dir = dir.substr(0, pos);
            SHCreateDirectoryExA(NULL, dir.c_str(), NULL);
        }
    }
    FILE* log = fopen(logPath.c_str(), "w");
    if (log) {
        fprintf(log, "==============================================\n");
        fprintf(log, "F4SEVR DLSS4 Plugin v%d.%d.%d\n", 
                PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_BUILD);
        fprintf(log, "DLSS4 Multi Frame Generation for Fallout 4 VR\n");
        fprintf(log, "==============================================\n");
        fclose(log);
    }
    
    // Populate plugin info
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name = "F4SEVR_DLSS4";
    info->version = MAKE_VERSION(PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_BUILD);

    // Check if running in editor
    if (f4se->isEditor != 0) {
        Log("Plugin does not support the editor");
        return false;
    }
    
    // Version checks
    Log("F4SE Version: %08X", f4se->f4seVersion);
    Log("Runtime Version: %08X", f4se->runtimeVersion);

    if (f4se->runtimeVersion < kMinRuntimeVersion) {
        Log("ERROR: Runtime version %08X is older than required minimum %08X", f4se->runtimeVersion, kMinRuntimeVersion);
        return false;
    }
    
    // Check for VR
    HMODULE vrModule = GetModuleHandleA("openvr_api.dll");
    if (vrModule) {
        Log("VR Mode Detected - OpenVR API Present");
        F4SEVR_Upscaler::GetSingleton()->SetVRMode(true);
    } else {
        Log("Standard Mode - No VR detected");
        F4SEVR_Upscaler::GetSingleton()->SetVRMode(false);
    }
    
    Log("Plugin Query successful");
    return true;
}

static bool VerifyDependency(const std::string& path, const char* description) {
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        Log("%s found: %s", description, path.c_str());
        return true;
    }

    Log("WARNING: %s not found at %s", description, path.c_str());
    Log("         Please place the file under Fallout 4 VR\\Data\\F4SE\\Plugins.");
    return false;
}

static bool VerifyDLSSRuntimePresent() {
    // Prefer EXE root per Streamline guidance
    const std::wstring exeDirW = GetExeDir();
    std::string exeDir(exeDirW.begin(), exeDirW.end());
    const std::string exePath = exeDir + "nvngx_dlss.dll";
    const std::string pluginPath = std::string("Data\\F4SE\\Plugins\\nvngx_dlss.dll");

    bool found = false;
    if (GetFileAttributesA(exePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        Log("DLSS runtime (nvngx_dlss.dll) found at EXE root: %s", exePath.c_str());
        found = true;
    } else if (GetFileAttributesA(pluginPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        Log("DLSS runtime (nvngx_dlss.dll) found at Data\\F4SE\\Plugins (legacy): %s", pluginPath.c_str());
        Log("Note: Recommended location is next to fallout4vr.exe for Streamline.");
        found = true;
    } else {
        Log("WARNING: DLSS runtime (nvngx_dlss.dll) not found at EXE root (%s) nor at %s", exePath.c_str(), pluginPath.c_str());
        Log("         Recommended: place nvngx_dlss.dll next to fallout4vr.exe. If using MO2, place it in the game root (not VFS).");
    }

    return found;
}

__declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface* f4se) {
    if (!f4se) {
        Log("ERROR: F4SE interface missing during Load");
        return false;
    }

    Log("Plugin Load called");

    // Detect and guard for ENB/ReShade
    DetectExternalOverlays();

    if (f4se->GetPluginHandle) {
        g_pluginHandle = f4se->GetPluginHandle();
        Log("Plugin Handle: %u", g_pluginHandle);
    }
    
    // Install D3D11 hooks
    if (!InstallDLSSHooks()) {
        Log("ERROR: Failed to install D3D11 hooks");
        return false;
    }
    
    Log("D3D11 hooks installed");
    
    // Load upscaler settings
    F4SEVR_Upscaler::GetSingleton()->LoadSettings();
    Log("Settings loaded");
    
    // Check for required DLLs (prefer EXE root for DLSS)
    const bool hasDLSS = VerifyDLSSRuntimePresent();
    VerifyDependency("Data\\F4SE\\Plugins\\ffx_fsr2_api_x64.dll", "FSR2 runtime (optional)");
    VerifyDependency("Data\\F4SE\\Plugins\\libxess.dll", "XeSS runtime (optional)");

    if (!hasDLSS) {
        Log("DLSS features will remain disabled until the runtime DLL is installed.");
    }
    
    Log("Plugin loaded successfully");
    Log("==============================================");
    
    return true;
}

__declspec(dllexport) unsigned int F4SEPlugin_GetVersion() {
    return MAKE_VERSION(PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_BUILD);
}

__declspec(dllexport) const char* F4SEPlugin_GetName() {
    return "F4SEVR_DLSS4";
}

} // extern "C"

// DLL Entry Point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            // Cleanup
            F4SEVR_Upscaler::GetSingleton()->Shutdown();
            break;
    }
    return TRUE;
}
