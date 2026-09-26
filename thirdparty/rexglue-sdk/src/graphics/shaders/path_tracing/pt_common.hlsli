// Shared definitions for the experimental path-traced lighting passes.
//
// Built with the Windows SDK DirectX Shader Compiler (shader model 6.5 for
// inline ray queries), see build_pt_shaders.cmd.
//
// View space here is the guest's: x right, y up, z (= clip w) forward, with
// the camera at the origin. World space is fixed to the scenery (tracked from
// the camera motion on the CPU, see UpdatePathTracingCamera).
//
// The working textures cover only the scene's rectangle of the output, with
// "local" pixel coordinates (0, 0 at pt_rect_min), so they can be handed to the
// denoisers as they are. The frame textures (the scene color, the final frame,
// the output) use "output" pixel coordinates.

#ifndef PT_COMMON_HLSLI_
#define PT_COMMON_HLSLI_

cbuffer PTConstants : register(b0) {
  // 1 / projection scale (guest clip xy = view xy * proj).
  float2 pt_inv_proj;
  // Host clip xy = guest clip xy * ndc_scale + ndc_offset * w.
  float2 pt_ndc_scale;
  float2 pt_ndc_offset;
  // Host viewport in output pixels.
  float2 pt_viewport_offset;
  float2 pt_viewport_extent;
  // Output pixels the scene covers, max exclusive.
  uint2 pt_rect_min;
  uint2 pt_rect_max;
  uint2 pt_output_size;
  // Vertices in the acceleration structure (triangle list).
  uint pt_vertex_count;
  // Which half of the statistics buffer this frame accumulates into.
  uint pt_stats_slot;
  float pt_gi_distance;
  // How much of the game's own lighting is replaced (0 to 1).
  float pt_strength;
  float3 pt_sun_color;
  // Multiplier of the light from the background (sky).
  float pt_sky_scale;
  // Sun elevation above the estimated ground plane and azimuth relative to
  // the camera, in radians (if the game's light isn't used).
  float2 pt_sun_angles;
  // Sun cone half-angle tangent, for soft shadows.
  float pt_sun_softness;
  uint pt_ray_count;
  uint pt_debug_view;
  // Specular reflection strength (0 disables the reflection rays).
  float pt_specular;
  // Denoiser pass: distance between taps and taps each side.
  uint pt_filter_step;
  float pt_bounce_scale;
  float pt_ambient;
  // Average brightness auto exposure aims for, relative to the original.
  float pt_exposure_target;
  float pt_shadow_distance;
  uint pt_filter_radius;
  uint pt_flags;
  float pt_sky_saturation;
  // Surfaces further away are scenery (sky, clouds, distant landscape) and
  // are left as they are, like the background.
  float pt_max_distance;
  // GGX linear roughness of the surfaces (alpha = roughness^2).
  float pt_roughness;
  // For varying the sampling between frames.
  uint pt_frame_index;
  // Whether the previous frame's surfaces and lighting can be reused.
  uint pt_history_valid;
  // Smallest weight of the new samples in the built-in temporal accumulation.
  float pt_temporal_alpha;
  float pt_specular_temporal_alpha;
  // kPTDenoiser*.
  uint pt_denoiser;
  // The world space restarted this frame (the camera couldn't be tracked).
  uint pt_world_reset;
  uint2 pt_padding0;
  // Rows of the view to world rotation (xyz).
  float4 pt_view_to_world[3];
  // Rows of the transform from this frame's view space to the previous
  // frame's (rotation xyz, translation w) for static geometry.
  float4 pt_view_to_previous_view[3];
  // REBLUR hit distance normalization (A, B, C).
  float4 pt_nrd_hit_distance_parameters;
};

// pt_flags.
static const uint kPTFlagReplaceGameShadows = 1u << 0;
// The sun comes from the direction of the game's light (see pt_sun).
static const uint kPTFlagGameSun = 1u << 1;
// The previous frame's view is related to this one's (pt_view_to_previous_view
// is valid).
static const uint kPTFlagCameraTracked = 1u << 2;

// pt_denoiser.
static const uint kPTDenoiserBuiltin = 0;
static const uint kPTDenoiserNRD = 1;
static const uint kPTDenoiserDLSSRR = 2;
static const uint kPTDenoiserFSRRR = 3;

// Whether the lighting is traced as separate signals (sun visibility,
// indirect diffuse, indirect specular) for a denoiser working on them, rather
// than as total irradiance and specular.
bool PTSplitSignals() {
  return pt_denoiser == kPTDenoiserNRD || pt_denoiser == kPTDenoiserFSRRR;
}

