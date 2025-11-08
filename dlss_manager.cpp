#include "dlss_manager.h"
#include "dlss_config.h"
#include "dlss_hooks.h"
#include "backends/IUpscaleBackend.h"
#if USE_STREAMLINE
#include "backends/SLBackend.h"
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#endif
#include "common/IDebugLog.h"

#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>
#include <shlobj.h>
#include <d3dcompiler.h>

#if __has_include(<nvsdk_ngx.h>) && __has_include(<nvsdk_ngx_params.h>)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#else
#error "NVIDIA NGX SDK headers not found. Set NGX_SDK_PATH to your NGX SDK root or place the headers in the include path."
#endif
namespace {
    HMODULE g_ngxModule = nullptr;

    std::string WideToUtf8(const std::wstring& value) {
        if (value.empty()) {
            return {};
        }

        const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            return {};
        }

        std::string result(static_cast<size_t>(required - 1), '\0');
        if (!result.empty()) {
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], required, nullptr, nullptr);
        }
        return result;
    }

    std::wstring GetPluginDirectory() {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&GetPluginDirectory),
                                &module)) {
            return {};
        }

        wchar_t buffer[MAX_PATH];
        const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(_countof(buffer)));
        if (length == 0 || length >= _countof(buffer)) {
            return {};
        }

        std::wstring path(buffer, buffer + length);
        const size_t slash = path.find_last_of(L"/\\");
        if (slash != std::wstring::npos) {
            path.erase(slash + 1);
        } else {
            path.clear();
        }
        return path;
    }

    decltype(&NVSDK_NGX_D3D11_Init_with_ProjectID) g_pfnNGXInitProjectId = nullptr;
    decltype(&NVSDK_NGX_D3D11_Init) g_pfnNGXInit = nullptr;
    decltype(&NVSDK_NGX_D3D11_Shutdown1) g_pfnNGXShutdown = nullptr;
    decltype(&NVSDK_NGX_D3D11_GetCapabilityParameters) g_pfnNGXGetCapabilityParameters = nullptr;
    decltype(&NVSDK_NGX_D3D11_AllocateParameters) g_pfnNGXAllocateParameters = nullptr;
    decltype(&NVSDK_NGX_D3D11_DestroyParameters) g_pfnNGXDestroyParameters = nullptr;
    decltype(&NVSDK_NGX_D3D11_CreateFeature) g_pfnNGXCreateFeature = nullptr;
    decltype(&NVSDK_NGX_D3D11_ReleaseFeature) g_pfnNGXReleaseFeature = nullptr;
    decltype(&NVSDK_NGX_D3D11_EvaluateFeature) g_pfnNGXEvaluateFeature = nullptr;
    decltype(&NVSDK_NGX_D3D11_GetScratchBufferSize) g_pfnNGXGetScratchBufferSize = nullptr;
    // Diagnostics helpers (optional, resolved dynamically if present)
    using PFN_NGX_GetAPIVersion = NVSDK_NGX_Result (NVSDK_CONV*)(unsigned int*);
    using PFN_NGX_GetDriverVersion = NVSDK_NGX_Result (NVSDK_CONV*)(unsigned int*);
    using PFN_NGX_GetDriverVersionEx = NVSDK_NGX_Result (NVSDK_CONV*)(unsigned int*, unsigned int*);
    PFN_NGX_GetAPIVersion g_pfnNGXGetAPIVersion = nullptr;
    PFN_NGX_GetDriverVersion g_pfnNGXGetDriverVersion = nullptr;
    PFN_NGX_GetDriverVersionEx g_pfnNGXGetDriverVersionEx = nullptr;

    // Some NGX functions are no longer exported directly from nvngx_dlss.dll in recent SDKs.
    // We link against nvsdk_ngx_s.lib and, if GetProcAddress fails, fall back to the
    // statically linked entry points here.
    template <typename T>
    void TryAssignStatic(T& target, T staticFn) {
        if (!target && staticFn) {
            target = staticFn;
        }
    }

    template <typename T>
    void LoadNGXFunctionOptional(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(g_ngxModule, name));
        if (!target) {
            _MESSAGE("NGX export not found (will try stub): %s", name);
        }
    }

    std::wstring GetWritableNGXPath() {
        wchar_t docs[MAX_PATH] = {};
        if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYDOCUMENTS, NULL, 0, docs))) {
            return L".\\"; // fallback
        }
        std::wstring path = std::wstring(docs) + L"\\My Games\\Fallout4VR\\F4SE\\Plugins\\NGX\\";
        // ensure directory exists
        SHCreateDirectoryExW(NULL, path.c_str(), NULL);
        return path;
    }

    bool LoadNGXLibrary() {
        if (g_ngxModule) {
            return true;
        }

        std::wstring pluginDir = GetPluginDirectory();
        if (!pluginDir.empty()) {
            std::wstring localPath = pluginDir + L"nvngx_dlss.dll";
            g_ngxModule = LoadLibraryExW(localPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (g_ngxModule) {
                std::string pathUtf8 = WideToUtf8(localPath);
                _MESSAGE("Loaded nvngx_dlss.dll from plugin directory: %s", pathUtf8.c_str());
            } else {
                std::string pathUtf8 = WideToUtf8(localPath);
                _MESSAGE("Failed to load nvngx_dlss.dll from plugin directory (%s); trying process search path", pathUtf8.c_str());
            }
        }

        if (!g_ngxModule) {
            g_ngxModule = LoadLibraryW(L"nvngx_dlss.dll");
        }

        if (!g_ngxModule) {
            _ERROR("Failed to load nvngx_dlss.dll from plugin directory or process search path");
            return false;
        }

        // Try to resolve exports first (works with older NGX runtimes)
        LoadNGXFunctionOptional(g_pfnNGXInitProjectId, "NVSDK_NGX_D3D11_Init_with_ProjectID");
        LoadNGXFunctionOptional(g_pfnNGXInit, "NVSDK_NGX_D3D11_Init");
        LoadNGXFunctionOptional(g_pfnNGXShutdown, "NVSDK_NGX_D3D11_Shutdown1");
        LoadNGXFunctionOptional(g_pfnNGXGetCapabilityParameters, "NVSDK_NGX_D3D11_GetCapabilityParameters");
        LoadNGXFunctionOptional(g_pfnNGXAllocateParameters, "NVSDK_NGX_D3D11_AllocateParameters");
        LoadNGXFunctionOptional(g_pfnNGXDestroyParameters, "NVSDK_NGX_D3D11_DestroyParameters");
        LoadNGXFunctionOptional(g_pfnNGXCreateFeature, "NVSDK_NGX_D3D11_CreateFeature");
        LoadNGXFunctionOptional(g_pfnNGXReleaseFeature, "NVSDK_NGX_D3D11_ReleaseFeature");
        LoadNGXFunctionOptional(g_pfnNGXEvaluateFeature, "NVSDK_NGX_D3D11_EvaluateFeature");
        LoadNGXFunctionOptional(g_pfnNGXGetScratchBufferSize, "NVSDK_NGX_D3D11_GetScratchBufferSize");
        // Diagnostics-only exports (best-effort)
        LoadNGXFunctionOptional(g_pfnNGXGetAPIVersion, "NVSDK_NGX_GetAPIVersion");
        LoadNGXFunctionOptional(g_pfnNGXGetDriverVersion, "NVSDK_NGX_GetDriverVersion");
        LoadNGXFunctionOptional(g_pfnNGXGetDriverVersionEx, "NVSDK_NGX_GetDriverVersionEx");

        // Fallback to statically linked functions where exports are missing (required for NGX 3.1+)
        TryAssignStatic(g_pfnNGXInit, &NVSDK_NGX_D3D11_Init);
        // NVSDK_NGX_D3D11_Init_with_ProjectID may not be exported; ok if null.
        TryAssignStatic(g_pfnNGXShutdown, &NVSDK_NGX_D3D11_Shutdown1);
        TryAssignStatic(g_pfnNGXGetCapabilityParameters, &NVSDK_NGX_D3D11_GetCapabilityParameters);
        TryAssignStatic(g_pfnNGXAllocateParameters, &NVSDK_NGX_D3D11_AllocateParameters);
        TryAssignStatic(g_pfnNGXDestroyParameters, &NVSDK_NGX_D3D11_DestroyParameters);
        TryAssignStatic(g_pfnNGXCreateFeature, &NVSDK_NGX_D3D11_CreateFeature);
        TryAssignStatic(g_pfnNGXReleaseFeature, &NVSDK_NGX_D3D11_ReleaseFeature);
        TryAssignStatic(g_pfnNGXEvaluateFeature, &NVSDK_NGX_D3D11_EvaluateFeature);
        TryAssignStatic(g_pfnNGXGetScratchBufferSize, &NVSDK_NGX_D3D11_GetScratchBufferSize);
        // No static fallbacks for diagnostics

        // Validate minimal set
        const bool haveInit = (g_pfnNGXInitProjectId != nullptr) || (g_pfnNGXInit != nullptr);
        const bool haveParams = (g_pfnNGXGetCapabilityParameters != nullptr) &&
                                (g_pfnNGXAllocateParameters != nullptr) &&
                                (g_pfnNGXDestroyParameters != nullptr);
        const bool haveFeature = (g_pfnNGXCreateFeature != nullptr) &&
                                 (g_pfnNGXReleaseFeature != nullptr) &&
                                 (g_pfnNGXEvaluateFeature != nullptr);

        if (!haveInit || !haveParams || !haveFeature) {
            _ERROR("Failed to resolve required NGX entry points (init=%d, params=%d, feature=%d)",
                   haveInit ? 1 : 0, haveParams ? 1 : 0, haveFeature ? 1 : 0);
            FreeLibrary(g_ngxModule);
            g_ngxModule = nullptr;
            g_pfnNGXInitProjectId = nullptr;
            g_pfnNGXInit = nullptr;
            g_pfnNGXShutdown = nullptr;
            g_pfnNGXGetCapabilityParameters = nullptr;
            g_pfnNGXAllocateParameters = nullptr;
            g_pfnNGXDestroyParameters = nullptr;
            g_pfnNGXCreateFeature = nullptr;
            g_pfnNGXReleaseFeature = nullptr;
            g_pfnNGXEvaluateFeature = nullptr;
            g_pfnNGXGetScratchBufferSize = nullptr;
            return false;
        }

        _MESSAGE("[NGX] Pointers: Init=%p InitPID=%p GetCaps=%p Alloc=%p Free=%p Create=%p Eval=%p Release=%p Scratch=%p",
                 g_pfnNGXInit, g_pfnNGXInitProjectId, g_pfnNGXGetCapabilityParameters,
                 g_pfnNGXAllocateParameters, g_pfnNGXDestroyParameters,
                 g_pfnNGXCreateFeature, g_pfnNGXEvaluateFeature, g_pfnNGXReleaseFeature,
                 g_pfnNGXGetScratchBufferSize);

        if (g_pfnNGXGetAPIVersion) {
            unsigned int api = 0;
            if (NVSDK_NGX_SUCCEED(g_pfnNGXGetAPIVersion(&api))) {
                _MESSAGE("[NGX] Runtime API version: %u (Header=%u)", api, (unsigned int)NVSDK_NGX_Version_API);
            }
        }
        if (g_pfnNGXGetDriverVersion) {
            unsigned int drv = 0;
            if (NVSDK_NGX_SUCCEED(g_pfnNGXGetDriverVersion(&drv))) {
                _MESSAGE("[NGX] Driver version: %u", drv);
            }
        }

        return true;
    }

    NVSDK_NGX_PerfQuality_Value MapQuality(DLSSManager::Quality quality) {
        switch (quality) {
            case DLSSManager::Quality::Performance:
                return NVSDK_NGX_PerfQuality_Value_MaxPerf;
            case DLSSManager::Quality::Balanced:
                return NVSDK_NGX_PerfQuality_Value_Balanced;
            case DLSSManager::Quality::Quality:
                return NVSDK_NGX_PerfQuality_Value_MaxQuality;
            case DLSSManager::Quality::UltraPerformance:
                return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
            case DLSSManager::Quality::UltraQuality:
                return NVSDK_NGX_PerfQuality_Value_UltraQuality;
            case DLSSManager::Quality::DLAA:
                return NVSDK_NGX_PerfQuality_Value_DLAA;
            default:
                return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        }
    }
}

