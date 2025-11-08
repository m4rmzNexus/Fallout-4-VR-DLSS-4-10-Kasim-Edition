# FO4VR DLSS4 – Kök Neden Analizi ve Uygulanabilir Düzeltme Planı

Bu belge, mevcut DLSS entegrasyonunun Fallout 4 VR (D3D11 + OpenVR) üzerinde neden “etki yok gibi” hissedildiğini detaylı biçimde analiz eder ve sorunun kök nedenini adresleyen uygulanabilir bir plan sunar. Referanslar, bu depo içindeki somut kod noktalarına ve log satırlarına dayanmaktadır.

---

## Özet (1 dakikalık okuma)

- DLSS Evaluate gerçekten 1344×1488 → 2016×2232 çalışıyor ve Submit içinde DLSS çıktısı “orijinal eye RT”’ye kopyalanıyor (Blit/Copy) – loglar doğruluyor.
- Buna rağmen HMD’de “DLSS etkisi yok” hissi var; çünkü sahne hâlâ full-res (≈2016×2232/eye) gölgeleniyor, sonradan 1344×1488’e downsample edilip tekrar 2016×2232’ye upsample ediliyor. Shading maliyeti düşmüyor; sadece ek bir upscaler hattı çalışıyor.
- Mevcut kodda “Erken DLSS” için iki yaklaşım var: viewport clamp (mode=0) ve RT redirect (mode=1). Varsayılan INI ayarı viewport clamp; redirect kapalı. Clamp sahada yeterince etkili olmayabiliyor; asıl kazanım RT redirect (veya CreateTexture2D seviyesinde küçültme) ile gelir.
- Çözüm: Oyunu gerçekten düşük çözünürlükte render ettir (RT redirect veya CreateTexture2D alias). Önerilen hedef (SxS stereo için): combined 2688×1488 (sol 1344×1488 + sağ 1344×1488), DLSS out: 2016×2232 per-eye.

---

## Belirti ve Log Kanıtları

- DLSS Evaluate boyutları doğru (1344→2016). Kanıt: `dlss_manager.cpp` Evaluate logları (NGX/SL yolları).
- Submit öncesi “güvenli kopya” yapılıyor. Kanıt: `dlss_hooks.cpp` içinde:
  - “Copied DLSS output into original eye texture (Blit)” ve “(CopyResource)” satırları.
  - Kopya sonrası “orijinal handle + orijinal bounds” ile VR Compositor’a submit ediliyor; yani handle değişmiyor.
- Overlay’de “Display/Render 0x0” gibi anomaliler görülebiliyor; bu, overlay’in metrik/simge pointer’larına erişememesiyle alakalı ve burada kök neden değil.

---

## Kök Neden

Mevcut akışta sahne hâlâ per-eye ≈2016×2232 boyutunda gölgeleniyor. DLSS girdisi, ya bu full-res eye RT’den downscale ile üretiliyor (post-process), ya da clamp kısmen uygulanıyor ama gerçek RT boyutu küçülmediği için toplam shading maliyeti düşmüyor. Sonrasında DLSS 1344×1488 → 2016×2232 upsample ediyor ve sonuç, Submit öncesi orijinal eye RT’ye kopyalanıyor. Sonuç: Ekstra maliyet + gözle görülür kazanç yok.

Temel prensip: DLSS’den fayda görmek için sahne gölgelenmesinin DLSS “render size”ında yapılması gerekir. Sadece sonradan down→up yapmak shading maliyetini düşürmez.

---

## Kod Düzeyi Durum Özeti

- VR Submit hook: `dlss_hooks.cpp:566` `HookedVRCompositorSubmit`
  - DLSS işlenmiş output, mümkünse orijinal eye RT’ye kopyalanıyor (safe path).
  - İlgili loglar: “Copied DLSS output into original eye texture (Blit/CopyResource)”.
- DLSS Evaluate: `dlss_manager.cpp` NGX/SL yolunda Evaluate çağrısı ve boyut logları (1344→2016 senaryosu doğrulanıyor).
- Early DLSS (Erken render entegrasyonu):
  - Viewport clamp (mode=0): `dlss_hooks.cpp:1420` `HookedRSSetViewports`
    - Scene içinde, viewport’lar hedef render W×H’e “clamp” edilir. Heuristik ve %5 tolerans var.
  - RT redirect (mode=1): `dlss_hooks.cpp:1362` `HookedOMSetRenderTargets`
    - Büyük scene RT bind edildiğinde, aynı formatta küçük bir RTV oluşturulur ve shading oraya yönlendirilir.
    - Submit sırasında, küçük RT mevcutsa DLSS girdisi doğrudan küçük RT’ye çevrilir (downscale maliyeti atlanır).
  - CreateTexture2D hook: `dlss_hooks.cpp:1039` sadece “tespit” amaçlı; şu an aktif küçültme yapmıyor.
