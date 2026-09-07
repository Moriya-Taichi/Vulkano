#include "engine.hpp"
#include "spirv-reflect/spirv_reflect.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

namespace vulkano {
void require(bool condition, const char* message) { if (!condition) throw std::invalid_argument(message); }
void check(VkResult r, const char* op) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(op) + " failed (VkResult " + std::to_string(r) + ")");
}
namespace {
bool extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [name](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}
void same(const Resource& a, const Resource& b) { require(a.owner() == b.owner(), "Resources belong to different devices"); }
void range(VkDeviceSize capacity, VkDeviceSize offset, VkDeviceSize count) {
    require(count > 0 && offset <= capacity && count <= capacity - offset, "Buffer range is out of bounds");
}
VkImageView makeView(Device& d, VkImage image, VkFormat format) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image; info.viewType = VK_IMAGE_VIEW_TYPE_2D; info.format = format;
    info.subresourceRange = {format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    check(vkCreateImageView(d.device, &info, nullptr, &view), "vkCreateImageView");
    return view;
}
VkRenderPass makePass(Device& d, VkFormat color, VkFormat depth,
                      VkAttachmentLoadOp load, VkAttachmentStoreOp store,
                      VkAttachmentLoadOp depthLoad, VkAttachmentStoreOp depthStore) {
    const std::array<int, 6> key{color, depth, load, store, depthLoad, depthStore};
    if (auto it = d.renderPassCache.find(key); it != d.renderPassCache.end()) return it->second;
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = color;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = load; attachments[0].storeOp = store;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1] = attachments[0]; attachments[1].format = depth;
    attachments[1].loadOp = depthLoad; attachments[1].storeOp = depthStore;
    attachments[1].initialLayout = attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &depthRef : nullptr;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = depth != VK_FORMAT_UNDEFINED ? 2 : 1;
    info.pAttachments = attachments; info.subpassCount = 1; info.pSubpasses = &subpass;
    VkRenderPass pass;
    check(vkCreateRenderPass(d.device, &info, nullptr, &pass), "vkCreateRenderPass");
    try { d.renderPassCache.emplace(key, pass); }
    catch (...) { vkDestroyRenderPass(d.device, pass, nullptr); throw; }
    return pass;
}
struct Module {
    Device& d; VkShaderModule module = VK_NULL_HANDLE;
    uint32_t entryId = 0;
    std::vector<BindingLayout> reflectedBindings;
    uint32_t reflectedPushBytes = 0;
    std::array<uint32_t, 3> local{0, 0, 0};
    Module(Device& device, const Shader& shader, uint32_t executionModel) : d(device) {
        const auto& code = shader.code;
        require(code.size() >= 5 && code[0] == 0x07230203 && code[1] >= 0x00010000 && code[1] <= 0x00010300 && code[4] == 0,
                "Expected SPIR-V 1.0-1.3 compiled for Vulkan 1.1");
        for (size_t i = 5; i < code.size();) {
            uint32_t words = code[i] >> 16, op = code[i] & 0xffff;
            require(words > 0 && words <= code.size() - i, "Malformed SPIR-V instruction");
            if (op == SpvOpMemoryModel) require(words == 3 && code[i + 1] == SpvAddressingModelLogical && code[i + 2] == SpvMemoryModelGLSL450, "Shaders must use the Logical / GLSL450 memory model");
            if (op == 15 && words >= 4 && code[i + 1] == executionModel) {
                const auto* name = reinterpret_cast<const char*>(&code[i + 3]);
                const auto* end = static_cast<const char*>(std::memchr(name, 0, (words - 3) * 4));
                require(end != nullptr, "Malformed SPIR-V entry point");
                if (std::string(name, end) == shader.entry) entryId = code[i + 2];
            }
            i += words;
        }
        require(entryId != 0, "Shader entry point not found for the requested stage");
        if (executionModel == 5) {
            for (size_t i = 5; i < code.size(); i += code[i] >> 16) {
                if ((code[i] & 0xffff) == 16 && (code[i] >> 16) == 6 && code[i + 1] == entryId && code[i + 2] == 17)
                    local = {code[i + 3], code[i + 4], code[i + 5]};
            }
            uint64_t total = 1;
            for (int i = 0; i < 3; ++i) {
                require(local[i] > 0 && local[i] <= d.properties.limits.maxComputeWorkGroupSize[i], "Unsupported shader workgroup size (use literal local_size)");
                require(total <= d.properties.limits.maxComputeWorkGroupInvocations / local[i], "Shader workgroup exceeds maxComputeWorkGroupInvocations");
                total *= local[i];
            }
            require(total <= d.properties.limits.maxComputeWorkGroupInvocations, "Shader workgroup exceeds maxComputeWorkGroupInvocations");
        }
        SpvReflectShaderModule reflection{};
        require(spvReflectCreateShaderModule(code.size() * 4, code.data(), &reflection) == SPV_REFLECT_RESULT_SUCCESS, "SPIR-V reflection failed");
        struct ReflectionGuard {
            SpvReflectShaderModule& module;
            ~ReflectionGuard() { spvReflectDestroyShaderModule(&module); }
        } reflectionGuard{reflection};
        const auto* reflectedEntry = spvReflectGetEntryPoint(&reflection, shader.entry.c_str());
        require(reflectedEntry && reflectedEntry->spirv_execution_model == static_cast<SpvExecutionModel>(executionModel), "Invalid shader stage");
        require(reflection.spec_constant_count == 0, "Specialization constants are not exposed in this release");
        if (executionModel == 0) for (uint32_t i = 0; i < reflectedEntry->input_variable_count; ++i)
            require((reflectedEntry->input_variables[i]->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0, "Use vertex pulling; vertex attribute layouts are not exposed");
        bool hasSubgroups = false, hasSmallArithmetic = false;
        for (uint32_t i = 0; i < reflection.capability_count; ++i) {
            const auto cap = reflection.capabilities[i].value;
            uint32_t requiredFeature = 0;
            VkSubgroupFeatureFlags subgroupOperation = 0;
            switch (cap) {
                case SpvCapabilityMatrix: case SpvCapabilityShader: case SpvCapabilityImageQuery: case SpvCapabilityDerivativeControl: break;
                case SpvCapabilityFloat16: requiredFeature = Float16; hasSmallArithmetic = true; break;
                case SpvCapabilityInt16: requiredFeature = Int16; hasSmallArithmetic = true; break;
                case SpvCapabilityStorageBuffer16BitAccess: requiredFeature = Storage16; break;
                case SpvCapabilityStorageImageExtendedFormats: requiredFeature = ExtendedStorageFormats; break;
                case SpvCapabilityGroupNonUniform: subgroupOperation = VK_SUBGROUP_FEATURE_BASIC_BIT; break;
                case SpvCapabilityGroupNonUniformVote: subgroupOperation = VK_SUBGROUP_FEATURE_VOTE_BIT; break;
                case SpvCapabilityGroupNonUniformArithmetic: subgroupOperation = VK_SUBGROUP_FEATURE_ARITHMETIC_BIT; break;
                case SpvCapabilityGroupNonUniformBallot: subgroupOperation = VK_SUBGROUP_FEATURE_BALLOT_BIT; break;
                case SpvCapabilityGroupNonUniformShuffle: subgroupOperation = VK_SUBGROUP_FEATURE_SHUFFLE_BIT; break;
                case SpvCapabilityGroupNonUniformShuffleRelative: subgroupOperation = VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT; break;
                case SpvCapabilityGroupNonUniformClustered: subgroupOperation = VK_SUBGROUP_FEATURE_CLUSTERED_BIT; break;
                case SpvCapabilityGroupNonUniformQuad:
                    require(executionModel == 4 || executionModel == 5 || d.subgroup.quadOperationsInAllStages, "Subgroup quad operations unavailable in this stage");
                    subgroupOperation = VK_SUBGROUP_FEATURE_QUAD_BIT; break;
                default: throw std::invalid_argument("Unsupported SPIR-V capability: " + std::to_string(cap));
            }
            require(!requiredFeature || (d.enabled & requiredFeature), "Shader requires a feature that was not enabled at device creation");
            if (subgroupOperation) {
                hasSubgroups = true;
                require((d.subgroup.supportedStages & reflectedEntry->shader_stage) && (d.subgroup.supportedOperations & subgroupOperation), "Unsupported subgroup stage/operation");
            }
        }
        require(!(hasSubgroups && hasSmallArithmetic), "16-bit arithmetic with subgroups requires extended-type support, which is not enabled");
        for (uint32_t set = 0; set < reflectedEntry->descriptor_set_count; ++set) {
            const auto& descriptors = reflectedEntry->descriptor_sets[set];
            require(descriptors.set == 0, "Only descriptor set 0 is exposed");
            for (uint32_t i = 0; i < descriptors.binding_count; ++i) {
                const auto& b = *descriptors.bindings[i];
                require(b.count == 1 && b.array.dims_count == 0, "Descriptor arrays are not exposed");
                BindingLayout binding{b.binding, static_cast<VkDescriptorType>(b.descriptor_type)};
                if (binding.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || binding.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                    binding.minimumBytes = b.block.size;
                    if (executionModel != 5 && binding.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                        bool readonly = (b.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0;
                        if (!readonly && b.block.member_count) readonly = std::all_of(b.block.members, b.block.members + b.block.member_count, [](const auto& m) {
                            return (m.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0;
                        });
                        require(readonly, "Graphics storage buffers must be declared readonly");
                    }
                } else if (binding.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || binding.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                    require(b.image.dim == SpvDim2D && !b.image.arrayed && !b.image.ms && !b.image.depth, "Only non-array, non-shadow, single-sample 2D textures are exposed");
                    if (binding.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                        switch (b.image.image_format) {
                            case SpvImageFormatRgba8: binding.storageFormat = VK_FORMAT_R8G8B8A8_UNORM; break;
                            case SpvImageFormatRgba16f: binding.storageFormat = VK_FORMAT_R16G16B16A16_SFLOAT; break;
                            case SpvImageFormatRgba32f: binding.storageFormat = VK_FORMAT_R32G32B32A32_SFLOAT; break;
                            case SpvImageFormatR32f: binding.storageFormat = VK_FORMAT_R32_SFLOAT; break;
                            default: throw std::invalid_argument("Unsupported storage image format; declare rgba8/rgba16f/rgba32f/r32f");
                        }
                    }
                } else throw std::invalid_argument("Unsupported shader descriptor type");
                reflectedBindings.push_back(binding);
            }
        }
        uint32_t pushCount = 0;
        require(spvReflectEnumerateEntryPointPushConstantBlocks(&reflection, shader.entry.c_str(), &pushCount, nullptr) == SPV_REFLECT_RESULT_SUCCESS, "Cannot reflect push constants");
        std::vector<SpvReflectBlockVariable*> pushes(pushCount);
        require(spvReflectEnumerateEntryPointPushConstantBlocks(&reflection, shader.entry.c_str(), &pushCount, pushes.data()) == SPV_REFLECT_RESULT_SUCCESS, "Cannot reflect push constants");
        for (const auto* push : pushes) for (uint32_t i = 0; i < push->member_count; ++i)
            reflectedPushBytes = std::max(reflectedPushBytes, push->members[i].offset + push->members[i].size);
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * 4; info.pCode = code.data();
        check(vkCreateShaderModule(d.device, &info, nullptr, &module), "vkCreateShaderModule");
    }
    ~Module() { if (module) vkDestroyShaderModule(d.device, module, nullptr); }
};
void resolveLayout(Pipeline& pipeline, std::initializer_list<const Module*> modules) {
    std::map<uint32_t, BindingLayout> reflected;
    uint32_t pushBytes = 0;
    for (const auto* module : modules) {
        pushBytes = std::max(pushBytes, module->reflectedPushBytes);
        for (const auto& b : module->reflectedBindings) {
            auto [it, inserted] = reflected.emplace(b.binding, b);
            if (!inserted) {
                require(it->second.type == b.type && it->second.storageFormat == b.storageFormat, "Shader stages disagree on descriptor type");
                it->second.minimumBytes = std::max(it->second.minimumBytes, b.minimumBytes);
            }
        }
    }
    if (!pipeline.bindings.empty()) {
        require(pipeline.bindings.size() == reflected.size(), "Explicit bindings do not match reflected shader bindings");
        std::set<uint32_t> indices;
        for (const auto& b : pipeline.bindings) {
            require(indices.insert(b.binding).second && reflected.count(b.binding) && reflected.at(b.binding).type == b.type, "Explicit binding differs from shader declaration");
        }
    }
    pipeline.bindings.clear();
    for (const auto& [index, b] : reflected) { (void)index; pipeline.bindings.push_back(b); }
    if (pipeline.pushBytes == 0) pipeline.pushBytes = pushBytes;
    require(pipeline.pushBytes >= pushBytes, "Push constant range is smaller than the shader block");
}
}

std::shared_ptr<Device> Device::create(uint32_t required, bool validation, bool allowSoftware) {
    require((required & ~127u) == 0, "Unknown requested feature");
    auto result = std::make_shared<Device>();
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    check(vkEnumerateInstanceVersion(&loaderVersion), "vkEnumerateInstanceVersion");
    require(loaderVersion >= VK_API_VERSION_1_1, "Vulkan 1.1 loader required");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Vulkano"; app.apiVersion = std::min(loaderVersion, uint32_t(VK_API_VERSION_1_3));
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    std::vector<const char*> instanceExtensions;
#ifdef __ANDROID__
    instanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
#endif
    const char* layer = "VK_LAYER_KHRONOS_validation";
    VkValidationFeatureEnableEXT syncValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    if (validation) {
        uint32_t count = 0; check(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate layers");
        std::vector<VkLayerProperties> layers(count);
        check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "enumerate layers");
        require(std::any_of(layers.begin(), layers.end(), [&](const auto& p) { return std::strcmp(p.layerName, layer) == 0; }), "Validation layer requested but not installed");
        instanceInfo.enabledLayerCount = 1; instanceInfo.ppEnabledLayerNames = &layer;
        uint32_t extensionCount = 0;
        check(vkEnumerateInstanceExtensionProperties(layer, &extensionCount, nullptr), "enumerate validation extensions");
        std::vector<VkExtensionProperties> validationExtensions(extensionCount);
        check(vkEnumerateInstanceExtensionProperties(layer, &extensionCount, validationExtensions.data()), "enumerate validation extensions");
        if (extension(validationExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
            instanceExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            validationFeatures.enabledValidationFeatureCount = 1;
            validationFeatures.pEnabledValidationFeatures = &syncValidation;
            instanceInfo.pNext = &validationFeatures;
        }
    }
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
    check(vkCreateInstance(&instanceInfo, nullptr, &result->instance), "vkCreateInstance (Vulkan 1.1 required)");
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(result->instance, &count, nullptr), "enumerate devices");
    std::vector<VkPhysicalDevice> physicals(count);
    check(vkEnumeratePhysicalDevices(result->instance, &count, physicals.data()), "enumerate devices");
    for (auto physical : physicals) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        if (props.apiVersion < VK_API_VERSION_1_1 || (!allowSoftware && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)) continue;
        uint32_t ec = 0;
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &ec, nullptr), "enumerate extensions");
        std::vector<VkExtensionProperties> exts(ec);
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &ec, exts.data()), "enumerate extensions");
#ifdef __ANDROID__
        if (!extension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
#endif
        const bool coreFloat16 = std::min(props.apiVersion, app.apiVersion) >= VK_API_VERSION_1_2;
        const bool float16 = coreFloat16 || extension(exts, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        VkPhysicalDeviceShaderFloat16Int8Features f16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        VkPhysicalDevice16BitStorageFeatures storage16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
        storage16.pNext = float16 ? &f16 : nullptr;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &storage16;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        const auto& f = features.features;
        const uint32_t available = (f.samplerAnisotropy ? Anisotropy : 0u) | (f.shaderInt16 ? Int16 : 0u) |
            (storage16.storageBuffer16BitAccess ? Storage16 : 0u) | (f16.shaderFloat16 ? Float16 : 0u) |
            (f.textureCompressionASTC_LDR ? Astc : 0u) | (f.textureCompressionETC2 ? Etc2 : 0u) |
            (f.shaderStorageImageExtendedFormats ? ExtendedStorageFormats : 0u);
        if ((available & required) != required) continue;
        uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> families(qc); vkGetPhysicalDeviceQueueFamilyProperties(physical, &qc, families.data());
        auto it = std::find_if(families.begin(), families.end(), [](const auto& q) {
            return q.queueCount > 0 && (q.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
        });
        if (it == families.end()) continue;
        result->physical = physical; result->properties = props; result->available = available; result->enabled = required;
        result->family = static_cast<uint32_t>(it - families.begin());
        result->memoryBudget = extension(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        std::vector<const char*> enabledExtensions;
#ifdef __ANDROID__
        enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#endif
        if (result->memoryBudget) enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        if ((required & Float16) && !coreFloat16) enabledExtensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        VkPhysicalDeviceFeatures enabledFeatures{};
        enabledFeatures.samplerAnisotropy = (required & Anisotropy) != 0;
        enabledFeatures.shaderInt16 = (required & Int16) != 0;
        enabledFeatures.textureCompressionASTC_LDR = (required & Astc) != 0;
        enabledFeatures.textureCompressionETC2 = (required & Etc2) != 0;
        enabledFeatures.shaderStorageImageExtendedFormats = (required & ExtendedStorageFormats) != 0;
        VkPhysicalDeviceShaderFloat16Int8Features enableF16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        enableF16.shaderFloat16 = (required & Float16) != 0;
        VkPhysicalDevice16BitStorageFeatures enableStorage{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
        enableStorage.storageBuffer16BitAccess = (required & Storage16) != 0;
        enableStorage.pNext = (required & Float16) ? &enableF16 : nullptr;
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = result->family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &enableStorage; deviceInfo.pEnabledFeatures = &enabledFeatures;
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()); deviceInfo.ppEnabledExtensionNames = enabledExtensions.data();
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &result->device), "vkCreateDevice");
        vkGetDeviceQueue(result->device, result->family, 0, &result->queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &result->memory);
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; properties2.pNext = &result->subgroup;
        vkGetPhysicalDeviceProperties2(physical, &properties2);
        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.physicalDevice = physical; allocatorInfo.device = result->device; allocatorInfo.instance = result->instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_1;
        allocatorInfo.flags = result->memoryBudget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0;
        check(vmaCreateAllocator(&allocatorInfo, &result->allocator), "vmaCreateAllocator");
        VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        check(vkCreatePipelineCache(result->device, &cacheInfo, nullptr, &result->pipelineCache), "vkCreatePipelineCache");
        return result;
    }
    throw std::runtime_error("No Vulkan 1.1 graphics/compute device supports the requested features");
}
Device::~Device() {
    if (device) vkDeviceWaitIdle(device);
    for (auto [key, pass] : renderPassCache) { (void)key; vkDestroyRenderPass(device, pass, nullptr); }
    if (pipelineCache) vkDestroyPipelineCache(device, pipelineCache, nullptr);
    if (allocator) vmaDestroyAllocator(allocator);
    if (device) vkDestroyDevice(device, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
}
void Device::collect() {
    pending.erase(std::remove_if(pending.begin(), pending.end(), [](const auto& weak) {
        auto cmd = weak.lock(); return !cmd || cmd->wait(0);
    }), pending.end());
}
void Device::waitIdle() { check(vkQueueWaitIdle(queue), "vkQueueWaitIdle"); collect(); }

Buffer::Buffer(std::shared_ptr<Device> device, VkDeviceSize length, VkBufferUsageFlags flags, Storage mode)
    : Resource(std::move(device)), size(length), usage(flags), storage(mode) {
    require(size > 0 && storage != Storage::Memoryless, "Buffers require positive length and shared/private storage");
    constexpr VkBufferUsageFlags allowed = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    require(usage != 0 && (usage & ~allowed) == 0, "Unsupported buffer usage");
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = size; info.usage = usage;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = storage == Storage::Shared ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (storage == Storage::Shared) {
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        alloc.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    check(vmaCreateBuffer(d->allocator, &info, &alloc, &buffer, &allocation, nullptr), "vmaCreateBuffer");
}
Buffer::~Buffer() { if (buffer) vmaDestroyBuffer(d->allocator, buffer, allocation); }
void Buffer::write(VkDeviceSize offset, const void* bytes, size_t count) {
    range(size, offset, count); require(storage == Storage::Shared, "Private buffers require a blit from shared storage");
    d->collect(); require(inFlight == 0, "Buffer is in use by the GPU; wait for command completion");
    check(vmaCopyMemoryToAllocation(d->allocator, bytes, allocation, offset, count), "write/flush shared buffer");
}
void Buffer::read(VkDeviceSize offset, void* bytes, size_t count) {
    range(size, offset, count); require(storage == Storage::Shared, "Private buffers require a blit to shared storage");
    d->collect(); require(inFlight == 0, "Buffer is in use by the GPU; wait for command completion");
    check(vmaCopyAllocationToMemory(d->allocator, allocation, offset, bytes, count), "invalidate/read shared buffer");
}
Texture::Texture(std::shared_ptr<Device> device, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags u, Storage s)
    : Resource(std::move(device)), format(f), width(w), height(h), usage(u), storage(s) {
    require(width > 0 && height > 0 && storage != Storage::Shared, "Textures require positive dimensions and private/memoryless storage");
    pixelSize();
    constexpr VkImageUsageFlags allowed = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    require(usage != 0 && (usage & ~allowed) == 0, "Unsupported texture usage");
    require(depth() ? (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) == 0 : (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0, "Format and attachment usage do not match");
    require(!depth() || (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) == 0, "Depth textures are attachment-only in this release");
    if (storage == Storage::Memoryless) {
        require(usage == (depth() ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), "Memoryless textures are attachment-only");
        usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    }
    VkImageFormatProperties supported{};
    check(vkGetPhysicalDeviceImageFormatProperties(d->physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported), "Unsupported texture format/usage");
    require(width <= supported.maxExtent.width && height <= supported.maxExtent.height, "Texture dimensions exceed device limits");
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.extent = {width, height, 1}; info.mipLevels = 1; info.arrayLayers = 1;
    info.format = format; info.tiling = VK_IMAGE_TILING_OPTIMAL; info.samples = VK_SAMPLE_COUNT_1_BIT; info.usage = usage;
    VmaAllocationCreateInfo alloc{}; alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (storage == Storage::Memoryless) alloc.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    check(vmaCreateImage(d->allocator, &info, &alloc, &image, &allocation, nullptr), "vmaCreateImage");
    VkMemoryPropertyFlags memoryFlags; vmaGetAllocationMemoryProperties(d->allocator, allocation, &memoryFlags);
    lazy = (memoryFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0;
    try {
        if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
            view = makeView(*d, image, format);
    }
    catch (...) { vmaDestroyImage(d->allocator, image, allocation); image = VK_NULL_HANDLE; throw; }
}
Texture::Texture(std::shared_ptr<Device> device, uint32_t w, uint32_t h, VkFormat f, VkImage i, VkImageView v)
    : Resource(std::move(device)), image(i), view(v), format(f), width(w), height(h),
      usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), storage(Storage::Private), borrowed(true) {}
void Texture::usable() const { require(!frame || frame->active, "Drawable is no longer acquired"); }
uint32_t Texture::pixelSize() const {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
        default: throw std::invalid_argument("Unsupported texture format");
    }
}
Texture::~Texture() {
    if (!borrowed) {
        if (view) vkDestroyImageView(d->device, view, nullptr);
        if (image) vmaDestroyImage(d->allocator, image, allocation);
    }
}
Sampler::Sampler(std::shared_ptr<Device> device, bool linearFilter, bool repeat, float anisotropy)
    : Resource(std::move(device)), linear(linearFilter) {
    require(anisotropy >= 1 && anisotropy <= d->properties.limits.maxSamplerAnisotropy, "Invalid anisotropy");
    require(anisotropy == 1 || (d->enabled & Anisotropy), "Sampler anisotropy feature was not enabled");
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = info.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = info.addressModeV = info.addressModeW = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.anisotropyEnable = anisotropy > 1; info.maxAnisotropy = anisotropy;
    check(vkCreateSampler(d->device, &info, nullptr, &sampler), "vkCreateSampler");
}
Sampler::~Sampler() { if (sampler) vkDestroySampler(d->device, sampler, nullptr); }

void Pipeline::makeLayout() {
    require(pushBytes % 4 == 0 && pushBytes <= d->properties.limits.maxPushConstantsSize, "Invalid push constant byte count");
    std::vector<VkDescriptorSetLayoutBinding> vkBindings;
    std::set<uint32_t> indices;
    std::map<VkDescriptorType, uint32_t> counts;
    for (const auto& b : bindings) {
        require(indices.insert(b.binding).second, "Duplicate descriptor binding");
        require(b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, "Unsupported descriptor type");
        require(compute || b.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, "Storage images are compute-only");
        ++counts[b.type];
        vkBindings.push_back({b.binding, b.type, 1, compute ? VkShaderStageFlags(VK_SHADER_STAGE_COMPUTE_BIT) : VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT), nullptr});
    }
    const auto& l = d->properties.limits;
    require(counts[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] <= l.maxPerStageDescriptorStorageBuffers && counts[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] <= l.maxDescriptorSetStorageBuffers, "Too many storage buffers");
    require(counts[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] <= l.maxPerStageDescriptorUniformBuffers && counts[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] <= l.maxDescriptorSetUniformBuffers, "Too many uniform buffers");
    require(counts[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] <= l.maxPerStageDescriptorStorageImages && counts[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] <= l.maxDescriptorSetStorageImages, "Too many storage images");
    const auto sampled = counts[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER];
    require(sampled <= l.maxPerStageDescriptorSampledImages && sampled <= l.maxPerStageDescriptorSamplers && sampled <= l.maxDescriptorSetSampledImages && sampled <= l.maxDescriptorSetSamplers, "Too many sampled textures");
    require(bindings.size() <= l.maxPerStageResources, "Too many per-stage resources");
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = static_cast<uint32_t>(vkBindings.size()); setInfo.pBindings = vkBindings.data();
    check(vkCreateDescriptorSetLayout(d->device, &setInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
    VkPushConstantRange range{compute ? VkShaderStageFlags(VK_SHADER_STAGE_COMPUTE_BIT) : VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT), 0, pushBytes};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = pushBytes ? 1 : 0; layoutInfo.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(d->device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
}
Pipeline::Pipeline(std::shared_ptr<Device> device, std::vector<BindingLayout> b, uint32_t p, const Shader& shader)
    : Resource(std::move(device)), bindings(std::move(b)), pushBytes(p), compute(true) {
    try {
        Module module(*d, shader, 5); localSize = module.local;
        resolveLayout(*this, {&module});
        makeLayout();
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; info.layout = layout;
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module.module, shader.entry.c_str(), nullptr};
        check(vkCreateComputePipelines(d->device, d->pipelineCache, 1, &info, nullptr, &pipeline), "vkCreateComputePipelines");
    } catch (...) {
        if (pipeline) vkDestroyPipeline(d->device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(d->device, layout, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(d->device, setLayout, nullptr);
        throw;
    }
}
Pipeline::Pipeline(std::shared_ptr<Device> device, std::vector<BindingLayout> b, uint32_t p,
                   const Shader& vertex, const Shader& fragment, VkFormat color, VkFormat depth, bool blend)
    : Resource(std::move(device)), bindings(std::move(b)), pushBytes(p), compute(false), colorFormat(color), depthFormat(depth) {
    try {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(d->physical, color, &fp);
        require((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0, "Unsupported color attachment format");
        require(!blend || (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT), "Format does not support blending");
        require(depth == VK_FORMAT_UNDEFINED || depth == VK_FORMAT_D32_SFLOAT, "Unsupported depth format");
        if (depth != VK_FORMAT_UNDEFINED) {
            vkGetPhysicalDeviceFormatProperties(d->physical, depth, &fp);
            require((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0, "Unsupported depth attachment format");
        }
        Module vs(*d, vertex, 0), fs(*d, fragment, 4);
        resolveLayout(*this, {&vs, &fs});
        makeLayout();
        compatiblePass = makePass(*d, color, depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE);
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs.module, vertex.entry.c_str(), nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs.module, fragment.entry.c_str(), nullptr}};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask = 15; attachment.blendEnable = blend;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blending.attachmentCount = 1; blending.pAttachments = &attachment;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = ds.depthWriteEnable = depth != VK_FORMAT_UNDEFINED; ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamics;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertexInput; info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &samples;
        info.pDepthStencilState = &ds; info.pColorBlendState = &blending; info.pDynamicState = &dynamic; info.layout = layout; info.renderPass = compatiblePass;
        check(vkCreateGraphicsPipelines(d->device, d->pipelineCache, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines");
    } catch (...) {
        if (pipeline) vkDestroyPipeline(d->device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(d->device, layout, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(d->device, setLayout, nullptr);
        throw;
    }
}
Pipeline::~Pipeline() {
    if (pipeline) vkDestroyPipeline(d->device, pipeline, nullptr);
    if (layout) vkDestroyPipelineLayout(d->device, layout, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(d->device, setLayout, nullptr);
}

Command::Command(std::shared_ptr<Device> device) : Resource(std::move(device)) {}
void Command::recording() const { require(state == State::Recording, "Command buffer is not recording (one submission only)"); }
void Command::barrier() {
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
}
void Command::transition(Texture& texture, VkImageLayout layout, bool read) {
    texture.usable();
    require(!texture.borrowed || (presentation && presentation->texture.get() == &texture), "Drawable attachment must be presented by the same command buffer");
    auto [it, inserted] = images.emplace(&texture, ImageState{texture.layout, texture.initialized});
    (void)inserted;
    require(!read || it->second.initialized, "Cannot load or read an uninitialized/discarded texture");
    // Every operation already has a memory dependency. Within prepare() there
    // are no intervening accesses, so an unchanged layout needs no second barrier.
    if (it->second.layout == layout) return;
    ++imageBarrierCount;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = it->second.layout; b.newLayout = layout;
    b.srcAccessMask = b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = texture.image;
    b.subresourceRange = {texture.depth() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    it->second.layout = layout;
}
void Command::markInitialized(Texture& texture, bool value) { images.at(&texture).initialized = value; }
void Command::validateBindings(const Pipeline& p, const std::vector<Binding>& bs, const std::vector<uint8_t>& constants) {
    same(*this, p);
    require(constants.size() == p.pushBytes, "Set exactly the pipeline's declared push constant bytes");
    require(bs.size() == p.bindings.size(), "Every pipeline binding must be supplied");
    std::set<uint32_t> found;
    for (const auto& b : bs) {
        require(found.insert(b.index).second, "Duplicate binding");
        auto schema = std::find_if(p.bindings.begin(), p.bindings.end(), [&](const auto& s) { return s.binding == b.index; });
        require(schema != p.bindings.end(), "Binding is not declared in pipeline");
        const auto& limits = d->properties.limits;
        if (schema->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || schema->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            require(b.buffer && !b.texture && !b.sampler, "Binding requires a buffer"); same(*this, *b.buffer);
            range(b.buffer->size, b.offset, b.length);
            require(b.length >= schema->minimumBytes, "Buffer range is smaller than the shader block");
            const bool uniform = schema->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            require((b.buffer->usage & (uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) != 0, "Buffer usage does not match binding");
            require(b.offset % (uniform ? limits.minUniformBufferOffsetAlignment : limits.minStorageBufferOffsetAlignment) == 0, "Buffer binding offset is not aligned");
            require(b.length <= (uniform ? limits.maxUniformBufferRange : limits.maxStorageBufferRange), "Buffer binding exceeds device range limit");
        } else {
            require(b.texture && !b.buffer, "Binding requires a texture"); same(*this, *b.texture); b.texture->usable();
            const bool sampled = schema->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            require(sampled || b.texture->format == schema->storageFormat, "Storage texture format differs from the shader declaration");
            require((b.texture->usage & (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT)) != 0, "Texture usage does not match binding");
            require(sampled == bool(b.sampler), "Sampled textures require a sampler; storage textures must not have one");
            if (b.sampler) {
                same(*this, *b.sampler);
                VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(d->physical, b.texture->format, &fp);
                require(!b.sampler->linear || (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT), "Texture format does not support linear filtering");
            }
        }
    }
}
void Command::prepare(const std::vector<Binding>& bindings, bool compute) {
    for (const auto& b : bindings) {
        if (b.texture) {
            // Keep GENERAL for textures that can alias sampled/storage descriptors.
            const auto layout = (b.texture->usage & VK_IMAGE_USAGE_STORAGE_BIT)
                ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            transition(*b.texture, layout, !compute || bool(b.sampler));
            if (compute && !b.sampler) markInitialized(*b.texture, true);
        }
    }
}
void Command::bind(const Pipeline& p, const std::vector<Binding>& bs, const std::vector<uint8_t>& constants) {
    const auto point = p.compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    auto& bound = boundPipelines[p.compute ? 0 : 1];
    if (bound != p.pipeline) { vkCmdBindPipeline(command, point, p.pipeline); bound = p.pipeline; }
    if (!bs.empty()) {
        // Canonical binding order; offsets, ranges, sampler and layout identity
        // are part of the key. Push constants deliberately are not.
        std::vector<const Binding*> ordered;
        for (const auto& binding : bs) ordered.push_back(&binding);
        std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->index < b->index; });
        std::vector<uint64_t> key{reinterpret_cast<uintptr_t>(&p)};
        for (auto b : ordered) {
            key.insert(key.end(), {b->index, reinterpret_cast<uintptr_t>(b->buffer.get()), b->offset, b->length,
                reinterpret_cast<uintptr_t>(b->texture.get()), reinterpret_cast<uintptr_t>(b->sampler.get())});
        }
        VkDescriptorSet set;
        auto cached = descriptorSets.find(key);
        if (cached != descriptorSets.end()) { set = cached->second; ++descriptorCacheHits; }
        else {
            constexpr uint32_t setsPerPool = 64;
            auto& arena = descriptorArenas[&p];
            if (!arena.pool || arena.used == setsPerPool) {
                std::map<VkDescriptorType, uint32_t> counts;
                for (const auto& b : p.bindings) counts[b.type] += setsPerPool;
                std::vector<VkDescriptorPoolSize> sizes;
                for (auto [type, count] : counts) sizes.push_back({type, count});
                VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                pi.maxSets = setsPerPool; pi.poolSizeCount = static_cast<uint32_t>(sizes.size()); pi.pPoolSizes = sizes.data();
                descriptorPools.reserve(descriptorPools.size() + 1);
                VkDescriptorPool dp; check(vkCreateDescriptorPool(d->device, &pi, nullptr, &dp), "vkCreateDescriptorPool");
                descriptorPools.push_back(dp); arena = {dp, 0};
            }
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = arena.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &p.setLayout;
            check(vkAllocateDescriptorSets(d->device, &ai, &set), "vkAllocateDescriptorSets");
            ++arena.used;
            std::vector<VkDescriptorBufferInfo> buffersInfo(bs.size());
            std::vector<VkDescriptorImageInfo> imageInfo(bs.size());
            std::vector<VkWriteDescriptorSet> writes(bs.size());
            for (size_t i = 0; i < bs.size(); ++i) {
                const auto& b = bs[i];
                auto& w = writes[i]; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = set; w.dstBinding = b.index; w.descriptorCount = 1;
                w.descriptorType = std::find_if(p.bindings.begin(), p.bindings.end(), [&](const auto& s) { return s.binding == b.index; })->type;
                if (b.buffer) { buffersInfo[i] = {b.buffer->buffer, b.offset, b.length}; w.pBufferInfo = &buffersInfo[i]; }
                else { imageInfo[i] = {b.sampler ? b.sampler->sampler : VK_NULL_HANDLE, b.texture->view, (b.texture->usage & VK_IMAGE_USAGE_STORAGE_BIT) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; w.pImageInfo = &imageInfo[i]; }
            }
            vkUpdateDescriptorSets(d->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            descriptorSets.emplace(std::move(key), set);
        }
        vkCmdBindDescriptorSets(command, point, p.layout, 0, 1, &set, 0, nullptr);
    }
    if (!constants.empty()) vkCmdPushConstants(command, p.layout,
        p.compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, static_cast<uint32_t>(constants.size()), constants.data());
}
void Command::dispatch(Dispatch op) {
    recording(); require(op.pipeline && op.pipeline->compute, "Compute pipeline required");
    validateBindings(*op.pipeline, op.bindings, op.constants);
    for (int i = 0; i < 3; ++i) require(op.groups[i] > 0 && op.groups[i] <= d->properties.limits.maxComputeWorkGroupCount[i], "Dispatch exceeds device workgroup count limits");
    for (const auto& b : op.bindings) if (b.buffer) buffers.push_back(b.buffer);
    operations.push_back([op = std::move(op)](Command& c) {
        c.barrier(); c.prepare(op.bindings, true); c.bind(*op.pipeline, op.bindings, op.constants);
        vkCmdDispatch(c.command, op.groups[0], op.groups[1], op.groups[2]);
    });
}
void Command::render(Render op) {
    recording(); require(bool(op.color), "A color attachment is required"); same(*this, *op.color); op.color->usable();
    const auto& limits = d->properties.limits;
    require(op.color->width <= limits.maxFramebufferWidth && op.color->height <= limits.maxFramebufferHeight &&
            op.color->width <= limits.maxViewportDimensions[0] && op.color->height <= limits.maxViewportDimensions[1], "Render target exceeds framebuffer/viewport limits");
    require((op.color->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0, "Texture is not a color attachment");
    auto validActions = [](const Texture& t, VkAttachmentLoadOp load, VkAttachmentStoreOp store) {
        require(load == VK_ATTACHMENT_LOAD_OP_LOAD || load == VK_ATTACHMENT_LOAD_OP_CLEAR || load == VK_ATTACHMENT_LOAD_OP_DONT_CARE, "Invalid load action");
        require(store == VK_ATTACHMENT_STORE_OP_STORE || store == VK_ATTACHMENT_STORE_OP_DONT_CARE, "Invalid store action");
        require(t.storage != Storage::Memoryless || (load != VK_ATTACHMENT_LOAD_OP_LOAD && store == VK_ATTACHMENT_STORE_OP_DONT_CARE), "Memoryless attachments cannot load or store");
    };
    validActions(*op.color, op.colorLoad, op.colorStore);
    if (op.depth) {
        same(*this, *op.depth); op.depth->usable();
        require(op.depth->depth() && (op.depth->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT), "Invalid depth attachment");
        require(op.depth->width == op.color->width && op.depth->height == op.color->height, "Attachment sizes must match");
        require(op.clearDepth >= 0 && op.clearDepth <= 1, "Depth clear must be in [0, 1]");
        validActions(*op.depth, op.depthLoad, op.depthStore);
    }
    for (const auto& draw : op.draws) {
        require(draw.pipeline && !draw.pipeline->compute, "Render pipeline required");
        validateBindings(*draw.pipeline, draw.bindings, draw.constants);
        require(draw.pipeline->colorFormat == op.color->format && draw.pipeline->depthFormat == (op.depth ? op.depth->format : VK_FORMAT_UNDEFINED), "Pipeline formats must match render attachments");
        require(draw.vertices > 0 && draw.instances > 0 && draw.firstVertex <= UINT32_MAX - draw.vertices && draw.firstInstance <= UINT32_MAX - draw.instances, "Invalid draw range");
        for (const auto& b : draw.bindings) {
            require(!b.texture || (b.texture != op.color && b.texture != op.depth), "Attachment feedback loops are unsupported");
            if (b.buffer) buffers.push_back(b.buffer);
        }
    }
    operations.push_back([op = std::move(op)](Command& c) {
        c.barrier();
        for (const auto& draw : op.draws) c.prepare(draw.bindings, false);
        c.transition(*op.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, op.colorLoad == VK_ATTACHMENT_LOAD_OP_LOAD);
        if (op.depth) c.transition(*op.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, op.depthLoad == VK_ATTACHMENT_LOAD_OP_LOAD);
        auto pass = makePass(*c.d, op.color->format, op.depth ? op.depth->format : VK_FORMAT_UNDEFINED, op.colorLoad, op.colorStore, op.depthLoad, op.depthStore);
        VkImageView views[] = {op.color->view, op.depth ? op.depth->view : VK_NULL_HANDLE};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = pass;
        fi.attachmentCount = op.depth ? 2 : 1; fi.pAttachments = views; fi.width = op.color->width; fi.height = op.color->height; fi.layers = 1;
        VkFramebuffer fb; check(vkCreateFramebuffer(c.d->device, &fi, nullptr, &fb), "vkCreateFramebuffer"); c.framebuffers.push_back(fb);
        VkClearValue clear[2]{}; std::copy(op.clearColor.begin(), op.clearColor.end(), clear[0].color.float32); clear[1].depthStencil.depth = op.clearDepth;
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = pass; begin.framebuffer = fb;
        begin.renderArea.extent = {op.color->width, op.color->height}; begin.clearValueCount = fi.attachmentCount; begin.pClearValues = clear;
        vkCmdBeginRenderPass(c.command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        // Vulkan 1.1 negative viewport height gives Metal's upward NDC Y while
        // framebuffer/texture coordinates keep a top-left origin.
        VkViewport viewport{0, static_cast<float>(op.color->height), static_cast<float>(op.color->width), -static_cast<float>(op.color->height), 0, 1};
        VkRect2D scissor{{0, 0}, {op.color->width, op.color->height}};
        vkCmdSetViewport(c.command, 0, 1, &viewport); vkCmdSetScissor(c.command, 0, 1, &scissor);
        for (const auto& draw : op.draws) {
            c.bind(*draw.pipeline, draw.bindings, draw.constants);
            vkCmdDraw(c.command, draw.vertices, draw.instances, draw.firstVertex, draw.firstInstance);
        }
        vkCmdEndRenderPass(c.command);
        c.markInitialized(*op.color, op.colorStore == VK_ATTACHMENT_STORE_OP_STORE);
        if (op.depth) c.markInitialized(*op.depth, op.depthStore == VK_ATTACHMENT_STORE_OP_STORE);
    });
}
void Command::copy(std::shared_ptr<Buffer> src, std::shared_ptr<Buffer> dst, VkDeviceSize so, VkDeviceSize to, VkDeviceSize size) {
    recording(); require(src && dst, "Buffers are required"); same(*this, *src); same(*this, *dst);
    range(src->size, so, size); range(dst->size, to, size);
    require((src->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) && (dst->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT), "Blit requires transfer usage");
    require(src != dst, "Copies within the same buffer are unsupported");
    require(so % 4 == 0 && to % 4 == 0 && size % 4 == 0, "Buffer blits require 4-byte alignment");
    buffers.push_back(src); buffers.push_back(dst);
    operations.push_back([src, dst, so, to, size](Command& c) { c.barrier(); VkBufferCopy region{so, to, size}; vkCmdCopyBuffer(c.command, src->buffer, dst->buffer, 1, &region); });
}
void Command::copy(std::shared_ptr<Buffer> buffer, std::shared_ptr<Texture> texture, VkDeviceSize offset, bool toTexture) {
    recording(); require(buffer && texture, "Buffer and texture are required"); same(*this, *buffer); same(*this, *texture); texture->usable();
    require(!texture->depth(), "Depth copies are unsupported");
    const uint64_t bytes = uint64_t(texture->width) * texture->height * texture->pixelSize(); range(buffer->size, offset, bytes);
    require(offset % 4 == 0 && offset % texture->pixelSize() == 0, "Texture blit offset must align to pixel size and 4 bytes");
    require((buffer->usage & (toTexture ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT)) &&
            (texture->usage & (toTexture ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT)), "Blit requires matching transfer usage");
    buffers.push_back(buffer);
    operations.push_back([buffer, texture, offset, toTexture](Command& c) {
        c.barrier(); c.transition(*texture, toTexture ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, !toTexture);
        VkBufferImageCopy region{}; region.bufferOffset = offset; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.imageExtent = {texture->width, texture->height, 1};
        if (toTexture) { vkCmdCopyBufferToImage(c.command, buffer->buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region); c.markInitialized(*texture, true); }
        else vkCmdCopyImageToBuffer(c.command, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->buffer, 1, &region);
    });
}
void Command::present(std::shared_ptr<Drawable> drawable) {
    recording(); require(drawable && !presentation && drawable->didAcquire && drawable->frame->active, "Present requires one currently acquired drawable");
    same(*this, *drawable); presentation = std::move(drawable);
}
void Command::commit() {
    recording(); d->collect();
    try {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex = d->family; poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        check(vkCreateCommandPool(d->device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; alloc.commandPool = pool; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(d->device, &alloc, &command), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        for (const auto& op : operations) op(*this);
        if (presentation) transition(*presentation->texture, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, true);
        barrier();
        check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(d->device, &fi, nullptr, &fence), "vkCreateFence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (presentation) {
            submit.waitSemaphoreCount = submit.signalSemaphoreCount = 1;
            submit.pWaitSemaphores = &presentation->acquired; submit.pSignalSemaphores = &presentation->rendered; submit.pWaitDstStageMask = &waitStage;
        }
        // Reserve the bookkeeping before submission; no allocating operation
        // may make a successfully submitted command appear unsubmitted.
        d->pending.reserve(d->pending.size() + 1);
        auto self = shared_from_this();
        check(vkQueueSubmit(d->queue, 1, &submit, fence), "vkQueueSubmit");
        state = State::Submitted;
        for (auto& b : buffers) ++b->inFlight;
        for (const auto& [texture, current] : images) { texture->layout = current.layout; texture->initialized = current.initialized; }
        d->pending.push_back(self);
        if (presentation) {
            VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; info.waitSemaphoreCount = 1; info.pWaitSemaphores = &presentation->rendered;
            info.swapchainCount = 1; info.pSwapchains = &presentation->surface->swapchain; info.pImageIndices = &presentation->index;
            VkResult result = vkQueuePresentKHR(d->queue, &info);
            presentation->presented = true; presentation->frame->active = false; presentation->surface->outstanding = false;
            if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) presentation->surface->needsRebuild = true;
            else check(result, "vkQueuePresentKHR");
        }
    } catch (...) {
        if (state == State::Recording) state = State::Failed;
        throw;
    }
}
bool Command::wait(uint64_t timeout) {
    if (state == State::Completed) return true;
    require(state == State::Submitted, "Only submitted commands can be waited on");
    auto result = vkWaitForFences(d->device, 1, &fence, VK_TRUE, timeout);
    if (result == VK_TIMEOUT) return false;
    check(result, "vkWaitForFences");
    state = State::Completed;
    for (auto& b : buffers) --b->inFlight;
    // Keep all recorded objects alive until the command pool is destroyed.
    return true;
}
Command::~Command() {
    if (state == State::Submitted) {
        // Device loss is terminal, but destructors must not throw through JNI.
        vkWaitForFences(d->device, 1, &fence, VK_TRUE, UINT64_MAX);
        for (auto& b : buffers) --b->inFlight;
    }
    if (pool) vkDestroyCommandPool(d->device, pool, nullptr);
    for (auto fb : framebuffers) vkDestroyFramebuffer(d->device, fb, nullptr);
    for (auto dp : descriptorPools) vkDestroyDescriptorPool(d->device, dp, nullptr);
    if (fence) vkDestroyFence(d->device, fence, nullptr);
}

#ifdef __ANDROID__
Surface::Surface(std::shared_ptr<Device> device, ANativeWindow* nativeWindow, uint32_t w, uint32_t h)
    : Resource(std::move(device)), requestedWidth(w), requestedHeight(h), window(nativeWindow) {
    require(window != nullptr && w > 0 && h > 0, "Invalid Android surface size");
    ANativeWindow_acquire(window);
    try {
        VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR}; info.window = window;
        check(vkCreateAndroidSurfaceKHR(d->instance, &info, nullptr, &surface), "vkCreateAndroidSurfaceKHR");
        VkBool32 supported = false;
        check(vkGetPhysicalDeviceSurfaceSupportKHR(d->physical, d->family, surface, &supported), "query presentation support");
        require(supported, "Selected graphics/compute queue cannot present to this surface");
        rebuild();
    } catch (...) {
        for (auto view : views) vkDestroyImageView(d->device, view, nullptr);
        if (swapchain) vkDestroySwapchainKHR(d->device, swapchain, nullptr);
        if (surface) vkDestroySurfaceKHR(d->instance, surface, nullptr);
        ANativeWindow_release(window); throw;
    }
}
#endif
void Surface::rebuild() {
    require(!outstanding, "Close or present the acquired drawable before resizing");
    d->waitIdle();
    VkSurfaceCapabilitiesKHR caps; check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->physical, surface, &caps), "query surface capabilities");
    require((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0, "Surface does not support color attachments");
    uint32_t count = 0; check(vkGetPhysicalDeviceSurfaceFormatsKHR(d->physical, surface, &count, nullptr), "query surface formats");
    std::vector<VkSurfaceFormatKHR> formats(count); check(vkGetPhysicalDeviceSurfaceFormatsKHR(d->physical, surface, &count, formats.data()), "query surface formats");
    auto chosen = std::find_if(formats.begin(), formats.end(), [](auto f) {
        return (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    VkSurfaceFormatKHR selected{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    if (formats.size() != 1 || formats[0].format != VK_FORMAT_UNDEFINED) {
        require(chosen != formats.end(), "Surface requires an unsupported color format"); selected = *chosen;
    }
    VkExtent2D newExtent = caps.currentExtent;
    if (newExtent.width == UINT32_MAX) newExtent = {std::clamp(requestedWidth, caps.minImageExtent.width, caps.maxImageExtent.width), std::clamp(requestedHeight, caps.minImageExtent.height, caps.maxImageExtent.height)};
    require(newExtent.width > 0 && newExtent.height > 0, "Surface has zero extent; wait for surfaceChanged");
    uint32_t imageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) imageCount = std::min(imageCount, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (auto option : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
        if (caps.supportedCompositeAlpha & option) { alpha = option; break; }
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface; info.minImageCount = imageCount; info.imageFormat = selected.format; info.imageColorSpace = selected.colorSpace;
    info.imageExtent = newExtent; info.imageArrayLayers = 1; info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Identity requests compositor rotation; shader pre-rotation is a future optimization.
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    info.compositeAlpha = alpha; info.presentMode = VK_PRESENT_MODE_FIFO_KHR; info.clipped = VK_TRUE; info.oldSwapchain = swapchain;
    VkSwapchainKHR next = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(d->device, &info, nullptr, &next), "vkCreateSwapchainKHR");
    // oldSwapchain is retired after successful creation, including if view creation fails.
    for (auto view : views) vkDestroyImageView(d->device, view, nullptr);
    views.clear(); images.clear();
    if (swapchain) vkDestroySwapchainKHR(d->device, swapchain, nullptr);
    swapchain = next; format = selected.format; extent = newExtent; needsRebuild = true;
    check(vkGetSwapchainImagesKHR(d->device, swapchain, &count, nullptr), "query swapchain images");
    images.resize(count); check(vkGetSwapchainImagesKHR(d->device, swapchain, &count, images.data()), "query swapchain images");
    views.reserve(count);
    for (auto img : images) views.push_back(makeView(*d, img, format));
    needsRebuild = false;
}
void Surface::resize(uint32_t width, uint32_t height) {
    require(width > 0 && height > 0, "Surface dimensions must be positive");
    requestedWidth = width; requestedHeight = height; rebuild();
}
std::shared_ptr<Drawable> Surface::acquire(uint64_t timeout) {
    require(!outstanding, "Only one drawable may be acquired per surface");
    // Conservative pacing: presentation waits must finish before semaphores or
    // swapchain views can be reused/destroyed. No presentation-fence extension required.
    d->waitIdle();
    if (needsRebuild) rebuild();
    auto drawable = std::make_shared<Drawable>(shared_from_this());
    const auto result = vkAcquireNextImageKHR(d->device, swapchain, timeout, drawable->acquired, drawable->acquireFence, &drawable->index);
    if (result == VK_TIMEOUT || result == VK_NOT_READY) return nullptr;
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { needsRebuild = true; return nullptr; }
    if (result == VK_SUBOPTIMAL_KHR) needsRebuild = true;
    else check(result, "vkAcquireNextImageKHR");
    drawable->didAcquire = true; outstanding = true;
    drawable->texture = std::make_shared<Texture>(d, extent.width, extent.height, format, images.at(drawable->index), views.at(drawable->index));
    drawable->texture->surface = shared_from_this(); drawable->texture->frame = drawable->frame;
    return drawable;
}
Surface::~Surface() {
    vkQueueWaitIdle(d->queue);
    for (auto view : views) vkDestroyImageView(d->device, view, nullptr);
    if (swapchain) vkDestroySwapchainKHR(d->device, swapchain, nullptr);
    if (surface) vkDestroySurfaceKHR(d->instance, surface, nullptr);
#ifdef __ANDROID__
    if (window) ANativeWindow_release(window);
#endif
}
Drawable::Drawable(std::shared_ptr<Surface> s) : Resource(s->d), surface(std::move(s)) {
    try {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(d->device, &si, nullptr, &acquired), "create acquire semaphore");
        check(vkCreateSemaphore(d->device, &si, nullptr, &rendered), "create present semaphore");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(d->device, &fi, nullptr, &acquireFence), "create acquire fence");
    } catch (...) {
        if (acquired) vkDestroySemaphore(d->device, acquired, nullptr);
        if (rendered) vkDestroySemaphore(d->device, rendered, nullptr);
        throw;
    }
}
Drawable::~Drawable() {
    if (didAcquire) {
        vkWaitForFences(d->device, 1, &acquireFence, VK_TRUE, UINT64_MAX);
        vkQueueWaitIdle(d->queue); // render-completion fences do not retire present waits
        if (!presented) { surface->outstanding = false; surface->needsRebuild = true; }
    }
    frame->active = false;
    if (acquireFence) vkDestroyFence(d->device, acquireFence, nullptr);
    if (acquired) vkDestroySemaphore(d->device, acquired, nullptr);
    if (rendered) vkDestroySemaphore(d->device, rendered, nullptr);
}
} // namespace vulkano
