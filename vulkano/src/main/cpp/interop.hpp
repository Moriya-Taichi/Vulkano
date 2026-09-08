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
int duplicateSyncFd(int fd);
} // namespace vulkano
