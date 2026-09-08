package dev.vulkano

import dev.vulkano.internal.Native

enum class TensorDataType(val byteSize: Int, internal val matrixType: Int) {
    FLOAT16(2, 0),
    FLOAT32(4, 1),
    FLOAT64(8, 2),
    INT8(1, 3),
    INT16(2, 4),
    INT32(4, 5),
    INT64(8, 6),
    UINT8(1, 7),
    UINT16(2, 8),
    UINT32(4, 9),
    UINT64(8, 10),
}

/** Strides are measured in elements; the default is contiguous row-major storage. */
class TensorDescriptor(
    shape: List<Long>,
    val dataType: TensorDataType = TensorDataType.FLOAT32,
    strides: List<Long>? = null,
) {
    val shape: List<Long> = shape.toList()
    val strides: List<Long>
    val requiredBytes: Long

    init {
        require(shape.isNotEmpty() && shape.all { it > 0 })
        try {
            this.strides =
                strides?.toList()
                    ?: run {
                        val values = MutableList(shape.size) { 1L }
                        for (n in shape.lastIndex - 1 downTo 0) values[n] =
                            Math.multiplyExact(values[n + 1], shape[n + 1])
                        values.toList()
                    }
            require(this.strides.size == shape.size && this.strides.all { it > 0 })
            var last = 0L
            shape.indices.forEach {
                last = Math.addExact(last, Math.multiplyExact(shape[it] - 1, this.strides[it]))
            }
            requiredBytes = Math.multiplyExact(Math.addExact(last, 1), dataType.byteSize.toLong())
        } catch (e: ArithmeticException) {
            throw IllegalArgumentException("Tensor storage size overflow", e)
        }
    }

    fun byteOffset(indices: List<Long>): Long {
        require(
            indices.size == shape.size && indices.indices.all { indices[it] in 0 until shape[it] }
        )
        return indices.indices.sumOf { indices[it] * strides[it] } * dataType.byteSize
    }
}

/** Tensor views own a reference to the buffer and survive closing their parent tensor. */
class Tensor
internal constructor(
    val buffer: Buffer,
    val descriptor: TensorDescriptor,
    val byteOffset: Long = 0,
) : AutoCloseable {
    val device: Device
        get() = buffer.device

    fun makeView(descriptor: TensorDescriptor, byteOffset: Long = 0): Tensor =
        buffer.access {
            require(
                descriptor.dataType == this.descriptor.dataType &&
                    byteOffset >= 0 &&
                    byteOffset % descriptor.dataType.byteSize == 0L &&
                    byteOffset <= this.descriptor.requiredBytes &&
                    descriptor.requiredBytes <= this.descriptor.requiredBytes - byteOffset
            )
            val alias =
                Buffer(
                    device,
                    Native.referenceBuffer(it),
                    buffer.length,
                    buffer.storageMode,
                    buffer.isCpuWriteOnly,
                )
            Tensor(alias, descriptor, this.byteOffset + byteOffset)
        }

    override fun close() = buffer.close()
}

fun Device.makeTensor(
    descriptor: TensorDescriptor,
    storageMode: StorageMode = StorageMode.SHARED,
    gpuAddress: Boolean = false,
): Tensor = access {
    val usage =
        setOf(BufferUsage.STORAGE, BufferUsage.TRANSFER_SOURCE, BufferUsage.TRANSFER_DESTINATION) +
            if (gpuAddress) setOf(BufferUsage.SHADER_DEVICE_ADDRESS) else emptySet()
    Tensor(
        makeBuffer(descriptor.requiredBytes, storageMode = storageMode, usage = usage),
        descriptor,
    )
}

/** Tensor shape and strides are shader parameters; pass them with setBytes or uniform buffers. */
fun ShaderCommandEncoder.setTensor(tensor: Tensor, index: Int, arrayElement: Int = 0) =
    setBuffer(
        tensor.buffer,
        index,
        tensor.byteOffset,
        tensor.descriptor.requiredBytes,
        arrayElement,
    )

/** Subgroup-scoped M×K times K×N plus M×N, as reported by the Vulkan driver. */
data class CooperativeMatrixConfiguration(
    val m: Int,
    val n: Int,
    val k: Int,
    val aType: TensorDataType,
    val bType: TensorDataType,
    val accumulatorType: TensorDataType,
    val resultType: TensorDataType,
    val saturatingAccumulation: Boolean,
)

fun Device.cooperativeMatrixConfigurations(): List<CooperativeMatrixConfiguration> = access {
    Native.cooperativeMatrixConfigurations(nativeHandle).toList().chunked(8).mapNotNull { values ->
        val types =
            (3..6).map { index ->
                TensorDataType.entries.find { it.matrixType == values[index] }
                    ?: return@mapNotNull null
            }
        CooperativeMatrixConfiguration(
            values[0],
            values[1],
            values[2],
            types[0],
            types[1],
            types[2],
            types[3],
            values[7] != 0,
        )
    }
}
