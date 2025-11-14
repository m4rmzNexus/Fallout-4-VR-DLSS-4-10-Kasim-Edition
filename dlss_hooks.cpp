#include "dlss_hooks.h"
#include "dlss_manager.h"
#include "dlss_config.h"
#include "common/IDebugLog.h"

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_dx11.h"
#include "third_party/imgui/backends/imgui_impl_win32.h"

#include "openvr.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <limits>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <mutex>

extern DLSSManager* g_dlssManager;
extern DLSSConfig* g_dlssConfig;

namespace {
    // Compile-time guard to keep Early DLSS disabled in this build.
    constexpr bool kEarlyDlssFeatureEnabled = false;

    inline bool DlssTraceEnabled() {
        return kEarlyDlssFeatureEnabled && g_dlssConfig && g_dlssConfig->debugEarlyDlss;
    }

    inline bool DlssTraceSampled(uint32_t modulo) {
        if (!DlssTraceEnabled() || modulo == 0) {
            return false;
        }
        static std::atomic<uint32_t> s_traceCounter{0};
        return (s_traceCounter.fetch_add(1, std::memory_order_relaxed) % modulo) == 0;
    }

    inline bool IsEarlyDlssActive() {
        return kEarlyDlssFeatureEnabled && g_dlssConfig && g_dlssConfig->earlyDlssEnabled;
    }
}

#define DLSS_TRACE(fmt, ...)            do { if (DlssTraceEnabled()) _MESSAGE("[DLSS_TRACE] " fmt, __VA_ARGS__); } while (0)
#define DLSS_TRACE_SAMPLED(mod, fmt, ...) do { if (DlssTraceSampled(mod)) _MESSAGE("[DLSS_TRACE] " fmt, __VA_ARGS__); } while (0)

extern "C" {
    bool InitializeImGuiMenu();
    void RenderImGuiMenu();
    void ProcessImGuiHotkeys();
    void UpdateImGuiMetrics(float deltaTime);
    void SyncImGuiMenuFromConfig();
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

template <typename T>
bool HookVTableFunction(void* pVTable, int index, T hookFunc, T* originalFunc);

// Forward declaration so we can call it before its definition
namespace DLSSHooks { void InstallContextHooks(ID3D11DeviceContext* ctx); }

namespace {
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;
    IDXGISwapChain* g_swapChain = nullptr;

    ID3D11Texture2D* g_motionVectorTexture = nullptr;
    ID3D11Texture2D* g_fallbackDepthTexture = nullptr;

    HWND g_imguiWindow = nullptr;
    WNDPROC g_originalWndProc = nullptr;
    bool g_imguiBackendInitialized = false;
    bool g_imguiMenuInitialized = false;
    bool g_overlaySafeMode = false;

    LARGE_INTEGER g_perfFrequency = {};
    // Helper to fetch texture desc from RTV (if possible)
    static bool GetDescFromRTV(ID3D11RenderTargetView* rtv, D3D11_TEXTURE2D_DESC* outDesc) {
        if (!rtv || !outDesc) return false;
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        if (!res) return false;
        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        res->Release();
        if (FAILED(hr) || !tex) return false;
        tex->GetDesc(outDesc);
        tex->Release();
        return true;
    }
    
    LARGE_INTEGER g_lastFrameTime = {};

    bool g_initializedGlobals = false;
    bool g_dlssRuntimeInitialized = false;
    constexpr const char* kTempWindowClass = "TempDLSSWindow";
    bool g_classRegistered = false;
    bool g_presentHookInstalled = false;
    bool g_resizeHookInstalled = false;
    bool g_pendingResizeHook = false;
    bool g_loggedResizeFailure = false;
    bool g_deviceHookInstalled = false;
    ID3D11Device* g_hookedDevice = nullptr;
    bool g_loggedDLSSInitFailure = false;

    std::atomic<bool> g_hookThreadStarted{false};
    std::atomic<bool> g_hookInstallComplete{false};
    std::atomic<bool> g_hookInstallSucceeded{false};

    // DLSS submit readiness guard
    enum class DlssState { Cold = 0, HaveCompositor, HaveSwapChain, HaveDlss, Ready };
    std::atomic<DlssState> g_state{DlssState::Cold};
    static ID3D11Texture2D* g_upscaledEyeTex[2] = { nullptr, nullptr };
    std::atomic<bool> g_lastEvaluateOk{false};
    // Phase 1 (viewport clamp) state
    std::atomic<bool> g_sceneActive{false};
    D3D11_TEXTURE2D_DESC g_sceneRTDesc{};
    int g_clampLogBudgetPerFrame = 4;
    bool g_compositedThisFrame = false;
    // Phase 2 (RT redirect) state and cache
    std::atomic<bool> g_redirectUsedThisFrame{false};
    // Reentrancy guard to avoid recursion while we composite small->big
    static std::atomic<bool> g_inComposite{ false };
    struct RedirectEntry {
        ID3D11Texture2D* smallTex = nullptr;
        ID3D11RenderTargetView* smallRTV = nullptr;
        UINT smallW = 0, smallH = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    static std::unordered_map<ID3D11Texture2D*, RedirectEntry> g_redirectMap;
    static std::mutex g_redirectMutex;

    static void CleanupRedirectCache() {
        std::lock_guard<std::mutex> lock(g_redirectMutex);
        for (auto& kv : g_redirectMap) {
            RedirectEntry& e = kv.second;
            if (e.smallRTV) { e.smallRTV->Release(); e.smallRTV = nullptr; }
            if (e.smallTex) { e.smallTex->Release(); e.smallTex = nullptr; }
        }
        g_redirectMap.clear();
    }


    void SafeAssignTexture(ID3D11Texture2D*& target, ID3D11Texture2D* source) {
        if (target == source) {
            return;
        }

        if (target) {
            target->Release();
            target = nullptr;
        }

        target = source;
        if (target) {
            target->AddRef();
        }
    }

    LRESULT CALLBACK ImGuiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
            return TRUE;
        }

        return g_originalWndProc
            ? CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam)
            : DefWindowProc(hwnd, msg, wParam, lParam);
    }

    void EnsureGlobalInstances() {
        if (g_initializedGlobals) {
            return;
        }

        if (!g_dlssManager) {
            g_dlssManager = new DLSSManager();
        }

        if (!g_dlssConfig) {
            g_dlssConfig = new DLSSConfig();
            g_dlssConfig->Load();
        }

        g_initializedGlobals = true;
    }


    bool InstallResizeHook(IDXGISwapChain* swapChain) {
        if (!swapChain || g_resizeHookInstalled) {
            return g_resizeHookInstalled;
        }

        if (!HookVTableFunction(swapChain, 13, DLSSHooks::HookedResizeBuffers, &DLSSHooks::RealResizeBuffers)) {
            return false;
        }

        g_resizeHookInstalled = true;
        g_pendingResizeHook = false;
        g_loggedResizeFailure = false;
        _MESSAGE("IDXGISwapChain::ResizeBuffers hook installed");
        return true;
    }

    void EnsureVRSubmitHookInstalled();
    void TryHookDevice(ID3D11Device* device);
    void DetectSpecialTextures(const D3D11_TEXTURE2D_DESC& desc, ID3D11Texture2D* texture);
    bool EnsureDLSSRuntimeReady();
}

// fwd decl
static ID3D11RenderTargetView* GetOrCreateSmallRTVFor(ID3D11RenderTargetView* bigRTV, UINT prW, UINT prH);
static void CompositeIfNeededOnBigBind(ID3D11RenderTargetView* bigRTV);
namespace DLSSHooks {
    PFN_Present RealPresent = nullptr;
    PFN_ResizeBuffers RealResizeBuffers = nullptr;
    PFN_CreateTexture2D RealCreateTexture2D = nullptr;
    PFN_CreateDeferredContext RealCreateDeferredContext = nullptr;
    PFN_CreateSamplerState RealCreateSamplerState = nullptr;
    PFN_FactoryCreateSwapChain RealFactoryCreateSwapChain = nullptr;
    PFN_RSSetViewports RealRSSetViewports = nullptr;
    PFN_OMSetRenderTargets RealOMSetRenderTargets = nullptr;

    // Forward declarations for helpers defined later in this file
    static bool IsLikelyVRSceneRT(const D3D11_TEXTURE2D_DESC& d);
    static bool ApproxEqUINT(UINT a, UINT b, float relTol = 0.1f);

