package dev.vulkano

import dev.vulkano.internal.Native

class CommandQueue internal constructor(private val device: Device) : AutoCloseable {
    private var closed = false
    /** Logical queues share one ordered Vulkan graphics/compute queue. */
    fun makeCommandBuffer(): CommandBuffer = device.access {
        check(!closed) { "Command queue is closed" }
        CommandBuffer(device, Native.createCommand(device.nativeHandle))
    }
    override fun close() = synchronized(device) { closed = true }
}

class CommandBuffer internal constructor(device: Device, id: Long) : Resource(device, id) {
    private var encoder: CommandEncoder? = null
    val status: CommandBufferStatus get() = access { CommandBufferStatus.entries[Native.commandState(it)] }
    internal fun <T> encode(current: CommandEncoder, block: (Long) -> T): T = access {
        check(encoder === current) { "Encoder has ended or is not active" }
        block(it)
    }
    internal fun ended(current: CommandEncoder) { check(encoder === current); encoder = null }
    private fun recording() {
        check(status == CommandBufferStatus.RECORDING) { "Command buffer is not recording" }
        check(encoder == null) { "End the current encoder first" }
    }
    fun makeComputeCommandEncoder(): ComputeCommandEncoder = access {
        recording(); ComputeCommandEncoder(this).also { encoder = it }
    }
    fun makeBlitCommandEncoder(): BlitCommandEncoder = access {
        recording(); BlitCommandEncoder(this).also { encoder = it }
    }
    fun makeRenderCommandEncoder(pass: RenderPassDescriptor): RenderCommandEncoder = access {
        recording()
        val c = pass.colorAttachment
        val d = pass.depthAttachment
        require(c.texture.device === device && (d == null || d.texture.device === device))
        val clear = c.clearColor
        val id = Native.beginRender(it, c.texture.handle(), d?.texture?.handle() ?: 0,
            intArrayOf(c.loadAction.vk, c.storeAction.vk, d?.loadAction?.vk ?: 1, d?.storeAction?.vk ?: 1),
            floatArrayOf(clear.red, clear.green, clear.blue, clear.alpha, d?.clearDepth ?: 1f))
        RenderCommandEncoder(this, id).also { encoder = it }
    }
    fun compute(block: ComputeCommandEncoder.() -> Unit) = scope(makeComputeCommandEncoder(), block)
    fun blit(block: BlitCommandEncoder.() -> Unit) = scope(makeBlitCommandEncoder(), block)
    fun render(pass: RenderPassDescriptor, block: RenderCommandEncoder.() -> Unit = {}) = scope(makeRenderCommandEncoder(pass), block)
    private fun <T : CommandEncoder> scope(current: T, block: T.() -> Unit) {
        try { current.block(); current.endEncoding() }
        catch (error: Throwable) { close(); throw error }
    }
    fun present(drawable: Drawable): Unit = access {
        recording(); require(drawable.device === device)
        Native.present(it, drawable.handle())
    }
    /** Submits once, asynchronously. Resources are retained through GPU completion. */
    fun commit(): Unit = access { recording(); Native.commit(it) }
    /** Blocking; use a worker thread. False means timeout, not completion. */
    fun waitUntilCompleted(timeoutNanos: Long = Long.MAX_VALUE): Boolean = access {
        require(timeoutNanos >= 0); Native.waitCommand(it, timeoutNanos)
    }
    /** A submitted command waits before releasing its native resources. */
    override fun close() = synchronized(device) {
        encoder?.abort()
        encoder = null
        super.close()
    }
}

