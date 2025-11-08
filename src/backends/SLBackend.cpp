#include "SLBackend.h"



#include "SLBackend.h"
#include "common/IDebugLog.h"

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <dxgi.h>

namespace {
    // SL -> our log bridge
    static void SLLogCallback(sl::LogType type, const char* msg) {
        if (!msg) return;
        switch (type) {
            case sl::LogType::eInfo: _MESSAGE("[SL] %s", msg); break;
            case sl::LogType::eWarn: _MESSAGE("[SL][WARN] %s", msg); break;
            case sl::LogType::eError: _ERROR("[SL][ERROR] %s", msg); break;
            default: _MESSAGE("[SL] %s", msg); break;
        }
    }
    std::wstring GetDocumentsSLPath() {
        wchar_t docs[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYDOCUMENTS, NULL, 0, docs))) {
            std::wstring path = std::wstring(docs) + L"\\My Games\\Fallout4VR\\F4SE\\Plugins\\SL\\";
            SHCreateDirectoryExW(NULL, path.c_str(), NULL);
            return path;
        }
        return L".\\SL\\";
    }

    std::wstring GetGameDir() {
        wchar_t buf[MAX_PATH] = {};
        DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len == 0) return L"";
        std::wstring p(buf, buf + len);
        size_t pos = p.find_last_of(L"/\\");
        if (pos != std::wstring::npos) p.erase(pos + 1);
        return p;
    }

    DXGI_FORMAT ResolveDepthFormat(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
            default: return format;
        }
    }
}

SLBackend::SLBackend() = default;
SLBackend::~SLBackend() { Shutdown(); }

bool SLBackend::Init(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device = device;
    m_context = context;
    if (!m_device || !m_context) return false;

#ifndef USE_STREAMLINE
    return false;
#else
    // Prepare preferences
    static const sl::Feature kFeatures[] = { sl::kFeatureDLSS };
    std::wstring logs = GetDocumentsSLPath();
    std::wstring gameDir = GetGameDir();

    const wchar_t* paths[] = { gameDir.c_str() };

    sl::Preferences pref{};
    // Maximize Streamline logging for debugging runs
    pref.showConsole = true;
    pref.logLevel = sl::LogLevel::eVerbose;
    pref.logMessageCallback = SLLogCallback;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = logs.c_str();
    // Load only local plugins from provided paths; do not scan ProgramData OTA cache
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                 sl::PreferenceFlags::eUseManualHooking;
    pref.featuresToLoad = kFeatures;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "Custom";
    // Provide a valid GUID-style projectId so NGX does not infer an invalid ID from module name
    // This avoids "projectID [...] contains invalid character" during NGX initialization via SL
    pref.projectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
    // Keep numeric applicationId at 0 unless an NVIDIA-provided ID is available
    pref.applicationId = 0; // can be replaced with a real numeric app id if assigned
    pref.renderAPI = sl::RenderAPI::eD3D11;
    _MESSAGE("[SL] Init: plugins=%S, logs=%S, features=%u", paths[0], logs.c_str(), pref.numFeaturesToLoad);
    _MESSAGE("[SL] OTA disabled; local plugins only (no ProgramData)");
    sl::Result r = slInit(pref);
    if (r != sl::Result::eOk) {
        _ERROR("[SL] slInit failed: %d", (int)r);
        return false;
    }

    if (sl::Result rr = slSetD3DDevice((void*)m_device); rr != sl::Result::eOk) {
        _ERROR("[SL] slSetD3DDevice failed: %d", (int)rr);
        return false;
    }

    // Check DLSS support on current adapter
    bool loaded = false;
    do {
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) && dxgiDevice) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter) {
                DXGI_ADAPTER_DESC1 ad{};
                // Try Desc1 first
                IDXGIAdapter1* a1 = nullptr;
                if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter1), (void**)&a1)) && a1) {
                    DXGI_ADAPTER_DESC1 d1{}; a1->GetDesc1(&d1);
                    sl::AdapterInfo ai{};
                    ai.deviceLUID = reinterpret_cast<uint8_t*>(&d1.AdapterLuid);
                    ai.deviceLUIDSizeInBytes = sizeof(LUID);
                    sl::Result sup = slIsFeatureSupported(sl::kFeatureDLSS, ai);
                    _MESSAGE("[SL] slIsFeatureSupported(DLSS) result=%d", (int)sup);
                    a1->Release();
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }
    } while (0);

    if (sl::Result lr = slIsFeatureLoaded(sl::kFeatureDLSS, loaded); lr == sl::Result::eOk) {
        _MESSAGE("[SL] DLSS loaded=%d", (int)loaded);
    }
    if (sl::Result er = slSetFeatureLoaded(sl::kFeatureDLSS, true); er != sl::Result::eOk) {
        _ERROR("[SL] slSetFeatureLoaded(DLSS,true) failed: %d", (int)er);
        // continue; some builds auto-load
    }
    if (sl::Result lr2 = slIsFeatureLoaded(sl::kFeatureDLSS, loaded); lr2 == sl::Result::eOk) {
        _MESSAGE("[SL] DLSS loaded(after enable)=%d", (int)loaded);
    }

    // Query requirements and versions for logging
    sl::FeatureRequirements req{};
    if (sl::Result rq = slGetFeatureRequirements(sl::kFeatureDLSS, req); rq == sl::Result::eOk) {
        _MESSAGE("[SL] DLSS requirements: flags=0x%08X maxViewports=%u requiredTags=%u",
                 (unsigned)req.flags, req.maxNumViewports, req.numRequiredTags);
    }
    sl::FeatureVersion ver{};
    if (sl::Result vr = slGetFeatureVersion(sl::kFeatureDLSS, ver); vr == sl::Result::eOk) {
        _MESSAGE("[SL] DLSS versions: SL=%u.%u.%u NGX=%u.%u.%u",
                 ver.versionSL.major, ver.versionSL.minor, ver.versionSL.build,
                 ver.versionNGX.major, ver.versionNGX.minor, ver.versionNGX.build);
    }

    // Default options
    m_options = sl::DLSSOptions{};
    m_options.mode = sl::DLSSMode::eMaxQuality;
    m_options.outputWidth = 0;  // set per-frame
    m_options.outputHeight = 0; // set per-frame
    m_ready = true;