    static void InitializeImGuiBackend(IDXGISwapChain* swapChain) {
        if (g_imguiBackendInitialized || !swapChain || !g_device || !g_context) {
            return;
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        if (FAILED(swapChain->GetDesc(&desc))) {
            return;
        }

        HWND hwnd = desc.OutputWindow;
        if (!hwnd) {
            return;
        }

        g_imguiWindow = hwnd;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;

        ImGui::StyleColorsDark();
        // Apply global UI scaling from config (for VR readability)
        if (g_dlssConfig) {
            float scale = g_dlssConfig->uiScale;
            if (scale < 0.5f) scale = 0.5f;
            if (scale > 3.0f) scale = 3.0f;
            io.FontGlobalScale = scale;
            ImGui::GetStyle().ScaleAllSizes(scale);
            _MESSAGE("Applied ImGui UI scale: %.2f", scale);
        }
        ImGui_ImplWin32_EnableDpiAwareness();
        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX11_Init(g_device, g_context);

        if (!g_originalWndProc && !g_overlaySafeMode) {
            g_originalWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ImGuiWndProc)));
        }

        if (g_perfFrequency.QuadPart == 0) {
            QueryPerformanceFrequency(&g_perfFrequency);
        }

        g_lastFrameTime.QuadPart = 0;
        g_imguiBackendInitialized = true;
        g_state.store(DlssState::HaveSwapChain, std::memory_order_relaxed);
    }

    static float ComputeFrameDeltaMs() {
        if (g_perfFrequency.QuadPart == 0) {
            QueryPerformanceFrequency(&g_perfFrequency);
        }

        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);

        float deltaMs = 0.0f;
        if (g_lastFrameTime.QuadPart != 0 && g_perfFrequency.QuadPart != 0) {
            deltaMs = static_cast<float>((now.QuadPart - g_lastFrameTime.QuadPart) * 1000.0 / g_perfFrequency.QuadPart);
        }

        g_lastFrameTime = now;
        return deltaMs;
    }

    static void ShutdownImGuiBackend() {
        if (!g_imguiBackendInitialized) {
            return;
        }

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        if (g_originalWndProc && g_imguiWindow) {
            SetWindowLongPtr(g_imguiWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        }

        g_originalWndProc = nullptr;
        g_imguiWindow = nullptr;
        g_imguiBackendInitialized = false;
        g_imguiMenuInitialized = false;
        g_lastFrameTime.QuadPart = 0;
    }

HRESULT WINAPI HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
        EnsureGlobalInstances();
        EnsureVRSubmitHookInstalled();
        // Reset Phase 1 scene/clamp state per frame
        g_sceneActive.store(false, std::memory_order_relaxed);
        g_sceneRTDesc = {};
        g_clampLogBudgetPerFrame = 4;
        g_compositedThisFrame = false;
        g_redirectUsedThisFrame.store(false, std::memory_order_relaxed);
        if (g_pendingResizeHook && pSwapChain && !g_resizeHookInstalled) {
            if (InstallResizeHook(pSwapChain)) {
                _MESSAGE("Deferred IDXGISwapChain::ResizeBuffers hook installed");
            } else if (!g_loggedResizeFailure) {
                _ERROR("Deferred ResizeBuffers hook installation failed");
                g_loggedResizeFailure = true;
            }
        }

        if (pSwapChain && (!g_device || !g_context)) {
            ID3D11Device* device = nullptr;
            if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
                device->GetImmediateContext(&g_context);
                g_device = device;
                g_swapChain = pSwapChain;
                TryHookDevice(g_device);
            }
        } else if (pSwapChain) {
            g_swapChain = pSwapChain;
            TryHookDevice(g_device);
        }

        if (g_dlssManager && g_device && g_context && !g_dlssRuntimeInitialized) {
            if (g_dlssManager->Initialize()) {
                _MESSAGE("DLSS features initialized from Present hook");
                g_dlssRuntimeInitialized = true;
                g_state.store(DlssState::HaveDlss, std::memory_order_relaxed);
            }
        }

        InitializeImGuiBackend(pSwapChain);

        if (g_imguiBackendInitialized && !g_imguiMenuInitialized) {
            g_imguiMenuInitialized = InitializeImGuiMenu();
            if (g_imguiMenuInitialized) {
                SyncImGuiMenuFromConfig();
                _MESSAGE("ImGui menu initialized");
            }
        }

        if (g_imguiBackendInitialized) {
            const float deltaMs = ComputeFrameDeltaMs();
            const float deltaSeconds = deltaMs > 0.0f ? deltaMs / 1000.0f : 1.0f / 60.0f;

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = deltaSeconds;

            ImGui::NewFrame();

            if (g_imguiMenuInitialized) {
                ProcessImGuiHotkeys();
                UpdateImGuiMetrics(deltaMs);
                RenderImGuiMenu();
            }

            ImGui::Render();
            if (ImDrawData* drawData = ImGui::GetDrawData()) {
                ImGui_ImplDX11_RenderDrawData(drawData);
            }
        }

        return RealPresent
            ? RealPresent(pSwapChain, SyncInterval, Flags)
            : S_OK;
    }

    HRESULT WINAPI HookedResizeBuffers(IDXGISwapChain* pSwapChain,
        UINT BufferCount,
        UINT Width,
        UINT Height,
        DXGI_FORMAT NewFormat,
        UINT SwapChainFlags) {
        _MESSAGE("ResizeBuffers called: %ux%u", Width, Height);

        // Clear redirect caches and per-frame scene state on resize
        CleanupRedirectCache();
        g_sceneActive.store(false, std::memory_order_relaxed);
        g_sceneRTDesc = {};
        g_clampLogBudgetPerFrame = 4;
        g_compositedThisFrame = false;
        g_redirectUsedThisFrame.store(false, std::memory_order_relaxed);

        if (g_imguiBackendInitialized) {
            ShutdownImGuiBackend();
        }

        if (g_dlssManager && g_dlssManager->IsEnabled()) {
            g_dlssManager->Shutdown();
            g_dlssRuntimeInitialized = false;
            g_lastEvaluateOk.store(false, std::memory_order_relaxed);
            g_upscaledEyeTex[0] = g_upscaledEyeTex[1] = nullptr;
            g_state.store(DlssState::HaveSwapChain, std::memory_order_relaxed);
        }

        // Clamp mirror/backbuffer size against HMD per-eye recommendation (PureDark-style)
        UINT clampedW = Width;
        UINT clampedH = Height;
        {
            uint32_t recW = 0, recH = 0;
            if (HMODULE openVRModule = GetModuleHandleW(L"openvr_api.dll")) {
                using PFN_VR_GetGenericInterface = void* (VR_CALLTYPE*)(const char*, vr::EVRInitError*);
                if (auto getIface = reinterpret_cast<PFN_VR_GetGenericInterface>(GetProcAddress(openVRModule, "VR_GetGenericInterface"))) {
                    vr::EVRInitError err = vr::VRInitError_None;
                    void* ptr = getIface(vr::IVRSystem_Version, &err);
                    if (ptr && err == vr::VRInitError_None) {
                        auto sys = reinterpret_cast<vr::IVRSystem*>(ptr);
                        uint32_t w=0,h=0; sys->GetRecommendedRenderTargetSize(&w, &h);
                        recW = w; recH = h;
                    }
                }
            }
            if (recW > 0 && recH > 0) {
                const UINT maxW = recW * 2u;
                const UINT maxH = recH;
                UINT newW = clampedW;
                UINT newH = clampedH;
                if (newW > maxW) newW = maxW;
                if (newH > maxH) newH = maxH;
                if (newW != clampedW || newH != clampedH) {
                    _MESSAGE("[MirrorClamp] ResizeBuffers %ux%u -> %ux%u (max %ux%u)", clampedW, clampedH, newW, newH, maxW, maxH);
                    clampedW = newW; clampedH = newH;
                }
            }
        }

        HRESULT result = RealResizeBuffers
            ? RealResizeBuffers(pSwapChain, BufferCount, clampedW, clampedH, NewFormat, SwapChainFlags)
            : DXGI_ERROR_INVALID_CALL;

        if (SUCCEEDED(result)) {
            g_swapChain = pSwapChain;

            if (g_dlssManager && g_dlssManager->IsEnabled()) {
                if (g_dlssManager->Initialize()) {
                    _MESSAGE("DLSS features re-initialized after resize");
                    g_dlssRuntimeInitialized = true;
                    g_state.store(DlssState::HaveDlss, std::memory_order_relaxed);
                }
            }
        }

        return result;
    }

    static void UnbindResourcesForSubmit() {
        if (!g_context) {
            return;
        }
        ID3D11UnorderedAccessView* nullUAV[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
        UINT counts[8] = { 0,0,0,0,0,0,0,0 };
        g_context->CSSetUnorderedAccessViews(0, 8, nullUAV, counts);
        ID3D11ShaderResourceView* nullSRV[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
        g_context->PSSetShaderResources(0, 8, nullSRV);
        ID3D11SamplerState* nullSamp[4] = { nullptr, nullptr, nullptr, nullptr };
        g_context->PSSetSamplers(0, 4, nullSamp);
    }

    void ProcessVREyeTexture(ID3D11Texture2D* eyeTexture, bool isLeftEye) {
        if (!g_dlssManager || !g_dlssManager->IsEnabled() || !g_dlssRuntimeInitialized) {
            return;
        }

        if (!eyeTexture) {
            return;
        }

        DLSS_TRACE_SAMPLED(60, "ProcessVREyeTexture eye=%d eyeTex=%p depth=%p mv=%p",
                           isLeftEye ? 0 : 1, eyeTexture, g_fallbackDepthTexture, g_motionVectorTexture);

        ID3D11Texture2D* upscaledTexture = nullptr;
        ID3D11Texture2D* depthTexture = g_fallbackDepthTexture;
        ID3D11Texture2D* motionVectors = g_motionVectorTexture;

        if (isLeftEye) {
            upscaledTexture = g_dlssManager->ProcessLeftEye(eyeTexture, depthTexture, motionVectors);
        } else {
            upscaledTexture = g_dlssManager->ProcessRightEye(eyeTexture, depthTexture, motionVectors);
        }

        if (upscaledTexture && eyeTexture && g_context && upscaledTexture != eyeTexture) {
            D3D11_TEXTURE2D_DESC srcDesc{}; upscaledTexture->GetDesc(&srcDesc);
            D3D11_TEXTURE2D_DESC dstDesc{}; eyeTexture->GetDesc(&dstDesc);
            const bool fmtMatch = (srcDesc.Format == dstDesc.Format) &&
                                  (srcDesc.SampleDesc.Count == dstDesc.SampleDesc.Count) &&
                                  (srcDesc.SampleDesc.Count <= 1);
            if (fmtMatch) {
                UINT copyW = std::min(srcDesc.Width, dstDesc.Width);
                UINT copyH = std::min(srcDesc.Height, dstDesc.Height);
                if (copyW > 0 && copyH > 0) {
                    D3D11_BOX src{};
                    src.left = 0; src.top = 0; src.front = 0;
                    src.right = copyW; src.bottom = copyH; src.back = 1;
                    UnbindResourcesForSubmit();
                    g_context->CopySubresourceRegion(eyeTexture, 0, 0, 0, 0, upscaledTexture, 0, &src);
                    DLSS_TRACE_SAMPLED(60, "ProcessVREyeTexture copy eye=%d %ux%u -> %ux%u (src=%p dst=%p)",
                                       isLeftEye ? 0 : 1, copyW, copyH, dstDesc.Width, dstDesc.Height,
                                       upscaledTexture, eyeTexture);
                }
            } else {
                _MESSAGE("[VRSubmit] Skip direct copy: fmt/msaa mismatch srcFmt=%u dstFmt=%u srcS=%u dstS=%u",
                         (unsigned)srcDesc.Format, (unsigned)dstDesc.Format,
                         (unsigned)srcDesc.SampleDesc.Count, (unsigned)dstDesc.SampleDesc.Count);
            }
        } else if (DlssTraceSampled(60)) {
            _MESSAGE("[DLSS_TRACE] ProcessVREyeTexture skip copy eye=%d reason=%s (src=%p dst=%p)",
                     isLeftEye ? 0 : 1,
                     (!upscaledTexture || !eyeTexture) ? "null" :
                     (upscaledTexture == eyeTexture ? "same-texture" : "missing-context"),
                     upscaledTexture, eyeTexture);
        }
    }

    void RegisterMotionVectorTexture(ID3D11Texture2D* motionTexture,
                                     const D3D11_TEXTURE2D_DESC* desc,
                                     UINT targetWidth,
                                     UINT targetHeight) {
        static bool logged = false;
        static UINT cachedWidth = 0;
        static UINT cachedHeight = 0;

        if (!motionTexture) {
            SafeAssignTexture(g_motionVectorTexture, nullptr);
            cachedWidth = cachedHeight = 0;
            if (logged) {
                _MESSAGE("Motion vector texture cleared for DLSS");
                logged = false;
            }
            return;
        }

        D3D11_TEXTURE2D_DESC localDesc{};
        const D3D11_TEXTURE2D_DESC* useDesc = desc;
        if (!useDesc) {
            motionTexture->GetDesc(&localDesc);
            useDesc = &localDesc;
        }

        const bool matchesTarget = (targetWidth > 0 && targetHeight > 0 &&
                                    useDesc->Width == targetWidth &&
                                    useDesc->Height == targetHeight);
        const bool matchesCached = (cachedWidth == useDesc->Width && cachedHeight == useDesc->Height);
        if (!matchesTarget && matchesCached) {
            // stale resolution, ignore
            return;
        }

        SafeAssignTexture(g_motionVectorTexture, motionTexture);
        cachedWidth = useDesc->Width;
        cachedHeight = useDesc->Height;

        if (motionTexture) {
            if (!logged) {
                _MESSAGE("Motion vector texture registered for DLSS");
                logged = true;
            }
            _MESSAGE("Registered motion vectors: %ux%u fmt=%u", useDesc->Width, useDesc->Height, (unsigned)useDesc->Format);
        }
    }

    void RegisterFallbackDepthTexture(ID3D11Texture2D* depthTexture,
                                      const D3D11_TEXTURE2D_DESC* desc,
                                      UINT targetWidth,
                                      UINT targetHeight) {
        static bool logged = false;
        static UINT cachedWidth = 0;
        static UINT cachedHeight = 0;

        if (!depthTexture) {
            SafeAssignTexture(g_fallbackDepthTexture, nullptr);
            cachedWidth = cachedHeight = 0;
            if (logged) {
                _MESSAGE("Fallback depth texture cleared for DLSS");
                logged = false;
            }
            return;
        }

        D3D11_TEXTURE2D_DESC localDesc{};
        const D3D11_TEXTURE2D_DESC* resolvedDesc = desc;
        if (!resolvedDesc) {
            depthTexture->GetDesc(&localDesc);
            resolvedDesc = &localDesc;
        }

        const bool matchesTarget = (targetWidth > 0 && targetHeight > 0 &&
                                    resolvedDesc->Width == targetWidth &&
                                    resolvedDesc->Height == targetHeight);
        const bool matchesCached = (cachedWidth == resolvedDesc->Width && cachedHeight == resolvedDesc->Height);
        if (!matchesTarget && matchesCached) {
            return;
        }

        SafeAssignTexture(g_fallbackDepthTexture, depthTexture);
        cachedWidth = resolvedDesc->Width;
        cachedHeight = resolvedDesc->Height;

        if (!logged) {
            _MESSAGE("Fallback depth texture registered for DLSS");
            logged = true;
        }
        _MESSAGE("Registered fallback depth: %ux%u fmt=%u", resolvedDesc->Width, resolvedDesc->Height, (unsigned)resolvedDesc->Format);
    }

    bool GetD3D11Device(ID3D11Device** ppDevice, ID3D11DeviceContext** ppContext) {
        if (!ppDevice || !ppContext) {
            return false;
        }

        if (g_device && g_context) {
            *ppDevice = g_device;
            *ppContext = g_context;
            return true;
        }

        return false;
    }

    IDXGISwapChain* GetSwapChain() {
        return g_swapChain;
    }
}

namespace {
    // IVRCompositor::Submit (method) signature on Win64
    // RCX=this*, RDX=eye, R8=texture, R9=bounds, [rsp+20]=flags
    using VRSubmitFn = vr::EVRCompositorError(VR_CALLTYPE*)(
        void* /*this*/,
        vr::EVREye,
        const vr::Texture_t*,
        const vr::VRTextureBounds_t*,
        vr::EVRSubmitFlags);
    using VRSubmitWithArrayIndexFn = vr::EVRCompositorError(VR_CALLTYPE*)(
        void* /*this*/,
        vr::EVREye,
        const vr::Texture_t*,
        uint32_t /*unTextureArrayIndex*/,
        const vr::VRTextureBounds_t*,
        vr::EVRSubmitFlags);

    VRSubmitFn g_realVRSubmit = nullptr;
    VRSubmitWithArrayIndexFn g_realVRSubmitWithArrayIndex = nullptr;
    bool g_vrSubmitHookInstalled = false;
    bool g_loggedSubmitFailure = false;
    // Per-eye display (output) sizes tracked from Submit bounds
    static std::atomic<uint32_t> g_perEyeOutW[2] = {0,0};
    static std::atomic<uint32_t> g_perEyeOutH[2] = {0,0};

    // Atlas for SxS replacement (to preserve original bounds semantics)
    static ID3D11Texture2D* g_submitAtlasTex = nullptr;
    static UINT g_submitAtlasW = 0, g_submitAtlasH = 0;

    static bool EnsureSubmitAtlas(UINT width, UINT height, DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM) {
        if (!g_device || !width || !height) return false;
        if (g_submitAtlasTex && g_submitAtlasW == width && g_submitAtlasH == height) {
            return true;
        }
        if (g_submitAtlasTex) { g_submitAtlasTex->Release(); g_submitAtlasTex = nullptr; }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width; d.Height = height; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt; d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.CPUAccessFlags = 0; d.MiscFlags = 0;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &tex)) || !tex) {
            return false;
        }
        g_submitAtlasTex = tex;
        g_submitAtlasW = width; g_submitAtlasH = height;
        return true;
    }

    ID3D11Texture2D* ExtractColorTexture(const vr::Texture_t* texture) {
        if (!texture || !texture->handle) {
            return nullptr;
        }

        if (texture->eType == vr::TextureType_DirectX) {
            return reinterpret_cast<ID3D11Texture2D*>(texture->handle);
        }

        return nullptr;
    }

    ID3D11Texture2D* ExtractDepthTexture(const vr::Texture_t* texture, vr::EVRSubmitFlags flags) {
        if (!(flags & vr::Submit_TextureWithDepth)) {
            return nullptr;
        }

        const auto* withDepth = reinterpret_cast<const vr::VRTextureWithDepth_t*>(texture);
        if (!withDepth || !withDepth->depth.handle) {
            return nullptr;
        }

        if (texture->eType == vr::TextureType_DirectX) {
            return reinterpret_cast<ID3D11Texture2D*>(withDepth->depth.handle);
        }

        return nullptr;
    }

    vr::EVRCompositorError VR_CALLTYPE HookedVRCompositorSubmit(void* self,
        vr::EVREye eye,
        const vr::Texture_t* texture,
        const vr::VRTextureBounds_t* bounds,
        vr::EVRSubmitFlags flags) {
        if (!g_realVRSubmit) {
            return vr::VRCompositorError_RequestFailed;
        }

        if (!texture) {
            return g_realVRSubmit(self, eye, texture, bounds, flags);
        }

        EnsureGlobalInstances();
        const bool earlyActive = IsEarlyDlssActive();
        DLSS_TRACE_SAMPLED(120,
                           "Submit eye=%d handle=%p flags=0x%X bounds=[%.2f %.2f %.2f %.2f]",
                           (int)eye,
                           texture ? texture->handle : nullptr,
                           (unsigned)flags,
                           bounds ? bounds->uMin : 0.0f,
                           bounds ? bounds->vMin : 0.0f,
                           bounds ? bounds->uMax : 0.0f,
                           bounds ? bounds->vMax : 0.0f);

        // Track per-eye display size using OpenVR recommended target size
        {
            uint32_t recW = 0, recH = 0;
            // Try to get IVRSystem via VR_GetGenericInterface without a direct import
            if (HMODULE openVRModule = GetModuleHandleW(L"openvr_api.dll")) {
                using PFN_VR_GetGenericInterface = void* (VR_CALLTYPE*)(const char*, vr::EVRInitError*);
                if (auto getIface = reinterpret_cast<PFN_VR_GetGenericInterface>(GetProcAddress(openVRModule, "VR_GetGenericInterface"))) {
                    vr::EVRInitError err = vr::VRInitError_None;
                    void* ptr = getIface(vr::IVRSystem_Version, &err);
                    if (ptr && err == vr::VRInitError_None) {
                        auto sys = reinterpret_cast<vr::IVRSystem*>(ptr);
                        uint32_t w=0,h=0; sys->GetRecommendedRenderTargetSize(&w, &h);
                        recW = w; recH = h;
                    }
                }
            }
            // Fallback: derive from submitted texture bounds if VRSystem not available yet
            if (recW == 0 || recH == 0) {
                ID3D11Texture2D* colorTexture = ExtractColorTexture(texture);
                if (colorTexture) {
                    D3D11_TEXTURE2D_DESC eyeDesc{}; colorTexture->GetDesc(&eyeDesc);
                    uint32_t fullW = eyeDesc.Width;
                    uint32_t fullH = eyeDesc.Height;
                    double uSpan = 1.0, vSpan = 1.0;
                    if (bounds) {
                        uSpan = std::max(0.0, std::min(1.0, (double)bounds->uMax - (double)bounds->uMin));
                        vSpan = std::max(0.0, std::min(1.0, (double)bounds->vMax - (double)bounds->vMin));
                    }
                    recW = (uint32_t)std::max(1.0, uSpan * (double)fullW);
                    recH = (uint32_t)std::max(1.0, vSpan * (double)fullH);
                    // If bounds cover the whole texture (or are missing), try to detect atlas (SxS or top-bottom)
                    const bool fullSpan = (uSpan > 0.99 && vSpan > 0.99) || !bounds;
                    if (fullSpan) {
                        // Heuristic atlas detection
                        if (fullW >= (uint32_t)((double)fullH * 1.7)) {
                            // side-by-side
                            recW = fullW / 2u;
                            recH = fullH;
                            if (DlssTraceEnabled()) {
                                _MESSAGE("[EarlyDLSS][SIZE] SxS atlas detected: per-eye=%ux%u from full=%ux%u", recW, recH, fullW, fullH);
                            }
                        } else if (fullH >= (uint32_t)((double)fullW * 1.7)) {
                            // top-bottom
                            recW = fullW;
                            recH = fullH / 2u;
                            if (DlssTraceEnabled()) {
                                _MESSAGE("[EarlyDLSS][SIZE] T/B atlas detected: per-eye=%ux%u from full=%ux%u", recW, recH, fullW, fullH);
                            }
                        }
                    }
                }
            }
            // even-align
            recW &= ~1u; recH &= ~1u;
            // Optional per-eye cap to keep sizes sane
            if (earlyActive && g_dlssConfig && g_dlssConfig->enablePerEyeCap && g_dlssConfig->perEyeMaxDim > 0) {
                const uint32_t cap = (uint32_t)g_dlssConfig->perEyeMaxDim;
                uint32_t maxDim = recW > recH ? recW : recH;
                if (maxDim > cap && maxDim > 0) {
                    const double scale = (double)cap / (double)maxDim;
                    uint32_t newW = (uint32_t)std::max(1.0, std::floor((double)recW * scale));
                    uint32_t newH = (uint32_t)std::max(1.0, std::floor((double)recH * scale));
                    // even-align after scale
                    newW &= ~1u; newH &= ~1u;
                    if (newW == 0) newW = 2; if (newH == 0) newH = 2;
                    if (DlssTraceEnabled()) {
                        _MESSAGE("[EarlyDLSS][SIZE] Cap applied: %ux%u -> %ux%u (cap=%u)", recW, recH, newW, newH, cap);
                    }
                    recW = newW; recH = newH;
                }
            }
            const int idx = (eye == vr::Eye_Left) ? 0 : 1;
            if (recW > 0 && recH > 0) {
                g_perEyeOutW[idx].store(recW, std::memory_order_relaxed);
                g_perEyeOutH[idx].store(recH, std::memory_order_relaxed);
            }

            // Phase 0 instrumentation: optional debug probe of predicted render size
            // based on current DLSS quality and per-eye output, without changing behavior.
            static uint64_t s_dbgCounter = 0;
            ++s_dbgCounter;
            if (earlyActive && DlssTraceEnabled()) {
                // Log at a low rate to avoid spam
                if ((s_dbgCounter % 300) == 1) {
                    uint32_t prW = 0, prH = 0;
                    if (g_dlssManager && g_dlssManager->ComputeRenderSizeForOutput(recW, recH, prW, prH)) {
                        _MESSAGE("[EarlyDLSS][DBG] eye=%s out=%ux%u -> predicted render=%ux%u (mode=%d)",
                                 (eye==vr::Eye_Left?"L":"R"), recW, recH, prW, prH, (int)g_dlssConfig->earlyDlssMode);
                    } else {
                        _MESSAGE("[EarlyDLSS][DBG] eye=%s out=%ux%u -> predicted render=(n/a)",
                                 (eye==vr::Eye_Left?"L":"R"), recW, recH);
                    }
                }
            }
        }

        // Submit path: perform per-eye DLSS evaluate and copy back into submitted texture region
        // so the compositor displays the upscaled result without handle replacement.
        if (EnsureDLSSRuntimeReady()) {
            ID3D11Texture2D* colorTexture = ExtractColorTexture(texture);
            if (colorTexture && g_device && g_context && g_dlssManager && g_dlssManager->IsEnabled()) {
                // Determine per-eye output recommended size (recW/recH from the block above)
                uint32_t outW = 0, outH = 0;
                (void)DLSSHooks::GetPerEyeDisplaySize(eye == vr::Eye_Left ? 0 : 1, outW, outH);
                if (outW == 0 || outH == 0) {
                    // Fallback from submitted texture bounds
                    D3D11_TEXTURE2D_DESC eyeDesc{}; colorTexture->GetDesc(&eyeDesc);
                    uint32_t fullW = eyeDesc.Width;
                    uint32_t fullH = eyeDesc.Height;
                    double uSpan = 1.0, vSpan = 1.0;
                    if (bounds) {
                        uSpan = std::max(0.0, std::min(1.0, (double)bounds->uMax - (double)bounds->uMin));
                        vSpan = std::max(0.0, std::min(1.0, (double)bounds->vMax - (double)bounds->vMin));
                    }
                    outW = (uint32_t)std::max(1.0, uSpan * (double)fullW);
                    outH = (uint32_t)std::max(1.0, vSpan * (double)fullH);
                    if ((uSpan > 0.99 && vSpan > 0.99) || !bounds) {
                        if (fullW >= (uint32_t)((double)fullH * 1.7)) { outW = fullW / 2u; outH = fullH; }
                        else if (fullH >= (uint32_t)((double)fullW * 1.7)) { outW = fullW; outH = fullH / 2u; }
                    }
                    outW &= ~1u; outH &= ~1u;
                }

                // Process eye via DLSS
                ID3D11Texture2D* up = (eye == vr::Eye_Left)
                    ? g_dlssManager->ProcessLeftEye(colorTexture, g_fallbackDepthTexture, g_motionVectorTexture)
                    : g_dlssManager->ProcessRightEye(colorTexture, g_fallbackDepthTexture, g_motionVectorTexture);
                DLSS_TRACE_SAMPLED(120,
                                   "Submit eye=%d dlssIn=%p dlssOut=%p perEyeOut=%ux%u",
                                   (int)eye, colorTexture, up, outW, outH);
                if (up && up != colorTexture) {
                    // Compute destination region from bounds (or infer atlas half)
                    D3D11_TEXTURE2D_DESC dstDesc{}; colorTexture->GetDesc(&dstDesc);
                    UINT dstX = 0, dstY = 0;
                    UINT dstW = outW ? outW : dstDesc.Width;
                    UINT dstH = outH ? outH : dstDesc.Height;
                    if (bounds) {
                        const double uMin = std::max(0.0, std::min(1.0, (double)bounds->uMin));
                        const double vMin = std::max(0.0, std::min(1.0, (double)bounds->vMin));
                        const double uMax = std::max(0.0, std::min(1.0, (double)bounds->uMax));
                        const double vMax = std::max(0.0, std::min(1.0, (double)bounds->vMax));
                        dstX = (UINT)std::floor(uMin * (double)dstDesc.Width + 0.5);
                        dstY = (UINT)std::floor(vMin * (double)dstDesc.Height + 0.5);
                        dstW = (UINT)std::max(1.0, std::floor((uMax - uMin) * (double)dstDesc.Width + 0.5));
                        dstH = (UINT)std::max(1.0, std::floor((vMax - vMin) * (double)dstDesc.Height + 0.5));
                    } else {
                        // Infer SxS / TB split when bounds are not provided
                        if (dstDesc.Width >= (UINT)(dstDesc.Height * 1.7f)) {
                            dstW = dstDesc.Width / 2u; dstH = dstDesc.Height;
                            dstX = (eye == vr::Eye_Left) ? 0u : dstW; dstY = 0u;
                        } else if (dstDesc.Height >= (UINT)(dstDesc.Width * 1.7f)) {
                            dstW = dstDesc.Width; dstH = dstDesc.Height / 2u;
                            dstX = 0u; dstY = (eye == vr::Eye_Left) ? 0u : dstH;
                        } else {
                            dstX = 0u; dstY = 0u; dstW = dstDesc.Width; dstH = dstDesc.Height;
                        }
                    }
                    // Copy full upscaled rect into the destination region (formats and MSAA must match)
                    D3D11_TEXTURE2D_DESC srcDesc{}; up->GetDesc(&srcDesc);
                    if (srcDesc.Format == dstDesc.Format && srcDesc.SampleDesc.Count == dstDesc.SampleDesc.Count && srcDesc.SampleDesc.Count == 1) {
                        // Clamp copy size to destination bounds
                        const UINT maxW = (dstX < dstDesc.Width)  ? (dstDesc.Width  - dstX) : 0u;
                        const UINT maxH = (dstY < dstDesc.Height) ? (dstDesc.Height - dstY) : 0u;
                        UINT copyW = srcDesc.Width; UINT copyH = srcDesc.Height;
                        if (dstW > 0) copyW = (copyW > dstW) ? dstW : copyW;
                        if (dstH > 0) copyH = (copyH > dstH) ? dstH : copyH;
                        if (copyW > maxW) copyW = maxW;
                        if (copyH > maxH) copyH = maxH;
                        if (copyW > 0 && copyH > 0) {
                            D3D11_BOX src{}; src.left = 0; src.top = 0; src.front = 0; src.right = copyW; src.bottom = copyH; src.back = 1;
                            DLSSHooks::UnbindResourcesForSubmit();
                            g_context->CopySubresourceRegion(colorTexture, 0, dstX, dstY, 0, up, 0, &src);
                            DLSS_TRACE_SAMPLED(120,
                                               "Submit eye=%d copy dst=(%u,%u %ux%u) src=(%ux%u)",
                                               (int)eye, dstX, dstY, copyW, copyH, srcDesc.Width, srcDesc.Height);
                            if (DlssTraceEnabled()) {
                                _MESSAGE("[Submit] Copy DLSS eye=%s dst=(%u,%u %ux%u) src=(%ux%u)",
                                         eye==vr::Eye_Left?"L":"R", dstX, dstY, copyW, copyH, srcDesc.Width, srcDesc.Height);
                            }
                        }
                    } else if (DlssTraceEnabled()) {
                        _MESSAGE("[Submit] Skip copy: fmt/msaa mismatch srcFmt=%u dstFmt=%u srcS=%u dstS=%u",
                                 (unsigned)srcDesc.Format, (unsigned)dstDesc.Format,
                                 (unsigned)srcDesc.SampleDesc.Count, (unsigned)dstDesc.SampleDesc.Count);
                    }
                } else if (DlssTraceSampled(120)) {
                    _MESSAGE("[DLSS_TRACE] Submit eye=%d skip copy (%s)",
                             (int)eye,
                             up ? "dlss returned original texture" : "dlss output null");
                }
            }
        }

        return g_realVRSubmit(self, eye, texture, bounds, flags);
    }

    vr::EVRCompositorError VR_CALLTYPE HookedVRCompositorSubmitWithArrayIndex(void* self,
        vr::EVREye eye,
        const vr::Texture_t* texture,
        uint32_t unTextureArrayIndex,
        const vr::VRTextureBounds_t* bounds,
        vr::EVRSubmitFlags flags) {
        if (!g_realVRSubmitWithArrayIndex) {
            return vr::VRCompositorError_RequestFailed;
        }

        if (!texture) {
            return g_realVRSubmitWithArrayIndex(self, eye, texture, unTextureArrayIndex, bounds, flags);
        }

        EnsureGlobalInstances();

        // Track per-eye output size as in HookedVRCompositorSubmit
        {
            uint32_t recW = 0, recH = 0;
            if (HMODULE openVRModule = GetModuleHandleW(L"openvr_api.dll")) {
                using PFN_VR_GetGenericInterface = void* (VR_CALLTYPE*)(const char*, vr::EVRInitError*);
                if (auto getIface = reinterpret_cast<PFN_VR_GetGenericInterface>(GetProcAddress(openVRModule, "VR_GetGenericInterface"))) {
                    vr::EVRInitError err = vr::VRInitError_None;
                    void* ptr = getIface(vr::IVRSystem_Version, &err);
                    if (ptr && err == vr::VRInitError_None) {
                        auto sys = reinterpret_cast<vr::IVRSystem*>(ptr);
                        uint32_t w=0,h=0; sys->GetRecommendedRenderTargetSize(&w, &h);
                        recW = w; recH = h;
                    }
                }
            }
            if (recW == 0 || recH == 0) {
                ID3D11Texture2D* colorTexture = ExtractColorTexture(texture);
                if (colorTexture) {
                    D3D11_TEXTURE2D_DESC eyeDesc{}; colorTexture->GetDesc(&eyeDesc);
                    uint32_t fullW = eyeDesc.Width;
                    uint32_t fullH = eyeDesc.Height;
                    double uSpan = 1.0, vSpan = 1.0;
                    if (bounds) {
                        uSpan = std::max(0.0, std::min(1.0, (double)bounds->uMax - (double)bounds->uMin));
                        vSpan = std::max(0.0, std::min(1.0, (double)bounds->vMax - (double)bounds->vMin));
                    }
                    recW = (uint32_t)std::max(1.0, uSpan * (double)fullW);
                    recH = (uint32_t)std::max(1.0, vSpan * (double)fullH);
                    const bool fullSpan = (uSpan > 0.99 && vSpan > 0.99) || !bounds;
                    if (fullSpan) {
                        if (fullW >= (uint32_t)((double)fullH * 1.7)) { recW = fullW / 2u; recH = fullH; }
                        else if (fullH >= (uint32_t)((double)fullW * 1.7)) { recW = fullW; recH = fullH / 2u; }
                    }
                }
            }
            recW &= ~1u; recH &= ~1u;
            const int idx = (eye == vr::Eye_Left) ? 0 : 1;
            if (recW > 0 && recH > 0) {
                g_perEyeOutW[idx].store(recW, std::memory_order_relaxed);
                g_perEyeOutH[idx].store(recH, std::memory_order_relaxed);
            }
        }

        if (EnsureDLSSRuntimeReady() && (!g_dlssConfig || g_dlssConfig->submitCopyEnabled)) {
            ID3D11Texture2D* colorTexture = ExtractColorTexture(texture);
            if (colorTexture && g_device && g_context && g_dlssManager && g_dlssManager->IsEnabled()) {
                uint32_t outW = 0, outH = 0;
                (void)DLSSHooks::GetPerEyeDisplaySize(eye == vr::Eye_Left ? 0 : 1, outW, outH);
                D3D11_TEXTURE2D_DESC dstDesc{}; colorTexture->GetDesc(&dstDesc);
        ID3D11Texture2D* up = (eye == vr::Eye_Left)
            ? g_dlssManager->ProcessLeftEye(colorTexture, g_fallbackDepthTexture, g_motionVectorTexture)
            : g_dlssManager->ProcessRightEye(colorTexture, g_fallbackDepthTexture, g_motionVectorTexture);
        if (up && up != colorTexture) {
                    UINT dstX = 0, dstY = 0; UINT dstW = outW ? outW : dstDesc.Width; UINT dstH = outH ? outH : dstDesc.Height;
                    if (bounds) {
                        const double uMin = std::max(0.0, std::min(1.0, (double)bounds->uMin));
                        const double vMin = std::max(0.0, std::min(1.0, (double)bounds->vMin));
                        const double uMax = std::max(0.0, std::min(1.0, (double)bounds->uMax));
                        const double vMax = std::max(0.0, std::min(1.0, (double)bounds->vMax));
                        dstX = (UINT)std::floor(uMin * (double)dstDesc.Width + 0.5);
                        dstY = (UINT)std::floor(vMin * (double)dstDesc.Height + 0.5);
                        dstW = (UINT)std::max(1.0, std::floor((uMax - uMin) * (double)dstDesc.Width + 0.5));
                        dstH = (UINT)std::max(1.0, std::floor((vMax - vMin) * (double)dstDesc.Height + 0.5));
                    }
                    D3D11_TEXTURE2D_DESC srcDesc{}; up->GetDesc(&srcDesc);
                    if (srcDesc.Format == dstDesc.Format && srcDesc.SampleDesc.Count == dstDesc.SampleDesc.Count && srcDesc.SampleDesc.Count == 1) {
                        const UINT maxW = (dstX < dstDesc.Width)  ? (dstDesc.Width  - dstX) : 0u;
                        const UINT maxH = (dstY < dstDesc.Height) ? (dstDesc.Height - dstY) : 0u;
                        UINT copyW = srcDesc.Width; UINT copyH = srcDesc.Height;
                        if (dstW > 0) copyW = (copyW > dstW) ? dstW : copyW;
                        if (dstH > 0) copyH = (copyH > dstH) ? dstH : copyH;
                        if (copyW > maxW) copyW = maxW;
                        if (copyH > maxH) copyH = maxH;
                        if (copyW > 0 && copyH > 0) {
                            const UINT dstSub = D3D11CalcSubresource(0, std::min(unTextureArrayIndex, dstDesc.ArraySize ? (dstDesc.ArraySize-1) : 0u), dstDesc.MipLevels ? dstDesc.MipLevels : 1u);
                            D3D11_BOX src{}; src.left = 0; src.top = 0; src.front = 0; src.right = copyW; src.bottom = copyH; src.back = 1;
                            DLSSHooks::UnbindResourcesForSubmit();
                            g_context->CopySubresourceRegion(colorTexture, dstSub, dstX, dstY, 0, up, 0, &src);
                            DLSS_TRACE_SAMPLED(120,
                                               "SubmitIdx eye=%d copy dst=(%u,%u %ux%u sub=%u) src=(%ux%u)",
                                               (int)eye, dstX, dstY, copyW, copyH, dstSub, srcDesc.Width, srcDesc.Height);
                        }
                    } else if (DlssTraceEnabled()) {
                        _MESSAGE("[SubmitIdx] Skip copy: fmt/msaa mismatch srcFmt=%u dstFmt=%u srcS=%u dstS=%u",
                                 (unsigned)srcDesc.Format, (unsigned)dstDesc.Format,
                                 (unsigned)srcDesc.SampleDesc.Count, (unsigned)dstDesc.SampleDesc.Count);
                    }
                } else if (DlssTraceSampled(120)) {
                    _MESSAGE("[DLSS_TRACE] SubmitIdx eye=%d skip copy (%s)",
                             (int)eye,
                             up ? "dlss returned original texture" : "dlss output null");
                }
            }
        }

        return g_realVRSubmitWithArrayIndex(self, eye, texture, unTextureArrayIndex, bounds, flags);
    }

    void EnsureVRSubmitHookInstalled() {
        if (g_vrSubmitHookInstalled) {
            return;
        }

        HMODULE openVRModule = GetModuleHandleA("openvr_api.dll");
        if (!openVRModule) {
            return;
        }

        // Try fast path first: exported VRCompositor()
        vr::IVRCompositor* compositor = nullptr;
        using PFN_VRCompositor = vr::IVRCompositor* (VR_CALLTYPE*)();
        if (auto vrCompositorFn = reinterpret_cast<PFN_VRCompositor>(GetProcAddress(openVRModule, "VRCompositor"))) {
            compositor = vrCompositorFn();
        }

        // Fallback: VR_GetGenericInterface("IVRCompositor_XXX")
        if (!compositor) {
            using PFN_VR_GetGenericInterface = void* (VR_CALLTYPE*)(const char*, vr::EVRInitError*);
            auto getIface = reinterpret_cast<PFN_VR_GetGenericInterface>(GetProcAddress(openVRModule, "VR_GetGenericInterface"));
            if (getIface) {
                vr::EVRInitError err = vr::VRInitError_None;
                void* ptr = getIface(vr::IVRCompositor_Version, &err);
                if (ptr && err == vr::VRInitError_None) {
                    compositor = reinterpret_cast<vr::IVRCompositor*>(ptr);
                    _MESSAGE("OpenVR compositor obtained via VR_GetGenericInterface(%s)", vr::IVRCompositor_Version);
                } else if (!g_loggedSubmitFailure) {
                    _MESSAGE("VR_GetGenericInterface for IVRCompositor failed err=%d (submit hook pending)", (int)err);
                    g_loggedSubmitFailure = true;
                }
            } else if (!g_loggedSubmitFailure) {
                _MESSAGE("OpenVR not ready yet; exports VRCompositor/VR_GetGenericInterface not found (submit hook pending)");
                g_loggedSubmitFailure = true;
            }
        }

        if (!compositor) {
            return;
        }

        bool okSubmit = HookVTableFunction(compositor, 6, HookedVRCompositorSubmit, &g_realVRSubmit); bool okSubmitArr = HookVTableFunction(compositor, 7, HookedVRCompositorSubmitWithArrayIndex, &g_realVRSubmitWithArrayIndex); if (okSubmit) { g_vrSubmitHookInstalled = true; g_loggedSubmitFailure = false; _MESSAGE("OpenVR Submit hook installed successfully"); if (okSubmitArr) { _MESSAGE("OpenVR SubmitWithArrayIndex hook installed successfully"); } g_state.store(DlssState::HaveCompositor, std::memory_order_relaxed); } else if (!g_loggedSubmitFailure) { _ERROR("Failed to install OpenVR Submit hook"); g_loggedSubmitFailure = true; } else if (!g_loggedSubmitFailure) {
            _ERROR("Failed to install OpenVR Submit hook");
            g_loggedSubmitFailure = true;
        }
    }
    void TryHookDevice(ID3D11Device* device) {
        if (!device) {
            return;
        }
        if (g_hookedDevice == device && g_deviceHookInstalled) {
            return;
        }
        if (HookVTableFunction(device, 5, DLSSHooks::HookedCreateTexture2D, &DLSSHooks::RealCreateTexture2D)) {
            g_hookedDevice = device;
            g_deviceHookInstalled = true;
            _MESSAGE("ID3D11Device::CreateTexture2D hook installed");
            // Hook the immediate context for viewport clamp and RT redirect
            ID3D11DeviceContext* ctx = nullptr;
            device->GetImmediateContext(&ctx);
            if (ctx) {
                DLSSHooks::InstallContextHooks(ctx);
                _MESSAGE("Immediate context hooks installed (OMSetRenderTargets, RSSetViewports)");
                ctx->Release();
            }
            // Hook CreateDeferredContext to reach all deferred contexts
            HookVTableFunction(device, 27, DLSSHooks::HookedCreateDeferredContext, &DLSSHooks::RealCreateDeferredContext);
            _MESSAGE("ID3D11Device::CreateDeferredContext hook installed");
            // Hook CreateSamplerState to apply Mip LOD Bias (PureDark-style)
            HookVTableFunction(device, 23, DLSSHooks::HookedCreateSamplerState, &DLSSHooks::RealCreateSamplerState);
            _MESSAGE("ID3D11Device::CreateSamplerState hook installed");
        } else {
            g_deviceHookInstalled = false;
            _ERROR("Failed to hook ID3D11Device::CreateTexture2D");
        }
    }

    bool GetSwapChainSize(UINT& width, UINT& height) {
        if (!g_swapChain) {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        if (FAILED(g_swapChain->GetDesc(&desc))) {
            return false;
        }

        width = desc.BufferDesc.Width;
        height = desc.BufferDesc.Height;
        return true;
    }

    bool IsDepthFormat(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_D16_UNORM:
            case DXGI_FORMAT_R32_TYPELESS:
            case DXGI_FORMAT_R24G8_TYPELESS:
            case DXGI_FORMAT_R16_TYPELESS:
            case DXGI_FORMAT_R32G8X24_TYPELESS:
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
                return true;
            default:
                return false;
        }
    }

    bool IsMotionVectorCandidate(const D3D11_TEXTURE2D_DESC& desc, UINT targetWidth, UINT targetHeight) {
        // Typical MV: R16G16_FLOAT, SRV+RTV, single-sample, single-mip, reasonably large
        if (desc.Format != DXGI_FORMAT_R16G16_FLOAT) {
            return false;
        }

        const UINT requiredFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if ((desc.BindFlags & requiredFlags) != requiredFlags) {
            return false;
        }

        if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
            return false;
        }

        if (desc.Width < 256 || desc.Height < 256) {
            return false;
        }

        // If we know a target (mirror) size, accept a broad range since VR render/MV is often a fraction of it.
        if (targetWidth && targetHeight) {
            const float wRatio = static_cast<float>(desc.Width) / static_cast<float>(targetWidth);
            const float hRatio = static_cast<float>(desc.Height) / static_cast<float>(targetHeight);
            // Accept 25%..100% range and common SxS (half width) cases
            const bool inRange = (wRatio >= 0.25f && wRatio <= 1.01f) && (hRatio >= 0.25f && hRatio <= 1.01f);
            if (inRange) {
                return true;
            }
        }

        // Fallback: compare against detected scene RT (or half width for SxS atlases)
        if (g_sceneRTDesc.Width > 0 && g_sceneRTDesc.Height > 0) {
            if ((desc.Width == g_sceneRTDesc.Width && desc.Height == g_sceneRTDesc.Height) ||
                (desc.Width == (g_sceneRTDesc.Width / 2u) && desc.Height == g_sceneRTDesc.Height)) {
                return true;
            }
        }

        return false;
    }

    bool IsDepthCandidate(const D3D11_TEXTURE2D_DESC& desc, UINT targetWidth, UINT targetHeight) {
        if (!(desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
            return false;
        }
        if (!IsDepthFormat(desc.Format)) {
            return false;
        }
        // Prefer non-MSAA depth for SRV tagging
        if (desc.SampleDesc.Count != 1) {
            return false;
        }
        // Accept any reasonable size; VR eye targets will be large
        if (desc.Width < 512 || desc.Height < 512) {
            return false;
        }
        if (targetWidth && targetHeight) {
            const float widthRatio = static_cast<float>(desc.Width) / static_cast<float>(targetWidth);
            const float heightRatio = static_cast<float>(desc.Height) / static_cast<float>(targetHeight);
            if (widthRatio < 0.35f || widthRatio > 0.95f) {
                return false;
            }
            if (heightRatio < 0.35f || heightRatio > 0.95f) {
                return false;
            }
        }
        return true;
    }

    void DetectSpecialTextures(const D3D11_TEXTURE2D_DESC& desc, ID3D11Texture2D* texture) {
        if (!texture) {
            return;
        }

        UINT targetWidth = 0;
        UINT targetHeight = 0;
        const bool haveSwapSize = GetSwapChainSize(targetWidth, targetHeight);

        const UINT matchWidth = haveSwapSize ? targetWidth : 0;
        const UINT matchHeight = haveSwapSize ? targetHeight : 0;

        // Probe log for potential motion vector textures (R16G16_FLOAT)
        if (desc.Format == DXGI_FORMAT_R16G16_FLOAT) {
            const float wRatio = matchWidth  ? (float)desc.Width  / (float)matchWidth  : 0.0f;
            const float hRatio = matchHeight ? (float)desc.Height / (float)matchHeight : 0.0f;
            const bool mvCand = IsMotionVectorCandidate(desc, matchWidth, matchHeight);
            _MESSAGE("[MVProbe] R16G16F %ux%u mips=%u samples=%u flags=0x%08X usage=%u tgt=%ux%u wr=%.2f hr=%.2f cand=%d",
                     desc.Width, desc.Height,
                     (unsigned)desc.MipLevels,
                     (unsigned)desc.SampleDesc.Count,
                     (unsigned)desc.BindFlags,
                     (unsigned)desc.Usage,
                     matchWidth, matchHeight,
                     wRatio, hRatio,
                     mvCand ? 1 : 0);
        }

        if (IsMotionVectorCandidate(desc, matchWidth, matchHeight)) {
            DLSSHooks::RegisterMotionVectorTexture(texture, &desc, matchWidth, matchHeight);
            return;
        }

        if (IsDepthCandidate(desc, matchWidth, matchHeight)) {
            DLSSHooks::RegisterFallbackDepthTexture(texture, &desc, matchWidth, matchHeight);
        }
    }

    bool EnsureDLSSRuntimeReady() {
        if (!g_dlssManager || !g_dlssManager->IsEnabled()) {
            return false;
        }

        if (!g_dlssRuntimeInitialized) {
            if (g_dlssManager->Initialize()) {
                g_dlssRuntimeInitialized = true;
                g_loggedDLSSInitFailure = false;
                _MESSAGE("DLSS runtime initialized from VR submit path");
            } else if (!g_loggedDLSSInitFailure) {
                g_loggedDLSSInitFailure = true;
                _ERROR("Failed to initialize DLSS runtime from VR submit path");
            }
        }

        return g_dlssRuntimeInitialized;
    }
}

