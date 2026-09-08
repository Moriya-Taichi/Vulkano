package dev.vulkano

import dev.vulkano.internal.Native
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Android Vulkan 1.1 device. All entry points are serialized per device. Close the device to wait
 * for work and release any remaining child resources.
 */
class Device private constructor(internal val nativeHandle: Long) : AutoCloseable {
    internal var closed = false
        private set

    internal fun <T> access(block: () -> T): T =
        synchronized(this) {
            check(!closed) { "Device is closed" }
            block()
        }

    val capabilities: DeviceCapabilities = run {
        val p = Native.deviceInfo(nativeHandle)
        val version = p[0].toInt()
        DeviceCapabilities(
            Native.deviceName(nativeHandle),
            "${version ushr 22}.${(version ushr 12) and 1023}.${version and 4095}",
            p[1] == 4L,
            Feature.entries.filterTo(mutableSetOf()) {
                p[if (it.group == 0) 2 else 22] and it.bit != 0L
            },
            Feature.entries.filterTo(mutableSetOf()) {
                p[if (it.group == 0) 3 else 23] and it.bit != 0L
            },
            DeviceLimits(
                p[4],
                p[5],
                p[6],
                p[7],
                p[8].toInt(),
                p[9].toInt(),
                p[10].toInt(),
                Size(p[11].toInt(), p[12].toInt(), p[13].toInt()),
                Size(p[14].toInt(), p[15].toInt(), p[16].toInt()),
                p[21] / 1000f,
            ),
            p[17].toInt(),
            p[18].toInt(),
            p[19].toInt(),
            p[20] != 0L,
        )
    }

    /** Physical queues created for this device. Index zero preserves the default ordered queue. */
    val commandQueues: List<CommandQueueCapabilities> =
        Native.queueInfo(nativeHandle).toList().chunked(7).mapIndexed { index, p ->
            CommandQueueCapabilities(
                index,
                p[0],
                p[1],
                p[2] and 1 != 0,
                p[2] and 2 != 0,
                p[2] and 7 != 0,
                p[3],
                TransferGranularity(p[4], p[5], p[6]),
            )
        }

    fun makeCommandQueue(index: Int = 0): CommandQueue = access {
        require(index in commandQueues.indices) { "Unavailable physical queue index" }
        CommandQueue(this, commandQueues[index])
    }

    fun makeBuffer(
        length: Long,
        storageMode: StorageMode = StorageMode.SHARED,
        usage: Set<BufferUsage> =
            setOf(
                BufferUsage.STORAGE,
                BufferUsage.TRANSFER_SOURCE,
                BufferUsage.TRANSFER_DESTINATION,
            ),
    ): Buffer = access {
        require(length > 0 && usage.isNotEmpty() && storageMode != StorageMode.MEMORYLESS)
        Buffer(
            this,
            Native.createBuffer(
                nativeHandle,
                length,
                usage.fold(0) { a, b -> a or b.bit },
                storageMode.ordinal,
            ),
            length,
            storageMode,
        )
    }

    /** CPU-write-only shared storage; GPU usage can include UNIFORM/STORAGE for direct upload. */
    fun makeUploadBuffer(
        length: Long,
        usage: Set<BufferUsage> = setOf(BufferUsage.TRANSFER_SOURCE),
    ): Buffer = access {
        require(length > 0 && usage.isNotEmpty())
        Buffer(
            this,
            Native.createUploadBuffer(nativeHandle, length, usage.fold(0) { a, b -> a or b.bit }),
            length,
            StorageMode.SHARED,
            isCpuWriteOnly = true,
        )
    }

    fun makeTexture(descriptor: TextureDescriptor): Texture = access {
        val id =
            Native.createTexture(
                nativeHandle,
                descriptor.width,
                descriptor.height,
                descriptor.pixelFormat.vk,
                descriptor.usage.fold(0) { a, b -> a or b.bit },
                descriptor.storageMode.ordinal,
                descriptor.options(),
            )
        Texture(
            this,
            id,
            descriptor.copy(usage = descriptor.usage.toSet()),
            Native.textureIsLazy(id),
        )
    }

    fun supportsTexture(descriptor: TextureDescriptor): Boolean = access {
        Native.supportsTexture(
            nativeHandle,
            descriptor.width,
            descriptor.height,
            descriptor.pixelFormat.vk,
            descriptor.usageBits,
            descriptor.options(),
        )
    }

    fun makeSampler(descriptor: SamplerDescriptor = SamplerDescriptor()): Sampler = access {
        Sampler(
            this,
            Native.createSampler(
                nativeHandle,
                descriptor.linearFiltering,
                descriptor.repeat,
                descriptor.maxAnisotropy,
                intArrayOf(
                    descriptor.mipFilter.ordinal,
                    descriptor.compareFunction?.ordinal ?: 7,
                    if (descriptor.compareFunction != null) 1 else 0,
                    descriptor.addressMode?.ordinal ?: -1,
                    descriptor.reductionMode.ordinal,
                    descriptor.borderColor.ordinal,
                ),
                floatArrayOf(descriptor.minLod, descriptor.maxLod, descriptor.lodBias),
            ),
        )
    }

