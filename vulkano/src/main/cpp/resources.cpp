#include "engine.hpp"
#include "heaps.hpp"
#include "interop.hpp"
#include "sparse.hpp"
#include <algorithm>
#include <cmath>
#include <set>
namespace vulkano {
namespace {
#include "format_classes.inc"
constexpr VkImageUsageFlags viewUsages =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
VkImageView imageView(Texture &t, VkFormat format, VkImageViewType type, uint32_t mip, uint32_t levels, uint32_t layer,
                      uint32_t layers) {
    VkImageViewCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    i.image = t.image;
    i.viewType = type;
    i.format = format;
    i.components = t.components;
    VkImageViewUsageCreateInfo u{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    u.usage = t.usage;
    i.pNext = &u;
    VkSamplerYcbcrConversionInfo conversion{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    if (t.external && t.external->conversionSampler) {
        conversion.conversion = t.external->conversionSampler->conversion;
        u.pNext = &conversion;
    }
    i.subresourceRange = {t.depth() ? uint32_t(VK_IMAGE_ASPECT_DEPTH_BIT) : t.aspects(), mip, levels, layer, layers};
    VkImageView v;
    check(vkCreateImageView(t.d->device, &i, nullptr, &v), "vkCreateImageView");
    return v;
}
void bounds(VkDeviceSize capacity, VkDeviceSize offset, VkDeviceSize length) {
    require(length && offset <= capacity && length <= capacity - offset, "Buffer range out of bounds");
}
void sameOwner(const Resource &a, const Resource &b) {
    require(a.owner() == b.owner(), "Resources belong to different devices");
}
bool powerOfTwo(uint32_t n) { return n && !(n & (n - 1)) && n <= 64; }
void requireCompressionFeature(const Device &d, VkFormat format) {
    if (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK)
        require(d.enabled & Bc, "BC compression feature was not enabled");
    if (format >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && format <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK)
        require(d.enabled & Etc2, "ETC2 compression feature was not enabled");
    if (format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
        require(d.enabled & Astc, "ASTC compression feature was not enabled");
    if (format >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && format <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK)
        require(d.enabledExtra & AstcHdr, "ASTC HDR compression feature was not enabled");
    if (format >= VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG && format <= VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG)
        require(d.enabledExtra & Pvrtc, "PVRTC compression feature was not enabled");
}

} // namespace
int numericClass(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8_SINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_B8G8R8_SINT:
    case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32:
    case VK_FORMAT_A2R10G10B10_SINT_PACK32:
    case VK_FORMAT_A2B10G10R10_SINT_PACK32:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16_SINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32_SINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R64_SINT:
    case VK_FORMAT_R64G64_SINT:
    case VK_FORMAT_R64G64B64_SINT:
    case VK_FORMAT_R64G64B64A64_SINT:
        return 1;
    case VK_FORMAT_S8_UINT:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8B8_UINT:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_B8G8R8_UINT:
    case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32:
    case VK_FORMAT_A2R10G10B10_UINT_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16B16_UINT:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R64_UINT:
    case VK_FORMAT_R64G64_UINT:
    case VK_FORMAT_R64G64B64_UINT:
    case VK_FORMAT_R64G64B64A64_UINT:
        return 2;
    default:
        return 0;
    }
}
bool Texture::depth() const {
    return format >= VK_FORMAT_D16_UNORM && format <= VK_FORMAT_D32_SFLOAT_S8_UINT && format != VK_FORMAT_S8_UINT;
}
bool Texture::stencil() const { return format >= VK_FORMAT_S8_UINT && format <= VK_FORMAT_D32_SFLOAT_S8_UINT; }
VkImageAspectFlags Texture::aspects() const {
    return (depth() ? uint32_t(VK_IMAGE_ASPECT_DEPTH_BIT) : 0u) |
           (stencil() ? uint32_t(VK_IMAGE_ASPECT_STENCIL_BIT) : 0u) |
           (!depth() && !stencil() ? uint32_t(VK_IMAGE_ASPECT_COLOR_BIT) : 0u);
}
VkExtent3D Texture::extent(uint32_t mip) const {
    require(mip < options.mipLevels, "Mip level out of range");
    return {std::max(1u, width >> mip), std::max(1u, height >> mip), std::max(1u, options.depth >> mip)};
}
VkImageType Texture::imageType() const {
    if (options.type == VK_IMAGE_VIEW_TYPE_1D || options.type == VK_IMAGE_VIEW_TYPE_1D_ARRAY)
        return VK_IMAGE_TYPE_1D;
    return options.type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
}
VkFormatFeatureFlags Texture::formatFeatures() const {
    if (external && format == VK_FORMAT_UNDEFINED)
        return external->formatFeatures;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(d->physical, format, &properties);
    return properties.optimalTilingFeatures;
}
VkImageCreateFlags Texture::flags() const {
    if (external)
        return external->flags;
    return (sparse ? (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                      (d->coreFeatures.sparseResidencyAliased ? uint32_t(VK_IMAGE_CREATE_SPARSE_ALIASED_BIT) : 0u))
                   : 0u) |
           VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
           ((options.type == VK_IMAGE_VIEW_TYPE_CUBE || options.type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
                ? uint32_t(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
                : 0u);
}
uint32_t Texture::pixelSize() const {
    if (format == VK_FORMAT_R8G8B8_UNORM)
        return 3;
    if (format == VK_FORMAT_R5G6B5_UNORM_PACK16)
        return 2;
    if (format >= VK_FORMAT_R8_UNORM && format <= VK_FORMAT_R8_SRGB)
        return 1;
    if (format >= VK_FORMAT_R8G8_UNORM && format <= VK_FORMAT_R8G8_SRGB)
        return 2;
    if (format >= VK_FORMAT_R8G8B8A8_UNORM && format <= VK_FORMAT_A2B10G10R10_SINT_PACK32)
        return 4;
    if (format >= VK_FORMAT_R16_UNORM && format <= VK_FORMAT_R16_SFLOAT)
        return 2;
    if (format >= VK_FORMAT_R16G16_UNORM && format <= VK_FORMAT_R16G16_SFLOAT)
        return 4;
    if (format >= VK_FORMAT_R16G16B16A16_UNORM && format <= VK_FORMAT_R16G16B16A16_SFLOAT)
        return 8;
    if (format >= VK_FORMAT_R32_UINT && format <= VK_FORMAT_R32_SFLOAT)
        return 4;
    if (format >= VK_FORMAT_R32G32_UINT && format <= VK_FORMAT_R32G32_SFLOAT)
        return 8;
    if (format >= VK_FORMAT_R32G32B32_UINT && format <= VK_FORMAT_R32G32B32_SFLOAT)
        return 12;
    if (format >= VK_FORMAT_R32G32B32A32_UINT && format <= VK_FORMAT_R32G32B32A32_SFLOAT)
        return 16;
    if (format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
        return 4;
    if (format == VK_FORMAT_D16_UNORM)
        return 2;
    if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
        format == VK_FORMAT_D24_UNORM_S8_UINT)
        return 4;
    if (format == VK_FORMAT_S8_UINT)
        return 1;
    if (format == VK_FORMAT_D16_UNORM_S8_UINT)
        return 3;
    if (format == VK_FORMAT_D32_SFLOAT_S8_UINT)
        return 8;
    if (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK) {
        return (format <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
                (format >= VK_FORMAT_BC4_UNORM_BLOCK && format <= VK_FORMAT_BC4_SNORM_BLOCK))
                   ? 8
                   : 16;
    }
    if (format >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && format <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK)
        return (format <= VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK || format == VK_FORMAT_EAC_R11_UNORM_BLOCK ||
                format == VK_FORMAT_EAC_R11_SNORM_BLOCK)
                   ? 8
                   : 16;
    if (format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
        return 16;
    if (format >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && format <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK)
        return 16;
    if (format >= VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG && format <= VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG)
        return 8;
    throw std::invalid_argument("Unsupported pixel format");
}
uint32_t Texture::blockWidth() const {
    if (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK)
        return 4;
    const uint32_t widths[] = {4, 5, 5, 6, 6, 8, 8, 8, 10, 10, 10, 10, 12, 12};
    if (format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
        return widths[(format - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2];
    if (format >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && format <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK)
        return widths[format - VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK];
    if (format >= VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG && format <= VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG)
        return (format - VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG) % 2 == 0 ? 8 : 4;
    return 1;
}
uint32_t Texture::blockHeight() const {
    if (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK)
        return 4;
    const uint32_t heights[] = {4, 4, 5, 5, 6, 5, 6, 8, 5, 6, 8, 10, 10, 12};
    if (format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
        return heights[(format - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2];
    if (format >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && format <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK)
        return heights[format - VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK];
    if (format >= VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG && format <= VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG)
        return 4;
    return 1;
}
uint64_t Texture::byteSize(uint32_t mip) const {
    const auto e = extent(mip);
    return uint64_t((e.width + blockWidth() - 1) / blockWidth()) * ((e.height + blockHeight() - 1) / blockHeight()) *
           e.depth * pixelSize();
}
Texture::Texture(std::shared_ptr<Device> device, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags u, Storage s,
                 TextureOptions o, std::shared_ptr<Heap> hpool, VkDeviceSize offset, bool unbound, bool sparseResource)
    : Resource(std::move(device)), format(f), width(w), height(h), options(o), usage(u), storage(s) {
    heap = std::move(hpool);
    if (sparseResource) {
        require((d->enabled & SparseResources) && !heap && storage == Storage::Private && o.samples == 1 && !depth() &&
                    !stencil() &&
                    (imageType() == VK_IMAGE_TYPE_2D
                         ? d->coreFeatures.sparseResidencyImage2D
                         : imageType() == VK_IMAGE_TYPE_3D && d->coreFeatures.sparseResidencyImage3D),
                "Sparse single-sampled color texture residency is unavailable");
        sparse = std::make_shared<SparseState>(d);
    }
    heapOffset = offset;
    require(!offset || (heap && heap->placement()), "An offset requires a placement heap");
    if (heap)
        require(heap->owner() == d.get() && heap->storage == Storage::Private,
                "Texture requires a private heap on the same device");
    require(w && h && o.depth && o.layers && o.mipLevels && o.mipLevels <= 32 && storage != Storage::Shared,
            "Invalid texture dimensions or storage");
    require(o.type >= VK_IMAGE_VIEW_TYPE_1D && o.type <= VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && powerOfTwo(o.samples),
            "Invalid texture type or sample count");
    uint32_t maxMip = 1;
    for (uint32_t n = std::max({w, h, o.depth}); n > 1; n >>= 1)
        ++maxMip;
    require(o.mipLevels <= maxMip, "Mip count exceeds texture size");
    require(imageType() != VK_IMAGE_TYPE_1D || (h == 1 && o.depth == 1), "1D textures require height/depth 1");
    require(imageType() == VK_IMAGE_TYPE_3D ? o.layers == 1 : o.depth == 1, "Invalid texture depth/layers");
    if (o.type == VK_IMAGE_VIEW_TYPE_CUBE || o.type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        require(w == h && o.layers % 6 == 0 && o.samples == 1, "Cube textures require square faces in groups of six");
        require(o.type != VK_IMAGE_VIEW_TYPE_CUBE || o.layers == 6, "Cube requires six layers");
        require(o.type != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY || (d->enabled & CubeArray),
                "Cube array feature was not enabled");
    } else if (o.type == VK_IMAGE_VIEW_TYPE_1D || o.type == VK_IMAGE_VIEW_TYPE_2D || o.type == VK_IMAGE_VIEW_TYPE_3D)
        require(o.layers == 1, "Non-array texture requires one layer");
    require(o.samples == 1 || (imageType() == VK_IMAGE_TYPE_2D && o.mipLevels == 1), "MSAA requires 2D and one mip");
    require(!(usage & VK_IMAGE_USAGE_STORAGE_BIT) || o.samples == 1 || (d->enabled & StorageMs),
            "Multisample storage feature was not enabled");
    pixelSize();
    requireCompressionFeature(*d, format);
    if (format >= VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG && format <= VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG) {
        require(d->enabledExtra & Pvrtc, "PVRTC compression feature was not enabled");
        const bool pvrtc1 = ((format - VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG) % 4) < 2;
        require(!pvrtc1 || (!(w & (w - 1)) && !(h & (h - 1))), "PVRTC1 dimensions must be powers of two");
    }
    require(usage && !(usage & ~(63u | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)),
            "Unsupported image usage");
    if (usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)
        require((d->enabled & AttachmentRate) && f == VK_FORMAT_R8_UINT && o.samples == 1 &&
                    imageType() == VK_IMAGE_TYPE_2D,
                "Rate map requires attachment shading rate, R8_UINT and single-sampled 2D texture");
    require(depth() || stencil() ? !(usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT))
                                 : !(usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
            "Attachment format/usage mismatch");
    if (storage == Storage::Memoryless) {
        require((usage & ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) == (depth() || stencil()
                                                                       ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                                       : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                "Memoryless textures are attachment-only");
        usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    }
    VkImageFormatProperties supported{};
    check(vkGetPhysicalDeviceImageFormatProperties(d->physical, format, imageType(), VK_IMAGE_TILING_OPTIMAL, usage,
                                                   flags(), &supported),
          "Unsupported texture format/usage");
    require(w <= supported.maxExtent.width && h <= supported.maxExtent.height && o.depth <= supported.maxExtent.depth &&
                o.layers <= supported.maxArrayLayers && o.mipLevels <= supported.maxMipLevels &&
                (supported.sampleCounts & o.samples),
            "Texture exceeds device format limits");
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    d->share(info);
    info.flags = flags();
    info.imageType = imageType();
    info.extent = {w, h, o.depth};
    info.mipLevels = o.mipLevels;
    info.arrayLayers = o.layers;
    info.format = f;
    info.samples = o.samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (storage == Storage::Memoryless)
        alloc.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    if (heap)
        alloc.pool = heap->pool;
    if (unbound || sparse || (heap && heap->placement())) {
        check(vkCreateImage(d->device, &info, nullptr, &image), "create unbound image");
        try {
            if (!unbound && !sparse) {
                bool dedicated;
                const auto requirements = textureRequirements(*d, image, &dedicated);
                heapSpan = heap->validate(requirements, dedicated, offset);
                check(vmaBindImageMemory2(d->allocator, heap->block, offset, image, nullptr), "bind placed image");
            }
        } catch (...) {
            vkDestroyImage(d->device, image, nullptr);
            image = VK_NULL_HANDLE;
            throw;
        }
        if (unbound)
            return;
    } else
        check(vmaCreateImage(d->allocator, &info, &alloc, &image, &allocation, nullptr), "vmaCreateImage");
    try {
        states.resize(size_t(o.layers) * o.mipLevels);
        VkMemoryPropertyFlags mf = 0;
        if (!sparse)
            vmaGetAllocationMemoryProperties(d->allocator, heap && heap->placement() ? heap->block : allocation, &mf);
        lazy = (mf & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0;
        if (sparse) {
            sparse->initialize(*this);
            for (auto &state : states)
                state = {VK_IMAGE_LAYOUT_GENERAL, true}; // Unbound reads follow sparse residency rules.
            layout = VK_IMAGE_LAYOUT_GENERAL;
            initialized = true;
        }
        if (usage & viewUsages)
            view = imageView(*this, f, o.type, 0, o.mipLevels, 0, o.layers);
    } catch (...) {
        vmaDestroyImage(d->allocator, image, allocation);
        image = VK_NULL_HANDLE;
        throw;
    }
}
Texture::Texture(std::shared_ptr<Device> device, uint32_t w, uint32_t h, VkFormat f, VkImage i, VkImageView v)
    : Resource(std::move(device)), image(i), view(v), format(f), width(w), height(h),
      usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), storage(Storage::Private), borrowed(true) {
    states.resize(1);
}
Texture::Texture(std::shared_ptr<Device> device, std::shared_ptr<ExternalImage> imported, uint32_t w, uint32_t h,
                 VkFormat f, VkImageUsageFlags use, TextureOptions o)
    : Resource(std::move(device)), image(imported->image), external(std::move(imported)), format(f), width(w),
      height(h), options(o), usage(use), storage(Storage::Private) {
    states.resize(size_t(o.layers) * o.mipLevels, {VK_IMAGE_LAYOUT_GENERAL, true});
    layout = VK_IMAGE_LAYOUT_GENERAL;
    initialized = true;
    if (usage & viewUsages)
        view = imageView(*this, f, o.type, 0, o.mipLevels, 0, o.layers);
}
Texture::Texture(std::shared_ptr<Texture> p, VkFormat f, VkImageViewType type, uint32_t mip, uint32_t levels,
                 uint32_t layer, uint32_t layers, VkImageUsageFlags viewUsage, VkComponentMapping swizzle)
    : Resource(p->d), image(p->image), external(p->external), format(f), width(p->extent(mip).width),
      height(p->extent(mip).height), options(p->options), baseMip(p->baseMip + mip), baseLayer(p->baseLayer + layer),
      parent(p), usage(p->usage), storage(p->storage) {
    p->usable();
    require(!p->borrowed, "Drawable views are not exposed");
    require(levels && mip < p->options.mipLevels && levels <= p->options.mipLevels - mip && layers &&
                layer < p->options.layers && layers <= p->options.layers - layer,
            "Texture view subresource range out of bounds");
    const auto sourceClass = formatCompatibilityClass(p->root().format);
    require(f == p->root().format || (sourceClass && sourceClass == formatCompatibilityClass(f)),
            "Incompatible Vulkan format class");
    if (format != VK_FORMAT_UNDEFINED)
        pixelSize();
    requireCompressionFeature(*d, format);
    require(!external || f == p->root().format || (external->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT),
            "Imported image does not allow mutable formats");
    if (viewUsage) {
        require((viewUsage & p->usage) == viewUsage, "View usage must be a subset of parent usage");
        usage = viewUsage;
    }
    components = swizzle;
    const bool identity = (swizzle.r == VK_COMPONENT_SWIZZLE_IDENTITY || swizzle.r == VK_COMPONENT_SWIZZLE_R) &&
                          (swizzle.g == VK_COMPONENT_SWIZZLE_IDENTITY || swizzle.g == VK_COMPONENT_SWIZZLE_G) &&
                          (swizzle.b == VK_COMPONENT_SWIZZLE_IDENTITY || swizzle.b == VK_COMPONENT_SWIZZLE_B) &&
                          (swizzle.a == VK_COMPONENT_SWIZZLE_IDENTITY || swizzle.a == VK_COMPONENT_SWIZZLE_A);
    require(identity || ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(usage & (viewUsages & ~VK_IMAGE_USAGE_SAMPLED_BIT))),
            "Component swizzles require a sampled-only image view");
    for (auto value : {swizzle.r, swizzle.g, swizzle.b, swizzle.a})
        require(value >= VK_COMPONENT_SWIZZLE_IDENTITY && value <= VK_COMPONENT_SWIZZLE_A, "Invalid component swizzle");
    const auto features = formatFeatures();
    for (auto [use, feature] : std::initializer_list<std::pair<VkImageUsageFlags, VkFormatFeatureFlags>>{
             {VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
             {VK_IMAGE_USAGE_STORAGE_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT},
             {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT},
             {VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT},
             {VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
              VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR}})
        require(!(usage & use) || (features & feature), "View format does not support requested usage");
    require(!external || !external->conversionSampler || (identity && type == VK_IMAGE_VIEW_TYPE_2D),
            "Converted image views require identity swizzle and 2D type");
    options.type = type;
    options.depth = p->extent(mip).depth;
    options.mipLevels = levels;
    options.layers = layers;
    require(imageType() == p->imageType(), "Incompatible image/view type");
    if (type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        require((p->root().flags() & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && layers % 6 == 0 &&
                    (type != VK_IMAGE_VIEW_TYPE_CUBE || layers == 6),
                "Invalid cube view");
    if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        require(d->enabled & CubeArray, "Cube array feature was not enabled");
    if (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_3D)
        require(layers == 1, "Non-array view requires one layer");
    if (usage & viewUsages)
        view = imageView(*this, f, type, baseMip, levels, baseLayer, layers);
}
VkImageView Texture::attachmentView(uint32_t mip, uint32_t layer, uint32_t layers) {
    require(mip < options.mipLevels && layer < options.layers && layers > 0 && layers <= options.layers - layer &&
                imageType() == VK_IMAGE_TYPE_2D,
            "Invalid attachment subresource");
    if (borrowed)
        return view;
    auto key = std::make_tuple(mip, layer, layers);
    auto it = attachmentViews.find(key);
    if (it != attachmentViews.end())
        return it->second;
    VkImageViewCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    i.image = image;
    i.viewType = layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    i.format = format;
    VkImageViewUsageCreateInfo u{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    u.usage = usage;
    i.pNext = &u;
    i.subresourceRange = {aspects(), baseMip + mip, 1, baseLayer + layer, layers};
    VkImageView v;
    check(vkCreateImageView(d->device, &i, nullptr, &v), "create attachment view");
    try {
        attachmentViews.emplace(key, v);
    } catch (...) {
        vkDestroyImageView(d->device, v, nullptr);
        throw;
    }
    return v;
}
void Texture::usable() const {
    require(!frame || frame->active, "Drawable is no longer acquired");
    if (parent)
        parent->usable();
}
Texture::~Texture() {
    for (auto [key, v] : attachmentViews) {
        (void)key;
        vkDestroyImageView(d->device, v, nullptr);
    }
    if (!borrowed) {
        if (view)
            vkDestroyImageView(d->device, view, nullptr);
        if (image && !parent && !external)
            vmaDestroyImage(d->allocator, image, allocation);
    }
}
Sampler::Sampler(std::shared_ptr<Device> device, bool filtering, bool repeat, float anisotropy, VkSamplerMipmapMode mip,
                 float minLod, float maxLod, float bias, VkCompareOp comparison, bool comparisonEnabled,
                 VkSamplerAddressMode address, VkSamplerReductionMode reduction, VkBorderColor border)
    : Resource(std::move(device)), linear(filtering || mip == VK_SAMPLER_MIPMAP_MODE_LINEAR),
      compare(comparisonEnabled) {
    reductionMode = reduction;
    require(std::isfinite(anisotropy) && anisotropy >= 1 && anisotropy <= d->properties.limits.maxSamplerAnisotropy &&
                (anisotropy == 1 || (d->enabled & Anisotropy)),
            "Invalid or disabled anisotropy");
    require(std::isfinite(minLod) && std::isfinite(maxLod) && minLod >= 0 && minLod <= maxLod && std::isfinite(bias) &&
                std::abs(bias) <= d->properties.limits.maxSamplerLodBias,
            "Invalid sampler LOD");
    VkSamplerCreateInfo i{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    i.magFilter = i.minFilter = filtering ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    i.mipmapMode = mip;
    i.addressModeU = i.addressModeV = i.addressModeW =
        address == VK_SAMPLER_ADDRESS_MODE_MAX_ENUM
            ? (repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
            : address;
    require(i.addressModeU <= VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, "Invalid sampler address mode");
    i.anisotropyEnable = anisotropy > 1;
    i.maxAnisotropy = anisotropy;
    i.minLod = minLod;
    i.maxLod = maxLod;
    i.mipLodBias = bias;
    i.compareEnable = compare;
    i.compareOp = comparison;
    i.borderColor = border;
    require(reduction <= VK_SAMPLER_REDUCTION_MODE_MAX && border <= VK_BORDER_COLOR_INT_OPAQUE_WHITE,
            "Invalid sampler reduction/border");
    require(reduction == VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE || ((d->enabled & SamplerMinMax) && !compare),
            "Min/max sampler requires enabled feature and comparison disabled");
    VkSamplerReductionModeCreateInfo mode{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    mode.reductionMode = reduction;
    if (reduction != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE)
        i.pNext = &mode;
    check(vkCreateSampler(d->device, &i, nullptr, &sampler), "vkCreateSampler");
}
Sampler::~Sampler() {
    if (sampler)
        vkDestroySampler(d->device, sampler, nullptr);
    if (conversion)
        vkDestroySamplerYcbcrConversion(d->device, conversion, nullptr);
}

void Command::transition(Texture &t, VkImageLayout layout, bool read) {
    transition(t, layout, read, 0, 0, t.options.mipLevels, t.options.layers);
}
void Command::transition(Texture &t, VkImageLayout layout, bool read, uint32_t mip, uint32_t layer, uint32_t levels,
                         uint32_t layers) {
    t.usable();
    requireOwnership(t);
    require(!t.borrowed || (presentation && presentation->texture.get() == &t),
            "Drawable must be presented by the same command");
    require(levels && mip < t.options.mipLevels && levels <= t.options.mipLevels - mip && layers &&
                layer < t.options.layers && layers <= t.options.layers - layer,
            "Invalid subresource range");
    auto &root = t.root();
    auto [it, added] = images.emplace(&root, root.states);
    (void)added;
    for (uint32_t a = layer; a < layer + layers; ++a)
        for (uint32_t m = mip; m < mip + levels; ++m) {
            auto &state = it->second[(a + t.baseLayer) * root.options.mipLevels + m + t.baseMip];
            require(!read || state.initialized, "Cannot read uninitialized/discarded texture subresource");
            if (state.layout == layout)
                continue;
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = state.layout;
            b.newLayout = layout;
            b.srcAccessMask =
                state.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t.image;
            b.subresourceRange = {t.aspects(), m + t.baseMip, 1, a + t.baseLayer, 1};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
            state.layout = layout;
            ++imageBarrierCount;
        }
}
void Command::markInitialized(Texture &t, bool value) {
    markInitialized(t, value, 0, 0, t.options.mipLevels, t.options.layers);
}
void Command::markInitialized(Texture &t, bool value, uint32_t mip, uint32_t layer, uint32_t levels, uint32_t layers) {
    auto &root = t.root();
    auto &states = images.at(&root);
    for (uint32_t a = layer; a < layer + layers; ++a)
        for (uint32_t m = mip; m < mip + levels; ++m)
            states[(a + t.baseLayer) * root.options.mipLevels + m + t.baseMip].initialized = value;
}
void Command::fill(std::shared_ptr<Buffer> b, VkDeviceSize offset, VkDeviceSize size, uint32_t value) {
    recording();
    requireQueue(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    require(bool(b), "Buffer required");
    sameOwner(*this, *b);
    bounds(b->size, offset, size);
    require((b->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) && offset % 4 == 0 && size % 4 == 0,
            "Fill requires transfer destination and 4-byte alignment");
    buffers.push_back(b);
    operations.push_back([b, offset, size, value](Command &c) {
        c.barrier();
        vkCmdFillBuffer(c.command, b->buffer, offset, size, value);
    });
}
namespace {
void validRegion(Texture &t, ImageRegion &r) {
    const auto e = t.extent(r.mip);
    require(r.layers && r.layer < t.options.layers && r.layers <= t.options.layers - r.layer, "Layer out of range");
    if (!r.size.width && !r.size.height && !r.size.depth)
        r.size = e;
    require(r.origin.x >= 0 && r.origin.y >= 0 && r.origin.z >= 0 && uint32_t(r.origin.x) < e.width &&
                uint32_t(r.origin.y) < e.height && uint32_t(r.origin.z) < e.depth,
            "Texture origin out of range");
    require(r.size.width && r.size.height && r.size.depth && r.size.width <= e.width - r.origin.x &&
                r.size.height <= e.height - r.origin.y && r.size.depth <= e.depth - r.origin.z,
            "Texture region out of range");
    require(uint32_t(r.origin.x) % t.blockWidth() == 0 && uint32_t(r.origin.y) % t.blockHeight() == 0 &&
                (r.size.width % t.blockWidth() == 0 || uint32_t(r.origin.x) + r.size.width == e.width) &&
                (r.size.height % t.blockHeight() == 0 || uint32_t(r.origin.y) + r.size.height == e.height),
            "Compressed region must align to blocks or image edge");
}
bool fullRegion(Texture &t, const ImageRegion &r) {
    auto e = t.extent(r.mip);
    return r.size.width == e.width && r.size.height == e.height && r.size.depth == e.depth;
}
} // namespace
void Command::validateTransfer(const Texture &t, const ImageRegion &r) const {
    const auto g = queueInfo().properties.minImageTransferGranularity;
    const auto e = t.extent(r.mip);
    auto valid = [](uint32_t origin, uint32_t size, uint32_t extent, uint64_t alignment) {
        return alignment ? origin % alignment == 0 && (size % alignment == 0 || origin + size == extent)
                         : origin == 0 && size == extent;
    };
    require(valid(uint32_t(r.origin.x), r.size.width, e.width, uint64_t(g.width) * t.blockWidth()) &&
                valid(uint32_t(r.origin.y), r.size.height, e.height, uint64_t(g.height) * t.blockHeight()) &&
                valid(uint32_t(r.origin.z), r.size.depth, e.depth, g.depth),
            "Texture region does not satisfy this queue's image transfer granularity");
}
void Command::copy(std::shared_ptr<Buffer> b, std::shared_ptr<Texture> t, VkDeviceSize offset, bool toTexture) {
    copy(b, t, offset, toTexture, {});
}
void Command::copy(std::shared_ptr<Buffer> b, std::shared_ptr<Texture> t, VkDeviceSize offset, bool toTexture,
                   ImageRegion r, uint32_t row, uint32_t height) {
    recording();
    requireQueue(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    require(b && t, "Buffer and texture required");
    require(!memoryOverlaps(*b, *t), "Buffer/image copy aliases physical memory");
    sameOwner(*this, *b);
    sameOwner(*this, *t);
    t->usable();
    validRegion(*t, r);
    validateTransfer(*t, r);
    if (t->depth() || t->stencil())
        requireQueue(VK_QUEUE_GRAPHICS_BIT);
    require(t->options.samples == 1 && !(t->depth() && t->stencil()),
            "Resolve multisampling first; packed depth/stencil copies require separate aspects");
    require((b->usage & (toTexture ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT)) &&
                (t->usage & (toTexture ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT)),
            "Matching transfer usage required");
    require(!row || (row >= r.size.width && row % t->blockWidth() == 0), "Invalid row length");
    require(!height || (height >= r.size.height && height % t->blockHeight() == 0), "Invalid image height");
    require(offset % 4 == 0 && offset % t->pixelSize() == 0, "Invalid copy alignment");
    uint64_t bw = t->blockWidth(), bh = t->blockHeight(), rows = (r.size.height + bh - 1) / bh;
    uint64_t rowBytes = ((row ? row : r.size.width) + bw - 1) / bw * t->pixelSize();
    uint64_t sliceBytes = rowBytes * ((height ? height : r.size.height) + bh - 1) / bh;
    uint64_t bytes = (uint64_t(r.layers) * r.size.depth - 1) * sliceBytes + (rows - 1) * rowBytes +
                     ((r.size.width + bw - 1) / bw) * t->pixelSize();
    bounds(b->size, offset, bytes);
    buffers.push_back(b);
    operations.push_back([b, t, offset, toTexture, r, row, height](Command &c) {
        c.barrier();
        const auto layout = toTexture ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        c.transition(*t, layout, !toTexture || !fullRegion(*t, r), r.mip, r.layer, 1, r.layers);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.bufferRowLength = row;
        region.bufferImageHeight = height;
        region.imageSubresource = {t->aspects(), t->baseMip + r.mip, t->baseLayer + r.layer, r.layers};
        region.imageOffset = r.origin;
        region.imageExtent = r.size;
        if (toTexture) {
            vkCmdCopyBufferToImage(c.command, b->buffer, t->image, layout, 1, &region);
            c.markInitialized(*t, true, r.mip, r.layer, 1, r.layers);
        } else
            vkCmdCopyImageToBuffer(c.command, t->image, layout, b->buffer, 1, &region);
    });
}
void Command::copy(std::shared_ptr<Texture> source, std::shared_ptr<Texture> dest, ImageRegion a, ImageRegion b) {
    recording();
    requireQueue(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    require(source && dest, "Textures required");
    sameOwner(*this, *source);
    sameOwner(*this, *dest);
    validRegion(*source, a);
    validRegion(*dest, b);
    validateTransfer(*source, a);
    validateTransfer(*dest, b);
    const bool sameImage = source->image == dest->image;
    require(!sparseMemoryOverlaps(*source, *dest),
            "Copies between shared sparse mappings require an intermediate image");
    require(sameImage || !memoryOverlaps(*source, *dest), "Image copy aliases physical memory");
    if (sameImage && source->baseMip + a.mip == dest->baseMip + b.mip) {
        auto overlap = [](uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return a < b + bn && b < a + an; };
        const bool aliases = overlap(source->baseLayer + a.layer, a.layers, dest->baseLayer + b.layer, b.layers) &&
                             overlap(a.origin.x, a.size.width, b.origin.x, b.size.width) &&
                             overlap(a.origin.y, a.size.height, b.origin.y, b.size.height) &&
                             overlap(a.origin.z, a.size.depth, b.origin.z, b.size.depth);
        require(!aliases, "Texture copy regions overlap");
    }
    require(source->format == dest->format && source->options.samples == dest->options.samples &&
                a.layers == b.layers && a.size.width == b.size.width && a.size.height == b.size.height &&
                a.size.depth == b.size.depth,
            "Texture copy format/extent mismatch");
    require((source->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (dest->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT),
            "Texture transfer usage required");
    operations.push_back([source, dest, a, b, sameImage](Command &c) {
        c.barrier();
        const auto sourceLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const auto destinationLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        c.transition(*source, sourceLayout, true, a.mip, a.layer, 1, a.layers);
        c.transition(*dest, destinationLayout, !fullRegion(*dest, b), b.mip, b.layer, 1, b.layers);
        VkImageCopy region{{source->aspects(), source->baseMip + a.mip, source->baseLayer + a.layer, a.layers},
                           a.origin,
                           {dest->aspects(), dest->baseMip + b.mip, dest->baseLayer + b.layer, b.layers},
                           b.origin,
                           a.size};
        vkCmdCopyImage(c.command, source->image, sourceLayout, dest->image, destinationLayout, 1, &region);
        c.markInitialized(*dest, true, b.mip, b.layer, 1, b.layers);
    });
}
void Command::generateMipmaps(std::shared_ptr<Texture> t, VkFilter filter) {
    recording();
    requireQueue(VK_QUEUE_GRAPHICS_BIT);
    require(bool(t), "Texture required");
    sameOwner(*this, *t);
    t->usable();
    require(t->options.samples == 1 && !t->depth() && !t->stencil() && t->options.mipLevels > 1,
            "Mip generation requires a color texture with multiple levels");
    require((t->usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ==
                (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
            "Mip generation requires both transfer usages");
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(d->physical, t->format, &fp);
    require((fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) ==
                (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT),
            "Format cannot generate mipmaps using blit");
    require(filter == VK_FILTER_NEAREST ||
                (filter == VK_FILTER_LINEAR &&
                 (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)),
            "Unsupported mip filter");
    operations.push_back([t, filter](Command &c) {
        c.barrier();
        for (uint32_t m = 1; m < t->options.mipLevels; ++m) {
            c.transition(*t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true, m - 1, 0, 1, t->options.layers);
            c.transition(*t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false, m, 0, 1, t->options.layers);
            auto a = t->extent(m - 1), b = t->extent(m);
            VkImageBlit region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->baseMip + m - 1, t->baseLayer, t->options.layers};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->baseMip + m, t->baseLayer, t->options.layers};
            region.srcOffsets[1] = {int32_t(a.width), int32_t(a.height), int32_t(a.depth)};
            region.dstOffsets[1] = {int32_t(b.width), int32_t(b.height), int32_t(b.depth)};
            vkCmdBlitImage(c.command, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter);
            c.markInitialized(*t, true, m, 0, 1, t->options.layers);
        }
    });
}
} // namespace vulkano
