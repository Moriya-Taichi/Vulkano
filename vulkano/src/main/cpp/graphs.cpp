#include "graphs.hpp"
#include <algorithm>
#include <set>

namespace vulkano {
namespace {
void matches(const TensorOptions &tensor, const BindingLayout &schema) {
    require(tensor.format == schema.storageFormat, "Graph tensor element type differs from SPIR-V");
    require(!schema.tensorRank || tensor.dimensions.size() == schema.tensorRank,
            "Graph tensor rank differs from SPIR-V");
    require(schema.tensorDimensions.empty() || tensor.dimensions == schema.tensorDimensions,
            "Graph tensor shape differs from SPIR-V");
}
std::vector<int64_t> strides(const TensorOptions &tensor) {
    if (!tensor.strides.empty())
        return tensor.strides;
    std::vector<int64_t> result(tensor.dimensions.size());
    int64_t stride = tensorElementSize(tensor.format);
    for (size_t i = result.size(); i-- > 0;) {
        result[i] = stride;
        if (i)
            stride *= tensor.dimensions[i];
    }
    return result;
}
void destroyPipeline(GraphPipeline &p) {
    if (p.pipeline)
        vkDestroyPipeline(p.d->device, p.pipeline, nullptr);
    if (p.layout)
        vkDestroyPipelineLayout(p.d->device, p.layout, nullptr);
    for (auto set : p.setLayouts)
        vkDestroyDescriptorSetLayout(p.d->device, set, nullptr);
}
} // namespace

GraphPipeline::GraphPipeline(std::shared_ptr<Device> device, uint32_t queue, Shader shader,
                             std::vector<GraphBinding> resources, std::vector<GraphConstant> constants,
                             std::string options, bool optimize, const std::vector<uint8_t> &identifier)
    : Resource(std::move(device)), family(0), bindings(std::move(resources)) {
    require(d->enabledExtra & DataGraph, "Machine learning graph feature was not enabled");
    require(queue < d->queues.size(), "Invalid graph queue index");
    family = d->queues[queue].family;
    const auto &extension = *d->extensions;
    require(extension.graphQueues.count(family), "Queue has no supported GPU graph engine");
    require(options.find('\0') == std::string::npos, "Graph compiler options contain NUL");
    GraphInterface interface;
    if (identifier.empty()) {
        require(extension.graph.dataGraphShaderModule, "This device cannot compile graph SPIR-V");
        interface = reflectGraph(*d, shader);
        require(extension.graph.dataGraphSpecializationConstants ||
                    (!interface.specialization && shader.constants.empty()),
                "Graph function constants are unsupported");
        for (const auto &name : interface.operationSets) {
            const auto &operations = extension.graphQueues.at(family);
            require(
                std::any_of(
                    operations.begin(), operations.end(),
                    [&](const auto &op) {
                        return op.operation.operationType ==
                                   VK_PHYSICAL_DEVICE_DATA_GRAPH_OPERATION_TYPE_SPIRV_EXTENDED_INSTRUCTION_SET_ARM &&
                               name == op.operation.name;
                    }),
                "Graph imports an instruction set unsupported by this queue");
        }
        require(bindings.size() == interface.bindings.size(), "Describe every graph tensor interface");
    } else {
        require(extension.cacheControl.pipelineCreationCacheControl, "Graph cache loading is unsupported");
        require(constants.empty() && shader.constants.empty(), "Cached graphs already contain their constants");
    }
    uint64_t descriptorCount = 0;
    std::set<uint64_t> used;
    uint32_t setCount = 1;
    std::vector<VkDescriptorSetLayoutBinding> layouts;
    std::vector<VkTensorDescriptionARM> descriptions(bindings.size());
    std::vector<VkDataGraphPipelineResourceInfoARM> infos(bindings.size(),
                                                          {VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_RESOURCE_INFO_ARM});
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto &b = bindings[i];
        require(b.count && used.insert(b.location()).second, "Graph bindings must be unique and non-empty");
        require(b.set < d->properties.limits.maxBoundDescriptorSets, "Graph descriptor set exceeds device limit");
        setCount = std::max(setCount, b.set + 1);
        require(b.tensor.usage & VK_TENSOR_USAGE_DATA_GRAPH_BIT_ARM,
                "Graph tensor description requires MACHINE_LEARNING usage");
        b.tensor.validate(*d);
        descriptorCount += b.count;
        require(descriptorCount <= extension.tensorProperties.maxDescriptorSetStorageTensors,
                "Graph tensor descriptors exceed limits");
        if (identifier.empty()) {
            auto schema = std::find_if(interface.bindings.begin(), interface.bindings.end(),
                                       [&](const auto &s) { return s.location() == b.location(); });
            require(schema != interface.bindings.end() && schema->count == b.count,
                    "Graph binding or array count differs from SPIR-V");
            matches(b.tensor, *schema);
        }
        // Data graph pipelines have their own bind point and no shader stage bit.
        layouts.push_back({b.index, VK_DESCRIPTOR_TYPE_TENSOR_ARM, b.count, 0, nullptr});
        descriptions[i] = b.tensor.description();
        infos[i].pNext = &descriptions[i];
        infos[i].binding = b.index;
        infos[i].descriptorSet = b.set;
    }
    std::vector<VkTensorDescriptionARM> constantDescriptions(constants.size());
    std::vector<VkDataGraphPipelineConstantARM> constantInfos(constants.size(),
                                                              {VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_CONSTANT_ARM});
    used.clear();
    require(identifier.size() <= UINT32_MAX, "Graph identifier is too large");
    if (identifier.empty())
        require(constants.size() == interface.constants.size(), "Supply every graph constant");
    for (size_t i = 0; i < constants.size(); ++i) {
        const auto &c = constants[i];
        require(used.insert(c.id).second && interface.constants.count(c.id), "Unknown or duplicate graph constant ID");
        require(c.tensor.tiling == VK_TENSOR_TILING_LINEAR_ARM && (c.tensor.usage & VK_TENSOR_USAGE_DATA_GRAPH_BIT_ARM),
                "Graph constants require linear tensor layout and MACHINE_LEARNING usage");
        require(c.data.size() == c.tensor.validate(*d, false),
                "Graph constant byte size differs from its tensor layout");
        matches(c.tensor, interface.constants.at(c.id));
        constantDescriptions[i] = c.tensor.description();
        constantInfos[i].pNext = &constantDescriptions[i];
        constantInfos[i].id = c.id;
        constantInfos[i].pConstantData = c.data.data();
    }
    try {
        setLayouts.resize(setCount, VK_NULL_HANDLE);
        for (uint32_t setIndex = 0; setIndex < setCount; ++setIndex) {
            std::vector<VkDescriptorSetLayoutBinding> entries;
            for (size_t i = 0; i < bindings.size(); ++i)
                if (bindings[i].set == setIndex) entries.push_back(layouts[i]);
            VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            set.bindingCount = uint32_t(entries.size());
            set.pBindings = entries.data();
            check(vkCreateDescriptorSetLayout(d->device, &set, nullptr, &setLayouts[setIndex]), "create graph descriptor layout");
        }
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = uint32_t(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();
        check(vkCreatePipelineLayout(d->device, &layoutInfo, nullptr, &layout), "create graph pipeline layout");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = shader.code.size() * 4;
        module.pCode = shader.code.data();
        SpecializationData specializationData(shader);
        const auto specialization = specializationData.info();
        VkDataGraphPipelineShaderModuleCreateInfoARM source{
            VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SHADER_MODULE_CREATE_INFO_ARM, &module};
        source.pName = shader.entry.c_str();
        source.pSpecializationInfo = specializationData.entries.empty() ? nullptr : &specialization;
        source.constantCount = uint32_t(constantInfos.size());
        source.pConstants = constantInfos.data();
        VkDataGraphPipelineIdentifierCreateInfoARM cached{
            VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_IDENTIFIER_CREATE_INFO_ARM};
        cached.identifierSize = uint32_t(identifier.size());
        cached.pIdentifier = identifier.data();
        VkDataGraphPipelineCompilerControlCreateInfoARM compiler{
            VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_COMPILER_CONTROL_CREATE_INFO_ARM};
        compiler.pVendorOptions = options.c_str();
        compiler.pNext = identifier.empty() ? static_cast<const void *>(&source) : &cached;
        VkDataGraphPipelineCreateInfoARM create{VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_CREATE_INFO_ARM, &compiler};
        create.layout = layout;
        create.flags = optimize ? 0 : VK_PIPELINE_CREATE_2_DISABLE_OPTIMIZATION_BIT;
        if (identifier.empty()) {
            create.resourceInfoCount = uint32_t(infos.size());
            create.pResourceInfos = infos.data();
        } else
            create.flags |= VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        const auto result =
            extension.createGraphPipelines(d->device, VK_NULL_HANDLE, d->pipelineCache, 1, &create, nullptr, &pipeline);
        if (!identifier.empty() && result == VK_PIPELINE_COMPILE_REQUIRED)
            throw GraphCacheMiss();
        check(result, "create machine learning graph pipeline");
    } catch (...) {
        destroyPipeline(*this);
        throw;
    }
}
GraphPipeline::~GraphPipeline() { destroyPipeline(*this); }

std::vector<VkDataGraphPipelinePropertyARM> GraphPipeline::availableProperties() const {
    VkDataGraphPipelineInfoARM info{VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_INFO_ARM};
    info.dataGraphPipeline = pipeline;
    uint32_t count = 0;
    check(d->extensions->graphAvailableProperties(d->device, &info, &count, nullptr), "query graph properties");
    std::vector<VkDataGraphPipelinePropertyARM> available(count);
    check(d->extensions->graphAvailableProperties(d->device, &info, &count, available.data()),
          "query graph property list");
    available.resize(count);
    return available;
}
std::vector<uint8_t> GraphPipeline::property(VkDataGraphPipelinePropertyARM property) const {
    auto available = availableProperties();
    require(std::find(available.begin(), available.end(), property) != available.end(),
            "Graph property is unavailable");
    VkDataGraphPipelineInfoARM info{VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_INFO_ARM};
    info.dataGraphPipeline = pipeline;
    VkDataGraphPipelinePropertyQueryResultARM result{VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_PROPERTY_QUERY_RESULT_ARM};
    result.property = property;
    check(d->extensions->graphProperties(d->device, &info, 1, &result), "query graph property size");
    std::vector<uint8_t> bytes(result.dataSize);
    result.pData = bytes.data();
    check(d->extensions->graphProperties(d->device, &info, 1, &result), "read graph property");
    bytes.resize(result.dataSize);
    return bytes;
}

namespace {
struct GraphExecution {
    std::shared_ptr<GraphPipeline> pipeline;
    std::vector<Binding> bindings;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;
    VkDataGraphPipelineSessionARM session = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> memory;
    ~GraphExecution() {
        auto &d = *pipeline->d;
        if (pool)
            vkDestroyDescriptorPool(d.device, pool, nullptr);
        if (session)
            d.extensions->destroyGraphSession(d.device, session, nullptr);
        for (auto m : memory)
            vkFreeMemory(d.device, m, nullptr);
    }
    void initialize() {
        auto &d = *pipeline->d;
        VkDataGraphPipelineSessionCreateInfoARM create{VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SESSION_CREATE_INFO_ARM};
        create.dataGraphPipeline = pipeline->pipeline;
        check(d.extensions->createGraphSession(d.device, &create, nullptr, &session), "create graph execution session");
        VkDataGraphPipelineSessionBindPointRequirementsInfoARM query{
            VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SESSION_BIND_POINT_REQUIREMENTS_INFO_ARM};
        query.session = session;
        uint32_t count = 0;
        check(d.extensions->graphBindRequirements(d.device, &query, &count, nullptr),
              "query graph session bind points");
        std::vector<VkDataGraphPipelineSessionBindPointRequirementARM> points(
            count, {VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SESSION_BIND_POINT_REQUIREMENT_ARM});
        check(d.extensions->graphBindRequirements(d.device, &query, &count, points.data()),
              "query graph session requirements");
        for (const auto &point : points) {
            require(point.bindPointType == VK_DATA_GRAPH_PIPELINE_SESSION_BIND_POINT_TYPE_MEMORY_ARM,
                    "Unknown graph session binding type");
            for (uint32_t i = 0; i < point.numObjects; ++i) {
                VkDataGraphPipelineSessionMemoryRequirementsInfoARM request{
                    VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SESSION_MEMORY_REQUIREMENTS_INFO_ARM};
                request.session = session;
                request.bindPoint = point.bindPoint;
                request.objectIndex = i;
                VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
                d.extensions->graphMemoryRequirements(d.device, &request, &requirements);
                const auto &r = requirements.memoryRequirements;
                uint32_t selected = UINT32_MAX;
                for (uint32_t k = 0; k < d.memory.memoryTypeCount; ++k) {
                    const auto flags = d.memory.memoryTypes[k].propertyFlags;
                    if ((r.memoryTypeBits & (1u << k)) &&
                        !(flags & (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT))) {
                        if (selected == UINT32_MAX)
                            selected = k;
                        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                            selected = k;
                            break;
                        }
                    }
                }
                require(selected != UINT32_MAX, "No compatible graph session memory");
                VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocation.allocationSize = std::max<VkDeviceSize>(1, r.size);
                allocation.memoryTypeIndex = selected;
                memory.reserve(memory.size() + 1);
                VkDeviceMemory allocated;
                check(vkAllocateMemory(d.device, &allocation, nullptr, &allocated), "allocate graph session memory");
                memory.push_back(allocated);
                VkBindDataGraphPipelineSessionMemoryInfoARM bind{
                    VK_STRUCTURE_TYPE_BIND_DATA_GRAPH_PIPELINE_SESSION_MEMORY_INFO_ARM};
                bind.session = session;
                bind.bindPoint = point.bindPoint;
                bind.objectIndex = i;
                bind.memory = allocated;
                check(d.extensions->bindGraphMemory(d.device, 1, &bind), "bind graph session memory");
            }
        }
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_TENSOR_ARM, uint32_t(bindings.size())};
        VkDescriptorPoolCreateInfo descriptorPool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorPool.maxSets = uint32_t(pipeline->setLayouts.size());
        descriptorPool.poolSizeCount = bindings.empty() ? 0 : 1;
        descriptorPool.pPoolSizes = bindings.empty() ? nullptr : &size;
        check(vkCreateDescriptorPool(d.device, &descriptorPool, nullptr, &pool), "create graph descriptor pool");
        VkDescriptorSetAllocateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        descriptor.descriptorPool = pool;
        descriptor.descriptorSetCount = uint32_t(pipeline->setLayouts.size());
        sets.resize(descriptor.descriptorSetCount);
        descriptor.pSetLayouts = pipeline->setLayouts.data();
        check(vkAllocateDescriptorSets(d.device, &descriptor, sets.data()), "allocate graph descriptor set");
        std::vector<VkWriteDescriptorSetTensorARM> tensors(bindings.size(),
                                                           {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_TENSOR_ARM});
        std::vector<VkWriteDescriptorSet> writes(bindings.size(), {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
        for (size_t i = 0; i < bindings.size(); ++i) {
            tensors[i].tensorViewCount = 1;
            tensors[i].pTensorViews = &bindings[i].tensor->view;
            writes[i].pNext = &tensors[i];
            writes[i].dstSet = sets[bindings[i].set];
            writes[i].dstBinding = bindings[i].index;
            writes[i].dstArrayElement = bindings[i].element;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_TENSOR_ARM;
            writes[i].descriptorCount = 1;
        }
        vkUpdateDescriptorSets(d.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    }
};
} // namespace
void Command::dispatchGraph(std::shared_ptr<GraphPipeline> pipeline, std::vector<Binding> bindings) {
    recording();
    requireQueue(VK_QUEUE_DATA_GRAPH_BIT_ARM);
    require(pipeline && pipeline->d == d && pipeline->family == queueInfo().family,
            "Graph pipeline belongs to another device or queue family");
    uint64_t count = 0;
    for (const auto &b : pipeline->bindings)
        count += b.count;
    require(count == bindings.size(), "Bind every graph tensor and array element");
    std::set<std::pair<uint64_t, uint32_t>> used;
    for (const auto &b : bindings) {
        require(b.tensor && b.tensor->d == d && !b.buffer && !b.texture && !b.sampler && !b.acceleration && !b.texel,
                "Graph bindings require tensor views from this device");
        require(used.emplace(b.location(), b.element).second, "Duplicate graph tensor binding");
        auto expected = std::find_if(pipeline->bindings.begin(), pipeline->bindings.end(),
                                     [&](const auto &s) { return s.location() == b.location(); });
        require(expected != pipeline->bindings.end() && b.element < expected->count,
                "Unknown graph binding or array element");
        const auto &actual = b.tensor->tensor->options;
        const auto &schema = expected->tensor;
        require((actual.usage & VK_TENSOR_USAGE_DATA_GRAPH_BIT_ARM) && b.tensor->format == schema.format &&
                    actual.dimensions == schema.dimensions && actual.tiling == schema.tiling &&
                    strides(actual) == strides(schema),
                "Graph tensor type, shape, layout, or usage differs from the compiled pipeline");
    }
    auto execution = std::make_shared<GraphExecution>();
    execution->pipeline = std::move(pipeline);
    execution->bindings = std::move(bindings);
    execution->initialize();
    for (const auto &b : execution->bindings)
        tensors.push_back(b.tensor->tensor);
    operations.push_back([execution](Command &c) {
        c.barrier();
        vkCmdBindPipeline(c.command, VK_PIPELINE_BIND_POINT_DATA_GRAPH_ARM, execution->pipeline->pipeline);
        vkCmdBindDescriptorSets(c.command, VK_PIPELINE_BIND_POINT_DATA_GRAPH_ARM, execution->pipeline->layout, 0, uint32_t(execution->sets.size()),
                                execution->sets.data(), 0, nullptr);
        c.d->extensions->dispatchGraph(c.command, execution->session, nullptr);
    });
}
} // namespace vulkano