    /** Load an offline-compiled SPIR-V module, not Metal Shading Language. */
    fun makeLibrary(spirv: ByteArray): ShaderLibrary = access { ShaderLibrary(this, spirv) }

    fun makeComputePipelineState(
        function: ShaderFunction,
        bindings: List<BindingLayout> = emptyList(),
        pushConstantBytes: Int = 0,
        supportsIndirectCommands: Boolean = false,
    ): ComputePipelineState = access {
        require(function.library.device === this && pushConstantBytes >= 0)
        val id =
            Native.createComputePipeline(
                nativeHandle,
                function.library.code,
                function.name,
                packLayout(bindings),
                pushConstantBytes,
                function.constants,
                supportsIndirectCommands,
            )
        val size = Native.pipelineLocalSize(id)
        ComputePipelineState(this, id, Size(size[0], size[1], size[2]), size[3])
    }

    fun makeRenderPipelineState(
        vertexFunction: ShaderFunction,
        fragmentFunction: ShaderFunction,
        colorFormat: PixelFormat = PixelFormat.RGBA8_UNORM,
        depthFormat: PixelFormat? = null,
        bindings: List<BindingLayout> = emptyList(),
        pushConstantBytes: Int = 0,
        alphaBlending: Boolean = false,
        sampleCount: Int = 1,
    ): RenderPipelineState =
        makeRenderPipelineState(
            RenderPipelineDescriptor(
                vertexFunction,
                fragmentFunction,
                listOf(RenderColorAttachmentDescriptor(colorFormat, alphaBlending)),
                depthFormat = depthFormat,
                bindings = bindings,
                pushConstantBytes = pushConstantBytes,
                sampleCount = sampleCount,
            )
        )

    fun makeRenderPipelineState(descriptor: RenderPipelineDescriptor): RenderPipelineState =
        access {
            val functions =
                listOfNotNull(
                    descriptor.vertexFunction,
                    descriptor.fragmentFunction,
                    descriptor.tessellationControlFunction,
                    descriptor.tessellationEvaluationFunction,
                    descriptor.objectFunction,
                )
            require(
                functions.all { it.library.device === this } && descriptor.pushConstantBytes >= 0
            )
            require(
                (descriptor.tessellationControlFunction == null) ==
                    (descriptor.tessellationEvaluationFunction == null)
            )
            val id =
                Native.createGraphics(
                    nativeHandle,
                    functions.map { it.library.code }.toTypedArray(),
                    functions.map { it.name }.toTypedArray(),
                    functions.map { it.constants }.toTypedArray(),
                    descriptor.pack(),
                    packLayout(descriptor.bindings),
                    descriptor.pushConstantBytes,
                    descriptor.meshShader,
                    descriptor.subpassLayout?.pack() ?: intArrayOf(),
                    descriptor.subpassIndex,
                    descriptor.supportsIndirectCommands,
                    descriptor.tileShading?.pack() ?: intArrayOf(),
                )
            RenderPipelineState(this, id, Native.pipelineLocalSize(id)[3])
        }

    fun memoryHeaps(): List<MemoryHeap> = access {
        Native.memoryHeaps(nativeHandle).toList().chunked(5).map {
            MemoryHeap(it[0], it[1] != 0L, it[2], it[3], it[4] != 0L)
        }
    }

    fun waitUntilIdle() {
        while (!access { Native.pollIdle(nativeHandle) }) Thread.sleep(1)
    }

    override fun close() {
        if (synchronized(this) { closed }) return
        waitUntilIdle()
        synchronized(this) {
            if (!closed) {
                Native.closeDevice(nativeHandle)
                closed = true
            }
        }
    }

    companion object {
        /** Unsupported devices/features fail at creation; OS version alone is insufficient. */
        fun create(
            requiredFeatures: Set<Feature> = emptySet(),
            enableValidation: Boolean = false,
            allowSoftwareRenderer: Boolean = false,
        ): Device {
            val id =
                Native.createDevice(
                    requiredFeatures.filter { it.group == 0 }.fold(0L) { a, b -> a or b.bit },
                    enableValidation,
                    allowSoftwareRenderer,
                    requiredFeatures.filter { it.group == 1 }.fold(0L) { a, b -> a or b.bit },
                )
            try {
                return Device(id)
            } catch (error: Throwable) {
                Native.closeDevice(id)
                throw error
            }
        }
    }
}

internal fun packLayout(bindings: List<BindingLayout>): IntArray =
    bindings
        .flatMap {
            val sampler = it.immutableSampler?.handle() ?: 0L
            listOf(it.index, it.type.vk, it.count, sampler.toInt(), (sampler ushr 32).toInt())
        }
        .toIntArray()

