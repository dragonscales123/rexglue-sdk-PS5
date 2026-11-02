/**
 * @file        ui/gpu_plugin_link_ps5.cpp
 * @brief       PS5: pull the statically linked Xenos GPU into a title
 *
 * rexglue_configure_target compiles this into a PS5 title that asks for
 * GPU_PLUGINS xenos. LoadGpuPlugin (system/gpu_plugin_loader.cpp) refers to
 * rex_gpu_create weakly, so titles without the GPU still link; but a weak
 * reference does not make the linker take an object out of a static library,
 * so without this strong one librexgpu-xenos.a stayed out, rex_gpu_create
 * stayed undefined, and the title's undefined symbol became an import no
 * console module exports.
 */

#include <rex/system/gpu_plugin.h>

extern "C" rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi_version,
                                                        const rex::system::GpuCreateInfo* info);

extern "C" __attribute__((used)) void* const rex_gpu_plugin_link_anchor =
    reinterpret_cast<void*>(&rex_gpu_create);
