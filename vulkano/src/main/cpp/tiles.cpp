#include "tiles.hpp"
#include <algorithm>

namespace vulkano {
void validateTileOptions(Device &d, bool enabled, VkExtent2D apron) {
    require(enabled || (!apron.width && !apron.height), "Tile apron requires tile shading");
    if (!enabled)
        return;
    require(d.enabledExtra & TileShading, "Tile shading feature was not enabled");
    const auto &f = d.extensions->tile;
    const auto &p = d.extensions->tileProperties;
    require((!apron.width && !apron.height) || f.tileShadingApron, "Tile apron is unavailable");
    require(apron.width <= p.maxApronSize && apron.height <= p.maxApronSize, "Tile apron exceeds the device limit");
    require(apron.width == apron.height || f.tileShadingAnisotropicApron,
            "Device requires equal tile apron width and height");
}
VkSubpassDependency tileDependency(uint32_t subpass, uint32_t destination) {
    VkSubpassDependency dep{};
    dep.srcSubpass = subpass;
    dep.dstSubpass = destination;
    dep.srcStageMask = dep.dstStageMask =
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    // MEMORY_* covers the 64-bit tile attachment access flags even on synchronization1.
    dep.srcAccessMask = dep.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    return dep;
}
void tileBarrier(Command &c) {
    const auto dep = tileDependency(0, 0);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = dep.srcAccessMask;
    barrier.dstAccessMask = dep.dstAccessMask;
    vkCmdPipelineBarrier(c.command, dep.srcStageMask, dep.dstStageMask, dep.dependencyFlags, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}
void validateTileBinding(const Pipeline &pipeline, const BindingLayout &schema, const Binding &binding,
                         const Render &render, uint32_t subpass) {
    require(render.tileShading && binding.texture, "Tile descriptor requires a tile render attachment");
    auto &t = *binding.texture;
    require(t.options.mipLevels == 1 && t.imageType() == VK_IMAGE_TYPE_2D && t.options.layers == render.layers &&
                t.options.type == (render.layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY),
            "Tile descriptor view must cover the framebuffer attachment");
    const auto &sw = t.components;
    require((sw.r == VK_COMPONENT_SWIZZLE_IDENTITY || sw.r == VK_COMPONENT_SWIZZLE_R) &&
                (sw.g == VK_COMPONENT_SWIZZLE_IDENTITY || sw.g == VK_COMPONENT_SWIZZLE_G) &&
                (sw.b == VK_COMPONENT_SWIZZLE_IDENTITY || sw.b == VK_COMPONENT_SWIZZLE_B) &&
                (sw.a == VK_COMPONENT_SWIZZLE_IDENTITY || sw.a == VK_COMPONENT_SWIZZLE_A),
            "Tile attachment descriptors require identity components");
    uint32_t index = UINT32_MAX;
    auto match = [&](const std::shared_ptr<Texture> &a, uint32_t mip, uint32_t layer, uint32_t slot) {
        if (a && &a->root() == &t.root() && a->format == t.format && a->baseMip + mip == t.baseMip &&
            a->baseLayer + layer == t.baseLayer)
            index = slot;
    };
    for (uint32_t n = 0; n < render.colors.size(); ++n)
        match(render.colors[n].texture, render.colors[n].mip, render.colors[n].layer, n);
    match(render.depth, render.depthMip, render.depthLayer, uint32_t(render.colors.size()));
    if (render.passLayout)
        for (uint32_t n = 0; n < render.passLayout->resolveColors.size(); ++n) {
            const auto &a = render.colors[render.passLayout->resolveColors[n]];
            match(a.resolve, a.resolveMip, a.resolveLayer, uint32_t(render.colors.size()) + bool(render.depth) + n);
        }
    require(index != UINT32_MAX, "Tile descriptor does not match a framebuffer attachment");
    bool input = false, color = index < render.colors.size(), depth = index == render.colors.size() && render.depth;
    if (render.passLayout) {
        const auto &s = render.passLayout->subpasses.at(subpass);
        input = std::find(s.inputs.begin(), s.inputs.end(), index) != s.inputs.end();
        color = std::find(s.colors.begin(), s.colors.end(), index) != s.colors.end();
        depth = depth && s.depth;
    }
    require(input || color || depth, "Tile attachment is not used by this subpass");
    const auto &f = pipeline.d->extensions->tile;
    require(!input || f.tileShadingInputAttachments, "Tile input attachments are unavailable");
    require(!color || f.tileShadingColorAttachments, "Tile color attachments are unavailable");
    require(!depth || (schema.numericType == 2 ? f.tileShadingStencilAttachments : f.tileShadingDepthAttachments),
            "Tile depth/stencil attachment access is unavailable");
    require((pipeline.compute && color) || schema.readonly,
            "Fragment, depth/stencil and input tile attachments must be declared readonly");
    const bool sampled =
        schema.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || schema.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    require(!sampled || f.tileShadingSampledAttachments, "Sampled tile attachments are unavailable");
}
} // namespace vulkano
