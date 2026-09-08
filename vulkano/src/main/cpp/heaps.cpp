#include "heaps.hpp"
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
    if (pool)
        vmaDestroyPool(d->allocator, pool);
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
