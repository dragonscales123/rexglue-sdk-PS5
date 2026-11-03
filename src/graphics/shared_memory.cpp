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
#include <cstring>
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/dbg.h>
#include <rex/graphics/shared_memory.h>
#include <rex/math.h>
#include <rex/cvar.h>
#include <rex/platform.h>
#if REX_PLATFORM_PS5
#include "../core/ps5_profiler.h"
#endif
#include <rex/memory.h>

REXCVAR_DEFINE_BOOL(shared_memory_hot_pages, false, "GPU",
                    "Re-upload pages the CPU keeps rewriting on every GPU request instead of "
                    "write-watching them (saves a write fault and a protection change per page "
                    "per frame; costs a page copy per request). Pages with texture watches stay "
                    "watched.");

namespace rex::graphics {

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);
  hot_pages_.assign(num_system_page_flags_, 0);
  hot_block_invalidations_.assign(num_system_page_flags_, 0);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  num_system_page_flags_ = 0;
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  std::fill(system_page_flags_valid_.begin(), system_page_flags_valid_.end(), uint64_t(0));
  std::fill(system_page_flags_valid_and_gpu_written_.begin(),
            system_page_flags_valid_and_gpu_written_.end(), uint64_t(0));
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto global_lock = global_critical_region_.Acquire();

  // Pages that are valid only because the CPU uploaded them lose their valid
  // bit here, so the next frame re-reads them from guest memory.
  system_page_flags_valid_ = system_page_flags_valid_and_gpu_written_;
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  // A texture is loaded by requesting its range first and watching it after,
  // so its pages may have been taken as hot (left unwatched) in between. They
  // must fault on CPU writes for this watch to fire: take them out of the hot
  // set and protect them again. They stay invalid, so the next request still
  // re-uploads them (the GPU copy matched guest memory for the bytes the
  // texture was just loaded from).
  if (REXCVAR_GET(shared_memory_hot_pages) && memory_invalidation_callback_handle_) {
    uint32_t run_first = UINT32_MAX;
    for (uint32_t page = watch_page_first; page <= watch_page_last + 1; ++page) {
      bool hot = false;
      if (page <= watch_page_last) {
        uint64_t& block = hot_pages_[page >> 6];
        const uint64_t bit = uint64_t(1) << (page & 63);
        if (block & bit) {
          hot = true;
          block &= ~bit;
          hot_page_shadows_.erase(page);
        }
      }
      if (hot) {
        if (run_first == UINT32_MAX) {
          run_first = page;
        }
      } else if (run_first != UINT32_MAX) {
        memory().EnablePhysicalMemoryAccessCallbacks(
            run_first << page_size_log2_, (page - run_first) << page_size_log2_, true, false);
        run_first = UINT32_MAX;
      }
    }
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
  RangeAccessedByGpu(start, length);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  const bool hot_mode = !written_by_gpu && REXCVAR_GET(shared_memory_hot_pages);
  bool any_hot = false;
  {
    auto global_lock = global_critical_region_.Acquire();

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      if (hot_mode && (hot_pages_[i] & valid_bits)) {
        // Hot pages that something (a texture) watches stay watched; the rest
        // are left invalid, so every request uploads them again.
        uint64_t hot = hot_pages_[i] & valid_bits;
        uint64_t remaining = hot;
        uint32_t bit;
        while (rex::bit_scan_forward(remaining, &bit)) {
          remaining &= ~(uint64_t(1) << bit);
          if (PageHasRangeWatch((i << 6) + bit)) {
            hot &= ~(uint64_t(1) << bit);
          }
        }
        if (hot) {
          any_hot = true;
          // Snapshot before UploadRanges copies the page, so a CPU write in
          // between only causes an extra upload later, never a missed one.
          uint64_t snap = hot;
          uint32_t snap_bit;
          const size_t page_bytes = size_t(1) << page_size_log2_;
          while (rex::bit_scan_forward(snap, &snap_bit)) {
            snap &= ~(uint64_t(1) << snap_bit);
            uint32_t page = (i << 6) + snap_bit;
            auto& shadow = hot_page_shadows_[page];
            if (!shadow) {
              shadow = std::make_unique<uint8_t[]>(page_bytes);
            }
            std::memcpy(shadow.get(), memory().TranslatePhysical(page << page_size_log2_),
                        page_bytes);
          }
#if REX_PLATFORM_PS5
          rex::ps5prof::counters().hot_page_bytes.fetch_add(
              uint64_t(rex::bit_count(hot)) << page_size_log2_, std::memory_order_relaxed);
#endif
        }
        valid_bits &= ~hot;
      }
#if REX_PLATFORM_PS5
      if (written_by_gpu) {
        rex::ps5prof::counters().gpu_written_bytes.fetch_add(
            uint64_t(rex::bit_count(valid_bits)) << page_size_log2_, std::memory_order_relaxed);
      }
#endif
      if (written_by_gpu && !hot_page_shadows_.empty()) {
        // The GPU copy now differs from what the shadow describes.
        uint64_t written = valid_bits;
        uint32_t written_bit;
        while (rex::bit_scan_forward(written, &written_bit)) {
          written &= ~(uint64_t(1) << written_bit);
          hot_page_shadows_.erase((i << 6) + written_bit);
        }
      }
      system_page_flags_valid_[i] |= valid_bits;
      uint64_t& gpu_written = system_page_flags_valid_and_gpu_written_[i];
      gpu_written = written_by_gpu ? (gpu_written | valid_bits) : (gpu_written & ~valid_bits);
    }
  }

  if (!memory_invalidation_callback_handle_) {
    return;
  }
  if (!any_hot) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
    return;
  }
  // Watch only the runs of pages that were made valid (not the hot ones).
  uint32_t run_first = UINT32_MAX;
  for (uint32_t page = valid_page_first; page <= valid_page_last + 1; ++page) {
    bool valid = false;
    if (page <= valid_page_last) {
      auto global_lock = global_critical_region_.Acquire();
      valid = (system_page_flags_valid_[page >> 6] >> (page & 63)) & 1;
    }
    if (valid) {
      if (run_first == UINT32_MAX) {
        run_first = page;
      }
    } else if (run_first != UINT32_MAX) {
      memory().EnablePhysicalMemoryAccessCallbacks(
          run_first << page_size_log2_, (page - run_first) << page_size_log2_, true, false);
      run_first = UINT32_MAX;
    }
  }
}

