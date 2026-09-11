package dev.vulkano

import dev.vulkano.internal.Native

class CommandQueue
internal constructor(private val device: Device, val capabilities: CommandQueueCapabilities) :
    AutoCloseable {
    private var closed = false

    /** Queues with the same capabilities.index share one ordered physical Vulkan queue. */
    fun makeCommandBuffer(): CommandBuffer =
        device.access {
            check(!closed) { "Command queue is closed" }
            CommandBuffer(
                device,
                Native.createCommand(device.nativeHandle, capabilities.index),
                capabilities,
            )
        }

    override fun close() = synchronized(device) { closed = true }
}

class CommandBuffer
internal constructor(device: Device, id: Long, val queueCapabilities: CommandQueueCapabilities) :
    Resource(device, id) {
    private var encoder: CommandEncoder? = null
    val status: CommandBufferStatus
        get() = access { CommandBufferStatus.entries[Native.commandState(it)] }

    internal fun <T> encode(current: CommandEncoder, block: (Long) -> T): T = access {
        check(encoder === current) { "Encoder has ended or is not active" }
        block(it)
    }

    internal fun ended(current: CommandEncoder) {
        check(encoder === current)
        encoder = null
    }

    private fun recording() {
        check(status == CommandBufferStatus.RECORDING) { "Command buffer is not recording" }
        check(encoder == null) { "End the current encoder first" }
    }

    /** Waits once before this command starts; uses a binary external semaphore. */
    fun waitForExternalSemaphore(semaphore: ExternalSemaphore): Unit = access {
        recording()
        require(semaphore.device === device)
        Native.waitExternalSemaphore(it, semaphore.handle())
    }

    /** Signals once after all work completes. Submit before exporting its SyncFd. */
    fun signalExternalSemaphore(semaphore: ExternalSemaphore): Unit = access {
        recording()
        require(semaphore.device === device)
        Native.signalExternalSemaphore(it, semaphore.handle())
    }

    /**
     * Takes image ownership from its external producer. Wait on the producer's SyncFd semaphore in
     * this command, or ensure it has already finished. Set preserveContents=false for a fresh
     * image.
     */
    fun acquireExternalTexture(texture: Texture, preserveContents: Boolean = true): Unit = access {
        recording()
        require(texture.device === device)
        Native.acquireExternalTexture(it, texture.handle(), preserveContents)
    }

    /** Returns the image in GENERAL layout. Signal an external semaphore or wait for completion. */
    fun releaseExternalTexture(texture: Texture): Unit = access {
        recording()
        require(texture.device === device)
        Native.releaseExternalTexture(it, texture.handle())
    }

    fun makeComputeCommandEncoder(): ComputeCommandEncoder = access {
        recording()
        require(queueCapabilities.supportsCompute) { "This queue cannot execute compute commands" }
        ComputeCommandEncoder(this).also { encoder = it }
    }

    fun makeMachineLearningCommandEncoder(): MachineLearningCommandEncoder = access {
        recording()
        require(queueCapabilities.supportsMachineLearning) {
            "This queue cannot execute machine learning graphs"
        }
        MachineLearningCommandEncoder(this).also { encoder = it }
    }

    fun makeBlitCommandEncoder(): BlitCommandEncoder = access {
        recording()
        require(queueCapabilities.supportsTransfer) {
            "This queue cannot execute transfer commands"
        }
        BlitCommandEncoder(this).also { encoder = it }
    }

    fun makeAccelerationStructureCommandEncoder(): AccelerationStructureCommandEncoder = access {
        recording()
        require(queueCapabilities.supportsCompute) {
            "This queue cannot build acceleration structures"
        }
        AccelerationStructureCommandEncoder(this).also { encoder = it }
    }

    fun makeRayTracingCommandEncoder(): RayTracingCommandEncoder = access {
        recording()
        require(queueCapabilities.supportsCompute) { "This queue cannot trace rays" }
        RayTracingCommandEncoder(this).also { encoder = it }
    }

    fun makeRenderCommandEncoder(pass: RenderPassDescriptor): RenderCommandEncoder = access {
        recording()
        require(queueCapabilities.supportsRendering) { "This queue cannot render" }
        val colors = pass.colorAttachments
        val d = pass.depthAttachment
        val rate = pass.rasterizationRateMap
        require(
            colors.all {
                it.texture.device === device &&
                    (it.resolveTexture == null || it.resolveTexture.device === device)
            } &&
                (d == null ||
                    (d.texture.device === device &&
                        (d.resolveTexture == null || d.resolveTexture.device === device))) &&
                (rate == null || rate.texture.device === device)
        )
        val handles =
            (listOf(
                    d?.texture?.handle() ?: 0L,
                    d?.resolveTexture?.handle() ?: 0L,
                    rate?.texture?.handle() ?: 0L,
                ) +
                    colors.flatMap {
                        listOf(it.texture.handle(), it.resolveTexture?.handle() ?: 0L)
                    })
                .toLongArray()
        val actions =
            (listOf(
                    d?.loadAction?.vk ?: 1,
                    d?.storeAction?.vk ?: 1,
                    d?.level ?: 0,
                    d?.slice ?: 0,
                    d?.clearStencil ?: 0,
                    pass.viewMask,
                    pass.renderTargetArrayLength,
                    d?.depthResolveMode?.vk ?: 1,
                    d?.stencilResolveMode?.vk ?: 1,
                    d?.resolveLevel ?: 0,
                    d?.resolveSlice ?: 0,
                    rate?.texelSize?.width ?: 0,
                    rate?.texelSize?.height ?: 0,
                    rate?.level ?: 0,
                    rate?.slice ?: 0,
                    (d?.stencilLoadAction ?: d?.loadAction)?.vk ?: 1,
                    (d?.stencilStoreAction ?: d?.storeAction)?.vk ?: 1,
                    if (d?.depthReadOnly == true) 1 else 0,
                    if (d?.stencilReadOnly == true) 1 else 0,
                ) +
                    colors.flatMap {
                        listOf(
                            it.loadAction.vk,
                            it.storeAction.vk,
                            it.level,
                            it.slice,
                            it.resolveLevel,
                            it.resolveSlice,
                        )
                    })
                .toIntArray()
        val clear =
            (listOf(d?.clearDepth ?: 1f) +
                    colors.flatMap {
                        if (
                            it.texture.descriptor.pixelFormat.name.endsWith("_UINT") ||
                                it.texture.descriptor.pixelFormat.name.endsWith("_SINT")
                        ) {
                            val c = it.clearIntegerColor ?: ClearIntegerColor()
                            listOf(c.red, c.green, c.blue, c.alpha).map { Float.fromBits(it) }
                        } else
                            listOf(
                                it.clearColor.red,
                                it.clearColor.green,
                                it.clearColor.blue,
                                it.clearColor.alpha,
                            )
                    })
                .toFloatArray()
        RenderCommandEncoder(
                this,
                Native.beginRenderAdvanced(
                    it,
                    handles,
                    actions,
                    clear,
                    pass.subpassLayout?.pack() ?: intArrayOf(),
                    pass.tileShading?.pack() ?: intArrayOf(),
                ),
            )
            .also { encoder = it }
    }

    fun compute(block: ComputeCommandEncoder.() -> Unit) = scope(makeComputeCommandEncoder(), block)

    fun machineLearning(block: MachineLearningCommandEncoder.() -> Unit) =
        scope(makeMachineLearningCommandEncoder(), block)

    fun blit(block: BlitCommandEncoder.() -> Unit) = scope(makeBlitCommandEncoder(), block)

    fun render(pass: RenderPassDescriptor, block: RenderCommandEncoder.() -> Unit = {}) =
        scope(makeRenderCommandEncoder(pass), block)

    fun accelerationStructure(block: AccelerationStructureCommandEncoder.() -> Unit) =
        scope(makeAccelerationStructureCommandEncoder(), block)

    fun rayTracing(block: RayTracingCommandEncoder.() -> Unit) =
        scope(makeRayTracingCommandEncoder(), block)

    private fun <T : CommandEncoder> scope(current: T, block: T.() -> Unit) {
        try {
            current.block()
            current.endEncoding()
        } catch (error: Throwable) {
            close()
            throw error
        }
    }

    fun present(drawable: Drawable): Unit = access {
        recording()
        require(drawable.device === device)
        Native.present(it, drawable.handle())
    }

    /** Submits once, asynchronously. Resources remain alive through completion. */
    fun commit(): Unit = access {
        recording()
        Native.commit(it)
    }

    /** Blocking worker-thread wait. Releases device locks between completion polls. */
    fun waitUntilCompleted(timeoutNanos: Long = Long.MAX_VALUE): Boolean {
        require(timeoutNanos >= 0)
        val start = System.nanoTime()
        do {
            if (access { Native.waitCommand(it, 0) }) return true
            if (timeoutNanos == 0L || System.nanoTime() - start >= timeoutNanos) return false
            Thread.sleep(1)
        } while (true)
    }

    /** Waits at submission start; encode this before GPU operations. */
    fun waitForEvent(event: SharedEvent, value: Long): Unit = access {
        recording()
        require(event.device === device && value >= 0)
        Native.waitEvent(it, event.handle(), value)
    }

    /** Signals once all operations in this command buffer complete. */
    fun signalEventOnCompletion(event: SharedEvent, value: Long): Unit = access {
        recording()
        require(event.device === device && value >= 0)
        Native.signalCommandEvent(it, event.handle(), value)
    }

    fun sampleCounters(buffer: CounterSampleBuffer, index: Int): Unit = access {
        recording()
        require(buffer.device === device && index in 0 until buffer.count)
        Native.sampleCounter(it, buffer.handle(), index)
    }

    override fun close() {
        val pending =
            synchronized(device) {
                !device.closed && !isClosed && status == CommandBufferStatus.SUBMITTED
            }
        if (pending) waitUntilCompleted()
        synchronized(device) {
            encoder?.abort()
            encoder = null
            super.close()
        }
    }
}