DLSSManager* g_dlssManager = nullptr;
DLSSConfig* g_dlssConfig = nullptr;

DLSSManager::DLSSManager() {
    if (m_useOptimalMipLodBias) {
        const float mip = -1.0f;
        m_manualMipLodBias = mip;
    }
}

DLSSManager::~DLSSManager() {
    Shutdown();
}

void DLSSManager::SetSharpness(float sharpness) {
    m_sharpness = sharpness;
#if USE_STREAMLINE
    if (m_backend) {
        m_backend->SetSharpness(sharpness);
    }
#endif
}

bool DLSSManager::Initialize() {
    if (m_initialized) {
        return true;
    }

    if (!InitializeDevice()) {
        return false;
    }

    // Prefer Streamline backend when available
#if USE_STREAMLINE
    if (!m_backend) {
        m_slBackend = new SLBackend();
        m_backend = m_slBackend;
    } else {
        m_slBackend = static_cast<SLBackend*>(m_backend);
    }
    if (m_backend && !m_backend->Init(m_device, m_context)) {
        _ERROR("[SL] Backend init failed; DLSS unavailable via SL");
        m_backend->Shutdown();
        delete m_backend;
        m_backend = nullptr;
        m_slBackend = nullptr;
    }
#else
    m_backend = nullptr;
#endif

    // Keep NGX path as fallback (if SL not used or failed)
    if (!m_backend || !m_backend->IsReady()) {
        _MESSAGE("[DLSS] Using NGX fallback path");
        if (!InitializeNGX()) {
            return false;
        }
    } else {
        _MESSAGE("[DLSS] Using Streamline backend (DLSS SR)");
    }

    m_initialized = true;
    return true;
}

