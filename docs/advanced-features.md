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
作成前に`device.counterCapabilities().timestampValidBits > 0`で対応を確認できます。
`elapsedNanos(startIndex, endIndex)`は有効ビットの折り返しを処理します。
Counter値は全Sampleが記録され、GPU処理が完了してから読み出します。
Pipeline Cacheは`device.serializePipelineCache()`で保存し、同じDevice/Driverで`loadPipelineCache(bytes)`へ渡せます。

## Depth ResolveとShading Rate

`Feature.DEPTH_STENCIL_RESOLVE`を要求すると、`DepthAttachment.resolveTexture`を指定できます。
`depthStencilResolveSupport()`でDepthとStencilそれぞれのModeを確認し、`depthResolveMode`と`stencilResolveMode`を設定します。
Sample Zero、Average、Min、Maxから、端末が提供するModeを選びます。
Depth/Stencil複合Textureでは、独立したModeをサポートしない端末に異なるModeを指定できません。

`FRAGMENT_SHADING_RATE`では`fragmentShadingRates()`からサイズとSample Countの組み合わせを取得し、`RenderPipelineDescriptor.fragmentSize`へ指定します。
Primitive ShaderからRateを出力する場合は`PRIMITIVE_SHADING_RATE`を追加し、`primitiveShadingRateCombiner`を指定します。
画面内のRate Mapは`ATTACHMENT_SHADING_RATE`で有効にします。

## Acceleration StructureのCompaction

```kotlin
val source = device.makePrimitiveAccelerationStructure(
    listOf(TriangleGeometry(vertices, vertexCount = 3)),
    allowCompaction = true,
)
// sourceをBuildして、Commandの完了を待つ
val destination = source.makeCopyDestination(compact = true)
command.accelerationStructure { copy(source, destination) }
// このCommandの完了後はdestinationをTLASのInstanceに使える
```

`makeCopyDestination(compact = true)`はGPUへサイズQueryを送信して結果を待ちます。
呼び出し前にDeviceの送信済みCommandを完了させ、ワーカースレッドで実行してください。
サイズを取得した後にSourceをBuild/Refitすると、そのDestinationへのCopyは拒否します。
Copy Destinationは読み取り用で、直接Build/Refitできません。
元のGeometry Descriptorから作成したStructureは、従来どおりRefitできます。
`allocatedSize`で確保サイズを確認できます。


## TextureのFormatと成分を変更する

```kotlin
val view = texture.makeTextureView(
    usage = setOf(TextureUsage.SAMPLED),
    swizzle = TextureSwizzle(
        red = TextureComponent.BLUE,
        green = TextureComponent.GREEN,
        blue = TextureComponent.RED,
        alpha = TextureComponent.ONE,
    ),
)
```

`pixelFormat`にはVulkanのFormat互換クラスが一致する形式を指定できます。
Usageは元のTextureの部分集合です。
Swizzleを使うViewはSampling用に限定します。
Sampling、Storage、Input Attachmentの整数型と浮動小数点型はShader宣言と照合します。
Textureの親を閉じても、ViewがNativeのImageを保持します。

## Subpassで描画結果を読む

```kotlin
val layout = RenderPassLayout(
    colorFormats = listOf(PixelFormat.RGBA8_UNORM, PixelFormat.RGBA8_UNORM),
    subpasses = listOf(
        RenderSubpass(colorAttachments = listOf(0)),
        RenderSubpass(colorAttachments = listOf(1), inputAttachments = listOf(0)),
    ),
)
val first = device.makeRenderPipelineState(RenderPipelineDescriptor(
    vertexFunction, fragmentFunction, subpassLayout = layout,
))
val second = device.makeRenderPipelineState(RenderPipelineDescriptor(
    vertexFunction, inputFragmentFunction, subpassLayout = layout, subpassIndex = 1,
))
command.render(RenderPassDescriptor(
    colorAttachments = listOf(
        ColorAttachment(intermediate, storeAction = StoreAction.DONT_CARE),
        ColorAttachment(output),
    ),
    subpassLayout = layout,
)) {
    setRenderPipelineState(first)
    drawPrimitives(3)
    nextSubpass()
    setRenderPipelineState(second)
    setTexture(intermediate, index = 0)
    drawPrimitives(3)
}
```

