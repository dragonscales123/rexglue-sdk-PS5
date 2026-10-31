> [!CAUTION]
> This project is in early development. Expect things to not work quite right and there to be significant changes and breaking public API updates as development progresses. Contributions and feedback are welcome, but please be aware that the codebase is still evolving rapidly.

<h1 align="center">
  <br>
  <a href="https://github.com/rexglue/rexglue-sdk">
    <img src="https://github.com/rexglue/rexglue-media/blob/main/ReX_Banner.png" alt="ReXGlue banner">
  </a>
  <br>
  <br>
  <a href="https://discord.gg/CNTxwSNZfT">
    <img src="https://img.shields.io/badge/Discord-Join%20Server-5865F2?logo=discord&logoColor=white" alt="Discord">
  </a>
  <a href="https://github.com/rexglue/rexglue-sdk/stargazers">
    <img src="https://img.shields.io/github/stars/rexglue/rexglue-sdk" alt="rexglue-sdk stargazers">
  </a>
</h1>

ReXGlue converts Xbox 360 PowerPC code into portable C++ that runs natively on modern platforms.

ReXGlue is heavily rooted on the foundations of [Xenia](https://github.com/xenia-project), the Xbox 360 emulator. Rather than interpreting or JIT-compiling PPC instructions at runtime, ReXGlue takes a different path: it generates C++ source code ahead of time, an approach inspired by [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and [rexdex's recompiler](https://github.com/rexdex/recompiler).

Latest SDK builds and releases are published on [GitHub Releases](https://github.com/rexglue/rexglue-sdk/releases). Join the [Discord server](https://discord.gg/CNTxwSNZfT) for updates and share what you have created.

## PS5 port (this branch)

> [!NOTE]
> This is the `ps5-port` branch of a fork. It adds a PS5 (homebrew) target to ReXGlue: titles built on it run natively on a jailbroken PS5, rendering through Vulkan on Mesa's RADV. It is unofficial and not supported by the ReXGlue team. The first title on it is [re:Blue PS5](https://github.com/dragonscales123/rebluePS5/tree/ps5-port) (Blue Dragon), which plays through its first disc.

### What changed

**Platform**
- `REX_PLATFORM_PS5` (from prospero clang's `__PROSPERO__`) and `REX_PLATFORM_POSIX`, which Linux, macOS and the PS5 share, in `include/rex/platform.h`. CMake's `elseif(PS5)` branch sets the `ps5-amd64` platform.
- The runtime builds as a static library: a PS5 title is one executable.
- The code generator never runs on the console. A cross build imports a host-built `rexglue` through `REXGLUE_HOST_TOOL`.

**Guest memory** (`src/core/memory_ps5.cpp`, `Memory::MapViewsPs5` in `src/system/xmemory.cpp`)
- One direct-memory arena, reserved at `0x1000000000`, backs the 4 GiB guest space and its physical views. They're mapped at fixed addresses with 64 KiB allocation granularity, and page protection is tracked in a shadow table.

**Exceptions, threads, clocks** (`*_posix.cpp`)
- The MMIO and fault handler reads FreeBSD's `mcontext` (the `mc_*` registers, with XMM state in the FXSAVE area) and also handles SIGBUS.
- Threads use `pthread_set_name_np`, `cpuset_t` affinity and SIGUSR1/SIGUSR2.

**Fibers** (`src/core/fiber_posix.cpp`, `include/rex/thread/fiber.h`)
- The console's `getcontext()` writes a larger context than the SDK's `ucontext_t` declares. That overran every thread's `Fiber` object and corrupted the heap at boot.
- On PS5, fibers switch with their own small routine instead (`rex_ps5_fiber_switch`). It saves the callee-saved registers, MXCSR and the x87 control word on the fiber's stack, and doesn't use ucontext at all.

**Physical-heap host offset** (`resources/templates/codegen/pch_h.inja`)
- With allocation granularity above 4 KiB, guest addresses at `0xE0000000` and up sit `0x1000` higher in host memory. The runtime (`PhysicalHostOffset`) applied this on PS5, but the codegen template only enabled it for Windows and Apple arm64. Recompiled code therefore read the kernel's writes into that heap 4 KiB away.
- In re:Blue this showed up as XMA contexts reading back as null and the audio mixer storing to address 0. The template now enables the offset on PS5. Any game must be regenerated after this change: the template isn't part of codegen's input fingerprint.

**Libraries**
- The SDK's VulkanMemoryAllocator is renamed to `rexsdk_*` in the static runtime (`cmake/rex_ps5_isolate_vma.cmake`), so it doesn't collide with an application's own VMA.
- SDL3 and FFmpeg build for the PS5 with `tools/patches/sdl-ps5.patch` and `tools/patches/ffmpeg-ps5.patch`. The submodules point at upstream, so apply the patches to them before building.
- The UI builds without X11 or Wayland. RenderDoc is off. `stb_image` is static, so it doesn't clash with a title's copy.
- Smaller fixes: libc++ 18 gaps, endianness in `sha256.cpp`, the executable path (`/app0/eboot.bin`), and dynamic library names.

### Building a title on it

You need mihawk-99's [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) and [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (RADV), and clang/lld 20. The SDK is consumed as a subdirectory of the title, configured with the payload SDK's `toolchain/prospero.cmake` and `-DREXGLUE_HOST_TOOL=<host rexglue>`. [re:Blue PS5's `ps5/build.sh`](https://github.com/dragonscales123/rebluePS5/blob/ps5-port/ps5/build.sh) and [`docs/PS5.md`](https://github.com/dragonscales123/rebluePS5/blob/ps5-port/docs/PS5.md) are a complete example: host tools, shader cache, codegen, link and packaging.

## Builds

| Channel | CI | Download |
| --- | --- | --- |
| Release | [![win-amd64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-win-amd64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-win-amd64.yaml) [![linux-amd64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-amd64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-amd64.yaml) [![linux-arm64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-aarch64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-aarch64.yaml) | [Latest stable](https://github.com/rexglue/rexglue-sdk/releases/latest) |
| Nightly | [![nightly](https://github.com/rexglue/rexglue-sdk/actions/workflows/nightly.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/nightly.yaml) | [Latest pre-release](https://github.com/rexglue/rexglue-sdk/releases?q=prerelease%3Atrue) |

## Quickstart

For quick start guide, full CLI reference, and config file options, see the [wiki](https://github.com/rexglue/rexglue-sdk/wiki).

# **Disclaimer**
ReXGlue is not affiliated with nor endorsed by Microsoft or Xbox. It is an independent project created for educational and development purposes. All trademarks and copyrights belong to their respective owners. 

This project is not intended to promote piracy nor unauthorized use of copyrighted material. Any misuse of this software to endorse or enable this type of activity is strictly prohibited.


# Credits

## ReXGlue
- [Tom (crack)](https://github.com/tomcl7) - Project Founder
- [Loreaxe](https://github.com/Loreaxe) - Linux Contributor
- [mystixor](https://github.com/Mystixor) - Windows Contributor
- [Graine25](https://github.com/Graine25) - Project Support
- [Carlos Estrague (mrcmunir)](https://github.com/mrcmunir) - Linux / ARM64 Contributor
- [sanjay900](https://github.com/sanjay900) - Linux / SDL Contributor
- [Toby](https://github.com/TbyDtch) - Project Support
- [Roxxsen](https://github.com/Roxxsen) - CI/CD Contributor

The list above is not exhaustive. Thanks to everyone in the ReXGlue community who contributes code, files issues, tests builds, and keeps the project moving.

## Very Special Thank You:
- [Project Xenia](https://github.com/xenia-project/xenia/tree/master/src/xenia) - Their invaluable work on Xbox 360 emulation laid the groundwork for ReXGlue's development. This project (and numerous others) would not exist without their hard work and dedication.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) - For pioneering the modern static recompilation approach for Xbox 360. A lot of the codegen analysis logic and instruction translations are based on their work. Thank you!
- [rexdex's recompiler](https://github.com/rexdex/recompiler) - The OG static recompiler for Xbox 360. 
- Many others in the Xbox 360 homebrew and modding communities whose work and research have contributed to the collective knowledge that makes projects like this possible.