bool DLSSManager::InitializeDevice() {
    if (m_device && m_context) {
        return true;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (!DLSSHooks::GetD3D11Device(&device, &context) || !device || !context) {
        return false;
    }

    m_device = device;
    m_device->AddRef();

    m_context = context;
    m_context->AddRef();
    return true;
}

bool DLSSManager::InitializeNGX() {
    if (!LoadNGXLibrary()) {
        return false;
    }

    if (m_ngxParameters) {
        return true;
    }

    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;

    // Prefer a writable NGX data path under Documents
    const std::wstring ngxDataPath = GetWritableNGXPath();
    _MESSAGE("[NGX] Using data path: %s", WideToUtf8(ngxDataPath).c_str());

    // Avoid Init_with_ProjectID to prevent NGX ProjectID policy issues

    if (!NVSDK_NGX_SUCCEED(result) && g_pfnNGXInit) {
        // Build a search path list to help NGX locate companion DLLs/models
        std::vector<std::wstring> ownedPaths;
        std::vector<const wchar_t*> pathPtrs;
        // Prefer plugin directory
        ownedPaths.push_back(GetPluginDirectory());
        // Also add Documents plugins folder
        ownedPaths.push_back(L"\\My Games\\Fallout4VR\\F4SE\\Plugins\\");
        // Ensure absolute for the second entry if needed by prefixing Documents root
        if (ownedPaths.back().front() == L'\\') {
            wchar_t docs[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYDOCUMENTS, NULL, 0, docs))) {
                ownedPaths.back() = std::wstring(docs) + ownedPaths.back();
            }
        }
        for (auto& w : ownedPaths) {
            if (!w.empty()) pathPtrs.push_back(w.c_str());
        }
        NVSDK_NGX_PathListInfo pathList{};
        pathList.Path = pathPtrs.empty() ? nullptr : pathPtrs.data();
        pathList.Length = static_cast<unsigned int>(pathPtrs.size());

        NVSDK_NGX_FeatureCommonInfo featureInfo{};
        featureInfo.PathListInfo = pathList;
        featureInfo.InternalData = nullptr; // NGX will handle if needed
        featureInfo.LoggingInfo.LoggingCallback = nullptr;
        featureInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        featureInfo.LoggingInfo.DisableOtherLoggingSinks = false;

        _MESSAGE("[NGX] PathListInfo length: %u", featureInfo.PathListInfo.Length);
        for (unsigned int i = 0; i < featureInfo.PathListInfo.Length; ++i) {
            _MESSAGE("[NGX]   Path[%u]=%s", i, WideToUtf8(featureInfo.PathListInfo.Path[i]).c_str());
        }

        result = g_pfnNGXInit(0, ngxDataPath.c_str(), m_device, &featureInfo, NVSDK_NGX_Version_API);
        _MESSAGE("[NGX] Init returned: 0x%08X", result);
    }

    if (!NVSDK_NGX_SUCCEED(result)) {
        _ERROR("NVSDK_NGX_D3D11_Init failed: 0x%08X", result);
        return false;
    }

    NVSDK_NGX_Parameter* capabilityParameters = nullptr;
    result = g_pfnNGXGetCapabilityParameters(&capabilityParameters);
    if (!NVSDK_NGX_SUCCEED(result) || !capabilityParameters) {
        _ERROR("Failed to query NGX capability parameters: 0x%08X", result);
        return false;
    }

    unsigned int dlssAvailable = 0;
    capabilityParameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable);
    if (!dlssAvailable) {
        _ERROR("NGX reports that Super Sampling is unavailable on this system");
        return false;
    }

    result = g_pfnNGXAllocateParameters(&m_ngxParameters);
    if (!NVSDK_NGX_SUCCEED(result) || !m_ngxParameters) {
        _ERROR("Failed to allocate NGX parameter block: 0x%08X", result);
        return false;
    }

    m_ngxParameters->Reset();
    m_ngxParameters->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
    return true;
}

namespace {
    struct QualityInfo {
        float scale;
        float mipBias;
    };

    QualityInfo GetQualityInfo(DLSSManager::Quality quality) {
        switch (quality) {
            case DLSSManager::Quality::Performance:
                return {0.50f, -1.0f};
            case DLSSManager::Quality::Balanced:
                return {0.58f, -0.75f};
            case DLSSManager::Quality::Quality:
                return {0.67f, -0.50f};
            case DLSSManager::Quality::UltraPerformance:
                return {0.33f, -1.585f};
            case DLSSManager::Quality::UltraQuality:
                return {0.77f, -0.25f};
            case DLSSManager::Quality::DLAA:
                return {1.0f, 0.0f};
            default:
                return {0.67f, -0.50f};
        }
    }
}

void DLSSManager::GetOptimalSettings(uint32_t& renderWidth, uint32_t& renderHeight) {
    const QualityInfo info = GetQualityInfo(m_quality);
    renderWidth = static_cast<uint32_t>(static_cast<float>(m_renderWidth) * info.scale);
    renderHeight = static_cast<uint32_t>(static_cast<float>(m_renderHeight) * info.scale);
    renderWidth = std::max(1u, renderWidth);
    renderHeight = std::max(1u, renderHeight);
}

namespace {
    void ReleaseTexture(ID3D11Texture2D*& texture) {
        if (texture) {
            texture->Release();
            texture = nullptr;
        }
    }
}

bool DLSSManager::CreateDLSSFeatures() {
    // Actual feature creation happens on-demand when we process an eye.
    return true;
}

void DLSSManager::SetQuality(Quality quality) {
    m_quality = quality;
    if (m_useOptimalMipLodBias) {
        m_manualMipLodBias = GetQualityInfo(quality).mipBias;
    }
#if USE_STREAMLINE
    if (m_backend) {
        m_backend->SetQuality(static_cast<int>(quality));
    }
#endif
    m_leftEye.requiresReset = true;
    m_rightEye.requiresReset = true;
    _MESSAGE("[CFG] Quality set to %d", static_cast<int>(quality));
}

