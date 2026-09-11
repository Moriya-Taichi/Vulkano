#pragma once

#include "vk_mem_alloc.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace vulkano {

void require(bool condition, const char *message);
void check(VkResult result, const char *operation);
int numericClass(VkFormat format);
struct Device;
struct Extensions;
struct Heap;
struct SparseState;
struct ExternalSemaphore;
struct ExternalImage;
struct Texture;
struct Sampler;
struct TextureBuffer;
struct TensorResource;
struct TensorView;
struct GraphPipeline;
struct SharedEvent;
struct CounterPool;
struct AccelerationStructure;
struct RayTracingPipeline;
struct Command;
struct Surface;
struct Drawable;
struct GeneratedLayout;
struct GeneratedExecution;

enum Feature : uint64_t {
    Anisotropy = 1,
    Int16 = 2,
    Storage16 = 4,
    Float16 = 8,
    Astc = 16,
    Etc2 = 32,
    ExtendedStorageFormats = 64,
    Tessellation = 1ull << 7,
    NonSolid = 1ull << 8,
    DepthClamp = 1ull << 9,
    DepthBiasClamp = 1ull << 10,
    DepthBounds = 1ull << 11,
    WideLines = 1ull << 12,
    LargePoints = 1ull << 13,
    SampleShading = 1ull << 14,
    AlphaToOne = 1ull << 15,
    IndependentBlend = 1ull << 16,
    DualSourceBlend = 1ull << 17,
    LogicOp = 1ull << 18,
    MultiViewport = 1ull << 19,
    Int64 = 1ull << 20,
    Bc = 1ull << 21,
    CubeArray = 1ull << 22,
    ClipDistance = 1ull << 23,
    CullDistance = 1ull << 24,
    VertexStores = 1ull << 25,
    FragmentStores = 1ull << 26,
    IndirectFirstInstance = 1ull << 27,
    MultiDraw = 1ull << 28,
    StorageMs = 1ull << 29,
    BufferAddress = 1ull << 30,
    Timeline = 1ull << 31,
    RayQuery = 1ull << 32,
    RayPipeline = 1ull << 33,
    MeshShader = 1ull << 34,
    TaskShader = 1ull << 35,
    DescriptorIndexing = 1ull << 36,
    Multiview = 1ull << 37,
    PixelInterlock = 1ull << 38,
    Int8 = 1ull << 39,
    Storage8 = 1ull << 40,
    Atomics64 = 1ull << 41,
    FloatAtomics = 1ull << 42,
    SamplerMinMax = 1ull << 43,
    ViewportLayer = 1ull << 44,
    DynamicIndexing = 1ull << 45,
    ImageGather = 1ull << 46,
    StorageRead = 1ull << 47,
    StorageWrite = 1ull << 48,
    SubgroupExtended = 1ull << 49,
    Float64 = 1ull << 50,
    PreciseOcclusion = 1ull << 51,
    Uniform16 = 1ull << 52,
    MemoryModel = 1ull << 53,
    DepthResolve = 1ull << 54,
    FragmentRate = 1ull << 55,
    PrimitiveRate = 1ull << 56,
    AttachmentRate = 1ull << 57,
    CooperativeMatrix = 1ull << 58,
    SparseResources = 1ull << 59
};
enum ExtraFeature : uint64_t {
    HardwareBufferInterop = 1,
    ExternalSyncFd = 2,
    SamplerYcbcr = 4,
    DrawIndirectCount = 8,
    AstcHdr = 16,
    Pvrtc = 32,
    DeviceGeneratedCommands = 64,
    TileShading = 128,
    IndependentQueues = 256,
    Synchronization2 = 512,
    TensorResources = 1024,
    DataGraph = 2048,
    RayMotionBlur = 4096,
    VertexDivisor = 8192,
    VertexZeroDivisor = 16384,
    AttachmentStoreNone = 32768
};
enum class Storage { Shared, Private, Memoryless };

