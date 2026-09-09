#pragma once
#include "engine.hpp"
namespace vulkano {
struct ExternalSemaphore : Resource {
    enum class State { Fresh, Imported, Signalled, Consumed, Exported };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    State state = State::Fresh;
    ExternalSemaphore(std::shared_ptr<Device>, int importFd = -2);
    int exportFd();
    ~ExternalSemaphore() override;
};
struct ExternalImage : Resource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageCreateFlags flags = 0;
    VkFormatFeatureFlags formatFeatures = 0;
    uint32_t externalFamily = VK_QUEUE_FAMILY_FOREIGN_EXT;
    bool gpuOwned = false;
    void *hardwareBuffer = nullptr;
    std::shared_ptr<Sampler> conversionSampler;
    explicit ExternalImage(std::shared_ptr<Device> device) : Resource(std::move(device)) {}
    ~ExternalImage() override;
};
#ifdef __ANDROID__
std::shared_ptr<Texture> importHardwareBuffer(std::shared_ptr<Device>, AHardwareBuffer *, VkImageUsageFlags,
                                              bool externalFormat, bool linear, int model, int range,
                                              uint32_t externalFamily);
#endif
int duplicateSyncFd(int fd);
} // namespace vulkano
