#include "ray.hpp"
#include <cstring>
#include <limits>
namespace vulkano {
namespace {
void range(const Buffer &b, VkDeviceSize o, VkDeviceSize n) {
    require(n && o <= b.size && n <= b.size - o, "Acceleration structure input exceeds buffer");
}
void input(Device &d, const Buffer &b) {
    require(b.owner() == &d && (b.usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR),
            "Acceleration input requires matching device and build-input usage");
}
} // namespace
VkDeviceAddress bufferAddress(const Buffer &b) {
    require((b.d->enabled & BufferAddress) && (b.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT),
            "Buffer requires enabled device-address feature and usage");
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer = b.buffer;
    return b.d->extensions->getBufferAddress(b.d->device, &i);
}
AccelerationStructure::AccelerationStructure(std::shared_ptr<Device> device, const std::vector<Geometry> &descriptors,
                                             bool update)
    : Resource(std::move(device)) {
    require(d->enabled & (RayQuery | RayPipeline), "Ray tracing feature was not enabled");
    require(!descriptors.empty() && descriptors.size() <= d->extensions->accelerationProperties.maxGeometryCount,
            "Invalid geometry count");
    if (update)
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    uint64_t primitives = 0;
    for (const auto &g : descriptors) {
        require(g.vertices && g.primitiveCount, "Geometry requires a vertex/AABB buffer and primitives");
        input(*d, *g.vertices);
        inputs.push_back(g.vertices);
        primitives += g.primitiveCount;
        VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geo.flags = g.flags;
        if (g.boxes) {
            require(g.vertexOffset % 8 == 0 && g.stride >= 24 && g.stride % 8 == 0,
                    "AABB stride/offset must align to 8 bytes");
            range(*g.vertices, g.vertexOffset, uint64_t(g.primitiveCount - 1) * g.stride + 24);
            geo.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
            auto &a = geo.geometry.aabbs;
            a.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
            a.data.deviceAddress = bufferAddress(*g.vertices) + g.vertexOffset;
            a.stride = g.stride;
        } else {
            require(g.vertexCount > 0 && g.format == VK_FORMAT_R32G32B32_SFLOAT && g.vertexOffset % 4 == 0 &&
                        g.stride >= 12 && g.stride % 4 == 0,
                    "Triangles require aligned float3 vertices");
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(d->physical, g.format, &fp);
            require(fp.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR,
                    "Unsupported ray vertex format");
            range(*g.vertices, g.vertexOffset, uint64_t(g.vertexCount - 1) * g.stride + 12);
            geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            auto &t = geo.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = g.format;
            t.vertexData.deviceAddress = bufferAddress(*g.vertices) + g.vertexOffset;
            t.vertexStride = g.stride;
            t.maxVertex = g.vertexCount - 1;
            t.indexType = g.indexType;
            if (g.indices) {
                input(*d, *g.indices);
                require(g.indexType == VK_INDEX_TYPE_UINT16 || g.indexType == VK_INDEX_TYPE_UINT32,
                        "Invalid acceleration index type");
                uint32_t size = g.indexType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
                require(g.indexOffset % size == 0, "Misaligned acceleration indices");
                range(*g.indices, g.indexOffset, uint64_t(g.primitiveCount) * 3 * size);
                inputs.push_back(g.indices);
                t.indexData.deviceAddress = bufferAddress(*g.indices) + g.indexOffset;
            } else
                require(g.indexType == VK_INDEX_TYPE_NONE_KHR && uint64_t(g.primitiveCount) * 3 <= g.vertexCount,
                        "Insufficient non-indexed vertices");
        }
        geometries.push_back(geo);
        primitiveCounts.push_back(g.primitiveCount);
    }
    require(primitives <= d->extensions->accelerationProperties.maxPrimitiveCount, "Too many acceleration primitives");
    allocate();
}
AccelerationStructure::AccelerationStructure(std::shared_ptr<Device> device,
                                             const std::vector<AccelerationInstance> &instances, bool update)
    : Resource(std::move(device)) {
    require(d->enabled & (RayQuery | RayPipeline), "Ray tracing feature was not enabled");
    type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    require(!instances.empty() && instances.size() <= d->extensions->accelerationProperties.maxInstanceCount,
            "Invalid instance count");
    if (update)
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    std::vector<VkAccelerationStructureInstanceKHR> data;
    for (const auto &i : instances) {
        require(i.structure && i.structure->owner() == d.get() &&
                    i.structure->type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                "Instance requires a same-device primitive acceleration structure");
        require(i.customIndex <= 0xffffff && i.mask <= 255 && i.recordOffset <= 0xffffff && i.flags <= 255,
                "Instance field overflow");
        VkAccelerationStructureInstanceKHR v{};
        v.transform = i.transform;
        v.instanceCustomIndex = i.customIndex;
        v.mask = i.mask;
        v.instanceShaderBindingTableRecordOffset = i.recordOffset;
        v.flags = i.flags;
        v.accelerationStructureReference = i.structure->address();
        data.push_back(v);
        children.push_back(i.structure);
    }
    auto b = std::make_shared<Buffer>(d, data.size() * sizeof(data[0]),
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      Storage::Shared, true);
    b->write(0, data.data(), data.size() * sizeof(data[0]));
    inputs.push_back(b);
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.data.deviceAddress = bufferAddress(*b);
    geometries.push_back(g);
    primitiveCounts.push_back(uint32_t(data.size()));
    allocate();
}
void AccelerationStructure::allocate() {
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = type;
    build.flags = flags;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = uint32_t(geometries.size());
    build.pGeometries = geometries.data();
    d->extensions->buildSizes(d->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                              primitiveCounts.data(), &sizes);
    storage = std::make_shared<Buffer>(d, sizes.accelerationStructureSize,
                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       Storage::Private);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = storage->buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = type;
    check(d->extensions->createAcceleration(d->device, &ci, nullptr, &acceleration),
          "vkCreateAccelerationStructureKHR");
}
VkDeviceAddress AccelerationStructure::address() const {
    VkAccelerationStructureDeviceAddressInfoKHR i{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    i.accelerationStructure = acceleration;
    return d->extensions->getAccelerationAddress(d->device, &i);
}
AccelerationStructure::~AccelerationStructure() {
    if (acceleration)
        d->extensions->destroyAcceleration(d->device, acceleration, nullptr);
}
void Command::build(std::shared_ptr<AccelerationStructure> target, bool update) {
    recording();
    require(target && target->owner() == d.get(), "Invalid acceleration structure device");
    require(!update || (target->flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR),
            "Acceleration structure was not created for refit");
    uint64_t size = update ? target->sizes.updateScratchSize : target->sizes.buildScratchSize;
    uint64_t alignment = d->extensions->accelerationProperties.minAccelerationStructureScratchOffsetAlignment;
    require(alignment && size <= UINT64_MAX - alignment, "Scratch allocation overflow");
    auto scratch = std::make_shared<Buffer>(
        d, size + alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        Storage::Private);
    buffers.push_back(scratch);
    buffers.push_back(target->storage);
    for (auto &b : target->inputs)
        buffers.push_back(b);
    operations.push_back([target, scratch, alignment, update](Command &c) {
        auto built = [&](const auto &a) {
            auto it = c.accelerationStates.find(a.get());
            return it == c.accelerationStates.end() ? a->built : it->second;
        };
        require(!update || built(target), "Build acceleration structure before refit");
        for (const auto &child : target->children)
            require(built(child), "Build primitive acceleration structures before instances");
        c.barrier();
        auto address = bufferAddress(*scratch);
        address = (address + alignment - 1) & ~(alignment - 1);
        VkAccelerationStructureBuildGeometryInfoKHR i{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        i.type = target->type;
        i.flags = target->flags;
        i.mode =
            update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        i.srcAccelerationStructure = update ? target->acceleration : VK_NULL_HANDLE;
        i.dstAccelerationStructure = target->acceleration;
        i.geometryCount = uint32_t(target->geometries.size());
        i.pGeometries = target->geometries.data();
        i.scratchData.deviceAddress = address;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        for (auto n : target->primitiveCounts)
            ranges.push_back({n, 0, 0, 0});
        const auto *ptr = ranges.data();
        c.d->extensions->buildAcceleration(c.command, 1, &i, &ptr);
        c.accelerationStates[target.get()] = true;
    });
}
} // namespace vulkano
