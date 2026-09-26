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
#include <cstdio>
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
REXCVAR_DEFINE_BOOL(path_tracing_albedo_override, false, "GPU/Path Tracing",
                    "Render the surfaces with the lighting factor register overridden (unlit "
                    "colors) - otherwise the game's lighting is divided out of its colors, which "
                    "also works where the factor selects colors rather than brightness");
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
REXCVAR_DEFINE_INT32(path_tracing_rays, 2, "GPU/Path Tracing",
                     "Global illumination rays per pixel (1 to 16)");
REXCVAR_DEFINE_STRING(path_tracing_denoiser, "nrd", "GPU/Path Tracing",
                      "Denoiser of the traced lighting: nrd (NVIDIA Real-time Denoisers), dlss_rr "
                      "(DLSS Ray Reconstruction, NVIDIA RTX GPUs), fsr_rr (FSR Ray Regeneration, "
                      "AMD Radeon RX 9000 GPUs), builtin. Falls back to nrd, then builtin, if "
                      "unavailable. off shows the game's own lighting");
REXCVAR_DEFINE_DOUBLE(path_tracing_gi_distance, 100.0, "GPU/Path Tracing",
                      "Maximum distance of global illumination rays in view space units");
REXCVAR_DEFINE_DOUBLE(path_tracing_bounce, 1.0, "GPU/Path Tracing",
                      "Strength of the light bounced off surfaces");
REXCVAR_DEFINE_BOOL(path_tracing_radiance_cache, true, "GPU/Path Tracing",
                    "Take the light bounced off surfaces seen in the previous frame from its "
                    "result (multiple bounces, less noise)");
REXCVAR_DEFINE_DOUBLE(path_tracing_sky, 1.0, "GPU/Path Tracing",
                      "Strength of the sky light (colored like the frame's background)");
REXCVAR_DEFINE_DOUBLE(path_tracing_sky_saturation, 0.6, "GPU/Path Tracing",
                      "How much of the background's color the sky light keeps (0 - neutral, 1 - "
                      "all)");
REXCVAR_DEFINE_BOOL(path_tracing_replace_game_shadows, true, "GPU/Path Tracing",
                    "Remove the game's own black shadows on the ground (with the materials "
                    "known), as the traced sun casts shadows instead");
REXCVAR_DEFINE_DOUBLE(path_tracing_ambient, 0.05, "GPU/Path Tracing",
                      "Minimum sky light, for dark backgrounds");
REXCVAR_DEFINE_DOUBLE(path_tracing_sun_intensity, 2.5, "GPU/Path Tracing",
                      "Sun brightness relative to the sky");
REXCVAR_DEFINE_BOOL(path_tracing_game_sun, true, "GPU/Path Tracing",
                    "Take the sun direction from the game's own lighting (fixed in the world), "
                    "rather than from path_tracing_sun_elevation and path_tracing_sun_azimuth "
                    "relative to the camera");
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
REXCVAR_DEFINE_DOUBLE(path_tracing_roughness, 0.45, "GPU/Path Tracing",
                      "Roughness of the scenery for specular reflections (0.02 - mirror, 1 - "
                      "matte)");
REXCVAR_DEFINE_DOUBLE(path_tracing_character_roughness, 0.28, "GPU/Path Tracing",
                      "Roughness of the characters and other moving objects (found by their "
                      "motion)");
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
                     "the game's lighting factor, 8 - denoised sun visibility, 9 - denoised "
                     "indirect diffuse, 10 - denoised indirect specular (NRD and FSR)");
REXCVAR_DEFINE_BOOL(path_tracing_debug_log, false, "GPU/Path Tracing",
                    "Periodically log path tracing statistics");
REXCVAR_DEFINE_BOOL(path_tracing_debug_trace, false, "GPU/Path Tracing",
                    "Every 300 frames, log all draws and resolves of a frame");
REXCVAR_DEFINE_STRING(path_tracing_debug_dump, "", "GPU/Path Tracing",
                      "Path prefix to periodically dump the traced triangles to, for analysis");

