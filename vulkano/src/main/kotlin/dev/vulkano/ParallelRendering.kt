package dev.vulkano

import dev.vulkano.internal.Native

/**
 * Children may record on different CPU threads. Their draw order is the order of child creation,
 * regardless of which child finishes first. Each child owns independent pipeline/binding state.
 * Native entry points remain serialized; this API does not promise parallel GPU execution.
 */
class ParallelRenderCommandEncoder
internal constructor(command: CommandBuffer, private var nativeEncoder: Long) : CommandEncoder(command) {
    private val children = mutableSetOf<CommandEncoder>()

    fun makeRenderCommandEncoder(): RenderCommandEncoder = encode {
        RenderCommandEncoder(commandBuffer, Native.beginParallelRenderChild(nativeEncoder)).also { children.add(it) }
    }

    internal fun owns(child: CommandEncoder): Boolean = child in children
    internal fun childEnded(child: CommandEncoder) { check(children.remove(child)) }

    override fun finish() {
        check(children.isEmpty()) { "End every child render encoder before ending parallel recording" }
        Native.endRender(nativeEncoder)
        nativeEncoder = 0
    }

    internal override fun abort() {
        children.forEach { it.abort() }
        children.clear()
        if (nativeEncoder != 0L) { Native.close(nativeEncoder); nativeEncoder = 0 }
        super.abort()
    }
}
