#pragma once
#include <d3d11.h>
#include <windows.h>
#include <cstdint>

// Forward declarations
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGISwapChain;
struct ID3D11Resource;

// NVIDIA NGX SDK forward declarations
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

// Forward declare upscaler backend interface (Streamline)
class IUpscaleBackend;
#if USE_STREAMLINE
class SLBackend;
#endif

class DLSSManager {
public:
    DLSSManager();
    ~DLSSManager();
    
    bool Initialize();
    void Shutdown();
    
    // VR specific - process each eye separately
    ID3D11Texture2D* ProcessLeftEye(ID3D11Texture2D* inputTexture, ID3D11Texture2D* depthTexture, ID3D11Texture2D* motionVectors);
    ID3D11Texture2D* ProcessRightEye(ID3D11Texture2D* inputTexture, ID3D11Texture2D* depthTexture, ID3D11Texture2D* motionVectors);
    
    // Configuration
    enum class Quality {
        Performance = 0,      // 50% render scale (4x pixels)
        Balanced = 1,        // 58% render scale (3x pixels)  
        Quality = 2,         // 67% render scale (2.25x pixels)
        UltraPerformance = 3, // 33% render scale (9x pixels)
        UltraQuality = 4,    // 77% render scale (1.7x pixels)
        DLAA = 5            // 100% render scale (native AA)
    };
    
    void SetQuality(Quality quality);
    Quality GetQuality() const { return m_quality; }
    
    void SetSharpness(float sharpness);
    float GetSharpness() const { return m_sharpness; }
    
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    
    // DLSS 4 specific features (without frame generation)
    void SetTransformerModel(bool enabled) { m_useTransformerModel = enabled; }
    void SetRayReconstruction(bool enabled) { m_rayReconstructionEnabled = enabled; }
    
    void SetSharpeningEnabled(bool enabled);
    void SetUseOptimalMipLodBias(bool enabled);
    void SetManualMipLodBias(float bias);
    void SetRenderReShadeBeforeUpscaling(bool value);
    void SetUpscaleDepthForReShade(bool value);
    void SetUseTAAPeriphery(bool value);
    void SetDLSSPreset(int preset);
    void SetFOV(float value);
    void SetFixedFoveatedRendering(bool enabled);
    void SetFixedFoveatedUpscaling(bool enabled);
    void SetFoveatedRadii(float inner, float middle, float outer);
    void SetFoveatedScale(float scaleX, float scaleY);
    void SetFoveatedOffsets(float offsetX, float offsetY);
    void SetFoveatedCutout(float cutoutRadius);
    void SetFoveatedWiden(float widen);

    // Compute the DLSS render size for a given per-eye output size according to
    // current quality/mode. Uses Streamline OptimalSettings when available; falls
    // back to the static quality scale table otherwise. Returns true on success.
    bool ComputeRenderSizeForOutput(uint32_t outW, uint32_t outH, uint32_t& renderW, uint32_t& renderH);

    // Utility: blit a source texture into a destination RTV at given size using
    // the internal fullscreen VS/PS (linear sampling). Saves/restores minimal state.
    bool BlitToRTV(ID3D11Texture2D* src, ID3D11RenderTargetView* dstRTV, uint32_t dstW, uint32_t dstH);

private:
    // Per-eye DLSS contexts for VR
    struct EyeContext {
        NVSDK_NGX_Handle* dlssHandle = nullptr;
        ID3D11Texture2D* outputTexture = nullptr;
        // Render-size color (downscaled from eye input) used as DLSS input
        ID3D11Texture2D* renderColor = nullptr;
        ID3D11RenderTargetView* renderColorRTV = nullptr;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        bool requiresReset = true;
    };
    
