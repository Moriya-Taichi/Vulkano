package dev.vulkano

import dev.vulkano.internal.Native

class SharedEvent internal constructor(device: Device, id: Long) : Resource(device, id) {
    val signaledValue: Long
        get() = access { Native.eventValue(it) }

    fun signal(value: Long): Unit = access {
        require(value >= 0)
        Native.signalEvent(it, value)
    }

    fun wait(value: Long, timeoutNanos: Long = Long.MAX_VALUE): Boolean {
        require(value >= 0 && timeoutNanos >= 0)
        val start = System.nanoTime()
        do {
            if (signaledValue >= value) return true
            if (timeoutNanos == 0L || System.nanoTime() - start >= timeoutNanos) return false
            Thread.sleep(1)
        } while (true)
    }
}

fun Device.makeSharedEvent(initialValue: Long = 0): SharedEvent = access {
    require(initialValue >= 0)
    SharedEvent(this, Native.createEvent(nativeHandle, initialValue))
}

/** Results become available after the command that last wrote the pool completes. */
class CounterSampleBuffer
internal constructor(device: Device, id: Long, val count: Int, val isTimestamp: Boolean) :
    Resource(device, id) {
    fun read(): LongArray? = access { Native.readCounters(it).takeIf { it.isNotEmpty() } }
}

fun Device.makeCounterSampleBuffer(count: Int, timestamp: Boolean = true): CounterSampleBuffer =
    access {
        require(count > 0)
        CounterSampleBuffer(
            this,
            Native.createCounters(nativeHandle, count, timestamp),
            count,
            timestamp,
        )
    }

/** The opaque cache is specific to the driver/device. Incompatible cache headers are rejected. */
fun Device.serializePipelineCache(): ByteArray = access { Native.pipelineCacheData(nativeHandle) }

fun Device.loadPipelineCache(data: ByteArray): Unit = access {
    Native.loadPipelineCache(nativeHandle, data)
}