abstract class CommandEncoder internal constructor(protected val commandBuffer: CommandBuffer) : AutoCloseable {
    private var ended = false
    protected fun <T> encode(block: (Long) -> T): T = commandBuffer.encode(this, block)
    protected open fun finish() {}
    internal open fun abort() { ended = true }
    fun endEncoding() = synchronized(commandBuffer.device) {
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
) {
    fun pack() = listOf(index.toLong(), buffer?.handle() ?: 0, offset, length, texture?.handle() ?: 0, sampler?.handle() ?: 0)
}

abstract class ShaderCommandEncoder internal constructor(command: CommandBuffer) : CommandEncoder(command) {
    private val bindings = sortedMapOf<Int, BoundResource>()
    private var constants = byteArrayOf()
    fun setBuffer(buffer: Buffer, index: Int, offset: Long = 0, length: Long = buffer.length - offset): Unit = encode {
        require(buffer.device === commandBuffer.device && index >= 0 && offset >= 0 && length > 0 && offset <= buffer.length && length <= buffer.length - offset)
        buffer.handle(); bindings[index] = BoundResource(index, buffer, offset, length)
    }
    fun setTexture(texture: Texture, index: Int, sampler: Sampler? = null): Unit = encode {
        require(texture.device === commandBuffer.device && index >= 0 && (sampler == null || sampler.device === commandBuffer.device))
        texture.handle(); sampler?.handle(); bindings[index] = BoundResource(index, texture = texture, sampler = sampler)
    }
    fun setBytes(bytes: ByteArray): Unit = encode { constants = bytes.copyOf() }
    protected fun bindingData(): LongArray = bindings.values.flatMap { it.pack() }.toLongArray()
    protected fun constantData(): ByteArray = constants
}

class ComputeCommandEncoder internal constructor(command: CommandBuffer) : ShaderCommandEncoder(command) {
    private var pipeline: ComputePipelineState? = null
    fun setComputePipelineState(state: ComputePipelineState): Unit = encode {
        require(state.device === commandBuffer.device); state.handle(); pipeline = state
    }
    fun dispatchThreadgroups(groups: Size): Unit = encode {
        val state = checkNotNull(pipeline) { "Set a compute pipeline first" }
        Native.dispatch(it, state.handle(), bindingData(), constantData(), groups.array())
    }
    /** Rounds up to whole workgroups. The shader MUST bounds-check excess invocations. */
    fun dispatchThreads(threads: Size): Unit = encode {
        val local = checkNotNull(pipeline) { "Set a compute pipeline first" }.threadgroupSize
        fun groups(n: Int, size: Int) = ((n.toLong() + size - 1) / size).toInt()
        dispatchThreadgroups(Size(groups(threads.width, local.width), groups(threads.height, local.height), groups(threads.depth, local.depth)))
    }
}
class RenderCommandEncoder internal constructor(command: CommandBuffer, private var nativeEncoder: Long) : ShaderCommandEncoder(command) {
    private var pipeline: RenderPipelineState? = null
    fun setRenderPipelineState(state: RenderPipelineState): Unit = encode {
        require(state.device === commandBuffer.device); state.handle(); pipeline = state
    }
    fun drawPrimitives(vertexCount: Int, instanceCount: Int = 1, firstVertex: Int = 0, firstInstance: Int = 0): Unit = encode {
        require(vertexCount > 0 && instanceCount > 0 && firstVertex >= 0 && firstInstance >= 0)
        val state = checkNotNull(pipeline) { "Set a render pipeline first" }
        Native.draw(nativeEncoder, state.handle(), bindingData(), constantData(), intArrayOf(vertexCount, instanceCount, firstVertex, firstInstance))
    }
    override fun finish() { Native.endRender(nativeEncoder); nativeEncoder = 0 }
    internal override fun abort() {
        if (nativeEncoder != 0L) { Native.close(nativeEncoder); nativeEncoder = 0 }
        super.abort()
    }
}
class BlitCommandEncoder internal constructor(command: CommandBuffer) : CommandEncoder(command) {
    fun copy(source: Buffer, destination: Buffer, length: Long = source.length, sourceOffset: Long = 0, destinationOffset: Long = 0): Unit = encode {
        require(source.device === commandBuffer.device && destination.device === commandBuffer.device)
        Native.copyBuffers(it, source.handle(), destination.handle(), sourceOffset, destinationOffset, length)
    }
    /** Tightly packed rows, full 2D image, single mip/layer. */
    fun copy(source: Buffer, destination: Texture, sourceOffset: Long = 0): Unit = encode {
        require(source.device === commandBuffer.device && destination.device === commandBuffer.device)
        Native.copyTexture(it, source.handle(), destination.handle(), sourceOffset, true)
    }
    fun copy(source: Texture, destination: Buffer, destinationOffset: Long = 0): Unit = encode {
        require(source.device === commandBuffer.device && destination.device === commandBuffer.device)
        Native.copyTexture(it, destination.handle(), source.handle(), destinationOffset, false)
    }
}
