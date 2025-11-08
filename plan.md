# DLSS VR Per‑Eye Output Auto‑Detection and Uniform‑Scale Plan

This plan aligns the plugin with NVIDIA Streamline 2.9.0 programming guidance (ProgrammingGuideDLSS.md §8.0 Multiple Viewports) and fixes the `slAllocateResources failed: 25` error by ensuring:

- Per‑eye display (output) dimensions are detected reliably from the OpenVR Submit path (not guessed from intermediate textures).
- Render and output dimensions preserve identical aspect ratio (uniform scale) and even‑pixel alignment.
- DLSSOptions/Allocate/Tag/Evaluate are sequenced per‑viewport with correct sizes.

The work is organized into incremental, verifiable steps. No code is changed yet; this is the implementation blueprint.

---

## 1) Per‑Eye Display Size: Detect From OpenVR Submit

Files to touch:
- `dlss_hooks.cpp` (OpenVR Submit hook already present)
- `dlss_hooks.h` (expose a small query API)

Goals:
- Compute the true per‑eye “display” W/H in texels from the last IVRCompositor::Submit call:
  - Use submitted D3D11 texture desc (Width/Height)
  - Use `VRTextureBounds_t {uMin,uMax,vMin,vMax}` to compute the active region:
    - `eyeOutW = round_even(|uMax - uMin| * texWidth)`
    - `eyeOutH = round_even(|vMax - vMin| * texHeight)`
  - If bounds cover full texture (0..1), per‑eye out = texture desc.
  - If stereo atlas is used (side‑by‑side or top‑bottom), bounds will be half‑width or half‑height region; the math above already captures this.

Implementation details:
- Add a small “display tracker” in `dlss_hooks.cpp`:
  - `static std::atomic<uint32_t> g_perEyeOutW[2], g_perEyeOutH[2];`
  - Update these on every Submit(Left/Right) using the bounds+desc calculation.
  - Ensure the values are even (clear LSB) and clamped to sensible limits (≤8192).
- Expose getters in `dlss_hooks.h`:
  - `bool GetPerEyeDisplaySize(int eyeIndex, uint32_t& outW, uint32_t& outH);`
  - Returns false if not yet known.

Notes:
- This removes heuristics based on intermediate textures and makes output size robust across HMDs and driver settings.

Acceptance:
- Plugin log shows, shortly after first Submit per eye, a trace of detected display sizes per eye (optional informational log).

---

## 2) Use Streamline to Derive Render Size From Output (Optimal Settings)

Files to touch:
- `dlss_manager.h/.cpp` (where render size is currently decided via `GetOptimalSettings`)

Goals:
- Switch to canonical flow:
  1) Determine per‑eye display (output) size from step 1.
  2) Fill `sl::DLSSOptions` with desired `outputWidth/Height` and quality mode.
  3) Call `slDLSSGetOptimalSettings(options, settings)` to obtain recommended `renderWidth/Height`.
  4) Use those render dims in our downscale pass and DLSS tagging.

Implementation details:
- Change `GetOptimalSettings(uint32_t& renderW, uint32_t& renderH)` signature and call‑site to accept desired `outputW/outputH` and current DLSS mode.
- Call `slDLSSGetOptimalSettings` and read back `settings.renderWidth/Height` (round to even if needed).
- If the function returns non‑OK (rare), fall back to a conservative scale (e.g., Quality scale table), but keeping aspect ratio identical to output.

Acceptance:
- Log shows `ProcessEye: rw=R rh=R oh=O ow=O` where `(ow/rw) == (oh/rh)` within tolerance, and both R/O are even.

---

## 3) Enforce Uniform Scale + Even Alignment Everywhere

Files to touch:
- `dlss_manager.cpp` (in `ProcessEye` before calling backend)
- `src/backends/SLBackend.cpp` (on allocation thresholds and logging)

Goals:
- Ensure that for every frame:
  - `ow, oh, rw, rh` are all even.
  - `(ow/rw) ≈ (oh/rh)` (uniform scale). If not, adjust `rw/rh` to match output via OptimalSettings.
  - Clamp to safe maxima (≤8192) and ensure `ow ≥ rw`, `oh ≥ rh`.

Implementation details:
- After obtaining `outputW/H` from the display tracker and `renderW/H` from OptimalSettings, run a normalization step:
  - `ow &= ~1; oh &= ~1; rw &= ~1; rh &= ~1;`
  - If `fabs((double)ow/rw - (double)oh/rh) > 0.002` → recompute `rw/rh` with OptimalSettings (or adjust the larger side to match ratio) and re‑align to even.
- Keep a small hysteresis to avoid thrashing reallocations when dimensions fluctuate by ±2.

Acceptance:
- `slAllocateResources failed: 25` disappears from logs.
- First allocation after size change succeeds and remains stable until a real size change.

---

## 4) Depth/MV Compliance With Render Size

