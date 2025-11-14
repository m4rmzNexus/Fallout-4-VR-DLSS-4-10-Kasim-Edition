#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <memory>

// Upscaler Types
enum UpscalerType {
    UPSCALER_DLSS = 0,
    UPSCALER_FSR2 = 1,
    UPSCALER_XESS = 2,
    UPSCALER_DLAA = 3,
    UPSCALER_DLSS4 = 4,  // New DLSS4 with Multi Frame Generation
    UPSCALER_TAA = 5
};

// Quality Levels
enum QualityLevel {
    QUALITY_PERFORMANCE = 0,
    QUALITY_BALANCED = 1,
    QUALITY_QUALITY = 2,
    QUALITY_ULTRA_PERFORMANCE = 3,
    QUALITY_ULTRA_QUALITY = 4,
    QUALITY_NATIVE = 5  // DLSS Native = DLAA
};

// Image wrapper for texture management
class ImageWrapper {
public:
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    
    ID3D11RenderTargetView* GetRTV();
    ID3D11ShaderResourceView* GetSRV();
    ID3D11DepthStencilView* GetDSV();
    void Release();
};

// Main Upscaler class
class F4SEVR_Upscaler {
private:
    static F4SEVR_Upscaler* instance;
    
    // D3D11 resources
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    
    // Display settings
    unsigned int displayWidth = 0;
    unsigned int displayHeight = 0;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    
    // Upscaler settings
    UpscalerType currentUpscaler = UPSCALER_DLSS;
    QualityLevel currentQuality = QUALITY_QUALITY;
    float sharpness = 0.8f;
    float mipLodBias = -1.0f;
    bool useOptimalMipLodBias = true;
    
    // VR specific
    bool isVR = false;
    bool useTAAForPeriphery = false;
    bool enableFixedFoveatedRendering = true;
    float foveatedScaleX = 0.8f;
    float foveatedScaleY = 0.6f;
    float foveatedOffsetX = -0.05f;
    float foveatedOffsetY = 0.04f;
    
    // Buffers
    ImageWrapper colorBuffer;
    ImageWrapper depthBuffer;
    ImageWrapper motionVectorBuffer;
    ImageWrapper transparentMask;
    ImageWrapper opaqueColor;
    ImageWrapper outputBuffer;

    ID3D11Texture2D* depthCopyTexture = nullptr;
    ID3D11Texture2D* motionVectorCopyTexture = nullptr;
    ID3D11Texture2D* CopyTextureToSRV(ID3D11Texture2D* source, ID3D11Texture2D*& cache, bool* outRecreated = nullptr);
    void ReleaseCopyTexture(ID3D11Texture2D*& cache);

    // DLSS specific
    void* dlssHandle = nullptr;
    bool dlssInitialized = false;
    
    // Hook management
    std::unordered_set<ID3D11SamplerState*> passThroughSamplers;
    std::unordered_map<ID3D11SamplerState*, ID3D11SamplerState*> mappedSamplers;
    
public:
    static F4SEVR_Upscaler* GetSingleton();
    
    F4SEVR_Upscaler();
    ~F4SEVR_Upscaler();
    
    // Initialization
    bool Initialize(ID3D11Device* device, IDXGISwapChain* swapChain);
    void Shutdown();
    
    // Upscaler management
    bool InitializeUpscaler();
    void ShutdownUpscaler();
    bool SwitchUpscaler(UpscalerType type);
    bool SetQuality(QualityLevel quality);
    
    // Frame processing
    void ProcessFrame();
    void OnPresent();
    void OnCreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, ID3D11Texture2D* texture);
    
    // Buffer setup
    void SetupColorBuffer(ID3D11Texture2D* texture);
    void SetupDepthBuffer(ID3D11Texture2D* texture);
    void SetupMotionVector(ID3D11Texture2D* texture);
    void SetupTransparentMask(ID3D11Texture2D* texture);
    void SetupOpaqueColor(ID3D11Texture2D* texture);
    
    // Settings
    void LoadSettings();
    void SaveSettings();
    
    // VR specific
    void SetVRMode(bool vr) { isVR = vr; }
    bool IsVR() const { return isVR; }
    void ApplyFixedFoveatedRendering();
    
    // Getters
    ID3D11Device* GetDevice() { return device; }
    ID3D11DeviceContext* GetContext() { return context; }
    unsigned int GetDisplayWidth() { return displayWidth; }
    unsigned int GetDisplayHeight() { return displayHeight; }
    float GetMipLodBias() { return mipLodBias; }
};
