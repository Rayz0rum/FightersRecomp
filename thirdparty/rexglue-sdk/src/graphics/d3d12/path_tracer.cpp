/**
 * @file        graphics/d3d12/path_tracer.cpp
 * @brief       Experimental path-traced lighting for the D3D12 backend.
 *
 * The guest's scene triangles are captured with stream output (host clip space
 * positions) while it draws the frame. At swap time they're converted back to
 * view space, built into a DirectX Raytracing acceleration structure, and
 * traced with inline ray queries: a primary ray per pixel finds the surface,
 * which is then lit by a sun with ray-traced soft shadows and by global
 * illumination - rays escaping to the sky (colored like the frame's
 * background) or bouncing off other surfaces, themselves lit by the sun (with
 * their own shadow rays) and the sky, with their colors taken from the frame.
 *
 * The frame's colors serve as albedo: the lighting is traced as a ratio to
 * that of open ground and multiplied into the frame, replacing the flat
 * shading of the game (up to path_tracing_strength), after edge-aware
 * denoising and with auto exposure.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_INT32(path_tracing_projection_constant, -1, "GPU/Path Tracing",
                     "Vertex shader float constant with the scene projection's x scale in .x, "
                     "followed by the one with the y scale in .y (-1 to derive the projection "
                     "from path_tracing_vertical_fov and the viewport aspect ratio)");
REXCVAR_DEFINE_DOUBLE(path_tracing_vertical_fov, 68.9, "GPU/Path Tracing",
                      "Vertical field of view of the scene in degrees, if the projection isn't "
                      "taken from the shader constants");
REXCVAR_DEFINE_DOUBLE(path_tracing_strength, 1.0, "GPU/Path Tracing",
                      "How much of the game's own lighting the traced lighting replaces (0 to 1)");
REXCVAR_DEFINE_INT32(path_tracing_rays, 4, "GPU/Path Tracing",
                     "Global illumination rays per pixel (1 to 16)");
REXCVAR_DEFINE_DOUBLE(path_tracing_gi_distance, 100.0, "GPU/Path Tracing",
                      "Maximum distance of global illumination rays in view space units");
REXCVAR_DEFINE_DOUBLE(path_tracing_bounce, 1.0, "GPU/Path Tracing",
                      "Strength of the light bounced off surfaces");
REXCVAR_DEFINE_DOUBLE(path_tracing_sky, 1.0, "GPU/Path Tracing",
                      "Strength of the sky light (colored like the frame's background)");
REXCVAR_DEFINE_DOUBLE(path_tracing_sky_saturation, 0.25, "GPU/Path Tracing",
                      "How much of the background's color the sky light keeps (0 - neutral, 1 - "
                      "all)");
REXCVAR_DEFINE_BOOL(path_tracing_replace_game_shadows, false, "GPU/Path Tracing",
                    "Fill in the game's own black shadows on the ground, as the traced sun casts "
                    "shadows instead");
REXCVAR_DEFINE_DOUBLE(path_tracing_ambient, 0.05, "GPU/Path Tracing",
                      "Minimum sky light, for dark backgrounds");
REXCVAR_DEFINE_DOUBLE(path_tracing_sun_intensity, 1.0, "GPU/Path Tracing",
                      "Sun brightness relative to the sky");
REXCVAR_DEFINE_DOUBLE(path_tracing_sun_elevation, 50.0, "GPU/Path Tracing",
                      "Sun elevation above the ground in degrees");
REXCVAR_DEFINE_DOUBLE(path_tracing_sun_azimuth, 180.0, "GPU/Path Tracing",
                      "Sun direction around the vertical axis in degrees relative to the view, 0 "
                      "behind the camera, 180 facing it");
REXCVAR_DEFINE_DOUBLE(path_tracing_sun_softness, 0.04, "GPU/Path Tracing",
                      "Sun size (shadow penumbra), as the tangent of its angular radius");
REXCVAR_DEFINE_DOUBLE(path_tracing_max_distance, 120.0, "GPU/Path Tracing",
                      "Surfaces further than this in view space units (sky, clouds, distant "
                      "scenery) keep their original look");
REXCVAR_DEFINE_DOUBLE(path_tracing_shadow_distance, 60.0, "GPU/Path Tracing",
                      "Maximum distance of sun shadow casters");
REXCVAR_DEFINE_DOUBLE(path_tracing_exposure, 0.9, "GPU/Path Tracing",
                      "Average brightness of the lit scene relative to the original (auto "
                      "exposure target)");
REXCVAR_DEFINE_INT32(path_tracing_debug_view, 0, "GPU/Path Tracing",
                     "0 - off, 1 - lighting, 2 - normals, 3 - traced silhouettes and HUD mask, 4 "
                     "- split screen comparison, 5 - surface colors");
REXCVAR_DEFINE_BOOL(path_tracing_debug_log, false, "GPU/Path Tracing",
                    "Periodically log path tracing statistics");

namespace rex::graphics::d3d12 {

// Generated with src/graphics/shaders/path_tracing/build_pt_shaders.cmd.
namespace shaders {
#include "../shaders/bytecode/d3d12_6_5/pt_albedo_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_composite_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_convert_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_denoise_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_lighting_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_primary_cs.h"
}  // namespace shaders

namespace {

// Must match PTConstants in pt_common.hlsli.
struct PathTracingConstants {
  float inv_proj[2];
  float ndc_scale[2];
  float ndc_offset[2];
  float viewport_offset[2];
  float viewport_extent[2];
  uint32_t rect_min[2];
  uint32_t rect_max[2];
  uint32_t output_size[2];
  uint32_t vertex_count;
  uint32_t stats_slot;
  float gi_distance;
  float strength;
  float sun_color[3];
  float sky_scale;
  float sun_angles[2];
  float sun_softness;
  uint32_t ray_count;
  uint32_t debug_view;
  uint32_t frame;
  uint32_t filter_step;
  float bounce_scale;
  float ambient;
  float exposure_target;
  float shadow_distance;
  uint32_t filter_radius;
  uint32_t flags;
  float sky_saturation;
  float max_distance;
  float padding;
};
static_assert(sizeof(PathTracingConstants) == 40 * sizeof(uint32_t));

constexpr DXGI_FORMAT kGBufferFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr DXGI_FORMAT kLightingFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

constexpr float kDegreesToRadians = 0.017453292519943295f;

void GetBlasInputs(D3D12_RAYTRACING_GEOMETRY_DESC& geometry,
                   D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
                   D3D12_GPU_VIRTUAL_ADDRESS vertices, uint32_t vertex_count) {
  geometry = {};
  geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
  geometry.Triangles.VertexCount = vertex_count;
  geometry.Triangles.VertexBuffer.StartAddress = vertices;
  geometry.Triangles.VertexBuffer.StrideInBytes = 12;
  inputs = {};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  inputs.NumDescs = 1;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.pGeometryDescs = &geometry;
}

void GetTlasInputs(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
                   D3D12_GPU_VIRTUAL_ADDRESS instances) {
  inputs = {};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  inputs.NumDescs = 1;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.InstanceDescs = instances;
}

}  // namespace

bool D3D12CommandProcessor::InitializePathTracing() {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  Microsoft::WRL::ComPtr<ID3D12Device5> device5;
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
  D3D12_FEATURE_DATA_SHADER_MODEL shader_model = {D3D_SHADER_MODEL_6_5};
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) ||
      FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                         sizeof(options5))) ||
      options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 ||
      FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model,
                                         sizeof(shader_model))) ||
      shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_5) {
    REXGPU_WARN("Path tracing needs DirectX Raytracing tier 1.1 and shader model 6.5");
    return false;
  }

  D3D12_HEAP_FLAGS heap_flags = provider.GetHeapFlagCreateNotZeroed();
  auto create_buffer = [&](uint64_t size, D3D12_RESOURCE_FLAGS flags,
                           const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS buffer_heap_flags,
                           D3D12_RESOURCE_STATES state,
                           Microsoft::WRL::ComPtr<ID3D12Resource>& resource) {
    D3D12_RESOURCE_DESC desc;
    ui::d3d12::util::FillBufferResourceDesc(desc, size, flags);
    return SUCCEEDED(device->CreateCommittedResource(&heap, buffer_heap_flags, &desc, state,
                                                     nullptr, IID_PPV_ARGS(&resource)));
  };

  // Acceleration structure sizes for the largest capture.
  D3D12_RAYTRACING_GEOMETRY_DESC geometry;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
  GetBlasInputs(geometry, inputs, 0, kPathTracingMaxTriangles * 3);
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info = {};
  device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &blas_info);
  GetTlasInputs(inputs, 0);
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info = {};
  device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &tlas_info);
  uint64_t blas_scratch_size = rex::align(
      blas_info.ScratchDataSizeInBytes,
      uint64_t(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT));
  pt_tlas_scratch_offset_ = blas_scratch_size;

  const auto& heap_default = ui::d3d12::util::kHeapPropertiesDefault;
  const auto& heap_upload = ui::d3d12::util::kHeapPropertiesUpload;
  const auto& heap_readback = ui::d3d12::util::kHeapPropertiesReadback;
  constexpr auto kUAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (!create_buffer(kPathTracingCaptureSize, D3D12_RESOURCE_FLAG_NONE, heap_default,
                     heap_flags, D3D12_RESOURCE_STATE_STREAM_OUT, pt_capture_buffer_) ||
      !create_buffer(256, D3D12_RESOURCE_FLAG_NONE, heap_default, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_STREAM_OUT, pt_capture_counter_) ||
      !create_buffer(256, D3D12_RESOURCE_FLAG_NONE, heap_upload, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_GENERIC_READ, pt_zero_upload_) ||
      !create_buffer(256, D3D12_RESOURCE_FLAG_NONE, heap_readback, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_COPY_DEST, pt_count_readback_) ||
      !create_buffer(kPathTracingVertexBufferSize, kUAV, heap_default, heap_flags,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pt_vertex_buffer_) ||
      // Zeroed - the first frame reads the statistics without clearing them.
      !create_buffer(256, kUAV, heap_default, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pt_stats_buffer_) ||
      !create_buffer(blas_info.ResultDataMaxSizeInBytes, kUAV, heap_default, heap_flags,
                     D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, pt_blas_) ||
      !create_buffer(tlas_info.ResultDataMaxSizeInBytes, kUAV, heap_default, heap_flags,
                     D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, pt_tlas_) ||
      !create_buffer(blas_scratch_size + tlas_info.ScratchDataSizeInBytes, kUAV, heap_default,
                     heap_flags, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pt_scratch_) ||
      !create_buffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RESOURCE_FLAG_NONE,
                     heap_upload, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                     pt_instance_upload_)) {
    REXGPU_ERROR("Path tracing: failed to create the buffers");
    ShutdownPathTracing();
    return false;
  }

  D3D12_RANGE no_read = {};
  void* mapping;
  if (FAILED(pt_zero_upload_->Map(0, &no_read, &mapping))) {
    ShutdownPathTracing();
    return false;
  }
  std::memset(mapping, 0, 256);
  pt_zero_upload_->Unmap(0, nullptr);

  if (FAILED(pt_instance_upload_->Map(0, &no_read, &mapping))) {
    ShutdownPathTracing();
    return false;
  }
  D3D12_RAYTRACING_INSTANCE_DESC instance = {};
  instance.Transform[0][0] = 1.0f;
  instance.Transform[1][1] = 1.0f;
  instance.Transform[2][2] = 1.0f;
  instance.InstanceMask = 0xFF;
  instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
  instance.AccelerationStructure = pt_blas_->GetGPUVirtualAddress();
  std::memcpy(mapping, &instance, sizeof(instance));
  pt_instance_upload_->Unmap(0, nullptr);

  // Kept mapped, read when the copies are known to be complete.
  D3D12_RANGE read_range = {0, kPathTracingCountReadbackSlots * sizeof(uint64_t)};
  if (FAILED(pt_count_readback_->Map(0, &read_range, &mapping))) {
    ShutdownPathTracing();
    return false;
  }
  pt_count_readback_mapping_ = static_cast<const uint64_t*>(mapping);
  std::fill(std::begin(pt_count_readback_submissions_), std::end(pt_count_readback_submissions_),
            uint64_t(0));
  // Until the first counts come back.
  pt_recent_triangles_ = 16384;

  // Root signature shared by all the passes.
  D3D12_ROOT_PARAMETER parameters[size_t(PathTracingRootParameter::kCount)];
  D3D12_DESCRIPTOR_RANGE ranges[7];
  for (UINT i = 0; i < UINT(PathTracingRootParameter::kCount); ++i) {
    D3D12_ROOT_PARAMETER& parameter = parameters[i];
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    switch (PathTracingRootParameter(i)) {
      case PathTracingRootParameter::kConstants:
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.ShaderRegister = 0;
        parameter.Constants.RegisterSpace = 0;
        parameter.Constants.Num32BitValues = sizeof(PathTracingConstants) / sizeof(uint32_t);
        break;
      case PathTracingRootParameter::kBuffer0:
      case PathTracingRootParameter::kBuffer1:
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor.ShaderRegister = i - UINT(PathTracingRootParameter::kBuffer0);
        parameter.Descriptor.RegisterSpace = 0;
        break;
      case PathTracingRootParameter::kRWBuffer0:
      case PathTracingRootParameter::kRWBuffer1:
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor.ShaderRegister = i - UINT(PathTracingRootParameter::kRWBuffer0);
        parameter.Descriptor.RegisterSpace = 0;
        break;
      default: {
        // t2...t6, u2...u3.
        UINT table_index = i - UINT(PathTracingRootParameter::kTexture0);
        bool uav = i >= UINT(PathTracingRootParameter::kRWTexture0);
        D3D12_DESCRIPTOR_RANGE& range = ranges[table_index];
        range.RangeType = uav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister =
            uav ? 2 + (i - UINT(PathTracingRootParameter::kRWTexture0)) : 2 + table_index;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
      } break;
    }
  }
  D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
  root_signature_desc.NumParameters = UINT(PathTracingRootParameter::kCount);
  root_signature_desc.pParameters = parameters;
  root_signature_desc.NumStaticSamplers = 0;
  root_signature_desc.pStaticSamplers = nullptr;
  root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(pt_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, root_signature_desc);
  if (!pt_root_signature_) {
    REXGPU_ERROR("Path tracing: failed to create the root signature");
    ShutdownPathTracing();
    return false;
  }
  struct PipelineInfo {
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline;
    const void* shader;
    size_t shader_size;
  };
  PipelineInfo pipelines[] = {
      {pt_convert_pipeline_, shaders::pt_convert_cs, sizeof(shaders::pt_convert_cs)},
      {pt_primary_pipeline_, shaders::pt_primary_cs, sizeof(shaders::pt_primary_cs)},
      {pt_albedo_pipeline_, shaders::pt_albedo_cs, sizeof(shaders::pt_albedo_cs)},
      {pt_lighting_pipeline_, shaders::pt_lighting_cs, sizeof(shaders::pt_lighting_cs)},
      {pt_denoise_pipeline_, shaders::pt_denoise_cs, sizeof(shaders::pt_denoise_cs)},
      {pt_composite_pipeline_, shaders::pt_composite_cs, sizeof(shaders::pt_composite_cs)},
  };
  for (PipelineInfo& pipeline_info : pipelines) {
    *(pipeline_info.pipeline.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
        device, pipeline_info.shader, pipeline_info.shader_size, pt_root_signature_.Get());
    if (!pipeline_info.pipeline) {
      REXGPU_ERROR("Path tracing: failed to create the compute pipelines");
      ShutdownPathTracing();
      return false;
    }
  }

  REXGPU_INFO("Path tracing: enabled, up to {} triangles ({} MB acceleration structure)",
              kPathTracingMaxTriangles, (blas_info.ResultDataMaxSizeInBytes + 0xFFFFF) >> 20);
  return true;
}

void D3D12CommandProcessor::ShutdownPathTracing() {
  pt_retired_textures_.clear();
  pt_output_.Reset();
  pt_lighting_temp_.Reset();
  pt_lighting_.Reset();
  pt_albedo_.Reset();
  pt_gbuffer_.Reset();
  pt_texture_width_ = 0;
  pt_texture_height_ = 0;
  pt_composite_pipeline_.Reset();
  pt_denoise_pipeline_.Reset();
  pt_lighting_pipeline_.Reset();
  pt_albedo_pipeline_.Reset();
  pt_primary_pipeline_.Reset();
  pt_convert_pipeline_.Reset();
  pt_root_signature_.Reset();
  pt_instance_upload_.Reset();
  pt_scratch_.Reset();
  pt_tlas_.Reset();
  pt_blas_.Reset();
  pt_stats_buffer_.Reset();
  pt_vertex_buffer_.Reset();
  if (pt_count_readback_mapping_) {
    D3D12_RANGE no_write = {};
    pt_count_readback_->Unmap(0, &no_write);
    pt_count_readback_mapping_ = nullptr;
  }
  pt_count_readback_.Reset();
  pt_zero_upload_.Reset();
  pt_capture_counter_.Reset();
  pt_capture_buffer_.Reset();
}

void D3D12CommandProcessor::UpdatePathTracingCapture(
    bool depth_tested_scene_draw, const draw_util::ViewportInfo& viewport_info) {
  if (!depth_tested_scene_draw || pt_capture_done_this_frame_) {
    // Leave the target bound, rebinding it later doesn't reliably keep
    // appending on all drivers.
    return;
  }
  if (!pt_captured_this_frame_) {
    pt_captured_this_frame_ = true;
    pt_viewport_ = viewport_info;
    float scale_x = 0.0f, scale_y = 0.0f;
    int32_t constant = REXCVAR_GET(path_tracing_projection_constant);
    if (constant >= 0 && constant < 255) {
      const RegisterFile& regs = *register_file_;
      std::memcpy(&scale_x, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * constant],
                  sizeof(float));
      std::memcpy(&scale_y, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * constant + 5],
                  sizeof(float));
    }
    if (!(std::abs(scale_x) > 1.0e-4f && std::abs(scale_x) < 1.0e4f &&
          std::abs(scale_y) > 1.0e-4f && std::abs(scale_y) < 1.0e4f)) {
      // Symmetric projection filling the viewport.
      float fov = float(std::clamp(REXCVAR_GET(path_tracing_vertical_fov), 1.0, 179.0));
      scale_y = 1.0f / std::tan(fov * kDegreesToRadians * 0.5f);
      float aspect = (float(viewport_info.xy_extent[0]) * std::abs(viewport_info.ndc_scale[0])) /
                     std::max(float(viewport_info.xy_extent[1]) *
                                  std::abs(viewport_info.ndc_scale[1]),
                              1.0f);
      scale_x = scale_y / std::max(aspect, 1.0e-3f);
    }
    pt_projection_[0] = scale_x;
    pt_projection_[1] = scale_y;
  } else if (viewport_info.xy_offset[0] != pt_viewport_.xy_offset[0] ||
             viewport_info.xy_offset[1] != pt_viewport_.xy_offset[1] ||
             viewport_info.xy_extent[0] != pt_viewport_.xy_extent[0] ||
             viewport_info.xy_extent[1] != pt_viewport_.xy_extent[1]) {
    // Some other pass (such as a render to texture) with a different mapping.
    return;
  }
  // Rebind before every captured draw: the append offset is only reliably
  // taken from BufferFilledSizeLocation when the target is set.
  D3D12_STREAM_OUTPUT_BUFFER_VIEW view;
  view.BufferLocation = pt_capture_buffer_->GetGPUVirtualAddress();
  view.SizeInBytes = kPathTracingCaptureSize;
  view.BufferFilledSizeLocation = pt_capture_counter_->GetGPUVirtualAddress();
  deferred_command_list_.D3DSOSetTarget(&view);
  pt_capture_bound_ = true;
}

bool D3D12CommandProcessor::EnsurePathTracingTextures(uint32_t width, uint32_t height) {
  if (pt_gbuffer_ && width <= pt_texture_width_ && height <= pt_texture_height_) {
    return true;
  }
  for (Microsoft::WRL::ComPtr<ID3D12Resource>* texture :
       {std::addressof(pt_gbuffer_), std::addressof(pt_albedo_), std::addressof(pt_lighting_),
        std::addressof(pt_lighting_temp_), std::addressof(pt_output_)}) {
    if (*texture) {
      pt_retired_textures_.emplace_back(submission_current_, std::move(*texture));
    }
  }
  uint32_t new_width = std::max(width, pt_texture_width_);
  uint32_t new_height = std::max(height, pt_texture_height_);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = new_width;
  desc.Height = new_height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  std::pair<Microsoft::WRL::ComPtr<ID3D12Resource>*, DXGI_FORMAT> textures[] = {
      {std::addressof(pt_gbuffer_), kGBufferFormat},
      {std::addressof(pt_albedo_), kLightingFormat},
      {std::addressof(pt_lighting_), kLightingFormat},
      {std::addressof(pt_lighting_temp_), kLightingFormat},
      {std::addressof(pt_output_), kOutputFormat},
  };
  for (auto& texture : textures) {
    desc.Format = texture.second;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(texture.first->ReleaseAndGetAddressOf())))) {
      REXGPU_ERROR("Path tracing: failed to create {}x{} textures", new_width, new_height);
      pt_gbuffer_.Reset();
      pt_albedo_.Reset();
      pt_lighting_.Reset();
      pt_lighting_temp_.Reset();
      pt_output_.Reset();
      pt_texture_width_ = 0;
      pt_texture_height_ = 0;
      return false;
    }
  }
  pt_texture_width_ = new_width;
  pt_texture_height_ = new_height;
  return true;
}

ID3D12Resource* D3D12CommandProcessor::PathTracingRender(
    ID3D12Resource* swap_texture, const D3D12_SHADER_RESOURCE_VIEW_DESC& swap_texture_srv_desc,
    uint32_t width, uint32_t height, D3D12_SHADER_RESOURCE_VIEW_DESC& srv_desc_out) {
  // The capture buffer is about to be read.
  if (pt_capture_bound_) {
    deferred_command_list_.D3DSOSetTarget(nullptr);
    pt_capture_bound_ = false;
  }
  if (!pt_root_signature_ || !pt_captured_this_frame_ || !width || !height) {
    return nullptr;
  }

  // Output pixels covered by the scene viewport. Skip frames where the 3D
  // scene is only a small part of the image (menus drawing models into
  // textures and such).
  const draw_util::ViewportInfo& viewport = pt_viewport_;
  uint32_t rect_min[2], rect_max[2];
  uint32_t output_size[2] = {width, height};
  for (uint32_t i = 0; i < 2; ++i) {
    rect_min[i] = std::min(viewport.xy_offset[i], output_size[i]);
    rect_max[i] = std::min(viewport.xy_offset[i] + viewport.xy_extent[i], output_size[i]);
  }
  uint64_t rect_area = uint64_t(rect_max[0] - rect_min[0]) * (rect_max[1] - rect_min[1]);
  if (rect_area * 4 < uint64_t(width) * height || !viewport.ndc_scale[0] ||
      !viewport.ndc_scale[1]) {
    return nullptr;
  }
  if (!EnsurePathTracingTextures(width, height)) {
    return nullptr;
  }

  // Size the build for recent frames with some margin - triangles beyond the
  // estimate are dropped until the estimate catches up.
  uint32_t build_triangles = std::min(
      kPathTracingMaxTriangles,
      rex::align(pt_recent_triangles_ + pt_recent_triangles_ / 4 + 1024, uint32_t(1024)));

  // The scene before the HUD was drawn over it, if found, to take the surface
  // colors from and to apply the lighting to only where the HUD isn't.
  ID3D12Resource* scene_texture = swap_texture;
  D3D12_SHADER_RESOURCE_VIEW_DESC scene_srv_desc = swap_texture_srv_desc;
  if (pt_scene_fetch_valid_) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    xenos::TextureFormat format;
    ID3D12Resource* texture =
        texture_cache_->RequestTexture(pt_scene_fetch_, srv_desc, format);
    if (texture) {
      D3D12_RESOURCE_DESC desc = texture->GetDesc();
      if (desc.Width >= rect_max[0] && desc.Height >= rect_max[1]) {
        scene_texture = texture;
        scene_srv_desc = srv_desc;
      }
    }
  }

  // All descriptors at once, so they're in the same heap.
  enum Descriptor {
    kFrameSRV,
    kColorSRV,
    kGBufferSRV,
    kGBufferUAV,
    kAlbedoSRV,
    kAlbedoUAV,
    kLightingSRV,
    kLightingUAV,
    kLightingTempSRV,
    kLightingTempUAV,
    kOutputUAV,
    kDescriptorCount,
  };
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[kDescriptorCount];
  if (!RequestOneUseSingleViewDescriptors(kDescriptorCount, descriptors)) {
    return nullptr;
  }
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  device->CreateShaderResourceView(swap_texture, &swap_texture_srv_desc,
                                   descriptors[kFrameSRV].first);
  device->CreateShaderResourceView(scene_texture, &scene_srv_desc, descriptors[kColorSRV].first);
  auto create_srv = [&](ID3D12Resource* texture, DXGI_FORMAT format, Descriptor descriptor) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture, &srv_desc, descriptors[descriptor].first);
  };
  auto create_uav = [&](ID3D12Resource* texture, DXGI_FORMAT format, Descriptor descriptor) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = format;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(texture, nullptr, &uav_desc, descriptors[descriptor].first);
  };
  create_srv(pt_gbuffer_.Get(), kGBufferFormat, kGBufferSRV);
  create_uav(pt_gbuffer_.Get(), kGBufferFormat, kGBufferUAV);
  create_srv(pt_albedo_.Get(), kLightingFormat, kAlbedoSRV);
  create_uav(pt_albedo_.Get(), kLightingFormat, kAlbedoUAV);
  create_srv(pt_lighting_.Get(), kLightingFormat, kLightingSRV);
  create_uav(pt_lighting_.Get(), kLightingFormat, kLightingUAV);
  create_srv(pt_lighting_temp_.Get(), kLightingFormat, kLightingTempSRV);
  create_uav(pt_lighting_temp_.Get(), kLightingFormat, kLightingTempUAV);
  create_uav(pt_output_.Get(), kOutputFormat, kOutputUAV);

  PathTracingConstants constants = {};
  constants.inv_proj[0] = 1.0f / pt_projection_[0];
  constants.inv_proj[1] = 1.0f / pt_projection_[1];
  for (uint32_t i = 0; i < 2; ++i) {
    constants.ndc_scale[i] = viewport.ndc_scale[i];
    constants.ndc_offset[i] = viewport.ndc_offset[i];
    constants.viewport_offset[i] = float(viewport.xy_offset[i]);
    constants.viewport_extent[i] = float(std::max(viewport.xy_extent[i], uint32_t(1)));
    constants.rect_min[i] = rect_min[i];
    constants.rect_max[i] = rect_max[i];
    constants.output_size[i] = output_size[i];
  }
  constants.vertex_count = build_triangles * 3;
  constants.stats_slot = pt_frame_ & 1;
  constants.gi_distance = std::max(float(REXCVAR_GET(path_tracing_gi_distance)), 1.0e-3f);
  constants.strength = std::clamp(float(REXCVAR_GET(path_tracing_strength)), 0.0f, 1.0f);
  float sun_intensity = std::max(float(REXCVAR_GET(path_tracing_sun_intensity)), 0.0f);
  // Slightly warm sunlight.
  constants.sun_color[0] = sun_intensity * 1.0f;
  constants.sun_color[1] = sun_intensity * 0.95f;
  constants.sun_color[2] = sun_intensity * 0.86f;
  constants.sky_scale = std::max(float(REXCVAR_GET(path_tracing_sky)), 0.0f);
  constants.sun_angles[0] = float(REXCVAR_GET(path_tracing_sun_elevation)) * kDegreesToRadians;
  constants.sun_angles[1] = float(REXCVAR_GET(path_tracing_sun_azimuth)) * kDegreesToRadians;
  constants.sun_softness = std::max(float(REXCVAR_GET(path_tracing_sun_softness)), 0.0f);
  constants.ray_count = uint32_t(std::clamp(REXCVAR_GET(path_tracing_rays), 1, 16));
  constants.bounce_scale = std::max(float(REXCVAR_GET(path_tracing_bounce)), 0.0f);
  constants.ambient = std::max(float(REXCVAR_GET(path_tracing_ambient)), 0.0f);
  constants.exposure_target = std::max(float(REXCVAR_GET(path_tracing_exposure)), 0.01f);
  constants.shadow_distance =
      std::max(float(REXCVAR_GET(path_tracing_shadow_distance)), 1.0e-3f);
  constants.flags = REXCVAR_GET(path_tracing_replace_game_shadows) ? 1 : 0;
  constants.sky_saturation =
      std::clamp(float(REXCVAR_GET(path_tracing_sky_saturation)), 0.0f, 1.0f);
  constants.max_distance = std::max(float(REXCVAR_GET(path_tracing_max_distance)), 1.0e-2f);
  constants.debug_view = uint32_t(std::clamp(REXCVAR_GET(path_tracing_debug_view), 0, 5));
  constants.frame = pt_frame_;

  deferred_command_list_.D3DSetComputeRootSignature(pt_root_signature_.Get());
  deferred_command_list_.D3DSetComputeRoot32BitConstants(
      UINT(PathTracingRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t),
      &constants, 0);
  D3D12_GPU_VIRTUAL_ADDRESS vertices = pt_vertex_buffer_->GetGPUVirtualAddress();
  D3D12_GPU_VIRTUAL_ADDRESS tlas = pt_tlas_->GetGPUVirtualAddress();

  // Captured triangles to view space.
  PushTransitionBarrier(pt_capture_buffer_.Get(), D3D12_RESOURCE_STATE_STREAM_OUT,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  PushTransitionBarrier(pt_capture_counter_.Get(), D3D12_RESOURCE_STATE_STREAM_OUT,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  PushUAVBarrier(pt_vertex_buffer_.Get());
  PushUAVBarrier(pt_stats_buffer_.Get());
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kBuffer0), pt_capture_buffer_->GetGPUVirtualAddress());
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kBuffer1), pt_capture_counter_->GetGPUVirtualAddress());
  deferred_command_list_.D3DSetComputeRootUnorderedAccessView(
      UINT(PathTracingRootParameter::kRWBuffer0), vertices);
  deferred_command_list_.D3DSetComputeRootUnorderedAccessView(
      UINT(PathTracingRootParameter::kRWBuffer1), pt_stats_buffer_->GetGPUVirtualAddress());
  SetExternalPipeline(pt_convert_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch((build_triangles + 63) / 64, 1, 1);
  PushTransitionBarrier(pt_capture_buffer_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_STREAM_OUT);
  PushTransitionBarrier(pt_capture_counter_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_STREAM_OUT);
  PushTransitionBarrier(pt_vertex_buffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  PushUAVBarrier(pt_stats_buffer_.Get());
  // The previous frame's traversal and builds must be done.
  PushUAVBarrier(pt_blas_.Get());
  PushUAVBarrier(pt_tlas_.Get());
  PushUAVBarrier(pt_scratch_.Get());
  SubmitBarriers();

  // Acceleration structures.
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
  D3D12_RAYTRACING_GEOMETRY_DESC geometry;
  GetBlasInputs(geometry, build.Inputs, vertices, build_triangles * 3);
  build.DestAccelerationStructureData = pt_blas_->GetGPUVirtualAddress();
  build.ScratchAccelerationStructureData = pt_scratch_->GetGPUVirtualAddress();
  deferred_command_list_.D3DBuildRaytracingAccelerationStructure(build);
  PushUAVBarrier(pt_blas_.Get());
  SubmitBarriers();
  build = {};
  GetTlasInputs(build.Inputs, pt_instance_upload_->GetGPUVirtualAddress());
  build.DestAccelerationStructureData = tlas;
  build.ScratchAccelerationStructureData =
      pt_scratch_->GetGPUVirtualAddress() + pt_tlas_scratch_offset_;
  deferred_command_list_.D3DBuildRaytracingAccelerationStructure(build);
  PushUAVBarrier(pt_tlas_.Get());

  auto set_constants = [&]() {
    deferred_command_list_.D3DSetComputeRoot32BitConstants(
        UINT(PathTracingRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t),
        &constants, 0);
  };
  auto set_table = [&](PathTracingRootParameter parameter, Descriptor descriptor) {
    deferred_command_list_.D3DSetComputeRootDescriptorTable(UINT(parameter),
                                                            descriptors[descriptor].second);
  };
  constexpr auto kNPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto kUAVState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint32_t rect_groups_x = (rect_max[0] - rect_min[0] + 7) / 8;
  uint32_t rect_groups_y = (rect_max[1] - rect_min[1] + 7) / 8;

  // Primary surfaces and background statistics.
  PushTransitionBarrier(pt_gbuffer_.Get(), kNPSR, kUAVState);
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kBuffer0), tlas);
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kBuffer1), vertices);
  set_table(PathTracingRootParameter::kTexture0, kColorSRV);
  set_table(PathTracingRootParameter::kRWTexture0, kGBufferUAV);
  SetExternalPipeline(pt_primary_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Surface colors.
  PushTransitionBarrier(pt_gbuffer_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_albedo_.Get(), kNPSR, kUAVState);
  PushUAVBarrier(pt_stats_buffer_.Get());
  set_table(PathTracingRootParameter::kTexture1, kGBufferSRV);
  set_table(PathTracingRootParameter::kRWTexture0, kAlbedoUAV);
  SetExternalPipeline(pt_albedo_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Lighting, with the surface colors for the bounces.
  PushTransitionBarrier(pt_albedo_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_lighting_.Get(), kNPSR, kUAVState);
  set_table(PathTracingRootParameter::kTexture0, kAlbedoSRV);
  set_table(PathTracingRootParameter::kRWTexture0, kLightingUAV);
  SetExternalPipeline(pt_lighting_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Denoising: densely over the 4x4 sampling pattern, then wider.
  PushTransitionBarrier(pt_lighting_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_lighting_temp_.Get(), kNPSR, kUAVState);
  constants.filter_radius = 3;
  constants.filter_step = 1;
  set_constants();
  set_table(PathTracingRootParameter::kTexture2, kLightingSRV);
  set_table(PathTracingRootParameter::kRWTexture0, kLightingTempUAV);
  SetExternalPipeline(pt_denoise_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);
  PushTransitionBarrier(pt_lighting_temp_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_lighting_.Get(), kNPSR, kUAVState);
  constants.filter_radius = 2;
  constants.filter_step = 3;
  set_constants();
  set_table(PathTracingRootParameter::kTexture2, kLightingTempSRV);
  set_table(PathTracingRootParameter::kRWTexture0, kLightingUAV);
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Applying to the whole output.
  PushTransitionBarrier(pt_lighting_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_output_.Get(), kNPSR, kUAVState);
  PushUAVBarrier(pt_stats_buffer_.Get());
  set_table(PathTracingRootParameter::kTexture0, kColorSRV);
  set_table(PathTracingRootParameter::kTexture2, kLightingSRV);
  set_table(PathTracingRootParameter::kTexture3, kFrameSRV);
  set_table(PathTracingRootParameter::kTexture4, kAlbedoSRV);
  set_table(PathTracingRootParameter::kRWTexture1, kOutputUAV);
  SetExternalPipeline(pt_composite_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch((width + 7) / 8, (height + 7) / 8, 1);
  PushTransitionBarrier(pt_output_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_vertex_buffer_.Get(), kNPSR, kUAVState);

  if (REXCVAR_GET(path_tracing_debug_log) && pt_frame_ % 120 == 0) {
    REXGPU_INFO(
        "Path tracing: {} recent triangles, building {}, scene {},{} {}x{}, projection {:.4f} "
        "{:.4f}, scene texture {}",
        pt_recent_triangles_, build_triangles, rect_min[0], rect_min[1],
        rect_max[0] - rect_min[0], rect_max[1] - rect_min[1], pt_projection_[0],
        pt_projection_[1],
        scene_texture != swap_texture ? fmt::format("{:08X}", pt_scene_address_) : "not found");
  }

  srv_desc_out = {};
  srv_desc_out.Format = kOutputFormat;
  srv_desc_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_desc_out.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_desc_out.Texture2D.MipLevels = 1;
  return pt_output_.Get();
}

void D3D12CommandProcessor::PathTracingFrameEnd() {
  if (!pt_capture_buffer_) {
    return;
  }
  if (pt_capture_bound_) {
    deferred_command_list_.D3DSOSetTarget(nullptr);
    pt_capture_bound_ = false;
  }
  pt_captured_this_frame_ = false;
  pt_capture_done_this_frame_ = false;
  pt_scene_address_ = 0;
  pt_scene_fetch_valid_ = false;

  // Triangle counts of completed frames.
  for (uint32_t i = 0; i < kPathTracingCountReadbackSlots; ++i) {
    uint64_t submission = pt_count_readback_submissions_[i];
    if (submission && submission_completed_ >= submission) {
      uint32_t triangles = uint32_t(std::min(pt_count_readback_mapping_[i] / 48,
                                             uint64_t(kPathTracingMaxTriangles)));
      pt_recent_triangles_ = std::max(triangles, pt_recent_triangles_ - pt_recent_triangles_ / 32);
      pt_count_readback_submissions_[i] = 0;
    }
  }

  // Read back this frame's count, then restart the capture from the
  // beginning of the buffer.
  uint32_t slot = pt_frame_ % kPathTracingCountReadbackSlots;
  PushTransitionBarrier(pt_capture_counter_.Get(), D3D12_RESOURCE_STATE_STREAM_OUT,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
  SubmitBarriers();
  deferred_command_list_.D3DCopyBufferRegion(pt_count_readback_.Get(), slot * sizeof(uint64_t),
                                             pt_capture_counter_.Get(), 0, sizeof(uint64_t));
  pt_count_readback_submissions_[slot] = submission_current_;
  PushTransitionBarrier(pt_capture_counter_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
  SubmitBarriers();
  deferred_command_list_.D3DCopyBufferRegion(pt_capture_counter_.Get(), 0, pt_zero_upload_.Get(),
                                             0, sizeof(uint64_t));
  PushTransitionBarrier(pt_capture_counter_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_STREAM_OUT);

  pt_retired_textures_.erase(
      std::remove_if(pt_retired_textures_.begin(), pt_retired_textures_.end(),
                     [this](const auto& retired) { return submission_completed_ >= retired.first; }),
      pt_retired_textures_.end());
  ++pt_frame_;
}

}  // namespace rex::graphics::d3d12
