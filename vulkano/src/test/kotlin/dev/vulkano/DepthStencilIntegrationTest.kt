package dev.vulkano

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test

class DepthStencilIntegrationTest {
    @Before fun enabled() = assumeTrue(System.getProperty("vulkano.runNativeTests") == "true")
    private fun device(features: Set<Feature> = emptySet()) = Device.create(requiredFeatures = features, enableValidation = System.getenv("VULKANO_VALIDATION") != null, allowSoftwareRenderer = true)
    private fun Device.submit(block: CommandBuffer.() -> Unit) {
        makeCommandQueue().use { q -> q.makeCommandBuffer().use { c -> c.block(); c.commit(); assertTrue(c.waitUntilCompleted()) } }
    }
    private fun Device.depth(): Texture {
        val descriptor = TextureDescriptor(2, 2, PixelFormat.DEPTH32_FLOAT_STENCIL8,
            setOf(TextureUsage.DEPTH_ATTACHMENT, TextureUsage.SAMPLED, TextureUsage.TRANSFER_SOURCE))
        assumeTrue(supportsTexture(descriptor))
        return makeTexture(descriptor)
    }
    private fun Device.function(name: String) = makeLibrary(TestShaders.read(name)).makeFunction()

    @Test fun independentLoadStorePreservesOnlyTheSelectedAspect(): Unit = device().use { d ->
        val texture = d.depth()
        val readback = d.makeBuffer(16)
        val depth = texture.makeTextureView(aspect = TextureAspect.DEPTH)
        val stencil = texture.makeTextureView(aspect = TextureAspect.STENCIL)
        fun pass(a: DepthAttachment) = d.submit { render(RenderPassDescriptor(emptyList(), a)) {} }
        pass(DepthAttachment(texture, storeAction = StoreAction.STORE, clearDepth = 0.25f, clearStencil = 7))
        pass(DepthAttachment(texture, loadAction = LoadAction.LOAD, storeAction = StoreAction.STORE,
            stencilLoadAction = LoadAction.CLEAR, stencilStoreAction = StoreAction.STORE, clearStencil = 19))
        d.submit { blit { copy(depth, readback) } }
        val values = ByteBuffer.wrap(readback.readBytes(16)).order(ByteOrder.nativeOrder())
        repeat(4) { assertEquals(0.25f, values.float, 0f) }
        d.submit { blit { copy(stencil, readback) } }
        assertArrayEquals(ByteArray(4) { 19 }, readback.readBytes(4))
        pass(DepthAttachment(texture, loadAction = LoadAction.LOAD, storeAction = StoreAction.DONT_CARE,
            stencilLoadAction = LoadAction.LOAD, stencilStoreAction = StoreAction.STORE))
        assertThrows(IllegalArgumentException::class.java) { d.submit { blit { copy(depth, readback) } } }
        d.submit { blit { copy(stencil, readback) } }
        assertArrayEquals(ByteArray(4) { 19 }, readback.readBytes(4))
        // Loading valid stencil does not require discarded depth to be initialized.
        pass(DepthAttachment(texture, loadAction = LoadAction.CLEAR, clearDepth = 0.75f, storeAction = StoreAction.STORE,
            stencilLoadAction = LoadAction.LOAD, stencilStoreAction = StoreAction.DONT_CARE))
        d.submit { blit { copy(depth, readback) } }
        assertEquals(0.75f, ByteBuffer.wrap(readback.readBytes(4)).order(ByteOrder.nativeOrder()).float, 0f)
        assertThrows(IllegalArgumentException::class.java) { d.submit { blit { copy(stencil, readback) } } }
        // A failed submission must not publish the proposed depth clear.
        assertThrows(IllegalArgumentException::class.java) {
            d.submit { render(RenderPassDescriptor(emptyList(), DepthAttachment(texture, clearDepth = 0.5f,
                storeAction = StoreAction.STORE, stencilLoadAction = LoadAction.LOAD))) {} }
        }
        d.submit { blit { copy(depth, readback) } }
        assertEquals(0.75f, ByteBuffer.wrap(readback.readBytes(4)).order(ByteOrder.nativeOrder()).float, 0f)
    }

