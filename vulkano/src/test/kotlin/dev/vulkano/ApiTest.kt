package dev.vulkano

import org.junit.Assert.*
import org.junit.Test

class ApiTest {
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
