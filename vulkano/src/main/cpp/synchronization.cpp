#include "synchronization.hpp"
#include <algorithm>
namespace vulkano {
SharedEvent::SharedEvent(std::shared_ptr<Device> device, uint64_t initial)
    : Resource(std::move(device)), lastScheduled(initial) {
    require(d->enabled & Timeline, "Timeline semaphore feature was not enabled");
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = initial;
    VkSemaphoreCreateInfo i{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    i.pNext = &type;
    check(vkCreateSemaphore(d->device, &i, nullptr, &semaphore), "create shared event");
}
SharedEvent::~SharedEvent() {
    if (semaphore)
        vkDestroySemaphore(d->device, semaphore, nullptr);
}
uint64_t SharedEvent::value() const {
    uint64_t value;
    check(d->extensions->semaphoreValue(d->device, semaphore, &value), "read shared event");
    return value;
}
void SharedEvent::signal(uint64_t n) {
    pendingSignals.erase(std::remove_if(pendingSignals.begin(), pendingSignals.end(),
                                        [](const auto &p) {
                                            auto c = p.second.lock();
                                            return !c || c->state == Command::State::Completed ||
                                                   (c->state == Command::State::Submitted && c->wait(0));
                                        }),
                         pendingSignals.end());
    const auto current = value();
    require(n > current, "Host signal must increase the current event value");
    require(n - current <= d->extensions->timelineProperties.maxTimelineSemaphoreValueDifference,
            "Event value exceeds timeline distance limit");
    for (const auto &pending : pendingSignals)
        require(n < pending.first, "Host signal must be below every pending GPU signal");
    VkSemaphoreSignalInfo i{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    i.semaphore = semaphore;
    i.value = n;
    check(d->extensions->signalSemaphore(d->device, &i), "signal shared event");
    lastScheduled = std::max(lastScheduled, n);
}
CounterPool::CounterPool(std::shared_ptr<Device> device, uint32_t n, bool time, uint32_t queue)
    : Resource(std::move(device)), count(n), queueIndex(queue), timestamp(time), issued(n, false) {
    require(n && n <= 65536, "Invalid counter count");
    require(queueIndex < d->queues.size(), "Unknown counter queue index");
    const auto &q = d->queues[queueIndex].properties;
    require(time ? q.timestampValidBits > 0 : bool(q.queueFlags & VK_QUEUE_GRAPHICS_BIT),
            "Queue does not support this counter type");
    VkQueryPoolCreateInfo i{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    i.queryType = time ? VK_QUERY_TYPE_TIMESTAMP : VK_QUERY_TYPE_OCCLUSION;
    i.queryCount = n;
    check(vkCreateQueryPool(d->device, &i, nullptr, &pool), "create counter sample buffer");
}
CounterPool::~CounterPool() {
    if (pool)
        vkDestroyQueryPool(d->device, pool, nullptr);
}
std::vector<uint64_t> CounterPool::read() {
    auto c = writer.lock();
    if (c && c->state == Command::State::Submitted && !c->wait(0))
        return {};
    for (bool used : issued)
        if (!used)
            return {};
    std::vector<uint64_t> values(count);
    auto result =
        vkGetQueryPoolResults(d->device, pool, 0, count, values.size() * 8, values.data(), 8, VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY)
        return {};
    check(result, "read counters");
    return values;
}
void Command::sample(std::shared_ptr<CounterPool> pool, uint32_t index) {
    recording();
    require(pool && pool->owner() == d.get() && pool->timestamp && index < pool->count, "Invalid timestamp sample");
    require(pool->queueIndex == queueIndex, "Counter belongs to a different physical queue");
    counters.push_back(pool);
    counterIndices.push_back(index);
    operations.push_back([pool, index](Command &c) {
        vkCmdResetQueryPool(c.command, pool->pool, index, 1);
        vkCmdWriteTimestamp(c.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool->pool, index);
    });
}
void Command::waitEvent(std::shared_ptr<SharedEvent> event, uint64_t value) {
    recording();
    require(event && event->owner() == d.get() && operations.empty(),
            "Event waits must be encoded before GPU operations");
    for (const auto &e : eventWaits)
        require(e.first != event, "Duplicate wait event");
    eventWaits.emplace_back(event, value);
}
void Command::signalEvent(std::shared_ptr<SharedEvent> event, uint64_t value) {
    recording();
    require(event && event->owner() == d.get(), "Invalid signal event");
    for (const auto &e : eventSignals)
        require(e.first != event, "Duplicate signal event");
    eventSignals.emplace_back(event, value);
}
} // namespace vulkano
