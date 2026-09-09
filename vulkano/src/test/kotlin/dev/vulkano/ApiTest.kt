package dev.vulkano

import org.junit.Assert.*
import org.junit.Test

class ApiTest {
    @Test fun halfConstantsRoundTiesAndPreserveSpecialValues() {
        fun bits(value: Float) = FunctionConstants().setHalf(0, value).pack()[2]
        assertEquals(0x3c00, bits(1.00048828125f))
        assertEquals(0x3c02, bits(1.00146484375f))
        assertEquals(1, bits(Math.scalb(1f, -24)))
        assertEquals(0, bits(Math.scalb(1f, -25)))
        assertEquals(2, bits(Math.scalb(3f, -25)))
        assertEquals(0x8000, bits(-0f))
        assertEquals(0x7bff, bits(65504f))
        assertEquals(0x7c00, bits(65520f))
        assertEquals(0xfc00, bits(Float.NEGATIVE_INFINITY))
        assertTrue(bits(Float.NaN) and 0x7c00 == 0x7c00)
        assertTrue(bits(Float.NaN) and 0x3ff != 0)
        assertEquals(0x7e15, FunctionConstants().setHalfBits(0, 0x7e15).pack()[2])
        val constants = FunctionConstants().setUInt(0, UInt.MAX_VALUE).setULong(1, ULong.MAX_VALUE)
        val first = constants.pack()
        constants.setInt(0, 0)
        assertArrayEquals(intArrayOf(0, 4, -1, 0, 1, 8, -1, -1), first)
    }
    @Test fun subpassResolveInputsMustFollowTheirProducer() {
        val layout = RenderPassLayout(listOf(PixelFormat.RGBA8_UNORM), listOf(
            RenderSubpass(listOf(0), usesDepthAttachment = true, resolveDepthStencil = true),
            RenderSubpass(emptyList(), inputAttachments = listOf(2, 3))),
            depthFormat = PixelFormat.DEPTH32_FLOAT, sampleCount = 4, resolveColorAttachments = setOf(0))
        assertEquals(2, layout.resolveAttachmentIndex(0))
        assertEquals(3, layout.depthResolveAttachmentIndex())
        assertThrows(IllegalArgumentException::class.java) {
            layout.copy(subpasses = listOf(RenderSubpass(listOf(0), listOf(2))))
        }
        assertThrows(IllegalArgumentException::class.java) {
            layout.copy(subpasses = listOf(RenderSubpass(listOf(0), listOf(3), true, true)))
        }
        assertThrows(IllegalArgumentException::class.java) {
            layout.copy(subpasses = listOf(RenderSubpass(listOf(0), resolveDepthStencil = true)))
        }
        assertThrows(IllegalArgumentException::class.java) { layout.copy(sampleCount = 1) }
    }
    @Test fun motionTransformsOwnTheirEndpointsAndValidateSrt() {
        val translation = floatArrayOf(4f, 5f, 6f)
        val srt = SrtTransform(translation = translation)
        translation.fill(0f)
        assertArrayEquals(floatArrayOf(4f, 5f, 6f), srt.packed.copyOfRange(13, 16), 0f)
        val start = floatArrayOf(1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 1f, 0f)
        val end = start.copyOf().apply { this[3] = 4f }
        val matrix = AccelerationMotionTransform.Matrix(start, end)
        start.fill(0f); end.fill(0f)
        assertEquals(1f, matrix.packed[0], 0f)
        assertEquals(4f, matrix.packed[15], 0f)
        assertEquals(4f, AccelerationMotionTransform.Srt(SrtTransform(), srt).packed[29], 0f)
        assertThrows(IllegalArgumentException::class.java) { SrtTransform(scale = floatArrayOf(0f, 1f, 1f)) }
        assertThrows(IllegalArgumentException::class.java) { SrtTransform(rotationQuaternion = floatArrayOf(0f, 0f, 0f, 2f)) }
        assertThrows(IllegalArgumentException::class.java) { SrtTransform(translation = floatArrayOf(Float.NaN, 0f, 0f)) }
        assertThrows(IllegalArgumentException::class.java) { AccelerationMotionTransform.Matrix(FloatArray(11), FloatArray(12)) }
    }
    @Test fun graphConstantsOwnTheirDataAndValidateLayouts() {
        val descriptor = TensorResourceDescriptor(listOf(2, 3), layout = TensorLayout.LINEAR,
            usage = setOf(TensorUsage.MACHINE_LEARNING))
        val bytes = ByteArray(24) { it.toByte() }
        val constant = MachineLearningConstant(7, descriptor, bytes)
        bytes.fill(0)
        assertEquals(23, constant.bytes.last().toInt())
        assertThrows(IllegalArgumentException::class.java) { MachineLearningConstant(7, descriptor, ByteArray(23)) }
        assertThrows(IllegalArgumentException::class.java) { MachineLearningConstant(-1, descriptor, bytes) }
        assertThrows(IllegalArgumentException::class.java) { MachineLearningTensorBinding(0, TensorResourceDescriptor(listOf(2, 3))) }
        assertThrows(IllegalArgumentException::class.java) { MachineLearningTensorBinding(0, descriptor, 0) }
    }
    @Test fun tensorLayoutsCheckStridesAndOverflow() {
        val strided = TensorResourceDescriptor(listOf(2, 3), layout = TensorLayout.LINEAR, byteStrides = listOf(16, 4))
        assertEquals(6L, strided.elementCount)
        assertEquals(28L, strided.byteLength)
        assertEquals(24L, strided.byteOffset(listOf(1, 2)))
        assertEquals(6L, TensorResourceDescriptor(listOf(2, 3), TensorDataType.BOOL).byteLength)
        assertThrows(IllegalArgumentException::class.java) { strided.byteOffset(listOf(2, 0)) }
        assertThrows(IllegalArgumentException::class.java) { TensorResourceDescriptor(listOf(2, 3)).byteOffset(listOf(0, 0)) }
        assertThrows(IllegalArgumentException::class.java) { TensorResourceDescriptor(listOf(2, 3), byteStrides = listOf(12, 4)) }
        assertThrows(IllegalArgumentException::class.java) { TensorResourceDescriptor(listOf(2, 3), layout = TensorLayout.LINEAR, byteStrides = listOf(8, 4)) }
        assertThrows(IllegalArgumentException::class.java) { TensorResourceDescriptor(listOf(2, 3), layout = TensorLayout.LINEAR, byteStrides = listOf(24, 8)) }
        assertThrows(IllegalArgumentException::class.java) { TensorResourceDescriptor(listOf(Long.MAX_VALUE, 2)) }
    }
    @Test fun memorylessAllowsOnlyAttachments() {
        assertThrows(IllegalArgumentException::class.java) {
            TextureDescriptor(64, 64, storageMode = StorageMode.MEMORYLESS)
        }
        TextureDescriptor(64, 64, PixelFormat.DEPTH32_FLOAT, setOf(TextureUsage.DEPTH_ATTACHMENT), StorageMode.MEMORYLESS)
        assertThrows(IllegalArgumentException::class.java) {
            TextureDescriptor(64, 64, PixelFormat.DEPTH32_FLOAT, setOf(TextureUsage.STORAGE))
        }
    }
    @Test fun descriptorsRejectEmptyAndNegativeSizes() {
        assertThrows(IllegalArgumentException::class.java) { Size(0) }
        assertThrows(IllegalArgumentException::class.java) { TextureDescriptor(-1, 8) }
        assertThrows(IllegalArgumentException::class.java) { BindingLayout(-1, BindingType.STORAGE_BUFFER) }
    }
}
