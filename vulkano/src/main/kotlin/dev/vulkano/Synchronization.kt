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

    val timestampValidBits: Int
        get() = access { Native.counterInfo(it)[0].toInt() }

    val timestampPeriodNanos: Double
        get() = access {
            require(isTimestamp)
            Float.fromBits(Native.counterInfo(it)[1].toInt()).toDouble()
        }

    fun elapsedNanos(startIndex: Int, endIndex: Int): Double? {
        require(isTimestamp && startIndex in 0 until count && endIndex in 0 until count)
        val values = read() ?: return null
        val bits = timestampValidBits
        val mask = if (bits == 64) -1L else (1L shl bits) - 1
        return ((values[endIndex] - values[startIndex]) and mask).toULong().toDouble() *
            timestampPeriodNanos
    }
}

fun Device.makeCounterSampleBuffer(
    count: Int,
    timestamp: Boolean = true,
    queueIndex: Int = 0,
): CounterSampleBuffer = access {
    require(count > 0)
    CounterSampleBuffer(
        this,
        Native.createCounters(nativeHandle, count, timestamp, queueIndex),
        count,
        timestamp,
    )
}

/** The opaque cache is specific to the driver/device. Incompatible cache headers are rejected. */
fun Device.serializePipelineCache(): ByteArray = access { Native.pipelineCacheData(nativeHandle) }

fun Device.loadPipelineCache(data: ByteArray): Unit = access {
    Native.loadPipelineCache(nativeHandle, data)
}

data class CounterCapabilities(
    val timestampValidBits: Int,
    val timestampPeriodNanos: Double,
    val preciseOcclusionAvailable: Boolean,
)

fun Device.counterCapabilities(queueIndex: Int = 0): CounterCapabilities = access {
    val data = Native.counterCapabilities(nativeHandle, queueIndex)
    CounterCapabilities(data[0].toInt(), Float.fromBits(data[1].toInt()).toDouble(), data[2] != 0L)
}
