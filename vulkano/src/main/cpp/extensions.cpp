#include "extensions.hpp"
#include <algorithm>
#include <cstring>
namespace vulkano {
namespace {
bool has(const std::vector<VkExtensionProperties> &v, const char *s) {
    return std::any_of(v.begin(), v.end(), [&](const auto &p) { return std::strcmp(p.extensionName, s) == 0; });
}
template <class T> void link(void *&head, T &item) {
    item.pNext = head;
    head = &item;
}
} // namespace
void Extensions::inspect(VkPhysicalDevice d, uint32_t api, const std::vector<VkExtensionProperties> &e) {
    supported = e;
    core12 = api >= VK_API_VERSION_1_2;
    void *head = nullptr;
    const bool bda = core12 || has(e, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    if (bda)
        link(head, address);
    if (core12 || has(e, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
        link(head, timeline);
    const bool spirv14 =
        core12 || (has(e, VK_KHR_SPIRV_1_4_EXTENSION_NAME) && has(e, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME));
    const bool as = has(e, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                    has(e, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) && bda &&
                    (core12 || has(e, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME));
    if (as) {
        link(head, acceleration);
        if (spirv14 && has(e, VK_KHR_RAY_QUERY_EXTENSION_NAME))
            link(head, query);
        if (spirv14 && has(e, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME))
            link(head, ray);
    }
    if (spirv14 && has(e, VK_EXT_MESH_SHADER_EXTENSION_NAME))
        link(head, mesh);
    if (core12 || has(e, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
        link(head, indexing);
    if (true)
        link(head, multiview);
    if (has(e, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME))
        link(head, interlock);
    if (core12 || has(e, VK_KHR_8BIT_STORAGE_EXTENSION_NAME))
        link(head, storage8);
    if (core12 || has(e, VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME))
        link(head, atomic64);
    if (has(e, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME))
        link(head, atomicFloat);
    if (core12 || has(e, VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME))
        link(head, subgroupTypes);
    if (core12 || has(e, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME))
        link(head, memoryModel);
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = head;
    vkGetPhysicalDeviceFeatures2(d, &f);
    if (indexing.runtimeDescriptorArray && indexing.descriptorBindingPartiallyBound &&
        indexing.shaderSampledImageArrayNonUniformIndexing && indexing.shaderStorageBufferArrayNonUniformIndexing &&
        indexing.shaderStorageImageArrayNonUniformIndexing && indexing.shaderUniformBufferArrayNonUniformIndexing)
        available |= DescriptorIndexing;
    if (multiview.multiview)
        available |= Multiview;
    if (interlock.fragmentShaderPixelInterlock)
        available |= PixelInterlock;
    if (storage8.storageBuffer8BitAccess)
        available |= Storage8;
    if (atomic64.shaderBufferInt64Atomics)
        available |= Atomics64;
    if (atomicFloat.shaderBufferFloat32Atomics && atomicFloat.shaderBufferFloat32AtomicAdd)
        available |= FloatAtomics;
    if (subgroupTypes.shaderSubgroupExtendedTypes)
        available |= SubgroupExtended;
    if (memoryModel.vulkanMemoryModel && memoryModel.vulkanMemoryModelDeviceScope)
        available |= MemoryModel;
    if (core12) {
        // Query 1.2 as a separate chain: do not duplicate promoted feature structs.
        VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        core.pNext = &coreFeatures12;
        vkGetPhysicalDeviceFeatures2(d, &core);
        if (coreFeatures12.samplerFilterMinmax)
            available |= SamplerMinMax;
        if (coreFeatures12.shaderOutputViewportIndex && coreFeatures12.shaderOutputLayer)
            available |= ViewportLayer;
    } else {
        if (has(e, VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME))
            available |= SamplerMinMax;
        if (has(e, VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME))
            available |= ViewportLayer;
    }
    if (address.bufferDeviceAddress)
        available |= BufferAddress;
    if (timeline.timelineSemaphore)
        available |= Timeline;
    if (address.bufferDeviceAddress && acceleration.accelerationStructure) {
        if (query.rayQuery)
            available |= RayQuery;
        if (ray.rayTracingPipeline)
            available |= RayPipeline;
    }
    if (mesh.meshShader) {
        available |= MeshShader;
        if (mesh.taskShader)
            available |= TaskShader;
    }
    head = nullptr;
    link(head, multiviewProperties);
    if (available & Timeline)
        link(head, timelineProperties);
    if (available & (RayQuery | RayPipeline)) {
        link(head, accelerationProperties);
        if (available & RayPipeline)
            link(head, rayProperties);
    }
    if (available & MeshShader)
        link(head, meshProperties);
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p.pNext = head;
    vkGetPhysicalDeviceProperties2(d, &p);
}
void Extensions::enable(uint64_t f, std::vector<const char *> &names) {
    chain = nullptr;
    auto extension = [&](const char *name) {
        require(has(supported, name), "Missing extension dependency");
        if (std::none_of(names.begin(), names.end(), [&](const char *n) { return std::strcmp(n, name) == 0; }))
            names.push_back(name);
    };
    address = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    timeline = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    acceleration = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    ray = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    mesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    indexing = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    if (f & DescriptorIndexing) {
        indexing.runtimeDescriptorArray = VK_TRUE;
        indexing.descriptorBindingPartiallyBound = VK_TRUE;
        indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        indexing.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        indexing.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        indexing.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
        link(chain, indexing);
        if (!core12)
            extension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    }
    multiview = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    if (f & Multiview) {
        multiview.multiview = VK_TRUE;
        link(chain, multiview);
    }
    interlock = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    if (f & PixelInterlock) {
        interlock.fragmentShaderPixelInterlock = VK_TRUE;
        link(chain, interlock);
        extension(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    }
    storage8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    if (f & Storage8) {
        storage8.storageBuffer8BitAccess = VK_TRUE;
        link(chain, storage8);
        if (!core12)
            extension(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    }
    atomic64 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
    if (f & Atomics64) {
        atomic64.shaderBufferInt64Atomics = VK_TRUE;
        link(chain, atomic64);
        if (!core12)
            extension(VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME);
    }
    atomicFloat = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    if (f & FloatAtomics) {
        atomicFloat.shaderBufferFloat32Atomics = VK_TRUE;
        atomicFloat.shaderBufferFloat32AtomicAdd = VK_TRUE;
        link(chain, atomicFloat);
        extension(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
    }
    subgroupTypes = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES};
    if (f & SubgroupExtended) {
        subgroupTypes.shaderSubgroupExtendedTypes = VK_TRUE;
        link(chain, subgroupTypes);
        if (!core12)
            extension(VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME);
    }
    memoryModel = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
    if (f & MemoryModel) {
        memoryModel.vulkanMemoryModel = VK_TRUE;
        memoryModel.vulkanMemoryModelDeviceScope = VK_TRUE;
        link(chain, memoryModel);
        if (!core12)
            extension(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
    }
    if ((f & SamplerMinMax) && !core12)
        extension(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
    if ((f & ViewportLayer) && !core12)
        extension(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
    if (f & BufferAddress) {
        address.bufferDeviceAddress = VK_TRUE;
        link(chain, address);
        if (!core12)
            extension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }
    if (f & Timeline) {
        timeline.timelineSemaphore = VK_TRUE;
        link(chain, timeline);
        if (!core12)
            extension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    }
    if (f & (RayQuery | RayPipeline)) {
        acceleration.accelerationStructure = VK_TRUE;
        link(chain, acceleration);
        extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        extension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        if (!core12)
            extension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    }
    if (f & RayQuery) {
        query.rayQuery = VK_TRUE;
        link(chain, query);
        extension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }
    if (f & RayPipeline) {
        ray.rayTracingPipeline = VK_TRUE;
        link(chain, ray);
        extension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    }
    if (f & MeshShader) {
        mesh.meshShader = VK_TRUE;
        mesh.taskShader = (f & TaskShader) != 0;
        link(chain, mesh);
        extension(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    }
    if (core12 && (f & (SamplerMinMax | ViewportLayer))) {
        // Keep only non-promoted extensions and Vulkan1.1 multiview in this chain.
        chain = nullptr;
        if (f & (RayQuery | RayPipeline))
            link(chain, acceleration);
        if (f & RayQuery)
            link(chain, query);
        if (f & RayPipeline)
            link(chain, ray);
        if (f & MeshShader)
            link(chain, mesh);
        if (f & PixelInterlock)
            link(chain, interlock);
        if (f & FloatAtomics)
            link(chain, atomicFloat);
        if (f & Multiview)
            link(chain, multiview);
        coreFeatures12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        coreFeatures12.samplerFilterMinmax = bool(f & SamplerMinMax);
        coreFeatures12.shaderOutputViewportIndex = coreFeatures12.shaderOutputLayer = bool(f & ViewportLayer);
        coreFeatures12.bufferDeviceAddress = bool(f & BufferAddress);
        coreFeatures12.timelineSemaphore = bool(f & Timeline);
        coreFeatures12.shaderFloat16 = bool(f & Float16);
        coreFeatures12.shaderInt8 = bool(f & Int8);
        coreFeatures12.storageBuffer8BitAccess = bool(f & Storage8);
        coreFeatures12.shaderBufferInt64Atomics = bool(f & Atomics64);
        coreFeatures12.shaderSubgroupExtendedTypes = bool(f & SubgroupExtended);
        coreFeatures12.vulkanMemoryModel = coreFeatures12.vulkanMemoryModelDeviceScope = bool(f & MemoryModel);
        coreFeatures12.runtimeDescriptorArray = bool(f & DescriptorIndexing);
        coreFeatures12.descriptorBindingPartiallyBound = bool(f & DescriptorIndexing);
        coreFeatures12.shaderSampledImageArrayNonUniformIndexing = bool(f & DescriptorIndexing);
        coreFeatures12.shaderStorageBufferArrayNonUniformIndexing = bool(f & DescriptorIndexing);
        coreFeatures12.shaderStorageImageArrayNonUniformIndexing = bool(f & DescriptorIndexing);
        coreFeatures12.shaderUniformBufferArrayNonUniformIndexing = bool(f & DescriptorIndexing);
        link(chain, coreFeatures12);
    }
    if (!core12 && (f & (RayQuery | RayPipeline | MeshShader))) {
        extension(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
        extension(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    }
}
void Extensions::load(Device &d) {
#define GET(member, name)                                                                                              \
    member = reinterpret_cast<decltype(member)>(vkGetDeviceProcAddr(d.device, name));                                  \
    require(member, "Enabled Vulkan entry point is missing")
    if (d.enabled & BufferAddress) {
        GET(getBufferAddress, core12 ? "vkGetBufferDeviceAddress" : "vkGetBufferDeviceAddressKHR");
    }
    if (d.enabled & Timeline) {
        GET(semaphoreValue, core12 ? "vkGetSemaphoreCounterValue" : "vkGetSemaphoreCounterValueKHR");
        GET(signalSemaphore, core12 ? "vkSignalSemaphore" : "vkSignalSemaphoreKHR");
        GET(waitSemaphores, core12 ? "vkWaitSemaphores" : "vkWaitSemaphoresKHR");
    }
    if (d.enabled & (RayQuery | RayPipeline)) {
        GET(createAcceleration, "vkCreateAccelerationStructureKHR");
        GET(destroyAcceleration, "vkDestroyAccelerationStructureKHR");
        GET(buildSizes, "vkGetAccelerationStructureBuildSizesKHR");
        GET(getAccelerationAddress, "vkGetAccelerationStructureDeviceAddressKHR");
        GET(buildAcceleration, "vkCmdBuildAccelerationStructuresKHR");
    }
    if (d.enabled & RayPipeline) {
        GET(createRayPipelines, "vkCreateRayTracingPipelinesKHR");
        GET(getGroupHandles, "vkGetRayTracingShaderGroupHandlesKHR");
        GET(traceRays, "vkCmdTraceRaysKHR");
    }
    if (d.enabled & MeshShader) {
        GET(drawMesh, "vkCmdDrawMeshTasksEXT");
        GET(drawMeshIndirect, "vkCmdDrawMeshTasksIndirectEXT");
    }
#undef GET
}
} // namespace vulkano
