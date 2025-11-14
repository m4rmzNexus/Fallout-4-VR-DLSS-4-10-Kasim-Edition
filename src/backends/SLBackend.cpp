#include "SLBackend.h"



#include "SLBackend.h"
#include "common/IDebugLog.h"

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <dxgi.h>
#include <atomic>
#ifdef USE_STREAMLINE
#include <sl_security.h>
#endif

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

    bool ShouldLogSLFrame() {
        static std::atomic<uint32_t> counter{0};
        uint32_t value = counter.fetch_add(1, std::memory_order_relaxed) + 1u;
        return (value % 240u) == 1u;
    }

    std::wstring GetDocumentsSLPath() {
        wchar_t docs[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYDOCUMENTS, NULL, 0, docs))) {
            std::wstring base = std::wstring(docs) + L"\\My Games\\";
            std::wstring dirNoSpace = base + L"Fallout4VR\\F4SE\\Plugins\\SL\\";
            std::wstring dirWithSpace = base + L"Fallout 4 VR\\F4SE\\Plugins\\SL\\";
            // Prefer an existing folder if present; else create the no-space variant
            DWORD a = GetFileAttributesW(dirNoSpace.c_str());
            DWORD b = GetFileAttributesW(dirWithSpace.c_str());
            std::wstring chosen = (a != INVALID_FILE_ATTRIBUTES) ? dirNoSpace :
                                   (b != INVALID_FILE_ATTRIBUTES) ? dirWithSpace : dirNoSpace;
            SHCreateDirectoryExW(NULL, chosen.c_str(), NULL);
            return chosen;
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

#ifdef USE_STREAMLINE
    bool VerifyStreamlineRuntime(const std::wstring& baseDir) {
        if (baseDir.empty()) {
            _ERROR("[SL] Game directory path is empty; cannot verify Streamline binaries.");
            return false;
        }

        std::wstring interposer = baseDir + L"sl.interposer.dll";
        if (GetFileAttributesW(interposer.c_str()) == INVALID_FILE_ATTRIBUTES) {
            _ERROR("[SL] Expected Streamline binary missing: %S", interposer.c_str());
            return false;
        }

        if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
            _ERROR("[SL] Signature verification failed for %S", interposer.c_str());
            return false;
        }

        static bool loggedSuccess = false;
        if (!loggedSuccess) {
            _MESSAGE("[SL] Verified digital signature for %S", interposer.c_str());
            loggedSuccess = true;
        }
        return true;
    }
#endif

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
    if (!VerifyStreamlineRuntime(gameDir)) {
        return false;
    }

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
    static bool s_loggedInitPaths = false;
    if (!s_loggedInitPaths) {
        _MESSAGE("[SL] Init: plugins=%S, logs=%S, features=%u", paths[0], logs.c_str(), pref.numFeaturesToLoad);
        _MESSAGE("[SL] OTA disabled; local plugins only (no ProgramData)");
        s_loggedInitPaths = true;
    }
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
            if (m_scratchIn[i]) { m_scratchIn[i]->Release(); m_scratchIn[i] = nullptr; }
            if (m_scratchOut[i]) { m_scratchOut[i]->Release(); m_scratchOut[i] = nullptr; }
            m_scratchInW[i] = m_scratchInH[i] = 0; m_scratchInFmt[i] = DXGI_FORMAT_UNKNOWN;
            m_scratchOutW[i] = m_scratchOutH[i] = 0; m_scratchOutFmt[i] = DXGI_FORMAT_UNKNOWN;
        }
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
    _MESSAGE("[SL] Backend quality set: %d -> DLSSMode=%u", qualityEnum, (unsigned)m_options.mode);
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