namespace rex::graphics::d3d12 {

// Generated with src/graphics/shaders/path_tracing/build_pt_shaders.cmd.
namespace shaders {
#include "../shaders/bytecode/d3d12_6_5/pt_bloom_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_compose_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_composite_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_convert_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_denoise_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_lighting_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_primary_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_sky_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_sun_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_resolve_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_temporal_cs.h"
#include "../shaders/bytecode/d3d12_6_5/pt_tint_cs.h"
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
  uint32_t denoiser;
  uint32_t world_reset;
  float dynamic_roughness;
  uint32_t padding0;
  // Rows (xyz, w - translation).
  float view_to_world[12];
  float view_to_previous_view[12];
  float nrd_hit_distance_parameters[4];
};
static_assert(sizeof(PathTracingConstants) == 76 * sizeof(uint32_t));

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

// Rigid transform b = rotation * a + translation (rotation row-major).
struct RigidTransform {
  double rotation[9];
  double translation[3];
};

void TransformPoint(const RigidTransform& transform, const double* point, double* result) {
  for (uint32_t i = 0; i < 3; ++i) {
    result[i] = transform.rotation[i * 3] * point[0] + transform.rotation[i * 3 + 1] * point[1] +
                transform.rotation[i * 3 + 2] * point[2] + transform.translation[i];
  }
}

// Eigenvector of the largest eigenvalue of a symmetric 4x4 matrix (Jacobi).
void LargestEigenvector4(double (&matrix)[4][4], double (&vector)[4]) {
  double eigenvectors[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  for (uint32_t sweep = 0; sweep < 32; ++sweep) {
    double off_diagonal = 0.0;
    for (uint32_t p = 0; p < 4; ++p) {
      for (uint32_t q = p + 1; q < 4; ++q) {
        off_diagonal += matrix[p][q] * matrix[p][q];
      }
    }
    if (off_diagonal < 1.0e-24) {
      break;
    }
    for (uint32_t p = 0; p < 4; ++p) {
      for (uint32_t q = p + 1; q < 4; ++q) {
        if (std::abs(matrix[p][q]) < 1.0e-30) {
          continue;
        }
        double theta = (matrix[q][q] - matrix[p][p]) / (2.0 * matrix[p][q]);
        double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        for (uint32_t k = 0; k < 4; ++k) {
          double kp = matrix[k][p], kq = matrix[k][q];
          matrix[k][p] = c * kp - s * kq;
          matrix[k][q] = s * kp + c * kq;
        }
        for (uint32_t k = 0; k < 4; ++k) {
          double pk = matrix[p][k], qk = matrix[q][k];
          matrix[p][k] = c * pk - s * qk;
          matrix[q][k] = s * pk + c * qk;
        }
        for (uint32_t k = 0; k < 4; ++k) {
          double kp = eigenvectors[k][p], kq = eigenvectors[k][q];
          eigenvectors[k][p] = c * kp - s * kq;
          eigenvectors[k][q] = s * kp + c * kq;
        }
      }
    }
  }
  uint32_t largest = 0;
  for (uint32_t i = 1; i < 4; ++i) {
    if (matrix[i][i] > matrix[largest][largest]) {
      largest = i;
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    vector[i] = eigenvectors[i][largest];
  }
}

// Least squares rigid transform from points a to points b (xyz triples),
// Horn's quaternion method. Fails for too few or collinear points.
bool FitRigidTransform(const double* a, const double* b, size_t count, RigidTransform& result) {
  if (count < 3) {
    return false;
  }
  double center_a[3] = {}, center_b[3] = {};
  for (size_t i = 0; i < count; ++i) {
    for (uint32_t j = 0; j < 3; ++j) {
      center_a[j] += a[i * 3 + j];
      center_b[j] += b[i * 3 + j];
    }
  }
  for (uint32_t j = 0; j < 3; ++j) {
    center_a[j] /= double(count);
    center_b[j] /= double(count);
  }
  double s[3][3] = {}, spread[3][3] = {};
  for (size_t i = 0; i < count; ++i) {
    double da[3], db[3];
    for (uint32_t j = 0; j < 3; ++j) {
      da[j] = a[i * 3 + j] - center_a[j];
      db[j] = b[i * 3 + j] - center_b[j];
    }
    for (uint32_t j = 0; j < 3; ++j) {
      for (uint32_t k = 0; k < 3; ++k) {
        s[j][k] += da[j] * db[k];
        spread[j][k] += da[j] * da[k];
      }
    }
  }
  // Not collinear: the spread must not be concentrated along one direction.
  double trace = spread[0][0] + spread[1][1] + spread[2][2];
  double second_invariant = spread[0][0] * spread[1][1] + spread[1][1] * spread[2][2] +
                            spread[0][0] * spread[2][2] - spread[0][1] * spread[0][1] -
                            spread[1][2] * spread[1][2] - spread[0][2] * spread[0][2];
  if (!(trace > 1.0e-12) || second_invariant < 1.0e-6 * trace * trace) {
    return false;
  }
  double n[4][4] = {
      {s[0][0] + s[1][1] + s[2][2], s[1][2] - s[2][1], s[2][0] - s[0][2], s[0][1] - s[1][0]},
      {s[1][2] - s[2][1], s[0][0] - s[1][1] - s[2][2], s[0][1] + s[1][0], s[2][0] + s[0][2]},
      {s[2][0] - s[0][2], s[0][1] + s[1][0], -s[0][0] + s[1][1] - s[2][2], s[1][2] + s[2][1]},
      {s[0][1] - s[1][0], s[2][0] + s[0][2], s[1][2] + s[2][1], -s[0][0] - s[1][1] + s[2][2]},
  };
  double q[4];
  LargestEigenvector4(n, q);
  double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(length > 1.0e-12)) {
    return false;
  }
  double w = q[0] / length, x = q[1] / length, y = q[2] / length, z = q[3] / length;
  double* r = result.rotation;
  r[0] = 1.0 - 2.0 * (y * y + z * z);
  r[1] = 2.0 * (x * y - w * z);
  r[2] = 2.0 * (x * z + w * y);
  r[3] = 2.0 * (x * y + w * z);
  r[4] = 1.0 - 2.0 * (x * x + z * z);
  r[5] = 2.0 * (y * z - w * x);
  r[6] = 2.0 * (x * z - w * y);
  r[7] = 2.0 * (y * z + w * x);
  r[8] = 1.0 - 2.0 * (x * x + y * y);
  for (uint32_t i = 0; i < 3; ++i) {
    result.translation[i] = center_b[i] - (r[i * 3] * center_a[0] + r[i * 3 + 1] * center_a[1] +
                                           r[i * 3 + 2] * center_a[2]);
  }
  return true;
}

// Root mean square distance between the transformed points a and points b.
double RigidTransformError(const RigidTransform& transform, const double* a, const double* b,
                           size_t count) {
  double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    double transformed[3];
    TransformPoint(transform, a + i * 3, transformed);
    for (uint32_t j = 0; j < 3; ++j) {
      double d = transformed[j] - b[i * 3 + j];
      sum += d * d;
    }
  }
  return std::sqrt(sum / double(std::max(count, size_t(1))));
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
      case PathTracingRootParameter::kTextures:
      case PathTracingRootParameter::kRWTextures: {
        D3D12_DESCRIPTOR_RANGE& range = ranges[i];
        range.RangeType = PathTracingRootParameter(i) == PathTracingRootParameter::kTextures
                              ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                              : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = kPathTracingPassTextures;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 3;
        range.OffsetInDescriptorsFromTableStart = 0;
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
      } break;
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
      default:
        assert_unhandled_case(PathTracingRootParameter(i));
        break;
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
      {pt_sun_pipeline_, shaders::pt_sun_cs, sizeof(shaders::pt_sun_cs)},
      {pt_sky_pipeline_, shaders::pt_sky_cs, sizeof(shaders::pt_sky_cs)},
      {pt_primary_pipeline_, shaders::pt_primary_cs, sizeof(shaders::pt_primary_cs)},
      {pt_lighting_pipeline_, shaders::pt_lighting_cs, sizeof(shaders::pt_lighting_cs)},
      {pt_temporal_pipeline_, shaders::pt_temporal_cs, sizeof(shaders::pt_temporal_cs)},
      {pt_denoise_pipeline_, shaders::pt_denoise_cs, sizeof(shaders::pt_denoise_cs)},
      {pt_compose_pipeline_, shaders::pt_compose_cs, sizeof(shaders::pt_compose_cs)},
      {pt_resolve_pipeline_, shaders::pt_resolve_cs, sizeof(shaders::pt_resolve_cs)},
      {pt_bloom_pipeline_, shaders::pt_bloom_cs, sizeof(shaders::pt_bloom_cs)},
      {pt_tint_pipeline_, shaders::pt_tint_cs, sizeof(shaders::pt_tint_cs)},
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

  // Descriptor tables need contiguous descriptors - with bindless resources,
  // a block per queued frame is reserved in the heap.
  if (bindless_resources_used_) {
    uint32_t count = kPathTracingDescriptorsPerFrame * kQueueFrames;
    if (kViewBindlessHeapSize - view_bindless_heap_allocated_ < count) {
      REXGPU_ERROR("Path tracing: not enough bindless descriptors");
      ShutdownPathTracing();
      return false;
    }
    pt_bindless_descriptor_base_ = view_bindless_heap_allocated_;
    view_bindless_heap_allocated_ += count;
  }

  // The learned sky (starts unseen - zero).
  {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kPathTracingSkyMapSize;
    desc.Height = kPathTracingSkyMapSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kLightingFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(
            &heap_default, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&pt_sky_map_)))) {
      REXGPU_ERROR("Path tracing: failed to create the sky map");
      ShutdownPathTracing();
      return false;
    }
  }

  // The game's light direction for the denoisers.
  if (!create_buffer(256 * kQueueFrames, D3D12_RESOURCE_FLAG_NONE,
                     ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
                     D3D12_RESOURCE_STATE_COPY_DEST, pt_sun_readback_)) {
    REXGPU_ERROR("Path tracing: failed to create the readback buffer");
    ShutdownPathTracing();
    return false;
  }
  for (uint64_t& frame : pt_sun_readback_frames_) {
    frame = UINT64_MAX;
  }

  REXGPU_INFO("Path tracing: enabled, up to {} triangles ({} MB acceleration structure)",
              kPathTracingMaxTriangles, (blas_info.ResultDataMaxSizeInBytes + 0xFFFFF) >> 20);
  return true;
}

