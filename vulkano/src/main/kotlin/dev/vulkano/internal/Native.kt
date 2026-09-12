package dev.vulkano.internal

import java.nio.ByteBuffer

internal object Native {
    init {
        System.loadLibrary("vulkano")
    }

    external fun createResourceBindings(device: Long, set: Int, data: LongArray): Long

    external fun updateResourceBindings(id: Long, data: LongArray, removed: LongArray, clear: Boolean)

    external fun graphCapabilities(device: Long): IntArray

    external fun graphOperations(device: Long, queue: Int): Array<String>

    external fun createGraphPipeline(
        device: Long,
        queue: Int,
        code: ByteArray,
        entry: String,
        specialization: IntArray,
        keys: IntArray,
        descriptions: Array<LongArray>,
        constantIds: IntArray,
        constantDescriptions: Array<LongArray>,
        constantData: Array<ByteArray>,
        compilerOptions: String,
        optimize: Boolean,
        identifier: ByteArray,
    ): Long

    external fun graphProperty(pipeline: Long, property: Int): ByteArray

    external fun graphAvailableProperties(pipeline: Long): IntArray

    external fun dispatchGraph(command: Long, pipeline: Long, tensors: LongArray)

    external fun createDevice(
        features: Long,
        validation: Boolean,
        allowSoftware: Boolean,
        extra: Long,
    ): Long

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

    external fun importHardwareBuffer(
        device: Long,
        buffer: Any,
        usage: Int,
        externalFormat: Boolean,
        linear: Boolean,
        model: Int,
        range: Int,
        externalFamily: Int,
    ): Long

    external fun hardwareBufferTextureInfo(texture: Long): IntArray

    external fun hardwareBufferSampler(texture: Long): Long

    external fun acquireExternalTexture(command: Long, texture: Long, preserve: Boolean)

    external fun releaseExternalTexture(command: Long, texture: Long)

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

    external fun vertexInputCapabilities(device: Long): LongArray

    external fun textureFormatCapabilities(
        device: Long,
        format: Int,
        type: Int,
        usage: Int,
    ): LongArray

    external fun createSampler(
        device: Long,
        linear: Boolean,
        repeat: Boolean,
        anisotropy: Float,
        options: IntArray,
        lod: FloatArray,
    ): Long

    external fun createGeneratedLayout(
        device: Long,
        pipelines: LongArray,
        tokens: IntArray,
        stride: Int,
        unordered: Boolean,
    ): Long

    external fun generatedLimits(device: Long): LongArray

    external fun executeGenerated(
        command: Long,
        generated: LongArray,
        bindings: LongArray,
        constants: ByteArray,
        kind: Int,
    )

    external fun rayTableRegions(pipeline: Long): LongArray

    external fun createComputePipeline(
        device: Long,
        code: ByteArray,
        entry: String,
        bindings: IntArray,
        pushBytes: Int,
        specialization: IntArray,
        indirect: Boolean,
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
        indirect: Boolean,
        tileOptions: IntArray,
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
        aspect: Int,
    ): Long

    external fun beginRenderAdvanced(
        command: Long,
        textures: LongArray,
        options: IntArray,
        clear: FloatArray,
        subpasses: IntArray,
        tileOptions: IntArray,
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
        generated: LongArray,
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

    external fun tileCapabilities(device: Long): IntArray

    external fun pipelineTileRate(pipeline: Long): IntArray

    external fun tileControl(encoder: Long, action: Int)

    external fun dispatchTile(
        encoder: Long,
        pipeline: Long,
        bindings: LongArray,
        constants: ByteArray,
        groups: IntArray,
        area: Boolean,
        indirect: Long,
        offset: Long,
    )

    external fun pipelineLocalSize(pipeline: Long): IntArray

    external fun createCommand(device: Long, queueIndex: Int): Long

    external fun queueInfo(device: Long): IntArray

    external fun tensorCapabilities(device: Long): LongArray

    external fun createTensor(
        device: Long,
        format: Int,
        tiling: Int,
        usage: Long,
        dimensions: LongArray,
        strides: LongArray,
        storage: Int,
    ): Long

    external fun supportsTensor(
        device: Long,
        format: Int,
        tiling: Int,
        usage: Long,
        dimensions: LongArray,
        strides: LongArray,
        storage: Int,
    ): Boolean

    external fun tensorInfo(tensor: Long): LongArray

    external fun createTensorView(tensor: Long, format: Int): Long

    external fun accessTensorBytes(
        tensor: Long,
        offset: Long,
        bytes: java.nio.ByteBuffer,
        write: Boolean,
    )

    external fun copyTensor(command: Long, source: Long, destination: Long)

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

    external fun createExternalSemaphore(device: Long, fd: Int): Long

    external fun exportSyncFd(semaphore: Long): Int

    external fun duplicateSyncFd(fd: Int): Int

    external fun closeSyncFd(fd: Int)

    external fun waitExternalSemaphore(command: Long, semaphore: Long)

    external fun signalExternalSemaphore(command: Long, semaphore: Long)

    external fun sparseCapabilities(device: Long): LongArray

    external fun sparseInfo(resource: Long): LongArray

    external fun createSparseBuffer(device: Long, size: Long, usage: Int): Long

    external fun createSparseTexture(
        device: Long,
        width: Int,
        height: Int,
        format: Int,
        usage: Int,
        options: IntArray,
    ): Long

    external fun mapSparseBuffer(
        resource: Long,
        page: Long,
        count: Int,
        resident: Boolean,
        source: Long,
        sourcePage: Long,
    )

    external fun mapSparseTexture(
        resource: Long,
        region: IntArray,
        resident: Boolean,
        source: Long,
        sourceRegion: IntArray,
    )

    external fun mapSparseTail(texture: Long, layer: Int, resident: Boolean)

    external fun sparseTextureResident(texture: Long, region: IntArray): Boolean

    external fun sparseBufferResident(buffer: Long, page: Long, count: Int): Boolean

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

    external fun createCounters(device: Long, count: Int, timestamp: Boolean, queueIndex: Int): Long

    external fun counterCapabilities(device: Long, queueIndex: Int): LongArray

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

    external fun refitPrimitiveAcceleration(command: Long, structure: Long, values: LongArray)

    external fun refitInstanceAcceleration(
        command: Long,
        structure: Long,
        values: LongArray,
        transforms: FloatArray,
    )

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
        indirect: Boolean,
        motion: Boolean,
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
