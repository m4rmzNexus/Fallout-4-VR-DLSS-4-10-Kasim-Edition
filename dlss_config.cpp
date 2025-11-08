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
    char path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYDOCUMENTS, NULL, 0, path))) {
        std::string configPath = path;
        configPath += "\\My Games\\Fallout4VR\\F4SE\\Plugins\\F4SEVR_DLSS.ini";
        return configPath;
    }
    return std::string("F4SEVR_DLSS.ini");
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
    std::string configPath = DLSSConfig::ResolveConfigPath(&isDocs, &isPlugin);
    if (isPlugin && !isDocs) {
        _MESSAGE("Loading config from plugin directory (legacy): %s", configPath.c_str());
    } else {
        _MESSAGE("Loading config from: %s", configPath.c_str());
    }

    ParseIniFile(configPath);
    
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
    std::string configPath = DLSSConfig::GetDocumentsConfigPath();

    std::string directory;
    const size_t delimiter = configPath.find_last_of("/\\");
    if (delimiter != std::string::npos) {
        directory = configPath.substr(0, delimiter);
        if (!directory.empty()) {
            CreateDirectoryA(directory.c_str(), NULL);
        }
    }

    std::ofstream file(configPath);
    if (!file.is_open()) {
        _ERROR("Failed to save config file");
        return;
    }

    auto boolToString = [](bool value) { return value ? "true" : "false"; };

    file << "; F4SEVR DLSS Plugin Configuration" << std::endl;
    file << "; Generated automatically - edit with care" << std::endl << std::endl;

    file << "[Settings]" << std::endl;
    file << "EnableUpscaler = " << boolToString(enableUpscaler) << std::endl;
    file << "UpscalerType = " << static_cast<int>(upscalerType) << std::endl;
    file << "QualityLevel = " << static_cast<int>(quality) << std::endl;
    file << "Sharpening = " << boolToString(enableSharpening) << std::endl;
    file << "Sharpness = " << sharpness << std::endl;
    file << "UseOptimalMipLodBias = " << boolToString(useOptimalMipLodBias) << std::endl;
    file << "MipLodBias = " << mipLodBias << std::endl;
    file << "RenderReShadeBeforeUpscaling = " << boolToString(renderReShadeBeforeUpscaling) << std::endl;
    file << "UpscaleDepthForReShade = " << boolToString(upscaleDepthForReShade) << std::endl;
    file << "UseTAAForPeriphery = " << boolToString(useTAAForPeriphery) << std::endl;
    // Early DLSS integration flags
    file << "EarlyDlssEnabled = " << boolToString(earlyDlssEnabled) << std::endl;
    file << "EarlyDlssMode = " << earlyDlssMode << std::endl;
    file << "PeripheryTAAEnabled = " << boolToString(peripheryTAAEnabled) << std::endl;
    file << "FoveatedRenderingEnabled = " << boolToString(foveatedRenderingEnabled) << std::endl;
    file << "DebugEarlyDlss = " << boolToString(debugEarlyDlss) << std::endl;
    file << "DLSSPreset = " << dlssPreset << std::endl;
    file << "FOV = " << fov << std::endl << std::endl;
    file << "; UI scale for ImGui menu (0.5 - 3.0). 1.5 is good for VR" << std::endl;
    file << "UIScale = " << uiScale << std::endl << std::endl;

    file << "[DLSS4]" << std::endl;
    file << "EnableTransformerModel = " << boolToString(enableTransformerModel) << std::endl;
    file << "EnableRayReconstruction = " << boolToString(enableRayReconstruction) << std::endl << std::endl;

    file << "[FixedFoveatedUpscaling]" << std::endl;
    file << "EnableFixedFoveatedUpscaling = " << boolToString(enableFixedFoveatedUpscaling) << std::endl;
    file << "FoveatedScaleX = " << foveatedScaleX << std::endl;
    file << "FoveatedScaleY = " << foveatedScaleY << std::endl;
    file << "FoveatedOffsetX = " << foveatedOffsetX << std::endl;
    file << "FoveatedOffsetY = " << foveatedOffsetY << std::endl << std::endl;

    file << "[FixedFoveatedRendering]" << std::endl;
    file << "EnableFixedFoveatedRendering = " << boolToString(enableFixedFoveatedRendering) << std::endl;
    file << "InnerRadius = " << foveatedInnerRadius << std::endl;
    file << "MiddleRadius = " << foveatedMiddleRadius << std::endl;
    file << "OuterRadius = " << foveatedOuterRadius << std::endl;
    file << "CutoutRadius = " << foveatedCutoutRadius << std::endl;
    file << "Widen = " << foveatedWiden << std::endl << std::endl;

    file << "[Performance]" << std::endl;
    file << "EnableLowLatencyMode = " << boolToString(enableLowLatencyMode) << std::endl;
    file << "EnableReflex = " << boolToString(enableReflex) << std::endl << std::endl;

    file << "[Hotkeys]" << std::endl;
    file << "; Virtual-key codes. See: https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes" << std::endl;
    file << "ToggleMenu = " << FormatVirtualKey(toggleMenuKey) << std::endl;
    file << "ToggleUpscaler = " << FormatVirtualKey(toggleUpscalerKey) << std::endl;
    file << "CycleQuality = " << FormatVirtualKey(cycleQualityKey) << std::endl;
    file << "CycleUpscaler = " << FormatVirtualKey(cycleUpscalerKey) << std::endl;

    file.close();
    _MESSAGE("Config saved to: %s", configPath.c_str());
}





