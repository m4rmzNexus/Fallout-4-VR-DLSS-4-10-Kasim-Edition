#include "dlss_config.h"
#include "common/IDebugLog.h"
#include <windows.h>
#include <shlobj.h>
#include <fstream>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

extern DLSSManager* g_dlssManager;

extern "C" void SyncImGuiMenuFromConfig();

namespace {
	std::string ToLower(std::string value) {
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	std::string NormalizeKey(std::string value) {
		value = ToLower(std::move(value));
		if (!value.empty() && value[0] == 'm') {
			value.erase(0, 1);
		}
		return value;
	}

	bool StringToBool(const std::string& value) {
		std::string lower = ToLower(value);
		return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
	}

	int ParseInt(const std::string& value) {
		try {
			if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
				return std::stoi(value, nullptr, 16);
			}
			return std::stoi(value, nullptr, 10);
		} catch (...) {
			return 0;
		}
	}

	float ParseFloat(const std::string& value) {
		try {
			return std::stof(value);
		} catch (...) {
			return 0.0f;
		}
	}

	template <typename T>
	T ClampValue(T value, T minValue, T maxValue) {
		return std::max(minValue, std::min(maxValue, value));
	}

	std::string FormatVirtualKey(int key) {
	std::ostringstream oss;
	oss << "0x" << std::uppercase << std::hex << key;
	return oss.str();
}

int NormalizeHotkeyValue(int value) {
	if (value <= 0xFF) {
		return value;
	}
	switch (value) {
		case 520: return VK_END;
		case 544: return VK_HOME;
		case 545: return VK_LEFT;
		case 546: return VK_UP;
		case 547: return VK_RIGHT;
		case 548: return VK_DOWN;
		case 549: return VK_PRIOR;
		case 550: return VK_NEXT;
		case 551: return VK_INSERT;
		case 552: return VK_DELETE;
		case 612: return VK_MULTIPLY;
		default: return value;
	}
}

}
DLSSConfig::DLSSConfig() {
    _MESSAGE("DLSSConfig constructor");
}

DLSSConfig::~DLSSConfig() {
}

std::string DLSSConfig::GetDocumentsConfigPath() {
    char docs[MAX_PATH] = {};
    if (!SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYDOCUMENTS, NULL, 0, docs))) {
        return std::string("F4SEVR_DLSS.ini");
    }
    // Support both folder variants used by different FO4VR installs
    std::string base = docs;
    const std::string pathNoSpace = base + "\\My Games\\Fallout4VR\\F4SE\\Plugins\\F4SEVR_DLSS.ini";
    const std::string pathWithSpace = base + "\\My Games\\Fallout 4 VR\\F4SE\\Plugins\\F4SEVR_DLSS.ini";
    // Prefer whichever already exists
    if (GetFileAttributesA(pathNoSpace.c_str()) != INVALID_FILE_ATTRIBUTES) return pathNoSpace;
    if (GetFileAttributesA(pathWithSpace.c_str()) != INVALID_FILE_ATTRIBUTES) return pathWithSpace;
    // If neither INI exists yet, prefer the folder that exists; otherwise default to no‑space variant
    const std::string dirNoSpace = base + "\\My Games\\Fallout4VR\\F4SE\\Plugins\\";
    const std::string dirWithSpace = base + "\\My Games\\Fallout 4 VR\\F4SE\\Plugins\\";
    if (GetFileAttributesA(dirNoSpace.c_str()) != INVALID_FILE_ATTRIBUTES) return pathNoSpace;
    if (GetFileAttributesA(dirWithSpace.c_str()) != INVALID_FILE_ATTRIBUTES) return pathWithSpace;
    return pathNoSpace;
}

void DLSSConfig::ForceDisableEarlyDlss(const char* reason) {
    const bool wasEnabled = earlyDlssEnabled;
    const bool wasModeNonZero = (earlyDlssMode != 0);
    if (!wasEnabled && !wasModeNonZero) {
        return;
    }

    earlyDlssEnabled = false;
    earlyDlssMode = 0;

    if (wasEnabled || wasModeNonZero) {
        if (reason && reason[0] != '\0') {
            _MESSAGE("Early DLSS forcibly disabled (%s)", reason);
        } else {
            _MESSAGE("Early DLSS forcibly disabled (runtime override)");
        }
    }
}

