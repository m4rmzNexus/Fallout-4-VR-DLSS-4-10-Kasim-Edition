# DLSS VR Performans - Uygulama Patch Seti

> **Hedef**: Bu doküman, DLSS'i saf kalite yerine **performans için** kullanmak isteyen bir FO4VR modder'ı için pratik uygulama adımlarını içerir.

## Patch Uygulama Sırası

1. **Handle Swap** - Copy overhead'i kaldır (Kolay, hemen test edilebilir)
2. **Debug/Metrics** - Ne olup bittiğini anlamak için log altyapısı
3. **GetRecommendedRenderTargetSize Hook** - Gerçek DLSS performansı (Orta zorluk)
4. **Early DLSS Stabilizasyon** - RT redirect POC (İleri seviye)

---

## Patch 1: Handle Swap Implementation

### Amaç
Copy overhead'i kaldırıp DLSS texture'ı direkt compositor'a göndermek.

### Dosya: `dlss_hooks.cpp`
### Fonksiyon: `HookedVRCompositorSubmit`

**Mevcut Kod (Satır 888-949)**:
```cpp
// ❌ Eski: Copy yaklaşımı
if (up && up != colorTexture) {
    // Format/MSAA kontrolü
    if (srcDesc.Format == dstDesc.Format &&
        srcDesc.SampleDesc.Count == dstDesc.SampleDesc.Count &&
        srcDesc.SampleDesc.Count == 1) {

        // Destination region hesaplama
        D3D11_TEXTURE2D_DESC dstDesc{};
        colorTexture->GetDesc(&dstDesc);
        UINT dstX = 0, dstY = 0;
        UINT dstW = outW ? outW : dstDesc.Width;
        UINT dstH = outH ? outH : dstDesc.Height;

        // [... bounds hesaplama ...]

        // Copy işlemi
        DLSSHooks::UnbindResourcesForSubmit();
        g_context->CopySubresourceRegion(colorTexture, 0, dstX, dstY, 0, up, 0, &src);
    }
}
return g_realVRSubmit(self, eye, texture, bounds, flags);
```

**✅ Yeni: Handle Swap**:
```cpp
// Handle swap yaklaşımı
if (up && up != colorTexture) {
    // DLSS başarılı - texture handle'ı değiştir

    // Log for debugging
    DLSS_LOG("[Submit] Handle swap: eye=%d original=%p dlss=%p",
             (int)eye, colorTexture, up);

    // Modified texture descriptor
    vr::Texture_t modifiedTexture = *texture;
    modifiedTexture.handle = up;  // DLSS output'u direkt kullan

    // Optional: Adjust bounds if DLSS output size differs
    vr::VRTextureBounds_t modifiedBounds = bounds ? *bounds : vr::VRTextureBounds_t{0,0,1,1};

    // Submit DLSS texture directly
    return g_realVRSubmit(self, eye, &modifiedTexture, &modifiedBounds, flags);
}

// DLSS başarısız veya disabled - orijinali kullan
return g_realVRSubmit(self, eye, texture, bounds, flags);
```

**Test Notları**:
- OpenVR'ın handle swap'i kabul ettiğinden emin ol
- Bounds adjustment gerekebilir (DLSS output boyutu farklıysa)
- Performance counter ekle (copy time saved)

---

## Patch 2: Debug/Metrics Infrastructure

### Amaç
Frame-by-frame ne olup bittiğini anlamak için detaylı loglama.

### Dosya: `dlss_hooks.cpp` (üst kısım)

**Yeni Makrolar**:
```cpp
// Frame counter ve metrics
static std::atomic<uint64_t> g_frameCounter{0};
static std::atomic<uint64_t> g_dlssSuccessCount{0};
static std::atomic<uint64_t> g_dlssFailCount{0};
static std::atomic<uint64_t> g_copySkipCount{0};

// Enhanced logging with frame ID
#define DLSS_LOG(fmt, ...) \
    do { \
        if (g_dlssConfig && g_dlssConfig->verboseLogging) { \
            _MESSAGE("[F%llu][DLSS] " fmt, g_frameCounter.load(), __VA_ARGS__); \
        } \
    } while(0)

#define DLSS_METRICS() \
    do { \
        static uint64_t lastReport = 0; \
        uint64_t frame = g_frameCounter.load(); \
        if (frame - lastReport >= 300) { /* Every 5 seconds at 60fps */ \
            uint64_t success = g_dlssSuccessCount.exchange(0); \
            uint64_t fail = g_dlssFailCount.exchange(0); \
            uint64_t skip = g_copySkipCount.exchange(0); \
            _MESSAGE("[METRICS] Last 5s: Success=%llu Fail=%llu Skip=%llu", \
                     success, fail, skip); \
            lastReport = frame; \
        } \
    } while(0)
```

