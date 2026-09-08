package dev.vulkano

/** Optional features are queried independently and enabled only when requested. */
enum class Feature(internal val bit: Long, internal val group: Int = 0) {
    SAMPLER_ANISOTROPY(1),
    SHADER_INT16(2),
    STORAGE_BUFFER_16_BIT_ACCESS(4),
    SHADER_FLOAT16(8),
    TEXTURE_COMPRESSION_ASTC_LDR(16),
    TEXTURE_COMPRESSION_ETC2(32),
    STORAGE_IMAGE_EXTENDED_FORMATS(64),
    TESSELLATION(1L shl 7),
    WIREFRAME(1L shl 8),
    DEPTH_CLAMP(1L shl 9),
    DEPTH_BIAS_CLAMP(1L shl 10),
    DEPTH_BOUNDS(1L shl 11),
    WIDE_LINES(1L shl 12),
    LARGE_POINTS(1L shl 13),
    SAMPLE_RATE_SHADING(1L shl 14),
    ALPHA_TO_ONE(1L shl 15),
    INDEPENDENT_BLEND(1L shl 16),
    DUAL_SOURCE_BLEND(1L shl 17),
    LOGIC_OP(1L shl 18),
    MULTI_VIEWPORT(1L shl 19),
    SHADER_INT64(1L shl 20),
    TEXTURE_COMPRESSION_BC(1L shl 21),
    TEXTURE_CUBE_ARRAY(1L shl 22),
    SHADER_CLIP_DISTANCE(1L shl 23),
    SHADER_CULL_DISTANCE(1L shl 24),
    VERTEX_STORES_AND_ATOMICS(1L shl 25),
    FRAGMENT_STORES_AND_ATOMICS(1L shl 26),
    INDIRECT_FIRST_INSTANCE(1L shl 27),
    MULTI_DRAW_INDIRECT(1L shl 28),
    STORAGE_IMAGE_MULTISAMPLE(1L shl 29),
    BUFFER_DEVICE_ADDRESS(1L shl 30),
    TIMELINE_SEMAPHORE(1L shl 31),
    RAY_QUERY(1L shl 32),
    RAY_TRACING_PIPELINE(1L shl 33),
    MESH_SHADER(1L shl 34),
    TASK_SHADER(1L shl 35),
    DESCRIPTOR_INDEXING(1L shl 36),
    MULTIVIEW(1L shl 37),
    FRAGMENT_PIXEL_INTERLOCK(1L shl 38),
    SHADER_INT8(1L shl 39),
    STORAGE_BUFFER_8_BIT_ACCESS(1L shl 40),
    SHADER_BUFFER_INT64_ATOMICS(1L shl 41),
    SHADER_BUFFER_FLOAT32_ATOMICS(1L shl 42),
    SAMPLER_MIN_MAX(1L shl 43),
    SHADER_VIEWPORT_LAYER(1L shl 44),
    DYNAMIC_RESOURCE_INDEXING(1L shl 45),
    IMAGE_GATHER_EXTENDED(1L shl 46),
    STORAGE_IMAGE_READ_WITHOUT_FORMAT(1L shl 47),
    STORAGE_IMAGE_WRITE_WITHOUT_FORMAT(1L shl 48),
    SUBGROUP_EXTENDED_TYPES(1L shl 49),
    SHADER_FLOAT64(1L shl 50),
    PRECISE_OCCLUSION(1L shl 51),
    UNIFORM_AND_STORAGE_BUFFER_16_BIT_ACCESS(1L shl 52),
    VULKAN_MEMORY_MODEL(1L shl 53),
    DEPTH_STENCIL_RESOLVE(1L shl 54),
    FRAGMENT_SHADING_RATE(1L shl 55),
    PRIMITIVE_SHADING_RATE(1L shl 56),
    ATTACHMENT_SHADING_RATE(1L shl 57),
    COOPERATIVE_MATRIX(1L shl 58),
    SPARSE_RESOURCES(1L shl 59),
    ANDROID_HARDWARE_BUFFER(1, 1),
    EXTERNAL_SYNC_FD(2, 1),
    SAMPLER_YCBCR_CONVERSION(4, 1),
    DRAW_INDIRECT_COUNT(8, 1),
    TEXTURE_COMPRESSION_ASTC_HDR(16, 1),
    TEXTURE_COMPRESSION_PVRTC(32, 1),
    DEVICE_GENERATED_COMMANDS(64, 1),
    TILE_SHADING(128, 1),
    INDEPENDENT_QUEUES(256, 1),
    SYNCHRONIZATION_2(512, 1),
    TENSOR_RESOURCES(1024, 1),
    MACHINE_LEARNING_GRAPH(2048, 1),
}

