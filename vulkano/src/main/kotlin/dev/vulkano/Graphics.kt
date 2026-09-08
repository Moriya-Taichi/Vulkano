package dev.vulkano

enum class PrimitiveType(internal val vk: Int) {
    POINT(0),
    LINE(1),
    LINE_STRIP(2),
    TRIANGLE(3),
    TRIANGLE_STRIP(4),
    TRIANGLE_FAN(5),
    PATCH(10),
}

enum class IndexType(internal val vk: Int, val byteSize: Int) {
    UINT16(0, 2),
    UINT32(1, 4),
}

enum class CullMode {
    NONE,
    FRONT,
    BACK,
    FRONT_AND_BACK,
}

enum class Winding {
    COUNTER_CLOCKWISE,
    CLOCKWISE,
}

enum class TriangleFillMode {
    FILL,
    LINES,
    POINTS,
}

enum class CompareFunction {
    NEVER,
    LESS,
    EQUAL,
    LESS_EQUAL,
    GREATER,
    NOT_EQUAL,
    GREATER_EQUAL,
    ALWAYS,
}

enum class StencilOperation {
    KEEP,
    ZERO,
    REPLACE,
    INCREMENT_CLAMP,
    DECREMENT_CLAMP,
    INVERT,
    INCREMENT_WRAP,
    DECREMENT_WRAP,
}

enum class VertexStepFunction {
    PER_VERTEX,
    PER_INSTANCE,
}

enum class LogicOperation {
    CLEAR,
    AND,
    AND_REVERSE,
    COPY,
    AND_INVERTED,
    NO_OP,
    XOR,
    OR,
    NOR,
    EQUIVALENT,
    INVERT,
    OR_REVERSE,
    COPY_INVERTED,
    OR_INVERTED,
    NAND,
    SET,
}

enum class BlendOperation {
    ADD,
    SUBTRACT,
    REVERSE_SUBTRACT,
    MIN,
    MAX,
}

enum class BlendFactor {
    ZERO,
    ONE,
    SOURCE_COLOR,
    ONE_MINUS_SOURCE_COLOR,
    DESTINATION_COLOR,
    ONE_MINUS_DESTINATION_COLOR,
    SOURCE_ALPHA,
    ONE_MINUS_SOURCE_ALPHA,
    DESTINATION_ALPHA,
    ONE_MINUS_DESTINATION_ALPHA,
    CONSTANT_COLOR,
    ONE_MINUS_CONSTANT_COLOR,
    CONSTANT_ALPHA,
    ONE_MINUS_CONSTANT_ALPHA,
    SOURCE_ALPHA_SATURATE,
    SOURCE1_COLOR,
    ONE_MINUS_SOURCE1_COLOR,
    SOURCE1_ALPHA,
    ONE_MINUS_SOURCE1_ALPHA,
}

data class VertexBufferLayout(
    val index: Int,
    val stride: Int,
    val stepFunction: VertexStepFunction = VertexStepFunction.PER_VERTEX,
) {
    init {
        require(index >= 0 && stride >= 0)
    }
}

data class VertexAttribute(
    val location: Int,
    val bufferIndex: Int,
    val format: PixelFormat,
    val offset: Int = 0,
) {
    init {
        require(
            location >= 0 &&
                bufferIndex >= 0 &&
                offset >= 0 &&
                !format.isCompressed &&
                !format.isDepth &&
                !format.isStencil
        )
    }
}

data class StencilDescriptor(
    val compareFunction: CompareFunction = CompareFunction.ALWAYS,
    val stencilFailureOperation: StencilOperation = StencilOperation.KEEP,
    val depthFailureOperation: StencilOperation = StencilOperation.KEEP,
    val depthStencilPassOperation: StencilOperation = StencilOperation.KEEP,
    val readMask: Int = -1,
    val writeMask: Int = -1,
) {
    internal fun pack() =
        listOf(
            stencilFailureOperation.ordinal,
            depthStencilPassOperation.ordinal,
            depthFailureOperation.ordinal,
            compareFunction.ordinal,
            readMask,
            writeMask,
            0,
        )
}

data class DepthStencilDescriptor(
    val depthCompareFunction: CompareFunction = CompareFunction.LESS,
    val depthWriteEnabled: Boolean = true,
    val depthTestEnabled: Boolean = true,
    val frontFaceStencil: StencilDescriptor? = null,
    val backFaceStencil: StencilDescriptor? = frontFaceStencil,
)

data class RenderColorAttachmentDescriptor(
    val pixelFormat: PixelFormat = PixelFormat.RGBA8_UNORM,
    val blendingEnabled: Boolean = false,
    val sourceRGBBlendFactor: BlendFactor = BlendFactor.SOURCE_ALPHA,
    val destinationRGBBlendFactor: BlendFactor = BlendFactor.ONE_MINUS_SOURCE_ALPHA,
    val rgbBlendOperation: BlendOperation = BlendOperation.ADD,
    val sourceAlphaBlendFactor: BlendFactor = BlendFactor.ONE,
    val destinationAlphaBlendFactor: BlendFactor = BlendFactor.ONE_MINUS_SOURCE_ALPHA,
    val alphaBlendOperation: BlendOperation = BlendOperation.ADD,
    val writeMask: Int = 15,
) {
    init {
        require(writeMask in 0..15)
    }

    internal fun pack() =
        listOf(
            pixelFormat.vk,
            if (blendingEnabled) 1 else 0,
            sourceRGBBlendFactor.ordinal,
            destinationRGBBlendFactor.ordinal,
            rgbBlendOperation.ordinal,
            sourceAlphaBlendFactor.ordinal,
            destinationAlphaBlendFactor.ordinal,
            alphaBlendOperation.ordinal,
            writeMask,
        )
}

