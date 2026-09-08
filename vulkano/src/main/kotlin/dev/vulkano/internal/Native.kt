package dev.vulkano.internal

import java.nio.ByteBuffer

internal object Native {
    init {
        System.loadLibrary("vulkano")
    }

    external fun createDevice(features: Long, validation: Boolean, allowSoftware: Boolean): Long

    external fun deviceName(device: Long): String

    external fun deviceInfo(device: Long): LongArray

    external fun memoryHeaps(device: Long): LongArray

    external fun closeDevice(device: Long)

    external fun close(handle: Long)

    external fun waitIdle(device: Long)

    external fun pollIdle(device: Long): Boolean

    external fun createUploadBuffer(device: Long, length: Long, usage: Int): Long

    external fun createBuffer(device: Long, length: Long, usage: Int, storage: Int): Long

    external fun writeBuffer(buffer: Long, offset: Long, bytes: ByteBuffer)

    external fun readBuffer(buffer: Long, offset: Long, bytes: ByteBuffer)

    external fun createTexture(
        device: Long,
        width: Int,
        height: Int,
        format: Int,
        usage: Int,
        storage: Int,
        options: IntArray,
    ): Long

    external fun textureIsLazy(texture: Long): Boolean

    external fun supportsTexture(
        device: Long,
        width: Int,
        height: Int,
        format: Int,
        usage: Int,
        options: IntArray,
    ): Boolean

    external fun createSampler(
        device: Long,
        linear: Boolean,
        repeat: Boolean,
        anisotropy: Float,
        options: IntArray,
        lod: FloatArray,
    ): Long

    external fun createComputePipeline(
        device: Long,
        code: ByteArray,
        entry: String,
        bindings: IntArray,
        pushBytes: Int,
        specialization: IntArray,
    ): Long

    external fun createRenderPipeline(
        device: Long,
        vertex: ByteArray,
        vertexEntry: String,
        fragment: ByteArray,
        fragmentEntry: String,
        bindings: IntArray,
        pushBytes: Int,
        color: Int,
        depth: Int,
        blend: Boolean,
    ): Long

    external fun createGraphics(
        device: Long,
        code: Array<ByteArray>,
        entries: Array<String>,
        specialization: Array<IntArray>,
        options: IntArray,
        bindings: IntArray,
        pushBytes: Int,
        mesh: Boolean,
        subpasses: IntArray,
        subpass: Int,
    ): Long

    external fun createTextureView(
        texture: Long,
        format: Int,
        type: Int,
        level: Int,
        levels: Int,
        slice: Int,
        slices: Int,
        usage: Int,
        swizzle: IntArray,
    ): Long

    external fun beginRenderAdvanced(
        command: Long,
        textures: LongArray,
        options: IntArray,
        clear: FloatArray,
        subpasses: IntArray,
    ): Long

    external fun nextSubpass(encoder: Long)

    external fun drawAdvanced(
        encoder: Long,
        pipeline: Long,
        bindings: LongArray,
        constants: ByteArray,
        counts: IntArray,
        resources: LongArray,
        state: FloatArray,
    )

    external fun dispatchIndirect(
        command: Long,
        pipeline: Long,
        bindings: LongArray,
        constants: ByteArray,
        buffer: Long,
        offset: Long,
    )

    external fun copyTextureRegion(
        command: Long,
        buffer: Long,
        texture: Long,
        offset: Long,
        toTexture: Boolean,
        region: IntArray,
        rowLength: Int,
        imageHeight: Int,
    )

    external fun copyImages(
        command: Long,
        source: Long,
        destination: Long,
        sourceRegion: IntArray,
        destinationRegion: IntArray,
    )

    external fun generateMipmaps(command: Long, texture: Long, filter: Int)

    external fun fillBuffer(command: Long, buffer: Long, offset: Long, length: Long, value: Int)

    external fun pipelineLocalSize(pipeline: Long): IntArray

    external fun createCommand(device: Long): Long

    external fun dispatch(
        command: Long,
        pipeline: Long,
        bindings: LongArray,
        constants: ByteArray,
        groups: IntArray,
    )

    external fun beginRender(
        command: Long,
        color: Long,
        depth: Long,
        actions: IntArray,
        clear: FloatArray,
    ): Long

    external fun draw(
        encoder: Long,
        pipeline: Long,
        bindings: LongArray,
        constants: ByteArray,
        counts: IntArray,
    )