abstract class CommandEncoder internal constructor(protected val commandBuffer: CommandBuffer) :
    AutoCloseable {
    private var ended = false

    protected fun <T> encode(block: (Long) -> T): T = commandBuffer.encode(this, block)

    protected open fun finish() {}

    internal open fun abort() {
        ended = true
    }

    fun endEncoding() =
        synchronized(commandBuffer.device) {
            if (!ended) {
                encode { finish() }
                commandBuffer.ended(this)
                ended = true
            }
        }

    final override fun close() = endEncoding()
}

private class BoundResource(
    val index: Int,
    val buffer: Buffer? = null,
    val offset: Long = 0,
    val length: Long = 0,
    val texture: Texture? = null,
    val sampler: Sampler? = null,
    val arrayElement: Int = 0,
    val acceleration: AccelerationStructure? = null,
    val texel: TextureBuffer? = null,
    val tensor: TensorView? = null,
) {
    fun pack() =
        listOf(
            index.toLong(),
            buffer?.handle() ?: 0,
            offset,
            length,
            texture?.handle() ?: 0,
            sampler?.handle() ?: 0,
            arrayElement.toLong(),
            acceleration?.handle() ?: 0,
            texel?.handle() ?: 0,
            tensor?.handle() ?: 0,
        )
}

