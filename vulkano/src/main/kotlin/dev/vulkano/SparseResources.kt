package dev.vulkano

import dev.vulkano.internal.Native

class SparseCapabilities internal constructor(values: LongArray) {
    val supported: Boolean = values[0] != 0L
    val buffers: Boolean = values[1] != 0L
    val textures2D: Boolean = values[2] != 0L
    val textures3D: Boolean = values[3] != 0L
    val shaderResidency: Boolean = values[4] != 0L
    val aliasedMappings: Boolean = values[5] != 0L
    val shaderMinLod: Boolean = values[6] != 0L
    val nonResidentReadsAreZero: Boolean = values[7] != 0L
    val virtualAddressSpaceSize: Long = values[8]
}

fun Device.sparseCapabilities(): SparseCapabilities = access {
    SparseCapabilities(Native.sparseCapabilities(nativeHandle))
}

/** Buffer with independently resident pages. Mapping waits for previous Device commands. */
class SparseBuffer internal constructor(val buffer: Buffer) : AutoCloseable {
    val pageSize: Long = buffer.access { Native.sparseInfo(it)[1] }
    val pageCount: Long = buffer.access { Native.sparseInfo(it)[0] / pageSize }
    val allocatedBytes: Long
        get() = buffer.access { Native.sparseInfo(it)[2] }

    fun setResident(firstPage: Long, pageCount: Int = 1, resident: Boolean = true) {
        require(firstPage >= 0 && pageCount > 0)
        buffer.device.waitUntilIdle()
        buffer.access { Native.mapSparseBuffer(it, firstPage, pageCount, resident, 0, 0) }
    }

    fun isResident(firstPage: Long, pageCount: Int = 1): Boolean =
        buffer.access { Native.sparseBufferResident(it, firstPage, pageCount) }

    /**
     * Shares physical pages; requires sparse aliased mappings. GPU access must remain synchronized.
     */
    fun copyMappings(
        source: SparseBuffer,
        sourcePage: Long,
        destinationPage: Long,
        pageCount: Int = 1,
    ) {
        require(source.buffer.device === buffer.device)
        buffer.device.waitUntilIdle()
        buffer.access {
            Native.mapSparseBuffer(
                it,
                destinationPage,
                pageCount,
                true,
                source.buffer.handle(),
                sourcePage,
            )
        }
    }

    override fun close() = buffer.close()
}

fun Device.makeSparseBuffer(
    length: Long,
    usage: Set<BufferUsage> =
        setOf(BufferUsage.STORAGE, BufferUsage.TRANSFER_SOURCE, BufferUsage.TRANSFER_DESTINATION),
): SparseBuffer = access {
    require(length > 0 && usage.isNotEmpty())
    SparseBuffer(
        Buffer(
            this,
            Native.createSparseBuffer(nativeHandle, length, usage.fold(0) { a, b -> a or b.bit }),
            length,
            StorageMode.PRIVATE,
        )
    )
}

/**
 * Sparse single-sampled color image. Newly mapped tiles must be initialized before their values are
 * used.
 */
class SparseTexture internal constructor(val texture: Texture) : AutoCloseable {
    private val properties = texture.access { Native.sparseInfo(it) }
    val tileSize: Size = Size(properties[4].toInt(), properties[5].toInt(), properties[6].toInt())
    val tileBytes: Long = properties[1]
    val mipTailFirstLevel: Int = properties[7].toInt()
    val mipTailBytes: Long = properties[8]
    val singleMipTail: Boolean = properties[9] != 0L
    val allocatedBytes: Long
        get() = texture.access { Native.sparseInfo(it)[2] }

    val metadataBytes: Long = properties[10]

    /** Uses texel coordinates aligned to tileSize, except at an image edge. */
    fun setResident(region: TextureRegion, resident: Boolean = true) {
        texture.device.waitUntilIdle()
        texture.access { Native.mapSparseTexture(it, region.pack(), resident, 0, intArrayOf()) }
    }

    /** Tests all mapped tiles in a tile-aligned region, or its mip-tail allocation. */
    fun isResident(region: TextureRegion): Boolean =
        texture.access { Native.sparseTextureResident(it, region.pack()) }

    /** A single mip tail uses slice 0; otherwise each array slice has an independent tail. */
    fun setMipTailResident(slice: Int = 0, resident: Boolean = true) {
        texture.device.waitUntilIdle()
        texture.access { Native.mapSparseTail(it, slice, resident) }
    }

    /** Shares ordinary tiles; mip tails cannot be shared with this operation. */
    fun copyMappings(
        source: SparseTexture,
        sourceRegion: TextureRegion,
        destinationRegion: TextureRegion,
    ) {
        require(source.texture.device === texture.device)
        texture.device.waitUntilIdle()
        texture.access {
            Native.mapSparseTexture(
                it,
                destinationRegion.pack(),
                true,
                source.texture.handle(),
                sourceRegion.pack(),
            )
        }
    }

    override fun close() = texture.close()
}

fun Device.makeSparseTexture(descriptor: TextureDescriptor): SparseTexture {
    require(descriptor.storageMode == StorageMode.PRIVATE && descriptor.sampleCount == 1)
    waitUntilIdle()
    return access {
        SparseTexture(
            Texture(
                this,
                Native.createSparseTexture(
                    nativeHandle,
                    descriptor.width,
                    descriptor.height,
                    descriptor.pixelFormat.vk,
                    descriptor.usage.fold(0) { a, b -> a or b.bit },
                    descriptor.options(),
                ),
                descriptor.copy(usage = descriptor.usage.toSet()),
            )
        )
    }
}
