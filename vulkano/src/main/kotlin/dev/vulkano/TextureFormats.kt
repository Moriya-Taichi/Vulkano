package dev.vulkano

import dev.vulkano.internal.Native

/** Operations supported by optimal tiling for a pixel format. */
enum class TextureFormatFeature(internal val bit: Long) {
    SAMPLED(1),
    STORAGE(2),
    STORAGE_ATOMIC(4),
    COLOR_ATTACHMENT(128),
    COLOR_ATTACHMENT_BLEND(256),
    DEPTH_STENCIL_ATTACHMENT(512),
    BLIT_SOURCE(1024),
    BLIT_DESTINATION(2048),
    LINEAR_FILTER(4096),
    TRANSFER_SOURCE(16384),
    TRANSFER_DESTINATION(32768),
    FILTER_MIN_MAX(65536),
}

/** Limits for the requested format, usage, texture type and storage mode together. */
data class TextureFormatCapabilities(
    val maxSize: Size,
    val maxMipLevels: Int,
    val maxArrayLength: Int,
    val sampleCounts: Set<Int>,
    val maxResourceBytes: Long,
    val features: Set<TextureFormatFeature>,
    /**
     * Enable these before allocation; multisample storage additionally requires
     * STORAGE_IMAGE_MULTISAMPLE.
     */
    val requiredFeatures: Set<Feature>,
)

/** Returns null for unsupported combinations. Does not allocate memory or enable any Features. */
fun Device.textureFormatCapabilities(
    pixelFormat: PixelFormat,
    usage: Set<TextureUsage> = setOf(TextureUsage.SAMPLED, TextureUsage.TRANSFER_DESTINATION),
    textureType: TextureType = TextureType.TYPE_2D,
    storageMode: StorageMode = StorageMode.PRIVATE,
): TextureFormatCapabilities? = access {
    require(
        pixelFormat != PixelFormat.EXTERNAL &&
            usage.isNotEmpty() &&
            storageMode != StorageMode.SHARED
    )
    val depthStencil = pixelFormat.isDepth || pixelFormat.isStencil
    require(
        if (depthStencil) TextureUsage.COLOR_ATTACHMENT !in usage && TextureUsage.STORAGE !in usage
        else TextureUsage.DEPTH_ATTACHMENT !in usage
    )
    if (storageMode == StorageMode.MEMORYLESS)
        require(
            usage - TextureUsage.INPUT_ATTACHMENT ==
                setOf(
                    if (depthStencil) TextureUsage.DEPTH_ATTACHMENT
                    else TextureUsage.COLOR_ATTACHMENT
                )
        )
    if (
        TextureUsage.SHADING_RATE_ATTACHMENT in usage &&
            (pixelFormat != PixelFormat.R8_UINT ||
                textureType !in setOf(TextureType.TYPE_2D, TextureType.TYPE_2D_ARRAY))
    )
        return@access null
    val required = buildSet {
        when (pixelFormat.vk) {
            in 131..146 -> add(Feature.TEXTURE_COMPRESSION_BC)
            in 147..156 -> add(Feature.TEXTURE_COMPRESSION_ETC2)
            in 157..184 -> add(Feature.TEXTURE_COMPRESSION_ASTC_LDR)
            in 1000066000..1000066013 -> add(Feature.TEXTURE_COMPRESSION_ASTC_HDR)
            in 1000054000..1000054007 -> add(Feature.TEXTURE_COMPRESSION_PVRTC)
        }
        if (textureType == TextureType.CUBE_ARRAY) add(Feature.TEXTURE_CUBE_ARRAY)
        if (TextureUsage.SHADING_RATE_ATTACHMENT in usage) add(Feature.ATTACHMENT_SHADING_RATE)
    }
    if (!capabilities.availableFeatures.containsAll(required)) return@access null
    val bits =
        usage.fold(if (storageMode == StorageMode.MEMORYLESS) 64 else 0) { a, b -> a or b.bit }
    val p = Native.textureFormatCapabilities(nativeHandle, pixelFormat.vk, textureType.vk, bits)
    if (p.isEmpty()) return@access null
    TextureFormatCapabilities(
        Size(p[0].toInt(), p[1].toInt(), p[2].toInt()),
        p[3].toInt(),
        p[4].toInt(),
        setOf(1, 2, 4, 8, 16, 32, 64).filterTo(mutableSetOf()) { p[5] and it.toLong() != 0L },
        p[6],
        TextureFormatFeature.entries.filterTo(mutableSetOf()) { p[7] and it.bit != 0L },
        required,
    )
}

internal val PixelFormat.isPvrtc1: Boolean
    get() = vk in 1000054000..1000054007 && (vk - 1000054000) % 4 < 2
