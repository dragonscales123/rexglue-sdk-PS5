/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime (POSIX + macOS)
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless an XSI feature level is
// requested before <ucontext.h> is included.
#define _XOPEN_SOURCE 700
#endif

#include <rex/exception_handler.h>

#if REX_PLATFORM_POSIX

#include <signal.h>
#if defined(__PROSPERO__)
#include <fcntl.h>
#include <pthread_np.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#endif

#include <cstdint>
#include <cstring>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/platform.h>

#include <ucontext.h>

namespace rex::arch {

bool signal_handlers_installed_ = false;
struct sigaction original_sigill_handler_;
struct sigaction original_sigsegv_handler_;
#if REX_PLATFORM_MAC || REX_PLATFORM_PS5
struct sigaction original_sigbus_handler_;
#endif

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

#if REX_PLATFORM_PS5
// Byte offset of XMM0 in an FXSAVE image (FreeBSD's mc_fpstate).
constexpr size_t kFxsaveXmmOffset = 160;
#endif

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

#if REX_PLATFORM_PS5
// PS5 diagnostics: the console's kernel reports a fatal fault but nothing of
// the title's own state, so the first faults that reach this handler, and
// every one it does not handle, are written to /app0/faults.txt with write(2).
namespace {
std::atomic<uint32_t> ps5_fault_count{0};
constexpr uint32_t kPs5FaultsLogged = 200;
// One descriptor for every line, in append mode, so the fault lines and the
// stack lines can't overwrite each other.
int Ps5FaultFd() {
  static int fd = open("/app0/faults.txt", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
  return fd;
}
void Ps5FaultLine(const char* what, int signal_number, const siginfo_t* info, uint64_t rip,
                  bool always) {
  const uint32_t n = ps5_fault_count.load(std::memory_order_relaxed);
  if (!always && n > kPs5FaultsLogged) {
    return;
  }
  const int fd = Ps5FaultFd();
  if (fd < 0) {
    return;
  }
  char line[192];
  const int len = std::snprintf(line, sizeof(line), "%u %s sig=%d code=%d addr=%p rip=0x%llx tid=%d\n",
                                n, what, signal_number, info ? info->si_code : 0,
                                info ? info->si_addr : nullptr, (unsigned long long)rip,
                                (int)pthread_getthreadid_np());
  if (len > 0) {
    (void)write(fd, line, size_t(len));
  }
}
// The code addresses on the faulting thread's stack, for a backtrace of an
// unhandled fault: symbolized offline against the linked ELF (the title is
// loaded at 0x400000 and its code is the first, executable segment).
// PS5_Vulkan's title layout (ps5-pie-unwind.ld) starts the read-only segment,
// right after the code, with .eh_frame_hdr and names its start.
extern "C" char __eh_frame_hdr_start[];
void Ps5FaultStack(uint64_t rsp) {
  const int fd = Ps5FaultFd();
  if (fd < 0 || !rsp) {
    return;
  }
  const uint64_t lo = 0x400000;
  const uint64_t hi = uint64_t(__eh_frame_hdr_start);
  const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
  char line[64];
  int found = 0;
  for (int i = 0; i < 4096 && found < 48; ++i) {
    const uint64_t value = stack[i];
    if (value >= lo && value < hi) {
      const int len = std::snprintf(line, sizeof(line), "  stack[%d]=0x%llx\n", i,
                                    (unsigned long long)value);
      if (len > 0) {
        (void)write(fd, line, size_t(len));
      }
      ++found;
    }
  }
}
}  // namespace
#endif

static void ExceptionHandlerCallback(int signal_number, siginfo_t* signal_info,
                                     void* signal_context) {
#if REX_PLATFORM_PS5
  const uint64_t ps5_fault_rip =
      uint64_t(reinterpret_cast<ucontext_t*>(signal_context)->uc_mcontext.mc_rip);
  ps5_fault_count.fetch_add(1, std::memory_order_relaxed);
  Ps5FaultLine("enter", signal_number, signal_info, ps5_fault_rip, false);
#endif
#if REX_PLATFORM_MAC
  // Darwin's mcontext_t is already a pointer (__darwin_mcontext64*); copy it.
  mcontext_t mcontext = reinterpret_cast<ucontext_t*>(signal_context)->uc_mcontext;
#else
  // Linux's mcontext_t is a large struct; bind by reference to avoid copying it
  // inside the signal handler.
  mcontext_t& mcontext = reinterpret_cast<ucontext_t*>(signal_context)->uc_mcontext;
#endif

  HostThreadContext thread_context;

#if REX_ARCH_AMD64
#if REX_PLATFORM_MAC
  thread_context.rip = uint64_t(mcontext->__ss.__rip);
  thread_context.eflags = uint32_t(mcontext->__ss.__rflags);
  // TODO (Graine25): Double check this one for me
  thread_context.rax = uint64_t(mcontext->__ss.__rax);
  thread_context.rcx = uint64_t(mcontext->__ss.__rcx);
  thread_context.rdx = uint64_t(mcontext->__ss.__rdx);
  thread_context.rbx = uint64_t(mcontext->__ss.__rbx);
  thread_context.rsp = uint64_t(mcontext->__ss.__rsp);
  thread_context.rbp = uint64_t(mcontext->__ss.__rbp);
  thread_context.rsi = uint64_t(mcontext->__ss.__rsi);
  thread_context.rdi = uint64_t(mcontext->__ss.__rdi);
  thread_context.r8 = uint64_t(mcontext->__ss.__r8);
  thread_context.r9 = uint64_t(mcontext->__ss.__r9);
  thread_context.r10 = uint64_t(mcontext->__ss.__r10);
  thread_context.r11 = uint64_t(mcontext->__ss.__r11);
  thread_context.r12 = uint64_t(mcontext->__ss.__r12);
  thread_context.r13 = uint64_t(mcontext->__ss.__r13);
  thread_context.r14 = uint64_t(mcontext->__ss.__r14);
  thread_context.r15 = uint64_t(mcontext->__ss.__r15);
  std::memcpy(thread_context.xmm_registers, &mcontext->__fs.__fpu_xmm0,
              sizeof(thread_context.xmm_registers));
#elif REX_PLATFORM_PS5
  // FreeBSD amd64 (machine/ucontext.h): named mc_* registers; mc_fpstate is the
  // FXSAVE image, XMM0..XMM15 at byte 160.
  thread_context.rip = uint64_t(mcontext.mc_rip);
  thread_context.eflags = uint32_t(mcontext.mc_rflags);
  thread_context.rax = uint64_t(mcontext.mc_rax);
  thread_context.rcx = uint64_t(mcontext.mc_rcx);
  thread_context.rdx = uint64_t(mcontext.mc_rdx);
  thread_context.rbx = uint64_t(mcontext.mc_rbx);
  thread_context.rsp = uint64_t(mcontext.mc_rsp);
  thread_context.rbp = uint64_t(mcontext.mc_rbp);
  thread_context.rsi = uint64_t(mcontext.mc_rsi);
  thread_context.rdi = uint64_t(mcontext.mc_rdi);
  thread_context.r8 = uint64_t(mcontext.mc_r8);
  thread_context.r9 = uint64_t(mcontext.mc_r9);
  thread_context.r10 = uint64_t(mcontext.mc_r10);
  thread_context.r11 = uint64_t(mcontext.mc_r11);
  thread_context.r12 = uint64_t(mcontext.mc_r12);
  thread_context.r13 = uint64_t(mcontext.mc_r13);
  thread_context.r14 = uint64_t(mcontext.mc_r14);
  thread_context.r15 = uint64_t(mcontext.mc_r15);
  std::memcpy(thread_context.xmm_registers,
              reinterpret_cast<const char*>(mcontext.mc_fpstate) + kFxsaveXmmOffset,
              sizeof(thread_context.xmm_registers));
#else
  thread_context.rip = uint64_t(mcontext.gregs[REG_RIP]);
  thread_context.eflags = uint32_t(mcontext.gregs[REG_EFL]);
  // The REG_ order may be different than the register indices in the
  // instruction encoding.
  thread_context.rax = uint64_t(mcontext.gregs[REG_RAX]);
  thread_context.rcx = uint64_t(mcontext.gregs[REG_RCX]);
  thread_context.rdx = uint64_t(mcontext.gregs[REG_RDX]);
  thread_context.rbx = uint64_t(mcontext.gregs[REG_RBX]);
  thread_context.rsp = uint64_t(mcontext.gregs[REG_RSP]);
  thread_context.rbp = uint64_t(mcontext.gregs[REG_RBP]);
  thread_context.rsi = uint64_t(mcontext.gregs[REG_RSI]);
  thread_context.rdi = uint64_t(mcontext.gregs[REG_RDI]);
  thread_context.r8 = uint64_t(mcontext.gregs[REG_R8]);
  thread_context.r9 = uint64_t(mcontext.gregs[REG_R9]);
  thread_context.r10 = uint64_t(mcontext.gregs[REG_R10]);
  thread_context.r11 = uint64_t(mcontext.gregs[REG_R11]);
  thread_context.r12 = uint64_t(mcontext.gregs[REG_R12]);
  thread_context.r13 = uint64_t(mcontext.gregs[REG_R13]);
  thread_context.r14 = uint64_t(mcontext.gregs[REG_R14]);
  thread_context.r15 = uint64_t(mcontext.gregs[REG_R15]);
  std::memcpy(thread_context.xmm_registers, mcontext.fpregs->_xmm,
              sizeof(thread_context.xmm_registers));
#endif
#elif REX_ARCH_ARM64
#if REX_PLATFORM_MAC
  // macOS ARM64: mcontext_t is __darwin_mcontext64* (pointer, not value)
  std::memcpy(thread_context.x, mcontext->__ss.__x, sizeof(mcontext->__ss.__x));
#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
  thread_context.x[29] = reinterpret_cast<uintptr_t>(mcontext->__ss.__opaque_fp);
  thread_context.x[30] = reinterpret_cast<uintptr_t>(mcontext->__ss.__opaque_lr);
  thread_context.sp = reinterpret_cast<uintptr_t>(mcontext->__ss.__opaque_sp);
  thread_context.pc = reinterpret_cast<uintptr_t>(mcontext->__ss.__opaque_pc);
#else
  thread_context.x[29] = mcontext->__ss.__fp;
  thread_context.x[30] = mcontext->__ss.__lr;
  thread_context.sp = mcontext->__ss.__sp;
  thread_context.pc = mcontext->__ss.__pc;
#endif
  thread_context.pstate = mcontext->__ss.__cpsr;
  thread_context.fpsr = mcontext->__ns.__fpsr;
  thread_context.fpcr = mcontext->__ns.__fpcr;
  static_assert(sizeof(thread_context.v) == sizeof(mcontext->__ns.__v));
  std::memcpy(thread_context.v, mcontext->__ns.__v, sizeof(thread_context.v));
#else
  // Linux ARM64: mcontext_t is struct sigcontext (value, not pointer)
  std::memcpy(thread_context.x, mcontext.regs, sizeof(thread_context.x));
  thread_context.sp = mcontext.sp;
  thread_context.pc = mcontext.pc;
  thread_context.pstate = mcontext.pstate;
  struct fpsimd_context* mcontext_fpsimd = nullptr;
  struct esr_context* mcontext_esr = nullptr;
  for (struct _aarch64_ctx* mcontext_extension =
           reinterpret_cast<struct _aarch64_ctx*>(mcontext.__reserved);
       mcontext_extension->magic;
       mcontext_extension = reinterpret_cast<struct _aarch64_ctx*>(
           reinterpret_cast<uint8_t*>(mcontext_extension) + mcontext_extension->size)) {
    switch (mcontext_extension->magic) {
      case FPSIMD_MAGIC:
        mcontext_fpsimd = reinterpret_cast<struct fpsimd_context*>(mcontext_extension);
        break;
      case ESR_MAGIC:
        mcontext_esr = reinterpret_cast<struct esr_context*>(mcontext_extension);
        break;
      default:
        break;
    }
  }
  assert_not_null(mcontext_fpsimd);
  if (mcontext_fpsimd) {
    thread_context.fpsr = mcontext_fpsimd->fpsr;
    thread_context.fpcr = mcontext_fpsimd->fpcr;
    std::memcpy(thread_context.v, mcontext_fpsimd->vregs, sizeof(thread_context.v));
  }
#endif  // REX_PLATFORM_MAC
#endif  // REX_ARCH

  Exception ex;
  switch (signal_number) {
    case SIGILL:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case SIGBUS:
      // On macOS, SIGBUS (KERN_PROTECTION_FAILURE) is raised for writes to
      // read-protected pages — the same scenario that raises SIGSEGV on Linux.
      // Fall through to treat it as an access violation.
#if !REX_PLATFORM_MAC
      // On non-Mac POSIX, SIGBUS is a bus error (misaligned access), not a
      // protection fault — don't handle it here. FreeBSD (the PS5) can raise
      // SIGBUS for a protection fault, as macOS does.
#if !REX_PLATFORM_PS5
      assert_unhandled_case(signal_number);
      return;
#endif
#endif
      [[fallthrough]];
    case SIGSEGV: {
      Exception::AccessViolationOperation access_violation_operation;
#if REX_ARCH_AMD64
      // x86_pf_error_code::X86_PF_WRITE
      constexpr uint64_t kX86PageFaultErrorCodeWrite = UINT64_C(1) << 1;
#if REX_PLATFORM_MAC
      access_violation_operation = (uint64_t(mcontext->__es.__err) & kX86PageFaultErrorCodeWrite)
                                       ? Exception::AccessViolationOperation::kWrite
                                       : Exception::AccessViolationOperation::kRead;
#elif REX_PLATFORM_PS5
      access_violation_operation = (uint64_t(mcontext.mc_err) & kX86PageFaultErrorCodeWrite)
                                       ? Exception::AccessViolationOperation::kWrite
                                       : Exception::AccessViolationOperation::kRead;
#else
      access_violation_operation = (uint64_t(mcontext.gregs[REG_ERR]) & kX86PageFaultErrorCodeWrite)
                                       ? Exception::AccessViolationOperation::kWrite
                                       : Exception::AccessViolationOperation::kRead;
#endif
#elif REX_ARCH_ARM64
      // For a Data Abort (EC - ESR_EL1 bits 31:26 - 0b100100 from a lower
      // Exception Level, 0b100101 without a change in the Exception Level),
      // bit 6 is 0 for reading from a memory location, 1 for writing to a
      // memory location.
#if REX_PLATFORM_MAC
      {
        uint32_t mac_esr = mcontext->__es.__esr;
        if (((mac_esr >> 26) & 0b111110) == 0b100100) {
          access_violation_operation = (mac_esr & (1U << 6))
                                           ? Exception::AccessViolationOperation::kWrite
                                           : Exception::AccessViolationOperation::kRead;
        } else {
          bool instruction_is_store;
          if (IsArm64LoadPrefetchStore(*reinterpret_cast<const uint32_t*>(thread_context.pc),
                                       instruction_is_store)) {
            access_violation_operation = instruction_is_store
                                             ? Exception::AccessViolationOperation::kWrite
                                             : Exception::AccessViolationOperation::kRead;
          } else {
            assert_always(
                "No ESR in the exception thread context, or it's not a Data "
                "Abort, and the faulting instruction is not a known load, "
                "prefetch or store instruction");
            access_violation_operation = Exception::AccessViolationOperation::kUnknown;
          }
        }
      }
#else
      if (mcontext_esr && ((mcontext_esr->esr >> 26) & 0b111110) == 0b100100) {
        access_violation_operation = (mcontext_esr->esr & (UINT64_C(1) << 6))
                                         ? Exception::AccessViolationOperation::kWrite
                                         : Exception::AccessViolationOperation::kRead;
      } else {
        // Determine the memory access direction based on which instruction has
        // requested it.
        // esr_context may be unavailable on certain hosts (for instance, on
        // Android, it was added only in NDK r16 - which is the first NDK
        // version to support the Android API level 27, while NDK r15 doesn't
        // have esr_context in its API 26 sigcontext.h).
        // On AArch64 (unlike on AArch32), the program counter is the address of
        // the currently executing instruction.
        bool instruction_is_store;
        if (IsArm64LoadPrefetchStore(*reinterpret_cast<const uint32_t*>(mcontext.pc),
                                     instruction_is_store)) {
          access_violation_operation = instruction_is_store
                                           ? Exception::AccessViolationOperation::kWrite
                                           : Exception::AccessViolationOperation::kRead;
        } else {
          assert_always(
              "No ESR in the exception thread context, or it's not a Data "
              "Abort, and the faulting instruction is not a known load, "
              "prefetch or store instruction");
          access_violation_operation = Exception::AccessViolationOperation::kUnknown;
        }
      }
#endif  // REX_PLATFORM_MAC
#else
      access_violation_operation = Exception::AccessViolationOperation::kUnknown;
#endif  // REX_ARCH
      ex.InitializeAccessViolation(&thread_context,
                                   reinterpret_cast<uint64_t>(signal_info->si_addr),
                                   access_violation_operation);
    } break;
    default:
      assert_unhandled_case(signal_number);
  }

  for (size_t i = 0; i < rex::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      // Exception handled.
#if REX_PLATFORM_PS5
      Ps5FaultLine("handled", signal_number, signal_info, ps5_fault_rip, false);
#endif
#if REX_ARCH_AMD64
#if REX_PLATFORM_MAC
      mcontext->__ss.__rip = thread_context.rip;
      mcontext->__ss.__rflags = thread_context.eflags;
      uint32_t modified_register_index;
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        switch (modified_register_index) {
          case 0:
            mcontext->__ss.__rax = thread_context.int_registers[modified_register_index];
            break;
          case 1:
            mcontext->__ss.__rcx = thread_context.int_registers[modified_register_index];
            break;
          case 2:
            mcontext->__ss.__rdx = thread_context.int_registers[modified_register_index];
            break;
          case 3:
            mcontext->__ss.__rbx = thread_context.int_registers[modified_register_index];
            break;
          case 4:
            mcontext->__ss.__rsp = thread_context.int_registers[modified_register_index];
            break;
          case 5:
            mcontext->__ss.__rbp = thread_context.int_registers[modified_register_index];
            break;
          case 6:
            mcontext->__ss.__rsi = thread_context.int_registers[modified_register_index];
            break;
          case 7:
            mcontext->__ss.__rdi = thread_context.int_registers[modified_register_index];
            break;
          case 8:
            mcontext->__ss.__r8 = thread_context.int_registers[modified_register_index];
            break;
          case 9:
            mcontext->__ss.__r9 = thread_context.int_registers[modified_register_index];
            break;
          case 10:
            mcontext->__ss.__r10 = thread_context.int_registers[modified_register_index];
            break;
          case 11:
            mcontext->__ss.__r11 = thread_context.int_registers[modified_register_index];
            break;
          case 12:
            mcontext->__ss.__r12 = thread_context.int_registers[modified_register_index];
            break;
          case 13:
            mcontext->__ss.__r13 = thread_context.int_registers[modified_register_index];
            break;
          case 14:
            mcontext->__ss.__r14 = thread_context.int_registers[modified_register_index];
            break;
          case 15:
            mcontext->__ss.__r15 = thread_context.int_registers[modified_register_index];
            break;
          default:
            assert_unhandled_case(modified_register_index);
        }
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(reinterpret_cast<char*>(&mcontext->__fs.__fpu_xmm0) +
                        modified_register_index * sizeof(vec128_t),
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
#elif REX_PLATFORM_PS5
      mcontext.mc_rip = __register_t(thread_context.rip);
      mcontext.mc_rflags = __register_t(thread_context.eflags);
      uint32_t modified_register_index;
      // The order must match the order in X64Register.
      __register_t* const kIntRegisterMap[] = {
          &mcontext.mc_rax, &mcontext.mc_rcx, &mcontext.mc_rdx, &mcontext.mc_rbx,
          &mcontext.mc_rsp, &mcontext.mc_rbp, &mcontext.mc_rsi, &mcontext.mc_rdi,
          &mcontext.mc_r8,  &mcontext.mc_r9,  &mcontext.mc_r10, &mcontext.mc_r11,
          &mcontext.mc_r12, &mcontext.mc_r13, &mcontext.mc_r14, &mcontext.mc_r15,
      };
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        *kIntRegisterMap[modified_register_index] =
            __register_t(thread_context.int_registers[modified_register_index]);
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(reinterpret_cast<char*>(mcontext.mc_fpstate) + kFxsaveXmmOffset +
                        modified_register_index * sizeof(vec128_t),
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
#else
      mcontext.gregs[REG_RIP] = greg_t(thread_context.rip);
      mcontext.gregs[REG_EFL] = greg_t(thread_context.eflags);
      uint32_t modified_register_index;
      // The order must match the order in X64Register.
      static const size_t kIntRegisterMap[] = {
          REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
          REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
      };
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        mcontext.gregs[kIntRegisterMap[modified_register_index]] =
            thread_context.int_registers[modified_register_index];
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(&mcontext.fpregs->_xmm[modified_register_index],
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
#endif
#elif REX_ARCH_ARM64
      uint32_t modified_register_index;
      uint32_t modified_x_registers_remaining = ex.modified_x_registers();
#if REX_PLATFORM_MAC
      while (rex::bit_scan_forward(modified_x_registers_remaining, &modified_register_index)) {
        modified_x_registers_remaining &= ~(UINT32_C(1) << modified_register_index);
        if (modified_register_index < 29) {
          mcontext->__ss.__x[modified_register_index] = thread_context.x[modified_register_index];
        } else if (modified_register_index == 29) {
#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
          mcontext->__ss.__opaque_fp = reinterpret_cast<void*>(thread_context.x[29]);
#else
          mcontext->__ss.__fp = thread_context.x[29];
#endif
        } else {
#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
          mcontext->__ss.__opaque_lr = reinterpret_cast<void*>(thread_context.x[30]);
#else
          mcontext->__ss.__lr = thread_context.x[30];
#endif
        }
      }
#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
      mcontext->__ss.__opaque_sp = reinterpret_cast<void*>(thread_context.sp);
      mcontext->__ss.__opaque_pc = reinterpret_cast<void*>(thread_context.pc);
#else
      mcontext->__ss.__sp = thread_context.sp;
      mcontext->__ss.__pc = thread_context.pc;
#endif
      mcontext->__ss.__cpsr = thread_context.pstate;
      mcontext->__ns.__fpsr = thread_context.fpsr;
      mcontext->__ns.__fpcr = thread_context.fpcr;
      {
        uint32_t modified_v_registers_remaining = ex.modified_v_registers();
        while (rex::bit_scan_forward(modified_v_registers_remaining, &modified_register_index)) {
          modified_v_registers_remaining &= ~(UINT32_C(1) << modified_register_index);
          std::memcpy(&mcontext->__ns.__v[modified_register_index],
                      &thread_context.v[modified_register_index], sizeof(vec128_t));
        }
      }
#else
      while (rex::bit_scan_forward(modified_x_registers_remaining, &modified_register_index)) {
        modified_x_registers_remaining &= ~(UINT32_C(1) << modified_register_index);
        mcontext.regs[modified_register_index] = thread_context.x[modified_register_index];
      }
      mcontext.sp = thread_context.sp;
      mcontext.pc = thread_context.pc;
      mcontext.pstate = thread_context.pstate;
      if (mcontext_fpsimd) {
        mcontext_fpsimd->fpsr = thread_context.fpsr;
        mcontext_fpsimd->fpcr = thread_context.fpcr;
        uint32_t modified_v_registers_remaining = ex.modified_v_registers();
        while (rex::bit_scan_forward(modified_v_registers_remaining, &modified_register_index)) {
          modified_v_registers_remaining &= ~(UINT32_C(1) << modified_register_index);
          std::memcpy(&mcontext_fpsimd->vregs[modified_register_index],
                      &thread_context.v[modified_register_index], sizeof(vec128_t));
          mcontext.regs[modified_register_index] = thread_context.x[modified_register_index];
        }
      }
#endif  // REX_PLATFORM_MAC
#endif  // REX_ARCH
      return;
    }
  }

  // No handler took it: a real crash. Returning would re-run the faulting
  // instruction forever (a silent hang at 100% CPU), so put back the handler
  // that was there before ours and return: the fault re-runs into it. That is
  // the default action (terminate with a report) unless the process had its
  // own crash reporter - a PS5 title records the crash in /app0/boot.txt,
  // which is otherwise the only trace a crash there leaves.
#if REX_PLATFORM_PS5
  Ps5FaultLine("UNHANDLED", signal_number, signal_info, ps5_fault_rip, true);
  Ps5FaultStack(uint64_t(reinterpret_cast<ucontext_t*>(signal_context)->uc_mcontext.mc_rsp));
#endif
  const struct sigaction* original = nullptr;
  switch (signal_number) {
    case SIGILL:
      original = &original_sigill_handler_;
      break;
    case SIGSEGV:
      original = &original_sigsegv_handler_;
      break;
#if REX_PLATFORM_MAC || REX_PLATFORM_PS5
    case SIGBUS:
      original = &original_sigbus_handler_;
      break;
#endif
    default:
      break;
  }
  if (!original || sigaction(signal_number, original, nullptr) != 0) {
    signal(signal_number, SIG_DFL);
  }
}

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!signal_handlers_installed_) {
    struct sigaction signal_handler;

    std::memset(&signal_handler, 0, sizeof(signal_handler));
    signal_handler.sa_sigaction = ExceptionHandlerCallback;
    signal_handler.sa_flags = SA_SIGINFO;

    if (sigaction(SIGILL, &signal_handler, &original_sigill_handler_) != 0) {
      assert_always("Failed to install new SIGILL handler");
    }
    if (sigaction(SIGSEGV, &signal_handler, &original_sigsegv_handler_) != 0) {
      assert_always("Failed to install new SIGSEGV handler");
    }
#if REX_PLATFORM_MAC || REX_PLATFORM_PS5
    if (sigaction(SIGBUS, &signal_handler, &original_sigbus_handler_) != 0) {
      assert_always("Failed to install new SIGBUS handler");
    }
#endif
    signal_handlers_installed_ = true;
  }

  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < rex::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }

  bool has_any = false;
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first) {
      has_any = true;
      break;
    }
  }
  if (!has_any) {
    if (signal_handlers_installed_) {
      if (sigaction(SIGILL, &original_sigill_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGILL handler");
      }
      if (sigaction(SIGSEGV, &original_sigsegv_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGSEGV handler");
      }
#if REX_PLATFORM_MAC || REX_PLATFORM_PS5
      if (sigaction(SIGBUS, &original_sigbus_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGBUS handler");
      }
#endif
      signal_handlers_installed_ = false;
    }
  }
}

}  // namespace rex::arch

#endif  // REX_PLATFORM_POSIX
