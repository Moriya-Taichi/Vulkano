#include "heaps.hpp"
#include "sparse.hpp"
#include <algorithm>
#include <limits>
namespace vulkano {
Heap::Heap(std::shared_ptr<Device> device, VkDeviceSize size, Storage mode)
    : Resource(std::move(device)), capacity(size), storage(mode) {
    require(size > 0 && mode != Storage::Memoryless, "Heap requires positive size and shared/private storage");
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    b.size = size;
    b.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo a{};
    a.usage = mode == Storage::Shared ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (mode == Storage::Shared) {
        a.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        a.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        a.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    uint32_t type;
    check(vmaFindMemoryTypeIndexForBufferInfo(d->allocator, &b, &a, &type), "select heap memory type");
    VmaPoolCreateInfo i{};
    i.memoryTypeIndex = type;
    i.blockSize = size;
    i.minBlockCount = i.maxBlockCount = 1;
    check(vmaCreatePool(d->allocator, &i, &pool), "create resource heap");
}
Heap::~Heap() {
    if (block)
        vmaFreeMemory(d->allocator, block);
    if (pool)
        vmaDestroyPool(d->allocator, pool);
}
VkMemoryRequirements bufferRequirements(Device &d, VkBuffer buffer, bool *dedicated) {
    VkMemoryDedicatedRequirements special{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 result{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &special};
    VkBufferMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
    info.buffer = buffer;
    vkGetBufferMemoryRequirements2(d.device, &info, &result);
    if (dedicated)
        *dedicated = special.requiresDedicatedAllocation;
    return result.memoryRequirements;
}
VkMemoryRequirements textureRequirements(Device &d, VkImage image, bool *dedicated) {
    VkMemoryDedicatedRequirements special{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 result{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &special};
    VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    info.image = image;
    vkGetImageMemoryRequirements2(d.device, &info, &result);
    if (dedicated)
        *dedicated = special.requiresDedicatedAllocation;
    return result.memoryRequirements;
}
Heap::Heap(std::shared_ptr<Device> device, VkDeviceSize size, Storage mode, uint32_t types, VkDeviceSize align)
    : Resource(std::move(device)), capacity(size), storage(mode) {
    require(size && types && mode != Storage::Memoryless && align && (align & (align - 1)) == 0,
            "Invalid placement heap requirements");
    alignment =
        std::max({align, d->properties.limits.bufferImageGranularity, d->properties.limits.nonCoherentAtomSize});
    VkMemoryRequirements requirements{size, alignment, types};
    VmaAllocationCreateInfo info{};
    // Unknown-resource allocations must account for granularity against other allocations as well.
    info.flags = VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT | VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (mode == Storage::Shared) {
        info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        info.preferredFlags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }
    VmaAllocationInfo allocated{};
    check(vmaAllocateMemory(d->allocator, &requirements, &info, &block, &allocated), "allocate placement heap");
    memoryType = allocated.memoryType;
}
VkDeviceSize Heap::validate(const VkMemoryRequirements &r, bool dedicated, VkDeviceSize offset) const {
    require(placement() && !dedicated && (r.memoryTypeBits & (1u << memoryType)),
            "Resource cannot bind to this placement heap memory type");
    const auto a = std::max(alignment, r.alignment);
    require(offset % a == 0 && offset <= capacity && r.size <= capacity - offset,
            "Placement offset exceeds heap size/alignment");
    require(r.size <= std::numeric_limits<VkDeviceSize>::max() - (a - 1), "Placement size overflow");
    // Reserve the granularity page when comparing aliases, even if the last page is partial.
    return (r.size + a - 1) & ~(a - 1);
}
struct Placement {
    Heap *heap = nullptr;
    VkDeviceSize offset = 0, size = 0;
    Resource *root = nullptr;
};
static Placement placement(Resource &r) {
    if (auto *b = dynamic_cast<Buffer *>(&r))
        return {b->heap && b->heap->placement() ? b->heap.get() : nullptr, b->heapOffset, b->heapSpan, b};
    if (auto *t = dynamic_cast<Texture *>(&r)) {
        auto &root = t->root();
        return {root.heap && root.heap->placement() ? root.heap.get() : nullptr, root.heapOffset, root.heapSpan, &root};
    }
    return {};
}
bool memoryOverlaps(Resource &a, Resource &b) {
    if (sparseMemoryOverlaps(a, b))
        return true;
    const auto x = placement(a), y = placement(b);
    return x.heap && x.heap == y.heap && x.offset < y.offset + y.size && y.offset < x.offset + x.size;
}
void Command::alias(std::shared_ptr<Resource> before, std::shared_ptr<Resource> after) {
    recording();
    require(before && after && before->owner() == d.get() && after->owner() == d.get() &&
                placement(*before).root != placement(*after).root && memoryOverlaps(*before, *after),
            "Alias barrier requires distinct overlapping resources from one placement heap");
    operations.push_back([before, after](Command &c) {
        c.barrier();
        for (const auto &r : {before, after})
            if (auto t = std::dynamic_pointer_cast<Texture>(r)) {
                auto &root = t->root();
                auto [it, inserted] = c.images.emplace(&root, root.states);
                (void)inserted;
                std::fill(it->second.begin(), it->second.end(), ImageState{});
            }
    });
}
TextureBuffer::TextureBuffer(std::shared_ptr<Buffer> b, VkFormat f, VkDeviceSize offset, VkDeviceSize length,
                             bool write)
    : Resource(b->d), buffer(b), format(f), writable(write) {
    require(length > 0 && offset <= b->size && length <= b->size - offset &&
                offset % d->properties.limits.minTexelBufferOffsetAlignment == 0,
            "Invalid texture buffer range/alignment");
    require(b->usage & (write ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT),
            "Buffer requires texel usage");
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(d->physical, f, &props);
    require(props.bufferFeatures &
                (write ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT),
            "Unsupported texel buffer format");
    uint32_t size = 0;
    switch (f) {
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
        size = 4;
        break;
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
        size = 8;
        break;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
        size = 16;
        break;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
        size = 4;
        break;
    default:
        throw std::invalid_argument("Unsupported texture buffer pixel format");
    }
    require(length % size == 0 && length / size <= d->properties.limits.maxTexelBufferElements,
            "Texture buffer exceeds element limits");
    VkBufferViewCreateInfo i{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    i.buffer = b->buffer;
    i.format = f;
    i.offset = offset;
    i.range = length;
    check(vkCreateBufferView(d->device, &i, nullptr, &view), "create texture buffer view");
}
TextureBuffer::~TextureBuffer() {
    if (view)
        vkDestroyBufferView(d->device, view, nullptr);
}
} // namespace vulkano
