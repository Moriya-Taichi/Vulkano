# 拡張APIの使用例

以下は、作成済みのDeviceとShader Functionを使う例です。
完全なシェーダーとGPU出力の確認は[GraphicsIntegrationTest](../vulkano/src/test/kotlin/dev/vulkano/GraphicsIntegrationTest.kt)を参照してください。

## MSAAとIndexed Draw

```kotlin
val msaa = device.makeTexture(TextureDescriptor(
    width, height,
    usage = setOf(TextureUsage.COLOR_ATTACHMENT),
    storageMode = StorageMode.MEMORYLESS,
    sampleCount = 4,
))
val resolved = device.makeTexture(TextureDescriptor(
    width, height,
    usage = setOf(TextureUsage.COLOR_ATTACHMENT, TextureUsage.SAMPLED),
))
val pipeline = device.makeRenderPipelineState(RenderPipelineDescriptor(
    vertexFunction, fragmentFunction,
    sampleCount = 4,
    vertexBuffers = listOf(VertexBufferLayout(index = 0, stride = 8)),
    vertexAttributes = listOf(VertexAttribute(0, 0, PixelFormat.RG32_FLOAT)),
))
command.render(RenderPassDescriptor(ColorAttachment(
    msaa, storeAction = StoreAction.DONT_CARE, resolveTexture = resolved,
))) {
    setRenderPipelineState(pipeline)
    setVertexBuffer(vertices, index = 0)
    drawIndexedPrimitives(indices, indexCount = 6, indexType = IndexType.UINT16)
}
```

`vertices`は`BufferUsage.VERTEX`、`indices`は`BufferUsage.INDEX`で作成します。
PipelineとAttachmentのSample Countを揃えます。
端末の対応は`supportsTexture(msaaDescriptor)`で問い合わせられます。

## MipとTexture View

```kotlin
val texture = device.makeTexture(TextureDescriptor(
    256, 256, mipLevels = 9,
    usage = setOf(TextureUsage.SAMPLED, TextureUsage.TRANSFER_SOURCE,
        TextureUsage.TRANSFER_DESTINATION),
))
command.blit {
    copy(upload, texture)
    generateMipmaps(texture)
}
val mip = texture.makeTextureView(level = 2, levelCount = 1)
val sampler = device.makeSampler(SamplerDescriptor(mipFilter = MipFilter.LINEAR))
```

Mip生成はLevel 0を初期化した後に記録します。
Texture Arrayの場合は全SliceのLevel 0を初期化します。
圧縮Textureは、用意した圧縮Mipを領域コピーで転送できます。

## FeatureとRay Query

```kotlin
val device = Device.create(requiredFeatures = setOf(Feature.RAY_QUERY))
val vertices = device.makeBuffer(
    length = 36,
    usage = setOf(BufferUsage.ACCELERATION_STRUCTURE_INPUT,
        BufferUsage.SHADER_DEVICE_ADDRESS),
)
// float3の頂点3個をverticesに書き込む
val primitive = device.makePrimitiveAccelerationStructure(
    listOf(TriangleGeometry(vertices, vertexCount = 3)), allowRefit = true,
)
val scene = device.makeInstanceAccelerationStructure(
    listOf(AccelerationStructureInstance(primitive)),
)
command.accelerationStructure {
    build(primitive)
    build(scene)
}
command.compute {
    setComputePipelineState(rayQueryPipeline)
    setAccelerationStructure(scene, index = 0)
    setBuffer(result, index = 1)
    dispatchThreads(Size(1))
}
```

この例の`command`、Pipeline、Bufferは同じDeviceから作成します。
Ray Query用GLSLの例は[query.comp](../tests/shaders/query.comp)です。
`RAY_TRACING_PIPELINE`を要求すると専用Ray Encoderも使用できます。
Ray Queryのみ対応する端末にRay Tracing Pipelineの対応を要求しません。

## Runtime Resource配列

```kotlin
val pipeline = device.makeComputePipelineState(
    function,
    bindings = listOf(BindingLayout(0, BindingType.STORAGE_BUFFER, count = 128)),
)
command.compute {
    setComputePipelineState(pipeline)
    setBuffer(buffer, index = 0, arrayElement = 17)
    // ShaderはBinding済みの要素17だけを参照する
    dispatchThreads(Size(1))
}
```

Runtime Arrayには`Feature.DESCRIPTOR_INDEXING`を要求します。
`count`は配列の容量です。
DeviceのDescriptor上限を超える容量は拒否します。
固定配列はFeatureを追加せずに使えますが、全要素のBindingが必要です。

## EventとCounter

```kotlin
val event = device.makeSharedEvent()
val counters = device.makeCounterSampleBuffer(count = 2)
command.waitForEvent(event, value = 1)
command.sampleCounters(counters, index = 0)
// ComputeやBlitを記録する
command.sampleCounters(counters, index = 1)
command.signalEventOnCompletion(event, value = 2)
command.commit()
event.signal(1)
command.waitUntilCompleted()
val values = counters.read()
```

Shared Eventには`Feature.TIMELINE_SEMAPHORE`が必要です。
TimestampはGPUのTick単位です。
Counter値は全Sampleが記録され、GPU処理が完了してから読み出します。
Pipeline Cacheは`device.serializePipelineCache()`で保存し、同じDevice/Driverで`loadPipelineCache(bytes)`へ渡せます。