    @Test fun mixedLayoutsWriteOnlyTheWritableAspect(): Unit = device().use { d ->
        val texture=d.depth()
        val color=d.makeTexture(TextureDescriptor(2,2,usage=setOf(TextureUsage.COLOR_ATTACHMENT)))
        val output=d.makeBuffer(16)
        val vertex=d.function("fullscreen.vert.spv")
        val fragment=d.function("solid.frag.spv")
        d.submit { render(RenderPassDescriptor(emptyList(),DepthAttachment(texture,storeAction=StoreAction.STORE,
            clearDepth=0.25f,clearStencil=7))) {} }
        val stencilPipeline=d.makeRenderPipelineState(RenderPipelineDescriptor(vertex,fragment,depthFormat=texture.pixelFormat,
            depthStencil=DepthStencilDescriptor(depthWriteEnabled=false,depthTestEnabled=false,
                frontFaceStencil=StencilDescriptor(depthStencilPassOperation=StencilOperation.REPLACE))))
        d.submit { render(RenderPassDescriptor(listOf(ColorAttachment(color)),DepthAttachment(texture,
            loadAction=LoadAction.LOAD,storeAction=StoreAction.STORE,depthReadOnly=true))) {
            setRenderPipelineState(stencilPipeline);setStencilReferenceValue(13);drawPrimitives(3)
        };blit { copy(texture.makeTextureView(aspect=TextureAspect.DEPTH),output) } }
        assertEquals(0.25f,ByteBuffer.wrap(output.readBytes(4)).order(ByteOrder.nativeOrder()).float,0f)
        val depthPipeline=d.makeRenderPipelineState(RenderPipelineDescriptor(vertex,fragment,depthFormat=texture.pixelFormat,
            depthStencil=DepthStencilDescriptor(depthCompareFunction=CompareFunction.ALWAYS,
                frontFaceStencil=StencilDescriptor(writeMask=0))))
        d.submit { render(RenderPassDescriptor(listOf(ColorAttachment(color)),DepthAttachment(texture,
            loadAction=LoadAction.LOAD,storeAction=StoreAction.STORE,stencilReadOnly=true))) {
            setRenderPipelineState(depthPipeline);drawPrimitives(3)
        };blit { copy(texture.makeTextureView(aspect=TextureAspect.DEPTH),output) } }
        assertEquals(0.5f,ByteBuffer.wrap(output.readBytes(4)).order(ByteOrder.nativeOrder()).float,0f)
        d.submit { blit { copy(texture.makeTextureView(aspect=TextureAspect.STENCIL),output) } }
        assertArrayEquals(ByteArray(4){13},output.readBytes(4))
    }

    @Test fun subpassesAndMemorylessRespectIndependentActions(): Unit = device().use { d ->
        val texture = d.depth()
        val output = d.makeBuffer(16)
        val layout = RenderPassLayout(emptyList(), listOf(RenderSubpass(emptyList(), usesDepthAttachment=true),
            RenderSubpass(emptyList()), RenderSubpass(emptyList(), usesDepthAttachment=true)), depthFormat=texture.pixelFormat)
        fun submit(a: DepthAttachment) = d.submit { render(RenderPassDescriptor(emptyList(), a, subpassLayout=layout)) { nextSubpass(); nextSubpass() } }
        submit(DepthAttachment(texture, storeAction=StoreAction.STORE,clearDepth=0.125f,clearStencil=23))
        submit(DepthAttachment(texture, loadAction=LoadAction.LOAD,storeAction=StoreAction.STORE,
            stencilLoadAction=LoadAction.CLEAR,stencilStoreAction=StoreAction.STORE,clearStencil=42))
        d.submit { blit { copy(texture.makeTextureView(aspect=TextureAspect.DEPTH),output) } }
        assertEquals(0.125f,ByteBuffer.wrap(output.readBytes(4)).order(ByteOrder.nativeOrder()).float,0f)
        d.submit { blit { copy(texture.makeTextureView(aspect=TextureAspect.STENCIL),output) } }
        assertArrayEquals(ByteArray(4){42},output.readBytes(4))
        val temporary = d.makeTexture(TextureDescriptor(2,2,texture.pixelFormat,setOf(TextureUsage.DEPTH_ATTACHMENT),
            storageMode=StorageMode.MEMORYLESS))
        assertThrows(IllegalArgumentException::class.java) { d.submit {
            render(RenderPassDescriptor(emptyList(),DepthAttachment(temporary,stencilStoreAction=StoreAction.STORE))) {}
        } }
        assertThrows(IllegalArgumentException::class.java) { d.submit {
            render(RenderPassDescriptor(emptyList(),DepthAttachment(temporary,stencilLoadAction=LoadAction.LOAD))) {}
        } }
    }

