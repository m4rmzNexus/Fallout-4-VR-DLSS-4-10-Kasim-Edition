# FO4VR — DLSS-SR Erken Entegrasyon (D3D11 · Streamline 2.9 + NGX 310.x)

Bu plan, DLSS’i “post-process” aşamasından sahnenin asıl render aşamasına taşıyarak gerçek GPU maliyetini düşürmeyi ve FPS kazanımı elde etmeyi hedefler. Fallout 4 VR’de D3D11 hook’ları ile uygulanır, kademeli (risk kontrollü) fazlara ayrılmıştır.

---

## Hedefler
- Shading’i DLSS render W×H’ta çalıştırmak (geometry/shading piksel sayısını azaltmak).
- VR Submit öncesinde yalnızca upscaling/post-işlemi çalıştırmak.
- Per‑eye boyutları istikrarlı hesaplamak (OpenVR RecommendedRenderTargetSize).
- HUD/menü/post‑FX sırasını bozmayacak, foveated/periphery opsiyonlarını destekleyen bir mimari.
- Geri alma (rollback) ve telemetry/log görünürlüğü.

## Kısıtlar ve Prensipler
- Oyun motoruna doğrudan erişim yok; D3D11 hook’ları (CreateTexture2D, RSSetViewports, OMSetRenderTargets, Present/Resize) ile yönlendirilecek.
- Riskli değişiklikler feature flag’lerle aç/kapa yapılabilir olmalı.
- CPU‑bound sahnelerde FPS artışı sınırlı olabilir; hedef GPU frametime’dır.

## Feature Flag’ler (INI)
- `EarlyDlssEnabled` (bool, default=false): Erken render entegrasyonunu aktifleştirir.
- `EarlyDlssMode` (enum: `viewport` | `rt_redirect`, default=viewport): Uygulama tekniği.
- `PeripheryTAAEnabled` (bool, default=true): DLSS dikdörtgeni dışını TAA ile çöz.
- `FoveatedRenderingEnabled` (bool, default=false): FFR/FFU’yu açar (mevcut foveated parametrelerini kullanır).
- `DebugEarlyDlss` (bool, default=false): Geniş loglama ve güvenlik kontrolleri.

Flag’ler `dlss_config.{h,cpp}` altında tanımlanıp UI ile senkronize edilir.

---

## Faz 0 — Gözlemlenebilirlik ve Guard’lar (No‑op)
Amaç: Davranışı değiştirmeden, hangi RT/viewport’un “sahne” olduğunu güvenli şekilde belirlemek; flags/log altyapısını hazırlamak.

- INI okuma: Yeni flag’ler eklenir ve menü senkronizasyonu yapılır.
- Log:
  - Per-frame: “SceneBegin/SceneEnd” ipuçları (Present sonrası ilk büyük RTV bind → SceneBegin; VR Submit öncesi → SceneEnd).
  - RSSetViewports/OMSetRenderTargets çağrıları (DebugEarlyDlss açıkken kısıtlı sayıda log).
  - OptimalSettings query/result ve per-eye out ölçüleri zaten loglanıyor; koru.
- Doğrulama: Native/DLAA/DLSS modlarında loglar stabil; hiçbir davranış değişimi yok.

---

## Faz 1 — Viewport Küçültme (Düşük risk · Hızlı kazanç)
Amaç: Shading piksel sayısını azaltmak için sahne viewport’unu DLSS render W×H’ye “clamp” etmek.

- Hook: `ID3D11DeviceContext::RSSetViewports` (context vtable).
- Yardımcı: `OMSetRenderTargets` (sahne başlangıcını anlamak için heuristik).
- Heuristik:
  - SceneBegin: Present’tan sonra ilk büyük renk RTV bind edilince işaretle.
  - Scene içinde RSSetViewports çağrılarını dinle; `EarlyDlssEnabled && EarlyDlssMode==viewport` iken, width/height değerlerini `renderW/renderH`’e clamp et.
- Emniyet ve Log:
  - UI/HUD gibi küçük RT’lere ve depth-only pass’lere dokunma (RT boyutu + bind flag filtreleri).
  - DebugEarlyDlss açıkken “RSSetViewports clamp old=(w,h) → new=(rw,rh)” logla (spam limitli).