void DLSSManager::SetSharpeningEnabled(bool enabled) {
    m_sharpeningEnabled = enabled;
}

void DLSSManager::SetUseOptimalMipLodBias(bool enabled) {
    m_useOptimalMipLodBias = enabled;
    if (m_useOptimalMipLodBias) {
        m_manualMipLodBias = GetQualityInfo(m_quality).mipBias;
    }
}

void DLSSManager::SetManualMipLodBias(float bias) {
    m_manualMipLodBias = bias;
    m_useOptimalMipLodBias = false;
}

void DLSSManager::SetRenderReShadeBeforeUpscaling(bool value) {
    m_renderReShadeBeforeUpscaling = value;
}

void DLSSManager::SetUpscaleDepthForReShade(bool value) {
    m_upscaleDepthForReShade = value;
}

void DLSSManager::SetUseTAAPeriphery(bool value) {
    m_useTAAPeriphery = value;
}

void DLSSManager::SetDLSSPreset(int preset) {
    m_dlssPreset = std::max(0, std::min(preset, 6));
}

void DLSSManager::SetFOV(float value) {
    m_fov = value;
}

void DLSSManager::SetFixedFoveatedRendering(bool enabled) {
    m_enableFixedFoveatedRendering = enabled;
}

void DLSSManager::SetFixedFoveatedUpscaling(bool enabled) {
    m_enableFixedFoveatedUpscaling = enabled;
}

void DLSSManager::SetFoveatedRadii(float inner, float middle, float outer) {
    m_foveatedInnerRadius = inner;
    m_foveatedMiddleRadius = middle;
    m_foveatedOuterRadius = outer;
}

void DLSSManager::SetFoveatedScale(float scaleX, float scaleY) {
    m_foveatedScaleX = scaleX;
    m_foveatedScaleY = scaleY;
}

void DLSSManager::SetFoveatedOffsets(float offsetX, float offsetY) {
    m_foveatedOffsetX = offsetX;
    m_foveatedOffsetY = offsetY;
}

void DLSSManager::SetFoveatedCutout(float cutoutRadius) {
    m_foveatedCutoutRadius = cutoutRadius;
}

void DLSSManager::SetFoveatedWiden(float widen) {
    m_foveatedWiden = widen;
}

bool DLSSManager::ComputeRenderSizeForOutput(uint32_t outW, uint32_t outH, uint32_t& renderW, uint32_t& renderH) {
    renderW = 0;
    renderH = 0;
    if (outW == 0 || outH == 0) {
        return false;
    }
#if USE_STREAMLINE
    // Prefer Streamline's DLSS optimal settings when backend is ready
    if (m_backend && m_backend->IsReady()) {
        auto MapToSLMode = [&](Quality q)->sl::DLSSMode {
            switch (q) {
                case Quality::Performance:       return sl::DLSSMode::eMaxPerformance;
                case Quality::Balanced:         return sl::DLSSMode::eBalanced;
                case Quality::Quality:          return sl::DLSSMode::eMaxQuality;
                case Quality::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
                case Quality::UltraQuality:     return sl::DLSSMode::eUltraQuality;
                case Quality::DLAA:             return sl::DLSSMode::eDLAA;
                default:                        return sl::DLSSMode::eMaxQuality;
            }
        };
        sl::DLSSOptions opts{};
        opts.mode = MapToSLMode(m_quality);
        opts.outputWidth = outW;
        opts.outputHeight = outH;
        sl::DLSSOptimalSettings os{};
        if (sl::Result::eOk == slDLSSGetOptimalSettings(opts, os)) {
            renderW = os.optimalRenderWidth;
            renderH = os.optimalRenderHeight;
        }
    }
#endif
    if (renderW == 0 || renderH == 0) {
        // Fallback: uniform scale from the static quality table
        const float s = GetQualityInfo(m_quality).scale;
        renderW = static_cast<uint32_t>(static_cast<float>(outW) * s);
        renderH = static_cast<uint32_t>(static_cast<float>(outH) * s);
    }
    // Even align and clamp to not exceed output
    renderW &= ~1u; renderH &= ~1u;
    if (renderW == 0) renderW = 2; if (renderH == 0) renderH = 2;
    if (renderW > outW)  renderW = outW;
    if (renderH > outH)  renderH = outH;
    return true;
}

bool DLSSManager::BlitToRTV(ID3D11Texture2D* src, ID3D11RenderTargetView* dstRTV, uint32_t dstW, uint32_t dstH) {
    if (!m_device || !m_context || !src || !dstRTV || dstW == 0 || dstH == 0) {
        return false;
    }
    if (!EnsureDownscaleShaders()) {
        return false;
    }

    // Create SRV for src
    ID3D11ShaderResourceView* srcSRV = nullptr;
    HRESULT hr = m_device->CreateShaderResourceView(src, nullptr, &srcSRV);
    if (FAILED(hr) || !srcSRV) {
        return false;
    }

    // Save minimal state
    ID3D11RenderTargetView* oldRTV = nullptr; ID3D11DepthStencilView* oldDSV = nullptr;
    m_context->OMGetRenderTargets(1, &oldRTV, &oldDSV);
    D3D11_VIEWPORT oldVP{}; UINT vpCount = 1; m_context->RSGetViewports(&vpCount, &oldVP);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo; m_context->IAGetPrimitiveTopology(&oldTopo);
    ID3D11VertexShader* oldVS = nullptr; ID3D11PixelShader* oldPS = nullptr;
    m_context->VSGetShader(&oldVS, nullptr, nullptr);
    m_context->PSGetShader(&oldPS, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSRV = nullptr; m_context->PSGetShaderResources(0, 1, &oldSRV);
    ID3D11SamplerState* oldSamp = nullptr; m_context->PSGetSamplers(0, 1, &oldSamp);

    // Set viewport and RT
    D3D11_VIEWPORT vp{}; vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)dstW; vp.Height = (float)dstH; vp.MinDepth = 0; vp.MaxDepth = 1;
    m_context->RSSetViewports(1, &vp);
    m_context->OMSetRenderTargets(1, &dstRTV, nullptr);

    // Bind FS pipeline
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_fsVS, nullptr, 0);
    m_context->PSSetShader(m_fsPS, nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &srcSRV);
    m_context->PSSetSamplers(0, 1, &m_linearSampler);
    m_context->Draw(3, 0);

    // Unbind and restore
    ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    m_context->PSSetShaderResources(0, 1, nullSRV);

    m_context->OMSetRenderTargets(1, &oldRTV, oldDSV);
    if (oldRTV) oldRTV->Release(); if (oldDSV) oldDSV->Release();
    m_context->RSSetViewports(vpCount, &oldVP);
    m_context->IASetPrimitiveTopology(oldTopo);
    m_context->VSSetShader(oldVS, nullptr, 0);
    m_context->PSSetShader(oldPS, nullptr, 0);
    if (oldVS) oldVS->Release(); if (oldPS) oldPS->Release();
    if (oldSRV) { ID3D11ShaderResourceView* r[1] = { oldSRV }; m_context->PSSetShaderResources(0, 1, r); oldSRV->Release(); }
    if (oldSamp) { ID3D11SamplerState* s[1] = { oldSamp }; m_context->PSSetSamplers(0, 1, s); oldSamp->Release(); }
    if (srcSRV) srcSRV->Release();
    return true;
}

