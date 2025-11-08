#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "F4SEVR_Upscaler.h"
#include "dlss_manager.h"
#include "dlss_config.h"

extern DLSSManager* g_dlssManager;
extern DLSSConfig* g_dlssConfig;

class ImGuiMenu {
private:
    struct HotkeyBinding {
        int virtualKey = 0;
        bool latched = false;
    };

    bool menuVisible = false;
    bool initialized = false;
    bool showPerformanceMetrics = true;
    bool showAdvancedSettings = false;

    bool enableUpscalerSetting = true;
    bool sharpeningEnabled = true;
    bool useOptimalMip = true;
    float mipLodBiasSetting = -1.585315f;
    bool renderReShadeBeforeUpscalingSetting = true;
    bool upscaleDepthForReShadeSetting = false;
    bool useTAAForPeripherySetting = false;
    int dlssPresetSetting = 4;
    float fovSetting = 90.0f;

    float fps = 0.0f;
    float frameTime = 0.0f;
    float gpuUsage = 0.0f;

    int currentUpscaler = 0;
    int currentQuality = 2;
    float sharpness = 0.8f;
    bool enableFrameGen = true;
    int frameGenMode = 2;
    bool enableVROptimizations = true;
    bool enableFixedFoveated = true;
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

    // Early DLSS (experimental) UI state
    bool earlyDlssEnabledSetting = false;
    int  earlyDlssModeSetting = 0; // 0=viewport clamp, 1=rt_redirect
    bool debugEarlyDlssSetting = false;

    HotkeyBinding menuHotkey{VK_END, false};
    HotkeyBinding toggleHotkey{VK_MULTIPLY, false};
    HotkeyBinding cycleQualityHotkey{VK_HOME, false};
    HotkeyBinding cycleUpscalerHotkey{VK_INSERT, false};
    bool hotkeysDirty = true;

    const ImVec4 colorGreen = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    const ImVec4 colorYellow = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    const ImVec4 colorRed = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    const ImVec4 colorCyan = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);

