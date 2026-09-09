package dev.vulkano

import dev.vulkano.internal.Native

/** A fixed-capacity allocation pool; its resources keep the underlying allocation alive. */
class Heap
internal constructor(
    device: Device,
    id: Long,
    val size: Long,
    val storageMode: StorageMode,
    val isPlacement: Boolean = false,
) : Resource(device, id) {
    fun makeBuffer(
        length: Long,
        usage: Set<BufferUsage> =
            setOf(
                BufferUsage.STORAGE,
                BufferUsage.TRANSFER_SOURCE,
                BufferUsage.TRANSFER_DESTINATION,
            ),
        offset: Long? = null,
    ): Buffer = access {
        require(
            length > 0 &&
                usage.isNotEmpty() &&
                (offset != null) == isPlacement &&
                (offset ?: 0) >= 0
        )
        Buffer(
            device,
            Native.createHeapBuffer(it, length, usage.fold(0) { a, b -> a or b.bit }, offset ?: 0),
            length,
            storageMode,
        )
    }

    fun makeTexture(descriptor: TextureDescriptor, offset: Long? = null): Texture = access {
        require((offset != null) == isPlacement && (offset ?: 0) >= 0)
        require(storageMode == StorageMode.PRIVATE && descriptor.storageMode == StorageMode.PRIVATE)
        val id =
            Native.createHeapTexture(
                it,
                descriptor.width,
                descriptor.height,
                descriptor.pixelFormat.vk,
                descriptor.usage.fold(0) { a, b -> a or b.bit },
                descriptor.options(),
                offset ?: 0,
            )
        Texture(device, id, descriptor.copy(usage = descriptor.usage.toSet()))
    }
}

fun Device.makeHeap(size: Long, storageMode: StorageMode = StorageMode.PRIVATE): Heap = access {
    require(size > 0 && storageMode != StorageMode.MEMORYLESS)
    Heap(this, Native.createHeap(nativeHandle, size, storageMode.ordinal), size, storageMode)
}

class TextureBuffer internal constructor(device: Device, id: Long, val pixelFormat: PixelFormat) :
    Resource(device, id)

fun Buffer.makeTextureBuffer(
    pixelFormat: PixelFormat,
    offset: Long = 0,
    length: Long = this.length - offset,
    writable: Boolean = false,
): TextureBuffer = access {
    TextureBuffer(
        device,
        Native.createTextureBuffer(it, pixelFormat.vk, offset, length, writable),
        pixelFormat,
    )
}

/** Native memory requirements, including padding for buffer/image granularity and cache atoms. */
class HeapResourceRequirements
internal constructor(internal val device: Device, values: LongArray) {
    val size: Long = values[0]
    val alignment: Long = values[1]
    internal val memoryTypeBits: Int = values[2].toInt()
    val requiresDedicatedAllocation: Boolean = values[3] != 0L
}

fun Device.heapBufferRequirements(
    length: Long,
    usage: Set<BufferUsage> =
        setOf(BufferUsage.STORAGE, BufferUsage.TRANSFER_SOURCE, BufferUsage.TRANSFER_DESTINATION),
): HeapResourceRequirements = access {
    require(length > 0 && usage.isNotEmpty())
    HeapResourceRequirements(
        this,
        Native.heapBufferRequirements(nativeHandle, length, usage.fold(0) { a, b -> a or b.bit }),
    )
}

fun Device.heapTextureRequirements(descriptor: TextureDescriptor): HeapResourceRequirements =
    access {
        require(descriptor.storageMode == StorageMode.PRIVATE)
        HeapResourceRequirements(
            this,
            Native.heapTextureRequirements(
                nativeHandle,
                descriptor.width,
                descriptor.height,
                descriptor.pixelFormat.vk,
                descriptor.usage.fold(0) { a, b -> a or b.bit },
                descriptor.options(),
            ),
        )
    }

/**
 * Creates one allocation for resources placed at explicit byte offsets. Overlapping resources
 * require aliasResources between uses; aliased texture contents are discarded. Use requirements
 * from every resource type to select a compatible memory type.
 */
fun Device.makePlacementHeap(
    size: Long,
    requirements: List<HeapResourceRequirements>,
    storageMode: StorageMode = StorageMode.PRIVATE,
): Heap = access {
    require(size > 0 && storageMode != StorageMode.MEMORYLESS && requirements.isNotEmpty())
    require(
        requirements.all {
            it.device === this && !it.requiresDedicatedAllocation && it.size <= size
        }
    )
    val types = requirements.fold(-1) { bits, r -> bits and r.memoryTypeBits }
    require(types != 0) { "Resources have no common memory type" }
    Heap(
        this,
        Native.createPlacementHeap(
            nativeHandle,
            size,
            storageMode.ordinal,
            types,
            requirements.maxOf { it.alignment },
        ),
        size,
        storageMode,
        true,
    )
}
