package dev.vulkano

import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlinx.coroutines.suspendCancellableCoroutine

/** A terminal GPU result. Recording cancellation and GPU failures carry an error. */
data class CommandBufferCompletion(val status: CommandBufferStatus, val error: Throwable? = null)

internal object CompletionMonitor {
    private val poller =
        Executors.newSingleThreadScheduledExecutor { task ->
            Thread(task, "Vulkano completion polling").apply { isDaemon = true }
        }
    val callbacks =
        Executors.newCachedThreadPool { task ->
            Thread(task, "Vulkano completion callback").apply { isDaemon = true }
        }

    fun watch(command: CommandBuffer) {
        if (command.completionQueued.get() || !command.monitoring.compareAndSet(false, true)) return
        poller.execute { poll(command) }
    }

    private fun poll(command: CommandBuffer) {
        if (command.completionQueued.get()) return
        val result = command.pollCompletion()
        if (result != null) finish(command, result)
        else poller.schedule({ poll(command) }, 1, TimeUnit.MILLISECONDS)
    }

    fun finish(command: CommandBuffer, result: CommandBufferCompletion) {
        if (!command.completionQueued.compareAndSet(false, true)) return
        command.device.forgetCompletion(command)
        // Completing a CompletableFuture may invoke arbitrary user continuations. Never do so
        // while holding Device/JNI locks or on the shared fence-polling thread.
        callbacks.execute { command.completion.complete(result) }
    }
}

/** A cancellable CPU wait. Cancelling this coroutine does not cancel submitted GPU work. */
suspend fun CommandBuffer.awaitCompleted(): Unit = suspendCancellableCoroutine { continuation ->
    val future = completionFuture()
    future.whenComplete { result, failure ->
        val error = failure ?: result.error
        if (error != null) continuation.resumeWithException(error) else continuation.resume(Unit)
    }
    continuation.invokeOnCancellation { future.cancel(false) }
}
