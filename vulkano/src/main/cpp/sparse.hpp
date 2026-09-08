#pragma once
#include "engine.hpp"
#include <array>
namespace vulkano {
bool sparseMemoryOverlaps(Resource &, Resource &);
struct SparsePage : Resource {
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo info{};
    SparsePage(std::shared_ptr<Device>, VkMemoryRequirements);
    ~SparsePage() override;
};
struct SparseState : Resource {
    using Key = std::array<uint64_t, 6>; // kind, mip, layer, x, y, z; buffer page in x
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkMemoryRequirements requirements{};
    VkSparseImageMemoryRequirements tiles{};
    std::map<Key, std::shared_ptr<SparsePage>> pages;
    std::vector<std::shared_ptr<SparsePage>> metadata;
    bool reserved = false;
    explicit SparseState(std::shared_ptr<Device>);
    SparseState(std::shared_ptr<Device>, VkBuffer);
    void initialize(Texture &);
    void reserve();
    void bind(VkBindSparseInfo &);
    void mapBuffer(uint64_t page, uint32_t count, bool resident, SparseState *source = nullptr,
                   uint64_t sourcePage = 0);
    void mapTexture(Texture &, ImageRegion, bool resident, Texture *source = nullptr, ImageRegion sourceRegion = {});
    void mapTail(Texture &, uint32_t layer, bool resident);
    bool isResident(Texture &, ImageRegion);
    ~SparseState() override;
};
} // namespace vulkano