namespace {
    std::wstring GetThisModuleDir() {
        HMODULE hModule = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&GetThisModuleDir),
                                 &hModule)) {
            return {};
        }
        wchar_t buffer[MAX_PATH] = {};
        DWORD length = GetModuleFileNameW(hModule, buffer, static_cast<DWORD>(_countof(buffer)));
        if (length == 0 || length >= _countof(buffer)) {
            return {};
        }
        std::wstring path(buffer, buffer + length);
        const size_t slash = path.find_last_of(L"/\\");
        if (slash != std::wstring::npos) {
            path.erase(slash + 1);
        } else {
            path.clear();
        }
        return path;
    }
}

std::string DLSSConfig::GetPluginConfigPath() {
    std::wstring dir = GetThisModuleDir();
    if (dir.empty()) {
        return std::string("Data/F4SE/Plugins/F4SEVR_DLSS.ini");
    }
    std::wstring wpath = dir + L"F4SEVR_DLSS.ini";
    int needed = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string path;
    if (needed > 0) {
        path.resize(static_cast<size_t>(needed - 1));
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, &path[0], needed, nullptr, nullptr);
    }
    return path;
}

std::string DLSSConfig::ResolveConfigPath(bool* outIsDocuments, bool* outIsPlugin) {
    const std::string docs = GetDocumentsConfigPath();
    const std::string plugin = GetPluginConfigPath();

    if (outIsDocuments) *outIsDocuments = false;
    if (outIsPlugin) *outIsPlugin = false;

    if (GetFileAttributesA(docs.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (outIsDocuments) *outIsDocuments = true;
        return docs;
    }
    if (GetFileAttributesA(plugin.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (outIsPlugin) *outIsPlugin = true;
        return plugin;
    }
    // Default preferred path if none exists yet
    if (outIsDocuments) *outIsDocuments = true;
    return docs;
}

std::string DLSSConfig::GetConfigPath() {
    return GetDocumentsConfigPath();
}

void DLSSConfig::Load() {
    bool isDocs = false, isPlugin = false;
    const std::string docsPath = DLSSConfig::GetDocumentsConfigPath();
    const std::string pluginPath = DLSSConfig::GetPluginConfigPath();

    const DWORD docsAttr = GetFileAttributesA(docsPath.c_str());
    const DWORD plugAttr = GetFileAttributesA(pluginPath.c_str());

    std::string configPath;
    bool shouldPersistToDocs = false;

    if (docsAttr != INVALID_FILE_ATTRIBUTES) {
        // Normal case: prefer Documents
        configPath = docsPath;
        isDocs = true;
    } else if (plugAttr != INVALID_FILE_ATTRIBUTES) {
        // Documents missing but plugin INI present (e.g., fresh install via MO2)
        // Load from plugin and then persist to Documents to unify the source of truth
        configPath = pluginPath;
        isPlugin = true;
        shouldPersistToDocs = true;
    } else {
        // Neither exists yet; will create defaults in Documents on Parse failure
        configPath = docsPath;
        isDocs = true;
    }

    if (isPlugin && !isDocs) {
        _MESSAGE("Loading config from plugin directory (legacy): %s", configPath.c_str());
    } else {
        _MESSAGE("Loading config from: %s", configPath.c_str());
    }

    ParseIniFile(configPath);
    ForceDisableEarlyDlss("VR stability override");
    if (shouldPersistToDocs) {
        // Copy current in-memory settings to Documents to avoid future ambiguity
        Save();
        _MESSAGE("Config replicated to Documents path after plugin load");
    }
    
    // Apply settings to DLSS Manager
    if (g_dlssManager) {
        g_dlssManager->SetEnabled(enableUpscaler);
        g_dlssManager->SetQuality(quality);
        g_dlssManager->SetSharpeningEnabled(enableSharpening);
        g_dlssManager->SetSharpness(sharpness);
        g_dlssManager->SetUseOptimalMipLodBias(useOptimalMipLodBias);
        g_dlssManager->SetManualMipLodBias(mipLodBias);
        g_dlssManager->SetRenderReShadeBeforeUpscaling(renderReShadeBeforeUpscaling);
        g_dlssManager->SetUpscaleDepthForReShade(upscaleDepthForReShade);
        g_dlssManager->SetUseTAAPeriphery(useTAAForPeriphery);
        g_dlssManager->SetDLSSPreset(dlssPreset);
        g_dlssManager->SetFOV(fov);
        g_dlssManager->SetFixedFoveatedRendering(enableFixedFoveatedRendering);
        g_dlssManager->SetFoveatedRadii(foveatedInnerRadius, foveatedMiddleRadius, foveatedOuterRadius);
        g_dlssManager->SetFixedFoveatedUpscaling(enableFixedFoveatedUpscaling);
        g_dlssManager->SetFoveatedScale(foveatedScaleX, foveatedScaleY);
        g_dlssManager->SetFoveatedOffsets(foveatedOffsetX, foveatedOffsetY);
        g_dlssManager->SetFoveatedCutout(foveatedCutoutRadius);
        g_dlssManager->SetFoveatedWiden(foveatedWiden);
        g_dlssManager->SetTransformerModel(enableTransformerModel);
        g_dlssManager->SetRayReconstruction(enableRayReconstruction);
    }
}

void DLSSConfig::ParseIniFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        _MESSAGE("Config file not found, creating default config");
        Save();
        return;
    }

    std::string line;
    std::string section;

    auto trimInPlace = [](std::string& value) {
        const auto first = value.find_first_not_of(" 	\r\n");
        if (first == std::string::npos) {
            value.clear();
            return;
        }
        const auto last = value.find_last_not_of(" 	\r\n");
        value = value.substr(first, last - first + 1);
    };

    while (std::getline(file, line)) {
        const size_t commentPos = line.find_first_of("#;");
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }

        trimInPlace(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = ToLower(line.substr(1, line.size() - 2));
            continue;
        }

        const size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, equalPos);
        std::string value = line.substr(equalPos + 1);
        trimInPlace(key);
        trimInPlace(value);
        if (key.empty()) {
            continue;
        }

        const std::string normalizedKey = NormalizeKey(key);
        const std::string lowerSection = ToLower(section);

        if (lowerSection == "settings") {
            if (normalizedKey == "enableupscaler") {
                enableUpscaler = StringToBool(value);
            } else if (normalizedKey == "backend" || normalizedKey == "slonly") {
                // mBackend = SL|NGX  OR  mSLOnly = true/false
                std::string v = ToLower(value);
                if (normalizedKey == "backend") {
                    streamlineOnly = (v == "sl" || v == "streamline");
                } else {
                    streamlineOnly = StringToBool(value);
                }
            } else if (normalizedKey == "upscaletype" || normalizedKey == "upscalertype") {
                int type = ClampValue(ParseInt(value), 0, 3);
                upscalerType = static_cast<UpscalerType>(type);
            } else if (normalizedKey == "quality" || normalizedKey == "qualitylevel") {
                int q = ClampValue(ParseInt(value), 0, 5);
                quality = static_cast<DLSSManager::Quality>(q);
            } else if (normalizedKey == "sharpening" || normalizedKey == "enablesharpening") {
                enableSharpening = StringToBool(value);
            } else if (normalizedKey == "sharpness") {
                sharpness = ClampValue(ParseFloat(value), 0.0f, 1.0f);
            } else if (normalizedKey == "useoptimalmiplodbias") {
                useOptimalMipLodBias = StringToBool(value);
            } else if (normalizedKey == "miplodbias") {
                mipLodBias = ParseFloat(value);
            } else if (normalizedKey == "renderreshadebeforeupscaling") {
                renderReShadeBeforeUpscaling = StringToBool(value);
            } else if (normalizedKey == "earlydlssenabled") {
                earlyDlssEnabled = StringToBool(value);
            } else if (normalizedKey == "earlydlssmode") {
                std::string v = ToLower(value);
                if (v == "viewport") earlyDlssMode = 0; else if (v == "rt_redirect" || v == "rtredirect") earlyDlssMode = 1; else earlyDlssMode = ClampValue(ParseInt(value), 0, 1);
            } else if (normalizedKey == "peripherytaaenabled") {
                peripheryTAAEnabled = StringToBool(value);
            } else if (normalizedKey == "foveatedrenderingenabled") {
                foveatedRenderingEnabled = StringToBool(value);
            } else if (normalizedKey == "debugearlydlss") {
                debugEarlyDlss = StringToBool(value);
            } else if (normalizedKey == "upscaledepthforreshade" || normalizedKey == "upscaledeptforreshade") {
                upscaleDepthForReShade = StringToBool(value);
            } else if (normalizedKey == "usetaaforperiphery") {
                useTAAForPeriphery = StringToBool(value);
            } else if (normalizedKey == "dlsspreset") {
                dlssPreset = ClampValue(ParseInt(value), 0, 6);
            } else if (normalizedKey == "fov") {
                fov = ParseFloat(value);
            } else if (normalizedKey == "uiscale" || normalizedKey == "menuscale") {
                uiScale = ClampValue(ParseFloat(value), 0.5f, 3.0f);
            } else if (normalizedKey == "enablepereyecap") {
                enablePerEyeCap = StringToBool(value);
            } else if (normalizedKey == "pereyemaxdim" || normalizedKey == "pereyemaxdimension") {
                perEyeMaxDim = ClampValue(ParseInt(value), 512, 8192);
            } else if (normalizedKey == "highqualitycomposite") {
                highQualityComposite = StringToBool(value);
            }
        } else if (lowerSection == "dlss4" || lowerSection == "dlss") {
            if (normalizedKey == "enabletransformermodel") {
                enableTransformerModel = StringToBool(value);
            } else if (normalizedKey == "enablerayreconstruction") {
                enableRayReconstruction = StringToBool(value);
            }
        } else if (lowerSection == "vr") {
            if (normalizedKey == "enablefixedfoveatedrendering" || normalizedKey == "enablefixedfoveated") {
                enableFixedFoveatedRendering = StringToBool(value);
            } else if (normalizedKey == "foveatedinnerradius" || normalizedKey == "innerradius") {
                foveatedInnerRadius = ParseFloat(value);
            } else if (normalizedKey == "foveatedmiddleradius" || normalizedKey == "middleradius") {
                foveatedMiddleRadius = ParseFloat(value);
            } else if (normalizedKey == "foveatedouterradius" || normalizedKey == "outerradius") {
                foveatedOuterRadius = ParseFloat(value);
            }
        } else if (lowerSection == "fixedfoveatedupscaling") {
            if (normalizedKey == "enablefixedfoveatedupscaling") {
                enableFixedFoveatedUpscaling = StringToBool(value);
            } else if (normalizedKey == "foveatedscalex") {
                foveatedScaleX = ParseFloat(value);
            } else if (normalizedKey == "foveatedscaley") {
                foveatedScaleY = ParseFloat(value);
            } else if (normalizedKey == "foveatedoffsetx") {
                foveatedOffsetX = ParseFloat(value);
            } else if (normalizedKey == "foveatedoffsety") {
                foveatedOffsetY = ParseFloat(value);
            }
        } else if (lowerSection == "fixedfoveatedrendering") {
            if (normalizedKey == "enablefixedfoveatedrendering") {
                enableFixedFoveatedRendering = StringToBool(value);
            } else if (normalizedKey == "innerradius") {
                foveatedInnerRadius = ParseFloat(value);
            } else if (normalizedKey == "middleradius") {
                foveatedMiddleRadius = ParseFloat(value);
            } else if (normalizedKey == "outerradius") {
                foveatedOuterRadius = ParseFloat(value);
            } else if (normalizedKey == "cutoutradius") {
                foveatedCutoutRadius = ParseFloat(value);
            } else if (normalizedKey == "widen") {
                foveatedWiden = ParseFloat(value);
            }
        } else if (lowerSection == "performance") {
            if (normalizedKey == "enablelowlatencymode") {
                enableLowLatencyMode = StringToBool(value);
            } else if (normalizedKey == "enablereflex") {
                enableReflex = StringToBool(value);
            }
        } else if (lowerSection == "settings") {
            if (normalizedKey == "submitcopyenabled") {
                submitCopyEnabled = StringToBool(value);
            }
        } else if (lowerSection == "hotkeys") {
            if (normalizedKey == "togglemenu") {
                toggleMenuKey = NormalizeHotkeyValue(ParseInt(value));
            } else if (normalizedKey == "toggleupscaler") {
                toggleUpscalerKey = NormalizeHotkeyValue(ParseInt(value));
            } else if (normalizedKey == "cyclequality") {
                cycleQualityKey = NormalizeHotkeyValue(ParseInt(value));
            } else if (normalizedKey == "cycleupscaler") {
                cycleUpscalerKey = NormalizeHotkeyValue(ParseInt(value));
            }
        }
    }

    file.close();
    _MESSAGE("Config loaded successfully");
}

