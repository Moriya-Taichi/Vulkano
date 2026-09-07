package dev.vulkano.internal

import java.nio.ByteBuffer

internal object Native {
    init { System.loadLibrary("vulkano") }
    external fun createDevice(features: Int, validation: Boolean, allowSoftware: Boolean): Long
    external fun deviceName(device: Long): String
    external fun deviceInfo(device: Long): LongArray
    external fun memoryHeaps(device: Long): LongArray
    external fun closeDevice(device: Long)
    external fun close(handle: Long)
    external fun waitIdle(device: Long)
    external fun createUploadBuffer(device: Long, length: Long, usage: Int): Long
    external fun createBuffer(device: Long, length: Long, usage: Int, storage: Int): Long
    external fun writeBuffer(buffer: Long, offset: Long, bytes: ByteBuffer)
    external fun readBuffer(buffer: Long, offset: Long, bytes: ByteBuffer)
    external fun createTexture(device: Long, width: Int, height: Int, format: Int, usage: Int, storage: Int): Long
    external fun textureIsLazy(texture: Long): Boolean
    external fun supportsTexture(device: Long, width: Int, height: Int, format: Int, usage: Int): Boolean
    external fun createSampler(device: Long, linear: Boolean, repeat: Boolean, anisotropy: Float): Long
    external fun createComputePipeline(device: Long, code: ByteArray, entry: String, bindings: IntArray, pushBytes: Int): Long
    external fun createRenderPipeline(device: Long, vertex: ByteArray, vertexEntry: String, fragment: ByteArray, fragmentEntry: String, bindings: IntArray, pushBytes: Int, color: Int, depth: Int, blend: Boolean): Long
    external fun pipelineLocalSize(pipeline: Long): IntArray
    external fun createCommand(device: Long): Long
    external fun dispatch(command: Long, pipeline: Long, bindings: LongArray, constants: ByteArray, groups: IntArray)
    external fun beginRender(command: Long, color: Long, depth: Long, actions: IntArray, clear: FloatArray): Long
    external fun draw(encoder: Long, pipeline: Long, bindings: LongArray, constants: ByteArray, counts: IntArray)
    external fun endRender(encoder: Long)
    external fun copyBuffers(command: Long, source: Long, destination: Long, sourceOffset: Long, destinationOffset: Long, length: Long)
    external fun copyTexture(command: Long, buffer: Long, texture: Long, offset: Long, toTexture: Boolean)
    external fun commit(command: Long)
    external fun waitCommand(command: Long, timeoutNanos: Long): Boolean
    external fun commandState(command: Long): Int
    external fun createSurface(device: Long, surface: Any, width: Int, height: Int): Long
    external fun resizeSurface(surface: Long, width: Int, height: Int)
    external fun surfaceInfo(surface: Long): IntArray
    external fun acquireDrawable(surface: Long, timeoutNanos: Long): LongArray
    external fun present(command: Long, drawable: Long)
}