- INI (varsayılan): `F4SEVR_DLSS.ini` içinde `mEarlyDlssEnabled = true`, `mEarlyDlssMode = 0` (viewport clamp). Redirect kapalı.

Sonuç: Mevcut konfig ile shading çoğu durumda hâlâ büyük eye RT üstünde gerçekleşiyor. Redirect kapalı olduğundan sahici piksel sayısı düşmüyor.

---

## Uygulanabilir Düzeltme Planı

1) RT Redirect’i etkinleştir (düşük intrusiveness, yüksek kazanım)
   - INI: `mEarlyDlssMode = 1` (rt_redirect) ve `mEarlyDlssEnabled = true`.
   - Beklenen davranış:
     - SceneBegin’de büyük RT (ör. SxS 4032×2232) bind edilince, küçük RT (≈2688×1488) oluşturulur ve shading oraya yönlendirilir.
     - Submit’te DLSS girdisi, bu küçük RT olur; Evaluate 1344×1488 → 2016×2232 çalışır; sonuç orijinal eye RT’ye kompozit/blit edilir.
   - Doğrulama logları:
     - “[EarlyDLSS][RT] Created small RT …”
     - “[EarlyDLSS][Redirect] RTV old=… -> small=…”
     - “[EarlyDLSS][Submit] Using small RT as DLSS input”
     - “[EarlyDLSS][Composite] small->big …”

2) Alternatif/İleri adım: CreateTexture2D seviyesinde küçültme (daha deterministik)
   - `dlss_hooks.cpp:1039` `HookedCreateTexture2D` içinde VR eye RT’yi (tipik: BindFlags RT+SRV, Format RGBA8, boyut stereo SxS 4032×2232) tespit et.
   - Orijinal desc’i kopyalayıp küçült (combined 2688×1488; sol 0..1344, sağ 1344..2688 viewport).
   - Küçük tex’i cache’le ve orijinale alias’la (redirect map). `RSSetViewports` içinde iki viewport set et:
     - Sol: (0,0, 1344×1488), Sağ: (1344,0, 1344×1488).
   - Not: Bu yol, engine’in ileri/geri bind/resolve kalıplarına daha az bağımlıdır ama daha intrusivedir. Feature flag arkasına al.

3) Format / ColorSpace güvenliği
   - DLSS output’u `DXGI_FORMAT_R8G8B8A8_UNORM` olarak üretmek ve Submit’te `vr::Texture_t::eColorSpace` için Gamma kullanmak (Quest/Link gibi HDR zinciri yoksa) en sorunsuz yoldur.
   - Şu an Submit kopya yolu “orijinal handle + orijinal color space” ile gider; “handle replacement” fallback’ında bounds ve eColorSpace’i dikkatli set et.

4) Bounds (SxS stereo)
   - SxS ile tek birleşik yüzey gönderirken bounds’ı mutlaka set et:
     - Sol göz: uMin=0.0, uMax=0.5; Sağ göz: uMin=0.5, uMax=1.0.
   - Safe-copy yolunda bounds değişmeden kalır (tercihen böyle). Handle replacement yapacaksan, bounds’ı doğru set etmezsen SteamVR kareyi reddedebilir/tek göz gösterebilir.

5) “Güvenli kopya” stratejisini koru
   - “Copied DLSS output into original eye texture (Blit/Copy)” çizgisi Submit’ten önce olmalı. Şu an doğru yerde (bkz. `dlss_hooks.cpp` Submit).
   - Doğrudan DLSS output handle’ını submit etmeyi denemeden önce 1) ve 3)’ü mümkün kıl.

6) Doğrulama ve Telemetri
   - CreateTexture2D/OMSetRenderTargets logları: Eski → yeni boyutlar (4032×2232 → 2688×1488) yazılsın.
   - RSSetViewports logları: clamp/redirect sonrası iki viewport 1344×1488 atanıyor mu?
   - SteamVR Frame Timing (GPU time) ile ölç: Piksel sayısı gerçekten düştüyse GPU time düşer. DLSS overlay FPS/metrikleri yanıltıcı olabilir.

---

## Kod Noktaları ve Önerilen Dokunuşlar

