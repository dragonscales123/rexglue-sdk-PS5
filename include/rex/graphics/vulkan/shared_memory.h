#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class VulkanSharedMemory : public SharedMemory {
 public:
  VulkanSharedMemory(VulkanCommandProcessor& command_processor, memory::Memory& memory,
                     VkPipelineStageFlags guest_shader_pipeline_stages);
  ~VulkanSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void CompletedSubmissionUpdated();
  void EndSubmission();

  enum class Usage {
    // Index buffer, vfetch, compute read, transfer source.
    kRead,
    // Index buffer, vfetch, memexport.
    kGuestDrawReadWrite,
    kComputeWrite,
    kTransferDestination,
  };
  // Inserts a pipeline barrier for the target usage, also ensuring consecutive
  // read-write accesses are ordered with each other.
  void Use(Usage usage, std::pair<uint32_t, uint32_t> written_range = {});
  // While the command processor records a draw preparation chunk, Use tracks
  // the usage within the chunk separately: the chunk starts from "any usage"
  // and ends making its accesses visible to all usages, so it is correct
  // wherever it is placed in the main command buffer.
  void BeginPrepRecording();
  void EndPrepRecording();

  VkBuffer buffer() const { return buffer_; }

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;
  void RangeAccessedByGpu(uint32_t start, uint32_t length) override;

 private:
  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                     VkAccessFlags& access_mask) const;
  // Stages and accesses of every usage, including writes.
  void GetAllUsagesMasks(VkPipelineStageFlags& stage_mask, VkAccessFlags& access_mask) const;

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  VkBuffer buffer_ = VK_NULL_HANDLE;
  uint32_t buffer_memory_type_;
  // Single for non-sparse, every allocation so far for sparse.
  std::vector<VkDeviceMemory> buffer_memory_;

  Usage last_usage_;
  std::pair<uint32_t, uint32_t> last_written_range_;
  // Usage tracking within a preparation chunk (prep_usage_known_ false until
  // the chunk's first Use).
  bool prep_usage_known_ = false;
  Usage prep_last_usage_ = Usage::kRead;
  std::pair<uint32_t, uint32_t> prep_last_written_range_;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;

  // Pages requested or written by the GPU in the current submission. Pages not
  // in it are uploaded in the command processor's upload command buffer,
  // executed before the rest of the submission, without ending the render pass.
  std::vector<uint64_t> submission_accessed_pages_;
  bool upload_first_used_ = false;
};

}  // namespace rex::graphics::vulkan
