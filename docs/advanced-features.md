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

## Placement Heap

`heapBufferRequirements`と`heapTextureRequirements`は、物理メモリを確保せずに端末の配置条件を問い合わせます。
Heapに置く各Resourceの条件を渡し、互換性のあるMemory Typeを選びます。

```kotlin
val desc = TextureDescriptor(256, 256, usage = setOf(TextureUsage.COLOR_ATTACHMENT))
val req = device.heapTextureRequirements(desc)
val heap = device.makePlacementHeap(req.size, listOf(req))
val first = heap.makeTexture(desc, offset = 0)
val second = heap.makeTexture(desc, offset = 0)
command.render(RenderPassDescriptor(listOf(ColorAttachment(first))))
command.blit { aliasResources(first, second) }
command.render(RenderPassDescriptor(listOf(ColorAttachment(second))))
```

同じ領域のResourceを同時には使いません。
各切り替えでAlias Barrierを発行し、TextureはClearや全面転送から再開します。
異なる画像間で内容を引き継ぐことはできません。
Shared Placement BufferへのCPUアクセスは、DeviceのGPU Commandが完了した状態で行います。
Heapを閉じた後も、作成済みResourceが割り当てを保持します。

## Sparse Resource

`SPARSE_RESOURCES`を有効化し、`sparseCapabilities()`でBuffer、2D/3D Texture、Shader Residency、Mapping共有の対応を確認します。
各Residency機能は端末がサポートするものだけが有効になります。

```kotlin
val device = Device.create(setOf(Feature.SPARSE_RESOURCES))
val sparse = device.makeSparseBuffer(16L * 1024 * 1024)
sparse.setResident(firstPage = 0, pageCount = 2)
command.blit { fill(sparse.buffer, 0, length = sparse.pageSize * 2) }
// Submit and wait before consuming initialized values.
command.commit()
command.waitUntilCompleted()
sparse.setResident(firstPage = 0, pageCount = 2, resident = false)
```

Textureでは`tileSize`に沿ったTexel領域を`setResident`に渡します。
端のTileは画像サイズまでの部分領域を指定できます。
`mipTailFirstLevel`以降は`setMipTailResident`で管理します。
`singleMipTail`の場合はSlice 0が全Layerを表し、それ以外は各Sliceを指定します。
必要なMetadataは自動的に確保されます。
`allocatedBytes`はMetadataを含む現在の割り当て量です。

`isResident`はCPU側のMappingを問い合わせます。
Shader側は対応端末でSPIR-V Sparse Residency命令を使えます。
`copyMappings`は同じ物理Page/Tileを共有するため、元のResourceを閉じてもコピー先が割り当てを保持します。
同じ共有Mappingへの読み書きにはCommand間の同期が必要です。
新規Pageの内容や、Strict Residencyのない端末の未割り当て領域の値には依存しないでください。

Mapping操作は先行Commandの完了を待つ同期APIです。
Frameの描画中に頻繁に呼ぶことを避け、Region単位でまとめて更新します。

## 外部Fenceとの同期

`EXTERNAL_SYNC_FD`が利用可能な端末では、AndroidなどのSYNC_FDをVulkanのBinary Semaphoreに取り込めます。
ImportはFDを複製し、渡した`SyncFd`の所有権を変更しません。

```kotlin
val signal = device.makeExternalSemaphore()
command.signalExternalSemaphore(signal)
command.commit()
val fence = signal.exportSyncFd() // GPU完了前でも取得可能
// 他のAPIに渡す場合はfence.detach()でFDの所有権を移す。
fence.close()
```

`device.importSyncFd(fd)`で取り込んだSemaphoreは、Commandの`waitForExternalSemaphore`で待機します。
Semaphoreは1回だけ使えます。
GPUでSignalするSemaphoreもExportは1回で、同じSignalをWaitとExportの両方に消費することはできません。
`SyncFd.adopt(-1)`は、すでに完了したFenceを表します。

## Immutable Sampler

`BindingLayout.immutableSampler`でSamplerをPipelineに固定できます。
固定したSamplerはPipelineが保持するため、作成元のSamplerを閉じても利用できます。
描画時は`setTexture(texture, index)`にSamplerを渡す必要がありません。
独立したSampler Bindingを固定した場合は、そのBindingへの`setSampler`も不要です。


## HardwareBufferとCamera画像

`ANDROID_HARDWARE_BUFFER`を有効化すると、Androidの`HardwareBuffer`を直接Textureとして取り込めます。
YUVなどの外部Formatには`SAMPLER_YCBCR_CONVERSION`も必要です。
外部Fenceを使う場合は`EXTERNAL_SYNC_FD`を有効化します。
以下の`hardwareBuffer`と`producerFence`は、画像の生成元から受け取ったものです。

