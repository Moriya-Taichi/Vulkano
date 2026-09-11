#include "engine.hpp"
#include "extensions.hpp"
#include "generated.hpp"
#include "heaps.hpp"
#include "interop.hpp"
#include "ray.hpp"
#include "sparse.hpp"
#include "spirv-reflect/spirv_reflect.h"
#include "synchronization.hpp"
#include "tensors.hpp"
#include "tiles.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

namespace vulkano {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::invalid_argument(message);
}
void check(VkResult r, const char *op) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(op) + " failed (VkResult " + std::to_string(r) + ")");
}
namespace {
bool extension(const std::vector<VkExtensionProperties> &list, const char *name) {
    return std::any_of(list.begin(), list.end(),
                       [name](const auto &e) { return std::strcmp(e.extensionName, name) == 0; });
}
void same(const Resource &a, const Resource &b) {
    require(a.owner() == b.owner(), "Resources belong to different devices");
}
void range(VkDeviceSize capacity, VkDeviceSize offset, VkDeviceSize count) {
    require(count > 0 && offset <= capacity && count <= capacity - offset, "Buffer range is out of bounds");
}
VkImageView makeView(Device &d, VkImage image, VkFormat format) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0,
                             1, 0, 1};
    VkImageView view;
    check(vkCreateImageView(d.device, &info, nullptr, &view), "vkCreateImageView");
    return view;
}
void validateRateTexel(Device &d, VkExtent2D size) {
    require(d.enabled & AttachmentRate, "Attachment shading rate was not enabled");
    const auto &p = d.extensions->fragmentRateProperties;
    require(size.width && !(size.width & (size.width - 1)) && size.height && !(size.height & (size.height - 1)) &&
                size.width >= p.minFragmentShadingRateAttachmentTexelSize.width &&
                size.height >= p.minFragmentShadingRateAttachmentTexelSize.height &&
                size.width <= p.maxFragmentShadingRateAttachmentTexelSize.width &&
                size.height <= p.maxFragmentShadingRateAttachmentTexelSize.height &&
                uint64_t(size.width) <=
                    uint64_t(size.height) * p.maxFragmentShadingRateAttachmentTexelSizeAspectRatio &&
                uint64_t(size.height) <= uint64_t(size.width) * p.maxFragmentShadingRateAttachmentTexelSizeAspectRatio,
            "Rate map texel size exceeds device limits");
}
VkRenderPass makePass(Device &d, const std::vector<VkFormat> &colors, VkFormat depth, VkSampleCountFlagBits samples,
                      const std::vector<Attachment> &targets = {},
                      VkAttachmentLoadOp depthLoad = VK_ATTACHMENT_LOAD_OP_CLEAR,
                      VkAttachmentStoreOp depthStore = VK_ATTACHMENT_STORE_OP_DONT_CARE, uint32_t viewMask = 0,
                      const Render *render = nullptr) {
    if (render && render->passLayout) {
        if (render->rateMapTexelSize.width)
            validateRateTexel(d, render->rateMapTexelSize);
        return makeSubpassPass(d, *render->passLayout, targets, depthLoad, depthStore, viewMask, render->tileShading,
                               render->tileApron, render->rateMapTexelSize, render);
    }
    std::vector<int> key{int(colors.size()), depth, samples, depthLoad, depthStore, int(viewMask)};
    const bool tile = render && render->tileShading;
    const VkExtent2D apron = render ? render->tileApron : VkExtent2D{};
    validateTileOptions(d, tile, apron);
    key.insert(key.end(), {tile, int(apron.width), int(apron.height)});
    const auto colorLayout = tile ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const auto depthLayout = render ? render->depthLayout() : (tile ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    const auto stencilLoad = render ? render->stencilLoadOp() : depthLoad;
    const auto stencilStore = render ? render->stencilStoreOp() : depthStore;
    key.insert(key.end(), {stencilLoad, stencilStore, int(depthLayout)});
    const bool depthResolve = render && render->depthResolve;
    const bool rateMap = render && render->rateMapTexelSize.width;
    if (rateMap)
        validateRateTexel(d, render->rateMapTexelSize);
    key.insert(key.end(),
               {rateMap ? int(render->rateMapTexelSize.width) : 0, rateMap ? int(render->rateMapTexelSize.height) : 0});
    key.insert(key.end(), {depthResolve, depthResolve ? int(render->depthResolveMode) : 0,
                           depthResolve ? int(render->stencilResolveMode) : 0});
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> refs, resolves;
    for (size_t n = 0; n < colors.size(); ++n) {
        const auto load = targets.empty() ? VK_ATTACHMENT_LOAD_OP_CLEAR : targets[n].load;
        const auto store = targets.empty() ? VK_ATTACHMENT_STORE_OP_STORE : targets[n].store;
        key.insert(key.end(), {colors[n], load, store, targets.empty() ? 0 : bool(targets[n].resolve)});
        VkAttachmentDescription a{};
        a.format = colors[n];
        a.samples = samples;
        a.loadOp = load;
        a.storeOp = store;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = colorLayout;
        refs.push_back({uint32_t(attachments.size()), a.finalLayout});
        attachments.push_back(a);
    }
    VkAttachmentReference dr{VK_ATTACHMENT_UNUSED, depthLayout};
    if (depth != VK_FORMAT_UNDEFINED) {
        VkAttachmentDescription a{};
        a.format = depth;
        a.samples = samples;
        a.loadOp = depthLoad;
        a.stencilLoadOp = stencilLoad;
        a.storeOp = depthStore;
        a.stencilStoreOp = stencilStore;
        a.initialLayout = a.finalLayout = dr.layout;
        dr.attachment = uint32_t(attachments.size());
        attachments.push_back(a);
    }
    bool anyResolve = false;
    for (size_t n = 0; n < colors.size(); ++n) {
        VkAttachmentReference rr{VK_ATTACHMENT_UNUSED, colorLayout};
        if (!targets.empty() && targets[n].resolve) {
            anyResolve = true;
            VkAttachmentDescription a{};
            a.format = colors[n];
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = a.finalLayout = rr.layout;
            rr.attachment = uint32_t(attachments.size());
            attachments.push_back(a);
        }
        resolves.push_back(rr);
    }
    auto found = d.renderPassCache.find(key);
    if (found != d.renderPassCache.end())
        return found->second;
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = uint32_t(refs.size());
    sub.pColorAttachments = refs.data();
    sub.pResolveAttachments = anyResolve ? resolves.data() : nullptr;
    sub.pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &dr : nullptr;
    VkRenderPassCreateInfo i{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    i.attachmentCount = uint32_t(attachments.size());
    i.pAttachments = attachments.data();
    i.subpassCount = 1;
    i.pSubpasses = &sub;
    VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    mv.subpassCount = 1;
    mv.pViewMasks = &viewMask;
    mv.correlationMaskCount = 1;
    mv.pCorrelationMasks = &viewMask;
    if (viewMask)
        i.pNext = &mv;
    VkRenderPassTileShadingCreateInfoQCOM tileInfo{VK_STRUCTURE_TYPE_RENDER_PASS_TILE_SHADING_CREATE_INFO_QCOM};
    tileInfo.flags = VK_TILE_SHADING_RENDER_PASS_ENABLE_BIT_QCOM;
    tileInfo.tileApronSize = apron;
    auto dependency = tileDependency(0, 0);
    if (viewMask)
        dependency.dependencyFlags |= VK_DEPENDENCY_VIEW_LOCAL_BIT;
    if (tile) {
        tileInfo.pNext = i.pNext;
        i.pNext = &tileInfo;
        i.dependencyCount = 1;
        i.pDependencies = &dependency;
        if (apron.width || apron.height)
            sub.flags |= VK_SUBPASS_DESCRIPTION_TILE_SHADING_APRON_BIT_QCOM;
    }
    VkRenderPass pass;
    if (!depthResolve && !rateMap) {
        check(vkCreateRenderPass(d.device, &i, nullptr, &pass), "vkCreateRenderPass");
    } else {
        require(!depthResolve || (d.enabled & DepthResolve), "Depth/stencil resolve feature was not enabled");
        std::vector<VkAttachmentDescription2> descriptions;
        for (const auto &a : attachments) {
            VkAttachmentDescription2 b{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
            b.flags = a.flags;
            b.format = a.format;
            b.samples = a.samples;
            b.loadOp = a.loadOp;
            b.storeOp = a.storeOp;
            b.stencilLoadOp = a.stencilLoadOp;
            b.stencilStoreOp = a.stencilStoreOp;
            b.initialLayout = a.initialLayout;
            b.finalLayout = a.finalLayout;
            descriptions.push_back(b);
        }
        auto ref = [](const VkAttachmentReference &a) {
            VkAttachmentReference2 b{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
            b.attachment = a.attachment;
            b.layout = a.layout;
            return b;
        };
        std::vector<VkAttachmentReference2> colors2, resolves2;
        for (auto a : refs)
            colors2.push_back(ref(a));
        for (auto a : resolves)
            resolves2.push_back(ref(a));
        auto depth2 = ref(dr);
        VkAttachmentReference2 resolve2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        resolve2.attachment = uint32_t(descriptions.size());
        resolve2.layout = tile ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentDescription2 target{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        target.format = depth;
        target.samples = VK_SAMPLE_COUNT_1_BIT;
        target.loadOp = target.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        target.storeOp = target.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        target.initialLayout = target.finalLayout = resolve2.layout;
        if (depthResolve)
            descriptions.push_back(target);
        VkSubpassDescriptionDepthStencilResolve resolve{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
        resolve.depthResolveMode =
            depthResolve && render->depth->depth() ? render->depthResolveMode : VK_RESOLVE_MODE_NONE;
        resolve.stencilResolveMode =
            depthResolve && render->depth->stencil() ? render->stencilResolveMode : VK_RESOLVE_MODE_NONE;
        resolve.pDepthStencilResolveAttachment = &resolve2;
        VkSubpassDescription2 sub2{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        if (depthResolve)
            sub2.pNext = &resolve;
        VkAttachmentReference2 rateReference{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        VkFragmentShadingRateAttachmentInfoKHR rateInfo{VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
        if (rateMap) {
            rateReference.attachment = uint32_t(descriptions.size());
            rateReference.layout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
            VkAttachmentDescription2 rate{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
            rate.format = VK_FORMAT_R8_UINT;
            rate.samples = VK_SAMPLE_COUNT_1_BIT;
            rate.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            rate.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            rate.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            rate.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            rate.initialLayout = rate.finalLayout = rateReference.layout;
            descriptions.push_back(rate);
            rateInfo.pFragmentShadingRateAttachment = &rateReference;
            rateInfo.shadingRateAttachmentTexelSize = render->rateMapTexelSize;
            rateInfo.pNext = sub2.pNext;
            sub2.pNext = &rateInfo;
        }
        sub2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub2.viewMask = viewMask;
        sub2.colorAttachmentCount = uint32_t(colors2.size());
        sub2.pColorAttachments = colors2.data();
        sub2.pResolveAttachments = anyResolve ? resolves2.data() : nullptr;
        sub2.pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &depth2 : nullptr;
        VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        VkSubpassDependency2 dependency2{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
        if (tile) {
            tileInfo.pNext = info.pNext;
            info.pNext = &tileInfo;
            dependency2.srcSubpass = dependency.srcSubpass;
            dependency2.dstSubpass = dependency.dstSubpass;
            dependency2.srcStageMask = dependency.srcStageMask;
            dependency2.dstStageMask = dependency.dstStageMask;
            dependency2.srcAccessMask = dependency.srcAccessMask;
            dependency2.dstAccessMask = dependency.dstAccessMask;
            dependency2.dependencyFlags = dependency.dependencyFlags;
            info.dependencyCount = 1;
            info.pDependencies = &dependency2;
            sub2.flags = sub.flags;
        }
        info.attachmentCount = uint32_t(descriptions.size());
        info.pAttachments = descriptions.data();
        info.subpassCount = 1;
        info.pSubpasses = &sub2;
        info.correlatedViewMaskCount = viewMask ? 1 : 0;
        info.pCorrelatedViewMasks = &viewMask;
        check(d.extensions->createRenderPass2(d.device, &info, nullptr, &pass), "vkCreateRenderPass2");
    }
    try {
        d.renderPassCache.emplace(key, pass);
    } catch (...) {
        vkDestroyRenderPass(d.device, pass, nullptr);
        throw;
    }
    return pass;
}
struct Module {
    Device &d;
    VkShaderModule module = VK_NULL_HANDLE;
    uint32_t entryId = 0;
    bool tileShader = false;
    std::array<uint32_t, 3> tileRate{};
    std::set<uint32_t> tileVariables;
    std::vector<BindingLayout> reflectedBindings;
    std::map<uint32_t, int> inputs, outputs;
    std::vector<std::vector<uint64_t>> outputInterface;
    uint32_t reflectedPushBytes = 0;
    std::array<uint32_t, 3> local{0, 0, 0};
    SpecializationData specializationData;
    VkSpecializationInfo specialization{};
    Module(Device &device, const Shader &shader, uint32_t executionModel) : d(device) {
        const auto &code = shader.code;
        require(code.size() >= 5 && code[0] == 0x07230203 && code[1] >= 0x00010000 &&
                    code[1] <= (d.properties.apiVersion >= VK_API_VERSION_1_3         ? 0x00010600u
                                : d.properties.apiVersion >= VK_API_VERSION_1_2       ? 0x00010500u
                                : (d.enabled & (RayQuery | RayPipeline | MeshShader)) ? 0x00010400u
                                                                                      : 0x00010300u) &&
                    code[4] == 0,
                "SPIR-V version exceeds the device Vulkan version");
        for (size_t i = 5; i < code.size();) {
            uint32_t words = code[i] >> 16, op = code[i] & 0xffff;
            require(words > 0 && words <= code.size() - i, "Malformed SPIR-V instruction");
            if (op == SpvOpCapability && words == 2 && code[i + 1] == SpvCapabilityTileShadingQCOM) {
                require((d.enabledExtra & TileShading) && (executionModel == 4 || executionModel == 5),
                        "Tile shader requires enabled tile shading in compute/fragment stage");
                require(executionModel != 4 || d.extensions->tile.tileShadingFragmentStage,
                        "Fragment tile shading is unavailable");
                tileShader = true;
            }
            if (op == SpvOpCapability && words == 2 && code[i + 1] == SpvCapabilityTensorsARM)
                require((d.enabledExtra & TensorResources) && d.extensions->tensor.shaderTensorAccess,
                        "Tensor shader access was not enabled");
            if (op == SpvOpVariable && words >= 4 && code[i + 3] == SpvStorageClassTileAttachmentQCOM)
                tileVariables.insert(code[i + 2]);
            if (op == SpvOpDecorate && words == 4 && code[i + 2] == SpvDecorationBuiltIn &&
                code[i + 3] == SpvBuiltInPrimitiveShadingRateKHR)
                require(d.enabled & PrimitiveRate, "Primitive shading rate shader feature was not enabled");
            if (op == SpvOpMemoryModel)
                require(
                    words == 3 &&
                        (code[i + 1] == SpvAddressingModelLogical ||
                         ((d.enabled & BufferAddress) && code[i + 1] == SpvAddressingModelPhysicalStorageBuffer64)) &&
                        (code[i + 2] == SpvMemoryModelGLSL450 ||
                         ((d.enabled & MemoryModel) && code[i + 2] == SpvMemoryModelVulkanKHR)),
                    "Shaders must use the Logical / GLSL450 memory model");
            if (op == 15 && words >= 4 && code[i + 1] == executionModel) {
                const auto *name = reinterpret_cast<const char *>(&code[i + 3]);
                const auto *end = static_cast<const char *>(std::memchr(name, 0, (words - 3) * 4));
                require(end != nullptr, "Malformed SPIR-V entry point");
                if (std::string(name, end) == shader.entry)
                    entryId = code[i + 2];
            }
            i += words;
        }
        require(entryId != 0, "Shader entry point not found for the requested stage");
        for (size_t i = 5; tileShader && i < code.size(); i += code[i] >> 16) {
            const auto op = code[i] & 0xffff, words = code[i] >> 16;
            if (op == SpvOpExecutionMode && words == 6 && code[i + 1] == entryId &&
                code[i + 2] == SpvExecutionModeTileShadingRateQCOM) {
                require(executionModel == 5 && d.extensions->tile.tileShadingDispatchTile,
                        "Area tile dispatch is unavailable");
                tileRate = {code[i + 3], code[i + 4], code[i + 5]};
                const auto &limit = d.extensions->tileProperties.maxTileShadingRate;
                require(tileRate[0] && !(tileRate[0] & (tileRate[0] - 1)) && tileRate[0] <= limit.width &&
                            tileRate[1] && !(tileRate[1] & (tileRate[1] - 1)) && tileRate[1] <= limit.height &&
                            tileRate[2],
                        "Tile shading rate exceeds device limits");
            }
            if (op == SpvOpImageTexelPointer)
                require(d.extensions->tile.tileShadingAtomicOps, "Tile image atomics are unavailable");
        }
        if (executionModel == 5 || executionModel == SpvExecutionModelMeshEXT ||
            executionModel == SpvExecutionModelTaskEXT) {
            local = reflectWorkgroupSize(shader, entryId);
            const bool meshStage = executionModel != 5;
            require(!meshStage || (d.enabled & (executionModel == SpvExecutionModelTaskEXT ? TaskShader : MeshShader)),
                    "Mesh/task feature was not enabled");
            const auto &mp = d.extensions->meshProperties;
            const uint32_t *limits = meshStage ? (executionModel == SpvExecutionModelTaskEXT ? mp.maxTaskWorkGroupSize
                                                                                             : mp.maxMeshWorkGroupSize)
                                               : d.properties.limits.maxComputeWorkGroupSize;
            uint32_t invocations = meshStage
                                       ? (executionModel == SpvExecutionModelTaskEXT ? mp.maxTaskWorkGroupInvocations
                                                                                     : mp.maxMeshWorkGroupInvocations)
                                       : d.properties.limits.maxComputeWorkGroupInvocations;
            uint64_t total = 1;
            if (tileRate[0])
                local = {1, 1, 1}; // Workgroup size is chosen by the tile implementation.
            for (int n = 0; n < 3; ++n) {
                require(local[n] && local[n] <= limits[n] && total <= invocations / local[n],
                        "Shader workgroup exceeds device limits");
                total *= local[n];
            }
        }
        SpvReflectShaderModule reflection{};
        require(spvReflectCreateShaderModule(code.size() * 4, code.data(), &reflection) == SPV_REFLECT_RESULT_SUCCESS,
                "SPIR-V reflection failed");
        struct ReflectionGuard {
            SpvReflectShaderModule &module;
            ~ReflectionGuard() { spvReflectDestroyShaderModule(&module); }
        } reflectionGuard{reflection};
        const auto *reflectedEntry = spvReflectGetEntryPoint(&reflection, shader.entry.c_str());
        require(reflectedEntry &&
                    reflectedEntry->spirv_execution_model == static_cast<SpvExecutionModel>(executionModel),
                "Invalid shader stage");
        auto interface = [&](auto &&self, const SpvReflectInterfaceVariable &v, std::map<uint32_t, int> &out) -> void {
            if (v.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN)
                return;
            if (v.member_count) {
                for (uint32_t n = 0; n < v.member_count; ++n)
                    self(self, v.members[n], out);
                return;
            }
            require(v.location != UINT32_MAX, "Shader interface has no location");
            uint64_t locations = std::max(1u, v.numeric.matrix.column_count);
            for (uint32_t n = 0; n < v.array.dims_count; ++n) {
                require(v.array.dims[n] > 0 && locations <= UINT32_MAX / v.array.dims[n], "Invalid interface array");
                locations *= v.array.dims[n];
            }
            if (v.numeric.scalar.width == 64 && v.numeric.vector.component_count > 2)
                locations *= 2;
            require(locations <= 128 && v.location <= UINT32_MAX - locations,
                    "Shader interface exceeds supported location range");
            for (uint32_t n = 0; n < locations; ++n)
                out[v.location + n] = numericClass(VkFormat(v.format));
        };
        if (executionModel == 0)
            for (uint32_t n = 0; n < reflectedEntry->input_variable_count; ++n)
                interface(interface, *reflectedEntry->input_variables[n], inputs);
        if (executionModel == 4)
            for (uint32_t n = 0; n < reflectedEntry->output_variable_count; ++n)
                interface(interface, *reflectedEntry->output_variables[n], outputs);
        if (executionModel == 4) {
            auto outputKey = [&](auto &&self, const SpvReflectInterfaceVariable &v,
                                 std::vector<uint64_t> &key) -> void {
                key.insert(key.end(),
                           {v.location, v.component, uint32_t(v.built_in), v.decoration_flags, v.format,
                            v.numeric.scalar.width, v.numeric.scalar.signedness, v.numeric.vector.component_count,
                            v.numeric.matrix.column_count, v.numeric.matrix.row_count, v.array.dims_count});
                for (uint32_t n = 0; n < v.array.dims_count; ++n)
                    key.push_back(v.array.dims[n]);
                key.push_back(v.member_count);
                for (uint32_t n = 0; n < v.member_count; ++n)
                    self(self, v.members[n], key);
            };
            for (uint32_t n = 0; n < reflectedEntry->output_variable_count; ++n) {
                std::vector<uint64_t> key;
                outputKey(outputKey, *reflectedEntry->output_variables[n], key);
                outputInterface.push_back(std::move(key));
            }
            std::sort(outputInterface.begin(), outputInterface.end());
        }
        specializationData = SpecializationData(shader);
        specialization = specializationData.info();
        bool hasSubgroups = false, hasSmallArithmetic = false;
        for (uint32_t i = 0; i < reflection.capability_count; ++i) {
            const auto cap = reflection.capabilities[i].value;
            uint64_t requiredFeature = 0;
            VkSubgroupFeatureFlags subgroupOperation = 0;
            switch (cap) {
            case SpvCapabilityTileShadingQCOM:
            case SpvCapabilityTensorsARM:
                break;
            case SpvCapabilityStorageTensorArrayDynamicIndexingARM:
                require((d.enabledExtra & TensorResources) &&
                            d.extensions->tensor.shaderStorageTensorArrayDynamicIndexing,
                        "Tensor dynamic indexing was not enabled");
                break;
            case SpvCapabilityStorageTensorArrayNonUniformIndexingARM:
                require((d.enabledExtra & TensorResources) &&
                            d.extensions->tensor.shaderStorageTensorArrayNonUniformIndexing,
                        "Tensor non-uniform indexing was not enabled");
                break;
            case SpvCapabilitySparseResidency:
                require(d.coreFeatures.shaderResourceResidency, "Sparse shader residency was not enabled");
                requiredFeature = SparseResources;
                break;
            case SpvCapabilityMinLod:
                require(d.coreFeatures.shaderResourceMinLod, "Shader minimum LOD was not enabled");
                requiredFeature = SparseResources;
                break;
            case SpvCapabilityCooperativeMatrixKHR:
                require(executionModel == 5, "Cooperative matrix is exposed in compute shaders");
                validateCooperativeShader(d, shader, local);
                requiredFeature = CooperativeMatrix;
                break;
            case SpvCapabilityMatrix:
            case SpvCapabilityShader:
            case SpvCapabilityImageQuery:
            case SpvCapabilityDerivativeControl:
            case SpvCapabilitySampled1D:
            case SpvCapabilityImage1D:
            case SpvCapabilitySampledBuffer:
            case SpvCapabilityImageBuffer:
            case SpvCapabilityInputAttachment:
            case SpvCapabilityImageMSArray:
                break;
            case SpvCapabilityImageGatherExtended:
                requiredFeature = ImageGather;
                break;
            case SpvCapabilityInt8:
                hasSmallArithmetic = true;
                requiredFeature = Int8;
                break;
            case SpvCapabilityFloat64:
                hasSmallArithmetic = true;
                requiredFeature = Float64;
                break;
            case SpvCapabilityStorageBuffer8BitAccess:
                requiredFeature = Storage8;
                break;
            case SpvCapabilityUniformAndStorageBuffer16BitAccess:
                requiredFeature = Uniform16;
                break;
            case SpvCapabilityInt64Atomics:
                requiredFeature = Atomics64;
                break;
            case SpvCapabilityAtomicFloat32AddEXT:
                requiredFeature = FloatAtomics;
                break;
            case SpvCapabilityFragmentShaderPixelInterlockEXT:
                requiredFeature = PixelInterlock;
                break;
            case SpvCapabilityMultiView:
                requiredFeature = Multiview;
                break;
            case SpvCapabilityShaderViewportIndexLayerEXT:
                requiredFeature = ViewportLayer;
                break;
            case SpvCapabilityShaderLayer:
                requiredFeature = ViewportLayer;
                break;
            case SpvCapabilityShaderViewportIndex:
                requiredFeature = ViewportLayer;
                break;
            case SpvCapabilityUniformBufferArrayDynamicIndexing:
                requiredFeature = DynamicIndexing;
                break;
            case SpvCapabilitySampledImageArrayDynamicIndexing:
                requiredFeature = DynamicIndexing;
                break;
            case SpvCapabilityStorageBufferArrayDynamicIndexing:
                requiredFeature = DynamicIndexing;
                break;
            case SpvCapabilityStorageImageArrayDynamicIndexing:
                requiredFeature = DynamicIndexing;
                break;
            case SpvCapabilityStorageImageReadWithoutFormat:
                requiredFeature = StorageRead;
                break;
            case SpvCapabilityStorageImageWriteWithoutFormat:
                requiredFeature = StorageWrite;
                break;
            case SpvCapabilityRuntimeDescriptorArray:
                requiredFeature = DescriptorIndexing;
                break;
            case SpvCapabilityShaderNonUniform:
                require((d.enabled & DescriptorIndexing) ||
                            ((d.enabledExtra & TensorResources) &&
                             d.extensions->tensor.shaderStorageTensorArrayNonUniformIndexing),
                        "Non-uniform descriptor indexing was not enabled");
                break;
            case SpvCapabilitySampledImageArrayNonUniformIndexing:
                requiredFeature = DescriptorIndexing;
                break;
            case SpvCapabilityStorageBufferArrayNonUniformIndexing:
                requiredFeature = DescriptorIndexing;
                break;
            case SpvCapabilityStorageImageArrayNonUniformIndexing:
                requiredFeature = DescriptorIndexing;
                break;
            case SpvCapabilityUniformBufferArrayNonUniformIndexing:
                requiredFeature = DescriptorIndexing;
                break;
            case SpvCapabilityVulkanMemoryModel:
                requiredFeature = MemoryModel;
                break;
            case SpvCapabilityVulkanMemoryModelDeviceScope:
                requiredFeature = MemoryModel;
                break;
            case SpvCapabilityFragmentShadingRateKHR:
                requiredFeature = FragmentRate | PrimitiveRate | AttachmentRate;
                break;
            case SpvCapabilityTessellation:
                requiredFeature = Tessellation;
                break;
            case SpvCapabilityInt64:
                hasSmallArithmetic = true;
                requiredFeature = Int64;
                break;
            case SpvCapabilityPhysicalStorageBufferAddresses:
                requiredFeature = BufferAddress;
                break;
            case SpvCapabilityRayQueryKHR:
                requiredFeature = RayQuery;
                break;
            case SpvCapabilityRayTracingKHR:
                requiredFeature = RayPipeline;
                break;
            case SpvCapabilityRayTracingMotionBlurNV:
                require(d.enabledExtra & RayMotionBlur, "Ray tracing motion blur was not enabled");
                break;
            case SpvCapabilityMeshShadingEXT:
                requiredFeature = MeshShader;
                break;
            case SpvCapabilitySampledCubeArray:
            case SpvCapabilityImageCubeArray:
                requiredFeature = CubeArray;
                break;
            case SpvCapabilityClipDistance:
                requiredFeature = ClipDistance;
                break;
            case SpvCapabilityCullDistance:
                requiredFeature = CullDistance;
                break;
            case SpvCapabilitySampleRateShading:
                requiredFeature = SampleShading;
                break;
            case SpvCapabilityStorageImageMultisample:
                requiredFeature = StorageMs;
                break;
            case SpvCapabilityFloat16:
                requiredFeature = Float16;
                hasSmallArithmetic = true;
                break;
            case SpvCapabilityInt16:
                requiredFeature = Int16;
                hasSmallArithmetic = true;
                break;
            case SpvCapabilityStorageBuffer16BitAccess:
                requiredFeature = Storage16;
                break;
            case SpvCapabilityStorageImageExtendedFormats:
                requiredFeature = ExtendedStorageFormats;
                break;
            case SpvCapabilityGroupNonUniform:
                subgroupOperation = VK_SUBGROUP_FEATURE_BASIC_BIT;
                break;
            case SpvCapabilityGroupNonUniformVote:
                subgroupOperation = VK_SUBGROUP_FEATURE_VOTE_BIT;
                break;
            case SpvCapabilityGroupNonUniformArithmetic:
                subgroupOperation = VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
                break;
            case SpvCapabilityGroupNonUniformBallot:
                subgroupOperation = VK_SUBGROUP_FEATURE_BALLOT_BIT;
                break;
            case SpvCapabilityGroupNonUniformShuffle:
                subgroupOperation = VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
                break;
            case SpvCapabilityGroupNonUniformShuffleRelative:
                subgroupOperation = VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
                break;
            case SpvCapabilityGroupNonUniformClustered:
                subgroupOperation = VK_SUBGROUP_FEATURE_CLUSTERED_BIT;
                break;
            case SpvCapabilityGroupNonUniformQuad:
                require(executionModel == 4 || executionModel == 5 || d.subgroup.quadOperationsInAllStages,
                        "Subgroup quad operations unavailable in this stage");
                subgroupOperation = VK_SUBGROUP_FEATURE_QUAD_BIT;
                break;
            default:
                throw std::invalid_argument("Unsupported SPIR-V capability: " + std::to_string(cap));
            }
            require(!requiredFeature || (d.enabled & requiredFeature),
                    "Shader requires a feature that was not enabled at device creation");
            if (subgroupOperation) {
                hasSubgroups = true;
                require((d.subgroup.supportedStages & reflectedEntry->shader_stage) &&
                            (d.subgroup.supportedOperations & subgroupOperation),
                        "Unsupported subgroup stage/operation");
            }
        }
        require(!(hasSubgroups && hasSmallArithmetic) || (d.enabled & SubgroupExtended),
                "16-bit arithmetic with subgroups requires extended-type support, which is not enabled");
        for (uint32_t set = 0; set < reflectedEntry->descriptor_set_count; ++set) {
            const auto &descriptors = reflectedEntry->descriptor_sets[set];
            require(descriptors.set < d.properties.limits.maxBoundDescriptorSets, "Descriptor set exceeds device limit");
            for (uint32_t i = 0; i < descriptors.binding_count; ++i) {
                const auto &b = *descriptors.bindings[i];
                require(b.count > 0 || (d.enabled & DescriptorIndexing), "Runtime arrays require descriptor indexing");
                BindingLayout binding{b.binding, static_cast<VkDescriptorType>(b.descriptor_type)};
                binding.set = descriptors.set;
                binding.count = b.count;
                binding.runtime = b.count == 0;
                binding.stages = reflectedEntry->shader_stage;
                binding.imageDim = b.image.dim;
                binding.arrayed = b.image.arrayed;
                binding.multisampled = b.image.ms;
                binding.shadow = b.image.depth;
                binding.tile = tileVariables.count(b.spirv_id);
                binding.readonly = (b.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0 ||
                                   binding.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (b.type_description &&
                    (b.type_description->type_flags &
                     (SPV_REFLECT_TYPE_FLAG_EXTERNAL_IMAGE | SPV_REFLECT_TYPE_FLAG_EXTERNAL_SAMPLED_IMAGE))) {
                    const auto &t = *b.type_description;
                    require(t.traits.numeric.scalar.width == 32, "Image sampled components must be 32-bit scalars");
                    binding.numericType = (t.type_flags & SPV_REFLECT_TYPE_FLAG_FLOAT) ? 0
                                          : t.traits.numeric.scalar.signedness         ? 1
                                                                                       : 2;
                }
                if (binding.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                    binding.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                    binding.minimumBytes = b.block.size;
                    if ((executionModel <= 4 || executionModel == SpvExecutionModelMeshEXT ||
                         executionModel == SpvExecutionModelTaskEXT) &&
                        binding.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                        bool readonly = (b.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0;
                        if (!readonly && b.block.member_count)
                            readonly =
                                std::all_of(b.block.members, b.block.members + b.block.member_count, [](const auto &m) {
                                    return (m.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0;
                                });
                        require(readonly || (d.enabled & (executionModel == 4 ? FragmentStores : VertexStores)),
                                "Writable graphics buffers require the corresponding stores/atomics feature");
                    }
                } else if (binding.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                           binding.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                           binding.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                           binding.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
                           binding.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
                    require(b.image.dim == SpvDim1D || b.image.dim == SpvDim2D || b.image.dim == SpvDim3D ||
                                b.image.dim == SpvDimCube || b.image.dim == SpvDimBuffer,
                            "Unsupported image dimension");
                    if (binding.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                        binding.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                        if (executionModel <= 4 || executionModel == SpvExecutionModelMeshEXT ||
                            executionModel == SpvExecutionModelTaskEXT)
                            require((b.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) ||
                                        (d.enabled & (executionModel == 4 ? FragmentStores : VertexStores)),
                                    "Writable graphics images require stores/atomics feature");
                        switch (b.image.image_format) {
                        case SpvImageFormatUnknown:
                            require(d.enabled & (StorageRead | StorageWrite),
                                    "Formatless storage requires explicit feature");
                            break;
                        case SpvImageFormatRgba32i:
                            binding.storageFormat = VK_FORMAT_R32G32B32A32_SINT;
                            break;
                        case SpvImageFormatRgba32ui:
                            binding.storageFormat = VK_FORMAT_R32G32B32A32_UINT;
                            break;
                        case SpvImageFormatRgba16i:
                            binding.storageFormat = VK_FORMAT_R16G16B16A16_SINT;
                            break;
                        case SpvImageFormatRgba16ui:
                            binding.storageFormat = VK_FORMAT_R16G16B16A16_UINT;
                            break;
                        case SpvImageFormatRgba16:
                            binding.storageFormat = VK_FORMAT_R16G16B16A16_UNORM;
                            break;
                        case SpvImageFormatRgba16Snorm:
                            binding.storageFormat = VK_FORMAT_R16G16B16A16_SNORM;
                            break;
                        case SpvImageFormatRgba8Snorm:
                            binding.storageFormat = VK_FORMAT_R8G8B8A8_SNORM;
                            break;
                        case SpvImageFormatRgba8i:
                            binding.storageFormat = VK_FORMAT_R8G8B8A8_SINT;
                            break;
                        case SpvImageFormatRgba8ui:
                            binding.storageFormat = VK_FORMAT_R8G8B8A8_UINT;
                            break;
                        case SpvImageFormatRg32f:
                            binding.storageFormat = VK_FORMAT_R32G32_SFLOAT;
                            break;
                        case SpvImageFormatRg32i:
                            binding.storageFormat = VK_FORMAT_R32G32_SINT;
                            break;
                        case SpvImageFormatRg32ui:
                            binding.storageFormat = VK_FORMAT_R32G32_UINT;
                            break;
                        case SpvImageFormatRg16f:
                            binding.storageFormat = VK_FORMAT_R16G16_SFLOAT;
                            break;
                        case SpvImageFormatRg16:
                            binding.storageFormat = VK_FORMAT_R16G16_UNORM;
                            break;
                        case SpvImageFormatRg16Snorm:
                            binding.storageFormat = VK_FORMAT_R16G16_SNORM;
                            break;
                        case SpvImageFormatRg16i:
                            binding.storageFormat = VK_FORMAT_R16G16_SINT;
                            break;
                        case SpvImageFormatRg16ui:
                            binding.storageFormat = VK_FORMAT_R16G16_UINT;
                            break;
                        case SpvImageFormatRg8:
                            binding.storageFormat = VK_FORMAT_R8G8_UNORM;
                            break;
                        case SpvImageFormatRg8Snorm:
                            binding.storageFormat = VK_FORMAT_R8G8_SNORM;
                            break;
                        case SpvImageFormatRg8i:
                            binding.storageFormat = VK_FORMAT_R8G8_SINT;
                            break;
                        case SpvImageFormatRg8ui:
                            binding.storageFormat = VK_FORMAT_R8G8_UINT;
                            break;
                        case SpvImageFormatR32i:
                            binding.storageFormat = VK_FORMAT_R32_SINT;
                            break;
                        case SpvImageFormatR32ui:
                            binding.storageFormat = VK_FORMAT_R32_UINT;
                            break;
                        case SpvImageFormatR16f:
                            binding.storageFormat = VK_FORMAT_R16_SFLOAT;
                            break;
                        case SpvImageFormatR16:
                            binding.storageFormat = VK_FORMAT_R16_UNORM;
                            break;
                        case SpvImageFormatR16Snorm:
                            binding.storageFormat = VK_FORMAT_R16_SNORM;
                            break;
                        case SpvImageFormatR16i:
                            binding.storageFormat = VK_FORMAT_R16_SINT;
                            break;
                        case SpvImageFormatR16ui:
                            binding.storageFormat = VK_FORMAT_R16_UINT;
                            break;
                        case SpvImageFormatR8:
                            binding.storageFormat = VK_FORMAT_R8_UNORM;
                            break;
                        case SpvImageFormatR8Snorm:
                            binding.storageFormat = VK_FORMAT_R8_SNORM;
                            break;
                        case SpvImageFormatR8i:
                            binding.storageFormat = VK_FORMAT_R8_SINT;
                            break;
                        case SpvImageFormatR8ui:
                            binding.storageFormat = VK_FORMAT_R8_UINT;
                            break;
                        case SpvImageFormatRgb10A2:
                            binding.storageFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
                            break;
                        case SpvImageFormatRgb10a2ui:
                            binding.storageFormat = VK_FORMAT_A2B10G10R10_UINT_PACK32;
                            break;
                        case SpvImageFormatR11fG11fB10f:
                            binding.storageFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
                            break;

                        case SpvImageFormatRgba8:
                            binding.storageFormat = VK_FORMAT_R8G8B8A8_UNORM;
                            break;
                        case SpvImageFormatRgba16f:
                            binding.storageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
                            break;
                        case SpvImageFormatRgba32f:
                            binding.storageFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
                            break;
                        case SpvImageFormatR32f:
                            binding.storageFormat = VK_FORMAT_R32_SFLOAT;
                            break;
                        default:
                            throw std::invalid_argument(
                                "Unsupported storage image format; declare rgba8/rgba16f/rgba32f/r32f");
                        }
                    }
                } else if (binding.type == VK_DESCRIPTOR_TYPE_TENSOR_ARM) {
                    binding.readonly = (b.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0;
                    reflectTensorBinding(d, shader, b.spirv_id, binding);
                    if (executionModel <= 4 || executionModel == SpvExecutionModelMeshEXT ||
                        executionModel == SpvExecutionModelTaskEXT)
                        require(binding.readonly || (d.enabled & (executionModel == 4 ? FragmentStores : VertexStores)),
                                "Writable graphics tensors require the corresponding stores feature");
                } else if (binding.type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
                    require((executionModel == 4 || binding.tile) && b.count == 1,
                            "Input attachment must be a scalar fragment descriptor");
                    binding.inputAttachmentIndex = b.input_attachment_index;
                } else if (binding.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                } else if (binding.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
                    require(d.enabled & (RayQuery | RayPipeline), "Acceleration binding requires ray tracing feature");
                } else
                    throw std::invalid_argument("Unsupported shader descriptor type");
                reflectedBindings.push_back(binding);
            }
        }
        uint32_t pushCount = 0;
        require(spvReflectEnumerateEntryPointPushConstantBlocks(&reflection, shader.entry.c_str(), &pushCount,
                                                                nullptr) == SPV_REFLECT_RESULT_SUCCESS,
                "Cannot reflect push constants");
        std::vector<SpvReflectBlockVariable *> pushes(pushCount);
        require(spvReflectEnumerateEntryPointPushConstantBlocks(&reflection, shader.entry.c_str(), &pushCount,
                                                                pushes.data()) == SPV_REFLECT_RESULT_SUCCESS,
                "Cannot reflect push constants");
        for (const auto *push : pushes)
            for (uint32_t i = 0; i < push->member_count; ++i)
                reflectedPushBytes = std::max(reflectedPushBytes, push->members[i].offset + push->members[i].size);
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * 4;
        info.pCode = code.data();
        check(vkCreateShaderModule(d.device, &info, nullptr, &module), "vkCreateShaderModule");
    }
    ~Module() {
        if (module)
            vkDestroyShaderModule(d.device, module, nullptr);
    }
};
void resolveLayout(Pipeline &pipeline, const std::vector<const Module *> &modules) {
    std::map<uint64_t, BindingLayout> reflected;
    uint32_t pushBytes = 0;
    for (const auto *module : modules) {
        pushBytes = std::max(pushBytes, module->reflectedPushBytes);
        for (const auto &b : module->reflectedBindings) {
            auto [it, inserted] = reflected.emplace(b.location(), b);
            if (!inserted) {
                require(it->second.type == b.type && it->second.storageFormat == b.storageFormat &&
                            it->second.count == b.count && it->second.imageDim == b.imageDim &&
                            it->second.arrayed == b.arrayed && it->second.multisampled == b.multisampled &&
                            it->second.numericType == b.numericType && it->second.tile == b.tile &&
                            it->second.inputAttachmentIndex == b.inputAttachmentIndex,
                        "Shader stages disagree on descriptor type");
                require(!it->second.tensorRank || !b.tensorRank || it->second.tensorRank == b.tensorRank,
                        "Shader stages disagree on tensor rank");
                require(it->second.tensorDimensions.empty() || b.tensorDimensions.empty() ||
                            it->second.tensorDimensions == b.tensorDimensions,
                        "Shader stages disagree on tensor dimensions");
                if (!it->second.tensorRank)
                    it->second.tensorRank = b.tensorRank;
                if (it->second.tensorDimensions.empty())
                    it->second.tensorDimensions = b.tensorDimensions;
                it->second.minimumBytes = std::max(it->second.minimumBytes, b.minimumBytes);
                it->second.stages |= b.stages;
                it->second.readonly = it->second.readonly && b.readonly;
            }
        }
    }
    if (!pipeline.bindings.empty()) {
        require(pipeline.bindings.size() == reflected.size(),
                "Explicit bindings do not match reflected shader bindings");
        std::set<uint64_t> indices;
        for (const auto &b : pipeline.bindings) {
            require(indices.insert(b.location()).second && reflected.count(b.location()) &&
                        reflected.at(b.location()).type == b.type && b.count > 0 &&
                        (reflected.at(b.location()).count == 0 || reflected.at(b.location()).count == b.count),
                    "Explicit binding differs from shader declaration");
            reflected.at(b.location()).immutableSampler = b.immutableSampler;
            if (reflected.at(b.location()).runtime)
                reflected.at(b.location()).count = b.count;
        }
    }
    pipeline.bindings.clear();
    for (const auto &[index, b] : reflected) {
        (void)index;
        require(b.count > 0, "Runtime descriptor array needs an explicit BindingLayout capacity");
        pipeline.bindings.push_back(b);
    }
    if (pipeline.pushBytes == 0)
        pipeline.pushBytes = pushBytes;
    require(pipeline.pushBytes >= pushBytes, "Push constant range is smaller than the shader block");
}
} // namespace

std::shared_ptr<Device> Device::create(uint64_t required, bool validation, bool allowSoftware, uint64_t extra) {
    require((extra >> 16) == 0, "Unknown extended feature");
    if (extra & VertexZeroDivisor)
        extra |= VertexDivisor;
    if (extra & RayMotionBlur)
        required |= RayPipeline;
    if (extra & DataGraph) {
        extra |= TensorResources;
        required |= Timeline;
    }
    if (extra & (TensorResources | DataGraph))
        extra |= Synchronization2;
    if ((extra & (IndependentQueues | HardwareBufferInterop)) == (IndependentQueues | HardwareBufferInterop))
        extra |= Synchronization2;
    require((required >> 60) == 0, "Unknown requested feature");
    if ((required & (RayQuery | RayPipeline)) || (extra & DeviceGeneratedCommands))
        required |= BufferAddress;
    if (required & TaskShader)
        required |= MeshShader;
    if (required & Uniform16)
        required |= Storage16;
    if (required & Atomics64)
        required |= Int64;
    if (required & CooperativeMatrix)
        required |= MemoryModel;
    auto result = std::make_shared<Device>();
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    check(vkEnumerateInstanceVersion(&loaderVersion), "vkEnumerateInstanceVersion");
    require(loaderVersion >= VK_API_VERSION_1_1, "Vulkan 1.1 loader required");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Vulkano";
    app.apiVersion = std::min(loaderVersion, uint32_t(VK_API_VERSION_1_3));
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    std::vector<const char *> instanceExtensions;
#ifdef __ANDROID__
    instanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
#endif
    const char *layer = "VK_LAYER_KHRONOS_validation";
    VkValidationFeatureEnableEXT syncValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    if (validation) {
        uint32_t count = 0;
        check(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate layers");
        std::vector<VkLayerProperties> layers(count);
        check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "enumerate layers");
        require(std::any_of(layers.begin(), layers.end(),
                            [&](const auto &p) { return std::strcmp(p.layerName, layer) == 0; }),
                "Validation layer requested but not installed");
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = &layer;
        uint32_t extensionCount = 0;
        check(vkEnumerateInstanceExtensionProperties(layer, &extensionCount, nullptr),
              "enumerate validation extensions");
        std::vector<VkExtensionProperties> validationExtensions(extensionCount);
        check(vkEnumerateInstanceExtensionProperties(layer, &extensionCount, validationExtensions.data()),
              "enumerate validation extensions");
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
        if (props.apiVersion < VK_API_VERSION_1_1 ||
            (!allowSoftware && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU))
            continue;
        uint32_t ec = 0;
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &ec, nullptr), "enumerate extensions");
        std::vector<VkExtensionProperties> exts(ec);
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &ec, exts.data()), "enumerate extensions");
#ifdef __ANDROID__
        if (!extension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            continue;
#endif
        const bool coreFloat16 = std::min(props.apiVersion, app.apiVersion) >= VK_API_VERSION_1_2;
        const bool float16 = coreFloat16 || extension(exts, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        VkPhysicalDeviceShaderFloat16Int8Features f16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        VkPhysicalDevice16BitStorageFeatures storage16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
        storage16.pNext = float16 ? &f16 : nullptr;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &storage16;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        const auto &f = features.features;
        uint64_t available = 0;
        if (f.samplerAnisotropy)
            available |= Anisotropy;
        if (f.shaderInt16)
            available |= Int16;
        if (storage16.storageBuffer16BitAccess)
            available |= Storage16;
        if (f16.shaderFloat16)
            available |= Float16;
        if (f16.shaderInt8)
            available |= Int8;
        if (storage16.uniformAndStorageBuffer16BitAccess)
            available |= Uniform16;
        if (f.textureCompressionASTC_LDR)
            available |= Astc;
        if (f.textureCompressionETC2)
            available |= Etc2;
        if (f.shaderStorageImageExtendedFormats)
            available |= ExtendedStorageFormats;
#define X(name, field)                                                                                                 \
    if (f.field)                                                                                                       \
        available |= name;
#include "core_features.inc"
#undef X
        if (!(f.shaderUniformBufferArrayDynamicIndexing && f.shaderSampledImageArrayDynamicIndexing &&
              f.shaderStorageBufferArrayDynamicIndexing && f.shaderStorageImageArrayDynamicIndexing))
            available &= ~DynamicIndexing;
        if (f.sparseBinding && (f.sparseResidencyBuffer || f.sparseResidencyImage2D || f.sparseResidencyImage3D))
            available |= SparseResources;
        auto extended = std::make_shared<Extensions>();
        extended->inspect(physical, std::min(props.apiVersion, app.apiVersion), exts);
        available |= extended->available;
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> families(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qc, families.data());
        extended->inspectGraphQueues(result->instance, physical, families);
        auto it = std::find_if(families.begin(), families.end(), [](const auto &q) {
            return q.queueCount > 0 && (q.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                                           (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
        });
        if (it == families.end())
            continue;
        auto sparseQueue = std::find_if(families.begin(), families.end(), [](const auto &q) {
            return q.queueCount && (q.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT);
        });
        if (it->queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)
            sparseQueue = it;
        if (sparseQueue == families.end())
            available &= ~SparseResources;
        uint64_t availableExtra = extended->availableExtra;
        uint64_t queueCount = 0;
        constexpr auto commandQueues = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        for (const auto &q : families)
            if (q.queueFlags & commandQueues)
                queueCount += q.queueCount;
        if (queueCount > 1)
            availableExtra |= IndependentQueues;
        if ((required & available) != required || (extra & availableExtra) != extra)
            continue;
        result->extensions = extended;
        result->sparseFamily = sparseQueue == families.end() ? 0 : uint32_t(sparseQueue - families.begin());
        result->physical = physical;
        result->properties = props;
        result->properties.apiVersion = std::min(props.apiVersion, app.apiVersion);
        result->available = available;
        result->enabled = required;
        result->availableExtra = availableExtra;
        result->enabledExtra = extra;
        result->family = static_cast<uint32_t>(it - families.begin());
        result->timestampBits = it->timestampValidBits;
        result->memoryBudget = extension(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        std::vector<const char *> enabledExtensions;
#ifdef __ANDROID__
        enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#endif
        if (result->memoryBudget)
            enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        if ((required & (Float16 | Int8)) && !coreFloat16)
            enabledExtensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        extended->enable(required, enabledExtensions, extra);
        extended->enableExtra(extra, enabledExtensions);
        std::sort(enabledExtensions.begin(), enabledExtensions.end(),
                  [](auto a, auto b) { return std::strcmp(a, b) < 0; });
        enabledExtensions.erase(std::unique(enabledExtensions.begin(), enabledExtensions.end(),
                                            [](auto a, auto b) { return std::strcmp(a, b) == 0; }),
                                enabledExtensions.end());
        VkPhysicalDeviceFeatures enabledFeatures{};
        enabledFeatures.robustBufferAccess = f.robustBufferAccess;
        enabledFeatures.samplerAnisotropy = (required & Anisotropy) != 0;
        enabledFeatures.shaderInt16 = (required & Int16) != 0;
        enabledFeatures.textureCompressionASTC_LDR = (required & Astc) != 0;
        enabledFeatures.textureCompressionETC2 = (required & Etc2) != 0;
        enabledFeatures.shaderStorageImageExtendedFormats = (required & ExtendedStorageFormats) != 0;
#define X(name, field) enabledFeatures.field = (required & name) != 0;
#include "core_features.inc"
#undef X
        if (required & SparseResources) {
            enabledFeatures.sparseBinding = f.sparseBinding;
            enabledFeatures.sparseResidencyBuffer = f.sparseResidencyBuffer;
            enabledFeatures.sparseResidencyImage2D = f.sparseResidencyImage2D;
            enabledFeatures.sparseResidencyImage3D = f.sparseResidencyImage3D;
            enabledFeatures.sparseResidencyAliased = f.sparseResidencyAliased;
            enabledFeatures.shaderResourceResidency = f.shaderResourceResidency;
            enabledFeatures.shaderResourceMinLod = f.shaderResourceMinLod;
        }
        result->coreFeatures = enabledFeatures;
        VkPhysicalDeviceShaderFloat16Int8Features enableF16{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        enableF16.shaderFloat16 = (required & Float16) != 0;
        enableF16.shaderInt8 = (required & Int8) != 0;
        VkPhysicalDevice16BitStorageFeatures enableStorage{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
        enableStorage.storageBuffer16BitAccess = (required & Storage16) != 0;
        enableStorage.uniformAndStorageBuffer16BitAccess = (required & Uniform16) != 0;
        enableF16.pNext = extended->chain;
        enableStorage.pNext =
            (required & (Float16 | Int8)) &&
                    !(extended->core12 && ((required & (SamplerMinMax | ViewportLayer)) || (extra & DrawIndirectCount)))
                ? &enableF16
                : extended->chain;
        std::map<uint32_t, uint32_t> requestedQueues{{result->family, 1}};
        if (extra & IndependentQueues)
            for (uint32_t f = 0; f < families.size(); ++f)
                if (families[f].queueCount && (families[f].queueFlags & commandQueues))
                    requestedQueues[f] = families[f].queueCount;
        if (extra & DataGraph)
            for (const auto &[f, operations] : extended->graphQueues) {
                (void)operations;
                requestedQueues.emplace(f, 1);
            }
        for (const auto &[f, count] : requestedQueues) {
            (void)count;
            result->resourceFamilies.push_back(f);
        }
        if (required & SparseResources)
            requestedQueues.emplace(result->sparseFamily, 1);
        uint32_t maxQueues = 1;
        for (const auto &[f, count] : requestedQueues) {
            (void)f;
            maxQueues = std::max(maxQueues, count);
        }
        std::vector<float> priorities(maxQueues, 1.0f);
        std::vector<VkDeviceQueueCreateInfo> queueCreates;
        for (const auto &[f, count] : requestedQueues) {
            VkDeviceQueueCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            info.queueFamilyIndex = f;
            info.queueCount = count;
            info.pQueuePriorities = priorities.data();
            queueCreates.push_back(info);
        }
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &enableStorage;
        deviceInfo.pEnabledFeatures = &enabledFeatures;
        deviceInfo.queueCreateInfoCount = uint32_t(queueCreates.size());
        deviceInfo.pQueueCreateInfos = queueCreates.data();
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        deviceInfo.ppEnabledExtensionNames = enabledExtensions.data();
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &result->device), "vkCreateDevice");
        extended->load(*result);
        vkGetDeviceQueue(result->device, result->family, 0, &result->queue);
        result->queues.push_back({result->queue, result->family, 0, families[result->family]});
        for (auto f : result->resourceFamilies)
            for (uint32_t i = 0; i < requestedQueues[f]; ++i) {
                if (f == result->family && i == 0)
                    continue;
                QueueInfo q{VK_NULL_HANDLE, f, i, families[f]};
                vkGetDeviceQueue(result->device, f, i, &q.handle);
                result->queues.push_back(q);
            }
        if (required & SparseResources)
            vkGetDeviceQueue(result->device, result->sparseFamily, 0, &result->sparseQueue);
        vkGetPhysicalDeviceMemoryProperties(physical, &result->memory);
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &result->subgroup;
        vkGetPhysicalDeviceProperties2(physical, &properties2);
        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.physicalDevice = physical;
        allocatorInfo.device = result->device;
        allocatorInfo.instance = result->instance;
        allocatorInfo.vulkanApiVersion = std::min(props.apiVersion, app.apiVersion);
        allocatorInfo.flags = result->memoryBudget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0;
        if (required & BufferAddress)
            allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        VmaVulkanFunctions vmaFunctions{};
        vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        allocatorInfo.pVulkanFunctions = &vmaFunctions;
        check(vmaCreateAllocator(&allocatorInfo, &result->allocator), "vmaCreateAllocator");
        VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        check(vkCreatePipelineCache(result->device, &cacheInfo, nullptr, &result->pipelineCache),
              "vkCreatePipelineCache");
        return result;
    }
    throw std::runtime_error("No Vulkan 1.1 graphics/compute device supports the requested features");
}
Device::~Device() {
    if (device)
        vkDeviceWaitIdle(device);
    clearIdleResources();
    reclaimPresentation(true);
    for (auto [key, pass] : renderPassCache) {
        (void)key;
        vkDestroyRenderPass(device, pass, nullptr);
    }
    if (pipelineCache)
        vkDestroyPipelineCache(device, pipelineCache, nullptr);
    for (const auto &[index, order] : graphOrder) {
        (void)index;
        if (order.semaphore)
            vkDestroySemaphore(device, order.semaphore, nullptr);
    }
    for (size_t i = 0; i < idleCommandCount; ++i)
        vkDestroyCommandPool(device, idleCommands[i].pool, nullptr);
    if (allocator)
        vmaDestroyAllocator(allocator);
    if (device)
        vkDestroyDevice(device, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
}
void Device::collect() {
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [](const auto &weak) {
                                     auto cmd = weak.lock();
                                     return !cmd || cmd->wait(0);
                                 }),
                  pending.end());
    if (pending.empty())
        reclaimPresentation();
}
void Device::waitIdle() {
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
    collect();
}

Buffer::Buffer(std::shared_ptr<Device> device, VkDeviceSize length, VkBufferUsageFlags flags, Storage mode,
               bool writeOnly, std::shared_ptr<Heap> h, VkDeviceSize offset, bool unbound, bool sparseResource)
    : Resource(std::move(device)), size(length), usage(flags), storage(mode), cpuWriteOnly(writeOnly) {
    heap = std::move(h);
    if (heap)
        require(heap->owner() == d.get() && heap->storage == storage, "Heap device/storage mismatch");
    require(!cpuWriteOnly || storage == Storage::Shared, "CPU write-only access requires shared storage");
    require(size > 0 && storage != Storage::Memoryless, "Buffers require positive length and shared/private storage");
    constexpr VkBufferUsageFlags allowed =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
    require(usage != 0 && (usage & ~allowed) == 0, "Unsupported buffer usage");
    require(!(usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) || (d->enabled & BufferAddress),
            "Buffer device address feature was not enabled");
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    d->share(info);
    info.size = size;
    info.usage = usage;
    if (sparseResource) {
        require((d->enabled & SparseResources) && d->coreFeatures.sparseResidencyBuffer && !heap &&
                    storage == Storage::Private &&
                    !(usage & (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                               VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR)),
                "Sparse buffer residency is unavailable or has unsupported usage");
        info.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
        if (d->coreFeatures.sparseResidencyAliased)
            info.flags |= VK_BUFFER_CREATE_SPARSE_ALIASED_BIT;
        require(size <= d->properties.limits.sparseAddressSpaceSize, "Sparse buffer exceeds address space limit");
        check(vkCreateBuffer(d->device, &info, nullptr, &buffer), "create sparse buffer");
        try {
            sparse = std::make_shared<SparseState>(d, buffer);
        } catch (...) {
            vkDestroyBuffer(d->device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            throw;
        }
        return;
    }
    heapOffset = offset;
    require(!offset || (heap && heap->placement()), "An offset requires a placement heap");
    if (unbound || (heap && heap->placement())) {
        check(vkCreateBuffer(d->device, &info, nullptr, &buffer), "create unbound buffer");
        try {
            if (!unbound) {
                bool dedicated;
                const auto requirements = bufferRequirements(*d, buffer, &dedicated);
                heapSpan = heap->validate(requirements, dedicated, offset);
                check(vmaBindBufferMemory2(d->allocator, heap->block, offset, buffer, nullptr), "bind placed buffer");
            }
        } catch (...) {
            vkDestroyBuffer(d->device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            throw;
        }
        return;
    }
    VmaAllocationCreateInfo alloc{};
    alloc.usage = storage == Storage::Shared ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (storage == Storage::Shared) {
        alloc.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT | (cpuWriteOnly ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                                             : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
        alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        alloc.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!cpuWriteOnly)
            alloc.preferredFlags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        else {
            // UMA-friendly direct upload: let VMA score actual memory types.
            // No PCIe, dedicated VRAM or uncached memory requirement.
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            if (usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
                alloc.preferredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
    }
    if (heap)
        alloc.pool = heap->pool;
    check(vmaCreateBuffer(d->allocator, &info, &alloc, &buffer, &allocation, nullptr), "vmaCreateBuffer");
}
Buffer::~Buffer() {
    if (buffer)
        vmaDestroyBuffer(d->allocator, buffer, allocation);
}
void Buffer::write(VkDeviceSize offset, const void *bytes, size_t count) {
    range(size, offset, count);
    require(storage == Storage::Shared, "Private buffers require a blit from shared storage");
    d->collect();
    require(inFlight == 0, "Buffer is in use by the GPU; wait for command completion");
    if (heap && heap->placement())
        require(d->pending.empty(), "Placed CPU access requires completed GPU commands");
    check(vmaCopyMemoryToAllocation(d->allocator, bytes, heap && heap->placement() ? heap->block : allocation,
                                    heapOffset + offset, count),
          "write/flush shared buffer");
}
void Buffer::read(VkDeviceSize offset, void *bytes, size_t count) {
    require(!cpuWriteOnly, "Upload buffers prohibit CPU reads; blit to a shared readback buffer");
    range(size, offset, count);
    require(storage == Storage::Shared, "Private buffers require a blit to shared storage");
    d->collect();
    require(inFlight == 0, "Buffer is in use by the GPU; wait for command completion");
    if (heap && heap->placement())
        require(d->pending.empty(), "Placed CPU access requires completed GPU commands");
    check(vmaCopyAllocationToMemory(d->allocator, heap && heap->placement() ? heap->block : allocation,
                                    heapOffset + offset, bytes, count),
          "invalidate/read shared buffer");
}
void Pipeline::makeLayout() {
    require(pushBytes % 4 == 0 && pushBytes <= d->properties.limits.maxPushConstantsSize,
            "Invalid push constant byte count");
    std::vector<VkDescriptorSetLayoutBinding> vkBindings;
    std::vector<std::vector<VkSampler>> immutableSamplers;
    immutableSamplers.reserve(bindings.size());
    std::set<uint64_t> indices;
    uint32_t setCount = 1;
    std::map<VkDescriptorType, uint64_t> counts;
    for (const auto &b : bindings) {
        require(indices.insert(b.location()).second, "Duplicate descriptor binding");
        require(b.set < d->properties.limits.maxBoundDescriptorSets, "Descriptor set exceeds device limit");
        setCount = std::max(setCount, b.set + 1);
        require(b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                    b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                    b.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR ||
                    b.type == VK_DESCRIPTOR_TYPE_TENSOR_ARM || b.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                    b.type == VK_DESCRIPTOR_TYPE_SAMPLER || b.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                    b.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || b.type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                "Unsupported descriptor type");

        if (b.immutableSampler) {
            require(b.immutableSampler->owner() == d.get() &&
                        (b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b.type == VK_DESCRIPTOR_TYPE_SAMPLER),
                    "Immutable sampler requires a sampler binding on the same device");
            require(!b.immutableSampler->conversion || b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    "YCbCr conversion requires a combined image sampler");
            immutableSamplers.emplace_back(b.count, b.immutableSampler->sampler);
        } else
            immutableSamplers.emplace_back();
        counts[b.type] += b.descriptorCost();
        vkBindings.push_back(
            {b.binding, b.type, b.count, b.stages, b.immutableSampler ? immutableSamplers.back().data() : nullptr});
    }
    const auto &l = d->properties.limits;
    auto checkLimits = [&](const std::map<VkDescriptorType, uint64_t> &counts, bool perStage) {
        auto count = [&](VkDescriptorType t) {
            auto i = counts.find(t);
            return i == counts.end() ? 0ull : i->second;
        };
        uint64_t sampled = count(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) + count(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) +
                           count(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
        uint64_t samplers = count(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) + count(VK_DESCRIPTOR_TYPE_SAMPLER);
        uint64_t storage = count(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) + count(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
        require(sampled <= (perStage ? l.maxPerStageDescriptorSampledImages : l.maxDescriptorSetSampledImages),
                "Too many sampled images/texel buffers");
        require(samplers <= (perStage ? l.maxPerStageDescriptorSamplers : l.maxDescriptorSetSamplers),
                "Too many samplers");
        require(count(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) <=
                    (perStage ? l.maxPerStageDescriptorInputAttachments : l.maxDescriptorSetInputAttachments),
                "Too many input attachments");
        require(storage <= (perStage ? l.maxPerStageDescriptorStorageImages : l.maxDescriptorSetStorageImages),
                "Too many storage images/texel buffers");
        require(count(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) <=
                    (perStage ? l.maxPerStageDescriptorStorageBuffers : l.maxDescriptorSetStorageBuffers),
                "Too many storage buffers");
        require(count(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) <=
                    (perStage ? l.maxPerStageDescriptorUniformBuffers : l.maxDescriptorSetUniformBuffers),
                "Too many uniform buffers");
        const auto &tensor = d->extensions->tensorProperties;
        require(count(VK_DESCRIPTOR_TYPE_TENSOR_ARM) <=
                    (perStage ? tensor.maxPerStageDescriptorSetStorageTensors : tensor.maxDescriptorSetStorageTensors),
                "Too many tensor descriptors");
        const auto &a = d->extensions->accelerationProperties;
        require(
            count(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) <=
                (perStage ? a.maxPerStageDescriptorAccelerationStructures : a.maxDescriptorSetAccelerationStructures),
            "Too many acceleration structures");
        if (perStage) {
            uint64_t total = 0;
            for (const auto &[t, c] : counts)
                if (t != VK_DESCRIPTOR_TYPE_SAMPLER)
                    total += c;
            require(total <= l.maxPerStageResources, "Too many per-stage resources");
        }
    };
    checkLimits(counts, false);
    for (uint32_t stage = 1; stage; stage <<= 1)
        if (stages & stage) {
            std::map<VkDescriptorType, uint64_t> stageCounts;
            for (const auto &b : bindings)
                if (b.stages & stage)
                    stageCounts[b.type] += b.descriptorCost();
            checkLimits(stageCounts, true);
        }
    setLayouts.resize(setCount, VK_NULL_HANDLE);
    for (uint32_t set = 0; set < setCount; ++set) {
        std::vector<VkDescriptorSetLayoutBinding> entries;
        std::vector<VkDescriptorBindingFlags> flags;
        for (size_t i = 0; i < bindings.size(); ++i) {
            if (bindings[i].set != set) continue;
            entries.push_back(vkBindings[i]);
            flags.push_back(bindings[i].runtime ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0);
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo indexing{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        indexing.bindingCount = uint32_t(flags.size());
        indexing.pBindingFlags = flags.data();
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        if (d->enabled & DescriptorIndexing) info.pNext = &indexing;
        info.bindingCount = uint32_t(entries.size());
        info.pBindings = entries.data();
        check(vkCreateDescriptorSetLayout(d->device, &info, nullptr, &setLayouts[set]), "vkCreateDescriptorSetLayout");
    }
    setLayout = setLayouts.front();
    VkPushConstantRange range{stages, 0, pushBytes};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = uint32_t(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = pushBytes ? 1 : 0;
    layoutInfo.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(d->device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
}
Pipeline::Pipeline(std::shared_ptr<Device> device, std::vector<BindingLayout> b, uint32_t p, const Shader &shader,
                   bool indirect)
    : Resource(std::move(device)), bindings(std::move(b)), pushBytes(p), compute(true), indirectBindable(indirect) {
    try {
        stages = VK_SHADER_STAGE_COMPUTE_BIT;
        if (indirectBindable)
            validateIndirectPipeline(*this);
        Module module(*d, shader, 5);
        localSize = module.local;
        tileShader = module.tileShader;
        tileRate = module.tileRate;
        require(!tileShader || !indirectBindable, "Tile compute pipelines cannot be selected by generated commands");
        resolveLayout(*this, {&module});
        makeLayout();
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.layout = layout;
        VkPipelineCreateFlags2CreateInfo flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
        flags.flags = VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT;
        if (indirectBindable)
            info.pNext = &flags;
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      nullptr,
                      0,
                      VK_SHADER_STAGE_COMPUTE_BIT,
                      module.module,
                      shader.entry.c_str(),
                      &module.specialization};
        check(vkCreateComputePipelines(d->device, d->pipelineCache, 1, &info, nullptr, &pipeline),
              "vkCreateComputePipelines");
    } catch (...) {
        if (pipeline)
            vkDestroyPipeline(d->device, pipeline, nullptr);
        if (layout)
            vkDestroyPipelineLayout(d->device, layout, nullptr);
        for (auto set : setLayouts)
            vkDestroyDescriptorSetLayout(d->device, set, nullptr);
        throw;
    }
}
Pipeline::Pipeline(std::shared_ptr<Device> device, std::vector<BindingLayout> b, uint32_t p, const Shader &vertex,
                   const Shader &fragment, VkFormat color, VkFormat depth, bool blend, GraphicsOptions g)
    : Resource(std::move(device)), bindings(std::move(b)), pushBytes(p), compute(false), graphics(std::move(g)),
      colorFormat(color), depthFormat(depth) {
    try {
        auto &g = graphics;
        validateTileOptions(*d, g.tileShading, g.tileApron);
        indirectBindable = g.indirectBindable;
        const auto &l = d->properties.limits;
        if (g.passLayout) {
            validateSubpassLayout(*d, *g.passLayout);
            require(g.subpass < g.passLayout->subpasses.size(), "Invalid subpass index");
            const auto &sub = g.passLayout->subpasses[g.subpass];
            std::vector<VkFormat> outputs;
            for (auto index : sub.colors)
                outputs.push_back(g.passLayout->colors[index]);
            require(outputs == g.colors && g.samples == g.passLayout->samples &&
                        depth == (sub.depth ? g.passLayout->depth : VK_FORMAT_UNDEFINED),
                    "Pipeline formats do not match subpass layout");
        } else
            require(g.subpass == 0, "Subpass index requires a render pass layout");
        if (g.colors.empty() && color != VK_FORMAT_UNDEFINED)
            g.colors.push_back(color);
        require(g.colors.size() <= l.maxColorAttachments, "Too many color attachments");
        require(!g.colors.empty() || depth != VK_FORMAT_UNDEFINED, "Pipeline needs a color or depth format");
        if (g.blends.empty())
            for (size_t i = 0; i < g.colors.size(); ++i) {
                VkPipelineColorBlendAttachmentState a{};
                a.colorWriteMask = 15;
                a.blendEnable = blend;
                a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                g.blends.push_back(a);
            }
        require(g.blends.size() == g.colors.size(), "Blend attachment count mismatch");
        for (size_t n = 0; n < g.colors.size(); ++n) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(d->physical, g.colors[n], &fp);
            require(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "Unsupported color format");
            require(!g.blends[n].blendEnable ||
                        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT),
                    "Format cannot blend");
            const auto &a = g.blends[n];
            const bool dual = a.srcColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
                              a.dstColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
                              a.srcAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
                              a.dstAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR;
            require(!dual || ((d->enabled & DualSourceBlend) && g.colors.size() <= l.maxFragmentDualSrcAttachments),
                    "Dual-source blending exceeds enabled device support");
            if (n && !(d->enabled & IndependentBlend))
                require(std::memcmp(&g.blends[0], &g.blends[n], sizeof(g.blends[n])) == 0,
                        "Different blend states require independentBlend");
        }
        require(g.samples && !(g.samples & (g.samples - 1)) &&
                    (g.colors.empty() || (l.framebufferColorSampleCounts & g.samples)) &&
                    (depth == VK_FORMAT_UNDEFINED || (l.framebufferDepthSampleCounts & g.samples)),
                "Unsupported sample count");
        require(g.topology <= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN || g.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
                "Unsupported primitive topology");
        require(g.viewportCount > 0 && g.viewportCount <= l.maxViewports &&
                    (g.viewportCount == 1 || (d->enabled & MultiViewport)),
                "Invalid/disabled multi viewport");
        require(!g.sampleShading || (d->enabled & SampleShading), "Sample-rate shading feature was not enabled");
        require(g.minSampleShading >= 0 && g.minSampleShading <= 1, "Invalid minimum sample shading");
        require(!g.alphaToOne || (d->enabled & AlphaToOne), "Alpha-to-one feature was not enabled");
        require(!g.logicEnabled || ((d->enabled & LogicOp) && g.logic <= VK_LOGIC_OP_SET),
                "Invalid/disabled logic operation");
        require(!g.depthBounds || ((d->enabled & DepthBounds) && g.minDepthBounds >= 0 && g.maxDepthBounds <= 1 &&
                                   g.minDepthBounds <= g.maxDepthBounds),
                "Invalid/disabled depth bounds");
        require(!g.primitiveRestart || g.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
                    g.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                    g.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
                "Primitive restart requires a strip/fan topology");
        require(g.polygon == VK_POLYGON_MODE_FILL || (d->enabled & NonSolid), "Wireframe feature was not enabled");
        require(!g.depthClamp || (d->enabled & DepthClamp), "Depth clamp feature was not enabled");
        require(g.vertexBindings.size() <= l.maxVertexInputBindings &&
                    g.attributes.size() <= l.maxVertexInputAttributes,
                "Too many vertex bindings/attributes");
        std::set<uint32_t> bindingIds, locations;
        for (const auto &v : g.vertexBindings)
            require(bindingIds.insert(v.binding).second && v.binding < l.maxVertexInputBindings &&
                        v.stride <= l.maxVertexInputBindingStride && v.inputRate <= VK_VERTEX_INPUT_RATE_INSTANCE,
                    "Invalid vertex buffer layout");
        std::set<uint32_t> divisorBindings;
        for (const auto &v : g.vertexDivisors) {
            auto binding = std::find_if(g.vertexBindings.begin(), g.vertexBindings.end(),
                                        [&](const auto &b) { return b.binding == v.binding; });
            require(divisorBindings.insert(v.binding).second && binding != g.vertexBindings.end() &&
                        binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE,
                    "Step rate requires a unique instance buffer binding");
            require((d->enabledExtra & VertexDivisor) && (v.divisor || (d->enabledExtra & VertexZeroDivisor)) &&
                        v.divisor <= d->extensions->vertexDivisorProperties.maxVertexAttribDivisor,
                    "Vertex step rate exceeds enabled device support");
        }
        for (const auto &a : g.attributes) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(d->physical, a.format, &fp);
            require(locations.insert(a.location).second && a.location < l.maxVertexInputAttributes &&
                        bindingIds.count(a.binding) && a.offset <= l.maxVertexInputAttributeOffset &&
                        (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT),
                    "Invalid vertex attribute");
        }
        require(!g.mesh || ((d->enabled & MeshShader) && !g.tessControl && !g.tessEvaluation &&
                            g.vertexBindings.empty() && g.attributes.empty()),
                "Invalid/disabled mesh pipeline");
        Module vs(*d, vertex, g.mesh ? SpvExecutionModelMeshEXT : 0), fs(*d, fragment, 4);
        for (const auto &[location, kind] : vs.inputs) {
            const auto attribute =
                std::find_if(g.attributes.begin(), g.attributes.end(),
                             [location = location](const auto &a) { return a.location == location; });
            require(attribute != g.attributes.end(), "Vertex shader input has no attribute");
            require(numericClass(attribute->format) == kind, "Vertex attribute numeric type differs from shader");
        }
        for (const auto &binding : fs.reflectedBindings)
            if (binding.type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
                require(g.passLayout && binding.inputAttachmentIndex < g.passLayout->subpasses[g.subpass].inputs.size(),
                        "Shader input attachment index is missing in subpass");
        for (const auto &[location, kind] : fs.outputs)
            if (location < g.colors.size())
                require(numericClass(g.colors[location]) == kind,
                        "Fragment output numeric type differs from attachment");
        stages = (g.mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT) | VK_SHADER_STAGE_FRAGMENT_BIT;
        std::unique_ptr<Module> task;
        if (g.task) {
            require(g.mesh && (d->enabled & TaskShader), "Task shader feature was not enabled");
            task = std::make_unique<Module>(*d, *g.task, SpvExecutionModelTaskEXT);
            stages |= VK_SHADER_STAGE_TASK_BIT_EXT;
        }
        std::unique_ptr<Module> tc, te;
        if (g.tessControl || g.tessEvaluation) {
            require(g.tessControl && g.tessEvaluation && (d->enabled & Tessellation) &&
                        g.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST && g.patchPoints > 0 &&
                        g.patchPoints <= l.maxTessellationPatchSize,
                    "Invalid/disabled tessellation configuration");
            tc = std::make_unique<Module>(*d, *g.tessControl, 1);
            te = std::make_unique<Module>(*d, *g.tessEvaluation, 2);
            stages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            resolveLayout(*this, {&vs, &fs, tc.get(), te.get()});
        } else {
            require(g.topology != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, "Patch primitives require tessellation shaders");
            if (task)
                resolveLayout(*this, {&vs, &fs, task.get()});
            else
                resolveLayout(*this, {&vs, &fs});
        }
        makeLayout();
        require(!g.viewMask || ((d->enabled & Multiview) && !g.mesh && !tc && !te),
                "Multiview requires enabled vertex/fragment multiview");
        require(!g.viewMask || (32u - uint32_t(__builtin_clz(g.viewMask))) <=
                                   d->extensions->multiviewProperties.maxMultiviewViewCount,
                "View mask exceeds multiview limit");
        tileShader = fs.tileShader;
        require(!tileShader || g.tileShading, "Fragment tile shader requires tile-enabled pipeline descriptor");
        fragmentInterface = fs.outputInterface;
        generatedStateKey = generatedGraphicsKey(*this);
        if (indirectBindable)
            validateIndirectPipeline(*this);
        Render compatibleRender;
        compatibleRender.tileShading = g.tileShading;
        compatibleRender.tileApron = g.tileApron;
        compatibleRender.rateMapTexelSize = g.rateMapTexelSize;
        compatibleRender.passLayout = g.passLayout;
        compatiblePass = makePass(*d, g.colors, depth, g.samples, {}, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                  VK_ATTACHMENT_STORE_OP_DONT_CARE, g.viewMask, &compatibleRender);
        std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
        auto stage = [&](Module &m, const Shader &s, VkShaderStageFlagBits flag) {
            shaderStages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, flag, m.module,
                                    s.entry.c_str(), &m.specialization});
        };
        stage(vs, vertex, g.mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT);
        if (task)
            stage(*task, *g.task, VK_SHADER_STAGE_TASK_BIT_EXT);
        stage(fs, fragment, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (tc) {
            stage(*tc, *g.tessControl, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
            stage(*te, *g.tessEvaluation, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
        }
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = uint32_t(g.vertexBindings.size());
        vi.pVertexBindingDescriptions = g.vertexBindings.data();
        vi.vertexAttributeDescriptionCount = uint32_t(g.attributes.size());
        vi.pVertexAttributeDescriptions = g.attributes.data();
        VkPipelineVertexInputDivisorStateCreateInfoKHR divisors{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR};
        divisors.vertexBindingDivisorCount = uint32_t(g.vertexDivisors.size());
        divisors.pVertexBindingDivisors = g.vertexDivisors.data();
        if (!g.vertexDivisors.empty())
            vi.pNext = &divisors;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = g.topology;
        ia.primitiveRestartEnable = g.primitiveRestart;
        VkPipelineTessellationStateCreateInfo tess{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        tess.patchControlPoints = g.patchPoints;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = g.viewportCount;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = g.polygon;
        raster.lineWidth = 1;
        raster.frontFace = g.frontFace;
        raster.cullMode = g.cull;
        raster.depthClampEnable = g.depthClamp;
        raster.rasterizerDiscardEnable = g.rasterizationDisabled;
        raster.depthBiasEnable = VK_TRUE;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = g.samples;
        ms.alphaToCoverageEnable = g.alphaToCoverage;
        ms.sampleShadingEnable = g.sampleShading;
        ms.minSampleShading = g.minSampleShading;
        ms.alphaToOneEnable = g.alphaToOne;
        ms.pSampleMask = g.sampleMask.data();
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = uint32_t(g.blends.size());
        cb.pAttachments = g.blends.data();
        cb.logicOpEnable = g.logicEnabled;
        cb.logicOp = g.logic;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = depth != VK_FORMAT_UNDEFINED && g.depthTest;
        ds.depthWriteEnable = depth != VK_FORMAT_UNDEFINED && g.depthWrite;
        ds.depthCompareOp = g.depthCompare;
        ds.stencilTestEnable = g.stencilTest;
        ds.depthBoundsTestEnable = g.depthBounds;
        ds.minDepthBounds = g.minDepthBounds;
        ds.maxDepthBounds = g.maxDepthBounds;
        ds.front = g.front;
        ds.back = g.back;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                   VK_DYNAMIC_STATE_SCISSOR,
                                   VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                   VK_DYNAMIC_STATE_DEPTH_BIAS,
                                   VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                   VK_DYNAMIC_STATE_LINE_WIDTH,
                                   VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = (d->enabledExtra & DeviceGeneratedCommands) && d->extensions->generatedVertexInput &&
                                        !g.mesh && !g.vertexBindings.empty()
                                    ? 7
                                    : 6;
        dyn.pDynamicStates = states;
        VkPipelineFragmentShadingRateStateCreateInfoKHR rate{
            VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR};
        rate.fragmentSize = g.fragmentSize;
        rate.combinerOps[0] = g.primitiveRateCombiner;
        rate.combinerOps[1] = g.attachmentRateCombiner;
        const bool useRate = g.fragmentSize.width != 1 || g.fragmentSize.height != 1 ||
                             g.primitiveRateCombiner != VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR ||
                             g.attachmentRateCombiner != VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        require(!useRate || (d->enabled & (FragmentRate | PrimitiveRate | AttachmentRate)),
                "Fragment shading rate was not enabled");
        if (useRate) {
            require((d->enabled & FragmentRate) || (g.fragmentSize.width == 1 && g.fragmentSize.height == 1),
                    "Pipeline fragment shading rate was not enabled");
            require(g.primitiveRateCombiner == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR ||
                        (d->enabled & PrimitiveRate),
                    "Primitive shading rate was not enabled");
            require(g.attachmentRateCombiner == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR ||
                        ((d->enabled & AttachmentRate) && g.rateMapTexelSize.width),
                    "Rate map combiner requires a rate map layout");
            require(g.attachmentRateCombiner <= VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR &&
                        (g.attachmentRateCombiner <= VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR ||
                         d->extensions->fragmentRateProperties.fragmentShadingRateNonTrivialCombinerOps),
                    "Unsupported rate map combiner");
            require(g.primitiveRateCombiner <= VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR &&
                        (g.primitiveRateCombiner <= VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR ||
                         d->extensions->fragmentRateProperties.fragmentShadingRateNonTrivialCombinerOps),
                    "Unsupported shading rate combiner");
            require(std::any_of(d->extensions->fragmentRates.begin(), d->extensions->fragmentRates.end(),
                                [&](const auto &r) {
                                    return r.fragmentSize.width == g.fragmentSize.width &&
                                           r.fragmentSize.height == g.fragmentSize.height &&
                                           (r.sampleCounts & g.samples);
                                }),
                    "Unsupported fragment size/sample count combination");
            require(!g.mesh || g.primitiveRateCombiner == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
                    "Mesh primitive shading rate requires additional mesh feature");
        }
        VkGraphicsPipelineCreateInfo i{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        if (useRate)
            i.pNext = &rate;
        VkPipelineCreateFlags2CreateInfo flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
        flags.flags = VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT;
        if (indirectBindable) {
            flags.pNext = i.pNext;
            i.pNext = &flags;
        }
        i.stageCount = uint32_t(shaderStages.size());
        i.pStages = shaderStages.data();
        i.pVertexInputState = g.mesh ? nullptr : &vi;
        i.pInputAssemblyState = g.mesh ? nullptr : &ia;
        i.pTessellationState = tc ? &tess : nullptr;
        i.pViewportState = &viewport;
        i.pRasterizationState = &raster;
        i.pMultisampleState = &ms;
        i.pDepthStencilState = &ds;
        i.pColorBlendState = &cb;
        i.pDynamicState = &dyn;
        i.layout = layout;
        i.renderPass = compatiblePass;
        i.subpass = g.subpass;
        check(vkCreateGraphicsPipelines(d->device, d->pipelineCache, 1, &i, nullptr, &pipeline),
              "vkCreateGraphicsPipelines");
    } catch (...) {
        if (pipeline)
            vkDestroyPipeline(d->device, pipeline, nullptr);
        if (layout)
            vkDestroyPipelineLayout(d->device, layout, nullptr);
        for (auto set : setLayouts)
            vkDestroyDescriptorSetLayout(d->device, set, nullptr);
        throw;
    }
}
RayTracingPipeline::RayTracingPipeline(std::shared_ptr<Device> device, std::vector<BindingLayout> b, uint32_t push,
                                       const std::vector<Shader> &shaders,
                                       const std::vector<VkShaderStageFlagBits> &stageFlags,
                                       const std::vector<VkRayTracingShaderGroupCreateInfoKHR> &groups,
                                       uint32_t recursion, bool indirect, bool motion)
    : Pipeline(std::move(device), std::move(b), push), supportsMotion(motion) {
    require(d->enabled & RayPipeline, "Ray-tracing pipeline feature was not enabled");
    require(!motion || (d->enabledExtra & RayMotionBlur), "Ray tracing motion blur was not enabled");
    require(!shaders.empty() && shaders.size() == stageFlags.size() && !groups.empty(),
            "Invalid ray pipeline shader groups");
    require(recursion > 0 && recursion <= d->extensions->rayProperties.maxRayRecursionDepth,
            "Ray recursion exceeds device limit");
    rayTracing = true;
    indirectBindable = indirect;
    generatedStateKey = {recursion, uint32_t(motion)};
    std::vector<std::unique_ptr<Module>> modules;
    std::vector<const Module *> reflection;
    std::vector<VkPipelineShaderStageCreateInfo> vkStages;
    for (size_t n = 0; n < shaders.size(); ++n) {
        const auto &code = shaders[n].code;
        for (size_t i = 5; i < code.size();) {
            const auto words = code[i] >> 16;
            require(words && words <= code.size() - i, "Malformed ray SPIR-V instruction");
            if ((code[i] & 0xffff) == SpvOpCapability && words == 2 &&
                code[i + 1] == SpvCapabilityRayTracingMotionBlurNV)
                require(motion, "The pipeline must enable supportsMotionBlur for motion shaders");
            i += words;
        }
        uint32_t model = 0;
        switch (stageFlags[n]) {
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            model = SpvExecutionModelRayGenerationKHR;
            break;
        case VK_SHADER_STAGE_MISS_BIT_KHR:
            model = SpvExecutionModelMissKHR;
            break;
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            model = SpvExecutionModelClosestHitKHR;
            break;
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            model = SpvExecutionModelAnyHitKHR;
            break;
        case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
            model = SpvExecutionModelIntersectionKHR;
            break;
        case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
            model = SpvExecutionModelCallableKHR;
            break;
        default:
            throw std::invalid_argument("Invalid ray shader stage");
        }
        auto m = std::make_unique<Module>(*d, shaders[n], model);
        reflection.push_back(m.get());
        vkStages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, stageFlags[n], m->module,
                            shaders[n].entry.c_str(), &m->specialization});
        stages |= stageFlags[n];
        modules.push_back(std::move(m));
    }
    std::array<std::vector<uint32_t>, 4> categories;
    auto stageIs = [&](uint32_t index, VkShaderStageFlagBits flag) {
        return index == VK_SHADER_UNUSED_KHR || (index < stageFlags.size() && stageFlags[index] == flag);
    };
    for (uint32_t n = 0; n < groups.size(); ++n) {
        const auto &g = groups[n];
        if (g.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR) {
            require(g.generalShader < stageFlags.size() && g.closestHitShader == VK_SHADER_UNUSED_KHR &&
                        g.anyHitShader == VK_SHADER_UNUSED_KHR && g.intersectionShader == VK_SHADER_UNUSED_KHR,
                    "Invalid general ray group");
            auto flag = stageFlags[g.generalShader];
            require(flag == VK_SHADER_STAGE_RAYGEN_BIT_KHR || flag == VK_SHADER_STAGE_MISS_BIT_KHR ||
                        flag == VK_SHADER_STAGE_CALLABLE_BIT_KHR,
                    "Invalid general ray stage");
            categories[flag == VK_SHADER_STAGE_RAYGEN_BIT_KHR ? 0
                       : flag == VK_SHADER_STAGE_MISS_BIT_KHR ? 1
                                                              : 3]
                .push_back(n);
        } else {
            require(g.generalShader == VK_SHADER_UNUSED_KHR &&
                        stageIs(g.closestHitShader, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR) &&
                        stageIs(g.anyHitShader, VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
                    "Invalid hit group");
            require((g.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR &&
                     g.intersectionShader == VK_SHADER_UNUSED_KHR) ||
                        (g.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR &&
                         g.intersectionShader != VK_SHADER_UNUSED_KHR &&
                         stageIs(g.intersectionShader, VK_SHADER_STAGE_INTERSECTION_BIT_KHR)),
                    "Invalid intersection group");
            categories[2].push_back(n);
        }
    }
    require(categories[0].size() == 1, "Exactly one ray-generation group is required");
    resolveLayout(*this, reflection);
    makeLayout();
    VkRayTracingPipelineCreateInfoKHR i{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    if (motion)
        i.flags |= VK_PIPELINE_CREATE_RAY_TRACING_ALLOW_MOTION_BIT_NV;
    VkPipelineCreateFlags2CreateInfo flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
    flags.flags = VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT | i.flags;
    if (indirectBindable) {
        validateIndirectPipeline(*this);
        i.pNext = &flags;
    }
    i.stageCount = uint32_t(vkStages.size());
    i.pStages = vkStages.data();
    i.groupCount = uint32_t(groups.size());
    i.pGroups = groups.data();
    i.maxPipelineRayRecursionDepth = recursion;
    i.layout = layout;
    check(d->extensions->createRayPipelines(d->device, VK_NULL_HANDLE, d->pipelineCache, 1, &i, nullptr, &pipeline),
          "vkCreateRayTracingPipelinesKHR");
    const auto &props = d->extensions->rayProperties;
    auto align = [](uint64_t n, uint64_t a) { return (n + a - 1) & ~(a - 1); };
    uint64_t stride = align(props.shaderGroupHandleSize, props.shaderGroupHandleAlignment);
    require(stride <= props.maxShaderGroupStride, "Shader group stride exceeds device limit");
    std::array<uint64_t, 4> offsets{};
    uint64_t total = 0;
    for (int n = 0; n < 4; ++n) {
        total = align(total, props.shaderGroupBaseAlignment);
        offsets[n] = total;
        total += stride * categories[n].size();
    }
    table = std::make_shared<Buffer>(d, total + props.shaderGroupBaseAlignment,
                                     VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     Storage::Shared, true);
    auto base = bufferAddress(*table);
    auto shift = align(base, props.shaderGroupBaseAlignment) - base;
    std::vector<uint8_t> handles(groups.size() * props.shaderGroupHandleSize);
    check(d->extensions->getGroupHandles(d->device, pipeline, 0, groups.size(), handles.size(), handles.data()),
          "vkGetRayTracingShaderGroupHandlesKHR");
    std::vector<uint8_t> data(total);
    VkStridedDeviceAddressRegionKHR *regions[] = {&raygen, &miss, &hit, &callable};
    for (int n = 0; n < 4; ++n) {
        for (size_t k = 0; k < categories[n].size(); ++k)
            std::memcpy(data.data() + offsets[n] + stride * k,
                        handles.data() + categories[n][k] * props.shaderGroupHandleSize, props.shaderGroupHandleSize);
        if (!categories[n].empty())
            *regions[n] = {base + shift + offsets[n], stride, stride * categories[n].size()};
    }
    table->write(shift, data.data(), data.size());
}
static void resolveSamplers(const Pipeline &p, std::vector<Binding> &bindings) {
    for (auto &b : bindings)
        for (const auto &schema : p.bindings)
            if (b.location() == schema.location() && schema.immutableSampler && !b.sampler)
                b.sampler = schema.immutableSampler;
}
void Command::trace(std::shared_ptr<RayTracingPipeline> p, std::vector<Binding> bs, std::vector<uint8_t> constants,
                    std::array<uint32_t, 3> size) {
    recording();
    requireQueue(VK_QUEUE_COMPUTE_BIT);
    require(p && p->rayTracing, "Ray pipeline required");
    resolveSamplers(*p, bs);
    validateBindings(*p, bs, constants);
    uint64_t total = 1;
    for (auto n : size) {
        require(n && total <= d->extensions->rayProperties.maxRayDispatchInvocationCount / n,
                "Ray dispatch exceeds device limit");
        total *= n;
    }
    for (const auto &b : bs)
        if (b.texel)
            buffers.push_back(b.texel->buffer);
    for (auto &b : bs) {
        if (b.buffer)
            buffers.push_back(b.buffer);
        if (b.texel)
            buffers.push_back(b.texel->buffer);
    }
    buffers.push_back(p->table);
    operations.push_back([p, bs = std::move(bs), constants = std::move(constants), size](Command &c) {
        c.barrier(VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
        c.prepare(*p, bs, true);
        c.bind(*p, bs, constants);
        c.d->extensions->traceRays(c.command, &p->raygen, &p->miss, &p->hit, &p->callable, size[0], size[1], size[2]);
    });
}
Pipeline::~Pipeline() {
    if (pipeline)
        vkDestroyPipeline(d->device, pipeline, nullptr);
    if (layout)
        vkDestroyPipelineLayout(d->device, layout, nullptr);
    for (auto set : setLayouts)
        vkDestroyDescriptorSetLayout(d->device, set, nullptr);
}

Command::Command(std::shared_ptr<Device> device, uint32_t index) : Resource(std::move(device)), queueIndex(index) {
    require(index < d->queues.size(), "Command queue index is unavailable; enable INDEPENDENT_QUEUES first");
}
void Command::requireQueue(VkQueueFlags any) const {
    require(queueInfo().properties.queueFlags & any, "Operation is unsupported by this command queue");
}
void Command::recording() const {
    require(state == State::Recording, "Command buffer is not recording (one submission only)");
}
void Command::barrier(VkPipelineStageFlags destination) {
    // Graph-only families do not support vkCmdPipelineBarrier2. Their operations
    // are split into semaphore-ordered submissions at commit time instead.
    if (graphOnly())
        return;
    if (!(destination & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) ++scopedBarrierCount;
    if (d->enabledExtra & Synchronization2) {
        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        memory.dstStageMask = destination;
        memory.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &memory;
        d->extensions->pipelineBarrier2(command, &dependency);
        return;
    }
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         destination, 0, 1, &memory, 0, nullptr, 0,
                         nullptr);
}
void Command::validateBindings(const Pipeline &p, const std::vector<Binding> &bs,
                               const std::vector<uint8_t> &constants) {
    same(*this, p);
    require(constants.size() == p.pushBytes, "Set exactly the pipeline's declared push constant bytes");
    for (const auto &schema : p.bindings) {
        if (schema.runtime || (schema.type == VK_DESCRIPTOR_TYPE_SAMPLER && schema.immutableSampler))
            continue; // Unbound runtime elements MUST NOT be accessed by a shader.
        size_t count = std::count_if(bs.begin(), bs.end(), [&](const Binding &b) { return b.location() == schema.location(); });
        require(count == schema.count, "Every fixed binding/array element must be supplied");
    }
    std::set<std::pair<uint64_t, uint32_t>> found;
    for (const auto &b : bs) {
        require(found.insert({b.location(), b.element}).second, "Duplicate binding");
        auto schema =
            std::find_if(p.bindings.begin(), p.bindings.end(), [&](const auto &s) { return s.location() == b.location(); });
        require(schema != p.bindings.end() && b.element < schema->count,
                "Binding/array element is not declared in pipeline");
        require(!schema->immutableSampler || b.sampler == schema->immutableSampler,
                "Binding sampler differs from the immutable pipeline sampler");
        require(!b.tensor || schema->type == VK_DESCRIPTOR_TYPE_TENSOR_ARM, "Unexpected tensor binding");
        const auto &limits = d->properties.limits;
        if (schema->type == VK_DESCRIPTOR_TYPE_TENSOR_ARM) {
            require(b.tensor && !b.buffer && !b.texture && !b.sampler && !b.texel && !b.acceleration,
                    "Tensor view binding required");
            same(*this, *b.tensor);
            const auto &tensor = *b.tensor->tensor;
            require(tensor.options.usage & VK_TENSOR_USAGE_SHADER_BIT_ARM, "Tensor was not created for shader access");
            require(b.tensor->format == schema->storageFormat, "Tensor element type differs from shader");
            require(!schema->tensorRank || tensor.options.dimensions.size() == schema->tensorRank,
                    "Tensor rank differs from shader");
            require(schema->tensorDimensions.empty() || tensor.options.dimensions == schema->tensorDimensions,
                    "Tensor dimensions differ from shader");
        } else if (schema->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                   schema->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            require(b.buffer && !b.texture && !b.sampler, "Binding requires a buffer");
            same(*this, *b.buffer);
            range(b.buffer->size, b.offset, b.length);
            require(b.length >= schema->minimumBytes, "Buffer range is smaller than the shader block");
            const bool uniform = schema->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            require((b.buffer->usage &
                     (uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) != 0,
                    "Buffer usage does not match binding");
            require(b.offset % (uniform ? limits.minUniformBufferOffsetAlignment
                                        : limits.minStorageBufferOffsetAlignment) ==
                        0,
                    "Buffer binding offset is not aligned");
            require(b.length <= (uniform ? limits.maxUniformBufferRange : limits.maxStorageBufferRange),
                    "Buffer binding exceeds device range limit");
        } else if (schema->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
            require(b.texture && !b.sampler && !b.buffer && !b.texel && !b.acceleration &&
                        (b.texture->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) && b.texture->options.mipLevels == 1 &&
                        b.texture->imageType() == VK_IMAGE_TYPE_2D &&
                        schema->multisampled == (b.texture->options.samples > 1),
                    "Invalid input attachment binding");
            same(*this, *b.texture);
            b.texture->usable();
            require(b.texture->viewNumericClass() == schema->numericType,
                    "Input attachment numeric type differs from shader");
        } else if (schema->type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            require(b.sampler && !b.texture && !b.buffer && !b.texel && !b.acceleration, "Sampler binding required");
            same(*this, *b.sampler);
        } else if (schema->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                   schema->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            require(b.texel && !b.buffer && !b.texture && !b.acceleration && !b.sampler,
                    "Texture buffer binding required");
            same(*this, *b.texel);
            require(numericClass(b.texel->format) == schema->numericType,
                    "Texel buffer numeric type differs from shader");
            require(b.texel->writable == (schema->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER),
                    "Texture buffer read/write type mismatch");
            require(!b.texel->writable ||
                        (schema->storageFormat == VK_FORMAT_UNDEFINED || b.texel->format == schema->storageFormat),
                    "Texel format differs from shader");
        } else if (schema->type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
            require(b.acceleration && !b.buffer && !b.texture && !b.sampler &&
                        b.acceleration->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                    "Binding requires instance acceleration structure");
            same(*this, *b.acceleration);
            if (b.acceleration->flags & VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV) {
                const auto *ray = dynamic_cast<const RayTracingPipeline *>(&p);
                require(ray && ray->supportsMotion, "Motion structures require a motion-enabled ray tracing pipeline");
            }
        } else {
            require(b.texture && !b.buffer, "Binding requires a texture");
            same(*this, *b.texture);
            b.texture->usable();
            require(b.texture->viewNumericClass() == schema->numericType, "Texture numeric type differs from shader");
            const bool sampled = schema->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                                 schema->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            require(sampled || (b.texture->options.mipLevels == 1 && (schema->storageFormat == VK_FORMAT_UNDEFINED ||
                                                                      b.texture->format == schema->storageFormat)),
                    "Storage texture format differs from the shader declaration");
            require((b.texture->usage & (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT)) != 0,
                    "Texture usage does not match binding");
            require((schema->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) == bool(b.sampler),
                    "Sampled textures require a sampler; storage textures must not have one");
            const auto &t = *b.texture;
            require(!t.external || !t.external->conversionSampler || b.sampler,
                    "Converted images require combined image sampler descriptors");
            const uint32_t dim =
                t.imageType() == VK_IMAGE_TYPE_1D                                                                ? 0
                : t.imageType() == VK_IMAGE_TYPE_3D                                                              ? 2
                : (t.options.type == VK_IMAGE_VIEW_TYPE_CUBE || t.options.type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ? 3
                                                                                                                 : 1;
            const bool arrayed = t.options.type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
                                 t.options.type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
                                 t.options.type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            require(schema->imageDim == dim && schema->arrayed == arrayed &&
                        schema->multisampled == (t.options.samples > 1),
                    "Shader image type does not match texture view");
            if (b.sampler) {
                require(!schema->shadow || (t.viewAspects() == VK_IMAGE_ASPECT_DEPTH_BIT && b.sampler->compare),
                        "Shadow sampling requires depth and comparison sampler");
                same(*this, *b.sampler);
                const auto features = t.formatFeatures();
                const auto expected = t.external && t.external->conversionSampler
                                          ? t.external->conversionSampler->conversion
                                          : VK_NULL_HANDLE;
                require(b.sampler->conversion == expected && (!expected || schema->immutableSampler),
                        "YCbCr textures require a matching immutable conversion sampler");
                require(!b.sampler->linear || (t.viewAspects() != VK_IMAGE_ASPECT_STENCIL_BIT &&
                                               (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)),
                        "Texture format does not support linear filtering");
                require(b.sampler->reductionMode == VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE ||
                            (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT),
                        "Texture format does not support min/max filtering");
            }
        }
    }
}
void Command::prepare(const Pipeline &p, const std::vector<Binding> &bindings, bool compute) {
    for (const auto &b : bindings) {
        if (b.tensor)
            tensors.push_back(b.tensor->tensor);
        if (b.acceleration) {
            auto found = accelerationStates.find(b.acceleration.get());
            require(found == accelerationStates.end() ? b.acceleration->built : found->second,
                    "Build acceleration structure before tracing");
        }
        if (b.texture) {
            // Keep GENERAL for textures that can alias sampled/storage descriptors.
            const auto layout = b.imageLayout != VK_IMAGE_LAYOUT_UNDEFINED ? b.imageLayout : (b.texture->usage & VK_IMAGE_USAGE_STORAGE_BIT)
                                    ? VK_IMAGE_LAYOUT_GENERAL
                                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            auto schema =
                std::find_if(p.bindings.begin(), p.bindings.end(), [&](const auto &s) { return s.location() == b.location(); });
            if (schema->tile || schema->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
                continue;
            const bool sampled = schema->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                                 schema->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            transition(*b.texture, layout, !compute || sampled, b.texture->viewAspects());
            if (compute && !sampled)
                markInitialized(*b.texture, true);
        }
    }
}
void Command::bind(const Pipeline &p, const std::vector<Binding> &bs, const std::vector<uint8_t> &constants) {
    const auto point = p.rayTracing ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR
                       : p.compute  ? VK_PIPELINE_BIND_POINT_COMPUTE
                                    : VK_PIPELINE_BIND_POINT_GRAPHICS;
    auto &bound = boundPipelines[p.rayTracing ? 2 : p.compute ? 0 : 1];
    if (bound != p.pipeline) {
        vkCmdBindPipeline(command, point, p.pipeline);
        bound = p.pipeline;
    }
    for (uint32_t setIndex = 0; setIndex < p.setLayouts.size(); ++setIndex) {
        if (std::none_of(p.bindings.begin(), p.bindings.end(), [&](const auto &b) { return b.set == setIndex; })) continue;
        // Canonical binding order; offsets, ranges, sampler and layout identity
        // are part of the key. Push constants deliberately are not.
        std::vector<const Binding *> ordered;
        for (const auto &binding : bs)
            if (binding.set == setIndex) ordered.push_back(&binding);
        std::sort(ordered.begin(), ordered.end(),
                  [](auto a, auto b) { return std::tie(a->index, a->element) < std::tie(b->index, b->element); });
        std::vector<uint64_t> key{reinterpret_cast<uintptr_t>(&p), setIndex};
        for (auto b : ordered) {
            key.insert(key.end(),
                       {b->index, b->element, reinterpret_cast<uintptr_t>(b->buffer.get()), b->offset, b->length,
                        reinterpret_cast<uintptr_t>(b->texture.get()), reinterpret_cast<uintptr_t>(b->sampler.get()),
                        reinterpret_cast<uintptr_t>(b->acceleration.get()), reinterpret_cast<uintptr_t>(b->texel.get()),
                        reinterpret_cast<uintptr_t>(b->tensor.get()), uint64_t(b->imageLayout)});
        }
        VkDescriptorSet set;
        auto cached = descriptorSets.find(key);
        if (cached != descriptorSets.end()) {
            set = cached->second;
            ++descriptorCacheHits;
        } else {
            uint64_t descriptors = 0;
            for (const auto &b : p.bindings)
                if (b.set == setIndex) descriptors += b.descriptorCost();
            const uint32_t setsPerPool = uint32_t(std::max(
                1ull, std::min(64ull, 4096ull / std::max(1ull, static_cast<unsigned long long>(descriptors)))));
            auto &arena = descriptorArenas[{&p, setIndex}];
            if (!arena.pool || arena.used == setsPerPool) {
                std::map<VkDescriptorType, uint32_t> counts;
                for (const auto &b : p.bindings)
                    if (b.set == setIndex) counts[b.type] += setsPerPool * b.descriptorCost();
                std::vector<VkDescriptorPoolSize> sizes;
                for (auto [type, count] : counts)
                    sizes.push_back({type, count});
                descriptorPools.reserve(descriptorPools.size() + 1);
                descriptorAllocations.reserve(descriptorAllocations.size() + 1);
                auto allocation = d->takeDescriptorPool(setsPerPool, sizes);
                const auto dp = allocation.pool;
                descriptorPools.push_back(dp);
                descriptorAllocations.push_back(std::move(allocation));
                arena = {dp, 0};
            }
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = arena.pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &p.setLayouts[setIndex];
            check(vkAllocateDescriptorSets(d->device, &ai, &set), "vkAllocateDescriptorSets");
            ++arena.used;
            std::vector<VkDescriptorBufferInfo> buffersInfo(ordered.size());
            std::vector<VkDescriptorImageInfo> imageInfo(ordered.size());
            std::vector<VkWriteDescriptorSet> writes(ordered.size());
            std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelerationInfo(ordered.size());
            std::vector<VkWriteDescriptorSetTensorARM> tensorInfo(ordered.size());
            for (size_t i = 0; i < ordered.size(); ++i) {
                const auto &b = *ordered[i];
                auto &w = writes[i];
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = set;
                w.dstBinding = b.index;
                w.dstArrayElement = b.element;
                w.descriptorCount = 1;
                const auto schema = std::find_if(p.bindings.begin(), p.bindings.end(),
                                                 [&](const auto &s) { return s.location() == b.location(); });
                w.descriptorType = schema->type;
                if (b.buffer) {
                    buffersInfo[i] = {b.buffer->buffer, b.offset, b.length};
                    w.pBufferInfo = &buffersInfo[i];
                } else if (b.tensor) {
                    auto &tensor = tensorInfo[i];
                    tensor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_TENSOR_ARM;
                    tensor.tensorViewCount = 1;
                    tensor.pTensorViews = &b.tensor->view;
                    w.pNext = &tensor;
                } else if (b.texel) {
                    w.pTexelBufferView = &b.texel->view;
                } else if (b.acceleration) {
                    auto &a = accelerationInfo[i];
                    a.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                    a.accelerationStructureCount = 1;
                    a.pAccelerationStructures = &b.acceleration->acceleration;
                    w.pNext = &a;
                } else {
                    imageInfo[i] = {
                        b.sampler ? b.sampler->sampler : VK_NULL_HANDLE, b.texture ? b.texture->view : VK_NULL_HANDLE,
                        b.imageLayout != VK_IMAGE_LAYOUT_UNDEFINED ? b.imageLayout : (schema->tile ||
                         (w.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT && p.graphics.tileShading) ||
                         (w.descriptorType != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT && b.texture &&
                          (b.texture->usage & VK_IMAGE_USAGE_STORAGE_BIT)))
                            ? VK_IMAGE_LAYOUT_GENERAL
                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    w.pImageInfo = &imageInfo[i];
                }
            }
            vkUpdateDescriptorSets(d->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            descriptorSets.emplace(std::move(key), set);
        }
        vkCmdBindDescriptorSets(command, point, p.layout, setIndex, 1, &set, 0, nullptr);
    }
    if (!constants.empty())
        vkCmdPushConstants(command, p.layout, p.stages, 0, static_cast<uint32_t>(constants.size()), constants.data());
}
void Command::executeGenerated(std::shared_ptr<GeneratedExecution> g, std::vector<Binding> bs,
                               std::vector<uint8_t> constants) {
    recording();
    requireQueue(VK_QUEUE_COMPUTE_BIT);
    auto p = g->layout->pipelines.front();
    require((p->compute || p->rayTracing) && !p->tileShader, "Generated dispatch requires a non-tile pipeline");
    resolveSamplers(*p, bs);
    if (constants.empty())
        constants.resize(p->pushBytes);
    validateBindings(*p, bs, constants);
    g->retain(*this);
    for (const auto &b : bs) {
        if (b.buffer)
            buffers.push_back(b.buffer);
        if (b.texel)
            buffers.push_back(b.texel->buffer);
    }
    operations.push_back([g = std::move(g), p, bs = std::move(bs), constants = std::move(constants)](Command &c) {
        c.barrier();
        c.prepare(*p, bs, true);
        c.bind(*p, bs, constants);
        g->execute(c);
    });
}
void Command::dispatch(Dispatch op) {
    recording();
    requireQueue(VK_QUEUE_COMPUTE_BIT);
    require(op.pipeline && op.pipeline->compute && !op.pipeline->tileShader,
            "Ordinary dispatch requires a compute pipeline without tile shading");
    resolveSamplers(*op.pipeline, op.bindings);
    validateBindings(*op.pipeline, op.bindings, op.constants);
    for (int i = 0; !op.indirect && i < 3; ++i)
        require(op.groups[i] > 0 && op.groups[i] <= d->properties.limits.maxComputeWorkGroupCount[i],
                "Dispatch exceeds device workgroup count limits");
    if (op.indirect) {
        same(*this, *op.indirect);
        range(op.indirect->size, op.indirectOffset, 12);
        require((op.indirect->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) && op.indirectOffset % 4 == 0,
                "Indirect dispatch requires aligned indirect buffer");
        buffers.push_back(op.indirect);
    }
    for (const auto &b : op.bindings)
        if (b.texel)
            buffers.push_back(b.texel->buffer);
    for (const auto &b : op.bindings)
        if (b.buffer)
            buffers.push_back(b.buffer);
    operations.push_back([op = std::move(op)](Command &c) {
        c.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
        c.prepare(*op.pipeline, op.bindings, true);
        c.bind(*op.pipeline, op.bindings, op.constants);
        if (op.indirect)
            vkCmdDispatchIndirect(c.command, op.indirect->buffer, op.indirectOffset);
        else
            vkCmdDispatch(c.command, op.groups[0], op.groups[1], op.groups[2]);
    });
}
void Command::render(Render op) {
    recording();
    requireQueue(VK_QUEUE_GRAPHICS_BIT);
    validateTileOptions(*d, op.tileShading, op.tileApron);
    if (op.colors.empty() && op.color)
        op.colors.push_back({op.color, {}, 0, 0, 0, 0, op.colorLoad, op.colorStore, op.clearColor});
    if (!op.memoryBarriers.empty()) {
        require(!op.passLayout && !op.tileShading, "Use subpass dependencies or tileMemoryBarrier inside specialized passes");
        for (const auto &a : op.colors)
            require(a.texture && a.texture->storage != Storage::Memoryless, "Render memory barriers require stored attachments");
        require(!op.depth || op.depth->storage != Storage::Memoryless, "Render memory barriers cannot preserve memoryless depth/stencil");
        require(std::is_sorted(op.memoryBarriers.begin(), op.memoryBarriers.end()) && op.memoryBarriers.back() <= op.draws.size(),
                "Invalid render memory barrier order");
        // Resolve inherited aspect actions before changing either aspect for a segment.
        op.stencilLoad = op.stencilLoadOp();
        op.stencilStore = op.stencilStoreOp();
        auto boundaries = std::move(op.memoryBarriers);
        op.memoryBarriers.clear();
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        boundaries.erase(std::remove_if(boundaries.begin(), boundaries.end(), [&](size_t index) {
            return index == 0 || index == op.draws.size();
        }), boundaries.end());
        boundaries.push_back(op.draws.size());
        const auto oldOperations = operations.size(), oldBuffers = buffers.size(), oldCounters = counters.size(),
                   oldIndices = counterIndices.size();
        try {
            size_t start = 0;
            for (auto end : boundaries) {
                if (end == start && !op.draws.empty()) continue;
                Render segment = op;
                segment.draws.assign(op.draws.begin() + start, op.draws.begin() + end);
                if (start) {
                    for (auto &a : segment.colors) a.load = VK_ATTACHMENT_LOAD_OP_LOAD;
                    segment.depthLoad = segment.stencilLoad = VK_ATTACHMENT_LOAD_OP_LOAD;
                }
                if (end < op.draws.size()) {
                    for (auto &a : segment.colors) a.store = VK_ATTACHMENT_STORE_OP_STORE;
                    if (segment.depthStore != VK_ATTACHMENT_STORE_OP_NONE) segment.depthStore = VK_ATTACHMENT_STORE_OP_STORE;
                    if (segment.stencilStore != VK_ATTACHMENT_STORE_OP_NONE) segment.stencilStore = VK_ATTACHMENT_STORE_OP_STORE;
                }
                // Each segment emits the existing full memory dependency outside the render pass.
                render(std::move(segment));
                start = end;
            }
        } catch (...) {
            operations.resize(oldOperations);
            buffers.resize(oldBuffers);
            counters.resize(oldCounters);
            counterIndices.resize(oldIndices);
            throw;
        }
        return;
    }
    require(!op.colors.empty() || op.depth, "Render pass requires attachments");
    auto first = op.colors.empty() ? op.depth : op.colors[0].texture;
    require(bool(first), "Missing attachment");
    const auto extent = first->extent(op.colors.empty() ? op.depthMip : op.colors[0].mip);
    const auto samples = first->options.samples;
    require(!op.viewMask || (d->enabled & Multiview), "Multiview feature was not enabled");
    if (op.viewMask) {
        op.layers = 32u - uint32_t(__builtin_clz(op.viewMask));
        require(op.layers <= d->extensions->multiviewProperties.maxMultiviewViewCount, "View mask exceeds limit");
    }
    require(op.layers > 0 && op.layers <= d->properties.limits.maxFramebufferLayers,
            "Attachment layer count exceeds limit");
    const auto &l = d->properties.limits;
    require(extent.width <= l.maxFramebufferWidth && extent.height <= l.maxFramebufferHeight &&
                extent.width <= l.maxViewportDimensions[0] && extent.height <= l.maxViewportDimensions[1] &&
                op.colors.size() <= l.maxColorAttachments,
            "Framebuffer exceeds limits");
    std::set<std::tuple<Texture *, uint32_t, uint32_t>> used;
    auto attachment = [&](const std::shared_ptr<Texture> &t, uint32_t mip, uint32_t layer, VkAttachmentLoadOp load,
                          VkAttachmentStoreOp store, VkSampleCountFlagBits count, bool depth) {
        require(bool(t), "Missing attachment");
        same(*this, *t);
        t->usable();
        auto e = t->extent(mip);
        require(t->imageType() == VK_IMAGE_TYPE_2D && layer < t->options.layers &&
                    op.layers <= t->options.layers - layer && e.width == extent.width && e.height == extent.height &&
                    t->options.samples == count,
                "Attachment dimensions/sample count mismatch");
        for (uint32_t n = 0; n < op.layers; ++n)
            require(used.emplace(&t->root(), mip + t->baseMip, layer + t->baseLayer + n).second,
                    "Attachment subresources overlap");
        require(t->usage & (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                "Invalid attachment usage");
        require(load >= VK_ATTACHMENT_LOAD_OP_LOAD && load <= VK_ATTACHMENT_LOAD_OP_DONT_CARE &&
                    (store == VK_ATTACHMENT_STORE_OP_STORE || store == VK_ATTACHMENT_STORE_OP_DONT_CARE ||
                     (depth && store == VK_ATTACHMENT_STORE_OP_NONE && (d->enabledExtra & AttachmentStoreNone))),
                "Invalid attachment actions");
        require(t->storage != Storage::Memoryless ||
                    (load != VK_ATTACHMENT_LOAD_OP_LOAD && store == VK_ATTACHMENT_STORE_OP_DONT_CARE),
                "Memoryless attachments cannot load/store");
    };
    std::vector<VkFormat> formats;
    for (const auto &a : op.colors) {
        attachment(a.texture, a.mip, a.layer, a.load, a.store, samples, false);
        formats.push_back(a.texture->format);
        if (a.resolve) {
            require(samples > 1 && a.resolve->format == a.texture->format, "Invalid MSAA resolve format/source");
            attachment(a.resolve, a.resolveMip, a.resolveLayer, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                       VK_ATTACHMENT_STORE_OP_STORE, VK_SAMPLE_COUNT_1_BIT, false);
        }
    }
    if (op.depth) {
        attachment(op.depth, op.depthMip, op.depthLayer,
                   op.depth->depth() ? op.depthLoad : op.stencilLoadOp(),
                   op.depth->depth() ? op.depthStore : op.stencilStoreOp(), samples, true);
        if (op.depth->stencil()) {
            require(op.stencilLoadOp() >= VK_ATTACHMENT_LOAD_OP_LOAD && op.stencilLoadOp() <= VK_ATTACHMENT_LOAD_OP_DONT_CARE &&
                    (op.stencilStoreOp() == VK_ATTACHMENT_STORE_OP_STORE || op.stencilStoreOp() == VK_ATTACHMENT_STORE_OP_DONT_CARE ||
                     (op.stencilStoreOp() == VK_ATTACHMENT_STORE_OP_NONE && (d->enabledExtra & AttachmentStoreNone))),
                    "Invalid stencil actions");
            require(op.depth->storage != Storage::Memoryless || (op.stencilLoadOp() != VK_ATTACHMENT_LOAD_OP_LOAD &&
                    op.stencilStoreOp() == VK_ATTACHMENT_STORE_OP_DONT_CARE), "Memoryless stencil cannot load/store");
        }
        require(!op.depth->depth() || op.depthStore != VK_ATTACHMENT_STORE_OP_NONE || op.depthReadOnly,
                "NONE store requires read-only depth");
        require(!op.depth->stencil() || op.stencilStoreOp() != VK_ATTACHMENT_STORE_OP_NONE || op.stencilReadOnly,
                "NONE store requires read-only stencil");
        require(!op.depthReadOnly || !op.depth->depth() || op.depthLoad == VK_ATTACHMENT_LOAD_OP_LOAD,
                "Read-only depth requires LOAD");
        require(!op.stencilReadOnly || !op.depth->stencil() || op.stencilLoadOp() == VK_ATTACHMENT_LOAD_OP_LOAD,
                "Read-only stencil requires LOAD");
        require(std::isfinite(op.clearDepth) && op.clearDepth >= 0 && op.clearDepth <= 1, "Invalid depth clear");
    }
    if (op.depthResolve) {
        require(op.depth && samples > 1 && (d->enabled & DepthResolve) && op.depth->format == op.depthResolve->format,
                "Invalid/disabled depth resolve");
        attachment(op.depthResolve, op.depthResolveMip, op.depthResolveLayer, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                   VK_ATTACHMENT_STORE_OP_STORE, VK_SAMPLE_COUNT_1_BIT, true);
        const auto &p = d->extensions->depthResolveProperties;
        auto supported = [](VkResolveModeFlagBits mode, VkResolveModeFlags flags) {
            return mode && !(mode & (mode - 1)) && (mode & flags);
        };
        require(!op.depth->depth() || supported(op.depthResolveMode, p.supportedDepthResolveModes),
                "Unsupported depth resolve mode");
        require(!op.depth->stencil() || supported(op.stencilResolveMode, p.supportedStencilResolveModes),
                "Unsupported stencil resolve mode");
        require(!op.depth->depth() || !op.depth->stencil() || p.independentResolve ||
                    op.depthResolveMode == op.stencilResolveMode,
                "Device requires matching depth/stencil resolve modes");
    }
    if (op.passLayout) {
        validateSubpassLayout(*d, *op.passLayout);
        require(bool(op.depthResolve) == op.passLayout->hasDepthResolve(),
                "Depth resolve target differs from subpass layout");
        if (op.depthResolve) {
            require(!op.depth->depth() || op.depthResolveMode == op.passLayout->depthResolveMode,
                    "Depth resolve mode differs from subpass layout");
            require(!op.depth->stencil() || op.stencilResolveMode == op.passLayout->stencilResolveMode,
                    "Stencil resolve mode differs from subpass layout");
        }
        require(op.passLayout->colors == formats && op.passLayout->samples == samples &&
                    op.passLayout->depth == (op.depth ? op.depth->format : VK_FORMAT_UNDEFINED),
                "Render attachments do not match subpass layout");
        for (size_t n = 0; n < op.colors.size(); ++n)
            require(bool(op.colors[n].resolve) ==
                        (std::find(op.passLayout->resolveColors.begin(), op.passLayout->resolveColors.end(), n) !=
                         op.passLayout->resolveColors.end()),
                    "Resolve targets differ from subpass layout");
        std::set<uint32_t> usedAttachments;
        for (const auto &sub : op.passLayout->subpasses) {
            for (auto input : sub.inputs) {
                const auto a = subpassAttachment(op, input);
                require(a.texture && (a.texture->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT),
                        "Subpass input requires INPUT_ATTACHMENT usage");
                if (!usedAttachments.count(input))
                    require(a.load == VK_ATTACHMENT_LOAD_OP_LOAD,
                            "Attachment first used as input must LOAD previously initialized contents");
            }
            usedAttachments.insert(sub.inputs.begin(), sub.inputs.end());
            usedAttachments.insert(sub.colors.begin(), sub.colors.end());
            for (size_t n = 0; n < op.passLayout->resolveColors.size(); ++n)
                if (std::find(sub.colors.begin(), sub.colors.end(), op.passLayout->resolveColors[n]) !=
                    sub.colors.end())
                    usedAttachments.insert(op.colors.size() + bool(op.depth) + n);
            if (sub.depth)
                usedAttachments.insert(uint32_t(op.colors.size()));
            if (sub.resolveDepth)
                usedAttachments.insert(op.passLayout->depthResolveIndex());
        }
    }
    require(bool(op.rateMap) == bool(op.rateMapTexelSize.width),
            "Rate map texture and texel size must be provided together");
    if (op.rateMap) {
        validateRateTexel(*d, op.rateMapTexelSize);
        same(*this, *op.rateMap);
        auto &t = *op.rateMap;
        t.usable();
        const auto size = t.extent(op.rateMip);
        require(t.format == VK_FORMAT_R8_UINT && t.options.samples == 1 && t.imageType() == VK_IMAGE_TYPE_2D &&
                    (t.usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR),
                "Invalid rate map texture");
        require(uint64_t(size.width) * op.rateMapTexelSize.width >= extent.width &&
                    uint64_t(size.height) * op.rateMapTexelSize.height >= extent.height,
                "Rate map is too small for render area");
        require(op.rateLayer < t.options.layers &&
                    (t.options.layers - op.rateLayer == 1 ||
                     (d->extensions->fragmentRateProperties.layeredShadingRateAttachments &&
                      t.options.layers - op.rateLayer >= op.layers)),
                "Invalid rate map layer range");
        for (auto [root, mip, layer] : used) {
            (void)mip;
            (void)layer;
            require(&t.root() != root && !memoryOverlaps(t, *root), "Rate map aliases a render target");
        }
    }
    for (auto [a, am, al] : used)
        for (auto [b, bm, bl] : used) {
            (void)am;
            (void)al;
            (void)bm;
            (void)bl;
            require(a == b || !memoryOverlaps(*a, *b), "Render targets alias placement memory");
        }
    bool perTile = false;
    uint32_t subpass = 0;
    for (auto &draw : op.draws) {
        require(draw.subpass >= subpass && draw.subpass < (op.passLayout ? op.passLayout->subpasses.size() : 1),
                "Render commands must follow the subpass order");
        require(draw.subpass == subpass || !perTile, "End per-tile execution before changing subpass");
        subpass = draw.subpass;
        require(draw.tileAction <= 5 && (!draw.tileAction || op.tileShading), "Invalid tile action");
        if (draw.tileAction >= 3) {
            if (draw.tileAction == 3) {
                require(!perTile, "Per-tile execution is already active");
                require(d->extensions->tile.tileShadingPerTileDraw || d->extensions->tile.tileShadingPerTileDispatch,
                        "Per-tile execution is unavailable");
                perTile = true;
            }
            if (draw.tileAction == 4) {
                require(perTile, "Per-tile execution is not active");
                perTile = false;
            }
            continue;
        }
        draw.perTile = perTile;
        require(bool(draw.pipeline) && !draw.pipeline->rayTracing && draw.pipeline->compute == bool(draw.tileAction),
                "Pipeline stage differs from the render command");
        require(!draw.pipeline->tileShader || op.tileShading, "Tile shader requires a tile render pass");
        if (draw.tileAction) {
            const auto &f = d->extensions->tile;
            require(perTile && (draw.tileAction == 2 ? f.tileShadingDispatchTile : f.tileShadingPerTileDispatch),
                    "Tile dispatch is unavailable or outside per-tile execution");
            require((draw.tileAction == 2) == bool(draw.pipeline->tileRate[0]),
                    "Area dispatch requires TileShadingRateQCOM; threadgroup dispatch requires LocalSize");
        } else if (perTile) {
            require(d->extensions->tile.tileShadingPerTileDraw && !draw.visibility && !draw.generated &&
                        !(draw.pipeline->stages & ~(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)),
                    "Per-tile drawing requires supported vertex/fragment commands without queries");
        }
        if (draw.generated && draw.constants.empty())
            draw.constants.resize(draw.pipeline->pushBytes);
        resolveSamplers(*draw.pipeline, draw.bindings);
    }
    require(!perTile, "End per-tile execution before ending the render pass");
    std::set<std::pair<CounterPool *, uint32_t>> visibilityIndices;
    for (auto &draw : op.draws) {
        if (draw.tileAction >= 3)
            continue;
        if (draw.visibility) {
            require(draw.visibility->owner() == d.get() && draw.visibility->queueIndex == queueIndex &&
                        !draw.visibility->timestamp && draw.visibilityIndex < draw.visibility->count,
                    "Invalid visibility counter");
            require(visibilityIndices.emplace(draw.visibility.get(), draw.visibilityIndex).second,
                    "Use distinct visibility indices within a pass");
            counters.push_back(draw.visibility);
            counterIndices.push_back(draw.visibilityIndex);
        }
        validateBindings(*draw.pipeline, draw.bindings, draw.constants);
        if (draw.generated) {
            require(op.viewMask == 0, "Vulkan generated commands do not support multiview");
            draw.generated->retain(*this);
            const auto &g = *draw.generated->layout;
            const bool indexed = g.action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT ||
                                 g.action == VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_COUNT_EXT;
            require(!indexed || g.indexToken || draw.indexBuffer,
                    "Generated indexed drawing requires an index binding");
        }
        if (!draw.tileAction) {
            const auto &g = draw.pipeline->graphics;
            require(!op.depth || !op.depth->depth() || !op.depthReadOnly || !g.depthWrite,
                    "Pipeline writes read-only depth");
            require(!op.depth || !op.depth->stencil() || !op.stencilReadOnly || !g.stencilTest ||
                    (!g.front.writeMask && !g.back.writeMask), "Pipeline writes read-only stencil");
            require(draw.pipeline->graphics.tileShading == op.tileShading &&
                        draw.pipeline->graphics.tileApron.width == op.tileApron.width &&
                        draw.pipeline->graphics.tileApron.height == op.tileApron.height,
                    "Pipeline and render pass tile options differ");
            if (op.passLayout) {
                require(draw.pipeline->graphics.passLayout &&
                            draw.pipeline->graphics.passLayout->key == op.passLayout->key &&
                            draw.subpass == draw.pipeline->graphics.subpass &&
                            draw.subpass < op.passLayout->subpasses.size() &&
                            draw.pipeline->graphics.viewMask == op.viewMask,
                        "Pipeline subpass layout/index mismatch");
            } else {
                require(!draw.pipeline->graphics.passLayout && draw.pipeline->graphics.colors == formats &&
                            draw.pipeline->depthFormat == (op.depth ? op.depth->format : VK_FORMAT_UNDEFINED) &&
                            draw.pipeline->graphics.samples == samples &&
                            draw.pipeline->graphics.viewMask == op.viewMask,
                        "Pipeline attachment formats/sample count mismatch");
            }
            require(draw.pipeline->graphics.rateMapTexelSize.width == op.rateMapTexelSize.width &&
                        draw.pipeline->graphics.rateMapTexelSize.height == op.rateMapTexelSize.height,
                    "Pipeline rate map layout differs from render pass");
        }
        for (auto &b : draw.bindings) {
            if (b.texel)
                buffers.push_back(b.texel->buffer);
            if (b.buffer)
                buffers.push_back(b.buffer);
            const auto schema = std::find_if(draw.pipeline->bindings.begin(), draw.pipeline->bindings.end(),
                                             [&](const auto &s) { return s.location() == b.location(); });
            if (schema->tile) {
                validateTileBinding(*draw.pipeline, *schema, b, op, draw.subpass);
                continue;
            }
            if (schema->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
                require(op.passLayout &&
                            schema->inputAttachmentIndex < op.passLayout->subpasses[draw.subpass].inputs.size(),
                        "Missing input attachment index");
                const auto index = op.passLayout->subpasses[draw.subpass].inputs[schema->inputAttachmentIndex];
                const auto attachment = subpassAttachment(op, index);
                const auto &t = attachment.texture;
                const auto mip = attachment.mip, layer = attachment.layer;
                require(b.texture && &b.texture->root() == &t->root() && b.texture->format == t->format &&
                            b.texture->baseMip == t->baseMip + mip && b.texture->baseLayer == t->baseLayer + layer &&
                            b.texture->options.layers == op.layers,
                        "Input descriptor does not match framebuffer attachment subresource");
                continue;
            }
            const bool depthSample = b.texture && op.depth && &b.texture->root() == &op.depth->root() &&
                (schema->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || schema->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            if (depthSample) {
                const auto aspects = b.texture->viewAspects();
                // Store and even DONT_CARE can write adjacent samples/aspects. Only
                // LOAD/NONE on every present aspect permits unrestricted sampling.
                require((!op.depth->depth() || (op.depthReadOnly && op.depthStore == VK_ATTACHMENT_STORE_OP_NONE)) &&
                        (!op.depth->stencil() || (op.stencilReadOnly && op.stencilStoreOp() == VK_ATTACHMENT_STORE_OP_NONE)),
                        "Sampled depth/stencil attachments require read-only LOAD/NONE for all aspects");
                require(!op.tileShading && (!op.passLayout || op.passLayout->subpasses[draw.subpass].depth) &&
                        (!(aspects & VK_IMAGE_ASPECT_DEPTH_BIT) || op.depthReadOnly) &&
                        (!(aspects & VK_IMAGE_ASPECT_STENCIL_BIT) || op.stencilReadOnly),
                        "Sampling an attachment requires read-only access for every sampled aspect");
                require(b.texture->baseMip == op.depth->baseMip + op.depthMip && b.texture->options.mipLevels == 1 &&
                        b.texture->baseLayer == op.depth->baseLayer + op.depthLayer && b.texture->options.layers == op.layers,
                        "Read-only sampled view must match the framebuffer subresource");
                b.imageLayout = op.depthLayout();
            }
            Resource *resource = b.buffer  ? static_cast<Resource *>(b.buffer.get())
                                 : b.texel ? static_cast<Resource *>(b.texel->buffer.get())
                                           : static_cast<Resource *>(b.texture.get());
            if (resource) {
                if (op.rateMap)
                    require(!memoryOverlaps(*resource, *op.rateMap), "Binding aliases rate map memory");
                for (auto [root, mip, layer] : used) {
                    (void)mip;
                    (void)layer;
                    require((depthSample && root == &op.depth->root()) || !memoryOverlaps(*resource, *root), "Binding aliases render target memory");
                }
            }
            if (b.texture && op.rateMap)
                require(&b.texture->root() != &op.rateMap->root(),
                        "Rate map cannot also be a shader binding in its pass");
            if (b.texture)
                for (auto [root, mip, layer] : used) {
                    (void)mip;
                    (void)layer;
                    require((depthSample && root == &op.depth->root()) || &b.texture->root() != root, "Attachment feedback is unsupported");
                }
        }
        if (draw.tileAction) {
            if (draw.indirect) {
                same(*this, *draw.indirect);
                require(draw.tileAction == 1 && (draw.indirect->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) &&
                            draw.indirectOffset % 4 == 0,
                        "Invalid indirect tile dispatch buffer");
                range(draw.indirect->size, draw.indirectOffset, 12);
                buffers.push_back(draw.indirect);
            } else if (draw.tileAction == 1) {
                for (int n = 0; n < 3; ++n)
                    require(draw.meshGroups[n] > 0 && draw.meshGroups[n] <= l.maxComputeWorkGroupCount[n],
                            "Tile dispatch workgroup count exceeds device limits");
            }
            continue;
        }
        require(draw.pipeline->graphics.mesh == (draw.meshGroups[0] != 0),
                "Use mesh draws with mesh pipelines and primitive draws with vertex pipelines");
        if (draw.pipeline->graphics.mesh) {
            require(!draw.indexBuffer && draw.vertexBuffers.empty(), "Mesh draws cannot use vertex/index bindings");
            const auto &mp = d->extensions->meshProperties;
            bool task = bool(draw.pipeline->graphics.task);
            auto count = task ? mp.maxTaskWorkGroupCount : mp.maxMeshWorkGroupCount;
            uint64_t total = 1;
            for (int n = 0; n < 3; ++n) {
                require(draw.meshGroups[n] && draw.meshGroups[n] <= count[n], "Mesh group count exceeds device limits");
                total *= draw.meshGroups[n];
            }
            require(total <= (task ? mp.maxTaskWorkGroupTotalCount : mp.maxMeshWorkGroupTotalCount),
                    "Mesh total group count exceeds device limits");
        }
        if (draw.indexBuffer) {
            same(*this, *draw.indexBuffer);
            require(draw.indexType == VK_INDEX_TYPE_UINT16 || draw.indexType == VK_INDEX_TYPE_UINT32,
                    "Invalid index type");
            uint64_t bytes = draw.indexType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
            require((draw.indexBuffer->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) && draw.indexOffset % bytes == 0,
                    "Invalid index buffer usage/alignment");
            range(draw.indexBuffer->size, draw.indexOffset,
                  (draw.indirect || draw.generated) ? bytes : (uint64_t(draw.firstVertex) + draw.vertices) * bytes);
            buffers.push_back(draw.indexBuffer);
        }
        if (draw.indirect) {
            same(*this, *draw.indirect);
            const uint32_t commandBytes = draw.pipeline->graphics.mesh ? 12 : draw.indexBuffer ? 20 : 16;
            require((draw.countBuffer || draw.drawCount > 0) && draw.drawCount <= l.maxDrawIndirectCount &&
                        draw.indirectOffset % 4 == 0 &&
                        ((draw.drawCount == 1 && !draw.countBuffer) ||
                         (draw.stride >= commandBytes && draw.stride % 4 == 0)) &&
                        (draw.indirect->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT),
                    "Invalid indirect draw range");
            require(draw.countBuffer || draw.drawCount == 1 || (d->enabled & MultiDraw),
                    "Multi-draw feature was not enabled");
            if (draw.drawCount)
                range(draw.indirect->size, draw.indirectOffset,
                      uint64_t(draw.drawCount - 1) * draw.stride + commandBytes);
            else
                require(draw.indirectOffset <= draw.indirect->size, "Indirect offset exceeds buffer size");
            if (draw.countBuffer) {
                same(*this, *draw.countBuffer);
                require((d->enabledExtra & DrawIndirectCount) && draw.countOffset % 4 == 0 &&
                            (draw.countBuffer->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT),
                        "Draw count requires enabled feature and an aligned indirect count buffer");
                range(draw.countBuffer->size, draw.countOffset, 4);
                buffers.push_back(draw.countBuffer);
            }
            buffers.push_back(draw.indirect);
        } else
            require((!op.viewMask || uint64_t(draw.firstInstance) + draw.instances - 1 <=
                                         d->extensions->multiviewProperties.maxMultiviewInstanceIndex) &&
                        draw.vertices && draw.instances && draw.firstVertex <= UINT32_MAX - draw.vertices &&
                        draw.firstInstance <= UINT32_MAX - draw.instances,
                    "Invalid draw counts");
        if (!draw.indirect && !draw.generated && draw.firstInstance &&
            !d->extensions->vertexDivisorProperties.supportsNonZeroFirstInstance)
            require(std::none_of(draw.pipeline->graphics.vertexDivisors.begin(),
                                 draw.pipeline->graphics.vertexDivisors.end(),
                                 [](const auto &v) { return v.divisor != 1; }),
                    "This device requires firstInstance zero with a custom instance step rate");
        std::set<uint32_t> bound;
        for (const auto &v : draw.vertexBuffers) {
            require(bool(v.buffer) && bound.insert(v.index).second, "Invalid/duplicate vertex binding");
            same(*this, *v.buffer);
            require(v.offset < v.buffer->size && (v.buffer->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT),
                    "Invalid vertex buffer range/usage");
            buffers.push_back(v.buffer);
        }
        for (const auto &v : draw.pipeline->graphics.vertexBindings)
            require(bound.count(v.binding) || (draw.generated && draw.generated->layout->vertexTokens.count(v.binding)),
                    "Missing vertex buffer");
        const auto count = draw.pipeline->graphics.viewportCount;
        require((draw.viewports.empty() || draw.viewports.size() == count) &&
                    (draw.scissors.empty() || draw.scissors.size() == count),
                "Viewport/scissor count differs from pipeline");
        for (const auto &v : draw.viewports)
            require(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.width) && std::isfinite(v.height) &&
                        v.width > 0 && v.height != 0 && v.width <= l.maxViewportDimensions[0] &&
                        std::abs(v.height) <= l.maxViewportDimensions[1] && v.x >= l.viewportBoundsRange[0] &&
                        v.x + v.width <= l.viewportBoundsRange[1] &&
                        std::min(v.y, v.y + v.height) >= l.viewportBoundsRange[0] &&
                        std::max(v.y, v.y + v.height) <= l.viewportBoundsRange[1] && v.minDepth >= 0 &&
                        v.maxDepth <= 1 && v.minDepth <= v.maxDepth,
                    "Invalid viewport");
        for (const auto &s : draw.scissors)
            require(s.offset.x >= 0 && s.offset.y >= 0 && uint64_t(s.offset.x) + s.extent.width <= extent.width &&
                        uint64_t(s.offset.y) + s.extent.height <= extent.height,
                    "Invalid scissor");
        require(std::isfinite(draw.lineWidth) && draw.lineWidth > 0 &&
                    (draw.lineWidth == 1 || ((d->enabled & WideLines) && draw.lineWidth >= l.lineWidthRange[0] &&
                                             draw.lineWidth <= l.lineWidthRange[1])),
                "Invalid/disabled wide line");
        require(std::isfinite(draw.biasClamp) && std::isfinite(draw.depthBias) && std::isfinite(draw.slopeBias) &&
                    (draw.biasClamp == 0 || (d->enabled & DepthBiasClamp)),
                "Invalid/disabled depth bias clamp");
    }
    operations.push_back([op = std::move(op), formats, extent, samples](Command &c) {
        const bool advanced = op.tileShading || std::any_of(op.draws.begin(), op.draws.end(), [](const auto &draw) { return bool(draw.generated); });
        c.barrier(advanced ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
        auto transition = [&](Texture &t, VkImageLayout layout, bool read, uint32_t mip, uint32_t layer, VkImageAspectFlags aspects = 0) {
            for (uint32_t n = 0; n < op.layers; ++n)
                if (!op.viewMask || (op.viewMask & (1u << n)))
                    c.transition(t, layout, read, mip, layer + n, 1, 1, aspects);
        };
        auto initialized = [&](Texture &t, bool value, uint32_t mip, uint32_t layer, VkImageAspectFlags aspects = 0) {
            for (uint32_t n = 0; n < op.layers; ++n)
                if (!op.viewMask || (op.viewMask & (1u << n)))
                    c.markInitialized(t, value, mip, layer + n, 1, 1, aspects);
        };
        for (const auto &draw : op.draws)
            if (draw.visibility)
                vkCmdResetQueryPool(c.command, draw.visibility->pool, draw.visibilityIndex, 1);
        for (const auto &draw : op.draws)
            if (draw.tileAction < 3)
                c.prepare(*draw.pipeline, draw.bindings, draw.tileAction != 0);
        const auto colorLayout = op.tileShading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const auto depthLayout = op.depthLayout();
        std::vector<VkImageView> views;
        std::vector<VkClearValue> clears;
        for (const auto &a : op.colors) {
            transition(*a.texture, colorLayout, a.load == VK_ATTACHMENT_LOAD_OP_LOAD, a.mip, a.layer);
            views.push_back(a.texture->attachmentView(a.mip, a.layer, op.layers));
            VkClearValue value{};
            std::memcpy(&value.color, a.clear.data(), sizeof(value.color));
            clears.push_back(value);
        }
        if (op.depth) {
            VkImageAspectFlags reads = 0;
            if (op.depth->depth() && op.depthLoad == VK_ATTACHMENT_LOAD_OP_LOAD) reads |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (op.depth->stencil() && op.stencilLoadOp() == VK_ATTACHMENT_LOAD_OP_LOAD) reads |= VK_IMAGE_ASPECT_STENCIL_BIT;
            transition(*op.depth, depthLayout, reads != 0, op.depthMip, op.depthLayer, reads);
            views.push_back(op.depth->attachmentView(op.depthMip, op.depthLayer, op.layers));
            VkClearValue value{};
            value.depthStencil = {op.clearDepth, op.clearStencil};
            clears.push_back(value);
        }
        for (const auto &a : op.colors)
            if (a.resolve) {
                transition(*a.resolve, colorLayout, false, a.resolveMip, a.resolveLayer);
                views.push_back(a.resolve->attachmentView(a.resolveMip, a.resolveLayer, op.layers));
                clears.push_back({});
            }
        if (op.depthResolve) {
            transition(*op.depthResolve, op.tileShading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false, op.depthResolveMip, op.depthResolveLayer);
            views.push_back(op.depthResolve->attachmentView(op.depthResolveMip, op.depthResolveLayer, op.layers));
            clears.push_back({});
        }
        if (op.rateMap) {
            const auto layers = op.rateMap->options.layers - op.rateLayer == 1 ? 1 : op.layers;
            c.transition(*op.rateMap, VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR, true, op.rateMip,
                         op.rateLayer, 1, layers);
            views.push_back(op.rateMap->attachmentView(op.rateMip, op.rateLayer, layers));
            clears.push_back({});
        }
        auto pass = makePass(*c.d, formats, op.depth ? op.depth->format : VK_FORMAT_UNDEFINED, samples, op.colors,
                             op.depthLoad, op.depthStore, op.viewMask, &op);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = pass;
        fi.attachmentCount = uint32_t(views.size());
        fi.pAttachments = views.data();
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = op.viewMask ? 1 : op.layers;
        const auto fb = c.framebuffer(fi);
        if (op.tileShading &&
            std::any_of(op.draws.begin(), op.draws.end(), [](const auto &draw) { return draw.tileAction == 2; })) {
            uint32_t count = 0;
            check(c.d->extensions->framebufferTiles(c.d->device, fb, &count, nullptr), "query framebuffer tile count");
            std::vector<VkTilePropertiesQCOM> tiles(count, {VK_STRUCTURE_TYPE_TILE_PROPERTIES_QCOM});
            check(c.d->extensions->framebufferTiles(c.d->device, fb, &count, tiles.data()), "query framebuffer tiles");
            require(count >= (op.passLayout ? op.passLayout->subpasses.size() : 1),
                    "Missing framebuffer tile properties");
            for (const auto &draw : op.draws)
                if (draw.tileAction == 2) {
                    auto z = draw.pipeline->tileRate[2];
                    require(z <= tiles[draw.subpass].tileSize.depth && tiles[draw.subpass].tileSize.depth % z == 0,
                            "Tile shading Z rate must divide the framebuffer tile depth");
                }
        }
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = pass;
        bi.framebuffer = fb;
        bi.renderArea.extent = {extent.width, extent.height};
        bi.clearValueCount = uint32_t(clears.size());
        bi.pClearValues = clears.data();
        vkCmdBeginRenderPass(c.command, &bi, VK_SUBPASS_CONTENTS_INLINE);
        uint32_t activeSubpass = 0;
        for (const auto &draw : op.draws) {
            while (activeSubpass < draw.subpass) {
                vkCmdNextSubpass(c.command, VK_SUBPASS_CONTENTS_INLINE);
                ++activeSubpass;
            }
            if (draw.tileAction >= 3) {
                if (draw.tileAction == 3) {
                    VkPerTileBeginInfoQCOM info{VK_STRUCTURE_TYPE_PER_TILE_BEGIN_INFO_QCOM};
                    c.d->extensions->beginTile(c.command, &info);
                } else if (draw.tileAction == 4) {
                    VkPerTileEndInfoQCOM info{VK_STRUCTURE_TYPE_PER_TILE_END_INFO_QCOM};
                    c.d->extensions->endTile(c.command, &info);
                    // Per-tile state changes do not promise state for subsequent non-tile commands.
                    c.boundPipelines = {};
                } else
                    tileBarrier(c);
                continue;
            }
            if (draw.tileAction) {
                c.bind(*draw.pipeline, draw.bindings, draw.constants);
                if (draw.tileAction == 2) {
                    VkDispatchTileInfoQCOM info{VK_STRUCTURE_TYPE_DISPATCH_TILE_INFO_QCOM};
                    c.d->extensions->dispatchTile(c.command, &info);
                } else if (draw.indirect)
                    vkCmdDispatchIndirect(c.command, draw.indirect->buffer, draw.indirectOffset);
                else
                    vkCmdDispatch(c.command, draw.meshGroups[0], draw.meshGroups[1], draw.meshGroups[2]);
                continue;
            }
            const VkViewport defaultViewport{0, float(extent.height), float(extent.width), -float(extent.height), 0, 1};
            const VkRect2D defaultScissor{{0, 0}, {extent.width, extent.height}};
            for (uint32_t n = 0; n < draw.pipeline->graphics.viewportCount; ++n) {
                vkCmdSetViewport(c.command, n, 1, draw.viewports.empty() ? &defaultViewport : &draw.viewports[n]);
                vkCmdSetScissor(c.command, n, 1, draw.scissors.empty() ? &defaultScissor : &draw.scissors[n]);
            }
            vkCmdSetBlendConstants(c.command, draw.blendColor.data());
            vkCmdSetDepthBias(c.command, draw.depthBias, draw.biasClamp, draw.slopeBias);
            vkCmdSetStencilReference(c.command, VK_STENCIL_FACE_FRONT_AND_BACK, draw.stencilReference);
            vkCmdSetLineWidth(c.command, draw.lineWidth);
            c.bind(*draw.pipeline, draw.bindings, draw.constants);
            for (const auto &v : draw.vertexBuffers) {
                if ((c.d->enabledExtra & DeviceGeneratedCommands) && c.d->extensions->generatedVertexInput) {
                    auto schema = std::find_if(draw.pipeline->graphics.vertexBindings.begin(),
                                               draw.pipeline->graphics.vertexBindings.end(),
                                               [&](const auto &b) { return b.binding == v.index; });
                    VkDeviceSize size = v.buffer->size - v.offset;
                    VkDeviceSize stride = schema == draw.pipeline->graphics.vertexBindings.end() ? 0 : schema->stride;
                    c.d->extensions->bindVertexBuffers2(c.command, v.index, 1, &v.buffer->buffer, &v.offset, &size,
                                                        &stride);
                } else
                    vkCmdBindVertexBuffers(c.command, v.index, 1, &v.buffer->buffer, &v.offset);
            }
            if (draw.indexBuffer)
                vkCmdBindIndexBuffer(c.command, draw.indexBuffer->buffer, draw.indexOffset, draw.indexType);
            if (draw.visibility)
                vkCmdBeginQuery(c.command, draw.visibility->pool, draw.visibilityIndex,
                                (c.d->enabled & PreciseOcclusion) ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
            if (draw.generated) {
                draw.generated->execute(c);
            } else if (draw.indirect) {
                if (draw.countBuffer) {
                    if (draw.pipeline->graphics.mesh)
                        c.d->extensions->drawMeshIndirectCount(c.command, draw.indirect->buffer, draw.indirectOffset,
                                                               draw.countBuffer->buffer, draw.countOffset,
                                                               draw.drawCount, draw.stride);
                    else if (draw.indexBuffer)
                        c.d->extensions->drawIndexedIndirectCount(c.command, draw.indirect->buffer, draw.indirectOffset,
                                                                  draw.countBuffer->buffer, draw.countOffset,
                                                                  draw.drawCount, draw.stride);
                    else
                        c.d->extensions->drawIndirectCount(c.command, draw.indirect->buffer, draw.indirectOffset,
                                                           draw.countBuffer->buffer, draw.countOffset, draw.drawCount,
                                                           draw.stride);
                } else if (draw.pipeline->graphics.mesh)
                    c.d->extensions->drawMeshIndirect(c.command, draw.indirect->buffer, draw.indirectOffset,
                                                      draw.drawCount, draw.stride);
                else if (draw.indexBuffer)
                    vkCmdDrawIndexedIndirect(c.command, draw.indirect->buffer, draw.indirectOffset, draw.drawCount,
                                             draw.stride);
                else
                    vkCmdDrawIndirect(c.command, draw.indirect->buffer, draw.indirectOffset, draw.drawCount,
                                      draw.stride);
            } else if (draw.pipeline->graphics.mesh)
                c.d->extensions->drawMesh(c.command, draw.meshGroups[0], draw.meshGroups[1], draw.meshGroups[2]);
            else if (draw.indexBuffer)
                vkCmdDrawIndexed(c.command, draw.vertices, draw.instances, draw.firstVertex, draw.baseVertex,
                                 draw.firstInstance);
            else
                vkCmdDraw(c.command, draw.vertices, draw.instances, draw.firstVertex, draw.firstInstance);
            if (draw.visibility)
                vkCmdEndQuery(c.command, draw.visibility->pool, draw.visibilityIndex);
        }
        if (op.passLayout)
            while (activeSubpass + 1 < op.passLayout->subpasses.size()) {
                vkCmdNextSubpass(c.command, VK_SUBPASS_CONTENTS_INLINE);
                ++activeSubpass;
            }
        vkCmdEndRenderPass(c.command);
        for (const auto &a : op.colors) {
            initialized(*a.texture, a.store == VK_ATTACHMENT_STORE_OP_STORE, a.mip, a.layer);
            if (a.resolve)
                initialized(*a.resolve, true, a.resolveMip, a.resolveLayer);
        }
        if (op.depthResolve)
            initialized(*op.depthResolve, true, op.depthResolveMip, op.depthResolveLayer);
        if (op.depth) {
            if (op.depth->depth()) initialized(*op.depth, op.depthStore != VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                              op.depthMip, op.depthLayer, VK_IMAGE_ASPECT_DEPTH_BIT);
            if (op.depth->stencil()) initialized(*op.depth, op.stencilStoreOp() != VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                op.depthMip, op.depthLayer, VK_IMAGE_ASPECT_STENCIL_BIT);
        }
    });
}
void Command::copy(std::shared_ptr<Buffer> src, std::shared_ptr<Buffer> dst, VkDeviceSize so, VkDeviceSize to,
                   VkDeviceSize size) {
    recording();
    requireQueue(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    require(src && dst, "Buffers are required");
    require(!sparseMemoryOverlaps(*src, *dst), "Copies between shared sparse mappings require an intermediate buffer");
    same(*this, *src);
    same(*this, *dst);
    range(src->size, so, size);
    range(dst->size, to, size);
    require((src->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) && (dst->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT),
            "Blit requires transfer usage");
    const bool sharedMemory = src == dst || (src->heap && src->heap == dst->heap && src->heap->placement());
    require(!sharedMemory || src->heapOffset + so + size <= dst->heapOffset + to ||
                dst->heapOffset + to + size <= src->heapOffset + so,
            "Buffer copy ranges overlap in memory");
    require(so % 4 == 0 && to % 4 == 0 && size % 4 == 0, "Buffer blits require 4-byte alignment");
    buffers.push_back(src);
    buffers.push_back(dst);
    operations.push_back([src, dst, so, to, size](Command &c) {
        c.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferCopy region{so, to, size};
        vkCmdCopyBuffer(c.command, src->buffer, dst->buffer, 1, &region);
    });
}
void Command::present(std::shared_ptr<Drawable> drawable) {
    recording();
    requireQueue(VK_QUEUE_GRAPHICS_BIT);
    require(drawable && !presentation && drawable->didAcquire && drawable->frame->active,
            "Present requires one currently acquired drawable");
    same(*this, *drawable);
    presentation = std::move(drawable);
}
void Command::commit() {
    recording();
    d->collect();
    try {
        for (size_t i = 0; i < d->idleCommandCount; ++i) {
            const auto cached = d->idleCommands[i];
            if (cached.family != queueInfo().family)
                continue;
            pool = cached.pool;
            command = cached.command;
            d->idleCommands[i] = d->idleCommands[--d->idleCommandCount];
            break;
        }
        if (!pool) {
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.queueFamilyIndex = queueInfo().family;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            check(vkCreateCommandPool(d->device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
            VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            alloc.commandPool = pool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(d->device, &alloc, &command), "vkAllocateCommandBuffers");
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        if (graphOnly())
            graphSegments.push_back(command);
        bool firstOperation = true;
        for (const auto &op : operations) {
            if (graphOnly() && !firstOperation) {
                check(vkEndCommandBuffer(command), "end graph segment");
                VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                allocation.commandPool = pool;
                allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocation.commandBufferCount = 1;
                graphSegments.reserve(graphSegments.size() + 1);
                check(vkAllocateCommandBuffers(d->device, &allocation, &command), "allocate graph segment");
                graphSegments.push_back(command);
                check(vkBeginCommandBuffer(command, &begin), "begin graph segment");
            }
            op(*this);
            firstOperation = false;
        }
        if (presentation)
            transition(*presentation->texture, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, true);
        barrier(VK_PIPELINE_STAGE_HOST_BIT);
        check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(d->device, &fi, nullptr, &fence), "vkCreateFence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        std::vector<VkSemaphore> waits, signals;
        std::vector<uint64_t> waitValues, signalValues;
        for (const auto &[event, value] : eventWaits) {
            const auto current = event->value();
            require(value <= current ||
                        value - current <= d->extensions->timelineProperties.maxTimelineSemaphoreValueDifference,
                    "Event wait exceeds timeline distance limit");
            waits.push_back(event->semaphore);
            waitValues.push_back(value);
        }
        for (const auto &[event, value] : eventSignals) {
            const auto current = event->value();
            require(value > current &&
                        value - current <= d->extensions->timelineProperties.maxTimelineSemaphoreValueDifference,
                    "Event signal exceeds timeline distance limit");
            require(value > event->lastScheduled && value > event->value(),
                    "Event signals must increase monotonically");
            for (const auto &wait : eventWaits)
                if (wait.first == event)
                    require(value > wait.second, "Signal value must exceed the same submission wait");
            if (event->lastScheduled > 0) {
                auto previous = std::find(waits.begin(), waits.end(), event->semaphore);
                if (previous == waits.end()) {
                    waits.push_back(event->semaphore);
                    waitValues.push_back(event->lastScheduled);
                } else {
                    auto index = size_t(previous - waits.begin());
                    waitValues[index] = std::max(waitValues[index], event->lastScheduled);
                }
            }
            signals.push_back(event->semaphore);
            signalValues.push_back(value);
        }
        for (const auto &event : externalWaits) {
            require(event->state == ExternalSemaphore::State::Imported ||
                        event->state == ExternalSemaphore::State::Signalled,
                    "External semaphore has no unconsumed signal");
            waits.push_back(event->semaphore);
            waitValues.push_back(0);
        }
        for (const auto &event : externalSignals) {
            require(event->state == ExternalSemaphore::State::Fresh, "External semaphore has already been used");
            require(std::find(externalWaits.begin(), externalWaits.end(), event) == externalWaits.end(),
                    "Cannot signal and wait the same external semaphore");
            signals.push_back(event->semaphore);
            signalValues.push_back(0);
        }
        if (presentation) {
            waits.push_back(presentation->acquired);
            waitValues.push_back(0);
            signals.push_back(presentation->rendered);
            signalValues.push_back(0);
        }
        std::vector<VkPipelineStageFlags> waitStages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        submit.waitSemaphoreCount = uint32_t(waits.size());
        submit.pWaitSemaphores = waits.data();
        submit.pWaitDstStageMask = waitStages.data();
        submit.signalSemaphoreCount = uint32_t(signals.size());
        submit.pSignalSemaphores = signals.data();
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = uint32_t(waitValues.size());
        timeline.pWaitSemaphoreValues = waitValues.data();
        timeline.signalSemaphoreValueCount = uint32_t(signalValues.size());
        timeline.pSignalSemaphoreValues = signalValues.data();
        if (!eventWaits.empty() || !eventSignals.empty())
            submit.pNext = &timeline;
        // Reserve the bookkeeping before submission; no allocating operation
        // may make a successfully submitted command appear unsubmitted.
        d->pending.reserve(d->pending.size() + 1);
        auto self = shared_from_this();
        for (const auto &[event, value] : eventSignals) {
            (void)value;
            event->pendingSignals.erase(std::remove_if(event->pendingSignals.begin(), event->pendingSignals.end(),
                                                       [](const auto &pending) {
                                                           auto c = pending.second.lock();
                                                           return !c || c->state == State::Completed;
                                                       }),
                                        event->pendingSignals.end());
            event->pendingSignals.reserve(event->pendingSignals.size() + 1);
        }
        if (graphOnly())
            submitGraphSegments(submit);
        else
            check(vkQueueSubmit(queueInfo().handle, 1, &submit, fence), "vkQueueSubmit");
        state = State::Submitted;
        for (const auto &event : externalWaits)
            event->state = ExternalSemaphore::State::Consumed;
        for (const auto &event : externalSignals)
            event->state = ExternalSemaphore::State::Signalled;
        for (const auto &[event, value] : eventSignals) {
            event->lastScheduled = value;
            event->pendingSignals.emplace_back(value, self);
        }
        for (size_t i = 0; i < counters.size(); ++i) {
            counters[i]->writer = self;
            counters[i]->issued[counterIndices[i]] = true;
        }
        for (auto &b : buffers)
            ++b->inFlight;
        for (auto &t : tensors)
            ++t->inFlight;
        for (const auto &[a, built] : accelerationStates) {
            a->built = built;
            ++a->generation;
        }
        for (const auto &[a, inputs] : accelerationInputStates)
            a->refitInputs = inputs;
        for (const auto &[external, owned] : externalOwnership)
            external->gpuOwned = owned;
        for (const auto &[texture, current] : images) {
            texture->states = current;
            texture->layout = current[0].layout;
            texture->initialized = (current[0].initialized & texture->aspects()) == texture->aspects();
        }
        d->pending.push_back(self);
        if (presentation) {
            VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &presentation->rendered;
            info.swapchainCount = 1;
            info.pSwapchains = &presentation->surface->swapchain;
            info.pImageIndices = &presentation->index;
            VkResult result = vkQueuePresentKHR(d->queue, &info);
            presentation->presented = true;
            presentation->frame->active = false;
            presentation->surface->outstanding = false;
            if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
                presentation->surface->needsRebuild = true;
            else
                check(result, "vkQueuePresentKHR");
        }
    } catch (...) {
        if (state == State::Recording)
            state = State::Failed;
        throw;
    }
}
bool Command::wait(uint64_t timeout) {
    if (state == State::Completed)
        return true;
    require(state == State::Submitted, "Only submitted commands can be waited on");
    auto result = vkWaitForFences(d->device, 1, &fence, VK_TRUE, timeout);
    if (result == VK_TIMEOUT)
        return false;
    check(result, "vkWaitForFences");
    state = State::Completed;
    for (auto &b : buffers)
        --b->inFlight;
    for (auto &t : tensors)
        --t->inFlight;
    // Keep all recorded objects alive until the command pool is destroyed.
    return true;
}
Command::~Command() {
    bool completed = state == State::Completed;
    if (state == State::Submitted) {
        // Device loss is terminal, but destructors must not throw through JNI.
        completed = vkWaitForFences(d->device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
        for (auto &b : buffers)
            --b->inFlight;
        for (auto &t : tensors)
            --t->inFlight;
    }
    // Reset only after GPU completion, before releasing recorded resources.
    // Retain one primary buffer per pool rather than allocating one each reuse.
    if (pool && completed && graphSegments.size() <= 1 && d->idleCommandCount < d->idleCommands.size() &&
        vkResetCommandPool(d->device, pool, 0) == VK_SUCCESS) {
        d->idleCommands[d->idleCommandCount++] = {pool, command, queueInfo().family};
    } else if (pool)
        vkDestroyCommandPool(d->device, pool, nullptr);
    for (auto &fb : framebufferAllocations) d->recycle(std::move(fb), completed);
    for (auto &dp : descriptorAllocations) d->recycle(std::move(dp), completed);
    if (fence)
        vkDestroyFence(d->device, fence, nullptr);
}

#ifdef __ANDROID__
Surface::Surface(std::shared_ptr<Device> device, ANativeWindow *nativeWindow, uint32_t w, uint32_t h)
    : Resource(std::move(device)), requestedWidth(w), requestedHeight(h), window(nativeWindow) {
    require(window != nullptr && w > 0 && h > 0, "Invalid Android surface size");
    d->retiredSurfaces.reserve(d->retiredSurfaces.size() + d->liveSurfaces + 1);
    ANativeWindow_acquire(window);
    try {
        VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        info.window = window;
        check(vkCreateAndroidSurfaceKHR(d->instance, &info, nullptr, &surface), "vkCreateAndroidSurfaceKHR");
        VkBool32 supported = false;
        check(vkGetPhysicalDeviceSurfaceSupportKHR(d->physical, d->family, surface, &supported),
              "query presentation support");
        require(supported, "Selected graphics/compute queue cannot present to this surface");
        rebuild();
        ++d->liveSurfaces;
    } catch (...) {
        for (auto view : views)
            d->destroyView(view);
        if (swapchain)
            vkDestroySwapchainKHR(d->device, swapchain, nullptr);
        if (surface)
            vkDestroySurfaceKHR(d->instance, surface, nullptr);
        ANativeWindow_release(window);
        throw;
    }
}
#endif
void Surface::rebuild() {
    require(!outstanding, "Close or present the acquired drawable before resizing");
    d->collect();
    require(d->pending.empty() && d->retiredDrawables.empty(),
            "Complete submitted work and image acquisition before rebuilding the surface");
    d->waitIdle();
    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->physical, surface, &caps), "query surface capabilities");
    require((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0,
            "Surface does not support color attachments");
    uint32_t count = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(d->physical, surface, &count, nullptr), "query surface formats");
    std::vector<VkSurfaceFormatKHR> formats(count);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(d->physical, surface, &count, formats.data()), "query surface formats");
    auto chosen = std::find_if(formats.begin(), formats.end(), [](auto f) {
        return (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
               f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    VkSurfaceFormatKHR selected{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    if (formats.size() != 1 || formats[0].format != VK_FORMAT_UNDEFINED) {
        require(chosen != formats.end(), "Surface requires an unsupported color format");
        selected = *chosen;
    }
    VkExtent2D newExtent = caps.currentExtent;
    if (newExtent.width == UINT32_MAX)
        newExtent = {std::clamp(requestedWidth, caps.minImageExtent.width, caps.maxImageExtent.width),
                     std::clamp(requestedHeight, caps.minImageExtent.height, caps.maxImageExtent.height)};
    require(newExtent.width > 0 && newExtent.height > 0, "Surface has zero extent; wait for surfaceChanged");
    uint32_t imageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount)
        imageCount = std::min(imageCount, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (auto option : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
        if (caps.supportedCompositeAlpha & option) {
            alpha = option;
            break;
        }
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    if (d->resourceFamilies.size() > 1) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = uint32_t(d->resourceFamilies.size());
        info.pQueueFamilyIndices = d->resourceFamilies.data();
    }
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = selected.format;
    info.imageColorSpace = selected.colorSpace;
    info.imageExtent = newExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Identity requests compositor rotation; shader pre-rotation is a future optimization.
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                            : caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;
    VkSwapchainKHR next = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(d->device, &info, nullptr, &next), "vkCreateSwapchainKHR");
    // oldSwapchain is retired after successful creation, including if view creation fails.
    for (auto view : views)
        d->destroyView(view);
    views.clear();
    images.clear();
    if (swapchain)
        vkDestroySwapchainKHR(d->device, swapchain, nullptr);
    swapchain = next;
    format = selected.format;
    extent = newExtent;
    needsRebuild = true;
    check(vkGetSwapchainImagesKHR(d->device, swapchain, &count, nullptr), "query swapchain images");
    images.resize(count);
    check(vkGetSwapchainImagesKHR(d->device, swapchain, &count, images.data()), "query swapchain images");
    views.reserve(count);
    for (auto img : images)
        views.push_back(makeView(*d, img, format));
    needsRebuild = false;
}
void Surface::resize(uint32_t width, uint32_t height) {
    require(width > 0 && height > 0, "Surface dimensions must be positive");
    requestedWidth = width;
    requestedHeight = height;
    rebuild();
}
std::shared_ptr<Drawable> Surface::acquire(uint64_t timeout) {
    require(!outstanding, "Only one drawable may be acquired per surface");
    d->collect();
    if (!d->pending.empty() || !d->retiredDrawables.empty())
        return nullptr;
    // Conservative pacing: presentation waits must finish before semaphores or
    // swapchain views can be reused/destroyed. No presentation-fence extension required.
    d->waitIdle();
    if (needsRebuild)
        rebuild();
    auto drawable = std::make_shared<Drawable>(shared_from_this());
    const auto result = vkAcquireNextImageKHR(d->device, swapchain, timeout, drawable->acquired, drawable->acquireFence,
                                              &drawable->index);
    if (result == VK_TIMEOUT || result == VK_NOT_READY)
        return nullptr;
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        needsRebuild = true;
        return nullptr;
    }
    if (result == VK_SUBOPTIMAL_KHR)
        needsRebuild = true;
    else
        check(result, "vkAcquireNextImageKHR");
    drawable->didAcquire = true;
    outstanding = true;
    drawable->texture = std::make_shared<Texture>(d, extent.width, extent.height, format, images.at(drawable->index),
                                                  views.at(drawable->index));
    drawable->texture->surface = shared_from_this();
    drawable->texture->frame = drawable->frame;
    return drawable;
}
Surface::~Surface() {
    // Capacity is reserved at construction; retirement cannot allocate or hold a Device reference.
    d->retiredSurfaces.push_back({surface, swapchain, std::move(views)
#ifdef __ANDROID__
                                                          ,
                                  window
#endif
    });
    --d->liveSurfaces;
}
Drawable::Drawable(std::shared_ptr<Surface> s) : Resource(s->d), surface(std::move(s)) {
    d->retiredDrawables.reserve(d->retiredDrawables.size() + d->liveDrawables + 1);
    try {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(d->device, &si, nullptr, &acquired), "create acquire semaphore");
        check(vkCreateSemaphore(d->device, &si, nullptr, &rendered), "create present semaphore");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(d->device, &fi, nullptr, &acquireFence), "create acquire fence");
        ++d->liveDrawables;
    } catch (...) {
        if (acquired)
            vkDestroySemaphore(d->device, acquired, nullptr);
        if (rendered)
            vkDestroySemaphore(d->device, rendered, nullptr);
        throw;
    }
}
Drawable::~Drawable() {
    if (didAcquire && !presented) {
        surface->outstanding = false;
        surface->needsRebuild = true;
    }
    frame->active = false;
    d->retiredDrawables.push_back({acquired, rendered, acquireFence, didAcquire});
    --d->liveDrawables;
}
} // namespace vulkano

namespace vulkano {
void Device::reclaimPresentation(bool shutdown) {
    if (retiredDrawables.empty() && retiredSurfaces.empty())
        return;
    // A render fence alone does not prove that the presentation semaphore wait has retired.
    const auto idle = vkQueueWaitIdle(queue);
    if (!shutdown)
        check(idle, "retire presentation work");
    for (const auto &item : retiredDrawables) {
        if (!item.didAcquire || !item.fence)
            continue;
        if (shutdown)
            vkWaitForFences(device, 1, &item.fence, VK_TRUE, UINT64_MAX);
        else {
            auto status = vkGetFenceStatus(device, item.fence);
            if (status == VK_NOT_READY)
                return;
            check(status, "retire acquired image");
        }
    }
    for (const auto &item : retiredDrawables) {
        if (item.fence)
            vkDestroyFence(device, item.fence, nullptr);
        if (item.acquired)
            vkDestroySemaphore(device, item.acquired, nullptr);
        if (item.rendered)
            vkDestroySemaphore(device, item.rendered, nullptr);
    }
    retiredDrawables.clear();
    for (const auto &item : retiredSurfaces) {
        for (auto view : item.views)
            destroyView(view);
        if (item.swapchain)
            vkDestroySwapchainKHR(device, item.swapchain, nullptr);
        if (item.surface)
            vkDestroySurfaceKHR(instance, item.surface, nullptr);
#ifdef __ANDROID__
        if (item.window)
            ANativeWindow_release(item.window);
#endif
    }
    retiredSurfaces.clear();
}
} // namespace vulkano
