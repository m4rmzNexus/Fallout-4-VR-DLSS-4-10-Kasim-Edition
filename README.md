# F4SEVR DLSS4 Plugin (Fallout 4 VR)

DLSS Super Resolution for Fallout 4 VR via F4SE. DX11 + OpenVR manual hooking, per‑eye viewport pipeline, and Streamline 2.9 integration.

This repository is prepared for community collaboration. The current focus is stabilizing DLSS SR in VR by enforcing correct per‑eye sizing and Streamline requirements.

## Current Status

- Done
  - Manual hooking enabled (DXGI + OpenVR Submit)
  - Streamline 2.9 (SL) + NGX 3.10.4 wired (DLSS SR only)
  - Per‑eye viewport lifecycle with frame token sharing (stereo)
  - Per‑eye display (output) auto‑detection from OpenVR Submit bounds
  - Render size derived from `slDLSSGetOptimalSettings` (uniform scale)
  - Even‑pixel alignment and safe bounds (≤8192), out ≥ in
  - Depth SRV view‑format fix (typeless → typed), MSAA=1 enforcement
  - Zero‑motion‑vectors and zero‑depth fallbacks at render size
  - FG (Frame Generation) explicitly disabled for VR (do not ship nvngx_dlssg.dll)

- Pending / Nice‑to‑have
  - Submit gating polish: never submit stale upscaled textures on evaluate failure
  - Optional: more camera constants (FOV, projection) for quality
  - Motion vector generation (quality improvement)

See `plan.md` for the detailed implementation roadmap and acceptance criteria.

## Why We Had Failures (Root Cause)

Past runs failed with `slAllocateResources failed: 25` due to invalid parameters. The core issue was anisotropic scaling: input (render) and output (display) sizes didn’t preserve the same aspect ratio per eye. DLSS SR requires uniform scaling with consistent aspect ratio. We now detect per‑eye output from OpenVR Submit bounds and derive render size from SL OptimalSettings to preserve ratio and alignment.

## Building

Requirements
- Windows 10/11, VS 2022 (v143 toolset)
- NVIDIA driver supporting DLSS 3.10.x
- SDKs (place locally, not committed):
  - NVIDIA Streamline SDK 2.9.0 (headers + `sl.interposer.lib` + runtime DLLs)
  - NVIDIA NGX SDK 3.10.4 (headers + import libs)

Scripts
- `build_vs2022.bat` (preferred) → builds `build\F4SEVR_DLSS.dll` and packages to `dist/`
- `test_build.bat` (MSBuild path)

Notes
- Repo includes path stubs for `streamline-sdk-v2.9.0` and `DLSS-310.4.0`. If you don’t have these SDKs, put them at repo root or set env vars before calling the scripts: `NGX_SDK_PATH`, optionally `SL_SDK_PATH`.

## Installing (Fallout 4 VR)

Copy the following:
- Plugin (via MO2 or manual):
  - `dist\MO2\Data\F4SE\Plugins\F4SEVR_DLSS.dll`
  - `dist\MO2\Data\F4SE\Plugins\F4SEVR_DLSS.ini`
- EXE root (next to `Fallout4VR.exe`):
  - `dist\Root\sl.interposer.dll`
  - `dist\Root\sl.common.dll`
  - `dist\Root\sl.dlss.dll`
  - `dist\Root\nvngx_dlss.dll`

Do NOT place `nvngx_dlssg.dll` (FG) — FG is disabled for VR.

## Runtime Verification

Logs
- `Documents\My Games\Fallout4VR\F4SE\Plugins\F4SEVR_DLSS.log`
- `Documents\My Games\Fallout4VR\F4SE\Plugins\SL\sl.log`

Expectations
- No `slAllocateResources failed: 25`
- `Evaluate: in=render out=display depth=1 mv=1`
- Submit logs show `used=DLSS`

## Contributing

We welcome PRs for:
- Motion vector generation (DX11 path), better constants, quality polish
- Robustness around device lost / resize / multi‑mod environments

Please include logs and your HMD setup (per‑eye resolution, SteamVR SS) in issues. For feature work, see `plan.md`.

## License / SDKs

This repo references NVIDIA SDKs (Streamline, NGX). Follow NVIDIA’s licensing/redistribution terms. Prefer placing SDKs locally and excluding large binaries from commits.
