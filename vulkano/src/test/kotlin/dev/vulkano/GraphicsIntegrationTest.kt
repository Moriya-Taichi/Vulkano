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

    @Test fun workgroupExpressionsMatchGpuInvocationCounts(): Unit = device().use { d ->
        for ((factor, choose, expected) in listOf(Triple(3, true, 8), Triple(7, true, 16), Triple(7, false, 2))) {
            val constants = FunctionConstants().setInt(7, factor).setBoolean(9, choose)
            val pipeline = d.makeComputePipelineState(d.function("local-size-expression.spv", constants))
            assertEquals(Size(expected), pipeline.threadgroupSize)
            val result = d.makeBuffer(expected * 4L)
            d.submit { compute {
                setComputePipelineState(pipeline); setBuffer(result, 0); dispatchThreadgroups(Size(1))
            } }
            val actual = ByteBuffer.wrap(result.readBytes(expected * 4)).order(ByteOrder.nativeOrder())
            repeat(expected) { assertEquals(expected, actual.int) }
        }
        assertThrows(IllegalArgumentException::class.java) {
            d.makeComputePipelineState(d.function("local-size-expression.spv", FunctionConstants().setInt(7, -1)))
        }
        assertThrows(IllegalArgumentException::class.java) {
            d.makeComputePipelineState(d.function("local-size-expression.spv", FunctionConstants().setInt(7, 100000)))
        }
    }

    @Test fun depthStencilAspectsTransferIndependentlyAcrossMipViews(): Unit = device().use { d ->
        val usage = setOf(TextureUsage.TRANSFER_SOURCE, TextureUsage.TRANSFER_DESTINATION, TextureUsage.SAMPLED)
        val sampler = d.makeSampler(SamplerDescriptor(linearFiltering = false))
        val sampleDepth = d.makeComputePipelineState(d.function("sample-depth-aspect.comp.spv"))
        val sampleStencil = d.makeComputePipelineState(d.function("sample-stencil-aspect.comp.spv"))
        var checked = 0
        for (format in listOf(PixelFormat.DEPTH24_STENCIL8, PixelFormat.DEPTH32_FLOAT_STENCIL8)) {
            val descriptor = TextureDescriptor(4, 4, format, usage, mipLevels = 2,
                arrayLength = 2, textureType = TextureType.TYPE_2D_ARRAY)
            if (!d.supportsTexture(descriptor)) continue
            checked++
            val root = d.makeTexture(descriptor)
            fun view(aspect: TextureAspect) = root.makeTextureView(textureType = TextureType.TYPE_2D,
                level = 1, levelCount = 1, slice = 1, sliceCount = 1, aspect = aspect)
            val depth = view(TextureAspect.DEPTH)
            val stencil = view(TextureAspect.STENCIL)
            val expectedDepth = listOf(0.125f, 0.25f, 0.5f, 0.75f)
            val packed = expectedDepth.map { if (format == PixelFormat.DEPTH24_STENCIL8)
                (it * 16777215f).toInt() else it.toRawBits() }
            val upload = d.makeBuffer(20).apply { write(ByteBuffer.allocateDirect(20).order(ByteOrder.nativeOrder()).apply {
                putInt(0x12345678); packed.forEach { putInt(it) }; flip()
            }) }
            val result = d.makeBuffer(24)
            // A four-byte buffer offset must work even for the eight-byte combined D32/S8 format.
            d.submit {
                blit { copy(upload, depth, sourceOffset = 4); copy(depth, result, destinationOffset = 4) }
            }
            val transferred = ByteBuffer.wrap(result.readBytes(20)).order(ByteOrder.nativeOrder()).apply { position(4) }
            packed.forEach { assertEquals(it, if (format == PixelFormat.DEPTH24_STENCIL8) transferred.int and 0xffffff else transferred.int) }
            assertThrows(IllegalArgumentException::class.java) { d.submit { blit { copy(stencil, result) } } }
            d.submit { compute {
                setComputePipelineState(sampleDepth); setTexture(depth, 0, sampler); setBuffer(result, 1)
                dispatchThreads(Size(2, 2))
            } }
            val sampledDepth = ByteBuffer.wrap(result.readBytes(16)).order(ByteOrder.nativeOrder())
            expectedDepth.forEach { assertEquals(it, sampledDepth.float, 0.000001f) }
            val expectedStencil = listOf(7, 63, 129, 251)
            val padded = d.makeBuffer(12).apply { write(ByteArray(12) { i -> when (i) {
                4 -> 7; 5 -> 63; 8 -> 129.toByte(); 9 -> 251.toByte(); else -> 0
            } }) }
            d.submit {
                blit { copy(padded, root, TextureRegion(size = Size(2, 2), level = 1, slice = 1,
                    aspect = TextureAspect.STENCIL), sourceOffset = 4, rowLength = 4) }
                compute {
                    setComputePipelineState(sampleStencil); setTexture(stencil, 0, sampler); setBuffer(result, 1)
                    dispatchThreads(Size(2, 2))
                }
            }
            val sampledStencil = ByteBuffer.wrap(result.readBytes(16)).order(ByteOrder.nativeOrder())
            expectedStencil.forEach { assertEquals(it, sampledStencil.int) }
            assertThrows(IllegalArgumentException::class.java) { d.submit { compute {
                setComputePipelineState(sampleDepth); setTexture(stencil, 0, sampler); setBuffer(result, 1)
                dispatchThreads(Size(2, 2))
            } } }
            // A stencil upload preserves the previously written depth aspect.
            d.submit { blit { copy(depth, result) } }
            val preserved = ByteBuffer.wrap(result.readBytes(16)).order(ByteOrder.nativeOrder())
            packed.forEach { assertEquals(it, if (format == PixelFormat.DEPTH24_STENCIL8) preserved.int and 0xffffff else preserved.int) }
            // Views of views inherit the selected aspect and retain the original image.
            val child = stencil.makeTextureView()
            root.close(); stencil.close(); depth.close()
            d.submit { blit { copy(child, result) } }
            assertArrayEquals(expectedStencil.map { it.toByte() }.toByteArray(), result.readBytes(4))
            assertThrows(IllegalArgumentException::class.java) {
                child.makeTextureView(aspect = TextureAspect.COLOR)
            }
        }
        assertTrue("At least one packed depth/stencil format must support transfer and sampling", checked > 0)
    }

    @Test fun aspectCopiesAndFailedSubmissionsPreserveInitialization(): Unit = device().use { d ->
        val descriptor = TextureDescriptor(2, 2, PixelFormat.DEPTH32_FLOAT_STENCIL8,
            setOf(TextureUsage.TRANSFER_SOURCE, TextureUsage.TRANSFER_DESTINATION))
        assumeTrue(d.supportsTexture(descriptor))
        val source = d.makeTexture(descriptor)
        val destination = d.makeTexture(descriptor)
        val upload = d.makeBuffer(16).apply { write(floats(0.25f, 0.25f, 0.25f, 0.25f)) }
        val stencilUpload = d.makeBuffer(4).apply { write(byteArrayOf(1, 2, 3, 4)) }
        val output = d.makeBuffer(16)
        val depth = TextureRegion(size = Size(2, 2), aspect = TextureAspect.DEPTH)
        val stencil = depth.copy(aspect = TextureAspect.STENCIL)
        assertThrows(IllegalArgumentException::class.java) { d.submit { blit {
            copy(upload, source, depth); copy(source, output, stencil)
        } } }
        assertThrows(IllegalArgumentException::class.java) { d.submit { blit { copy(source, output, depth) } } }
        d.submit { blit { copy(upload, source, depth); copy(source, destination, depth, depth); copy(destination, output, depth) } }
        assertEquals(0.25f, ByteBuffer.wrap(output.readBytes(4)).order(ByteOrder.nativeOrder()).float, 0f)
        assertThrows(IllegalArgumentException::class.java) { d.submit { blit { copy(destination, output, stencil) } } }
        assertThrows(IllegalArgumentException::class.java) { d.submit { blit { copy(source, destination, depth, stencil) } } }
        d.submit { blit {
            copy(stencilUpload, source, stencil); copy(source, destination); copy(destination, output, stencil)
        } }
        assertArrayEquals(byteArrayOf(1, 2, 3, 4), output.readBytes(4))
        // Packed buffer copies still require an explicit aspect.
        assertThrows(IllegalArgumentException::class.java) { d.submit { blit { copy(source, output) } } }
        val small = d.makeBuffer(3)
        assertThrows(IllegalArgumentException::class.java) { d.submit { blit { copy(source, small, stencil) } } }
    }

    @Test fun combinedStencilViewReadsSubpassInputAndDiscardedAspectsAreRejected(): Unit = device().use { d ->
        val format = PixelFormat.DEPTH32_FLOAT_STENCIL8
        val descriptor = TextureDescriptor(2, 2, format, setOf(TextureUsage.DEPTH_ATTACHMENT,
            TextureUsage.INPUT_ATTACHMENT, TextureUsage.TRANSFER_SOURCE))
        assumeTrue(d.supportsTexture(descriptor))
        val depthStencil = d.makeTexture(descriptor)
        val stencil = depthStencil.makeTextureView(aspect = TextureAspect.STENCIL)
        val color = d.makeTexture(TextureDescriptor(2, 2,
            usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE)))
        val layout = RenderPassLayout(listOf(PixelFormat.RGBA8_UNORM), listOf(
            RenderSubpass(emptyList(), usesDepthAttachment = true), RenderSubpass(listOf(0), listOf(1))), depthFormat = format)
        val pipeline = d.makeRenderPipelineState(RenderPipelineDescriptor(d.function("fullscreen.vert.spv"),
            d.function("input_stencil.frag.spv"), subpassLayout = layout, subpassIndex = 1))
        val output = d.makeBuffer(16)
        d.submit {
            render(RenderPassDescriptor(listOf(ColorAttachment(color)),
                DepthAttachment(depthStencil, storeAction = StoreAction.STORE, clearStencil = 37), subpassLayout = layout)) {
                nextSubpass(); setRenderPipelineState(pipeline); setTexture(stencil, 0); drawPrimitives(3)
            }
            blit { copy(color, output) }
        }
        assertArrayEquals(ByteArray(16) { when (it % 4) { 0 -> 37; 3 -> 255.toByte(); else -> 0 } }, output.readBytes(16))
        d.submit { blit { copy(stencil, output) } }
        assertArrayEquals(ByteArray(4) { 37 }, output.readBytes(4))
        d.submit { render(RenderPassDescriptor(listOf(ColorAttachment(color)),
            DepthAttachment(depthStencil, storeAction = StoreAction.DONT_CARE))) {} }
        for (aspect in listOf(TextureAspect.DEPTH, TextureAspect.STENCIL))
            assertThrows(IllegalArgumentException::class.java) { d.submit { blit {
                copy(depthStencil, output, TextureRegion(size = Size(2, 2), aspect = aspect))
            } } }
    }

    @Test fun instanceStepRatesRepeatAttributesForDirectIndexedAndIndirectDraws(): Unit {
        val available = device().use { it.capabilities.availableFeatures }
        assumeTrue(Feature.VERTEX_ATTRIBUTE_DIVISOR in available)
        val features = if (Feature.VERTEX_ATTRIBUTE_ZERO_DIVISOR in available)
            setOf(Feature.VERTEX_ATTRIBUTE_ZERO_DIVISOR) else setOf(Feature.VERTEX_ATTRIBUTE_DIVISOR)
        device(features).use { d ->
            assertTrue(Feature.VERTEX_ATTRIBUTE_DIVISOR in d.capabilities.enabledFeatures)
            val caps = d.vertexInputCapabilities()
            assertTrue(caps.maxStepRate >= 2)
            val texture = d.makeTexture(TextureDescriptor(4, 1,
                usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE)))
            val colors = d.makeBuffer(64, usage = setOf(BufferUsage.VERTEX))
            colors.write(floats(1f, 0f, 0f, 1f, 0f, 1f, 0f, 1f, 0f, 0f, 1f, 1f, 1f, 1f, 1f, 1f))
            val indices = d.makeBuffer(12, usage = setOf(BufferUsage.INDEX))
            indices.write(ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()).apply {
                repeat(6) { putShort(it.toShort()) }; flip()
            })
            val indirect = d.makeBuffer(16, usage = setOf(BufferUsage.INDIRECT))
            indirect.write(ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder()).apply {
                putInt(6); putInt(4); putInt(0); putInt(0); flip()
            })
            val result = d.makeBuffer(16)
            fun pipeline(rate: Long) = d.makeRenderPipelineState(RenderPipelineDescriptor(
                d.function("instance-divisor.vert.spv"), d.function("instance-divisor.frag.spv"),
                vertexBuffers = listOf(VertexBufferLayout(0, 16, VertexStepFunction.PER_INSTANCE, rate)),
                vertexAttributes = listOf(VertexAttribute(0, 0, PixelFormat.RGBA32_FLOAT))))
            val rates = listOf(1L, 2L) + (if (caps.supportsZeroStepRate) listOf(0L) else emptyList()) +
                (if (caps.maxStepRate > Int.MAX_VALUE) listOf(caps.maxStepRate) else emptyList())
            for (rate in rates) pipeline(rate).use { pipeline ->
                for (mode in 0..2) {
                    d.submit {
                        render(RenderPassDescriptor(ColorAttachment(texture))) {
                            setRenderPipelineState(pipeline); setVertexBuffer(colors, 0)
                            when (mode) {
                                0 -> drawPrimitives(6, instanceCount = 4)
                                1 -> drawIndexedPrimitives(indices, 6, instanceCount = 4)
                                else -> drawPrimitives(indirect)
                            }
                        }
                        blit { copy(texture, result) }
                    }
                    val bytes = result.readBytes(16)
                    repeat(4) { i ->
                        val element = if (rate == 0L) 0 else (i / rate).toInt()
                        repeat(3) { channel -> assertEquals(if (element == channel || element == 3) 255 else 0,
                            bytes[i * 4 + channel].toInt() and 255) }
                        assertEquals(255, bytes[i * 4 + 3].toInt() and 255)
                    }
                }
                if (rate != 1L && caps.supportsNonZeroFirstInstance) {
                    d.submit {
                        render(RenderPassDescriptor(ColorAttachment(texture))) {
                            setRenderPipelineState(pipeline); setVertexBuffer(colors, 0)
                            drawPrimitives(6, instanceCount = 2, firstInstance = 2)
                        }
                        blit { copy(texture, result) }
                    }
                    val bytes = result.readBytes(16)
                    for (i in 2..3) {
                        assertEquals(255, bytes[i * 4 + 2].toInt() and 255)
                        assertEquals(0, bytes[i * 4].toInt() and 255)
                    }
                } else if (rate != 1L) {
                    for (indexed in listOf(false, true)) assertThrows(IllegalArgumentException::class.java) {
                        d.submit { render(RenderPassDescriptor(ColorAttachment(texture))) {
                            setRenderPipelineState(pipeline); setVertexBuffer(colors, 0)
                            if (indexed) drawIndexedPrimitives(indices, 6, instanceCount = 2, firstInstance = 2)
                            else drawPrimitives(6, instanceCount = 2, firstInstance = 2)
                        } }
                    }
                }
            }
            if (caps.maxStepRate < 0xffffffffL) assertThrows(IllegalArgumentException::class.java) {
                pipeline(caps.maxStepRate + 1)
            }
        }
    }

    @Test fun customInstanceStepRatesRequireEnabledFeatures(): Unit = device().use { d ->
        for (rate in listOf(0L, 2L)) assertThrows(IllegalArgumentException::class.java) {
            d.makeRenderPipelineState(RenderPipelineDescriptor(
                d.function("instance-divisor.vert.spv"), d.function("instance-divisor.frag.spv"),
                vertexBuffers = listOf(VertexBufferLayout(0, 16, VertexStepFunction.PER_INSTANCE, rate)),
                vertexAttributes = listOf(VertexAttribute(0, 0, PixelFormat.RGBA32_FLOAT))))
        }
        assertThrows(IllegalArgumentException::class.java) { VertexBufferLayout(0, 16, stepRate = 2) }
        for (rate in listOf(-1L, 0x100000000L)) assertThrows(IllegalArgumentException::class.java) {
            VertexBufferLayout(0, 16, VertexStepFunction.PER_INSTANCE, rate)
        }
    }

    @Test fun textureFormatQueriesMatchAllocationsAndReportCombinationLimits(): Unit = device().use { d ->
        for (format in listOf(PixelFormat.RGBA8_UNORM, PixelFormat.DEPTH32_FLOAT, PixelFormat.STENCIL8)) {
            val depth = format.isDepth || format.isStencil
            val usage = setOf(if (depth) TextureUsage.DEPTH_ATTACHMENT else TextureUsage.COLOR_ATTACHMENT)
            for (storage in listOf(StorageMode.PRIVATE, StorageMode.MEMORYLESS)) {
                val caps = d.textureFormatCapabilities(format, usage, storageMode = storage) ?: continue
                assertTrue(caps.maxSize.width >= 4 && caps.maxMipLevels > 0 && caps.maxResourceBytes > 0)
                assertTrue((if (depth) TextureFormatFeature.DEPTH_STENCIL_ATTACHMENT else TextureFormatFeature.COLOR_ATTACHMENT) in caps.features)
                for (samples in caps.sampleCounts) {
                    val descriptor = TextureDescriptor(4, 4, format, usage, storage, sampleCount = samples)
                    assertTrue(d.supportsTexture(descriptor)); d.makeTexture(descriptor).close()
                }
                assertFalse(d.supportsTexture(TextureDescriptor(caps.maxSize.width + 1, 1, format, usage, storage)))
            }
        }
        val color = checkNotNull(d.textureFormatCapabilities(PixelFormat.RGBA8_UNORM))
        assertTrue(TextureFormatFeature.LINEAR_FILTER in color.features)
        assertFalse(d.supportsTexture(TextureDescriptor(4, 2, textureType = TextureType.TYPE_1D)))
        assertFalse(d.supportsTexture(TextureDescriptor(4, 4, arrayLength = 2)))
        assertFalse(d.supportsTexture(TextureDescriptor(4, 2, arrayLength = 6, textureType = TextureType.CUBE)))
        assertNull(d.textureFormatCapabilities(PixelFormat.RGBA8_UNORM, setOf(TextureUsage.SHADING_RATE_ATTACHMENT)))
        val bc = d.textureFormatCapabilities(PixelFormat.BC1_RGBA_UNORM)
        if (Feature.TEXTURE_COMPRESSION_BC in d.capabilities.availableFeatures && bc != null)
            assertTrue(Feature.TEXTURE_COMPRESSION_BC in bc.requiredFeatures)
        else assertNull(bc)
    }

    @Test fun functionConstantsPreserveSmallScalarWidths(): Unit {
        val features = setOf(Feature.SHADER_INT8, Feature.SHADER_INT16, Feature.SHADER_FLOAT16)
        assumeTrue(device().use { it.capabilities.availableFeatures.containsAll(features) })
        device(features).use { d ->
            val constants = FunctionConstants().setByte(0, -101).setUByte(1, 231u)
                .setShort(2, -30001).setUShort(3, 65530u).setHalf(4, 1.5f).setBoolean(5, false)
            val function = d.function("specialization-small.comp.spv", constants)
            constants.setByte(0, 0) // ShaderFunction owns the snapshot used to create its pipeline.
            val pipeline = d.makeComputePipelineState(function)
            val result = d.makeBuffer(24)
            d.submit { compute {
                setComputePipelineState(pipeline); setBuffer(result, 0); dispatchThreads(Size(1))
            } }
            val bytes = ByteBuffer.wrap(result.readBytes(24)).order(ByteOrder.nativeOrder())
            assertEquals(-101, bytes.int); assertEquals(231, bytes.int)
            assertEquals(-30001, bytes.int); assertEquals(65530, bytes.int)
            assertEquals(1.5f, bytes.float, 0f); assertEquals(77, bytes.int)
            assertThrows(IllegalArgumentException::class.java) {
                d.makeComputePipelineState(d.function("specialization-small.comp.spv", FunctionConstants().setFloat(4, 1.5f)))
            }
        }
    }

    @Test fun functionConstantsPreserveWideIntegersAndDouble(): Unit {
        val features = setOf(Feature.SHADER_INT64, Feature.SHADER_FLOAT64)
        assumeTrue(device().use { it.capabilities.availableFeatures.containsAll(features) })
        device(features).use { d ->
            val constants = FunctionConstants().setBoolean(0, true).setLong(1, -4294967301L)
                .setULong(2, 0xfedcba9876543210uL).setDouble(3, 1.23456789012345)
            val pipeline = d.makeComputePipelineState(d.function("specialization-wide.comp.spv", constants))
            val result = d.makeBuffer(28)
            d.submit { compute {
                setComputePipelineState(pipeline); setBuffer(result, 0); dispatchThreads(Size(1))
            } }
            val bytes = ByteBuffer.wrap(result.readBytes(28)).order(ByteOrder.nativeOrder())
            assertEquals(1, bytes.int)
            assertEquals(-4294967294L, bytes.long)
            assertEquals(0xfedcba9876543213uL.toLong(), bytes.long)
            assertEquals(1.23456789012345 + 0.5, bytes.double, 0.0)
            assertThrows(IllegalArgumentException::class.java) {
                d.makeComputePipelineState(d.function("specialization-wide.comp.spv", FunctionConstants().setInt(1, 1)))
            }
        }
    }

    @Test fun functionConstantsRejectWrongWidthAndUnknownId(): Unit = device().use { d ->
        assertThrows(IllegalArgumentException::class.java) {
            d.makeComputePipelineState(d.function("specialized.comp.spv", FunctionConstants().setLong(0, 7)))
        }
        assertThrows(IllegalArgumentException::class.java) {
            d.makeComputePipelineState(d.function("specialized.comp.spv", FunctionConstants().setInt(99, 7)))
        }
    }

    @Test
    fun machineLearningEncoderRequiresEnabledGraphQueue(): Unit = device().use { d ->
        d.makeCommandQueue().use { q -> q.makeCommandBuffer().use { c ->
            assertThrows(IllegalArgumentException::class.java) { c.makeMachineLearningCommandEncoder() }
        } }
        assertThrows(IllegalStateException::class.java) { d.machineLearningOperationSets() }
        assertThrows(IllegalArgumentException::class.java) {
            d.makeComputePipelineState(d.function("graph-identity.spv"))
        }
    }

    @Test
    fun machineLearningGraphsChainDispatchesAndRetainTensorViews(): Unit {
        assumeTrue(device().use { Feature.MACHINE_LEARNING_GRAPH in it.capabilities.availableFeatures })
        device(setOf(Feature.MACHINE_LEARNING_GRAPH)).use { d ->
            assumeTrue(checkNotNull(d.machineLearningCapabilities).supportsShaderCompilation)
            assertTrue(Feature.TENSOR_RESOURCES in d.capabilities.enabledFeatures)
            assertTrue(Feature.TIMELINE_SEMAPHORE in d.capabilities.enabledFeatures)
            val graphQueue = d.commandQueues.first { it.supportsMachineLearning }.index
            d.machineLearningOperationSets(graphQueue)
            val usage = setOf(TensorUsage.MACHINE_LEARNING, TensorUsage.TRANSFER_SOURCE, TensorUsage.TRANSFER_DESTINATION)
            val gpu = TensorResourceDescriptor(listOf(2, 3), usage = usage)
            val host = TensorResourceDescriptor(listOf(2, 3), layout = TensorLayout.LINEAR,
                usage = setOf(TensorUsage.TRANSFER_SOURCE, TensorUsage.TRANSFER_DESTINATION))
            assumeTrue(d.supportsTensor(gpu) && d.supportsTensor(host, StorageMode.SHARED))
            val source = d.makeTensorResource(host, StorageMode.SHARED)
            val input = d.makeTensorResource(gpu)
            val intermediate = d.makeTensorResource(gpu)
            val output = d.makeTensorResource(gpu)
            val readback = d.makeTensorResource(host, StorageMode.SHARED)
            val inputView = input.makeView()
            val middleView = intermediate.makeView()
            val outputView = output.makeView()
            val values = floats(1f, 2f, 4f, 8f, 16f, 32f)
            source.write(values)
            val pipeline = d.makeMachineLearningPipelineState(d.function("graph-identity.spv"),
                listOf(MachineLearningTensorBinding(0, gpu), MachineLearningTensorBinding(1, gpu)))
            val event = d.makeSharedEvent()
            val upload = d.makeCommandQueue().use { it.makeCommandBuffer() }
            upload.blit { copy(source, input) }
            upload.signalEventOnCompletion(event, 1)
            upload.commit()
            val command = d.makeCommandQueue(graphQueue).use { it.makeCommandBuffer() }
            command.waitForEvent(event, 1)
            command.makeMachineLearningCommandEncoder().use { encoder ->
                encoder.setMachineLearningPipelineState(pipeline)
                encoder.setTensor(inputView, 0)
                assertThrows(IllegalArgumentException::class.java) { encoder.dispatch() }
                encoder.setTensor(middleView, 1)
                encoder.dispatch()
                encoder.setTensor(middleView, 0)
                encoder.setTensor(outputView, 1)
                encoder.dispatch()
            }
            command.signalEventOnCompletion(event, 2)
            input.close(); intermediate.close(); inputView.close(); middleView.close(); outputView.close(); pipeline.close()
            command.commit()
            val download = d.makeCommandQueue().use { it.makeCommandBuffer() }
            download.waitForEvent(event, 2)
            download.blit { copy(output, readback) }
            download.commit()
            assertTrue(download.waitUntilCompleted())
            val actual = ByteBuffer.wrap(readback.readBytes(24)).order(ByteOrder.nativeOrder())
            for (i in 0..5) assertEquals(values.getFloat(i * 4), actual.getFloat(i * 4), 0f)
            command.close(); upload.close(); download.close()
        }
    }

    @Test
    fun machineLearningGraphUsesImmutableWeights(): Unit = checkConstantGraph(false)

    @Test
    fun machineLearningGraphRestoresPipelineCache(): Unit = checkConstantGraph(true)

    private fun checkConstantGraph(restoreCache: Boolean) {
        assumeTrue(device().use { Feature.MACHINE_LEARNING_GRAPH in it.capabilities.availableFeatures })
        device(setOf(Feature.MACHINE_LEARNING_GRAPH)).use { d ->
            assumeTrue(checkNotNull(d.machineLearningCapabilities).supportsShaderCompilation)
            val usage = setOf(TensorUsage.MACHINE_LEARNING, TensorUsage.TRANSFER_SOURCE, TensorUsage.TRANSFER_DESTINATION)
            val gpu = TensorResourceDescriptor(listOf(2, 3), usage = usage)
            val weights = TensorResourceDescriptor(listOf(2, 3), layout = TensorLayout.LINEAR,
                usage = setOf(TensorUsage.MACHINE_LEARNING))
            val host = TensorResourceDescriptor(listOf(2, 3), layout = TensorLayout.LINEAR,
                usage = setOf(TensorUsage.TRANSFER_SOURCE, TensorUsage.TRANSFER_DESTINATION))
            assumeTrue(d.supportsTensor(gpu) && d.supportsTensor(host, StorageMode.SHARED))
            val data = ByteArray(24)
            floats(3f, 5f, 7f, 11f, 13f, 17f).get(data)
            val constants = listOf(MachineLearningConstant(7, weights, data))
            var pipeline = d.makeMachineLearningPipelineState(d.function("graph-constant.spv"),
                listOf(MachineLearningTensorBinding(1, gpu)), constants)
            if (restoreCache) {
                assumeTrue(checkNotNull(d.machineLearningCapabilities).supportsCachedPipelines &&
                    MachineLearningPipelineProperty.IDENTIFIER in pipeline.availableProperties)
                val identifier = pipeline.identifier
                val cache = d.serializePipelineCache()
                pipeline.close()
                d.loadPipelineCache(cache)
                val restored = d.restoreMachineLearningPipelineState(identifier, listOf(MachineLearningTensorBinding(1, gpu)))
                assumeTrue("Driver reported a graph pipeline cache miss", restored != null)
                pipeline = checkNotNull(restored)
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeMachineLearningPipelineState(d.function("graph-constant.spv"),
                    listOf(MachineLearningTensorBinding(1, gpu)))
            }
            val output = d.makeTensorResource(gpu)
            val view = output.makeView()
            val readback = d.makeTensorResource(host, StorageMode.SHARED)
            val event = d.makeSharedEvent()
            val queue = d.commandQueues.first { it.supportsMachineLearning }.index
            val command = d.makeCommandQueue(queue).use { it.makeCommandBuffer() }
            command.makeMachineLearningCommandEncoder().use {
                it.setMachineLearningPipelineState(pipeline)
                it.setTensor(view, 1)
                it.dispatch()
            }
            command.signalEventOnCompletion(event, 1)
            view.close(); pipeline.close()
            command.commit()
            val download = d.makeCommandQueue().use { it.makeCommandBuffer() }
            download.waitForEvent(event, 1)
            download.blit { copy(output, readback) }
            download.commit()
            assertTrue(download.waitUntilCompleted())
            assertArrayEquals(data, readback.readBytes(24))
            command.close(); download.close()
        }
    }

    @Test
    fun tensorResourcesRequireFeatureAndSharedLinearStorage(): Unit =
        device().use { d ->
            val tensor = TensorResourceDescriptor(listOf(2, 3))
            assertFalse(d.supportsTensor(tensor))
            assertThrows(IllegalArgumentException::class.java) { d.makeTensorResource(tensor) }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeTensorResource(tensor, StorageMode.SHARED)
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeTensorResource(tensor, StorageMode.MEMORYLESS)
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeComputePipelineState(d.function("tensor-double.comp.spv"))
            }
        }

    @Test
    fun synchronization2PreservesTransferVisibility(): Unit {
        assumeTrue(device().use { Feature.SYNCHRONIZATION_2 in it.capabilities.availableFeatures })
        device(setOf(Feature.SYNCHRONIZATION_2)).use { d ->
            val source = d.makeBuffer(32, StorageMode.PRIVATE)
            val output = d.makeBuffer(32)
            d.submit {
                blit {
                    fill(source, 79)
                    copy(source, output)
                }
            }
            assertArrayEquals(ByteArray(32) { 79 }, output.readBytes(32))
        }
    }

    @Test
    fun tensorCopiesConvertLayoutsAndRetainResources(): Unit {
        val available = device().use { it.capabilities.availableFeatures }
        assumeTrue(Feature.TENSOR_RESOURCES in available)
        val features =
            setOf(Feature.TENSOR_RESOURCES) +
                if (Feature.TIMELINE_SEMAPHORE in available) setOf(Feature.TIMELINE_SEMAPHORE)
                else emptySet()
        device(features).use { d ->
            val usage = setOf(TensorUsage.TRANSFER_SOURCE, TensorUsage.TRANSFER_DESTINATION)
            val host =
                TensorResourceDescriptor(listOf(2, 3), layout = TensorLayout.LINEAR, usage = usage)
            val gpu = TensorResourceDescriptor(listOf(2, 3), usage = usage)
            assumeTrue(d.supportsTensor(host, StorageMode.SHARED) && d.supportsTensor(gpu))
            val source = d.makeTensorResource(host, StorageMode.SHARED)
            val intermediate = d.makeTensorResource(gpu)
            val output = d.makeTensorResource(host, StorageMode.SHARED)
            val values = ByteArray(24) { (it * 7).toByte() }
            source.write(values)
            assertArrayEquals(values, source.readBytes(24))
            assertTrue(source.allocatedBytes >= host.byteLength)
            assertThrows(IllegalArgumentException::class.java) { intermediate.readBytes(4) }
            assertThrows(IllegalArgumentException::class.java) { source.readBytes(8, 20) }
            assertThrows(IllegalArgumentException::class.java) { source.makeView() }
            val command = d.makeCommandQueue().use { it.makeCommandBuffer() }
            val gate = if (Feature.TIMELINE_SEMAPHORE in features) d.makeSharedEvent() else null
            if (gate != null) command.waitForEvent(gate, 1)
            command.blit {
                copy(source, intermediate)
                copy(intermediate, output)
            }
            source.close()
            intermediate.close()
            command.commit()
            try {
                if (gate != null) {
                    assertThrows(IllegalArgumentException::class.java) { output.readBytes(4) }
                    assertThrows(IllegalArgumentException::class.java) {
                        output.write(byteArrayOf(0))
                    }
                }
            } finally {
                gate?.signal(1)
            }
            assertTrue(command.waitUntilCompleted())
            assertArrayEquals(values, output.readBytes(24))
            command.close()
        }
    }

    @Test
    fun tensorShaderChecksShapeAndDoublesElements(): Unit {
        assumeTrue(device().use { Feature.TENSOR_RESOURCES in it.capabilities.availableFeatures })
        device(setOf(Feature.TENSOR_RESOURCES)).use { d ->
            val capabilities = checkNotNull(d.tensorCapabilities)
            assumeTrue(
                capabilities.supportsShaderAccess &&
                    TensorShaderStage.COMPUTE in capabilities.shaderStages
            )
            val descriptor = TensorResourceDescriptor(listOf(2, 3))
            val hostDescriptor =
                TensorResourceDescriptor(
                    listOf(2, 3),
                    layout = TensorLayout.LINEAR,
                    byteStrides = if (capabilities.supportsNonPackedLayout) listOf(16, 4) else null,
                )
            assumeTrue(
                d.supportsTensor(hostDescriptor, StorageMode.SHARED) && d.supportsTensor(descriptor)
            )
            val upload = d.makeTensorResource(hostDescriptor, StorageMode.SHARED)
            val input = d.makeTensorResource(descriptor)
            val output = d.makeTensorResource(descriptor)
            val readback = d.makeTensorResource(hostDescriptor, StorageMode.SHARED)
            val data =
                ByteBuffer.allocateDirect(hostDescriptor.byteLength.toInt())
                    .order(ByteOrder.nativeOrder())
            for (row in 0..1) for (col in 0..2) data.putFloat(
                hostDescriptor.byteOffset(listOf(row.toLong(), col.toLong())).toInt(),
                (row * 3 + col + 1).toFloat(),
            )
            upload.write(data)
            val sourceView = input.makeView()
            val destinationView = output.makeView()
            val pipeline = d.makeComputePipelineState(d.function("tensor-double.comp.spv"))
            val mismatch =
                d.makeComputePipelineState(
                    d.function("tensor-double.comp.spv", FunctionConstants().setInt(0, 1))
                )
            d.makeCommandQueue().use { q ->
                val wrong = q.makeCommandBuffer()
                assertThrows(IllegalArgumentException::class.java) {
                    wrong.compute {
                        setComputePipelineState(mismatch)
                        setTensor(sourceView, 0)
                        setTensor(destinationView, 1)
                        dispatchThreadgroups(Size(3, 2))
                    }
                }
            }
            val command = d.makeCommandQueue().use { it.makeCommandBuffer() }
            command.blit { copy(upload, input) }
            command.compute {
                setComputePipelineState(pipeline)
                setTensor(sourceView, 0)
                setTensor(destinationView, 1)
                dispatchThreadgroups(Size(3, 2))
            }
            command.blit { copy(output, readback) }
            upload.close()
            input.close()
            output.close()
            sourceView.close()
            destinationView.close()
            pipeline.close()
            command.commit()
            assertTrue(command.waitUntilCompleted())
            val result =
                ByteBuffer.wrap(readback.readBytes(hostDescriptor.byteLength.toInt()))
                    .order(ByteOrder.nativeOrder())
            for (row in 0..1) for (col in 0..2) assertEquals(
                (row * 3 + col + 1) * 2f,
                result.getFloat(
                    hostDescriptor.byteOffset(listOf(row.toLong(), col.toLong())).toInt()
                ),
                0f,
            )
            command.close()
        }
    }

    @Test
    fun defaultQueueStaysOrderedAndChecksIndices(): Unit =
        device().use { d ->
            assertEquals(1, d.commandQueues.size)
            val info = d.commandQueues.single()
            assertEquals(0, info.index)
            assertTrue(info.supportsRendering && info.supportsCompute && info.supportsTransfer)
            assertEquals(TransferGranularity(1, 1, 1), info.imageTransferGranularity)
            assertEquals(info.timestampValidBits, d.counterCapabilities().timestampValidBits)
            for (index in listOf(-1, 1)) {
                assertThrows(IllegalArgumentException::class.java) { d.makeCommandQueue(index) }
                assertThrows(IllegalArgumentException::class.java) {
                    d.makeCounterSampleBuffer(1, queueIndex = index)
                }
                assertThrows(IllegalArgumentException::class.java) { d.counterCapabilities(index) }
            }
            val source = d.makeBuffer(16)
            val output = d.makeBuffer(16)
            val queue = d.makeCommandQueue()
            val first = queue.makeCommandBuffer()
            queue.close()
            assertThrows(IllegalStateException::class.java) { queue.makeCommandBuffer() }
            first.blit { fill(source, 29) }
            first.commit()
            d.submit { blit { copy(source, output) } }
            assertArrayEquals(ByteArray(16) { 29 }, output.readBytes(16))
            assertTrue(first.waitUntilCompleted())
            first.close()
        }

    @Test
    fun independentQueuesTransferWithTimelineDependenciesAndReusePools(): Unit {
        val features = setOf(Feature.INDEPENDENT_QUEUES, Feature.TIMELINE_SEMAPHORE)
        assumeTrue(device().use { it.capabilities.availableFeatures.containsAll(features) })
        device(features).use { d ->
            assertTrue(d.commandQueues.size > 1)
            assertEquals(
                d.commandQueues.size,
                d.commandQueues.map { it.familyIndex to it.indexInFamily }.toSet().size,
            )
            val event = d.makeSharedEvent()
            val texture =
                d.makeTexture(
                    TextureDescriptor(
                        16,
                        16,
                        usage =
                            setOf(TextureUsage.TRANSFER_SOURCE, TextureUsage.TRANSFER_DESTINATION),
                    )
                )
            val target =
                d.makeTexture(
                    TextureDescriptor(16, 16, usage = setOf(TextureUsage.COLOR_ATTACHMENT))
                )
            var value = 0L
            repeat(3) { round ->
                var source = d.makeBuffer(1024)
                val expected = ByteArray(1024) { ((it + round * 17) % 251).toByte() }
                source.write(
                    ByteBuffer.allocateDirect(expected.size).apply {
                        put(expected)
                        flip()
                    }
                )
                val commands = mutableListOf<CommandBuffer>()
                for (q in d.commandQueues) {
                    val destination = d.makeBuffer(1024)
                    val command = d.makeCommandQueue(q.index).use { it.makeCommandBuffer() }
                    assertEquals(q, command.queueCapabilities)
                    if (!q.supportsCompute) {
                        assertThrows(IllegalArgumentException::class.java) {
                            command.makeComputeCommandEncoder()
                        }
                        assertThrows(IllegalArgumentException::class.java) {
                            command.makeRayTracingCommandEncoder()
                        }
                        assertThrows(IllegalArgumentException::class.java) {
                            command.makeAccelerationStructureCommandEncoder()
                        }
                    }
                    if (!q.supportsRendering) {
                        assertThrows(IllegalArgumentException::class.java) {
                            command.makeRenderCommandEncoder(
                                RenderPassDescriptor(ColorAttachment(target))
                            )
                        }
                    }
                    if (value > 0) command.waitForEvent(event, value)
                    val counters =
                        if (q.timestampValidBits > 0)
                            d.makeCounterSampleBuffer(2, queueIndex = q.index)
                        else null
                    if (counters != null) command.sampleCounters(counters, 0)
                    command.blit {
                        copy(source, texture)
                        copy(texture, destination)
                    }
                    if (counters != null) command.sampleCounters(counters, 1)
                    command.signalEventOnCompletion(event, ++value)
                    command.commit()
                    source.close()
                    counters?.close()
                    commands.add(command)
                    source = destination
                }
                assertTrue(commands.last().waitUntilCompleted())
                assertArrayEquals(expected, source.readBytes(1024))
                assertEquals(value, event.signaledValue)
                commands.forEach { it.close() }
                source.close()
            }
        }
    }

    @Test
    fun aWaitingQueueDoesNotBlockAnotherQueue(): Unit {
        val features = setOf(Feature.INDEPENDENT_QUEUES, Feature.TIMELINE_SEMAPHORE)
        assumeTrue(device().use { it.capabilities.availableFeatures.containsAll(features) })
        device(features).use { d ->
            val gate = d.makeSharedEvent()
            val blockedOutput = d.makeBuffer(4)
            val freeOutput = d.makeBuffer(4)
            val blocked = d.makeCommandQueue(0).use { it.makeCommandBuffer() }
            val free = d.makeCommandQueue(1).use { it.makeCommandBuffer() }
            blocked.waitForEvent(gate, 1)
            blocked.blit { fill(blockedOutput, 31) }
            blocked.commit()
            try {
                free.blit { fill(freeOutput, 67) }
                free.commit()
                assertTrue(free.waitUntilCompleted(5_000_000_000L))
                assertFalse(blocked.waitUntilCompleted(0))
                assertArrayEquals(ByteArray(4) { 67 }, freeOutput.readBytes(4))
            } finally {
                gate.signal(1)
                blocked.close()
                free.close()
            }
            assertArrayEquals(ByteArray(4) { 31 }, blockedOutput.readBytes(4))
        }
    }

    @Test
    fun independentQueueSignalsStayMonotonicWithoutHostWaiting(): Unit {
        val features = setOf(Feature.INDEPENDENT_QUEUES, Feature.TIMELINE_SEMAPHORE)
        assumeTrue(device().use { it.capabilities.availableFeatures.containsAll(features) })
        device(features).use { d ->
            val event = d.makeSharedEvent()
            val source = d.makeBuffer(64)
            val output = d.makeBuffer(64)
            val commands = mutableListOf<CommandBuffer>()
            repeat(8) { index ->
                val c =
                    d.makeCommandQueue(index % d.commandQueues.size).use { it.makeCommandBuffer() }
                c.blit { if (index == 0) fill(source, 47) else copy(source, output) }
                c.signalEventOnCompletion(event, index + 1L)
                c.commit()
                commands.add(c)
            }
            d.waitUntilIdle()
            assertEquals(8L, event.signaledValue)
            assertArrayEquals(ByteArray(64) { 47 }, output.readBytes(64))
            commands.forEach {
                assertTrue(it.waitUntilCompleted())
                it.close()
            }
        }
    }

    @Test
    fun tileShadingRequiresExplicitFeatureAndRenderPass(): Unit =
        device().use { d ->
            assertThrows(IllegalArgumentException::class.java) { TileShadingDescriptor(-1) }
            for (file in listOf("tile-loop.comp.spv", "tile-area.comp.spv")) {
                assertThrows(IllegalArgumentException::class.java) {
                    d.makeComputePipelineState(d.function(file))
                }
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("solid.frag.spv"),
                        tileShading = TileShadingDescriptor(),
                    )
                )
            }
            val texture =
                d.makeTexture(TextureDescriptor(8, 8, usage = setOf(TextureUsage.COLOR_ATTACHMENT)))
            d.makeCommandQueue().use { q ->
                q.makeCommandBuffer().use { c ->
                    assertThrows(IllegalArgumentException::class.java) {
                        c.makeRenderCommandEncoder(
                            RenderPassDescriptor(
                                listOf(ColorAttachment(texture)),
                                tileShading = TileShadingDescriptor(),
                            )
                        )
                    }
                    c.render(RenderPassDescriptor(ColorAttachment(texture))) {
                        assertThrows(IllegalArgumentException::class.java) {
                            beginPerTileExecution()
                        }
                    }
                }
            }
        }

    private fun Device.tileTarget() =
        makeTexture(
            TextureDescriptor(
                32,
                32,
                usage =
                    setOf(
                        TextureUsage.COLOR_ATTACHMENT,
                        TextureUsage.STORAGE,
                        TextureUsage.TRANSFER_SOURCE,
                    ),
            )
        )

    private fun tileExtent() =
        ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(32).putInt(32).array()

    private fun assertTilePixels(bytes: ByteArray, green: Int) {
        for (at in bytes.indices step 4) {
            assertEquals("red at ${at / 4}", 255, bytes[at].toInt() and 255)
            assertTrue(
                "green at ${at / 4}",
                kotlin.math.abs((bytes[at + 1].toInt() and 255) - green) <= 1,
            )
            assertEquals("blue at ${at / 4}", 128, bytes[at + 2].toInt() and 255)
            assertEquals("alpha at ${at / 4}", 255, bytes[at + 3].toInt() and 255)
        }
    }

    @Test
    fun tileThreadgroupsReadWriteAttachmentsAndRetainResources(): Unit {
        val caps = device().use { it.tileShadingCapabilities() }
        assumeTrue(caps != null && caps.perTileDispatch && caps.colorAttachments)
        device(setOf(Feature.TILE_SHADING)).use { d ->
            val target = d.tileTarget()
            val output = d.makeBuffer(32L * 32 * 4)
            val compute = d.makeComputePipelineState(d.function("tile-loop.comp.spv"))
            assertNull(compute.tileShadingRate)
            val draw =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("solid.frag.spv"),
                        tileShading = TileShadingDescriptor(),
                    )
                )
            val args = d.makeBuffer(12, usage = setOf(BufferUsage.INDIRECT))
            args.write(
                ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder()).apply {
                    putInt(1)
                    putInt(1)
                    putInt(1)
                    flip()
                }
            )
            d.makeCommandQueue().use { q ->
                q.makeCommandBuffer().use { c ->
                    c.compute {
                        setComputePipelineState(compute)
                        setTexture(target, 0)
                        setBytes(tileExtent())
                        assertThrows(IllegalArgumentException::class.java) {
                            dispatchThreadgroups(Size(1))
                        }
                    }
                }
            }
            d.submit {
                render(
                    RenderPassDescriptor(
                        listOf(ColorAttachment(target)),
                        tileShading = TileShadingDescriptor(),
                    )
                ) {
                    setRenderPipelineState(draw)
                    drawPrimitives(3)
                    resetBindings()
                    setTileComputePipelineState(compute)
                    setTexture(target, 0)
                    setBytes(tileExtent())
                    assertThrows(IllegalArgumentException::class.java) {
                        dispatchTileThreadgroups(Size(1))
                    }
                    perTile {
                        assertThrows(IllegalArgumentException::class.java) {
                            beginPerTileExecution()
                        }
                        tileMemoryBarrier()
                        dispatchTileThreadgroups(Size(1))
                        tileMemoryBarrier()
                        dispatchTileThreadgroups(args)
                    }
                }
                blit { copy(target, output) }
                target.close()
                compute.close()
                args.close()
                draw.close()
            }
            assertTilePixels(output.readBytes(32 * 32 * 4), 192)
        }
    }

    @Test
    fun areaTileDispatchCoversFramebuffer(): Unit {
        val caps = device().use { it.tileShadingCapabilities() }
        assumeTrue(
            caps != null &&
                caps.areaDispatch &&
                caps.colorAttachments &&
                (caps.perTileDraw || caps.perTileDispatch)
        )
        device(setOf(Feature.TILE_SHADING)).use { d ->
            val target = d.tileTarget()
            val output = d.makeBuffer(32L * 32 * 4)
            val compute = d.makeComputePipelineState(d.function("tile-area.comp.spv"))
            assertEquals(Size(1), compute.tileShadingRate)
            d.submit {
                render(
                    RenderPassDescriptor(
                        listOf(
                            ColorAttachment(target, clearColor = ClearColor(1f, 0.25f, 0.5f, 1f))
                        ),
                        tileShading = TileShadingDescriptor(),
                    )
                ) {
                    setTileComputePipelineState(compute)
                    setTexture(target, 0)
                    setBytes(tileExtent())
                    perTile {
                        tileMemoryBarrier()
                        dispatchTile()
                    }
                }
                blit { copy(target, output) }
            }
            assertTilePixels(output.readBytes(32 * 32 * 4), 128)
        }
    }

    @Test
    fun fragmentTileReadsObservePreviousDraw(): Unit {
        val caps = device().use { it.tileShadingCapabilities() }
        assumeTrue(caps != null && caps.fragmentStage && caps.colorAttachments)
        device(setOf(Feature.TILE_SHADING)).use { d ->
            val target = d.tileTarget()
            val output = d.makeBuffer(32L * 32 * 4)
            val descriptor =
                RenderPipelineDescriptor(
                    d.function("fullscreen.vert.spv"),
                    d.function("solid.frag.spv"),
                    tileShading = TileShadingDescriptor(),
                )
            val first = d.makeRenderPipelineState(descriptor)
            val second =
                d.makeRenderPipelineState(
                    descriptor.copy(fragmentFunction = d.function("tile-read.frag.spv"))
                )
            d.submit {
                render(
                    RenderPassDescriptor(
                        listOf(ColorAttachment(target)),
                        tileShading = TileShadingDescriptor(),
                    )
                ) {
                    setRenderPipelineState(first)
                    drawPrimitives(3)
                    tileMemoryBarrier()
                    setRenderPipelineState(second)
                    setTexture(target, 0)
                    if (caps!!.perTileDraw) perTile { drawPrimitives(3) } else drawPrimitives(3)
                }
                blit { copy(target, output) }
            }
            assertTilePixels(output.readBytes(32 * 32 * 4), 128)
        }
    }

    @Test
    fun generatedCommandsRequireFeatureAndValidateTokenOffsets(): Unit =
        device().use { d ->
            assertThrows(IllegalArgumentException::class.java) { IndirectCommandToken.draw(2) }
            val function = d.function("generated-worker.comp.spv")
            val ordinary = d.makeComputePipelineState(function)
            assertThrows(IllegalArgumentException::class.java) {
                d.makeComputePipelineState(function, supportsIndirectCommands = true)
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("solid.frag.spv"),
                        supportsIndirectCommands = true,
                    )
                )
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeIndirectCommandLayout(
                    listOf(ordinary),
                    listOf(IndirectCommandToken.dispatch(0)),
                    12,
                )
            }
        }

    @Test
    fun generatedComputeSelectsPipelinesPushDataAndSequenceIndex(): Unit {
        val limits = device().use { it.indirectCommandLimits() }
        assumeTrue(limits != null && ShaderStage.COMPUTE in limits.pipelineBindingStages)
        device(setOf(Feature.DEVICE_GENERATED_COMMANDS)).use { d ->
            val pipelines =
                listOf(2, 3).map { multiplier ->
                    d.makeComputePipelineState(
                        d.function(
                            "generated-worker.comp.spv",
                            FunctionConstants().setInt(0, multiplier),
                        ),
                        supportsIndirectCommands = true,
                    )
                }
            val tokens =
                listOf(
                    IndirectCommandToken.pipeline(0),
                    IndirectCommandToken.pushConstants(4, 4, 4),
                    IndirectCommandToken.sequenceIndex(8, 0),
                    IndirectCommandToken.dispatch(8),
                )
            val layout = d.makeIndirectCommandLayout(pipelines, tokens, 20)
            assertThrows(IllegalArgumentException::class.java) {
                d.makeIndirectCommandLayout(pipelines, tokens, 16)
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeIndirectCommandLayout(
                    pipelines,
                    listOf(
                        IndirectCommandToken.pipeline(0),
                        IndirectCommandToken.pushConstants(4, 0, 8),
                        IndirectCommandToken.sequenceIndex(12, 4),
                        IndirectCommandToken.dispatch(12),
                    ),
                    24,
                )
            }
            val writer = d.makeComputePipelineState(d.function("generated-stream.comp.spv"))
            val usage =
                setOf(BufferUsage.INDIRECT, BufferUsage.STORAGE, BufferUsage.SHADER_DEVICE_ADDRESS)
            val args = d.makeBuffer(40, StorageMode.PRIVATE, usage)
            val count = d.makeBuffer(8, StorageMode.PRIVATE, usage)
            val output = d.makeBuffer(12)
            for (gpuCount in listOf(0, 2)) {
                d.submit {
                    blit { fill(output, 0) }
                    compute {
                        setComputePipelineState(writer)
                        setBuffer(args, 0)
                        setBuffer(count, 1)
                        setBytes(
                            ByteBuffer.allocate(24)
                                .order(ByteOrder.LITTLE_ENDIAN)
                                .putInt(0)
                                .putInt(gpuCount)
                                .array()
                        )
                        dispatchThreadgroups(Size(1))
                        resetBindings()
                        setBuffer(output, 0)
                        executeCommands(layout, args, 2, countBuffer = count, countOffset = 4)
                        setComputePipelineState(pipelines[0])
                        setBytes(
                            ByteBuffer.allocate(8)
                                .order(ByteOrder.LITTLE_ENDIAN)
                                .putInt(2)
                                .putInt(5)
                                .array()
                        )
                        dispatchThreadgroups(Size(1))
                    }
                }
                val result = ByteBuffer.wrap(output.readBytes(12)).order(ByteOrder.LITTLE_ENDIAN)
                assertEquals(if (gpuCount == 0) 0 else 14, result.int)
                assertEquals(if (gpuCount == 0) 0 else 24, result.int)
                assertEquals(10, result.int)
            }
            // Layout retains closed pipeline handles; every execution has separate preprocess
            // memory.
            pipelines.forEach { it.close() }
            d.makeCommandQueue().use { q ->
                q.makeCommandBuffer().use { c ->
                    c.compute {
                        setBuffer(output, 0)
                        executeCommands(layout, args, 2)
                    }
                    layout.close()
                    args.close()
                    count.close()
                    writer.close()
                    c.commit()
                    assertTrue(c.waitUntilCompleted())
                }
            }
            assertEquals(
                14,
                ByteBuffer.wrap(output.readBytes(12)).order(ByteOrder.LITTLE_ENDIAN).int,
            )
        }
    }

    @Test
    fun generatedDrawingSelectsShadersAndVertexIndexBuffers(): Unit {
        val available = device().use { it.capabilities.availableFeatures }
        val limits = device().use { it.indirectCommandLimits() }
        assumeTrue(
            limits != null &&
                ShaderStage.VERTEX in limits.pipelineBindingStages &&
                ShaderStage.FRAGMENT in limits.pipelineBindingStages
        )
        val mesh =
            Feature.MESH_SHADER in available &&
                Feature.TASK_SHADER in available &&
                ShaderStage.MESH in limits!!.pipelineBindingStages
        device(
                setOf(Feature.DEVICE_GENERATED_COMMANDS) +
                    if (mesh) setOf(Feature.MESH_SHADER, Feature.TASK_SHADER) else emptySet()
            )
            .use { d ->
                val usage =
                    setOf(
                        BufferUsage.INDIRECT,
                        BufferUsage.STORAGE,
                        BufferUsage.SHADER_DEVICE_ADDRESS,
                    )
                val args = d.makeBuffer(120, StorageMode.PRIVATE, usage)
                val count = d.makeBuffer(8, StorageMode.PRIVATE, usage)
                val vertices =
                    d.makeBuffer(
                        24,
                        usage = setOf(BufferUsage.VERTEX, BufferUsage.SHADER_DEVICE_ADDRESS),
                    )
                vertices.write(floats(-1f, -1f, 3f, -1f, -1f, 3f))
                val indices =
                    d.makeBuffer(
                        6,
                        usage = setOf(BufferUsage.INDEX, BufferUsage.SHADER_DEVICE_ADDRESS),
                    )
                indices.write(byteArrayOf(0, 0, 1, 0, 2, 0))
                val writer = d.makeComputePipelineState(d.function("generated-stream.comp.spv"))
                val target =
                    d.makeTexture(
                        TextureDescriptor(
                            1,
                            1,
                            usage =
                                setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                        )
                    )
                val readback = d.makeBuffer(4)
                val modes =
                    listOf(1) +
                        (if (limits!!.supportsVertexBuffers) listOf(2) else emptyList()) +
                        (if (mesh) listOf(3) else emptyList())
                for (mode in modes) {
                    val pipelines =
                        (0..1).map { channel ->
                            d.makeRenderPipelineState(
                                RenderPipelineDescriptor(
                                    d.function(
                                        if (mode == 2) "attribute.vert.spv"
                                        else if (mode == 3) "fullscreen.mesh.spv"
                                        else "fullscreen.vert.spv"
                                    ),
                                    d.function(
                                        "generated-color.frag.spv",
                                        FunctionConstants().setInt(0, channel),
                                    ),
                                    colorAttachments =
                                        listOf(
                                            RenderColorAttachmentDescriptor(
                                                blendingEnabled = true,
                                                sourceRGBBlendFactor = BlendFactor.ONE,
                                                destinationRGBBlendFactor = BlendFactor.ONE,
                                            )
                                        ),
                                    vertexBuffers =
                                        if (mode == 2) listOf(VertexBufferLayout(0, 8))
                                        else emptyList(),
                                    vertexAttributes =
                                        if (mode == 2)
                                            listOf(VertexAttribute(0, 0, PixelFormat.RG32_FLOAT))
                                        else emptyList(),
                                    meshShader = mode == 3,
                                    supportsIndirectCommands = true,
                                )
                            )
                        }
                    val tokens =
                        listOf(
                            IndirectCommandToken.pipeline(0),
                            IndirectCommandToken.pushConstants(4, 0, 4),
                        ) +
                            when (mode) {
                                2 ->
                                    listOf(
                                        IndirectCommandToken.vertexBuffer(8, 0),
                                        IndirectCommandToken.indexBuffer(24),
                                        IndirectCommandToken.drawIndexed(40),
                                    )
                                3 -> listOf(IndirectCommandToken.drawMesh(8))
                                else -> listOf(IndirectCommandToken.draw(8))
                            }
                    val layout =
                        d.makeIndirectCommandLayout(
                            pipelines,
                            tokens,
                            when (mode) {
                                2 -> 60
                                3 -> 20
                                else -> 24
                            },
                        )
                    d.submit {
                        compute {
                            setComputePipelineState(writer)
                            setBuffer(args, 0)
                            setBuffer(count, 1)
                            setBytes(
                                ByteBuffer.allocate(24)
                                    .order(ByteOrder.LITTLE_ENDIAN)
                                    .putInt(mode)
                                    .putInt(2)
                                    .putLong(vertices.gpuAddress)
                                    .putLong(indices.gpuAddress)
                                    .array()
                            )
                            dispatchThreadgroups(Size(1))
                        }
                        render(RenderPassDescriptor(listOf(ColorAttachment(target)))) {
                            useResource(vertices)
                            useResource(indices)
                            executeCommands(layout, args, 2, countBuffer = count, countOffset = 4)
                            setRenderPipelineState(pipelines[0])
                            setBytes(
                                ByteBuffer.allocate(4)
                                    .order(ByteOrder.LITTLE_ENDIAN)
                                    .putFloat(0.25f)
                                    .array()
                            )
                            if (mode == 2) {
                                setVertexBuffer(vertices, 0)
                                drawIndexedPrimitives(indices, 3)
                            } else if (mode == 3) drawMeshThreadgroups(Size(1))
                            else drawPrimitives(3)
                        }
                        blit { copy(target, readback) }
                    }
                    val actual = readback.readBytes(4)
                    assertTrue("red mode $mode", (actual[0].toInt() and 255) in 127..129)
                    assertTrue("green mode $mode", (actual[1].toInt() and 255) in 127..129)
                    assertEquals(0, actual[2].toInt())
                    layout.close()
                    pipelines.forEach { it.close() }
                }
            }
    }

    @Test
    fun additionalCompressionRequiresFeature(): Unit =
        device().use { d ->
            for (format in listOf(PixelFormat.ASTC_4x4_FLOAT, PixelFormat.PVRTC1_2BPP_UNORM)) {
                assertThrows(IllegalArgumentException::class.java) {
                    d.makeTexture(TextureDescriptor(16, 8, format))
                }
            }
        }

    @Test
    fun astcHdrSamplingPreservesValuesAboveOne(): Unit {
        assumeTrue(
            device().use {
                Feature.TEXTURE_COMPRESSION_ASTC_HDR in it.capabilities.availableFeatures
            }
        )
        device(setOf(Feature.TEXTURE_COMPRESSION_ASTC_HDR)).use { d ->
            val texture = d.makeTexture(TextureDescriptor(4, 4, PixelFormat.ASTC_4x4_FLOAT))
            // ASTC HDR void-extent block: half-float RGBA = (2, 0.5, 0.25, 1).
            val block =
                ByteBuffer.allocate(16)
                    .order(ByteOrder.LITTLE_ENDIAN)
                    .put(0xfc.toByte())
                    .apply { repeat(7) { put(0xff.toByte()) } }
                    .putShort(0x4000)
                    .putShort(0x3800)
                    .putShort(0x3400)
                    .putShort(0x3c00)
                    .array()
            val upload = d.makeBuffer(16).apply { write(block) }
            val output = d.makeBuffer(16)
            val target =
                d.makeTexture(
                    TextureDescriptor(
                        1,
                        1,
                        PixelFormat.RGBA32_FLOAT,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val sampler = d.makeSampler()
            val pipeline =
                d.makeRenderPipelineState(
                    d.function("fullscreen.vert.spv"),
                    d.function("sample.frag.spv"),
                    colorFormat = PixelFormat.RGBA32_FLOAT,
                )
            d.submit {
                blit { copy(upload, texture) }
                render(RenderPassDescriptor(listOf(ColorAttachment(target)))) {
                    setRenderPipelineState(pipeline)
                    setTexture(texture, 0, sampler)
                    drawPrimitives(3)
                }
                blit { copy(target, output) }
            }
            val actual = ByteBuffer.wrap(output.readBytes(16)).order(ByteOrder.nativeOrder())
            for (value in listOf(2f, 0.5f, 0.25f, 1f)) assertEquals(value, actual.float, 0.001f)
        }
    }

    @Test
    fun pvrtcTransfersCompressedBlocksAndChecksPowerOfTwo(): Unit {
        assumeTrue(
            device().use { Feature.TEXTURE_COMPRESSION_PVRTC in it.capabilities.availableFeatures }
        )
        device(setOf(Feature.TEXTURE_COMPRESSION_PVRTC)).use { d ->
            for (format in PixelFormat.entries.filter { it.name.startsWith("PVRTC") }) {
                val descriptor =
                    TextureDescriptor(
                        format.blockWidth * 2,
                        8,
                        format,
                        usage =
                            setOf(
                                TextureUsage.SAMPLED,
                                TextureUsage.TRANSFER_SOURCE,
                                TextureUsage.TRANSFER_DESTINATION,
                            ),
                    )
                if (!d.supportsTexture(descriptor)) continue
                val texture = d.makeTexture(descriptor)
                val data = ByteArray(32) { -1 }
                val upload = d.makeBuffer(32).apply { write(data) }
                val output = d.makeBuffer(32)
                d.submit {
                    blit {
                        copy(upload, texture)
                        copy(texture, output)
                    }
                }
                assertArrayEquals(data, output.readBytes(32))
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeTexture(TextureDescriptor(15, 8, PixelFormat.PVRTC1_2BPP_UNORM))
            }
        }
    }

    @Test
    fun indirectCountRequiresFeature(): Unit =
        device().use { d ->
            val args = d.makeBuffer(16, usage = setOf(BufferUsage.INDIRECT))
            val count = d.makeBuffer(4, usage = setOf(BufferUsage.INDIRECT))
            val target =
                d.makeTexture(TextureDescriptor(1, 1, usage = setOf(TextureUsage.COLOR_ATTACHMENT)))
            val pipeline =
                d.makeRenderPipelineState(
                    d.function("fullscreen.vert.spv"),
                    d.function("solid.frag.spv"),
                )
            assertThrows(IllegalArgumentException::class.java) {
                d.submit {
                    render(RenderPassDescriptor(listOf(ColorAttachment(target)))) {
                        setRenderPipelineState(pipeline)
                        drawPrimitives(args, count, 1)
                    }
                }
            }
        }

    @Test
    fun gpuGeneratedIndirectCountSupportsZeroClampIndexedAndMeshDraw(): Unit {
        val available = device().use { it.capabilities.availableFeatures }
        assumeTrue(Feature.DRAW_INDIRECT_COUNT in available)
        val mesh = Feature.MESH_SHADER in available
        device(
                setOf(Feature.DRAW_INDIRECT_COUNT) +
                    if (mesh) setOf(Feature.MESH_SHADER) else emptySet()
            )
            .use { d ->
                val usage = setOf(BufferUsage.INDIRECT, BufferUsage.STORAGE)
                val args = d.makeBuffer(64, usage = usage, storageMode = StorageMode.PRIVATE)
                val count = d.makeBuffer(8, usage = usage, storageMode = StorageMode.PRIVATE)
                val indices = d.makeBuffer(6, usage = setOf(BufferUsage.INDEX))
                indices.write(
                    ByteBuffer.allocateDirect(6)
                        .order(ByteOrder.nativeOrder())
                        .putShort(0)
                        .putShort(1)
                        .putShort(2)
                        .apply { flip() }
                )
                val compute =
                    d.makeComputePipelineState(
                        d.function("draw-count.comp.spv"),
                        pushConstantBytes = 8,
                    )
                val pipeline =
                    d.makeRenderPipelineState(
                        RenderPipelineDescriptor(
                            d.function("fullscreen.vert.spv"),
                            d.function("solid.frag.spv"),
                            colorAttachments =
                                listOf(
                                    RenderColorAttachmentDescriptor(
                                        blendingEnabled = true,
                                        sourceRGBBlendFactor = BlendFactor.ONE,
                                        destinationRGBBlendFactor = BlendFactor.ONE,
                                    )
                                ),
                        )
                    )
                val meshPipeline =
                    if (mesh)
                        d.makeRenderPipelineState(
                            RenderPipelineDescriptor(
                                d.function("fullscreen.mesh.spv"),
                                d.function("solid.frag.spv"),
                                meshShader = true,
                                colorAttachments =
                                    listOf(
                                        RenderColorAttachmentDescriptor(
                                            blendingEnabled = true,
                                            sourceRGBBlendFactor = BlendFactor.ONE,
                                            destinationRGBBlendFactor = BlendFactor.ONE,
                                        )
                                    ),
                            )
                        )
                    else null
                val target =
                    d.makeTexture(
                        TextureDescriptor(
                            1,
                            1,
                            usage =
                                setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                        )
                    )
                val readback = d.makeBuffer(4)
                // Multi-draw-indirect is deliberately not enabled: count draws have their own
                // feature.
                for (mode in 0..(if (mesh) 2 else 1)) for ((gpuCount, maximum) in
                    listOf(0 to 2, 1 to 2, 2 to 2, 3 to 1, 2 to 0)) {
                    d.submit {
                        compute {
                            setComputePipelineState(compute)
                            setBuffer(args, 0)
                            setBuffer(count, 1)
                            setBytes(
                                ByteBuffer.allocate(8)
                                    .order(ByteOrder.nativeOrder())
                                    .putInt(gpuCount)
                                    .putInt(mode)
                                    .array()
                            )
                            dispatchThreadgroups(Size(1))
                        }
                        render(RenderPassDescriptor(listOf(ColorAttachment(target)))) {
                            setRenderPipelineState(
                                if (mode == 2) checkNotNull(meshPipeline) else pipeline
                            )
                            if (mode == 2)
                                drawMeshThreadgroups(
                                    args,
                                    count,
                                    maximum,
                                    indirectOffset = 16,
                                    countOffset = 4,
                                )
                            else if (mode == 1)
                                drawIndexedPrimitives(
                                    indices,
                                    args,
                                    count,
                                    maximum,
                                    indirectOffset = 16,
                                    countOffset = 4,
                                )
                            else
                                drawPrimitives(
                                    args,
                                    count,
                                    maximum,
                                    indirectOffset = 16,
                                    countOffset = 4,
                                )
                        }
                        blit { copy(target, readback) }
                    }
                    val draws = minOf(gpuCount, maximum)
                    assertEquals(64 * draws, readback.readBytes(4)[1].toInt() and 255)
                }
                for ((offset, stride, maximum) in
                    listOf(
                        Triple(2L, 16, 1),
                        Triple(8L, 16, 1),
                        Triple(4L, 4, 1),
                        Triple(4L, 16, 4),
                    )) {
                    assertThrows(IllegalArgumentException::class.java) {
                        d.submit {
                            render(RenderPassDescriptor(listOf(ColorAttachment(target)))) {
                                setRenderPipelineState(pipeline)
                                drawPrimitives(
                                    args,
                                    count,
                                    maximum,
                                    indirectOffset = 16,
                                    countOffset = offset,
                                    stride = stride,
                                )
                            }
                        }
                    }
                }
            }
    }

    @Test
    fun hardwareBufferImportAndOwnershipRequireExternalResources(): Unit =
        device().use { d ->
            assertThrows(IllegalArgumentException::class.java) {
                dev.vulkano.internal.Native.importHardwareBuffer(
                    d.nativeHandle,
                    Any(),
                    4,
                    false,
                    false,
                    -1,
                    -1,
                    -3,
                )
            }
            val texture = d.makeTexture(TextureDescriptor(1, 1))
            d.makeCommandQueue().makeCommandBuffer().use { command ->
                assertThrows(IllegalArgumentException::class.java) {
                    command.acquireExternalTexture(texture)
                }
                assertThrows(IllegalArgumentException::class.java) {
                    command.releaseExternalTexture(texture)
                }
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeTexture(TextureDescriptor(1, 1, PixelFormat.EXTERNAL))
            }
        }

    @Test
    fun hardwareBufferRgbImportTransfersOwnershipAndRetainsMemory(): Unit {
        assumeTrue(
            device().use { Feature.ANDROID_HARDWARE_BUFFER in it.capabilities.availableFeatures }
        )
        device(setOf(Feature.ANDROID_HARDWARE_BUFFER)).use { d ->
            val buffer =
                android.hardware.HardwareBuffer.create(
                    2,
                    2,
                    android.hardware.HardwareBuffer.RGBA_8888,
                    1,
                    android.hardware.HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE or
                        android.hardware.HardwareBuffer.USAGE_GPU_COLOR_OUTPUT,
                )
            val usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE)
            val imported = d.importHardwareBuffer(buffer, usage)
            assertThrows(IllegalArgumentException::class.java) {
                d.importHardwareBuffer(buffer, usage)
            }
            assertNull(imported.conversionSampler)
            val readback = d.makeBuffer(16)
            assertThrows(IllegalArgumentException::class.java) {
                d.submit { render(RenderPassDescriptor(listOf(ColorAttachment(imported.texture)))) }
            }
            buffer.close() // The native memory reference must keep this image alive.
            d.submit {
                acquireExternalTexture(imported.texture, preserveContents = false)
                render(
                    RenderPassDescriptor(
                        listOf(
                            ColorAttachment(
                                imported.texture,
                                clearColor = ClearColor(1f, 0f, 0f, 1f),
                            )
                        )
                    )
                )
                blit { copy(imported.texture, readback) }
                releaseExternalTexture(imported.texture)
            }
            assertArrayEquals(
                ByteArray(16) { if (it % 4 == 0 || it % 4 == 3) -1 else 0 },
                readback.readBytes(16),
            )
            assertThrows(IllegalArgumentException::class.java) {
                d.submit { releaseExternalTexture(imported.texture) }
            }
            imported.close()
        }
    }

    @Test
    fun hardwareBufferExternalConversionSamplesWithImmutableSampler(): Unit {
        val features = setOf(Feature.ANDROID_HARDWARE_BUFFER, Feature.SAMPLER_YCBCR_CONVERSION)
        assumeTrue(device().use { it.capabilities.availableFeatures.containsAll(features) })
        device(features).use { d ->
            val buffer =
                android.hardware.HardwareBuffer.create(
                    2,
                    2,
                    android.hardware.HardwareBuffer.RGBA_8888,
                    1,
                    android.hardware.HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE or
                        android.hardware.HardwareBuffer.USAGE_GPU_COLOR_OUTPUT,
                )
            d.importHardwareBuffer(buffer, setOf(TextureUsage.COLOR_ATTACHMENT)).use { rgb ->
                d.submit {
                    acquireExternalTexture(rgb.texture, false)
                    render(
                        RenderPassDescriptor(
                            listOf(
                                ColorAttachment(
                                    rgb.texture,
                                    clearColor = ClearColor(0f, 1f, 0f, 1f),
                                )
                            )
                        )
                    )
                    releaseExternalTexture(rgb.texture)
                }
            }
            val converted = d.importHardwareBuffer(buffer, conversion = HardwareBufferConversion())
            assertEquals(PixelFormat.EXTERNAL, converted.texture.pixelFormat)
            val sampler = checkNotNull(converted.conversionSampler)
            val view = converted.texture.makeTextureView()
            val pipeline =
                d.makeRenderPipelineState(
                    d.function("fullscreen.vert.spv"),
                    d.function("sample.frag.spv"),
                    bindings =
                        listOf(
                            BindingLayout(
                                0,
                                BindingType.SAMPLED_TEXTURE,
                                immutableSampler = sampler,
                            )
                        ),
                )
            sampler.close()
            buffer.close()
            val target =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val readback = d.makeBuffer(16)
            d.submit {
                acquireExternalTexture(converted.texture)
                render(RenderPassDescriptor(listOf(ColorAttachment(target)))) {
                    setRenderPipelineState(pipeline)
                    setTexture(view, 0)
                    drawPrimitives(3)
                }
                blit { copy(target, readback) }
                releaseExternalTexture(converted.texture)
            }
            assertArrayEquals(
                ByteArray(16) { if (it % 4 == 1 || it % 4 == 3) -1 else 0 },
                readback.readBytes(16),
            )
            view.close()
            converted.close()
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
    fun rasterizationRateMapAttachmentDraw(): Unit = checkRateMapDraw(false)

    @Test
    fun subpassRateMapAttachmentsDraw(): Unit = checkRateMapDraw(true)

    private fun checkRateMapDraw(subpasses: Boolean) {
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
            val layout = if (subpasses) RenderPassLayout(listOf(PixelFormat.RGBA8_UNORM),
                listOf(RenderSubpass(listOf(0)), RenderSubpass(listOf(0)))) else null
            val p =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("fullscreen.vert.spv"),
                        d.function("solid.frag.spv"),
                        rateMapTexelSize = texel,
                        subpassLayout = layout,
                    )
                )
            val second = if (layout != null) d.makeRenderPipelineState(RenderPipelineDescriptor(
                d.function("fullscreen.vert.spv"), d.function("solid.frag.spv"),
                rateMapTexelSize = texel, subpassLayout = layout, subpassIndex = 1)) else null
            d.submit {
                blit { copy(upload, map) }
                render(
                    RenderPassDescriptor(
                        listOf(ColorAttachment(target)),
                        rasterizationRateMap = RasterizationRateMap(map, texel),
                        subpassLayout = layout,
                    )
                ) {
                    setRenderPipelineState(p)
                    drawPrimitives(3)
                    if (second != null) {
                        nextSubpass(); setRenderPipelineState(second); drawPrimitives(3)
                    }
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

    @Test fun subpassDepthResolveRetainsResultsAcrossPasses(): Unit = checkSubpassResolve(false, false, false)
    @Test fun subpassStencilResolveRetainsResultsAcrossPasses(): Unit = checkSubpassResolve(true, false, false)
    @Test fun subpassDepthResolveSupportsMultiview(): Unit = checkSubpassResolve(false, false, true)
    @Test fun subpassDepthResolveCombinesWithRateMap(): Unit = checkSubpassResolve(false, true, false)

    private fun checkSubpassResolve(stencil: Boolean, withRateMap: Boolean, multiview: Boolean) {
        val features = mutableSetOf(Feature.DEPTH_STENCIL_RESOLVE)
        if (withRateMap) features += Feature.ATTACHMENT_SHADING_RATE
        if (multiview) features += Feature.MULTIVIEW
        assumeTrue(device().use { it.capabilities.availableFeatures.containsAll(features) })
        device(features).use { d ->
            val format = if (stencil) PixelFormat.STENCIL8 else PixelFormat.DEPTH32_FLOAT
            val layers = if (multiview) 2 else 1
            val viewMask = if (multiview) 3 else 0
            val type = if (multiview) TextureType.TYPE_2D_ARRAY else TextureType.TYPE_2D
            val depth = d.makeTexture(TextureDescriptor(4, 4, format, setOf(TextureUsage.DEPTH_ATTACHMENT),
                StorageMode.MEMORYLESS, sampleCount = 4, arrayLength = layers, textureType = type))
            val resolvedDepth = d.makeTexture(TextureDescriptor(4, 4, format,
                setOf(TextureUsage.DEPTH_ATTACHMENT, TextureUsage.INPUT_ATTACHMENT),
                arrayLength = layers, textureType = type))
            val colors = List(3) { d.makeTexture(TextureDescriptor(4, 4,
                usage = setOf(TextureUsage.COLOR_ATTACHMENT), storageMode = StorageMode.MEMORYLESS,
                sampleCount = 4, arrayLength = layers, textureType = type)) }
            val resolvedColor = d.makeTexture(TextureDescriptor(4, 4,
                usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                arrayLength = layers, textureType = type))
            val layout = RenderPassLayout(List(3) { PixelFormat.RGBA8_UNORM }, listOf(
                RenderSubpass(listOf(0), usesDepthAttachment = true, resolveDepthStencil = true),
                RenderSubpass(listOf(1)),
                RenderSubpass(listOf(2), listOf(5))), depthFormat = format, sampleCount = 4,
                resolveColorAttachments = setOf(2))
            assertEquals(5, layout.depthResolveAttachmentIndex())
            val texel = if (withRateMap) d.rasterizationRateMapLimits().minimumTexelSize else null
            val map = texel?.let {
                d.makeTexture(TextureDescriptor((4 + it.width - 1) / it.width, (4 + it.height - 1) / it.height,
                    PixelFormat.R8_UINT, setOf(TextureUsage.SHADING_RATE_ATTACHMENT, TextureUsage.TRANSFER_DESTINATION)))
            }
            val upload = map?.let {
                val rate = d.fragmentShadingRates().first { 4 in it.sampleCounts }.fragmentSize
                d.makeBuffer(maxOf(4, it.width * it.height).toLong()).apply {
                    write(ByteArray(maxOf(4, it.width * it.height)) { RasterizationRateMap.encode(rate) })
                }
            }
            fun pipeline(index: Int) = d.makeRenderPipelineState(RenderPipelineDescriptor(
                d.function("fullscreen.vert.spv"),
                d.function(if (index == 2) (if (stencil) "input_stencil.frag.spv" else "input_depth.frag.spv") else "solid.frag.spv"),
                depthFormat = if (index == 0) format else null,
                depthStencil = if (stencil) DepthStencilDescriptor(depthTestEnabled = false, depthWriteEnabled = false,
                    frontFaceStencil = if (index == 0) StencilDescriptor(depthStencilPassOperation = StencilOperation.REPLACE) else null)
                    else DepthStencilDescriptor(),
                sampleCount = 4, viewMask = viewMask, subpassLayout = layout, subpassIndex = index, rateMapTexelSize = texel))
            val pipelines = List(3) { pipeline(it) }
            val output = d.makeBuffer(64L * layers)
            fun renderPass(resolve: Texture?) = RenderPassDescriptor(colors.mapIndexed { index, color ->
                ColorAttachment(color, storeAction = StoreAction.DONT_CARE, resolveTexture = if (index == 2) resolvedColor else null)
            }, DepthAttachment(depth, clearStencil = 91, resolveTexture = resolve), viewMask = viewMask,
                subpassLayout = layout, rasterizationRateMap = if (map != null) RasterizationRateMap(map, checkNotNull(texel)) else null)
            assertThrows(IllegalArgumentException::class.java) { d.submit { render(renderPass(null)) {} } }
            d.submit {
                if (map != null) blit { copy(checkNotNull(upload), map) }
                render(renderPass(resolvedDepth)) {
                    for (index in 0..2) {
                        if (index > 0) nextSubpass()
                        setRenderPipelineState(pipelines[index])
                        if (index == 0 && stencil) setStencilReferenceValue(37)
                        if (index == 2) setTexture(resolvedDepth, 0)
                        drawPrimitives(3)
                    }
                }
                blit { copy(resolvedColor, output, TextureRegion(size = Size(4, 4), sliceCount = layers)) }
            }
            val expected = byteArrayOf((if (stencil) 37 else 128).toByte(), 0, 0, 255.toByte())
            assertArrayEquals(ByteArray(64 * layers) { expected[it % 4] }, output.readBytes(64 * layers))
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
    fun accelerationRefitsCopiedCompactedAndRestoredInputs(): Unit {
        assumeTrue(device().use { Feature.RAY_QUERY in it.capabilities.availableFeatures })
        device(setOf(Feature.RAY_QUERY)).use { d ->
            fun vertices(x: Float) = d.makeBuffer(36, usage = setOf(
                BufferUsage.ACCELERATION_STRUCTURE_INPUT, BufferUsage.SHADER_DEVICE_ADDRESS)).apply {
                write(floats(x - 1, -1f, 0f, x + 1, -1f, 0f, x, 1f, 0f))
            }
            fun transform(x: Float) = floatArrayOf(1f, 0f, 0f, x, 0f, 1f, 0f, 0f, 0f, 0f, 1f, 0f)
            val originalVertices = vertices(0f)
            val source = d.makePrimitiveAccelerationStructure(listOf(TriangleGeometry(originalVertices, 3)),
                allowRefit = true, allowCompaction = true)
            d.submit { accelerationStructure { build(source) } }
            val clone = source.makeCopyDestination()
            val compact = source.makeCopyDestination(compact = true)
            d.submit { accelerationStructure { copy(source, clone); copy(source, compact) } }
            val restored = d.restoreAccelerationStructure(source.serialize())
            source.close(); originalVertices.close()
            val pipeline = d.makeComputePipelineState(d.function("query.comp.spv"))
            val result = d.makeBuffer(4)
            fun checkHit(scene: AccelerationStructure, expected: Int) {
                d.submit { compute {
                    setComputePipelineState(pipeline); setAccelerationStructure(scene, 0)
                    setBuffer(result, 1); dispatchThreads(Size(1))
                } }
                assertEquals(expected, ByteBuffer.wrap(result.readBytes(4)).order(ByteOrder.nativeOrder()).int)
            }
            for (primitive in listOf(clone, compact, restored)) {
                val scene = d.makeInstanceAccelerationStructure(listOf(AccelerationStructureInstance(primitive)), allowRefit = true)
                d.submit { accelerationStructure { build(scene) } }
                checkHit(scene, 1)
                val moved = vertices(4f)
                val geometry = listOf(TriangleGeometry(moved, 3))
                assertThrows(IllegalArgumentException::class.java) {
                    d.submit { accelerationStructure { refitPrimitives(primitive, listOf(TriangleGeometry(moved, 3, opaque = false))) } }
                }
                assertThrows(IllegalArgumentException::class.java) {
                    d.submit { accelerationStructure { refitInstances(scene, listOf(
                        AccelerationStructureInstance(primitive), AccelerationStructureInstance(primitive))) } }
                }
                if (primitive !== restored) {
                    val unbuilt = d.makePrimitiveAccelerationStructure(geometry)
                    val invalidScene = d.makeInstanceAccelerationStructure(listOf(AccelerationStructureInstance(unbuilt)))
                    assertThrows(IllegalArgumentException::class.java) {
                        d.submit { accelerationStructure { refitPrimitives(primitive, geometry); build(invalidScene) } }
                    }
                    // The failed submission must not replace the last successful refit inputs.
                    d.submit { accelerationStructure { refit(primitive); refit(scene) } }
                    checkHit(scene, 1)
                    invalidScene.close(); unbuilt.close()
                } else {
                    assertThrows(IllegalArgumentException::class.java) {
                        d.submit { accelerationStructure { refit(primitive) } }
                    }
                }
                d.makeCommandQueue().use { q -> q.makeCommandBuffer().use { command ->
                    command.accelerationStructure {
                        refitPrimitives(primitive, geometry)
                        refit(primitive) // Uses the replacement recorded earlier in this command.
                        refitInstances(scene, listOf(AccelerationStructureInstance(primitive)))
                    }
                    moved.close()
                    command.commit(); assertTrue(command.waitUntilCompleted())
                } }
                checkHit(scene, 0)
                d.submit { accelerationStructure {
                    refit(primitive) // Closed replacement buffer remains retained after submission.
                    refitInstances(scene, listOf(AccelerationStructureInstance(primitive, transform(-4f))))
                } }
                checkHit(scene, 1)
                val archive = scene.serialize()
                val restoredScene = d.restoreAccelerationStructure(archive,
                    archive.bottomLevelAddresses.associateWith { primitive })
                checkHit(restoredScene, 1)
                d.submit { accelerationStructure {
                    refitInstances(restoredScene, listOf(AccelerationStructureInstance(primitive)))
                } }
                checkHit(restoredScene, 0)
                restoredScene.close(); scene.close(); primitive.close()
            }
        }
    }

    @Test
    fun motionBlurFeatureMustBeEnabled(): Unit = device().use { d ->
        assertThrows(IllegalArgumentException::class.java) {
            d.makeComputePipelineState(d.function("motion.rgen.spv"))
        }
        if (Feature.RAY_TRACING_MOTION_BLUR !in d.capabilities.availableFeatures) {
            assertThrows(IllegalStateException::class.java) { device(setOf(Feature.RAY_TRACING_MOTION_BLUR)) }
        }
    }

    @Test fun motionVerticesTraceRefitAndRestore(): Unit = checkMotionBlur(0)
    @Test fun motionMatricesTraceRefitAndRestore(): Unit = checkMotionBlur(1)
    @Test fun motionSrtTraceRefitAndRestore(): Unit = checkMotionBlur(2)

    private fun checkMotionBlur(mode: Int) {
        assumeTrue(device().use { Feature.RAY_TRACING_MOTION_BLUR in it.capabilities.availableFeatures })
        device(setOf(Feature.RAY_TRACING_MOTION_BLUR)).use { d ->
            fun vertices(x: Float) = d.makeBuffer(36, usage = setOf(
                BufferUsage.ACCELERATION_STRUCTURE_INPUT, BufferUsage.SHADER_DEVICE_ADDRESS)).apply {
                write(floats(x - 1, -1f, 0f, x + 1, -1f, 0f, x, 1f, 0f))
            }
            fun transform(x: Float) = floatArrayOf(1f, 0f, 0f, x, 0f, 1f, 0f, 0f, 0f, 0f, 1f, 0f)
            val start = vertices(0f); val end = vertices(4f)
            fun geometry(reverse: Boolean) = listOf(TriangleGeometry(
                if (mode == 0 && reverse) end else start, 3,
                motionVertexBuffer = if (mode != 0) null else if (reverse) start else end))
            val primitive = d.makePrimitiveAccelerationStructure(geometry(false), allowRefit = true, allowCompaction = true)
            fun instance(structure: AccelerationStructure, reverse: Boolean): AccelerationStructureInstance {
                val a = if (reverse) 4f else 0f; val b = if (reverse) 0f else 4f
                val motion = when (mode) {
                    1 -> AccelerationMotionTransform.Matrix(transform(a), transform(b))
                    2 -> AccelerationMotionTransform.Srt(
                        SrtTransform(translation = floatArrayOf(a, 0f, 0f)),
                        SrtTransform(translation = floatArrayOf(b, 0f, 0f)))
                    else -> null
                }
                return AccelerationStructureInstance(structure, motionTransform = motion)
            }
            val scene = d.makeInstanceAccelerationStructure(listOf(instance(primitive, false)), allowRefit = true)
            val descriptor = RayTracingPipelineDescriptor(listOf(
                RayShader(d.function("motion.rgen.spv"), RayShaderStage.RAY_GENERATION),
                RayShader(d.function("hit.rmiss.spv"), RayShaderStage.MISS),
                RayShader(d.function("hit.rchit.spv"), RayShaderStage.CLOSEST_HIT)),
                listOf(RayShaderGroup(general = 0), RayShaderGroup(general = 1), RayShaderGroup(closestHit = 2)),
                supportsMotionBlur = true)
            assertThrows(IllegalArgumentException::class.java) {
                d.makeRayTracingPipelineState(descriptor.copy(supportsMotionBlur = false))
            }
            val pipeline = d.makeRayTracingPipelineState(descriptor)
            val output = d.makeBuffer(12)
            fun trace(target: AccelerationStructure, expected: IntArray) {
                d.submit { rayTracing {
                    setRayTracingPipelineState(pipeline); setAccelerationStructure(target, 0)
                    setBuffer(output, 1); traceRays(Size(3))
                } }
                val data = ByteBuffer.wrap(output.readBytes(12)).order(ByteOrder.nativeOrder())
                assertArrayEquals(expected, IntArray(3) { data.int })
            }
            d.submit { accelerationStructure { build(primitive); build(scene) } }
            trace(scene, intArrayOf(1, 0, 0))
            d.submit { accelerationStructure {
                if (mode == 0) refitPrimitives(primitive, geometry(true))
                refitInstances(scene, listOf(instance(primitive, true)))
            } }
            trace(scene, intArrayOf(0, 0, 1))
            val compact = primitive.makeCopyDestination(compact = true)
            d.submit { accelerationStructure {
                copy(primitive, compact)
                refitInstances(scene, listOf(instance(compact, true)))
            } }
            trace(scene, intArrayOf(0, 0, 1))
            val restoredPrimitive = d.restoreAccelerationStructure(compact.serialize())
            val archive = scene.serialize()
            val restoredScene = d.restoreAccelerationStructure(archive,
                archive.bottomLevelAddresses.associateWith { restoredPrimitive })
            primitive.close(); compact.close(); scene.close()
            trace(restoredScene, intArrayOf(0, 0, 1))
            d.submit { accelerationStructure {
                refitPrimitives(restoredPrimitive, geometry(false))
                refitInstances(restoredScene, listOf(instance(restoredPrimitive, false)))
            } }
            start.close(); end.close(); restoredPrimitive.close()
            trace(restoredScene, intArrayOf(1, 0, 0))
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
