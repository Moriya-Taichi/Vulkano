package dev.vulkano

import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test

class RenderSynchronizationTest {
    @Before fun enabled() = assumeTrue(System.getProperty("vulkano.runNativeTests") == "true")

    private fun device(features: Set<Feature> = emptySet()) =
        Device.create(features, System.getenv("VULKANO_VALIDATION") != null, true)

    private fun Device.function(name: String, constants: FunctionConstants = FunctionConstants()) =
        makeLibrary(TestShaders.read(name)).makeFunction(constants = constants)

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
    fun fragmentWritesBecomeVertexReadsWhileColorIsPreserved(): Unit {
        assumeTrue(
            device().use {
                Feature.FRAGMENT_STORES_AND_ATOMICS in it.capabilities.availableFeatures
            }
        )
        device(setOf(Feature.FRAGMENT_STORES_AND_ATOMICS)).use { d ->
            val write =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("draw-write.frag.spv"),
                    )
                )
            val read =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("draw-read.vert.spv"),
                        d.function("draw-color.frag.spv"),
                    )
                )
            val data = d.makeBuffer(64)
            val color =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val output = d.makeBuffer(16)
            d.submit {
                render(RenderPassDescriptor(listOf(ColorAttachment(color)))) {
                    memoryBarrier() // No earlier draw: must not duplicate attachment clears.
                    setRenderPipelineState(write)
                    setBuffer(data, 0)
                    drawPrimitives(3)
                    memoryBarrier()
                    memoryBarrier()
                    setRenderPipelineState(read)
                    setScissorRect(ScissorRect(0, 0, 1, 2))
                    drawPrimitives(3)
                    memoryBarrier() // No later draw: preserve the requested final store.
                }
                blit { copy(color, output) }
            }
            val expected = byteArrayOf(0, -1, 0, -1, -1, 0, 0, -1, 0, -1, 0, -1, -1, 0, 0, -1)
            assertArrayEquals(expected, output.readBytes(16))
        }
    }

    @Test
    fun splitPassPreservesDepthStencilAndMsaaResolve(): Unit =
        device().use { d ->
            for (samples in listOf(1, 4)) {
                val descriptor =
                    TextureDescriptor(
                        2,
                        2,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT),
                        sampleCount = samples,
                    )
                val ds =
                    TextureDescriptor(
                        2,
                        2,
                        PixelFormat.DEPTH32_FLOAT_STENCIL8,
                        setOf(TextureUsage.DEPTH_ATTACHMENT),
                        sampleCount = samples,
                    )
                if (!d.supportsTexture(descriptor) || !d.supportsTexture(ds)) continue
                val color =
                    d.makeTexture(
                        if (samples == 1)
                            descriptor.copy(
                                usage =
                                    setOf(
                                        TextureUsage.COLOR_ATTACHMENT,
                                        TextureUsage.TRANSFER_SOURCE,
                                    )
                            )
                        else descriptor
                    )
                val resolved =
                    if (samples > 1)
                        d.makeTexture(
                            TextureDescriptor(
                                2,
                                2,
                                usage =
                                    setOf(
                                        TextureUsage.COLOR_ATTACHMENT,
                                        TextureUsage.TRANSFER_SOURCE,
                                    ),
                            )
                        )
                    else color
                val depth = d.makeTexture(ds)
                val output = d.makeBuffer(16)
                val vertex = d.function("fullscreen.vert.spv")
                val first =
                    d.makeRenderPipelineState(
                        RenderPipelineDescriptor(
                            vertex,
                            d.function("solid.frag.spv"),
                            depthFormat = ds.pixelFormat,
                            sampleCount = samples,
                            depthStencil =
                                DepthStencilDescriptor(
                                    depthCompareFunction = CompareFunction.ALWAYS,
                                    frontFaceStencil =
                                        StencilDescriptor(
                                            depthStencilPassOperation = StencilOperation.REPLACE
                                        ),
                                ),
                        )
                    )
                val second =
                    d.makeRenderPipelineState(
                        RenderPipelineDescriptor(
                            vertex,
                            d.function("solid.frag.spv", FunctionConstants().setFloat(0, 0f)),
                            depthFormat = ds.pixelFormat,
                            sampleCount = samples,
                            depthStencil =
                                DepthStencilDescriptor(
                                    depthWriteEnabled = false,
                                    depthCompareFunction = CompareFunction.EQUAL,
                                    frontFaceStencil =
                                        StencilDescriptor(
                                            compareFunction = CompareFunction.EQUAL,
                                            writeMask = 0,
                                        ),
                                ),
                        )
                    )
                d.submit {
                    render(
                        RenderPassDescriptor(
                            listOf(
                                ColorAttachment(
                                    color,
                                    resolveTexture = if (samples > 1) resolved else null,
                                )
                            ),
                            DepthAttachment(
                                depth,
                                storeAction = StoreAction.DONT_CARE,
                                stencilStoreAction = StoreAction.DONT_CARE,
                            ),
                        )
                    ) {
                        setRenderPipelineState(first)
                        setStencilReferenceValue(37)
                        drawPrimitives(3)
                        memoryBarrier()
                        setRenderPipelineState(second)
                        setScissorRect(ScissorRect(0, 0, 1, 2))
                        drawPrimitives(3)
                    }
                    blit { copy(resolved, output) }
                }
                val pixels = output.readBytes(16)
                for (row in 0..1) {
                    assertEquals(0, pixels[row * 8].toInt() and 255)
                    assertEquals(255, pixels[row * 8 + 4].toInt() and 255)
                }
            }
        }

    @Test
    fun memorylessAndSubpassBarriersAreRejected(): Unit =
        device().use { d ->
            val memoryless =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT),
                        storageMode = StorageMode.MEMORYLESS,
                    )
                )
            d.makeCommandQueue().use { q ->
                q.makeCommandBuffer().use { c ->
                    c.render(
                        RenderPassDescriptor(
                            listOf(ColorAttachment(memoryless, storeAction = StoreAction.DONT_CARE))
                        )
                    ) {
                        assertThrows(IllegalArgumentException::class.java) { memoryBarrier() }
                    }
                }
            }
            val color =
                d.makeTexture(TextureDescriptor(2, 2, usage = setOf(TextureUsage.COLOR_ATTACHMENT)))
            val layout =
                RenderPassLayout(listOf(color.pixelFormat), listOf(RenderSubpass(listOf(0))))
            d.makeCommandQueue().use { q ->
                q.makeCommandBuffer().use { c ->
                    c.render(
                        RenderPassDescriptor(listOf(ColorAttachment(color)), subpassLayout = layout)
                    ) {
                        assertThrows(IllegalArgumentException::class.java) { memoryBarrier() }
                    }
                }
            }
        }
}
