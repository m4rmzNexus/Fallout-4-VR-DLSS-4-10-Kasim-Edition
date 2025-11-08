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
    PFN_FactoryCreateSwapChain RealFactoryCreateSwapChain = nullptr;
    PFN_RSSetViewports RealRSSetViewports = nullptr;
    PFN_OMSetRenderTargets RealOMSetRenderTargets = nullptr;

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

        HRESULT result = RealResizeBuffers
            ? RealResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags)
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

    void ProcessVREyeTexture(ID3D11Texture2D* eyeTexture, bool isLeftEye) {
        if (!g_dlssManager || !g_dlssManager->IsEnabled() || !g_dlssRuntimeInitialized) {
            return;
        }

        if (!eyeTexture) {
            return;
        }

        ID3D11Texture2D* upscaledTexture = nullptr;
        ID3D11Texture2D* depthTexture = g_fallbackDepthTexture;
        ID3D11Texture2D* motionVectors = g_motionVectorTexture;

        if (isLeftEye) {
            upscaledTexture = g_dlssManager->ProcessLeftEye(eyeTexture, depthTexture, motionVectors);
        } else {
            upscaledTexture = g_dlssManager->ProcessRightEye(eyeTexture, depthTexture, motionVectors);
        }

        (void)upscaledTexture;
    }

    void RegisterMotionVectorTexture(ID3D11Texture2D* motionTexture) {
        static bool logged = false;

        SafeAssignTexture(g_motionVectorTexture, motionTexture);

        if (motionTexture) {
            if (!logged) {
                _MESSAGE("Motion vector texture registered for DLSS");
                logged = true;
            }
        } else if (logged) {
            _MESSAGE("Motion vector texture cleared for DLSS");
            logged = false;
        }
    }

    void RegisterFallbackDepthTexture(ID3D11Texture2D* depthTexture,
                                      const D3D11_TEXTURE2D_DESC* desc,
                                      UINT targetWidth,
                                      UINT targetHeight) {
        static bool logged = false;
        static float bestScore = std::numeric_limits<float>::max();
        static UINT bestWidth = 0;
        static UINT bestHeight = 0;

        if (!depthTexture) {
            SafeAssignTexture(g_fallbackDepthTexture, nullptr);
            bestScore = std::numeric_limits<float>::max();
            bestWidth = bestHeight = 0;
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

        const float idealRatio = 0.66f;
        float widthRatio = targetWidth ? static_cast<float>(resolvedDesc->Width) / static_cast<float>(targetWidth) : idealRatio;
        float score = std::fabs(widthRatio - idealRatio);

        if (score > bestScore + 0.05f) {
            return;
        }

        if (score < bestScore - 0.05f || resolvedDesc->Width > bestWidth) {
            SafeAssignTexture(g_fallbackDepthTexture, depthTexture);
            bestScore = score;
            bestWidth = resolvedDesc->Width;
            bestHeight = resolvedDesc->Height;
            if (!logged) {
                _MESSAGE("Fallback depth texture registered for DLSS");
                logged = true;
            }
            _MESSAGE("Registered fallback depth: %ux%u fmt=%u", resolvedDesc->Width, resolvedDesc->Height, (unsigned)resolvedDesc->Format);
        }
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

    VRSubmitFn g_realVRSubmit = nullptr;
    bool g_vrSubmitHookInstalled = false;
    bool g_loggedSubmitFailure = false;
    // Per-eye display (output) sizes tracked from Submit bounds
    static std::atomic<uint32_t> g_perEyeOutW[2] = {0,0};
    static std::atomic<uint32_t> g_perEyeOutH[2] = {0,0};

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

        ID3D11Texture2D* colorTexture = ExtractColorTexture(texture);
        ID3D11Texture2D* depthTexture = ExtractDepthTexture(texture, flags);
        if (!depthTexture) {
            depthTexture = g_fallbackDepthTexture;
        }

        ID3D11Texture2D* motionVectors = g_motionVectorTexture;
        ID3D11Texture2D* processedTexture = nullptr;        // Prefer small redirected RT as DLSS input if available
        if (colorTexture && g_dlssConfig && g_dlssConfig->earlyDlssEnabled && g_dlssConfig->earlyDlssMode == 1) {
            ID3D11Texture2D* candidateSmall = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_redirectMutex);
                // Look up by big color texture key
                auto it = g_redirectMap.find(colorTexture);
                if (it != g_redirectMap.end() && it->second.smallTex) {
                    candidateSmall = it->second.smallTex;
                    if (g_dlssConfig->debugEarlyDlss) {
                        _MESSAGE("[EarlyDLSS][Submit] Using small RT as DLSS input");
                    }
                }
            }
            if (candidateSmall) {
                colorTexture = candidateSmall;
            }
        }
        const bool dlssReady = EnsureDLSSRuntimeReady();

        // Track per-eye display size using OpenVR recommended target size (more stable than texture size)
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
                            if (g_dlssConfig && g_dlssConfig->debugEarlyDlss) {
                                _MESSAGE("[EarlyDLSS][SIZE] SxS atlas detected: per-eye=%ux%u from full=%ux%u", recW, recH, fullW, fullH);
                            }
                        } else if (fullH >= (uint32_t)((double)fullW * 1.7)) {
                            // top-bottom
                            recW = fullW;
                            recH = fullH / 2u;
                            if (g_dlssConfig && g_dlssConfig->debugEarlyDlss) {
                                _MESSAGE("[EarlyDLSS][SIZE] T/B atlas detected: per-eye=%ux%u from full=%ux%u", recW, recH, fullW, fullH);
                            }
                        }
                    }
                }
            }
            // even-align
            recW &= ~1u; recH &= ~1u;
            // Optional per-eye cap to keep sizes sane
            if (g_dlssConfig && g_dlssConfig->enablePerEyeCap && g_dlssConfig->perEyeMaxDim > 0) {
                const uint32_t cap = (uint32_t)g_dlssConfig->perEyeMaxDim;
                uint32_t maxDim = recW > recH ? recW : recH;
                if (maxDim > cap && maxDim > 0) {
                    const double scale = (double)cap / (double)maxDim;
                    uint32_t newW = (uint32_t)std::max(1.0, std::floor((double)recW * scale));
                    uint32_t newH = (uint32_t)std::max(1.0, std::floor((double)recH * scale));
                    // even-align after scale
                    newW &= ~1u; newH &= ~1u;
                    if (newW == 0) newW = 2; if (newH == 0) newH = 2;
                    if (g_dlssConfig->debugEarlyDlss) {
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
            if (g_dlssConfig && g_dlssConfig->debugEarlyDlss) {
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

        if (colorTexture && dlssReady) {
            const bool isLeftEye = (eye == vr::Eye_Left);
            processedTexture = isLeftEye
                ? g_dlssManager->ProcessLeftEye(colorTexture, depthTexture, motionVectors)
                : g_dlssManager->ProcessRightEye(colorTexture, depthTexture, motionVectors);
            // Track per-eye result for readiness state machine
            const int idx = (eye == vr::Eye_Left) ? 0 : 1;
            if (processedTexture && processedTexture != colorTexture) {
                g_upscaledEyeTex[idx] = processedTexture;
                g_lastEvaluateOk.store(true, std::memory_order_relaxed);
                if (g_state.load(std::memory_order_relaxed) != DlssState::Ready) {
                    g_state.store(DlssState::Ready, std::memory_order_relaxed);
                }
            } else {
                g_upscaledEyeTex[idx] = nullptr;
                g_lastEvaluateOk.store(false, std::memory_order_relaxed);
            }
        }

        // One-time debug: confirm what we submit to compositor
        static int s_submitLogCount = 0;
        if (s_submitLogCount < 2) {
            D3D11_TEXTURE2D_DESC inDesc{}; if (colorTexture) colorTexture->GetDesc(&inDesc);
            D3D11_TEXTURE2D_DESC outDesc{}; if (processedTexture) processedTexture->GetDesc(&outDesc);
            const bool usedDLSS = processedTexture && processedTexture != colorTexture;
            _MESSAGE("[DLSS][DBG] OnSubmit(eye=%s) in=%ux%u out=%ux%u used=%s flags=0x%X cs=%d",
                (eye==vr::Eye_Left?"L":"R"),
                inDesc.Width, inDesc.Height,
                usedDLSS ? outDesc.Width : inDesc.Width,
                usedDLSS ? outDesc.Height : inDesc.Height,
                usedDLSS ? "DLSS" : "Native",
                (unsigned)flags,
                (int)(texture?texture->eColorSpace:0));
            ++s_submitLogCount;
        }

        // Guard: only submit DLSS output when fully ready and output is valid
        bool canUseUpscaled = (g_state.load(std::memory_order_relaxed) == DlssState::Ready) &&
                              g_lastEvaluateOk.load(std::memory_order_relaxed) &&
                              processedTexture && (processedTexture != colorTexture);

        // Validate sample count for processed texture; allow different dimensions
        if (canUseUpscaled) {
            D3D11_TEXTURE2D_DESC outDesc{}; processedTexture->GetDesc(&outDesc);
            if (outDesc.SampleDesc.Count != 1) {
                canUseUpscaled = false;
            }
        }

        if (canUseUpscaled) {
            // Prefer safest path: keep original texture handle for VR compositor and copy/blit DLSS result into it
            D3D11_TEXTURE2D_DESC inDesc{}; colorTexture->GetDesc(&inDesc);
            D3D11_TEXTURE2D_DESC outDesc{}; processedTexture->GetDesc(&outDesc);

            bool submitted = false;
            bool copied = false;
            do {
                // Fast path: exact match copy
                if (inDesc.Width == outDesc.Width && inDesc.Height == outDesc.Height && inDesc.Format == outDesc.Format) {
                    if (g_device && g_context) {
                        g_context->CopyResource(colorTexture, processedTexture);
                        _MESSAGE("[DLSS][Submit] Copied DLSS output into original eye texture (CopyResource)");
                        copied = true;
                    }
                }
                if (!copied && g_dlssManager && g_device) {
                    // Try linear blit into original using an RTV on the original eye texture
                    ID3D11RenderTargetView* rtv = nullptr;
                    if (SUCCEEDED(g_device->CreateRenderTargetView(colorTexture, nullptr, &rtv)) && rtv) {
                        if (g_dlssManager->BlitToRTV(processedTexture, rtv, inDesc.Width, inDesc.Height)) {
                            _MESSAGE("[DLSS][Submit] Copied DLSS output into original eye texture (Blit)");
                            copied = true;
                        }
                        rtv->Release();
                    }
                }
                // Submit original texture (with original bounds) after copy/blit
                if (copied) {
                    if (flags & vr::Submit_TextureWithDepth) {
                        const vr::VRTextureWithDepth_t* orig = reinterpret_cast<const vr::VRTextureWithDepth_t*>(texture);
                        return g_realVRSubmit(self, eye, reinterpret_cast<const vr::Texture_t*>(orig), bounds, flags);
                    }
                    return g_realVRSubmit(self, eye, texture, bounds, flags);
                }
            } while (false);

            // Fallback: replace handle if copy/blit failed; fix bounds when atlas->single conversion likely
            const vr::VRTextureBounds_t* submitBounds = bounds;
            vr::VRTextureBounds_t fixedBounds{ 0.0f, 1.0f, 0.0f, 1.0f };
            const bool dimsDiffer = (inDesc.Width != outDesc.Width) || (inDesc.Height != outDesc.Height);
            if (dimsDiffer && bounds) {
                const float uSpan = static_cast<float>(bounds->uMax - bounds->uMin);
                const float vSpan = static_cast<float>(bounds->vMax - bounds->vMin);
                const bool approxHalfU = (uSpan > 0.48f && uSpan < 0.52f);
                const bool approxHalfV = (vSpan > 0.48f && vSpan < 0.52f);
                const bool likelySxS = approxHalfU && (outDesc.Width * 2u <= inDesc.Width + 8u);
                const bool likelyTB  = approxHalfV && (outDesc.Height * 2u <= inDesc.Height + 8u);
                if (likelySxS || likelyTB) {
                    submitBounds = &fixedBounds;
                    _MESSAGE("[DLSS][Submit] Adjusted bounds for per-eye output (atlas->single)");
                }
            }

            _MESSAGE("[DLSS][Submit] Fallback to handle replacement path");
            if (flags & vr::Submit_TextureWithDepth) {
                vr::VRTextureWithDepth_t textureCopy = *reinterpret_cast<const vr::VRTextureWithDepth_t*>(texture);
                textureCopy.handle = processedTexture;
                return g_realVRSubmit(self, eye, reinterpret_cast<const vr::Texture_t*>(&textureCopy), submitBounds, flags);
            }
            vr::Texture_t textureCopy = *texture;
            textureCopy.handle = processedTexture;
            textureCopy.eType = vr::TextureType_DirectX;
            return g_realVRSubmit(self, eye, &textureCopy, submitBounds, flags);
        }

        return g_realVRSubmit(self, eye, texture, bounds, flags);
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

        if (HookVTableFunction(compositor, 6, HookedVRCompositorSubmit, &g_realVRSubmit)) {
            g_vrSubmitHookInstalled = true;
            g_loggedSubmitFailure = false;
            _MESSAGE("OpenVR Submit hook installed successfully");
            g_state.store(DlssState::HaveCompositor, std::memory_order_relaxed);
        } else if (!g_loggedSubmitFailure) {
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
        if (desc.Format != DXGI_FORMAT_R16G16_FLOAT) {
            return false;
        }

        const UINT requiredFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if ((desc.BindFlags & requiredFlags) != requiredFlags) {
            return false;
        }

        if (targetWidth && targetHeight) {
            if (desc.Width != targetWidth || desc.Height != targetHeight) {
                return false;
            }
        }

        if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
            return false;
        }

        return true;
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

        if (IsMotionVectorCandidate(desc, matchWidth, matchHeight)) {
            DLSSHooks::RegisterMotionVectorTexture(texture);
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

        HRESULT result = RealCreateTexture2D(device, desc, initialData, texture);
        if (FAILED(result) || !desc || !texture || !*texture) {
            return result;
        }

        DetectSpecialTextures(*desc, *texture);
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

    void STDMETHODCALLTYPE HookedOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT numRTVs, ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV) {
        // Composite small->big if a post/HUD big RT is bound after redirect
        if (ppRTVs && numRTVs > 0 && ppRTVs[0]) { CompositeIfNeededOnBigBind(ppRTVs[0]); }
        if (!ppRTVs || numRTVs == 0 || !ppRTVs[0]) {
            if (RealOMSetRenderTargets) RealOMSetRenderTargets(ctx, numRTVs, ppRTVs, pDSV);
            return;
        }
        // Detect scene begin on any mode
        D3D11_TEXTURE2D_DESC d{};
        if (GetDescFromRTV(ppRTVs[0], &d)) {
            if (IsSceneColorRTDesc(d)) {
                g_sceneRTDesc = d;
                g_sceneActive.store(true, std::memory_order_relaxed);
                if (g_dlssConfig && g_dlssConfig->debugEarlyDlss) {
                    _MESSAGE("[EarlyDLSS][SceneBegin] RTV=%ux%u fmt=%u", d.Width, d.Height, (unsigned)d.Format);
                }
            }
        }

        // Redirect only when enabled and mode == rt_redirect (1) and only once per frame
        bool didRedirect = false;
        if (g_dlssConfig && g_dlssConfig->earlyDlssEnabled && g_dlssConfig->earlyDlssMode == 1 && !g_redirectUsedThisFrame.load(std::memory_order_relaxed)) {
            if (IsSceneColorRTDesc(g_sceneRTDesc)) {
                // Compute target output and predicted render size
                uint32_t outLw=0, outLh=0, outRw=0, outRh=0;
                (void)DLSSHooks::GetPerEyeDisplaySize(0, outLw, outLh);
                (void)DLSSHooks::GetPerEyeDisplaySize(1, outRw, outRh);
                uint32_t tgtOutW = outLw ? outLw : outRw;
                uint32_t tgtOutH = outLh ? outLh : outRh;
                if (tgtOutW == 0 || tgtOutH == 0) { tgtOutW = g_sceneRTDesc.Width; tgtOutH = g_sceneRTDesc.Height; }
                uint32_t prW=0, prH=0;
                if (g_dlssManager && g_dlssManager->ComputeRenderSizeForOutput(tgtOutW, tgtOutH, prW, prH)) {
                    if (prW > 0 && prH > 0 && (prW < g_sceneRTDesc.Width || prH < g_sceneRTDesc.Height)) {
                        ID3D11RenderTargetView* smallRTV = GetOrCreateSmallRTVFor(ppRTVs[0], prW, prH);
                        if (smallRTV) {
                            // Build a local array replacing RTV[0]
                            std::vector<ID3D11RenderTargetView*> rtvs(numRTVs);
                            for (UINT i=0;i<numRTVs;++i) rtvs[i] = ppRTVs[i];
                            rtvs[0] = smallRTV;
                            if (g_dlssConfig->debugEarlyDlss) {
                                _MESSAGE("[EarlyDLSS][Redirect] RTV old=%ux%u -> small=%ux%u", g_sceneRTDesc.Width, g_sceneRTDesc.Height, prW, prH);
                            }
                            if (RealOMSetRenderTargets) RealOMSetRenderTargets(ctx, numRTVs, rtvs.data(), pDSV);
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
        // Default: pass through
        if (!g_dlssConfig || !g_dlssConfig->earlyDlssEnabled || g_dlssConfig->earlyDlssMode != 0 || !viewports || count == 0) {
            if (RealRSSetViewports) RealRSSetViewports(ctx, count, viewports);
            return;
        }
        // Only clamp inside a detected scene
        if (!g_sceneActive.load(std::memory_order_relaxed)) {
            if (g_dlssConfig->debugEarlyDlss && g_clampLogBudgetPerFrame > 0) {
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
            if (g_dlssConfig->debugEarlyDlss && g_clampLogBudgetPerFrame > 0) {
                _MESSAGE("[EarlyDLSS][CLAMP] skip: no optimal size for %ux%u", tgtOutW, tgtOutH);
                --g_clampLogBudgetPerFrame;
            }
            if (RealRSSetViewports) RealRSSetViewports(ctx, count, viewports);
            return;
        }
        if (prW == 0 || prH == 0) {
            if (g_dlssConfig->debugEarlyDlss && g_clampLogBudgetPerFrame > 0) {
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
                    if (g_dlssConfig->debugEarlyDlss && g_clampLogBudgetPerFrame > 0) {
                        _MESSAGE("[EarlyDLSS][CLAMP] vp old=(%.0fx%.0f) -> new=(%ux%u)", vp.Width, vp.Height, prW, prH);
                        --g_clampLogBudgetPerFrame;
                    }
                    vp.Width  = (float)prW;
                    vp.Height = (float)prH;
                    anyClamped = true;
                } else if (g_dlssConfig->debugEarlyDlss && g_clampLogBudgetPerFrame > 0) {
                    _MESSAGE("[EarlyDLSS][CLAMP] skip: already at predicted size (%ux%u)", prW, prH);
                    --g_clampLogBudgetPerFrame;
                }
            }
        }
        if (RealRSSetViewports) {
            RealRSSetViewports(ctx, count, vps.data());
        }
        if (!anyClamped && !anyMatched && g_dlssConfig->debugEarlyDlss && g_clampLogBudgetPerFrame > 0) {
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
        if (g_dlssConfig && g_dlssConfig->debugEarlyDlss) {
            _MESSAGE("[EarlyDLSS][RT] Created small RT %ux%u for fmt=%u", prW, prH, (unsigned)d.Format);
        }
    }
    bigTex->Release();
    return e.smallRTV;
}

    // If a big scene RT gets rebound after redirect, composite small->big once
    static void CompositeIfNeededOnBigBind(ID3D11RenderTargetView* bigRTV) {
        if (!g_dlssConfig || !g_dlssConfig->earlyDlssEnabled) return;
        if (!g_redirectUsedThisFrame.load(std::memory_order_relaxed) || g_compositedThisFrame) return;
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
        // Composite small->big; optional HQ path can be enabled via config
        if (g_dlssManager && entry.smallTex) {
            bool ok = false;
            if (g_dlssConfig && g_dlssConfig->highQualityComposite) {
                // Placeholder: fall back to linear blit for now
                if (g_dlssConfig->debugEarlyDlss) {
                    _MESSAGE("[EarlyDLSS][Composite] HQ path enabled (linear fallback)");
                }
            }
            // Default linear blit
            ok = g_dlssManager->BlitToRTV(entry.smallTex, bigRTV, g_sceneRTDesc.Width, g_sceneRTDesc.Height);
            if (ok) {
                g_compositedThisFrame = true;
                if (g_dlssConfig->debugEarlyDlss) {
                    _MESSAGE("[EarlyDLSS][Composite] small->big %ux%u", g_sceneRTDesc.Width, g_sceneRTDesc.Height);
                }
            }
        }
        bigTex->Release();
    }
