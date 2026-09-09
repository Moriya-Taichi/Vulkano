package dev.vulkano

import dev.vulkano.internal.Native

sealed interface AccelerationGeometry

/** Indices supplied by the application must stay within vertexCount. */
data class TriangleGeometry(
    val vertexBuffer: Buffer,
    val vertexCount: Int,
    val triangleCount: Int = vertexCount / 3,
    val vertexStride: Long = 12,
    val vertexOffset: Long = 0,
    val indexBuffer: Buffer? = null,
    val indexType: IndexType = IndexType.UINT32,
    val indexOffset: Long = 0,
    val opaque: Boolean = true,
    val motionVertexBuffer: Buffer? = null,
    val motionVertexOffset: Long = vertexOffset,
) : AccelerationGeometry {
    init {
        require(
            vertexCount > 0 &&
                triangleCount > 0 &&
                vertexStride >= 12 &&
                vertexOffset >= 0 &&
                indexOffset >= 0 &&
                motionVertexOffset >= 0
        )
    }
}

/** Each AABB contains minX/minY/minZ/maxX/maxY/maxZ as six Float values. */
data class BoundingBoxGeometry(
    val buffer: Buffer,
    val count: Int,
    val offset: Long = 0,
    val stride: Long = 24,
    val opaque: Boolean = true,
) : AccelerationGeometry {
    init {
        require(count > 0 && offset >= 0 && stride >= 24)
    }
}

class AccelerationStructure internal constructor(device: Device, id: Long) : Resource(device, id) {
    /**
     * Serializes after prior commands complete; the archive is specific to compatible Vulkan
     * drivers.
     */
    fun serialize(): AccelerationStructureArchive = access {
        AccelerationStructureArchive(Native.serializeAcceleration(it))
    }

    val allocatedSize: Long
        get() = access { Native.accelerationStorageSize(it) }

    /**
     * Source must be built. Compact sizing waits for a GPU query and requires prior commands to be
     * complete.
     */
    fun makeCopyDestination(compact: Boolean = false): AccelerationStructure = access {
        AccelerationStructure(device, Native.createAccelerationCopy(it, compact))
    }
}

class AccelerationStructureInstance(
    val structure: AccelerationStructure,
    transform: FloatArray = floatArrayOf(1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 1f, 0f),
    val customIndex: Int = 0,
    val mask: Int = 255,
    val hitGroupOffset: Int = 0,
    val triangleCullingDisabled: Boolean = true,
    val motionTransform: AccelerationMotionTransform? = null,
) {
    internal val matrix = transform.copyOf()
    internal val transforms = motionTransform?.packed?.copyOf() ?: matrix.copyOf(32)

    init {
        require(
            matrix.size == 12 &&
                matrix.all { it.isFinite() } &&
                customIndex in 0..0xffffff &&
                mask in 0..255 &&
                hitGroupOffset in 0..0xffffff
        )
        require(
            motionTransform == null ||
                matrix.contentEquals(floatArrayOf(1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 1f, 0f))
        ) {
            "Specify the complete transform in motionTransform when using motion"
        }
    }
}

private fun Device.packAccelerationGeometry(geometries: List<AccelerationGeometry>): LongArray {
    require(geometries.isNotEmpty())
    val packed =
        geometries
            .flatMap {
                when (it) {
                    is TriangleGeometry -> {
                        require(
                            it.vertexBuffer.device === this &&
                                (it.indexBuffer == null || it.indexBuffer.device === this) &&
                                (it.motionVertexBuffer == null ||
                                    it.motionVertexBuffer.device === this)
                        )
                        listOf(
                            0L,
                            it.vertexBuffer.handle(),
                            it.vertexOffset,
                            it.vertexStride,
                            it.vertexCount.toLong(),
                            it.triangleCount.toLong(),
                            it.indexBuffer?.handle() ?: 0L,
                            if (it.indexBuffer == null) 1000165000L else it.indexType.vk.toLong(),
                            it.indexOffset,
                            if (it.opaque) 1L else 0L,
                            it.motionVertexBuffer?.handle() ?: 0L,
                            it.motionVertexOffset,
                        )
                    }
                    is BoundingBoxGeometry -> {
                        require(it.buffer.device === this)
                        listOf(
                            1L,
                            it.buffer.handle(),
                            it.offset,
                            it.stride,
                            0L,
                            it.count.toLong(),
                            0L,
                            1000165000L,
                            0L,
                            if (it.opaque) 1L else 0L,
                            0L,
                            0L,
                        )
                    }
                }
            }
            .toLongArray()
    return packed
}

