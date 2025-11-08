#pragma once

#include <d3d11.h>

#ifdef USE_STREAMLINE
#include <sl.h>
#include <sl_dlss.h>
#include <sl_core_api.h>
#include <sl_core_types.h>
#include <sl_device_wrappers.h>
#endif

#include "common/IDebugLog.h"
#include "backends/IUpscaleBackend.h"

class SLBackend : public IUpscaleBackend {
public:
    SLBackend();
    ~SLBackend() override;

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void Shutdown() override;
    bool IsReady() const override { return m_ready; }

    void SetQuality(int qualityEnum) override;
    void SetSharpness(float value) override;

    ID3D11Texture2D* ProcessEye(ID3D11Texture2D* inputColor,
                                ID3D11Texture2D* inputDepth,
                                ID3D11Texture2D* inputMotionVectors,
                                ID3D11Texture2D* outputTarget,
                                unsigned int renderWidth,
                                unsigned int renderHeight,
                                unsigned int outputWidth,
                                unsigned int outputHeight,
                                bool resetHistory) override;

#ifdef USE_STREAMLINE
    void BeginFrame();
    void EndFrame();
    void SetCurrentEyeIndex(int eyeIndex);
#endif

private:
    bool m_ready = false;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

#ifdef USE_STREAMLINE
    static constexpr int kMaxEyes = 2;
    sl::ViewportHandle m_viewports[kMaxEyes]{};
    sl::DLSSOptions m_options{};
    int m_quality = 2; // default Quality
    float m_sharpness = 0.0f;
    bool m_vpAllocated[kMaxEyes]{};
    unsigned int m_vpInW[kMaxEyes]{};
    unsigned int m_vpInH[kMaxEyes]{};
    unsigned int m_vpOutW[kMaxEyes]{};
    unsigned int m_vpOutH[kMaxEyes]{};
    int m_currentEye = 0;
    sl::FrameToken* m_frameToken = nullptr;
    bool m_frameActive = false;
    uint32_t m_frameEyeCount = 0;
#endif
};

// Implementations moved to SLBackend.cpp