// Statistics buffer, per frame slot:
// +0 int3 - sum of up-facing triangle normals (ground plane estimate).
// +16 uint3 - sum of background (no geometry) pixel colors, x255.
// +28 uint - background pixel count.
// +32 uint - sum of lighting luminance, x32.
// +36 uint - lit pixel count.
// Not cleared between frames, from +128, per frame slot:
// +0 float3 - sky light color smoothed over time.
// From +160, per frame slot:
// +0 float4 - direction of the game's light in world space, and how well it
// explains the game's lighting (0 if not found yet).
static const uint kPTStatsSlotSize = 64;
static const uint kPTStatsSkyOffset = 128;
static const uint kPTStatsSunOffset = 160;
static const float kPTStatsScale = 4096.0;

static const float3 kPTLuminance = float3(0.2126, 0.7152, 0.0722);
static const float kPTPi = 3.14159265359;
// View depth of pixels without scene geometry for the denoisers.
static const float kPTBackgroundDepth = 1.0e5;
static const float kPTFP16Max = 65504.0;

uint2 PTRectSize() { return pt_rect_max - pt_rect_min; }

bool PTInRect(int2 local) { return all(local >= 0) && all(local < int2(PTRectSize())); }

// Ground normal from the up-facing triangle normal sums.
float3 PTGroundUp(uint3 sum_bits) {
  float3 up = float3(asint(sum_bits)) * (1.0 / kPTStatsScale);
  float up_length = length(up);
  return up_length > 1.0e-3 ? up / up_length : float3(0.0, 1.0, 0.0);
}

float3 PTViewToWorld(float3 v) {
  return float3(dot(pt_view_to_world[0].xyz, v), dot(pt_view_to_world[1].xyz, v),
                dot(pt_view_to_world[2].xyz, v));
}

float3 PTWorldToView(float3 v) {
  return pt_view_to_world[0].xyz * v.x + pt_view_to_world[1].xyz * v.y +
         pt_view_to_world[2].xyz * v.z;
}

// Where a static point of this frame's view space was in the previous frame's.
float3 PTViewToPreviousView(float3 p) {
  return float3(dot(pt_view_to_previous_view[0].xyz, p) + pt_view_to_previous_view[0].w,
                dot(pt_view_to_previous_view[1].xyz, p) + pt_view_to_previous_view[1].w,
                dot(pt_view_to_previous_view[2].xyz, p) + pt_view_to_previous_view[2].w);
}

// The inverse: a point of the previous frame's view space in this frame's
// (static).
float3 PTPreviousViewToView(float3 p) {
  float3 d = p - float3(pt_view_to_previous_view[0].w, pt_view_to_previous_view[1].w,
                        pt_view_to_previous_view[2].w);
  return pt_view_to_previous_view[0].xyz * d.x + pt_view_to_previous_view[1].xyz * d.y +
         pt_view_to_previous_view[2].xyz * d.z;
}

// View space direction (with z = 1, so the ray distance is the view depth)
// through a point in output pixels.
float3 PTPixelRay(float2 pixel) {
  float2 host_ndc =
      float2((pixel.x - pt_viewport_offset.x) / pt_viewport_extent.x * 2.0 - 1.0,
             1.0 - (pixel.y - pt_viewport_offset.y) / pt_viewport_extent.y * 2.0);
  float2 guest_ndc = (host_ndc - pt_ndc_offset) / pt_ndc_scale;
  return float3(guest_ndc * pt_inv_proj, 1.0);
}

// Output pixel position of a view space point in front of the camera.
float2 PTProjectToPixel(float3 p) {
  float2 guest_ndc = p.xy / (pt_inv_proj * p.z);
  float2 host_ndc = guest_ndc * pt_ndc_scale + pt_ndc_offset;
  return float2(pt_viewport_offset.x + (host_ndc.x * 0.5 + 0.5) * pt_viewport_extent.x,
                pt_viewport_offset.y + (0.5 - host_ndc.y * 0.5) * pt_viewport_extent.y);
}