namespace {
    bool CreateOutputTexture(ID3D11Device* device,
                              const D3D11_TEXTURE2D_DESC& inputDesc,
                              uint32_t width,
                              uint32_t height,
                              ID3D11Texture2D** outTexture) {
        if (!device || !outTexture) {
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = inputDesc;
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.BindFlags &= ~(D3D11_BIND_DEPTH_STENCIL);
        desc.MiscFlags &= ~(D3D11_RESOURCE_MISC_SHARED);

        ID3D11Texture2D* texture = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(hr)) {
            _ERROR("Failed to create DLSS output texture (%ux%u): HRESULT 0x%08X", width, height, hr);
            return false;
        }

        *outTexture = texture;
        return true;
    }
}

bool DLSSManager::EnsureEyeFeature(EyeContext& eye,
                                   ID3D11Texture2D* inputTexture,
                                   uint32_t renderWidth,
                                   uint32_t renderHeight,
                                   uint32_t outputWidth,
                                   uint32_t outputHeight) {
    if (!m_device || !m_context || !m_ngxParameters || !inputTexture) {
        return false;
    }

    if (eye.dlssHandle &&
        eye.renderWidth == renderWidth &&
        eye.renderHeight == renderHeight &&
        eye.outputWidth == outputWidth &&
        eye.outputHeight == outputHeight) {
        return true;
    }

    if (eye.dlssHandle) {
        g_pfnNGXReleaseFeature(eye.dlssHandle);
        eye.dlssHandle = nullptr;
    }

    ReleaseTexture(eye.outputTexture);

    D3D11_TEXTURE2D_DESC inputDesc = {};
    inputTexture->GetDesc(&inputDesc);

    if (!CreateOutputTexture(m_device, inputDesc, outputWidth, outputHeight, &eye.outputTexture)) {
        return false;
    }

    m_ngxParameters->Reset();

    m_ngxParameters->Set(NVSDK_NGX_Parameter_Width, renderWidth);
    m_ngxParameters->Set(NVSDK_NGX_Parameter_Height, renderHeight);
    m_ngxParameters->Set(NVSDK_NGX_Parameter_OutWidth, outputWidth);
    m_ngxParameters->Set(NVSDK_NGX_Parameter_OutHeight, outputHeight);
    m_ngxParameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<unsigned int>(MapQuality(m_quality)));
    m_ngxParameters->Set(NVSDK_NGX_Parameter_Sharpness, m_sharpness);
    m_ngxParameters->Set(NVSDK_NGX_Parameter_Reset, 1);

    size_t scratchSize = 0;
    if (g_pfnNGXGetScratchBufferSize) {
        NVSDK_NGX_Result scratchResult = g_pfnNGXGetScratchBufferSize(NVSDK_NGX_Feature_SuperSampling, m_ngxParameters, &scratchSize);
        if (!NVSDK_NGX_SUCCEED(scratchResult)) {
            scratchSize = 0;
        }
    }

    if (scratchSize > 0) {
        if (!CreateScratchBuffer(scratchSize)) {
            return false;
        }
        if (m_scratchBuffer) {
            m_ngxParameters->Set(NVSDK_NGX_Parameter_Scratch, static_cast<void*>(m_scratchBuffer));
            m_ngxParameters->Set(NVSDK_NGX_Parameter_Scratch_SizeInBytes, static_cast<unsigned long long>(m_scratchSize));
        }
    }

    NVSDK_NGX_Result result = g_pfnNGXCreateFeature(m_context, NVSDK_NGX_Feature_SuperSampling, m_ngxParameters, &eye.dlssHandle);
    if (!NVSDK_NGX_SUCCEED(result)) {
        _ERROR("NVSDK_NGX_D3D11_CreateFeature failed: 0x%08X", result);
        ReleaseTexture(eye.outputTexture);
        return false;
    }

    eye.renderWidth = renderWidth;
    eye.renderHeight = renderHeight;
    eye.outputWidth = outputWidth;
    eye.outputHeight = outputHeight;
    eye.requiresReset = true;
    return true;
}

bool DLSSManager::CreateScratchBuffer(size_t scratchSize) {
    if (scratchSize == 0) {
        ReleaseScratchBuffer();
        return true;
    }

    if (m_scratchBuffer && m_scratchSize >= scratchSize) {
        return true;
    }

    ReleaseScratchBuffer();

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(scratchSize);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

    ID3D11Buffer* buffer = nullptr;
    HRESULT hr = m_device->CreateBuffer(&desc, nullptr, &buffer);
    if (FAILED(hr)) {
        _ERROR("Failed to allocate NGX scratch buffer (%zu bytes): HRESULT 0x%08X", scratchSize, hr);
        return false;
    }

    m_scratchBuffer = buffer;
    m_scratchSize = scratchSize;
    return true;
}

void DLSSManager::ReleaseScratchBuffer() {
    if (m_scratchBuffer) {
        m_scratchBuffer->Release();
        m_scratchBuffer = nullptr;
        m_scratchSize = 0;
    }
}

void DLSSManager::ReleaseZeroMotionVectors() {
    if (m_zeroMotionVectors) {
        m_zeroMotionVectors->Release();
        m_zeroMotionVectors = nullptr;
        m_zeroMVWidth = m_zeroMVHeight = 0;
    }
}