enum class StorageMode {
    SHARED,
    PRIVATE,
    MEMORYLESS,
}

enum class BufferUsage(internal val bit: Int) {
    TRANSFER_SOURCE(1),
    TRANSFER_DESTINATION(2),
    UNIFORM_TEXEL(4),
    STORAGE_TEXEL(8),
    UNIFORM(16),
    STORAGE(32),
    INDEX(64),
    VERTEX(128),
    INDIRECT(256),
    SHADER_DEVICE_ADDRESS(131072),
    ACCELERATION_STRUCTURE_INPUT(524288),
    ACCELERATION_STRUCTURE_STORAGE(1048576),
    SHADER_BINDING_TABLE(1024),
}

enum class TextureUsage(internal val bit: Int) {
    TRANSFER_SOURCE(1),
    TRANSFER_DESTINATION(2),
    SAMPLED(4),
    STORAGE(8),
    COLOR_ATTACHMENT(16),
    DEPTH_ATTACHMENT(32),
    INPUT_ATTACHMENT(128),
    SHADING_RATE_ATTACHMENT(256),
}

enum class PixelFormat(
    internal val vk: Int,
    val bytesPerPixel: Int,
    val blockWidth: Int = 1,
    val blockHeight: Int = 1,
    val bytesPerBlock: Int = bytesPerPixel,
) {
    /** Opaque Android hardware-buffer format; only produced by importHardwareBuffer. */
    EXTERNAL(0, 0),
    RGB565_UNORM(4, 2),
    RGB8_UNORM(23, 3),
    R8_UNORM(9, 1),
    R8_SNORM(10, 1),
    R8_UINT(13, 1),
    R8_SINT(14, 1),
    RG8_UNORM(16, 2),
    RG8_SNORM(17, 2),
    RG8_UINT(20, 2),
    RG8_SINT(21, 2),
    RGBA8_UNORM(37, 4),
    RGBA8_SNORM(38, 4),
    RGBA8_UINT(41, 4),
    RGBA8_SINT(42, 4),
    RGBA8_SRGB(43, 4),
    BGRA8_UNORM(44, 4),
    BGRA8_SRGB(50, 4),
    RGB10_A2_UNORM(64, 4),
    RGB10_A2_UINT(68, 4),
    R16_UNORM(70, 2),
    R16_SNORM(71, 2),
    R16_UINT(74, 2),
    R16_SINT(75, 2),
    R16_FLOAT(76, 2),
    RG16_UNORM(77, 4),
    RG16_SNORM(78, 4),
    RG16_UINT(81, 4),
    RG16_SINT(82, 4),
    RG16_FLOAT(83, 4),
    RGBA16_UNORM(91, 8),
    RGBA16_SNORM(92, 8),
    RGBA16_UINT(95, 8),
    RGBA16_SINT(96, 8),
    RGBA16_FLOAT(97, 8),
    R32_UINT(98, 4),
    R32_SINT(99, 4),
    R32_FLOAT(100, 4),
    RG32_UINT(101, 8),
    RG32_SINT(102, 8),
    RG32_FLOAT(103, 8),
    RGB32_UINT(104, 12),
    RGB32_SINT(105, 12),
    RGB32_FLOAT(106, 12),
    RGBA32_UINT(107, 16),
    RGBA32_SINT(108, 16),
    RGBA32_FLOAT(109, 16),
    RG11_B10_FLOAT(122, 4),
    RGB9_E5_FLOAT(123, 4),
    DEPTH16_UNORM(124, 2),
    DEPTH24_UNORM(125, 4),
    DEPTH32_FLOAT(126, 4),
    STENCIL8(127, 1),
    DEPTH24_STENCIL8(129, 4),
    DEPTH32_FLOAT_STENCIL8(130, 8),
    BC1_RGBA_UNORM(133, 0, 4, 4, 8),
    BC1_RGBA_SRGB(134, 0, 4, 4, 8),
    BC2_RGBA_UNORM(135, 0, 4, 4, 16),
    BC2_RGBA_SRGB(136, 0, 4, 4, 16),
    BC3_RGBA_UNORM(137, 0, 4, 4, 16),
    BC3_RGBA_SRGB(138, 0, 4, 4, 16),
    BC4_R_UNORM(139, 0, 4, 4, 8),
    BC4_R_SNORM(140, 0, 4, 4, 8),
    BC5_RG_UNORM(141, 0, 4, 4, 16),
    BC5_RG_SNORM(142, 0, 4, 4, 16),
    BC6H_RGB_UFLOAT(143, 0, 4, 4, 16),
    BC6H_RGB_SFLOAT(144, 0, 4, 4, 16),
    BC7_RGBA_UNORM(145, 0, 4, 4, 16),
    BC7_RGBA_SRGB(146, 0, 4, 4, 16),
    ETC2_RGB8_UNORM(147, 0, 4, 4, 8),
    ETC2_RGB8_SRGB(148, 0, 4, 4, 8),
    ETC2_RGB8_A1_UNORM(149, 0, 4, 4, 8),
    ETC2_RGB8_A1_SRGB(150, 0, 4, 4, 8),
    ETC2_RGBA8_UNORM(151, 0, 4, 4, 16),
    ETC2_RGBA8_SRGB(152, 0, 4, 4, 16),
    EAC_R11_UNORM(153, 0, 4, 4, 8),
    EAC_R11_SNORM(154, 0, 4, 4, 8),
    EAC_RG11_UNORM(155, 0, 4, 4, 16),
    EAC_RG11_SNORM(156, 0, 4, 4, 16),
    ASTC_4x4_UNORM(157, 0, 4, 4, 16),
    ASTC_4x4_SRGB(158, 0, 4, 4, 16),
    ASTC_5x4_UNORM(159, 0, 5, 4, 16),
    ASTC_5x4_SRGB(160, 0, 5, 4, 16),
    ASTC_5x5_UNORM(161, 0, 5, 5, 16),
    ASTC_5x5_SRGB(162, 0, 5, 5, 16),
    ASTC_6x5_UNORM(163, 0, 6, 5, 16),
    ASTC_6x5_SRGB(164, 0, 6, 5, 16),
    ASTC_6x6_UNORM(165, 0, 6, 6, 16),
    ASTC_6x6_SRGB(166, 0, 6, 6, 16),
    ASTC_8x5_UNORM(167, 0, 8, 5, 16),
    ASTC_8x5_SRGB(168, 0, 8, 5, 16),
    ASTC_8x6_UNORM(169, 0, 8, 6, 16),
    ASTC_8x6_SRGB(170, 0, 8, 6, 16),
    ASTC_8x8_UNORM(171, 0, 8, 8, 16),
    ASTC_8x8_SRGB(172, 0, 8, 8, 16),
    ASTC_10x5_UNORM(173, 0, 10, 5, 16),
    ASTC_10x5_SRGB(174, 0, 10, 5, 16),
    ASTC_10x6_UNORM(175, 0, 10, 6, 16),
    ASTC_10x6_SRGB(176, 0, 10, 6, 16),
    ASTC_10x8_UNORM(177, 0, 10, 8, 16),
    ASTC_10x8_SRGB(178, 0, 10, 8, 16),
    ASTC_10x10_UNORM(179, 0, 10, 10, 16),
    ASTC_10x10_SRGB(180, 0, 10, 10, 16),
    ASTC_12x10_UNORM(181, 0, 12, 10, 16),
    ASTC_12x10_SRGB(182, 0, 12, 10, 16),
    ASTC_12x12_UNORM(183, 0, 12, 12, 16),
    ASTC_12x12_SRGB(184, 0, 12, 12, 16),
    ASTC_4x4_FLOAT(1000066000, 0, 4, 4, 16),
    ASTC_5x4_FLOAT(1000066001, 0, 5, 4, 16),
    ASTC_5x5_FLOAT(1000066002, 0, 5, 5, 16),
    ASTC_6x5_FLOAT(1000066003, 0, 6, 5, 16),
    ASTC_6x6_FLOAT(1000066004, 0, 6, 6, 16),
    ASTC_8x5_FLOAT(1000066005, 0, 8, 5, 16),
    ASTC_8x6_FLOAT(1000066006, 0, 8, 6, 16),
    ASTC_8x8_FLOAT(1000066007, 0, 8, 8, 16),
    ASTC_10x5_FLOAT(1000066008, 0, 10, 5, 16),
    ASTC_10x6_FLOAT(1000066009, 0, 10, 6, 16),
    ASTC_10x8_FLOAT(1000066010, 0, 10, 8, 16),
    ASTC_10x10_FLOAT(1000066011, 0, 10, 10, 16),
    ASTC_12x10_FLOAT(1000066012, 0, 12, 10, 16),
    ASTC_12x12_FLOAT(1000066013, 0, 12, 12, 16),
    PVRTC1_2BPP_UNORM(1000054000, 0, 8, 4, 8),
    PVRTC1_4BPP_UNORM(1000054001, 0, 4, 4, 8),
    PVRTC2_2BPP_UNORM(1000054002, 0, 8, 4, 8),
    PVRTC2_4BPP_UNORM(1000054003, 0, 4, 4, 8),
    PVRTC1_2BPP_SRGB(1000054004, 0, 8, 4, 8),
    PVRTC1_4BPP_SRGB(1000054005, 0, 4, 4, 8),
    PVRTC2_2BPP_SRGB(1000054006, 0, 8, 4, 8),
    PVRTC2_4BPP_SRGB(1000054007, 0, 4, 4, 8);

    val isDepth
        get() = vk in 124..130 && vk != 127

    val isStencil
        get() = vk in 127..130

    val isCompressed
        get() = blockWidth > 1
}