// Random numbers: a hash per pixel and frame (uncorrelated between pixels and
// frames, as the denoisers expect), advanced per sample.
uint PTHash(uint x) {
  // PCG output permutation.
  uint state = x * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

uint PTRandomSeed(uint2 pixel, uint stream) {
  return PTHash(pixel.x + PTHash(pixel.y + PTHash(pt_frame_index * 4u + stream)));
}

float PTRandom(inout uint state) {
  state = PTHash(state);
  return float(state >> 8) * (1.0 / 16777216.0);
}

float2 PTRandom2(inout uint state) {
  float x = PTRandom(state);
  return float2(x, PTRandom(state));
}

void PTBasis(float3 n, out float3 t, out float3 b) {
  // Duff et al., "Building an Orthonormal Basis, Revisited".
  float s = n.z >= 0.0 ? 1.0 : -1.0;
  float a = -1.0 / (s + n.z);
  float c = n.x * n.y * a;
  t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
  b = float3(c, s + n.y * n.y * a, -n.y);
}

// Edge-stopping weight between two G-buffer samples (normal xyz, depth w).
float PTSurfaceWeight(float4 center, float4 tap) {
  if (tap.w <= 0.0) {
    return 0.0;
  }
  float n_dot_n = saturate(dot(tap.xyz, center.xyz));
  float n_weight = n_dot_n * n_dot_n;
  n_weight *= n_weight;
  n_weight *= n_weight;
  return exp(-abs(tap.w - center.w) / (0.02 * center.w + 1.0e-3)) * n_weight;
}

float3 PTLinear(float3 color) { return pow(max(color, 0.0), 2.2); }

// Schlick's approximation for a dielectric (F0 = 0.04).
float PTFresnel(float cosine) {
  float f = 1.0 - saturate(cosine);
  float f2 = f * f;
  return 0.04 + 0.96 * f2 * f2 * f;
}

// Smith G1 for GGX.
float PTSmithG1(float n_dot_x, float alpha2) {
  return 2.0 * n_dot_x / (n_dot_x + sqrt(alpha2 + (1.0 - alpha2) * n_dot_x * n_dot_x));
}

// Directional albedo of the specular lobe (pre-integrated Fresnel and
// geometry, "Ray Tracing Gems" chapter 32, as in the DLSS-RR guide), for
// dividing the specular signal by before denoising and multiplying after.
float3 PTSpecularAlbedo(float3 specular_color, float alpha, float n_dot_v) {
  n_dot_v = abs(n_dot_v);
  float4 x = float4(1.0, n_dot_v, n_dot_v * n_dot_v, n_dot_v * n_dot_v * n_dot_v);
  float4 y = float4(1.0, alpha, alpha * alpha, alpha * alpha * alpha);
  float2x2 m1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
  float3x3 m2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13,
                         316.627);
  float2x2 m3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
  float3x3 m4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886,
                         -20.2123);
  float bias = dot(mul(m1, x.xy), y.xy) * rcp(dot(mul(m2, x.xyw), y.xyw));
  float scale = dot(mul(m3, x.xy), y.xy) * rcp(dot(mul(m4, x.xzw), y.xyw));
  bias *= saturate(specular_color.g * 50.0);
  return specular_color * max(0.0, scale) + max(0.0, bias);
}

// GGX highlight of a directional light (without the light's color): the BRDF
// times n.l.
float PTSpecularHighlight(float3 normal, float3 view, float3 light, float roughness) {
  float n_dot_l = dot(normal, light);
  float n_dot_v = max(dot(normal, view), 1.0e-3);
  if (n_dot_l <= 0.0) {
    return 0.0;
  }
  float alpha = roughness * roughness;
  float alpha2 = alpha * alpha;
  float3 half_vector = normalize(light + view);
  float n_dot_h = saturate(dot(normal, half_vector));
  float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
  float distribution = alpha2 / (kPTPi * d * d);
  float geometry = PTSmithG1(n_dot_l, alpha2) * PTSmithG1(n_dot_v, alpha2);
  return distribution * geometry * PTFresnel(dot(view, half_vector)) / (4.0 * n_dot_v);
}

// Octahedral normal encoding (FSR Ray Regeneration).
float2 PTNormalToOctahedron(float3 n) {
  n.xy /= abs(n.x) + abs(n.y) + abs(n.z);
  float2 k = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
  float s = saturate(-n.z);
  n.xy = lerp(n.xy, (1.0 - abs(n.yx)) * k, s);
  return n.xy * 0.5 + 0.5;
}

// The sun direction in view space: the game's light (tracked in world space
// by pt_sun) or the configured angles relative to the camera.
float3 PTSunDirection(uint4 stats_state, float3 up) {
  float3 forward_flat = float3(0.0, 0.0, 1.0) - up * up.z;
  forward_flat = dot(forward_flat, forward_flat) > 1.0e-6 ? normalize(forward_flat)
                                                          : float3(0.0, 0.0, 1.0);
  float3 right_flat = cross(up, forward_flat);
  float elevation = pt_sun_angles.x, azimuth = pt_sun_angles.y;
  float3 sun = up * sin(elevation) +
               (-forward_flat * cos(azimuth) + right_flat * sin(azimuth)) * cos(elevation);
  float4 game_sun = asfloat(stats_state);
  if ((pt_flags & kPTFlagGameSun) && game_sun.w > 0.0) {
    // Where the game's light comes from, kept above the horizon.
    float3 game_direction = normalize(PTWorldToView(game_sun.xyz));
    float height = dot(game_direction, up);
    float min_height = sin(0.2);
    if (height < min_height) {
      float3 flat = game_direction - up * height;
      flat = dot(flat, flat) > 1.0e-6 ? normalize(flat) : forward_flat;
      game_direction = flat * cos(0.2) + up * min_height;
    }
    sun = game_direction;
  }
  return sun;
}

#endif  // PT_COMMON_HLSLI_
