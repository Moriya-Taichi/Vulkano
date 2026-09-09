#pragma once
#include "engine.hpp"
namespace vulkano {
struct Heap : Resource {
    VmaPool pool = nullptr;
    VmaAllocation block = nullptr;
    uint32_t memoryType = 0;
    VkDeviceSize alignment = 1;
    bool placement() const { return block != nullptr; }
    Heap(std::shared_ptr<Device>, VkDeviceSize, Storage, uint32_t memoryTypes, VkDeviceSize alignment);
    VkDeviceSize validate(const VkMemoryRequirements &, bool dedicated, VkDeviceSize offset) const;
    VkDeviceSize capacity;
    Storage storage;
    Heap(std::shared_ptr<Device>, VkDeviceSize, Storage);
    ~Heap() override;
};
VkMemoryRequirements bufferRequirements(Device &, VkBuffer, bool *dedicated = nullptr);
VkMemoryRequirements textureRequirements(Device &, VkImage, bool *dedicated = nullptr);
bool memoryOverlaps(Resource &, Resource &);
struct TextureBuffer : Resource {
    std::shared_ptr<Buffer> buffer;
    VkBufferView view = VK_NULL_HANDLE;
    VkFormat format;
    bool writable;
    TextureBuffer(std::shared_ptr<Buffer>, VkFormat, VkDeviceSize offset, VkDeviceSize length, bool writable);
    ~TextureBuffer() override;
};
} // namespace vulkano
