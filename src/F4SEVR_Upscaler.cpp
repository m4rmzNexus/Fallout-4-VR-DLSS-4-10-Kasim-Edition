#include "F4SEVR_Upscaler.h"
#include "../dlss_hooks.h"
#include "../dlss_config.h"

#include <stdio.h>
#include <string>
#include <fstream>
#include "LogUtils.h"

F4SEVR_Upscaler* F4SEVR_Upscaler::instance = nullptr;

F4SEVR_Upscaler* F4SEVR_Upscaler::GetSingleton() {
    if (!instance) {
        instance = new F4SEVR_Upscaler();
    }
    return instance;
}

F4SEVR_Upscaler::F4SEVR_Upscaler() {
    LoadSettings();
}

F4SEVR_Upscaler::~F4SEVR_Upscaler() {
    Shutdown();
}

bool F4SEVR_Upscaler::Initialize(ID3D11Device* d3dDevice, IDXGISwapChain* dxgiSwapChain) {
    device = d3dDevice;
    swapChain = dxgiSwapChain;
    
    if (device) {
        device->GetImmediateContext(&context);
    }
    
    // Get display dimensions from swap chain
    if (swapChain) {
        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(swapChain->GetDesc(&desc))) {
            displayWidth = desc.BufferDesc.Width;
            displayHeight = desc.BufferDesc.Height;
            
            // Calculate render resolution based on quality
            switch (currentQuality) {
                case QUALITY_PERFORMANCE:
                    renderWidth = displayWidth / 2;
                    renderHeight = displayHeight / 2;
                    break;
                case QUALITY_BALANCED:
                    renderWidth = (unsigned int)(displayWidth * 0.67f);
                    renderHeight = (unsigned int)(displayHeight * 0.67f);
                    break;
                case QUALITY_QUALITY:
                    renderWidth = (unsigned int)(displayWidth * 0.75f);
                    renderHeight = (unsigned int)(displayHeight * 0.75f);
                    break;
                case QUALITY_ULTRA_PERFORMANCE:
                    renderWidth = displayWidth / 3;
                    renderHeight = displayHeight / 3;
                    break;
                case QUALITY_ULTRA_QUALITY:
                    renderWidth = (unsigned int)(displayWidth * 0.85f);
                    renderHeight = (unsigned int)(displayHeight * 0.85f);
                    break;
                case QUALITY_NATIVE:
                    renderWidth = displayWidth;
                    renderHeight = displayHeight;
                    break;
            }
        }
    }
    
    // Check if running in VR mode
    HMODULE vrModule = GetModuleHandleA("openvr_api.dll");
    if (vrModule) {
        isVR = true;
        #include "LogUtils.h"
        LogUtils::Logf("VR Mode detected - OpenVR API found");
    }
    
    return InitializeUpscaler();
}

void F4SEVR_Upscaler::Shutdown() {
    ShutdownUpscaler();

    colorBuffer.Release();
    depthBuffer.Release();
    motionVectorBuffer.Release();
    transparentMask.Release();
    opaqueColor.Release();
    outputBuffer.Release();
    ReleaseCopyTexture(depthCopyTexture);
    ReleaseCopyTexture(motionVectorCopyTexture);

    DLSSHooks::RegisterMotionVectorTexture(nullptr, nullptr, 0, 0);
    DLSSHooks::RegisterFallbackDepthTexture(nullptr, nullptr, 0, 0);

    if (context) {
        context->Release();
        context = nullptr;
    }
}

bool F4SEVR_Upscaler::InitializeUpscaler() {
    #include "LogUtils.h"
    LogUtils::Logf("Initializing upscaler - Type: %d, Quality: %d", currentUpscaler, currentQuality);
    LogUtils::Logf("Display: %dx%d, Render: %dx%d", displayWidth, displayHeight, renderWidth, renderHeight);
    
    // Initialize based on selected upscaler
    switch (currentUpscaler) {
        case UPSCALER_DLSS:
        case UPSCALER_DLSS4:
            // TODO: Initialize NVIDIA NGX for DLSS
            dlssInitialized = false;
            break;
        case UPSCALER_FSR2:
            // TODO: Initialize AMD FSR2
            break;
        case UPSCALER_XESS:
            // TODO: Initialize Intel XeSS
            break;
        case UPSCALER_DLAA:
            // DLAA is DLSS at native resolution
            currentQuality = QUALITY_NATIVE;
            dlssInitialized = false;
            break;
        case UPSCALER_TAA:
            // Use game's native TAA
            break;
    }
    
    return true;
}

