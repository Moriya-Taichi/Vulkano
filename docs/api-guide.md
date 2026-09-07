# APIの詳細と対応範囲

基本的な導入と使用例は[README](../README.md)を参照してください。ここではメモリの使い分け、Featureの選択、現在の対応範囲を説明します。

Metalに近いオブジェクト構成のAPIですが、Metal全体との機能互換性はありません。KotlinからJNIを介してVulkanを呼び出し、シェーダーにはSPIR-Vを使用します。

## Deviceの作成

`Device.create()`はGPUと要求されたFeatureを検査し、利用できなければ失敗します。AndroidのOSバージョンだけでは対応Featureを判断できません。通常はソフトウェアVulkanを選択せず、テスト時に`allowSoftwareRenderer = true`で許可できます。

## シェーダーと描画

BindingとPush Constantsの範囲、Workgroupの大きさはシェーダーから取得します。`bindings`や`pushConstantBytes`を明示した場合は、シェーダーの宣言との整合性も検査します。`setBytes`には`pipeline.pushConstantBytes`で確認できる範囲全体を渡してください。

オフスクリーン描画には`COLOR_ATTACHMENT`用途のTextureを渡します。頂点データはStorage Bufferから読むVertex Pulling、または`gl_VertexIndex`による生成を使用します。深度形式は`DEPTH32_FLOAT`に対応します。NDCのYは上向き、テクスチャの原点は左上、深度範囲は0〜1です。

## Android向けのメモリモデル

| Storage Mode | 用途 | 挙動 |
| --- | --- | --- |
| `SHARED` | CPUで更新・読み戻しするBuffer | HOST_VISIBLEを必須とし、CPUキャッシュを利用できるメモリを優先。必要に応じてFlush・Invalidateを実施 |
| `PRIVATE` | GPU用Buffer・Texture | GPUに適したメモリを優先。CPUアクセスにはShared BufferとのBlitを使用 |
| `MEMORYLESS` | Render Pass内で使い切るAttachment | Transient Attachmentとして確保。Lazy Allocationが使えれば選択し、使えなければ通常のメモリで同じ破棄規則を維持 |

AndroidではCPUとGPUが物理メモリを共有することが一般的ですが、すべてのメモリ種別がCPUから見えるとは限りません。`PRIVATE`も専用VRAMの存在を意味しません。`memoryHeaps()`の値を単純に合計して空きRAMとみなすことはできません。[Androidのメモリ選択指針](https://developer.android.com/ndk/guides/graphics/design-notes)

```kotlin
val depth = device.makeTexture(
    TextureDescriptor(
        width = 1920,
        height = 1080,
        pixelFormat = PixelFormat.DEPTH32_FLOAT,
        usage = setOf(TextureUsage.DEPTH_ATTACHMENT),
        storageMode = StorageMode.MEMORYLESS,
    )
)
// DepthAttachment(depth)の既定値はCLEAR / DONT_CARE
```

`MEMORYLESS`では前回の内容のLoad、Render Pass終了時のStore、Sampling、Compute、転送を禁止します。`texture.isLazilyAllocated`で実際にLazy Allocationを利用できたか確認できます。サブアロケーションとNon-coherent Atom Sizeへの対応はVMAが担当します。[Khronosのメモリ管理ガイド](https://docs.vulkan.org/guide/latest/memory_allocation.html)

`Buffer.write/read`はDirect ByteBufferの`position`〜`limit`をコピーし、呼び出し元のPositionを変更しません。GPU処理中のBufferにはCPUからアクセスできません。`waitUntilCompleted()`後に読み書きしてください。GPU完了を検査できた場合は自動的にアクセスを許可します。直接MapしたポインタをJVMへ公開するAPIはありません。

詳細は[メモリと同期](memory-and-synchronization.md)を参照してください。

CPUからの書き込みだけを行う場合は`makeUploadBuffer()`も利用できます。Uniform／Storageとして直接GPUから参照する例と、CPU読み出しの制約は[Upload Bufferの説明](mobile-gpu-optimization.md#cpu書き込み専用upload-buffer)を参照してください。

## Featureの選択

```kotlin
val device = Device.create(
    requiredFeatures = setOf(
        Feature.SHADER_FLOAT16,
        Feature.STORAGE_BUFFER_16_BIT_ACCESS,
    )
)
```

要求したFeatureが利用できなければ作成に失敗します。`capabilities.availableFeatures`と`enabledFeatures`は区別され、広告されているだけのFeatureをシェーダーで使うことはできません。Float16の演算と16-bit Storageは別のFeatureです。

Subgroupの幅・Stage・演算を取得し、使用するシェーダーとの適合性を確認します。幅を32や64に固定しません。画像形式は`supportsTexture(descriptor)`で問い合わせられます。`memoryHeaps()`は`VK_EXT_memory_budget`に対応する端末でBudgetとUsageを取得し、非対応時の推定値には`estimated = true`を付けます。

## 対応範囲と制約

| 項目 | 0.1の対応 |
| --- | --- |
| Compute | Storage/Uniform Buffer、Storage/Sampled Texture、Push Constants、固定Local Size、端末対応範囲のSubgroup |
| Graphics | Triangle List、Instancing、Vertex Pulling、Color 1枚、任意のDepth 1枚、Alpha Blending、全面Viewport/Scissor |
| Texture | 2D、1 Mip、1 Layer、1 Sample。RGBA8/BGRA8 UNORM、RGBA16/RGBA32/R32 Float、Depth32 Float |
| Blit | Buffer間、BufferとTextureの相互転送。Textureは画像全体・行を詰めた配置 |
| Presentation | Android Surface、FIFO。未提示Drawableの破棄、Resize、Out-of-dateへの対処 |
| Shader | SPIR-V 1.0〜1.3、Logical/GLSL450 Memory Model、Descriptor Set 0、Bindingごとに1 Resource |

GraphicsのStorage Bufferは`readonly`宣言が必要です。MSAA、Mip生成、Texture Array/Cube、圧縮Textureの作成、Descriptor Array/Bindless、頂点属性レイアウト、Indexed/Indirect Draw、複数Render Target、複数Queueでの並列実行、Specialization Constants、AHardwareBuffer/Cameraとの共有、Ray Tracingは未対応です。ASTC/ETC2のFeature情報は取得できますが、圧縮Texture形式はまだ公開していません。16-bit演算とSubgroupを混在させるシェーダーも、Extended Typesを有効化していないため拒否します。

自動同期は保守的なBarrierを使用します。Descriptor SetをCommand内で再利用し、Surfaceは同時に1枚だけ取得してPresentationの完了を待ちます。Mali・PowerVR・Adreno・Xclipseを対象としたDescriptor、Pipeline、メモリ、画像Layoutの最適化は[モバイルGPU最適化](mobile-gpu-optimization.md)を参照してください。GPU固有の性能や熱・電力特性は計測していません。
