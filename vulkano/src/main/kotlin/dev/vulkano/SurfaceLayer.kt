package dev.vulkano

import android.view.Surface
import dev.vulkano.internal.Native

/** Create from SurfaceHolder callbacks; close before the Android surface is destroyed. */
fun Device.makeSurfaceLayer(surface: Surface, width: Int, height: Int): SurfaceLayer = access {
    require(surface.isValid && width > 0 && height > 0)
    SurfaceLayer(this, Native.createSurface(nativeHandle, surface, width, height))
}

class SurfaceLayer internal constructor(device: Device, id: Long) : Resource(device, id) {
    val pixelFormat: PixelFormat get() = access { id ->
        val format = Native.surfaceInfo(id)[2]
        PixelFormat.entries.first { it.vk == format }
    }
    val drawableSize: Size get() = access { Native.surfaceInfo(it).let { p -> Size(p[0], p[1]) } }
    /** Null means timeout or an out-of-date swapchain; retry on a later frame. */
    fun nextDrawable(timeoutNanos: Long = 1_000_000_000): Drawable? = access {
        require(timeoutNanos >= 0)
        val result = Native.acquireDrawable(it, timeoutNanos)
        if (result.isEmpty()) null else {
            val descriptor = TextureDescriptor(result[2].toInt(), result[3].toInt(),
                PixelFormat.entries.first { f -> f.vk == result[4].toInt() }, setOf(TextureUsage.COLOR_ATTACHMENT))
            Drawable(device, result[0], Texture(device, result[1], descriptor))
        }
    }
    /** No drawable may remain acquired. Waits for previous GPU/presentation work. */
    fun resize(width: Int, height: Int): Unit = access {
        require(width > 0 && height > 0); Native.resizeSurface(it, width, height)
    }
}

class Drawable internal constructor(device: Device, id: Long, val texture: Texture) : Resource(device, id) {
    override fun close() = synchronized(device) { texture.close(); super.close() }
}