enum class BindingType(internal val vk: Int) {
    INPUT_ATTACHMENT(10),
    SAMPLED_TEXTURE(1),
    STORAGE_TEXTURE(3),
    UNIFORM_BUFFER(6),
    STORAGE_BUFFER(7),
    ACCELERATION_STRUCTURE(1000150000),
    TENSOR(1000460000),
    SAMPLER(0),
    SAMPLED_IMAGE(2),
    UNIFORM_TEXEL_BUFFER(4),
    STORAGE_TEXEL_BUFFER(5),
}

enum class LoadAction(internal val vk: Int) {
    LOAD(0),
    CLEAR(1),
    DONT_CARE(2),
}

enum class StoreAction(internal val vk: Int) {
    STORE(0),
    DONT_CARE(1),
}

enum class CommandBufferStatus {
    RECORDING,
    SUBMITTED,
    COMPLETED,
    FAILED,
}

data class Size(val width: Int, val height: Int = 1, val depth: Int = 1) {
    init {
        require(width > 0 && height > 0 && depth > 0) { "Size must be positive" }
    }

    internal fun array() = intArrayOf(width, height, depth)
}

data class ClearColor(
    val red: Float = 0f,
    val green: Float = 0f,
    val blue: Float = 0f,
    val alpha: Float = 1f,
)

