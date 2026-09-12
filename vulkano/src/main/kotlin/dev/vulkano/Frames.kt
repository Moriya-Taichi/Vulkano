package dev.vulkano

/** Reuse slots only after their GPU fence signals. Keep mutable per-frame resources per slot. */
class FrameScheduler
internal constructor(private val device: Device, val maxFramesInFlight: Int, queueIndex: Int) :
    AutoCloseable {
    private val queue = device.makeCommandQueue(queueIndex)
    private val pending = arrayOfNulls<CommandBuffer>(maxFramesInFlight)
    private var nextSlot = 0
    private var active: Frame? = null
    private var closed = false

    /** Non-blocking acquisition. Null means this slot is still used by the GPU. */
    fun beginFrame(): Frame? =
        device.access {
            check(!closed) { "Frame scheduler is closed" }
            check(active == null) { "Submit or close the active frame first" }
            val previous = pending[nextSlot]
            if (previous != null && !previous.isClosed) {
                if (previous.status == CommandBufferStatus.SUBMITTED) return@access null
                previous.close()
            }
            pending[nextSlot] = null
            Frame(this, nextSlot, queue.makeCommandBuffer()).also { active = it }
        }

    internal fun submit(frame: Frame): Unit =
        device.access {
            check(!closed && active === frame) { "Frame is no longer active" }
            try {
                frame.commandBuffer.commit()
            } finally {
                release(frame)
            }
        }

    private fun release(frame: Frame) {
        if (frame.commandBuffer.isClosed) {
            active = null
            frame.finished = true
            return
        }
        val state = frame.commandBuffer.status
        if (state == CommandBufferStatus.SUBMITTED || state == CommandBufferStatus.COMPLETED) {
            pending[frame.index] = frame.commandBuffer
            nextSlot = (frame.index + 1) % maxFramesInFlight
        } else frame.commandBuffer.close()
        active = null
        frame.finished = true
    }

    internal fun abandon(frame: Frame) =
        synchronized(device) {
            if (!frame.finished && active === frame) {
                if (device.closed) {
                    frame.commandBuffer.close()
                    active = null
                    frame.finished = true
                } else release(frame)
            }
        }

    override fun close() {
        val commands =
            synchronized(device) {
                if (closed) return
                closed = true
                val commands = pending.filterNotNull() + listOfNotNull(active?.commandBuffer)
                active?.finished = true
                active = null
                pending.fill(null)
                queue.close()
                commands
            }
        // Fence waits release Device locks, so other threads can signal timeline dependencies.
        var failure: Throwable? = null
        for (command in commands) try {
            command.close()
        } catch (error: Throwable) {
            if (failure == null) failure = error
        }
        failure?.let { throw it }
    }
}

class Frame
internal constructor(
    private val scheduler: FrameScheduler,
    val index: Int,
    val commandBuffer: CommandBuffer,
) : AutoCloseable {
    internal var finished = false

    /** Submit once. The scheduler keeps the command alive until its slot is reused or closed. */
    fun submit() = scheduler.submit(this)

    /** Discard unsubmitted work; submitted work remains owned by the scheduler. */
    override fun close() = scheduler.abandon(this)
}

fun Device.makeFrameScheduler(maxFramesInFlight: Int = 3, queueIndex: Int = 0): FrameScheduler =
    access {
        require(maxFramesInFlight in 1..8) { "Choose one to eight in-flight frames" }
        FrameScheduler(this, maxFramesInFlight, queueIndex)
    }