abstract class ShaderCommandEncoder internal constructor(command: CommandBuffer) :
    CommandEncoder(command) {
    private val bindings =
        sortedMapOf<Pair<Int, Int>, BoundResource>(
            compareBy<Pair<Int, Int>> { it.first }.thenBy { it.second }
        )
    private var constants = byteArrayOf()

    fun resetBindings(): Unit = encode {
        bindings.clear()
        constants = byteArrayOf()
    }

    fun setBuffer(
        buffer: Buffer,
        index: Int,
        offset: Long = 0,
        length: Long = buffer.length - offset,
        arrayElement: Int = 0,
    ): Unit = encode {
        require(
            buffer.device === commandBuffer.device &&
                index >= 0 &&
                offset >= 0 &&
                length > 0 &&
                offset <= buffer.length &&
                length <= buffer.length - offset
        )
        buffer.handle()
        require(arrayElement >= 0)
        bindings[index to arrayElement] =
            BoundResource(index, buffer, offset, length, arrayElement = arrayElement)
    }

    fun setTexture(
        texture: Texture,
        index: Int,
        sampler: Sampler? = null,
        arrayElement: Int = 0,
    ): Unit = encode {
        require(
            texture.device === commandBuffer.device &&
                index >= 0 &&
                (sampler == null || sampler.device === commandBuffer.device)
        )
        texture.handle()
        sampler?.handle()
        require(arrayElement >= 0)
        bindings[index to arrayElement] =
            BoundResource(index, texture = texture, sampler = sampler, arrayElement = arrayElement)
    }

    fun setSampler(sampler: Sampler, index: Int, arrayElement: Int = 0): Unit = encode {
        require(sampler.device === commandBuffer.device && index >= 0 && arrayElement >= 0)
        sampler.handle()
        bindings[index to arrayElement] =
            BoundResource(index, sampler = sampler, arrayElement = arrayElement)
    }

    fun setTextureBuffer(texture: TextureBuffer, index: Int, arrayElement: Int = 0): Unit = encode {
        require(texture.device === commandBuffer.device && index >= 0 && arrayElement >= 0)
        texture.handle()
        bindings[index to arrayElement] =
            BoundResource(index, texel = texture, arrayElement = arrayElement)
    }

    fun setTensor(tensor: TensorView, index: Int, arrayElement: Int = 0): Unit = encode {
        require(tensor.device === commandBuffer.device && index >= 0 && arrayElement >= 0)
        tensor.handle()
        bindings[index to arrayElement] =
            BoundResource(index, tensor = tensor, arrayElement = arrayElement)
    }

    fun setAccelerationStructure(
        structure: AccelerationStructure,
        index: Int,
        arrayElement: Int = 0,
    ): Unit = encode {
        require(structure.device === commandBuffer.device && index >= 0 && arrayElement >= 0)
        structure.handle()
        bindings[index to arrayElement] =
            BoundResource(index, arrayElement = arrayElement, acceleration = structure)
    }

    /** Retain buffers accessed through GPU addresses until this command completes. */
    fun useResource(buffer: Buffer): Unit = encode {
        require(buffer.device === commandBuffer.device)
        Native.retainBuffer(it, buffer.handle())
    }

    fun setBytes(bytes: ByteArray): Unit = encode { constants = bytes.copyOf() }

    protected fun bindingData(): LongArray = bindings.values.flatMap { it.pack() }.toLongArray()

    protected fun constantData(): ByteArray = constants
}