    external fun endRender(encoder: Long)

    external fun copyBuffers(
        command: Long,
        source: Long,
        destination: Long,
        sourceOffset: Long,
        destinationOffset: Long,
        length: Long,
    )

    external fun copyTexture(
        command: Long,
        buffer: Long,
        texture: Long,
        offset: Long,
        toTexture: Boolean,
    )

    external fun commit(command: Long)

    external fun waitCommand(command: Long, timeoutNanos: Long): Boolean

    external fun commandState(command: Long): Int

    external fun heapBufferRequirements(device: Long, size: Long, usage: Int): LongArray

    external fun heapTextureRequirements(
        device: Long,
        width: Int,
        height: Int,
        format: Int,
        usage: Int,
        options: IntArray,
    ): LongArray

    external fun createPlacementHeap(
        device: Long,
        size: Long,
        storage: Int,
        types: Int,
        alignment: Long,
    ): Long

    external fun aliasResources(command: Long, before: Long, after: Long)

    external fun createHeap(device: Long, size: Long, storage: Int): Long

    external fun createHeapBuffer(heap: Long, size: Long, usage: Int, offset: Long): Long

    external fun createHeapTexture(
        heap: Long,
        width: Int,
        height: Int,
        format: Int,
        usage: Int,
        options: IntArray,
        offset: Long,
    ): Long

    external fun createTextureBuffer(
        buffer: Long,
        format: Int,
        offset: Long,
        length: Long,
        writable: Boolean,
    ): Long

    external fun referenceBuffer(buffer: Long): Long

    external fun cooperativeMatrixConfigurations(device: Long): IntArray

    external fun rateMapLimits(device: Long): IntArray

    external fun fragmentShadingRates(device: Long): IntArray

    external fun depthResolveSupport(device: Long): IntArray

    external fun createEvent(device: Long, initial: Long): Long

    external fun eventValue(event: Long): Long

    external fun signalEvent(event: Long, value: Long)

    external fun waitEvent(command: Long, event: Long, value: Long)

    external fun signalCommandEvent(command: Long, event: Long, value: Long)

    external fun createCounters(device: Long, count: Int, timestamp: Boolean): Long

    external fun counterCapabilities(device: Long): LongArray

    external fun counterInfo(id: Long): LongArray

    external fun readCounters(pool: Long): LongArray

    external fun sampleCounter(command: Long, pool: Long, index: Int)

    external fun retainBuffer(command: Long, buffer: Long)

    external fun pipelineCacheData(device: Long): ByteArray

    external fun loadPipelineCache(device: Long, data: ByteArray)

    external fun bufferAddress(buffer: Long): Long

    external fun createPrimitiveAcceleration(
        device: Long,
        geometries: LongArray,
        refit: Boolean,
        compact: Boolean,
    ): Long

    external fun createInstanceAcceleration(
        device: Long,
        instances: LongArray,
        transforms: FloatArray,
        refit: Boolean,
        compact: Boolean,
    ): Long

    external fun createAccelerationCopy(source: Long, compact: Boolean): Long

    external fun copyAcceleration(command: Long, source: Long, destination: Long)

    external fun serializeAcceleration(structure: Long): ByteArray

    external fun accelerationArchiveAddresses(bytes: ByteArray): LongArray

    external fun restoreAcceleration(device: Long, bytes: ByteArray, replacements: LongArray): Long

    external fun accelerationStorageSize(id: Long): Long

    external fun buildAcceleration(command: Long, structure: Long, update: Boolean)

    external fun createRayPipeline(
        device: Long,
        code: Array<ByteArray>,
        entries: Array<String>,
        specialization: Array<IntArray>,
        stages: IntArray,
        groups: IntArray,
        recursion: Int,
        bindings: IntArray,
        pushBytes: Int,
    ): Long

    external fun traceRays(
        command: Long,
        pipeline: Long,
        bindings: LongArray,
        constants: ByteArray,
        size: IntArray,
    )

    external fun createSurface(device: Long, surface: Any, width: Int, height: Int): Long

    external fun resizeSurface(surface: Long, width: Int, height: Int)

    external fun surfaceInfo(surface: Long): IntArray

    external fun acquireDrawable(surface: Long, timeoutNanos: Long): LongArray

    external fun present(command: Long, drawable: Long)
}