fun Device.makePrimitiveAccelerationStructure(
    geometries: List<AccelerationGeometry>,
    allowRefit: Boolean = false,
    allowCompaction: Boolean = false,
): AccelerationStructure = access {
    val packed = packAccelerationGeometry(geometries)
    AccelerationStructure(
        this,
        Native.createPrimitiveAcceleration(nativeHandle, packed, allowRefit, allowCompaction),
    )
}

private fun Device.packAccelerationInstances(
    instances: List<AccelerationStructureInstance>
): Pair<LongArray, FloatArray> {
    require(instances.isNotEmpty() && instances.all { it.structure.device === this })
    return (instances
        .flatMap {
            listOf(
                it.structure.handle(),
                it.customIndex.toLong(),
                it.mask.toLong(),
                it.hitGroupOffset.toLong(),
                if (it.triangleCullingDisabled) 1L else 0L,
                (it.motionTransform?.type ?: 0).toLong(),
            )
        }
        .toLongArray()) to instances.flatMap { it.transforms.toList() }.toFloatArray()
}

fun Device.makeInstanceAccelerationStructure(
    instances: List<AccelerationStructureInstance>,
    allowRefit: Boolean = false,
    allowCompaction: Boolean = false,
): AccelerationStructure = access {
    val packed = packAccelerationInstances(instances)
    AccelerationStructure(
        this,
        Native.createInstanceAcceleration(
            nativeHandle,
            packed.first,
            packed.second,
            allowRefit,
            allowCompaction,
        ),
    )
}

class AccelerationStructureCommandEncoder internal constructor(command: CommandBuffer) :
    CommandEncoder(command) {
    fun copy(source: AccelerationStructure, destination: AccelerationStructure): Unit = encode {
        require(
            source.device === commandBuffer.device && destination.device === commandBuffer.device
        )
        Native.copyAcceleration(it, source.handle(), destination.handle())
    }

    /**
     * Updates vertex/AABB positions; counts, formats, indices, and active primitives must stay
     * unchanged.
     */
    fun refitPrimitives(
        structure: AccelerationStructure,
        geometries: List<AccelerationGeometry>,
    ): Unit = encode {
        require(structure.device === commandBuffer.device)
        Native.refitPrimitiveAcceleration(
            it,
            structure.handle(),
            commandBuffer.device.packAccelerationGeometry(geometries),
        )
    }

    /**
     * Updates instance transforms and references with the original instance count and motion mode.
     */
    fun refitInstances(
        structure: AccelerationStructure,
        instances: List<AccelerationStructureInstance>,
    ): Unit = encode {
        require(structure.device === commandBuffer.device)
        val packed = commandBuffer.device.packAccelerationInstances(instances)
        Native.refitInstanceAcceleration(it, structure.handle(), packed.first, packed.second)
    }

    fun build(structure: AccelerationStructure): Unit = encode {
        require(structure.device === commandBuffer.device)
        Native.buildAcceleration(it, structure.handle(), false)
    }

    /**
     * Refits existing geometry with updated input-buffer contents and unchanged geometry counts.
     */
    fun refit(structure: AccelerationStructure): Unit = encode {
        require(structure.device === commandBuffer.device)
        Native.buildAcceleration(it, structure.handle(), true)
    }
}

enum class RayShaderStage(internal val vk: Int) {
    RAY_GENERATION(256),
    ANY_HIT(512),
    CLOSEST_HIT(1024),
    MISS(2048),
    INTERSECTION(4096),
    CALLABLE(8192),
}

data class RayShader(val function: ShaderFunction, val stage: RayShaderStage)

/** Shader indices refer to the shaders list. -1 means unused. */
data class RayShaderGroup(
    val general: Int = -1,
    val closestHit: Int = -1,
    val anyHit: Int = -1,
    val intersection: Int = -1,
) {
    internal fun pack() =
        listOf(
            if (general >= 0) 0 else if (intersection >= 0) 2 else 1,
            general,
            closestHit,
            anyHit,
            intersection,
        )
}