`intermediate`には`COLOR_ATTACHMENT`と`INPUT_ATTACHMENT`のUsageを指定します。
Memorylessでも利用でき、Subpass間の読み取りに必要な内容をRender Pass内で保持します。
入力Shaderは[input.frag](../tests/shaders/input.frag)のように`subpassInput`を宣言します。
`nextSubpass()`後はPipelineとBindingを設定し直します。

MSAA入力には`subpassInputMS`を使います。
Color Resolveを行う場合はLayoutの`resolveColorAttachments`と、Render Passの`ColorAttachment.resolveTexture`を揃えます。
Resolve結果を入力にする際のIndexは`layout.resolveAttachmentIndex(colorIndex)`で取得できます。
DepthはColor Attachmentの後、ResolveはDepthの後に並びます。
同じSubpassで読み書きするAttachmentは分離します。

## 位置ごとのShading Rate

`device.rasterizationRateMapLimits()`で、Rate Mapの1 Texelが覆う描画領域のサイズを確認します。
`R8_UINT`のTextureを`SHADING_RATE_ATTACHMENT`と`TRANSFER_DESTINATION`のUsageで作成します。
各Texelの値は`RasterizationRateMap.encode(fragmentSize)`で生成し、`fragmentShadingRates()`で対応するSample Countを確認します。

Pipelineの`rateMapTexelSize`とRender Passの`RasterizationRateMap.texelSize`を揃えます。

```kotlin
val pipeline = device.makeRenderPipelineState(RenderPipelineDescriptor(
    vertexFunction, fragmentFunction, rateMapTexelSize = texelSize,
))
command.render(RenderPassDescriptor(
    colorAttachments = listOf(ColorAttachment(output)),
    rasterizationRateMap = RasterizationRateMap(rateTexture, texelSize),
)) {
    setRenderPipelineState(pipeline)
    drawPrimitives(3)
}
```

Rate Textureは描画前に初期化します。
Render Areaを覆う大きさが必要で、Layerを持つMapは端末の`layered`対応も確認します。

## Acceleration Structureの保存と復元

次はBLASが1個のTLASを復元する例です。
保存と復元は先行Commandの完了後に、ワーカースレッドで実行します。

```kotlin
val primitiveArchive = primitive.serialize()
val sceneArchive = scene.serialize()
// toByteArray()で保存し、fromByteArray()で読み込める
val restoredPrimitive = device.restoreAccelerationStructure(primitiveArchive)
val restoredScene = device.restoreAccelerationStructure(
    sceneArchive,
    bottomLevelStructures = sceneArchive.bottomLevelAddresses.associateWith { restoredPrimitive },
)
```

BLASが複数ある場合は、保存時の各アドレスとアプリ側のGeometry識別子の対応も保持します。
復元には同等のGeometryを持つBLASを割り当てます。
Driver互換性とデータ破損を検査し、必要なBLAS参照が足りない場合は拒否します。
ArchiveはVulkanoが生成したデータの保存用です。
任意の外部データを安全に実行するための形式ではありません。
復元したStructureは読み取り用で、Build/Refitできません。

## TensorとCooperative Matrix

```kotlin
val tensor = device.makeTensor(TensorDescriptor(
    shape = listOf(64L, 128L), dataType = TensorDataType.FLOAT32,
))
val transpose = tensor.makeView(TensorDescriptor(
    shape = listOf(128L, 64L), dataType = TensorDataType.FLOAT32,
    strides = listOf(1L, 128L),
))
command.compute {
    setComputePipelineState(tensorPipeline)
    setTensor(transpose, index = 0)
    // ShapeとStrideは、このPipelineのShaderが定める引数として渡す
    dispatchThreadgroups(groups)
}
```

Strideの単位は要素数です。
Tensor ViewはBufferを保持し、親Tensorの解放後も利用できます。
Bufferの初期化や、Private Storageとの転送には既存のBuffer APIを使います。
ViewのBinding Offsetには、通常のStorage Bufferと同じAlignment制約があります。

`COOPERATIVE_MATRIX`を要求すると、対応端末で`VK_KHR_cooperative_matrix`の演算を使えます。
`cooperativeMatrixConfigurations()`が返すM、N、K、数値型、Saturationの組み合わせからShaderを作成します。
16ビットの演算を含むShaderでは`SHADER_FLOAT16`などのFeatureも要求します。
[cooperative.comp](../tests/shaders/cooperative.comp)と対応する統合テストは、Function Constantsで行列サイズとSubgroup幅を指定する例です。
