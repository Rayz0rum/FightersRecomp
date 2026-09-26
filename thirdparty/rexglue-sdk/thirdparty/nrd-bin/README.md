# Path tracing denoiser runtimes

Libraries the D3D12 path tracer (`src/graphics/d3d12/path_tracer_denoisers.cpp`)
loads at runtime, copied next to `rexgpu-xenos.dll` by its build.

- `NRD.dll` here: NVIDIA Real-time Denoisers v4.17.3 (`thirdparty/NRD`
  submodule), built as a DLL with only the DXIL shaders embedded (normal
  encoding R10G10B10A2, linear roughness - the defaults the path tracer's
  shaders pack for):

  ```
  cmake -S thirdparty/NRD -B build-nrd -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DNRD_STATIC_LIBRARY=OFF -DNRD_EMBEDS_SPIRV_SHADERS=OFF ^
      -DNRD_EMBEDS_DXBC_SHADERS=OFF -DNRD_EMBEDS_DXIL_SHADERS=ON
  cmake --build build-nrd
  ```

  (the configure step downloads NRD's MathLib, ShaderMake and DXC).
- `../dlss`: NGX headers and `nvsdk_ngx_d.lib` from the DLSS SDK 310.7, and
  DLSS Ray Reconstruction `nvngx_dlssd.dll` 310.9.1.0 (NVIDIA signed).
- `../fidelityfx`: FSR Ray Regeneration from the AMD FidelityFX SDK
  (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, `Kits/FidelityFX`):
  the API and denoiser headers, and the signed `amd_fidelityfx_loader_dx12.dll`
  and `amd_fidelityfx_denoiser_dx12.dll` (Radeon RX 9000 series GPUs only).