```kotlin
val imported = device.importHardwareBuffer(hardwareBuffer)
val wait = device.importSyncFd(producerFence)
val signal = device.makeExternalSemaphore()
val binding = BindingLayout(
    0, BindingType.SAMPLED_TEXTURE,
    immutableSampler = imported.conversionSampler ?: device.makeSampler(),
)
// bindingをRender Pipelineのbindingsへ指定する。
command.waitForExternalSemaphore(wait)
command.acquireExternalTexture(imported.texture)
// Render EncoderでsetTexture(imported.texture, 0)を指定して描画する。
command.releaseExternalTexture(imported.texture)
command.signalExternalSemaphore(signal)
command.commit()
val consumerFence = signal.exportSyncFd()
// 生成元へconsumerFenceを返し、画像を再利用できる時点を伝える。
```

RGBとDepthは対応するVulkan Formatを使い、HardwareBufferのUsageの範囲で描画・転送にも利用できます。
YUVなどの外部Formatは`PixelFormat.EXTERNAL`になり、Combined Image Samplerで読み取ります。
`HardwareBufferConversion`を指定すると、RGB画像も外部Formatとして取り込めます。
色のModelとRangeはDriverの提案値を初期値とし、CameraやCodecのColor Metadataに従ってBT.601/709/2020、Full/Narrowを指定できます。
この変換はY′CbCrからRGBへの変換で、HDRのTone Mappingや色域変換までは行いません。
Linear Filterは端末のLuma/Chroma両方の対応を検査します。
同じ変換設定はNative Samplerを共有するため、同じFormatのフレームには既存Pipelineを再利用できます。

ImportはJavaのHardwareBufferとは独立した参照を保持します。
ただしCameraのImageを返却すると生成元が同じメモリを書き換える可能性があるため、GPU処理の完了前に再利用させないでください。
CPUで同期済みの場合は外部Semaphoreを省けます。
新規に確保した画像の内容を破棄して描画する場合は`acquireExternalTexture(texture, preserveContents = false)`を使います。
`releaseExternalTexture`は画像をGENERAL Layoutで返します。
通常は`ExternalTextureOwner.FOREIGN`を使い、同じGPU/Driver UUIDを持つ別のVulkan DeviceやOpenGL ESから渡す場合は`EXTERNAL`を指定します。
同じBufferを同じDeviceに重複してImportせず、必要に応じてTexture Viewを作成します。


## GPUで決める描画数

`DRAW_INDIRECT_COUNT`を有効化すると、Draw Commandの個数もGPU上のBufferから読み取れます。
引数Bufferと個数Bufferには`BufferUsage.INDIRECT`を指定し、Computeで書く場合は`STORAGE`も付けます。

```kotlin
encoder.drawPrimitives(
    indirectBuffer = drawArguments,
    countBuffer = drawCount,
    maxDrawCount = 256,
    indirectOffset = 0,
    countOffset = 0,
)
```

実行する個数はBuffer内の32ビット符号なし整数と`maxDrawCount`の小さい方です。
Bufferの個数が0なら描画を実行しません。
`drawIndexedPrimitives`と`drawMeshThreadgroups`にも同じCount Bufferを受けるオーバーロードがあります。
Indexed DrawのStrideは20バイト、通常Drawは16バイト、Meshは12バイト以上の4バイト単位です。
個数のOffsetも4バイト単位にします。
個数Bufferに書く値は、端末の`maxDrawIndirectCount`以内にしてください。
これらの呼び出しは`MULTI_DRAW_INDIRECT`とは別のFeatureで、同じPipelineとBindingを使います。
Pipeline、Push Constants、Vertex/Index BufferもGPUで選択する場合は、次のDevice Generated Commandsを使用します。


## ASTC HDRとPVRTC

`TEXTURE_COMPRESSION_ASTC_HDR`を有効化すると、`ASTC_4x4_FLOAT`から`ASTC_12x12_FLOAT`までの14種類を使えます。
HDRの16バイトBlockをBlitで転送し、ShaderからFloatの色として読み取ります。
圧縮画像のMip生成はBlit対応を問い合わせ、非対応の場合は事前に圧縮したMipを転送します。