namespace DLSSHooks {
    bool GetPerEyeDisplaySize(int eyeIndex, uint32_t& outW, uint32_t& outH) {
        if (eyeIndex < 0 || eyeIndex > 1) return false;
        uint32_t w = g_perEyeOutW[eyeIndex].load(std::memory_order_relaxed);
        uint32_t h = g_perEyeOutH[eyeIndex].load(std::memory_order_relaxed);
        if (w == 0 || h == 0) return false;
        outW = w; outH = h;
        return true;
    }
}

namespace DLSSHooks {
    HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(ID3D11Device* device,
        const D3D11_TEXTURE2D_DESC* desc,
        const D3D11_SUBRESOURCE_DATA* initialData,
        ID3D11Texture2D** texture) {
        if (!RealCreateTexture2D) {
            return E_FAIL;
        }

        // Deterministic downscale: PureDark-style small RT creation for scene color targets
        // Safety: Only enable after VR Submit hook is installed to avoid breaking HMD bring-up
        D3D11_TEXTURE2D_DESC local{};
        const D3D11_TEXTURE2D_DESC* useDesc = desc;
        bool modified = false;
        if (desc) {
            local = *desc;
            // Only when enabled
            if (IsEarlyDlssActive() && g_vrSubmitHookInstalled) {
                // Avoid touching depth/typeless DS, shared, non-RT
                const bool isColorRT = (local.BindFlags & D3D11_BIND_RENDER_TARGET) && (local.BindFlags & D3D11_BIND_SHADER_RESOURCE) && ((local.BindFlags & D3D11_BIND_DEPTH_STENCIL) == 0);
                if (isColorRT && IsLikelyVRSceneRT(local)) {
                    // Determine per-eye output and render sizes
                    uint32_t eyeW = 0, eyeH = 0;
                    (void)DLSSHooks::GetPerEyeDisplaySize(0, eyeW, eyeH);
                    if (!eyeW || !eyeH) (void)DLSSHooks::GetPerEyeDisplaySize(1, eyeW, eyeH);
                    if (eyeW && eyeH && g_dlssManager) {
                        uint32_t prW = 0, prH = 0;
                        if (g_dlssManager->ComputeRenderSizeForOutput(eyeW, eyeH, prW, prH)) {
                            // Heuristic: shrink only if current size matches eye/native or SxS atlas
                            const bool looksEye = ApproxEqUINT(local.Width, eyeW) && ApproxEqUINT(local.Height, eyeH);
                            const bool looksSxS = ApproxEqUINT(local.Width, eyeW * 2u) && ApproxEqUINT(local.Height, eyeH);
                            const bool looksRender = ApproxEqUINT(local.Width, prW) && ApproxEqUINT(local.Height, prH);
                            const bool looksRenderSxS = ApproxEqUINT(local.Width, prW * 2u) && ApproxEqUINT(local.Height, prH);
                            if (!looksRender && !looksRenderSxS && (looksEye || looksSxS)) {
                                // Keep format/sample/mips; shrink dimensions only
                                local.Width  = looksSxS ? (prW * 2u) : prW;
                                local.Height = prH;
                                useDesc = &local;
                                modified = true;
                                if (DlssTraceEnabled()) {
                                    _MESSAGE("[CreateTex2D][Scale] %ux%u -> %ux%u fmt=%u", desc->Width, desc->Height, local.Width, local.Height, (unsigned)local.Format);
                                }
                            }
                        }
                    }
                }
            }
        }

        HRESULT result = RealCreateTexture2D(device, useDesc, initialData, texture);
        if (FAILED(result) || !useDesc || !texture || !*texture) {
            return result;
        }

        // Mark depth candidates for fallback selection and other probes
        DetectSpecialTextures(*useDesc, *texture);
        return result;
    }