#ifdef USE_STREAMLINE
    for (int i = 0; i < kMaxEyes; ++i) {
        m_viewports[i] = sl::ViewportHandle(0);
        m_vpAllocated[i] = false;
        m_vpInW[i] = m_vpInH[i] = 0;
        m_vpOutW[i] = m_vpOutH[i] = 0;
    }
    m_currentEye = 0;
    m_frameToken = nullptr;
    m_frameActive = false;
    m_frameEyeCount = 0;
#endif
    return true;
#endif
}

void SLBackend::Shutdown() {
#ifdef USE_STREAMLINE
    if (m_ready) {
        for (int i = 0; i < kMaxEyes; ++i) {
            if (m_vpAllocated[i] && m_viewports[i] != 0) {
                slFreeResources(sl::kFeatureDLSS, m_viewports[i]);
                m_vpAllocated[i] = false;
            }
            m_viewports[i] = sl::ViewportHandle(0);
            m_vpInW[i] = m_vpInH[i] = m_vpOutW[i] = m_vpOutH[i] = 0;
        }
        m_frameToken = nullptr;
        m_frameActive = false;
        m_frameEyeCount = 0;
        slSetFeatureLoaded(sl::kFeatureDLSS, false);
        slShutdown();
    }
#endif
    m_ready = false;
    m_device = nullptr;
    m_context = nullptr;
}

void SLBackend::SetQuality(int qualityEnum) {
#ifdef USE_STREAMLINE
    m_quality = qualityEnum;
    switch (qualityEnum) {
        case 0: m_options.mode = sl::DLSSMode::eMaxPerformance; break; // Perf
        case 1: m_options.mode = sl::DLSSMode::eBalanced; break;       // Balanced
        case 2: m_options.mode = sl::DLSSMode::eMaxQuality; break;     // Quality
        case 3: m_options.mode = sl::DLSSMode::eUltraPerformance; break;
        case 4: m_options.mode = sl::DLSSMode::eUltraQuality; break;
        case 5: m_options.mode = sl::DLSSMode::eDLAA; break;
        default: m_options.mode = sl::DLSSMode::eMaxQuality; break;
    }
#endif
}

void SLBackend::SetSharpness(float value) {
#ifdef USE_STREAMLINE
    m_sharpness = value;
    m_options.sharpness = value; // deprecated in 2.9 but accepted
#endif
}

void SLBackend::BeginFrame() {
#ifdef USE_STREAMLINE
    if (m_frameActive) {
        return;
    }
    sl::FrameToken* token = nullptr;
    if (sl::Result fr = slGetNewFrameToken(token, nullptr); fr != sl::Result::eOk || !token) {
        _ERROR("[SL] slGetNewFrameToken failed: %d", (int)fr);
        m_frameToken = nullptr;
        m_frameActive = false;
        m_frameEyeCount = 0;
        return;
    }
    m_frameToken = token;
    m_frameActive = true;
    m_frameEyeCount = 0;
#endif
}