- Doğrulama:
  - SteamVR overlay: GPU frametime düşmeli.
  - Log: Clamp olayları görünür; DLSS color_in=render, color_out=display eşleşir.

Not: Bu yaklaşım shader graph’ı değiştirmeden hızlı kazanç sağlar; bazı post/overlay etkileşimlerini minimumda tutmak için filtreler uygulanır.

---

## Faz 2 — RT Yeniden Yönlendirme (Orta risk · Yüksek kazanç)
Amaç: Sahne göz RT’lerini doğrudan DLSS render W×H boyutunda oluşturmak veya oraya yönlendirmek; viewport clamp’a göre daha sağlam ve kalıcı çözüm.

- Hook’lar:
  - `ID3D11Device::CreateTexture2D`: Sahne renk RT’lerini tanı ve “renderW×renderH” boyutunda muadilini oluştur (format/bind koruyarak; MSAA=1). Orijinal talep büyükse swap et.
  - `ID3D11DeviceContext::OMSetRenderTargets`: Büyük RT bind edildiğinde bizim küçük RT’ye redirect et.
- Akış:
  1) SceneBegin → küçük RT bind edilir, shading render W×H’ta çalışır.
  2) SceneEnd → DLSS Evaluate ile display W×H üretilir.
  3) HUD/Post etkileniyorsa, kompozit/resolve adımı ile display’e bind edilir.
- Emniyet:
  - Yalnızca `EarlyDlssMode==rt_redirect` ve flag açıkken aktif.
  - Uyuşmayan RT tanımlarında swap yok (logla ve devam et).
- Doğrulama:
  - GPU frametime daha da düşer; Submit out boyutu sabit.

---

## Faz 3 — Periphery TAA ve Foveated Opsiyonları (Opsiyonel · VR’ye özel)
Amaç: Periferide maliyeti kısarken merkezde DLSS/IQ’yu korumak.

- `PeripheryTAAEnabled`: DLSS dikdörtgeni merkezde tutulur; dışarıyı TAA ile çözmek için basit mask‑tabanlı blend (UI/menü koruma).
- `FoveatedRenderingEnabled`: Mevcut foveated parametreleri (scale/offset/radius/widen) ile periferide shading ve/veya upscaling yoğunluğunu düşür.
- Doğrulama: Periferideki IQ kaybı kontrol altında, GPU frametime’da düşüş.

---

## Faz 4 — MIP LOD Bias ve Konstlar (Kalite ve Doğruluk)
Amaç: DLSS moduna göre MIP LOD bias’ı otomatik set etmek; jitter/matris/mvecScale doğrularını teyit etmek.

- `mUseOptimalMipLodBias==true` iken, kalite moduna göre tablodan bias uygula (örn. Perf → daha negatif; Quality → daha az negatif).
- Matrisler row‑major ve jitter ayrı parametre (SL Constants) olarak kalır; mvecScale piksel mi normalize mi doğrulanır (MV RG16F pikselse 1/renderW,1/renderH; normalize ise 1,1).

---

## Faz 5 — Sağlamlaştırma ve Telemetry
- Overlay/third‑party çatışmalarında `EarlyDlssEnabled=false`’a otomatik düşmek (log uyarısı ile).
- Sık boyut değişiminde kontrollü re‑alloc; VRAM limitleme (scratch ve viewport cache).
- Log ve Sayaçlar:
  - Per‑frame: RSSetViewports clamp sayısı, RT redirect sayısı, DLSS evaluate süresi (ms), downscale/diffuse süreleri.

---

## Faz 6 — Test Planı ve Kabul
- Sahneler: Native, DLAA, DLSS MaxQuality/Balanced/MaxPerf (her biri 10–15 sn), SS %100 ve %150+ varyasyonları.
- Ölçümler: SteamVR overlay CPU/GPU frametime, FPS; F4SEVR_DLSS.log (ProcessEye rw/rh/ow/oh + clamp/redirect logları), sl.log (dlssBeginEvent extents, used=DLSS).
- Kabul Kriterleri:
  - GPU frametime düşer (özellikle Performance modunda belirgin).
  - used=DLSS stabil; IQ kabul edilebilir (halolar/ghosting yok veya az).
  - Menü/HUD sağlam.

---

## Uygulama Detayları (Özet)

