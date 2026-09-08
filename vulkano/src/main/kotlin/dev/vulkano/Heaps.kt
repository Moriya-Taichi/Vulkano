package dev.vulkano

import dev.vulkano.internal.Native

/** A fixed-capacity allocation pool; its resources keep the underlying allocation alive. */
class Heap
internal constructor(device: Device, id: Long, val size: Long, val storageMode: StorageMode) :
    Resource(device, id) {
    fun makeBuffer(
        length: Long,
        usage: Set<BufferUsage> =
            setOf(
                BufferUsage.STORAGE,
                BufferUsage.TRANSFER_SOURCE,
                BufferUsage.TRANSFER_DESTINATION,
            ),
    ): Buffer = access {
        require(length > 0 && usage.isNotEmpty())
        Buffer(
            device,
            Native.createHeapBuffer(it, length, usage.fold(0) { a, b -> a or b.bit }),
            length,
            storageMode,
        )
    }

    fun makeTexture(descriptor: TextureDescriptor): Texture = access {
        require(storageMode == StorageMode.PRIVATE && descriptor.storageMode == StorageMode.PRIVATE)
        val id =
            Native.createHeapTexture(
                it,
                descriptor.width,
                descriptor.height,
                descriptor.pixelFormat.vk,
                descriptor.usage.fold(0) { a, b -> a or b.bit },
                descriptor.options(),
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