void D3D12CommandProcessor::ShutdownPathTracing() {
  ShutdownPathTracingDenoisers();
  pt_retired_textures_.clear();
  pt_output_.Reset();
  for (auto& texture : pt_textures_) {
    texture.Reset();
  }
  pt_texture_width_ = 0;
  pt_texture_height_ = 0;
  pt_output_width_ = 0;
  pt_output_height_ = 0;
  pt_sun_readback_.Reset();
  pt_sky_map_.Reset();
  pt_sky_pipeline_.Reset();
  pt_tint_pipeline_.Reset();
  pt_composite_pipeline_.Reset();
  pt_denoise_pipeline_.Reset();
  pt_compose_pipeline_.Reset();
  pt_resolve_pipeline_.Reset();
  pt_bloom_pipeline_.Reset();
  pt_temporal_pipeline_.Reset();
  pt_lighting_pipeline_.Reset();
  pt_primary_pipeline_.Reset();
  pt_convert_pipeline_.Reset();
  pt_sun_pipeline_.Reset();
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

uint32_t D3D12CommandProcessor::PathTracingTextureSlot(uint32_t fetch_constant_index) {
  uint64_t key = PathTracingFetchKey(register_file_->GetTextureFetch(fetch_constant_index));
  auto it = pt_texture_slots_.find(key);
  if (it != pt_texture_slots_.end()) {
    return it->second;
  }
  if (pt_texture_handles_.size() >= kPathTracingMaxTextures) {
    return UINT32_MAX;
  }
  // The texture the draw uses.
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
  void* texture = texture_cache_->GetActiveTexture(fetch_constant_index, srv_desc);
  if (!texture) {
    return UINT32_MAX;
  }
  uint32_t slot = uint32_t(pt_texture_handles_.size());
  pt_texture_handles_.push_back(texture);
  pt_texture_srv_descs_.push_back(srv_desc);
  pt_texture_slots_.emplace(key, slot);
  return slot;
}

void D3D12CommandProcessor::UpdatePathTracingCapture(
    const PrimitiveProcessor::ProcessingResult& primitive_processing, bool primitive_polygonal,
    bool rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    const Shader& vertex_shader, const Shader* pixel_shader,
    const draw_util::ViewportInfo& viewport_info) {
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
    pt_draw_samples_.clear();
    pt_draw_sample_ranges_.clear();
    pt_draw_triangles_ = 0;
    pt_texture_handles_.clear();
    pt_texture_srv_descs_.clear();
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
    texture = PathTracingTextureSlot(0);
    palette = PathTracingTextureSlot(1);
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
    if (REXCVAR_GET(path_tracing_debug_trace) && pt_frame_ % 300 == 7) {
      float c255[4], c0[4];
      std::memcpy(c255, pixel_constants + 4 * 255, sizeof(c255));
      std::memcpy(c0, pixel_constants + 4 * 0, sizeof(c0));
      REXGPU_INFO(
          "Path tracing draw {}: c254 {:.4f} {:.4f} {:.4f} {:.4f}, c255 {:.4f} {:.4f} {:.4f} "
          "{:.4f}, c0 {:.4f}, c1 {:.4f} {:.4f} {:.4f} {:.4f}, z write {}",
          pt_draws_.size(), c254[0], c254[1], c254[2], c254[3], c255[0], c255[1], c255[2],
          c255[3], c0[0], c1[0], c1[1], c1[2], c1[3], bool(normalized_depth_control.z_write_enable));
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
    SamplePathTracingDrawVertices(primitive_processing, vertex_shader);
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

void D3D12CommandProcessor::SamplePathTracingDrawVertices(
    const PrimitiveProcessor::ProcessingResult& primitive_processing, const Shader& vertex_shader) {
  uint32_t first = uint32_t(pt_draw_samples_.size() / 3);
  uint32_t count = 0;
  // The position: 3 floats at the start of a vertex.
  const Shader::VertexBinding* position_binding = nullptr;
  for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    for (const Shader::VertexBinding::Attribute& attribute : binding.attributes) {
      if (attribute.fetch_instr.attributes.data_format == xenos::VertexFormat::k_32_32_32_FLOAT &&
          attribute.fetch_instr.attributes.offset == 0) {
        position_binding = &binding;
        break;
      }
    }
    if (position_binding) {
      break;
    }
  }
  const RegisterFile& regs = *register_file_;
  uint32_t debug_factor_min = UINT32_MAX, debug_factor_max = 0;
  if (position_binding && position_binding->stride_words >= 3 && memory_) {
    xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(position_binding->fetch_constant);
    uint32_t buffer_address = fetch.address << 2;
    uint32_t buffer_size = fetch.size << 2;
    uint32_t stride = position_binding->stride_words * 4;
    uint32_t index_count = primitive_processing.guest_draw_vertex_count;
    auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
    uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
    bool indexed = draw_initiator.source_select == xenos::SourceSelect::kDMA;
    bool index_32bit = draw_initiator.index_size == xenos::IndexFormat::kInt32;
    xenos::Endian index_endian = regs.Get<reg::VGT_DMA_SIZE>().swap_mode;
    uint32_t index_base = regs[XE_GPU_REG_VGT_DMA_BASE] & ~uint32_t(index_32bit ? 3 : 1);
    uint32_t step = std::max(index_count / kPathTracingDrawVertexSamples, uint32_t(1));
    for (uint32_t i = 0; i < index_count && count < kPathTracingDrawVertexSamples; i += step) {
      uint32_t index = i;
      if (indexed) {
        if (index_32bit) {
          index = xenos::GpuSwap(*memory_->TranslatePhysical<const uint32_t*>(index_base + i * 4),
                                 index_endian);
        } else {
          index = xenos::GpuSwap(*memory_->TranslatePhysical<const uint16_t*>(index_base + i * 2),
                                 index_endian);
        }
      }
      uint32_t offset = ((index + index_offset) & 0xFFFFFF) * stride;
      if (offset >= buffer_size || buffer_size - offset < 12) {
        continue;
      }
      const uint32_t* vertex = memory_->TranslatePhysical<const uint32_t*>(buffer_address + offset);
      float position[3];
      bool valid = true;
      for (uint32_t j = 0; j < 3; ++j) {
        uint32_t bits = xenos::GpuSwap(vertex[j], fetch.endian);
        std::memcpy(&position[j], &bits, sizeof(float));
        valid = valid && std::isfinite(position[j]) && std::abs(position[j]) < 1.0e6f;
      }
      if (!valid) {
        continue;
      }
      pt_draw_samples_.insert(pt_draw_samples_.end(), position, position + 3);
      ++count;
      if (REXCVAR_GET(path_tracing_debug_trace) && pt_frame_ % 300 == 7 &&
          position_binding->stride_words > 3) {
        uint32_t factor_bits = xenos::GpuSwap(vertex[3], fetch.endian);
        debug_factor_min = std::min(debug_factor_min, factor_bits);
        debug_factor_max = std::max(debug_factor_max, factor_bits);
      }
    }
    if (REXCVAR_GET(path_tracing_debug_trace) && pt_frame_ % 300 == 7) {
      REXGPU_INFO("PTTRACE capture draw {}: tf0 {:08X} tf1 {:08X}, {} vertices, lighting dword "
                  "{:08X}...{:08X}, flags {:X}",
                  pt_draw_sample_ranges_.size() / 2, regs.GetTextureFetch(0).base_address << 12,
                  regs.GetTextureFetch(1).base_address << 12, index_count, debug_factor_min,
                  debug_factor_max, pt_draws_.empty() ? 0 : pt_draws_.back().flags);
    }
  }
  pt_draw_sample_ranges_.push_back(first);
  pt_draw_sample_ranges_.push_back(count);
}

void D3D12CommandProcessor::UpdatePathTracingCamera() {
  std::memcpy(pt_previous_view_to_world_rotation_, pt_view_to_world_rotation_,
              sizeof(pt_view_to_world_rotation_));
  std::memcpy(pt_previous_view_to_world_translation_, pt_view_to_world_translation_,
              sizeof(pt_view_to_world_translation_));
  pt_camera_draws_agreeing_ = 0;

  // Sample pairs of the draws seen in both frames: this frame's positions and
  // the previous frame's.
  struct DrawPairs {
    size_t first;
    size_t count;
    double depth;
    size_t draw;
  };
  std::vector<DrawPairs> draws;
  std::vector<double> current, previous;
  for (size_t i = 0; i < pt_draw_previous_index_.size(); ++i) {
    uint32_t previous_index = pt_draw_previous_index_[i];
    if (previous_index == UINT32_MAX || i * 2 + 1 >= pt_draw_sample_ranges_.size() ||
        size_t(previous_index) * 2 + 1 >= pt_previous_draw_sample_ranges_.size()) {
      continue;
    }
    uint32_t first = pt_draw_sample_ranges_[i * 2], count = pt_draw_sample_ranges_[i * 2 + 1];
    uint32_t previous_first = pt_previous_draw_sample_ranges_[size_t(previous_index) * 2];
    uint32_t previous_count = pt_previous_draw_sample_ranges_[size_t(previous_index) * 2 + 1];
    if (count < 3 || count != previous_count) {
      continue;
    }
    DrawPairs pairs = {current.size() / 3, count, 0.0, i};
    for (uint32_t j = 0; j < count * 3; ++j) {
      current.push_back(pt_draw_samples_[size_t(first) * 3 + j]);
      previous.push_back(pt_previous_draw_samples_[size_t(previous_first) * 3 + j]);
    }
    for (uint32_t j = 0; j < count; ++j) {
      const double* point = &current[(pairs.first + j) * 3];
      pairs.depth += std::sqrt(point[0] * point[0] + point[1] * point[1] + point[2] * point[2]);
    }
    pairs.depth /= double(count);
    draws.push_back(pairs);
  }

  // The static scenery moves the same way (with the camera), anything else
  // differently. The motion agreed on by the most draws is the camera's.
  auto tolerance = [](const DrawPairs& pairs) {
    return 2.0e-3 * std::max(pairs.depth, 1.0) + 1.0e-4;
  };
  std::vector<size_t> candidates(draws.size());
  for (size_t i = 0; i < draws.size(); ++i) {
    candidates[i] = i;
  }
  std::sort(candidates.begin(), candidates.end(),
            [&](size_t a, size_t b) { return draws[a].count > draws[b].count; });
  if (candidates.size() > 32) {
    candidates.resize(32);
  }
  size_t best_support = 0;
  RigidTransform best = {};
  for (size_t candidate : candidates) {
    const DrawPairs& pairs = draws[candidate];
    RigidTransform transform;
    if (!FitRigidTransform(&current[pairs.first * 3], &previous[pairs.first * 3], pairs.count,
                           transform) ||
        RigidTransformError(transform, &current[pairs.first * 3], &previous[pairs.first * 3],
                            pairs.count) > tolerance(pairs)) {
      continue;
    }
    size_t support = 0;
    for (const DrawPairs& other : draws) {
      if (RigidTransformError(transform, &current[other.first * 3], &previous[other.first * 3],
                              other.count) <= tolerance(other)) {
        support += other.count;
      }
    }
    if (support > best_support) {
      best_support = support;
      best = transform;
    }
  }

  // Refined with all the agreeing draws.
  bool tracked = false;
  RigidTransform motion = {};
  if (best_support >= 12) {
    std::vector<double> inliers_current, inliers_previous;
    uint32_t agreeing = 0;
    for (const DrawPairs& pairs : draws) {
      if (RigidTransformError(best, &current[pairs.first * 3], &previous[pairs.first * 3],
                              pairs.count) <= tolerance(pairs)) {
        inliers_current.insert(inliers_current.end(), current.begin() + pairs.first * 3,
                               current.begin() + (pairs.first + pairs.count) * 3);
        inliers_previous.insert(inliers_previous.end(), previous.begin() + pairs.first * 3,
                                previous.begin() + (pairs.first + pairs.count) * 3);
        ++agreeing;
      }
    }
    if (agreeing >= 2 && FitRigidTransform(inliers_current.data(), inliers_previous.data(),
                                           inliers_current.size() / 3, motion)) {
      tracked = true;
      pt_camera_draws_agreeing_ = agreeing;
    }
  }

  // Draws moving on their own (characters, props) - with the ones that did
  // recently, so parts holding still for a moment don't change materials.
  for (const DrawPairs& pairs : draws) {
    if (tracked && RigidTransformError(motion, &current[pairs.first * 3],
                                       &previous[pairs.first * 3], pairs.count) > tolerance(pairs)) {
      pt_dynamic_draw_frames_[pt_draw_keys_[pairs.draw]] = pt_frame_;
    }
  }
  for (size_t i = 0; i < pt_draws_.size(); ++i) {
    auto it = pt_dynamic_draw_frames_.find(pt_draw_keys_[i]);
    if (it != pt_dynamic_draw_frames_.end() && pt_frame_ - it->second < 120) {
      pt_draws_[i].flags |= kPathTracingDrawDynamic;
    }
  }
  if (pt_dynamic_draw_frames_.size() > 16384) {
    for (auto it = pt_dynamic_draw_frames_.begin(); it != pt_dynamic_draw_frames_.end();) {
      it = pt_frame_ - it->second >= 120 ? pt_dynamic_draw_frames_.erase(it) : std::next(it);
    }
  }

  if (tracked && pt_camera_tracked_initialized_) {
    // View to world = previous view to world * (this view to the previous).
    double rotation[9], translation[3];
    const double* r0 = pt_previous_view_to_world_rotation_;
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t j = 0; j < 3; ++j) {
        rotation[i * 3 + j] = r0[i * 3] * motion.rotation[j] +
                              r0[i * 3 + 1] * motion.rotation[3 + j] +
                              r0[i * 3 + 2] * motion.rotation[6 + j];
      }
      translation[i] = r0[i * 3] * motion.translation[0] + r0[i * 3 + 1] * motion.translation[1] +
                       r0[i * 3 + 2] * motion.translation[2] +
                       pt_previous_view_to_world_translation_[i];
    }
    // Keep the rotation orthonormal (Gram-Schmidt on the rows).
    double* row0 = rotation;
    double* row1 = rotation + 3;
    double* row2 = rotation + 6;
    auto normalize = [](double* v) {
      double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      for (uint32_t k = 0; k < 3; ++k) {
        v[k] /= length;
      }
    };
    normalize(row0);
    double d = row0[0] * row1[0] + row0[1] * row1[1] + row0[2] * row1[2];
    for (uint32_t k = 0; k < 3; ++k) {
      row1[k] -= d * row0[k];
    }
    normalize(row1);
    row2[0] = row0[1] * row1[2] - row0[2] * row1[1];
    row2[1] = row0[2] * row1[0] - row0[0] * row1[2];
    row2[2] = row0[0] * row1[1] - row0[1] * row1[0];
    std::memcpy(pt_view_to_world_rotation_, rotation, sizeof(rotation));
    std::memcpy(pt_view_to_world_translation_, translation, sizeof(translation));
    pt_camera_tracked_ = true;
  } else {
    // Restart the world space where the camera is.
    static const double kIdentity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::memcpy(pt_view_to_world_rotation_, kIdentity, sizeof(kIdentity));
    std::memset(pt_view_to_world_translation_, 0, sizeof(pt_view_to_world_translation_));
    pt_camera_tracked_ = false;
    pt_camera_tracked_initialized_ = true;
    ++pt_world_resets_;
  }
}