class ComputeCommandEncoder internal constructor(command: CommandBuffer) :
    ShaderCommandEncoder(command) {
    private var pipeline: ComputePipelineState? = null

    fun setComputePipelineState(state: ComputePipelineState): Unit = encode {
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
            0,
        )
    }

    fun dispatchThreadgroups(groups: Size): Unit = encode {
        val state = checkNotNull(pipeline) { "Set a compute pipeline first" }
        Native.dispatch(it, state.handle(), bindingData(), constantData(), groups.array())
    }

    fun dispatchThreadgroups(indirectBuffer: Buffer, offset: Long = 0): Unit = encode {
        require(indirectBuffer.device === commandBuffer.device && offset >= 0)
        Native.dispatchIndirect(
            it,
            checkNotNull(pipeline).handle(),
            bindingData(),
            constantData(),
            indirectBuffer.handle(),
            offset,
        )
    }

    /** Rounds up to whole workgroups. The shader MUST bounds-check excess invocations. */
    fun dispatchThreads(threads: Size): Unit = encode {
        val local = checkNotNull(pipeline) { "Set a compute pipeline first" }.threadgroupSize
        fun groups(n: Int, size: Int) = ((n.toLong() + size - 1) / size).toInt()
        dispatchThreadgroups(
            Size(
                groups(threads.width, local.width),
                groups(threads.height, local.height),
                groups(threads.depth, local.depth),
            )
        )
    }
}

