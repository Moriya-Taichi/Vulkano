#include "interop.hpp"
#include "extensions.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
namespace vulkano {
int duplicateSyncFd(int fd) {
    require(fd >= -1, "Invalid sync file descriptor");
    if (fd == -1)
        return -1;
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0)
        throw std::system_error(errno, std::generic_category(), "duplicate sync fd");
    return copy;
}
ExternalSemaphore::ExternalSemaphore(std::shared_ptr<Device> device, int importFd) : Resource(std::move(device)) {
    require(d->enabledExtra & ExternalSyncFd, "External SYNC_FD feature was not enabled");
    require(importFd >= -2, "Invalid external semaphore descriptor");
    VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (importFd == -2)
        create.pNext = &exportInfo;
    check(vkCreateSemaphore(d->device, &create, nullptr, &semaphore), "create external semaphore");
    int copy = -1;
    try {
        if (importFd != -2) {
            copy = duplicateSyncFd(importFd);
            VkImportSemaphoreFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
            info.semaphore = semaphore;
            info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            info.fd = copy;
            check(d->extensions->importSemaphoreFd(d->device, &info), "import sync fd");
            copy = -1; // Vulkan owns the duplicate after successful import.
            state = State::Imported;
        }
    } catch (...) {
        if (copy >= 0)
            close(copy);
        vkDestroySemaphore(d->device, semaphore, nullptr);
        semaphore = VK_NULL_HANDLE;
        throw;
    }
}
int ExternalSemaphore::exportFd() {
    require(state == State::Signalled, "Submit a signal before exporting this one-shot semaphore");
    VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    info.semaphore = semaphore;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int fd;
    check(d->extensions->getSemaphoreFd(d->device, &info, &fd), "export sync fd");
    state = State::Exported;
    return fd;
}
ExternalSemaphore::~ExternalSemaphore() {
    if (semaphore)
        vkDestroySemaphore(d->device, semaphore, nullptr);
}
void Command::waitExternal(std::shared_ptr<ExternalSemaphore> event) {
    recording();
    require(event && event->owner() == d.get(), "External semaphore device mismatch");
    require(std::find(externalWaits.begin(), externalWaits.end(), event) == externalWaits.end(),
            "Duplicate external wait");
    externalWaits.push_back(std::move(event));
}
void Command::signalExternal(std::shared_ptr<ExternalSemaphore> event) {
    recording();
    require(event && event->owner() == d.get(), "External semaphore device mismatch");
    require(std::find(externalSignals.begin(), externalSignals.end(), event) == externalSignals.end(),
            "Duplicate external signal");
    externalSignals.push_back(std::move(event));
}
} // namespace vulkano

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#endif
namespace vulkano {
ExternalImage::~ExternalImage() {
    if (image)
        vkDestroyImage(d->device, image, nullptr);
    if (memory)
        vkFreeMemory(d->device, memory, nullptr);
#ifdef __ANDROID__
    if (hardwareBuffer)
        AHardwareBuffer_release(static_cast<AHardwareBuffer *>(hardwareBuffer));
#endif
}
void Command::requireOwnership(Texture &texture) {
    const auto &external = texture.root().external;
    if (!external)
        return;
    const auto found = externalOwnership.find(external.get());
    require(found == externalOwnership.end() ? external->gpuOwned : found->second,
            "Acquire the external texture before using it on this device");
}
void Command::acquireExternal(std::shared_ptr<Texture> texture, bool preserve) {
    recording();
    require(texture && texture->owner() == d.get() && texture->external && !texture->parent,
            "Acquire requires a root external texture from this device");
    operations.push_back([texture, preserve](Command &c) {
        auto &external = *texture->external;
        auto [owned, inserted] = c.externalOwnership.emplace(&external, external.gpuOwned);
        (void)inserted;
        require(!owned->second, "External texture is already acquired");
        auto [states, added] = c.images.emplace(texture.get(), texture->states);
        (void)added;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = preserve ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = external.externalFamily;
        barrier.dstQueueFamilyIndex = c.d->family;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.image = texture->image;
        barrier.subresourceRange = {texture->aspects(), 0, texture->options.mipLevels, 0, texture->options.layers};
        vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        for (auto &state : states->second)
            state = {VK_IMAGE_LAYOUT_GENERAL, preserve};
        owned->second = true;
        ++c.imageBarrierCount;
    });
}
void Command::releaseExternal(std::shared_ptr<Texture> texture) {
    recording();
    require(texture && texture->owner() == d.get() && texture->external && !texture->parent,
            "Release requires a root external texture from this device");
    operations.push_back([texture](Command &c) {
        c.requireOwnership(*texture);
        c.transition(*texture, VK_IMAGE_LAYOUT_GENERAL, false);
        auto &external = *texture->external;
        auto [owned, added] = c.externalOwnership.emplace(&external, external.gpuOwned);
        (void)added;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = c.d->family;
        barrier.dstQueueFamilyIndex = external.externalFamily;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.image = texture->image;
        barrier.subresourceRange = {texture->aspects(), 0, texture->options.mipLevels, 0, texture->options.layers};
        vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        owned->second = false;
        ++c.imageBarrierCount;
    });
}
#ifdef __ANDROID__
namespace {
std::shared_ptr<Sampler> hardwareConversion(std::shared_ptr<Device> d,
                                            const VkAndroidHardwareBufferFormatPropertiesANDROID &format, bool linear,
                                            int model, int range) {
    require(d->enabledExtra & SamplerYcbcr, "YCbCr conversion feature was not enabled");
    require(format.externalFormat != 0, "Hardware buffer has no external format");
    if (model == -1)
        model = format.suggestedYcbcrModel;
    if (range == -1)
        range = format.suggestedYcbcrRange;
    require(model >= VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY &&
                model <= VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020 && range >= VK_SAMPLER_YCBCR_RANGE_ITU_FULL &&
                range <= VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
            "Invalid YCbCr model or range");
    const auto features = format.formatFeatures;
    require(features & (VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT | VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT),
            "External format lacks chroma reconstruction support");
    require(!linear || ((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) &&
                        (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)),
            "External format does not support linear luma/chroma filtering");
    const auto supportedLocation = [&](VkChromaLocation suggested) {
        const auto bit = suggested == VK_CHROMA_LOCATION_MIDPOINT ? VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT
                                                                  : VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;
        return features & bit
                   ? suggested
                   : (features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT ? VK_CHROMA_LOCATION_MIDPOINT
                                                                               : VK_CHROMA_LOCATION_COSITED_EVEN);
    };
    const auto x = supportedLocation(format.suggestedXChromaOffset);
    const auto y = supportedLocation(format.suggestedYChromaOffset);
    std::vector<uint64_t> key{format.externalFormat, uint64_t(model), uint64_t(range),
                              uint64_t(x),           uint64_t(y),     uint64_t(linear)};
    auto &cached = d->conversionSamplers[key];
    if (auto existing = cached.lock())
        return existing;
    auto sampler = std::make_shared<Sampler>(d);
    sampler->linear = linear;
    // A combined YCbCr descriptor may occupy one slot per plane; reserve the
    // conservative three-plane upper bound for opaque Android external formats.
    sampler->descriptorCost = 3;
    VkExternalFormatANDROID external{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    external.externalFormat = format.externalFormat;
    VkSamplerYcbcrConversionCreateInfo conversion{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    conversion.pNext = &external;
    conversion.format = VK_FORMAT_UNDEFINED;
    conversion.ycbcrModel = VkSamplerYcbcrModelConversion(model);
    conversion.ycbcrRange = VkSamplerYcbcrRange(range);
    conversion.xChromaOffset = x;
    conversion.yChromaOffset = y;
    conversion.chromaFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    check(vkCreateSamplerYcbcrConversion(d->device, &conversion, nullptr, &sampler->conversion),
          "create hardware buffer YCbCr conversion");
    VkSamplerYcbcrConversionInfo attached{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    attached.conversion = sampler->conversion;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.pNext = &attached;
    info.minFilter = info.magFilter = conversion.chromaFilter;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(d->device, &info, nullptr, &sampler->sampler), "create hardware buffer sampler");
    cached = sampler;
    return sampler;
}
} // namespace
std::shared_ptr<Texture> importHardwareBuffer(std::shared_ptr<Device> d, AHardwareBuffer *buffer,
                                              VkImageUsageFlags usage, bool useExternalFormat, bool linear, int model,
                                              int range, uint32_t externalFamily) {
    require(d->enabledExtra & HardwareBufferInterop, "Android hardware buffer feature was not enabled");
    require(buffer && (externalFamily == VK_QUEUE_FAMILY_FOREIGN_EXT || externalFamily == VK_QUEUE_FAMILY_EXTERNAL),
            "Invalid hardware buffer or external queue family");
    uint64_t identity = reinterpret_cast<uintptr_t>(buffer);
    using GetId = int (*)(const AHardwareBuffer *, uint64_t *);
    // AHardwareBuffer_getId was added at API 31; retain API 29 compatibility.
    static const auto getId = reinterpret_cast<GetId>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_getId"));
    const bool stableId = getId && getId(buffer, &identity) == 0;
    for (auto it = d->importedImages.begin(); it != d->importedImages.end();)
        if (it->second.expired())
            it = d->importedImages.erase(it);
        else
            ++it;
    auto &existing = d->importedImages[{identity, stableId}];
    require(existing.expired(), "Hardware buffer already imported; reuse its texture and views");
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);
    require(desc.width && desc.height && desc.layers && desc.format != AHARDWAREBUFFER_FORMAT_BLOB &&
                !(desc.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT),
            "Image import requires an unprotected image hardware buffer");
    constexpr VkImageUsageFlags allowed = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                          VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    require(usage && !(usage & ~allowed), "Invalid hardware buffer texture usage");
    require(!(usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) ||
                (desc.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE),
            "Hardware buffer lacks sampled usage");
    require(!(usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) ||
                (desc.usage & AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER),
            "Hardware buffer lacks framebuffer usage");
    require(!(usage & VK_IMAGE_USAGE_STORAGE_BIT) || (desc.usage & AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER),
            "Hardware buffer lacks GPU data buffer usage");
    require(desc.usage & (AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                          AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER),
            "Hardware buffer requires GPU usage");
    require(desc.width <= d->properties.limits.maxImageDimension2D &&
                desc.height <= d->properties.limits.maxImageDimension2D &&
                desc.layers <= d->properties.limits.maxImageArrayLayers,
            "Hardware buffer exceeds device limits");
    VkAndroidHardwareBufferFormatPropertiesANDROID format{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID properties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    properties.pNext = &format;
    check(d->extensions->hardwareBufferProperties(d->device, buffer, &properties), "query hardware buffer properties");
    useExternalFormat |= format.format == VK_FORMAT_UNDEFINED || format.format >= VK_FORMAT_G8B8G8R8_422_UNORM;
    const auto vkFormat = useExternalFormat ? VK_FORMAT_UNDEFINED : format.format;
    const bool depthStencil = vkFormat >= VK_FORMAT_D16_UNORM && vkFormat <= VK_FORMAT_D32_SFLOAT_S8_UINT;
    require(depthStencil ? !(usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT))
                         : !(usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
            "Hardware buffer attachment format/usage mismatch");
    TextureOptions options;
    options.layers = desc.layers;
    options.type = desc.layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (desc.usage & AHARDWAREBUFFER_USAGE_GPU_MIPMAP_COMPLETE)
        for (auto extent = std::max(desc.width, desc.height); extent > 1; extent >>= 1)
            ++options.mipLevels;
    require(!useExternalFormat || (usage == VK_IMAGE_USAGE_SAMPLED_BIT && desc.layers == 1 && options.mipLevels == 1),
            "External-format images require sampled-only, single-layer, single-mip usage");
    auto imported = std::make_shared<ExternalImage>(d);
    imported->externalFamily = externalFamily;
    imported->formatFeatures = format.formatFeatures;
    imported->hardwareBuffer = buffer;
    AHardwareBuffer_acquire(buffer);
    if (useExternalFormat)
        imported->conversionSampler = hardwareConversion(d, format, linear, model, range);
    else {
        require(model == -1 && range == -1, "Color conversion options require external-format import");
        imported->flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (desc.usage & AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP)
            imported->flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        query.pNext = &external;
        query.format = vkFormat;
        query.type = VK_IMAGE_TYPE_2D;
        query.tiling = VK_IMAGE_TILING_OPTIMAL;
        query.usage = usage;
        query.flags = imported->flags;
        VkExternalImageFormatProperties support{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 answer{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        answer.pNext = &support;
        check(vkGetPhysicalDeviceImageFormatProperties2(d->physical, &query, &answer),
              "unsupported hardware buffer usage");
        require(support.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
                "Image format cannot import hardware buffers");
        const auto &limit = answer.imageFormatProperties;
        require(desc.width <= limit.maxExtent.width && desc.height <= limit.maxExtent.height &&
                    desc.layers <= limit.maxArrayLayers && options.mipLevels <= limit.maxMipLevels,
                "Hardware buffer dimensions exceed format limits");
    }
    VkExternalFormatANDROID externalFormat{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    externalFormat.externalFormat = useExternalFormat ? format.externalFormat : 0;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    external.pNext = &externalFormat;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = &external;
    info.flags = imported->flags;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = vkFormat;
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = options.mipLevels;
    info.arrayLayers = desc.layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    check(vkCreateImage(d->device, &info, nullptr, &imported->image), "create hardware buffer image");
    // AHB memory requirements are not queryable before binding. The AHB
    // properties provide the required allocation size and memory type mask.
    uint32_t memoryType = d->memory.memoryTypeCount;
    for (uint32_t i = 0; i < d->memory.memoryTypeCount; ++i)
        if ((properties.memoryTypeBits & (1u << i)) &&
            !(d->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT)) {
            memoryType = i;
            if (d->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                break;
        }
    require(memoryType < d->memory.memoryTypeCount, "No compatible hardware buffer memory type");
    VkImportAndroidHardwareBufferInfoANDROID import{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    import.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = imported->image;
    dedicated.pNext = &import;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &dedicated;
    allocation.allocationSize = properties.allocationSize;
    allocation.memoryTypeIndex = memoryType;
    check(vkAllocateMemory(d->device, &allocation, nullptr, &imported->memory), "import hardware buffer memory");
    check(vkBindImageMemory(d->device, imported->image, imported->memory, 0), "bind hardware buffer image");
    auto texture = std::make_shared<Texture>(d, imported, desc.width, desc.height, vkFormat, usage, options);
    existing = texture;
    return texture;
}
#endif
} // namespace vulkano