    // Install OMSetRenderTargets and RSSetViewports hooks on a given context
    static void InstallContextHooks(ID3D11DeviceContext* ctx) {
        if (!ctx) return;
        HookVTableFunction(ctx, 33, DLSSHooks::HookedOMSetRenderTargets, &DLSSHooks::RealOMSetRenderTargets);
        HookVTableFunction(ctx, 44, DLSSHooks::HookedRSSetViewports, &DLSSHooks::RealRSSetViewports);
    }

    // Hook ID3D11Device::CreateDeferredContext to ensure deferred contexts are also hooked
    HRESULT STDMETHODCALLTYPE HookedCreateDeferredContext(ID3D11Device* device, UINT ContextFlags, ID3D11DeviceContext** ppDeferredContext) {
        if (!RealCreateDeferredContext) {
            return E_FAIL;
        }
        HRESULT hr = RealCreateDeferredContext(device, ContextFlags, ppDeferredContext);
        if (SUCCEEDED(hr) && ppDeferredContext && *ppDeferredContext) {
            InstallContextHooks(*ppDeferredContext);
            _MESSAGE("Deferred context hooks installed (OMSetRenderTargets, RSSetViewports)");
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE HookedCreateSamplerState(ID3D11Device* device,
        const D3D11_SAMPLER_DESC* pDesc,
        ID3D11SamplerState** ppSamplerState) {
        if (!RealCreateSamplerState) {
            return E_FAIL;
        }
        if (!pDesc) {
            return RealCreateSamplerState(device, pDesc, ppSamplerState);
        }
        D3D11_SAMPLER_DESC sd = *pDesc;
        // Apply Mip LOD Bias if configured and original bias == 0
        if (g_dlssConfig && g_dlssConfig->useOptimalMipLodBias) {
            // Heuristic: only adjust simple samplers (low anisotropy)
            if (sd.MipLODBias == 0.0f && sd.MaxAnisotropy <= 1) {
                float bias = g_dlssConfig->mipLodBias;
                if (bias < -3.0f) bias = -3.0f; if (bias > 3.0f) bias = 3.0f;
                sd.MipLODBias = bias;
            }
        }
        return RealCreateSamplerState(device, &sd, ppSamplerState);
    }
}

template <typename T>
bool HookVTableFunction(void* pVTable, int index, T hookFunc, T* originalFunc) {
    if (!pVTable || !originalFunc) {
        return false;
    }

    void** vtable = *(void***)pVTable;
    if (!vtable) {
        return false;
    }

    DWORD oldProtect;
    if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *originalFunc = reinterpret_cast<T>(vtable[index]);
        vtable[index] = reinterpret_cast<void*>(hookFunc);
        VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
        return true;
    }

    return false;
}

static bool InstallHooksAttempt() {
    _MESSAGE("Installing DLSS hooks");

    EnsureGlobalInstances();

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kTempWindowClass;

    if (!RegisterClassExA(&wc)) {
        _ERROR("RegisterClassExA failed: %lu", GetLastError());
        return false;
    }
    g_classRegistered = true;

    HWND hWnd = CreateWindowExA(
        0,
        kTempWindowClass,
        "",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        100,
        100,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);
    if (!hWnd) {
        _ERROR("CreateWindowExA failed: %lu", GetLastError());
        UnregisterClassA(kTempWindowClass, wc.hInstance);
        g_classRegistered = false;
        return false;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 1;
    swapChainDesc.BufferDesc.Width = 100;
    swapChainDesc.BufferDesc.Height = 100;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = hWnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    ID3D11Device* tempDevice = nullptr;
    ID3D11DeviceContext* tempContext = nullptr;
    IDXGISwapChain* tempSwapChain = nullptr;

    auto tryCreateSwapChain = [&](D3D_DRIVER_TYPE driverType, DWORD flags) -> HRESULT {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        IDXGISwapChain* swapChain = nullptr;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;

        HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            driverType,
            nullptr,
            flags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &swapChainDesc,
            &swapChain,
            &device,
            &featureLevel,
            &context);

        if (SUCCEEDED(result)) {
            tempDevice = device;
            tempContext = context;
            tempSwapChain = swapChain;
        } else {
            if (swapChain) {
                swapChain->Release();
            }
            if (context) {
                context->Release();
            }
            if (device) {
                device->Release();
            }
        }

        return result;
    };

    const DWORD baseFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = tryCreateSwapChain(D3D_DRIVER_TYPE_HARDWARE, baseFlags);
    if (FAILED(hr)) {
        _ERROR("Failed to create temporary D3D11 device for hooking (hardware) hr=0x%08X", hr);
        HRESULT warpHr = tryCreateSwapChain(D3D_DRIVER_TYPE_WARP, baseFlags);
        if (SUCCEEDED(warpHr)) {
            _MESSAGE("Temporary D3D11 device created using WARP driver for hook installation");
            hr = warpHr;
        } else {
            _ERROR("Failed to create temporary D3D11 device for hooking (warp) hr=0x%08X", warpHr);
        }
    }

    bool presentHooked = false;
    if (SUCCEEDED(hr) && tempSwapChain) {
        // Early hook: IDXGIFactory::CreateSwapChain
        IDXGIFactory* pFactory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory)) && pFactory) {
            if (HookVTableFunction(pFactory, 10, DLSSHooks::HookedFactoryCreateSwapChain, &DLSSHooks::RealFactoryCreateSwapChain)) {
                _MESSAGE("IDXGIFactory::CreateSwapChain hook installed");
            } else {
                _ERROR("Failed to hook IDXGIFactory::CreateSwapChain");
            }
            pFactory->Release();
        }
        presentHooked = HookVTableFunction(tempSwapChain, 8, DLSSHooks::HookedPresent, &DLSSHooks::RealPresent);
        if (!presentHooked) {
            _ERROR("Failed to hook IDXGISwapChain::Present");
        } else {
            g_presentHookInstalled = true;
            if (!InstallResizeHook(tempSwapChain)) {
                g_pendingResizeHook = true;
                if (!g_loggedResizeFailure) {
                    _ERROR("Failed to hook IDXGISwapChain::ResizeBuffers; will retry on live swap chain");
                    g_loggedResizeFailure = true;
                }
            }
        }
    }

    if (tempSwapChain) {
        tempSwapChain->Release();
        tempSwapChain = nullptr;
    }
    if (tempContext) {
        tempContext->Release();
        tempContext = nullptr;
    }
    if (tempDevice) {
        tempDevice->Release();
        tempDevice = nullptr;
    }

    if (hWnd) {
        DestroyWindow(hWnd);
        hWnd = nullptr;
    }

    if (g_classRegistered) {
        UnregisterClassA(kTempWindowClass, wc.hInstance);
        g_classRegistered = false;
    }

    if (!presentHooked) {
        _ERROR("Failed to install D3D11 hooks");
        return false;
    }

    if (g_pendingResizeHook) {
        _MESSAGE("ResizeBuffers hook will be installed when the live swap chain is available");
    }

    _MESSAGE("DLSS hooks installed successfully");
    return true;
}
static bool InstallHooksImmediate() {
    constexpr int kMaxAttempts = 8;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (InstallHooksAttempt()) {
            return true;
        }
    }

    _ERROR("Failed to install D3D11 hooks after %d attempts", kMaxAttempts);
    return false;
}