bool DLSSManager::EnsureZeroMotionVectors(uint32_t width, uint32_t height) {
    if (!m_device || !m_context) {
        return false;
    }
    if (m_zeroMotionVectors && m_zeroMVWidth == width && m_zeroMVHeight == height) {
        return true;
    }
    ReleaseZeroMotionVectors();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &tex);
    if (FAILED(hr) || !tex) {
        _ERROR("Failed to allocate zero motion vector texture %ux%u: 0x%08X", width, height, hr);
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_context->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        const size_t rowBytes = static_cast<size_t>(width) * 4; // R16G16
        for (uint32_t y = 0; y < height; ++y) {
            memset(static_cast<char*>(mapped.pData) + y * mapped.RowPitch, 0, rowBytes);
        }
        m_context->Unmap(tex, 0);
    }

    m_zeroMotionVectors = tex;
    m_zeroMVWidth = width;
    m_zeroMVHeight = height;
    _MESSAGE("Zero motion vector texture created: %ux%u", width, height);
    return true;
}

void DLSSManager::ReleaseZeroDepthTexture() {
    if (m_zeroDepthTexture) {
        m_zeroDepthTexture->Release();
        m_zeroDepthTexture = nullptr;
        m_zeroDepthWidth = m_zeroDepthHeight = 0;
    }
}

bool DLSSManager::EnsureZeroDepthTexture(uint32_t width, uint32_t height) {
    if (!m_device || !m_context || width == 0 || height == 0) {
        return false;
    }
    if (m_zeroDepthTexture && m_zeroDepthWidth == width && m_zeroDepthHeight == height) {
        return true;
    }

    ReleaseZeroDepthTexture();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &tex);
    if (FAILED(hr) || !tex) {
        _ERROR("Failed to allocate zero depth texture %ux%u: 0x%08X", width, height, hr);
        return false;
    }

    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(m_device->CreateRenderTargetView(tex, nullptr, &rtv))) {
        const FLOAT clear[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        m_context->ClearRenderTargetView(rtv, clear);
        rtv->Release();
    }

    m_zeroDepthTexture = tex;
    m_zeroDepthWidth = width;
    m_zeroDepthHeight = height;
    _MESSAGE("Zero depth texture created: %ux%u", width, height);
    return true;
}

void DLSSManager::ReleaseEyeRender(EyeContext& eye) {
    if (eye.renderColorRTV) { eye.renderColorRTV->Release(); eye.renderColorRTV = nullptr; }
    if (eye.renderColor) { eye.renderColor->Release(); eye.renderColor = nullptr; }
}

bool DLSSManager::EnsureDownscaleShaders() {
    if (m_fsVS && m_fsPS && m_linearSampler) return true;
    const char* vsSrc = R"(
    struct VSOut { float4 pos:SV_Position; float2 uv:TEX; };
    VSOut main(uint id:SV_VertexID){
        float2 p = float2((id<<1)&2, id&2);
        VSOut o;
        o.pos = float4(p*float2(2,-2)+float2(-1,1),0,1);
        o.uv = p; // no vertical flip; D3D texcoords origin at top-left
        return o;
    })";
    const char* psSrc = R"(
    Texture2D srcTex:register(t0);
    SamplerState samLinear:register(s0);
    float4 main(float4 pos:SV_Position, float2 uv:TEX):SV_Target{
        return srcTex.Sample(samLinear, uv);
    })";
    ID3DBlob* vsBlob=nullptr; ID3DBlob* psBlob=nullptr; ID3DBlob* err=nullptr;
    HRESULT hr = D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr) || !vsBlob) { if (err) err->Release(); return false; }
    hr = D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &err);
    if (FAILED(hr) || !psBlob) { vsBlob->Release(); if (err) err->Release(); return false; }
    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_fsVS))) { vsBlob->Release(); psBlob->Release(); return false; }
    if (FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_fsPS))) { vsBlob->Release(); psBlob->Release(); return false; }
    vsBlob->Release(); psBlob->Release(); if (err) err->Release();
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateSamplerState(&sd, &m_linearSampler))) return false;
    return true;
}

bool DLSSManager::DownscaleToRender(EyeContext& eye, ID3D11Texture2D* inputTexture, uint32_t renderWidth, uint32_t renderHeight) {
    if (!EnsureDownscaleShaders()) return false;
    if (!inputTexture) return false;
    D3D11_TEXTURE2D_DESC inDesc{}; inputTexture->GetDesc(&inDesc);
    // Ensure render target
    if (!eye.renderColor || eye.renderWidth != renderWidth || eye.renderHeight != renderHeight) {
        ReleaseEyeRender(eye);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = renderWidth; td.Height = renderHeight; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = inDesc.Format;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &eye.renderColor))) return false;
        if (FAILED(m_device->CreateRenderTargetView(eye.renderColor, nullptr, &eye.renderColorRTV))) return false;
        eye.renderWidth = renderWidth; eye.renderHeight = renderHeight;
        eye.requiresReset = true;
    }

    // Create SRV for input (or copied input if not SRV-bindable)
    ID3D11ShaderResourceView* inSRV = nullptr;
    HRESULT hr = m_device->CreateShaderResourceView(inputTexture, nullptr, &inSRV);
    ID3D11Texture2D* tempCopy = nullptr;
    if (FAILED(hr) || !inSRV) {
        // Make a copy with SRV bind
        D3D11_TEXTURE2D_DESC cd = inDesc; cd.BindFlags |= D3D11_BIND_SHADER_RESOURCE; cd.Usage = D3D11_USAGE_DEFAULT; cd.MipLevels = 1; cd.ArraySize = 1;
        if (FAILED(m_device->CreateTexture2D(&cd, nullptr, &tempCopy))) return false;
        m_context->CopyResource(tempCopy, inputTexture);
        if (FAILED(m_device->CreateShaderResourceView(tempCopy, nullptr, &inSRV))) { tempCopy->Release(); return false; }
    }

    // Save state (minimal)
    ID3D11RenderTargetView* oldRTV = nullptr; ID3D11DepthStencilView* oldDSV = nullptr;
    m_context->OMGetRenderTargets(1, &oldRTV, &oldDSV);
    D3D11_VIEWPORT oldVP{}; UINT vpCount = 1; m_context->RSGetViewports(&vpCount, &oldVP);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo; m_context->IAGetPrimitiveTopology(&oldTopo);
    ID3D11VertexShader* oldVS = nullptr; ID3D11PixelShader* oldPS = nullptr;
    m_context->VSGetShader(&oldVS, nullptr, nullptr);
    m_context->PSGetShader(&oldPS, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSRV = nullptr; m_context->PSGetShaderResources(0, 1, &oldSRV);
    ID3D11SamplerState* oldSamp = nullptr; m_context->PSGetSamplers(0, 1, &oldSamp);

    // Set viewport and render target
    D3D11_VIEWPORT vp{}; vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)renderWidth; vp.Height = (float)renderHeight; vp.MinDepth = 0; vp.MaxDepth = 1;
    m_context->RSSetViewports(1, &vp);
    m_context->OMSetRenderTargets(1, &eye.renderColorRTV, nullptr);

    // Bind pipeline
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_fsVS, nullptr, 0);
    m_context->PSSetShader(m_fsPS, nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &inSRV);
    m_context->PSSetSamplers(0, 1, &m_linearSampler);
    m_context->Draw(3, 0);

    // Unbind
    ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    m_context->PSSetShaderResources(0, 1, nullSRV);

    // Restore state
    m_context->OMSetRenderTargets(1, &oldRTV, oldDSV);
    if (oldRTV) oldRTV->Release(); if (oldDSV) oldDSV->Release();
    m_context->RSSetViewports(vpCount, &oldVP);
    m_context->IASetPrimitiveTopology(oldTopo);
    m_context->VSSetShader(oldVS, nullptr, 0);
    m_context->PSSetShader(oldPS, nullptr, 0);
    if (oldVS) oldVS->Release(); if (oldPS) oldPS->Release();
    if (oldSRV) { ID3D11ShaderResourceView* r[1] = { oldSRV }; m_context->PSSetShaderResources(0, 1, r); oldSRV->Release(); }
    if (oldSamp) { ID3D11SamplerState* s[1] = { oldSamp }; m_context->PSSetSamplers(0, 1, s); oldSamp->Release(); }
    if (inSRV) inSRV->Release(); if (tempCopy) tempCopy->Release();
    return true;
}

