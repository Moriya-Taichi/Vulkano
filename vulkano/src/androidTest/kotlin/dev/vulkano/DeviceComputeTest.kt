package dev.vulkano

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.nio.ByteBuffer
import java.nio.ByteOrder

@RunWith(AndroidJUnit4::class)
class DeviceComputeTest {
    @Test fun executesRealVulkanComputeAndReadback() {
        val assets = InstrumentationRegistry.getInstrumentation().context.assets
        Device.create(allowSoftwareRenderer = true).use { device ->
            val function = device.makeLibrary(assets.open("double.comp.spv").use { it.readBytes() }).makeFunction()
            device.makeComputePipelineState(function, listOf(BindingLayout(0, BindingType.STORAGE_BUFFER)), 4).use { pipeline ->
                device.makeBuffer(16).use { buffer ->
                    val data = ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder())
                    data.asFloatBuffer().put(floatArrayOf(1f, 2f, 3f, 4f)); buffer.write(data)
                    device.makeCommandQueue().use { queue -> queue.makeCommandBuffer().use { command ->
                        command.compute {
                            setComputePipelineState(pipeline); setBuffer(buffer, index = 0)
                            setBytes(ByteBuffer.allocate(4).order(ByteOrder.nativeOrder()).putInt(4).array())
                            dispatchThreads(Size(4))
                        }
                        command.commit(); assertTrue(command.waitUntilCompleted())
                    } }
                    buffer.read(data)
                    assertArrayEquals(floatArrayOf(2f, 4f, 6f, 8f), FloatArray(4) { data.getFloat(it * 4) }, 0f)
                }
            }
        }
    }
}