data class BindingLayout(
    val index: Int,
    val type: BindingType,
    val count: Int = 1,
    val immutableSampler: Sampler? = null,
) {
    init {
        require(index >= 0 && count > 0)
    }
}

data class TextureDescriptor(
    val width: Int,
    val height: Int,
    val pixelFormat: PixelFormat = PixelFormat.RGBA8_UNORM,
    val usage: Set<TextureUsage> = setOf(TextureUsage.SAMPLED, TextureUsage.TRANSFER_DESTINATION),
    val storageMode: StorageMode = StorageMode.PRIVATE,
    val depth: Int = 1,
    val mipLevels: Int = 1,
    val arrayLength: Int = 1,
    val sampleCount: Int = 1,
    val textureType: TextureType = TextureType.TYPE_2D,
) {
    init {
        require(
            width > 0 &&
                height > 0 &&
                depth > 0 &&
                arrayLength > 0 &&
                mipLevels in 1..32 &&
                usage.isNotEmpty()
        )
        require(sampleCount in setOf(1, 2, 4, 8, 16, 32, 64))
        require(mipLevels <= 32 - Integer.numberOfLeadingZeros(maxOf(width, height, depth)))
        require(storageMode != StorageMode.SHARED) {
            "Use a shared buffer and blit for CPU texture access"
        }
        if (pixelFormat.isDepth || pixelFormat.isStencil) {
            require(TextureUsage.COLOR_ATTACHMENT !in usage && TextureUsage.STORAGE !in usage) {
                "Invalid depth/stencil usage"
            }
        } else {
            require(TextureUsage.DEPTH_ATTACHMENT !in usage) {
                "Depth attachment usage requires a depth format"
            }
        }
        if (storageMode == StorageMode.MEMORYLESS)
            require(
                (usage - TextureUsage.INPUT_ATTACHMENT) ==
                    setOf(
                        if (pixelFormat.isDepth || pixelFormat.isStencil)
                            TextureUsage.DEPTH_ATTACHMENT
                        else TextureUsage.COLOR_ATTACHMENT
                    )
            ) {
                "Memoryless textures are attachment-only"
            }
    }

    internal val usageBits
        get() =
            usage.fold(0) { a, b -> a or b.bit } or
                if (storageMode == StorageMode.MEMORYLESS) 64 else 0
}

enum class TextureType(internal val vk: Int) {
    TYPE_1D(0),
    TYPE_2D(1),
    TYPE_3D(2),
    CUBE(3),
    TYPE_1D_ARRAY(4),
    TYPE_2D_ARRAY(5),
    CUBE_ARRAY(6),
}