void D3D12CommandProcessor::InvalidatePathTracingCommandListState() {
  // Same as at the beginning of a submission (the descriptor heaps and root
  // signatures are restored by the command list itself).
  current_guest_pipeline_ = nullptr;
  current_external_pipeline_ = nullptr;
  current_graphics_root_signature_ = nullptr;
  current_graphics_root_up_to_date_ = 0;
  primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ff_viewport_update_needed_ = true;
  ff_scissor_update_needed_ = true;
  ff_blend_factor_update_needed_ = true;
  ff_stencil_ref_update_needed_ = true;
}

bool D3D12CommandProcessor::IsPathTracingAlbedoDraw(const Shader& pixel_shader,
                                                    reg::RB_DEPTHCONTROL normalized_depth_control) {
  const std::string& shader_text = REXCVAR_GET(path_tracing_albedo_shader);
  if (shader_text != pt_albedo_shader_text_) {
    pt_albedo_shader_text_ = shader_text;
    pt_albedo_shader_hash_ = std::strtoull(shader_text.c_str(), nullptr, 16);
  }
  // Solid geometry only - blended effects may fade with the lighting factor.
  if (!REXCVAR_GET(path_tracing_albedo_override) || !pt_capture_buffer_ ||
      !normalized_depth_control.z_enable ||
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

bool D3D12CommandProcessor::EnsurePathTracingTextures(uint32_t rect_width, uint32_t rect_height,
                                                      uint32_t output_width,
                                                      uint32_t output_height) {
  if (pt_textures_[0] && rect_width == pt_texture_width_ && rect_height == pt_texture_height_ &&
      output_width == pt_output_width_ && output_height == pt_output_height_) {
    return true;
  }
  struct TextureInfo {
    PathTracingTexture texture;
    DXGI_FORMAT format;
    bool quarter;
  };
  static const TextureInfo kTextures[] = {
      {PathTracingTexture::kGBuffer0, kGBufferFormat, false},
      {PathTracingTexture::kGBuffer1, kGBufferFormat, false},
      {PathTracingTexture::kMotion, kGBufferFormat, false},
      {PathTracingTexture::kAlbedo, kLightingFormat, false},
      {PathTracingTexture::kViewDepth, DXGI_FORMAT_R32_FLOAT, false},
      {PathTracingTexture::kNRDNormalRoughness, DXGI_FORMAT_R10G10B10A2_UNORM, false},
      {PathTracingTexture::kWorldMotion, kLightingFormat, false},
      {PathTracingTexture::kScreenMotion, kLightingFormat, false},
      {PathTracingTexture::kNormalRoughness, kLightingFormat, false},
      {PathTracingTexture::kOctahedralNormal, kLightingFormat, false},
      {PathTracingTexture::kDiffuseAlbedo, kLightingFormat, false},
      {PathTracingTexture::kSpecularAlbedo, kLightingFormat, false},
      {PathTracingTexture::kLighting, kLightingFormat, false},
      {PathTracingTexture::kLightingTemp, kLightingFormat, false},
      {PathTracingTexture::kSpecular, kLightingFormat, false},
      {PathTracingTexture::kShadow, kLightingFormat, false},
      {PathTracingTexture::kIrradianceHistory0, kLightingFormat, false},
      {PathTracingTexture::kIrradianceHistory1, kLightingFormat, false},
      {PathTracingTexture::kSpecularHistory0, kLightingFormat, false},
      {PathTracingTexture::kSpecularHistory1, kLightingFormat, false},
      {PathTracingTexture::kDenoisedDiffuse, kLightingFormat, false},
      {PathTracingTexture::kDenoisedSpecular, kLightingFormat, false},
      {PathTracingTexture::kDenoisedShadow, kLightingFormat, false},
      {PathTracingTexture::kRRColor, kLightingFormat, false},
      {PathTracingTexture::kRROutput, kLightingFormat, false},
      {PathTracingTexture::kHDR, kLightingFormat, false},
      {PathTracingTexture::kBloomA, kLightingFormat, true},
      {PathTracingTexture::kBloomB, kLightingFormat, true},
  };
  static_assert(rex::countof(kTextures) == size_t(PathTracingTexture::kCount));
  for (auto& texture : pt_textures_) {
    if (texture) {
      pt_retired_textures_.emplace_back(submission_current_, std::move(texture));
    }
  }
  if (pt_output_) {
    pt_retired_textures_.emplace_back(submission_current_, std::move(pt_output_));
  }
  // The histories are lost.
  pt_rendered_previous_frame_ = false;
  pt_texture_width_ = 0;
  pt_texture_height_ = 0;
  pt_output_width_ = 0;
  pt_output_height_ = 0;
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto create = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& resource, DXGI_FORMAT format,
                    uint32_t width, uint32_t height) {
    desc.Format = format;
    desc.Width = std::max(width, uint32_t(1));
    desc.Height = std::max(height, uint32_t(1));
    return SUCCEEDED(device->CreateCommittedResource(
        &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
        IID_PPV_ARGS(resource.ReleaseAndGetAddressOf())));
  };
  bool created = create(pt_output_, kOutputFormat, output_width, output_height);
  for (const TextureInfo& info : kTextures) {
    if (!created) {
      break;
    }
    created = create(pt_textures_[size_t(info.texture)], info.format,
                     info.quarter ? (rect_width + 3) / 4 : rect_width,
                     info.quarter ? (rect_height + 3) / 4 : rect_height);
  }
  if (!created) {
    REXGPU_ERROR("Path tracing: failed to create {}x{} textures", rect_width, rect_height);
    for (auto& texture : pt_textures_) {
      texture.Reset();
    }
    pt_output_.Reset();
    return false;
  }
  pt_texture_width_ = rect_width;
  pt_texture_height_ = rect_height;
  pt_output_width_ = output_width;
  pt_output_height_ = output_height;
  return true;
}

void D3D12CommandProcessor::PathTracingUseResource(ID3D12Resource* resource,
                                                   D3D12_RESOURCE_STATES state) {
  if (!resource) {
    return;
  }
  for (auto& resource_state : pt_resource_states_) {
    if (resource_state.first != resource) {
      continue;
    }
    if (resource_state.second == state) {
      if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        PushUAVBarrier(resource);
      }
    } else {
      PushTransitionBarrier(resource, resource_state.second, state);
      resource_state.second = state;
    }
    return;
  }
  constexpr auto kResting = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (state != kResting) {
    PushTransitionBarrier(resource, kResting, state);
  }
  pt_resource_states_.emplace_back(resource, state);
}

