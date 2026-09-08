#include "graphs.hpp"
#include <algorithm>

namespace vulkano {
void Extensions::inspectGraphQueues(VkInstance instance, VkPhysicalDevice device,
                                    const std::vector<VkQueueFamilyProperties> &families) {
    if (!(availableExtra & DataGraph))
        return;
    const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM"));
    if (!query) {
        availableExtra &= ~DataGraph;
        return;
    }
    for (uint32_t family = 0; family < families.size(); ++family) {
        if (!families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_DATA_GRAPH_BIT_ARM))
            continue;
        uint32_t count = 0;
        check(query(device, family, &count, nullptr), "query graph queue operations");
        std::vector<VkQueueFamilyDataGraphPropertiesARM> rows(
            count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_DATA_GRAPH_PROPERTIES_ARM});
        VkResult result = query(device, family, &count, rows.data());
        if (result == VK_INCOMPLETE) {
            check(query(device, family, &count, nullptr), "query graph operation count");
            rows.assign(count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_DATA_GRAPH_PROPERTIES_ARM});
            result = query(device, family, &count, rows.data());
        }
        check(result, "query graph operations");
        rows.resize(count);
        // Vulkano targets GPU execution. Foreign NPU engines require a separate
        // external-memory contract and must never be selected implicitly.
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [](const auto &row) {
                                      return row.engine.isForeign ||
                                             row.engine.type !=
                                                 VK_PHYSICAL_DEVICE_DATA_GRAPH_PROCESSING_ENGINE_TYPE_DEFAULT_ARM;
                                  }),
                   rows.end());
        if (!rows.empty())
            graphQueues.emplace(family, std::move(rows));
    }
    if (graphQueues.empty())
        availableExtra &= ~DataGraph;
}

void Command::submitGraphSegments(const VkSubmitInfo &outer) {
    require((d->enabledExtra & DataGraph) && !graphSegments.empty(), "Graph queue is unavailable");
    auto &order = d->graphOrder[queueIndex];
    if (!order.semaphore) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        check(vkCreateSemaphore(d->device, &info, nullptr, &order.semaphore), "create graph queue timeline");
    }
    const size_t n = graphSegments.size();
    require(n <= UINT64_MAX - order.value, "Graph queue timeline exhausted");
    uint64_t completed = 0;
    check(d->extensions->semaphoreValue(d->device, order.semaphore, &completed), "query graph queue timeline");
    require(order.value + n - completed <= d->extensions->timelineProperties.maxTimelineSemaphoreValueDifference,
            "Too many queued graph dispatches");
    const auto *outerTimeline = static_cast<const VkTimelineSemaphoreSubmitInfo *>(outer.pNext);
    std::vector<std::vector<VkSemaphore>> waits(n), signals(n);
    std::vector<std::vector<uint64_t>> waitValues(n), signalValues(n);
    std::vector<std::vector<VkPipelineStageFlags>> stages(n);
    std::vector<VkSubmitInfo> submits(n, {VK_STRUCTURE_TYPE_SUBMIT_INFO});
    std::vector<VkTimelineSemaphoreSubmitInfo> timelines(n, {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO});
    for (size_t i = 0; i < n; ++i) {
        if (i == 0)
            for (uint32_t j = 0; j < outer.waitSemaphoreCount; ++j) {
                waits[i].push_back(outer.pWaitSemaphores[j]);
                waitValues[i].push_back(outerTimeline ? outerTimeline->pWaitSemaphoreValues[j] : 0);
            }
        if (order.value + i) {
            waits[i].push_back(order.semaphore);
            waitValues[i].push_back(order.value + i);
        }
        if (i + 1 == n)
            for (uint32_t j = 0; j < outer.signalSemaphoreCount; ++j) {
                signals[i].push_back(outer.pSignalSemaphores[j]);
                signalValues[i].push_back(outerTimeline ? outerTimeline->pSignalSemaphoreValues[j] : 0);
            }
        signals[i].push_back(order.semaphore);
        signalValues[i].push_back(order.value + i + 1);
        stages[i].assign(waits[i].size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        auto &t = timelines[i];
        t.waitSemaphoreValueCount = uint32_t(waitValues[i].size());
        t.pWaitSemaphoreValues = waitValues[i].data();
        t.signalSemaphoreValueCount = uint32_t(signalValues[i].size());
        t.pSignalSemaphoreValues = signalValues[i].data();
        auto &s = submits[i];
        s.pNext = &t;
        s.waitSemaphoreCount = uint32_t(waits[i].size());
        s.pWaitSemaphores = waits[i].data();
        s.pWaitDstStageMask = stages[i].data();
        s.signalSemaphoreCount = uint32_t(signals[i].size());
        s.pSignalSemaphores = signals[i].data();
        s.commandBufferCount = 1;
        s.pCommandBuffers = &graphSegments[i];
    }
    check(vkQueueSubmit(queueInfo().handle, uint32_t(n), submits.data(), fence), "submit graph segments");
    order.value += n;
}
} // namespace vulkano