### Hook Noktaları
- Device vtable: `CreateTexture2D` → RT redirect (Faz 2).
- Context vtable: `RSSetViewports` (Faz 1), `OMSetRenderTargets` (heuristic), `Present` (zaten var), `ResizeBuffers` (zaten var).

### Heuristik ve Filtreler
- Sahne RT tespiti: Büyük renk RT (RTV|SRV), sample count=1, format=renk, per‑eye boyuta yakın → SceneBegin.
- UI/menü/post pass’leri: Küçük RT’ler, farklı bind set’leri veya özel formatlar → clamp/redirect yapma.

### Emniyet / Rollback
- INI: `EarlyDlssEnabled=false` → anında eski akış.
- DebugEarlyDlss: Geniş log; sorun halinde otomatik kapama (log uyarısı).

### Riskler ve Azaltımlar
- UI/Post bozulması: Kademeli açma (Faz 1 → Faz 2), whitelist/blacklist filtrasyonu.
- Çakışan overlay/proxy: Güvenli modda kapatma.
- RT tanımı uyuşmazlıkları: Swap yok; logla ve devam et.

---

## Yol Haritası (Uygulama Sırası)
1) Faz 0: Flag’ler + log görünürlüğü (davranış değişmeden).  
2) Faz 1: RSSetViewports clamp (default kapalı, flag ile aç).  
3) Faz 2: RT redirect (default kapalı, ileri testlerle aç).  
4) Faz 3: Periphery TAA & Foveated (opsiyonel).  
5) Faz 4: MIP LOD bias & konstlar (ince ayar).  
6) Faz 5–6: Sağlamlaştırma, telemetry, test ve kabul.

---

## Beklenen Sonuç
- DLSS render boyutuna çekilen shading sayesinde gerçek FPS artışı (GPU frametime düşüşü).
- VR Submit öncesinde sadece upscale/post basamağı → daha öngörülebilir maliyet.
- VR özel optimizasyonlar (periphery/foveated) ile ek kazanç.

Not: İlk kazanım için Faz 1 (viewport clamp) güvenlidir; daha büyük kazanım için Faz 2 (RT redirect) önerilir. Tüm adımlar feature flag’lerle güvence altına alınır ve geri alınabilir.
---

## Uygulama Kontrol Listesi (Güncel Durum)

- [x] INI/Config: EarlyDlssEnabled, EarlyDlssMode, DebugEarlyDlss okuma/yazma.
- [x] UI: ImGui menüsünde "Early DLSS (Experimental)" bölümü (toggle, mod, debug).
- [x] Log: OpenVR RecommendedRenderTargetSize per-eye alınıp düşük frekansta debug satırları (davranış değişimi yok).
- [x] Yardımcı: DLSSManager::ComputeRenderSizeForOutput(out→render) eklendi.
- [x] Faz 1: RSSetViewports/OMSetRenderTargets hook'ları (viewport clamp) ve DebugEarlyDlss logları (konservatif clamp; flag‑gated).
- [x] Faz 2 (zemin): CreateTexture2D/OMSetRenderTargets ile RT redirect - ilk sahne RTV bind'ında küçük RT'ye yönlendirme, cache ve loglar (flag-gated).
    
### DEV‑v2 Additions
- [x] Context coverage: Hook CreateDeferredContext so RSSetViewports/OMSetRenderTargets clamps apply to deferred contexts too.
- [x] Per-eye sizing: Prefer IVRSystem::GetRecommendedRenderTargetSize; fallback detects SxS/top-bottom atlases (never treat full atlas as per‑eye). Optional cap to limit per‑eye max dimension (EnablePerEyeCap, PerEyeMaxDim).
- [x] Guardrails/logs: Throttled “[EarlyDLSS][CLAMP] skip: …” reasons when clamp is not applied.
- [/] Optional IQ path: HighQualityComposite flag is wired (currently falls back to linear blit). Default remains linear.

### Config precedence note
- The plugin first tries the Documents path: `C:\Users\<user>\Documents\My Games\Fallout4VR\F4SE\Plugins\F4SEVR_DLSS.ini`.
- If missing, it falls back to the plugin directory INI; Save() always writes to the Documents path.
- Ensure Balanced is set with `QualityLevel = 1` in the Documents INI for testing.
