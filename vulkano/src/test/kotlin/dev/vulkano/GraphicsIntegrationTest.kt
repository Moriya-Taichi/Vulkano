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
        makeLibrary(TestShaders.read(file)).makeFunction(constants = constants)

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
    fun immutableSamplersRetainClosedSamplerAndNeedNoDynamicBinding(): Unit =
        device().use { d ->
            val texture =
                d.makeTexture(
                    TextureDescriptor(
                        1,
                        1,
                        usage = setOf(TextureUsage.SAMPLED, TextureUsage.TRANSFER_DESTINATION),
                    )
                )
            val upload = d.makeBuffer(4).apply { write(byteArrayOf(21, 42, 63, -1)) }
            val output = d.makeBuffer(4)
            val target =
                d.makeTexture(
                    TextureDescriptor(
                        1,
                        1,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            d.submit { blit { copy(upload, texture) } }
            for (separate in listOf(false, true)) {
                val sampler = d.makeSampler()
                val bindings =
                    if (separate)
                        listOf(
                            BindingLayout(0, BindingType.SAMPLED_IMAGE),
                            BindingLayout(1, BindingType.SAMPLER, immutableSampler = sampler),
                        )
                    else
                        listOf(
                            BindingLayout(
                                0,
                                BindingType.SAMPLED_TEXTURE,
                                immutableSampler = sampler,
                            )
                        )
                val pipeline =
                    d.makeRenderPipelineState(
                        d.function("fullscreen.vert.spv"),
                        d.function(if (separate) "separate.frag.spv" else "sample.frag.spv"),
                        bindings = bindings,
                    )
                sampler.close()
                d.submit {
                    render(RenderPassDescriptor(listOf(ColorAttachment(target)))) {
                        setRenderPipelineState(pipeline)
                        setTexture(texture, 0)
                        drawPrimitives(3)
                    }
                    blit { copy(target, output) }
                }
                assertArrayEquals(byteArrayOf(21, 42, 63, -1), output.readBytes(4))
            }
        }

    @Test
    fun externalSyncRequiresFeatureAndOwnsFd(): Unit =
        device().use { d ->
            SyncFd.adopt(-1).use { signalled ->
                signalled.duplicate().use { copy -> assertEquals(-1, copy.detach()) }
                assertThrows(IllegalArgumentException::class.java) { d.importSyncFd(signalled) }
            }
            assertThrows(IllegalArgumentException::class.java) { d.makeExternalSemaphore() }
            val closed = SyncFd.adopt(-1)
            closed.close()
            closed.close()
            assertThrows(IllegalStateException::class.java) { closed.detach() }
        }

    @Test
    fun externalSyncSignalExportImportAndWait(): Unit {
        assumeTrue(device().use { Feature.EXTERNAL_SYNC_FD in it.capabilities.availableFeatures })
        device(setOf(Feature.EXTERNAL_SYNC_FD)).use { d ->
            val signal = d.makeExternalSemaphore()
            assertThrows(IllegalArgumentException::class.java) { signal.exportSyncFd() }
            val source = d.makeBuffer(4)
            val output = d.makeBuffer(4)
            val first = d.makeCommandQueue().makeCommandBuffer()
            first.blit { fill(source, 37) }
            first.signalExternalSemaphore(signal)
            first.commit()
            signal.exportSyncFd().use { fd ->
                val imported = d.importSyncFd(fd)
                d.submit {
                    waitForExternalSemaphore(imported)
                    blit { copy(source, output) }
                }
                assertArrayEquals(ByteArray(4) { 37 }, output.readBytes(4))
                assertThrows(IllegalArgumentException::class.java) {
                    d.submit { waitForExternalSemaphore(imported) }
                }
            }
            assertThrows(IllegalArgumentException::class.java) { signal.exportSyncFd() }
            first.waitUntilCompleted()
            first.close()
            SyncFd.adopt(-1).use { fd ->
                val imported = d.importSyncFd(fd)
                d.submit { waitForExternalSemaphore(imported) }
            }
        }
    }

    @Test
    fun sparseResourcesRequireExplicitFeature(): Unit =
        device().use { d ->
            val caps = d.sparseCapabilities()
            assertEquals(
                Feature.SPARSE_RESOURCES in d.capabilities.availableFeatures,
                caps.supported,
            )
            assertThrows(IllegalArgumentException::class.java) { d.makeSparseBuffer(4096) }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeSparseTexture(TextureDescriptor(32, 32))
            }
        }

    @Test
    fun sparseBufferMappingCopyAndUnmap(): Unit {
        assumeTrue(device().use { it.sparseCapabilities().let { c -> c.supported && c.buffers } })
        device(setOf(Feature.SPARSE_RESOURCES)).use { d ->
            val source = d.makeSparseBuffer(1024 * 1024)
            assertFalse(source.isResident(0))
            assertEquals(0L, source.allocatedBytes)
            source.setResident(0)
            assertTrue(source.isResident(0))
            val output = d.makeBuffer(4)
            d.submit {
                blit {
                    fill(source.buffer, 53, length = 4)
                    copy(source.buffer, output, length = 4)
                }
            }
            assertArrayEquals(ByteArray(4) { 53 }, output.readBytes(4))
            if (d.sparseCapabilities().aliasedMappings) {
                val alias = d.makeSparseBuffer(1024 * 1024)
                alias.copyMappings(source, 0, 0)
                source.setResident(0, resident = false)
                source.close()
                d.submit { blit { copy(alias.buffer, output, length = 4) } }
                assertArrayEquals(ByteArray(4) { 53 }, output.readBytes(4))
                alias.close()
            } else {
                source.setResident(0, resident = false)
                assertFalse(source.isResident(0))
                assertEquals(0L, source.allocatedBytes)
            }
        }
    }

    @Test
    fun sparseTextureTilesTailAndShaderResidency(): Unit {
        assumeTrue(
            device().use { it.sparseCapabilities().let { c -> c.supported && c.textures2D } }
        )
        device(setOf(Feature.SPARSE_RESOURCES)).use { d ->
            val descriptor =
                TextureDescriptor(
                    512,
                    512,
                    mipLevels = 10,
                    usage =
                        setOf(
                            TextureUsage.TRANSFER_SOURCE,
                            TextureUsage.TRANSFER_DESTINATION,
                            TextureUsage.SAMPLED,
                        ),
                )
            val sparse = d.makeSparseTexture(descriptor)
            val output = d.makeBuffer(4)
            val upload =
                d.makeBuffer(512 * 512 * 4).apply { write(ByteArray(512 * 512 * 4) { 41 }) }
            if (sparse.mipTailFirstLevel > 0) {
                val region =
                    TextureRegion(
                        size =
                            Size(
                                minOf(512, sparse.tileSize.width),
                                minOf(512, sparse.tileSize.height),
                            )
                    )
                assertFalse(sparse.isResident(region))
                sparse.setResident(region)
                assertTrue(sparse.isResident(region))
                assertTrue(sparse.allocatedBytes > sparse.metadataBytes)
                d.submit {
                    blit {
                        copy(upload, sparse.texture, region)
                        copy(sparse.texture, output, TextureRegion(size = Size(1, 1)))
                    }
                }
                assertArrayEquals(ByteArray(4) { 41 }, output.readBytes(4))
                if (d.sparseCapabilities().shaderResidency) {
                    val result = d.makeBuffer(8)
                    val pipeline = d.makeComputePipelineState(d.function("sparse.comp.spv"))
                    d.submit {
                        compute {
                            setComputePipelineState(pipeline)
                            setTexture(sparse.texture, 0, d.makeSampler())
                            setBuffer(result, 1)
                            dispatchThreads(Size(1))
                        }
                    }
                    val values = ByteBuffer.wrap(result.readBytes(8)).order(ByteOrder.nativeOrder())
                    assertEquals(1, values.int)
                    assertEquals(41, values.int)
                }
                if (d.sparseCapabilities().aliasedMappings) {
                    val alias = d.makeSparseTexture(descriptor)
                    alias.copyMappings(sparse, region, region)
                    sparse.setResident(region, false)
                    d.submit {
                        blit { copy(alias.texture, output, TextureRegion(size = Size(1, 1))) }
                    }
                    assertArrayEquals(ByteArray(4) { 41 }, output.readBytes(4))
                    alias.close()
                } else sparse.setResident(region, false)
            }
            if (sparse.mipTailFirstLevel < descriptor.mipLevels) {
                sparse.setMipTailResident()
                val tail = TextureRegion(size = Size(1, 1), level = 9)
                d.submit {
                    blit {
                        copy(upload, sparse.texture, tail)
                        copy(sparse.texture, output, tail)
                    }
                }
                assertArrayEquals(ByteArray(4) { 41 }, output.readBytes(4))
                assertTrue(sparse.isResident(tail))
                sparse.setMipTailResident(resident = false)
                assertFalse(sparse.isResident(tail))
            }
            assertEquals(sparse.metadataBytes, sparse.allocatedBytes)
        }
    }

    @Test
    fun placedBuffersAliasAndRejectInvalidRanges(): Unit =
        device().use { d ->
            val requirements = d.heapBufferRequirements(64)
            val heap =
                d.makePlacementHeap(requirements.size * 2, listOf(requirements), StorageMode.SHARED)
            val a = heap.makeBuffer(64, offset = 0)
            val b = heap.makeBuffer(64, offset = 0)
            val other = heap.makeBuffer(64, offset = requirements.size)
            assertThrows(IllegalArgumentException::class.java) { heap.makeBuffer(64, offset = 1) }
            assertThrows(IllegalArgumentException::class.java) {
                heap.makeBuffer(64, offset = heap.size)
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.submit { blit { copy(a, b, length = 64) } }
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.submit { blit { aliasResources(a, other) } }
            }
            heap.close()
            d.submit {
                blit {
                    fill(a, 19)
                    aliasResources(a, b)
                    copy(b, other, length = 64)
                }
            }
            assertArrayEquals(ByteArray(64) { 19 }, other.readBytes(64))
            b.write(ByteArray(64) { 27 })
            assertArrayEquals(ByteArray(64) { 27 }, a.readBytes(64))
        }

    @Test
    fun placedTexturesSwitchWithDiscardAndReadback(): Unit =
        device().use { d ->
            val descriptor =
                TextureDescriptor(
                    2,
                    2,
                    usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                )
            val requirements = d.heapTextureRequirements(descriptor)
            val heap = d.makePlacementHeap(requirements.size, listOf(requirements))
            val a = heap.makeTexture(descriptor, offset = 0)
            val b = heap.makeTexture(descriptor, offset = 0)
            val output = d.makeBuffer(48)
            heap.close()
            fun pass(t: Texture, color: ClearColor) =
                RenderPassDescriptor(listOf(ColorAttachment(t, clearColor = color)))
            d.submit {
                render(pass(a, ClearColor(1f, 0f, 0f, 1f)))
                blit {
                    copy(a, output, destinationOffset = 0)
                    aliasResources(a, b)
                }
                render(pass(b, ClearColor(0f, 1f, 0f, 1f)))
                blit {
                    copy(b, output, destinationOffset = 16)
                    aliasResources(b, a)
                }
                render(pass(a, ClearColor(0f, 0f, 1f, 1f)))
                blit { copy(a, output, destinationOffset = 32) }
            }
            val bytes = output.readBytes(48)
            for (channel in 0..2) for (pixel in 0..3) {
                for (component in 0..3) assertEquals(
                    if (component == channel || component == 3) 255 else 0,
                    bytes[channel * 16 + pixel * 4 + component].toInt() and 255,
                )
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.submit {
                    blit {
                        aliasResources(a, b)
                        copy(b, output)
                    }
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
            val counters =
                if (d.counterCapabilities().timestampValidBits > 0) d.makeCounterSampleBuffer(2)
                else null
            assertNull(counters?.read())
            heap.close()
            d.submit {
                if (counters != null) sampleCounters(counters, 0)
                blit { fill(data, 0) }
                compute {
                    setComputePipelineState(pipeline)
                    setTextureBuffer(view, 0)
                    dispatchThreads(Size(1))
                }
                if (counters != null) sampleCounters(counters, 1)
            }
            assertEquals(42, ByteBuffer.wrap(data.readBytes(4)).order(ByteOrder.nativeOrder()).int)
            if (counters != null) {
                assertNotNull(counters.read())
                assertTrue(checkNotNull(counters.elapsedNanos(0, 1)) >= 0)
            }
            val archive = d.serializePipelineCache()
            assertTrue(archive.size >= 32)
            d.loadPipelineCache(archive)
            assertThrows(IllegalArgumentException::class.java) {
                d.loadPipelineCache(byteArrayOf(0))
            }
            Unit
        }

    @Test
    fun sharedEventGpuSignalAndHostSignal(): Unit {
        assumeTrue(device().use { Feature.TIMELINE_SEMAPHORE in it.capabilities.availableFeatures })
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

    @Test
    fun depthResolvePreservesDepthValues() {
        assumeTrue(
            device().use { Feature.DEPTH_STENCIL_RESOLVE in it.capabilities.availableFeatures }
        )
        device(setOf(Feature.DEPTH_STENCIL_RESOLVE)).use { d ->
            assertTrue(ResolveMode.SAMPLE_ZERO in d.depthStencilResolveSupport().depthModes)
            val depth =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        PixelFormat.DEPTH32_FLOAT,
                        setOf(TextureUsage.DEPTH_ATTACHMENT),
                        StorageMode.MEMORYLESS,
                        sampleCount = 4,
                    )
                )
            val resolved =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        PixelFormat.DEPTH32_FLOAT,
                        setOf(TextureUsage.DEPTH_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val color =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT),
                        storageMode = StorageMode.MEMORYLESS,
                        sampleCount = 4,
                    )
                )
            val pipeline =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("solid.frag.spv"),
                        sampleCount = 4,
                        depthFormat = PixelFormat.DEPTH32_FLOAT,
                    )
                )
            val output = d.makeBuffer(64)
            d.submit {
                render(
                    RenderPassDescriptor(
                        ColorAttachment(color, storeAction = StoreAction.DONT_CARE),
                        DepthAttachment(depth, resolveTexture = resolved),
                    )
                ) {
                    setRenderPipelineState(pipeline)
                    drawPrimitives(3)
                }
                blit { copy(resolved, output) }
            }
            val pixels =
                ByteBuffer.wrap(output.readBytes(64)).order(ByteOrder.nativeOrder()).asFloatBuffer()
            repeat(16) { assertEquals(0.5f, pixels.get(), 0.0001f) }
        }
    }

    @Test
    fun integerClearPreservesAll32Bits() =
        device().use { d ->
            for (format in listOf(PixelFormat.R32_UINT, PixelFormat.R32_SINT)) {
                val texture =
                    d.makeTexture(
                        TextureDescriptor(
                            2,
                            2,
                            format,
                            setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                        )
                    )
                val output = d.makeBuffer(16)
                val value = if (format == PixelFormat.R32_UINT) -1 else Int.MIN_VALUE
                d.submit {
                    render(
                        RenderPassDescriptor(
                            ColorAttachment(texture, clearIntegerColor = ClearIntegerColor(value))
                        )
                    ) {}
                    blit { copy(texture, output) }
                }
                val data =
                    ByteBuffer.wrap(output.readBytes(16))
                        .order(ByteOrder.nativeOrder())
                        .asIntBuffer()
                repeat(4) { assertEquals(value, data.get()) }
            }
        }

    @Test
    fun missingAndMistypedVertexAttributesAreRejected(): Unit =
        device().use { d ->
            val v = d.function("attribute.vert.spv")
            val f = d.function("solid.frag.spv")
            assertThrows(IllegalArgumentException::class.java) {
                d.makeRenderPipelineState(RenderPipelineDescriptor(v, f))
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        v,
                        f,
                        vertexBuffers = listOf(VertexBufferLayout(0, 8)),
                        vertexAttributes = listOf(VertexAttribute(0, 0, PixelFormat.RG32_UINT)),
                    )
                )
            }
        }

    @Test
    fun pipelineShadingRateUsesSupportedSize() {
        assumeTrue(
            device().use { Feature.FRAGMENT_SHADING_RATE in it.capabilities.availableFeatures }
        )
        device(setOf(Feature.FRAGMENT_SHADING_RATE)).use { d ->
            val rate =
                d.fragmentShadingRates().firstOrNull {
                    1 in it.sampleCounts && it.fragmentSize.width > 1
                } ?: return@use
            val texture =
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
                        d.function("fullscreen.vert.spv"),
                        d.function("solid.frag.spv"),
                        fragmentSize = rate.fragmentSize,
                    )
                )
            val output = d.makeBuffer(256)
            d.submit {
                render(RenderPassDescriptor(ColorAttachment(texture))) {
                    setRenderPipelineState(pipeline)
                    drawPrimitives(3)
                }
                blit { copy(texture, output) }
            }
            val data = output.readBytes(256)
            repeat(64) { assertEquals(255, data[it * 4].toInt() and 255) }
        }
    }

    @Test
    fun accelerationCopiesAndCompactionRetainInstances() {
        assumeTrue(device().use { Feature.RAY_QUERY in it.capabilities.availableFeatures })
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
            val source =
                d.makePrimitiveAccelerationStructure(
                    listOf(TriangleGeometry(vertices, 3)),
                    allowCompaction = true,
                )
            d.submit { accelerationStructure { build(source) } }
            val clone = source.makeCopyDestination()
            val compact = source.makeCopyDestination(compact = true)
            assertTrue(compact.allocatedSize <= source.allocatedSize)
            d.submit {
                accelerationStructure {
                    copy(source, clone)
                    copy(source, compact)
                }
            }
            source.close()
            vertices.close()
            for (structure in listOf(clone, compact)) {
                val scene =
                    d.makeInstanceAccelerationStructure(
                        listOf(AccelerationStructureInstance(structure))
                    )
                val output = d.makeBuffer(4)
                val pipeline = d.makeComputePipelineState(d.function("query.comp.spv"))
                d.submit {
                    accelerationStructure { build(scene) }
                    compute {
                        setComputePipelineState(pipeline)
                        setAccelerationStructure(scene, 0)
                        setBuffer(output, 1)
                        dispatchThreads(Size(1))
                    }
                }
                assertEquals(
                    1,
                    ByteBuffer.wrap(output.readBytes(4)).order(ByteOrder.nativeOrder()).int,
                )
            }
        }
    }

    @Test
    fun nonOverlappingCopiesWithinAResource() =
        device().use { d ->
            val buffer = d.makeBuffer(16)
            buffer.write(byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0))
            d.submit {
                blit { copy(buffer, buffer, length = 8, sourceOffset = 0, destinationOffset = 8) }
            }
            assertArrayEquals(byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8), buffer.readBytes(8, offset = 8))
            val texture =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage =
                            setOf(TextureUsage.TRANSFER_SOURCE, TextureUsage.TRANSFER_DESTINATION),
                    )
                )
            d.submit {
                blit {
                    copy(buffer, texture)
                    copy(
                        texture,
                        texture,
                        TextureRegion(size = Size(2, 1)),
                        TextureRegion(origin = Origin(y = 1), size = Size(2, 1)),
                    )
                    copy(texture, buffer)
                }
            }
            assertArrayEquals(buffer.readBytes(8), buffer.readBytes(8, offset = 8))
            assertThrows(IllegalArgumentException::class.java) {
                d.submit { blit { copy(buffer, buffer, length = 8, destinationOffset = 4) } }
            }
            Unit
        }

    @Test
    fun compatibleFormatAndSwizzledViewsKeepParentStorage(): Unit =
        device().use { d ->
            val source =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage =
                            setOf(
                                TextureUsage.SAMPLED,
                                TextureUsage.STORAGE,
                                TextureUsage.TRANSFER_DESTINATION,
                                TextureUsage.TRANSFER_SOURCE,
                            ),
                    )
                )
            val view =
                source.makeTextureView(
                    usage = setOf(TextureUsage.SAMPLED),
                    swizzle =
                        TextureSwizzle(
                            TextureComponent.BLUE,
                            TextureComponent.GREEN,
                            TextureComponent.RED,
                            TextureComponent.ONE,
                        ),
                )
            val raw =
                source.makeTextureView(
                    PixelFormat.R32_UINT,
                    usage = setOf(TextureUsage.TRANSFER_SOURCE),
                )
            val wrongSampleType =
                source.makeTextureView(PixelFormat.R32_UINT, usage = setOf(TextureUsage.SAMPLED))
            assertThrows(IllegalArgumentException::class.java) {
                source.makeTextureView(PixelFormat.R16_UNORM)
            }
            assertThrows(IllegalArgumentException::class.java) {
                source.makeTextureView(PixelFormat.RGBA8_SRGB)
            }
            assertThrows(IllegalArgumentException::class.java) {
                source.makeTextureView(swizzle = TextureSwizzle(red = TextureComponent.BLUE))
            }
            source
                .makeTextureView(PixelFormat.RGBA8_SRGB, usage = setOf(TextureUsage.SAMPLED))
                .close()
            val original = ByteArray(16) { byteArrayOf(16, 64, 200.toByte(), 128.toByte())[it % 4] }
            val upload = d.makeBuffer(16).apply { write(original) }
            val target =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val p =
                d.makeRenderPipelineState(
                    d.function("fullscreen.vert.spv"),
                    d.function("separate.frag.spv"),
                )
            val sampler = d.makeSampler()
            val pixels = d.makeBuffer(16)
            val bytes = d.makeBuffer(16)
            d.submit { blit { copy(upload, source) } }
            source.close()
            assertThrows(IllegalArgumentException::class.java) {
                d.submit {
                    render(RenderPassDescriptor(ColorAttachment(target))) {
                        setRenderPipelineState(p)
                        setTexture(wrongSampleType, 0)
                        setSampler(sampler, 1)
                        drawPrimitives(3)
                    }
                }
            }
            d.submit {
                render(RenderPassDescriptor(ColorAttachment(target))) {
                    setRenderPipelineState(p)
                    setTexture(view, 0)
                    setSampler(sampler, 1)
                    drawPrimitives(3)
                }
                blit {
                    copy(target, pixels)
                    copy(raw, bytes)
                }
            }
            assertArrayEquals(original, bytes.readBytes(16))
            assertArrayEquals(
                ByteArray(16) { byteArrayOf(200.toByte(), 64, 16, 255.toByte())[it % 4] },
                pixels.readBytes(16),
            )
            val transfer =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage =
                            setOf(TextureUsage.TRANSFER_SOURCE, TextureUsage.TRANSFER_DESTINATION),
                    )
                )
            val logicalView = transfer.makeTextureView()
            transfer.close()
            d.submit {
                blit {
                    copy(upload, logicalView)
                    copy(logicalView, pixels)
                }
            }
            assertArrayEquals(original, pixels.readBytes(16))
        }

    @Test
    fun rasterizationRateMapAttachmentDraw(): Unit {
        val available = device().use { it.capabilities.availableFeatures }
        assumeTrue(Feature.ATTACHMENT_SHADING_RATE in available)
        device(setOf(Feature.ATTACHMENT_SHADING_RATE)).use { d ->
            val texel = d.rasterizationRateMapLimits().minimumTexelSize
            val fragment = d.fragmentShadingRates().first { 1 in it.sampleCounts }.fragmentSize
            val width = (16 + texel.width - 1) / texel.width
            val height = (16 + texel.height - 1) / texel.height
            val map =
                d.makeTexture(
                    TextureDescriptor(
                        width,
                        height,
                        PixelFormat.R8_UINT,
                        setOf(
                            TextureUsage.SHADING_RATE_ATTACHMENT,
                            TextureUsage.TRANSFER_DESTINATION,
                        ),
                    )
                )
            val upload =
                d.makeBuffer(maxOf(4, width * height).toLong()).apply {
                    write(
                        ByteArray(maxOf(4, width * height)) {
                            RasterizationRateMap.encode(fragment)
                        }
                    )
                }
            val target =
                d.makeTexture(
                    TextureDescriptor(
                        16,
                        16,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val output = d.makeBuffer(1024)
            val p =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("solid.frag.spv"),
                        rateMapTexelSize = texel,
                    )
                )
            d.submit {
                blit { copy(upload, map) }
                render(
                    RenderPassDescriptor(
                        listOf(ColorAttachment(target)),
                        rasterizationRateMap = RasterizationRateMap(map, texel),
                    )
                ) {
                    setRenderPipelineState(p)
                    drawPrimitives(3)
                }
                blit { copy(target, output) }
            }
            assertEquals(255, output.readBytes(4)[3].toInt() and 255)
        }
    }

    @Test
    fun subpassesPreserveMemorylessInputAcrossIntermediatePass(): Unit =
        device().use { d ->
            for (samples in listOf(1, 4)) {
                val scene =
                    d.makeTexture(
                        TextureDescriptor(
                            4,
                            4,
                            usage =
                                setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.INPUT_ATTACHMENT),
                            storageMode = StorageMode.MEMORYLESS,
                            sampleCount = samples,
                        )
                    )
                val intermediate =
                    d.makeTexture(
                        TextureDescriptor(
                            4,
                            4,
                            usage = setOf(TextureUsage.COLOR_ATTACHMENT),
                            storageMode = StorageMode.MEMORYLESS,
                            sampleCount = samples,
                        )
                    )
                val target =
                    d.makeTexture(
                        TextureDescriptor(
                            4,
                            4,
                            usage =
                                setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                            sampleCount = samples,
                        )
                    )
                val resolved =
                    if (samples == 1) target
                    else
                        d.makeTexture(
                            TextureDescriptor(
                                4,
                                4,
                                usage =
                                    setOf(
                                        TextureUsage.COLOR_ATTACHMENT,
                                        TextureUsage.TRANSFER_SOURCE,
                                    ),
                            )
                        )
                val layout =
                    RenderPassLayout(
                        List(3) { PixelFormat.RGBA8_UNORM },
                        listOf(
                            RenderSubpass(listOf(0)),
                            RenderSubpass(listOf(1)),
                            RenderSubpass(listOf(2), listOf(0)),
                        ),
                        sampleCount = samples,
                        resolveColorAttachments = if (samples > 1) setOf(2) else emptySet(),
                    )
                val pipelines =
                    (0..2).map { sub ->
                        d.makeRenderPipelineState(
                            RenderPipelineDescriptor(
                                d.function("fullscreen.vert.spv"),
                                d.function(
                                    if (sub < 2) "solid.frag.spv"
                                    else if (samples == 1) "input.frag.spv" else "input_ms.frag.spv"
                                ),
                                sampleCount = samples,
                                subpassLayout = layout,
                                subpassIndex = sub,
                            )
                        )
                    }
                val output = d.makeBuffer(64)
                d.submit {
                    render(
                        RenderPassDescriptor(
                            listOf(
                                ColorAttachment(scene, storeAction = StoreAction.DONT_CARE),
                                ColorAttachment(intermediate, storeAction = StoreAction.DONT_CARE),
                                ColorAttachment(
                                    target,
                                    resolveTexture = if (samples > 1) resolved else null,
                                ),
                            ),
                            subpassLayout = layout,
                        )
                    ) {
                        setRenderPipelineState(pipelines[0])
                        drawPrimitives(3)
                        nextSubpass()
                        setRenderPipelineState(pipelines[1])
                        drawPrimitives(3)
                        nextSubpass()
                        setRenderPipelineState(pipelines[2])
                        setTexture(scene, 0)
                        drawPrimitives(3)
                    }
                    blit { copy(resolved, output) }
                }
                assertArrayEquals(
                    ByteArray(64) {
                        byteArrayOf(128.toByte(), 64, 255.toByte(), 255.toByte())[it % 4]
                    },
                    output.readBytes(64),
                )
            }
        }

    @Test
    fun depthInputAttachmentReadsPreviousSubpass(): Unit =
        device().use { d ->
            val depth =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        PixelFormat.DEPTH32_FLOAT,
                        setOf(TextureUsage.DEPTH_ATTACHMENT, TextureUsage.INPUT_ATTACHMENT),
                        StorageMode.MEMORYLESS,
                    )
                )
            val target =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val scene =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT),
                        storageMode = StorageMode.MEMORYLESS,
                    )
                )
            val layout =
                RenderPassLayout(
                    List(2) { PixelFormat.RGBA8_UNORM },
                    listOf(
                        RenderSubpass(listOf(0), usesDepthAttachment = true),
                        RenderSubpass(listOf(1), listOf(2)),
                    ),
                    depthFormat = PixelFormat.DEPTH32_FLOAT,
                )
            val first =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("solid.frag.spv"),
                        depthFormat = PixelFormat.DEPTH32_FLOAT,
                        subpassLayout = layout,
                    )
                )
            val second =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("input_depth.frag.spv"),
                        subpassLayout = layout,
                        subpassIndex = 1,
                    )
                )
            val output = d.makeBuffer(64)
            d.submit {
                render(
                    RenderPassDescriptor(
                        listOf(
                            ColorAttachment(scene, storeAction = StoreAction.DONT_CARE),
                            ColorAttachment(target),
                        ),
                        DepthAttachment(depth),
                        subpassLayout = layout,
                    )
                ) {
                    setRenderPipelineState(first)
                    drawPrimitives(3)
                    nextSubpass()
                    setRenderPipelineState(second)
                    setTexture(depth, 0)
                    drawPrimitives(3)
                }
                blit { copy(target, output) }
            }
            assertArrayEquals(
                ByteArray(64) { byteArrayOf(128.toByte(), 0, 0, 255.toByte())[it % 4] },
                output.readBytes(64),
            )
        }

    @Test
    fun multiviewSubpassReadsMatchingLayer(): Unit {
        assumeTrue(device().use { Feature.MULTIVIEW in it.capabilities.availableFeatures })
        device(setOf(Feature.MULTIVIEW)).use { d ->
            val scene =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.INPUT_ATTACHMENT),
                        storageMode = StorageMode.MEMORYLESS,
                        arrayLength = 2,
                        textureType = TextureType.TYPE_2D_ARRAY,
                    )
                )
            val target =
                d.makeTexture(
                    TextureDescriptor(
                        4,
                        4,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                        arrayLength = 2,
                        textureType = TextureType.TYPE_2D_ARRAY,
                    )
                )
            val layout =
                RenderPassLayout(
                    List(2) { PixelFormat.RGBA8_UNORM },
                    listOf(RenderSubpass(listOf(0)), RenderSubpass(listOf(1), listOf(0))),
                )
            val first =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("multiview.vert.spv"),
                        d.function("multiview.frag.spv"),
                        viewMask = 3,
                        subpassLayout = layout,
                    )
                )
            val second =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("input.frag.spv"),
                        viewMask = 3,
                        subpassLayout = layout,
                        subpassIndex = 1,
                    )
                )
            val output = d.makeBuffer(128)
            d.submit {
                render(
                    RenderPassDescriptor(
                        listOf(
                            ColorAttachment(scene, storeAction = StoreAction.DONT_CARE),
                            ColorAttachment(target),
                        ),
                        viewMask = 3,
                        subpassLayout = layout,
                    )
                ) {
                    setRenderPipelineState(first)
                    drawPrimitives(3)
                    nextSubpass()
                    setRenderPipelineState(second)
                    setTexture(scene, 0)
                    drawPrimitives(3)
                }
                blit { copy(target, output, TextureRegion(size = Size(4, 4), sliceCount = 2)) }
            }
            assertArrayEquals(byteArrayOf(0, 0, -1, -1), output.readBytes(4))
            assertArrayEquals(byteArrayOf(0, -1, 0, -1), output.readBytes(4, offset = 64))
        }
    }

    @Test
    fun tensorViewsRetainBufferAndExposeStrides(): Unit =
        device().use { d ->
            val descriptor = TensorDescriptor(listOf(2, 4), TensorDataType.UINT32)
            assertEquals(32L, descriptor.requiredBytes)
            val tensor = d.makeTensor(descriptor)
            val transposed =
                tensor.makeView(TensorDescriptor(listOf(4, 2), TensorDataType.UINT32, listOf(1, 4)))
            assertEquals(28L, transposed.descriptor.byteOffset(listOf(3, 1)))
            tensor.close()
            val p =
                d.makeComputePipelineState(
                    d.function("local_size.comp.spv", FunctionConstants().setInt(0, 8))
                )
            d.submit {
                compute {
                    setComputePipelineState(p)
                    setTensor(transposed, 0)
                    dispatchThreadgroups(Size(1))
                }
            }
            val values =
                ByteBuffer.wrap(transposed.buffer.readBytes(32)).order(ByteOrder.nativeOrder())
            repeat(8) { assertEquals(8, values.int) }
            assertThrows(IllegalArgumentException::class.java) {
                TensorDescriptor(listOf(Long.MAX_VALUE, 2))
            }
            assertThrows(IllegalArgumentException::class.java) {
                transposed.makeView(TensorDescriptor(listOf(9), TensorDataType.UINT32))
            }
        }

    @Test
    fun cooperativeMatrixMultiplyOnSupportedGpu(): Unit {
        val available = device().use { it.capabilities.availableFeatures }
        assumeTrue(Feature.COOPERATIVE_MATRIX in available && Feature.SHADER_FLOAT16 in available)
        device(setOf(Feature.COOPERATIVE_MATRIX, Feature.SHADER_FLOAT16)).use { d ->
            val config =
                d.cooperativeMatrixConfigurations().firstOrNull {
                    it.aType == TensorDataType.FLOAT16 &&
                        it.bType == TensorDataType.FLOAT16 &&
                        it.accumulatorType == TensorDataType.FLOAT32 &&
                        it.resultType == TensorDataType.FLOAT32 &&
                        !it.saturatingAccumulation
                }
            assumeTrue(config != null)
            val selected = checkNotNull(config)
            val constants =
                FunctionConstants()
                    .setInt(0, selected.m)
                    .setInt(1, selected.n)
                    .setInt(2, selected.k)
                    .setInt(3, d.capabilities.subgroupSize)
            val pipeline = d.makeComputePipelineState(d.function("cooperative.comp.spv", constants))
            val output =
                d.makeTensor(TensorDescriptor(listOf(selected.m.toLong(), selected.n.toLong())))
            d.submit {
                compute {
                    setComputePipelineState(pipeline)
                    setTensor(output, 0)
                    dispatchThreadgroups(Size(1))
                }
            }
            val data =
                ByteBuffer.wrap(output.buffer.readBytes(output.descriptor.requiredBytes.toInt()))
                    .order(ByteOrder.nativeOrder())
            repeat(selected.m * selected.n) { assertEquals(selected.k.toFloat(), data.float, 0f) }
        }
    }

    @Test
    fun accelerationArchivesRejectMalformedData(): Unit {
        for (data in listOf(byteArrayOf(), ByteArray(87), ByteArray(128))) {
            assertThrows(IllegalArgumentException::class.java) {
                AccelerationStructureArchive.fromByteArray(data)
            }
        }
    }

    @Test
    fun accelerationArchiveRestoresAndRelocatesBlasReferences(): Unit {
        assumeTrue(device().use { Feature.RAY_QUERY in it.capabilities.availableFeatures })
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
                d.makePrimitiveAccelerationStructure(listOf(TriangleGeometry(vertices, 3)))
            val scene =
                d.makeInstanceAccelerationStructure(
                    listOf(AccelerationStructureInstance(primitive))
                )
            d.submit {
                accelerationStructure {
                    build(primitive)
                    build(scene)
                }
            }
            val blasArchive = primitive.serialize()
            val tlasArchive =
                AccelerationStructureArchive.fromByteArray(scene.serialize().toByteArray())
            val corrupt =
                blasArchive.toByteArray().apply {
                    this[lastIndex] = (this[lastIndex].toInt() xor 1).toByte()
                }
            assertThrows(IllegalArgumentException::class.java) {
                AccelerationStructureArchive.fromByteArray(corrupt)
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.restoreAccelerationStructure(tlasArchive)
            }
            val restoredPrimitive = d.restoreAccelerationStructure(blasArchive)
            val restoredScene =
                d.restoreAccelerationStructure(
                    tlasArchive,
                    tlasArchive.bottomLevelAddresses.associateWith { restoredPrimitive },
                )
            scene.close()
            primitive.close()
            vertices.close()
            restoredPrimitive.close()
            val pipeline = d.makeComputePipelineState(d.function("query.comp.spv"))
            val output = d.makeBuffer(4)
            d.submit {
                compute {
                    setComputePipelineState(pipeline)
                    setAccelerationStructure(restoredScene, 0)
                    setBuffer(output, 1)
                    dispatchThreads(Size(1))
                }
            }
            assertEquals(1, ByteBuffer.wrap(output.readBytes(4)).order(ByteOrder.nativeOrder()).int)
        }
    }
}