data class RenderPipelineDescriptor(
    val vertexFunction: ShaderFunction,
    val fragmentFunction: ShaderFunction,
    val colorAttachments: List<RenderColorAttachmentDescriptor> =
        listOf(RenderColorAttachmentDescriptor()),
    val depthFormat: PixelFormat? = null,
    val depthStencil: DepthStencilDescriptor = DepthStencilDescriptor(),
    val sampleCount: Int = 1,
    val primitiveType: PrimitiveType = PrimitiveType.TRIANGLE,
    val cullMode: CullMode = CullMode.NONE,
    val frontFace: Winding = Winding.COUNTER_CLOCKWISE,
    val fillMode: TriangleFillMode = TriangleFillMode.FILL,
    val depthClampEnabled: Boolean = false,
    val alphaToCoverageEnabled: Boolean = false,
    val vertexBuffers: List<VertexBufferLayout> = emptyList(),
    val vertexAttributes: List<VertexAttribute> = emptyList(),
    val bindings: List<BindingLayout> = emptyList(),
    val pushConstantBytes: Int = 0,
    val tessellationControlFunction: ShaderFunction? = null,
    val tessellationEvaluationFunction: ShaderFunction? = null,
    val patchControlPoints: Int = 3,
    val meshShader: Boolean = false,
    val objectFunction: ShaderFunction? = null,
    val viewportCount: Int = 1,
    val viewMask: Int = 0,
    val sampleShadingEnabled: Boolean = false,
    val minSampleShading: Float = 1f,
    val alphaToOneEnabled: Boolean = false,
    val sampleMask: Long = -1L,
    val primitiveRestartEnabled: Boolean = false,
    val rasterizationDisabled: Boolean = false,
    val logicOperation: LogicOperation? = null,
    val depthBoundsEnabled: Boolean = false,
    val minDepthBounds: Float = 0f,
    val maxDepthBounds: Float = 1f,
) {
    internal fun pack(): IntArray {
        val ds = depthStencil
        return (listOf(
                depthFormat?.vk ?: 0,
                sampleCount,
                primitiveType.vk,
                cullMode.ordinal,
                frontFace.ordinal,
                fillMode.ordinal,
                if (ds.depthWriteEnabled) 1 else 0,
                if (ds.depthTestEnabled) 1 else 0,
                ds.depthCompareFunction.ordinal,
                if (depthClampEnabled) 1 else 0,
                if (alphaToCoverageEnabled) 1 else 0,
                if (ds.frontFaceStencil != null || ds.backFaceStencil != null) 1 else 0,
                patchControlPoints,
                colorAttachments.size,
                vertexBuffers.size,
                vertexAttributes.size,
            ) +
                (ds.frontFaceStencil ?: StencilDescriptor()).pack() +
                (ds.backFaceStencil ?: StencilDescriptor()).pack() +
                listOf(
                    viewportCount,
                    viewMask,
                    if (sampleShadingEnabled) 1 else 0,
                    minSampleShading.toRawBits(),
                    if (alphaToOneEnabled) 1 else 0,
                    sampleMask.toInt(),
                    (sampleMask ushr 32).toInt(),
                    if (primitiveRestartEnabled) 1 else 0,
                    if (rasterizationDisabled) 1 else 0,
                    logicOperation?.ordinal ?: -1,
                    if (depthBoundsEnabled) 1 else 0,
                    minDepthBounds.toRawBits(),
                    maxDepthBounds.toRawBits(),
                ) +
                colorAttachments.flatMap { it.pack() } +
                vertexBuffers.flatMap { listOf(it.index, it.stride, it.stepFunction.ordinal) } +
                vertexAttributes.flatMap {
                    listOf(it.location, it.bufferIndex, it.format.vk, it.offset)
                })
            .toIntArray()
    }
}

data class Viewport(
    val x: Float,
    val y: Float,
    val width: Float,
    val height: Float,
    val minDepth: Float = 0f,
    val maxDepth: Float = 1f,
)

data class ScissorRect(val x: Int, val y: Int, val width: Int, val height: Int) {
    init {
        require(x >= 0 && y >= 0 && width >= 0 && height >= 0)
    }
}

data class Origin(val x: Int = 0, val y: Int = 0, val z: Int = 0) {
    init {
        require(x >= 0 && y >= 0 && z >= 0)
    }
}

data class TextureRegion(
    val origin: Origin = Origin(),
    val size: Size,
    val level: Int = 0,
    val slice: Int = 0,
    val sliceCount: Int = 1,
) {
    init {
        require(level >= 0 && slice >= 0 && sliceCount > 0)
    }

    internal fun pack() =
        intArrayOf(
            level,
            slice,
            sliceCount,
            origin.x,
            origin.y,
            origin.z,
            size.width,
            size.height,
            size.depth,
        )
}

/** Scalar 32-bit function constants keyed by the SPIR-V constant_id decoration. */
class FunctionConstants {
    private val values = sortedMapOf<Int, Int>()

    fun setInt(index: Int, value: Int) = apply {
        require(index >= 0)
        values[index] = value
    }

    fun setFloat(index: Int, value: Float) = setInt(index, value.toRawBits())

    fun setBoolean(index: Int, value: Boolean) = setInt(index, if (value) 1 else 0)

    internal fun pack() = values.flatMap { listOf(it.key, it.value) }.toIntArray()
}