### HookedVRCompositorSubmit'e Ekle:
```cpp
// Fonksiyon başı
g_frameCounter.fetch_add(1);
DLSS_METRICS();

// DLSS processing sonrası
if (up && up != colorTexture) {
    g_dlssSuccessCount.fetch_add(1);
    DLSS_LOG("Success: eye=%s in=%ux%u out=%ux%u",
             eye==vr::Eye_Left?"L":"R", renderW, renderH, outW, outH);
} else if (up == colorTexture) {
    g_dlssFailCount.fetch_add(1);
    DLSS_LOG("Fallback: ProcessEye returned original");
} else {
    g_dlssFailCount.fetch_add(1);
    DLSS_LOG("Failed: ProcessEye returned null");
}
```

---

## Patch 3: GetRecommendedRenderTargetSize Hook

### Amaç
Oyunu baştan küçük render ettirip gerçek DLSS performansı almak.

### Dosya: `dlss_hooks.cpp`

**Yeni Global Değişkenler**:
```cpp
// OpenVR System interface hooks
static void* g_vrSystemInterface = nullptr;
typedef void (__stdcall *PFN_GetRecommendedRenderTargetSize)(uint32_t* w, uint32_t* h);
static PFN_GetRecommendedRenderTargetSize g_realGetRecommendedSize = nullptr;
```

**Hook Fonksiyonu**:
```cpp
void __stdcall HookedGetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) {
    // Önce orijinal HMD recommended size'ı al
    if (g_realGetRecommendedSize) {
        g_realGetRecommendedSize(pnWidth, pnHeight);
    }

    if (!pnWidth || !pnHeight || !g_dlssManager) {
        return;
    }

    uint32_t originalW = *pnWidth;
    uint32_t originalH = *pnHeight;

    // DLSS aktifse render size'a düşür
    if (g_dlssManager->IsEnabled()) {
        uint32_t renderW = 0, renderH = 0;

        if (g_dlssManager->ComputeRenderSizeForOutput(originalW, originalH, renderW, renderH)) {
            *pnWidth = renderW;
            *pnHeight = renderH;

            DLSS_LOG("GetRecommendedSize override: %ux%u -> %ux%u (%.1f%%)",
                     originalW, originalH, renderW, renderH,
                     100.0f * (renderW * renderH) / (float)(originalW * originalH));
        }
    }
}
```

**Hook Kurulumu** (InstallVRHooks içine ekle):
```cpp
bool InstallRecommendedSizeHook() {
    // OpenVR System interface'i bul
    HMODULE openVRModule = GetModuleHandleW(L"openvr_api.dll");
    if (!openVRModule) return false;

    auto getInterface = (PFN_VR_GetGenericInterface)GetProcAddress(openVRModule, "VR_GetGenericInterface");
    if (!getInterface) return false;

    vr::EVRInitError error;
    g_vrSystemInterface = getInterface("IVRSystem_022", &error);
    if (!g_vrSystemInterface || error != vr::VRInitError_None) return false;

    // VTable hook
    void** vtable = *reinterpret_cast<void***>(g_vrSystemInterface);

    // GetRecommendedRenderTargetSize genelde index 44
    g_realGetRecommendedSize = reinterpret_cast<PFN_GetRecommendedRenderTargetSize>(vtable[44]);
    vtable[44] = reinterpret_cast<void*>(&HookedGetRecommendedRenderTargetSize);

    _MESSAGE("[DLSS] GetRecommendedRenderTargetSize hook installed");
    return true;
}
```

**⚠️ Önemli Notlar**:
- UI/HUD scaling'i kontrol et (küçük render'da UI bozulabilir)
- Post-processing effect'leri test et
- Engine'in farklı yerlerde cache'lediği size değerlerini bul

---

## Patch 4: Early DLSS RT Redirect POC (İleri Seviye)

### Amaç
Tek bir render target için RT redirect'i test etmek.

### Değişiklik: `dlss_hooks.cpp` - HookedOMSetRenderTargets

**Basitleştirilmiş POC**:
```cpp
void __stdcall HookedOMSetRenderTargets_POC(
    ID3D11DeviceContext* context,
    UINT numViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView) {

    // POC: Sadece ilk RT'yi redirect et
    static ID3D11RenderTargetView* smallRTV = nullptr;
    static ID3D11Texture2D* smallTex = nullptr;
    static bool pocActive = false;

    if (g_dlssConfig && g_dlssConfig->earlyDlssPOC && numViews > 0 && ppRenderTargetViews[0]) {
        ID3D11Texture2D* origRT = ExtractTextureFromRTV(ppRenderTargetViews[0]);
        if (origRT && IsLikelyVRSceneRT(origRT)) {

            // Küçük RT oluştur (bir kerelik)
            if (!smallTex) {
                D3D11_TEXTURE2D_DESC desc{};
                origRT->GetDesc(&desc);

                // DLSS render size
                uint32_t renderW, renderH;
                if (g_dlssManager->ComputeRenderSizeForOutput(desc.Width, desc.Height, renderW, renderH)) {
                    desc.Width = renderW;
                    desc.Height = renderH;

                    if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &smallTex))) {
                        g_device->CreateRenderTargetView(smallTex, nullptr, &smallRTV);
                        pocActive = true;
                        DLSS_LOG("POC: Created small RT %ux%u", renderW, renderH);
                    }
                }
            }

            // Redirect to small RT
            if (pocActive && smallRTV) {
                ID3D11RenderTargetView* redirected[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
                redirected[0] = smallRTV;
                for (UINT i = 1; i < numViews; i++) {
                    redirected[i] = ppRenderTargetViews[i]; // Diğerleri aynı
                }

                g_pfnOMSetRenderTargets(context, numViews, redirected, pDepthStencilView);
                g_redirectActive = true;
                return;
            }
        }
    }

    // Normal akış
    g_pfnOMSetRenderTargets(context, numViews, ppRenderTargetViews, pDepthStencilView);
}
```

