#pragma once
#include "engine.hpp"
namespace vulkano {
struct Extensions {
    VkPhysicalDeviceBufferDeviceAddressFeatures address{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceMeshShaderPropertiesEXT meshProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    PFN_vkGetBufferDeviceAddress getBufferAddress = nullptr;
    PFN_vkCreateAccelerationStructureKHR createAcceleration = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAcceleration = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR buildSizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelerationAddress = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR buildAcceleration = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR createRayPipelines = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR getGroupHandles = nullptr;
    PFN_vkCmdTraceRaysKHR traceRays = nullptr;
    PFN_vkGetSemaphoreCounterValue semaphoreValue = nullptr;
    PFN_vkSignalSemaphore signalSemaphore = nullptr;
    PFN_vkWaitSemaphores waitSemaphores = nullptr;
    PFN_vkCmdDrawMeshTasksEXT drawMesh = nullptr;
    PFN_vkCmdDrawMeshTasksIndirectEXT drawMeshIndirect = nullptr;
    VkPhysicalDeviceVulkan12Features coreFeatures12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceMultiviewProperties multiviewProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
    VkPhysicalDeviceTimelineSemaphoreProperties timelineProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES};
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDevice8BitStorageFeatures storage8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    VkPhysicalDeviceShaderAtomicInt64Features atomic64{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures subgroupTypes{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES};
    VkPhysicalDeviceVulkanMemoryModelFeatures memoryModel{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
    uint64_t available = 0;
    bool core12 = false;
    void *chain = nullptr;
    std::vector<VkExtensionProperties> supported;
    void inspect(VkPhysicalDevice, uint32_t api, const std::vector<VkExtensionProperties> &);
    void enable(uint64_t, std::vector<const char *> &);
    void load(Device &);
};
} // namespace vulkano