bool SharedMemory::PageHasRangeWatch(uint32_t page) const {
  // Call in the global critical region.
  uint32_t bucket = (page << page_size_log2_) >> kWatchBucketSizeLog2;
  for (const WatchNode* node = watch_buckets_[bucket]; node != nullptr;
       node = node->bucket_node_next) {
    const WatchRange* range = node->range;
    if (page >= range->page_first && page <= range->page_last) {
      return true;
    }
  }
  return false;
}

void SharedMemory::OnFrameSwapped() {
  if (!REXCVAR_GET(shared_memory_hot_pages)) {
    return;
  }
  // Every ~10 s forget the hot set, so pages that stopped changing (or got
  // reused for something static) go back to being watched; pages that are
  // still rewritten re-qualify after a few frames.
  if (++hot_frames_ < 300) {
    return;
  }
  hot_frames_ = 0;
  auto global_lock = global_critical_region_.Acquire();
  std::fill(hot_pages_.begin(), hot_pages_.end(), uint64_t(0));
  std::fill(hot_block_invalidations_.begin(), hot_block_invalidations_.end(), uint8_t(0));
  hot_page_shadows_.clear();
}

void SharedMemory::FilterUnchangedHotPages(
    const std::vector<std::pair<uint32_t, uint32_t>>& byte_ranges) {
  // Call in the global critical region, with upload_ranges_ filled from
  // byte_ranges (sorted, non-overlapping). Only the requested bytes of a hot
  // page are compared: the shadow always equals the GPU copy, so if those
  // bytes match, the GPU copy is correct for this request; the rest of the
  // page is checked when it is requested.
  if (hot_page_shadows_.empty() || upload_ranges_.empty()) {
    return;
  }
  const uint32_t page_bytes = uint32_t(1) << page_size_log2_;
  size_t range_index = 0;
  std::vector<std::pair<uint32_t, uint32_t>> kept;
  kept.reserve(upload_ranges_.size());
  auto keep = [&kept](uint32_t page) {
    if (!kept.empty() && kept.back().first + kept.back().second == page) {
      ++kept.back().second;
    } else {
      kept.emplace_back(page, 1);
    }
  };
  for (const auto& range : upload_ranges_) {
    for (uint32_t page = range.first; page < range.first + range.second; ++page) {
      if ((hot_pages_[page >> 6] >> (page & 63)) & 1) {
        auto it = hot_page_shadows_.find(page);
        if (it != hot_page_shadows_.end()) {
          const uint32_t page_start = page << page_size_log2_;
          const uint32_t page_end = page_start + page_bytes;
          // Pages come in increasing order, so the range cursor only moves on.
          while (range_index < byte_ranges.size() &&
                 byte_ranges[range_index].first + byte_ranges[range_index].second <= page_start) {
            ++range_index;
          }
          bool unchanged = true;
          const uint8_t* guest = memory().TranslatePhysical(page_start);
          for (size_t r = range_index; r < byte_ranges.size() && unchanged; ++r) {
            const uint32_t start = std::max(byte_ranges[r].first, page_start);
            const uint32_t end =
                std::min(byte_ranges[r].first + byte_ranges[r].second, page_end);
            if (byte_ranges[r].first >= page_end) {
              break;
            }
            if (start < end) {
              unchanged = std::memcmp(it->second.get() + (start - page_start),
                                      guest + (start - page_start), end - start) == 0;
            }
          }
          if (unchanged) {
            continue;  // the requested bytes match the GPU copy
          }
        }
      }
      keep(page);
    }
  }
  upload_ranges_.swap(kept);
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
  if (ranges == nullptr || !count) {
    return true;
  }

  // Some texture or buffer is empty, for example - safe to draw in this case.
  std::vector<std::pair<uint32_t, uint32_t>> merged_ranges;
  merged_ranges.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");

  std::sort(merged_ranges.begin(), merged_ranges.end(),
            [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return a.first < b.first;
            });
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = merged_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = merged_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  upload_ranges_.clear();
  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    auto global_lock = global_critical_region_.Acquire();
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = system_page_flags_valid_[i];
        // Consider pages in the block outside the requested range valid.
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          uint32_t block_page;
          if (range_start == UINT32_MAX) {
            // Check if need to open a new range.
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            // Check if need to close the range.
            // Ignore the valid pages before the beginning of the range.
            uint64_t block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            // In the next iteration within this block, consider this range
            // valid since it has been queued for upload.
            block_valid |= (uint64_t(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
    if (REXCVAR_GET(shared_memory_hot_pages)) {
      FilterUnchangedHotPages(merged_ranges);
    }
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  bool successful = upload_ranges_.empty() || UploadRanges(upload_ranges_);
  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    RangeAccessedByGpu(range.first, range.second);
  }
  return successful;
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

  if (!exact_range) {
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
  }

  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    system_page_flags_valid_[i] &= ~invalidate_bits;
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
    if (REXCVAR_GET(shared_memory_hot_pages) && i < hot_block_invalidations_.size()) {
      uint8_t& count = hot_block_invalidations_[i];
      if (count < 255) {
        ++count;
      }
      // Rewritten a few times since the last reset: stop watching it.
      if (count >= 3) {
        hot_pages_[i] |= invalidate_bits;
      }
    }
  }
#if REX_PLATFORM_PS5
  rex::ps5prof::counters().invalidations.fetch_add(1, std::memory_order_relaxed);
  (exact_range ? rex::ps5prof::counters().invalidated_bytes_exact
               : rex::ps5prof::counters().invalidated_bytes_fault)
      .fetch_add(uint64_t(page_last - page_first + 1) << page_size_log2_,
                 std::memory_order_relaxed);
#endif

  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
