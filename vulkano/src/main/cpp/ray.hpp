#pragma once
#include "extensions.hpp"
namespace vulkano {
struct Geometry {
    std::shared_ptr<Buffer> vertices, indices, motionVertices;
    VkDeviceSize vertexOffset = 0, indexOffset = 0, stride = 12;
    VkDeviceSize motionVertexOffset = 0;
    uint32_t vertexCount = 0, primitiveCount = 0;
    VkFormat format = VK_FORMAT_R32G32B32_SFLOAT;
    VkIndexType indexType = VK_INDEX_TYPE_NONE_KHR;
    VkGeometryFlagsKHR flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    bool boxes = false;
};
struct AccelerationInstance;
struct AccelerationStructure : Resource {
    VkAccelerationStructureKHR acceleration = VK_NULL_HANDLE;
    VkAccelerationStructureTypeKHR type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    std::shared_ptr<Buffer> storage;
    std::vector<std::shared_ptr<Buffer>> inputs;
    std::vector<std::shared_ptr<AccelerationStructure>> children;
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureGeometryMotionTrianglesDataNV> motionTriangles;
    std::vector<uint32_t> primitiveCounts;
    bool built = false, copyDestination = false;
    uint64_t generation = 0, copyGeneration = 0;
    std::weak_ptr<AccelerationStructure> copySource;
    std::shared_ptr<AccelerationStructure> refitInputs;
    VkCopyAccelerationStructureModeKHR copyMode = VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
    AccelerationStructure(std::shared_ptr<Device>, const std::vector<Geometry> &, bool update, bool compact = false,
                          bool allocateStorage = true);
    AccelerationStructure(std::shared_ptr<Device>, const std::vector<AccelerationInstance> &, bool update,
                          bool compact = false, bool allocateStorage = true);
    AccelerationStructure(std::shared_ptr<AccelerationStructure> source, bool compact);
    AccelerationStructure(std::shared_ptr<Device>, VkAccelerationStructureTypeKHR, VkDeviceSize,
                          VkBuildAccelerationStructureFlagsKHR, const std::vector<uint32_t> &schema = {});
    void allocate(bool querySizes = true);
    void queryBuildSizes();
    void validateRefitInputs(const AccelerationStructure &) const;
    VkDeviceAddress address() const;
    ~AccelerationStructure() override;
};
struct AccelerationInstance {
    std::shared_ptr<AccelerationStructure> structure;
    VkTransformMatrixKHR transform{};
    VkTransformMatrixKHR transformEnd{};
    VkSRTDataNV srtStart{}, srtEnd{};
    VkAccelerationStructureMotionInstanceTypeNV motionType = VK_ACCELERATION_STRUCTURE_MOTION_INSTANCE_TYPE_STATIC_NV;
    uint32_t customIndex = 0, mask = 255, recordOffset = 0;
    VkGeometryInstanceFlagsKHR flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
};
VkAccelerationStructureMotionInstanceNV packMotionInstance(const AccelerationInstance &, VkDeviceAddress);
VkDeviceAddress bufferAddress(const Buffer &);
std::vector<uint8_t> serializeAccelerationStructure(std::shared_ptr<AccelerationStructure>);
std::vector<uint64_t> accelerationArchiveAddresses(const std::vector<uint8_t> &);
std::shared_ptr<AccelerationStructure>
restoreAccelerationStructure(std::shared_ptr<Device>, const std::vector<uint8_t> &,
                             const std::map<uint64_t, std::shared_ptr<AccelerationStructure>> &);
struct RayTracingPipeline : Pipeline {
    bool supportsMotion = false;
    std::shared_ptr<Buffer> table;
    VkStridedDeviceAddressRegionKHR raygen{}, miss{}, hit{}, callable{};
    RayTracingPipeline(std::shared_ptr<Device>, std::vector<BindingLayout>, uint32_t, const std::vector<Shader> &,
                       const std::vector<VkShaderStageFlagBits> &,
                       const std::vector<VkRayTracingShaderGroupCreateInfoKHR> &, uint32_t recursion,
                       bool indirectBindable = false, bool motion = false);
};
} // namespace vulkano