enum class MipFilter {
    NEAREST,
    LINEAR,
}

enum class SamplerReductionMode {
    WEIGHTED_AVERAGE,
    MIN,
    MAX,
}

enum class BorderColor {
    FLOAT_TRANSPARENT_BLACK,
    INT_TRANSPARENT_BLACK,
    FLOAT_OPAQUE_BLACK,
    INT_OPAQUE_BLACK,
    FLOAT_OPAQUE_WHITE,
    INT_OPAQUE_WHITE,
}

enum class AddressMode {
    REPEAT,
    MIRROR_REPEAT,
    CLAMP_TO_EDGE,
    CLAMP_TO_BORDER,
}

data class SamplerDescriptor(
    val linearFiltering: Boolean = true,
    val repeat: Boolean = false,
    val maxAnisotropy: Float = 1f,
    val mipFilter: MipFilter = MipFilter.NEAREST,
    val minLod: Float = 0f,
    val maxLod: Float = 1000f,
    val lodBias: Float = 0f,
    val compareFunction: CompareFunction? = null,
    val addressMode: AddressMode? = null,
    val reductionMode: SamplerReductionMode = SamplerReductionMode.WEIGHTED_AVERAGE,
    val borderColor: BorderColor = BorderColor.FLOAT_TRANSPARENT_BLACK,
)

/** Integer attachment clear components. Unsigned values use their 32-bit bit pattern. */
data class ClearIntegerColor(
    val red: Int = 0,
    val green: Int = 0,
    val blue: Int = 0,
    val alpha: Int = 1,
)

data class ColorAttachment(
    val texture: Texture,
    val loadAction: LoadAction = LoadAction.CLEAR,
    val storeAction: StoreAction = StoreAction.STORE,
    val clearColor: ClearColor = ClearColor(),
    val level: Int = 0,
    val slice: Int = 0,
    val resolveTexture: Texture? = null,
    val resolveLevel: Int = 0,
    val resolveSlice: Int = 0,
    val clearIntegerColor: ClearIntegerColor? = null,
)

enum class ResolveMode(internal val vk: Int) {
    SAMPLE_ZERO(1),
    AVERAGE(2),
    MIN(4),
    MAX(8),
}

data class DepthStencilResolveSupport(
    val depthModes: Set<ResolveMode>,
    val stencilModes: Set<ResolveMode>,
    val independentModes: Boolean,
)

data class DepthAttachment(
    val texture: Texture,
    val loadAction: LoadAction = LoadAction.CLEAR,
    val storeAction: StoreAction = StoreAction.DONT_CARE,
    val clearDepth: Float = 1f,
    val level: Int = 0,
    val slice: Int = 0,
    val clearStencil: Int = 0,
    val resolveTexture: Texture? = null,
    val resolveLevel: Int = 0,
    val resolveSlice: Int = 0,
    val depthResolveMode: ResolveMode = ResolveMode.SAMPLE_ZERO,
    val stencilResolveMode: ResolveMode = ResolveMode.SAMPLE_ZERO,
)

data class RenderPassDescriptor(
    val colorAttachments: List<ColorAttachment>,
    val depthAttachment: DepthAttachment? = null,
    val viewMask: Int = 0,
    val renderTargetArrayLength: Int = 1,
    val rasterizationRateMap: RasterizationRateMap? = null,
    val subpassLayout: RenderPassLayout? = null,
    val tileShading: TileShadingDescriptor? = null,
) {
    constructor(
        colorAttachment: ColorAttachment,
        depthAttachment: DepthAttachment? = null,
    ) : this(listOf(colorAttachment), depthAttachment)

    val colorAttachment
        get() = colorAttachments.first()

    init {
        require(colorAttachments.isNotEmpty() || depthAttachment != null)
        require(renderTargetArrayLength > 0)
    }
}

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

/** Components returned by sampling a texture view. */
enum class TextureComponent {
    IDENTITY,
    ZERO,
    ONE,
    RED,
    GREEN,
    BLUE,
    ALPHA,
}

data class TextureSwizzle(
    val red: TextureComponent = TextureComponent.IDENTITY,
    val green: TextureComponent = TextureComponent.IDENTITY,
    val blue: TextureComponent = TextureComponent.IDENTITY,
    val alpha: TextureComponent = TextureComponent.IDENTITY,
) {
    internal fun pack() = intArrayOf(red.ordinal, green.ordinal, blue.ordinal, alpha.ordinal)
}