---

## TODO Checklist (Öncelik Sırasıyla)

- [ ] **Handle swap patch'i uygula ve test et**
  - [ ] OpenVR compat kontrolü
  - [ ] Performance counter ekle

- [ ] **Debug/metrics altyapısını kur**
  - [ ] Frame ID'li log sistemi
  - [ ] 5 saniyelik metrics raporu

- [ ] **GetRecommendedRenderTargetSize hook'u implemente et**
  - [ ] VTable index'i doğrula (44 veya farklı olabilir)
  - [ ] UI/HUD scaling sorunlarını test et

- [ ] **Early DLSS POC (tek RT için)**
  - [ ] Basit redirect mekanizması
  - [ ] Composite-back logic
  - [ ] Stability testleri

---

## Performance Test Metodolojisi

### Baseline (DLSS Kapalı)
```
1. fpsVR veya SteamVR frame timing aç
2. Sabit bir sahne seç (örn: Sanctuary)
3. 60 saniye boyunca ortalama frame time kaydet
```

### Handle Swap Testi
```
1. Patch uygula, rebuild
2. Aynı sahnede test
3. Frame time + GPU util karşılaştır
4. Copy overhead'in gittiğini doğrula
```

### GetRecommendedSize Testi
```
1. Hook'u aktifleştir
2. Render resolution düştüğünü loglardan doğrula
3. Frame time'da %30-40 iyileşme bekle
4. Visual quality vs performance trade-off'u değerlendir
```

---

*Bu patch seti, DLSS'i performans odaklı kullanmak için pratik adımları içerir.*
*Her patch bağımsız test edilebilir.*

---

## Son Durum Analizi (Kasım 2025)

Bu patch planını uygulamadan önce mevcut kod tabanının durumu dikkate alınmalıdır:

1. **Early DLSS tamamen devre dışı**  
   - `dlss_hooks.cpp` içinde `constexpr bool kEarlyDlssFeatureEnabled = false` tanımı tüm hook’ları kapatıyor.  
   - `dlss_config.cpp` içinde `ForceDisableEarlyDlss()` hem `Load()` hem `Save()` sırasında çağrılıyor, bu da kullanıcı INI/ImGui ayarlarını geri alıyor.  
   - Bu guard’lar kaldırılmadıkça Patch 3 ve Patch 4’teki render-size/RT redirect adımlarının hiçbirini test etmek mümkün değil.

2. **Handle swap öncesi dikkat edilmesi gerekenler**  
   - DLSS çıktısı (`up`) color space, bounds ve multisample ayarları orijinal eye RT ile uyuşmazsa SteamVR kareyi reddedebilir.  
   - Şu anki pipeline’da `ProcessLeftEye/RightEye` kaynakların ömrünü yönetiyor; handle swap yaparken referans süresini açıkça tanımlamak lazım.  
   - Dolayısıyla Patch 1 uygulanmadan önce DLSS evaluate gerçekten çalışıyor mu ve `up` texture’ı compositor teslimine uygun mu netleştirilmeli.

3. **GetRecommendedRenderTargetSize hook’u yüksek riskli**  
   - Engine bu değeri cache’leyip başka yerlerde kullanıyor olabilir; yanlış vtable index seçimi veya eksik hud scaling düzeltmeleri geri döndürülemez render hatalarına yol açabilir.  
   - Bu hook’u yalnızca Early DLSS tekrar aktif edilip loglarla doğrulandıktan sonra denemek mantıklı.

4. **RT redirect POC’nin daha gelişmiş versiyonu zaten mevcut**  
   - Repo içindeki `dlss_hooks.cpp` hâlihazırda redirect cache’i ve composite mekanizmasını barındırıyor. Statik `smallRTV` ile tek RT’ye indirgenmiş bir POC, mevcut sistemi geriye götürür ve kaynak sızıntısı yaratabilir.

Öneri: Önce compile-time/runtime kilitlerini kaldırın, ardından gerçek DLSS akışının loglarla çalıştığını doğrulayın. Bu sağlanmadan handle swap, VRSystem hook’u veya RT redirect gibi ileri adımlar kararsız sonuçlar doğuracaktır.
