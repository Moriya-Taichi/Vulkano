#include "sparse.hpp"
#include "heaps.hpp"
#include <algorithm>
#include <limits>
#include <set>
namespace vulkano {
SparsePage::SparsePage(std::shared_ptr<Device> device, VkMemoryRequirements req) : Resource(std::move(device)) {
    VmaAllocationCreateInfo create{};
    create.flags = VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT;
    create.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    check(vmaAllocateMemory(d->allocator, &req, &create, &allocation, &info), "allocate sparse pages");
}
SparsePage::~SparsePage() {
    if (allocation)
        vmaFreeMemory(d->allocator, allocation);
}
SparseState::SparseState(std::shared_ptr<Device> device) : Resource(std::move(device)) {}
SparseState::SparseState(std::shared_ptr<Device> device, VkBuffer b) : SparseState(std::move(device)) {
    buffer = b;
    requirements = bufferRequirements(*d, b);
    reserve();
}
void SparseState::reserve() {
    require(requirements.size && requirements.alignment && requirements.memoryTypeBits &&
                requirements.size <= d->properties.limits.sparseAddressSpaceSize - d->sparseVirtualBytes,
            "Sparse resources exceed available virtual address space");
    d->sparseVirtualBytes += requirements.size;
    reserved = true;
}
SparseState::~SparseState() {
    if (reserved)
        d->sparseVirtualBytes -= requirements.size;
}
void SparseState::bind(VkBindSparseInfo &info) {
    d->collect();
    require(d->sparseQueue && d->pending.empty(), "Sparse mapping requires completed GPU commands");
    VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    check(vkCreateFence(d->device, &create, nullptr, &fence), "create sparse binding fence");
    try {
        check(vkQueueBindSparse(d->sparseQueue, 1, &info, fence), "bind sparse resource memory");
        check(vkWaitForFences(d->device, 1, &fence, VK_TRUE, UINT64_MAX), "wait for sparse mapping");
    } catch (...) {
        vkDestroyFence(d->device, fence, nullptr);
        throw;
    }
    vkDestroyFence(d->device, fence, nullptr);
}
void SparseState::initialize(Texture &t) {
    image = t.image;
    format = t.format;
    requirements = textureRequirements(*d, image);
    reserve();
    uint32_t count = 0;
    vkGetImageSparseMemoryRequirements(d->device, image, &count, nullptr);
    std::vector<VkSparseImageMemoryRequirements> reqs(count);
    vkGetImageSparseMemoryRequirements(d->device, image, &count, reqs.data());
    bool found = false;
    std::vector<VkSparseMemoryBind> opaque;
    for (const auto &r : reqs) {
        if (r.formatProperties.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
            require(!found, "Multiple sparse color layouts are unsupported");
            tiles = r;
            found = true;
        } else if (r.formatProperties.aspectMask == VK_IMAGE_ASPECT_METADATA_BIT) {
            if (!r.imageMipTailSize)
                continue;
            const auto layers =
                (r.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) ? 1 : t.options.layers;
            for (uint32_t layer = 0; layer < layers; ++layer) {
                auto req = requirements;
                req.size = r.imageMipTailSize;
                auto page = std::make_shared<SparsePage>(d, req);
                opaque.push_back({r.imageMipTailOffset + layer * r.imageMipTailStride, req.size,
                                  page->info.deviceMemory, page->info.offset, VK_SPARSE_MEMORY_BIND_METADATA_BIT});
                metadata.push_back(std::move(page));
            }
        } else
            throw std::invalid_argument("Unsupported sparse image aspect");
    }
    require(found && tiles.formatProperties.imageGranularity.width && tiles.formatProperties.imageGranularity.height &&
                tiles.formatProperties.imageGranularity.depth,
            "Sparse image format has no tile layout");
    if (!opaque.empty()) {
        VkSparseImageOpaqueMemoryBindInfo binds{image, uint32_t(opaque.size()), opaque.data()};
        VkBindSparseInfo submit{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        submit.imageOpaqueBindCount = 1;
        submit.pImageOpaqueBinds = &binds;
        bind(submit);
    }
    // Establish the layout while ordinary pages are unbound. A later first use of
    // a copied mapping must not discard the shared page through an UNDEFINED transition.
    d->collect();
    require(d->pending.empty(), "Sparse image creation requires completed commands");
    auto command = std::make_shared<Command>(d);
    command->operations.push_back([image = image, levels = t.options.mipLevels, layers = t.options.layers](Command &c) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    });
    command->commit();
    command->wait();
}
void SparseState::mapBuffer(uint64_t start, uint32_t count, bool resident, SparseState *source, uint64_t from) {
    require(buffer && count && start < requirements.size / requirements.alignment &&
                count <= requirements.size / requirements.alignment - start,
            "Sparse buffer page range is invalid");
    if (source)
        require(source->buffer && source->owner() == d.get() && d->coreFeatures.sparseResidencyAliased &&
                    source->requirements.alignment == requirements.alignment &&
                    from < source->requirements.size / requirements.alignment &&
                    count <= source->requirements.size / requirements.alignment - from,
                "Sparse mapping copy is incompatible");
    auto next = pages;
    std::vector<VkSparseMemoryBind> binds;
    binds.reserve(count);
    for (uint64_t n = 0; n < count; ++n) {
        Key key{0, 0, 0, start + n, 0, 0};
        std::shared_ptr<SparsePage> page;
        if (source) {
            auto it = source->pages.find({0, 0, 0, from + n, 0, 0});
            if (it != source->pages.end())
                page = it->second;
            if (page)
                require(requirements.memoryTypeBits & (1u << page->info.memoryType), "Sparse memory type mismatch");
        } else if (resident) {
            auto it = pages.find(key);
            if (it != pages.end())
                page = it->second;
            else {
                auto req = requirements;
                req.size = req.alignment;
                page = std::make_shared<SparsePage>(d, req);
            }
        }
        if (page)
            next[key] = page;
        else
            next.erase(key);
        binds.push_back({(start + n) * requirements.alignment, requirements.alignment,
                         page ? page->info.deviceMemory : VK_NULL_HANDLE, page ? page->info.offset : 0, 0});
    }
    VkSparseBufferMemoryBindInfo buffers{buffer, uint32_t(binds.size()), binds.data()};
    VkBindSparseInfo submit{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    submit.bufferBindCount = 1;
    submit.pBufferBinds = &buffers;
    bind(submit);
    pages.swap(next);
}
struct Tile {
    SparseState::Key key;
    VkSparseImageMemoryBind bind;
};
static std::vector<Tile> regionTiles(Texture &t, ImageRegion r) {
    auto &s = *t.sparse;
    require(r.mip < s.tiles.imageMipTailFirstLod && r.mip < t.options.mipLevels && r.layers &&
                r.layer < t.options.layers && r.layers <= t.options.layers - r.layer,
            "Use mip-tail mapping for tail levels");
    const auto e = t.extent(r.mip);
    const auto g = s.tiles.formatProperties.imageGranularity;
    const uint32_t full[] = {e.width, e.height, e.depth}, gran[] = {g.width, g.height, g.depth};
    const int32_t origin[] = {r.origin.x, r.origin.y, r.origin.z};
    const uint32_t size[] = {r.size.width, r.size.height, r.size.depth};
    uint64_t total = r.layers;
    uint32_t counts[3];
    for (int n = 0; n < 3; ++n) {
        require(origin[n] >= 0 && uint32_t(origin[n]) < full[n] && uint32_t(origin[n]) % gran[n] == 0 && size[n] &&
                    size[n] <= full[n] - uint32_t(origin[n]) &&
                    (size[n] % gran[n] == 0 || size[n] == full[n] - uint32_t(origin[n])),
                "Sparse region is not tile aligned");
        counts[n] = (size[n] - 1) / gran[n] + 1;
        require(total <= UINT32_MAX / counts[n], "Sparse region has too many tiles");
        total *= counts[n];
    }
    std::vector<Tile> result;
    result.reserve(size_t(total));
    for (uint32_t a = 0; a < r.layers; ++a)
        for (uint32_t z = 0; z < counts[2]; ++z)
            for (uint32_t y = 0; y < counts[1]; ++y)
                for (uint32_t x = 0; x < counts[0]; ++x) {
                    VkOffset3D o{r.origin.x + int32_t(x * g.width), r.origin.y + int32_t(y * g.height),
                                 r.origin.z + int32_t(z * g.depth)};
                    VkSparseImageMemoryBind bind{};
                    bind.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, r.mip, r.layer + a};
                    bind.offset = o;
                    bind.extent = {std::min(g.width, e.width - uint32_t(o.x)),
                                   std::min(g.height, e.height - uint32_t(o.y)),
                                   std::min(g.depth, e.depth - uint32_t(o.z))};
                    result.push_back({{1, r.mip, r.layer + a, uint32_t(o.x) / g.width, uint32_t(o.y) / g.height,
                                       uint32_t(o.z) / g.depth},
                                      bind});
                }
    return result;
}
void SparseState::mapTexture(Texture &t, ImageRegion region, bool resident, Texture *source, ImageRegion sourceRegion) {
    require(t.sparse.get() == this && !t.parent, "Map the original sparse texture");
    auto locations = regionTiles(t, region);
    std::vector<Tile> sources;
    if (source) {
        require(source->owner() == d.get() && source->sparse && !source->parent &&
                    d->coreFeatures.sparseResidencyAliased && source->format == format &&
                    source->sparse->requirements.alignment == requirements.alignment &&
                    source->sparse->tiles.formatProperties.imageGranularity.width ==
                        tiles.formatProperties.imageGranularity.width &&
                    source->sparse->tiles.formatProperties.imageGranularity.height ==
                        tiles.formatProperties.imageGranularity.height &&
                    source->sparse->tiles.formatProperties.imageGranularity.depth ==
                        tiles.formatProperties.imageGranularity.depth,
                "Sparse texture mapping copy is incompatible");
        sources = regionTiles(*source, sourceRegion);
        require(sources.size() == locations.size(), "Sparse mapping regions have different tile counts");
    }
    auto next = pages;
    std::vector<VkSparseImageMemoryBind> binds;
    binds.reserve(locations.size());
    for (size_t n = 0; n < locations.size(); ++n) {
        const auto &key = locations[n].key;
        std::shared_ptr<SparsePage> page;
        if (source) {
            auto it = source->sparse->pages.find(sources[n].key);
            if (it != source->sparse->pages.end())
                page = it->second;
            if (page)
                require(requirements.memoryTypeBits & (1u << page->info.memoryType), "Sparse memory type mismatch");
        } else if (resident) {
            auto it = pages.find(key);
            if (it != pages.end())
                page = it->second;
            else {
                auto req = requirements;
                req.size = req.alignment;
                page = std::make_shared<SparsePage>(d, req);
            }
        }
        if (page)
            next[key] = page;
        else
            next.erase(key);
        auto b = locations[n].bind;
        b.memory = page ? page->info.deviceMemory : VK_NULL_HANDLE;
        b.memoryOffset = page ? page->info.offset : 0;
        binds.push_back(b);
    }
    VkSparseImageMemoryBindInfo images{image, uint32_t(binds.size()), binds.data()};
    VkBindSparseInfo submit{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    submit.imageBindCount = 1;
    submit.pImageBinds = &images;
    bind(submit);
    pages.swap(next);
}
void SparseState::mapTail(Texture &t, uint32_t layer, bool resident) {
    require(t.sparse.get() == this && !t.parent && tiles.imageMipTailSize &&
                tiles.imageMipTailFirstLod < t.options.mipLevels,
            "Texture has no mip tail");
    const bool single = (tiles.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0;
    require(layer < (single ? 1 : t.options.layers), "Mip tail layer is invalid");
    Key key{2, 0, layer, 0, 0, 0};
    auto next = pages;
    std::shared_ptr<SparsePage> page;
    if (resident) {
        auto it = pages.find(key);
        if (it != pages.end())
            page = it->second;
        else {
            auto req = requirements;
            req.size = tiles.imageMipTailSize;
            page = std::make_shared<SparsePage>(d, req);
        }
    }
    if (page)
        next[key] = page;
    else
        next.erase(key);
    VkSparseMemoryBind b{tiles.imageMipTailOffset + layer * tiles.imageMipTailStride, tiles.imageMipTailSize,
                         page ? page->info.deviceMemory : VK_NULL_HANDLE, page ? page->info.offset : 0, 0};
    VkSparseImageOpaqueMemoryBindInfo images{image, 1, &b};
    VkBindSparseInfo submit{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    submit.imageOpaqueBindCount = 1;
    submit.pImageOpaqueBinds = &images;
    bind(submit);
    pages.swap(next);
}
bool SparseState::isResident(Texture &t, ImageRegion region) {
    require(t.sparse.get() == this && !t.parent, "Query the original sparse texture");
    if (region.mip >= tiles.imageMipTailFirstLod) {
        require(region.mip < t.options.mipLevels && region.layers && region.layer < t.options.layers &&
                    region.layers <= t.options.layers - region.layer,
                "Invalid mip tail query");
        for (uint32_t layer = region.layer; layer < region.layer + region.layers; ++layer) {
            const uint32_t tail = tiles.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? 0 : layer;
            if (!pages.count({2, 0, tail, 0, 0, 0}))
                return false;
        }
        return true;
    }
    for (const auto &tile : regionTiles(t, region))
        if (!pages.count(tile.key))
            return false;
    return true;
}
static SparseState *stateOf(Resource &resource) {
    if (auto b = dynamic_cast<Buffer *>(&resource))
        return b->sparse.get();
    if (auto t = dynamic_cast<Texture *>(&resource))
        return t->root().sparse.get();
    return nullptr;
}
bool sparseMemoryOverlaps(Resource &a, Resource &b) {
    auto x = stateOf(a), y = stateOf(b);
    if (!x || !y)
        return false;
    std::set<SparsePage *> memory;
    for (const auto &[key, page] : x->pages) {
        (void)key;
        memory.insert(page.get());
    }
    for (const auto &[key, page] : y->pages) {
        (void)key;
        if (memory.count(page.get()))
            return true;
    }
    return false;
}

} // namespace vulkano
