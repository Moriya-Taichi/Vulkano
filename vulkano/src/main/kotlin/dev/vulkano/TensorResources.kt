package dev.vulkano

import dev.vulkano.internal.Native
import java.nio.ByteBuffer

enum class TensorLayout {
    OPTIMAL,
    LINEAR,
}

enum class TensorUsage(internal val bit: Long) {
    SHADER(2),
    TRANSFER_SOURCE(4),
    TRANSFER_DESTINATION(8),
    MACHINE_LEARNING(32),
}

internal val TensorDataType.tensorFormat: Int
    get() =
        when (this) {
            TensorDataType.FLOAT16 -> 76
            TensorDataType.FLOAT32 -> 100
            TensorDataType.FLOAT64 -> 112
            TensorDataType.INT8 -> 14
            TensorDataType.INT16 -> 75
            TensorDataType.INT32 -> 99
            TensorDataType.INT64 -> 111
            TensorDataType.UINT8 -> 13
            TensorDataType.UINT16 -> 74
            TensorDataType.UINT32 -> 98
            TensorDataType.UINT64 -> 110
            TensorDataType.BOOL -> 1000460000
        }

/** A device tensor layout. Explicit strides use bytes and must not overlap elements. */
class TensorResourceDescriptor(
    dimensions: List<Long>,
    val dataType: TensorDataType = TensorDataType.FLOAT32,
    val layout: TensorLayout = TensorLayout.OPTIMAL,
    byteStrides: List<Long>? = null,
    usage: Set<TensorUsage> =
        setOf(TensorUsage.SHADER, TensorUsage.TRANSFER_SOURCE, TensorUsage.TRANSFER_DESTINATION),
) {
    val dimensions = dimensions.toList()
    val byteStrides = byteStrides?.toList()
    val usage = usage.toSet()
    internal val usageBits
        get() = usage.fold(0L) { a, b -> a or b.bit }

    val elementCount: Long
    /** For linear layouts, includes gaps between rows but excludes trailing padding. */
    val byteLength: Long
    private val linearStrides: List<Long>

    init {
        require(
            this.dimensions.isNotEmpty() &&
                this.dimensions.all { it > 0 } &&
                this.usage.isNotEmpty()
        )
        require(layout == TensorLayout.LINEAR || this.byteStrides == null) {
            "Optimal tensors have an opaque layout"
        }
        try {
            elementCount = this.dimensions.fold(1L, Math::multiplyExact)
            val packed = MutableList(this.dimensions.size) { dataType.byteSize.toLong() }
            for (i in packed.lastIndex - 1 downTo 0) packed[i] =
                Math.multiplyExact(packed[i + 1], this.dimensions[i + 1])
            linearStrides = this.byteStrides ?: packed
            require(
                linearStrides.size == this.dimensions.size &&
                    linearStrides.last() == dataType.byteSize.toLong()
            )
            for (i in linearStrides.indices) {
                require(linearStrides[i] > 0 && linearStrides[i] % dataType.byteSize == 0L)
                if (i < linearStrides.lastIndex)
                    require(
                        linearStrides[i] >=
                            Math.multiplyExact(linearStrides[i + 1], this.dimensions[i + 1])
                    )
            }
            Math.multiplyExact(linearStrides[0], this.dimensions[0])
            byteLength =
                this.dimensions.indices.fold(dataType.byteSize.toLong()) { size, i ->
                    Math.addExact(
                        size,
                        Math.multiplyExact(this.dimensions[i] - 1, linearStrides[i]),
                    )
                }
        } catch (e: ArithmeticException) {
            throw IllegalArgumentException("Tensor size or stride overflow", e)
        }
    }

    fun byteOffset(indices: List<Long>): Long {
        require(layout == TensorLayout.LINEAR) { "Optimal tensors have an opaque layout" }
        require(
            indices.size == dimensions.size &&
                indices.indices.all { indices[it] in 0 until dimensions[it] }
        )
        return indices.indices.sumOf { indices[it] * linearStrides[it] }
    }
}

