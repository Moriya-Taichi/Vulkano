package dev.vulkano

import dev.vulkano.internal.Native

/** An instruction set and version reported by a GPU graph processing engine. */
data class MachineLearningOperationSet(val name: String, val version: Long)

data class MachineLearningCapabilities(
    val supportsShaderCompilation: Boolean,
    val supportsFunctionConstants: Boolean,
    val supportsCachedPipelines: Boolean,
)

enum class MachineLearningPipelineProperty {
    CREATION_LOG,
    IDENTIFIER,
}

val Device.machineLearningCapabilities: MachineLearningCapabilities?
    get() = access {
        if (Feature.MACHINE_LEARNING_GRAPH !in capabilities.availableFeatures) null
        else
            Native.graphCapabilities(nativeHandle).let {
                MachineLearningCapabilities(it[0] != 0, it[1] != 0, it[2] != 0)
            }
    }

/** GPU engines only. Foreign NPU engines require a separate external-memory contract. */
fun Device.machineLearningOperationSets(
    queueIndex: Int = defaultMachineLearningQueue()
): List<MachineLearningOperationSet> = access {
    Native.graphOperations(nativeHandle, queueIndex).map {
        val separator = it.indexOf('\n')
        MachineLearningOperationSet(
            it.substring(separator + 1),
            it.substring(0, separator).toLong(),
        )
    }
}

/** Every element of a descriptor array has the same shape and layout. */
data class MachineLearningTensorBinding(
    val index: Int,
    val descriptor: TensorResourceDescriptor,
    val arrayLength: Int = 1,
    val set: Int = 0,
) {
    init {
        require(index >= 0 && arrayLength > 0 && set >= 0)
        require(TensorUsage.MACHINE_LEARNING in descriptor.usage)
    }
}

/** Immutable weights or other graph constants; data uses the descriptor's linear layout. */
class MachineLearningConstant(
    val id: Int,
    val descriptor: TensorResourceDescriptor,
    data: ByteArray,
) {
    internal val bytes = data.copyOf()

    init {
        require(id >= 0 && descriptor.layout == TensorLayout.LINEAR)
        require(TensorUsage.MACHINE_LEARNING in descriptor.usage)
        require(bytes.size.toLong() == descriptor.byteLength)
    }
}

class MachineLearningPipelineState
internal constructor(
    device: Device,
    id: Long,
    val queueFamilyIndex: Int,
    val tensorBindings: List<MachineLearningTensorBinding>,
) : Resource(device, id) {
    val availableProperties: Set<MachineLearningPipelineProperty>
        get() = access {
            Native.graphAvailableProperties(it)
                .toList()
                .mapNotNull { value -> MachineLearningPipelineProperty.entries.getOrNull(value) }
                .toSet()
        }

    val creationLog: String
        get() = access { Native.graphProperty(it, 0).toString(Charsets.UTF_8).trimEnd('\u0000') }

    /** Save together with Device.serializePipelineCache(); identifiers are driver-specific. */
    val identifier: ByteArray
        get() = access { Native.graphProperty(it, 1) }
}

internal fun Device.defaultMachineLearningQueue(): Int = access {
    commandQueues.firstOrNull { it.supportsMachineLearning }?.index
        ?: throw IllegalStateException(
            "Enable MACHINE_LEARNING_GRAPH on a device with a GPU graph queue"
        )
}

internal fun TensorResourceDescriptor.graphDescription(): LongArray =
    (listOf(
            dataType.tensorFormat.toLong(),
            layout.ordinal.toLong(),
            usageBits,
            dimensions.size.toLong(),
        ) + dimensions + (byteStrides ?: emptyList()))
        .toLongArray()

/** Compiles an OpGraphEntryPointARM from offline graph SPIR-V. */
fun Device.makeMachineLearningPipelineState(
    function: ShaderFunction,
    tensorBindings: List<MachineLearningTensorBinding>,
    constants: List<MachineLearningConstant> = emptyList(),
    queueIndex: Int = defaultMachineLearningQueue(),
    compilerOptions: String = "",
    optimize: Boolean = true,
): MachineLearningPipelineState = access {
    require(function.library.device === this)
    checkNotNull(
        createMachineLearningPipeline(
            function,
            byteArrayOf(),
            tensorBindings,
            constants,
            queueIndex,
            compilerOptions,
            optimize,
        )
    )
}

/** Load the matching pipeline cache first. Returns null when the driver reports a cache miss. */
fun Device.restoreMachineLearningPipelineState(
    identifier: ByteArray,
    tensorBindings: List<MachineLearningTensorBinding>,
    queueIndex: Int = defaultMachineLearningQueue(),
): MachineLearningPipelineState? = access {
    require(identifier.isNotEmpty())
    createMachineLearningPipeline(
        null,
        identifier.copyOf(),
        tensorBindings,
        emptyList(),
        queueIndex,
        "",
        true,
    )
}

private fun Device.createMachineLearningPipeline(
    function: ShaderFunction?,
    identifier: ByteArray,
    tensorBindings: List<MachineLearningTensorBinding>,
    constants: List<MachineLearningConstant>,
    queueIndex: Int,
    compilerOptions: String,
    optimize: Boolean,
): MachineLearningPipelineState? {
    require(
        queueIndex in commandQueues.indices && commandQueues[queueIndex].supportsMachineLearning
    )
    require(tensorBindings.map { it.set to it.index }.toSet().size == tensorBindings.size)
    require(constants.map { it.id }.toSet().size == constants.size)
    require('\u0000' !in compilerOptions)
    val bindings = tensorBindings.toList()
    val id =
        Native.createGraphPipeline(
            nativeHandle,
            queueIndex,
            function?.library?.code ?: byteArrayOf(),
            function?.name ?: "main",
            function?.constants ?: intArrayOf(),
            bindings.flatMap { listOf(it.index, it.arrayLength, it.set) }.toIntArray(),
            bindings.map { it.descriptor.graphDescription() }.toTypedArray(),
            constants.map { it.id }.toIntArray(),
            constants.map { it.descriptor.graphDescription() }.toTypedArray(),
            constants.map { it.bytes }.toTypedArray(),
            compilerOptions,
            optimize,
            identifier,
        )
    if (id == 0L) return null
    return MachineLearningPipelineState(this, id, commandQueues[queueIndex].familyIndex, bindings)
}

class MachineLearningCommandEncoder internal constructor(command: CommandBuffer) :
    ShaderCommandEncoder(command) {
    private var pipeline: MachineLearningPipelineState? = null
    fun setMachineLearningPipelineState(state: MachineLearningPipelineState): Unit = encode {
        require(
            state.device === commandBuffer.device &&
                state.queueFamilyIndex == commandBuffer.queueCapabilities.familyIndex
        )
        state.handle()
        pipeline = state
    }

    /**
     * Encodes one graph invocation; subsequent invocations observe earlier writes on this queue.
     */
    fun dispatch(): Unit = encode { command ->
        val state = checkNotNull(pipeline) { "Set a machine learning pipeline first" }
        Native.dispatchGraph(
            command,
            state.handle(),
            bindingData(),
        )
    }
}