extern "C" bool InstallDLSSHooks() {
    EnsureGlobalInstances();

    if (g_hookInstallComplete.load()) {
        return g_hookInstallSucceeded.load();
    }

    bool expected = false;
    if (g_hookThreadStarted.compare_exchange_strong(expected, true)) {
        std::thread([]() {
            bool result = InstallHooksImmediate();
            g_hookInstallSucceeded.store(result);
            g_hookInstallComplete.store(true);
        }).detach();
    }

    return true;
}
namespace DLSSHooks {
    HRESULT STDMETHODCALLTYPE HookedFactoryCreateSwapChain(IDXGIFactory* factory,
        IUnknown* pDevice,
        DXGI_SWAP_CHAIN_DESC* pDesc,
        IDXGISwapChain** ppSwapChain) {
        if (!RealFactoryCreateSwapChain) {
            return E_FAIL;
        }
        HRESULT hr = RealFactoryCreateSwapChain(factory, pDevice, pDesc, ppSwapChain);
        if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
            _MESSAGE("IDXGIFactory::CreateSwapChain intercepted: %ux%u fmt=%d", pDesc ? pDesc->BufferDesc.Width : 0, pDesc ? pDesc->BufferDesc.Height : 0, pDesc ? pDesc->BufferDesc.Format : 0);
            g_swapChain = *ppSwapChain;
            ID3D11Device* dev = nullptr;
            if (SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
                g_device = dev;
                dev->GetImmediateContext(&g_context);
                TryHookDevice(g_device);
            }
            if (!g_resizeHookInstalled) {
                if (InstallResizeHook(*ppSwapChain)) {
                    _MESSAGE("ResizeBuffers hook installed from Factory::CreateSwapChain");
                }
            }
        }
        return hr;
    }
}

