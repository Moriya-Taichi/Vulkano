#include "engine.hpp"
#include "tiles.hpp"
#include <algorithm>
#include <set>
namespace vulkano {
namespace {
bool hasDepth(VkFormat f) {
    return f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT && f != VK_FORMAT_S8_UINT;
}
bool hasStencil(VkFormat f) { return f >= VK_FORMAT_S8_UINT && f <= VK_FORMAT_D32_SFLOAT_S8_UINT; }
VkImageAspectFlags aspects(VkFormat f) {
    return (hasDepth(f) ? uint32_t(VK_IMAGE_ASPECT_DEPTH_BIT) : 0u) |
           (hasStencil(f) ? uint32_t(VK_IMAGE_ASPECT_STENCIL_BIT) : 0u) |
           (!hasDepth(f) && !hasStencil(f) ? uint32_t(VK_IMAGE_ASPECT_COLOR_BIT) : 0u);
}
} // namespace
bool SubpassLayout::hasDepthResolve() const {
    return std::any_of(subpasses.begin(), subpasses.end(), [](const auto &s) { return s.resolveDepth; });
}
uint32_t SubpassLayout::depthResolveIndex() const {
    return uint32_t(colors.size()) + (depth != VK_FORMAT_UNDEFINED) + uint32_t(resolveColors.size());
}
Attachment subpassAttachment(const Render &r, uint32_t index) {
    require(bool(r.passLayout), "Subpass attachment requires a layout");
    if (index < r.colors.size())
        return r.colors[index];
    Attachment a;
    if (r.depth && index == r.colors.size()) {
        a.texture = r.depth;
        a.mip = r.depthMip;
        a.layer = r.depthLayer;
        a.load = r.depthLoad;
        a.store = r.depthStore;
    } else {
        const auto firstResolve = r.colors.size() + bool(r.depth);
        if (index >= firstResolve && index - firstResolve < r.passLayout->resolveColors.size()) {
            const auto &source = r.colors[r.passLayout->resolveColors[index - firstResolve]];
            a.texture = source.resolve;
            a.mip = source.resolveMip;
            a.layer = source.resolveLayer;
        } else {
            require(r.passLayout->hasDepthResolve() && index == r.passLayout->depthResolveIndex(),
                    "Invalid subpass attachment index");
            a.texture = r.depthResolve;
            a.mip = r.depthResolveMip;
            a.layer = r.depthResolveLayer;
        }
        a.load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return a;
}
std::shared_ptr<SubpassLayout> parseSubpassLayout(const std::vector<int> &raw) {
    if (raw.empty())
        return {};
    require(raw.size() >= 4 && raw[0] >= 0 && raw[0] <= 256 && raw[3] > 0 && raw[3] <= 256,
            "Invalid render pass layout counts");
    auto layout = std::make_shared<SubpassLayout>();
    layout->depth = VkFormat(raw[1]);
    layout->samples = VkSampleCountFlagBits(raw[2]);
    layout->key = raw;
    size_t at = 4;
    require(raw.size() >= at + size_t(raw[0]), "Truncated attachment formats");
    for (int n = 0; n < raw[0]; ++n)
        layout->colors.push_back(VkFormat(raw[at++]));
    for (int n = 0; n < raw[3]; ++n) {
        require(raw.size() >= at + 3 && raw[at] >= 0 && raw[at + 1] >= 0 &&
                    (raw[at + 2] == 0 || raw[at + 2] == 1 || raw[at + 2] == 3),
                "Invalid subpass layout");
        const size_t colors = raw[at], inputs = raw[at + 1];
        SubpassDescription sub;
        sub.depth = raw[at + 2] & 1;
        sub.resolveDepth = raw[at + 2] & 2;
        at += 3;
        require(colors <= 256 && inputs <= 256 && raw.size() >= at + colors + inputs, "Truncated subpass indices");
        for (size_t i = 0; i < colors; ++i) {
            require(raw[at] >= 0, "Invalid color attachment index");
            sub.colors.push_back(raw[at++]);
        }
        for (size_t i = 0; i < inputs; ++i) {
            require(raw[at] >= 0, "Invalid input attachment index");
            sub.inputs.push_back(raw[at++]);
        }
        layout->subpasses.push_back(std::move(sub));
    }
    require(at < raw.size() && raw[at] >= 0 && raw[at] <= raw[0], "Invalid subpass resolve count");
    const auto resolveCount = size_t(raw[at++]);
    require(raw.size() == at + resolveCount + 2, "Truncated or trailing render pass layout data");
    for (size_t n = 0; n < resolveCount; ++n) {
        require(raw[at] >= 0, "Invalid subpass resolve index");
        layout->resolveColors.push_back(uint32_t(raw[at++]));
    }
    layout->depthResolveMode = VkResolveModeFlagBits(raw[at++]);
    layout->stencilResolveMode = VkResolveModeFlagBits(raw[at++]);
    return layout;
}
void validateSubpassLayout(Device &d, const SubpassLayout &layout) {
    require(!layout.subpasses.empty() && (!layout.colors.empty() || layout.depth != VK_FORMAT_UNDEFINED),
            "Empty render pass layout");
    require(layout.samples && !(layout.samples & (layout.samples - 1)) && layout.samples <= VK_SAMPLE_COUNT_64_BIT,
            "Invalid render pass sample count");
    for (auto format : layout.colors) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(d.physical, format, &props);
        require(props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
                "Unsupported subpass color format");
    }
    if (layout.depth != VK_FORMAT_UNDEFINED) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(d.physical, layout.depth, &props);
        require(props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
                "Unsupported subpass depth format");
    }
    if (layout.hasDepthResolve()) {
        require((d.enabled & DepthResolve) && layout.samples > 1 &&
                    (hasDepth(layout.depth) || hasStencil(layout.depth)),
                "Subpass depth resolve requires an enabled feature and multisampled depth/stencil");
        const auto &p = d.extensions->depthResolveProperties;
        auto supported = [](VkResolveModeFlagBits mode, VkResolveModeFlags supported) {
            return mode && !(mode & (mode - 1)) && (mode & supported);
        };
        require(!hasDepth(layout.depth) || supported(layout.depthResolveMode, p.supportedDepthResolveModes),
                "Unsupported subpass depth resolve mode");
        require(!hasStencil(layout.depth) || supported(layout.stencilResolveMode, p.supportedStencilResolveModes),
                "Unsupported subpass stencil resolve mode");
        require(!hasDepth(layout.depth) || !hasStencil(layout.depth) || p.independentResolve ||
                    layout.depthResolveMode == layout.stencilResolveMode,
                "Device requires matching resolve modes");
    }
    const auto &limits = d.properties.limits;
    std::set<uint32_t> resolved;
    require(layout.resolveColors.empty() || layout.samples > 1, "Resolve requires multisampled color attachments");
    require(std::is_sorted(layout.resolveColors.begin(), layout.resolveColors.end()), "Resolve indices must be sorted");
    for (auto index : layout.resolveColors)
        require(index < layout.colors.size() && resolved.insert(index).second, "Invalid/duplicate resolve source");
    const auto count = layout.depthResolveIndex() + layout.hasDepthResolve();
    for (const auto &sub : layout.subpasses) {
        require(sub.colors.size() <= limits.maxColorAttachments &&
                    sub.inputs.size() <= limits.maxPerStageDescriptorInputAttachments,
                "Subpass exceeds attachment limits");
        std::set<uint32_t> writes;
        for (auto i : sub.colors) {
            require(i < layout.colors.size() && writes.insert(i).second, "Invalid/duplicate subpass color index");
            const auto resolve = std::find(layout.resolveColors.begin(), layout.resolveColors.end(), i);
            if (resolve != layout.resolveColors.end())
                writes.insert(uint32_t(layout.colors.size()) + (layout.depth != VK_FORMAT_UNDEFINED) +
                              uint32_t(resolve - layout.resolveColors.begin()));
        }
        require(!sub.resolveDepth || sub.depth, "Depth resolve requires a depth/stencil source in the subpass");
        if (sub.resolveDepth)
            writes.insert(layout.depthResolveIndex());
        if (sub.depth) {
            require(layout.depth != VK_FORMAT_UNDEFINED, "Subpass requires a depth attachment format");
            writes.insert(uint32_t(layout.colors.size()));
        }
        for (auto i : sub.inputs)
            require(i < count && !writes.count(i), "Input attachment must be distinct from writable attachments");
    }
}
VkRenderPass makeSubpassPass(Device &d, const SubpassLayout &layout, const std::vector<Attachment> &targets,
                             VkAttachmentLoadOp depthLoad, VkAttachmentStoreOp depthStore, uint32_t viewMask,
                             bool tileShading, VkExtent2D tileApron, VkExtent2D rateMapTexelSize, const Render *render) {
    validateSubpassLayout(d, layout);
    require(targets.empty() || targets.size() == layout.colors.size(), "Subpass target count mismatch");
    validateTileOptions(d, tileShading, tileApron);
    const bool depthResolve = layout.hasDepthResolve(), rateMap = rateMapTexelSize.width != 0;
    require(!rateMap || (d.enabled & AttachmentRate), "Attachment shading rate was not enabled");
    std::vector<int> key{-31,
                         tileShading,
                         int(tileApron.width),
                         int(tileApron.height),
                         int(rateMapTexelSize.width),
                         int(rateMapTexelSize.height)};
    key.insert(key.end(), layout.key.begin(), layout.key.end());
    key.insert(key.end(), {depthLoad, depthStore, int(viewMask)});
    const auto stencilLoad = render ? render->stencilLoadOp() : depthLoad;
    const auto stencilStore = render ? render->stencilStoreOp() : depthStore;
    const auto depthLayout = render ? render->depthLayout() : (tileShading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    key.insert(key.end(), {stencilLoad, stencilStore, int(depthLayout)});
    const auto baseCount = layout.colors.size() + (layout.depth != VK_FORMAT_UNDEFINED);
    const auto rateIndex = layout.depthResolveIndex() + depthResolve;
    const auto count = rateIndex + rateMap;
    std::map<uint32_t, uint32_t> resolves;
    for (size_t n = 0; n < layout.resolveColors.size(); ++n)
        resolves.emplace(layout.resolveColors[n], baseCount + n);
    std::vector<bool> firstInput(count, false), seen(count, false);
    for (const auto &sub : layout.subpasses) {
        for (auto i : sub.inputs) {
            if (!seen[i])
                firstInput[i] = true;
            seen[i] = true;
        }
        for (auto i : sub.colors) {
            seen[i] = true;
            if (resolves.count(i))
                seen[resolves[i]] = true;
        }
        if (sub.depth)
            seen[layout.colors.size()] = true;
        if (sub.resolveDepth)
            seen[layout.depthResolveIndex()] = true;
    }
    std::vector<VkAttachmentDescription> attachments(count);
    for (uint32_t i = 0; i < count; ++i) {
        const bool resolvedDepth = depthResolve && i == layout.depthResolveIndex();
        const bool depth = (layout.depth != VK_FORMAT_UNDEFINED && i == layout.colors.size()) || resolvedDepth;
        const bool rate = rateMap && i == rateIndex;
        const bool resolve = i >= baseCount && !rate;
        auto &a = attachments[i];
        a.format = rate      ? VK_FORMAT_R8_UINT
                   : depth   ? layout.depth
                   : resolve ? layout.colors[layout.resolveColors[i - baseCount]]
                             : layout.colors[i];
        a.samples = (resolve || rate) ? VK_SAMPLE_COUNT_1_BIT : layout.samples;
        a.loadOp = rate              ? VK_ATTACHMENT_LOAD_OP_LOAD
                   : resolve         ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                   : depth           ? depthLoad
                   : targets.empty() ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                     : targets[i].load;
        a.storeOp = (resolve || rate) ? VK_ATTACHMENT_STORE_OP_STORE
                    : depth           ? depthStore
                    : targets.empty() ? VK_ATTACHMENT_STORE_OP_STORE
                                      : targets[i].store;
        if (firstInput[i] && !render)
            a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        require(!firstInput[i] || (depth && !hasDepth(a.format)) || a.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD, "First input use requires LOAD");
        require(depth || resolve || rate || targets.empty() || bool(targets[i].resolve) == bool(resolves.count(i)),
                "Resolve targets do not match render pass layout");
        a.stencilLoadOp = depth ? (resolve || !render ? a.loadOp : stencilLoad) : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = depth ? (resolve ? a.storeOp : stencilStore) : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        require(!firstInput[i] || !hasStencil(a.format) || a.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD,
                "First stencil input use requires LOAD");
        a.initialLayout = a.finalLayout = rate          ? VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR
                                          : tileShading ? VK_IMAGE_LAYOUT_GENERAL
                                          : depth       ? (resolve ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : depthLayout)
                                                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        key.insert(key.end(), {a.loadOp, a.storeOp, a.stencilLoadOp, a.stencilStoreOp});
    }
    const auto found = d.renderPassCache.find(key);
    if (found != d.renderPassCache.end())
        return found->second;
    struct References {
        std::vector<VkAttachmentReference> colors, inputs, resolves;
        VkAttachmentReference depth{};
        std::vector<uint32_t> preserve;
    };
    std::vector<References> refs(layout.subpasses.size());
    std::vector<VkSubpassDescription> subpasses(layout.subpasses.size());
    for (size_t n = 0; n < subpasses.size(); ++n) {
        auto &ref = refs[n];
        auto &sub = subpasses[n];
        const auto &source = layout.subpasses[n];
        std::set<uint32_t> used;
        bool anyResolve = false;
        for (auto i : source.colors) {
            ref.colors.push_back({i, tileShading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
            used.insert(i);
            const auto target = resolves.count(i) ? resolves[i] : VK_ATTACHMENT_UNUSED;
            ref.resolves.push_back(
                {target, tileShading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
            if (target != VK_ATTACHMENT_UNUSED) {
                used.insert(target);
                anyResolve = true;
            }
        }
        for (auto i : source.inputs) {
            ref.inputs.push_back({i, tileShading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            used.insert(i);
        }
        if (tileApron.width || tileApron.height)
            sub.flags |= VK_SUBPASS_DESCRIPTION_TILE_SHADING_APRON_BIT_QCOM;
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = uint32_t(ref.colors.size());
        sub.pColorAttachments = ref.colors.data();
        sub.inputAttachmentCount = uint32_t(ref.inputs.size());
        sub.pInputAttachments = ref.inputs.data();
        sub.pResolveAttachments = anyResolve ? ref.resolves.data() : nullptr;
        if (source.depth) {
            ref.depth = {uint32_t(layout.colors.size()),
                         depthLayout};
            used.insert(ref.depth.attachment);
            sub.pDepthStencilAttachment = &ref.depth;
        }
        if (source.resolveDepth)
            used.insert(layout.depthResolveIndex());
        if (rateMap)
            used.insert(rateIndex);
        for (uint32_t i = 0; i < count; ++i)
            if (!used.count(i))
                ref.preserve.push_back(i);
        sub.preserveAttachmentCount = uint32_t(ref.preserve.size());
        sub.pPreserveAttachments = ref.preserve.data();
    }
    std::vector<VkSubpassDependency> dependencies;
    const VkPipelineStageFlags stages =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    for (uint32_t dst = 1; dst < subpasses.size(); ++dst)
        for (uint32_t src = 0; src < dst; ++src) {
            VkSubpassDependency dep{};
            dep.srcSubpass = src;
            dep.dstSubpass = dst;
            dep.srcStageMask = dep.dstStageMask = stages;
            dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
            dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
            dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
            if (tileShading)
                dep = tileDependency(src, dst);
            dependencies.push_back(dep);
        }
    if (tileShading) {
        for (uint32_t n = 0; n < subpasses.size(); ++n)
            dependencies.push_back(tileDependency(n, n));
        if (viewMask)
            for (auto &dep : dependencies)
                dep.dependencyFlags |= VK_DEPENDENCY_VIEW_LOCAL_BIT;
    }
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = uint32_t(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = uint32_t(subpasses.size());
    info.pSubpasses = subpasses.data();
    info.dependencyCount = uint32_t(dependencies.size());
    info.pDependencies = dependencies.data();
    VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    std::vector<uint32_t> viewMasks(subpasses.size(), viewMask);
    if (viewMask) {
        require(d.enabled & Multiview, "Multiview was not enabled");
        multiview.subpassCount = uint32_t(viewMasks.size());
        multiview.pViewMasks = viewMasks.data();
        multiview.correlationMaskCount = 1;
        multiview.pCorrelationMasks = &viewMask;
        info.pNext = &multiview;
    }
    VkRenderPassTileShadingCreateInfoQCOM tileInfo{VK_STRUCTURE_TYPE_RENDER_PASS_TILE_SHADING_CREATE_INFO_QCOM};
    if (tileShading) {
        tileInfo.flags = VK_TILE_SHADING_RENDER_PASS_ENABLE_BIT_QCOM;
        tileInfo.tileApronSize = tileApron;
        tileInfo.pNext = info.pNext;
        info.pNext = &tileInfo;
    }
    VkRenderPass pass;
    if (!depthResolve && !rateMap) {
        check(vkCreateRenderPass(d.device, &info, nullptr, &pass), "create subpass render pass");
    } else {
        std::vector<VkAttachmentDescription2> attachments2;
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
            attachments2.push_back(b);
        }
        auto reference = [&](const VkAttachmentReference &a) {
            VkAttachmentReference2 b{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
            b.attachment = a.attachment;
            b.layout = a.layout;
            if (a.attachment != VK_ATTACHMENT_UNUSED)
                b.aspectMask = aspects(attachments[a.attachment].format);
            return b;
        };
        struct References2 {
            std::vector<VkAttachmentReference2> colors, inputs, resolves;
            VkAttachmentReference2 depth{}, resolvedDepth{}, rate{};
            VkSubpassDescriptionDepthStencilResolve depthInfo{
                VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
            VkFragmentShadingRateAttachmentInfoKHR rateInfo{
                VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
        };
        std::vector<References2> refs2(refs.size());
        std::vector<VkSubpassDescription2> subs2(refs.size(), {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2});
        for (size_t n = 0; n < refs.size(); ++n) {
            const auto &old = subpasses[n];
            auto &r = refs2[n];
            auto &sub = subs2[n];
            for (const auto &v : refs[n].colors)
                r.colors.push_back(reference(v));
            for (const auto &v : refs[n].inputs)
                r.inputs.push_back(reference(v));
            for (const auto &v : refs[n].resolves)
                r.resolves.push_back(reference(v));
            r.depth = reference(refs[n].depth);
            sub.flags = old.flags;
            sub.pipelineBindPoint = old.pipelineBindPoint;
            sub.viewMask = viewMask;
            sub.colorAttachmentCount = old.colorAttachmentCount;
            sub.pColorAttachments = r.colors.data();
            sub.inputAttachmentCount = old.inputAttachmentCount;
            sub.pInputAttachments = r.inputs.data();
            sub.pResolveAttachments = old.pResolveAttachments ? r.resolves.data() : nullptr;
            sub.pDepthStencilAttachment = old.pDepthStencilAttachment ? &r.depth : nullptr;
            sub.preserveAttachmentCount = old.preserveAttachmentCount;
            sub.pPreserveAttachments = old.pPreserveAttachments;
            if (layout.subpasses[n].resolveDepth) {
                r.resolvedDepth = reference(
                    {layout.depthResolveIndex(),
                     tileShading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});
                r.depthInfo.depthResolveMode = hasDepth(layout.depth) ? layout.depthResolveMode : VK_RESOLVE_MODE_NONE;
                r.depthInfo.stencilResolveMode =
                    hasStencil(layout.depth) ? layout.stencilResolveMode : VK_RESOLVE_MODE_NONE;
                r.depthInfo.pDepthStencilResolveAttachment = &r.resolvedDepth;
                sub.pNext = &r.depthInfo;
            }
            if (rateMap) {
                r.rate = reference({rateIndex, VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR});
                r.rateInfo.pFragmentShadingRateAttachment = &r.rate;
                r.rateInfo.shadingRateAttachmentTexelSize = rateMapTexelSize;
                r.rateInfo.pNext = sub.pNext;
                sub.pNext = &r.rateInfo;
            }
        }
        std::vector<VkSubpassDependency2> deps2;
        for (const auto &a : dependencies) {
            VkSubpassDependency2 b{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
            b.srcSubpass = a.srcSubpass;
            b.dstSubpass = a.dstSubpass;
            b.srcStageMask = a.srcStageMask;
            b.dstStageMask = a.dstStageMask;
            b.srcAccessMask = a.srcAccessMask;
            b.dstAccessMask = a.dstAccessMask;
            b.dependencyFlags = a.dependencyFlags;
            deps2.push_back(b);
        }
        VkRenderPassCreateInfo2 info2{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        info2.attachmentCount = uint32_t(attachments2.size());
        info2.pAttachments = attachments2.data();
        info2.subpassCount = uint32_t(subs2.size());
        info2.pSubpasses = subs2.data();
        info2.dependencyCount = uint32_t(deps2.size());
        info2.pDependencies = deps2.data();
        info2.correlatedViewMaskCount = viewMask ? 1 : 0;
        info2.pCorrelatedViewMasks = &viewMask;
        if (tileShading) {
            tileInfo.pNext = nullptr;
            info2.pNext = &tileInfo;
        }
        require(d.extensions->createRenderPass2 != nullptr, "Render pass 2 is unavailable");
        check(d.extensions->createRenderPass2(d.device, &info2, nullptr, &pass), "create extended subpass render pass");
    }
    try {
        d.renderPassCache.emplace(std::move(key), pass);
    } catch (...) {
        vkDestroyRenderPass(d.device, pass, nullptr);
        throw;
    }
    return pass;
}
} // namespace vulkano