public:
    static ImGuiMenu& GetInstance() {
        static ImGuiMenu instance;
        return instance;
    }

    bool Initialize() {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        initialized = true;

        SyncFromConfig();
        UpdateHotkeyBindings();

        if (FILE* log = fopen("F4SEVR_DLSS.log", "a")) {
            fprintf(log, "ImGui Menu initialized\n");
            fclose(log);
        }

        return true;
    }

    void SyncFromConfig() {
        if (!g_dlssConfig) {
            hotkeysDirty = true;
            return;
        }

        enableUpscalerSetting = g_dlssConfig->enableUpscaler;
        currentUpscaler = static_cast<int>(g_dlssConfig->upscalerType);
        currentQuality = static_cast<int>(g_dlssConfig->quality);
        sharpeningEnabled = g_dlssConfig->enableSharpening;
        sharpness = g_dlssConfig->sharpness;
        useOptimalMip = g_dlssConfig->useOptimalMipLodBias;
        mipLodBiasSetting = g_dlssConfig->mipLodBias;
        renderReShadeBeforeUpscalingSetting = g_dlssConfig->renderReShadeBeforeUpscaling;
        upscaleDepthForReShadeSetting = g_dlssConfig->upscaleDepthForReShade;
        useTAAForPeripherySetting = g_dlssConfig->useTAAForPeriphery;
        dlssPresetSetting = g_dlssConfig->dlssPreset;
        fovSetting = g_dlssConfig->fov;
        enableFixedFoveated = g_dlssConfig->enableFixedFoveatedRendering;
        enableFixedFoveatedUpscaling = g_dlssConfig->enableFixedFoveatedUpscaling;
        foveatedInnerRadius = g_dlssConfig->foveatedInnerRadius;
        foveatedMiddleRadius = g_dlssConfig->foveatedMiddleRadius;
        foveatedOuterRadius = g_dlssConfig->foveatedOuterRadius;
        foveatedScaleX = g_dlssConfig->foveatedScaleX;
        foveatedScaleY = g_dlssConfig->foveatedScaleY;
        foveatedOffsetX = g_dlssConfig->foveatedOffsetX;
        foveatedOffsetY = g_dlssConfig->foveatedOffsetY;
        foveatedCutoutRadius = g_dlssConfig->foveatedCutoutRadius;
        foveatedWiden = g_dlssConfig->foveatedWiden;

        // Early DLSS flags
        earlyDlssEnabledSetting = g_dlssConfig->earlyDlssEnabled;
        earlyDlssModeSetting = g_dlssConfig->earlyDlssMode;
        debugEarlyDlssSetting = g_dlssConfig->debugEarlyDlss;

        hotkeysDirty = true;
    }

    void ToggleMenu() {
        menuVisible = !menuVisible;
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().MouseDrawCursor = menuVisible;
        }

        if (FILE* log = fopen("F4SEVR_DLSS.log", "a")) {
            fprintf(log, "Menu toggled: %s\n", menuVisible ? "ON" : "OFF");
            fclose(log);
        }
    }

    bool IsVisible() const {
        return menuVisible;
    }

    void Render() {
        if (!initialized || !menuVisible) {
            return;
        }

        const ImVec2 windowSize(520.0f, 640.0f);
        const ImVec2 windowPos(60.0f, 60.0f);
        ImGui::SetNextWindowSize(windowSize, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(windowPos, ImGuiCond_FirstUseEver);

        if (ImGui::Begin("F4SEVR DLSS4 Settings", &menuVisible)) {
            ImGui::Text("Fallout 4 VR DLSS4 Upscaler");
            ImGui::Text("Version 1.0.0 - RTX 40/50 Series");
            ImGui::Separator();

            if (showPerformanceMetrics && ImGui::CollapsingHeader("Performance Metrics", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("FPS: %.1f", fps);
                ImGui::Text("Frame Time: %.2f ms", frameTime);
                ImGui::Text("GPU Usage: %.1f%%", gpuUsage);

                if (F4SEVR_Upscaler* upscaler = F4SEVR_Upscaler::GetSingleton()) {
                    ImGui::Text("Display: %dx%d", upscaler->GetDisplayWidth(), upscaler->GetDisplayHeight());
                    ImGui::Text("Render: %dx%d",
                        static_cast<int>(upscaler->GetDisplayWidth() * 0.75f),
                        static_cast<int>(upscaler->GetDisplayHeight() * 0.75f));
                }

                ImGui::Separator();
            }

            if (ImGui::CollapsingHeader("Upscaler Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Checkbox("Enable Upscaler", &enableUpscalerSetting)) {
                    ApplyUpscalerChange();
                }

                const char* upscalerTypes[] = {
                    "DLSS",
                    "FSR2",
                    "XeSS",
                    "DLAA",
                    "DLSS4 (Multi Frame Gen)",
                    "TAA (Native)"
                };

                if (ImGui::Combo("Upscaler Type", &currentUpscaler, upscalerTypes, IM_ARRAYSIZE(upscalerTypes))) {
                    ApplyUpscalerChange();
                }

                const char* qualityLevels[] = {
                    "Performance",
                    "Balanced",
                    "Quality",
                    "Ultra Performance",
                    "Ultra Quality",
                    "Native (DLAA)"
                };

                if (ImGui::Combo("Quality Level", &currentQuality, qualityLevels, IM_ARRAYSIZE(qualityLevels))) {
                    ApplyQualityChange();
                }

                if (ImGui::Checkbox("Enable Sharpening", &sharpeningEnabled)) {
                    ApplySharpnessChange();
                }

                if (ImGui::SliderFloat("Sharpness", &sharpness, 0.0f, 1.0f, "%.2f")) {
                    ApplySharpnessChange();
                }

                if (ImGui::Checkbox("Use Optimal Mip LOD Bias", &useOptimalMip)) {
                    ApplyAdvancedSettings();
                }

                ImGui::BeginDisabled(useOptimalMip);
                if (ImGui::SliderFloat("Mip LOD Bias", &mipLodBiasSetting, -3.0f, 3.0f, "%.3f")) {
                    ApplyAdvancedSettings();
                }
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Early DLSS (Experimental)", 0)) {
                ImGui::TextWrapped("Render-time DLSS integration to reduce shading resolution.\n"
                                   "Faz 1 (Viewport clamp) and Faz 2 (RT redirect) are guarded by flags.");
                if (ImGui::Checkbox("Enable Early DLSS", &earlyDlssEnabledSetting)) {
                    WriteSettingsToConfig(false);
                }
                const char* earlyModes[] = { "Viewport clamp", "RT redirect" };
                if (ImGui::Combo("Mode", &earlyDlssModeSetting, earlyModes, IM_ARRAYSIZE(earlyModes))) {
                    WriteSettingsToConfig(false);
                }
                if (ImGui::Checkbox("Debug logs (low rate)", &debugEarlyDlssSetting)) {
                    WriteSettingsToConfig(false);
                }
                ImGui::Separator();
                ImGui::TextDisabled("Note: Phase 0 instrumentation only (no behavior change).");
            }

            if (ImGui::CollapsingHeader("Advanced Rendering", showAdvancedSettings ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                if (ImGui::Checkbox("Render ReShade before Upscaling", &renderReShadeBeforeUpscalingSetting)) {
                    ApplyAdvancedSettings();
                }
                if (ImGui::Checkbox("Upscale Depth for ReShade", &upscaleDepthForReShadeSetting)) {
                    ApplyAdvancedSettings();
                }
                if (ImGui::Checkbox("Use TAA for Periphery", &useTAAForPeripherySetting)) {
                    ApplyAdvancedSettings();
                }
                if (ImGui::SliderInt("DLSS Preset", &dlssPresetSetting, 0, 7)) {
                    ApplyAdvancedSettings();
                }
                if (ImGui::SliderFloat("Field of View", &fovSetting, 70.0f, 120.0f, "%.1f")) {
                    ApplyAdvancedSettings();
                }
            }

            if (ImGui::CollapsingHeader("Fixed Foveated Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Checkbox("Enable Fixed Foveated Rendering", &enableFixedFoveated)) {
                    ApplyFoveatedSettings();
                }
                if (ImGui::Checkbox("Enable Foveated Upscaling", &enableFixedFoveatedUpscaling)) {
                    ApplyFoveatedSettings();
                }
                if (ImGui::SliderFloat("Inner Radius", &foveatedInnerRadius, 0.0f, 1.0f, "%.2f")) {
                    ApplyFoveatedSettings();
                }
                if (ImGui::SliderFloat("Middle Radius", &foveatedMiddleRadius, 0.0f, 1.0f, "%.2f")) {
                    ApplyFoveatedSettings();
                }
                if (ImGui::SliderFloat("Outer Radius", &foveatedOuterRadius, 0.0f, 1.0f, "%.2f")) {
                    ApplyFoveatedSettings();
                }
                if (ImGui::SliderFloat("Scale X", &foveatedScaleX, 0.1f, 1.5f, "%.2f")) {
                    ApplyFoveatedSettings();
                }
                if (ImGui::SliderFloat("Scale Y", &foveatedScaleY, 0.1f, 1.5f, "%.2f")) {
                    ApplyFoveatedSettings();
                }
                if (ImGui::SliderFloat("Offset X", &foveatedOffsetX, -0.5f, 0.5f, "%.2f")) {
                    ApplyFoveatedSettings();
                }
                if (ImGui::SliderFloat("Offset Y", &foveatedOffsetY, -0.5f, 0.5f, "%.2f")) {
                    ApplyFoveatedSettings();
                }
                if (ImGui::SliderFloat("Cutout Radius", &foveatedCutoutRadius, 0.5f, 2.0f, "%.2f")) {
                    ApplyFoveatedSettings();
                }
                if (ImGui::SliderFloat("Widen Factor", &foveatedWiden, 1.0f, 2.5f, "%.2f")) {
                    ApplyFoveatedSettings();
                }
            }

            ImGui::Separator();

            if (ImGui::Button("Save Settings")) {
                SaveSettings();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset to Defaults")) {
                ResetToDefaults();
            }

            ImGui::Spacing();
            ImGui::TextColored(enableUpscalerSetting ? colorGreen : colorRed,
                enableUpscalerSetting ? "DLSS4 Active" : "DLSS4 Disabled");

        }
        ImGui::End();
        ImGui::GetIO().MouseDrawCursor = menuVisible;
    }

    void UpdatePerformanceMetrics(float deltaTimeMs) {
        if (deltaTimeMs > 0.0f) {
            fps = 1000.0f / deltaTimeMs;
            frameTime = deltaTimeMs;
        }

        gpuUsage = 75.0f;
    }

    void ProcessHotkeys() {
        if (hotkeysDirty) {
            UpdateHotkeyBindings();
        }

        HandleHotkey(menuHotkey, [this]() { ToggleMenu(); });
        HandleHotkey(toggleHotkey, [this]() { ToggleUpscaler(); });
        HandleHotkey(cycleQualityHotkey, [this]() { CycleQuality(); });
        HandleHotkey(cycleUpscalerHotkey, [this]() { CycleUpscaler(); });
    }

private:
    void ApplyUpscalerChange() {
        if (g_dlssManager) {
            g_dlssManager->SetEnabled(enableUpscalerSetting);
            g_dlssManager->SetQuality(static_cast<DLSSManager::Quality>(currentQuality));
        }
        WriteSettingsToConfig(false);
    }

    void ApplyQualityChange() {
        if (g_dlssManager) {
            g_dlssManager->SetQuality(static_cast<DLSSManager::Quality>(currentQuality));
        }
        WriteSettingsToConfig(false);
    }

    void ApplySharpnessChange() {
        if (g_dlssManager) {
            g_dlssManager->SetSharpeningEnabled(sharpeningEnabled);
            g_dlssManager->SetSharpness(sharpness);
        }
        WriteSettingsToConfig(false);
    }

    void ApplyAdvancedSettings() {
        if (g_dlssManager) {
            g_dlssManager->SetUseOptimalMipLodBias(useOptimalMip);
            g_dlssManager->SetManualMipLodBias(mipLodBiasSetting);
            g_dlssManager->SetRenderReShadeBeforeUpscaling(renderReShadeBeforeUpscalingSetting);
            g_dlssManager->SetUpscaleDepthForReShade(upscaleDepthForReShadeSetting);
            g_dlssManager->SetUseTAAPeriphery(useTAAForPeripherySetting);
            g_dlssManager->SetDLSSPreset(dlssPresetSetting);
            g_dlssManager->SetFOV(fovSetting);
        }
        WriteSettingsToConfig(false);
    }

    void ApplyFoveatedSettings() {
        if (g_dlssManager) {
            g_dlssManager->SetFixedFoveatedRendering(enableFixedFoveated);
            g_dlssManager->SetFixedFoveatedUpscaling(enableFixedFoveatedUpscaling);
            g_dlssManager->SetFoveatedRadii(foveatedInnerRadius, foveatedMiddleRadius, foveatedOuterRadius);
            g_dlssManager->SetFoveatedScale(foveatedScaleX, foveatedScaleY);
            g_dlssManager->SetFoveatedOffsets(foveatedOffsetX, foveatedOffsetY);
            g_dlssManager->SetFoveatedCutout(foveatedCutoutRadius);
            g_dlssManager->SetFoveatedWiden(foveatedWiden);
        }
        WriteSettingsToConfig(false);
    }

    void ApplyAllSettings() {
        ApplyUpscalerChange();
        ApplySharpnessChange();
        ApplyAdvancedSettings();
        ApplyFoveatedSettings();
    }

    void ResetToDefaults() {
        DLSSConfig defaults;
        enableUpscalerSetting = defaults.enableUpscaler;
        currentUpscaler = static_cast<int>(defaults.upscalerType);
        currentQuality = static_cast<int>(defaults.quality);
        sharpeningEnabled = defaults.enableSharpening;
        sharpness = defaults.sharpness;
        useOptimalMip = defaults.useOptimalMipLodBias;
        mipLodBiasSetting = defaults.mipLodBias;
        renderReShadeBeforeUpscalingSetting = defaults.renderReShadeBeforeUpscaling;
        upscaleDepthForReShadeSetting = defaults.upscaleDepthForReShade;
        useTAAForPeripherySetting = defaults.useTAAForPeriphery;
        dlssPresetSetting = defaults.dlssPreset;
        fovSetting = defaults.fov;
        enableFixedFoveated = defaults.enableFixedFoveatedRendering;
        enableFixedFoveatedUpscaling = defaults.enableFixedFoveatedUpscaling;
        foveatedInnerRadius = defaults.foveatedInnerRadius;
        foveatedMiddleRadius = defaults.foveatedMiddleRadius;
        foveatedOuterRadius = defaults.foveatedOuterRadius;
        foveatedScaleX = defaults.foveatedScaleX;
        foveatedScaleY = defaults.foveatedScaleY;
        foveatedOffsetX = defaults.foveatedOffsetX;
        foveatedOffsetY = defaults.foveatedOffsetY;
        foveatedCutoutRadius = defaults.foveatedCutoutRadius;
        foveatedWiden = defaults.foveatedWiden;

        ApplyAllSettings();
        WriteSettingsToConfig(true);
        hotkeysDirty = true;
    }

    void SaveSettings() {
        ApplyAllSettings();
        WriteSettingsToConfig(true);
    }

    void ToggleUpscaler() {
        enableUpscalerSetting = !enableUpscalerSetting;
        ApplyUpscalerChange();
        WriteSettingsToConfig(true);

        if (FILE* log = fopen("F4SEVR_DLSS.log", "a")) {
            fprintf(log, "Upscaler toggled: %s\n", enableUpscalerSetting ? "ON" : "OFF");
            fclose(log);
        }
    }

    void CycleQuality() {
        currentQuality = (currentQuality + 1) % 6;
        ApplyQualityChange();
        WriteSettingsToConfig(true);

        if (FILE* log = fopen("F4SEVR_DLSS.log", "a")) {
            fprintf(log, "Quality cycled to: %d\n", currentQuality);
            fclose(log);
        }
    }

    void CycleUpscaler() {
        if (FILE* log = fopen("F4SEVR_DLSS.log", "a")) {
            fprintf(log, "Upscaler cycle requested – alternative upscalers not implemented\n");
            fclose(log);
        }
    }

    void UpdateHotkeyBindings() {
        auto fallback = [](int value, int fallbackKey) {
            return value != 0 ? value : fallbackKey;
        };

        if (g_dlssConfig) {
            menuHotkey.virtualKey = fallback(g_dlssConfig->toggleMenuKey, 0x47);
            toggleHotkey.virtualKey = fallback(g_dlssConfig->toggleUpscalerKey, VK_MULTIPLY);
            cycleQualityHotkey.virtualKey = fallback(g_dlssConfig->cycleQualityKey, VK_HOME);
            cycleUpscalerHotkey.virtualKey = fallback(g_dlssConfig->cycleUpscalerKey, VK_INSERT);
        } else {
            menuHotkey.virtualKey = 0x47;
            toggleHotkey.virtualKey = VK_MULTIPLY;
            cycleQualityHotkey.virtualKey = VK_HOME;
            cycleUpscalerHotkey.virtualKey = VK_INSERT;
        }

        menuHotkey.latched = false;
        toggleHotkey.latched = false;
        cycleQualityHotkey.latched = false;
        cycleUpscalerHotkey.latched = false;
        hotkeysDirty = false;
    }

    void HandleHotkey(HotkeyBinding& binding, const std::function<void()>& action) {
        if (binding.virtualKey == 0) {
            binding.latched = false;
            return;
        }

        SHORT state = GetAsyncKeyState(binding.virtualKey);
        bool pressed = (state & 0x8000) != 0;
        if (pressed && !binding.latched) {
            action();
            binding.latched = true;
        } else if (!pressed) {
            binding.latched = false;
        }
    }

    void WriteSettingsToConfig(bool persist) {
        if (!g_dlssConfig) {
            return;
        }

        g_dlssConfig->enableUpscaler = enableUpscalerSetting;
        g_dlssConfig->upscalerType = static_cast<DLSSConfig::UpscalerType>(currentUpscaler);
        g_dlssConfig->quality = static_cast<DLSSManager::Quality>(currentQuality);
        g_dlssConfig->enableSharpening = sharpeningEnabled;
        g_dlssConfig->sharpness = sharpness;
        g_dlssConfig->useOptimalMipLodBias = useOptimalMip;
        g_dlssConfig->mipLodBias = mipLodBiasSetting;
        g_dlssConfig->renderReShadeBeforeUpscaling = renderReShadeBeforeUpscalingSetting;
        g_dlssConfig->upscaleDepthForReShade = upscaleDepthForReShadeSetting;
        g_dlssConfig->useTAAForPeriphery = useTAAForPeripherySetting;
        g_dlssConfig->dlssPreset = dlssPresetSetting;
        g_dlssConfig->fov = fovSetting;
        g_dlssConfig->enableFixedFoveatedRendering = enableFixedFoveated;
        g_dlssConfig->enableFixedFoveatedUpscaling = enableFixedFoveatedUpscaling;
        g_dlssConfig->foveatedInnerRadius = foveatedInnerRadius;
        g_dlssConfig->foveatedMiddleRadius = foveatedMiddleRadius;
        g_dlssConfig->foveatedOuterRadius = foveatedOuterRadius;
        g_dlssConfig->foveatedScaleX = foveatedScaleX;
        g_dlssConfig->foveatedScaleY = foveatedScaleY;
        g_dlssConfig->foveatedOffsetX = foveatedOffsetX;
        g_dlssConfig->foveatedOffsetY = foveatedOffsetY;
        g_dlssConfig->foveatedCutoutRadius = foveatedCutoutRadius;
        g_dlssConfig->foveatedWiden = foveatedWiden;

        // Early DLSS flags
        g_dlssConfig->earlyDlssEnabled = earlyDlssEnabledSetting;
        g_dlssConfig->earlyDlssMode = earlyDlssModeSetting;
        g_dlssConfig->debugEarlyDlss = debugEarlyDlssSetting;

        if (persist) {
            g_dlssConfig->Save();
        }
    }
};

extern "C" {

bool InitializeImGuiMenu() {
    return ImGuiMenu::GetInstance().Initialize();
}

void RenderImGuiMenu() {
    ImGuiMenu::GetInstance().Render();
}

void ProcessImGuiHotkeys() {
    ImGuiMenu::GetInstance().ProcessHotkeys();
}

void UpdateImGuiMetrics(float deltaTime) {
    ImGuiMenu::GetInstance().UpdatePerformanceMetrics(deltaTime);
}

bool IsImGuiMenuVisible() {
    return ImGuiMenu::GetInstance().IsVisible();
}

void ToggleImGuiMenu() {
    ImGuiMenu::GetInstance().ToggleMenu();
}

void SyncImGuiMenuFromConfig() {
    ImGuiMenu::GetInstance().SyncFromConfig();
}

}