    bool InitializeDevice();
    bool InitializeNGX();
    bool CreateDLSSFeatures();
    void GetOptimalSettings(uint32_t& renderWidth, uint32_t& renderHeight);
    bool EnsureEyeFeature(EyeContext& eye, ID3D11Texture2D* inputTexture, uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight);
    ID3D11Texture2D* ProcessEye(EyeContext& eye, ID3D11Texture2D* inputTexture, ID3D11Texture2D* depthTexture, ID3D11Texture2D* motionVectors, bool forceReset);
    bool CreateScratchBuffer(size_t scratchSize);
    void ReleaseScratchBuffer();
    void ReleaseZeroMotionVectors();
    bool EnsureZeroMotionVectors(uint32_t width, uint32_t height);
    bool EnsureZeroDepthTexture(uint32_t width, uint32_t height);
    void ReleaseZeroDepthTexture();
    void ReleaseEyeRender(EyeContext& eye);
    bool EnsureDownscaleShaders();
    // Downscale/crop inputTexture into eye.renderColor at renderWidth/renderHeight.
    // UV window selects a sub-rectangle of the source: uv = offset + uv * scale.
    bool DownscaleToRender(EyeContext& eye,
                           ID3D11Texture2D* inputTexture,
                           uint32_t renderWidth,
                           uint32_t renderHeight,
                           float uvOffsetX,
                           float uvOffsetY,
                           float uvScaleX,
                           float uvScaleY);

    EyeContext m_leftEye;
    EyeContext m_rightEye;
    
    // D3D11 resources
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    
    // DLSS parameters
    NVSDK_NGX_Parameter* m_ngxParameters = nullptr;
    
    // Settings
    bool m_enabled = true;
    bool m_initialized = false;
    Quality m_quality = Quality::Quality;
    float m_sharpness = 0.5f;
    
    // DLSS 4 features
    bool m_useTransformerModel = true;  // New transformer-based model in DLSS 4
    bool m_rayReconstructionEnabled = false;
    
    // VR specific
    uint32_t m_renderWidth = 2016;   // Typical F4VR per-eye width
    uint32_t m_renderHeight = 2240;  // Typical F4VR per-eye height

    ID3D11Resource* m_scratchBuffer = nullptr;
    size_t m_scratchSize = 0;
    ID3D11Texture2D* m_zeroMotionVectors = nullptr;
    uint32_t m_zeroMVWidth = 0;
    uint32_t m_zeroMVHeight = 0;
    ID3D11Texture2D* m_zeroDepthTexture = nullptr;
    uint32_t m_zeroDepthWidth = 0;
    uint32_t m_zeroDepthHeight = 0;

    // Simple downscale pipeline (fullscreen triangle)
    ID3D11VertexShader* m_fsVS = nullptr;
    ID3D11PixelShader* m_fsPS = nullptr;
    ID3D11SamplerState* m_linearSampler = nullptr;
    ID3D11Buffer* m_fsCB = nullptr;

    // Extended configuration state
    bool m_sharpeningEnabled = true;
    bool m_useOptimalMipLodBias = true;
    float m_manualMipLodBias = -1.585315f;
    bool m_renderReShadeBeforeUpscaling = true;
    bool m_upscaleDepthForReShade = false;
    bool m_useTAAPeriphery = false;
    bool m_enableFixedFoveatedRendering = true;
    bool m_enableFixedFoveatedUpscaling = false;
    float m_foveatedScaleX = 0.8f;
    float m_foveatedScaleY = 0.6f;
    float m_foveatedOffsetX = -0.05f;
    float m_foveatedOffsetY = 0.04f;
    float m_foveatedCutoutRadius = 1.2f;
    float m_foveatedWiden = 1.5f;
    float m_foveatedInnerRadius = 0.8f;
    float m_foveatedMiddleRadius = 0.85f;
    float m_foveatedOuterRadius = 0.9f;
    int m_dlssPreset = 4;
    float m_fov = 90.0f;

    // Optional Streamline backend
    IUpscaleBackend* m_backend = nullptr;
#if USE_STREAMLINE
    SLBackend* m_slBackend = nullptr;
#endif
};
