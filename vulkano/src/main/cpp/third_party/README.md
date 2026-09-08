# Vulkan Memory Allocator

Vendored `vk_mem_alloc.h` from GPUOpen VulkanMemoryAllocator **v3.2.1**,
commit `c788c52156f3ef7bc7ab769cb03c110a53ac8fcb`.

Source: https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tree/v3.2.1

The MIT license is reproduced in `VMA-LICENSE.txt` and the header. The build does
not download native dependencies. `allocator.cpp` is the sole implementation unit.

# SPIRV-Reflect

`spirv-reflect/` contains the C implementation, header and bundled SPIR-V header
from Khronos SPIRV-Reflect tag `vulkan-sdk-1.3.296.0`, commit
`8542f37bd9bb202e6c49dc6a9da364c58c34d2a4`. The Apache 2.0 license is included.

Source: https://github.com/KhronosGroup/SPIRV-Reflect/tree/vulkan-sdk-1.3.296.0


- **Vulkan-Headers v1.4.335**: C API headers for Android and host builds, including optional extension declarations.
  Source and full licenses: [vulkan-headers/README.md](vulkan-headers/README.md).
  Runtime Vulkan version and device extension support are still queried; including a header does not enable a feature.
