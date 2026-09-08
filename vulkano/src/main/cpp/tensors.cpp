#include "tensors.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace vulkano {
uint32_t tensorElementSize(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_BOOL_ARM:
        return 1;
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SFLOAT:
        return 2;
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SFLOAT:
        return 4;
    case VK_FORMAT_R64_SINT:
    case VK_FORMAT_R64_UINT:
    case VK_FORMAT_R64_SFLOAT:
        return 8;
    default:
        throw std::invalid_argument("Tensor requires a supported scalar numeric format");
    }
}
VkTensorDescriptionARM TensorOptions::description() const {
    VkTensorDescriptionARM info{VK_STRUCTURE_TYPE_TENSOR_DESCRIPTION_ARM};
    info.format = format;
    info.tiling = tiling;
    info.usage = usage;
    info.dimensionCount = uint32_t(dimensions.size());
    info.pDimensions = dimensions.data();
    info.pStrides = strides.empty() ? nullptr : strides.data();
    return info;
}
VkFormatFeatureFlags2 tensorFormatFeatures(const Device &d, VkFormat format, VkTensorTilingARM tiling) {
    require(d.availableExtra & TensorResources, "Tensor resources are unavailable");
    VkTensorFormatPropertiesARM tensor{VK_STRUCTURE_TYPE_TENSOR_FORMAT_PROPERTIES_ARM};
    VkFormatProperties2 properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &tensor};
    vkGetPhysicalDeviceFormatProperties2(d.physical, format, &properties);
    return tiling == VK_TENSOR_TILING_LINEAR_ARM ? tensor.linearTilingTensorFeatures
                                                 : tensor.optimalTilingTensorFeatures;
}
uint64_t TensorOptions::validate(const Device &d) const {
    require(d.enabledExtra & TensorResources, "Tensor resources feature was not enabled");
    const auto &limits = d.extensions->tensorProperties;
    const auto element = tensorElementSize(format);
    require(!dimensions.empty() && dimensions.size() <= limits.maxTensorDimensionCount,
            "Tensor rank exceeds device limits");
    require(tiling == VK_TENSOR_TILING_LINEAR_ARM || tiling == VK_TENSOR_TILING_OPTIMAL_ARM, "Unknown tensor tiling");
    require((tiling == VK_TENSOR_TILING_LINEAR_ARM || strides.empty()) &&
                (strides.empty() || strides.size() == dimensions.size()),
            "Invalid tensor strides");
    constexpr auto allowed = VK_TENSOR_USAGE_SHADER_BIT_ARM | VK_TENSOR_USAGE_TRANSFER_SRC_BIT_ARM |
                             VK_TENSOR_USAGE_TRANSFER_DST_BIT_ARM | VK_TENSOR_USAGE_DATA_GRAPH_BIT_ARM;
    require(usage && !(usage & ~allowed), "Unknown tensor usage");
    require(!(usage & VK_TENSOR_USAGE_DATA_GRAPH_BIT_ARM) || (d.enabledExtra & DataGraph),
            "ML graph feature was not enabled");
    require(!(usage & VK_TENSOR_USAGE_SHADER_BIT_ARM) || d.extensions->tensor.shaderTensorAccess,
            "Tensor shader access is unsupported");
    uint64_t elements = 1;
    for (auto n : dimensions) {
        require(n > 0 && uint64_t(n) <= limits.maxPerDimensionTensorElements &&
                    uint64_t(n) <= limits.maxTensorElements / elements,
                "Tensor dimensions exceed device limits");
        elements *= uint64_t(n);
    }
    require(elements <= limits.maxTensorSize / element, "Tensor byte size exceeds device limit");
    uint64_t size = elements * element;
    if (!strides.empty()) {
        uint64_t packed = element;
        bool nonPacked = false;
        for (size_t i = dimensions.size(); i-- > 0;) {
            require(strides[i] > 0 && strides[i] <= limits.maxTensorStride && uint64_t(strides[i]) % element == 0,
                    "Tensor stride exceeds device limits or element alignment");
            require(i + 1 != dimensions.size() || uint64_t(strides[i]) == element,
                    "Innermost tensor stride must equal element size");
            require(uint64_t(strides[i]) >= packed, "Tensor strides overlap elements");
            nonPacked |= uint64_t(strides[i]) != packed;
            require(uint64_t(strides[i]) <= limits.maxTensorSize / uint64_t(dimensions[i]),
                    "Tensor stride size overflow");
            packed = uint64_t(strides[i]) * uint64_t(dimensions[i]);
        }
        require(!nonPacked || d.extensions->tensor.tensorNonPacked, "Non-packed tensors are unsupported");
        size = element;
        for (size_t i = 0; i < dimensions.size(); ++i)
            size += uint64_t(dimensions[i] - 1) * uint64_t(strides[i]);
    }
    const auto features = tensorFormatFeatures(d, format, tiling);
    VkFormatFeatureFlags2 required = 0;
    if (usage & VK_TENSOR_USAGE_SHADER_BIT_ARM)
        required |= VK_FORMAT_FEATURE_2_TENSOR_SHADER_BIT_ARM;
    if (usage & VK_TENSOR_USAGE_TRANSFER_SRC_BIT_ARM)
        required |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
    if (usage & VK_TENSOR_USAGE_TRANSFER_DST_BIT_ARM)
        required |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    if (usage & VK_TENSOR_USAGE_DATA_GRAPH_BIT_ARM)
        required |= VK_FORMAT_FEATURE_2_TENSOR_DATA_GRAPH_BIT_ARM;
    require((features & required) == required, "Tensor format/tiling does not support the requested usage");
    return size;
}
TensorResource::TensorResource(std::shared_ptr<Device> device, TensorOptions descriptor, Storage mode)
    : Resource(std::move(device)), options(std::move(descriptor)), storage(mode) {
    linearSize = options.validate(*d);
    require(mode == Storage::Private || (mode == Storage::Shared && options.tiling == VK_TENSOR_TILING_LINEAR_ARM),
            "Shared tensors require linear tiling; memoryless tensors are unsupported");
    auto description = options.description();
    VkTensorCreateInfoARM info{VK_STRUCTURE_TYPE_TENSOR_CREATE_INFO_ARM};
    info.flags = VK_TENSOR_CREATE_MUTABLE_FORMAT_BIT_ARM;
    info.pDescription = &description;
    d->share(info);
    check(d->extensions->createTensor(d->device, &info, nullptr, &tensor), "create tensor");
    try {
        VkTensorMemoryRequirementsInfoARM query{VK_STRUCTURE_TYPE_TENSOR_MEMORY_REQUIREMENTS_INFO_ARM};
        query.tensor = tensor;
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        d->extensions->tensorMemoryRequirements(d->device, &query, &requirements);
        allocationSize = requirements.memoryRequirements.size;
        if (options.tiling == VK_TENSOR_TILING_LINEAR_ARM)
            require(allocationSize >= linearSize, "Tensor memory requirements are smaller than its linear layout");
        uint32_t type = UINT32_MAX;
        int best = -1;
        for (uint32_t i = 0; i < d->memory.memoryTypeCount; ++i) {
            if (!(requirements.memoryRequirements.memoryTypeBits & (1u << i)))
                continue;
            auto flags = d->memory.memoryTypes[i].propertyFlags;
            if (flags & (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT))
                continue;
            if (mode == Storage::Shared && !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                continue;
            int score = bool(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mode == Storage::Shared)
                score += 4 * bool(flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) +
                         2 * bool(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (score > best) {
                best = score;
                type = i;
            }
        }
        require(type != UINT32_MAX, "No memory type supports the tensor storage mode");
        VkMemoryDedicatedAllocateInfoTensorARM dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_TENSOR_ARM};
        dedicated.tensor = tensor;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
        allocation.allocationSize = allocationSize;
        allocation.memoryTypeIndex = type;
        check(vkAllocateMemory(d->device, &allocation, nullptr, &memory), "allocate tensor memory");
        VkBindTensorMemoryInfoARM binding{VK_STRUCTURE_TYPE_BIND_TENSOR_MEMORY_INFO_ARM};
        binding.tensor = tensor;
        binding.memory = memory;
        check(d->extensions->bindTensorMemory(d->device, 1, &binding), "bind tensor memory");
        coherent = d->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (mode == Storage::Shared)
            check(vkMapMemory(d->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map tensor memory");
    } catch (...) {
        if (mapped)
            vkUnmapMemory(d->device, memory);
        d->extensions->destroyTensor(d->device, tensor, nullptr);
        if (memory)
            vkFreeMemory(d->device, memory, nullptr);
        throw;
    }
}
TensorResource::~TensorResource() {
    if (mapped)
        vkUnmapMemory(d->device, memory);
    if (tensor)
        d->extensions->destroyTensor(d->device, tensor, nullptr);
    if (memory)
        vkFreeMemory(d->device, memory, nullptr);
}
void TensorResource::accessBytes(VkDeviceSize offset, void *data, size_t count, bool write) {
    d->collect();
    require(mapped && !inFlight, "Tensor must use shared linear storage and have no pending GPU access");
    require(offset <= linearSize && count <= linearSize - offset, "Tensor byte range is out of bounds");
    if (!count)
        return;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory;
    range.size = VK_WHOLE_SIZE;
    if (!write && !coherent)
        check(vkInvalidateMappedMemoryRanges(d->device, 1, &range), "invalidate tensor memory");
    auto *address = static_cast<uint8_t *>(mapped) + offset;
    if (write)
        std::memcpy(address, data, count);
    else
        std::memcpy(data, address, count);
    if (write && !coherent)
        check(vkFlushMappedMemoryRanges(d->device, 1, &range), "flush tensor memory");
}
TensorView::TensorView(std::shared_ptr<TensorResource> t, VkFormat f)
    : Resource(t->d), tensor(std::move(t)), format(f) {
    require(tensorElementSize(f) == tensorElementSize(tensor->options.format),
            "Tensor view requires a compatible scalar format");
    require(tensor->options.usage & (VK_TENSOR_USAGE_SHADER_BIT_ARM | VK_TENSOR_USAGE_DATA_GRAPH_BIT_ARM),
            "Tensor view requires shader or ML graph usage");
    auto options = tensor->options;
    options.format = f;
    options.validate(*d);
    VkTensorViewCreateInfoARM info{VK_STRUCTURE_TYPE_TENSOR_VIEW_CREATE_INFO_ARM};
    info.tensor = tensor->tensor;
    info.format = format;
    check(d->extensions->createTensorView(d->device, &info, nullptr, &view), "create tensor view");
}
TensorView::~TensorView() {
    if (view)
        d->extensions->destroyTensorView(d->device, view, nullptr);
}
void Command::copyTensor(std::shared_ptr<TensorResource> src, std::shared_ptr<TensorResource> dst) {
    recording();
    requireQueue(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT);
    require(src && dst && src != dst && src->owner() == d.get() && dst->owner() == d.get(),
            "Tensor copy requires distinct tensors on this device");
    require(src->options.dimensions == dst->options.dimensions &&
                tensorElementSize(src->options.format) == tensorElementSize(dst->options.format),
            "Tensor copy requires equal dimensions and size-compatible scalar formats");
    require((src->options.usage & VK_TENSOR_USAGE_TRANSFER_SRC_BIT_ARM) &&
                (dst->options.usage & VK_TENSOR_USAGE_TRANSFER_DST_BIT_ARM),
            "Tensor copy requires matching transfer usage");
    tensors.push_back(src);
    tensors.push_back(dst);
    operations.push_back([src, dst](Command &c) {
        c.barrier();
        // VK_ARM_tensors currently permits only complete copies of equally sized tensors.
        VkTensorCopyARM region{VK_STRUCTURE_TYPE_TENSOR_COPY_ARM};
        VkCopyTensorInfoARM info{VK_STRUCTURE_TYPE_COPY_TENSOR_INFO_ARM};
        info.srcTensor = src->tensor;
        info.dstTensor = dst->tensor;
        info.regionCount = 1;
        info.pRegions = &region;
        c.d->extensions->copyTensor(c.command, &info);
    });
}
} // namespace vulkano
