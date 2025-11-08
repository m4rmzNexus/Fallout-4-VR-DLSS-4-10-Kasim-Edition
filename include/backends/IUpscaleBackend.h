#pragma once

#include <d3d11.h>

class IUpscaleBackend {
public:
    virtual ~IUpscaleBackend() = default;

    virtual bool Init(ID3D11Device* device, ID3D11DeviceContext* context) = 0;
    virtual void Shutdown() = 0;
    virtual bool IsReady() const = 0;

    virtual void SetQuality(int qualityEnum /* engine-specific */) = 0;
    virtual void SetSharpness(float value) = 0;

    virtual ID3D11Texture2D* ProcessEye(ID3D11Texture2D* inputColor,
                                        ID3D11Texture2D* inputDepth,
                                        ID3D11Texture2D* inputMotionVectors,
                                        ID3D11Texture2D* outputTarget,
                                        unsigned int renderWidth,
                                        unsigned int renderHeight,
                                        unsigned int outputWidth,
                                        unsigned int outputHeight,
                                        bool resetHistory) = 0;
};