abstract class Resource internal constructor(val device: Device, private var id: Long) :
    AutoCloseable {
    internal val isClosed
        get() = id == 0L

    internal fun handle(): Long {
        check(id != 0L) { "Resource is closed" }
        return id
    }

    internal fun <T> access(block: (Long) -> T): T = device.access { block(handle()) }

    override fun close() =
        synchronized(device) {
            // Native close is idempotent, including after Device.close().
            if (id != 0L) {
                Native.close(id)
                id = 0
            }
        }
}

class Buffer
internal constructor(
    device: Device,
    id: Long,
    val length: Long,
    val storageMode: StorageMode,
    val isCpuWriteOnly: Boolean = false,
) : Resource(device, id) {
    val gpuAddress: Long
        get() = access { Native.bufferAddress(it) }

    /** Copies remaining bytes without changing the source position; flushes non-coherent memory. */
    fun write(source: ByteBuffer, offset: Long = 0): Unit = access {
        require(source.isDirect) { "Use a direct ByteBuffer" }
        Native.writeBuffer(it, offset, source.slice())
    }

    /** Copies into remaining bytes without changing position; invalidates non-coherent memory. */
    fun read(destination: ByteBuffer, offset: Long = 0): Unit = access {
        require(!isCpuWriteOnly) {
            "Upload buffers prohibit CPU reads; blit to a shared readback buffer"
        }
        require(destination.isDirect && !destination.isReadOnly) {
            "Use a writable direct ByteBuffer"
        }
        Native.readBuffer(it, offset, destination.slice())
    }

    fun write(bytes: ByteArray, offset: Long = 0) =
        write(ByteBuffer.allocateDirect(bytes.size).put(bytes).apply { flip() }, offset)

    fun readBytes(count: Int, offset: Long = 0): ByteArray {
        require(count > 0)
        val bytes = ByteBuffer.allocateDirect(count)
        read(bytes, offset)
        return ByteArray(count).also { bytes.get(it) }
    }
}

class Texture
internal constructor(
    device: Device,
    id: Long,
    val descriptor: TextureDescriptor,
    val isLazilyAllocated: Boolean = false,
) : Resource(device, id) {
    val width
        get() = descriptor.width

    val height
        get() = descriptor.height

    val pixelFormat
        get() = descriptor.pixelFormat

    fun sizeAtLevel(level: Int = 0): Size {
        require(level in 0 until descriptor.mipLevels)
        return Size(
            maxOf(1, width shr level),
            maxOf(1, height shr level),
            maxOf(1, descriptor.depth shr level),
        )
    }

    fun makeTextureView(
        pixelFormat: PixelFormat = this.pixelFormat,
        textureType: TextureType = descriptor.textureType,
        level: Int = 0,
        levelCount: Int = descriptor.mipLevels - level,
        slice: Int = 0,
        sliceCount: Int = descriptor.arrayLength - slice,
        usage: Set<TextureUsage> = descriptor.usage,
        swizzle: TextureSwizzle = TextureSwizzle(),
    ): Texture = access {
        val size = sizeAtLevel(level)
        val view =
            descriptor.copy(
                width = size.width,
                height = size.height,
                depth = size.depth,
                pixelFormat = pixelFormat,
                textureType = textureType,
                mipLevels = levelCount,
                arrayLength = sliceCount,
                usage = usage,
            )
        Texture(
            device,
            Native.createTextureView(
                it,
                pixelFormat.vk,
                textureType.vk,
                level,
                levelCount,
                slice,
                sliceCount,
                usage.fold(0) { bits, use -> bits or use.bit },
                swizzle.pack(),
            ),
            view,
            isLazilyAllocated,
        )
    }
}

class Sampler internal constructor(device: Device, id: Long) : Resource(device, id)

abstract class PipelineState internal constructor(device: Device, id: Long) : Resource(device, id)

class ComputePipelineState
internal constructor(
    device: Device,
    id: Long,
    val threadgroupSize: Size,
    val pushConstantBytes: Int,
) : PipelineState(device, id)

class RenderPipelineState
internal constructor(device: Device, id: Long, val pushConstantBytes: Int) :
    PipelineState(device, id)

class ShaderLibrary internal constructor(internal val device: Device, spirv: ByteArray) {
    internal val code = spirv.copyOf()

    init {
        require(
            code.size >= 20 &&
                code.size % 4 == 0 &&
                ByteBuffer.wrap(code).order(ByteOrder.LITTLE_ENDIAN).int == 0x07230203
        ) {
            "Invalid SPIR-V module"
        }
    }

    fun makeFunction(
        name: String = "main",
        constants: FunctionConstants = FunctionConstants(),
    ): ShaderFunction {
        require(name.isNotEmpty() && name.all { it.code in 1..127 }) {
            "Entry point must be non-empty ASCII"
        }
        return ShaderFunction(this, name, constants.pack())
    }
}

class ShaderFunction
internal constructor(
    internal val library: ShaderLibrary,
    val name: String,
    internal val constants: IntArray,
)

internal fun TextureDescriptor.options() =
    intArrayOf(depth, mipLevels, arrayLength, sampleCount, textureType.vk)
