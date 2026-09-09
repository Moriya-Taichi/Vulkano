#pragma once
#include "extensions.hpp"

namespace vulkano {
struct TensorOptions {
    VkFormat format = VK_FORMAT_R32_SFLOAT;
    VkTensorTilingARM tiling = VK_TENSOR_TILING_OPTIMAL_ARM;
    VkTensorUsageFlagsARM usage =
        VK_TENSOR_USAGE_SHADER_BIT_ARM | VK_TENSOR_USAGE_TRANSFER_SRC_BIT_ARM | VK_TENSOR_USAGE_TRANSFER_DST_BIT_ARM;
    std::vector<int64_t> dimensions, strides;
    VkTensorDescriptionARM description() const;
    uint64_t validate(const Device &, bool resource = true) const;
};
uint32_t tensorElementSize(VkFormat);
void reflectTensorBinding(Device &, const Shader &, uint32_t variable, BindingLayout &, bool graph = false);
VkFormatFeatureFlags2 tensorFormatFeatures(const Device &, VkFormat, VkTensorTilingARM);
struct TensorResource : Resource {
    VkTensorARM tensor = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocationSize = 0, linearSize = 0;
    void *mapped = nullptr;
    bool coherent = false;
    uint32_t inFlight = 0;
    TensorOptions options;
    Storage storage;
    TensorResource(std::shared_ptr<Device>, TensorOptions, Storage);
    void accessBytes(VkDeviceSize offset, void *data, size_t count, bool write);
    ~TensorResource() override;
};
struct TensorView : Resource {
    std::shared_ptr<TensorResource> tensor;
    VkTensorViewARM view = VK_NULL_HANDLE;
    VkFormat format;
    TensorView(std::shared_ptr<TensorResource>, VkFormat);
    ~TensorView() override;
};
} // namespace vulkano
