#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>

// Function to install all DLSS hooks
#ifdef __cplusplus
extern "C" {
#endif
bool InstallDLSSHooks();
void SetOverlaySafeMode(bool enabled);
#ifdef __cplusplus
}
#endif

// Hook functions
namespace DLSSHooks {
    // D3D11 Present hook for intercepting frame presentation
    typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
    extern PFN_Present RealPresent;
    HRESULT WINAPI HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
    
    // D3D11 ResizeBuffers hook for handling resolution changes
    typedef HRESULT(WINAPI* PFN_ResizeBuffers)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
    extern PFN_ResizeBuffers RealResizeBuffers;
    HRESULT WINAPI HookedResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

    typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture2D)(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initialData, ID3D11Texture2D** texture);
    extern PFN_CreateTexture2D RealCreateTexture2D;
    HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initialData, ID3D11Texture2D** texture);

    // DXGI Factory early hook to observe swapchain creation
    typedef HRESULT(STDMETHODCALLTYPE* PFN_FactoryCreateSwapChain)(IDXGIFactory* factory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
    extern PFN_FactoryCreateSwapChain RealFactoryCreateSwapChain;
    HRESULT STDMETHODCALLTYPE HookedFactoryCreateSwapChain(IDXGIFactory* factory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);

    // Context hooks for early DLSS (Phase 1, viewport clamp)
    typedef void (STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D11DeviceContext* ctx, UINT count, const D3D11_VIEWPORT* viewports);
    typedef void (STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D11DeviceContext* ctx, UINT numRTVs, ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV);
    extern PFN_RSSetViewports RealRSSetViewports;
    extern PFN_OMSetRenderTargets RealOMSetRenderTargets;
    void STDMETHODCALLTYPE HookedRSSetViewports(ID3D11DeviceContext* ctx, UINT count, const D3D11_VIEWPORT* viewports);
    void STDMETHODCALLTYPE HookedOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT numRTVs, ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV);

    void RegisterMotionVectorTexture(ID3D11Texture2D* motionTexture);
    void RegisterFallbackDepthTexture(ID3D11Texture2D* depthTexture,
                                      const D3D11_TEXTURE2D_DESC* desc = nullptr,
                                      UINT targetWidth = 0,
                                      UINT targetHeight = 0);

    // VR specific - intercept eye texture submission
    void ProcessVREyeTexture(ID3D11Texture2D* eyeTexture, bool isLeftEye);
    
    // Helper functions
    bool GetD3D11Device(ID3D11Device** ppDevice, ID3D11DeviceContext** ppContext);
    IDXGISwapChain* GetSwapChain();

    // Per-eye display size (output) detected from OpenVR Submit bounds (0=Left,1=Right)
    bool GetPerEyeDisplaySize(int eyeIndex, uint32_t& outW, uint32_t& outH);
}