class RenderCommandEncoder
internal constructor(command: CommandBuffer, private var nativeEncoder: Long) :
    ShaderCommandEncoder(command) {
    private var pipeline: RenderPipelineState? = null
    private var tilePipeline: ComputePipelineState? = null

    fun beginPerTileExecution(): Unit = encode { Native.tileControl(nativeEncoder, 3) }

    fun endPerTileExecution(): Unit = encode { Native.tileControl(nativeEncoder, 4) }

    /** Commands in this block execute independently for every tile; tile order is unspecified. */
    fun perTile(block: RenderCommandEncoder.() -> Unit) {
        beginPerTileExecution()
        try {
            block()
        } finally {
            endPerTileExecution()
        }
    }

    /** Orders attachment, compute and indirect accesses within each tile. */
    fun tileMemoryBarrier(): Unit = encode { Native.tileControl(nativeEncoder, 5) }

    fun setTileComputePipelineState(state: ComputePipelineState): Unit = encode {
        require(state.device === commandBuffer.device)
        state.handle()
        tilePipeline = state
    }

    fun dispatchTileThreadgroups(groups: Size): Unit = encode {
        Native.dispatchTile(
            nativeEncoder,
            checkNotNull(tilePipeline) { "Set a tile compute pipeline first" }.handle(),
            bindingData(),
            constantData(),
            intArrayOf(groups.width, groups.height, groups.depth),
            false,
            0,
            0,
        )
    }

    fun dispatchTileThreadgroups(indirectBuffer: Buffer, offset: Long = 0): Unit = encode {
        require(indirectBuffer.device === commandBuffer.device && offset >= 0)
        Native.dispatchTile(
            nativeEncoder,
            checkNotNull(tilePipeline) { "Set a tile compute pipeline first" }.handle(),
            bindingData(),
            constantData(),
            intArrayOf(1, 1, 1),
            false,
            indirectBuffer.handle(),
            offset,
        )
    }

    /** Executes TileShadingRateQCOM over the tile area; Vulkan selects the workgroup dimensions. */
    fun dispatchTile(): Unit = encode {
        Native.dispatchTile(
            nativeEncoder,
            checkNotNull(tilePipeline) { "Set a tile compute pipeline first" }.handle(),
            bindingData(),
            constantData(),
            intArrayOf(1, 1, 1),
            true,
            0,
            0,
        )
    }

    fun setRenderPipelineState(state: RenderPipelineState): Unit = encode {
        require(state.device === commandBuffer.device)
        state.handle()
        pipeline = state
    }

    /** Advance within the same render pass. Binding and pipeline state must be set again. */
    fun nextSubpass(): Unit = encode {
        Native.nextSubpass(nativeEncoder)
        pipeline = null
        tilePipeline = null
        resetBindings()
        vertexBuffers.clear()
        visibility = null
    }

    private val vertexBuffers = sortedMapOf<Int, Pair<Buffer, Long>>()
    private var viewports: List<Viewport> = emptyList()
    private var scissors: List<ScissorRect> = emptyList()
    private var blendColor = ClearColor(0f, 0f, 0f, 0f)
    private var depthBias = 0f
    private var slopeBias = 0f
    private var biasClamp = 0f
    private var lineWidth = 1f
    private var stencilReference = 0
    private var visibility: CounterSampleBuffer? = null
    private var visibilityIndex = 0

    fun setVisibilityResult(buffer: CounterSampleBuffer?, index: Int = 0): Unit = encode {
        require(
            buffer == null ||
                (buffer.device === commandBuffer.device &&
                    !buffer.isTimestamp &&
                    index in 0 until buffer.count)
        )
        visibility = buffer
        visibilityIndex = index
    }

    fun setVertexBuffer(buffer: Buffer, index: Int, offset: Long = 0): Unit = encode {
        require(buffer.device === commandBuffer.device && index >= 0 && offset >= 0)
        buffer.handle()
        vertexBuffers[index] = buffer to offset
    }

    fun setViewport(value: Viewport) = setViewports(listOf(value))

    fun setScissorRect(value: ScissorRect) = setScissorRects(listOf(value))

    fun setViewports(values: List<Viewport>): Unit = encode { viewports = values.toList() }

    fun setScissorRects(values: List<ScissorRect>): Unit = encode { scissors = values.toList() }

    fun setBlendColor(value: ClearColor): Unit = encode { blendColor = value }

    fun setDepthBias(bias: Float, slopeScale: Float = 0f, clamp: Float = 0f): Unit = encode {
        depthBias = bias
        slopeBias = slopeScale
        biasClamp = clamp
    }

    fun setStencilReferenceValue(value: Int): Unit = encode { stencilReference = value }

    fun setLineWidth(value: Float): Unit = encode { lineWidth = value }

    private fun draw(
        count: Int,
        instances: Int,
        first: Int,
        firstInstance: Int,
        baseVertex: Int = 0,
        indexBuffer: Buffer? = null,
        indexType: IndexType = IndexType.UINT16,
        indexOffset: Long = 0,
        indirect: Buffer? = null,
        indirectOffset: Long = 0,
        drawCount: Int = 1,
        stride: Int = 0,
        meshGroups: Size? = null,
        countBuffer: Buffer? = null,
        countOffset: Long = 0,
        generated: LongArray = longArrayOf(),
    ): Unit = encode {
        require(
            count > 0 &&
                instances > 0 &&
                first >= 0 &&
                firstInstance >= 0 &&
                indexOffset >= 0 &&
                indirectOffset >= 0
        )
        require(indexBuffer == null || indexBuffer.device === commandBuffer.device)
        require(indirect == null || indirect.device === commandBuffer.device)
        require(
            countBuffer == null || (indirect != null && countBuffer.device === commandBuffer.device)
        )
        require(countOffset >= 0 && drawCount >= 0 && (drawCount > 0 || countBuffer != null))
        val v = viewports.firstOrNull() ?: Viewport(0f, 0f, 1f, 1f)
        val s = scissors.firstOrNull()
        Native.drawAdvanced(
            nativeEncoder,
            if (generated.isEmpty())
                checkNotNull(pipeline) { "Set a render pipeline first" }.handle()
            else 0L,
            bindingData(),
            constantData(),
            intArrayOf(
                count,
                instances,
                first,
                firstInstance,
                baseVertex,
                indexType.vk,
                drawCount,
                stride,
                stencilReference,
                if (viewports.isNotEmpty()) 1 else 0,
                s?.x ?: -1,
                s?.y ?: 0,
                s?.width ?: 0,
                s?.height ?: 0,
                visibilityIndex,
                meshGroups?.width ?: 0,
                meshGroups?.height ?: 0,
                meshGroups?.depth ?: 0,
                viewports.size,
                scissors.size,
            ) + scissors.flatMap { listOf(it.x, it.y, it.width, it.height) }.toIntArray(),
            (listOf(
                    indexBuffer?.handle() ?: 0,
                    indexOffset,
                    indirect?.handle() ?: 0,
                    indirectOffset,
                    visibility?.handle() ?: 0,
                    countBuffer?.handle() ?: 0,
                    countOffset,
                ) +
                    vertexBuffers.flatMap { (index, value) ->
                        listOf(index.toLong(), value.first.handle(), value.second)
                    })
                .toLongArray(),
            floatArrayOf(
                v.x,
                v.y,
                v.width,
                v.height,
                v.minDepth,
                v.maxDepth,
                blendColor.red,
                blendColor.green,
                blendColor.blue,
                blendColor.alpha,
                depthBias,
                slopeBias,
                biasClamp,
                lineWidth,
            ) +
                viewports
                    .flatMap { listOf(it.x, it.y, it.width, it.height, it.minDepth, it.maxDepth) }
                    .toFloatArray(),
            generated,
        )
    }

    /** Uses this encoder's descriptors and dynamic state; the layout supplies the pipeline. */
    fun executeCommands(
        layout: IndirectCommandLayout,
        indirectBuffer: Buffer,
        maxSequenceCount: Int,
        offset: Long = 0,
        countBuffer: Buffer? = null,
        countOffset: Long = 0,
        maxDrawCount: Int = 1,
        indexBuffer: Buffer? = null,
        indexType: IndexType = IndexType.UINT16,
        indexBufferOffset: Long = 0,
    ): Unit = encode {
        require(layout.device === commandBuffer.device)
        draw(
            1,
            1,
            0,
            0,
            indexBuffer = indexBuffer,
            indexType = indexType,
            indexOffset = indexBufferOffset,
            generated =
                layout.arguments(
                    indirectBuffer,
                    maxSequenceCount,
                    offset,
                    countBuffer,
                    countOffset,
                    maxDrawCount,
                ),
        )
    }

    fun drawPrimitives(
        vertexCount: Int,
        instanceCount: Int = 1,
        firstVertex: Int = 0,
        firstInstance: Int = 0,
    ) = draw(vertexCount, instanceCount, firstVertex, firstInstance)

    fun drawIndexedPrimitives(
        indexBuffer: Buffer,
        indexCount: Int,
        indexType: IndexType = IndexType.UINT16,
        indexBufferOffset: Long = 0,
        firstIndex: Int = 0,
        baseVertex: Int = 0,
        instanceCount: Int = 1,
        firstInstance: Int = 0,
    ) =
        draw(
            indexCount,
            instanceCount,
            firstIndex,
            firstInstance,
            baseVertex,
            indexBuffer,
            indexType,
            indexBufferOffset,
        )

    /** GPU-generated counts and indices must obey the device limits and bound buffer ranges. */
    fun drawPrimitives(
        indirectBuffer: Buffer,
        offset: Long = 0,
        drawCount: Int = 1,
        stride: Int = 16,
    ) =
        draw(
            1,
            1,
            0,
            0,
            indirect = indirectBuffer,
            indirectOffset = offset,
            drawCount = drawCount,
            stride = stride,
        )

    fun drawIndexedPrimitives(
        indexBuffer: Buffer,
        indirectBuffer: Buffer,
        indexType: IndexType = IndexType.UINT16,
        indexBufferOffset: Long = 0,
        indirectOffset: Long = 0,
        drawCount: Int = 1,
        stride: Int = 20,
    ) =
        draw(
            1,
            1,
            0,
            0,
            indexBuffer = indexBuffer,
            indexType = indexType,
            indexOffset = indexBufferOffset,
            indirect = indirectBuffer,
            indirectOffset = indirectOffset,
            drawCount = drawCount,
            stride = stride,
        )

    /** Draws min(GPU count, maxDrawCount) commands. GPU count must obey maxDrawIndirectCount. */
    fun drawPrimitives(
        indirectBuffer: Buffer,
        countBuffer: Buffer,
        maxDrawCount: Int,
        indirectOffset: Long = 0,
        countOffset: Long = 0,
        stride: Int = 16,
    ) =
        draw(
            1,
            1,
            0,
            0,
            indirect = indirectBuffer,
            indirectOffset = indirectOffset,
            drawCount = maxDrawCount,
            stride = stride,
            countBuffer = countBuffer,
            countOffset = countOffset,
        )

    fun drawIndexedPrimitives(
        indexBuffer: Buffer,
        indirectBuffer: Buffer,
        countBuffer: Buffer,
        maxDrawCount: Int,
        indexType: IndexType = IndexType.UINT16,
        indexBufferOffset: Long = 0,
        indirectOffset: Long = 0,
        countOffset: Long = 0,
        stride: Int = 20,
    ) =
        draw(
            1,
            1,
            0,
            0,
            indexBuffer = indexBuffer,
            indexType = indexType,
            indexOffset = indexBufferOffset,
            indirect = indirectBuffer,
            indirectOffset = indirectOffset,
            drawCount = maxDrawCount,
            stride = stride,
            countBuffer = countBuffer,
            countOffset = countOffset,
        )

    fun drawMeshThreadgroups(
        indirectBuffer: Buffer,
        countBuffer: Buffer,
        maxDrawCount: Int,
        indirectOffset: Long = 0,
        countOffset: Long = 0,
        stride: Int = 12,
    ) =
        draw(
            1,
            1,
            0,
            0,
            indirect = indirectBuffer,
            indirectOffset = indirectOffset,
            drawCount = maxDrawCount,
            stride = stride,
            countBuffer = countBuffer,
            countOffset = countOffset,
            meshGroups = Size(1),
        )

    fun drawMeshThreadgroups(groups: Size) = draw(1, 1, 0, 0, meshGroups = groups)

    fun drawMeshThreadgroups(
        indirectBuffer: Buffer,
        offset: Long = 0,
        drawCount: Int = 1,
        stride: Int = 12,
    ) =
        draw(
            1,
            1,
            0,
            0,
            indirect = indirectBuffer,
            indirectOffset = offset,
            drawCount = drawCount,
            stride = stride,
            meshGroups = Size(1),
        )

    override fun finish() {
        Native.endRender(nativeEncoder)
        nativeEncoder = 0
    }

    internal override fun abort() {
        if (nativeEncoder != 0L) {
            Native.close(nativeEncoder)
            nativeEncoder = 0
        }
        super.abort()
    }
}

