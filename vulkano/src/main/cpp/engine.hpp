#pragma once

#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace vulkano {

void require(bool condition, const char* message);
void check(VkResult result, const char* operation);
struct Device;
struct Command;
struct Surface;
struct Drawable;

enum Feature : uint32_t {
    Anisotropy = 1, Int16 = 2, Storage16 = 4, Float16 = 8,
    Astc = 16, Etc2 = 32, ExtendedStorageFormats = 64
};
enum class Storage { Shared, Private, Memoryless };

struct Object {
    virtual ~Object() = default;
    virtual Device* owner() const = 0;
};
struct Device : Object, std::enable_shared_from_this<Device> {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0, available = 0, enabled = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, nullptr, 0, 0, 0, VK_FALSE};
    VmaAllocator allocator = VK_NULL_HANDLE;
    bool memoryBudget = false;
    std::vector<std::weak_ptr<Command>> pending;
    static std::shared_ptr<Device> create(uint32_t required, bool validation, bool allowSoftware);
    Device* owner() const override { return const_cast<Device*>(this); }
    void collect();
    void waitIdle();
    ~Device() override;
};
struct Resource : Object {
    std::shared_ptr<Device> d;
    explicit Resource(std::shared_ptr<Device> device) : d(std::move(device)) {}
    Device* owner() const override { return d.get(); }
};
struct Buffer : Resource {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    Storage storage;
    uint32_t inFlight = 0;
    Buffer(std::shared_ptr<Device>, VkDeviceSize, VkBufferUsageFlags, Storage);
    void write(VkDeviceSize offset, const void* bytes, size_t count);
    void read(VkDeviceSize offset, void* bytes, size_t count);
    ~Buffer() override;
};
struct FrameState { bool active = true; };
struct Texture : Resource {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkFormat format;
    uint32_t width, height;
    VkImageUsageFlags usage;
    Storage storage;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool initialized = false, lazy = false, borrowed = false;
    std::shared_ptr<Surface> surface;
    std::shared_ptr<FrameState> frame;
    Texture(std::shared_ptr<Device>, uint32_t, uint32_t, VkFormat, VkImageUsageFlags, Storage);
    Texture(std::shared_ptr<Device>, uint32_t, uint32_t, VkFormat, VkImage, VkImageView);
    bool depth() const { return format == VK_FORMAT_D32_SFLOAT; }
    uint32_t pixelSize() const;
    void usable() const;
    ~Texture() override;
};
struct Sampler : Resource {
    VkSampler sampler = VK_NULL_HANDLE;
    bool linear;
    Sampler(std::shared_ptr<Device>, bool linear, bool repeat, float anisotropy);
    ~Sampler() override;
};

// Descriptor set 0, one resource at each explicit binding. Stage visibility is
// the pipeline's stages; graphics resources are read-only in the public API.
struct BindingLayout {
    uint32_t binding;
    VkDescriptorType type;
    VkFormat storageFormat = VK_FORMAT_UNDEFINED;
    uint32_t minimumBytes = 0;
};
struct Shader { std::vector<uint32_t> code; std::string entry = "main"; };
struct Pipeline : Resource {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkRenderPass compatiblePass = VK_NULL_HANDLE;
    std::vector<BindingLayout> bindings;
    uint32_t pushBytes;
    bool compute;
    std::array<uint32_t, 3> localSize{1, 1, 1};
    VkFormat colorFormat = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED;
    Pipeline(std::shared_ptr<Device>, std::vector<BindingLayout>, uint32_t, const Shader&);
    Pipeline(std::shared_ptr<Device>, std::vector<BindingLayout>, uint32_t,
             const Shader& vertex, const Shader& fragment, VkFormat color, VkFormat depth, bool blend);
    void makeLayout();
    ~Pipeline() override;
};
struct Binding {
    uint32_t index;
    std::shared_ptr<Buffer> buffer;
    VkDeviceSize offset = 0, length = 0;
    std::shared_ptr<Texture> texture;
    std::shared_ptr<Sampler> sampler;
};
struct Dispatch {
    std::shared_ptr<Pipeline> pipeline;
    std::vector<Binding> bindings;
    std::vector<uint8_t> constants;
    std::array<uint32_t, 3> groups;
};
struct Draw {
    std::shared_ptr<Pipeline> pipeline;
    std::vector<Binding> bindings;
    std::vector<uint8_t> constants;
    uint32_t vertices = 3, instances = 1, firstVertex = 0, firstInstance = 0;
};
struct Render {
    std::shared_ptr<Texture> color, depth;
    VkAttachmentLoadOp colorLoad = VK_ATTACHMENT_LOAD_OP_CLEAR, depthLoad = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp colorStore = VK_ATTACHMENT_STORE_OP_STORE, depthStore = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    std::array<float, 4> clearColor{0, 0, 0, 1};
    float clearDepth = 1;
    std::vector<Draw> draws;
};
struct ImageState { VkImageLayout layout; bool initialized; };
struct Command : Resource, std::enable_shared_from_this<Command> {
    enum class State { Recording, Submitted, Completed, Failed };
    State state = State::Recording;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> descriptorPools;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkRenderPass> passes;
    std::vector<std::function<void(Command&)>> operations;
    std::vector<std::shared_ptr<Buffer>> buffers;
    std::unordered_map<Texture*, ImageState> images;
    std::shared_ptr<Drawable> presentation;
    explicit Command(std::shared_ptr<Device>);
    void recording() const;
    void dispatch(Dispatch);
    void render(Render);
    void copy(std::shared_ptr<Buffer> src, std::shared_ptr<Buffer> dst, VkDeviceSize srcOffset, VkDeviceSize dstOffset, VkDeviceSize size);
    void copy(std::shared_ptr<Buffer>, std::shared_ptr<Texture>, VkDeviceSize offset, bool toTexture);
    void present(std::shared_ptr<Drawable>);
    void commit();
    bool wait(uint64_t timeout = UINT64_MAX);
    void barrier();
    void transition(Texture&, VkImageLayout, bool read);
    void markInitialized(Texture&, bool);
    void validateBindings(const Pipeline&, const std::vector<Binding>&, const std::vector<uint8_t>&);
    void bind(const Pipeline&, const std::vector<Binding>&, const std::vector<uint8_t>&);
    void prepare(const std::vector<Binding>&, bool compute);
    ~Command() override;
};

struct Surface : Resource, std::enable_shared_from_this<Surface> {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    bool outstanding = false, needsRebuild = false;
    uint32_t requestedWidth, requestedHeight;
#ifdef __ANDROID__
    ANativeWindow* window = nullptr;
    Surface(std::shared_ptr<Device>, ANativeWindow*, uint32_t, uint32_t);
#endif
    void rebuild();
    void resize(uint32_t, uint32_t);
    std::shared_ptr<Drawable> acquire(uint64_t timeout);
    ~Surface() override;
};
struct Drawable : Resource {
    std::shared_ptr<Surface> surface;
    std::shared_ptr<Texture> texture;
    std::shared_ptr<FrameState> frame = std::make_shared<FrameState>();
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    VkFence acquireFence = VK_NULL_HANDLE;
    uint32_t index = 0;
    bool didAcquire = false, presented = false;
    explicit Drawable(std::shared_ptr<Surface>);
    ~Drawable() override;
};

} // namespace vulkano
