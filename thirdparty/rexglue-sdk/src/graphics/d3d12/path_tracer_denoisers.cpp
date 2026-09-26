/**
 * @file        graphics/d3d12/path_tracer_denoisers.cpp
 * @brief       Denoisers of the path-traced lighting.
 *
 * - NVIDIA Real-time Denoisers (NRD, the default): REBLUR for the indirect
 *   diffuse and specular lighting and SIGMA for the sun shadows, dispatched
 *   through its low-level API (its DXIL shaders, pipelines and texture pools
 *   created here). NRD.dll is loaded at runtime from the executable's
 *   directory.
 * - DLSS Ray Reconstruction (NVIDIA RTX GPUs): denoises the lit color with the
 *   albedo, normal, depth and motion guides, through NGX (nvngx_dlssd.dll next
 *   to the executable).
 * - FSR Ray Regeneration (AMD Radeon RX 9000 GPUs): the same separate signals
 *   as NRD, through the FidelityFX API loader (amd_fidelityfx_loader_dx12.dll
 *   and amd_fidelityfx_denoiser_dx12.dll next to the executable).
 *
 * All inputs and outputs cover the scene rectangle; the matrices describe the
 * world space tracked from the camera motion (UpdatePathTracingCamera). The
 * vendor denoisers record into the command list when it's executed (see
 * DeferredCommandList::ExternalCallback).
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include <NRD.h>

#include <nvsdk_ngx_helpers_dlssd.h>

#include <denoisers/include/ffx_denoiser.h>
#include <api/include/dx12/ffx_api_dx12.h>

REXCVAR_DECLARE(std::string, path_tracing_denoiser);
REXCVAR_DEFINE_DOUBLE(path_tracing_nrd_accumulation_time, 0.5, "GPU/Path Tracing",
                      "NRD: seconds of history accumulated (longer - smoother, more lag)");
REXCVAR_DEFINE_DOUBLE(path_tracing_nrd_blur_radius, 30.0, "GPU/Path Tracing",
                      "NRD: maximum spatial blur radius in pixels");

namespace rex::graphics::d3d12 {

namespace {

// Directory of a module (nullptr - the executable).
std::wstring ModuleDirectory(HMODULE module) {
  wchar_t path[MAX_PATH];
  DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
  std::wstring directory(path, length);
  size_t separator = directory.find_last_of(L"\\/");
  return separator != std::wstring::npos ? directory.substr(0, separator + 1) : std::wstring();
}

// The runtime libraries are looked for next to this plugin, then next to the
// executable.
HMODULE LoadRuntimeLibrary(const wchar_t* name) {
  HMODULE plugin = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&ModuleDirectory), &plugin);
  for (HMODULE module : {plugin, HMODULE(nullptr)}) {
    if (HMODULE library = LoadLibraryW((ModuleDirectory(module) + name).c_str())) {
      return library;
    }
  }
  return nullptr;
}

DXGI_FORMAT NRDFormatToDXGI(nrd::Format format) {
  switch (format) {
    case nrd::Format::R8_UNORM:
      return DXGI_FORMAT_R8_UNORM;
    case nrd::Format::R8_SNORM:
      return DXGI_FORMAT_R8_SNORM;
    case nrd::Format::R8_UINT:
      return DXGI_FORMAT_R8_UINT;
    case nrd::Format::R8_SINT:
      return DXGI_FORMAT_R8_SINT;
    case nrd::Format::RG8_UNORM:
      return DXGI_FORMAT_R8G8_UNORM;
    case nrd::Format::RG8_SNORM:
      return DXGI_FORMAT_R8G8_SNORM;
    case nrd::Format::RG8_UINT:
      return DXGI_FORMAT_R8G8_UINT;
    case nrd::Format::RG8_SINT:
      return DXGI_FORMAT_R8G8_SINT;
    case nrd::Format::RGBA8_UNORM:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA8_SNORM:
      return DXGI_FORMAT_R8G8B8A8_SNORM;
    case nrd::Format::RGBA8_UINT:
      return DXGI_FORMAT_R8G8B8A8_UINT;
    case nrd::Format::RGBA8_SINT:
      return DXGI_FORMAT_R8G8B8A8_SINT;
    case nrd::Format::RGBA8_SRGB:
      return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case nrd::Format::R16_UNORM:
      return DXGI_FORMAT_R16_UNORM;
    case nrd::Format::R16_SNORM:
      return DXGI_FORMAT_R16_SNORM;
    case nrd::Format::R16_UINT:
      return DXGI_FORMAT_R16_UINT;
    case nrd::Format::R16_SINT:
      return DXGI_FORMAT_R16_SINT;
    case nrd::Format::R16_SFLOAT:
      return DXGI_FORMAT_R16_FLOAT;
    case nrd::Format::RG16_UNORM:
      return DXGI_FORMAT_R16G16_UNORM;
    case nrd::Format::RG16_SNORM:
      return DXGI_FORMAT_R16G16_SNORM;
    case nrd::Format::RG16_UINT:
      return DXGI_FORMAT_R16G16_UINT;
    case nrd::Format::RG16_SINT:
      return DXGI_FORMAT_R16G16_SINT;
    case nrd::Format::RG16_SFLOAT:
      return DXGI_FORMAT_R16G16_FLOAT;
    case nrd::Format::RGBA16_UNORM:
      return DXGI_FORMAT_R16G16B16A16_UNORM;
    case nrd::Format::RGBA16_SNORM:
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case nrd::Format::RGBA16_UINT:
      return DXGI_FORMAT_R16G16B16A16_UINT;
    case nrd::Format::RGBA16_SINT:
      return DXGI_FORMAT_R16G16B16A16_SINT;
    case nrd::Format::RGBA16_SFLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case nrd::Format::R32_UINT:
      return DXGI_FORMAT_R32_UINT;
    case nrd::Format::R32_SINT:
      return DXGI_FORMAT_R32_SINT;
    case nrd::Format::R32_SFLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case nrd::Format::RG32_UINT:
      return DXGI_FORMAT_R32G32_UINT;
    case nrd::Format::RG32_SINT:
      return DXGI_FORMAT_R32G32_SINT;
    case nrd::Format::RG32_SFLOAT:
      return DXGI_FORMAT_R32G32_FLOAT;
    case nrd::Format::RGB32_UINT:
      return DXGI_FORMAT_R32G32B32_UINT;
    case nrd::Format::RGB32_SINT:
      return DXGI_FORMAT_R32G32B32_SINT;
    case nrd::Format::RGB32_SFLOAT:
      return DXGI_FORMAT_R32G32B32_FLOAT;
    case nrd::Format::RGBA32_UINT:
      return DXGI_FORMAT_R32G32B32A32_UINT;
    case nrd::Format::RGBA32_SINT:
      return DXGI_FORMAT_R32G32B32A32_SINT;
    case nrd::Format::RGBA32_SFLOAT:
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case nrd::Format::R10_G10_B10_A2_UINT:
      return DXGI_FORMAT_R10G10B10A2_UINT;
    case nrd::Format::R11_G11_B10_UFLOAT:
      return DXGI_FORMAT_R11G11B10_FLOAT;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:
      return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

constexpr nrd::Identifier kNRDReblur = 0;
constexpr nrd::Identifier kNRDSigma = 1;
// NRD constant data per frame (constantBufferMaxDataSize is well below this,
// times the dispatches).
constexpr uint32_t kNRDConstantSlotSize = 1024;
constexpr uint32_t kNRDMaxDispatches = 64;

}  // namespace

struct D3D12CommandProcessor::PathTracingDenoiserState {
  // NRD.
  bool nrd_load_attempted = false;
  bool nrd_failed = false;
  HMODULE nrd_module = nullptr;
  decltype(&nrd::CreateInstance) nrd_create_instance = nullptr;
  decltype(&nrd::DestroyInstance) nrd_destroy_instance = nullptr;
  decltype(&nrd::GetLibraryDesc) nrd_get_library_desc = nullptr;
  decltype(&nrd::GetInstanceDesc) nrd_get_instance_desc = nullptr;
  decltype(&nrd::SetCommonSettings) nrd_set_common_settings = nullptr;
  decltype(&nrd::SetDenoiserSettings) nrd_set_denoiser_settings = nullptr;
  decltype(&nrd::GetComputeDispatches) nrd_get_compute_dispatches = nullptr;
  nrd::Instance* nrd_instance = nullptr;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> nrd_root_signature;
  std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> nrd_pipelines;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> nrd_permanent_pool;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> nrd_transient_pool;
  uint32_t nrd_width = 0;
  uint32_t nrd_height = 0;
  uint32_t nrd_textures_max = 0;
  uint32_t nrd_storage_textures_max = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> nrd_constants;
  uint8_t* nrd_constants_mapping = nullptr;
  uint32_t nrd_frame_index = 0;

  // DLSS Ray Reconstruction.
  bool ngx_init_attempted = false;
  bool ngx_initialized = false;
  bool dlss_available = false;
  bool dlss_failed = false;
  NVSDK_NGX_Parameter* ngx_parameters = nullptr;
  NVSDK_NGX_Handle* dlss_handle = nullptr;
  uint32_t dlss_width = 0;
  uint32_t dlss_height = 0;
  struct DLSSJob {
    D3D12CommandProcessor* command_processor;
    PathTracingDenoiserState* state;
    PathTracingDenoiseInputs inputs;
    bool create;
  };
  DLSSJob dlss_jobs[kQueueFrames];

  // FSR Ray Regeneration.
  bool ffx_load_attempted = false;
  bool ffx_failed = false;
  HMODULE ffx_module = nullptr;
  PfnFfxCreateContext ffx_create_context = nullptr;
  PfnFfxDestroyContext ffx_destroy_context = nullptr;
  PfnFfxDispatch ffx_dispatch = nullptr;
  ffxContext ffx_context = nullptr;
  uint32_t ffx_width = 0;
  uint32_t ffx_height = 0;
  struct FSRJob {
    PathTracingDenoiserState* state;
    PathTracingDenoiseInputs inputs;
  };
  FSRJob fsr_jobs[kQueueFrames];
};

D3D12CommandProcessor::PathTracingDenoiser D3D12CommandProcessor::SelectPathTracingDenoiser() {
  if (!pt_denoiser_state_) {
    pt_denoiser_state_ = new PathTracingDenoiserState;
  }
  PathTracingDenoiserState& state = *pt_denoiser_state_;
  const std::string& name = REXCVAR_GET(path_tracing_denoiser);
  PathTracingDenoiser requested = PathTracingDenoiser::kNRD;
  if (name == "builtin") {
    requested = PathTracingDenoiser::kBuiltin;
  } else if (name == "dlss_rr" || name == "dlss") {
    requested = PathTracingDenoiser::kDLSSRR;
  } else if (name == "fsr_rr" || name == "fsr") {
    requested = PathTracingDenoiser::kFSRRR;
  }

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  std::wstring directory = ModuleDirectory(nullptr);

  if (requested == PathTracingDenoiser::kDLSSRR) {
    if (!state.ngx_init_attempted) {
      state.ngx_init_attempted = true;
      NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
          "7b3f1f9c-6a52-4e3d-9d31-5f0e2c4b8a71", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
          directory.c_str(), provider.GetDevice());
      if (NVSDK_NGX_SUCCEED(result)) {
        state.ngx_initialized = true;
        NVSDK_NGX_Parameter* capabilities = nullptr;
        int available = 0, needs_driver = 0;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities)) &&
            capabilities) {
          NVSDK_NGX_Parameter_GetI(capabilities,
                                   NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,
                                   &available);
          NVSDK_NGX_Parameter_GetI(capabilities,
                                   NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,
                                   &needs_driver);
          NVSDK_NGX_D3D12_DestroyParameters(capabilities);
        }
        if (available &&
            NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_AllocateParameters(&state.ngx_parameters))) {
          state.dlss_available = true;
          REXGPU_INFO("Path tracing: DLSS Ray Reconstruction available");
        } else {
          REXGPU_WARN("Path tracing: DLSS Ray Reconstruction not available{}",
                      needs_driver ? " (the driver needs updating)" : "");
        }
      } else {
        REXGPU_WARN("Path tracing: NGX initialization failed ({:08X})", uint32_t(result));
      }
    }
    if (state.dlss_available && !state.dlss_failed) {
      return PathTracingDenoiser::kDLSSRR;
    }
    requested = PathTracingDenoiser::kNRD;
  }

  if (requested == PathTracingDenoiser::kFSRRR) {
    if (!state.ffx_load_attempted) {
      state.ffx_load_attempted = true;
      state.ffx_module = LoadRuntimeLibrary(L"amd_fidelityfx_loader_dx12.dll");
      if (state.ffx_module) {
        state.ffx_create_context = reinterpret_cast<PfnFfxCreateContext>(
            GetProcAddress(state.ffx_module, "ffxCreateContext"));
        state.ffx_destroy_context = reinterpret_cast<PfnFfxDestroyContext>(
            GetProcAddress(state.ffx_module, "ffxDestroyContext"));
        state.ffx_dispatch =
            reinterpret_cast<PfnFfxDispatch>(GetProcAddress(state.ffx_module, "ffxDispatch"));
      }
      if (!state.ffx_create_context || !state.ffx_destroy_context || !state.ffx_dispatch) {
        REXGPU_WARN("Path tracing: FSR Ray Regeneration not available (the FidelityFX loader "
                    "wasn't found)");
        state.ffx_failed = true;
      }
    }
    if (!state.ffx_failed) {
      return PathTracingDenoiser::kFSRRR;
    }
    requested = PathTracingDenoiser::kNRD;
  }

  if (requested == PathTracingDenoiser::kNRD) {
    if (!state.nrd_load_attempted) {
      state.nrd_load_attempted = true;
      state.nrd_module = LoadRuntimeLibrary(L"NRD.dll");
      if (state.nrd_module) {
        auto get = [&](const char* name) { return GetProcAddress(state.nrd_module, name); };
        state.nrd_create_instance =
            reinterpret_cast<decltype(&nrd::CreateInstance)>(get("CreateInstance"));
        state.nrd_destroy_instance =
            reinterpret_cast<decltype(&nrd::DestroyInstance)>(get("DestroyInstance"));
        state.nrd_get_library_desc =
            reinterpret_cast<decltype(&nrd::GetLibraryDesc)>(get("GetLibraryDesc"));
        state.nrd_get_instance_desc =
            reinterpret_cast<decltype(&nrd::GetInstanceDesc)>(get("GetInstanceDesc"));
        state.nrd_set_common_settings =
            reinterpret_cast<decltype(&nrd::SetCommonSettings)>(get("SetCommonSettings"));
        state.nrd_set_denoiser_settings =
            reinterpret_cast<decltype(&nrd::SetDenoiserSettings)>(get("SetDenoiserSettings"));
        state.nrd_get_compute_dispatches =
            reinterpret_cast<decltype(&nrd::GetComputeDispatches)>(get("GetComputeDispatches"));
      }
      bool loaded = state.nrd_create_instance && state.nrd_destroy_instance &&
                    state.nrd_get_library_desc && state.nrd_get_instance_desc &&
                    state.nrd_set_common_settings && state.nrd_set_denoiser_settings &&
                    state.nrd_get_compute_dispatches;
      if (loaded) {
        const nrd::LibraryDesc* library = state.nrd_get_library_desc();
        if (!library || library->versionMajor != NRD_VERSION_MAJOR ||
            library->versionMinor != NRD_VERSION_MINOR ||
            library->normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM ||
            library->roughnessEncoding != nrd::RoughnessEncoding::LINEAR) {
          REXGPU_WARN("Path tracing: NRD.dll doesn't match the expected version and encodings");
          loaded = false;
        } else {
          REXGPU_INFO("Path tracing: NRD {}.{}.{} loaded", library->versionMajor,
                      library->versionMinor, library->versionBuild);
        }
      } else {
        REXGPU_WARN("Path tracing: NRD.dll not found next to the executable");
      }
      state.nrd_failed = !loaded;
    }
    if (!state.nrd_failed) {
      return PathTracingDenoiser::kNRD;
    }
  }
  return PathTracingDenoiser::kBuiltin;
}

bool D3D12CommandProcessor::PathTracingDenoiseNRD(const PathTracingDenoiseInputs& inputs) {
  PathTracingDenoiserState& state = *pt_denoiser_state_;
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // The instance, its pipelines and textures (for this size).
  if (!state.nrd_instance) {
    nrd::DenoiserDesc denoisers[] = {
        {kNRDReblur, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR},
        {kNRDSigma, nrd::Denoiser::SIGMA_SHADOW},
    };
    nrd::InstanceCreationDesc creation = {};
    creation.denoisers = denoisers;
    creation.denoisersNum = uint32_t(rex::countof(denoisers));
    if (state.nrd_create_instance(creation, state.nrd_instance) != nrd::Result::SUCCESS) {
      REXGPU_ERROR("Path tracing: failed to create the NRD instance");
      state.nrd_instance = nullptr;
      state.nrd_failed = true;
      return false;
    }
    const nrd::InstanceDesc& desc = *state.nrd_get_instance_desc(*state.nrd_instance);
    state.nrd_textures_max = std::max(desc.descriptorPoolDesc.perSetTexturesMaxNum, uint32_t(1));
    state.nrd_storage_textures_max =
        std::max(desc.descriptorPoolDesc.perSetStorageTexturesMaxNum, uint32_t(1));

    // Root signature: constants and samplers in one space, the textures in
    // another (see NRD.hlsli).
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = state.nrd_textures_max;
    ranges[0].BaseShaderRegister = desc.resourcesBaseRegisterIndex;
    ranges[0].RegisterSpace = desc.resourcesSpaceIndex;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = state.nrd_storage_textures_max;
    ranges[1].BaseShaderRegister = desc.resourcesBaseRegisterIndex;
    ranges[1].RegisterSpace = desc.resourcesSpaceIndex;
    D3D12_ROOT_PARAMETER parameters[3] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = desc.constantBufferRegisterIndex;
    parameters[0].Descriptor.RegisterSpace = desc.constantBufferAndSamplersSpaceIndex;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (uint32_t i = 0; i < 2; ++i) {
      parameters[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
      parameters[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
      parameters[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(desc.samplersNum);
    for (uint32_t i = 0; i < desc.samplersNum; ++i) {
      D3D12_STATIC_SAMPLER_DESC& sampler = samplers[i];
      sampler = {};
      sampler.Filter = desc.samplers[i] == nrd::Sampler::LINEAR_CLAMP
                           ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                           : D3D12_FILTER_MIN_MAG_MIP_POINT;
      sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      sampler.MaxLOD = D3D12_FLOAT32_MAX;
      sampler.ShaderRegister = desc.samplersBaseRegisterIndex + i;
      sampler.RegisterSpace = desc.constantBufferAndSamplersSpaceIndex;
      sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
    root_signature_desc.NumParameters = 3;
    root_signature_desc.pParameters = parameters;
    root_signature_desc.NumStaticSamplers = UINT(samplers.size());
    root_signature_desc.pStaticSamplers = samplers.data();
    *(state.nrd_root_signature.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_desc);
    bool created = state.nrd_root_signature != nullptr;
    state.nrd_pipelines.clear();
    for (uint32_t i = 0; created && i < desc.pipelinesNum; ++i) {
      const nrd::ComputeShaderDesc& shader = desc.pipelines[i].computeShaderDXIL;
      Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
      if (shader.bytecode && shader.size) {
        *(pipeline.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
            device, shader.bytecode, size_t(shader.size), state.nrd_root_signature.Get());
      }
      created = pipeline != nullptr;
      state.nrd_pipelines.push_back(pipeline);
    }
    if (created && !state.nrd_constants) {
      D3D12_RESOURCE_DESC buffer_desc;
      ui::d3d12::util::FillBufferResourceDesc(
          buffer_desc, uint64_t(kNRDConstantSlotSize) * kNRDMaxDispatches * kQueueFrames,
          D3D12_RESOURCE_FLAG_NONE);
      void* mapping = nullptr;
      D3D12_RANGE no_read = {};
      created = SUCCEEDED(device->CreateCommittedResource(
                    &ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&state.nrd_constants))) &&
                SUCCEEDED(state.nrd_constants->Map(0, &no_read, &mapping));
      state.nrd_constants_mapping = static_cast<uint8_t*>(mapping);
    }
    if (!created || desc.constantBufferMaxDataSize > kNRDConstantSlotSize) {
      REXGPU_ERROR("Path tracing: failed to create the NRD pipelines");
      state.nrd_destroy_instance(*state.nrd_instance);
      state.nrd_instance = nullptr;
      state.nrd_failed = true;
      return false;
    }
    state.nrd_width = 0;
    state.nrd_height = 0;
  }
  const nrd::InstanceDesc& desc = *state.nrd_get_instance_desc(*state.nrd_instance);
  if (state.nrd_width != inputs.width || state.nrd_height != inputs.height) {
    for (auto& texture : state.nrd_permanent_pool) {
      pt_retired_textures_.emplace_back(submission_current_, std::move(texture));
    }
    for (auto& texture : state.nrd_transient_pool) {
      pt_retired_textures_.emplace_back(submission_current_, std::move(texture));
    }
    state.nrd_permanent_pool.clear();
    state.nrd_transient_pool.clear();
    D3D12_RESOURCE_DESC texture_desc = {};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto create_pool = [&](const nrd::TextureDesc* textures, uint32_t count,
                           std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& pool) {
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t downsample = std::max(uint32_t(textures[i].downsampleFactor), uint32_t(1));
        texture_desc.Format = NRDFormatToDXGI(textures[i].format);
        texture_desc.Width = std::max((inputs.width + downsample - 1) / downsample, uint32_t(1));
        texture_desc.Height =
            std::max((inputs.height + downsample - 1) / downsample, uint32_t(1));
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        if (texture_desc.Format == DXGI_FORMAT_UNKNOWN ||
            FAILED(device->CreateCommittedResource(
                &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &texture_desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&texture)))) {
          return false;
        }
        pool.push_back(texture);
      }
      return true;
    };
    if (!create_pool(desc.permanentPool, desc.permanentPoolSize, state.nrd_permanent_pool) ||
        !create_pool(desc.transientPool, desc.transientPoolSize, state.nrd_transient_pool)) {
      REXGPU_ERROR("Path tracing: failed to create the NRD textures");
      state.nrd_permanent_pool.clear();
      state.nrd_transient_pool.clear();
      return false;
    }
    state.nrd_width = inputs.width;
    state.nrd_height = inputs.height;
  }

  // Settings.
  nrd::CommonSettings common = {};
  std::memcpy(common.viewToClipMatrix, inputs.view_to_clip, sizeof(common.viewToClipMatrix));
  std::memcpy(common.viewToClipMatrixPrev, inputs.view_to_clip,
              sizeof(common.viewToClipMatrixPrev));
  std::memcpy(common.worldToViewMatrix, inputs.world_to_view, sizeof(common.worldToViewMatrix));
  std::memcpy(common.worldToViewMatrixPrev, inputs.previous_world_to_view,
              sizeof(common.worldToViewMatrixPrev));
  common.motionVectorScale[0] = 1.0f;
  common.motionVectorScale[1] = 1.0f;
  common.motionVectorScale[2] = 1.0f;
  common.isMotionVectorInWorldSpace = true;
  common.resourceSize[0] = uint16_t(inputs.width);
  common.resourceSize[1] = uint16_t(inputs.height);
  common.resourceSizePrev[0] = uint16_t(inputs.width);
  common.resourceSizePrev[1] = uint16_t(inputs.height);
  common.rectSize[0] = uint16_t(inputs.width);
  common.rectSize[1] = uint16_t(inputs.height);
  common.rectSizePrev[0] = uint16_t(inputs.width);
  common.rectSizePrev[1] = uint16_t(inputs.height);
  common.timeDeltaBetweenFrames = inputs.frame_time_ms;
  common.denoisingRange = inputs.denoising_range;
  if (inputs.reset) {
    state.nrd_frame_index = 0;
  }
  common.frameIndex = state.nrd_frame_index++;
  common.accumulationMode =
      inputs.reset ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
  if (state.nrd_set_common_settings(*state.nrd_instance, common) != nrd::Result::SUCCESS) {
    return false;
  }
  nrd::ReblurSettings reblur = {};
  reblur.hitDistanceParameters.A = inputs.hit_distance_parameters[0];
  reblur.hitDistanceParameters.B = inputs.hit_distance_parameters[1];
  reblur.hitDistanceParameters.C = inputs.hit_distance_parameters[2];
  uint32_t frames = nrd::GetMaxAccumulatedFrameNum(
      float(std::max(REXCVAR_GET(path_tracing_nrd_accumulation_time), 0.05)),
      1000.0f / std::max(inputs.frame_time_ms, 1.0f));
  reblur.maxAccumulatedFrameNum = std::clamp(frames, 2u, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
  reblur.maxFastAccumulatedFrameNum = std::max(reblur.maxAccumulatedFrameNum / 5, 2u);
  reblur.maxBlurRadius = std::max(float(REXCVAR_GET(path_tracing_nrd_blur_radius)), 1.0f);
  reblur.enableAntiFirefly = true;
  nrd::SigmaSettings sigma = {};
  for (uint32_t i = 0; i < 3; ++i) {
    sigma.lightDirection[i] = inputs.sun_direction_world[i];
  }
  // Steadier shadow edges (one shadow ray per pixel).
  sigma.maxStabilizedFrameNum = nrd::SIGMA_MAX_HISTORY_FRAME_NUM;
  if (state.nrd_set_denoiser_settings(*state.nrd_instance, kNRDReblur, &reblur) !=
          nrd::Result::SUCCESS ||
      state.nrd_set_denoiser_settings(*state.nrd_instance, kNRDSigma, &sigma) !=
          nrd::Result::SUCCESS) {
    return false;
  }
  nrd::Identifier identifiers[] = {kNRDReblur, kNRDSigma};
  const nrd::DispatchDesc* dispatches = nullptr;
  uint32_t dispatch_count = 0;
  if (state.nrd_get_compute_dispatches(*state.nrd_instance, identifiers,
                                       uint32_t(rex::countof(identifiers)), dispatches,
                                       dispatch_count) != nrd::Result::SUCCESS ||
      dispatch_count > kNRDMaxDispatches) {
    return false;
  }

  // The dispatches.
  auto resource_for = [&](const nrd::ResourceDesc& resource) -> ID3D12Resource* {
    switch (resource.type) {
      case nrd::ResourceType::IN_MV:
        return inputs.world_motion;
      case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
        return inputs.nrd_normal_roughness;
      case nrd::ResourceType::IN_VIEWZ:
        return inputs.view_depth;
      case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
        return inputs.diffuse_signal;
      case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
        return inputs.specular_signal;
      case nrd::ResourceType::IN_PENUMBRA:
        return inputs.shadow_signal;
      case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
        return inputs.diffuse_output;
      case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
        return inputs.specular_output;
      case nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY:
        return inputs.shadow_output;
      case nrd::ResourceType::TRANSIENT_POOL:
        return resource.indexInPool < state.nrd_transient_pool.size()
                   ? state.nrd_transient_pool[resource.indexInPool].Get()
                   : nullptr;
      case nrd::ResourceType::PERMANENT_POOL:
        return resource.indexInPool < state.nrd_permanent_pool.size()
                   ? state.nrd_permanent_pool[resource.indexInPool].Get()
                   : nullptr;
      default:
        return nullptr;
    }
  };
  uint32_t upload_frame = uint32_t(frame_current_ % kQueueFrames);
  uint64_t constants_base = uint64_t(upload_frame) * kNRDConstantSlotSize * kNRDMaxDispatches;
  D3D12_GPU_VIRTUAL_ADDRESS constants_address = 0;
  deferred_command_list_.D3DSetComputeRootSignature(state.nrd_root_signature.Get());
  for (uint32_t i = 0; i < dispatch_count; ++i) {
    const nrd::DispatchDesc& dispatch = dispatches[i];
    if (dispatch.pipelineIndex >= state.nrd_pipelines.size()) {
      continue;
    }
    ui::d3d12::util::DescriptorCpuGpuHandlePair start;
    uint32_t table_size = state.nrd_textures_max + state.nrd_storage_textures_max;
    if (!PathTracingAllocateDescriptors(table_size, start)) {
      REXGPU_WARN("Path tracing: out of descriptors for NRD");
      return false;
    }
    // Null descriptors first, then the used ones.
    uint32_t srv_index = 0, uav_index = 0;
    for (uint32_t j = 0; j < dispatch.resourcesNum; ++j) {
      const nrd::ResourceDesc& resource_desc = dispatch.resources[j];
      ID3D12Resource* resource = resource_for(resource_desc);
      bool storage = resource_desc.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
      uint32_t slot = storage ? state.nrd_textures_max + uav_index++ : srv_index++;
      if (slot >= table_size || (!storage && slot >= state.nrd_textures_max)) {
        continue;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE handle = provider.OffsetViewDescriptor(start.first, slot);
      DXGI_FORMAT format = resource ? resource->GetDesc().Format : DXGI_FORMAT_R32_FLOAT;
      if (storage) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
        uav_desc.Format = format;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        PathTracingUseResource(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device->CreateUnorderedAccessView(resource, nullptr, &uav_desc, handle);
      } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        PathTracingUseResource(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        device->CreateShaderResourceView(resource, &srv_desc, handle);
      }
    }
    for (uint32_t slot = srv_index; slot < state.nrd_textures_max; ++slot) {
      D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
      srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
      srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv_desc.Texture2D.MipLevels = 1;
      device->CreateShaderResourceView(nullptr, &srv_desc,
                                       provider.OffsetViewDescriptor(start.first, slot));
    }
    for (uint32_t slot = uav_index; slot < state.nrd_storage_textures_max; ++slot) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
      uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
      uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      device->CreateUnorderedAccessView(
          nullptr, nullptr, &uav_desc,
          provider.OffsetViewDescriptor(start.first, state.nrd_textures_max + slot));
    }
    if (dispatch.constantBufferDataSize &&
        (!dispatch.constantBufferDataMatchesPreviousDispatch || !constants_address)) {
      uint64_t offset = constants_base + uint64_t(i) * kNRDConstantSlotSize;
      std::memcpy(state.nrd_constants_mapping + offset, dispatch.constantBufferData,
                  std::min(dispatch.constantBufferDataSize, kNRDConstantSlotSize));
      constants_address = state.nrd_constants->GetGPUVirtualAddress() + offset;
    }
    if (constants_address) {
      deferred_command_list_.D3DSetComputeRootConstantBufferView(0, constants_address);
    }
    deferred_command_list_.D3DSetComputeRootDescriptorTable(1, start.second);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        2, provider.OffsetViewDescriptor(start.second, state.nrd_textures_max));
    SetExternalPipeline(state.nrd_pipelines[dispatch.pipelineIndex].Get());
    SubmitBarriers();
    deferred_command_list_.D3DDispatch(dispatch.gridWidth, dispatch.gridHeight, 1);
  }
  return true;
}

bool D3D12CommandProcessor::PathTracingDenoiseDLSSRR(const PathTracingDenoiseInputs& inputs) {
  PathTracingDenoiserState& state = *pt_denoiser_state_;
  if (!state.dlss_available || state.dlss_failed) {
    return false;
  }
  bool create = !state.dlss_handle;
  if (state.dlss_handle && (state.dlss_width != inputs.width || state.dlss_height != inputs.height)) {
    // Recreated for the new size once nothing uses it.
    AwaitAllQueueOperationsCompletion();
    NVSDK_NGX_D3D12_ReleaseFeature(state.dlss_handle);
    state.dlss_handle = nullptr;
    create = true;
  }
  state.dlss_width = inputs.width;
  state.dlss_height = inputs.height;
  PathTracingDenoiserState::DLSSJob& job = state.dlss_jobs[frame_current_ % kQueueFrames];
  job.command_processor = this;
  job.state = &state;
  job.inputs = inputs;
  job.inputs.reset = inputs.reset || create;
  job.create = create;
  PathTracingUseResource(inputs.color_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  SubmitBarriers();
  deferred_command_list_.ExternalCallback(PathTracingDLSSCallback, &job);
  InvalidatePathTracingCommandListState();
  return true;
}

bool D3D12CommandProcessor::PathTracingDenoiseFSRRR(const PathTracingDenoiseInputs& inputs) {
  PathTracingDenoiserState& state = *pt_denoiser_state_;
  if (state.ffx_failed) {
    return false;
  }
  if (state.ffx_context && (state.ffx_width < inputs.width || state.ffx_height < inputs.height)) {
    AwaitAllQueueOperationsCompletion();
    state.ffx_destroy_context(&state.ffx_context, nullptr);
    state.ffx_context = nullptr;
  }
  bool reset = inputs.reset;
  if (!state.ffx_context) {
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = GetD3D12Provider().GetDevice();
    ffxCreateContextDescDenoiser create = {};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    create.header.pNext = &backend.header;
    create.version = FFX_DENOISER_VERSION;
    create.maxRenderSize = {inputs.width, inputs.height};
    create.signalFlags = FFX_DENOISER_SIGNAL_DOMINANT_LIGHT_VISIBILITY |
                         FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE |
                         FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
    ffxReturnCode_t result = state.ffx_create_context(&state.ffx_context, &create.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
      REXGPU_WARN(
          "Path tracing: FSR Ray Regeneration couldn't be created ({}) - it needs an AMD Radeon "
          "RX 9000 series GPU; using NRD",
          uint32_t(result));
      state.ffx_context = nullptr;
      state.ffx_failed = true;
      return false;
    }
    state.ffx_width = inputs.width;
    state.ffx_height = inputs.height;
    reset = true;
    REXGPU_INFO("Path tracing: FSR Ray Regeneration created");
  }
  PathTracingDenoiserState::FSRJob& job = state.fsr_jobs[frame_current_ % kQueueFrames];
  job.state = &state;
  job.inputs = inputs;
  job.inputs.reset = reset;
  PathTracingUseResource(inputs.diffuse_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  PathTracingUseResource(inputs.specular_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  PathTracingUseResource(inputs.shadow_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  SubmitBarriers();
  deferred_command_list_.ExternalCallback(PathTracingFSRCallback, &job);
  InvalidatePathTracingCommandListState();
  return true;
}

void D3D12CommandProcessor::PathTracingDLSSCallback(void* context,
                                                    ID3D12GraphicsCommandList* command_list) {
  auto& job = *static_cast<PathTracingDenoiserState::DLSSJob*>(context);
  auto& state = *job.state;
  const PathTracingDenoiseInputs& inputs = job.inputs;
  if (job.create) {
    NVSDK_NGX_DLSSD_Create_Params create = {};
    create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;
    create.InWidth = inputs.width;
    create.InHeight = inputs.height;
    create.InTargetWidth = inputs.width;
    create.InTargetHeight = inputs.height;
    create.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    create.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSSD_EXT(command_list, 1, 1, &state.dlss_handle,
                                                         state.ngx_parameters, &create);
    if (NVSDK_NGX_FAILED(result)) {
      REXGPU_ERROR("Path tracing: failed to create DLSS Ray Reconstruction ({:08X})",
                   uint32_t(result));
      state.dlss_handle = nullptr;
      state.dlss_failed = true;
      return;
    }
  }
  if (!state.dlss_handle) {
    return;
  }
  NVSDK_NGX_D3D12_DLSSD_Eval_Params evaluate = {};
  evaluate.pInDiffuseAlbedo = inputs.diffuse_albedo;
  evaluate.pInSpecularAlbedo = inputs.specular_albedo;
  evaluate.pInNormals = inputs.normal_roughness;
  evaluate.pInRoughness = inputs.normal_roughness;
  evaluate.pInColor = inputs.color;
  evaluate.pInOutput = inputs.color_output;
  evaluate.pInDepth = inputs.view_depth;
  evaluate.pInMotionVectors = inputs.screen_motion;
  // The motion vectors are in UV units.
  evaluate.InMVScaleX = float(inputs.width);
  evaluate.InMVScaleY = float(inputs.height);
  evaluate.InRenderSubrectDimensions = {inputs.width, inputs.height};
  evaluate.InReset = inputs.reset ? 1 : 0;
  // Row-major with row vectors - the same memory layout.
  evaluate.pInWorldToViewMatrix = const_cast<float*>(inputs.world_to_view);
  evaluate.pInViewToClipMatrix = const_cast<float*>(inputs.view_to_clip);
  evaluate.InFrameTimeDeltaInMsec = inputs.frame_time_ms;
  NVSDK_NGX_Result result =
      NGX_D3D12_EVALUATE_DLSSD_EXT(command_list, state.dlss_handle, state.ngx_parameters, &evaluate);
  if (NVSDK_NGX_FAILED(result)) {
    REXGPU_ERROR("Path tracing: DLSS Ray Reconstruction failed ({:08X})", uint32_t(result));
    state.dlss_failed = true;
  }
}

void D3D12CommandProcessor::PathTracingFSRCallback(void* context,
                                                   ID3D12GraphicsCommandList* command_list) {
  auto& job = *static_cast<PathTracingDenoiserState::FSRJob*>(context);
  auto& state = *job.state;
  const PathTracingDenoiseInputs& inputs = job.inputs;
  if (!state.ffx_context) {
    return;
  }
  auto read = [](ID3D12Resource* resource) {
    return ffxApiGetResourceDX12(resource, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  };
  auto write = [](ID3D12Resource* resource) {
    return ffxApiGetResourceDX12(resource, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  };
  ffxDispatchDescDenoiserIndirectSpecular specular = {};
  specular.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
  specular.signal.input = read(inputs.specular_signal);
  specular.signal.output = write(inputs.specular_output);
  ffxDispatchDescDenoiserIndirectDiffuse diffuse = {};
  diffuse.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
  diffuse.header.pNext = &specular.header;
  diffuse.signal.input = read(inputs.diffuse_signal);
  diffuse.signal.output = write(inputs.diffuse_output);
  ffxDispatchDescDenoiserDominantLight light = {};
  light.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DOMINANT_LIGHT;
  light.header.pNext = &diffuse.header;
  light.signal.input = read(inputs.shadow_signal);
  light.signal.output = write(inputs.shadow_output);
  // From the light to the target.
  light.direction = {-inputs.sun_direction_world[0], -inputs.sun_direction_world[1],
                     -inputs.sun_direction_world[2]};
  light.emission = {inputs.sun_color[0], inputs.sun_color[1], inputs.sun_color[2]};
  light.angularRadius = std::atan(inputs.sun_tan_angular_radius);
  ffxDispatchDescDenoiser dispatch = {};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
  dispatch.header.pNext = &light.header;
  dispatch.commandList = command_list;
  dispatch.linearDepth = read(inputs.view_depth);
  dispatch.motionVectors = read(inputs.screen_motion);
  dispatch.normals = read(inputs.octahedral_normal);
  dispatch.specularAlbedo = read(inputs.specular_albedo);
  dispatch.diffuseAlbedo = read(inputs.diffuse_albedo);
  dispatch.motionVectorScale = {1.0f, 1.0f, 1.0f};
  dispatch.jitterOffsets = {0.0f, 0.0f};
  dispatch.cameraPositionDelta = {inputs.camera_position_delta[0], inputs.camera_position_delta[1],
                                  inputs.camera_position_delta[2]};
  // Row-major with row vectors - the same memory layout.
  std::memcpy(&dispatch.view, inputs.world_to_view, sizeof(dispatch.view));
  std::memcpy(&dispatch.projection, inputs.view_to_clip, sizeof(dispatch.projection));
  dispatch.linearDepthBounds = {0.0f, inputs.denoising_range};
  dispatch.renderSize = {inputs.width, inputs.height};
  dispatch.frameIndex = inputs.frame_index;
  dispatch.flags =
      FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO | (inputs.reset ? FFX_DENOISER_DISPATCH_RESET : 0u);
  ffxReturnCode_t result = state.ffx_dispatch(&state.ffx_context, &dispatch.header);
  if (result != FFX_API_RETURN_OK) {
    REXGPU_ERROR("Path tracing: FSR Ray Regeneration failed ({})", uint32_t(result));
    state.ffx_failed = true;
  }
}

void D3D12CommandProcessor::ShutdownPathTracingDenoisers() {
  if (!pt_denoiser_state_) {
    return;
  }
  PathTracingDenoiserState& state = *pt_denoiser_state_;
  AwaitAllQueueOperationsCompletion();
  if (state.nrd_instance) {
    state.nrd_destroy_instance(*state.nrd_instance);
    state.nrd_instance = nullptr;
  }
  if (state.nrd_constants_mapping) {
    state.nrd_constants->Unmap(0, nullptr);
    state.nrd_constants_mapping = nullptr;
  }
  if (state.nrd_module) {
    FreeLibrary(state.nrd_module);
  }
  if (state.dlss_handle) {
    NVSDK_NGX_D3D12_ReleaseFeature(state.dlss_handle);
  }
  if (state.ngx_parameters) {
    NVSDK_NGX_D3D12_DestroyParameters(state.ngx_parameters);
  }
  if (state.ngx_initialized) {
    NVSDK_NGX_D3D12_Shutdown1(GetD3D12Provider().GetDevice());
  }
  if (state.ffx_context) {
    state.ffx_destroy_context(&state.ffx_context, nullptr);
  }
  if (state.ffx_module) {
    FreeLibrary(state.ffx_module);
  }
  delete pt_denoiser_state_;
  pt_denoiser_state_ = nullptr;
}

}  // namespace rex::graphics::d3d12
