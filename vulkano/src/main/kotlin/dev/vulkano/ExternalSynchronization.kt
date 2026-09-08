package dev.vulkano

import dev.vulkano.internal.Native

/** Owns a SYNC_FD. The value -1 represents a fence that has already signalled. */
class SyncFd private constructor(private var descriptor: Int) : AutoCloseable {
    internal fun <T> access(block: (Int) -> T): T =
        synchronized(this) {
            check(descriptor >= -1) { "SyncFd is closed or detached" }
            block(descriptor)
        }

    fun duplicate(): SyncFd = access { adopt(Native.duplicateSyncFd(it)) }

    /** Transfers ownership to the caller, for example ParcelFileDescriptor.adoptFd(fd). */
    fun detach(): Int = access { fd ->
        descriptor = -2
        fd
    }

    override fun close(): Unit =
        synchronized(this) {
            if (descriptor >= -1) {
                val fd = descriptor
                descriptor = -2
                Native.closeSyncFd(fd)
            }
        }

    companion object {
        /** Takes ownership; the caller must no longer close or reuse this descriptor. */
        fun adopt(fd: Int): SyncFd {
            require(fd >= -1)
            return SyncFd(fd)
        }
    }
}

/**
 * One-shot binary semaphore. Imported sync files can be waited once; GPU signals can be exported
 * once.
 */
class ExternalSemaphore internal constructor(device: Device, id: Long) : Resource(device, id) {
    /** Call after submitting the signalling command. It can still be running on the GPU. */
    fun exportSyncFd(): SyncFd = access { SyncFd.adopt(Native.exportSyncFd(it)) }
}

fun Device.makeExternalSemaphore(): ExternalSemaphore = access {
    ExternalSemaphore(this, Native.createExternalSemaphore(nativeHandle, -2))
}

/** Imports a duplicate; the provided SyncFd remains owned by its caller. */
fun Device.importSyncFd(fd: SyncFd): ExternalSemaphore = access {
    fd.access { ExternalSemaphore(this, Native.createExternalSemaphore(nativeHandle, it)) }
}