void F4SEVR_Upscaler::ShutdownUpscaler() {
    if (dlssInitialized) {
        // TODO: Cleanup DLSS
        dlssInitialized = false;
    }
}

bool F4SEVR_Upscaler::SwitchUpscaler(UpscalerType type) {
    if (type == currentUpscaler) {
        return true;
    }
    
    ShutdownUpscaler();
    currentUpscaler = type;
    return InitializeUpscaler();
}

bool F4SEVR_Upscaler::SetQuality(QualityLevel quality) {
    if (quality == currentQuality) {
        return true;
    }
    
    currentQuality = quality;
    
    // Recalculate render resolution
    switch (currentQuality) {
        case QUALITY_PERFORMANCE:
            renderWidth = displayWidth / 2;
            renderHeight = displayHeight / 2;
            mipLodBias = -1.0f;
            break;
        case QUALITY_BALANCED:
            renderWidth = (unsigned int)(displayWidth * 0.67f);
            renderHeight = (unsigned int)(displayHeight * 0.67f);
            mipLodBias = -0.75f;
            break;
        case QUALITY_QUALITY:
            renderWidth = (unsigned int)(displayWidth * 0.75f);
            renderHeight = (unsigned int)(displayHeight * 0.75f);
            mipLodBias = -0.5f;
            break;
        case QUALITY_ULTRA_PERFORMANCE:
            renderWidth = displayWidth / 3;
            renderHeight = displayHeight / 3;
            mipLodBias = -1.58f;
            break;
        case QUALITY_ULTRA_QUALITY:
            renderWidth = (unsigned int)(displayWidth * 0.85f);
            renderHeight = (unsigned int)(displayHeight * 0.85f);
            mipLodBias = -0.25f;
            break;
        case QUALITY_NATIVE:
            renderWidth = displayWidth;
            renderHeight = displayHeight;
            mipLodBias = 0.0f;
            break;
    }
    
    // Reinitialize upscaler with new settings
    ShutdownUpscaler();
    return InitializeUpscaler();
}

void F4SEVR_Upscaler::ProcessFrame() {
    if (!device || !context) {
        return;
    }

    auto refreshMotion = [&]() {
        if (!motionVectorBuffer.texture) {
            return;
        }
        bool recreated = false;
        ID3D11Texture2D* copy = CopyTextureToSRV(motionVectorBuffer.texture, motionVectorCopyTexture, &recreated);
        if (copy && recreated) {
            D3D11_TEXTURE2D_DESC desc{};
            copy->GetDesc(&desc);
            DLSSHooks::RegisterMotionVectorTexture(copy, &desc, displayWidth, displayHeight);
        }
    };

    auto refreshDepth = [&]() {
        if (!depthBuffer.texture) {
            return;
        }
        bool recreated = false;
        ID3D11Texture2D* copy = CopyTextureToSRV(depthBuffer.texture, depthCopyTexture, &recreated);
        if (copy && recreated) {
            D3D11_TEXTURE2D_DESC desc{};
            copy->GetDesc(&desc);
            DLSSHooks::RegisterFallbackDepthTexture(copy, &desc, displayWidth, displayHeight);
        }
    };

    refreshMotion();
    refreshDepth();

    // Apply fixed foveated rendering for VR
    if (isVR && enableFixedFoveatedRendering) {
        ApplyFixedFoveatedRendering();
    }
    
    // Process based on current upscaler
    switch (currentUpscaler) {
        case UPSCALER_DLSS:
        case UPSCALER_DLSS4:
            // TODO: Execute DLSS
            break;
        case UPSCALER_FSR2:
            // TODO: Execute FSR2
            break;
        case UPSCALER_XESS:
            // TODO: Execute XeSS
            break;
        case UPSCALER_DLAA:
            // TODO: Execute DLAA
            break;
        case UPSCALER_TAA:
            // Let game handle TAA
            break;
    }
}

void F4SEVR_Upscaler::OnPresent() {
    ProcessFrame();
}