extern "C" void SetOverlaySafeMode(bool enabled) {
    g_overlaySafeMode = enabled;
    if (enabled) {
        _MESSAGE("Overlay safe mode enabled (no WndProc hook; better compatibility with ENB/ReShade)");
    }
}













namespace DLSSHooks {
    // Heuristic: decide if an RTV looks like a scene color target
    static bool IsSceneColorRTDesc(const D3D11_TEXTURE2D_DESC& d) {
        if (d.SampleDesc.Count != 1) return false;
        if ((d.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) return false;
        if (d.Width < 1024 || d.Height < 1024) return false;
        return true;
    }

    static bool ApproxEqUINT(UINT a, UINT b, float relTol) {
        if (a == b) return true;
        float fa = static_cast<float>(a), fb = static_cast<float>(b);
        float diff = fabsf(fa - fb);
        float tol = std::max(2.0f, fb * relTol);
        return diff <= tol;
    }

    // More strict heuristic to identify VR eye scene RTs and avoid clamping mirror swapchain
    static bool IsLikelyVRSceneRT(const D3D11_TEXTURE2D_DESC& d) {
        // Exclude tiny/utility
        if (!IsSceneColorRTDesc(d)) return false;

        // Exclude mirror backbuffer if we can resolve swapchain size
        UINT sw = 0, sh = 0; bool haveSwap = GetSwapChainSize(sw, sh);
        if (haveSwap && d.Width == sw && d.Height == sh) {
            return false;
        }

        // Prefer OpenVR-derived per-eye sizes when available
        uint32_t eyeW = 0, eyeH = 0;
        bool gotL = DLSSHooks::GetPerEyeDisplaySize(0, eyeW, eyeH);
        if (!gotL) {
            (void)DLSSHooks::GetPerEyeDisplaySize(1, eyeW, eyeH);
        }
        if (eyeW > 0 && eyeH > 0) {
            // Accept single-eye RT (≈ eyeW x eyeH) or SxS atlas (≈ 2*eyeW x eyeH)
            if ((ApproxEqUINT(d.Width, eyeW) && ApproxEqUINT(d.Height, eyeH)) ||
                (ApproxEqUINT(d.Width, eyeW * 2u) && ApproxEqUINT(d.Height, eyeH))) {
                return true;
            }
            // Also accept DLSS render-size candidates (e.g., 1344x1488 per eye, 2688x1488 combined)
            uint32_t prW = 0, prH = 0;
            if (g_dlssManager && g_dlssManager->ComputeRenderSizeForOutput(eyeW, eyeH, prW, prH)) {
                if ((ApproxEqUINT(d.Width, prW) && ApproxEqUINT(d.Height, prH)) ||
                    (ApproxEqUINT(d.Width, prW * 2u) && ApproxEqUINT(d.Height, prH))) {
                    return true;
                }
            }
        }

        // Fallback: accept very wide SxS-like atlases (width >= 1.7*height) with large dimensions
        if (d.Width >= (UINT)(d.Height * 1.7f) && d.Width >= 2500 && d.Height >= 1200) {
            return true;
        }

        // Also accept tall-ish single-eye frames typical for VR (e.g., ~2016x2232)
        if (d.Height >= 2000 && d.Width >= 1500) {
            return true;
        }

        return false;
    }

    void STDMETHODCALLTYPE HookedOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT numRTVs, ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV) {
        const bool earlyActive = IsEarlyDlssActive();
        // Composite small->big if a post/HUD big RT is bound after redirect
        // but never re-enter while we are inside the composite blit itself
        if (earlyActive && !g_inComposite.load(std::memory_order_relaxed) && ppRTVs && numRTVs > 0 && ppRTVs[0]) {
            CompositeIfNeededOnBigBind(ppRTVs[0]);
        }
        if (!ppRTVs || numRTVs == 0 || !ppRTVs[0]) {
            if (RealOMSetRenderTargets) RealOMSetRenderTargets(ctx, numRTVs, ppRTVs, pDSV);
            return;
        }
        // Detect scene begin/end only when Early DLSS logic is active
        if (earlyActive) {
            D3D11_TEXTURE2D_DESC d{};
            if (GetDescFromRTV(ppRTVs[0], &d)) {
                if (IsLikelyVRSceneRT(d)) {
                    g_sceneRTDesc = d;
                    g_sceneActive.store(true, std::memory_order_relaxed);
                    if (DlssTraceEnabled()) {
                        _MESSAGE("[EarlyDLSS][SceneBegin] RTV=%ux%u fmt=%u", d.Width, d.Height, (unsigned)d.Format);
                    }
                } else {
                    // If we detect the mirror swapchain backbuffer, explicitly end scene for clamp
                    UINT sw=0, sh=0;
                    if (GetSwapChainSize(sw, sh) && d.Width==sw && d.Height==sh) {
                        if (g_sceneActive.load(std::memory_order_relaxed)) {
                            g_sceneActive.store(false, std::memory_order_relaxed);
                            if (DlssTraceEnabled() && g_clampLogBudgetPerFrame > 0) {
                                _MESSAGE("[EarlyDLSS][SceneEnd] Mirror backbuffer bound %ux%u - clamp disabled", d.Width, d.Height);
                                --g_clampLogBudgetPerFrame;
                            }
                        }
                    }
                }
            }
        } else {
            g_sceneActive.store(false, std::memory_order_relaxed);
        }

        // Redirect only when enabled and mode == rt_redirect (1) and only once per frame
        bool didRedirect = false;
        if (!g_inComposite.load(std::memory_order_relaxed) &&
            earlyActive && g_dlssConfig->earlyDlssMode == 1 && !g_redirectUsedThisFrame.load(std::memory_order_relaxed)) {
            // Only attempt redirect when the currently bound RTV also looks like the VR scene RT
            D3D11_TEXTURE2D_DESC boundDesc{};
            bool haveBound = GetDescFromRTV(ppRTVs[0], &boundDesc);
            if (haveBound && !IsLikelyVRSceneRT(boundDesc)) {
                // Skip redirect on mirror/backbuffer or unrelated passes
                if (RealOMSetRenderTargets) RealOMSetRenderTargets(ctx, numRTVs, ppRTVs, pDSV);
                return;
            }
            if (IsSceneColorRTDesc(g_sceneRTDesc)) {
                // Compute target output and predicted render size
                uint32_t outLw=0, outLh=0, outRw=0, outRh=0;
                (void)DLSSHooks::GetPerEyeDisplaySize(0, outLw, outLh);
                (void)DLSSHooks::GetPerEyeDisplaySize(1, outRw, outRh);
                uint32_t tgtOutW = outLw ? outLw : outRw;
                uint32_t tgtOutH = outLh ? outLh : outRh;
                if (tgtOutW == 0 || tgtOutH == 0) {
                    // Fallback: infer per-eye dimensions from scene RT atlas shape
                    if (g_sceneRTDesc.Width >= (UINT)(g_sceneRTDesc.Height * 1.7f)) {
                        // Side-by-side atlas
                        tgtOutW = g_sceneRTDesc.Width / 2u;
                        tgtOutH = g_sceneRTDesc.Height;
                    } else if (g_sceneRTDesc.Height >= (UINT)(g_sceneRTDesc.Width * 1.7f)) {
                        // Top-bottom atlas
                        tgtOutW = g_sceneRTDesc.Width;
                        tgtOutH = g_sceneRTDesc.Height / 2u;
                    } else {
                        // Unknown, treat as single eye
                        tgtOutW = g_sceneRTDesc.Width;
                        tgtOutH = g_sceneRTDesc.Height;
                    }
                }
                uint32_t prW=0, prH=0;
                if (g_dlssManager && g_dlssManager->ComputeRenderSizeForOutput(tgtOutW, tgtOutH, prW, prH)) {
                    if (prW > 0 && prH > 0 && (prW < g_sceneRTDesc.Width || prH < g_sceneRTDesc.Height)) {
                        ID3D11RenderTargetView* smallRTV = GetOrCreateSmallRTVFor(ppRTVs[0], prW, prH);
                        if (smallRTV) {
                            // Build a local array replacing RTV[0]
                            std::vector<ID3D11RenderTargetView*> rtvs(numRTVs);
                            for (UINT i=0;i<numRTVs;++i) rtvs[i] = ppRTVs[i];
                            rtvs[0] = smallRTV;
                            if (DlssTraceEnabled()) {
                                _MESSAGE("[EarlyDLSS][Redirect] RTV old=%ux%u -> small=%ux%u", g_sceneRTDesc.Width, g_sceneRTDesc.Height, prW, prH);
                            }
                            if (RealOMSetRenderTargets) RealOMSetRenderTargets(ctx, numRTVs, rtvs.data(), pDSV);

                            // After redirect, scale existing viewports so both eyes map into the smaller RT.
                            // This repositions right-eye viewports (TopLeftX/TopLeftY) and scales dimensions.
                            const float sx = tgtOutW > 0 ? (float)prW / (float)tgtOutW : 1.0f;
                            const float sy = tgtOutH > 0 ? (float)prH / (float)tgtOutH : 1.0f;
                            if (sx > 0.0f && sy > 0.0f) {
                                UINT vpCount = 0;
                                ctx->RSGetViewports(&vpCount, nullptr);
                                if (vpCount > 0) {
                                    std::vector<D3D11_VIEWPORT> vps(vpCount);
                                    ctx->RSGetViewports(&vpCount, vps.data());
                                    for (UINT i = 0; i < vpCount; ++i) {
                                        D3D11_VIEWPORT& vp = vps[i];
                                        vp.TopLeftX = vp.TopLeftX * sx;
                                        vp.TopLeftY = vp.TopLeftY * sy;
                                        float newW = vp.Width * sx;
                                        float newH = vp.Height * sy;
                                        // Clamp to at least 1 pixel and even-align like the rest of the pipeline
                                        if (newW < 1.0f) newW = 1.0f;
                                        if (newH < 1.0f) newH = 1.0f;
                                        // Round to nearest integer for stability
                                        vp.Width  = floorf(newW + 0.5f);
                                        vp.Height = floorf(newH + 0.5f);
                                    }
                                    if (RealRSSetViewports) RealRSSetViewports(ctx, vpCount, vps.data());
                                    else ctx->RSSetViewports(vpCount, vps.data());
                                }
                            }
                            didRedirect = true;
                            g_redirectUsedThisFrame.store(true, std::memory_order_relaxed);
                            // Do not call Real again below
                            return;
                        }
                    }
                }
            }
        }

        if (!didRedirect) {
            if (RealOMSetRenderTargets) RealOMSetRenderTargets(ctx, numRTVs, ppRTVs, pDSV);
        }
    }

