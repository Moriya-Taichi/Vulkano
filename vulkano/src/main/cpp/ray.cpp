#include "ray.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace vulkano {
namespace {
void range(const Buffer &b, VkDeviceSize o, VkDeviceSize n) {
    require(n && o <= b.size && n <= b.size - o, "Acceleration structure input exceeds buffer");
}
uint64_t inputSpan(uint32_t count, uint64_t stride, uint32_t tail) {
    require(count && (count == 1 || stride <= (UINT64_MAX - tail) / uint64_t(count - 1)),
            "Acceleration input byte range overflow");
    return uint64_t(count - 1) * stride + tail;
}
void input(Device &d, const Buffer &b) {
    require(b.owner() == &d && (b.usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR),
            "Acceleration input requires matching device and build-input usage");
}
void matrix(const VkTransformMatrixKHR &m) {
    for (const auto &row : m.matrix)
        for (auto n : row)
            require(std::isfinite(n), "Instance transform must be finite");
    const auto &a = m.matrix;
    const double determinant = double(a[0][0]) * (double(a[1][1]) * a[2][2] - double(a[1][2]) * a[2][1]) -
                               double(a[0][1]) * (double(a[1][0]) * a[2][2] - double(a[1][2]) * a[2][0]) +
                               double(a[0][2]) * (double(a[1][0]) * a[2][1] - double(a[1][1]) * a[2][0]);
    require(determinant != 0, "Instance transform must be invertible");
}
void srt(const VkSRTDataNV &value) {
    float data[16];
    std::memcpy(data, &value, sizeof(data));
    for (auto n : data)
        require(std::isfinite(n), "SRT transform must be finite");
    require(value.sx != 0 && value.sy != 0 && value.sz != 0, "SRT scale must be nonzero");
    const double norm = double(value.qx) * value.qx + double(value.qy) * value.qy + double(value.qz) * value.qz +
                        double(value.qw) * value.qw;
    require(std::abs(norm - 1) <= 0.0001, "SRT rotation quaternion must be normalized");
}
} // namespace
VkAccelerationStructureMotionInstanceNV packMotionInstance(const AccelerationInstance &i, VkDeviceAddress address) {
    static_assert(sizeof(VkAccelerationStructureMotionInstanceNV) == 152);
    static_assert(sizeof(VkSRTDataNV) == 64);
    require(i.customIndex <= 0xffffff && i.mask <= 255 && i.recordOffset <= 0xffffff && i.flags <= 255,
            "Instance field overflow");
    VkAccelerationStructureMotionInstanceNV result{};
    result.type = i.motionType;
    auto fields = [&](auto &v) {
        v.instanceCustomIndex = i.customIndex;
        v.mask = i.mask;
        v.instanceShaderBindingTableRecordOffset = i.recordOffset;
        v.flags = i.flags;
        v.accelerationStructureReference = address;
    };
    switch (i.motionType) {
    case VK_ACCELERATION_STRUCTURE_MOTION_INSTANCE_TYPE_STATIC_NV:
        matrix(i.transform);
        result.data.staticInstance.transform = i.transform;
        fields(result.data.staticInstance);
        break;
    case VK_ACCELERATION_STRUCTURE_MOTION_INSTANCE_TYPE_MATRIX_MOTION_NV:
        matrix(i.transform);
        matrix(i.transformEnd);
        result.data.matrixMotionInstance.transformT0 = i.transform;
        result.data.matrixMotionInstance.transformT1 = i.transformEnd;
        fields(result.data.matrixMotionInstance);
        break;
    case VK_ACCELERATION_STRUCTURE_MOTION_INSTANCE_TYPE_SRT_MOTION_NV:
        srt(i.srtStart);
        srt(i.srtEnd);
        result.data.srtMotionInstance.transformT0 = i.srtStart;
        result.data.srtMotionInstance.transformT1 = i.srtEnd;
        fields(result.data.srtMotionInstance);
        break;
    default:
        throw std::invalid_argument("Unknown acceleration instance motion type");
    }
    return result;
}
VkDeviceAddress bufferAddress(const Buffer &b) {
    require((b.d->enabled & BufferAddress) && (b.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT),
            "Buffer requires enabled device-address feature and usage");
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer = b.buffer;
    return b.d->extensions->getBufferAddress(b.d->device, &i);
}
AccelerationStructure::AccelerationStructure(std::shared_ptr<Device> device, const std::vector<Geometry> &descriptors,
                                             bool update, bool compact, bool allocateStorage)
    : Resource(std::move(device)) {
    require(d->enabled & (RayQuery | RayPipeline), "Ray tracing feature was not enabled");
    require(!descriptors.empty() && descriptors.size() <= d->extensions->accelerationProperties.maxGeometryCount,
            "Invalid geometry count");
    if (update)
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    if (compact)
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    motionTriangles.resize(descriptors.size(),
                           {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_MOTION_TRIANGLES_DATA_NV});
    uint64_t primitives = 0;
    for (const auto &g : descriptors) {
        require(g.vertices && g.primitiveCount, "Geometry requires a vertex/AABB buffer and primitives");
        input(*d, *g.vertices);
        inputs.push_back(g.vertices);
        primitives += g.primitiveCount;
        VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geo.flags = g.flags;
        if (g.boxes) {
            require(!g.motionVertices, "AABB motion is computed in the intersection shader");
            require(g.vertexOffset % 8 == 0 && g.stride >= 24 && g.stride % 8 == 0,
                    "AABB stride/offset must align to 8 bytes");
            range(*g.vertices, g.vertexOffset, inputSpan(g.primitiveCount, g.stride, 24));
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
            range(*g.vertices, g.vertexOffset, inputSpan(g.vertexCount, g.stride, 12));
            geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            auto &t = geo.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = g.format;
            t.vertexData.deviceAddress = bufferAddress(*g.vertices) + g.vertexOffset;
            t.vertexStride = g.stride;
            t.maxVertex = g.vertexCount - 1;
            t.indexType = g.indexType;
            if (g.motionVertices) {
                require(d->enabledExtra & RayMotionBlur, "Ray tracing motion blur was not enabled");
                input(*d, *g.motionVertices);
                require(g.motionVertexOffset % 4 == 0, "Motion vertex offset must align to float components");
                range(*g.motionVertices, g.motionVertexOffset, inputSpan(g.vertexCount, g.stride, 12));
                inputs.push_back(g.motionVertices);
                auto &motion = motionTriangles[geometries.size()];
                motion.vertexData.deviceAddress = bufferAddress(*g.motionVertices) + g.motionVertexOffset;
                t.pNext = &motion;
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV;
            }
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
    if (allocateStorage)
        allocate();
    else
        queryBuildSizes();
}
AccelerationStructure::AccelerationStructure(std::shared_ptr<Device> device,
                                             const std::vector<AccelerationInstance> &instances, bool update,
                                             bool compact, bool allocateStorage)
    : Resource(std::move(device)) {
    require(d->enabled & (RayQuery | RayPipeline), "Ray tracing feature was not enabled");
    type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    require(!instances.empty() && instances.size() <= d->extensions->accelerationProperties.maxInstanceCount,
            "Invalid instance count");
    if (update)
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    if (compact)
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    const bool motion = std::any_of(instances.begin(), instances.end(), [](const auto &i) {
        return i.motionType != VK_ACCELERATION_STRUCTURE_MOTION_INSTANCE_TYPE_STATIC_NV ||
               (i.structure && (i.structure->flags & VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV));
    });
    require(!motion || (d->enabledExtra & RayMotionBlur), "Ray tracing motion blur was not enabled");
    if (motion)
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV;
    std::vector<VkAccelerationStructureInstanceKHR> data;
    std::vector<VkAccelerationStructureMotionInstanceNV> motionData;
    for (const auto &i : instances) {
        require(i.structure && i.structure->owner() == d.get() &&
                    i.structure->type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                "Instance requires a same-device primitive acceleration structure");
        const auto packed = packMotionInstance(i, i.structure->address());
        if (motion)
            motionData.push_back(packed);
        else
            data.push_back(packed.data.staticInstance);
        children.push_back(i.structure);
    }
    const auto byteCount = motion ? motionData.size() * sizeof(motionData[0]) : data.size() * sizeof(data[0]);
    auto b = std::make_shared<Buffer>(d, byteCount,
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      Storage::Shared, true);
    b->write(0, motion ? static_cast<const void *>(motionData.data()) : data.data(), byteCount);
    inputs.push_back(b);
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.data.deviceAddress = bufferAddress(*b);
    geometries.push_back(g);
    primitiveCounts.push_back(uint32_t(instances.size()));
    if (allocateStorage)
        allocate();
    else
        queryBuildSizes();
}
void AccelerationStructure::queryBuildSizes() {
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = type;
    build.flags = flags;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = uint32_t(geometries.size());
    build.pGeometries = geometries.data();
    d->extensions->buildSizes(d->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                              primitiveCounts.data(), &sizes);
}
void AccelerationStructure::allocate(bool querySizes) {
    if (querySizes)
        queryBuildSizes();
    storage = std::make_shared<Buffer>(d, sizes.accelerationStructureSize,
                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       Storage::Private);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = storage->buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = type;
    VkAccelerationStructureMotionInfoNV motion{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MOTION_INFO_NV};
    if (flags & VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV) {
        require(d->enabledExtra & RayMotionBlur, "Motion acceleration structures require the motion blur feature");
        ci.createFlags |= VK_ACCELERATION_STRUCTURE_CREATE_MOTION_BIT_NV;
        if (type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR) {
            require(primitiveCounts.size() == 1, "Motion instance count metadata is required");
            motion.maxInstances = primitiveCounts[0];
            ci.pNext = &motion;
        }
    }
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
void AccelerationStructure::validateRefitInputs(const AccelerationStructure &source) const {
    require(type == source.type && flags == source.flags && geometries.size() == source.geometries.size() &&
                primitiveCounts == source.primitiveCounts,
            "Refit must preserve structure type, flags, and geometry counts");
    require(!geometries.empty() && !source.inputs.empty(),
            "Supply geometry inputs when refitting a restored structure");
    for (size_t i = 0; i < geometries.size(); ++i) {
        const auto &a = geometries[i], &b = source.geometries[i];
        require(a.geometryType == b.geometryType && a.flags == b.flags, "Refit must preserve geometry type and flags");
        if (a.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
            const auto &x = a.geometry.triangles, &y = b.geometry.triangles;
            require(x.vertexFormat == y.vertexFormat && x.maxVertex == y.maxVertex && x.indexType == y.indexType &&
                        bool(x.transformData.deviceAddress) == bool(y.transformData.deviceAddress) &&
                        bool(x.pNext) == bool(y.pNext),
                    "Refit must preserve vertex format/count, index type, transform presence, and motion layout");
        }
    }
}
void Command::build(std::shared_ptr<AccelerationStructure> target, bool update,
                    std::shared_ptr<AccelerationStructure> replacementInputs) {
    recording();
    requireQueue(VK_QUEUE_COMPUTE_BIT);
    require(target && target->owner() == d.get(), "Invalid acceleration structure device");
    require(!target->copyDestination || update, "Copy and restored destinations can only be refitted");
    require(!update || (target->flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR),
            "Acceleration structure was not created for refit");
    const auto recorded = recordedAccelerationInputs.find(target.get());
    auto source = replacementInputs                              ? std::move(replacementInputs)
                  : recorded != recordedAccelerationInputs.end() ? recorded->second
                  : target->refitInputs                          ? target->refitInputs
                                                                 : target;
    require(source->d == d, "Refit input device mismatch");
    if (update)
        target->validateRefitInputs(*source);
    uint64_t size = update ? source->sizes.updateScratchSize : source->sizes.buildScratchSize;
    uint64_t alignment = d->extensions->accelerationProperties.minAccelerationStructureScratchOffsetAlignment;
    require(alignment && size <= UINT64_MAX - alignment, "Scratch allocation overflow");
    auto scratch = std::make_shared<Buffer>(
        d, size + alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        Storage::Private);
    buffers.push_back(scratch);
    buffers.push_back(target->storage);
    for (auto &b : source->inputs)
        buffers.push_back(b);
    operations.push_back([target, source, scratch, alignment, update](Command &c) {
        auto built = [&](const auto &a) {
            auto it = c.accelerationStates.find(a.get());
            return it == c.accelerationStates.end() ? a->built : it->second;
        };
        require(!update || built(target), "Build acceleration structure before refit");
        for (const auto &child : source->children)
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
        i.geometryCount = uint32_t(source->geometries.size());
        i.pGeometries = source->geometries.data();
        i.scratchData.deviceAddress = address;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        for (auto n : source->primitiveCounts)
            ranges.push_back({n, 0, 0, 0});
        const auto *ptr = ranges.data();
        c.d->extensions->buildAcceleration(c.command, 1, &i, &ptr);
        c.accelerationStates[target.get()] = true;
        if (source != target)
            c.accelerationInputStates[target.get()] = source;
    });
    recordedAccelerationInputs[target.get()] = source;
}
} // namespace vulkano

namespace vulkano {
AccelerationStructure::AccelerationStructure(std::shared_ptr<AccelerationStructure> source, bool compact)
    : Resource(source->d), type(source->type), flags(source->flags), sizes(source->sizes), children(source->children),
      copyGeneration(source->generation), copySource(source) {
    copyDestination = true;
    const auto current = source->refitInputs ? source->refitInputs : source;
    inputs = current->inputs;
    geometries = current->geometries;
    motionTriangles = current->motionTriangles;
    primitiveCounts = current->primitiveCounts;
    children = current->children;
    sizes.updateScratchSize = current->sizes.updateScratchSize;
    for (size_t i = 0; i < geometries.size(); ++i)
        if (geometries[i].geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR && geometries[i].geometry.triangles.pNext) {
            require(i < motionTriangles.size(), "Motion geometry metadata is missing");
            geometries[i].geometry.triangles.pNext = &motionTriangles[i];
        }
    require(source->built, "Build the source acceleration structure before creating a copy destination");
    if (compact) {
        require(source->flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
                "Source was not built with compaction enabled");
        // Query results require GPU completion. Avoid blocking a timeline wait while holding the JNI lock.
        d->collect();
        require(d->pending.empty(), "Complete pending commands before requesting compacted storage");
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        info.queryCount = 1;
        VkQueryPool query = VK_NULL_HANDLE;
        check(vkCreateQueryPool(d->device, &info, nullptr, &query), "create compacted-size query");
        try {
            auto command = std::make_shared<Command>(d);
            command->buffers.push_back(source->storage);
            command->operations.push_back([source, query](Command &c) {
                c.barrier();
                vkCmdResetQueryPool(c.command, query, 0, 1);
                c.d->extensions->accelerationPropertiesQuery(c.command, 1, &source->acceleration,
                                                             VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                             query, 0);
            });
            command->commit();
            command->wait(UINT64_MAX);
            uint64_t bytes = 0;
            check(vkGetQueryPoolResults(d->device, query, 0, 1, 8, &bytes, 8, VK_QUERY_RESULT_64_BIT),
                  "read compacted size");
            require(bytes > 0 && bytes <= source->storage->size, "Invalid compacted-size result");
            sizes.accelerationStructureSize = bytes;
            vkDestroyQueryPool(d->device, query, nullptr);
            query = VK_NULL_HANDLE;
        } catch (...) {
            if (query)
                vkDestroyQueryPool(d->device, query, nullptr);
            throw;
        }
        copyMode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
    }
    allocate(false);
}
void Command::copyAccelerationStructure(std::shared_ptr<AccelerationStructure> source,
                                        std::shared_ptr<AccelerationStructure> target) {
    recording();
    requireQueue(VK_QUEUE_COMPUTE_BIT);
    require(source && target && source->owner() == d.get() && target->owner() == d.get() && source != target,
            "Invalid acceleration copy resources");
    require(target->copySource.lock() == source && !target->built,
            "Use a fresh copy destination created from the source");
    buffers.push_back(source->storage);
    buffers.push_back(target->storage);
    operations.push_back([source, target](Command &c) {
        require(source->built && source->generation == target->copyGeneration &&
                    !c.accelerationStates.count(source.get()),
                "Source changed after the copy destination was sized");
        require(!target->built && !c.accelerationStates.count(target.get()), "Copy destination already initialized");
        c.barrier();
        VkCopyAccelerationStructureInfoKHR copy{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
        copy.src = source->acceleration;
        copy.dst = target->acceleration;
        copy.mode = target->copyMode;
        c.d->extensions->copyAcceleration(c.command, &copy);
        c.accelerationStates[target.get()] = true;
    });
}
} // namespace vulkano
