/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/d3d12/primitive_processor.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/d3d12/d3d12_descriptor_heap_pool.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_upload_buffer_pool.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor : public CommandProcessor {
 public:
  explicit D3D12CommandProcessor(D3D12GraphicsSystem* graphics_system,
                                 system::KernelState* kernel_state);
  ~D3D12CommandProcessor();

  void ClearCaches() override;
  void InvalidateGpuMemory() override;

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  ui::d3d12::D3D12Provider& GetD3D12Provider() const {
    return *static_cast<ui::d3d12::D3D12Provider*>(graphics_system_->provider());
  }

  // Returns the deferred drawing command list for the currently open
  // submission.
  DeferredCommandList& GetDeferredCommandList() {
    assert_true(submission_open_);
    return deferred_command_list_;
  }

  uint64_t GetCurrentSubmission() const { return submission_current_; }
  uint64_t GetCompletedSubmission() const { return submission_completed_; }

  // Must be called when a subsystem does something like UpdateTileMappings so
  // it can be awaited in CheckSubmissionFence(submission_current_) if it was
  // done after the latest ExecuteCommandLists + Signal.
  void NotifyQueueOperationsDoneDirectly() {
    queue_operations_done_since_submission_signal_ = true;
  }

  uint64_t GetCurrentFrame() const { return frame_current_; }
  uint64_t GetCompletedFrame() const { return frame_completed_; }

  // Returns true if the barrier has been inserted (the new state is different).
  bool PushTransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES old_state,
                             D3D12_RESOURCE_STATES new_state,
                             UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
  void PushAliasingBarrier(ID3D12Resource* old_resource, ID3D12Resource* new_resource);
  void PushUAVBarrier(ID3D12Resource* resource);

  // Whether guest pipelines should capture geometry for path tracing (the
  // path_tracing option is on and the device supports it).
  bool IsPathTracingEnabled() const { return pt_capture_buffer_ != nullptr; }
  // Whether the draw should render unlit surface colors for the path tracer
  // to light (path_tracing_albedo_shader).
  bool IsPathTracingAlbedoDraw(const Shader& pixel_shader,
                               reg::RB_DEPTHCONTROL normalized_depth_control);

  void SubmitBarriers();

  // Finds or creates root signature for a pipeline.
  ID3D12RootSignature* GetRootSignature(const DxbcShader* vertex_shader,
                                        const DxbcShader* pixel_shader, bool tessellated);

  ui::d3d12::D3D12UploadBufferPool& GetConstantBufferPool() const { return *constant_buffer_pool_; }

  D3D12_CPU_DESCRIPTOR_HANDLE GetViewBindlessHeapCPUStart() const {
    assert_true(bindless_resources_used_);
    return view_bindless_heap_cpu_start_;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE GetViewBindlessHeapGPUStart() const {
    assert_true(bindless_resources_used_);
    return view_bindless_heap_gpu_start_;
  }
  // Returns UINT32_MAX if no free descriptors. If the unbounded SRV range for
  // bindless resources is also used in the root signature of the draw /
  // dispatch referencing this descriptor, this must only be used to allocate
  // SRVs, otherwise it won't work on Nvidia Fermi (root signature creation will
  // fail)!
  uint32_t RequestPersistentViewBindlessDescriptor();
  void ReleaseViewBindlessDescriptorImmediately(uint32_t descriptor_index);
  // Request non-contiguous CBV/SRV/UAV descriptors for use only within the next
  // draw or dispatch command done for internal purposes. May change the current
  // descriptor heap. If the unbounded SRV range for bindless resources is also
  // used in the root signature of the draw / dispatch referencing these
  // descriptors, this must only be used to allocate SRVs, otherwise it won't
  // work on Nvidia Fermi (root signature creation will fail)!
  bool RequestOneUseSingleViewDescriptors(uint32_t count,
                                          ui::d3d12::util::DescriptorCpuGpuHandlePair* handles_out);
  // These are needed often, so they are always allocated.
  enum class SystemBindlessView : uint32_t {
    // Both may be bound as one root parameter.
    kSharedMemoryRawSRVAndNullRawUAVStart,
    kSharedMemoryRawSRV = kSharedMemoryRawSRVAndNullRawUAVStart,
    kNullRawUAV,

    // Both may be bound as one root parameter.
    kNullRawSRVAndSharedMemoryRawUAVStart,
    kNullRawSRV = kNullRawSRVAndSharedMemoryRawUAVStart,
    kSharedMemoryRawUAV,

    kSharedMemoryR32UintSRV,
    kSharedMemoryR32G32UintSRV,
    kSharedMemoryR32G32B32A32UintSRV,
    kSharedMemoryR32UintUAV,
    kSharedMemoryR32G32UintUAV,
    kSharedMemoryR32G32B32A32UintUAV,

    kEdramRawSRV,
    kEdramR32UintSRV,
    kEdramR32G32UintSRV,
    kEdramR32G32B32A32UintSRV,
    kEdramRawUAV,
    kEdramR32UintUAV,
    kEdramR32G32UintUAV,
    kEdramR32G32B32A32UintUAV,

    kGammaRampTableSRV,
    kGammaRampPWLSRV,

    // Beyond this point, SRVs are accessible to shaders through an unbounded
    // range - no descriptors of other types bound to shaders alongside
    // unbounded ranges - must be located beyond this point.
    kUnboundedSRVsStart,
    kNullTexture2DArray = kUnboundedSRVsStart,
    kNullTexture3D,
    kNullTextureCube,

    kCount,
  };
  ui::d3d12::util::DescriptorCpuGpuHandlePair GetSystemBindlessViewHandlePair(
      SystemBindlessView view) const;
  ui::d3d12::util::DescriptorCpuGpuHandlePair GetSharedMemoryUintPow2BindlessSRVHandlePair(
      uint32_t element_size_bytes_pow2) const;
  ui::d3d12::util::DescriptorCpuGpuHandlePair GetSharedMemoryUintPow2BindlessUAVHandlePair(
      uint32_t element_size_bytes_pow2) const;
  ui::d3d12::util::DescriptorCpuGpuHandlePair GetEdramUintPow2BindlessSRVHandlePair(
      uint32_t element_size_bytes_pow2) const;
  ui::d3d12::util::DescriptorCpuGpuHandlePair GetEdramUintPow2BindlessUAVHandlePair(
      uint32_t element_size_bytes_pow2) const;

  // Returns a single temporary GPU-side buffer within a submission for tasks
  // like texture untiling and resolving.
  ID3D12Resource* RequestScratchGPUBuffer(uint32_t size, D3D12_RESOURCE_STATES state);
  // This must be called when done with the scratch buffer, to notify the
  // command processor about the new state in case the buffer was transitioned
  // by its user.
  void ReleaseScratchGPUBuffer(ID3D12Resource* buffer, D3D12_RESOURCE_STATES new_state);

  // Returns a pipeline with deferred creation by its handle. May return nullptr
  // if failed to create the pipeline.
  ID3D12PipelineState* GetD3D12PipelineByHandle(void* handle) const {
    return pipeline_cache_->GetD3D12PipelineByHandle(handle);
  }

  // Sets the current cached values to external ones. This is for cache
  // invalidation primarily. A submission must be open.
  void SetExternalPipeline(ID3D12PipelineState* pipeline);
  void SetExternalGraphicsRootSignature(ID3D12RootSignature* root_signature);
  void SetViewport(const D3D12_VIEWPORT& viewport);
  void SetScissorRect(const D3D12_RECT& scissor_rect);
  void SetStencilReference(uint32_t stencil_ref);
  void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology);

  // Returns the text to display in the GPU backend name in the window title.
  std::string GetWindowTitleText() const;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count) override;

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  void OnPrimaryBufferEnd() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  static constexpr uint32_t kQueueFrames = 3;

  enum RootParameter : UINT {
    // Keep the size of the root signature at each stage 13 dwords or less
    // (better 12 or less) so it fits in user data on AMD. Descriptor tables are
    // 1 dword, root descriptors are 2 dwords (however, root descriptors require
    // less setup on the CPU - balance needs to be maintained).

    // CBVs are set in both bindful and bindless cases via root descriptors.

    // - Bindful resources - multiple root signatures depending on extra
    //   parameters.

    // These are always present.

    // Very frequently changed, especially for UI draws, and for models drawn in
    // multiple parts - contains vertex and texture fetch constants.
    kRootParameter_Bindful_FetchConstants = 0,  // +2 dwords = 2 in all.
    // Quite frequently changed (for one object drawn multiple times, for
    // instance - may contain projection matrices).
    kRootParameter_Bindful_FloatConstantsVertex,  // +2 = 4 in VS.
    // Less frequently changed (per-material).
    kRootParameter_Bindful_FloatConstantsPixel,  // +2 = 4 in PS.
    // May stay the same across many draws.
    kRootParameter_Bindful_SystemConstants,  // +2 = 6 in all.
    // Pretty rarely used and rarely changed - flow control constants.
    kRootParameter_Bindful_BoolLoopConstants,  // +2 = 8 in all.
    // Changed only when starting a new descriptor heap or when switching
    // between shared memory as SRV and UAV - shared memory byte address buffer
    // (as SRV and as UAV, either may be null if not used), and, if ROV is used
    // for EDRAM, EDRAM R32_UINT UAV.
    kRootParameter_Bindful_SharedMemoryAndEdram,  // +1 = 9 in all.

    kRootParameter_Bindful_Count_Base,

    // Extra parameter that may or may not exist:
    // - Pixel textures (+1 = 10 in PS).
    // - Pixel samplers (+1 = 11 in PS).
    // - Vertex textures (+1 = 10 in VS).
    // - Vertex samplers (+1 = 11 in VS).

    kRootParameter_Bindful_Count_Max = kRootParameter_Bindful_Count_Base + 4,

    // - Bindless resources - two global root signatures (for non-tessellated
    //   and tessellated drawing), so these are always present.

    kRootParameter_Bindless_FetchConstants = 0,    // +2 = 2 in all.
    kRootParameter_Bindless_FloatConstantsVertex,  // +2 = 4 in VS.
    kRootParameter_Bindless_FloatConstantsPixel,   // +2 = 4 in PS.
    // Changed per-material, texture and sampler descriptor indices.
    kRootParameter_Bindless_DescriptorIndicesPixel,   // +2 = 6 in PS.
    kRootParameter_Bindless_DescriptorIndicesVertex,  // +2 = 6 in VS.
    kRootParameter_Bindless_SystemConstants,          // +2 = 8 in all.
    kRootParameter_Bindless_BoolLoopConstants,        // +2 = 10 in all.
    // Changed only when switching between shared memory as SRV and UAV - shared
    // memory byte address buffer (as SRV and as UAV, either may be null if not
    // used).
    kRootParameter_Bindless_SharedMemory,  // +1 = 11 in all.
    // Unbounded sampler descriptor table - changed in case of overflow.
    kRootParameter_Bindless_SamplerHeap,  // +1 = 12 in all.
    // Unbounded SRV/UAV descriptor table - never changed.
    kRootParameter_Bindless_ViewHeap,  // +1 = 13 in all.

    kRootParameter_Bindless_Count,
  };

  struct RootBindfulExtraParameterIndices {
    uint32_t textures_pixel;
    uint32_t samplers_pixel;
    uint32_t textures_vertex;
    uint32_t samplers_vertex;
    static constexpr uint32_t kUnavailable = UINT32_MAX;
  };
  // Gets the indices of optional root parameters. Returns the total parameter
  // count.
  static uint32_t GetRootBindfulExtraParameterIndices(
      const DxbcShader* vertex_shader, const DxbcShader* pixel_shader,
      RootBindfulExtraParameterIndices& indices_out);

  // BeginSubmission and EndSubmission may be called at any time. If there's an
  // open non-frame submission, BeginSubmission(true) will promote it to a
  // frame. EndSubmission(true) will close the frame no matter whether the
  // submission has already been closed.
  // Submission (ExecuteCommandLists) boundaries are implicit full UAV and
  // aliasing barriers, and also result in common resource state promotion and
  // decay.

  // Rechecks submission number and reclaims per-submission resources. Pass 0 as
  // the submission to await to simply check status, or pass submission_current_
  // to wait for all queue operations to be completed.
  void CheckSubmissionFence(uint64_t await_submission);
  // If is_guest_command is true, a new full frame - with full cleanup of
  // resources and, if needed, starting capturing - is opened if pending (as
  // opposed to simply resuming after mid-frame synchronization). Returns
  // whether a submission is open currently and the device is not removed.
  bool BeginSubmission(bool is_guest_command);
  // If is_swap is true, a full frame is closed - with, if needed, cache
  // clearing and stopping capturing. Returns whether the submission was done
  // successfully, if it has failed, leaves it open.
  bool EndSubmission(bool is_swap);
  // Checks if ending a submission right now would not cause potentially more
  // delay than it would reduce by making the GPU start working earlier - such
  // as when there are unfinished graphics pipeline creation requests that would
  // need to be fulfilled before actually submitting the command list.
  bool CanEndSubmissionImmediately() const;
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionFence(submission_current_);
    return submission_completed_ + 1 >= submission_current_;
  }
  void LogDeviceRemovalDiagnostics(ID3D12Device* device, HRESULT reason);

  void UpdateDebugMarkersEnabled();
  void PushDebugMarker(const char* format, ...);
  void PopDebugMarker();
  void InsertDebugMarker(const char* format, ...);
  bool debug_markers_enabled() const { return debug_markers_enabled_; }

  // Need to await submission completion before calling.
  void ClearCommandAllocatorCache();

  // Request descriptors and automatically rebind the descriptor heap on the
  // draw command list. Refer to D3D12DescriptorHeapPool::Request for partial /
  // full update explanation. Doesn't work when bindless descriptors are used.
  uint64_t RequestViewBindfulDescriptors(uint64_t previous_heap_index,
                                         uint32_t count_for_partial_update,
                                         uint32_t count_for_full_update,
                                         D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
                                         D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out);
  uint64_t RequestSamplerBindfulDescriptors(uint64_t previous_heap_index,
                                            uint32_t count_for_partial_update,
                                            uint32_t count_for_full_update,
                                            D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
                                            D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out);

  void UpdateFixedFunctionState(const draw_util::ViewportInfo& viewport_info,
                                const draw_util::Scissor& scissor, bool primitive_polygonal,
                                reg::RB_DEPTHCONTROL normalized_depth_control);
  void UpdateSystemConstantValues(bool shared_memory_is_uav, bool primitive_polygonal,
                                  uint32_t line_loop_closing_index, xenos::Endian index_endian,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask);
  bool UpdateBindings(const D3D12Shader* vertex_shader, const D3D12Shader* pixel_shader,
                      ID3D12RootSignature* root_signature, bool shared_memory_is_uav);
  bool IssueCopy_ReadbackResolvePath();
  bool IssueDraw_MemexportReadbackFullPath(uint32_t total_size);
  bool IssueDraw_MemexportReadbackFastPath(uint32_t total_size);

  // Returns a buffer for reading GPU data back to the CPU. Assuming
  // synchronizing immediately after use. Always in COPY_DEST state.
  ID3D12Resource* RequestReadbackBuffer(uint32_t size);
  struct ReadbackBuffer {
    ID3D12Resource* buffers[2] = {nullptr, nullptr};
    uint32_t sizes[2] = {0, 0};
    void* mapped_data[2] = {nullptr, nullptr};
    uint64_t submission_written[2] = {0, 0};
    uint32_t written_size[2] = {0, 0};
    uint32_t current_index = 0;
    uint64_t last_used_frame = 0;
  };
  void EvictOldReadbackBuffers(std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map);
  static constexpr uint32_t kReadbackBufferSizeIncrement = 16 * 1024 * 1024;
  static constexpr size_t kMaxReadbackBuffers = 256;
  static constexpr uint64_t kReadbackBufferEvictionAgeFrames = 60;
  static inline uint32_t AlignReadbackBufferSize(uint32_t size) {
    if (size < 1 * 1024 * 1024) {
      return rex::align(size, 256u * 1024u);
    }
    if (size < 4 * 1024 * 1024) {
      return rex::align(size, 1u * 1024u * 1024u);
    }
    return rex::align(size, kReadbackBufferSizeIncrement);
  }
  static inline uint64_t MakeReadbackResolveKey(uint32_t address, uint32_t length) {
    return (uint64_t(address) << 32) | uint64_t(length);
  }
  static inline uint64_t MakeMemexportReadbackKey(uint32_t first_base_address_dwords,
                                                  uint32_t total_size) {
    return (uint64_t(first_base_address_dwords) << 32) | uint64_t(total_size);
  }

  bool InitializeOcclusionQueryResources();
  void ShutdownOcclusionQueryResources();
  bool BeginGuestOcclusionQuery(uint32_t sample_count_address);
  bool EndGuestOcclusionQuery(uint32_t sample_count_address,
                              xenos::xe_gpu_depth_sample_counts* sample_counts);
  bool AcquireOcclusionQueryIndex(uint32_t& host_index_out);
  void DisableHostOcclusionQueries();
  uint64_t NormalizeOcclusionSamples(uint64_t samples) const;
  void WriteGuestOcclusionResult(xenos::xe_gpu_depth_sample_counts* sample_counts,
                                 uint64_t samples);
  void InvalidateAllVertexBufferResidency();
  void InvalidateVertexBufferResidency(uint32_t vfetch_index);
  void InvalidateVertexBufferResidencyRange(uint32_t first_vfetch, uint32_t last_vfetch);

  void WriteGammaRampSRV(bool is_pwl, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

  bool device_removed_ = false;

  bool cache_clear_requested_ = false;

  HANDLE fence_completion_event_ = nullptr;

  bool submission_open_ = false;
  // Values of submission_fence_.
  uint64_t submission_current_ = 1;
  uint64_t submission_completed_ = 0;
  ID3D12Fence* submission_fence_ = nullptr;

  // For awaiting non-submission queue operations such as UpdateTileMappings in
  // AwaitAllQueueOperationsCompletion when they're queued after the latest
  // ExecuteCommandLists + Signal, thus won't be awaited by just awaiting the
  // submission.
  ID3D12Fence* queue_operations_since_submission_fence_ = nullptr;
  uint64_t queue_operations_since_submission_fence_last_ = 0;
  bool queue_operations_done_since_submission_signal_ = false;

  bool frame_open_ = false;
  // Guest frame index, since some transient resources can be reused across
  // submissions. Values updated in the beginning of a frame.
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;
  // Submission indices of frames that have already been submitted.
  uint64_t closed_frame_submissions_[kQueueFrames] = {};

  struct CommandAllocator {
    ID3D12CommandAllocator* command_allocator;
    uint64_t last_usage_submission;
    CommandAllocator* next;
  };
  CommandAllocator* command_allocator_writable_first_ = nullptr;
  CommandAllocator* command_allocator_writable_last_ = nullptr;
  CommandAllocator* command_allocator_submitted_first_ = nullptr;
  CommandAllocator* command_allocator_submitted_last_ = nullptr;
  ID3D12GraphicsCommandList* command_list_ = nullptr;
  ID3D12GraphicsCommandList1* command_list_1_ = nullptr;
  DeferredCommandList deferred_command_list_;

  bool debug_markers_enabled_ = false;

  // Viewport info caching - avoids redundant GetHostViewportInfo recalculation
  // when viewport-affecting register state hasn't changed between draws.
  struct ViewportCacheKey {
    uint32_t pa_cl_clip_cntl;
    uint32_t pa_cl_vte_cntl;
    uint32_t pa_su_sc_mode_cntl;
    uint32_t pa_su_vtx_cntl;
    uint32_t pa_sc_window_offset;
    uint32_t normalized_depth_control;
    uint32_t vport_regs[6];  // XSCALE, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET
    uint32_t flags;          // packed: convert_z_to_float24, full_float24, ps_writes_depth
    bool operator==(const ViewportCacheKey&) const = default;
  };
  ViewportCacheKey previous_viewport_key_{};
  draw_util::ViewportInfo previous_viewport_info_{};
  bool viewport_cache_valid_ = false;

  // Should bindless textures and samplers be used - many times faster
  // UpdateBindings than bindful (that becomes a significant bottleneck with
  // bindful - mainly because of CopyDescriptorsSimple, which takes the majority
  // of UpdateBindings time, and that's outside the emulator's control even).
  bool bindless_resources_used_ = false;

  std::unique_ptr<D3D12SharedMemory> shared_memory_;

  std::unique_ptr<D3D12RenderTargetCache> render_target_cache_;

  std::unique_ptr<ui::d3d12::D3D12UploadBufferPool> constant_buffer_pool_;

  static constexpr uint32_t kViewBindfulHeapSize = 32768;
  static_assert(kViewBindfulHeapSize <= D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1);
  std::unique_ptr<ui::d3d12::D3D12DescriptorHeapPool> view_bindful_heap_pool_;
  // Currently bound descriptor heap - updated by RequestViewBindfulDescriptors.
  ID3D12DescriptorHeap* view_bindful_heap_current_;
  // Rationale: textures have 4 KB alignment in guest memory, and there can be
  // 512 MB / 4 KB in total of them at most, and multiply by 3 for different
  // swizzles, signedness, and multiple host textures for one guest texture, and
  // transient descriptors. Though in reality there will be a lot fewer of
  // course, this is just a "safe" value. The limit is 1000000 for resource
  // binding tier 2.
  static constexpr uint32_t kViewBindlessHeapSize = 262144;
  static_assert(kViewBindlessHeapSize <= D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2);
  ID3D12DescriptorHeap* view_bindless_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE view_bindless_heap_cpu_start_;
  D3D12_GPU_DESCRIPTOR_HANDLE view_bindless_heap_gpu_start_;
  uint32_t view_bindless_heap_allocated_ = 0;
  std::vector<uint32_t> view_bindless_heap_free_;
  // <Descriptor index, submission where requested>, sorted by the submission
  // number.
  std::deque<std::pair<uint32_t, uint64_t>> view_bindless_one_use_descriptors_;

  // Direct3D 12 only allows shader-visible heaps with no more than 2048
  // samplers (due to Nvidia addressing). However, there's also possibly a weird
  // bug in the Nvidia driver (tested on 440.97 and earlier on Windows 10 1803)
  // that caused the sampler with index 2047 not to work if a heap with 8 or
  // less samplers also exists - in case of Xenia, it's the immediate drawer's
  // sampler heap.
  // FIXME(Triang3l): Investigate the issue with the sampler 2047 on Nvidia.
  static constexpr uint32_t kSamplerHeapSize = 2000;
  static_assert(kSamplerHeapSize <= D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
  std::unique_ptr<ui::d3d12::D3D12DescriptorHeapPool> sampler_bindful_heap_pool_;
  ID3D12DescriptorHeap* sampler_bindful_heap_current_;
  ID3D12DescriptorHeap* sampler_bindless_heap_current_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE sampler_bindless_heap_cpu_start_;
  D3D12_GPU_DESCRIPTOR_HANDLE sampler_bindless_heap_gpu_start_;
  // Currently the sampler heap is used only for texture cache samplers, so
  // individual samplers are never freed, and using a simple linear allocator
  // inside the current heap without a free list.
  uint32_t sampler_bindless_heap_allocated_ = 0;
  // <Heap, overflow submission number>, if total sampler count used so far
  // exceeds kSamplerHeapSize, and the heap has been switched (this is not a
  // totally impossible situation considering Direct3D 9 has sampler parameter
  // state instead of sampler objects, and having one "unimportant" parameter
  // changed may result in doubling of sampler count). Sorted by the submission
  // number (so checking if the first can be reused is enough).
  std::deque<std::pair<ID3D12DescriptorHeap*, uint64_t>> sampler_bindless_heaps_overflowed_;
  // D3D12TextureCache::SamplerParameters::value -> indices within the current
  // bindless sampler heap.
  std::unordered_map<uint32_t, uint32_t> texture_cache_bindless_sampler_map_;

  // Root signatures for different descriptor counts.
  std::unordered_map<uint32_t, ID3D12RootSignature*> root_signatures_bindful_;
  ID3D12RootSignature* root_signature_bindless_vs_ = nullptr;
  ID3D12RootSignature* root_signature_bindless_ds_ = nullptr;

  std::unique_ptr<D3D12PrimitiveProcessor> primitive_processor_;

  std::unique_ptr<PipelineCache> pipeline_cache_;

  std::unique_ptr<D3D12TextureCache> texture_cache_;

  // Bytes 0x0...0x3FF - 256-entry gamma ramp table with B10G10R10X2 data (read
  // as R10G10B10X2 with swizzle).
  // Bytes 0x400...0x9FF - 128-entry PWL R16G16 gamma ramp (R - base, G - delta,
  // low 6 bits of each are zero, 3 elements per entry).
  // Experimental path tracing (path_tracer.cpp). Positions of rasterized
  // triangles are captured with stream output during the frame, turned into a
  // ray tracing acceleration structure at swap time, and traced to add
  // occlusion, sun shadows and bounce light to the frame.
  bool InitializePathTracing();
  void ShutdownPathTracing();
  // Called for every draw after the pipeline and the viewport are known.
  void UpdatePathTracingCapture(const PrimitiveProcessor::ProcessingResult& primitive_processing,
                                bool primitive_polygonal, bool rasterization_done,
                                reg::RB_DEPTHCONTROL normalized_depth_control,
                                const Shader& vertex_shader, const Shader* pixel_shader,
                                const draw_util::ViewportInfo& viewport_info);
  // Copies a sample of a captured draw's view space vertex positions from
  // guest memory (where the game has already transformed them) for tracking
  // the camera.
  void SamplePathTracingDrawVertices(const PrimitiveProcessor::ProcessingResult& primitive_processing,
                                     const Shader& vertex_shader);
  // Estimates the camera motion since the previous frame from the draws seen
  // in both (the static scenery moves rigidly with the camera) and updates
  // the world space the denoisers work in.
  void UpdatePathTracingCamera();
  // Invalidates the command list state cached for guest draws after work
  // that changed it outside the command processor's control.
  void InvalidatePathTracingCommandListState();
  // Triangles written to the capture buffer by a draw while it's bound (its
  // pipeline has stream output - see PipelineCache::CreateD3D12Pipeline).
  static uint32_t PathTracingStreamOutTriangles(
      const PrimitiveProcessor::ProcessingResult& primitive_processing, bool rasterization_done,
      bool has_pixel_shader);
  static uint64_t PathTracingFetchKey(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t PathTracingTextureSlot(uint32_t fetch_constant_index);
  // Called once per presented guest frame from IssueSwap. Returns the texture
  // to present instead of the swap texture (with its SRV description), or
  // nullptr if the frame is presented as is.
  ID3D12Resource* PathTracingRender(ID3D12Resource* swap_texture,
                                    const D3D12_SHADER_RESOURCE_VIEW_DESC& swap_texture_srv_desc,
                                    uint32_t width, uint32_t height,
                                    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_desc_out);
  // Called once per guest frame from IssueSwap, after PathTracingRender.
  void PathTracingFrameEnd();
  // Creates the working textures for the scene rectangle and the output.
  bool EnsurePathTracingTextures(uint32_t rect_width, uint32_t rect_height, uint32_t output_width,
                                 uint32_t output_height);

  // Denoisers (path_tracer_denoisers.cpp): NRD (default), DLSS Ray
  // Reconstruction, FSR Ray Regeneration, and the built-in filter.
  enum class PathTracingDenoiser : uint32_t {
    kBuiltin,
    kNRD,
    kDLSSRR,
    kFSRRR,
  };
  struct PathTracingDenoiserState;
  // The denoiser to use this frame - the configured one if available, NRD or
  // the built-in filter otherwise.
  PathTracingDenoiser SelectPathTracingDenoiser();
  // Inputs of the denoisers for a frame (all rectangle-sized textures, in
  // D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE).
  struct PathTracingDenoiseInputs {
    uint32_t width;
    uint32_t height;
    bool reset;
    uint32_t frame_index;
    // Column-major, column vectors (NRD's convention - same memory layout as
    // row-major with row vectors of DLSS and FSR).
    float view_to_clip[16];
    float world_to_view[16];
    float previous_world_to_view[16];
    float camera_position_delta[3];
    float denoising_range;
    float sun_direction_world[3];
    float sun_tan_angular_radius;
    float sun_color[3];
    float hit_distance_parameters[3];
    float frame_time_ms;
    ID3D12Resource* view_depth;
    ID3D12Resource* nrd_normal_roughness;
    ID3D12Resource* world_motion;
    ID3D12Resource* screen_motion;
    ID3D12Resource* normal_roughness;
    ID3D12Resource* octahedral_normal;
    ID3D12Resource* diffuse_albedo;
    ID3D12Resource* specular_albedo;
    ID3D12Resource* diffuse_signal;
    ID3D12Resource* specular_signal;
    ID3D12Resource* shadow_signal;
    ID3D12Resource* color;
    // Outputs.
    ID3D12Resource* diffuse_output;
    ID3D12Resource* specular_output;
    ID3D12Resource* shadow_output;
    ID3D12Resource* color_output;
  };
  bool PathTracingDenoiseNRD(const PathTracingDenoiseInputs& inputs);
  bool PathTracingDenoiseDLSSRR(const PathTracingDenoiseInputs& inputs);
  bool PathTracingDenoiseFSRRR(const PathTracingDenoiseInputs& inputs);
  void ShutdownPathTracingDenoisers();
  // Transitions a path tracing resource, tracking its state within a frame
  // (resources rest in NON_PIXEL_SHADER_RESOURCE between the passes).
  void PathTracingUseResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES state);
  void PathTracingRestoreResourceStates();
  std::vector<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> pt_resource_states_;
  // Contiguous shader-visible descriptors for this frame's descriptor tables.
  bool PathTracingAllocateDescriptors(uint32_t count,
                                      ui::d3d12::util::DescriptorCpuGpuHandlePair& start);
  static constexpr uint32_t kPathTracingDescriptorsPerFrame = 8192;
  uint32_t pt_bindless_descriptor_base_ = UINT32_MAX;
  // Descriptors of this frame (bindful: one request at the start of the
  // frame's path tracing).
  ui::d3d12::util::DescriptorCpuGpuHandlePair pt_frame_descriptors_ = {};
  uint32_t pt_frame_descriptor_count_ = 0;
  uint32_t pt_frame_descriptors_used_ = 0;
  // Owned, deleted by ShutdownPathTracingDenoisers.
  PathTracingDenoiserState* pt_denoiser_state_ = nullptr;
  // Recording DLSS Ray Reconstruction and FSR Ray Regeneration into the
  // command list (DeferredCommandList::ExternalCallback).
  static void PathTracingDLSSCallback(void* context, ID3D12GraphicsCommandList* command_list);
  static void PathTracingFSRCallback(void* context, ID3D12GraphicsCommandList* command_list);
  PathTracingDenoiser pt_previous_denoiser_ = PathTracingDenoiser::kBuiltin;

  enum class PathTracingRootParameter : UINT {
    kConstants,
    kBuffer0,
    kBuffer1,
    kRWBuffer0,
    kRWBuffer1,
    kMaterials,
    kAttributes,
    kRWAttributes,
    kPreviousVertices,
    // The pass's textures: t0...t15 and u0...u15 in space 3.
    kTextures,
    kRWTextures,
    // Unbounded, from the start of the view heap.
    kMaterialTextures,

    kCount,
  };
  static constexpr uint32_t kPathTracingMaxTriangles = 1u << 17;
  // Per vertex: clip space position, texture coordinates, color table row.
  static constexpr uint32_t kPathTracingCaptureVertexSize = 32;
  static constexpr uint32_t kPathTracingCaptureSize =
      kPathTracingMaxTriangles * 3 * kPathTracingCaptureVertexSize;
  // Per triangle: texture coordinates of the vertices, color table row, draw,
  // the game's lighting factors of the vertices.
  static constexpr uint32_t kPathTracingAttributeSize = 48;
  static constexpr uint32_t kPathTracingMaxDraws = 4096;
  static constexpr uint32_t kPathTracingMaxTextures = 256;
  static constexpr uint32_t kPathTracingCounterSize = (kPathTracingMaxDraws + 1) * 8;
  // Material header (80 bytes) and the draws (32 bytes each).
  static constexpr uint32_t kPathTracingMaterialUploadSize = 80 + kPathTracingMaxDraws * 32;
  // Per frame: constant buffers for the passes, then the materials.
  static constexpr uint32_t kPathTracingConstantSlots = 16;
  static constexpr uint32_t kPathTracingConstantSlotSize = 512;
  static constexpr uint32_t kPathTracingConstantsUploadSize =
      kPathTracingConstantSlots * kPathTracingConstantSlotSize;
  static constexpr uint32_t kPathTracingPassTextures = 16;
  static constexpr uint32_t kPathTracingFrameUploadSize =
      kPathTracingConstantsUploadSize + ((kPathTracingMaterialUploadSize + 255) & ~255u);
  // Two regions: solid triangles (opaque geometry of the acceleration
  // structure) and alpha-tested ones.
  static constexpr uint32_t kPathTracingVertexRegionSize = kPathTracingMaxTriangles * 3 * 12;
  static constexpr uint32_t kPathTracingVertexBufferSize = kPathTracingVertexRegionSize * 2;

  Microsoft::WRL::ComPtr<ID3D12Resource> pt_capture_buffer_;
  // UINT64 BufferFilledSize per draw (each draw writes into its own region of
  // the capture buffer, sized for the most triangles it can emit - geometry
  // shaders drop some, like ones with NaN positions), and one more for draws
  // beyond the maximum.
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_capture_counter_;
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_zero_upload_;
  // View space triangles for the acceleration structure, this and the
  // previous frame's (pt_history_index_).
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_vertex_buffers_[2];
  // Per-frame statistics (ground normal, background color, average lighting)
  // for 2 frames, see pt_common.hlsli.
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_stats_buffer_;
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_blas_;
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_tlas_;
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_scratch_;
  uint64_t pt_tlas_scratch_offset_ = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_instance_upload_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> pt_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_convert_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_sun_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_primary_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_lighting_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_temporal_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_denoise_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_resolve_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_bloom_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_composite_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_compose_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pt_sky_pipeline_;
  // The sky around the scene by world direction (octahedral, see pt_sky).
  static constexpr uint32_t kPathTracingSkyMapSize = 32;
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_sky_map_;
  // Working textures, covering the scene rectangle (see pt_common.hlsli).
  enum class PathTracingTexture : uint32_t {
    // Normal and view depth of the primary surface of every pixel, this and
    // the previous frame's.
    kGBuffer0,
    kGBuffer1,
    // Where the surfaces were in the previous frame.
    kMotion,
    // Surface colors (the scene without the game's own shadows).
    kAlbedo,
    // Denoiser guides.
    kViewDepth,
    kNRDNormalRoughness,
    kWorldMotion,
    kScreenMotion,
    kNormalRoughness,
    kOctahedralNormal,
    kDiffuseAlbedo,
    kSpecularAlbedo,
    // Traced lighting (total irradiance or indirect diffuse), the built-in
    // denoiser's intermediate, specular, sun visibility.
    kLighting,
    kLightingTemp,
    kSpecular,
    kShadow,
    // Built-in temporal accumulation, this and the previous frame's.
    kIrradianceHistory0,
    kIrradianceHistory1,
    kSpecularHistory0,
    kSpecularHistory1,
    // Outputs of the denoisers working on separate signals.
    kDenoisedDiffuse,
    kDenoisedSpecular,
    kDenoisedShadow,
    // Noisy lit color and its denoised version for DLSS Ray Reconstruction.
    kRRColor,
    kRROutput,
    // Exposed linear HDR color, and quarter resolution bloom (ping-pong).
    kHDR,
    kBloomA,
    kBloomB,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_textures_[size_t(PathTracingTexture::kCount)];
  ID3D12Resource* PathTracingTextureResource(PathTracingTexture texture) const {
    return pt_textures_[size_t(texture)].Get();
  }
  uint32_t pt_history_index_ = 0;
  bool pt_rendered_this_frame_ = false;
  bool pt_rendered_previous_frame_ = false;
  float pt_previous_projection_[2] = {};
  // The final image (output size).
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_output_;
  // Size of the working textures (the scene rectangle) and of the output.
  uint32_t pt_texture_width_ = 0;
  uint32_t pt_texture_height_ = 0;
  uint32_t pt_output_width_ = 0;
  uint32_t pt_output_height_ = 0;
  // The game's light direction in world space, read back from the GPU (a few
  // frames late - it's fixed in the world) for the denoisers.
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_sun_readback_;
  uint64_t pt_sun_readback_frames_[kQueueFrames] = {};
  float pt_sun_direction_world_[3] = {0.0f, 1.0f, 0.0f};
  uint64_t pt_last_render_time_ = 0;
  // Textures replaced while possibly still in use by the GPU.
  std::vector<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> pt_retired_textures_;
  bool pt_capture_bound_ = false;
  // Games using predicated tiling redraw the scene for every EDRAM tile; the
  // first tile's pass has every polygon once. Capture depth-tested draws with
  // the viewport of the first one until the next resolve.
  bool pt_captured_this_frame_ = false;
  bool pt_capture_done_this_frame_ = false;
  draw_util::ViewportInfo pt_viewport_;
  // The scene without the HUD: where the capture pass was resolved to, and the
  // fetch constant of the texture later drawn from there.
  uint32_t pt_scene_address_ = 0;
  xenos::xe_gpu_texture_fetch_t pt_scene_fetch_;
  bool pt_scene_fetch_valid_ = false;
  // Projection scale of the first captured draw (guest clip xy = view xy *
  // this), from the vertex shader constants if configured.
  float pt_projection_[2] = {};
  // Draws that wrote to the capture buffer this frame, in order.
  struct PathTracingDraw {
    uint32_t triangle_offset;
    uint32_t triangle_count;
    // Texture slots until uploaded, then the texture descriptor indices.
    uint32_t texture;
    uint32_t flags;
    uint32_t palette;
    // Alpha test: passes if texel alpha * scale - bias >= 0.
    float alpha_scale;
    float alpha_bias;
    // The same draw's first triangle in the previous frame, for motion.
    uint32_t previous_first;
  };
  enum PathTracingDrawFlags : uint32_t {
    // Solid scene geometry.
    kPathTracingDrawOpaque = 1 << 0,
    // Blended scene geometry (effects, water).
    kPathTracingDrawTransparent = 1 << 1,
    // Shaded with the configured material model.
    kPathTracingDrawMaterial = 1 << 2,
    // The material's alpha test may discard parts of the triangles.
    kPathTracingDrawAlphaTest = 1 << 3,
  };
  std::vector<PathTracingDraw> pt_draws_;
  // Identify draws across frames (texture, color table, size, flags).
  std::vector<uint64_t> pt_draw_keys_;
  std::vector<PathTracingDraw> pt_previous_draws_;
  std::vector<uint64_t> pt_previous_draw_keys_;
  // Index of each draw's match in the previous frame, or UINT32_MAX.
  std::vector<uint32_t> pt_draw_previous_index_;
  // View space vertex position samples (xyz) of the draws, per draw the
  // first sample and the count, this and the previous frame's.
  static constexpr uint32_t kPathTracingDrawVertexSamples = 24;
  std::vector<float> pt_draw_samples_;
  std::vector<uint32_t> pt_draw_sample_ranges_;
  std::vector<float> pt_previous_draw_samples_;
  std::vector<uint32_t> pt_previous_draw_sample_ranges_;
  // The world space the denoisers work in: fixed to the scenery, anchored at
  // where the camera was when tracking started. View to world rotation
  // (row-major 3x3) and translation, this and the previous frame's.
  double pt_view_to_world_rotation_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  double pt_view_to_world_translation_[3] = {};
  double pt_previous_view_to_world_rotation_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  double pt_previous_view_to_world_translation_[3] = {};
  // Whether this frame's camera is known relative to the previous frame's
  // (otherwise the world space restarts, and so do the histories).
  bool pt_camera_tracked_ = false;
  bool pt_camera_tracked_initialized_ = false;
  uint32_t pt_camera_draws_agreeing_ = 0;
  uint32_t pt_world_resets_ = 0;
  uint32_t pt_draw_triangles_ = 0;
  // Textures of the material draws (texture cache handles, see
  // D3D12TextureCache::GetActiveTexture) and their SRV descriptions.
  std::vector<void*> pt_texture_handles_;
  std::vector<D3D12_SHADER_RESOURCE_VIEW_DESC> pt_texture_srv_descs_;
  std::unordered_map<uint64_t, uint32_t> pt_texture_slots_;
  // From the first material draw: the pixel shader constants c254, c255,
  // c1, c0 of the material model (the alpha test and color table are per
  // draw).
  bool pt_material_found_ = false;
  float pt_material_constants_[16] = {};
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_material_upload_;
  uint8_t* pt_material_upload_mapping_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12Resource> pt_attribute_buffer_;
  uint32_t pt_frame_ = 0;
  // For the average frame time in the debug log.
  std::chrono::steady_clock::time_point pt_frame_time_;
  // Output area of the last path traced frame (resolution-scaled), to tell
  // scene draws from ones into smaller viewports.
  uint64_t pt_output_area_ = UINT64_MAX;
  std::string pt_albedo_shader_text_;
  uint64_t pt_albedo_shader_hash_ = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> gamma_ramp_buffer_;
  D3D12_RESOURCE_STATES gamma_ramp_buffer_state_;
  // Upload buffer for an image that is the same as gamma_ramp_, but with
  // kQueueFrames array layers.
  Microsoft::WRL::ComPtr<ID3D12Resource> gamma_ramp_upload_buffer_;
  uint8_t* gamma_ramp_upload_buffer_mapping_ = nullptr;
  bool gamma_ramp_256_entry_table_up_to_date_ = false;
  bool gamma_ramp_pwl_up_to_date_ = false;

  struct ApplyGammaConstants {
    uint32_t size[2];
  };
  enum class ApplyGammaRootParameter : UINT {
    kConstants,
    kDestination,
    kSource,
    kRamp,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> apply_gamma_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_table_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_table_fxaa_luma_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_pwl_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_pwl_fxaa_luma_pipeline_;

  struct FxaaConstants {
    uint32_t size[2];
    float size_inv[2];
  };
  enum class FxaaRootParameter : UINT {
    kConstants,
    kDestination,
    kSource,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> fxaa_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> fxaa_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> fxaa_extreme_pipeline_;

  struct ResolveDownscaleConstants {
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t pixel_size_log2;
    uint32_t tile_count;
    uint32_t half_pixel_offset;
  };
  enum class ResolveDownscaleRootParameter : UINT {
    kConstants,
    kSource,
    kDestination,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> resolve_downscale_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> resolve_downscale_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12Resource> resolve_downscale_buffer_;
  uint32_t resolve_downscale_buffer_size_ = 0;

  // PWL gamma ramp can result in values with more precision than 10bpc. Though
  // those sub-10bpc bits don't have any noticeable visual effect, so normally
  // R10G10B10A2_UNORM is enough. But what's the most important is that for the
  // original FXAA shader, the luma needs to be written to the alpha channel.
  // For simplicity (to avoid modifying the FXAA shader and adding more texture
  // fetches into it), and for the highest quality (preserving all 13 bits that
  // may be generated by applying the PWL gamma ramp with an increment of 2^3,
  // and also leaving some space for the result of applying fractional weights
  // to calculate the luma), using R16G16B16A16_UNORM instead of
  // R10G10B10X2_UNORM with a separate alpha texture.
  static constexpr DXGI_FORMAT kFxaaSourceTextureFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
  // Kept in NON_PIXEL_SHADER_RESOURCE state.
  Microsoft::WRL::ComPtr<ID3D12Resource> fxaa_source_texture_;
  uint64_t fxaa_source_texture_submission_ = 0;

  // Unsubmitted barrier batch.
  std::vector<D3D12_RESOURCE_BARRIER> barriers_;

  // <Submission where requested, resource>, sorted by the submission number.
  std::deque<std::pair<uint64_t, ID3D12Resource*>> resources_for_deletion_;

  static constexpr uint32_t kScratchBufferSizeIncrement = 16 * 1024 * 1024;
  ID3D12Resource* scratch_buffer_ = nullptr;
  uint32_t scratch_buffer_size_ = 0;
  D3D12_RESOURCE_STATES scratch_buffer_state_;
  bool scratch_buffer_used_ = false;

  ID3D12Resource* readback_buffer_ = nullptr;
  uint32_t readback_buffer_size_ = 0;
  std::unordered_map<uint64_t, ReadbackBuffer> readback_buffers_;
  std::unordered_map<uint64_t, ReadbackBuffer> memexport_readback_buffers_;

  static constexpr uint32_t kMaxOcclusionQueries = 8192;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> occlusion_query_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> occlusion_query_readback_;
  uint64_t* occlusion_query_readback_mapping_ = nullptr;
  uint32_t occlusion_query_cursor_ = 0;
  bool occlusion_query_resources_available_ = false;
  struct ActiveOcclusionQuery {
    uint32_t sample_count_address = 0;
    uint32_t host_index = UINT32_MAX;
    bool valid = false;
  } active_occlusion_query_;
  struct VertexBufferState {
    uint32_t address = UINT32_MAX;
    uint32_t size = UINT32_MAX;
  };
  std::array<VertexBufferState, 96> vertex_buffer_states_{};
  uint64_t vertex_buffers_in_sync_[2] = {};

  // The current fixed-function drawing state.
  D3D12_VIEWPORT ff_viewport_;
  D3D12_RECT ff_scissor_;
  float ff_blend_factor_[4];
  uint32_t ff_stencil_ref_;
  bool ff_viewport_update_needed_;
  bool ff_scissor_update_needed_;
  bool ff_blend_factor_update_needed_;
  bool ff_stencil_ref_update_needed_;

  // Currently bound pipeline, either a graphics pipeline from the pipeline
  // cache (with potentially deferred creation - current_external_pipeline_ is
  // nullptr in this case) or a non-Xenos graphics or compute pipeline
  // (current_guest_pipeline_ is nullptr in this case).
  void* current_guest_pipeline_;
  ID3D12PipelineState* current_external_pipeline_;

  // Currently bound graphics root signature.
  ID3D12RootSignature* current_graphics_root_signature_;
  // Extra parameters which may or may not be present.
  RootBindfulExtraParameterIndices current_graphics_root_bindful_extras_;
  // Whether root parameters are up to date - reset if a new signature is bound.
  uint32_t current_graphics_root_up_to_date_;

  // System shader constants.
  DxbcShaderTranslator::SystemConstants system_constants_;

  // Float constant usage masks of the last draw call.
  uint64_t current_float_constant_map_vertex_[4];
  uint64_t current_float_constant_map_pixel_[4];

  // Constant buffer bindings.
  struct ConstantBufferBinding {
    D3D12_GPU_VIRTUAL_ADDRESS address;
    bool up_to_date;
  };
  ConstantBufferBinding cbuffer_binding_system_;
  ConstantBufferBinding cbuffer_binding_float_vertex_;
  ConstantBufferBinding cbuffer_binding_float_pixel_;
  ConstantBufferBinding cbuffer_binding_bool_loop_;
  ConstantBufferBinding cbuffer_binding_fetch_;
  ConstantBufferBinding cbuffer_binding_descriptor_indices_vertex_;
  ConstantBufferBinding cbuffer_binding_descriptor_indices_pixel_;

  // Whether the latest shared memory and EDRAM buffer binding contains the
  // shared memory UAV rather than the SRV.
  // Separate descriptor tables for the SRV and the UAV, even though only one is
  // accessed dynamically in the shaders, are used to prevent a validation
  // message about missing resource states in PIX.
  std::optional<bool> current_shared_memory_binding_is_uav_;

  // Pages with the descriptors currently used for handling Xenos draw calls.
  uint64_t draw_view_bindful_heap_index_;
  uint64_t draw_sampler_bindful_heap_index_;

  // Whether the last used texture sampler bindings have been written to the
  // current view descriptor heap.
  bool bindful_textures_written_vertex_;
  bool bindful_textures_written_pixel_;
  bool bindful_samplers_written_vertex_;
  bool bindful_samplers_written_pixel_;
  // Layout UIDs and last texture and sampler bindings written to the current
  // descriptor heaps (for bindful) or descriptor index constant buffer (for
  // bindless) with the last used descriptor layout. Valid only when:
  // - For bindful, when bindful_#_written_#_ is true.
  // - For bindless, when cbuffer_binding_descriptor_indices_#_.up_to_date is
  //   true.
  size_t current_texture_layout_uid_vertex_;
  size_t current_texture_layout_uid_pixel_;
  size_t current_sampler_layout_uid_vertex_;
  size_t current_sampler_layout_uid_pixel_;
  // Size of these should be ignored when checking whether these are up to date,
  // layout UID should be checked first (they will be different for different
  // binding counts).
  std::vector<D3D12TextureCache::TextureSRVKey> current_texture_srv_keys_vertex_;
  std::vector<D3D12TextureCache::TextureSRVKey> current_texture_srv_keys_pixel_;
  std::vector<D3D12TextureCache::SamplerParameters> current_samplers_vertex_;
  std::vector<D3D12TextureCache::SamplerParameters> current_samplers_pixel_;
  std::vector<uint32_t> current_sampler_bindless_indices_vertex_;
  std::vector<uint32_t> current_sampler_bindless_indices_pixel_;

  // Latest bindful descriptor handles used for handling Xenos draw calls.
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_srv_and_edram_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_uav_and_edram_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_textures_vertex_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_textures_pixel_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_samplers_vertex_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_samplers_pixel_;

  // Current primitive topology.
  D3D_PRIMITIVE_TOPOLOGY primitive_topology_;

  // Temporary storage for memexport stream constants used in the draw.
  std::vector<draw_util::MemExportRange> memexport_ranges_;
};

}  // namespace rex::graphics::d3d12
