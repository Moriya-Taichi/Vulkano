#include "engine.hpp"
#include <algorithm>

namespace vulkano {
bool FramebufferAllocation::matches(const VkFramebufferCreateInfo &info) const {
    return pass == info.renderPass && width == info.width && height == info.height && layers == info.layers &&
           views.size() == info.attachmentCount && std::equal(views.begin(), views.end(), info.pAttachments);
}

DescriptorPoolAllocation Device::takeDescriptorPool(uint32_t maxSets, const std::vector<VkDescriptorPoolSize> &sizes) {
    for (size_t i = 0; i < idleDescriptorPoolCount; ++i) {
        const auto &candidate = idleDescriptorPools[i];
        if (candidate.maxSets != maxSets || candidate.sizes.size() != sizes.size() ||
            !std::equal(sizes.begin(), sizes.end(), candidate.sizes.begin(), [](const auto &a, const auto &b) {
                return a.type == b.type && a.descriptorCount == b.descriptorCount;
            })) continue;
        auto allocation = std::move(idleDescriptorPools[i]);
        idleDescriptorPools[i] = std::move(idleDescriptorPools[--idleDescriptorPoolCount]);
        idleDescriptorPools[idleDescriptorPoolCount] = {};
        ++descriptorPoolsReused;
        return allocation;
    }
    DescriptorPoolAllocation allocation{VK_NULL_HANDLE, maxSets, sizes};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = maxSets;
    info.poolSizeCount = uint32_t(sizes.size());
    info.pPoolSizes = sizes.data();
    check(vkCreateDescriptorPool(device, &info, nullptr, &allocation.pool), "vkCreateDescriptorPool");
    ++descriptorPoolsCreated;
    return allocation;
}

FramebufferAllocation Device::takeFramebuffer(const VkFramebufferCreateInfo &info) {
    for (size_t i = 0; i < idleFramebufferCount; ++i) {
        if (!idleFramebuffers[i].matches(info)) continue;
        auto allocation = std::move(idleFramebuffers[i]);
        idleFramebuffers[i] = std::move(idleFramebuffers[--idleFramebufferCount]);
        idleFramebuffers[idleFramebufferCount] = {};
        ++framebuffersReused;
        return allocation;
    }
    FramebufferAllocation allocation{VK_NULL_HANDLE, info.renderPass,
        std::vector<VkImageView>(info.pAttachments, info.pAttachments + info.attachmentCount), info.width, info.height, info.layers};
    check(vkCreateFramebuffer(device, &info, nullptr, &allocation.framebuffer), "vkCreateFramebuffer");
    ++framebuffersCreated;
    return allocation;
}

void Device::recycle(DescriptorPoolAllocation allocation, bool completed) noexcept {
    uint64_t descriptors = 0;
    for (auto size : allocation.sizes) descriptors += size.descriptorCount;
    if (completed && descriptors <= 4096 && idleDescriptorPoolCount < idleDescriptorPools.size() &&
        vkResetDescriptorPool(device, allocation.pool, 0) == VK_SUCCESS) {
        idleDescriptorPools[idleDescriptorPoolCount++] = std::move(allocation);
    } else vkDestroyDescriptorPool(device, allocation.pool, nullptr);
}

void Device::recycle(FramebufferAllocation allocation, bool completed) noexcept {
    if (completed && idleFramebufferCount < idleFramebuffers.size())
        idleFramebuffers[idleFramebufferCount++] = std::move(allocation);
    else vkDestroyFramebuffer(device, allocation.framebuffer, nullptr);
}

void Device::invalidateFramebuffers(VkImageView view) noexcept {
    for (size_t i = 0; i < idleFramebufferCount;) {
        const auto &views = idleFramebuffers[i].views;
        if (std::find(views.begin(), views.end(), view) == views.end()) { ++i; continue; }
        vkDestroyFramebuffer(device, idleFramebuffers[i].framebuffer, nullptr);
        idleFramebuffers[i] = std::move(idleFramebuffers[--idleFramebufferCount]);
        idleFramebuffers[idleFramebufferCount] = {};
    }
}

void Device::destroyView(VkImageView view) noexcept {
    if (!view) return;
    invalidateFramebuffers(view);
    vkDestroyImageView(device, view, nullptr);
}

void Device::clearIdleResources() noexcept {
    for (size_t i = 0; i < idleFramebufferCount; ++i) {
        vkDestroyFramebuffer(device, idleFramebuffers[i].framebuffer, nullptr);
        idleFramebuffers[i] = {};
    }
    for (size_t i = 0; i < idleDescriptorPoolCount; ++i) {
        vkDestroyDescriptorPool(device, idleDescriptorPools[i].pool, nullptr);
        idleDescriptorPools[i] = {};
    }
    idleFramebufferCount = idleDescriptorPoolCount = 0;
}

VkFramebuffer Command::framebuffer(const VkFramebufferCreateInfo &info) {
    for (const auto &allocation : framebufferAllocations) {
        if (!allocation.matches(info)) continue;
        ++d->framebuffersReused;
        return allocation.framebuffer;
    }
    framebuffers.reserve(framebuffers.size() + 1);
    framebufferAllocations.reserve(framebufferAllocations.size() + 1);
    auto allocation = d->takeFramebuffer(info);
    const auto handle = allocation.framebuffer;
    framebuffers.push_back(handle);
    framebufferAllocations.push_back(std::move(allocation));
    return handle;
}
} // namespace vulkano