data class RayTracingPipelineDescriptor(
    val shaders: List<RayShader>,
    val groups: List<RayShaderGroup>,
    val maxRecursionDepth: Int = 1,
    val bindings: List<BindingLayout> = emptyList(),
    val pushConstantBytes: Int = 0,
    val supportsIndirectCommands: Boolean = false,
    val supportsMotionBlur: Boolean = false,
)

class RayTracingPipelineState
internal constructor(device: Device, id: Long, val pushConstantBytes: Int) :
    PipelineState(device, id)

fun Device.makeRayTracingPipelineState(
    descriptor: RayTracingPipelineDescriptor
): RayTracingPipelineState = access {
    require(
        descriptor.shaders.isNotEmpty() &&
            descriptor.shaders.all { it.function.library.device === this }
    )
    val functions = descriptor.shaders.map { it.function }
    val id =
        Native.createRayPipeline(
            nativeHandle,
            functions.map { it.library.code }.toTypedArray(),
            functions.map { it.name }.toTypedArray(),
            functions.map { it.constants }.toTypedArray(),
            descriptor.shaders.map { it.stage.vk }.toIntArray(),
            descriptor.groups.flatMap { it.pack() }.toIntArray(),
            descriptor.maxRecursionDepth,
            packLayout(descriptor.bindings),
            descriptor.pushConstantBytes,
            descriptor.supportsIndirectCommands,
            descriptor.supportsMotionBlur,
        )
    RayTracingPipelineState(this, id, Native.pipelineLocalSize(id)[3])
}

class RayTracingCommandEncoder internal constructor(command: CommandBuffer) :
    ShaderCommandEncoder(command) {
    private var pipeline: RayTracingPipelineState? = null

    fun setRayTracingPipelineState(state: RayTracingPipelineState): Unit = encode {
        require(state.device === commandBuffer.device)
        state.handle()
        pipeline = state
    }

    /**
     * GPU count must not exceed maxSequenceCount; pointed-to resources must be retained with
     * useResource.
     */
    fun executeCommands(
        layout: IndirectCommandLayout,
        indirectBuffer: Buffer,
        maxSequenceCount: Int,
        offset: Long = 0,
        countBuffer: Buffer? = null,
        countOffset: Long = 0,
        maxDrawCount: Int = 1,
    ): Unit = encode {
        require(layout.device === commandBuffer.device)
        Native.executeGenerated(
            it,
            layout.arguments(
                indirectBuffer,
                maxSequenceCount,
                offset,
                countBuffer,
                countOffset,
                maxDrawCount,
            ),
            bindingData(),
            constantData(),
            2,
        )
    }

    fun traceRays(size: Size): Unit = encode {
        Native.traceRays(
            it,
            checkNotNull(pipeline).handle(),
            bindingData(),
            constantData(),
            size.array(),
        )
    }
}

/** Persist only archives created by Vulkano; driver compatibility is checked when restoring. */
class AccelerationStructureArchive internal constructor(bytes: ByteArray) {
    internal val data = bytes.copyOf()
    val bottomLevelAddresses: List<Long> = Native.accelerationArchiveAddresses(data).toList()

    fun toByteArray(): ByteArray = data.copyOf()

    companion object {
        fun fromByteArray(bytes: ByteArray): AccelerationStructureArchive =
            AccelerationStructureArchive(bytes)
    }
}

/**
 * Restores and waits for GPU completion. For TLAS archives map every saved BLAS address to its
 * restored BLAS with equivalent geometry. Restored structures are immutable.
 */
fun Device.restoreAccelerationStructure(
    archive: AccelerationStructureArchive,
    bottomLevelStructures: Map<Long, AccelerationStructure> = emptyMap(),
): AccelerationStructure = access {
    require(bottomLevelStructures.values.all { it.device === this })
    val replacements =
        bottomLevelStructures
            .flatMap { (address, structure) -> listOf(address, structure.handle()) }
            .toLongArray()
    AccelerationStructure(
        this,
        Native.restoreAcceleration(nativeHandle, archive.data, replacements),
    )
}
