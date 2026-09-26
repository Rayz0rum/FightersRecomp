/**
 * @file        graphics/d3d12/path_tracer.cpp
 * @brief       Experimental path-traced lighting for the D3D12 backend.
 *
 * The guest's scene triangles are captured with stream output (host clip space
 * positions and material attributes) while it draws the frame, with every
 * draw that writes recorded (texture, flags) so triangles can be matched with
 * their draws. At swap time they're converted back to view space, built into a
 * DirectX Raytracing acceleration structure, and traced with inline ray
 * queries (on the ray tracing hardware): a primary ray per pixel finds the
 * surface, which is lit by a sun with soft shadow rays and by global
 * illumination - rays escaping to the sky (colored like the frame's
 * background) or bouncing off other surfaces, themselves lit by the sun (with
 * their own shadow rays) and the sky, colored by their materials. Materials
 * reproduce the game's shading model (path_tracing_albedo_shader) from the
 * captured texture coordinates, and alpha-tested geometry lets light through.
 *
 * The game renders its surfaces unlit (albedo) with the lighting factor
 * neutralized in its pixel shader, and the traced light (linear HDR,
 * denoised) is multiplied into that, exposed automatically and tone mapped.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

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
REXCVAR_DEFINE_STRING(path_tracing_albedo_shader, "", "GPU/Path Tracing",
                      "Pixel shader (ucode hash, hexadecimal) of the scene's surfaces, whose "
                      "lighting factor register is overridden to render unlit colors for the "
                      "path tracer to light (empty to relight the game's shaded colors)");
REXCVAR_DEFINE_INT32(path_tracing_albedo_register, -1, "GPU/Path Tracing",
                     "Pixel shader register with the lighting factor for albedo rendering");
REXCVAR_DEFINE_INT32(path_tracing_albedo_component, 0, "GPU/Path Tracing",
                     "Component (0 to 3) of path_tracing_albedo_register");
REXCVAR_DEFINE_DOUBLE(path_tracing_albedo_value, 1.0, "GPU/Path Tracing",
                      "Neutral lighting factor for albedo rendering");
REXCVAR_DEFINE_INT32(path_tracing_material_uv_interpolator, -1, "GPU/Path Tracing",
                     "Interpolator with the texture coordinates of the material model (-1 if "
                     "none)");
REXCVAR_DEFINE_INT32(path_tracing_material_row_interpolator, -1, "GPU/Path Tracing",
                     "Interpolator with the color table row (x) of the material model");
REXCVAR_DEFINE_INT32(path_tracing_material_light_interpolator, -1, "GPU/Path Tracing",
                     "Interpolator with the game's own lighting factor (x) of the material "
                     "model, which the albedo rendering neutralizes");
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
REXCVAR_DEFINE_BOOL(path_tracing_replace_game_shadows, true, "GPU/Path Tracing",
                    "Remove the game's own black shadows on the ground (with the materials "
                    "known), as the traced sun casts shadows instead");
REXCVAR_DEFINE_DOUBLE(path_tracing_ambient, 0.05, "GPU/Path Tracing",
                      "Minimum sky light, for dark backgrounds");
REXCVAR_DEFINE_DOUBLE(path_tracing_sun_intensity, 2.5, "GPU/Path Tracing",
                      "Sun brightness relative to the sky");
REXCVAR_DEFINE_DOUBLE(path_tracing_sun_elevation, 50.0, "GPU/Path Tracing",
                      "Sun elevation above the ground in degrees");
REXCVAR_DEFINE_DOUBLE(path_tracing_sun_azimuth, 30.0, "GPU/Path Tracing",
                      "Sun direction around the vertical axis in degrees relative to the view, 0 "
                      "behind the camera, 180 facing it");
REXCVAR_DEFINE_DOUBLE(path_tracing_sun_softness, 0.04, "GPU/Path Tracing",
                      "Sun size (shadow penumbra), as the tangent of its angular radius");
REXCVAR_DEFINE_DOUBLE(path_tracing_specular, 0.6, "GPU/Path Tracing",
                      "Strength of specular reflections (sun highlights and ray traced "
                      "reflections, 0 to disable)");
REXCVAR_DEFINE_DOUBLE(path_tracing_roughness, 0.4, "GPU/Path Tracing",
                      "Roughness of the surfaces for specular reflections (0.02 - mirror, 1 - "
                      "matte)");
REXCVAR_DEFINE_DOUBLE(path_tracing_bloom, 0.06, "GPU/Path Tracing",
                      "Strength of the glow around bright areas");
REXCVAR_DEFINE_DOUBLE(path_tracing_bloom_threshold, 1.4, "GPU/Path Tracing",
                      "Exposed brightness above which areas glow");
REXCVAR_DEFINE_BOOL(path_tracing_temporal, true, "GPU/Path Tracing",
                    "Accumulate the lighting over frames (with motion from the captured "
                    "geometry) for less noise");
REXCVAR_DEFINE_DOUBLE(path_tracing_temporal_alpha, 0.15, "GPU/Path Tracing",
                      "Smallest weight of each frame's lighting in the accumulation (lower - "
                      "smoother, more lag)");
REXCVAR_DEFINE_DOUBLE(path_tracing_specular_temporal_alpha, 0.3, "GPU/Path Tracing",
                      "Smallest weight of each frame's reflections in the accumulation");
REXCVAR_DEFINE_DOUBLE(path_tracing_max_distance, 120.0, "GPU/Path Tracing",
                      "Surfaces further than this in view space units (sky, clouds, distant "
                      "scenery) keep their original look");
REXCVAR_DEFINE_DOUBLE(path_tracing_shadow_distance, 60.0, "GPU/Path Tracing",
                      "Maximum distance of sun shadow casters");
REXCVAR_DEFINE_DOUBLE(path_tracing_exposure, 1.2, "GPU/Path Tracing",
                      "Exposure of the average lighting of the scene (auto exposure target)");
REXCVAR_DEFINE_INT32(path_tracing_debug_view, 0, "GPU/Path Tracing",
                     "0 - off, 1 - lighting, 2 - normals, 3 - traced silhouettes and HUD mask, 4 "
                     "- split screen comparison, 5 - surface colors, 6 - material colors, 7 - "
                     "the game's lighting factor");
REXCVAR_DEFINE_BOOL(path_tracing_debug_log, false, "GPU/Path Tracing",
                    "Periodically log path tracing statistics");

namespace rex::graphics::d3d12 {

// Generated with src/graphics/shaders/path_tracing/build_pt_shaders.cmd.
namespace shaders {
#include "../shaders/bytecode/d3d12_6_5/pt_bloom_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_composite_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_convert_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_denoise_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_lighting_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_primary_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_resolve_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_temporal_cs.h"
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
  float specular;
  uint32_t filter_step;
  float bounce_scale;
  float ambient;
  float exposure_target;
  float shadow_distance;
  uint32_t filter_radius;
  uint32_t flags;
  float sky_saturation;
  float max_distance;
  float roughness;
  uint32_t frame_index;
  uint32_t history_valid;
  float temporal_alpha;
  float specular_temporal_alpha;
};
static_assert(sizeof(PathTracingConstants) == 44 * sizeof(uint32_t));

constexpr DXGI_FORMAT kGBufferFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr DXGI_FORMAT kLightingFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

constexpr float kDegreesToRadians = 0.017453292519943295f;

// Two geometries with the same triangle indexing: solid triangles, and
// non-opaque ones (for the alpha test in the ray queries) in the second
// region of the vertex buffer. Each triangle is inactive in one of them.
void GetBlasInputs(D3D12_RAYTRACING_GEOMETRY_DESC (&geometries)[2],
                   D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
                   D3D12_GPU_VIRTUAL_ADDRESS vertices, uint64_t region_size,
                   uint32_t vertex_count) {
  for (uint32_t i = 0; i < 2; ++i) {
    D3D12_RAYTRACING_GEOMETRY_DESC& geometry = geometries[i];
    geometry = {};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = i ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = vertex_count;
    geometry.Triangles.VertexBuffer.StartAddress =
        vertices + i * region_size;
    geometry.Triangles.VertexBuffer.StrideInBytes = 12;
  }
  inputs = {};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  inputs.NumDescs = 2;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.pGeometryDescs = geometries;
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
  D3D12_RAYTRACING_GEOMETRY_DESC geometries[2];
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
  GetBlasInputs(geometries, inputs, 0, kPathTracingVertexRegionSize, kPathTracingMaxTriangles * 3);
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
  constexpr auto kUAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (!create_buffer(kPathTracingCaptureSize, D3D12_RESOURCE_FLAG_NONE, heap_default,
                     heap_flags, D3D12_RESOURCE_STATE_STREAM_OUT, pt_capture_buffer_) ||
      !create_buffer(kPathTracingCounterSize, D3D12_RESOURCE_FLAG_NONE, heap_default,
                     D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_STREAM_OUT,
                     pt_capture_counter_) ||
      !create_buffer(kPathTracingCounterSize, D3D12_RESOURCE_FLAG_NONE, heap_upload,
                     D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                     pt_zero_upload_) ||
      !create_buffer(kPathTracingVertexBufferSize, kUAV, heap_default, heap_flags,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pt_vertex_buffers_[0]) ||
      !create_buffer(kPathTracingVertexBufferSize, kUAV, heap_default, heap_flags,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pt_vertex_buffers_[1]) ||
      // Zeroed - the first frame reads the statistics without clearing them.
      !create_buffer(256, kUAV, heap_default, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pt_stats_buffer_) ||
      !create_buffer(uint64_t(kPathTracingMaxTriangles) * kPathTracingAttributeSize, kUAV,
                     heap_default, heap_flags, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     pt_attribute_buffer_) ||
      !create_buffer(uint64_t(kPathTracingFrameUploadSize) * kQueueFrames,
                     D3D12_RESOURCE_FLAG_NONE, heap_upload, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_GENERIC_READ, pt_material_upload_) ||
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
  std::memset(mapping, 0, kPathTracingCounterSize);
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

  if (FAILED(pt_material_upload_->Map(0, &no_read, &mapping))) {
    ShutdownPathTracing();
    return false;
  }
  pt_material_upload_mapping_ = static_cast<uint8_t*>(mapping);

  // Root signature shared by all the passes.
  D3D12_ROOT_PARAMETER parameters[size_t(PathTracingRootParameter::kCount)];
  D3D12_DESCRIPTOR_RANGE ranges[size_t(PathTracingRootParameter::kCount)];
  for (UINT i = 0; i < UINT(PathTracingRootParameter::kCount); ++i) {
    D3D12_ROOT_PARAMETER& parameter = parameters[i];
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    switch (PathTracingRootParameter(i)) {
      case PathTracingRootParameter::kConstants:
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor.ShaderRegister = 0;
        parameter.Descriptor.RegisterSpace = 0;
        break;
      case PathTracingRootParameter::kPreviousVertices:
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor.ShaderRegister = 9;
        parameter.Descriptor.RegisterSpace = 0;
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
      case PathTracingRootParameter::kMaterials:
      case PathTracingRootParameter::kAttributes:
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor.ShaderRegister = 7 + (i - UINT(PathTracingRootParameter::kMaterials));
        parameter.Descriptor.RegisterSpace = 0;
        break;
      case PathTracingRootParameter::kRWAttributes:
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor.ShaderRegister = 4;
        parameter.Descriptor.RegisterSpace = 0;
        break;
      case PathTracingRootParameter::kMaterialTextures: {
        D3D12_DESCRIPTOR_RANGE& range = ranges[i];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 1;
        range.OffsetInDescriptorsFromTableStart = 0;
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
      } break;
      default: {
        // t2...t6, t0...t1 in space 2, u2, u3, u5.
        UINT table_index = i - UINT(PathTracingRootParameter::kTexture0);
        bool uav = i >= UINT(PathTracingRootParameter::kRWTexture0);
        bool space_2 = i >= UINT(PathTracingRootParameter::kTexture5) && !uav;
        D3D12_DESCRIPTOR_RANGE& range = ranges[i];
        range.RangeType = uav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        if (uav) {
          UINT uav_index = i - UINT(PathTracingRootParameter::kRWTexture0);
          range.BaseShaderRegister = uav_index < 2 ? 2 + uav_index : 5;
        } else {
          range.BaseShaderRegister =
              space_2 ? i - UINT(PathTracingRootParameter::kTexture5) : 2 + table_index;
        }
        range.RegisterSpace = space_2 ? 2 : 0;
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
  // For the material color tables.
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0;
  sampler.RegisterSpace = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  root_signature_desc.NumStaticSamplers = 1;
  root_signature_desc.pStaticSamplers = &sampler;
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
      {pt_lighting_pipeline_, shaders::pt_lighting_cs, sizeof(shaders::pt_lighting_cs)},
      {pt_temporal_pipeline_, shaders::pt_temporal_cs, sizeof(shaders::pt_temporal_cs)},
      {pt_denoise_pipeline_, shaders::pt_denoise_cs, sizeof(shaders::pt_denoise_cs)},
      {pt_resolve_pipeline_, shaders::pt_resolve_cs, sizeof(shaders::pt_resolve_cs)},
      {pt_bloom_pipeline_, shaders::pt_bloom_cs, sizeof(shaders::pt_bloom_cs)},
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
  pt_bloom_b_.Reset();
  pt_bloom_a_.Reset();
  pt_hdr_.Reset();
  pt_specular_.Reset();
  pt_lighting_temp_.Reset();
  pt_lighting_.Reset();
  pt_albedo_.Reset();
  for (uint32_t i = 0; i < 2; ++i) {
    pt_gbuffers_[i].Reset();
    pt_irradiance_history_[i].Reset();
    pt_specular_history_[i].Reset();
  }
  pt_motion_.Reset();
  pt_texture_width_ = 0;
  pt_texture_height_ = 0;
  pt_composite_pipeline_.Reset();
  pt_denoise_pipeline_.Reset();
  pt_resolve_pipeline_.Reset();
  pt_bloom_pipeline_.Reset();
  pt_temporal_pipeline_.Reset();
  pt_lighting_pipeline_.Reset();
  pt_primary_pipeline_.Reset();
  pt_convert_pipeline_.Reset();
  pt_root_signature_.Reset();
  pt_instance_upload_.Reset();
  pt_scratch_.Reset();
  pt_tlas_.Reset();
  pt_blas_.Reset();
  pt_stats_buffer_.Reset();
  pt_attribute_buffer_.Reset();
  if (pt_material_upload_mapping_) {
    pt_material_upload_->Unmap(0, nullptr);
    pt_material_upload_mapping_ = nullptr;
  }
  pt_material_upload_.Reset();
  pt_vertex_buffers_[1].Reset();
  pt_vertex_buffers_[0].Reset();
  pt_zero_upload_.Reset();
  pt_capture_counter_.Reset();
  pt_capture_buffer_.Reset();
}

uint32_t D3D12CommandProcessor::PathTracingStreamOutTriangles(
    const PrimitiveProcessor::ProcessingResult& primitive_processing, bool rasterization_done,
    bool has_pixel_shader) {
  // Pipelines with stream output are those with a geometry shader, which all
  // emit triangles - for triangles, the pass-through one if drawing color.
  if (!rasterization_done || primitive_processing.IsTessellated()) {
    return 0;
  }
  uint32_t vertex_count = primitive_processing.host_draw_vertex_count;
  switch (primitive_processing.host_primitive_type) {
    case xenos::PrimitiveType::kPointList:
      return vertex_count * 2;
    case xenos::PrimitiveType::kRectangleList:
      return vertex_count / 3 * 2;
    case xenos::PrimitiveType::kQuadList:
      return vertex_count / 4 * 2;
    case xenos::PrimitiveType::kTriangleList:
      return has_pixel_shader ? vertex_count / 3 : 0;
    case xenos::PrimitiveType::kTriangleStrip:
      return has_pixel_shader && vertex_count >= 3 ? vertex_count - 2 : 0;
    default:
      return 0;
  }
}

uint64_t D3D12CommandProcessor::PathTracingFetchKey(const xenos::xe_gpu_texture_fetch_t& fetch) {
  return (uint64_t(fetch.dword_1) << 32 | fetch.dword_2) ^
         (uint64_t(fetch.dword_3) << 17 | uint64_t(fetch.dword_0) << 3) ^
         (uint64_t(fetch.dword_4) << 41 | fetch.dword_5);
}

uint32_t D3D12CommandProcessor::PathTracingTextureSlot(const xenos::xe_gpu_texture_fetch_t& fetch) {
  uint64_t key = PathTracingFetchKey(fetch);
  auto it = pt_texture_slots_.find(key);
  if (it != pt_texture_slots_.end()) {
    return it->second;
  }
  if (pt_texture_fetches_.size() >= kPathTracingMaxTextures) {
    return UINT32_MAX;
  }
  uint32_t slot = uint32_t(pt_texture_fetches_.size());
  pt_texture_fetches_.push_back(fetch);
  pt_texture_slots_.emplace(key, slot);
  return slot;
}

void D3D12CommandProcessor::UpdatePathTracingCapture(
    const PrimitiveProcessor::ProcessingResult& primitive_processing, bool primitive_polygonal,
    bool rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    const Shader* pixel_shader, const draw_util::ViewportInfo& viewport_info) {
  if (pt_capture_done_this_frame_) {
    return;
  }
  uint32_t triangles =
      PathTracingStreamOutTriangles(primitive_processing, rasterization_done, pixel_shader != nullptr);
  // Depth-tested polygons (not HUD, backgrounds, clears).
  bool scene = pixel_shader && primitive_polygonal && normalized_depth_control.z_enable;
  const RegisterFile& regs = *register_file_;
  if (!pt_captured_this_frame_) {
    // The capture starts with the first solid scene draw.
    if (!scene || !normalized_depth_control.z_write_enable || !triangles) {
      return;
    }
    pt_captured_this_frame_ = true;
    pt_viewport_ = viewport_info;
    pt_draws_.clear();
    pt_draw_keys_.clear();
    pt_draw_triangles_ = 0;
    pt_texture_fetches_.clear();
    pt_texture_slots_.clear();
    pt_material_found_ = false;
    float scale_x = 0.0f, scale_y = 0.0f;
    int32_t constant = REXCVAR_GET(path_tracing_projection_constant);
    if (constant >= 0 && constant < 255) {
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
  }
  if (!triangles) {
    // No stream output from this pipeline.
    return;
  }
  // Every draw writing while the buffer is bound is recorded, so triangles can
  // be matched with their draws.
  uint32_t flags = 0;
  if (scene && viewport_info.xy_offset[0] == pt_viewport_.xy_offset[0] &&
      viewport_info.xy_offset[1] == pt_viewport_.xy_offset[1] &&
      viewport_info.xy_extent[0] == pt_viewport_.xy_extent[0] &&
      viewport_info.xy_extent[1] == pt_viewport_.xy_extent[1]) {
    flags |= normalized_depth_control.z_write_enable ? kPathTracingDrawOpaque
                                                     : kPathTracingDrawTransparent;
  }
  uint32_t texture = UINT32_MAX, palette = UINT32_MAX;
  float alpha_scale = 0.0f, alpha_bias = 0.0f;
  if (flags && pt_albedo_shader_hash_ && pixel_shader->ucode_data_hash() == pt_albedo_shader_hash_) {
    flags |= kPathTracingDrawMaterial;
    texture = PathTracingTextureSlot(regs.GetTextureFetch(0));
    palette = PathTracingTextureSlot(regs.GetTextureFetch(1));
    const uint32_t* pixel_constants = &regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X];
    // The shader kills if c254.y > alpha * c1.w - c1.x.
    float c1[4], c254[4];
    std::memcpy(c1, pixel_constants + 4 * 1, sizeof(c1));
    std::memcpy(c254, pixel_constants + 4 * 254, sizeof(c254));
    alpha_scale = c1[3];
    alpha_bias = c1[0] + c254[1];
    if (alpha_bias > 0.0f) {
      flags |= kPathTracingDrawAlphaTest;
    }
    if (!pt_material_found_) {
      pt_material_found_ = true;
      std::memcpy(&pt_material_constants_[0], pixel_constants + 4 * 254, sizeof(float) * 4);
      std::memcpy(&pt_material_constants_[4], pixel_constants + 4 * 255, sizeof(float) * 4);
      std::memcpy(&pt_material_constants_[8], pixel_constants + 4 * 1, sizeof(float) * 4);
      std::memcpy(&pt_material_constants_[12], pixel_constants + 4 * 0, sizeof(float) * 4);
    }
  }
  // Each draw writes into its own region of the capture buffer, with its own
  // filled size - the triangle count only has an upper bound here. Draws that
  // don't fit write into the last triangle, which isn't read.
  constexpr uint32_t kTriangleSize = 3 * kPathTracingCaptureVertexSize;
  D3D12_STREAM_OUTPUT_BUFFER_VIEW view;
  uint32_t available = kPathTracingMaxTriangles - 1 - pt_draw_triangles_;
  if (pt_draws_.size() < kPathTracingMaxDraws && available) {
    triangles = std::min(triangles, available);
    view.BufferLocation =
        pt_capture_buffer_->GetGPUVirtualAddress() + uint64_t(pt_draw_triangles_) * kTriangleSize;
    view.SizeInBytes = triangles * kTriangleSize;
    view.BufferFilledSizeLocation =
        pt_capture_counter_->GetGPUVirtualAddress() + pt_draws_.size() * sizeof(uint64_t);
    pt_draws_.push_back({pt_draw_triangles_, triangles, texture, flags, palette, alpha_scale,
                         alpha_bias, UINT32_MAX});
    uint64_t key = uint64_t(triangles) << 40 ^ uint64_t(flags) << 56;
    if (flags & kPathTracingDrawMaterial) {
      key ^= PathTracingFetchKey(regs.GetTextureFetch(0)) ^
             PathTracingFetchKey(regs.GetTextureFetch(1)) * 31;
    }
    pt_draw_keys_.push_back(key);
    pt_draw_triangles_ += triangles;
  } else {
    view.BufferLocation = pt_capture_buffer_->GetGPUVirtualAddress() +
                          uint64_t(kPathTracingMaxTriangles - 1) * kTriangleSize;
    view.SizeInBytes = kTriangleSize;
    view.BufferFilledSizeLocation = pt_capture_counter_->GetGPUVirtualAddress() +
                                    kPathTracingMaxDraws * sizeof(uint64_t);
  }
  deferred_command_list_.D3DSOSetTarget(&view);
  pt_capture_bound_ = true;
}

bool D3D12CommandProcessor::IsPathTracingAlbedoDraw(const Shader& pixel_shader,
                                                    reg::RB_DEPTHCONTROL normalized_depth_control) {
  const std::string& shader_text = REXCVAR_GET(path_tracing_albedo_shader);
  if (shader_text != pt_albedo_shader_text_) {
    pt_albedo_shader_text_ = shader_text;
    pt_albedo_shader_hash_ = std::strtoull(shader_text.c_str(), nullptr, 16);
  }
  // Solid geometry only - blended effects may fade with the lighting factor.
  if (!pt_capture_buffer_ || !normalized_depth_control.z_enable ||
      !normalized_depth_control.z_write_enable || !pt_albedo_shader_hash_ ||
      pixel_shader.ucode_data_hash() != pt_albedo_shader_hash_) {
    return false;
  }
  // Only the scene the path tracer lights (see PathTracingRender) - not
  // models drawn into small viewports, like in menus.
  const RegisterFile& regs = *register_file_;
  uint64_t viewport_area =
      uint64_t(std::abs(regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE)) * 2.0f *
               float(texture_cache_->draw_resolution_scale_x())) *
      uint64_t(std::abs(regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE)) * 2.0f *
               float(texture_cache_->draw_resolution_scale_y()));
  return viewport_area * 4 >= pt_output_area_;
}

bool D3D12CommandProcessor::EnsurePathTracingTextures(uint32_t width, uint32_t height) {
  if (pt_gbuffers_[0] && width <= pt_texture_width_ && height <= pt_texture_height_) {
    return true;
  }
  struct PathTracingTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource>* resource;
    DXGI_FORMAT format;
    bool quarter;
  };
  PathTracingTexture textures[] = {
      {std::addressof(pt_gbuffers_[0]), kGBufferFormat, false},
      {std::addressof(pt_gbuffers_[1]), kGBufferFormat, false},
      {std::addressof(pt_motion_), kGBufferFormat, false},
      {std::addressof(pt_albedo_), kLightingFormat, false},
      {std::addressof(pt_lighting_), kLightingFormat, false},
      {std::addressof(pt_lighting_temp_), kLightingFormat, false},
      {std::addressof(pt_specular_), kLightingFormat, false},
      {std::addressof(pt_irradiance_history_[0]), kLightingFormat, false},
      {std::addressof(pt_irradiance_history_[1]), kLightingFormat, false},
      {std::addressof(pt_specular_history_[0]), kLightingFormat, false},
      {std::addressof(pt_specular_history_[1]), kLightingFormat, false},
      {std::addressof(pt_hdr_), kLightingFormat, false},
      {std::addressof(pt_bloom_a_), kLightingFormat, true},
      {std::addressof(pt_bloom_b_), kLightingFormat, true},
      {std::addressof(pt_output_), kOutputFormat, false},
  };
  for (PathTracingTexture& texture : textures) {
    if (*texture.resource) {
      pt_retired_textures_.emplace_back(submission_current_, std::move(*texture.resource));
    }
  }
  // The history is lost.
  pt_rendered_previous_frame_ = false;
  uint32_t new_width = std::max(width, pt_texture_width_);
  uint32_t new_height = std::max(height, pt_texture_height_);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  for (PathTracingTexture& texture : textures) {
    desc.Format = texture.format;
    desc.Width = texture.quarter ? (new_width + 3) / 4 : new_width;
    desc.Height = texture.quarter ? (new_height + 3) / 4 : new_height;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(texture.resource->ReleaseAndGetAddressOf())))) {
      REXGPU_ERROR("Path tracing: failed to create {}x{} textures", new_width, new_height);
      for (PathTracingTexture& created : textures) {
        created.resource->Reset();
      }
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
  pt_output_area_ = uint64_t(width) * height;

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

  // All triangles the draws could have written.
  uint32_t build_triangles = pt_draw_triangles_;
  if (!build_triangles) {
    return nullptr;
  }

  // The scene before the HUD was drawn over it, if found, to take the surface
  // colors from and to apply the lighting to only where the HUD isn't.
  ID3D12Resource* scene_texture = swap_texture;
  D3D12_SHADER_RESOURCE_VIEW_DESC scene_srv_desc = swap_texture_srv_desc;
  if (pt_scene_fetch_valid_) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    xenos::TextureFormat format;
    ID3D12Resource* texture = texture_cache_->RequestTexture(pt_scene_fetch_, srv_desc, format);
    if (texture) {
      D3D12_RESOURCE_DESC desc = texture->GetDesc();
      if (desc.Width >= rect_max[0] && desc.Height >= rect_max[1]) {
        scene_texture = texture;
        scene_srv_desc = srv_desc;
      }
    }
  }

  // Materials.
  bool materials = pt_material_found_;
  struct MaterialTexture {
    ID3D12Resource* resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
  };
  std::vector<MaterialTexture> material_textures;
  if (materials) {
    material_textures.resize(pt_texture_fetches_.size());
    for (size_t i = 0; i < pt_texture_fetches_.size(); ++i) {
      xenos::TextureFormat format;
      material_textures[i].resource = texture_cache_->RequestTexture(
          pt_texture_fetches_[i], material_textures[i].srv_desc, format);
    }
  }

  // Reusing the previous frame: the same draws are found in it for motion.
  // The history is double-buffered, "current" being written this frame.
  uint32_t current = pt_history_index_ ^ 1, previous = pt_history_index_;
  bool history_valid = REXCVAR_GET(path_tracing_temporal) && pt_rendered_previous_frame_ &&
                       materials && pt_previous_projection_[0] == pt_projection_[0] &&
                       pt_previous_projection_[1] == pt_projection_[1];
  if (history_valid) {
    std::unordered_map<uint64_t, std::vector<uint32_t>> previous_draws;
    for (size_t i = 0; i < pt_previous_draws_.size(); ++i) {
      previous_draws[pt_previous_draw_keys_[i]].push_back(pt_previous_draws_[i].triangle_offset);
    }
    std::unordered_map<uint64_t, uint32_t> previous_draws_used;
    for (size_t i = 0; i < pt_draws_.size(); ++i) {
      auto it = previous_draws.find(pt_draw_keys_[i]);
      if (it == previous_draws.end()) {
        continue;
      }
      uint32_t& used = previous_draws_used[pt_draw_keys_[i]];
      if (used < it->second.size()) {
        pt_draws_[i].previous_first = it->second[used++];
      }
    }
  }

  // All descriptors at once, so they're in the same heap.
  enum Descriptor {
    kFrameSRV,
    kColorSRV,
    kGBufferSRV,
    kGBufferUAV,
    kPreviousGBufferSRV,
    kMotionSRV,
    kMotionUAV,
    kAlbedoSRV,
    kAlbedoUAV,
    kLightingSRV,
    kLightingUAV,
    kLightingTempSRV,
    kLightingTempUAV,
    kSpecularSRV,
    kSpecularUAV,
    kIrradianceHistorySRV,
    kIrradianceHistoryUAV,
    kPreviousIrradianceHistorySRV,
    kSpecularHistorySRV,
    kSpecularHistoryUAV,
    kPreviousSpecularHistorySRV,
    kHdrSRV,
    kHdrUAV,
    kBloomASRV,
    kBloomAUAV,
    kBloomBSRV,
    kBloomBUAV,
    kOutputUAV,
    kMaterialTexturesStart,
  };
  uint32_t descriptor_count = uint32_t(kMaterialTexturesStart + material_textures.size());
  std::vector<ui::d3d12::util::DescriptorCpuGpuHandlePair> descriptors(descriptor_count);
  if (!RequestOneUseSingleViewDescriptors(descriptor_count, descriptors.data())) {
    return nullptr;
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
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
  ID3D12Resource* gbuffer = pt_gbuffers_[current].Get();
  ID3D12Resource* irradiance_history = pt_irradiance_history_[current].Get();
  ID3D12Resource* specular_history = pt_specular_history_[current].Get();
  create_srv(gbuffer, kGBufferFormat, kGBufferSRV);
  create_uav(gbuffer, kGBufferFormat, kGBufferUAV);
  create_srv(pt_gbuffers_[previous].Get(), kGBufferFormat, kPreviousGBufferSRV);
  create_srv(pt_motion_.Get(), kGBufferFormat, kMotionSRV);
  create_uav(pt_motion_.Get(), kGBufferFormat, kMotionUAV);
  create_srv(pt_albedo_.Get(), kLightingFormat, kAlbedoSRV);
  create_uav(pt_albedo_.Get(), kLightingFormat, kAlbedoUAV);
  create_srv(pt_lighting_.Get(), kLightingFormat, kLightingSRV);
  create_uav(pt_lighting_.Get(), kLightingFormat, kLightingUAV);
  create_srv(pt_lighting_temp_.Get(), kLightingFormat, kLightingTempSRV);
  create_uav(pt_lighting_temp_.Get(), kLightingFormat, kLightingTempUAV);
  create_srv(pt_specular_.Get(), kLightingFormat, kSpecularSRV);
  create_uav(pt_specular_.Get(), kLightingFormat, kSpecularUAV);
  create_srv(irradiance_history, kLightingFormat, kIrradianceHistorySRV);
  create_uav(irradiance_history, kLightingFormat, kIrradianceHistoryUAV);
  create_srv(pt_irradiance_history_[previous].Get(), kLightingFormat,
             kPreviousIrradianceHistorySRV);
  create_srv(specular_history, kLightingFormat, kSpecularHistorySRV);
  create_uav(specular_history, kLightingFormat, kSpecularHistoryUAV);
  create_srv(pt_specular_history_[previous].Get(), kLightingFormat, kPreviousSpecularHistorySRV);
  create_srv(pt_hdr_.Get(), kLightingFormat, kHdrSRV);
  create_uav(pt_hdr_.Get(), kLightingFormat, kHdrUAV);
  create_srv(pt_bloom_a_.Get(), kLightingFormat, kBloomASRV);
  create_uav(pt_bloom_a_.Get(), kLightingFormat, kBloomAUAV);
  create_srv(pt_bloom_b_.Get(), kLightingFormat, kBloomBSRV);
  create_uav(pt_bloom_b_.Get(), kLightingFormat, kBloomBUAV);
  create_uav(pt_output_.Get(), kOutputFormat, kOutputUAV);

  // The material texture table covers the whole view heap, indexed by the
  // descriptors' positions in it.
  D3D12_GPU_DESCRIPTOR_HANDLE material_table =
      bindless_resources_used_ ? view_bindless_heap_gpu_start_ : descriptors[0].second;
  uint32_t view_descriptor_size = provider.GetViewDescriptorSize();
  auto descriptor_index = [&](uint32_t descriptor) {
    return uint32_t((descriptors[descriptor].second.ptr - material_table.ptr) /
                    view_descriptor_size);
  };

  // This frame's upload: constant buffers for the passes, then the materials.
  uint32_t upload_frame = uint32_t(frame_current_ % kQueueFrames);
  uint8_t* upload = pt_material_upload_mapping_ + size_t(upload_frame) * kPathTracingFrameUploadSize;
  D3D12_GPU_VIRTUAL_ADDRESS upload_address =
      pt_material_upload_->GetGPUVirtualAddress() +
      uint64_t(upload_frame) * kPathTracingFrameUploadSize;
  uint8_t* material_upload = upload + kPathTracingConstantsUploadSize;
  D3D12_GPU_VIRTUAL_ADDRESS material_upload_address =
      upload_address + kPathTracingConstantsUploadSize;
  {
    uint32_t header[20] = {};
    std::memcpy(header, pt_material_constants_, sizeof(pt_material_constants_));
    header[17] = uint32_t(pt_draws_.size());
    float bloom = std::max(float(REXCVAR_GET(path_tracing_bloom)), 0.0f);
    float bloom_threshold = std::max(float(REXCVAR_GET(path_tracing_bloom_threshold)), 0.0f);
    std::memcpy(&header[16], &bloom, sizeof(float));
    std::memcpy(&header[19], &bloom_threshold, sizeof(float));
    if (materials) {
      header[18] = 1;
    }
    std::memcpy(material_upload, header, sizeof(header));
  }
  // The draws (triangle ranges) are needed even without the materials.
  {
    std::vector<uint32_t> texture_descriptors(material_textures.size(), UINT32_MAX);
    for (size_t i = 0; i < material_textures.size(); ++i) {
      if (materials && material_textures[i].resource) {
        uint32_t descriptor = uint32_t(kMaterialTexturesStart + i);
        device->CreateShaderResourceView(material_textures[i].resource,
                                         &material_textures[i].srv_desc,
                                         descriptors[descriptor].first);
        texture_descriptors[i] = descriptor_index(descriptor);
      }
    }
    PathTracingDraw* draws = reinterpret_cast<PathTracingDraw*>(material_upload + 80);
    for (size_t i = 0; i < pt_draws_.size(); ++i) {
      PathTracingDraw draw = pt_draws_[i];
      draw.texture = draw.texture < texture_descriptors.size() ? texture_descriptors[draw.texture]
                                                              : UINT32_MAX;
      draw.palette = draw.palette < texture_descriptors.size() ? texture_descriptors[draw.palette]
                                                              : UINT32_MAX;
      draws[i] = draw;
    }
  }

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
  constants.debug_view = uint32_t(std::clamp(REXCVAR_GET(path_tracing_debug_view), 0, 7));
  constants.specular = std::max(float(REXCVAR_GET(path_tracing_specular)), 0.0f);
  constants.roughness = std::clamp(float(REXCVAR_GET(path_tracing_roughness)), 0.02f, 1.0f);
  constants.frame_index = pt_frame_;
  constants.history_valid = history_valid ? 1 : 0;
  constants.temporal_alpha =
      std::clamp(float(REXCVAR_GET(path_tracing_temporal_alpha)), 0.01f, 1.0f);
  constants.specular_temporal_alpha =
      std::clamp(float(REXCVAR_GET(path_tracing_specular_temporal_alpha)), 0.01f, 1.0f);

  // Each set of constants goes into its own slot of the upload.
  uint32_t constant_slot = 0;
  auto set_constants = [&]() {
    assert_true(constant_slot < kPathTracingConstantSlots);
    std::memcpy(upload + constant_slot * 256, &constants, sizeof(constants));
    deferred_command_list_.D3DSetComputeRootConstantBufferView(
        UINT(PathTracingRootParameter::kConstants), upload_address + constant_slot * 256);
    ++constant_slot;
  };
  auto set_table = [&](PathTracingRootParameter parameter, Descriptor descriptor) {
    deferred_command_list_.D3DSetComputeRootDescriptorTable(UINT(parameter),
                                                            descriptors[descriptor].second);
  };

  deferred_command_list_.D3DSetComputeRootSignature(pt_root_signature_.Get());
  set_constants();
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kMaterials), material_upload_address);
  deferred_command_list_.D3DSetComputeRootDescriptorTable(
      UINT(PathTracingRootParameter::kMaterialTextures), material_table);
  ID3D12Resource* vertex_buffer = pt_vertex_buffers_[current].Get();
  ID3D12Resource* previous_vertex_buffer = pt_vertex_buffers_[previous].Get();
  D3D12_GPU_VIRTUAL_ADDRESS vertices = vertex_buffer->GetGPUVirtualAddress();
  D3D12_GPU_VIRTUAL_ADDRESS attributes = pt_attribute_buffer_->GetGPUVirtualAddress();
  D3D12_GPU_VIRTUAL_ADDRESS tlas = pt_tlas_->GetGPUVirtualAddress();
  constexpr auto kNPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto kUAVState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  // Captured triangles to view space, and their material attributes. The
  // vertex buffers rest in the UAV state.
  PushTransitionBarrier(pt_capture_buffer_.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, kNPSR);
  PushTransitionBarrier(pt_capture_counter_.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, kNPSR);
  PushUAVBarrier(vertex_buffer);
  PushUAVBarrier(pt_attribute_buffer_.Get());
  PushUAVBarrier(pt_stats_buffer_.Get());
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kBuffer0), pt_capture_buffer_->GetGPUVirtualAddress());
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kBuffer1), pt_capture_counter_->GetGPUVirtualAddress());
  deferred_command_list_.D3DSetComputeRootUnorderedAccessView(
      UINT(PathTracingRootParameter::kRWBuffer0), vertices);
  deferred_command_list_.D3DSetComputeRootUnorderedAccessView(
      UINT(PathTracingRootParameter::kRWBuffer1), pt_stats_buffer_->GetGPUVirtualAddress());
  deferred_command_list_.D3DSetComputeRootUnorderedAccessView(
      UINT(PathTracingRootParameter::kRWAttributes), attributes);
  SetExternalPipeline(pt_convert_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch((build_triangles + 63) / 64, 1, 1);
  PushTransitionBarrier(pt_capture_buffer_.Get(), kNPSR, D3D12_RESOURCE_STATE_STREAM_OUT);
  PushTransitionBarrier(pt_capture_counter_.Get(), kNPSR, D3D12_RESOURCE_STATE_STREAM_OUT);
  PushTransitionBarrier(vertex_buffer, kUAVState, kNPSR);
  PushTransitionBarrier(previous_vertex_buffer, kUAVState, kNPSR);
  PushTransitionBarrier(pt_attribute_buffer_.Get(), kUAVState, kNPSR);
  PushUAVBarrier(pt_stats_buffer_.Get());
  // The previous frame's traversal and builds must be done.
  PushUAVBarrier(pt_blas_.Get());
  PushUAVBarrier(pt_tlas_.Get());
  PushUAVBarrier(pt_scratch_.Get());
  SubmitBarriers();

  // Acceleration structures.
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
  D3D12_RAYTRACING_GEOMETRY_DESC geometries[2];
  GetBlasInputs(geometries, build.Inputs, vertices, kPathTracingVertexRegionSize,
                build_triangles * 3);
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

  uint32_t rect_groups_x = (rect_max[0] - rect_min[0] + 7) / 8;
  uint32_t rect_groups_y = (rect_max[1] - rect_min[1] + 7) / 8;

  // Primary surfaces, their colors, motion and background statistics.
  PushTransitionBarrier(gbuffer, kNPSR, kUAVState);
  PushTransitionBarrier(pt_albedo_.Get(), kNPSR, kUAVState);
  PushTransitionBarrier(pt_motion_.Get(), kNPSR, kUAVState);
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kBuffer0), tlas);
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kBuffer1), vertices);
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kAttributes), attributes);
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kPreviousVertices),
      previous_vertex_buffer->GetGPUVirtualAddress());
  set_table(PathTracingRootParameter::kTexture0, kColorSRV);
  set_table(PathTracingRootParameter::kRWTexture0, kGBufferUAV);
  set_table(PathTracingRootParameter::kRWTexture1, kAlbedoUAV);
  set_table(PathTracingRootParameter::kRWTexture2, kMotionUAV);
  SetExternalPipeline(pt_primary_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Lighting (this frame's samples).
  PushTransitionBarrier(gbuffer, kUAVState, kNPSR);
  PushTransitionBarrier(pt_albedo_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_motion_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_lighting_.Get(), kNPSR, kUAVState);
  PushTransitionBarrier(pt_specular_.Get(), kNPSR, kUAVState);
  PushUAVBarrier(pt_stats_buffer_.Get());
  set_table(PathTracingRootParameter::kTexture1, kGBufferSRV);
  set_table(PathTracingRootParameter::kRWTexture0, kLightingUAV);
  set_table(PathTracingRootParameter::kRWTexture1, kSpecularUAV);
  SetExternalPipeline(pt_lighting_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Temporal accumulation into the current history.
  PushTransitionBarrier(pt_lighting_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_specular_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(irradiance_history, kNPSR, kUAVState);
  PushTransitionBarrier(specular_history, kNPSR, kUAVState);
  set_table(PathTracingRootParameter::kTexture0, kMotionSRV);
  set_table(PathTracingRootParameter::kTexture1, kGBufferSRV);
  set_table(PathTracingRootParameter::kTexture2, kLightingSRV);
  set_table(PathTracingRootParameter::kTexture3, kPreviousGBufferSRV);
  set_table(PathTracingRootParameter::kTexture4, kPreviousIrradianceHistorySRV);
  set_table(PathTracingRootParameter::kTexture5, kSpecularSRV);
  set_table(PathTracingRootParameter::kTexture6, kPreviousSpecularHistorySRV);
  set_table(PathTracingRootParameter::kRWTexture0, kIrradianceHistoryUAV);
  set_table(PathTracingRootParameter::kRWTexture1, kSpecularHistoryUAV);
  SetExternalPipeline(pt_temporal_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Spatial denoising of the accumulated lighting: irradiance (history ->
  // temporary -> lighting), then specular (history -> specular).
  PushTransitionBarrier(irradiance_history, kUAVState, kNPSR);
  PushTransitionBarrier(specular_history, kUAVState, kNPSR);
  PushTransitionBarrier(pt_lighting_temp_.Get(), kNPSR, kUAVState);
  set_table(PathTracingRootParameter::kTexture1, kGBufferSRV);
  constants.filter_radius = 2;
  constants.filter_step = 1;
  set_constants();
  set_table(PathTracingRootParameter::kTexture2, kIrradianceHistorySRV);
  set_table(PathTracingRootParameter::kRWTexture0, kLightingTempUAV);
  SetExternalPipeline(pt_denoise_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);
  PushTransitionBarrier(pt_lighting_temp_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_lighting_.Get(), kNPSR, kUAVState);
  constants.filter_radius = 2;
  constants.filter_step = 2;
  set_constants();
  set_table(PathTracingRootParameter::kTexture2, kLightingTempSRV);
  set_table(PathTracingRootParameter::kRWTexture0, kLightingUAV);
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);
  PushTransitionBarrier(pt_specular_.Get(), kNPSR, kUAVState);
  constants.filter_radius = 2;
  constants.filter_step = 1;
  set_constants();
  set_table(PathTracingRootParameter::kTexture2, kSpecularHistorySRV);
  set_table(PathTracingRootParameter::kRWTexture0, kSpecularUAV);
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Exposed HDR color.
  PushTransitionBarrier(pt_lighting_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_specular_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(pt_hdr_.Get(), kNPSR, kUAVState);
  PushUAVBarrier(pt_stats_buffer_.Get());
  set_table(PathTracingRootParameter::kTexture2, kLightingSRV);
  set_table(PathTracingRootParameter::kTexture4, kAlbedoSRV);
  set_table(PathTracingRootParameter::kTexture5, kSpecularSRV);
  set_table(PathTracingRootParameter::kRWTexture0, kHdrUAV);
  SetExternalPipeline(pt_resolve_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Bloom: bright part at a quarter resolution, blurred horizontally and
  // vertically.
  uint32_t bloom_groups_x = ((rect_max[0] - rect_min[0] + 3) / 4 + 7) / 8;
  uint32_t bloom_groups_y = ((rect_max[1] - rect_min[1] + 3) / 4 + 7) / 8;
  struct BloomPass {
    Descriptor source;
    Descriptor destination;
    ID3D12Resource* source_resource;
    ID3D12Resource* destination_resource;
  };
  BloomPass bloom_passes[] = {
      {kHdrSRV, kBloomAUAV, pt_hdr_.Get(), pt_bloom_a_.Get()},
      {kBloomASRV, kBloomBUAV, pt_bloom_a_.Get(), pt_bloom_b_.Get()},
      {kBloomBSRV, kBloomAUAV, pt_bloom_b_.Get(), pt_bloom_a_.Get()},
  };
  SetExternalPipeline(pt_bloom_pipeline_.Get());
  for (uint32_t i = 0; i < uint32_t(rex::countof(bloom_passes)); ++i) {
    const BloomPass& pass = bloom_passes[i];
    PushTransitionBarrier(pass.source_resource, kUAVState, kNPSR);
    PushTransitionBarrier(pass.destination_resource, kNPSR, kUAVState);
    constants.filter_step = i;
    set_constants();
    set_table(PathTracingRootParameter::kTexture2, pass.source);
    set_table(PathTracingRootParameter::kRWTexture0, pass.destination);
    SubmitBarriers();
    deferred_command_list_.D3DDispatch(bloom_groups_x, bloom_groups_y, 1);
  }
  PushTransitionBarrier(pt_bloom_a_.Get(), kUAVState, kNPSR);

  // Tone mapping into the whole output.
  PushTransitionBarrier(pt_output_.Get(), kNPSR, kUAVState);
  set_table(PathTracingRootParameter::kTexture0, kColorSRV);
  set_table(PathTracingRootParameter::kTexture2, kHdrSRV);
  set_table(PathTracingRootParameter::kTexture3, kFrameSRV);
  set_table(PathTracingRootParameter::kTexture4, kAlbedoSRV);
  set_table(PathTracingRootParameter::kTexture5, kBloomASRV);
  set_table(PathTracingRootParameter::kRWTexture1, kOutputUAV);
  SetExternalPipeline(pt_composite_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch((width + 7) / 8, (height + 7) / 8, 1);
  PushTransitionBarrier(pt_output_.Get(), kUAVState, kNPSR);
  PushTransitionBarrier(vertex_buffer, kNPSR, kUAVState);
  PushTransitionBarrier(previous_vertex_buffer, kNPSR, kUAVState);
  PushTransitionBarrier(pt_attribute_buffer_.Get(), kNPSR, kUAVState);

  if (REXCVAR_GET(path_tracing_debug_log) && pt_frame_ % 120 == 0) {
    uint32_t matched = 0;
    for (const PathTracingDraw& draw : pt_draws_) {
      matched += draw.previous_first != UINT32_MAX ? 1 : 0;
    }
    REXGPU_INFO(
        "Path tracing: {} triangles, scene {},{} {}x{}, projection {:.4f} {:.4f}, scene texture "
        "{}, materials {} ({} draws, {} matched with the previous frame, {} textures)",
        pt_draw_triangles_, rect_min[0], rect_min[1], rect_max[0] - rect_min[0],
        rect_max[1] - rect_min[1], pt_projection_[0], pt_projection_[1],
        scene_texture != swap_texture ? fmt::format("{:08X}", pt_scene_address_) : "not found",
        materials, pt_draws_.size(), matched, material_textures.size());
  }

  // This frame becomes the history.
  pt_history_index_ = current;
  pt_rendered_this_frame_ = true;
  pt_previous_draws_ = pt_draws_;
  pt_previous_draw_keys_ = pt_draw_keys_;
  pt_previous_projection_[0] = pt_projection_[0];
  pt_previous_projection_[1] = pt_projection_[1];

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
  pt_scene_address_ = 0;
  pt_scene_fetch_valid_ = false;

  // Restart the capture.
  PushTransitionBarrier(pt_capture_counter_.Get(), D3D12_RESOURCE_STATE_STREAM_OUT,
                        D3D12_RESOURCE_STATE_COPY_DEST);
  SubmitBarriers();
  deferred_command_list_.D3DCopyBufferRegion(pt_capture_counter_.Get(), 0, pt_zero_upload_.Get(),
                                             0, kPathTracingCounterSize);
  PushTransitionBarrier(pt_capture_counter_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_STREAM_OUT);

  pt_captured_this_frame_ = false;
  pt_capture_done_this_frame_ = false;
  // The history is only usable if the previous frame was path traced.
  pt_rendered_previous_frame_ = pt_rendered_this_frame_;
  pt_rendered_this_frame_ = false;

  if (REXCVAR_GET(path_tracing_debug_log)) {
    auto now = std::chrono::steady_clock::now();
    if (pt_frame_ % 120 == 0) {
      if (pt_frame_) {
        REXGPU_INFO("Path tracing: {:.2f} ms per guest frame on average",
                    std::chrono::duration<double, std::milli>(now - pt_frame_time_).count() / 120.0);
      }
      pt_frame_time_ = now;
    }
  }

  pt_retired_textures_.erase(
      std::remove_if(pt_retired_textures_.begin(), pt_retired_textures_.end(),
                     [this](const auto& retired) { return submission_completed_ >= retired.first; }),
      pt_retired_textures_.end());
  ++pt_frame_;
}

}  // namespace rex::graphics::d3d12
