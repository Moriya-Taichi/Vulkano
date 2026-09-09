#include "ray.hpp"
#include <cstring>
#include <set>
namespace vulkano {
namespace {
// Vulkano framing surrounds the unmodified, host-endian Vulkan serialization payload.
constexpr uint64_t legacyArchiveMagic = 0x00313053414b4c56ull;
constexpr uint64_t archiveMagic = 0x00323053414b4c56ull;
constexpr size_t headerBytes = 48;
struct ArchiveLayout {
    VkAccelerationStructureTypeKHR type;
    size_t payloadOffset, payloadBytes;
    std::vector<uint32_t> schema;
};
template <class T> T read(const std::vector<uint8_t> &data, size_t offset) {
    require(offset <= data.size() && sizeof(T) <= data.size() - offset, "Truncated acceleration archive");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}
template <class T> void write(std::vector<uint8_t> &data, size_t offset, T value) {
    require(offset <= data.size() && sizeof(T) <= data.size() - offset, "Truncated acceleration archive");
    std::memcpy(data.data() + offset, &value, sizeof(T));
}
uint64_t checksum(const std::vector<uint8_t> &data) {
    // Detect accidental cache corruption. This is not a signature for untrusted GPU data.
    uint64_t value = 14695981039346656037ull;
    for (size_t n = 0; n < data.size(); ++n)
        if (n < 24 || n >= 32) {
            value ^= data[n];
            value *= 1099511628211ull;
        }
    return value;
}
void validateSchema(const std::vector<uint32_t> &schema, VkAccelerationStructureTypeKHR type,
                    VkBuildAccelerationStructureFlagsKHR flags) {
    if (schema.empty())
        return; // Archives written before input metadata was available.
    require(schema.size() >= 1 && schema[0] <= (schema.size() - 1) / 8 && schema.size() == 1 + size_t(schema[0]) * 8,
            "Invalid acceleration input metadata length");
    require(type != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR || schema[0] <= 1,
            "Instance structure metadata must contain one geometry");
    require(!(flags & VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV) || schema[0] > 0,
            "Motion structure metadata is required");
    for (size_t n = 0; n < schema[0]; ++n) {
        const auto *g = schema.data() + 1 + n * 8;
        require(g[2] > 0 &&
                    !(g[1] & ~(VK_GEOMETRY_OPAQUE_BIT_KHR | VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR)),
                "Invalid geometry flags or primitive count in archive");
        if (type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
            require(g[0] == VK_GEOMETRY_TYPE_INSTANCES_KHR, "Top-level archive must describe instances");
        else
            require(g[0] == VK_GEOMETRY_TYPE_TRIANGLES_KHR || g[0] == VK_GEOMETRY_TYPE_AABBS_KHR,
                    "Bottom-level archive must describe triangles or AABBs");
        if (g[0] == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
            require(
                g[3] == VK_FORMAT_R32G32B32_SFLOAT &&
                    (g[4] == VK_INDEX_TYPE_NONE_KHR || g[4] == VK_INDEX_TYPE_UINT16 || g[4] == VK_INDEX_TYPE_UINT32) &&
                    g[6] <= 1 && g[7] == 0,
                "Unsupported triangle metadata in archive");
            require(!g[6] || (flags & VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV),
                    "Motion metadata contradicts build flags");
        } else
            for (size_t i = 3; i < 8; ++i)
                require(g[i] == 0, "Invalid non-triangle metadata");
    }
}
ArchiveLayout validateArchive(const std::vector<uint8_t> &data) {
    require(data.size() >= 32 + 56 && read<uint64_t>(data, 24) == checksum(data),
            "Invalid or damaged acceleration archive");
    const auto magic = read<uint64_t>(data, 0);
    require(magic == archiveMagic || magic == legacyArchiveMagic, "Unknown acceleration archive version");
    ArchiveLayout result;
    result.payloadOffset = magic == legacyArchiveMagic ? 32 : headerBytes;
    require(data.size() >= result.payloadOffset + 56, "Truncated acceleration archive");
    const auto payload = read<uint64_t>(data, 16);
    require(payload >= 56 && payload <= data.size() - result.payloadOffset, "Invalid acceleration payload size");
    result.payloadBytes = size_t(payload);
    result.type = VkAccelerationStructureTypeKHR(read<uint32_t>(data, 8));
    require(result.type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR ||
                result.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            "Unsupported acceleration archive type");
    const auto flags = read<VkBuildAccelerationStructureFlagsKHR>(data, 12);
    constexpr auto allowed = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                             VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                             VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR |
                             VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV;
    require(!(flags & ~allowed), "Unsupported acceleration build flags in archive");
    require(read<uint64_t>(data, result.payloadOffset + 32) == payload &&
                read<uint64_t>(data, result.payloadOffset + 40) > 0,
            "Invalid Vulkan serialization sizes");
    if (result.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
        require(read<uint64_t>(data, result.payloadOffset + 48) <= (payload - 56) / 8,
                "Invalid serialized BLAS handle count");
    const auto metadataSize = data.size() - result.payloadOffset - payload;
    if (magic == legacyArchiveMagic) {
        require(metadataSize == 0, "Trailing data in legacy acceleration archive");
        require(!(flags & VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV),
                "Legacy archives cannot contain motion structures");
    } else {
        require(read<uint64_t>(data, 32) == metadataSize && read<uint64_t>(data, 40) == 0 && metadataSize >= 4 &&
                    metadataSize % 4 == 0,
                "Invalid acceleration archive metadata");
        result.schema.resize(size_t(metadataSize / 4));
        std::memcpy(result.schema.data(), data.data() + result.payloadOffset + payload, size_t(metadataSize));
        validateSchema(result.schema, result.type, flags);
    }
    return result;
}
std::vector<uint32_t> encodeSchema(const AccelerationStructure &structure) {
    require(structure.geometries.size() == structure.primitiveCounts.size() &&
                structure.geometries.size() <= UINT32_MAX,
            "Incomplete acceleration input metadata");
    std::vector<uint32_t> result{uint32_t(structure.geometries.size())};
    for (size_t n = 0; n < structure.geometries.size(); ++n) {
        const auto &g = structure.geometries[n];
        result.insert(result.end(), {uint32_t(g.geometryType), g.flags, structure.primitiveCounts[n]});
        if (g.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
            const auto &t = g.geometry.triangles;
            result.insert(result.end(), {uint32_t(t.vertexFormat), uint32_t(t.indexType), t.maxVertex,
                                         uint32_t(t.pNext != nullptr), uint32_t(t.transformData.deviceAddress != 0)});
        } else
            result.insert(result.end(), {0, 0, 0, 0, 0});
    }
    validateSchema(result, structure.type, structure.flags);
    return result;
}
void idleForArchive(Device &d) {
    require(d.enabled & (RayQuery | RayPipeline), "Ray tracing was not enabled");
    d.collect();
    require(d.pending.empty(), "Complete pending commands before serializing or restoring acceleration structures");
}
uint64_t serializationSize(std::shared_ptr<AccelerationStructure> source) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR;
    info.queryCount = 1;
    VkQueryPool query;
    check(vkCreateQueryPool(source->d->device, &info, nullptr, &query), "create serialization size query");
    try {
        auto command = std::make_shared<Command>(source->d);
        command->buffers.push_back(source->storage);
        command->operations.push_back([source, query](Command &c) {
            c.barrier();
            vkCmdResetQueryPool(c.command, query, 0, 1);
            c.d->extensions->accelerationPropertiesQuery(c.command, 1, &source->acceleration,
                                                         VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR,
                                                         query, 0);
        });
        command->commit();
        command->wait();
        uint64_t size = 0;
        check(vkGetQueryPoolResults(source->d->device, query, 0, 1, sizeof(size), &size, sizeof(size),
                                    VK_QUERY_RESULT_64_BIT),
              "read serialization size");
        vkDestroyQueryPool(source->d->device, query, nullptr);
        return size;
    } catch (...) {
        vkDestroyQueryPool(source->d->device, query, nullptr);
        throw;
    }
}
} // namespace
std::vector<uint64_t> accelerationArchiveAddresses(const std::vector<uint8_t> &data) {
    const auto archive = validateArchive(data);
    std::set<uint64_t> addresses;
    if (archive.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR) {
        const auto count = read<uint64_t>(data, archive.payloadOffset + 48);
        for (uint64_t n = 0; n < count; ++n) {
            auto address = read<uint64_t>(data, archive.payloadOffset + 56 + n * 8);
            if (address)
                addresses.insert(address);
        }
    }
    return {addresses.begin(), addresses.end()};
}
std::vector<uint8_t> serializeAccelerationStructure(std::shared_ptr<AccelerationStructure> source) {
    idleForArchive(*source->d);
    require(source->built, "Build acceleration structure before serialization");
    const auto schema = encodeSchema(*source);
    const auto metadataSize = schema.size() * sizeof(uint32_t);
    const auto size = serializationSize(source);
    require(metadataSize < INT32_MAX - headerBytes - 256 && size >= 56 &&
                size <= INT32_MAX - headerBytes - 256 - metadataSize,
            "Acceleration archive exceeds byte array capacity");
    auto buffer = std::make_shared<Buffer>(source->d, size + 255,
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           Storage::Shared);
    const auto base = bufferAddress(*buffer), address = (base + 255) & ~uint64_t(255), offset = address - base;
    auto command = std::make_shared<Command>(source->d);
    command->buffers = {source->storage, buffer};
    command->operations.push_back([source, buffer, address](Command &c) {
        c.barrier();
        VkCopyAccelerationStructureToMemoryInfoKHR info{
            VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR};
        info.src = source->acceleration;
        info.dst.deviceAddress = address;
        info.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;
        c.d->extensions->serializeAcceleration(c.command, &info);
        c.barrier();
    });
    command->commit();
    command->wait();
    std::vector<uint8_t> data(headerBytes + size + metadataSize);
    write(data, 0, archiveMagic);
    write(data, 8, uint32_t(source->type));
    write(data, 12, source->flags);
    write(data, 16, size);
    write(data, 32, uint64_t(metadataSize));
    buffer->read(offset, data.data() + headerBytes, size);
    std::memcpy(data.data() + headerBytes + size, schema.data(), metadataSize);
    write(data, 24, checksum(data));
    validateArchive(data);
    return data;
}
AccelerationStructure::AccelerationStructure(std::shared_ptr<Device> device, VkAccelerationStructureTypeKHR t,
                                             VkDeviceSize size, VkBuildAccelerationStructureFlagsKHR f,
                                             const std::vector<uint32_t> &schema)
    : Resource(std::move(device)), type(t), flags(f), copyDestination(true) {
    validateSchema(schema, type, flags);
    const uint32_t count = schema.empty() ? 0 : schema[0];
    require(count <= d->extensions->accelerationProperties.maxGeometryCount,
            "Archived geometry count exceeds device limits");
    motionTriangles.resize(count, {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_MOTION_TRIANGLES_DATA_NV});
    uint64_t total = 0;
    for (uint32_t n = 0; n < count; ++n) {
        const auto *s = schema.data() + 1 + n * 8;
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VkGeometryTypeKHR(s[0]);
        g.flags = s[1];
        total += s[2];
        if (g.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
            auto &t = g.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = VkFormat(s[3]);
            t.indexType = VkIndexType(s[4]);
            t.maxVertex = s[5];
            if (s[6])
                t.pNext = &motionTriangles[n];
        } else if (g.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR)
            g.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
        else
            g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometries.push_back(g);
        primitiveCounts.push_back(s[2]);
    }
    const auto limit = type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                           ? d->extensions->accelerationProperties.maxInstanceCount
                           : d->extensions->accelerationProperties.maxPrimitiveCount;
    require(total <= limit, "Archived primitive or instance count exceeds device limits");
    sizes.accelerationStructureSize = size;
    allocate(false);
}
std::shared_ptr<AccelerationStructure>
restoreAccelerationStructure(std::shared_ptr<Device> d, const std::vector<uint8_t> &original,
                             const std::map<uint64_t, std::shared_ptr<AccelerationStructure>> &replacements) {
    idleForArchive(*d);
    const auto archive = validateArchive(original);
    const auto type = archive.type;
    VkAccelerationStructureVersionInfoKHR version{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR};
    version.pVersionData = original.data() + archive.payloadOffset;
    VkAccelerationStructureCompatibilityKHR compatible;
    d->extensions->accelerationCompatibility(d->device, &version, &compatible);
    require(compatible == VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR,
            "Acceleration archive is incompatible with this Vulkan driver");
    auto payload = std::vector<uint8_t>(original.begin() + archive.payloadOffset,
                                        original.begin() + archive.payloadOffset + archive.payloadBytes);
    std::vector<std::shared_ptr<AccelerationStructure>> children;
    const auto required = accelerationArchiveAddresses(original);
    require(replacements.size() == required.size(), "Map every archived BLAS address to its restored structure");
    for (auto address : required) {
        auto it = replacements.find(address);
        require(it != replacements.end() && it->second && it->second->owner() == d.get() && it->second->built &&
                    it->second->type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                "Invalid or missing restored BLAS mapping");
        children.push_back(it->second);
    }
    if (type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR) {
        const auto count = read<uint64_t>(payload, 48);
        for (uint64_t n = 0; n < count; ++n) {
            auto address = read<uint64_t>(payload, 56 + n * 8);
            if (address)
                write(payload, 56 + n * 8, replacements.at(address)->address());
        }
    }
    const auto size = read<uint64_t>(payload, 40);
    require(size <= INT64_MAX, "Invalid deserialized acceleration structure size");
    auto target = std::make_shared<AccelerationStructure>(d, type, size, read<uint32_t>(original, 12), archive.schema);
    target->children = std::move(children);
    auto upload = std::make_shared<Buffer>(d, payload.size() + 255,
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           Storage::Shared);
    const auto base = bufferAddress(*upload), address = (base + 255) & ~uint64_t(255);
    upload->write(address - base, payload.data(), payload.size());
    auto command = std::make_shared<Command>(d);
    command->buffers = {target->storage, upload};
    command->operations.push_back([target, upload, address](Command &c) {
        c.barrier();
        VkCopyMemoryToAccelerationStructureInfoKHR info{
            VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR};
        info.src.deviceAddress = address;
        info.dst = target->acceleration;
        info.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR;
        c.d->extensions->deserializeAcceleration(c.command, &info);
        c.accelerationStates[target.get()] = true;
    });
    command->commit();
    command->wait();
    return target;
}
} // namespace vulkano
