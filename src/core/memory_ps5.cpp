/**
 * @file        core/memory_ps5.cpp
 * @brief       Host memory for the PlayStation 5 (PS5 payload SDK).
 *
 * The POSIX backend builds the guest arena from one shm_open file mapped at
 * several addresses. On the console a title's own mappings, shm views
 * included, are charged to a small flexible-memory budget (448 MiB to 1 GiB),
 * so the arena lives in direct memory through the PS5 platform layer
 * (ps5platform/shm.h), as in the Lost Odyssey PS5 port:
 *
 *   - a "file mapping" is a list of direct-memory objects, one per 64 MiB of
 *     file offset, created when a view first touches that part of the file:
 *     only the offsets the guest layout uses get backing (the virtual range,
 *     the XEX range and the 512 MiB physical block, about 2.75 GiB);
 *   - a view is mapped chunk by chunk at consecutive addresses, so two views
 *     of the same offsets (the A/C/E physical mirrors, the 80/90 XEX views)
 *     share memory exactly as two mmaps of one file do;
 *   - views go inside a range reserved with ps5_vrange_reserve
 *     (Memory::MapViewsPs5).
 *
 * Pages are 16 KiB. Commit and decommit inside a view only change protection
 * (the memory is already backed); the console has no /proc, so protections
 * inside views are kept in a shadow table that QueryProtect reads.
 *
 * @copyright   Copyright (c) 2026 Tom Clay. PS5 backend 2026.
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_PS5

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include <ps5platform/shm.h>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>

namespace rex {
namespace memory {

namespace {

constexpr size_t kChunkBytes = size_t(64) << 20;  // backing object per 64 MiB of file
constexpr size_t kDirectUnit = size_t(64) << 10;  // direct-memory unit

constexpr uintptr_t AlignDown(uintptr_t value, size_t alignment) {
  return value & ~uintptr_t(alignment - 1);
}
constexpr uintptr_t AlignUp(uintptr_t value, size_t alignment) {
  return AlignDown(value + alignment - 1, alignment);
}

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      return PROT_NONE;
  }
}

struct Mapping {
  bool used = false;
  size_t length = 0;
  std::vector<ps5_shm> chunks;  // direct_start == 0 until created
  std::vector<bool> created;
};

// The guest arena is the only mapping a title makes; a few slots suffice.
std::mutex g_mutex;
std::array<Mapping, 4> g_mappings;

// Address ranges covered by views: commit/decommit/protect there only change
// protection, and the shadow table records it.
struct ViewRange {
  uintptr_t begin, end;
  std::vector<uint8_t> access;  // PageAccess per host page
};
std::vector<ViewRange> g_views;

ViewRange* FindView(uintptr_t address) {
  for (auto& v : g_views) {
    if (address >= v.begin && address < v.end) {
      return &v;
    }
  }
  return nullptr;
}

void RecordAccess(uintptr_t address, size_t length, PageAccess access) {
  const size_t page = page_size();
  uintptr_t end = address + length;
  while (address < end) {
    ViewRange* v = FindView(address);
    if (!v) {
      address = AlignUp(address + 1, page);
      continue;
    }
    const uintptr_t stop = std::min(end, v->end);
    for (uintptr_t p = AlignDown(address, page); p < stop; p += page) {
      v->access[(p - v->begin) / page] = uint8_t(access);
    }
    address = stop;
  }
}

bool InViews(uintptr_t address, size_t length) {
  const uintptr_t end = address + length;
  while (address < end) {
    ViewRange* v = FindView(address);
    if (!v) {
      return false;
    }
    address = v->end;
  }
  return true;
}

std::atomic<bool> g_mprotect_warned{false};

bool ChangeProtection(void* address, size_t length, PageAccess access) {
  if (mprotect(address, length, int(ToPosixProtectFlags(access))) == 0) {
    return true;
  }
  if (!g_mprotect_warned.exchange(true)) {
    REXSYS_WARN("PS5: mprotect({}, 0x{:X}) failed ({}); guest page protection is not enforced",
                address, length, errno);
  }
  return false;
}

}  // namespace

size_t page_size() {
  static const size_t size = size_t(getpagesize());
  return size;
}

// Views and their file offsets go by direct-memory units. Like Windows' 64 KiB
// granularity, the E0000000 view's 4 KiB shift is applied by the runtime
// (host_address_offset) instead of by the mapping.
size_t allocation_granularity() {
  return kDirectUnit;
}

bool IsWritableExecutableMemorySupported() {
  return false;
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
  if (base_address) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (InViews(address, length)) {
      // Already backed by its view: a commit or a reserve is a protection.
      const PageAccess effective =
          allocation_type == AllocationType::kReserve ? PageAccess::kNoAccess : access;
      ChangeProtection(base_address, length, effective);
      RecordAccess(address, length, effective);
      return base_address;
    }
  }

  // Outside the arena: committed memory from direct memory, so it does not
  // eat into the flexible budget.
  if (!base_address) {
    void* reserved = nullptr;
    const size_t bytes = rex::round_up(length, kDirectUnit);
    if (ps5_vrange_reserve(bytes, nullptr, kDirectUnit, &reserved) != 0 || !reserved) {
      return nullptr;
    }
    if (allocation_type != AllocationType::kReserve &&
        ps5_vrange_commit(reserved, bytes, int(ToPosixProtectFlags(access))) != 0) {
      ps5_vrange_release(reserved, bytes);
      return nullptr;
    }
    return reserved;
  }
  if (allocation_type == AllocationType::kReserve) {
    return ps5_vrange_reserve_at(base_address, rex::round_up(length, kDirectUnit)) == 0
               ? base_address
               : nullptr;
  }
  if (allocation_type == AllocationType::kReserveCommit) {
    ps5_vrange_reserve_at(base_address, rex::round_up(length, kDirectUnit));
  }
  return ps5_vrange_commit(base_address, length, int(ToPosixProtectFlags(access))) == 0
             ? base_address
             : nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (InViews(address, length)) {
      // The view keeps its backing: decommitted guest pages only lose access.
      // (Contents are not zeroed; the guest heaps zero on commit as needed.)
      ChangeProtection(base_address, length, PageAccess::kNoAccess);
      RecordAccess(address, length, PageAccess::kNoAccess);
      return true;
    }
  }
  switch (deallocation_type) {
    case DeallocationType::kDecommit:
      return ps5_vrange_decommit(base_address, length) == 0;
    case DeallocationType::kRelease:
      return ps5_vrange_release(base_address, length) == 0;
    default:
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    size_t query_length = length;
    if (!QueryProtect(base_address, query_length, *out_old_access)) {
      *out_old_access = PageAccess::kReadWrite;
    }
  }
  const bool changed = ChangeProtection(base_address, length, access);
  std::lock_guard<std::mutex> lock(g_mutex);
  RecordAccess(reinterpret_cast<uintptr_t>(base_address), length, access);
  // Inside the arena the shadow is authoritative even when the console refused
  // the change, so callers that pair Protect calls stay consistent.
  return changed || InViews(reinterpret_cast<uintptr_t>(base_address), length);
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
  ViewRange* v = FindView(address);
  if (!v) {
    length = 0;
    access_out = PageAccess::kNoAccess;
    return false;
  }
  const size_t page = page_size();
  size_t index = (address - v->begin) / page;
  const uint8_t first = v->access[index];
  size_t run = 0;
  while (index < v->access.size() && v->access[index] == first) {
    ++index;
    run += page;
  }
  length = run - (address - AlignDown(address, page));
  access_out = PageAccess(first);
  return true;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& /*path*/, size_t length,
                                          PageAccess /*access*/, bool /*commit*/) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (size_t i = 0; i < g_mappings.size(); ++i) {
    Mapping& m = g_mappings[i];
    if (m.used) {
      continue;
    }
    m.used = true;
    m.length = length;
    const size_t count = (length + kChunkBytes - 1) / kChunkBytes;
    m.chunks.assign(count, ps5_shm{});
    m.created.assign(count, false);
    return FileMappingHandle(i);
  }
  return kFileMappingHandleInvalid;
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& /*path*/) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (handle < 0 || size_t(handle) >= g_mappings.size()) {
    return;
  }
  Mapping& m = g_mappings[size_t(handle)];
  for (size_t i = 0; i < m.chunks.size(); ++i) {
    if (m.created[i]) {
      ps5_shm_destroy(&m.chunks[i]);
    }
  }
  m = Mapping{};
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  // A view needs a place: the arena reserves its range first.
  if (!base_address || handle < 0 || size_t(handle) >= g_mappings.size() ||
      file_offset % kDirectUnit || length % kDirectUnit) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  Mapping& m = g_mappings[size_t(handle)];
  if (!m.used || file_offset + length > rex::round_up(m.length, kChunkBytes)) {
    return nullptr;
  }

  auto* const base = static_cast<uint8_t*>(base_address);
  const int protection = (access == PageAccess::kNoAccess) ? PS5_SHM_READ
                         : (access == PageAccess::kReadOnly || access == PageAccess::kExecuteReadOnly)
                             ? PS5_SHM_READ
                             : PS5_SHM_READ | PS5_SHM_WRITE;
  size_t done = 0;
  while (done < length) {
    const size_t offset = file_offset + done;
    const size_t chunk = offset / kChunkBytes;
    const size_t in_chunk = offset % kChunkBytes;
    const size_t piece = std::min(length - done, kChunkBytes - in_chunk);
    if (!m.created[chunk]) {
      const int rc = ps5_shm_create(kChunkBytes, &m.chunks[chunk]);
      if (rc != 0) {
        REXSYS_ERROR("PS5: direct memory for guest offsets 0x{:X}+64MiB failed: 0x{:08X}",
                     chunk * kChunkBytes, uint32_t(rc));
        return nullptr;
      }
      m.created[chunk] = true;
    }
    void* view = nullptr;
    const int rc = ps5_shm_map(&m.chunks[chunk], in_chunk, piece, base + done, protection,
                               PS5_SHM_FIXED, &view);
    if (rc != 0 || view != base + done) {
      REXSYS_ERROR("PS5: mapping guest view at {} (offset 0x{:X}, 0x{:X} bytes) failed: 0x{:08X}",
                   static_cast<void*>(base + done), offset, piece, uint32_t(rc));
      if (done) {
        ps5_shm_unmap(base, done, PS5_SHM_KEEP_RESERVED);
      }
      return nullptr;
    }
    done += piece;
  }

  ViewRange range;
  range.begin = reinterpret_cast<uintptr_t>(base);
  range.end = range.begin + length;
  range.access.assign(length / page_size(), uint8_t(access));
  g_views.push_back(std::move(range));
  if (access == PageAccess::kNoAccess) {
    ChangeProtection(base, length, PageAccess::kNoAccess);
  }
  return base_address;
}

bool UnmapFileView(FileMappingHandle /*handle*/, void* base_address, size_t length) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  g_views.erase(std::remove_if(g_views.begin(), g_views.end(),
                               [&](const ViewRange& v) { return v.begin == begin; }),
                g_views.end());
  return ps5_shm_unmap(base_address, length, PS5_SHM_KEEP_RESERVED) == 0;
}

}  // namespace memory
}  // namespace rex

#endif  // REX_PLATFORM_PS5
