#pragma once
#include "extensions.hpp"
namespace vulkano {
struct SharedEvent : Resource {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t lastScheduled = 0;
    std::vector<std::pair<uint64_t, std::weak_ptr<Command>>> pendingSignals;
    SharedEvent(std::shared_ptr<Device>, uint64_t initial);
    uint64_t value() const;
    void signal(uint64_t);
    ~SharedEvent() override;
};
struct CounterPool : Resource {
    VkQueryPool pool = VK_NULL_HANDLE;
    uint32_t count;
    bool timestamp;
    std::weak_ptr<Command> writer;
    std::vector<bool> issued;
    CounterPool(std::shared_ptr<Device>, uint32_t count, bool timestamp);
    std::vector<uint64_t> read();
    ~CounterPool() override;
};
} // namespace vulkano
