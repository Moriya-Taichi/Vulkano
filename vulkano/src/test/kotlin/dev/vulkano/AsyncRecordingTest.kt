package dev.vulkano

import java.util.concurrent.CancellationException
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test

class AsyncRecordingTest {
    @Before fun enabled() = assumeTrue(System.getProperty("vulkano.runNativeTests") == "true")

    private fun device(features: Set<Feature> = emptySet()) =
        Device.create(features, System.getenv("VULKANO_VALIDATION") != null, true)

    private fun Device.function(name: String, constants: FunctionConstants = FunctionConstants()) =
        makeLibrary(TestShaders.read(name)).makeFunction(constants = constants)

    private fun <T> CompletableFuture<T>.result(): T = get(10, TimeUnit.SECONDS)

    @Test
    fun callbacksRunOnceOutsideLocksAndDoNotBlockOtherCompletions(): Unit =
        device().use { d ->
            d.makeCommandQueue().use { q ->
                q.makeCommandBuffer().use { c ->
                    val calls = AtomicInteger()
                    val direct = Executor { it.run() }
                    val handler =
                        c.addCompletedHandler(direct) { completion ->
                            assertFalse(Thread.holdsLock(d))
                            assertEquals(CommandBufferStatus.COMPLETED, completion.status)
                            assertNull(completion.error)
                            calls.incrementAndGet()
                            // A callback may wait for another submission without blocking the fence
                            // poller.
                            q.makeCommandBuffer().use { nested ->
                                nested.commit()
                                assertEquals(
                                    CommandBufferStatus.COMPLETED,
                                    nested.completionFuture().result().status,
                                )
                            }
                        }
                    val ignored = c.completionFuture()
                    assertTrue(ignored.cancel(false))
                    val observed = c.completionFuture()
                    c.commit()
                    handler.result()
                    assertEquals(1, calls.get())
                    assertEquals(CommandBufferStatus.COMPLETED, observed.result().status)
                    c.addCompletedHandler(direct) { calls.incrementAndGet() }.result()
                    assertEquals(2, calls.get())
                    val failedHandler = c.addCompletedHandler(direct) { error("handler failed") }
                    assertThrows(java.util.concurrent.ExecutionException::class.java) {
                        failedHandler.result()
                    }
                    assertEquals(
                        CommandBufferStatus.COMPLETED,
                        c.completionFuture().result().status,
                    )
                }
            }
        }

    @Test
    fun cancellingCoroutineWaitDoesNotCancelGpuSubmission(): Unit {
        assumeTrue(device().use { Feature.TIMELINE_SEMAPHORE in it.capabilities.availableFeatures })
        device(setOf(Feature.TIMELINE_SEMAPHORE)).use { d ->
            val event = d.makeSharedEvent()
            d.makeCommandQueue().use { q ->
                q.makeCommandBuffer().use { c ->
                    c.waitForEvent(event, 1)
                    try {
                        runBlocking {
                            withTimeout(10000) {
                                val waiting =
                                    launch(start = CoroutineStart.UNDISPATCHED) {
                                        c.awaitCompleted()
                                    }
                                c.commit()
                                waiting.cancelAndJoin()
                                assertTrue(waiting.isCancelled)
                                assertEquals(CommandBufferStatus.SUBMITTED, c.status)
                                event.signal(1)
                                c.awaitCompleted()
                                assertEquals(CommandBufferStatus.COMPLETED, c.status)
                            }
                        }
                    } finally {
                        if (event.signaledValue == 0L) event.signal(1)
                    }
                }
            }
        }
    }

    @Test
    fun failedAndAbandonedRecordingCompleteObservers(): Unit =
        device().use { d ->
            d.makeCommandQueue().use { q ->
                q.makeCommandBuffer().use { c ->
                    val future = c.completionFuture()
                    val input =
                        d.makeTexture(
                            TextureDescriptor(1, 1, usage = setOf(TextureUsage.TRANSFER_SOURCE))
                        )
                    val output = d.makeBuffer(4)
                    c.blit { copy(input, output) }
                    val failure = assertThrows(IllegalArgumentException::class.java) { c.commit() }
                    val result = future.result()
                    assertEquals(CommandBufferStatus.FAILED, result.status)
                    assertSame(failure, result.error)
                    assertThrows(IllegalArgumentException::class.java) {
                        runBlocking { c.awaitCompleted() }
                    }
                }
                val abandoned = q.makeCommandBuffer()
                val future = abandoned.completionFuture()
                abandoned.close()
                assertTrue(future.result().error is CancellationException)
            }
        }

    @Test
    fun deviceCloseStillReportsSuccessfulSubmittedCompletion() {
        val d = device()
        val c = d.makeCommandQueue().use { it.makeCommandBuffer() }
        val unsubmitted = d.makeCommandQueue().use { it.makeCommandBuffer() }
        val abandoned = unsubmitted.completionFuture()
        val future = c.completionFuture()
        c.commit()
        d.close()
        assertEquals(CommandBufferStatus.COMPLETED, future.result().status)
        assertEquals(CommandBufferStatus.COMPLETED, c.completionFuture().result().status)
        assertTrue(abandoned.result().error is CancellationException)
        c.close()
        unsubmitted.close()
    }

