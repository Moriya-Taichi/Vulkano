package dev.vulkano

import android.hardware.HardwareBuffer
import dev.vulkano.internal.Native

/** FOREIGN includes Camera, codecs and CPU access. EXTERNAL requires matching GPU/driver UUIDs. */
enum class ExternalTextureOwner(internal val vk: Int) {
    FOREIGN(-3),
    EXTERNAL(-2),
}

enum class YcbcrModel(internal val vk: Int) {
    RGB_IDENTITY(0),
    YCBCR_IDENTITY(1),
    BT709(2),
    BT601(3),
    BT2020(4),
}

enum class YcbcrRange(internal val vk: Int) {
    FULL(0),
    NARROW(1),
}

/** Null model/range use the driver's suggestions; override with the producer's color metadata. */
data class HardwareBufferConversion(
    val model: YcbcrModel? = null,
    val range: YcbcrRange? = null,
    val linearFiltering: Boolean = false,
)

/** Imported memory is retained independently of the Java HardwareBuffer. */
class HardwareBufferTexture
internal constructor(
    val texture: Texture,
    /** Set as BindingLayout.immutableSampler for external-format (including YUV) sampling. */
    val conversionSampler: Sampler?,
) : AutoCloseable {
    override fun close() {
        conversionSampler?.close()
        texture.close()
    }
}

/**
 * Imports RGB/depth images with their Vulkan format; opaque/YUV images use external-format
 * conversion. Supplying conversion forces external-format sampling even for RGB images. Such images
 * require sampled-only usage, one layer and one mip. Identical conversions share a native sampler
 * across frames. Acquire before GPU use, then release and signal an ExternalSemaphore before
 * returning to the producer. A HardwareBuffer may have only one live import per Device; use texture
 * views for additional bindings.
 */
fun Device.importHardwareBuffer(
    buffer: HardwareBuffer,
    usage: Set<TextureUsage> = setOf(TextureUsage.SAMPLED),
    conversion: HardwareBufferConversion? = null,
    externalOwner: ExternalTextureOwner = ExternalTextureOwner.FOREIGN,
): HardwareBufferTexture = access {
    require(usage.isNotEmpty())
    val id =
        Native.importHardwareBuffer(
            nativeHandle,
            buffer,
            usage.fold(0) { a, b -> a or b.bit },
            conversion != null,
            conversion?.linearFiltering ?: false,
            conversion?.model?.vk ?: -1,
            conversion?.range?.vk ?: -1,
            externalOwner.vk,
        )
    var samplerId = 0L
    try {
        val info = Native.hardwareBufferTextureInfo(id)
        val format = PixelFormat.entries.first { it.vk == info[2] }
        val descriptor =
            TextureDescriptor(
                info[0],
                info[1],
                format,
                usage,
                mipLevels = info[4],
                arrayLength = info[5],
                textureType = TextureType.entries.first { it.vk == info[6] },
            )
        samplerId = Native.hardwareBufferSampler(id)
        HardwareBufferTexture(
            Texture(this, id, descriptor),
            if (samplerId == 0L) null else Sampler(this, samplerId),
        )
    } catch (error: Throwable) {
        if (samplerId != 0L) Native.close(samplerId)
        Native.close(id)
        throw error
    }
}