    @Test fun readOnlyDepthSamplingDoesNotRaceWithStoreOperations(): Unit {
        device().use { assumeTrue(Feature.ATTACHMENT_STORE_NONE in it.capabilities.availableFeatures) }
        device(setOf(Feature.ATTACHMENT_STORE_NONE)).use { d ->
        val texture = d.depth()
        val depth = texture.makeTextureView(aspect = TextureAspect.DEPTH)
        val stencil = texture.makeTextureView(aspect = TextureAspect.STENCIL)
        val color = d.makeTexture(TextureDescriptor(2,2,usage=setOf(TextureUsage.COLOR_ATTACHMENT,TextureUsage.TRANSFER_SOURCE)))
        val output = d.makeBuffer(16)
        val sampler = d.makeSampler(SamplerDescriptor(linearFiltering = false))
        val vertex = d.function("fullscreen.vert.spv")
        val fragment = d.function("sample-readonly-depth.frag.spv")
        val state = DepthStencilDescriptor(depthWriteEnabled = false, depthTestEnabled = false,
            frontFaceStencil = StencilDescriptor(writeMask = 0))
        val pipeline = d.makeRenderPipelineState(RenderPipelineDescriptor(vertex,fragment,
            depthFormat=texture.pixelFormat,depthStencil=state))
        d.submit { render(RenderPassDescriptor(emptyList(), DepthAttachment(texture, storeAction=StoreAction.STORE,
            clearDepth=0.25f,clearStencil=7))) {} }
        val attachment = DepthAttachment(texture, loadAction=LoadAction.LOAD,storeAction=StoreAction.NONE,
            depthReadOnly=true,stencilReadOnly=true,stencilLoadAction=LoadAction.LOAD,stencilStoreAction=StoreAction.NONE)
        d.submit { render(RenderPassDescriptor(listOf(ColorAttachment(color)),attachment)) {
            setRenderPipelineState(pipeline);setTexture(depth,0,sampler);setStencilReferenceValue(13);drawPrimitives(3)
        };blit { copy(color,output) } }
        val bytes=output.readBytes(16)
        repeat(4) { assertEquals(64,bytes[it*4].toInt() and 255) }
        d.submit { blit { copy(stencil,output) } }
        assertArrayEquals(ByteArray(4){7},output.readBytes(4))
        val invalid = d.makeRenderPipelineState(RenderPipelineDescriptor(vertex,fragment,
            depthFormat=texture.pixelFormat,depthStencil=state.copy(depthWriteEnabled=true)))
        assertThrows(IllegalArgumentException::class.java) { d.submit {
            render(RenderPassDescriptor(listOf(ColorAttachment(color)),attachment)) {
                setRenderPipelineState(invalid);setTexture(depth,0,sampler);drawPrimitives(3)
            }
        } }
        assertThrows(IllegalArgumentException::class.java) { d.submit {
            render(RenderPassDescriptor(listOf(ColorAttachment(color)),attachment.copy(depthReadOnly=false))) {
                setRenderPipelineState(pipeline);setTexture(depth,0,sampler);drawPrimitives(3)
            }
        } }
    }
    }
}
