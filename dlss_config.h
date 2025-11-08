#pragma once
#include <string>
#include "dlss_manager.h"

class DLSSConfig {
public:
    enum class UpscalerType {
        DLSS = 0,
        FSR2 = 1,
        XeSS = 2,
        DLAA = 3
    };

    DLSSConfig();
    ~DLSSConfig();
    
    void Load();
    void Save();

    // Preferred save path (Documents). Remains for backward compatibility.
    static std::string GetConfigPath();

    // Normalized path helpers
    static std::string GetDocumentsConfigPath();
    static std::string GetPluginConfigPath();
    // Resolve the config file to load: prefers Documents if present, then plugin dir, else Documents.
    static std::string ResolveConfigPath(bool* outIsDocuments = nullptr, bool* outIsPlugin = nullptr);
    
    // Upscaler settings
    bool enableUpscaler = true;
    UpscalerType upscalerType = UpscalerType::DLSS;
    DLSSManager::Quality quality = DLSSManager::Quality::Quality;
    bool enableSharpening = true;
    float sharpness = 0.8f;
    bool useOptimalMipLodBias = true;
    float mipLodBias = -1.585315f;
    bool renderReShadeBeforeUpscaling = true;
    bool upscaleDepthForReShade = false;
    bool useTAAForPeriphery = false;
    int dlssPreset = 4;
    float fov = 90.0f;

    // UI
    // Global scale for ImGui menu in VR; 1.0 = default size
    // Typical comfortable range in VR is 1.2–2.0
    float uiScale = 1.5f;

    // DLSS 4 specific (without frame generation)
    bool enableTransformerModel = true;  // New DLSS 4 transformer model
    bool enableRayReconstruction = false;

    // VR specific settings
    bool enableFixedFoveatedRendering = true;
    float foveatedInnerRadius = 0.8f;
    float foveatedMiddleRadius = 0.85f;
    float foveatedOuterRadius = 0.9f;
    bool enableFixedFoveatedUpscaling = false;
    float foveatedScaleX = 0.8f;
    float foveatedScaleY = 0.6f;
    float foveatedOffsetX = -0.05f;
    float foveatedOffsetY = 0.04f;
    float foveatedCutoutRadius = 1.2f;
    float foveatedWiden = 1.5f;

    // Performance settings
    bool enableLowLatencyMode = true;
    bool enableReflex = false;  // NVIDIA Reflex

    // Hotkeys (Windows virtual-key codes)
    int toggleMenuKey = 0x47;      // 'G' key
    int toggleUpscalerKey = 0x6A;  // VK_MULTIPLY (NumPad *)
    int cycleQualityKey = 0x24;    // VK_HOME
    int cycleUpscalerKey = 0x2D;   // VK_INSERT

    // Early DLSS integration (render-time) flags
    bool earlyDlssEnabled = false;       // Faz 1/2 entegrasyonu aç/kapa
    int  earlyDlssMode = 0;              // 0=viewport clamp, 1=rt_redirect
    bool peripheryTAAEnabled = true;     // DLSS dikdörtgeni dışını TAA ile çöz
    bool foveatedRenderingEnabled = false; // FFR/FFU ana bayrak
    bool debugEarlyDlss = false;         // Geniş log

    // Guardrails / IQ options
    bool enablePerEyeCap = false;        // Optional cap to protect against extreme SS
    int  perEyeMaxDim = 4096;            // Max per-eye dimension when cap is enabled
    bool highQualityComposite = false;   // Use HQ composite path for small->big (optional)

private:
    void ParseIniFile(const std::string& path);
};