- INI/Config
  - Dosya: `F4SEVR_DLSS.ini` – `mEarlyDlssEnabled=true`, `mEarlyDlssMode=1` (rt_redirect). Debug için `mDebugEarlyDlss=true`.
  - Kod: `dlss_config.{h,cpp}` – flag’ler ve UI senkronizasyonu hazır.

- RT Redirect (aktif)
  - Dosya: `dlss_hooks.cpp:1362` `HookedOMSetRenderTargets`
    - Small RT oluşturma: `GetOrCreateSmallRTVFor`
    - Submit girişi küçük RT’ye: `HookedVRCompositorSubmit` içinde redirect map kullanımı
    - Composite small→big: `CompositeIfNeededOnBigBind`

- Viewport Clamp (opsiyonel, konservatif)
  - Dosya: `dlss_hooks.cpp:1420` `HookedRSSetViewports`
    - %5 toleransla hedef out W×H’e uyan viewport’ları `ComputeRenderSizeForOutput` sonucu W×H’e küçültür.

- DLSS Render Boyutu Hesabı
  - Dosya: `dlss_manager.cpp:542` `ComputeRenderSizeForOutput`
    - Streamline optimal settings veya kalite tablosu ile render W×H üretir; even align ve clamp uygular.

- Submit Güvenli Kopya
  - Dosya: `dlss_hooks.cpp:739+` `HookedVRCompositorSubmit`
    - Önce orijinale kopya/blit, sonra `g_realVRSubmit(...)` orijinal handle ile çağrılır.

- CreateTexture2D Geliştirmesi (opsiyonel)
  - Dosya: `dlss_hooks.cpp:1039` `HookedCreateTexture2D`
    - Şu an sadece “tespit” yapıyor. FO4VR’ye özgü SxS eye RT’yi tespit edip küçültme/alias eklemek için ideal yer.

---

## Neden Şimdi Etki Görmüyorsun?

- DLSS Evaluate doğru çalışıyor (1344→2016) – ✅  
- Blit yapıp orijinale yazıyorsun – ✅  
- Ama sahne zaten 2016×2232’de çizildiği için kazanım yok (hatta down→up maliyeti var) – ❌

Dolayısıyla “DLSS etkisi yok” hissi beklenen bir sonuç. Shading’i DLSS render boyutuna indirmedikçe (RT redirect veya CreateTexture2D küçültme) GPU time düşmez.

---

## Riskler ve Notlar

- UI/HUD/Post-FX zinciri: RT redirect sonrası bazı post pass’leri büyük RT bekleyebilir. Mevcut composite (small→big) bunun için var; debug loglarıyla teyit et.
- Color space uyumsuzlukları: HDR/sRGB/Gamma bayrakları farklılaşıyorsa siyah kare/soluk görüntü görülebilir. VR submit color space’i Gamma’da tutmak (HDR yoksa) güvenli yoldur.
- Deferred contexts: RSSetViewports/OMSetRenderTargets hook’ları `CreateDeferredContext` için de kuruluyor; kapsama geniş.
- Overlay metrikleri: “Display/Render 0x0” gibi anomali, metrik pointer’ı ve overlay uyumsuzluğundan kaynaklı. Ölçümde SteamVR Frame Timing esas alınmalı.

---

## Hızlı Kontrol Listesi (Önerilen Saha Adımları)

1) INI’de `mEarlyDlssMode = 1` yap ve test et.  
2) Loglarda `[EarlyDLSS][RT]`, `[EarlyDLSS][Redirect]`, `[EarlyDLSS][Composite]` satırlarını gör – yoksa feature devreye girmemiştir.  
3) GPU time düşüyorsa, RT redirect çalışıyordur.  
4) Hâlâ düşük değilse, `HookedCreateTexture2D` ile deterministik küçültmeyi ekle (FO4VR SxS 4032×2232 → 2688×1488).  
5) Stabil olduktan sonra istersen viewport clamp’i kapatıp sadece redirect kullan (daha temiz yoldur).  

---

## Ek: Beklenen Boyutlar (Quality örneği)

- FO4VR tipik per-eye out: ≈2016×2232 → SxS combined ≈4032×2232.
- DLSS “Quality” render (per-eye): ≈1344×1488 → SxS combined ≈2688×1488.
- Pipeline hedefi: Shading 1344×1488 (per-eye), DLSS out 2016×2232 (per-eye), Submit’te orijinal handle’a kopya veya doğrudan gönderim (bounds/CS güvenli).

---

Bu plan, depodaki mevcut “Early DLSS” altyapısıyla uyumludur. Kısa vadede INI ile RT redirect’i açmak, orta vadede CreateTexture2D ile deterministik küçültmeye geçmek en pratik yol haritasıdır.