class BlitCommandEncoder internal constructor(command: CommandBuffer) : CommandEncoder(command) {
    /** Copies the complete tensor; dimensions and scalar byte sizes must match. */
    fun copy(source: TensorResource, destination: TensorResource): Unit = encode {
        require(
            source.device === commandBuffer.device && destination.device === commandBuffer.device
        )
        Native.copyTensor(it, source.handle(), destination.handle())
    }

    /** Orders uses of overlapping placed resources and discards both textures' prior contents. */
    fun aliasResources(before: Resource, after: Resource): Unit = encode {
        require(before.device === commandBuffer.device && after.device === commandBuffer.device)
        Native.aliasResources(it, before.handle(), after.handle())
    }

    fun generateMipmaps(texture: Texture, filter: MipFilter = MipFilter.LINEAR): Unit = encode {
        require(texture.device === commandBuffer.device)
        Native.generateMipmaps(it, texture.handle(), filter.ordinal)
    }

    fun fill(
        buffer: Buffer,
        value: Byte,
        offset: Long = 0,
        length: Long = buffer.length - offset,
    ): Unit = encode {
        require(buffer.device === commandBuffer.device)
        Native.fillBuffer(it, buffer.handle(), offset, length, (value.toInt() and 255) * 0x01010101)
    }