void F4SEVR_Upscaler::OnCreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, ID3D11Texture2D* texture) {
    if (!desc || !texture) {
        return;
    }
    
    // Detect motion vectors (R16G16_FLOAT format)
    if (desc->Format == DXGI_FORMAT_R16G16_FLOAT && 
        desc->BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET)) {
        if (desc->Width == displayWidth && desc->Height == displayHeight) {
            SetupMotionVector(texture);
            FILE* log = fopen("F4SEVR_DLSS.log", "a");
            if (log) {
                fprintf(log, "Motion Vector detected: %dx%d\n", desc->Width, desc->Height);
                fclose(log);
            }
        }
    }
    
    // Detect depth buffer
    if (desc->Format >= DXGI_FORMAT_R24G8_TYPELESS && desc->Format <= DXGI_FORMAT_X24_TYPELESS_G8_UINT) {
        if (desc->Width == displayWidth && desc->Height == displayHeight && 
            desc->BindFlags & D3D11_BIND_DEPTH_STENCIL) {
            SetupDepthBuffer(texture);
            FILE* log = fopen("F4SEVR_DLSS.log", "a");
            if (log) {
                fprintf(log, "Depth Buffer detected: %dx%d\n", desc->Width, desc->Height);
                fclose(log);
            }
        }
    }
    
    // Detect color buffer (R11G11B10_FLOAT format)
    if (desc->Format == DXGI_FORMAT_R11G11B10_FLOAT) {
        if (desc->Width == displayWidth && desc->Height == displayHeight) {
            SetupOpaqueColor(texture);
            FILE* log = fopen("F4SEVR_DLSS.log", "a");
            if (log) {
                fprintf(log, "Opaque Color buffer detected: %dx%d\n", desc->Width, desc->Height);
                fclose(log);
            }
        }
    }
}

void F4SEVR_Upscaler::SetupColorBuffer(ID3D11Texture2D* texture) {
    colorBuffer.texture = texture;
    if (texture) {
        texture->AddRef();
    }
}

void F4SEVR_Upscaler::SetupDepthBuffer(ID3D11Texture2D* texture) {
    depthBuffer.texture = texture;
    if (texture) {
        texture->AddRef();
    } else {
        ReleaseCopyTexture(depthCopyTexture);
        DLSSHooks::RegisterFallbackDepthTexture(nullptr, nullptr, 0, 0);
        return;
    }

    bool recreated = false;
    ID3D11Texture2D* copy = CopyTextureToSRV(texture, depthCopyTexture, &recreated);
    if (copy) {
        if (recreated) {
            D3D11_TEXTURE2D_DESC copyDesc{};
            copy->GetDesc(&copyDesc);
            DLSSHooks::RegisterFallbackDepthTexture(copy, &copyDesc, displayWidth, displayHeight);
        }
    } else {
        DLSSHooks::RegisterFallbackDepthTexture(nullptr, nullptr, 0, 0);
    }
}


void F4SEVR_Upscaler::SetupMotionVector(ID3D11Texture2D* texture) {
    motionVectorBuffer.texture = texture;
    if (texture) {
        texture->AddRef();
    } else {
        ReleaseCopyTexture(motionVectorCopyTexture);
        DLSSHooks::RegisterMotionVectorTexture(nullptr, nullptr, 0, 0);
        return;
    }

    bool recreated = false;
    ID3D11Texture2D* copy = CopyTextureToSRV(texture, motionVectorCopyTexture, &recreated);
    if (copy) {
        if (recreated) {
            D3D11_TEXTURE2D_DESC copyDesc{};
            copy->GetDesc(&copyDesc);
            DLSSHooks::RegisterMotionVectorTexture(copy, &copyDesc, displayWidth, displayHeight);
        }
    } else {
        DLSSHooks::RegisterMotionVectorTexture(nullptr, nullptr, 0, 0);
    }
}


void F4SEVR_Upscaler::SetupTransparentMask(ID3D11Texture2D* texture) {
    transparentMask.texture = texture;
    if (texture) {
        texture->AddRef();
    }
}

void F4SEVR_Upscaler::SetupOpaqueColor(ID3D11Texture2D* texture) {
    opaqueColor.texture = texture;
    if (texture) {
        texture->AddRef();
    }
}

void F4SEVR_Upscaler::ApplyFixedFoveatedRendering() {
    // VR-specific fixed foveated rendering implementation
    // Reduces resolution in peripheral vision for better performance
    // TODO: Implement foveated rendering
}

