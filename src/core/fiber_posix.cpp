/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless a POSIX/XSI feature level
// is requested before <ucontext.h> is included. fiber.h includes ucontext.h,
// so the feature macro must be set in this translation unit first.
#define _XOPEN_SOURCE 700
#endif

#include <rex/platform.h>
#if REX_PLATFORM_POSIX

#include <rex/thread/fiber.h>

#include <cassert>
#include <cstdint>
#include <cstring>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

#if REX_PLATFORM_PS5
// PS5: getcontext()/swapcontext() from the console's libkernel write a
// context larger than the SDK's ucontext_t, which overran the heap block
// holding the Fiber (found by the title's debug heap: zeros past the end of
// the 976-byte block allocated in ConvertCurrentThread). Fibers here only
// switch at SwitchTo calls, so saving what the SysV ABI says a call keeps
// (rbx, rbp, r12-r15, MXCSR and the x87 control word) on the fiber's own
// stack is the whole context; the signal mask is the thread's anyway.
//
//   void rex_ps5_fiber_switch(void** save_sp, void* load_sp)
//
// Saved frame, from the stack pointer up: MXCSR (4 bytes), x87 control word
// (2), 2 spare, r15, r14, r13, r12, rbx, rbp, return address.
extern "C" void rex_ps5_fiber_switch(void** save_sp, void* load_sp);
asm(R"(
    .text
    .p2align 4
    .globl rex_ps5_fiber_switch
    .hidden rex_ps5_fiber_switch
    .type rex_ps5_fiber_switch, @function
rex_ps5_fiber_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size rex_ps5_fiber_switch, .-rex_ps5_fiber_switch
)");

Fiber* Fiber::ConvertCurrentThread() {
  // The thread's context is saved by the first SwitchTo away from it.
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  if (stack_size < 64 * 1024) stack_size = 64 * 1024;
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  // A first frame for rex_ps5_fiber_switch to "return" through into
  // Trampoline. Trampoline then starts with rsp = 8 (mod 16), as after a
  // call, and a zero return address above it ends any backtrace there.
  uintptr_t top = reinterpret_cast<uintptr_t>(f->stack_.data() + f->stack_.size());
  top &= ~uintptr_t(15);
  auto* slot = reinterpret_cast<uint64_t*>(top);
  *--slot = 0;                                                   // Trampoline's return address
  *--slot = reinterpret_cast<uint64_t>(&Fiber::Trampoline);      // ret target
  for (int i = 0; i < 6; ++i) *--slot = 0;                       // rbp rbx r12 r13 r14 r15
  uint32_t mxcsr;
  uint16_t fpucw;
  asm volatile("stmxcsr %0" : "=m"(mxcsr));
  asm volatile("fnstcw %0" : "=m"(fpucw));
  --slot;
  *slot = 0;
  std::memcpy(slot, &mxcsr, 4);
  std::memcpy(reinterpret_cast<uint8_t*>(slot) + 4, &fpucw, 2);
  f->sp_ = slot;
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before the switch landed here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
  // A fiber's entry must switch away for good; there is nothing to return to.
  __builtin_trap();
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_ps5_fiber_switch(&from->sp_, target->sp_);
}

#else  // !REX_PLATFORM_PS5

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = f->stack_.data();
  f->context_.uc_stack.ss_size = f->stack_.size();
  f->context_.uc_link = nullptr;
  // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before swapcontext returned here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  swapcontext(&from->context_, &target->context_);
}

#endif  // REX_PLATFORM_PS5

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // No POSIX equivalent of ConvertFiberToThread; stack_ is freed by the vector destructor.
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_POSIX