    void STDMETHODCALLTYPE HookedRSSetViewports(ID3D11DeviceContext* ctx, UINT count, const D3D11_VIEWPORT* viewports) {
        // Clamp viewports when Early DLSS is enabled (both clamp and redirect modes)
        if (!IsEarlyDlssActive() || !viewports || count == 0) {
            if (RealRSSetViewports) RealRSSetViewports(ctx, count, viewports);
            return;
        }
        // Only clamp inside a detected scene
        if (!g_sceneActive.load(std::memory_order_relaxed)) {
            if (DlssTraceEnabled() && g_clampLogBudgetPerFrame > 0) {
                _MESSAGE("[EarlyDLSS][CLAMP] skip: no scene active");
                --g_clampLogBudgetPerFrame;
            }
            if (RealRSSetViewports) RealRSSetViewports(ctx, count, viewports);
            return;
        }
        // Determine target per-eye output size (prefer left/right max)
        uint32_t outLw=0, outLh=0, outRw=0, outRh=0;
        (void)DLSSHooks::GetPerEyeDisplaySize(0, outLw, outLh);
        (void)DLSSHooks::GetPerEyeDisplaySize(1, outRw, outRh);
        uint32_t tgtOutW = outLw ? outLw : outRw;
        uint32_t tgtOutH = outLh ? outLh : outRh;
        if (tgtOutW == 0 || tgtOutH == 0) {
            // Fallback to scene RT size (not ideal for SxS atlases)
            tgtOutW = g_sceneRTDesc.Width;
            tgtOutH = g_sceneRTDesc.Height;
        }
        // Compute predicted render size
        uint32_t prW=0, prH=0;
        if (!g_dlssManager || !g_dlssManager->ComputeRenderSizeForOutput(tgtOutW, tgtOutH, prW, prH)) {
            if (DlssTraceEnabled() && g_clampLogBudgetPerFrame > 0) {
                _MESSAGE("[EarlyDLSS][CLAMP] skip: no optimal size for %ux%u", tgtOutW, tgtOutH);
                --g_clampLogBudgetPerFrame;
            }
            if (RealRSSetViewports) RealRSSetViewports(ctx, count, viewports);
            return;
        }
        if (prW == 0 || prH == 0) {
            if (DlssTraceEnabled() && g_clampLogBudgetPerFrame > 0) {
                _MESSAGE("[EarlyDLSS][CLAMP] skip: predicted size is zero");
                --g_clampLogBudgetPerFrame;
            }
            if (RealRSSetViewports) RealRSSetViewports(ctx, count, viewports);
            return;
        }
        // Prepare a modified copy of the viewport array
        std::vector<D3D11_VIEWPORT> vps(viewports, viewports + count);
        bool anyClamped = false;
        bool anyMatched = false;
        auto approxEq = [](float a, float b) {
            float diff = fabsf(a - b);
            float tol = b * 0.05f; // 5% tolerance
            return diff <= std::max(2.0f, tol);
        };
        for (UINT i = 0; i < count; ++i) {
            D3D11_VIEWPORT& vp = vps[i];
            if (approxEq(vp.Width, (float)tgtOutW) && approxEq(vp.Height, (float)tgtOutH)) {
                anyMatched = true;
                if (!approxEq(vp.Width, (float)prW) || !approxEq(vp.Height, (float)prH)) {
                    if (DlssTraceEnabled() && g_clampLogBudgetPerFrame > 0) {
                        _MESSAGE("[EarlyDLSS][CLAMP] vp old=(%.0fx%.0f) -> new=(%ux%u)", vp.Width, vp.Height, prW, prH);
                        --g_clampLogBudgetPerFrame;
                    }
                    vp.Width  = (float)prW;
                    vp.Height = (float)prH;
                    anyClamped = true;
                } else if (DlssTraceEnabled() && g_clampLogBudgetPerFrame > 0) {
                    _MESSAGE("[EarlyDLSS][CLAMP] skip: already at predicted size (%ux%u)", prW, prH);
                    --g_clampLogBudgetPerFrame;
                }
            }
        }
        if (RealRSSetViewports) {
            RealRSSetViewports(ctx, count, vps.data());
        }
        if (!anyClamped && !anyMatched && DlssTraceEnabled() && g_clampLogBudgetPerFrame > 0) {
            _MESSAGE("[EarlyDLSS][CLAMP] skip: no matching viewport for target %ux%u", tgtOutW, tgtOutH);
            --g_clampLogBudgetPerFrame;
        }
        (void)anyClamped; (void)anyMatched;
    }
}

