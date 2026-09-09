#pragma once
#include "extensions.hpp"
namespace vulkano {
void validateTileOptions(Device &, bool enabled, VkExtent2D apron);
VkSubpassDependency tileDependency(uint32_t subpass, uint32_t destination);
void tileBarrier(Command &);
void validateTileBinding(const Pipeline &, const BindingLayout &, const Binding &, const Render &, uint32_t subpass);
} // namespace vulkano