Files to touch:
- `dlss_manager.cpp` (where zero‑depth / zero‑MV fallbacks are created & validated)
- `src/backends/SLBackend.cpp` (ResolveDepthFormat already present)

Goals:
- Depth SRV view format is typed (R24_UNORM_X8_TYPELESS or R32_FLOAT) and size == render size; MSAA=1.
- MV is at least a zero‑texture at render size if real MV is unavailable.

Implementation details:
- Confirm existing checks:
  - If captured depth is mismatched or MSAA>1 → use zero‑depth created at `renderW/H` (typed format), else pass the real depth.
  - MV: if missing → zero‑MV at `renderW/H` (RG16F or similar). Maintain single allocation per size.

Acceptance:
- Log lines show `depth=1 mv=1` in Evaluate for normal gameplay (with fallbacks if needed). No format warnings.

---

## 5) Backend Allocation Sequence & Gating

Files to touch:
- `src/backends/SLBackend.cpp`

Goals:
- Correct order & conditions per viewport/eye:
  - Update `DLSSOptions.outputWidth/Height` → `slDLSSSetOptions(viewport, options)`.
  - (Re)allocate only when any of `(inW,inH,outW,outH)` changes.
  - `slAllocateResources(nullptr, sl::kFeatureDLSS, viewport)` must now succeed (no 25).
  - Tag inputs (with `eValidUntilEvaluate` extents) → set constants → `slSetTagForFrame` → `slEvaluateFeature`.
- Avoid stale upscaled submits on Evaluate failure.

Implementation details:
- Keep per‑eye cached `vpInW/H`, `vpOutW/H` and reallocate only on change.
- If `slEvaluateFeature` fails → mark “last evaluate ok = false” so submit falls back to native for that eye/frame.
- Remove hard‑coded per‑eye out heuristics; rely on sizes provided by `dlss_manager` (which uses display tracker + OptimalSettings).

Acceptance:
- `slAllocateResources failed: 25` no longer appears.
- Submit logs show `used=DLSS` only when Evaluate succeeded.

---

## 6) Config & Safety

Files to touch:
- `F4SEVR_DLSS.ini` (optional notes only)
- Docs (README/QUICK_START) to clarify placement and FG being disabled

Goals:
- Auto sizing is default (no user toggles required).
- FG (DLSS‑G) remains disabled for VR (no `nvngx_dlssg.dll` in package; feature not requested in code).
- Optional: Add `AutoOutputSize=true` and `EnforceUniformScale=true` flags (default true) for debugging.

Acceptance:
- Users do not need to tune sizes; detection works across devices.

---

## 7) Validation Pass

Steps:
1. Deploy minimal SL DLL set to EXE root: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, and `nvngx_dlss.dll`.
2. Start a short VR session; collect:
   - `F4SEVR_DLSS.log`
   - `SL\sl.log`
3. Verify:
   - First `ProcessEye`: `Evaluate: in=render out=display depth=1 mv=1` (no error 25).
   - Submit: `used=DLSS` with in/out per‑eye sizes consistent.
   - No “Missing viewport handle” or “Allocate 25”.

---

## 8) Task Breakdown & Touch Points

1. `dlss_hooks.cpp/.h`
   - Add `PerEyeDisplayTracker` (Submit hook computes OutW/OutH from bounds*texSize).
   - Expose `GetPerEyeDisplaySize(eye, outW, outH)`.

2. `dlss_manager.h/.cpp`
   - Replace ad‑hoc per‑eye output heuristics with `GetPerEyeDisplaySize` results; fallback if not yet known.
   - Call `slDLSSGetOptimalSettings(options, settings)` to obtain `renderW/H` from `outputW/H` + mode.
   - Normalize sizes (even & uniform scale), clamp to limits.
   - Pass normalized `renderW/H` & `outputW/H` to backend `ProcessEye`.

3. `src/backends/SLBackend.cpp`
   - Assume given `renderW/H` & `outputW/H` are final.
   - Update options → allocate on change → tag inputs (with extents) → set constants → evaluate.
   - Ensure “last evaluate ok” gating on submit paths is respected (no stale upscaled texture).

4. Docs (optional minor edits)
   - Clarify DLL placement and that FG is disabled for VR.

---

## 9) Risk & Rollback

Risks:
- Some HMDs or mods may alter Submit bounds unexpectedly. Our tracker must be resilient (reject zero/NaN/odd results, use last‑known good).

Rollback Plan:
- If `PerEyeDisplayTracker` fails to detect sizes in time, temporarily fall back to current heuristics (SBS/TB split), but still normalize via OptimalSettings.

---

## 10) Success Criteria

- No `slAllocateResources failed: 25` after size normalization is applied.
- DLSS SR evaluates successfully for both eyes; submit logs show `used=DLSS`.
- Visual output matches HMD per‑eye display resolution, with improved performance.

