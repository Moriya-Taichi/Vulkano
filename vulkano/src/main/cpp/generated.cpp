#include "generated.hpp"
#include "ray.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace vulkano {
void validateIndirectPipeline(const Pipeline &p) {
    require(p.d->enabledExtra & DeviceGeneratedCommands, "Device generated commands feature was not enabled");
    require((p.stages & ~p.d->extensions->generatedProperties.supportedIndirectCommandsShaderStagesPipelineBinding) ==
                0,
            "Device cannot select pipelines for these shader stages");
    if (const auto *ray = dynamic_cast<const RayTracingPipeline *>(&p))
        require(!ray->supportsMotion || p.d->extensions->motion.rayTracingMotionBlurPipelineTraceRaysIndirect,
                "Device does not support indirect motion ray tracing");
}
std::vector<uint64_t> generatedGraphicsKey(const Pipeline &p) {
    const auto &g = p.graphics;
    auto bits = [](float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        return v;
    };
    std::vector<uint64_t> k{g.mesh,
                            g.tileShading,
                            g.tileApron.width,
                            g.tileApron.height,
                            g.subpass,
                            g.samples,
                            g.topology,
                            g.cull,
                            g.frontFace,
                            g.polygon,
                            g.depthWrite,
                            g.depthTest,
                            g.depthClamp,
                            g.alphaToCoverage,
                            g.depthCompare,
                            g.stencilTest,
                            g.patchPoints,
                            g.viewportCount,
                            g.viewMask,
                            g.sampleShading,
                            g.alphaToOne,
                            g.primitiveRestart,
                            g.rasterizationDisabled,
                            bits(g.minSampleShading),
                            g.sampleMask[0],
                            g.sampleMask[1],
                            g.logicEnabled,
                            g.logic,
                            g.depthBounds,
                            bits(g.minDepthBounds),
                            bits(g.maxDepthBounds),
                            g.fragmentSize.width,
                            g.fragmentSize.height,
                            g.rateMapTexelSize.width,
                            g.rateMapTexelSize.height,
                            g.attachmentRateCombiner,
                            g.primitiveRateCombiner,
                            p.depthFormat};
    for (const auto &s : {g.front, g.back})
        k.insert(k.end(), {s.failOp, s.passOp, s.depthFailOp, s.compareOp, s.compareMask, s.writeMask, s.reference});
    k.push_back(g.vertexBindings.size());
    for (const auto &v : g.vertexBindings)
        k.insert(k.end(), {v.binding, v.stride, v.inputRate});
    k.push_back(g.vertexDivisors.size());
    for (const auto &v : g.vertexDivisors)
        k.insert(k.end(), {v.binding, v.divisor});
    k.push_back(g.attributes.size());
    for (const auto &a : g.attributes)
        k.insert(k.end(), {a.location, a.binding, a.format, a.offset});
    k.push_back(g.colors.size());
    for (auto f : g.colors)
        k.push_back(f);
    for (const auto &b : g.blends)
        k.insert(k.end(), {b.blendEnable, b.srcColorBlendFactor, b.dstColorBlendFactor, b.colorBlendOp,
                           b.srcAlphaBlendFactor, b.dstAlphaBlendFactor, b.alphaBlendOp, b.colorWriteMask});
    k.push_back(bool(g.passLayout));
    if (g.passLayout)
        for (auto v : g.passLayout->key)
            k.push_back(uint32_t(v));
    return k;
}
namespace {
bool sameBinding(const BindingLayout &a, const BindingLayout &b) {
    return std::tie(a.set, a.binding, a.type, a.storageFormat, a.minimumBytes, a.count, a.stages, a.imageDim, a.arrayed,
                    a.multisampled, a.shadow, a.runtime, a.inputAttachmentIndex, a.numericType, a.immutableSampler,
                    a.tile, a.readonly, a.tensorRank, a.tensorDimensions) ==
           std::tie(b.set, b.binding, b.type, b.storageFormat, b.minimumBytes, b.count, b.stages, b.imageDim, b.arrayed,
                    b.multisampled, b.shadow, b.runtime, b.inputAttachmentIndex, b.numericType, b.immutableSampler,
                    b.tile, b.readonly, b.tensorRank, b.tensorDimensions);
}
void compatible(const Pipeline &a, const Pipeline &b) {
    require(a.owner() == b.owner() && a.compute == b.compute && a.rayTracing == b.rayTracing && a.stages == b.stages &&
                a.pushBytes == b.pushBytes && a.bindings.size() == b.bindings.size() &&
                std::equal(a.bindings.begin(), a.bindings.end(), b.bindings.begin(), sameBinding),
            "Generated pipelines require matching stages and binding/push constant layouts");
    require(a.generatedStateKey == b.generatedStateKey && a.fragmentInterface == b.fragmentInterface,
            "Generated pipelines require identical fixed state and fragment output interfaces");
}
void indirectRange(const Buffer &b, const Device &d, VkDeviceSize offset, VkDeviceSize bytes) {
    constexpr auto usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    require(b.owner() == &d && (b.usage & usage) == usage && offset % 4 == 0 && bytes > 0 && offset <= b.size &&
                bytes <= b.size - offset,
            "Generated commands need an aligned INDIRECT/DEVICE_ADDRESS buffer range on the same device");
}
} // namespace
GeneratedLayout::GeneratedLayout(std::shared_ptr<Device> device, std::vector<std::shared_ptr<Pipeline>> p,
                                 const std::vector<GeneratedToken> &tokens, uint32_t s, bool unordered)
    : Resource(std::move(device)), pipelines(std::move(p)), stride(s) {
    require(d->enabledExtra & DeviceGeneratedCommands, "Device generated commands feature was not enabled");
    const auto &limits = d->extensions->generatedProperties;
    require(!pipelines.empty() && pipelines.front() && pipelines.front()->owner() == d.get(),
            "Missing initial pipeline");
    const auto &base = *pipelines.front();
    if (base.graphics.mesh)
        require((d->enabled & (MeshShader | TaskShader)) == (MeshShader | TaskShader),
                "Generated mesh tokens require both mesh and task shader features");
    require(!base.tileShader || !base.compute, "Generated compute cannot execute tile shaders");
    require(!(base.stages & ~limits.supportedIndirectCommandsShaderStages), "Unsupported generated shader stages");
    require(!tokens.empty() && tokens.size() <= limits.maxIndirectCommandsTokenCount && stride > 0 && stride % 4 == 0 &&
                stride <= limits.maxIndirectCommandsIndirectStride,
            "Generated token count or stride exceeds device limits");
    action = tokens.back().type;
    const bool indexed = action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT ||
                         action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_COUNT_EXT;
    countToken = action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_COUNT_EXT ||
                 action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_COUNT_EXT ||
                 action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_COUNT_EXT;
    require(!countToken || limits.deviceGeneratedCommandsMultiDrawIndirectCount,
            "Generated multi-draw count is unsupported");
    bool executionToken = tokens.front().type == VK_INDIRECT_COMMANDS_TOKEN_TYPE_EXECUTION_SET_EXT;
    require(executionToken || pipelines.size() == 1, "Multiple pipelines require a pipeline-selection token");
    if (executionToken)
        require(pipelines.size() <= limits.maxIndirectPipelineCount, "Too many generated pipelines");
    for (const auto &pipeline : pipelines) {
        require(bool(pipeline), "Missing generated pipeline");
        compatible(base, *pipeline);
        if (const auto *ray = dynamic_cast<const RayTracingPipeline *>(pipeline.get()))
            require(!ray->supportsMotion || d->extensions->motion.rayTracingMotionBlurPipelineTraceRaysIndirect,
                    "Device does not support generated motion ray tracing");
        if (executionToken) {
            require(pipeline->indirectBindable, "Create pipelines with supportsIndirectCommands=true");
            validateIndirectPipeline(*pipeline);
        }
    }
    const bool primitive = !base.compute && !base.rayTracing && !base.graphics.mesh;
    const bool mesh = !base.compute && !base.rayTracing && base.graphics.mesh;
    require((base.compute && action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT) ||
                (base.rayTracing && action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_TRACE_RAYS2_EXT) ||
                (primitive && (indexed || action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT ||
                               action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_COUNT_EXT)) ||
                (mesh && (action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_EXT ||
                          action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_COUNT_EXT)),
            "Generated action does not match pipeline type");
    std::vector<VkIndirectCommandsLayoutTokenEXT> vkTokens(tokens.size());
    std::vector<VkIndirectCommandsPushConstantTokenEXT> pushes(tokens.size());
    std::vector<VkIndirectCommandsVertexBufferTokenEXT> vertices(tokens.size());
    VkIndirectCommandsIndexBufferTokenEXT index{VK_INDIRECT_COMMANDS_INPUT_MODE_VULKAN_INDEX_BUFFER_EXT};
    VkIndirectCommandsExecutionSetTokenEXT execution{VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT, base.stages};
    std::vector<std::pair<uint32_t, uint32_t>> pushRanges;
    bool sequenceIndex = false;
    for (size_t n = 0; n < tokens.size(); ++n) {
        const auto &t = tokens[n];
        require(t.offset % 4 == 0 && t.offset <= limits.maxIndirectCommandsTokenOffset &&
                    (!n || t.offset >= tokens[n - 1].offset),
                "Invalid generated token offset/order");
        auto &v = vkTokens[n];
        v = {VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT};
        v.type = t.type;
        v.offset = t.offset;
        uint32_t bytes = 0;
        switch (t.type) {
        case VK_INDIRECT_COMMANDS_TOKEN_TYPE_EXECUTION_SET_EXT:
            require(n == 0, "Pipeline token must be first");
            bytes = 4;
            v.data.pExecutionSet = &execution;
            break;
        case VK_INDIRECT_COMMANDS_TOKEN_TYPE_SEQUENCE_INDEX_EXT:
        case VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT:
            if (t.type == VK_INDIRECT_COMMANDS_TOKEN_TYPE_SEQUENCE_INDEX_EXT) {
                require(!sequenceIndex && t.size == 4, "Sequence index requires one four-byte push range");
                sequenceIndex = true;
            } else
                bytes = t.size;
            require(t.target % 4 == 0 && t.size > 0 && t.size % 4 == 0 && t.target <= base.pushBytes &&
                        t.size <= base.pushBytes - t.target,
                    "Generated push range exceeds pipeline layout");
            for (auto range : pushRanges)
                require(t.target >= range.second || t.target + t.size <= range.first, "Generated push ranges overlap");
            pushRanges.push_back({t.target, t.target + t.size});
            pushes[n].updateRange = {base.stages, t.target, t.size};
            v.data.pPushConstant = &pushes[n];
            break;
        case VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT:
            require(indexed && !indexToken && (limits.supportedIndirectCommandsInputModes & index.mode),
                    "Invalid or unsupported generated index token");
            indexToken = true;
            bytes = sizeof(VkBindIndexBufferIndirectCommandEXT);
            v.data.pIndexBuffer = &index;
            break;
        case VK_INDIRECT_COMMANDS_TOKEN_TYPE_VERTEX_BUFFER_EXT:
            require(primitive && d->extensions->generatedVertexInput && vertexTokens.insert(t.target).second &&
                        t.target < base.graphics.vertexBindings.size() &&
                        std::any_of(base.graphics.vertexBindings.begin(), base.graphics.vertexBindings.end(),
                                    [&](const auto &b) { return b.binding == t.target; }),
                    "Generated vertex binding is unsupported, absent or duplicated");
            vertices[n].vertexBindingUnit = t.target;
            v.data.pVertexBuffer = &vertices[n];
            bytes = sizeof(VkBindVertexBufferIndirectCommandEXT);
            break;
        default:
            require(n + 1 == tokens.size(), "Exactly one action token is required, at the end");
            bytes = countToken             ? sizeof(VkDrawIndirectCountIndirectCommandEXT)
                    : indexed              ? sizeof(VkDrawIndexedIndirectCommand)
                    : base.compute || mesh ? sizeof(VkDispatchIndirectCommand)
                    : base.rayTracing      ? sizeof(VkTraceRaysIndirectCommand2KHR)
                                           : sizeof(VkDrawIndirectCommand);
        }
        require(n + 1 < tokens.size() || bytes > 0, "Layout ends with a state token");
        require(t.offset <= stride && bytes <= stride - t.offset, "Token data exceeds sequence stride");
    }
    try {
        if (executionToken) {
            VkIndirectExecutionSetPipelineInfoEXT pi{VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_PIPELINE_INFO_EXT};
            pi.initialPipeline = base.pipeline;
            pi.maxPipelineCount = uint32_t(pipelines.size());
            VkIndirectExecutionSetCreateInfoEXT ci{VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT};
            ci.type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT;
            ci.info.pPipelineInfo = &pi;
            check(d->extensions->createExecutionSet(d->device, &ci, nullptr, &executionSet),
                  "vkCreateIndirectExecutionSetEXT");
            std::vector<VkWriteIndirectExecutionSetPipelineEXT> writes;
            for (uint32_t n = 1; n < pipelines.size(); ++n)
                writes.push_back(
                    {VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_PIPELINE_EXT, nullptr, n, pipelines[n]->pipeline});
            if (!writes.empty())
                d->extensions->updateExecutionSet(d->device, executionSet, uint32_t(writes.size()), writes.data());
        }
        VkIndirectCommandsLayoutCreateInfoEXT ci{VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT};
        ci.flags = unordered ? VK_INDIRECT_COMMANDS_LAYOUT_USAGE_UNORDERED_SEQUENCES_BIT_EXT : 0;
        ci.shaderStages = base.stages;
        ci.indirectStride = stride;
        ci.pipelineLayout = base.layout;
        ci.tokenCount = uint32_t(vkTokens.size());
        ci.pTokens = vkTokens.data();
        check(d->extensions->createGeneratedLayout(d->device, &ci, nullptr, &layout),
              "vkCreateIndirectCommandsLayoutEXT");
    } catch (...) {
        if (executionSet)
            d->extensions->destroyExecutionSet(d->device, executionSet, nullptr);
        throw;
    }
}
GeneratedLayout::~GeneratedLayout() {
    if (layout)
        d->extensions->destroyGeneratedLayout(d->device, layout, nullptr);
    if (executionSet)
        d->extensions->destroyExecutionSet(d->device, executionSet, nullptr);
}
GeneratedExecution::GeneratedExecution(std::shared_ptr<GeneratedLayout> l, std::shared_ptr<Buffer> a,
                                       VkDeviceSize offset, uint32_t maxSequences, std::shared_ptr<Buffer> c,
                                       VkDeviceSize countOffset, uint32_t maxDrawCount)
    : Resource(l->d), layout(std::move(l)), arguments(std::move(a)), count(std::move(c)) {
    require(maxSequences > 0 && maxSequences <= d->extensions->generatedProperties.maxIndirectSequenceCount,
            "Generated sequence count exceeds device limit");
    require(!layout->countToken || (maxDrawCount > 0 && uint64_t(maxDrawCount) * maxSequences < (1u << 24) &&
                                    maxDrawCount <= d->properties.limits.maxDrawIndirectCount),
            "Generated draw count exceeds limits");
    require(bool(arguments), "Missing generated arguments");
    const auto bytes = uint64_t(maxSequences) * layout->stride;
    indirectRange(*arguments, *d, offset, bytes);
    if (count)
        indirectRange(*count, *d, countOffset, 4);
    pipelineInfo.pipeline = layout->pipelines.front()->pipeline;
    info.pNext = layout->executionSet ? nullptr : &pipelineInfo;
    info.shaderStages = layout->pipelines.front()->stages;
    info.indirectExecutionSet = layout->executionSet;
    info.indirectCommandsLayout = layout->layout;
    info.indirectAddress = bufferAddress(*arguments) + offset;
    info.indirectAddressSize = bytes;
    info.maxSequenceCount = maxSequences;
    info.maxDrawCount = maxDrawCount;
    info.sequenceCountAddress = count ? bufferAddress(*count) + countOffset : 0;
    VkGeneratedCommandsMemoryRequirementsInfoEXT query{
        VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT};
    query.pNext = info.pNext;
    query.indirectExecutionSet = layout->executionSet;
    query.indirectCommandsLayout = layout->layout;
    query.maxSequenceCount = maxSequences;
    query.maxDrawCount = maxDrawCount;
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    d->extensions->generatedMemoryRequirements(d->device, &query, &requirements);
    const auto &m = requirements.memoryRequirements;
    if (m.size) {
        require(m.alignment > 0 && m.size <= std::numeric_limits<uint64_t>::max() - m.alignment,
                "Invalid preprocess allocation size");
        VkBufferUsageFlags2CreateInfo usage{VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO};
        usage.usage = VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        d->share(ci);
        ci.pNext = &usage;
        ci.size = m.size + m.alignment - 1;
        ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        ai.memoryTypeBits = m.memoryTypeBits;
        check(vmaCreateBuffer(d->allocator, &ci, &ai, &preprocess, &allocation, nullptr),
              "allocate generated preprocess buffer");
        VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        address.buffer = preprocess;
        auto base = d->extensions->getBufferAddress(d->device, &address);
        info.preprocessAddress = (base + m.alignment - 1) & ~(m.alignment - 1);
        info.preprocessSize = m.size;
    }
}
void GeneratedExecution::retain(Command &c) {
    require(c.owner() == owner(), "Generated commands belong to another device");
    c.buffers.push_back(arguments);
    if (count)
        c.buffers.push_back(count);
    for (const auto &p : layout->pipelines)
        if (auto ray = std::dynamic_pointer_cast<RayTracingPipeline>(p))
            c.buffers.push_back(ray->table);
}
void GeneratedExecution::execute(Command &c) const {
    d->extensions->executeGenerated(c.command, VK_FALSE, &info);
    // Execution-set tokens invalidate pipeline state. Other state is rebound on each draw/dispatch.
    if (layout->executionSet)
        c.boundPipelines.fill(VK_NULL_HANDLE);
}
GeneratedExecution::~GeneratedExecution() {
    if (preprocess)
        vmaDestroyBuffer(d->allocator, preprocess, allocation);
}
} // namespace vulkano