void D3D12CommandProcessor::PathTracingRestoreResourceStates() {
  for (const auto& resource_state : pt_resource_states_) {
    PushTransitionBarrier(resource_state.first, resource_state.second,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  pt_resource_states_.clear();
}

bool D3D12CommandProcessor::PathTracingAllocateDescriptors(
    uint32_t count, ui::d3d12::util::DescriptorCpuGpuHandlePair& start) {
  if (pt_frame_descriptors_used_ + count > pt_frame_descriptor_count_) {
    return false;
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  start = std::make_pair(
      provider.OffsetViewDescriptor(pt_frame_descriptors_.first, pt_frame_descriptors_used_),
      provider.OffsetViewDescriptor(pt_frame_descriptors_.second, pt_frame_descriptors_used_));
  pt_frame_descriptors_used_ += count;
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
  if (!pt_root_signature_ || !pt_captured_this_frame_ || !width || !height ||
      REXCVAR_GET(path_tracing_denoiser) == "off") {
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
  uint32_t rect_width = rect_max[0] - rect_min[0], rect_height = rect_max[1] - rect_min[1];
  uint64_t rect_area = uint64_t(rect_width) * rect_height;
  if (rect_area * 4 < uint64_t(width) * height || !viewport.ndc_scale[0] ||
      !viewport.ndc_scale[1]) {
    return nullptr;
  }
  if (!EnsurePathTracingTextures(rect_width, rect_height, width, height)) {
    return nullptr;
  }

  // All triangles the draws could have written.
  uint32_t build_triangles = pt_draw_triangles_;
  if (!build_triangles) {
    return nullptr;
  }

  // Descriptors for the frame's tables, contiguous.
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  pt_frame_descriptors_used_ = 0;
  if (bindless_resources_used_) {
    uint32_t base = pt_bindless_descriptor_base_ +
                    uint32_t(frame_current_ % kQueueFrames) * kPathTracingDescriptorsPerFrame;
    pt_frame_descriptors_ =
        std::make_pair(provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, base),
                       provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, base));
    pt_frame_descriptor_count_ = kPathTracingDescriptorsPerFrame;
  } else {
    constexpr uint32_t kBindfulDescriptors = 2048;
    // One contiguous range (one-use bindful descriptors are).
    std::vector<ui::d3d12::util::DescriptorCpuGpuHandlePair> range(kBindfulDescriptors);
    if (!RequestOneUseSingleViewDescriptors(kBindfulDescriptors, range.data())) {
      return nullptr;
    }
    pt_frame_descriptors_ = range[0];
    pt_frame_descriptor_count_ = kBindfulDescriptors;
  }
  // The material texture table covers the whole view heap (bindless) or the
  // frame's descriptors, indexed by the descriptors' positions in it.
  D3D12_GPU_DESCRIPTOR_HANDLE material_table =
      bindless_resources_used_ ? view_bindless_heap_gpu_start_ : pt_frame_descriptors_.second;
  uint32_t view_descriptor_size = provider.GetViewDescriptorSize();
  auto descriptor_index = [&](const ui::d3d12::util::DescriptorCpuGpuHandlePair& descriptor) {
    return uint32_t((descriptor.second.ptr - material_table.ptr) / view_descriptor_size);
  };

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
    material_textures.resize(pt_texture_handles_.size());
    for (size_t i = 0; i < pt_texture_handles_.size(); ++i) {
      material_textures[i].resource =
          texture_cache_->PrepareActiveTextureForReading(pt_texture_handles_[i]);
      material_textures[i].srv_desc = pt_texture_srv_descs_[i];
    }
  }

  // The same draws in the previous frame, for motion and the camera.
  // The history is double-buffered, "current" being written this frame.
  uint32_t current = pt_history_index_ ^ 1, previous = pt_history_index_;
  bool previous_comparable = pt_rendered_previous_frame_ &&
                             pt_previous_projection_[0] == pt_projection_[0] &&
                             pt_previous_projection_[1] == pt_projection_[1];
  pt_draw_previous_index_.assign(pt_draws_.size(), UINT32_MAX);
  if (previous_comparable) {
    std::unordered_map<uint64_t, std::vector<uint32_t>> previous_draws;
    for (size_t i = 0; i < pt_previous_draws_.size(); ++i) {
      previous_draws[pt_previous_draw_keys_[i]].push_back(uint32_t(i));
    }
    std::unordered_map<uint64_t, uint32_t> previous_draws_used;
    for (size_t i = 0; i < pt_draws_.size(); ++i) {
      auto it = previous_draws.find(pt_draw_keys_[i]);
      if (it == previous_draws.end()) {
        continue;
      }
      uint32_t& used = previous_draws_used[pt_draw_keys_[i]];
      if (used < it->second.size()) {
        pt_draw_previous_index_[i] = it->second[used++];
      }
    }
  }
  // Where the camera is in the world, from the draws seen in both frames.
  UpdatePathTracingCamera();
  bool camera_tracked = previous_comparable && pt_camera_tracked_;
  bool history_valid = REXCVAR_GET(path_tracing_temporal) && previous_comparable && materials;
  if (history_valid) {
    for (size_t i = 0; i < pt_draws_.size(); ++i) {
      if (pt_draw_previous_index_[i] != UINT32_MAX) {
        pt_draws_[i].previous_first =
            pt_previous_draws_[pt_draw_previous_index_[i]].triangle_offset;
      }
    }
  }

  PathTracingDenoiser denoiser = SelectPathTracingDenoiser();
  bool denoiser_reset =
      denoiser != pt_previous_denoiser_ || !camera_tracked || !pt_rendered_previous_frame_;
  pt_previous_denoiser_ = denoiser;

  // The game's light direction in world space, from a completed frame.
  {
    uint32_t slot = uint32_t(frame_completed_ % kQueueFrames);
    if (pt_sun_readback_frames_[slot] == frame_completed_) {
      D3D12_RANGE range = {slot * 256, slot * 256 + 16};
      void* mapping;
      if (SUCCEEDED(pt_sun_readback_->Map(0, &range, &mapping))) {
        float sun[4];
        std::memcpy(sun, static_cast<const uint8_t*>(mapping) + slot * 256, sizeof(sun));
        D3D12_RANGE written = {};
        pt_sun_readback_->Unmap(0, &written);
        float length = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        if (sun[3] > 0.0f && length > 0.5f && length < 1.5f) {
          for (uint32_t i = 0; i < 3; ++i) {
            pt_sun_direction_world_[i] = sun[i] / length;
          }
        }
      }
    }
  }

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
      ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor;
      if (materials && material_textures[i].resource &&
          PathTracingAllocateDescriptors(1, descriptor)) {
        device->CreateShaderResourceView(material_textures[i].resource,
                                         &material_textures[i].srv_desc, descriptor.first);
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

  // Camera transforms.
  const double* r = pt_view_to_world_rotation_;
  const double* rp = pt_previous_view_to_world_rotation_;
  const double* t = pt_view_to_world_translation_;
  const double* tp = pt_previous_view_to_world_translation_;
  // This view to the previous view: previous rotation^T * (rotation, t - tp).
  double to_previous_rotation[9], to_previous_translation[3];
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t j = 0; j < 3; ++j) {
      to_previous_rotation[i * 3 + j] =
          rp[i] * r[j] + rp[3 + i] * r[3 + j] + rp[6 + i] * r[6 + j];
    }
    to_previous_translation[i] =
        rp[i] * (t[0] - tp[0]) + rp[3 + i] * (t[1] - tp[1]) + rp[6 + i] * (t[2] - tp[2]);
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
  constants.flags = (REXCVAR_GET(path_tracing_replace_game_shadows) ? 1u << 0 : 0u) |
                    (REXCVAR_GET(path_tracing_game_sun) ? 1u << 1 : 0u) |
                    (camera_tracked ? 1u << 2 : 0u) |
                    (REXCVAR_GET(path_tracing_albedo_override) ? 1u << 3 : 0u) |
                    (REXCVAR_GET(path_tracing_radiance_cache) ? 1u << 4 : 0u);
  constants.sky_saturation =
      std::clamp(float(REXCVAR_GET(path_tracing_sky_saturation)), 0.0f, 1.0f);
  constants.max_distance = std::max(float(REXCVAR_GET(path_tracing_max_distance)), 1.0e-2f);
  constants.debug_view = uint32_t(std::clamp(REXCVAR_GET(path_tracing_debug_view), 0, 10));
  constants.specular = std::max(float(REXCVAR_GET(path_tracing_specular)), 0.0f);
  constants.roughness = std::clamp(float(REXCVAR_GET(path_tracing_roughness)), 0.02f, 1.0f);
  constants.dynamic_roughness =
      std::clamp(float(REXCVAR_GET(path_tracing_character_roughness)), 0.02f, 1.0f);
  constants.frame_index = pt_frame_;
  constants.history_valid = history_valid ? 1 : 0;
  constants.temporal_alpha =
      std::clamp(float(REXCVAR_GET(path_tracing_temporal_alpha)), 0.01f, 1.0f);
  constants.specular_temporal_alpha =
      std::clamp(float(REXCVAR_GET(path_tracing_specular_temporal_alpha)), 0.01f, 1.0f);
  constants.denoiser = uint32_t(denoiser);
  constants.world_reset = pt_camera_tracked_ ? 0 : 1;
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t j = 0; j < 3; ++j) {
      constants.view_to_world[i * 4 + j] = float(r[i * 3 + j]);
      constants.view_to_previous_view[i * 4 + j] = float(to_previous_rotation[i * 3 + j]);
    }
    constants.view_to_world[i * 4 + 3] = float(t[i]);
    constants.view_to_previous_view[i * 4 + 3] = float(to_previous_translation[i]);
  }
  constants.nrd_hit_distance_parameters[0] = 3.0f;
  constants.nrd_hit_distance_parameters[1] = 0.1f;
  constants.nrd_hit_distance_parameters[2] = 20.0f;

  // Each set of constants goes into its own slot of the upload.
  uint32_t constant_slot = 0;
  auto set_constants = [&]() {
    assert_true(constant_slot < kPathTracingConstantSlots);
    std::memcpy(upload + constant_slot * kPathTracingConstantSlotSize, &constants,
                sizeof(constants));
    deferred_command_list_.D3DSetComputeRootConstantBufferView(
        UINT(PathTracingRootParameter::kConstants),
        upload_address + constant_slot * kPathTracingConstantSlotSize);
    constant_slot = std::min(constant_slot + 1, kPathTracingConstantSlots - 1);
  };

  ID3D12Resource* vertex_buffer = pt_vertex_buffers_[current].Get();
  ID3D12Resource* previous_vertex_buffer = pt_vertex_buffers_[previous].Get();
  D3D12_GPU_VIRTUAL_ADDRESS vertices = vertex_buffer->GetGPUVirtualAddress();
  D3D12_GPU_VIRTUAL_ADDRESS attributes = pt_attribute_buffer_->GetGPUVirtualAddress();
  D3D12_GPU_VIRTUAL_ADDRESS tlas = pt_tlas_->GetGPUVirtualAddress();
  constexpr auto kNPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto kUAVState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  // The root signature and the arguments shared by the passes after the
  // acceleration structure is built (also restored after external work).
  auto bind_common = [&]() {
    deferred_command_list_.D3DSetComputeRootSignature(pt_root_signature_.Get());
    deferred_command_list_.D3DSetComputeRootShaderResourceView(
        UINT(PathTracingRootParameter::kMaterials), material_upload_address);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(PathTracingRootParameter::kMaterialTextures), material_table);
    deferred_command_list_.D3DSetComputeRootShaderResourceView(
        UINT(PathTracingRootParameter::kBuffer0), tlas);
    deferred_command_list_.D3DSetComputeRootShaderResourceView(
        UINT(PathTracingRootParameter::kBuffer1), vertices);
    deferred_command_list_.D3DSetComputeRootShaderResourceView(
        UINT(PathTracingRootParameter::kAttributes), attributes);
    deferred_command_list_.D3DSetComputeRootShaderResourceView(
        UINT(PathTracingRootParameter::kPreviousVertices),
        previous_vertex_buffer->GetGPUVirtualAddress());
    deferred_command_list_.D3DSetComputeRootUnorderedAccessView(
        UINT(PathTracingRootParameter::kRWBuffer1), pt_stats_buffer_->GetGPUVirtualAddress());
  };

  // Binds the pass's textures (t0... and u0... in space 3). Frame textures are
  // given with their view descriptions.
  struct PassTexture {
    ID3D12Resource* resource;
    const D3D12_SHADER_RESOURCE_VIEW_DESC* srv_desc;
  };
  auto tex = [&](PathTracingTexture texture) -> PassTexture {
    return {PathTracingTextureResource(texture), nullptr};
  };
  auto set_pass = [&](std::initializer_list<PassTexture> srvs,
                      std::initializer_list<PassTexture> uavs) {
    ui::d3d12::util::DescriptorCpuGpuHandlePair start;
    if (!PathTracingAllocateDescriptors(kPathTracingPassTextures * 2, start)) {
      return false;
    }
    uint32_t index = 0;
    auto srv_iterator = srvs.begin();
    for (uint32_t i = 0; i < kPathTracingPassTextures; ++i, ++index) {
      D3D12_CPU_DESCRIPTOR_HANDLE handle = provider.OffsetViewDescriptor(start.first, index);
      D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
      srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv_desc.Texture2D.MipLevels = 1;
      if (srv_iterator != srvs.end() && srv_iterator->resource) {
        if (srv_iterator->srv_desc) {
          srv_desc = *srv_iterator->srv_desc;
        } else {
          srv_desc.Format = srv_iterator->resource->GetDesc().Format;
          PathTracingUseResource(srv_iterator->resource, kNPSR);
        }
        device->CreateShaderResourceView(srv_iterator->resource, &srv_desc, handle);
      } else {
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(nullptr, &srv_desc, handle);
      }
      if (srv_iterator != srvs.end()) {
        ++srv_iterator;
      }
    }
    auto uav_iterator = uavs.begin();
    for (uint32_t i = 0; i < kPathTracingPassTextures; ++i, ++index) {
      D3D12_CPU_DESCRIPTOR_HANDLE handle = provider.OffsetViewDescriptor(start.first, index);
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
      uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      if (uav_iterator != uavs.end() && uav_iterator->resource) {
        uav_desc.Format = uav_iterator->resource->GetDesc().Format;
        PathTracingUseResource(uav_iterator->resource, kUAVState);
        device->CreateUnorderedAccessView(uav_iterator->resource, nullptr, &uav_desc, handle);
      } else {
        uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(nullptr, nullptr, &uav_desc, handle);
      }
      if (uav_iterator != uavs.end()) {
        ++uav_iterator;
      }
    }
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(PathTracingRootParameter::kTextures), start.second);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(PathTracingRootParameter::kRWTextures),
        provider.OffsetViewDescriptor(start.second, kPathTracingPassTextures));
    SubmitBarriers();
    return true;
  };
  PassTexture scene = {scene_texture, &scene_srv_desc};
  PassTexture frame = {swap_texture, &swap_texture_srv_desc};

  deferred_command_list_.D3DSetComputeRootSignature(pt_root_signature_.Get());
  set_constants();
  deferred_command_list_.D3DSetComputeRootShaderResourceView(
      UINT(PathTracingRootParameter::kMaterials), material_upload_address);
  deferred_command_list_.D3DSetComputeRootDescriptorTable(
      UINT(PathTracingRootParameter::kMaterialTextures), material_table);

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

  uint32_t rect_groups_x = (rect_width + 7) / 8;
  uint32_t rect_groups_y = (rect_height + 7) / 8;
  bind_common();
  set_constants();

  // Direction of the game's light.
  if (REXCVAR_GET(path_tracing_game_sun)) {
    SetExternalPipeline(pt_sun_pipeline_.Get());
    SubmitBarriers();
    deferred_command_list_.D3DDispatch(1, 1, 1);
    PushUAVBarrier(pt_stats_buffer_.Get());
  }

  PathTracingTexture gbuffer_texture =
      current ? PathTracingTexture::kGBuffer1 : PathTracingTexture::kGBuffer0;
  PathTracingTexture previous_gbuffer_texture =
      current ? PathTracingTexture::kGBuffer0 : PathTracingTexture::kGBuffer1;
  bool ok = true;

  // Primary surfaces, their colors, motion, the denoiser guides and the
  // background statistics.
  ok = ok && set_pass({scene}, {tex(gbuffer_texture), tex(PathTracingTexture::kAlbedo),
                                tex(PathTracingTexture::kMotion),
                                tex(PathTracingTexture::kViewDepth),
                                tex(PathTracingTexture::kNRDNormalRoughness),
                                tex(PathTracingTexture::kWorldMotion),
                                tex(PathTracingTexture::kScreenMotion),
                                tex(PathTracingTexture::kNormalRoughness),
                                tex(PathTracingTexture::kOctahedralNormal),
                                tex(PathTracingTexture::kDiffuseAlbedo),
                                tex(PathTracingTexture::kSpecularAlbedo)});
  SetExternalPipeline(pt_primary_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // The sky seen in this frame.
  ok = ok && set_pass({scene, tex(gbuffer_texture)}, {{pt_sky_map_.Get(), nullptr}});
  SetExternalPipeline(pt_sky_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch((kPathTracingSkyMapSize + 7) / 8,
                                     (kPathTracingSkyMapSize + 7) / 8, 1);

  // Lighting (this frame's samples).
  PushUAVBarrier(pt_stats_buffer_.Get());
  ok = ok && set_pass({scene, tex(gbuffer_texture), tex(PathTracingTexture::kSpecularAlbedo),
                       {pt_sky_map_.Get(), nullptr}, tex(PathTracingTexture::kNormalRoughness),
                       tex(PathTracingTexture::kHDR), tex(previous_gbuffer_texture)},
                      {tex(PathTracingTexture::kLighting), tex(PathTracingTexture::kSpecular),
                       tex(PathTracingTexture::kShadow)});
  SetExternalPipeline(pt_lighting_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Denoising.
  PathTracingTexture lighting_result = PathTracingTexture::kLighting;
  PathTracingTexture specular_result = PathTracingTexture::kSpecular;
  PathTracingTexture shadow_result = PathTracingTexture::kShadow;
  PathTracingDenoiseInputs denoise = {};
  if (denoiser != PathTracingDenoiser::kBuiltin) {
    denoise.width = rect_width;
    denoise.height = rect_height;
    denoise.reset = denoiser_reset;
    denoise.frame_index = pt_frame_;
    // View to clip mapping view space to this rectangle's UV (see
    // PTProjectToPixel), with depth from near to far in 0...1.
    float k[2], o[2];
    for (uint32_t i = 0; i < 2; ++i) {
      float extent = constants.viewport_extent[i];
      float size = float(i ? rect_height : rect_width);
      k[i] = extent / size;
      o[i] = (2.0f * (constants.viewport_offset[i] - float(rect_min[i])) + extent) / size - 1.0f;
    }
    o[1] = -o[1];
    float near_z = 0.01f, far_z = 10000.0f;
    std::memset(denoise.view_to_clip, 0, sizeof(denoise.view_to_clip));
    denoise.view_to_clip[0] = k[0] * pt_projection_[0] * viewport.ndc_scale[0];
    denoise.view_to_clip[5] = k[1] * pt_projection_[1] * viewport.ndc_scale[1];
    denoise.view_to_clip[8] = k[0] * viewport.ndc_offset[0] + o[0];
    denoise.view_to_clip[9] = k[1] * viewport.ndc_offset[1] + o[1];
    denoise.view_to_clip[10] = far_z / (far_z - near_z);
    denoise.view_to_clip[11] = 1.0f;
    denoise.view_to_clip[14] = -near_z * far_z / (far_z - near_z);
    // World to view: the inverse of the view to world (rotation transposed).
    auto world_to_view = [](const double* rotation, const double* translation, float* m) {
      std::memset(m, 0, sizeof(float) * 16);
      for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column) {
          m[column * 4 + row] = float(rotation[column * 3 + row]);
        }
        m[12 + row] = -float(rotation[row] * translation[0] + rotation[3 + row] * translation[1] +
                             rotation[6 + row] * translation[2]);
      }
      m[15] = 1.0f;
    };
    world_to_view(r, t, denoise.world_to_view);
    if (camera_tracked) {
      world_to_view(rp, tp, denoise.previous_world_to_view);
    } else {
      std::memcpy(denoise.previous_world_to_view, denoise.world_to_view,
                  sizeof(denoise.world_to_view));
    }
    for (uint32_t i = 0; i < 3; ++i) {
      denoise.camera_position_delta[i] = camera_tracked ? float(tp[i] - t[i]) : 0.0f;
      denoise.sun_direction_world[i] = pt_sun_direction_world_[i];
      denoise.sun_color[i] = constants.sun_color[i];
      denoise.hit_distance_parameters[i] = constants.nrd_hit_distance_parameters[i];
    }
    denoise.sun_tan_angular_radius = constants.sun_softness;
    denoise.denoising_range = constants.max_distance * 1.01f;
    uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
    denoise.frame_time_ms =
        pt_last_render_time_ ? std::clamp(float(now - pt_last_render_time_) * 0.001f, 1.0f, 100.0f)
                             : 16.667f;
    pt_last_render_time_ = now;
    denoise.view_depth = PathTracingTextureResource(PathTracingTexture::kViewDepth);
    denoise.nrd_normal_roughness =
        PathTracingTextureResource(PathTracingTexture::kNRDNormalRoughness);
    denoise.world_motion = PathTracingTextureResource(PathTracingTexture::kWorldMotion);
    denoise.screen_motion = PathTracingTextureResource(PathTracingTexture::kScreenMotion);
    denoise.normal_roughness = PathTracingTextureResource(PathTracingTexture::kNormalRoughness);
    denoise.octahedral_normal = PathTracingTextureResource(PathTracingTexture::kOctahedralNormal);
    denoise.diffuse_albedo = PathTracingTextureResource(PathTracingTexture::kDiffuseAlbedo);
    denoise.specular_albedo = PathTracingTextureResource(PathTracingTexture::kSpecularAlbedo);
    denoise.diffuse_signal = PathTracingTextureResource(PathTracingTexture::kLighting);
    denoise.specular_signal = PathTracingTextureResource(PathTracingTexture::kSpecular);
    denoise.shadow_signal = PathTracingTextureResource(PathTracingTexture::kShadow);
    denoise.color = PathTracingTextureResource(PathTracingTexture::kRRColor);
    denoise.diffuse_output = PathTracingTextureResource(PathTracingTexture::kDenoisedDiffuse);
    denoise.specular_output = PathTracingTextureResource(PathTracingTexture::kDenoisedSpecular);
    denoise.shadow_output = PathTracingTextureResource(PathTracingTexture::kDenoisedShadow);
    denoise.color_output = PathTracingTextureResource(PathTracingTexture::kRROutput);
  }
  bool denoised = false;
  if (denoiser == PathTracingDenoiser::kNRD || denoiser == PathTracingDenoiser::kFSRRR) {
    PathTracingRestoreResourceStates();
    SubmitBarriers();
    denoised = denoiser == PathTracingDenoiser::kNRD ? PathTracingDenoiseNRD(denoise)
                                                     : PathTracingDenoiseFSRRR(denoise);
    PathTracingRestoreResourceStates();
    SubmitBarriers();
    bind_common();
    if (denoised) {
      lighting_result = PathTracingTexture::kDenoisedDiffuse;
      specular_result = PathTracingTexture::kDenoisedSpecular;
      shadow_result = PathTracingTexture::kDenoisedShadow;
    } else if (denoiser == PathTracingDenoiser::kNRD) {
      // The noisy signals are packed for NRD, but the visibility isn't.
      constants.denoiser = uint32_t(PathTracingDenoiser::kFSRRR);
    }
  } else if (denoiser == PathTracingDenoiser::kDLSSRR) {
    set_constants();
    ok = ok && set_pass({scene, tex(gbuffer_texture), tex(PathTracingTexture::kAlbedo),
                         tex(PathTracingTexture::kLighting), tex(PathTracingTexture::kSpecular)},
                        {tex(PathTracingTexture::kRRColor)});
    SetExternalPipeline(pt_compose_pipeline_.Get());
    SubmitBarriers();
    deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);
    PathTracingRestoreResourceStates();
    SubmitBarriers();
    denoised = PathTracingDenoiseDLSSRR(denoise);
    PathTracingRestoreResourceStates();
    SubmitBarriers();
    bind_common();
    if (denoised) {
      lighting_result = PathTracingTexture::kRROutput;
    } else {
      // Resolve the noisy color like the built-in path would.
      constants.denoiser = uint32_t(PathTracingDenoiser::kBuiltin);
    }
  } else {
    // Built-in: temporal accumulation into the current history, then an
    // edge-aware spatial filter.
    PathTracingTexture irradiance_history =
        current ? PathTracingTexture::kIrradianceHistory1 : PathTracingTexture::kIrradianceHistory0;
    PathTracingTexture previous_irradiance_history =
        current ? PathTracingTexture::kIrradianceHistory0 : PathTracingTexture::kIrradianceHistory1;
    PathTracingTexture specular_history =
        current ? PathTracingTexture::kSpecularHistory1 : PathTracingTexture::kSpecularHistory0;
    PathTracingTexture previous_specular_history =
        current ? PathTracingTexture::kSpecularHistory0 : PathTracingTexture::kSpecularHistory1;
    ok = ok && set_pass({tex(PathTracingTexture::kMotion), tex(gbuffer_texture),
                         tex(PathTracingTexture::kLighting), tex(previous_gbuffer_texture),
                         tex(previous_irradiance_history), tex(PathTracingTexture::kSpecular),
                         tex(previous_specular_history)},
                        {tex(irradiance_history), tex(specular_history)});
    SetExternalPipeline(pt_temporal_pipeline_.Get());
    SubmitBarriers();
    deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);
    struct FilterPass {
      PathTracingTexture source;
      PathTracingTexture destination;
      uint32_t step;
    };
    FilterPass filter_passes[] = {
        {irradiance_history, PathTracingTexture::kLightingTemp, 1},
        {PathTracingTexture::kLightingTemp, PathTracingTexture::kLighting, 2},
        {specular_history, PathTracingTexture::kSpecular, 1},
    };
    SetExternalPipeline(pt_denoise_pipeline_.Get());
    for (const FilterPass& pass : filter_passes) {
      constants.filter_radius = 2;
      constants.filter_step = pass.step;
      set_constants();
      ok = ok && set_pass({tex(gbuffer_texture), tex(pass.source)}, {tex(pass.destination)});
      deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);
    }
  }

  // Exposed HDR color.
  set_constants();
  PushUAVBarrier(pt_stats_buffer_.Get());
  ok = ok && set_pass({tex(gbuffer_texture), tex(PathTracingTexture::kAlbedo),
                       tex(PathTracingTexture::kSpecularAlbedo), tex(lighting_result),
                       tex(specular_result), tex(shadow_result),
                       tex(PathTracingTexture::kNormalRoughness)},
                      {tex(PathTracingTexture::kHDR)});
  SetExternalPipeline(pt_resolve_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(rect_groups_x, rect_groups_y, 1);

  // Bloom: bright part at a quarter resolution, blurred horizontally and
  // vertically.
  uint32_t bloom_groups_x = ((rect_width + 3) / 4 + 7) / 8;
  uint32_t bloom_groups_y = ((rect_height + 3) / 4 + 7) / 8;
  PathTracingTexture bloom_passes[][2] = {
      {PathTracingTexture::kHDR, PathTracingTexture::kBloomA},
      {PathTracingTexture::kBloomA, PathTracingTexture::kBloomB},
      {PathTracingTexture::kBloomB, PathTracingTexture::kBloomA},
  };
  SetExternalPipeline(pt_bloom_pipeline_.Get());
  for (uint32_t i = 0; i < uint32_t(rex::countof(bloom_passes)); ++i) {
    constants.filter_step = i;
    set_constants();
    ok = ok && set_pass({tex(bloom_passes[i][0])}, {tex(bloom_passes[i][1])});
    deferred_command_list_.D3DDispatch(bloom_groups_x, bloom_groups_y, 1);
  }

  // The tint the game draws the scene with (fades).
  PushUAVBarrier(pt_stats_buffer_.Get());
  ok = ok && set_pass({scene, frame}, {});
  SetExternalPipeline(pt_tint_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch(1, 1, 1);
  PushUAVBarrier(pt_stats_buffer_.Get());

  // Tone mapping into the whole output.
  set_constants();
  ok = ok && set_pass({scene, frame, tex(gbuffer_texture), tex(PathTracingTexture::kHDR),
                       tex(PathTracingTexture::kAlbedo), tex(PathTracingTexture::kBloomA)},
                      {{pt_output_.Get(), nullptr}});
  SetExternalPipeline(pt_composite_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch((width + 7) / 8, (height + 7) / 8, 1);
  PathTracingRestoreResourceStates();
  PushTransitionBarrier(vertex_buffer, kNPSR, kUAVState);
  PushTransitionBarrier(previous_vertex_buffer, kNPSR, kUAVState);
  PushTransitionBarrier(pt_attribute_buffer_.Get(), kNPSR, kUAVState);

  // The game's light for the denoisers, read back in a later frame.
  {
    uint32_t slot = upload_frame;
    PushTransitionBarrier(pt_stats_buffer_.Get(), kUAVState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    SubmitBarriers();
    deferred_command_list_.D3DCopyBufferRegion(pt_sun_readback_.Get(), slot * 256,
                                               pt_stats_buffer_.Get(),
                                               160 + (pt_frame_ & 1) * 16, 16);
    PushTransitionBarrier(pt_stats_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kUAVState);
    pt_sun_readback_frames_[slot] = frame_current_;
  }

  const std::string& dump_path = REXCVAR_GET(path_tracing_debug_dump);
  if (!dump_path.empty() && pt_frame_ % 600 < 90) {
    // Triangles (both regions), their attributes, the statistics and the
    // draws.
    uint32_t region_size = build_triangles * 36;
    uint32_t attributes_size = build_triangles * kPathTracingAttributeSize;
    uint32_t dump_size = region_size * 2 + attributes_size + 256;
    ID3D12Resource* readback = RequestReadbackBuffer(dump_size);
    if (readback) {
      PushTransitionBarrier(vertex_buffer, kUAVState, D3D12_RESOURCE_STATE_COPY_SOURCE);
      PushTransitionBarrier(pt_attribute_buffer_.Get(), kUAVState,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
      PushTransitionBarrier(pt_stats_buffer_.Get(), kUAVState, D3D12_RESOURCE_STATE_COPY_SOURCE);
      SubmitBarriers();
      deferred_command_list_.D3DCopyBufferRegion(readback, 0, vertex_buffer, 0, region_size);
      deferred_command_list_.D3DCopyBufferRegion(readback, region_size, vertex_buffer,
                                                 kPathTracingVertexRegionSize, region_size);
      deferred_command_list_.D3DCopyBufferRegion(readback, region_size * 2,
                                                 pt_attribute_buffer_.Get(), 0, attributes_size);
      deferred_command_list_.D3DCopyBufferRegion(readback, region_size * 2 + attributes_size,
                                                 pt_stats_buffer_.Get(), 0, 256);
      PushTransitionBarrier(vertex_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, kUAVState);
      PushTransitionBarrier(pt_attribute_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            kUAVState);
      PushTransitionBarrier(pt_stats_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kUAVState);
      void* mapping = nullptr;
      D3D12_RANGE range = {0, dump_size};
      if (AwaitAllQueueOperationsCompletion() && SUCCEEDED(readback->Map(0, &range, &mapping))) {
        std::string file_path = fmt::format("{}_{}.bin", dump_path, pt_frame_);
        if (FILE* file = std::fopen(file_path.c_str(), "wb")) {
          uint32_t header[2] = {build_triangles, uint32_t(pt_draws_.size())};
          std::fwrite(header, sizeof(header), 1, file);
          std::fwrite(mapping, dump_size, 1, file);
          std::fwrite(pt_draws_.data(), sizeof(PathTracingDraw), pt_draws_.size(), file);
          std::fclose(file);
        }
        D3D12_RANGE written = {};
        readback->Unmap(0, &written);
      }
    }
  }

  if (REXCVAR_GET(path_tracing_debug_log) && pt_frame_ % 120 == 0) {
    uint32_t matched = 0;
    for (const PathTracingDraw& draw : pt_draws_) {
      matched += draw.previous_first != UINT32_MAX ? 1 : 0;
    }
    static const char* const kDenoiserNames[] = {"built-in", "NRD", "DLSS Ray Reconstruction",
                                                 "FSR Ray Regeneration"};
    REXGPU_INFO(
        "Path tracing: {} triangles, scene {},{} {}x{}, projection {:.4f} {:.4f}, scene texture "
        "{}, materials {} ({} draws, {} matched with the previous frame, {} textures), denoiser "
        "{}{}, {} descriptors",
        pt_draw_triangles_, rect_min[0], rect_min[1], rect_width, rect_height, pt_projection_[0],
        pt_projection_[1],
        scene_texture != swap_texture ? fmt::format("{:08X}", pt_scene_address_) : "not found",
        materials, pt_draws_.size(), matched, material_textures.size(),
        kDenoiserNames[size_t(denoiser)], denoised || denoiser == PathTracingDenoiser::kBuiltin
                                              ? ""
                                              : " (failed)",
        pt_frame_descriptors_used_);
    REXGPU_INFO(
        "Path tracing: camera {} ({} draws agreeing, {} world resets), position {:.3f} {:.3f} "
        "{:.3f}, sun {:.3f} {:.3f} {:.3f}",
        pt_camera_tracked_ ? "tracked" : "lost", pt_camera_draws_agreeing_, pt_world_resets_,
        t[0], t[1], t[2], pt_sun_direction_world_[0], pt_sun_direction_world_[1],
        pt_sun_direction_world_[2]);
  }
  if (!ok) {
    REXGPU_WARN("Path tracing: out of descriptors");
  }

  // This frame becomes the history.
  pt_history_index_ = current;
  pt_rendered_this_frame_ = true;
  pt_previous_draws_ = pt_draws_;
  pt_previous_draw_keys_ = pt_draw_keys_;
  pt_previous_draw_samples_.swap(pt_draw_samples_);
  pt_previous_draw_sample_ranges_.swap(pt_draw_sample_ranges_);
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
  // Nothing to capture while path tracing is off.
  pt_capture_done_this_frame_ = REXCVAR_GET(path_tracing_denoiser) == "off";
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
