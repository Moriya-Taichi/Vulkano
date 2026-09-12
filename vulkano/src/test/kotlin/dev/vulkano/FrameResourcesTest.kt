package dev.vulkano

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test

class FrameResourcesTest {
    @Before fun enabled() = assumeTrue(System.getProperty("vulkano.runNativeTests") == "true")

    private fun device(features: Set<Feature> = emptySet()) =
        Device.create(features, System.getenv("VULKANO_VALIDATION") != null, true)

    private fun Device.submit(block: CommandBuffer.() -> Unit) {
        makeCommandQueue().use { q ->
            q.makeCommandBuffer().use { c ->
                c.block()
                c.commit()
                assertTrue(c.waitUntilCompleted())
            }
        }
    }

    @Test
    fun completedCommandsReuseResetDescriptorPools(): Unit =
        device().use { d ->
            val function = d.makeLibrary(TestShaders.read("double.comp.spv")).makeFunction()
            val pipeline = d.makeComputePipelineState(function)
            val data = d.makeBuffer(4)
            data.write(
                ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder()).putFloat(1f).flip()
                    as ByteBuffer
            )
            repeat(4) { iteration ->
                d.submit {
                    compute {
                        setComputePipelineState(pipeline)
                        setBuffer(data, 0)
                        setBytes(
                            ByteBuffer.allocate(4).order(ByteOrder.nativeOrder()).putInt(1).array()
                        )
                        dispatchThreadgroups(Size(1))
                    }
                }
                val actual = ByteBuffer.wrap(data.readBytes(4)).order(ByteOrder.nativeOrder()).float
                assertEquals((1 shl (iteration + 1)).toFloat(), actual, 0f)
            }
            val cached = d.resourceCacheStatistics()
            assertEquals(1L, cached.descriptorPoolsCreated)
            assertEquals(3L, cached.descriptorPoolsReused)
            assertEquals(1, cached.idleDescriptorPools)
            d.trimIdleResources()
            assertEquals(0, d.resourceCacheStatistics().idleDescriptorPools)
        }

    @Test
    fun framebuffersReuseExactViewsAndAreInvalidatedOnViewDestruction(): Unit =
        device().use { d ->
            val color =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                        arrayLength = 2,
                        textureType = TextureType.TYPE_2D_ARRAY,
                    )
                )
            val output = d.makeBuffer(16)
            repeat(4) { iteration ->
                val layer = iteration % 2
                d.submit {
                    render(
                        RenderPassDescriptor(
                            listOf(
                                ColorAttachment(
                                    color,
                                    slice = layer,
                                    clearColor =
                                        if (layer == 0) ClearColor(1f, 0f, 0f, 1f)
                                        else ClearColor(0f, 1f, 0f, 1f),
                                )
                            )
                        )
                    ) {}
                }
            }
            assertEquals(2L, d.resourceCacheStatistics().framebuffersCreated)
            assertEquals(2L, d.resourceCacheStatistics().framebuffersReused)
            for (layer in 0..1) {
                val view =
                    color.makeTextureView(
                        textureType = TextureType.TYPE_2D,
                        slice = layer,
                        sliceCount = 1,
                    )
                d.submit { blit { copy(view, output) } }
                val bytes = output.readBytes(16)
                repeat(4) { assertEquals(255, bytes[it * 4 + layer].toInt() and 255) }
                view.close()
            }
            color.close()
            assertEquals(0, d.resourceCacheStatistics().idleFramebuffers)
            d.trimIdleResources()
        }

    @Test
    fun frameRingDoesNotReuseResourcesBeforeGpuCompletion(): Unit {
        assumeTrue(device().use { Feature.TIMELINE_SEMAPHORE in it.capabilities.availableFeatures })
        device(setOf(Feature.TIMELINE_SEMAPHORE)).use { d ->
            val event = d.makeSharedEvent()
            val outputs = List(3) { d.makeBuffer(4) }
            d.makeFrameScheduler(3).use { frames ->
                val commands = mutableListOf<CommandBuffer>()
                try {
                    repeat(3) { index ->
                        checkNotNull(frames.beginFrame()).use { frame ->
                            assertEquals(index, frame.index)
                            if (index == 0) frame.commandBuffer.waitForEvent(event, 1)
                            frame.commandBuffer.blit {
                                fill(outputs[index], (0x20 + index).toByte())
                            }
                            commands.add(frame.commandBuffer)
                            frame.submit()
                        }
                    }
                    assertNull(frames.beginFrame())
                    event.signal(1)
                    commands.forEach { it.completionFuture().get(10, TimeUnit.SECONDS) }
                    commands[0]
                        .close() // Explicit command close also leaves its frame slot reusable.
                    checkNotNull(frames.beginFrame()).use { frame ->
                        assertEquals(0, frame.index)
                        assertThrows(IllegalStateException::class.java) { frames.beginFrame() }
                    }
                    checkNotNull(frames.beginFrame()).use { frame -> assertEquals(0, frame.index) }
                    outputs.forEachIndexed { index, buffer ->
                        assertEquals(
                            0x20202020 + index * 0x01010101,
                            ByteBuffer.wrap(buffer.readBytes(4)).order(ByteOrder.nativeOrder()).int,
                        )
                    }
                } finally {
                    if (event.signaledValue == 0L) event.signal(1)
                }
            }
        }
    }

    @Test
    fun abandonedAndFailedFramesReleaseTheirSlots(): Unit =
        device().use { d ->
            d.makeFrameScheduler(2).use { frames ->
                val first = checkNotNull(frames.beginFrame())
                first.close()
                first.close()
                assertThrows(IllegalStateException::class.java) { first.submit() }
                checkNotNull(frames.beginFrame()).use { frame ->
                    val source =
                        d.makeTexture(
                            TextureDescriptor(1, 1, usage = setOf(TextureUsage.TRANSFER_SOURCE))
                        )
                    frame.commandBuffer.blit { copy(source, d.makeBuffer(4)) }
                    assertThrows(IllegalArgumentException::class.java) { frame.submit() }
                }
                checkNotNull(frames.beginFrame()).use { frame ->
                    assertEquals(0, frame.index)
                    assertThrows(IllegalStateException::class.java) {
                        frame.commandBuffer.compute { error("abort recording") }
                    }
                }
                checkNotNull(frames.beginFrame()).use { it.submit() }
            }
            assertThrows(IllegalArgumentException::class.java) { d.makeFrameScheduler(0) }
        }
}
