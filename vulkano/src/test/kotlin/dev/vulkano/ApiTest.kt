package dev.vulkano

import org.junit.Assert.*
import org.junit.Test

class ApiTest {
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