void SLBackend::EndFrame() {
#ifdef USE_STREAMLINE
    m_frameToken = nullptr;
    m_frameActive = false;
    m_frameEyeCount = 0;
#endif
}

void SLBackend::SetCurrentEyeIndex(int eyeIndex) {
#ifdef USE_STREAMLINE
    if (eyeIndex < 0) eyeIndex = 0;
    if (eyeIndex >= kMaxEyes) eyeIndex = kMaxEyes - 1;
    m_currentEye = eyeIndex;
#endif
}

ID3D11Texture2D* SLBackend::ProcessEye(ID3D11Texture2D* inputColor,
                                       ID3D11Texture2D* inputDepth,
                                       ID3D11Texture2D* inputMotionVectors,
                                       ID3D11Texture2D* outputTarget,
                                       unsigned int renderWidth,
                                       unsigned int renderHeight,
                                       unsigned int outputWidth,
                                       unsigned int outputHeight,
                                       bool resetHistory) {
#ifndef USE_STREAMLINE
    return inputColor;
#else
    if (!m_ready || !inputColor || outputWidth == 0 || outputHeight == 0) {
        return inputColor;
    }

    if (!m_frameActive || !m_frameToken) {
        BeginFrame();
        if (!m_frameActive || !m_frameToken) {
            return inputColor;
        }
    }

    int eyeIndex = m_currentEye;
    if (eyeIndex < 0 || eyeIndex >= kMaxEyes) {
        eyeIndex = 0;
    }

    sl::ViewportHandle& viewport = m_viewports[eyeIndex];
    bool& vpAllocated = m_vpAllocated[eyeIndex];
    unsigned int& vpInW = m_vpInW[eyeIndex];
    unsigned int& vpInH = m_vpInH[eyeIndex];
    unsigned int& vpOutW = m_vpOutW[eyeIndex];
    unsigned int& vpOutH = m_vpOutH[eyeIndex];

    if (!vpAllocated) {
        viewport = sl::ViewportHandle(eyeIndex + 1);
    }

    bool needRealloc = (!vpAllocated || vpInW != renderWidth || vpInH != renderHeight || vpOutW != outputWidth || vpOutH != outputHeight);
    if (needRealloc) {
        // Hard safety: DLSS has practical limits (~8K). If output exceeds, skip DLSS for this frame.
        if (outputWidth > 8192u || outputHeight > 8192u) {
            _ERROR("[SL] Output too large for DLSS (%ux%u) - reduce VR SS; falling back to native this frame", outputWidth, outputHeight);
            return inputColor;
        }
        m_options.outputWidth = outputWidth;
        m_options.outputHeight = outputHeight;
        (void)slDLSSSetOptions(viewport, m_options);
        if (vpAllocated) {
            slFreeResources(sl::kFeatureDLSS, viewport);
            vpAllocated = false;
        }
    }

    // Wrap resources
    D3D11_TEXTURE2D_DESC inDesc{};
    inputColor->GetDesc(&inDesc);

    sl::Resource color{};
    color.native = static_cast<void*>(static_cast<ID3D11Resource*>(inputColor));
    color.type = sl::ResourceType::eTex2d;
    color.width = inDesc.Width;
    color.height = inDesc.Height;
    color.nativeFormat = static_cast<uint32_t>(inDesc.Format);

    sl::Extent inExtent{ renderWidth, renderHeight };
    sl::Extent outExtent{ outputWidth, outputHeight };

    sl::ResourceTag tags[5] = {
        sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &inExtent)
    };
    uint32_t numTags = 1;

    sl::Resource depth{};
    if (inputDepth) {
        D3D11_TEXTURE2D_DESC d{}; inputDepth->GetDesc(&d);
        depth.native = static_cast<void*>(static_cast<ID3D11Resource*>(inputDepth));
        depth.type = sl::ResourceType::eTex2d;
        depth.width = d.Width; depth.height = d.Height;
        depth.nativeFormat = static_cast<uint32_t>(ResolveDepthFormat(d.Format));
        tags[numTags++] = sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &inExtent);
    }

    sl::Resource mv{};
    if (inputMotionVectors) {
        D3D11_TEXTURE2D_DESC m{}; inputMotionVectors->GetDesc(&m);
        mv.native = static_cast<void*>(static_cast<ID3D11Resource*>(inputMotionVectors));
        mv.type = sl::ResourceType::eTex2d;
        mv.width = m.Width; mv.height = m.Height; mv.nativeFormat = static_cast<uint32_t>(m.Format);
        tags[numTags++] = sl::ResourceTag(&mv, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &inExtent);
    }

    sl::Resource out{};
    if (outputTarget) {
        D3D11_TEXTURE2D_DESC o{}; outputTarget->GetDesc(&o);
        out.native = static_cast<void*>(static_cast<ID3D11Resource*>(outputTarget));
        out.type = sl::ResourceType::eTex2d;
        out.width = o.Width; out.height = o.Height; out.nativeFormat = static_cast<uint32_t>(o.Format);
        tags[numTags++] = sl::ResourceTag(&out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outExtent);
    }

    m_options.outputWidth = outputWidth;
    m_options.outputHeight = outputHeight;
    (void)slDLSSSetOptions(viewport, m_options);
    sl::Constants consts{};
    consts.mvecScale.x = (renderWidth > 0) ? (1.0f / (float)renderWidth) : 0.0f;
    consts.mvecScale.y = (renderHeight > 0) ? (1.0f / (float)renderHeight) : 0.0f;
    consts.jitterOffset.x = 0.0f; consts.jitterOffset.y = 0.0f;
    consts.reset = resetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    (void)slSetConstants(consts, *m_frameToken, viewport);

    // D3D11: pass immediate context as command buffer to SL
    sl::Result rt = slSetTagForFrame(*m_frameToken, viewport, tags, numTags, reinterpret_cast<sl::CommandBuffer*>(m_context));
    if (rt != sl::Result::eOk) {
        _ERROR("[SL] slSetTagForFrame failed: %d (tags=%u)", (int)rt, numTags);
    }

    // Allocate after options + tags are set, so the plugin has full context
    if (needRealloc) {
        sl::Result ar = slAllocateResources(reinterpret_cast<sl::CommandBuffer*>(m_context), sl::kFeatureDLSS, viewport);
        if (ar != sl::Result::eOk) {
            _ERROR("[SL] slAllocateResources failed: %d (in=%ux%u out=%ux%u)", (int)ar, renderWidth, renderHeight, outputWidth, outputHeight);
            // Do not early-return; DLSS can lazy-init on evaluate in some builds.
        } else {
            vpAllocated = true;
            vpInW = renderWidth; vpInH = renderHeight; vpOutW = outputWidth; vpOutH = outputHeight;
        }
    }

    _MESSAGE("[SL] ProcessEye: eye=%d", eyeIndex);
    _MESSAGE("[SL] Evaluate: in=%ux%u out=%ux%u depth=%d mv=%d", renderWidth, renderHeight, outputWidth, outputHeight, inputDepth?1:0, inputMotionVectors?1:0);
    const sl::BaseStructure* inputs[] = { reinterpret_cast<const sl::BaseStructure*>(&viewport) };
    sl::Result rEval = slEvaluateFeature(sl::kFeatureDLSS, *m_frameToken, inputs, 1, reinterpret_cast<sl::CommandBuffer*>(m_context));
    if (rEval != sl::Result::eOk) {
        _ERROR("[SL] slEvaluateFeature failed: %d", (int)rEval);
        
        // Error recovery mechanism
        static int errorCount = 0;
        errorCount++;
        
        // Log first occurrence with details
        if (errorCount == 1) {
            _ERROR("[SL] First error occurrence - eye:%d", eyeIndex);
        }
        
        // Attempt recovery after 10 failures
        if (errorCount == 10) {
            _MESSAGE("[SL] Attempting to reallocate viewport after repeated errors...");
            if (vpAllocated) {
                slFreeResources(sl::kFeatureDLSS, viewport);
                vpAllocated = false;
                vpInW = 0; vpInH = 0; vpOutW = 0; vpOutH = 0;
            }
        }
        
        // Disable DLSS after 100 failures to prevent spam
        if (errorCount >= 100) {
            _ERROR("[SL] Too many DLSS errors (%d), disabling DLSS", errorCount);
            m_ready = false;
            return inputColor;
        }
        
        return inputColor;
    }
    
    // Reset error count on success
    static int successCount = 0;
    if (++successCount > 10) {
        successCount = 0;
        // Clear any error state after consistent success
    }

    ++m_frameEyeCount;
    if (m_frameEyeCount >= kMaxEyes) {
        EndFrame();
    }

    // Return output if provided; otherwise return input
    return outputTarget ? outputTarget : inputColor;
#endif
}




