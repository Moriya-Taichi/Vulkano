package dev.vulkano

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test

class GraphicsIntegrationTest {
    @Before
    fun enabled() {
        assumeTrue(System.getProperty("vulkano.runNativeTests") == "true")
    }

    private fun device(features: Set<Feature> = emptySet()) =
        Device.create(features, System.getenv("VULKANO_VALIDATION") != null, true)

    private fun Device.function(file: String, constants: FunctionConstants = FunctionConstants()) =
        makeLibrary(
                checkNotNull(javaClass.classLoader!!.getResourceAsStream(file)).use {
                    it.readBytes()
                }
            )
            .makeFunction(constants = constants)

    private fun floats(vararg values: Float): ByteBuffer =
        ByteBuffer.allocateDirect(values.size * 4).order(ByteOrder.nativeOrder()).apply {
            values.forEach { putFloat(it) }
            flip()
        }

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
    fun mipArrayViewsCopyAndReadback(): Unit =
        device().use { d ->
            val texture =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        usage =
                            setOf(
                                TextureUsage.TRANSFER_SOURCE,
                                TextureUsage.TRANSFER_DESTINATION,
                                TextureUsage.SAMPLED,
                            ),
                        mipLevels = 3,
                        arrayLength = 2,
                        textureType = TextureType.TYPE_2D_ARRAY,
                    )
                )
            val pixels =
                ByteArray(128) { n ->
                    if (n % 4 == 3) 255.toByte() else if (n < 64) 32 else 192.toByte()
                }
            val upload = d.makeBuffer(128)
            upload.write(pixels)
            val result = d.makeBuffer(8)
            d.submit {
                blit {
                    copy(upload, texture, TextureRegion(size = Size(4, 4), sliceCount = 2))
                    generateMipmaps(texture)
                    copy(
                        texture,
                        result,
                        TextureRegion(size = Size(1, 1), level = 2, sliceCount = 2),
                    )
                }
            }
            assertArrayEquals(byteArrayOf(32, 32, 32, -1, -64, -64, -64, -1), result.readBytes(8))
            val view =
                texture.makeTextureView(
                    textureType = TextureType.TYPE_2D,
                    level = 1,
                    levelCount = 1,
                    slice = 1,
                    sliceCount = 1,
                )
            assertEquals(Size(2, 2), view.sizeAtLevel())
            val destination =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage =
                            setOf(TextureUsage.TRANSFER_SOURCE, TextureUsage.TRANSFER_DESTINATION),
                    )
                )
            val output = d.makeBuffer(16)
            d.submit {
                blit {
                    copy(view, destination)
                    copy(destination, output)
                }
            }
            assertEquals(192, output.readBytes(16)[0].toInt() and 255)
            assertThrows(IllegalArgumentException::class.java) {
                texture.makeTextureView(level = 3)
            }
        }

    @Test
    fun indexedMsaaVertexInputResolveAndScissor(): Unit =
        device().use { d ->
            val vertex = d.makeBuffer(24, usage = setOf(BufferUsage.VERTEX))
            vertex.write(floats(-1f, -1f, 3f, -1f, -1f, 3f))
            val index = d.makeBuffer(8, usage = setOf(BufferUsage.INDEX))
            index.write(
                ByteBuffer.allocateDirect(8)
                    .order(ByteOrder.nativeOrder())
                    .putShort(0)
                    .putShort(0)
                    .putShort(1)
                    .putShort(2)
                    .apply { flip() }
            )
            val color =
                d.makeTexture(
                    TextureDescriptor(
                        8,
                        8,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT),
                        storageMode = StorageMode.MEMORYLESS,
                        sampleCount = 4,
                    )
                )
            val depth =
                d.makeTexture(
                    TextureDescriptor(
                        8,
                        8,
                        PixelFormat.DEPTH16_UNORM,
                        setOf(TextureUsage.DEPTH_ATTACHMENT),
                        StorageMode.MEMORYLESS,
                        sampleCount = 4,
                    )
                )
            val resolve =
                d.makeTexture(
                    TextureDescriptor(
                        8,
                        8,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val pipeline =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("attribute.vert.spv"),
                        d.function("solid.frag.spv", FunctionConstants().setFloat(0, 0.5f)),
                        sampleCount = 4,
                        depthFormat = PixelFormat.DEPTH16_UNORM,
                        vertexBuffers = listOf(VertexBufferLayout(0, 8)),
                        vertexAttributes = listOf(VertexAttribute(0, 0, PixelFormat.RG32_FLOAT)),
                    )
                )
            val output = d.makeBuffer(256)
            d.submit {
                render(
                    RenderPassDescriptor(
                        ColorAttachment(
                            color,
                            storeAction = StoreAction.DONT_CARE,
                            resolveTexture = resolve,
                        ),
                        DepthAttachment(depth),
                    )
                ) {
                    setRenderPipelineState(pipeline)
                    setVertexBuffer(vertex, 0)
                    setScissorRect(ScissorRect(0, 0, 4, 8))
                    drawIndexedPrimitives(index, 3, indexBufferOffset = 2)
                }
                blit { copy(resolve, output) }
            }
            val bytes = output.readBytes(256)
            assertTrue((bytes[0].toInt() and 255) in 127..128)
            assertEquals(64, bytes[1].toInt() and 255)
            assertEquals(0, bytes[7 * 4].toInt())
            assertThrows(IllegalArgumentException::class.java) {
                d.submit { blit { copy(color, output) } }
            }
        }

    @Test
    fun mrtAndIndirectDraw() =
        device().use { d ->
            val vertex = d.makeBuffer(24, usage = setOf(BufferUsage.VERTEX))
            vertex.write(floats(-1f, -1f, 3f, -1f, -1f, 3f))
            val indirect = d.makeBuffer(16, usage = setOf(BufferUsage.INDIRECT))
            indirect.write(
                ByteBuffer.allocateDirect(16)
                    .order(ByteOrder.nativeOrder())
                    .putInt(3)
                    .putInt(1)
                    .putInt(0)
                    .putInt(0)
                    .apply { flip() }
            )
            val colors =
                List(2) {
                    d.makeTexture(
                        TextureDescriptor(
                            4,
                            4,
                            usage =
                                setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                        )
                    )
                }
            val pipeline =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("attribute.vert.spv"),
                        d.function("mrt.frag.spv"),
                        colorAttachments = List(2) { RenderColorAttachmentDescriptor() },
                        vertexBuffers = listOf(VertexBufferLayout(0, 8)),
                        vertexAttributes = listOf(VertexAttribute(0, 0, PixelFormat.RG32_FLOAT)),
                    )
                )
            val output = List(2) { d.makeBuffer(64) }
            d.submit {
                render(RenderPassDescriptor(colors.map { ColorAttachment(it) })) {
                    setRenderPipelineState(pipeline)
                    setVertexBuffer(vertex, 0)
                    drawPrimitives(indirect)
                }
                blit { colors.indices.forEach { copy(colors[it], output[it]) } }
            }
            assertArrayEquals(byteArrayOf(-1, 0, 0, -1), output[0].readBytes(4))
            assertArrayEquals(byteArrayOf(0, -1, 0, -1), output[1].readBytes(4))
        }

    @Test
    fun functionConstantsDescriptorArraysAndIndirectCompute() =
        device().use { d ->
            val pipeline =
                d.makeComputePipelineState(
                    d.function("specialized.comp.spv", FunctionConstants().setInt(0, 7))
                )
            val inputs = List(2) { d.makeBuffer(4) }
            inputs.forEach {
                it.write(
                    ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder()).putInt(3).apply {
                        flip()
                    }
                )
            }
            val indirect = d.makeBuffer(12, usage = setOf(BufferUsage.INDIRECT))
            indirect.write(
                ByteBuffer.allocateDirect(12)
                    .order(ByteOrder.nativeOrder())
                    .putInt(1)
                    .putInt(1)
                    .putInt(1)
                    .apply { flip() }
            )
            d.submit {
                compute {
                    setComputePipelineState(pipeline)
                    setBuffer(inputs[0], 0, arrayElement = 0)
                    setBuffer(inputs[1], 0, arrayElement = 1)
                    dispatchThreadgroups(indirect)
                }
            }
            assertEquals(
                21,
                ByteBuffer.wrap(inputs[0].readBytes(4)).order(ByteOrder.nativeOrder()).int,
            )
            assertEquals(
                10,
                ByteBuffer.wrap(inputs[1].readBytes(4)).order(ByteOrder.nativeOrder()).int,
            )
        }

    @Test
    fun rayQueryBuildAndTraceOrRejectUnsupportedDevice() {
        val available = device().use { Feature.RAY_QUERY in it.capabilities.availableFeatures }
        if (!available) {
            assertThrows(IllegalStateException::class.java) { device(setOf(Feature.RAY_QUERY)) }
            return
        }
        device(setOf(Feature.RAY_QUERY)).use { d ->
            val vertices =
                d.makeBuffer(
                    36,
                    usage =
                        setOf(
                            BufferUsage.ACCELERATION_STRUCTURE_INPUT,
                            BufferUsage.SHADER_DEVICE_ADDRESS,
                        ),
                )
            vertices.write(floats(-1f, -1f, 0f, 1f, -1f, 0f, 0f, 1f, 0f))
            val primitive =
                d.makePrimitiveAccelerationStructure(
                    listOf(TriangleGeometry(vertices, 3)),
                    allowRefit = true,
                )
            val scene =
                d.makeInstanceAccelerationStructure(
                    listOf(AccelerationStructureInstance(primitive))
                )
            val result = d.makeBuffer(4)
            val pipeline = d.makeComputePipelineState(d.function("query.comp.spv"))
            d.submit {
                accelerationStructure {
                    build(primitive)
                    refit(primitive)
                    build(scene)
                }
                compute {
                    setComputePipelineState(pipeline)
                    setAccelerationStructure(scene, 0)
                    setBuffer(result, 1)
                    dispatchThreads(Size(1))
                }
            }
            assertEquals(1, ByteBuffer.wrap(result.readBytes(4)).order(ByteOrder.nativeOrder()).int)
        }
    }

    @Test
    fun heapTexelCountersAndPipelineArchive() =
        device().use { d ->
            val heap = d.makeHeap(1024 * 1024, StorageMode.SHARED)
            val data =
                heap.makeBuffer(
                    16,
                    setOf(
                        BufferUsage.STORAGE_TEXEL,
                        BufferUsage.TRANSFER_SOURCE,
                        BufferUsage.TRANSFER_DESTINATION,
                    ),
                )
            val view = data.makeTextureBuffer(PixelFormat.R32_UINT, writable = true)
            val pipeline = d.makeComputePipelineState(d.function("texel.comp.spv"))
            val counters = d.makeCounterSampleBuffer(2)
            assertNull(counters.read())
            heap.close()
            d.submit {
                sampleCounters(counters, 0)
                blit { fill(data, 0) }
                compute {
                    setComputePipelineState(pipeline)
                    setTextureBuffer(view, 0)
                    dispatchThreads(Size(1))
                }
                sampleCounters(counters, 1)
            }
            assertEquals(42, ByteBuffer.wrap(data.readBytes(4)).order(ByteOrder.nativeOrder()).int)
            val times = checkNotNull(counters.read())
            assertTrue(times[1] >= times[0])
            val archive = d.serializePipelineCache()
            assertTrue(archive.size >= 32)
            d.loadPipelineCache(archive)
            assertThrows(IllegalArgumentException::class.java) {
                d.loadPipelineCache(byteArrayOf(0))
            }
            Unit
        }

    @Test
    fun sharedEventGpuSignalAndHostSignal() =
        device(setOf(Feature.TIMELINE_SEMAPHORE)).use { d ->
            val event = d.makeSharedEvent()
            val buffer = d.makeBuffer(4)
            val queue = d.makeCommandQueue()
            val c = queue.makeCommandBuffer()
            c.waitForEvent(event, 1)
            c.blit { fill(buffer, 9) }
            c.signalEventOnCompletion(event, 2)
            c.commit()
            assertFalse(c.waitUntilCompleted(0))
            event.signal(1)
            assertTrue(c.waitUntilCompleted())
            assertTrue(event.wait(2, 0))
            assertArrayEquals(byteArrayOf(9, 9, 9, 9), buffer.readBytes(4))
            c.close()
            queue.close()
        }

    @Test
    fun specializedWorkgroupAndSeparateSampler() =
        device().use { d ->
            val p =
                d.makeComputePipelineState(
                    d.function("local_size.comp.spv", FunctionConstants().setInt(0, 8))
                )
            assertEquals(Size(8), p.threadgroupSize)
            val b = d.makeBuffer(32)
            d.submit {
                compute {
                    setComputePipelineState(p)
                    setBuffer(b, 0)
                    dispatchThreadgroups(Size(1))
                }
            }
            val v = ByteBuffer.wrap(b.readBytes(32)).order(ByteOrder.nativeOrder())
            repeat(8) { assertEquals(8, v.int) }
            val source = d.makeTexture(TextureDescriptor(2, 2))
            val upload = d.makeBuffer(16)
            upload.write(ByteArray(16) { 127 })
            val target =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val sampler = d.makeSampler()
            val pipeline =
                d.makeRenderPipelineState(
                    d.function("fullscreen.vert.spv"),
                    d.function("separate.frag.spv"),
                )
            val visibility = d.makeCounterSampleBuffer(1, timestamp = false)
            val output = d.makeBuffer(16)
            d.submit {
                blit { copy(upload, source) }
                render(RenderPassDescriptor(ColorAttachment(target))) {
                    setRenderPipelineState(pipeline)
                    setTexture(source, 0)
                    setSampler(sampler, 1)
                    setVisibilityResult(visibility)
                    drawPrimitives(3)
                }
                blit { copy(target, output) }
            }
            assertArrayEquals(ByteArray(16) { 127 }, output.readBytes(16))
            assertTrue(checkNotNull(visibility.read())[0] > 0)
        }

    @Test
    fun tessellationAndMeshShaderDraws() {
        val available = device().use { it.capabilities.availableFeatures }
        for (feature in listOf(Feature.TESSELLATION, Feature.MESH_SHADER)) {
            if (feature !in available) {
                assertThrows(IllegalStateException::class.java) { device(setOf(feature)) }
                continue
            }
            device(setOf(feature)).use { d ->
                val mesh = feature == Feature.MESH_SHADER
                val pipeline =
                    d.makeRenderPipelineState(
                        RenderPipelineDescriptor(
                            d.function(if (mesh) "fullscreen.mesh.spv" else "fullscreen.vert.spv"),
                            d.function("solid.frag.spv"),
                            primitiveType =
                                if (mesh) PrimitiveType.TRIANGLE else PrimitiveType.PATCH,
                            tessellationControlFunction =
                                if (mesh) null else d.function("passthrough.tesc.spv"),
                            tessellationEvaluationFunction =
                                if (mesh) null else d.function("passthrough.tese.spv"),
                            meshShader = mesh,
                        )
                    )
                val color =
                    d.makeTexture(
                        TextureDescriptor(
                            4,
                            4,
                            usage =
                                setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                        )
                    )
                val output = d.makeBuffer(64)
                d.submit {
                    render(RenderPassDescriptor(ColorAttachment(color))) {
                        setRenderPipelineState(pipeline)
                        if (mesh) drawMeshThreadgroups(Size(1)) else drawPrimitives(3)
                    }
                    blit { copy(color, output) }
                }
                assertEquals(255, output.readBytes(4)[0].toInt() and 255)
            }
        }
    }

    @Test
    fun multiviewRendersDistinctArrayLayers() {
        val available = device().use { Feature.MULTIVIEW in it.capabilities.availableFeatures }
        assumeTrue(available)
        device(setOf(Feature.MULTIVIEW)).use { d ->
            val texture =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                        arrayLength = 2,
                        textureType = TextureType.TYPE_2D_ARRAY,
                    )
                )
            val pipeline =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("multiview.vert.spv"),
                        d.function("multiview.frag.spv"),
                        viewMask = 3,
                    )
                )
            val output = d.makeBuffer(128)
            d.submit {
                render(RenderPassDescriptor(listOf(ColorAttachment(texture)), viewMask = 3)) {
                    setRenderPipelineState(pipeline)
                    drawPrimitives(3)
                }
                blit { copy(texture, output, TextureRegion(size = Size(4, 4), sliceCount = 2)) }
            }
            assertArrayEquals(byteArrayOf(-1, 0, 0, -1), output.readBytes(4))
            assertArrayEquals(byteArrayOf(0, -1, 0, -1), output.readBytes(4, offset = 64))
        }
    }

    @Test
    fun runtimeDescriptorArrayUsesExplicitCapacity() {
        val available =
            device().use { Feature.DESCRIPTOR_INDEXING in it.capabilities.availableFeatures }
        assumeTrue(available)
        device(setOf(Feature.DESCRIPTOR_INDEXING)).use { d ->
            val function = d.function("runtime.comp.spv")
            assertThrows(IllegalArgumentException::class.java) {
                d.makeComputePipelineState(function)
            }
            val pipeline =
                d.makeComputePipelineState(
                    function,
                    listOf(BindingLayout(0, BindingType.STORAGE_BUFFER, count = 8)),
                )
            val output = d.makeBuffer(4)
            d.submit {
                compute {
                    setComputePipelineState(pipeline)
                    setBuffer(output, 0, arrayElement = 7)
                    setBytes(
                        ByteBuffer.allocate(4).order(ByteOrder.nativeOrder()).putInt(7).array()
                    )
                    dispatchThreads(Size(1))
                }
            }
            assertEquals(
                73,
                ByteBuffer.wrap(output.readBytes(4)).order(ByteOrder.nativeOrder()).int,
            )
        }
    }

    @Test
    fun rayPipelineBuildsAndTracesOnSupportedDevice() {
        val available =
            device().use { Feature.RAY_TRACING_PIPELINE in it.capabilities.availableFeatures }
        assumeTrue(available)
        device(setOf(Feature.RAY_TRACING_PIPELINE)).use { d ->
            val vertices =
                d.makeBuffer(
                    36,
                    usage =
                        setOf(
                            BufferUsage.ACCELERATION_STRUCTURE_INPUT,
                            BufferUsage.SHADER_DEVICE_ADDRESS,
                        ),
                )
            vertices.write(floats(-1f, -1f, 0f, 1f, -1f, 0f, 0f, 1f, 0f))
            val primitive =
                d.makePrimitiveAccelerationStructure(listOf(TriangleGeometry(vertices, 3)))
            val scene =
                d.makeInstanceAccelerationStructure(
                    listOf(AccelerationStructureInstance(primitive))
                )
            val pipeline =
                d.makeRayTracingPipelineState(
                    RayTracingPipelineDescriptor(
                        listOf(
                            RayShader(d.function("hit.rgen.spv"), RayShaderStage.RAY_GENERATION),
                            RayShader(d.function("hit.rmiss.spv"), RayShaderStage.MISS),
                            RayShader(d.function("hit.rchit.spv"), RayShaderStage.CLOSEST_HIT),
                        ),
                        listOf(
                            RayShaderGroup(general = 0),
                            RayShaderGroup(general = 1),
                            RayShaderGroup(closestHit = 2),
                        ),
                    )
                )
            val output = d.makeBuffer(4)
            d.submit {
                accelerationStructure {
                    build(primitive)
                    build(scene)
                }
                rayTracing {
                    setRayTracingPipelineState(pipeline)
                    setAccelerationStructure(scene, 0)
                    setBuffer(output, 1)
                    traceRays(Size(1))
                }
            }
            assertEquals(1, ByteBuffer.wrap(output.readBytes(4)).order(ByteOrder.nativeOrder()).int)
        }
    }

    @Test
    fun optionalCoreStatesAndSamplerReduction() {
        val available = device().use { it.capabilities.availableFeatures }
        val selected =
            setOf(
                    Feature.SAMPLE_RATE_SHADING,
                    Feature.ALPHA_TO_ONE,
                    Feature.MULTI_VIEWPORT,
                    Feature.SAMPLER_MIN_MAX,
                )
                .intersect(available)
        device(selected).use { d ->
            val msaa =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT),
                        storageMode = StorageMode.MEMORYLESS,
                        sampleCount = 4,
                    )
                )
            val resolve =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val count = if (Feature.MULTI_VIEWPORT in selected) 2 else 1
            val pipeline =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("solid.frag.spv"),
                        sampleCount = 4,
                        viewportCount = count,
                        sampleShadingEnabled = Feature.SAMPLE_RATE_SHADING in selected,
                        alphaToOneEnabled = Feature.ALPHA_TO_ONE in selected,
                    )
                )
            val output = d.makeBuffer(64)
            d.submit {
                render(
                    RenderPassDescriptor(
                        ColorAttachment(
                            msaa,
                            storeAction = StoreAction.DONT_CARE,
                            resolveTexture = resolve,
                        )
                    )
                ) {
                    setRenderPipelineState(pipeline)
                    setViewports(List(count) { Viewport(0f, 4f, 4f, -4f) })
                    setScissorRects(List(count) { ScissorRect(0, 0, 4, 4) })
                    drawPrimitives(3)
                }
                blit { copy(resolve, output) }
            }
            assertEquals(255, output.readBytes(4)[0].toInt() and 255)
            if (Feature.SAMPLER_MIN_MAX in selected)
                d.makeSampler(SamplerDescriptor(reductionMode = SamplerReductionMode.MIN)).close()
        }
    }
}