    fun copy(
        source: Buffer,
        destination: Texture,
        region: TextureRegion,
        sourceOffset: Long = 0,
        rowLength: Int = 0,
        imageHeight: Int = 0,
    ): Unit = encode {
        require(
            source.device === commandBuffer.device && destination.device === commandBuffer.device
        )
        Native.copyTextureRegion(
            it,
            source.handle(),
            destination.handle(),
            sourceOffset,
            true,
            region.pack(),
            rowLength,
            imageHeight,
        )
    }

    fun copy(
        source: Texture,
        destination: Buffer,
        region: TextureRegion,
        destinationOffset: Long = 0,
        rowLength: Int = 0,
        imageHeight: Int = 0,
    ): Unit = encode {
        require(
            source.device === commandBuffer.device && destination.device === commandBuffer.device
        )
        Native.copyTextureRegion(
            it,
            destination.handle(),
            source.handle(),
            destinationOffset,
            false,
            region.pack(),
            rowLength,
            imageHeight,
        )
    }

    fun copy(
        source: Texture,
        destination: Texture,
        sourceRegion: TextureRegion = TextureRegion(size = source.sizeAtLevel()),
        destinationRegion: TextureRegion = TextureRegion(size = sourceRegion.size),
    ): Unit = encode {
        require(
            source.device === commandBuffer.device && destination.device === commandBuffer.device
        )
        Native.copyImages(
            it,
            source.handle(),
            destination.handle(),
            sourceRegion.pack(),
            destinationRegion.pack(),
        )
    }

    fun copy(
        source: Buffer,
        destination: Buffer,
        length: Long = source.length,
        sourceOffset: Long = 0,
        destinationOffset: Long = 0,
    ): Unit = encode {
        require(
            source.device === commandBuffer.device && destination.device === commandBuffer.device
        )
        Native.copyBuffers(
            it,
            source.handle(),
            destination.handle(),
            sourceOffset,
            destinationOffset,
            length,
        )
    }

    /** Copies level zero, slice zero with tightly packed rows. */
    fun copy(source: Buffer, destination: Texture, sourceOffset: Long = 0): Unit = encode {
        require(
            source.device === commandBuffer.device && destination.device === commandBuffer.device
        )
        Native.copyTexture(it, source.handle(), destination.handle(), sourceOffset, true)
    }

    fun copy(source: Texture, destination: Buffer, destinationOffset: Long = 0): Unit = encode {
        require(
            source.device === commandBuffer.device && destination.device === commandBuffer.device
        )
        Native.copyTexture(it, destination.handle(), source.handle(), destinationOffset, false)
    }
}
