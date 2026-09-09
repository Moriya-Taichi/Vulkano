#pragma once
#include "extensions.hpp"
#include <set>
namespace vulkano {
struct GeneratedToken {
    VkIndirectCommandsTokenTypeEXT type;
    uint32_t offset, target = 0, size = 0;
};
struct GeneratedLayout : Resource {
    VkIndirectCommandsLayoutEXT layout = VK_NULL_HANDLE;
    VkIndirectExecutionSetEXT executionSet = VK_NULL_HANDLE;
    std::vector<std::shared_ptr<Pipeline>> pipelines;
    uint32_t stride;
    VkIndirectCommandsTokenTypeEXT action;
    bool indexToken = false, countToken = false;
    std::set<uint32_t> vertexTokens;
    GeneratedLayout(std::shared_ptr<Device>, std::vector<std::shared_ptr<Pipeline>>,
                    const std::vector<GeneratedToken> &, uint32_t stride, bool unordered);
    ~GeneratedLayout() override;
};
// Each encoding gets distinct preprocess storage, as required by 11142.
struct GeneratedExecution : Resource {
    std::shared_ptr<GeneratedLayout> layout;
    std::shared_ptr<Buffer> arguments, count;
    VkBuffer preprocess = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkGeneratedCommandsInfoEXT info{VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT};
    VkGeneratedCommandsPipelineInfoEXT pipelineInfo{VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT};
    GeneratedExecution(std::shared_ptr<GeneratedLayout>, std::shared_ptr<Buffer>, VkDeviceSize offset,
                       uint32_t maxSequences, std::shared_ptr<Buffer> count, VkDeviceSize countOffset,
                       uint32_t maxDrawCount);
    void retain(Command &);
    void execute(Command &) const;
    ~GeneratedExecution() override;
};
void validateIndirectPipeline(const Pipeline &);
std::vector<uint64_t> generatedGraphicsKey(const Pipeline &);
} // namespace vulkano