void SLBackend::AbortFrame() {
#ifdef USE_STREAMLINE
    if (!m_frameActive && !m_frameToken && m_frameEyeCount == 0) {
        return;
    }
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
        AbortFrame();
        return inputColor;
    }

    if (!m_frameActive || !m_frameToken) {
        BeginFrame();
        if (!m_frameActive || !m_frameToken) {
            AbortFrame();
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

    const unsigned int prevInW = vpInW;
    const unsigned int prevInH = vpInH;
    const unsigned int prevOutW = vpOutW;
    const unsigned int prevOutH = vpOutH;
    bool needRealloc = (!vpAllocated || vpInW != renderWidth || vpInH != renderHeight || vpOutW != outputWidth || vpOutH != outputHeight);
    if (needRealloc) {
        // Hard safety: DLSS has practical limits (~8K). If output exceeds, skip DLSS for this frame.
        if (outputWidth > 8192u || outputHeight > 8192u) {
            _ERROR("[SL] Output too large for DLSS (%ux%u) - reduce VR SS; falling back to native this frame", outputWidth, outputHeight);
            AbortFrame();
            return inputColor;
        }
        // Free any previous allocations for this viewport; Streamline will re-allocate lazily on evaluate
        slFreeResources(sl::kFeatureDLSS, viewport);
        vpAllocated = false;
        if (ShouldLogSLFrame()) {
            _MESSAGE("[SL] Viewport realloc eye=%d in %ux%u->%ux%u out %ux%u->%ux%u",
                     eyeIndex, prevInW, prevInH, renderWidth, renderHeight,
                     prevOutW, prevOutH, outputWidth, outputHeight);
        }
    }

    // Wrap resources
    D3D11_TEXTURE2D_DESC inDesc{};
    inputColor->GetDesc(&inDesc);

    // Decide on scratch fallback for input
    const bool needScratchIn = (inDesc.Usage != D3D11_USAGE_DEFAULT) || (inDesc.SampleDesc.Count > 1) || ((inDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0);

    ID3D11Texture2D* tagInput = inputColor;
    if (needScratchIn) {
        // Create or resize scratch input (SRV-capable)
        bool create = (m_scratchIn[eyeIndex] == nullptr) || m_scratchInW[eyeIndex] != renderWidth || m_scratchInH[eyeIndex] != renderHeight || m_scratchInFmt[eyeIndex] != inDesc.Format;
        if (create) {
            if (m_scratchIn[eyeIndex]) { m_scratchIn[eyeIndex]->Release(); m_scratchIn[eyeIndex] = nullptr; }
            D3D11_TEXTURE2D_DESC s{};
            s.Width = renderWidth; s.Height = renderHeight; s.MipLevels = 1; s.ArraySize = 1;
            s.Format = inDesc.Format; s.SampleDesc.Count = 1; s.SampleDesc.Quality = 0;
            s.Usage = D3D11_USAGE_DEFAULT; s.BindFlags = D3D11_BIND_SHADER_RESOURCE; s.CPUAccessFlags = 0; s.MiscFlags = 0;
            HRESULT hr = m_device->CreateTexture2D(&s, nullptr, &m_scratchIn[eyeIndex]);
            if (FAILED(hr)) {
                _ERROR("[SL] Failed to create scratch input texture (hr=0x%08X)", (unsigned)hr);
            } else {
                m_scratchInW[eyeIndex] = renderWidth; m_scratchInH[eyeIndex] = renderHeight; m_scratchInFmt[eyeIndex] = inDesc.Format;
            }
        }
        if (m_scratchIn[eyeIndex]) {
            // Copy region from source to scratch
            D3D11_BOX srcBox{}; srcBox.left = 0; srcBox.top = 0; srcBox.front = 0; srcBox.right = renderWidth; srcBox.bottom = renderHeight; srcBox.back = 1;
            m_context->CopySubresourceRegion(m_scratchIn[eyeIndex], 0, 0, 0, 0, inputColor, 0, &srcBox);
            tagInput = m_scratchIn[eyeIndex];
        } else {
            tagInput = inputColor; // fallback to original
        }
    }

    sl::Resource color{};
    color.native = static_cast<void*>(static_cast<ID3D11Resource*>(tagInput));
    color.type = sl::ResourceType::eTex2d;
    color.width = needScratchIn ? renderWidth : inDesc.Width;
    color.height = needScratchIn ? renderHeight : inDesc.Height;
    color.nativeFormat = static_cast<uint32_t>(inDesc.Format);

    // Build extents with optional SxS/TB crop for stereo atlases
    auto clampTo = [](uint32_t wantW, uint32_t wantH, uint32_t resW, uint32_t resH) -> sl::Extent {
        sl::Extent e{};
        e.left = 0; e.top = 0;
        e.width = (wantW > resW) ? resW : wantW;
        e.height = (wantH > resH) ? resH : wantH;
        if (e.width == 0) e.width = resW; // fall back to full size if caller passed 0
        if (e.height == 0) e.height = resH;
        return e;
    };

    sl::Extent inExtent = clampTo(renderWidth, renderHeight, color.width, color.height);
    const bool colorMatchesRender = (color.width == renderWidth && color.height == renderHeight);
    if (!colorMatchesRender) {
        // Heuristic crop only when fallback textures still use atlas
        const bool looksSxS = (color.width >= (uint32_t)((float)color.height * 1.7f));
        const bool looksTB  = (!looksSxS) && (color.height >= (uint32_t)((float)color.width * 1.7f));
        if (looksSxS) {
            const uint32_t halfW = color.width / 2u;
            if (inExtent.width > halfW) inExtent.width = halfW;
            inExtent.left = (eyeIndex == 0) ? 0 : halfW;
            inExtent.top = 0;
        } else if (looksTB) {
            const uint32_t halfH = color.height / 2u;
            if (inExtent.height > halfH) inExtent.height = halfH;
            inExtent.left = 0;
            inExtent.top = (eyeIndex == 0) ? 0 : halfH;
        }
    }

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
        sl::Extent dpExtent = clampTo(renderWidth, renderHeight, d.Width, d.Height);
        dpExtent.left = inExtent.left;
        dpExtent.top = inExtent.top;
        tags[numTags++] = sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &dpExtent);
    }

    sl::Resource mv{};
    if (inputMotionVectors) {
        D3D11_TEXTURE2D_DESC m{}; inputMotionVectors->GetDesc(&m);
        mv.native = static_cast<void*>(static_cast<ID3D11Resource*>(inputMotionVectors));
        mv.type = sl::ResourceType::eTex2d;
        mv.width = m.Width; mv.height = m.Height; mv.nativeFormat = static_cast<uint32_t>(m.Format);
        sl::Extent mvExtent = clampTo(renderWidth, renderHeight, m.Width, m.Height);
        mvExtent.left = inExtent.left;
        mvExtent.top = inExtent.top;
        tags[numTags++] = sl::ResourceTag(&mv, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &mvExtent);
    }

    if (ShouldLogSLFrame()) {
        _MESSAGE("[SL] Tag eye=%d in(%u,%u %ux%u) depth=%d mv=%d out=%ux%u",
                 eyeIndex,
                 inExtent.left, inExtent.top, inExtent.width, inExtent.height,
                 inputDepth ? 1 : 0,
                 inputMotionVectors ? 1 : 0,
                 outputWidth, outputHeight);
    }

    // Output: prefer provided target if UAV-capable; else use scratch output
    sl::Resource out{};
    ID3D11Texture2D* tagOutput = outputTarget;
    D3D11_TEXTURE2D_DESC o{};
    bool usingScratchOut = false;
    if (outputTarget) { outputTarget->GetDesc(&o); }
    const bool needScratchOut = (!outputTarget) || (o.Usage != D3D11_USAGE_DEFAULT) || (o.SampleDesc.Count > 1) || ((o.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0);
    if (needScratchOut) {
        // Prefer BGRA8 UNORM for VR compositor compatibility
        DXGI_FORMAT outFmt = outputTarget ? o.Format : DXGI_FORMAT_B8G8R8A8_UNORM;
        bool create = (m_scratchOut[eyeIndex] == nullptr) || m_scratchOutW[eyeIndex] != outputWidth || m_scratchOutH[eyeIndex] != outputHeight || m_scratchOutFmt[eyeIndex] != outFmt;
        if (create) {
            if (m_scratchOut[eyeIndex]) { m_scratchOut[eyeIndex]->Release(); m_scratchOut[eyeIndex] = nullptr; }
            D3D11_TEXTURE2D_DESC s{};
            s.Width = outputWidth; s.Height = outputHeight; s.MipLevels = 1; s.ArraySize = 1;
            s.Format = outFmt; s.SampleDesc.Count = 1; s.SampleDesc.Quality = 0;
            s.Usage = D3D11_USAGE_DEFAULT; s.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; s.CPUAccessFlags = 0; s.MiscFlags = 0;
            HRESULT hr = m_device->CreateTexture2D(&s, nullptr, &m_scratchOut[eyeIndex]);
            if (FAILED(hr)) {
                _ERROR("[SL] Failed to create scratch output texture (hr=0x%08X)", (unsigned)hr);
            } else {
                m_scratchOutW[eyeIndex] = outputWidth; m_scratchOutH[eyeIndex] = outputHeight; m_scratchOutFmt[eyeIndex] = outFmt;
            }
        }
        if (m_scratchOut[eyeIndex]) {
            tagOutput = m_scratchOut[eyeIndex];
            usingScratchOut = true;
            // refresh desc 'o' to point to scratch dims
            o.Width = outputWidth; o.Height = outputHeight; o.Format = m_scratchOutFmt[eyeIndex];
        }
    }
    if (tagOutput) {
        out.native = static_cast<void*>(static_cast<ID3D11Resource*>(tagOutput));
        out.type = sl::ResourceType::eTex2d;
        out.width = o.Width; out.height = o.Height; out.nativeFormat = static_cast<uint32_t>(o.Format);
        const sl::Extent clampedOutExtent = clampTo(outputWidth, outputHeight, out.width, out.height);
        tags[numTags++] = sl::ResourceTag(&out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &clampedOutExtent);
    }

    m_options.outputWidth = outputWidth;
    m_options.outputHeight = outputHeight;
    sl::Constants consts{};
    // Motion vectors normalization: convert pixel-space MVs into [-1,1] (per SL guidance)
    consts.mvecScale.x = (renderWidth > 0) ? (1.0f / (float)renderWidth) : 1.0f;
    consts.mvecScale.y = (renderHeight > 0) ? (1.0f / (float)renderHeight) : 1.0f;
    // VR: jitter disabled for stereo comfort
    consts.jitterOffset.x = 0.0f; consts.jitterOffset.y = 0.0f;
    // Provide sane defaults for validation (will be refined when camera data is available)
    // Matrices: set to identity (row-major) to avoid 'invalid' warnings until real values are wired
    auto setIdentity = [](sl::float4x4& m) {
        m.setRow(0, sl::float4(1.f, 0.f, 0.f, 0.f));
        m.setRow(1, sl::float4(0.f, 1.f, 0.f, 0.f));
        m.setRow(2, sl::float4(0.f, 0.f, 1.f, 0.f));
        m.setRow(3, sl::float4(0.f, 0.f, 0.f, 1.f));
    };
    setIdentity(consts.cameraViewToClip);
    setIdentity(consts.clipToCameraView);
    setIdentity(consts.clipToPrevClip);
    setIdentity(consts.prevClipToClip);
    consts.cameraNear = 0.1f;
    consts.cameraFar  = 10000.0f;
    // Rough aspect from output (fallback)
    const float outAspect = (outputHeight > 0) ? (float)outputWidth / (float)outputHeight : 1.0f;
    consts.cameraAspectRatio = outAspect;
    // Provide a plausible FOV placeholder (radians)
    consts.cameraFOV = 1.0f;
    // Camera basis defaults
    consts.cameraPos = sl::float3(0.f, 0.f, 0.f);
    consts.cameraUp = sl::float3(0.f, 1.f, 0.f);
    consts.cameraRight = sl::float3(1.f, 0.f, 0.f);
    consts.cameraFwd = sl::float3(0.f, 0.f, 1.f);
    consts.cameraPinholeOffset = sl::float2(0.f, 0.f);
    // Assume 2D pixel-space motion vectors by default
    // Zero-MV defaults and flags
    consts.motionVectorsInvalidValue = 0.0f;
    consts.depthInverted = sl::Boolean::eFalse;           // TODO: set true if engine uses inverted depth
    consts.cameraMotionIncluded = sl::Boolean::eFalse;     // we do not include camera motion in MV
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.motionVectorsJittered = sl::Boolean::eFalse;    // VR jitter disabled
    consts.reset = resetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    // Per-eye tagging order:
    // 1) Tags -> 2) Constants -> 3) Options -> 4) Evaluate
    // D3D11: pass immediate context as command buffer to SL
    sl::Result rt = slSetTagForFrame(*m_frameToken, viewport, tags, numTags, reinterpret_cast<sl::CommandBuffer*>(m_context));
    if (rt != sl::Result::eOk) {
        _ERROR("[SL] slSetTagForFrame failed: %d (tags=%u)", (int)rt, numTags);
        AbortFrame();
        return inputColor;
    }

    (void)slSetConstants(consts, *m_frameToken, viewport);
    (void)slDLSSSetOptions(viewport, m_options);
    _MESSAGE("[SL] Options: mode=%u sharp=%.2f", (unsigned)m_options.mode, m_options.sharpness);

    _MESSAGE("[SL] ProcessEye: eye=%d", eyeIndex);
    _MESSAGE("[SL] Evaluate: in=%ux%u(outTex=%ux%u) out=%ux%u(outTex=%ux%u) depth=%d mv=%d",
             renderWidth, renderHeight, inDesc.Width, inDesc.Height,
             outputWidth, outputHeight, out.width, out.height,
             inputDepth?1:0, inputMotionVectors?1:0);
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
            AbortFrame();
            return inputColor;
        }

        AbortFrame();
        return inputColor;
    }
    // Mark viewport as allocated and cache sizes after a successful evaluate
    if (needRealloc) {
        vpAllocated = true;
        vpInW = renderWidth; vpInH = renderHeight; vpOutW = outputWidth; vpOutH = outputHeight;
    }
    
    // Reset error count on success
    static int successCount = 0;
    if (++successCount > 10) {
        successCount = 0;
        // Clear any error state after consistent success
    }

    // If we used scratch output and a real output target exists with same format/size, copy result back
    if (usingScratchOut && outputTarget && m_scratchOut[eyeIndex]) {
        // Only copy if formats and dimensions match exactly
        if (o.Format == m_scratchOutFmt[eyeIndex] && o.Width == m_scratchOutW[eyeIndex] && o.Height == m_scratchOutH[eyeIndex]) {
            m_context->CopyResource(outputTarget, m_scratchOut[eyeIndex]);
        }
    }

    ++m_frameEyeCount;
    if (m_frameEyeCount >= kMaxEyes) {
        EndFrame();
    }

    // Return chosen output if available; otherwise return input
    if (tagOutput) return tagOutput;
    return tagInput ? tagInput : inputColor;
#endif
}




