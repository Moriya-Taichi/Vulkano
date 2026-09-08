#pragma once
#include "tensors.hpp"

namespace vulkano {
struct GraphBinding {
    uint32_t index = 0, count = 1;
    TensorOptions tensor;
};
struct GraphConstant {
    uint32_t id = 0;
    TensorOptions tensor;
    std::vector<uint8_t> data;
};
struct GraphInterface {
    std::vector<BindingLayout> bindings;
    std::map<uint32_t, BindingLayout> constants;
    std::vector<std::string> operationSets;
    bool specialization = false;
};
struct GraphCacheMiss : std::runtime_error {
    GraphCacheMiss() : std::runtime_error("Graph pipeline is not available in this device's cache") {}
};
GraphInterface reflectGraph(Device &, const Shader &);
struct GraphPipeline : Resource {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    uint32_t family;
    std::vector<GraphBinding> bindings;
    GraphPipeline(std::shared_ptr<Device>, uint32_t queue, Shader, std::vector<GraphBinding>,
                  std::vector<GraphConstant>, std::string compilerOptions, bool optimize,
                  const std::vector<uint8_t> &identifier = {});
    std::vector<uint8_t> property(VkDataGraphPipelinePropertyARM) const;
    std::vector<VkDataGraphPipelinePropertyARM> availableProperties() const;
    ~GraphPipeline() override;
};
} // namespace vulkano
