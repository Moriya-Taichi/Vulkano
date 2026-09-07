package dev.vulkano

/** Optional features are queried independently and enabled only when requested. */
enum class Feature(internal val bit: Int) {
    SAMPLER_ANISOTROPY(1), SHADER_INT16(2), STORAGE_BUFFER_16_BIT_ACCESS(4),
    SHADER_FLOAT16(8), TEXTURE_COMPRESSION_ASTC_LDR(16), TEXTURE_COMPRESSION_ETC2(32),
    STORAGE_IMAGE_EXTENDED_FORMATS(64)
}

enum class StorageMode { SHARED, PRIVATE, MEMORYLESS }
enum class BufferUsage(internal val bit: Int) { TRANSFER_SOURCE(1), TRANSFER_DESTINATION(2), UNIFORM(16), STORAGE(32) }
enum class TextureUsage(internal val bit: Int) {
    TRANSFER_SOURCE(1), TRANSFER_DESTINATION(2), SAMPLED(4), STORAGE(8), COLOR_ATTACHMENT(16), DEPTH_ATTACHMENT(32)
}
enum class PixelFormat(internal val vk: Int, val bytesPerPixel: Int) {
    RGBA8_UNORM(37, 4), BGRA8_UNORM(44, 4), RGBA16_FLOAT(97, 8), R32_FLOAT(100, 4), RGBA32_FLOAT(109, 16), DEPTH32_FLOAT(126, 4)
}
enum class BindingType(internal val vk: Int) { SAMPLED_TEXTURE(1), STORAGE_TEXTURE(3), UNIFORM_BUFFER(6), STORAGE_BUFFER(7) }
enum class LoadAction(internal val vk: Int) { LOAD(0), CLEAR(1), DONT_CARE(2) }
enum class StoreAction(internal val vk: Int) { STORE(0), DONT_CARE(1) }
enum class CommandBufferStatus { RECORDING, SUBMITTED, COMPLETED, FAILED }

data class Size(val width: Int, val height: Int = 1, val depth: Int = 1) {
    init { require(width > 0 && height > 0 && depth > 0) { "Size must be positive" } }
    internal fun array() = intArrayOf(width, height, depth)
}
data class ClearColor(val red: Float = 0f, val green: Float = 0f, val blue: Float = 0f, val alpha: Float = 1f)
data class BindingLayout(val index: Int, val type: BindingType) {
    init { require(index >= 0) }
}
data class TextureDescriptor(
    val width: Int,
    val height: Int,
    val pixelFormat: PixelFormat = PixelFormat.RGBA8_UNORM,
    val usage: Set<TextureUsage> = setOf(TextureUsage.SAMPLED, TextureUsage.TRANSFER_DESTINATION),
    val storageMode: StorageMode = StorageMode.PRIVATE,
) {
    init {
        require(width > 0 && height > 0 && usage.isNotEmpty())
        require(storageMode != StorageMode.SHARED) { "Use a shared buffer and blit for CPU texture access" }
        if (pixelFormat == PixelFormat.DEPTH32_FLOAT) {
            require(usage == setOf(TextureUsage.DEPTH_ATTACHMENT)) { "Depth textures are attachment-only" }
        } else {
            require(TextureUsage.DEPTH_ATTACHMENT !in usage) { "Depth attachment usage requires a depth format" }
        }
        if (storageMode == StorageMode.MEMORYLESS) require(usage == setOf(
            if (pixelFormat == PixelFormat.DEPTH32_FLOAT) TextureUsage.DEPTH_ATTACHMENT else TextureUsage.COLOR_ATTACHMENT
        )) { "Memoryless textures are attachment-only" }
    }
    internal val usageBits get() = usage.fold(0) { a, b -> a or b.bit } or if (storageMode == StorageMode.MEMORYLESS) 64 else 0
}
data class SamplerDescriptor(val linearFiltering: Boolean = true, val repeat: Boolean = false, val maxAnisotropy: Float = 1f)
data class ColorAttachment(
    val texture: Texture,
    val loadAction: LoadAction = LoadAction.CLEAR,
    val storeAction: StoreAction = StoreAction.STORE,
    val clearColor: ClearColor = ClearColor(),
)
data class DepthAttachment(
    val texture: Texture,
    val loadAction: LoadAction = LoadAction.CLEAR,
    val storeAction: StoreAction = StoreAction.DONT_CARE,
    val clearDepth: Float = 1f,
)
data class RenderPassDescriptor(val colorAttachment: ColorAttachment, val depthAttachment: DepthAttachment? = null)

data class DeviceLimits(
    val maxStorageBufferRange: Long,
    val maxUniformBufferRange: Long,
    val minStorageBufferOffsetAlignment: Long,
    val minUniformBufferOffsetAlignment: Long,
    val maxPushConstantBytes: Int,
    val maxTextureDimension2D: Int,
    val maxThreadsPerThreadgroup: Int,
    val maxThreadgroupSize: Size,
    val maxThreadgroupCount: Size,
    val maxSamplerAnisotropy: Float,
)

data class DeviceCapabilities(
    val name: String,
    val apiVersion: String,
    val isSoftwareRenderer: Boolean,
    val availableFeatures: Set<Feature>,
    val enabledFeatures: Set<Feature>,
    val limits: DeviceLimits,
    /** Subgroup width is reported, never assumed to be 32/64 or equal to a workgroup. */
    val subgroupSize: Int,
    val subgroupSupportedStages: Int,
    val subgroupSupportedOperations: Int,
    val hasMemoryBudget: Boolean,
)

data class MemoryHeap(
    val sizeBytes: Long,
    val deviceLocal: Boolean,
    val budgetBytes: Long,
    val usageBytes: Long,
    /** Without VK_EXT_memory_budget, these are allocator estimates, not free system RAM. */
    val estimated: Boolean,
)