static ID3D11RenderTargetView* GetOrCreateSmallRTVFor(ID3D11RenderTargetView* bigRTV, UINT prW, UINT prH) {
    if (!bigRTV || !g_device) return nullptr;
    ID3D11Resource* res = nullptr;
    bigRTV->GetResource(&res);
    if (!res) return nullptr;
    ID3D11Texture2D* bigTex = nullptr;
    HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&bigTex);
    res->Release();
    if (FAILED(hr) || !bigTex) return nullptr;

    D3D11_TEXTURE2D_DESC d{};
    bigTex->GetDesc(&d);
    std::lock_guard<std::mutex> lock(g_redirectMutex);
    RedirectEntry& e = g_redirectMap[bigTex];
    // If no entry or mismatched size/format, (re)create
    if (!e.smallTex || e.smallW != prW || e.smallH != prH || e.format != d.Format) {
        if (e.smallRTV) { e.smallRTV->Release(); e.smallRTV = nullptr; }
        if (e.smallTex) { e.smallTex->Release(); e.smallTex = nullptr; }
        D3D11_TEXTURE2D_DESC td = d;
        td.Width = prW; td.Height = prH; td.MipLevels = 1; td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0; // must be 0 when Count==1
        td.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.BindFlags &= ~(D3D11_BIND_DEPTH_STENCIL);
        td.MiscFlags &= ~(D3D11_RESOURCE_MISC_SHARED);
        if (FAILED(g_device->CreateTexture2D(&td, nullptr, &e.smallTex))) {
            bigTex->Release();
            g_redirectMap.erase(bigTex);
            return nullptr;
        }
        if (FAILED(g_device->CreateRenderTargetView(e.smallTex, nullptr, &e.smallRTV))) {
            e.smallTex->Release(); e.smallTex = nullptr;
            bigTex->Release();
            g_redirectMap.erase(bigTex);
            return nullptr;
        }
        e.smallW = prW; e.smallH = prH; e.format = d.Format;
        if (DlssTraceEnabled()) {
            _MESSAGE("[EarlyDLSS][RT] Created small RT %ux%u for fmt=%u", prW, prH, (unsigned)d.Format);
        }
    }
    bigTex->Release();
    return e.smallRTV;
}

    // If a big scene RT gets rebound after redirect, composite small->big once
    static void CompositeIfNeededOnBigBind(ID3D11RenderTargetView* bigRTV) {
        if (!IsEarlyDlssActive()) return;
        if (!g_redirectUsedThisFrame.load(std::memory_order_relaxed) || g_compositedThisFrame) return;
        if (g_inComposite.load(std::memory_order_relaxed)) return; // avoid recursion
        if (!bigRTV) return;
        // Resolve big texture key
        ID3D11Resource* res=nullptr; bigRTV->GetResource(&res); if(!res) return; ID3D11Texture2D* bigTex=nullptr; if(FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&bigTex))){ res->Release(); return; } res->Release();
        // Lookup mapping
        RedirectEntry entry{};
        {
            std::lock_guard<std::mutex> lock(g_redirectMutex);
            auto it = g_redirectMap.find(bigTex);
            if (it == g_redirectMap.end() || !it->second.smallTex) { bigTex->Release(); return; }
            entry = it->second;
        }
        // Composite small->big via DLSS per-eye evaluate and atlas copy
        if (g_dlssManager && entry.smallTex) {
            g_inComposite.store(true, std::memory_order_relaxed);
            bool ok = false;
            // Determine per-eye output size (prefer IVRSystem recommendation)
            uint32_t outW = 0, outH = 0;
            (void)DLSSHooks::GetPerEyeDisplaySize(0, outW, outH);
            if (outW == 0 || outH == 0) {
                // Fallback: derive from big RT size if double-wide
                D3D11_TEXTURE2D_DESC bigDesc{}; bigTex->GetDesc(&bigDesc);
                if (bigDesc.Width >= (UINT)(bigDesc.Height * 1.7f)) { outW = bigDesc.Width / 2u; outH = bigDesc.Height; }
            }
            if (outW && outH) {
                // Evaluate per-eye
                ID3D11Texture2D* depth = g_fallbackDepthTexture;
                ID3D11Texture2D* mv = g_motionVectorTexture;
                ID3D11Texture2D* leftOut = g_dlssManager->ProcessLeftEye(entry.smallTex, depth, mv);
                ID3D11Texture2D* rightOut = g_dlssManager->ProcessRightEye(entry.smallTex, depth, mv);
                if (leftOut && rightOut && g_context) {
                    D3D11_TEXTURE2D_DESC l{}; leftOut->GetDesc(&l);
                    D3D11_TEXTURE2D_DESC r{}; rightOut->GetDesc(&r);
                    // Copy per-eye outputs into big texture halves BEFORE binding it
                    D3D11_BOX src{}; src.front = 0; src.top = 0; src.left = 0; src.back = 1;
                    // Left half
                    src.right = l.Width; src.bottom = l.Height;
                    g_context->CopySubresourceRegion(bigTex, 0, 0, 0, 0, leftOut, 0, &src);
                    // Right half
                    src.right = r.Width; src.bottom = r.Height;
                    g_context->CopySubresourceRegion(bigTex, 0, outW, 0, 0, rightOut, 0, &src);
                    ok = true;
                }
            }
            if (ok) {
                g_compositedThisFrame = true;
                if (DlssTraceEnabled()) {
                    _MESSAGE("[EarlyDLSS][Composite] DLSS engine-copy to %ux%u (per-eye out %ux%u)", g_sceneRTDesc.Width, g_sceneRTDesc.Height, outW, outH);
                }
            } else if (DlssTraceEnabled()) {
                _MESSAGE("[EarlyDLSS][Composite] DLSS engine-copy skipped (no sizes or outputs)");
            }
            g_inComposite.store(false, std::memory_order_relaxed);
        }
        bigTex->Release();
    }
    