struct Object {
    virtual ~Object() = default;
    virtual Device *owner() const = 0;
};
struct QueueInfo {
    VkQueue handle = VK_NULL_HANDLE;
    uint32_t family = 0, index = 0;
    VkQueueFamilyProperties properties{};
};
struct DescriptorPoolAllocation {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    uint32_t maxSets = 0;
    std::vector<VkDescriptorPoolSize> sizes;
};
struct FramebufferAllocation {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    uint32_t width = 0, height = 0, layers = 0;
    bool matches(const VkFramebufferCreateInfo &) const;
};
struct Device : Object, std::enable_shared_from_this<Device> {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0, timestampBits = 0, sparseFamily = 0;
    VkQueue sparseQueue = VK_NULL_HANDLE;
    std::vector<QueueInfo> queues;
    struct GraphQueueOrder {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        uint64_t value = 0;
    };
    std::map<uint32_t, GraphQueueOrder> graphOrder;
    std::vector<uint32_t> resourceFamilies;
    template <class T> void share(T &info) const {
        if (resourceFamilies.size() > 1) {
            info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = uint32_t(resourceFamilies.size());
            info.pQueueFamilyIndices = resourceFamilies.data();
        }
    }
    uint64_t available = 0, enabled = 0, availableExtra = 0, enabledExtra = 0;
    std::map<std::pair<uint64_t, bool>, std::weak_ptr<Texture>> importedImages;
    std::map<std::vector<uint64_t>, std::weak_ptr<Sampler>> conversionSamplers;
    VkDeviceSize sparseVirtualBytes = 0;
    VkPhysicalDeviceFeatures coreFeatures{};
    std::shared_ptr<Extensions> extensions;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceSubgroupProperties subgroup{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, nullptr, 0, 0, 0, VK_FALSE};
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::map<std::vector<int>, VkRenderPass> renderPassCache;
    VmaAllocator allocator = VK_NULL_HANDLE;
    bool memoryBudget = false;
    struct CommandAllocation {
        VkCommandPool pool;
        VkCommandBuffer command;
        uint32_t family;
    };
    std::array<CommandAllocation, 8> idleCommands{};
    size_t idleCommandCount = 0;
    std::array<DescriptorPoolAllocation, 32> idleDescriptorPools{};
    std::array<FramebufferAllocation, 32> idleFramebuffers{};
    size_t idleDescriptorPoolCount = 0, idleFramebufferCount = 0;
    uint64_t descriptorPoolsCreated = 0, descriptorPoolsReused = 0, framebuffersCreated = 0, framebuffersReused = 0;
    DescriptorPoolAllocation takeDescriptorPool(uint32_t maxSets, const std::vector<VkDescriptorPoolSize> &);
    FramebufferAllocation takeFramebuffer(const VkFramebufferCreateInfo &);
    void recycle(DescriptorPoolAllocation, bool completed) noexcept;
    void recycle(FramebufferAllocation, bool completed) noexcept;
    void invalidateFramebuffers(VkImageView) noexcept;
    void destroyView(VkImageView) noexcept;
    void clearIdleResources() noexcept;
    std::vector<std::weak_ptr<Command>> pending;
    struct RetiredDrawable {
        VkSemaphore acquired, rendered;
        VkFence fence;
        bool didAcquire;
    };
    struct RetiredSurface {
        VkSurfaceKHR surface;
        VkSwapchainKHR swapchain;
        std::vector<VkImageView> views;
#ifdef __ANDROID__
        ANativeWindow *window;
#endif
    };
    std::vector<RetiredDrawable> retiredDrawables;
    std::vector<RetiredSurface> retiredSurfaces;
    size_t liveDrawables = 0, liveSurfaces = 0;
    void reclaimPresentation(bool shutdown = false);
    static std::shared_ptr<Device> create(uint64_t required, bool validation, bool allowSoftware, uint64_t extra = 0);
    Device *owner() const override { return const_cast<Device *>(this); }
    void collect();
    void waitIdle();
    ~Device() override;
};
struct Resource : Object {
    std::shared_ptr<Device> d;
    explicit Resource(std::shared_ptr<Device> device) : d(std::move(device)) {}
    Device *owner() const override { return d.get(); }
};
struct Buffer : Resource {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::shared_ptr<Heap> heap;
    std::shared_ptr<SparseState> sparse;
    VkDeviceSize size;
    VkDeviceSize heapOffset = 0, heapSpan = 0;
    VkBufferUsageFlags usage;
    Storage storage;
    bool cpuWriteOnly;
    uint32_t inFlight = 0;
    Buffer(std::shared_ptr<Device>, VkDeviceSize, VkBufferUsageFlags, Storage, bool cpuWriteOnly = false,
           std::shared_ptr<Heap> heap = {}, VkDeviceSize heapOffset = 0, bool unbound = false, bool sparse = false);
    void write(VkDeviceSize offset, const void *bytes, size_t count);
    void read(VkDeviceSize offset, void *bytes, size_t count);
    ~Buffer() override;
};
struct FrameState {
    bool active = true;
};
struct ImageState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags initialized = 0;
};
struct TextureOptions {
    uint32_t depth = 1, mipLevels = 1, layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
};
VkImageType textureImageType(VkImageViewType);
VkImageCreateFlags textureImageFlags(VkImageViewType);
struct Texture : Resource {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::shared_ptr<Heap> heap;
    std::shared_ptr<SparseState> sparse;
    std::shared_ptr<ExternalImage> external;
    VkFormat format;
    uint32_t width, height;
    TextureOptions options;
    VkDeviceSize heapOffset = 0, heapSpan = 0;
    uint32_t baseMip = 0, baseLayer = 0;
    std::shared_ptr<Texture> parent;
    VkComponentMapping components{};
    VkImageAspectFlags viewAspect = 0;
    std::vector<ImageState> states;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, VkImageView> attachmentViews;
    VkImageUsageFlags usage;
    Storage storage;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool initialized = false, lazy = false, borrowed = false;
    std::shared_ptr<Surface> surface;
    std::shared_ptr<FrameState> frame;
    Texture(std::shared_ptr<Device>, uint32_t, uint32_t, VkFormat, VkImageUsageFlags, Storage, TextureOptions = {},
            std::shared_ptr<Heap> heap = {}, VkDeviceSize heapOffset = 0, bool unbound = false, bool sparse = false);
    Texture(std::shared_ptr<Device>, uint32_t, uint32_t, VkFormat, VkImage, VkImageView);
    Texture(std::shared_ptr<Device>, std::shared_ptr<ExternalImage>, uint32_t, uint32_t, VkFormat, VkImageUsageFlags,
            TextureOptions);
    VkFormatFeatureFlags formatFeatures() const;
    bool depth() const;
    bool stencil() const;
    VkImageAspectFlags aspects() const;
    VkImageAspectFlags viewAspects() const;
    VkImageAspectFlags transferAspects(VkImageAspectFlags requested = 0) const;
    uint32_t transferPixelSize(VkImageAspectFlags) const;
    int viewNumericClass() const;
    VkExtent3D extent(uint32_t mip = 0) const;
    VkImageType imageType() const;
    VkImageCreateFlags flags() const;
    Texture &root() { return parent ? parent->root() : *this; }
    Texture(std::shared_ptr<Texture>, VkFormat, VkImageViewType, uint32_t mip, uint32_t mipCount, uint32_t layer,
            uint32_t layerCount, VkImageUsageFlags viewUsage = 0, VkComponentMapping swizzle = {},
            VkImageAspectFlags aspect = 0);
    VkImageView attachmentView(uint32_t mip, uint32_t layer, uint32_t layers = 1);
    uint32_t blockWidth() const;
    uint32_t blockHeight() const;
    uint64_t byteSize(uint32_t mip = 0) const;
    uint32_t pixelSize() const;
    void usable() const;
    ~Texture() override;
};
struct Sampler : Resource {
    VkSampler sampler = VK_NULL_HANDLE;
    bool linear;
    bool compare = false;
    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    uint32_t descriptorCost = 1;
    explicit Sampler(std::shared_ptr<Device> device) : Resource(std::move(device)), linear(false) {}
    VkSamplerReductionMode reductionMode = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
    Sampler(std::shared_ptr<Device>, bool linear, bool repeat, float anisotropy,
            VkSamplerMipmapMode mipFilter = VK_SAMPLER_MIPMAP_MODE_NEAREST, float minLod = 0,
            float maxLod = VK_LOD_CLAMP_NONE, float bias = 0, VkCompareOp comparison = VK_COMPARE_OP_ALWAYS,
            bool compare = false, VkSamplerAddressMode address = VK_SAMPLER_ADDRESS_MODE_MAX_ENUM,
            VkSamplerReductionMode reduction = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE,
            VkBorderColor border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    ~Sampler() override;
};

// Reflection identifies resources by descriptor set, binding and array element.
struct BindingLayout {
    uint32_t binding;
    VkDescriptorType type;
    VkFormat storageFormat = VK_FORMAT_UNDEFINED;
    uint32_t minimumBytes = 0;
    uint32_t count = 1;
    VkShaderStageFlags stages = 0;
    uint32_t imageDim = 1;
    bool arrayed = false, multisampled = false, shadow = false, runtime = false;
    uint32_t inputAttachmentIndex = 0;
    int numericType = -1;
    bool tile = false, readonly = false;
    std::shared_ptr<Sampler> immutableSampler;
    uint32_t tensorRank = 0;
    std::vector<int64_t> tensorDimensions;
    uint32_t set = 0;
    uint64_t location() const { return (uint64_t(set) << 32) | binding; }
    uint64_t descriptorCost() const {
        return uint64_t(count) * (immutableSampler ? immutableSampler->descriptorCost : 1);
    }
};
struct FunctionConstant {
    uint64_t bits = 0;
    uint32_t bytes = 4;
    FunctionConstant(uint64_t value = 0, uint32_t width = 4) : bits(value), bytes(width) {}
    uint32_t uint32() const {
        require(bytes <= 4 && bits <= UINT32_MAX, "Specialized dimension must fit a 32-bit scalar");
        return static_cast<uint32_t>(bits);
    }
};
struct Shader {
    std::vector<uint32_t> code;
    std::string entry = "main";
    std::map<uint32_t, FunctionConstant> constants;
};
std::array<uint32_t, 3> reflectWorkgroupSize(const Shader &, uint32_t entryId);
struct SpecializationData {
    std::vector<VkSpecializationMapEntry> entries;
    std::vector<uint8_t> data;
    SpecializationData() = default;
    explicit SpecializationData(const Shader &);
    VkSpecializationInfo info() const { return {uint32_t(entries.size()), entries.data(), data.size(), data.data()}; }
};
void validateCooperativeShader(Device &, const Shader &, const std::array<uint32_t, 3> &);
struct SubpassDescription {
    std::vector<uint32_t> colors, inputs;
    bool depth = false, resolveDepth = false;
};
struct SubpassLayout {
    std::vector<VkFormat> colors;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<SubpassDescription> subpasses;
    std::vector<uint32_t> resolveColors;
    VkResolveModeFlagBits depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
                          stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    bool hasDepthResolve() const;
    uint32_t depthResolveIndex() const;
    std::vector<int> key;
};
std::shared_ptr<SubpassLayout> parseSubpassLayout(const std::vector<int> &);
void validateSubpassLayout(Device &, const SubpassLayout &);
struct GraphicsOptions {
    bool mesh = false, indirectBindable = false;
    bool tileShading = false;
    VkExtent2D tileApron{};
    std::shared_ptr<SubpassLayout> passLayout;
    uint32_t subpass = 0;
    std::shared_ptr<Shader> task;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPolygonMode polygon = VK_POLYGON_MODE_FILL;
    bool depthWrite = true, depthTest = true, depthClamp = false, alphaToCoverage = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;
    std::vector<VkVertexInputBindingDescription> vertexBindings;
    std::vector<VkVertexInputBindingDivisorDescriptionKHR> vertexDivisors;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkFormat> colors;
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    bool stencilTest = false;
    VkStencilOpState front{}, back{};
    std::shared_ptr<Shader> tessControl, tessEvaluation;
    uint32_t patchPoints = 3;
    uint32_t viewportCount = 1, viewMask = 0;
    bool sampleShading = false, alphaToOne = false, primitiveRestart = false, rasterizationDisabled = false;
    float minSampleShading = 1;
    std::array<VkSampleMask, 2> sampleMask{~0u, ~0u};
    bool logicEnabled = false;
    VkLogicOp logic = VK_LOGIC_OP_COPY;
    bool depthBounds = false;
    float minDepthBounds = 0, maxDepthBounds = 1;
    VkExtent2D fragmentSize{1, 1}, rateMapTexelSize{};
    VkFragmentShadingRateCombinerOpKHR attachmentRateCombiner = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    VkFragmentShadingRateCombinerOpKHR primitiveRateCombiner = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
};
struct Pipeline : Resource {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayout> setLayouts;
    VkRenderPass compatiblePass = VK_NULL_HANDLE;
    std::vector<BindingLayout> bindings;
    uint32_t pushBytes;
    bool compute;
    bool rayTracing = false, indirectBindable = false;
    std::vector<uint64_t> generatedStateKey;
    std::vector<std::vector<uint64_t>> fragmentInterface;
    bool tileShader = false;
    std::array<uint32_t, 3> tileRate{};
    GraphicsOptions graphics;
    VkShaderStageFlags stages = 0;
    std::array<uint32_t, 3> localSize{1, 1, 1};
    VkFormat colorFormat = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED;
    Pipeline(std::shared_ptr<Device>, std::vector<BindingLayout>, uint32_t, const Shader &,
             bool indirectBindable = false);
    Pipeline(std::shared_ptr<Device>, std::vector<BindingLayout>, uint32_t, const Shader &vertex,
             const Shader &fragment, VkFormat color, VkFormat depth, bool blend, GraphicsOptions = {});
    Pipeline(std::shared_ptr<Device> device, std::vector<BindingLayout> b, uint32_t p)
        : Resource(std::move(device)), bindings(std::move(b)), pushBytes(p), compute(false) {}
    void makeLayout();
    ~Pipeline() override;
};
struct Binding {
    uint32_t index;
    std::shared_ptr<Buffer> buffer;
    VkDeviceSize offset = 0, length = 0;
    std::shared_ptr<Texture> texture;
    std::shared_ptr<Sampler> sampler;
    uint32_t element = 0;
    std::shared_ptr<AccelerationStructure> acceleration;
    std::shared_ptr<TextureBuffer> texel;
    std::shared_ptr<TensorView> tensor;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t set = 0;
    uint64_t location() const { return (uint64_t(set) << 32) | index; }
};
struct Dispatch {
    std::shared_ptr<Pipeline> pipeline;
    std::vector<Binding> bindings;
    std::vector<uint8_t> constants;
    std::array<uint32_t, 3> groups;
    std::shared_ptr<Buffer> indirect;
    VkDeviceSize indirectOffset = 0;
};
struct VertexBinding {
    uint32_t index;
    std::shared_ptr<Buffer> buffer;
    VkDeviceSize offset;
};
struct Draw {
    std::shared_ptr<Pipeline> pipeline;
    std::vector<Binding> bindings;
    std::vector<uint8_t> constants;
    uint32_t vertices = 3, instances = 1, firstVertex = 0, firstInstance = 0;
    std::shared_ptr<Buffer> indexBuffer, indirect, countBuffer;
    VkDeviceSize indexOffset = 0, indirectOffset = 0, countOffset = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    int32_t baseVertex = 0;
    uint32_t drawCount = 1, stride = 0;
    std::vector<VertexBinding> vertexBuffers;
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    std::array<float, 4> blendColor{};
    float depthBias = 0, slopeBias = 0, biasClamp = 0, lineWidth = 1;
    std::array<uint32_t, 3> meshGroups{};
    uint32_t stencilReference = 0;
    std::shared_ptr<CounterPool> visibility;
    uint32_t visibilityIndex = 0;
    uint32_t subpass = 0;
    std::shared_ptr<GeneratedExecution> generated;
    bool perTile = false;
    uint32_t tileAction = 0; // 0 draw, 1 dispatch, 2 area dispatch, 3 begin, 4 end, 5 barrier
};
struct Attachment {
    std::shared_ptr<Texture> texture, resolve;
    uint32_t mip = 0, layer = 0, resolveMip = 0, resolveLayer = 0;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE;
    std::array<float, 4> clear{0, 0, 0, 1};
};
struct Render {
    std::shared_ptr<SubpassLayout> passLayout;
    std::shared_ptr<Texture> color, depth;
    VkAttachmentLoadOp colorLoad = VK_ATTACHMENT_LOAD_OP_CLEAR, depthLoad = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp colorStore = VK_ATTACHMENT_STORE_OP_STORE, depthStore = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    std::array<float, 4> clearColor{0, 0, 0, 1};
    float clearDepth = 1;
    std::vector<Draw> draws;
    std::vector<Attachment> colors;
    VkAttachmentLoadOp stencilLoad = VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
    VkAttachmentStoreOp stencilStore = VK_ATTACHMENT_STORE_OP_MAX_ENUM;
    bool depthReadOnly = false, stencilReadOnly = false;
    VkAttachmentLoadOp stencilLoadOp() const { return stencilLoad == VK_ATTACHMENT_LOAD_OP_MAX_ENUM ? depthLoad : stencilLoad; }
    VkAttachmentStoreOp stencilStoreOp() const { return stencilStore == VK_ATTACHMENT_STORE_OP_MAX_ENUM ? depthStore : stencilStore; }
    VkImageLayout depthLayout() const {
        if (tileShading) return VK_IMAGE_LAYOUT_GENERAL;
        if (!depth || (!depthReadOnly && !stencilReadOnly)) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (!depth->stencil()) return depthReadOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (!depth->depth()) return stencilReadOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const bool dr = depthReadOnly, sr = stencilReadOnly;
        if (dr && sr) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        if (dr) return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
        if (sr) return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    uint32_t depthMip = 0, depthLayer = 0, clearStencil = 0;
    uint32_t viewMask = 0, layers = 1;
    std::shared_ptr<Texture> depthResolve;
    std::shared_ptr<Texture> rateMap;
    VkExtent2D rateMapTexelSize{};
    uint32_t rateMip = 0, rateLayer = 0;
    uint32_t depthResolveMip = 0, depthResolveLayer = 0;
    VkResolveModeFlagBits depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
                          stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    bool tileShading = false;
    VkExtent2D tileApron{};
    std::vector<size_t> memoryBarriers;
};
VkRenderPass makeSubpassPass(Device &, const SubpassLayout &, const std::vector<Attachment> &, VkAttachmentLoadOp,
                             VkAttachmentStoreOp, uint32_t viewMask, bool tileShading = false,
                             VkExtent2D tileApron = {}, VkExtent2D rateMapTexelSize = {}, const Render *render = nullptr);
Attachment subpassAttachment(const Render &, uint32_t index);
struct ImageRegion {
    uint32_t mip = 0, layer = 0, layers = 1;
    VkOffset3D origin{};
    VkExtent3D size{};
    VkImageAspectFlags aspect = 0;
};
struct Command : Resource, std::enable_shared_from_this<Command> {
    enum class State { Recording, Submitted, Completed, Failed };
    State state = State::Recording;
    uint32_t queueIndex = 0;
    const QueueInfo &queueInfo() const { return d->queues[queueIndex]; }
    void requireQueue(VkQueueFlags any) const;
    void validateTransfer(const Texture &, const ImageRegion &) const;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> descriptorPools;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<DescriptorPoolAllocation> descriptorAllocations;
    std::vector<FramebufferAllocation> framebufferAllocations;
    VkFramebuffer framebuffer(const VkFramebufferCreateInfo &);
    // Command-scoped immutable descriptors retain resources through operations.
    struct DescriptorArena {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        uint32_t used = 0;
    };
    std::map<std::pair<const Pipeline *, uint32_t>, DescriptorArena> descriptorArenas;
    std::map<std::vector<uint64_t>, VkDescriptorSet> descriptorSets;
    uint32_t descriptorCacheHits = 0, imageBarrierCount = 0;
    std::array<VkPipeline, 3> boundPipelines{};
    std::vector<std::function<void(Command &)>> operations;
    std::vector<std::shared_ptr<Buffer>> buffers;
    std::vector<std::shared_ptr<TensorResource>> tensors;
    void copyTensor(std::shared_ptr<TensorResource>, std::shared_ptr<TensorResource>);
    std::vector<VkCommandBuffer> graphSegments;
    bool graphOnly() const {
        return !(queueInfo().properties.queueFlags &
                 (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT));
    }
    void submitGraphSegments(const VkSubmitInfo &);
    void dispatchGraph(std::shared_ptr<GraphPipeline>, std::vector<Binding>);
    std::unordered_map<Texture *, std::vector<ImageState>> images;
    std::shared_ptr<Drawable> presentation;
    explicit Command(std::shared_ptr<Device>, uint32_t queueIndex = 0);
    std::unordered_map<AccelerationStructure *, bool> accelerationStates;
    std::unordered_map<AccelerationStructure *, std::shared_ptr<AccelerationStructure>> accelerationInputStates;
    std::unordered_map<AccelerationStructure *, std::shared_ptr<AccelerationStructure>> recordedAccelerationInputs;
    void build(std::shared_ptr<AccelerationStructure>, bool update = false,
               std::shared_ptr<AccelerationStructure> replacementInputs = {});
    void copyAccelerationStructure(std::shared_ptr<AccelerationStructure>, std::shared_ptr<AccelerationStructure>);
    void trace(std::shared_ptr<RayTracingPipeline>, std::vector<Binding>, std::vector<uint8_t>,
               std::array<uint32_t, 3>);
    std::vector<std::pair<std::shared_ptr<SharedEvent>, uint64_t>> eventWaits, eventSignals;
    std::vector<std::shared_ptr<ExternalSemaphore>> externalWaits, externalSignals;
    std::unordered_map<ExternalImage *, bool> externalOwnership;
    void acquireExternal(std::shared_ptr<Texture>, bool preserveContents);
    void releaseExternal(std::shared_ptr<Texture>);
    void requireOwnership(Texture &);
    void waitExternal(std::shared_ptr<ExternalSemaphore>);
    void signalExternal(std::shared_ptr<ExternalSemaphore>);
    std::vector<std::shared_ptr<CounterPool>> counters;
    std::vector<uint32_t> counterIndices;
    void sample(std::shared_ptr<CounterPool>, uint32_t index);
    void waitEvent(std::shared_ptr<SharedEvent>, uint64_t value);
    void signalEvent(std::shared_ptr<SharedEvent>, uint64_t value);
    void recording() const;
    void dispatch(Dispatch);
    void render(Render);
    void executeGenerated(std::shared_ptr<GeneratedExecution>, std::vector<Binding>, std::vector<uint8_t>);
    void copy(std::shared_ptr<Buffer> src, std::shared_ptr<Buffer> dst, VkDeviceSize srcOffset, VkDeviceSize dstOffset,
              VkDeviceSize size);
    void copy(std::shared_ptr<Buffer>, std::shared_ptr<Texture>, VkDeviceSize offset, bool toTexture);
    void copy(std::shared_ptr<Buffer>, std::shared_ptr<Texture>, VkDeviceSize offset, bool toTexture, ImageRegion,
              uint32_t rowLength = 0, uint32_t imageHeight = 0);
    void copy(std::shared_ptr<Texture>, std::shared_ptr<Texture>, ImageRegion, ImageRegion);
    void generateMipmaps(std::shared_ptr<Texture>, VkFilter = VK_FILTER_LINEAR);
    void fill(std::shared_ptr<Buffer>, VkDeviceSize offset, VkDeviceSize size, uint32_t value);
    void present(std::shared_ptr<Drawable>);
    void commit();
    bool wait(uint64_t timeout = UINT64_MAX);
    void barrier();
    void alias(std::shared_ptr<Resource>, std::shared_ptr<Resource>);
    void transition(Texture &, VkImageLayout, bool read, VkImageAspectFlags readAspects = 0);
    void transition(Texture &, VkImageLayout, bool read, uint32_t mip, uint32_t layer, uint32_t levels, uint32_t layers,
                    VkImageAspectFlags readAspects = 0);
    void markInitialized(Texture &, bool);
    void markInitialized(Texture &, bool, uint32_t mip, uint32_t layer, uint32_t levels, uint32_t layers,
                         VkImageAspectFlags aspects = 0);
    void validateBindings(const Pipeline &, const std::vector<Binding> &, const std::vector<uint8_t> &);
    void bind(const Pipeline &, const std::vector<Binding> &, const std::vector<uint8_t> &);
    void prepare(const Pipeline &, const std::vector<Binding> &, bool compute);
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
    ANativeWindow *window = nullptr;
    Surface(std::shared_ptr<Device>, ANativeWindow *, uint32_t, uint32_t);
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
