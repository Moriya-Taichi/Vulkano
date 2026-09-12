package dev.vulkano

import dev.vulkano.internal.Native

data class ResourceCacheStatistics(
    val descriptorPoolsCreated: Long,
    val descriptorPoolsReused: Long,
    val framebuffersCreated: Long,
    val framebuffersReused: Long,
    val idleDescriptorPools: Int,
    val idleFramebuffers: Int,
)

fun Device.resourceCacheStatistics(): ResourceCacheStatistics = access {
    val values = Native.resourceCacheStatistics(nativeHandle)
    ResourceCacheStatistics(
        values[0],
        values[1],
        values[2],
        values[3],
        values[4].toInt(),
        values[5].toInt(),
    )
}

/** Release idle descriptor pools and framebuffers, for example on Android memory pressure. */
fun Device.trimIdleResources(): Unit = access { Native.trimIdleResources(nativeHandle) }