void DLSSConfig::Save() {
    ForceDisableEarlyDlss("config save");
    // Always prefer the no-space folder (Fallout4VR), but keep the with-space variant in sync if present
    char docs[MAX_PATH] = {};
    SHGetFolderPathA(NULL, CSIDL_MYDOCUMENTS, NULL, 0, docs);
    const std::string base = docs;
    const std::string pathNoSpace = base + "\\My Games\\Fallout4VR\\F4SE\\Plugins\\F4SEVR_DLSS.ini";
    const std::string pathWithSpace = base + "\\My Games\\Fallout 4 VR\\F4SE\\Plugins\\F4SEVR_DLSS.ini";

    auto ensureDir = [](const std::string& filePath) {
        std::string dir = filePath;
        size_t pos = dir.find_last_of("/\\");
        if (pos != std::string::npos) {
            dir = dir.substr(0, pos);
            if (!dir.empty()) SHCreateDirectoryExA(NULL, dir.c_str(), NULL);
        }
    };

    // Primary write: no-space variant
    ensureDir(pathNoSpace);
    std::ofstream file(pathNoSpace);
    if (!file.is_open()) {
        _ERROR("Failed to save config file at preferred path (no-space)");
        return;
    }

    auto boolToString = [](bool value) { return value ? "true" : "false"; };

    file << "; F4SEVR DLSS Plugin Configuration" << std::endl;
    file << "; Bu dosya otomatik olusturuldu. ImGui menusu ile uyumludur." << std::endl << std::endl;

    file << "[Settings]" << std::endl;
    file << "mEnableUpscaler = " << boolToString(enableUpscaler) << std::endl;
    file << "mBackend = " << (streamlineOnly ? "SL" : "NGX") << std::endl;
    file << "mUpscalerType = " << static_cast<int>(upscalerType) << std::endl;
    file << "mQualityLevel = " << static_cast<int>(quality) << std::endl;
    file << "mSharpening = " << boolToString(enableSharpening) << std::endl;
    file << "mSharpness = " << sharpness << std::endl;
    file << "mUseOptimalMipLodBias = " << boolToString(useOptimalMipLodBias) << std::endl;
    file << "mMipLodBias = " << mipLodBias << std::endl;
    file << "mRenderReShadeBeforeUpscaling = " << boolToString(renderReShadeBeforeUpscaling) << std::endl;
    file << "mUpscaleDepthForReShade = " << boolToString(upscaleDepthForReShade) << std::endl;
    file << "mUseTAAForPeriphery = " << boolToString(useTAAForPeriphery) << std::endl;
    // Early DLSS integration flags (UI-guarded)
    file << "mEarlyDlssEnabled = " << boolToString(earlyDlssEnabled) << std::endl;
    file << "mEarlyDlssMode = " << earlyDlssMode << std::endl;
    file << "mPeripheryTAAEnabled = " << boolToString(peripheryTAAEnabled) << std::endl;
    file << "mFoveatedRenderingEnabled = " << boolToString(foveatedRenderingEnabled) << std::endl;
    file << "mDebugEarlyDlss = " << boolToString(debugEarlyDlss) << std::endl;
    file << "; Asiri per-eye boyutlarini onlemek icin guardrail" << std::endl;
    file << "mEnablePerEyeCap = " << boolToString(enablePerEyeCap) << std::endl;
    file << "mPerEyeMaxDim = " << perEyeMaxDim << std::endl;
    file << "; Kucuk->buyuk kompozitte HQ yolunu kullan (varsayilan: false)" << std::endl;
    file << "mHighQualityComposite = " << boolToString(highQualityComposite) << std::endl;
    file << "mDLSSPreset = " << dlssPreset << std::endl;
    file << "mFOV = " << fov << std::endl << std::endl;
    file << "; ImGui menusu icin UI olcegi (0.5 - 3.0). VR icin 1.5 uygun" << std::endl;
    file << "mUIScale = " << uiScale << std::endl << std::endl;

    file << "[DLSS4]" << std::endl;
    file << "mEnableTransformerModel = " << boolToString(enableTransformerModel) << std::endl;
    file << "mEnableRayReconstruction = " << boolToString(enableRayReconstruction) << std::endl << std::endl;

    file << "[FixedFoveatedUpscaling]" << std::endl;
    file << "mEnableFixedFoveatedUpscaling = " << boolToString(enableFixedFoveatedUpscaling) << std::endl;
    file << "mFoveatedScaleX = " << foveatedScaleX << std::endl;
    file << "mFoveatedScaleY = " << foveatedScaleY << std::endl;
    file << "mFoveatedOffsetX = " << foveatedOffsetX << std::endl;
    file << "mFoveatedOffsetY = " << foveatedOffsetY << std::endl << std::endl;

    file << "[FixedFoveatedRendering]" << std::endl;
    file << "mEnableFixedFoveatedRendering = " << boolToString(enableFixedFoveatedRendering) << std::endl;
    file << "mInnerRadius = " << foveatedInnerRadius << std::endl;
    file << "mMiddleRadius = " << foveatedMiddleRadius << std::endl;
    file << "mOuterRadius = " << foveatedOuterRadius << std::endl;
    file << "mCutoutRadius = " << foveatedCutoutRadius << std::endl;
    file << "mWiden = " << foveatedWiden << std::endl << std::endl;

    file << "[Performance]" << std::endl;
    file << "mEnableLowLatencyMode = " << boolToString(enableLowLatencyMode) << std::endl;
    file << "mEnableReflex = " << boolToString(enableReflex) << std::endl << std::endl;

    file << "[Hotkeys]" << std::endl;
    file << "; Virtual-key codes. See: https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes" << std::endl;
    file << "mToggleMenu = " << FormatVirtualKey(toggleMenuKey) << std::endl;
    file << "mToggleUpscaler = " << FormatVirtualKey(toggleUpscalerKey) << std::endl;
    file << "mCycleQuality = " << FormatVirtualKey(cycleQualityKey) << std::endl;
    file << "mCycleUpscaler = " << FormatVirtualKey(cycleUpscalerKey) << std::endl;

    file.close();
    _MESSAGE("Config saved to: %s", pathNoSpace.c_str());

    // Secondary write (keep with-space in sync if it already exists)
    const DWORD attrWith = GetFileAttributesA(pathWithSpace.c_str());
    const std::string dirWith = base + "\\My Games\\Fallout 4 VR\\F4SE\\Plugins\\";
    const DWORD dirWithAttr = GetFileAttributesA(dirWith.c_str());
    if (attrWith != INVALID_FILE_ATTRIBUTES || dirWithAttr != INVALID_FILE_ATTRIBUTES) {
        ensureDir(pathWithSpace);
        std::ofstream file2(pathWithSpace);
        if (file2.is_open()) {
            // Reuse the same serialization
            auto boolToString = [](bool value) { return value ? "true" : "false"; };
            file2 << "; F4SEVR DLSS Plugin Configuration" << std::endl;
            file2 << "; Bu dosya otomatik olusturuldu. ImGui menusu ile uyumludur." << std::endl << std::endl;
            file2 << "[Settings]" << std::endl;
            file2 << "mEnableUpscaler = " << boolToString(enableUpscaler) << std::endl;
            file2 << "mBackend = " << (streamlineOnly ? "SL" : "NGX") << std::endl;
            file2 << "mUpscalerType = " << static_cast<int>(upscalerType) << std::endl;
            file2 << "mQualityLevel = " << static_cast<int>(quality) << std::endl;
            file2 << "mSharpening = " << boolToString(enableSharpening) << std::endl;
            file2 << "mSharpness = " << sharpness << std::endl;
            file2 << "mUseOptimalMipLodBias = " << boolToString(useOptimalMipLodBias) << std::endl;
            file2 << "mMipLodBias = " << mipLodBias << std::endl;
            file2 << "mRenderReShadeBeforeUpscaling = " << boolToString(renderReShadeBeforeUpscaling) << std::endl;
            file2 << "mUpscaleDepthForReShade = " << boolToString(upscaleDepthForReShade) << std::endl;
            file2 << "mUseTAAForPeriphery = " << boolToString(useTAAForPeriphery) << std::endl;
            file2 << "mEarlyDlssEnabled = " << boolToString(earlyDlssEnabled) << std::endl;
            file2 << "mEarlyDlssMode = " << earlyDlssMode << std::endl;
            file2 << "mPeripheryTAAEnabled = " << boolToString(peripheryTAAEnabled) << std::endl;
            file2 << "mFoveatedRenderingEnabled = " << boolToString(foveatedRenderingEnabled) << std::endl;
            file2 << "mDebugEarlyDlss = " << boolToString(debugEarlyDlss) << std::endl;
            file2 << "mEnablePerEyeCap = " << boolToString(enablePerEyeCap) << std::endl;
            file2 << "mPerEyeMaxDim = " << perEyeMaxDim << std::endl;
            file2 << "mHighQualityComposite = " << boolToString(highQualityComposite) << std::endl;
            file2 << "mDLSSPreset = " << dlssPreset << std::endl;
            file2 << "mFOV = " << fov << std::endl << std::endl;
            file2 << "; ImGui menusu icin UI olcegi (0.5 - 3.0). VR icin 1.5 uygun" << std::endl;
            file2 << "mUIScale = " << uiScale << std::endl << std::endl;
            file2 << "[DLSS4]" << std::endl;
            file2 << "mEnableTransformerModel = " << boolToString(enableTransformerModel) << std::endl;
            file2 << "mEnableRayReconstruction = " << boolToString(enableRayReconstruction) << std::endl << std::endl;
            file2 << "[FixedFoveatedUpscaling]" << std::endl;
            file2 << "mEnableFixedFoveatedUpscaling = " << boolToString(enableFixedFoveatedUpscaling) << std::endl;
            file2 << "mFoveatedScaleX = " << foveatedScaleX << std::endl;
            file2 << "mFoveatedScaleY = " << foveatedScaleY << std::endl;
            file2 << "mFoveatedOffsetX = " << foveatedOffsetX << std::endl;
            file2 << "mFoveatedOffsetY = " << foveatedOffsetY << std::endl << std::endl;
            file2 << "[FixedFoveatedRendering]" << std::endl;
            file2 << "mEnableFixedFoveatedRendering = " << boolToString(enableFixedFoveatedRendering) << std::endl;
            file2 << "mInnerRadius = " << foveatedInnerRadius << std::endl;
            file2 << "mMiddleRadius = " << foveatedMiddleRadius << std::endl;
            file2 << "mOuterRadius = " << foveatedOuterRadius << std::endl;
            file2 << "mCutoutRadius = " << foveatedCutoutRadius << std::endl;
            file2 << "mWiden = " << foveatedWiden << std::endl << std::endl;
            file2 << "[Performance]" << std::endl;
            file2 << "mEnableLowLatencyMode = " << boolToString(enableLowLatencyMode) << std::endl;
            file2 << "mEnableReflex = " << boolToString(enableReflex) << std::endl << std::endl;
            file2 << "[Hotkeys]" << std::endl;
            file2 << "; Virtual-key codes. See: https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes" << std::endl;
            file2 << "mToggleMenu = " << FormatVirtualKey(toggleMenuKey) << std::endl;
            file2 << "mToggleUpscaler = " << FormatVirtualKey(toggleUpscalerKey) << std::endl;
            file2 << "mCycleQuality = " << FormatVirtualKey(cycleQualityKey) << std::endl;
            file2 << "mCycleUpscaler = " << FormatVirtualKey(cycleUpscalerKey) << std::endl;
            file2.close();
            _MESSAGE("Config mirrored to: %s", pathWithSpace.c_str());
        }
    }
}





