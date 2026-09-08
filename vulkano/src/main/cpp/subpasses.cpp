#include "engine.hpp"
#include <algorithm>
#include <set>
namespace vulkano {
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
        require(raw.size() >= at + 3 && raw[at] >= 0 && raw[at + 1] >= 0 && (raw[at + 2] == 0 || raw[at + 2] == 1),
                "Invalid subpass layout");
        const size_t colors = raw[at], inputs = raw[at + 1];
        SubpassDescription sub;
        sub.depth = raw[at + 2];
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
    require(raw.size() == at + resolveCount, "Trailing render pass layout data");
    for (size_t n = 0; n < resolveCount; ++n) {
        require(raw[at] >= 0, "Invalid subpass resolve index");
        layout->resolveColors.push_back(uint32_t(raw[at++]));
    }
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
    const auto &limits = d.properties.limits;
    std::set<uint32_t> resolved;
    require(layout.resolveColors.empty() || layout.samples > 1, "Resolve requires multisampled color attachments");
    require(std::is_sorted(layout.resolveColors.begin(), layout.resolveColors.end()), "Resolve indices must be sorted");
    for (auto index : layout.resolveColors)
        require(index < layout.colors.size() && resolved.insert(index).second, "Invalid/duplicate resolve source");
    const auto count = layout.colors.size() + (layout.depth != VK_FORMAT_UNDEFINED) + layout.resolveColors.size();
    for (const auto &sub : layout.subpasses) {
        require(sub.colors.size() <= limits.maxColorAttachments &&
                    sub.inputs.size() <= limits.maxPerStageDescriptorInputAttachments,
                "Subpass exceeds attachment limits");
        std::set<uint32_t> writes;
        for (auto i : sub.colors)
            require(i < layout.colors.size() && writes.insert(i).second, "Invalid/duplicate subpass color index");
        if (sub.depth) {
            require(layout.depth != VK_FORMAT_UNDEFINED, "Subpass requires a depth attachment format");
            writes.insert(uint32_t(layout.colors.size()));
        }
        for (auto i : sub.inputs)
            require(i < count && !writes.count(i), "Input attachment must be distinct from writable attachments");
    }
}
VkRenderPass makeSubpassPass(Device &d, const SubpassLayout &layout, const std::vector<Attachment> &targets,
                             VkAttachmentLoadOp depthLoad, VkAttachmentStoreOp depthStore, uint32_t viewMask) {
    validateSubpassLayout(d, layout);
    require(targets.empty() || targets.size() == layout.colors.size(), "Subpass target count mismatch");
    std::vector<int> key{-31};
    key.insert(key.end(), layout.key.begin(), layout.key.end());
    key.insert(key.end(), {depthLoad, depthStore, int(viewMask)});
    const auto baseCount = layout.colors.size() + (layout.depth != VK_FORMAT_UNDEFINED);
    const auto count = baseCount + layout.resolveColors.size();
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
    }
    std::vector<VkAttachmentDescription> attachments(count);
    for (uint32_t i = 0; i < count; ++i) {
        const bool depth = layout.depth != VK_FORMAT_UNDEFINED && i == layout.colors.size();
        const bool resolve = i >= baseCount;
        auto &a = attachments[i];
        a.format = depth     ? layout.depth
                   : resolve ? layout.colors[layout.resolveColors[i - baseCount]]
                             : layout.colors[i];
        a.samples = resolve ? VK_SAMPLE_COUNT_1_BIT : layout.samples;
        a.loadOp = resolve           ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                   : depth           ? depthLoad
                   : targets.empty() ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                     : targets[i].load;
        a.storeOp = resolve           ? VK_ATTACHMENT_STORE_OP_STORE
                    : depth           ? depthStore
                    : targets.empty() ? VK_ATTACHMENT_STORE_OP_STORE
                                      : targets[i].store;
        if (firstInput[i] && targets.empty())
            a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        require(!firstInput[i] || a.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD, "First input use requires LOAD");
        require(depth || resolve || targets.empty() || bool(targets[i].resolve) == bool(resolves.count(i)),
                "Resolve targets do not match render pass layout");
        a.stencilLoadOp = depth ? a.loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = depth ? a.storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout =
            depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        key.insert(key.end(), {a.loadOp, a.storeOp});
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
            ref.colors.push_back({i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
            used.insert(i);
            const auto target = resolves.count(i) ? resolves[i] : VK_ATTACHMENT_UNUSED;
            ref.resolves.push_back({target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
            if (target != VK_ATTACHMENT_UNUSED) {
                used.insert(target);
                anyResolve = true;
            }
        }
        for (auto i : source.inputs) {
            ref.inputs.push_back({i, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            used.insert(i);
        }
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = uint32_t(ref.colors.size());
        sub.pColorAttachments = ref.colors.data();
        sub.inputAttachmentCount = uint32_t(ref.inputs.size());
        sub.pInputAttachments = ref.inputs.data();
        sub.pResolveAttachments = anyResolve ? ref.resolves.data() : nullptr;
        if (source.depth) {
            ref.depth = {uint32_t(layout.colors.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            used.insert(ref.depth.attachment);
            sub.pDepthStencilAttachment = &ref.depth;
        }
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
            dependencies.push_back(dep);
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
    VkRenderPass pass;
    check(vkCreateRenderPass(d.device, &info, nullptr, &pass), "create subpass render pass");
    try {
        d.renderPassCache.emplace(std::move(key), pass);
    } catch (...) {
        vkDestroyRenderPass(d.device, pass, nullptr);
        throw;
    }
    return pass;
}
} // namespace vulkano