ID3D11Texture2D* DLSSManager::ProcessEye(EyeContext& eye,
                                         ID3D11Texture2D* inputTexture,
                                         ID3D11Texture2D* depthTexture,
                                         ID3D11Texture2D* motionVectors,
                                         bool forceReset) {
    if (!inputTexture || !m_enabled) {
        return inputTexture;
    }

    if (!Initialize()) {
        return inputTexture;
    }

    D3D11_TEXTURE2D_DESC inputDesc = {};
    inputTexture->GetDesc(&inputDesc);

    // Determine per-eye display size from VR Submit (preferred), fallback to simple atlas split
    const bool isLeftEye = (&eye == &m_leftEye);
    uint32_t perEyeOutW = 0, perEyeOutH = 0;
    if (!DLSSHooks::GetPerEyeDisplaySize(isLeftEye ? 0 : 1, perEyeOutW, perEyeOutH)) {
        if (inputDesc.Width >= inputDesc.Height) { // side-by-side fallback
            perEyeOutW = inputDesc.Width / 2u;
            perEyeOutH = inputDesc.Height;
        } else { // top-bottom fallback
            perEyeOutW = inputDesc.Width;
            perEyeOutH = inputDesc.Height / 2u;
        }
    }
    // Align and clamp output
    perEyeOutW &= ~1u; perEyeOutH &= ~1u;
    if (perEyeOutW == 0) perEyeOutW = 2; if (perEyeOutH == 0) perEyeOutH = 2;
    if (perEyeOutW > 8192u) perEyeOutW = 8192u; if (perEyeOutH > 8192u) perEyeOutH = 8192u;

    // Derive render size from output via SL OptimalSettings (preferred) or uniform scale fallback
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
#if USE_STREAMLINE
    auto MapToSLMode = [&](Quality q)->sl::DLSSMode {
        switch (q) {
            case Quality::Performance: return sl::DLSSMode::eMaxPerformance;
            case Quality::Balanced:   return sl::DLSSMode::eBalanced;
            case Quality::Quality:    return sl::DLSSMode::eMaxQuality;
            case Quality::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
            case Quality::UltraQuality:     return sl::DLSSMode::eUltraQuality;
            case Quality::DLAA:             return sl::DLSSMode::eDLAA;
            default:                        return sl::DLSSMode::eMaxQuality;
        }
    };
    sl::DLSSOptions slOpts{};
    slOpts.mode = MapToSLMode(m_quality);
    slOpts.outputWidth = perEyeOutW;
    slOpts.outputHeight = perEyeOutH;
    _MESSAGE("[SL] OptimalSettings query: mode=%u out=%ux%u", (unsigned)slOpts.mode, perEyeOutW, perEyeOutH);
    sl::DLSSOptimalSettings slSet{};
    if (sl::Result::eOk == slDLSSGetOptimalSettings(slOpts, slSet)) {
        renderWidth = slSet.optimalRenderWidth;
        renderHeight = slSet.optimalRenderHeight;
        _MESSAGE("[SL] OptimalSettings result: render=%ux%u", renderWidth, renderHeight);
    }
#endif
    if (renderWidth == 0 || renderHeight == 0) {
        // Fallback: uniform scale from output using our quality table
        const float s = GetQualityInfo(m_quality).scale;
        renderWidth  = static_cast<uint32_t>(static_cast<float>(perEyeOutW) * s);
        renderHeight = static_cast<uint32_t>(static_cast<float>(perEyeOutH) * s);
    }
    // Even align and clamp
    renderWidth &= ~1u; renderHeight &= ~1u;
    if (renderWidth == 0) renderWidth = 2; if (renderHeight == 0) renderHeight = 2;
    if (renderWidth > perEyeOutW)  renderWidth = perEyeOutW;
    if (renderHeight > perEyeOutH) renderHeight = perEyeOutH;

    // Streamline path (no NGX params required)
    if (m_backend && m_backend->IsReady()) {
#if USE_STREAMLINE
        if (m_slBackend) {
            m_slBackend->SetCurrentEyeIndex(isLeftEye ? 0 : 1);
            if (isLeftEye) {
                m_slBackend->BeginFrame();
            }
        }
#endif
        _MESSAGE("[SL] ProcessEye: rw=%u rh=%u ow=%u oh=%u depth=%d mv=%d reset=%d",
                 renderWidth, renderHeight, perEyeOutW, perEyeOutH,
                 depthTexture?1:0, motionVectors?1:0, (eye.requiresReset||forceReset)?1:0);
        // If input already matches render size, skip downscale pass and use it directly
        bool useInputDirect = false;
        if (inputDesc.Width == renderWidth && inputDesc.Height == renderHeight) {
            useInputDirect = true;
            eye.renderWidth = renderWidth;
            eye.renderHeight = renderHeight;
            eye.requiresReset = true; // first time with direct-path
        } else {
            // Downscale input color to render size
            if (!DownscaleToRender(eye, inputTexture, renderWidth, renderHeight)) {
                return inputTexture;
            }
        }

        // Ensure output texture for the desired per-eye output dimensions
        bool needOutput = (eye.outputTexture == nullptr) ||
                          (eye.outputWidth != perEyeOutW) ||
                          (eye.outputHeight != perEyeOutH);
        if (needOutput) {
            ReleaseTexture(eye.outputTexture);
            if (!CreateOutputTexture(m_device, inputDesc, perEyeOutW, perEyeOutH, &eye.outputTexture)) {
                return inputTexture;
            }
            eye.outputWidth = perEyeOutW;
            eye.outputHeight = perEyeOutH;
            eye.renderWidth = renderWidth;
            eye.renderHeight = renderHeight;
            eye.requiresReset = true;
        }

        // Provide motion vectors (fallback to zero-MV if missing)
        ID3D11Texture2D* mv = motionVectors;
        if (!mv) {
            if (EnsureZeroMotionVectors(renderWidth, renderHeight) && m_zeroMotionVectors) {
                mv = m_zeroMotionVectors;
            }
        }

        // Validate depth dimensions (must match render size)
        ID3D11Texture2D* depthForDlss = depthTexture;
        if (depthForDlss) {
            D3D11_TEXTURE2D_DESC dd{}; depthForDlss->GetDesc(&dd);
            if (dd.Width != renderWidth || dd.Height != renderHeight || dd.SampleDesc.Count != 1) {
                depthForDlss = nullptr;
            }
        }
        if (!depthForDlss) {
            if (EnsureZeroDepthTexture(renderWidth, renderHeight) && m_zeroDepthTexture) {
                depthForDlss = m_zeroDepthTexture;
            }
        }

        ID3D11Texture2D* colorForBackend = useInputDirect ? inputTexture : eye.renderColor;
        ID3D11Texture2D* out = m_backend->ProcessEye(colorForBackend, depthForDlss, mv, eye.outputTexture,
                                                     renderWidth, renderHeight,
                                                     perEyeOutW, perEyeOutH,
                                                     (eye.requiresReset || forceReset));
        eye.requiresReset = false;
        // Treat success only when backend returns the designated output texture
        ID3D11Texture2D* result = (out == eye.outputTexture) ? out : inputTexture;
#if USE_STREAMLINE
        if (m_slBackend && !isLeftEye) {
            m_slBackend->EndFrame();
        }
#endif
        return result;
    }

    // NGX fallback path
    // Ensure feature and output texture exist for the target resolution.
    if (!EnsureEyeFeature(eye, inputTexture, renderWidth, renderHeight, inputDesc.Width, inputDesc.Height)) {
        return inputTexture;
    }

    m_ngxParameters->Reset();
    m_ngxParameters->Set(NVSDK_NGX_Parameter_Width, renderWidth);
    m_ngxParameters->Set(NVSDK_NGX_Parameter_Height, renderHeight);
    m_ngxParameters->Set(NVSDK_NGX_Parameter_OutWidth, inputDesc.Width);
    m_ngxParameters->Set(NVSDK_NGX_Parameter_OutHeight, inputDesc.Height);
    m_ngxParameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<unsigned int>(MapQuality(m_quality)));
    m_ngxParameters->Set(NVSDK_NGX_Parameter_Sharpness, m_sharpeningEnabled ? m_sharpness : 0.0f);
    m_ngxParameters->Set(NVSDK_NGX_Parameter_Reset, (eye.requiresReset || forceReset) ? 1 : 0);

    m_ngxParameters->Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(inputTexture));
    m_ngxParameters->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(eye.outputTexture));

    if (motionVectors) {
        m_ngxParameters->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(motionVectors));
    } else {
        if (EnsureZeroMotionVectors(renderWidth, renderHeight) && m_zeroMotionVectors) {
            m_ngxParameters->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(m_zeroMotionVectors));
        } else {
            m_ngxParameters->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(nullptr));
        }
    }

    if (depthTexture) {
        m_ngxParameters->Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depthTexture));
    } else {
        ID3D11Texture2D* zeroDepth = nullptr;
        if (EnsureZeroDepthTexture(renderWidth, renderHeight)) {
            zeroDepth = m_zeroDepthTexture;
        }
        m_ngxParameters->Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(zeroDepth));
    }

    _MESSAGE("[NGX] Evaluate: rw=%u rh=%u ow=%u oh=%u depth=%d mv=%d reset=%d",
             renderWidth, renderHeight, inputDesc.Width, inputDesc.Height, depthTexture?1:0, motionVectors?1:0, (eye.requiresReset||forceReset)?1:0);
    NVSDK_NGX_Result result = g_pfnNGXEvaluateFeature(m_context, eye.dlssHandle, m_ngxParameters, nullptr);
    if (!NVSDK_NGX_SUCCEED(result)) {
        _ERROR("NVSDK_NGX_D3D11_EvaluateFeature failed: 0x%08X", result);
        eye.requiresReset = true;
        return inputTexture;
    }

    eye.requiresReset = false;
    return eye.outputTexture ? eye.outputTexture : inputTexture;
}