void F4SEVR_Upscaler::LoadSettings() {
    bool isDocs = false, isPlugin = false;
    const std::string configPath = DLSSConfig::ResolveConfigPath(&isDocs, &isPlugin);
    std::ifstream ini(configPath);
    if (!ini.is_open()) {
        if (FILE* log = fopen("F4SEVR_DLSS.log", "a")) {
            fprintf(log, "[INFO] Config file not found; using built-in defaults.\n");
            fclose(log);
        }
        return;
    }
    
    std::string line;
    while (std::getline(ini, line)) {
        if (line.find("mUpscaleType") != std::string::npos) {
            size_t pos = line.find("=");
            if (pos != std::string::npos) {
                int type = std::stoi(line.substr(pos + 1));
                currentUpscaler = (UpscalerType)type;
            }
        }
        else if (line.find("mQualityLevel") != std::string::npos) {
            size_t pos = line.find("=");
            if (pos != std::string::npos) {
                int quality = std::stoi(line.substr(pos + 1));
                currentQuality = (QualityLevel)quality;
            }
        }
        else if (line.find("mSharpness") != std::string::npos) {
            size_t pos = line.find("=");
            if (pos != std::string::npos) {
                sharpness = std::stof(line.substr(pos + 1));
            }
        }
        else if (line.find("mMipLodBias") != std::string::npos) {
            size_t pos = line.find("=");
            if (pos != std::string::npos) {
                mipLodBias = std::stof(line.substr(pos + 1));
            }
        }
        else if (line.find("mEnableFixedFoveatedRendering") != std::string::npos) {
            size_t pos = line.find("=");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                enableFixedFoveatedRendering = (value.find("true") != std::string::npos);
            }
        }
    }
    ini.close();

    if (isPlugin && !isDocs) {
        if (FILE* log = fopen("F4SEVR_DLSS.log", "a")) {
            const std::string target = DLSSConfig::GetDocumentsConfigPath();
            fprintf(log, "[INFO] Using legacy INI from plugin directory. Future saves will write to: %s\n", target.c_str());
            fclose(log);
        }
    }
}

void F4SEVR_Upscaler::SaveSettings() {
    // TODO: Save current settings to INI file
}

// ImageWrapper implementation
ID3D11RenderTargetView* ImageWrapper::GetRTV() {
    if (texture && !rtv) {
        ID3D11Device* device;
        texture->GetDevice(&device);
        device->CreateRenderTargetView(texture, nullptr, &rtv);
    }
    return rtv;
}

ID3D11ShaderResourceView* ImageWrapper::GetSRV() {
    if (texture && !srv) {
        ID3D11Device* device;
        texture->GetDevice(&device);
        device->CreateShaderResourceView(texture, nullptr, &srv);
    }
    return srv;
}

ID3D11DepthStencilView* ImageWrapper::GetDSV() {
    if (texture && !dsv) {
        ID3D11Device* device;
        texture->GetDevice(&device);
        D3D11_DEPTH_STENCIL_VIEW_DESC desc = {};
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        device->CreateDepthStencilView(texture, &desc, &dsv);
    }
    return dsv;
}

void ImageWrapper::Release() {
    if (rtv) {
        rtv->Release();
        rtv = nullptr;
    }
    if (srv) {
        srv->Release();
        srv = nullptr;
    }
    if (dsv) {
        dsv->Release();
        dsv = nullptr;
    }
    if (texture) {
        texture->Release();
        texture = nullptr;
    }
}



ID3D11Texture2D* F4SEVR_Upscaler::CopyTextureToSRV(ID3D11Texture2D* source, ID3D11Texture2D*& cache, bool* outRecreated) {
    if (!source || !device || !context) {
        if (outRecreated) {
            *outRecreated = false;
        }
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    source->GetDesc(&srcDesc);

    D3D11_TEXTURE2D_DESC existingDesc{};
    bool needNew = true;
    if (cache) {
        cache->GetDesc(&existingDesc);
        needNew = existingDesc.Width != srcDesc.Width ||
                  existingDesc.Height != srcDesc.Height ||
                  existingDesc.Format != srcDesc.Format;
    }

    if (needNew) {
        ReleaseCopyTexture(cache);
        D3D11_TEXTURE2D_DESC desc = srcDesc;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &cache))) {
            cache = nullptr;
            if (outRecreated) {
                *outRecreated = false;
            }
            return nullptr;
        }
        if (outRecreated) {
            *outRecreated = true;
        }
    } else if (outRecreated) {
        *outRecreated = false;
    }

    context->CopyResource(cache, source);
    return cache;
}

void F4SEVR_Upscaler::ReleaseCopyTexture(ID3D11Texture2D*& cache) {
    if (cache) {
        cache->Release();
        cache = nullptr;
    }
}