    @Test
    fun parallelChildrenKeepCreationOrderAndIndependentState(): Unit =
        device().use { d ->
            val executor = Executors.newFixedThreadPool(4)
            try {
                val color =
                    d.makeTexture(
                        TextureDescriptor(
                            2,
                            2,
                            usage =
                                setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                        )
                    )
                val output = d.makeBuffer(16)
                val pipelines =
                    (0..3).map { n ->
                        d.makeRenderPipelineState(
                            RenderPipelineDescriptor(
                                d.function("fullscreen.vert.spv"),
                                d.function(
                                    "solid.frag.spv",
                                    FunctionConstants().setFloat(0, n / 4f),
                                ),
                            )
                        )
                    }
                d.makeCommandQueue().use { q ->
                    q.makeCommandBuffer().use { c ->
                        val parent =
                            c.makeParallelRenderCommandEncoder(
                                RenderPassDescriptor(listOf(ColorAttachment(color)))
                            )
                        val children = (0..3).map { parent.makeRenderCommandEncoder() }
                        assertThrows(IllegalStateException::class.java) { parent.endEncoding() }
                        assertThrows(IllegalStateException::class.java) {
                            children[0].drawPrimitives(3)
                        }
                        assertThrows(IllegalStateException::class.java) {
                            c.makeComputeCommandEncoder()
                        }
                        val finished = (0..3).map { CountDownLatch(1) }
                        val work =
                            children.mapIndexed { index, child ->
                                executor.submit {
                                    if (index < 3)
                                        assertTrue(finished[index + 1].await(10, TimeUnit.SECONDS))
                                    child.setRenderPipelineState(pipelines[index])
                                    child.drawPrimitives(3)
                                    child.endEncoding()
                                    finished[index].countDown()
                                }
                            }
                        work.forEach { it.get(10, TimeUnit.SECONDS) }
                        parent.endEncoding()
                        c.blit { copy(color, output) }
                        c.commit()
                        c.completionFuture().result()
                        val pixels = output.readBytes(16)
                        repeat(4) { assertEquals(191, pixels[it * 4].toInt() and 255) }
                        assertThrows(IllegalStateException::class.java) {
                            children[0].drawPrimitives(3)
                        }
                    }
                }
            } finally {
                executor.shutdownNow()
            }
        }

    @Test
    fun parallelChildrenCanOrderCrossChildMemoryAccesses(): Unit {
        assumeTrue(
            device().use {
                Feature.FRAGMENT_STORES_AND_ATOMICS in it.capabilities.availableFeatures
            }
        )
        device(setOf(Feature.FRAGMENT_STORES_AND_ATOMICS)).use { d ->
            val color =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val data = d.makeBuffer(64)
            val output = d.makeBuffer(16)
            val producer =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("draw-write.frag.spv"),
                    )
                )
            val consumer =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("draw-read.vert.spv"),
                        d.function("draw-color.frag.spv"),
                    )
                )
            d.makeCommandQueue().use { q ->
                q.makeCommandBuffer().use { c ->
                    c.makeParallelRenderCommandEncoder(
                            RenderPassDescriptor(listOf(ColorAttachment(color)))
                        )
                        .use { parent ->
                            val first = parent.makeRenderCommandEncoder()
                            val second = parent.makeRenderCommandEncoder()
                            second.setRenderPipelineState(consumer)
                            second.setBuffer(data, 0)
                            second.drawPrimitives(3)
                            second.close()
                            first.setRenderPipelineState(producer)
                            first.setBuffer(data, 0)
                            first.drawPrimitives(3)
                            first.memoryBarrier()
                            first.close()
                        }
                    c.blit { copy(color, output) }
                    c.commit()
                    c.completionFuture().result()
                    assertArrayEquals(
                        ByteArray(16) { if (it % 4 == 1 || it % 4 == 3) -1 else 0 },
                        output.readBytes(16),
                    )
                }
            }
        }
    }

    @Test
    fun abortingParallelRecordingClosesChildren(): Unit =
        device().use { d ->
            val color =
                d.makeTexture(TextureDescriptor(1, 1, usage = setOf(TextureUsage.COLOR_ATTACHMENT)))
            val c = d.makeCommandQueue().use { it.makeCommandBuffer() }
            val parent =
                c.makeParallelRenderCommandEncoder(
                    RenderPassDescriptor(listOf(ColorAttachment(color)))
                )
            val child = parent.makeRenderCommandEncoder()
            c.close()
            child.close()
            parent.close()
            assertThrows(IllegalStateException::class.java) { child.memoryBarrier() }
        }
}