`TEXTURE_COMPRESSION_PVRTC`は`VK_IMG_format_pvrtc`がある端末で利用できます。
PVRTC1/2、2/4bpp、UNORM/SRGBの8種類に対応し、PVRTC1の幅と高さは2の累乗に制限します。
Buffer転送ではVulkanの8バイトBlock配置を使い、PVRなどのファイルHeaderは含めません。
各FormatとUsageの組み合わせは`supportsTexture`で確認します。
PVRTC拡張は非推奨で、既存Assetとの互換用途に使えます。
新規のPowerVR向けTextureには、対応状況に応じてASTCやETC2を選択してください。
この扱いは[KhronosのPVRTC拡張説明](https://docs.vulkan.org/refpages/latest/refpages/source/VK_IMG_format_pvrtc.html)に基づきます。


## GPUによるPipelineとCommandの選択

`DEVICE_GENERATED_COMMANDS`を要求すると、対応端末で`VK_EXT_device_generated_commands`を使えます。
依存するBuffer Device Addressも有効になります。
`indirectCommandLimits()`で対応Stage、Pipeline選択Stage、Sequence数、Stride、Token数などを確認します。
Pipelineを選択するTokenを使う場合は、作成時に`supportsIndirectCommands = true`を指定します。

```kotlin
val first = device.makeComputePipelineState(firstFunction, supportsIndirectCommands = true)
val second = device.makeComputePipelineState(secondFunction, supportsIndirectCommands = true)
val layout = device.makeIndirectCommandLayout(
    pipelines = listOf(first, second),
    tokens = listOf(
        IndirectCommandToken.pipeline(0),
        IndirectCommandToken.dispatch(4),
    ),
    stride = 16,
)
val arguments = device.makeBuffer(
    16L * capacity,
    storageMode = StorageMode.PRIVATE,
    usage = setOf(BufferUsage.STORAGE, BufferUsage.INDIRECT, BufferUsage.SHADER_DEVICE_ADDRESS),
)
// 別のCompute処理で各16バイトへPipeline番号とWorkgroup数x/y/zを書き込む。
command.compute {
    setBuffer(output, index = 0) // firstとsecondに共通するDescriptor Layout
    executeCommands(layout, arguments, maxSequenceCount = capacity)
}
```

Pipeline番号は`pipelines`の0始まりのIndexです。
最後のTokenにDraw、DrawIndexed、Dispatch、DrawMesh、TraceRaysのいずれかを置きます。
Pipeline Tokenは先頭に置き、Push Constants、Sequence Index、Vertex/Index BufferのTokenを間に配置できます。
TokenのOffsetとStrideは4バイト単位です。
`drawCount`、`drawIndexedCount`、`drawMeshCount`は、アドレスで指定する別の間接引数Bufferを使って複数のDrawを実行します。
この場合は`supportsMultiDrawCount`を確認し、`executeCommands`へ`maxDrawCount`を指定します。

| Tokenのデータ | 配置 |
| --- | --- |
| Pipeline | uint32 Pipeline Index |
| Push Constants | 指定したバイト数の値 |
| Sequence Index | 入力バイトなし。Sequence番号をPush Constantsへ書く |
| Vertex Buffer | uint64 Address、uint32 Size、uint32 Stride |
| Index Buffer | uint64 Address、uint32 Size、uint32 IndexType（0=UINT16、1=UINT32） |
| Draw | uint32 Vertex Count、Instance Count、First Vertex、First Instance |
| Indexed Draw | uint32 Index Count、Instance Count、First Index、int32 Base Vertex、uint32 First Instance |
| Dispatch / Mesh | uint32のWorkgroup数x/y/z |
| Count付きDraw | uint64引数Address、uint32 Stride、uint32 Count |
| Trace Rays | VkTraceRaysIndirectCommand2KHR。`pipeline.indirectTraceArguments(size)`でも生成可能 |

`countBuffer`を指定すると、GPUが書いた32ビット値をSequence数として使用できます。
値は0から`maxSequenceCount`までにし、Bufferには`INDIRECT`と`SHADER_DEVICE_ADDRESS`を付けます。
ComputeがこのBufferを書く場合は`STORAGE`も必要です。
Bufferの内容に含めるVertex/Index/間接引数などのアドレスはCPUから検査できないため、範囲とUsageを守り、対象をEncoderの`useResource`で保持します。

DescriptorはEncoderで設定したものを引き継ぎます。
Push Constants未設定の場合の初期値は0で、Tokenに指定した範囲をGPUデータで上書きします。
ShaderからResourceを動的に選ぶ場合は、Runtime Descriptor ArrayかBuffer Device Addressを使います。
Pipeline Selectionは固定状態、Stage、Descriptor/Push Constant Layout、Fragment出力が一致するPipeline同士に制限されます。
Pipelineを切り替えない場合は、通常のPipelineを1個指定し、Pipeline Tokenを省けます。

ComputeのSequence同士には実行順序の保証がありません。
依存する演算は別のEncoder操作に分けます。
Graphicsでは既定でSequence順を保ち、順序が不要なら`unorderedSequences = true`を指定できます。
この拡張の規則によりMultiviewとは組み合わせられません。
前処理用メモリは実行ごとに確保し、Command完了まで保持します。
[KhronosのDevice Generated Commands仕様](https://docs.vulkan.org/spec/latest/chapters/device_generated_commands/generatedcommands.html)の制約に従います。
