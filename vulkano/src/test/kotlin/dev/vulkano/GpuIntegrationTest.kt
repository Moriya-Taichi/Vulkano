package dev.vulkano

import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Runs the production Kotlin -> JNI -> Vulkan path on a host software ICD. */
class GpuIntegrationTest {
    @Before fun nativeLibraryAvailable() {
        assumeTrue("Supply -Pvulkano.hostLibraryPath=<CMake build>", System.getProperty("vulkano.runNativeTests") == "true")
    }
    private fun device() = Device.create(enableValidation = System.getenv("VULKANO_VALIDATION") != null, allowSoftwareRenderer = true)
    private fun shader(name: String) = checkNotNull(javaClass.classLoader!!.getResourceAsStream(name)).use { it.readBytes() }

    @Test fun computeAndReadbackThroughKotlin() {
        device().use { device ->
            assertTrue(device.capabilities.enabledFeatures.isEmpty())
            assertTrue(device.memoryHeaps().all { it.sizeBytes > 0 })
            val function = device.makeLibrary(shader("double.comp.spv")).makeFunction()
            device.makeComputePipelineState(function).use { pipeline ->
                assertEquals(Size(64), pipeline.threadgroupSize)
                assertEquals(4, pipeline.pushConstantBytes)
                device.makeBuffer(257 * 4L).use { buffer ->
                    val data = ByteBuffer.allocateDirect(257 * 4).order(ByteOrder.nativeOrder())
                    repeat(257) { data.putFloat(it * 4, it.toFloat()) }
                    buffer.write(data)
                    device.makeCommandQueue().use { queue ->
                        queue.makeCommandBuffer().use { command ->
                            command.compute {
                                setComputePipelineState(pipeline)
                                setBuffer(buffer, index = 0)
                                setBytes(ByteBuffer.allocate(4).order(ByteOrder.nativeOrder()).putInt(257).array())
                                dispatchThreads(Size(257))
                            }
                            command.commit()
                            assertTrue(command.waitUntilCompleted())
                            assertEquals(CommandBufferStatus.COMPLETED, command.status)
                            assertThrows(IllegalStateException::class.java) { command.commit() }
                        }
                    }
                    buffer.read(data)
                    repeat(257) { assertEquals(it * 2f, data.getFloat(it * 4), 0f) }
                }
            }
        }
    }
    @Test fun uploadBufferSupportsDirectGpuAccessAndRejectsCpuReads() {
        device().use { device ->
            device.makeUploadBuffer(16, setOf(BufferUsage.STORAGE, BufferUsage.TRANSFER_SOURCE)).use { upload ->
                assertTrue(upload.isCpuWriteOnly)
                val bytes = ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder())
                repeat(4) { bytes.putFloat(it * 4, (it + 1).toFloat()) }
                upload.write(bytes)
                assertThrows(IllegalArgumentException::class.java) { upload.readBytes(16) }
                device.makeBuffer(16).use { result ->
                    device.makeComputePipelineState(device.makeLibrary(shader("double.comp.spv")).makeFunction()).use { pipeline ->
                        device.makeCommandQueue().use { queue ->
                            queue.makeCommandBuffer().use { command ->
                                command.compute {
                                    setComputePipelineState(pipeline); setBuffer(upload, index = 0)
                                    setBytes(ByteBuffer.allocate(4).order(ByteOrder.nativeOrder()).putInt(4).array())
                                    dispatchThreads(Size(4))
                                }
                                command.blit { copy(upload, result, length = 16) }
                                command.commit(); command.waitUntilCompleted()
                            }
                        }
                    }
                    val out = ByteBuffer.wrap(result.readBytes(16)).order(ByteOrder.nativeOrder())
                    repeat(4) { assertEquals((it + 1) * 2f, out.float, 0f) }
                }
            }
        }
    }
    @Test fun transferUsesByteBufferPositionAndRetainsClosedSource() {
        device().use { device ->
            val source = device.makeBuffer(16)
            val bytes = ByteBuffer.allocateDirect(24).order(ByteOrder.nativeOrder())
            bytes.putInt(4, 123); bytes.putInt(8, 456); bytes.position(4); bytes.limit(12)
            source.write(bytes, offset = 4)
            assertEquals(4, bytes.position())
            device.makeBuffer(16, StorageMode.PRIVATE).use { gpu ->
                device.makeBuffer(16).use { result ->
                    device.makeCommandQueue().use { queue ->
                        queue.makeCommandBuffer().use { command ->
                            command.blit { copy(source, gpu, length = 8, sourceOffset = 4, destinationOffset = 4); copy(gpu, result, length = 8, sourceOffset = 4) }
                            source.close()
                            command.commit(); command.waitUntilCompleted()
                        }
                    }
                    val out = ByteBuffer.wrap(result.readBytes(8)).order(ByteOrder.nativeOrder())
                    assertEquals(123, out.int); assertEquals(456, out.int)
                    assertThrows(IllegalArgumentException::class.java) { gpu.readBytes(4) }
                }
            }
        }
    }
    @Test fun offscreenClearAndTextureReadback() {
        device().use { device ->
            val descriptor = TextureDescriptor(8, 8, usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.TRANSFER_SOURCE))
            assertTrue(device.supportsTexture(descriptor))
            device.makeTexture(descriptor).use { target ->
                device.makeBuffer(8 * 8 * 4L).use { result ->
                    device.makeCommandQueue().use { queue ->
                        queue.makeCommandBuffer().use { command ->
                            command.render(RenderPassDescriptor(ColorAttachment(target, clearColor = ClearColor(1f, 0f, 0f))))
                            command.blit { copy(target, result) }
                            command.commit(); command.waitUntilCompleted()
                        }
                    }
                    val pixels = result.readBytes(8 * 8 * 4)
                    for (i in pixels.indices step 4) assertArrayEquals(byteArrayOf(-1, 0, 0, -1), pixels.copyOfRange(i, i + 4))
                }
            }
        }
    }
    @Test fun rejectsActiveEncoderAndClosesChildrenWithDevice() {
        val d = device()
        val buffer = d.makeBuffer(16)
        val command = d.makeCommandQueue().makeCommandBuffer()
        val encoder = command.makeBlitCommandEncoder()
        assertThrows(IllegalStateException::class.java) { command.commit() }
        encoder.endEncoding()
        assertThrows(IllegalStateException::class.java) { encoder.copy(buffer, buffer) }
        d.close()
        assertThrows(IllegalStateException::class.java) { buffer.readBytes(4) }
        command.close(); buffer.close(); d.close()
    }
    @Test fun rejectsCrossDeviceBindingsAndAbortsFailedScope() {
        device().use { a -> device().use { b ->
            a.makeCommandQueue().makeCommandBuffer().use { command ->
                b.makeBuffer(16).use { foreign ->
                    assertThrows(IllegalArgumentException::class.java) { command.compute { setBuffer(foreign, index = 0) } }
                    assertThrows(IllegalStateException::class.java) { command.commit() }
                }
            }
        } }
    }
}
