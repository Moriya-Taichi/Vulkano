package dev.vulkano

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test

class BindingIntegrationTest {
    @Before fun enabled() = assumeTrue(System.getProperty("vulkano.runNativeTests") == "true")

    private fun device() =
        Device.create(
            enableValidation = System.getenv("VULKANO_VALIDATION") != null,
            allowSoftwareRenderer = true,
        )

    private fun Device.function(name: String) = makeLibrary(TestShaders.read(name)).makeFunction()

    private fun Device.value(n: Int) =
        makeBuffer(4).apply {
            write(
                ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder()).putInt(n).flip()
                    as ByteBuffer
            )
        }

    private fun Buffer.value() = ByteBuffer.wrap(readBytes(4)).order(ByteOrder.nativeOrder()).int

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
    fun descriptorSetsKeepEqualBindingNumbersDistinct(): Unit =
        device().use { d ->
            val f = d.function("multiple-sets.comp.spv")
            val layout =
                listOf(0, 2, 3).map { BindingLayout(0, BindingType.STORAGE_BUFFER, set = it) }
            val a = d.value(19)
            val b = d.value(23)
            val result = d.makeBuffer(4)
            for (explicit in listOf(emptyList(), layout)) {
                val p = d.makeComputePipelineState(f, explicit)
                d.submit {
                    compute {
                        setComputePipelineState(p)
                        setBuffer(a, 0)
                        setBuffer(b, 0, set = 2)
                        setBuffer(result, 0, set = 3)
                        dispatchThreadgroups(Size(1))
                    }
                }
                assertEquals(42, result.value())
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeComputePipelineState(f, layout.map { it.copy(set = 0) })
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.submit {
                    compute {
                        setComputePipelineState(d.makeComputePipelineState(f))
                        setBuffer(a, 0)
                        setBuffer(result, 0, set = 3)
                        dispatchThreadgroups(Size(1))
                    }
                }
            }
            val invalid = TestShaders.read("multiple-sets.comp.spv").copyOf()
            val words = ByteBuffer.wrap(invalid).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer()
            var i = 5
            while (i < words.limit()) {
                val instruction = words[i]
                if (
                    instruction and 65535 == 71 &&
                        instruction ushr 16 == 4 &&
                        words[i + 2] == 34 &&
                        words[i + 3] == 2
                )
                    words.put(i + 3, d.capabilities.limits.maxBoundDescriptorSets)
                i += instruction ushr 16
            }
            assertThrows(IllegalArgumentException::class.java) {
                d.makeComputePipelineState(d.makeLibrary(invalid).makeFunction())
            }
        }

    @Test
    fun groupsSnapshotUpdatesAndRetainClosedResources(): Unit =
        device().use { d ->
            val pipeline = d.makeComputePipelineState(d.function("multiple-sets.comp.spv"))
            val first = d.value(2)
            val second = d.value(9)
            val other = d.value(3)
            val out1 = d.makeBuffer(4)
            val out2 = d.makeBuffer(4)
            val a = d.makeResourceBindings { setBuffer(first, 0) }
            val b = d.makeResourceBindings(2) { setBuffer(other, 0) }
            val output = d.makeResourceBindings(3) { setBuffer(out1, 0) }
            first.close()
            other.close()
            d.submit {
                compute {
                    setComputePipelineState(pipeline)
                    setResourceBindings(a)
                    setResourceBindings(b)
                    setResourceBindings(output)
                    dispatchThreadgroups(Size(1))
                    a.update { setBuffer(second, 0) }
                    second.close()
                    output.update { setBuffer(out2, 0) }
                    dispatchThreadgroups(Size(1))
                    a.close()
                    b.close()
                    output.close()
                }
            }
            assertEquals(5, out1.value())
            assertEquals(12, out2.value())
        }

    @Test
    fun failedTransactionsAndExplicitOverridesPreserveTheGroup(): Unit =
        device().use { d ->
            val pipeline = d.makeComputePipelineState(d.function("multiple-sets.comp.spv"))
            val a = d.value(4)
            val b = d.value(6)
            val replacement = d.value(20)
            val result = d.makeBuffer(4)
            val group = d.makeResourceBindings { setBuffer(a, 0) }
            assertThrows(IllegalStateException::class.java) {
                group.update {
                    setBuffer(replacement, 0)
                    error("abort")
                }
            }
            device().use { foreign ->
                val wrong = foreign.value(99)
                assertThrows(IllegalArgumentException::class.java) {
                    group.update { setBuffer(wrong, 0) }
                }
            }
            fun run(override: Boolean) =
                d.submit {
                    compute {
                        setComputePipelineState(pipeline)
                        setResourceBindings(group)
                        setBuffer(b, 0, set = 2)
                        setBuffer(result, 0, set = 3)
                        if (override) setBuffer(replacement, 0)
                        dispatchThreadgroups(Size(1))
                    }
                }
            run(false)
            assertEquals(10, result.value())
            run(true)
            assertEquals(26, result.value())
            run(false)
            assertEquals(10, result.value())
            group.update { remove(0) }
            assertThrows(IllegalArgumentException::class.java) { run(false) }
            group.update {
                clear()
                setBuffer(replacement, 0)
            }
            run(false)
            assertEquals(26, result.value())
            assertThrows(IllegalArgumentException::class.java) {
                d.makeResourceBindings(d.capabilities.limits.maxBoundDescriptorSets)
            }
            var transaction: ResourceBindingUpdate? = null
            group.update { transaction = this }
            assertThrows(IllegalStateException::class.java) { transaction!!.clear() }
        }

    @Test
    fun graphicsStagesUseDifferentTypesAtTheSameBindingNumber(): Unit =
        device().use { d ->
            fun data(vararg values: Float) =
                ByteBuffer.allocateDirect(values.size * 4).order(ByteOrder.nativeOrder()).apply {
                    values.forEach { putFloat(it) }
                    flip()
                }
            val parameters =
                d.makeBuffer(16, usage = setOf(BufferUsage.UNIFORM)).apply {
                    write(data(1f, 0.5f, 1f, 1f))
                }
            val factor = d.makeBuffer(16).apply { write(data(0.5f, 0.5f, 1f, 1f)) }
            val vertex = d.makeResourceBindings { setBuffer(parameters, 0) }
            val fragment = d.makeResourceBindings(2) { setBuffer(factor, 0) }
            val pipeline =
                d.makeRenderPipelineState(
                    RenderPipelineDescriptor(
                        d.function("multiple-sets.vert.spv"),
                        d.function("multiple-sets.frag.spv"),
                    )
                )
            val color =
                d.makeTexture(
                    TextureDescriptor(
                        2,
                        2,
                        usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE),
                    )
                )
            val result = d.makeBuffer(16)
            d.submit {
                render(RenderPassDescriptor(listOf(ColorAttachment(color)))) {
                    setRenderPipelineState(pipeline)
                    setResourceBindings(vertex)
                    setResourceBindings(fragment)
                    drawPrimitives(3)
                }
                blit { copy(color, result) }
            }
            val bytes = result.readBytes(16)
            repeat(4) { i ->
                assertEquals(128, bytes[i * 4].toInt() and 255)
                assertEquals(64, bytes[i * 4 + 1].toInt() and 255)
                assertEquals(255, bytes[i * 4 + 2].toInt() and 255)
                assertEquals(255, bytes[i * 4 + 3].toInt() and 255)
            }
        }
}
