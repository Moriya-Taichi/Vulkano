#pragma once
#include "engine.hpp"
namespace vulkano {
struct Heap : Resource {
    VmaPool pool = nullptr;
    VkDeviceSize capacity;
    Storage storage;
    Heap(std::shared_ptr<Device>, VkDeviceSize, Storage);
    ~Heap() override;
};
struct TextureBuffer : Resource {
    std::shared_ptr<Buffer> buffer;
    VkBufferView view = VK_NULL_HANDLE;
    VkFormat format;
    bool writable;
    TextureBuffer(std::shared_ptr<Buffer>, VkFormat, VkDeviceSize offset, VkDeviceSize length, bool writable);
    ~TextureBuffer() override;
};
} // namespace vulkano
