#pragma once
#include "engine.hpp"
namespace vulkano {
struct Extensions {
    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR vertexDivisor{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR};
    VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR vertexDivisorProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR, nullptr, 1, VK_TRUE};
    bool vertexDivisorKHR = false;
    VkPhysicalDeviceSynchronization2Features sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    PFN_vkCmdPipelineBarrier2 pipelineBarrier2 = nullptr;
    VkPhysicalDeviceTensorFeaturesARM tensor{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TENSOR_FEATURES_ARM};
    VkPhysicalDeviceTensorPropertiesARM tensorProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TENSOR_PROPERTIES_ARM};
    PFN_vkCreateTensorARM createTensor = nullptr;
    PFN_vkDestroyTensorARM destroyTensor = nullptr;
    PFN_vkCreateTensorViewARM createTensorView = nullptr;
    PFN_vkDestroyTensorViewARM destroyTensorView = nullptr;
    PFN_vkGetTensorMemoryRequirementsARM tensorMemoryRequirements = nullptr;
    PFN_vkGetDeviceTensorMemoryRequirementsARM deviceTensorMemoryRequirements = nullptr;
    PFN_vkBindTensorMemoryARM bindTensorMemory = nullptr;
    PFN_vkCmdCopyTensorARM copyTensor = nullptr;
    VkPhysicalDeviceDataGraphFeaturesARM graph{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_FEATURES_ARM};
    VkPhysicalDevicePipelineCreationCacheControlFeatures cacheControl{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES};
    std::map<uint32_t, std::vector<VkQueueFamilyDataGraphPropertiesARM>> graphQueues;
    PFN_vkCreateDataGraphPipelinesARM createGraphPipelines = nullptr;
    PFN_vkCreateDataGraphPipelineSessionARM createGraphSession = nullptr;
    PFN_vkDestroyDataGraphPipelineSessionARM destroyGraphSession = nullptr;
    PFN_vkGetDataGraphPipelineSessionBindPointRequirementsARM graphBindRequirements = nullptr;
    PFN_vkGetDataGraphPipelineSessionMemoryRequirementsARM graphMemoryRequirements = nullptr;
    PFN_vkBindDataGraphPipelineSessionMemoryARM bindGraphMemory = nullptr;
    PFN_vkCmdDispatchDataGraphARM dispatchGraph = nullptr;
    PFN_vkGetDataGraphPipelineAvailablePropertiesARM graphAvailableProperties = nullptr;
    PFN_vkGetDataGraphPipelinePropertiesARM graphProperties = nullptr;
    void inspectGraphQueues(VkInstance, VkPhysicalDevice, const std::vector<VkQueueFamilyProperties> &);
    VkPhysicalDeviceBufferDeviceAddressFeatures address{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingMotionBlurFeaturesNV motion{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MOTION_BLUR_FEATURES_NV};
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
    PFN_vkCmdCopyAccelerationStructureKHR copyAcceleration = nullptr;
    PFN_vkCmdCopyAccelerationStructureToMemoryKHR serializeAcceleration = nullptr;
    PFN_vkCmdCopyMemoryToAccelerationStructureKHR deserializeAcceleration = nullptr;
    PFN_vkGetDeviceAccelerationStructureCompatibilityKHR accelerationCompatibility = nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR accelerationPropertiesQuery = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR createRayPipelines = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR getGroupHandles = nullptr;
    PFN_vkCmdTraceRaysKHR traceRays = nullptr;
    PFN_vkGetSemaphoreCounterValue semaphoreValue = nullptr;
    PFN_vkSignalSemaphore signalSemaphore = nullptr;
    PFN_vkWaitSemaphores waitSemaphores = nullptr;
    PFN_vkCmdDrawMeshTasksEXT drawMesh = nullptr;
    PFN_vkCmdDrawMeshTasksIndirectEXT drawMeshIndirect = nullptr;
    PFN_vkCmdDrawIndirectCount drawIndirectCount = nullptr;
    PFN_vkCmdDrawIndexedIndirectCount drawIndexedIndirectCount = nullptr;
    PFN_vkCmdDrawMeshTasksIndirectCountEXT drawMeshIndirectCount = nullptr;
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
    VkPhysicalDeviceDepthStencilResolveProperties depthResolveProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
    PFN_vkCreateRenderPass2 createRenderPass2 = nullptr;
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fragmentRate{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR fragmentRateProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
    std::vector<VkPhysicalDeviceFragmentShadingRateKHR> fragmentRates;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR matrix{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceCooperativeMatrixPropertiesKHR matrixProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR};
    std::vector<VkCooperativeMatrixPropertiesKHR> matrixConfigurations;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    VkExternalSemaphoreProperties syncFdProperties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;
#ifdef __ANDROID__
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID hardwareBufferProperties = nullptr;
#endif
    uint64_t available = 0, availableExtra = 0;
    VkPhysicalDeviceTextureCompressionASTCHDRFeatures astcHdr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES};
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT generated{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT};
    VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT generatedProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_PROPERTIES_EXT};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamicState{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceMaintenance5Features maintenance5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    bool generatedVertexInput = false;
    PFN_vkCmdBindVertexBuffers2 bindVertexBuffers2 = nullptr;
    PFN_vkCreateIndirectCommandsLayoutEXT createGeneratedLayout = nullptr;
    PFN_vkDestroyIndirectCommandsLayoutEXT destroyGeneratedLayout = nullptr;
    PFN_vkCreateIndirectExecutionSetEXT createExecutionSet = nullptr;
    PFN_vkDestroyIndirectExecutionSetEXT destroyExecutionSet = nullptr;
    PFN_vkUpdateIndirectExecutionSetPipelineEXT updateExecutionSet = nullptr;
    PFN_vkGetGeneratedCommandsMemoryRequirementsEXT generatedMemoryRequirements = nullptr;
    PFN_vkCmdExecuteGeneratedCommandsEXT executeGenerated = nullptr;
    VkPhysicalDeviceTileShadingFeaturesQCOM tile{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_SHADING_FEATURES_QCOM};
    VkPhysicalDeviceTileShadingPropertiesQCOM tileProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_SHADING_PROPERTIES_QCOM};
    VkPhysicalDeviceTilePropertiesFeaturesQCOM tileQuery{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_PROPERTIES_FEATURES_QCOM};
    PFN_vkCmdBeginPerTileExecutionQCOM beginTile = nullptr;
    PFN_vkCmdEndPerTileExecutionQCOM endTile = nullptr;
    PFN_vkCmdDispatchTileQCOM dispatchTile = nullptr;
    PFN_vkGetFramebufferTilePropertiesQCOM framebufferTiles = nullptr;
    bool core12 = false, core13 = false;
    void *chain = nullptr;
    std::vector<VkExtensionProperties> supported;
    void inspect(VkPhysicalDevice, uint32_t api, const std::vector<VkExtensionProperties> &);
    void enable(uint64_t, std::vector<const char *> &, uint64_t extra = 0);
    void load(Device &);
    void enableExtra(uint64_t, std::vector<const char *> &);
};
} // namespace vulkano
