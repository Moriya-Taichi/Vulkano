#include "ray.hpp"
#include <cstring>
#include <set>
namespace vulkano {
namespace {
// Vulkano framing surrounds the unmodified, host-endian Vulkan serialization payload.
constexpr uint64_t archiveMagic = 0x00313053414b4c56ull;
constexpr size_t headerBytes = 32;
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
VkAccelerationStructureTypeKHR validateArchive(const std::vector<uint8_t> &data) {
    require(data.size() >= headerBytes + 56 && read<uint64_t>(data, 0) == archiveMagic &&
                read<uint64_t>(data, 16) == data.size() - headerBytes && read<uint64_t>(data, 24) == checksum(data),
            "Invalid or damaged acceleration archive");
    auto type = VkAccelerationStructureTypeKHR(read<uint32_t>(data, 8));
    require(type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR ||
                type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            "Unsupported acceleration archive type");
    require(read<uint64_t>(data, headerBytes + 32) == data.size() - headerBytes &&
                read<uint64_t>(data, headerBytes + 40) > 0,
            "Invalid Vulkan serialization sizes");
    if (type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
        require(read<uint64_t>(data, headerBytes + 48) <= (data.size() - headerBytes - 56) / 8,
                "Invalid serialized BLAS handle count");
    return type;
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
    const auto type = validateArchive(data);
    std::set<uint64_t> addresses;
    if (type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR) {
        const auto count = read<uint64_t>(data, headerBytes + 48);
        for (uint64_t n = 0; n < count; ++n) {
            auto address = read<uint64_t>(data, headerBytes + 56 + n * 8);
            if (address)
                addresses.insert(address);
        }
    }
    return {addresses.begin(), addresses.end()};
}
std::vector<uint8_t> serializeAccelerationStructure(std::shared_ptr<AccelerationStructure> source) {
    idleForArchive(*source->d);
    require(source->built, "Build acceleration structure before serialization");
    const auto size = serializationSize(source);
    require(size >= 56 && size <= INT32_MAX - headerBytes - 256, "Acceleration archive exceeds byte array capacity");
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
    std::vector<uint8_t> data(headerBytes + size);
    write(data, 0, archiveMagic);
    write(data, 8, uint32_t(source->type));
    write(data, 12, source->flags);
    write(data, 16, size);
    buffer->read(offset, data.data() + headerBytes, size);
    write(data, 24, checksum(data));
    validateArchive(data);
    return data;
}
AccelerationStructure::AccelerationStructure(std::shared_ptr<Device> device, VkAccelerationStructureTypeKHR t,
                                             VkDeviceSize size, VkBuildAccelerationStructureFlagsKHR f)
    : Resource(std::move(device)), type(t), flags(f), copyDestination(true) {
    sizes.accelerationStructureSize = size;
    allocate(false);
}
std::shared_ptr<AccelerationStructure>
restoreAccelerationStructure(std::shared_ptr<Device> d, const std::vector<uint8_t> &original,
                             const std::map<uint64_t, std::shared_ptr<AccelerationStructure>> &replacements) {
    idleForArchive(*d);
    const auto type = validateArchive(original);
    VkAccelerationStructureVersionInfoKHR version{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR};
    version.pVersionData = original.data() + headerBytes;
    VkAccelerationStructureCompatibilityKHR compatible;
    d->extensions->accelerationCompatibility(d->device, &version, &compatible);
    require(compatible == VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR,
            "Acceleration archive is incompatible with this Vulkan driver");
    auto payload = std::vector<uint8_t>(original.begin() + headerBytes, original.end());
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
    auto target = std::make_shared<AccelerationStructure>(d, type, size, read<uint32_t>(original, 12));
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