ID3D11Texture2D* DLSSManager::ProcessLeftEye(ID3D11Texture2D* inputTexture,
    ID3D11Texture2D* depthTexture,
    ID3D11Texture2D* motionVectors) {
    return ProcessEye(m_leftEye, inputTexture, depthTexture, motionVectors, false);
}

ID3D11Texture2D* DLSSManager::ProcessRightEye(ID3D11Texture2D* inputTexture,
    ID3D11Texture2D* depthTexture,
    ID3D11Texture2D* motionVectors) {
    return ProcessEye(m_rightEye, inputTexture, depthTexture, motionVectors, false);
}

void DLSSManager::Shutdown() {
    if (m_backend) {
        m_backend->Shutdown();
        delete m_backend;
        m_backend = nullptr;
#if USE_STREAMLINE
        m_slBackend = nullptr;
#endif
    }
    if (m_leftEye.dlssHandle) {
        g_pfnNGXReleaseFeature(m_leftEye.dlssHandle);
        m_leftEye.dlssHandle = nullptr;
    }
    ReleaseTexture(m_leftEye.outputTexture);
    m_leftEye = {};

    if (m_rightEye.dlssHandle) {
        g_pfnNGXReleaseFeature(m_rightEye.dlssHandle);
        m_rightEye.dlssHandle = nullptr;
    }
    ReleaseTexture(m_rightEye.outputTexture);
    m_rightEye = {};

    ReleaseScratchBuffer();
    ReleaseZeroMotionVectors();
    ReleaseZeroDepthTexture();

    if (m_ngxParameters) {
        g_pfnNGXDestroyParameters(m_ngxParameters);
        m_ngxParameters = nullptr;
    }

    if (m_device && g_pfnNGXShutdown) {
        g_pfnNGXShutdown(m_device);
    }

    if (g_ngxModule) {
        FreeLibrary(g_ngxModule);
        g_ngxModule = nullptr;
    }

    if (m_context) {
        m_context->Release();
        m_context = nullptr;
    }

    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }

    m_initialized = false;
}
