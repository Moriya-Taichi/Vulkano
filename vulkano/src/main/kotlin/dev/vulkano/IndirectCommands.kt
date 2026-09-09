package dev.vulkano

import dev.vulkano.internal.Native
import java.nio.ByteBuffer
import java.nio.ByteOrder

enum class ShaderStage(internal val vk: Int) {
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

/** Token offsets and sequence stride are in bytes and must be multiples of four. */
class IndirectCommandToken
private constructor(
    internal val type: Int,
    val offset: Int,
    internal val target: Int = 0,
    internal val size: Int = 0,
) {
    init {
        require(offset >= 0 && offset % 4 == 0 && target >= 0 && size >= 0)
    }

    internal fun pack() = listOf(type, offset, target, size)

    companion object {
        /** A uint32 index into the layout's pipeline list. This token must be first. */
        fun pipeline(offset: Int) = IndirectCommandToken(0, offset)

        fun pushConstants(offset: Int, destinationOffset: Int, byteCount: Int) =
            IndirectCommandToken(1, offset, destinationOffset, byteCount)

        /** Writes the sequence number to four push-constant bytes; consumes no input bytes. */
        fun sequenceIndex(offset: Int, destinationOffset: Int) =
            IndirectCommandToken(2, offset, destinationOffset, 4)

        /** uint64 address, uint32 size, uint32 Vulkan IndexType (0=UINT16, 1=UINT32). */
        fun indexBuffer(offset: Int) = IndirectCommandToken(3, offset)

        /**
         * uint64 address, uint32 size, uint32 stride. Retain addressed resources with useResource.
         */
        fun vertexBuffer(offset: Int, index: Int) = IndirectCommandToken(4, offset, index)

        /**
         * Five words: indexCount, instanceCount, firstIndex, signed vertexOffset, firstInstance.
         */
        fun drawIndexed(offset: Int) = IndirectCommandToken(5, offset)

        /** Four words: vertexCount, instanceCount, firstVertex, firstInstance. */
        fun draw(offset: Int) = IndirectCommandToken(6, offset)

        /** uint64 indirect-argument address, uint32 stride, uint32 draw count. */
        fun drawIndexedCount(offset: Int) = IndirectCommandToken(7, offset)

        fun drawCount(offset: Int) = IndirectCommandToken(8, offset)

        /** Three uint32 workgroup counts. Compute sequences may execute in any order. */
        fun dispatch(offset: Int) = IndirectCommandToken(9, offset)

        fun drawMesh(offset: Int) = IndirectCommandToken(1000328000, offset)

        fun drawMeshCount(offset: Int) = IndirectCommandToken(1000328001, offset)

        /**
         * VkTraceRaysIndirectCommand2KHR; use indirectTraceArguments to encode an initial value.
         */
        fun traceRays(offset: Int) = IndirectCommandToken(1000386004, offset)
    }
}

class IndirectCommandLayout internal constructor(device: Device, id: Long, val stride: Int) :
    Resource(device, id) {
    internal fun arguments(
        buffer: Buffer,
        maxSequenceCount: Int,
        offset: Long,
        countBuffer: Buffer?,
        countOffset: Long,
        maxDrawCount: Int,
    ): LongArray {
        require(buffer.device === device && (countBuffer == null || countBuffer.device === device))
        require(maxSequenceCount > 0 && offset >= 0 && countOffset >= 0 && maxDrawCount > 0)
        return longArrayOf(
            handle(),
            buffer.handle(),
            offset,
            maxSequenceCount.toLong(),
            countBuffer?.handle() ?: 0,
            countOffset,
            maxDrawCount.toLong(),
        )
    }
}

data class IndirectCommandLimits(
    val maxPipelineCount: Long,
    val maxSequenceCount: Long,
    val maxTokenCount: Int,
    val maxTokenOffset: Long,
    val maxStride: Long,
    val shaderStages: Set<ShaderStage>,
    val pipelineBindingStages: Set<ShaderStage>,
    val supportsMultiDrawCount: Boolean,
    val supportsVertexBuffers: Boolean,
)

/** Available limits can be inspected before enabling DEVICE_GENERATED_COMMANDS. */
fun Device.indirectCommandLimits(): IndirectCommandLimits? = access {
    if (Feature.DEVICE_GENERATED_COMMANDS !in capabilities.availableFeatures) return@access null
    val p = Native.generatedLimits(nativeHandle)
    fun stages(bits: Long) =
        ShaderStage.entries.filterTo(mutableSetOf()) { bits and it.vk.toLong() != 0L }
    IndirectCommandLimits(
        p[0],
        p[1],
        p[2].toInt(),
        p[3],
        p[4],
        stages(p[5]),
        stages(p[6]),
        p[7] != 0L,
        p[8] != 0L,
    )
}

/**
 * Retains every pipeline until this layout and its submitted commands have completed. Pipeline
 * selection requires supportsIndirectCommands=true and matching fixed state/interfaces. A layout
 * without a pipeline-selection token uses exactly one ordinary pipeline.
 */
fun Device.makeIndirectCommandLayout(
    pipelines: List<PipelineState>,
    tokens: List<IndirectCommandToken>,
    stride: Int,
    unorderedSequences: Boolean = false,
): IndirectCommandLayout = access {
    require(pipelines.isNotEmpty() && pipelines.all { it.device === this })
    require(tokens.isNotEmpty() && stride > 0 && stride % 4 == 0)
    IndirectCommandLayout(
        this,
        Native.createGeneratedLayout(
            nativeHandle,
            pipelines.map { it.handle() }.toLongArray(),
            tokens.flatMap { it.pack() }.toIntArray(),
            stride,
            unorderedSequences,
        ),
        stride,
    )
}

/**
 * Encodes the pipeline's SBT addresses and dispatch dimensions in Vulkan's 104-byte indirect
 * format. Keep this pipeline in the IndirectCommandLayout used to execute the resulting commands.
 */
fun RayTracingPipelineState.indirectTraceArguments(size: Size): ByteArray = access {
    val regions = Native.rayTableRegions(it)
    val output = ByteBuffer.allocate(104).order(ByteOrder.LITTLE_ENDIAN)
    output.putLong(regions[0]).putLong(regions[1])
    for (n in 3 until 12) output.putLong(regions[n])
    output.putInt(size.width).putInt(size.height).putInt(size.depth)
    output.array()
}
