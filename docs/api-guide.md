# APIの詳細と対応範囲

基本的な導入と使用例は[README](../README.md)を参照してください。ここではメモリの使い分け、Featureの選択、現在の対応範囲を説明します。

Metalに近いオブジェクト構成のAPIですが、Metal全体との機能互換性はありません。KotlinからJNIを介してVulkanを呼び出し、シェーダーにはSPIR-Vを使用します。

## Deviceの作成

`Device.create()`はGPUと要求されたFeatureを検査し、利用できなければ失敗します。AndroidのOSバージョンだけでは対応Featureを判断できません。通常はソフトウェアVulkanを選択せず、テスト時に`allowSoftwareRenderer = true`で許可できます。

## シェーダーと描画

BindingとPush Constantsの範囲、Workgroupの大きさはシェーダーから取得します。`bindings`や`pushConstantBytes`を明示した場合は、シェーダーの宣言との整合性も検査します。`setBytes`には`pipeline.pushConstantBytes`で確認できる範囲全体を渡してください。

オフスクリーン描画には`COLOR_ATTACHMENT`用途のTextureを渡します。頂点データは頂点属性レイアウトとVertex Buffer、Storage BufferからのVertex Pulling、`gl_VertexIndex`による生成を使用できます。
Depth16、Depth32、Stencil8、Depth/Stencil複合形式は、端末のFormat対応を検査して作成します。NDCのYは上向き、テクスチャの原点は左上、深度範囲は0〜1です。

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

Subgroupの幅・Stage・演算を取得し、使用するシェーダーとの適合性を確認します。幅を32や64に固定しません。画像形式は`supportsTexture(descriptor)`で問い合わせられます。
`textureFormatCapabilities(format, usage, textureType, storageMode)`では、指定した組み合わせのMSAA対応数、最大サイズ、Mip・Layer上限、Formatの演算対応を取得できます。
どちらも端末の対応を照会し、必要なFeatureの有効化と実メモリの確保は行いません。
Format照会が返す`requiredFeatures`をDevice作成時に有効にし、Storage MSAAには`STORAGE_IMAGE_MULTISAMPLE`も要求します。
`memoryHeaps()`は`VK_EXT_memory_budget`に対応する端末でBudgetとUsageを取得し、非対応時の推定値には`estimated = true`を付けます。

## 対応範囲と制約

機能別の対応状況は[Metalとの機能対応表](metal-coverage.md)、使用例は[拡張API](advanced-features.md)にまとめています。
SPIR-Vのバージョンは実効Vulkanバージョンに従い、Vulkan 1.1ではSPIR-V 1.3、Vulkan 1.2では1.5、Vulkan 1.3では1.6までを受け付けます。
Vulkan 1.1にRay Tracing/Meshの拡張を追加した構成では、依存するSPIR-V 1.4拡張も確認します。
シェーダーはDescriptor Set 0を使用します。

Function Constantsは符号付き・符号なしの8/16/32/64ビット整数、Half、Float、Double、Booleanを指定できます。
SPIR-VのScalar型とバイト数を照合し、型幅の違いと未宣言のConstant IDを拒否します。
整数や浮動小数点の演算には、型に対応するFeatureの有効化も必要です。
固定Local Sizeに加え、Local Sizeのスカラー特殊化定数に対応します。
Workgroupサイズは整数・Booleanの特殊化定数式を評価します。算術、比較、選択、ビット演算、整数型変換、Vectorの演算・抽出・挿入・並べ替えに対応します。
SPIR-Vの整数幅で計算し、ゼロ除算・不正なShift・循環参照・ゼロのサイズ・端末上限超過はPipeline作成前に拒否します。
`WorkgroupSize`が存在する場合は`LocalSize` / `LocalSizeId`より優先します。

固定Descriptor Arrayは全要素をBindingしてください。
Runtime Descriptor Arrayは`DESCRIPTOR_INDEXING`と明示的な`BindingLayout.count`を要求し、未使用要素を省略できます。
シェーダーが未Bindingの要素や配列範囲外へアクセスしないようにしてください。
ここでの配列はCommandに記録したBindingのスナップショットであり、送信済みDescriptorを書き換えるAPIではありません。

GraphicsでStorageを書き込む場合は、Stageに応じて`VERTEX_STORES_AND_ATOMICS`または`FRAGMENT_STORES_AND_ATOMICS`を要求してください。
同じRender Pass内のDraw間に任意のStorage依存を自動挿入しません。
後続Drawが先行DrawのStorage結果を必要とする場合はPassを分け、保持するAttachmentにはPrivate TextureのLOAD/STOREを使います。
Memoryless TextureはPassをまたいで保持できません。

GPUが生成するIndirect引数、Index値、デバイスアドレスの参照先はシェーダー側でも範囲を守る必要があります。
Instanceの`stepRate`を1以外にする場合、`vertexInputCapabilities().supportsNonZeroFirstInstance`がfalseの端末ではIndirect引数の`firstInstance`も0にします。
`Buffer.gpuAddress`から間接参照するBufferは、Encoderの`useResource(buffer)`でCommandに保持させてください。
TextureのStorage書き込みは、指定したSubresource全体の初期化をアプリが保証する契約です。

Mali、PowerVR、Adreno、Xclipse向けのメモリ選択とキャッシュ方針は[モバイルGPU最適化](mobile-gpu-optimization.md)を参照してください。
GPU固有の性能、熱特性、消費電力は実機未計測です。
