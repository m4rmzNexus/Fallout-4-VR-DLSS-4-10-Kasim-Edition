# DLSS VR İyileştirme Görev Listesi

Bu görev listesi, `docs/DLSS_VR_Final_Analysis_v3.md` içindeki bulgulara göre hazırlanmıştır. Her madde ilgili bölümdeki referansla birlikte açıklanmıştır.

## 1. Early DLSS Kilitlerini Kaldır
- **Referans:** “Kritik Uyarı” ve “Faz 0: Kilitleri Kaldır” bölümleri.
- **Görevler:**
  - [ ] `dlss_hooks.cpp` satır 29’daki `kEarlyDlssFeatureEnabled = false` sabitini konfigürasyona bağlı hâle getir ve varsayılanı `true` yap.
  - [ ] `dlss_config.cpp` satır 229 ve 439’daki `ForceDisableEarlyDlss()` çağrılarını kaldır ya da ini flag ile kontrol et.
  - [ ] Değişiklikten sonra loglarda `[EarlyDLSS]` mesajlarının göründüğünü doğrula.

## 2. DLSS Backend Sağlığını Doğrula
- **Referans:** “Kritik Uyarı #3” ve “Faz 1: DLSS’i Çalıştır”.
- **Görevler:**
  - [ ] `build_vs2022_last.txt` içindeki Streamline unresolved symbol hatalarını gider (delayload + `delayimp.lib`).
  - [ ] Gerekli DLL’leri (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll) oyun klasörüne kopyala.
  - [ ] Runtime’da `[SL] ProcessEye` loglarını takip ederek DLSS evaluate’in çalıştığını doğrula.

## 3. Telemetry ve Debug Altyapısı
- **Referans:** `DLSS_Implementation_Patches.md`, Patch 2 (Debug/Metrics).
- **Görevler:**
  - [ ] Frame counter ve success/fail sayaçları ekle, konfigürasyonla açılıp kapanabilen log makroları tanımla.
  - [ ] Copy/handle swap adımlarının süresini ölçmek için isteğe bağlı performance counter ekle.
  - [ ] SteamVR Frame Timing ölçümlerini kayıt altına almak için test prosedürü oluştur (belgede “Test Protokolü” bölümü).

## 4. Handle Swap Prototipi
- **Referans:** `DLSS_Implementation_Patches.md`, Patch 1 ve `DLSS_VR_Final_Analysis_v3.md` Faz 2.
- **Görevler:**
  - [ ] `HookedVRCompositorSubmit` içinde `up` texture’ını doğrudan submit edecek branch yaz.
  - [ ] Color space, bounds ve MSAA uyumluluğunu doğrulayan guard’lar ekle; başarısızlık durumunda copy path’e geri düş.
  - [ ] `ProcessEye` dönen texture’ın yaşam süresini yönetmek için referans sayacı veya wrapper geliştir.
  - [ ] Test sonuçlarını SteamVR frame timing ile belgeleyip compare et.

## 5. Viewport Clamp ve RT Redirect Reaktivasyonu
- **Referans:** `DLSS_VR_Final_Analysis_v3.md`, “Early DLSS Path” bölümü.
- **Görevler:**
  - [ ] `HookedRSSetViewports` ve `HookedOMSetRenderTargets` içindeki Early DLSS kontrollerini yeniden etkinleştir.
  - [ ] `[EarlyDLSS][RT]`, `[Redirect]`, `[Composite]` loglarının çalıştığını doğrula.
  - [ ] GPU time ölçerek clamp/redirect’in gerçekten render çözünürlüğünü düşürdüğünü ispatla.

## 6. GetRecommendedRenderTargetSize Hook (Deneysel)
- **Referans:** `DLSS_Implementation_Patches.md`, Patch 3; `DLSS_VR_Final_Analysis_v3.md`, Faz 3.
- **Görevler:**
  - [ ] IVRSystem vtable index’ini doğrula ve `GetRecommendedRenderTargetSize`’ı hook eden güvenli bir helper ekle.
  - [ ] Hook’u yalnızca Early DLSS stable olduğunda etkinleştir; fallback için enable/disable flag ekle.
  - [ ] HUD/UI scaling testleri yap; sorun olursa otomatik rollback mekanizması hazırla.

## 7. Regresyon ve Siyah Ekran Testleri
- **Referans:** “Siyah Ekran Analizi” ve “Test Protokolü” bölümleri.
- **Görevler:**
  - [ ] Sanctuary, Diamond City ve interior sahnelerinde siyah ekran/GPU time regresyon testleri yürüt; sonuçları tabloyla kaydet.
  - [ ] Hata durumunda (ör. DLSS up null, format mismatch) otomatik fallback logları ve sayaçlarını kontrol et.
  - [ ] Handle swap veya render-size hook devredeyken siyah ekran oluşursa hangi guard’ın tetiklendiğini logla.

## 8. Belgeleme ve İzleme
- **Referans:** `DLSS_VR_Final_Analysis_v3.md`, “Kritik Kontrol Listesi” ve “Özet” bölümleri.
- **Görevler:**
  - [ ] Yukarıdaki her fazı tamamladıkça `DLSS_VR_Final_Analysis_v3.md` veya yeni bir sürümde güncel durum notları ekle.
  - [ ] Test sonuçlarını (frame time, GPU util, gözlemlenen sorunlar) bir tablo halinde sakla.

Bu liste tamamlandığında DLSS’in Fallout 4 VR’da gerçek performans kazancı sağlaması için gerekli temel adımlar yerine getirilmiş olacaktır.