/** Dedicated tensor storage for tensor shaders and machine learning pipelines. */
class TensorResource
internal constructor(
    device: Device,
    id: Long,
    val descriptor: TensorResourceDescriptor,
    val storageMode: StorageMode,
) : Resource(device, id) {
    val allocatedBytes: Long
        get() = access { Native.tensorInfo(it)[0] }

    fun makeView(dataType: TensorDataType = descriptor.dataType): TensorView = access {
        require(dataType.byteSize == descriptor.dataType.byteSize)
        TensorView(
            device,
            Native.createTensorView(it, dataType.tensorFormat),
            TensorResourceDescriptor(
                descriptor.dimensions,
                dataType,
                descriptor.layout,
                descriptor.byteStrides,
                descriptor.usage,
            ),
        )
    }

    fun write(source: ByteBuffer, offset: Long = 0): Unit = access {
        require(source.isDirect)
        Native.accessTensorBytes(it, offset, source.slice(), true)
    }

    fun read(destination: ByteBuffer, offset: Long = 0): Unit = access {
        require(destination.isDirect && !destination.isReadOnly)
        Native.accessTensorBytes(it, offset, destination.slice(), false)
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

/** A view retains its tensor storage even after the public TensorResource is closed. */
class TensorView
internal constructor(device: Device, id: Long, val descriptor: TensorResourceDescriptor) :
    Resource(device, id) {
    val dataType: TensorDataType
        get() = descriptor.dataType
}

fun Device.makeTensorResource(
    descriptor: TensorResourceDescriptor,
    storageMode: StorageMode = StorageMode.PRIVATE,
): TensorResource = access {
    require(
        storageMode == StorageMode.PRIVATE ||
            (storageMode == StorageMode.SHARED && descriptor.layout == TensorLayout.LINEAR)
    )
    TensorResource(
        this,
        Native.createTensor(
            nativeHandle,
            descriptor.dataType.tensorFormat,
            descriptor.layout.ordinal,
            descriptor.usageBits,
            descriptor.dimensions.toLongArray(),
            descriptor.byteStrides?.toLongArray() ?: longArrayOf(),
            storageMode.ordinal,
        ),
        descriptor,
        storageMode,
    )
}

fun Device.supportsTensor(
    descriptor: TensorResourceDescriptor,
    storageMode: StorageMode = StorageMode.PRIVATE,
): Boolean = access {
    Native.supportsTensor(
        nativeHandle,
        descriptor.dataType.tensorFormat,
        descriptor.layout.ordinal,
        descriptor.usageBits,
        descriptor.dimensions.toLongArray(),
        descriptor.byteStrides?.toLongArray() ?: longArrayOf(),
        storageMode.ordinal,
    )
}

enum class TensorShaderStage(internal val bit: Int) {
    VERTEX(1),
    TESSELLATION_CONTROL(2),
    TESSELLATION_EVALUATION(4),
    GEOMETRY(8),
    FRAGMENT(16),
    COMPUTE(32),
    TASK(64),
    MESH(128),
    RAY_GENERATION(256),
    ANY_HIT(512),
    CLOSEST_HIT(1024),
    MISS(2048),
    INTERSECTION(4096),
    CALLABLE(8192),
}

data class TensorCapabilities(
    val maxRank: Int,
    val maxElements: Long,
    val maxElementsPerDimension: Long,
    val maxStride: Long,
    val maxBytes: Long,
    val maxShaderAccessArrayLength: Int,
    val maxShaderAccessBytes: Int,
    val maxDescriptors: Int,
    val maxDescriptorsPerStage: Int,
    val shaderStages: Set<TensorShaderStage>,
    val supportsNonPackedLayout: Boolean,
    val supportsShaderAccess: Boolean,
    val supportsDynamicArrayIndexing: Boolean,
    val supportsNonUniformArrayIndexing: Boolean,
)

val Device.tensorCapabilities: TensorCapabilities?
    get() = access {
        if (Feature.TENSOR_RESOURCES !in capabilities.availableFeatures) return@access null
        val p = Native.tensorCapabilities(nativeHandle)
        TensorCapabilities(
            p[0].toInt(),
            p[1],
            p[2],
            p[3],
            p[4],
            p[5].toInt(),
            p[6].toInt(),
            p[7].toInt(),
            p[8].toInt(),
            TensorShaderStage.entries.filterTo(mutableSetOf()) { p[9].toInt() and it.bit != 0 },
            p[10] != 0L,
            p[11] != 0L,
            p[12] != 0L,
            p[13] != 0L,
        )
    }
