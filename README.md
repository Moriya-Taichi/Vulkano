# Vulkano

AndroidのVulkanをKotlinから扱うライブラリです。Metalの`Device`、`CommandQueue`、`CommandBuffer`、各種Encoderに近いAPIで、Compute、テクスチャ転送、オフスクリーン描画、Androidの`Surface`への描画を記述できます。

Kotlinからの呼び出しをJNI経由でVulkanに接続します。Descriptor Set、画像レイアウト、メモリ確保、GPU処理が完了するまでのリソース保持はライブラリが管理します。シェーダーはSPIR-Vを使用します。

**0.1.0-SNAPSHOT**の初期実装です。対応範囲と制約は下記のとおりです。Metal全体との機能互換性はありません。

## 対応環境

- Android 10 / API 29以上、`arm64-v8a`・`x86_64`。
- Vulkan 1.1以上と、Graphics・Computeの両方に対応するQueueが必要です。`Device.create()`で実際のGPUを検査します。
- GPUを使う通常のアプリではソフトウェア実装を選択しません。テスト時のみ`allowSoftwareRenderer = true`を指定できます。
- ビルド環境：JDK 17、Gradle 8.11.1、AGP 8.9.2、Kotlin 2.1.20、SDK 35、NDK 28.1.13356709、CMake 3.22.1。

AndroidのOSバージョンだけでVulkanのFeatureを判断できないため、Featureと画像形式の対応を実行時に検査します。[AndroidのVulkan概要](https://developer.android.com/codelabs/beginning-vulkan-on-android)

## ビルドと導入

```sh
./gradlew :vulkano:assembleRelease :sample:assembleDebug
```

AARは`vulkano/build/outputs/aar/vulkano-release.aar`に生成されます。ソースから組み込む場合はAndroidライブラリモジュールとして`:vulkano`を参照してください。

ローカルのMavenリポジトリへ配置する場合：

```sh
./gradlew :vulkano:publishReleasePublicationToMavenLocal
```

```kotlin
// repositoriesにmavenLocal()を追加したプロジェクトで使用
implementation("dev.vulkano:vulkano:0.1.0-SNAPSHOT")
```

サンプルのRelease APKは難読化の検証用にDebug Keyで署名します。

Maven Centralには公開していません。ビルドに必要なVMAとSPIRV-Reflectは固定したバージョンを同梱しています。

## Compute

次のシェーダーを`double.comp`として用意します。`dispatchThreads`はWorkgroup数を切り上げるため、端のInvocationをシェーダーで除外します。

```glsl
#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) buffer Values { float values[]; };
layout(push_constant) uniform Parameters { uint count; } parameters;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < parameters.count) values[i] *= 2.0;
}
```

NDKの`glslc`でオフラインコンパイルします。サンプルアプリではAGPが`src/main/shaders`から自動コンパイルします。

```sh
glslc --target-env=vulkan1.1 double.comp -o double.comp.spv
```

以下はワーカースレッドから実行する例です。`assets`はAndroidの`AssetManager`です。

```kotlin
import dev.vulkano.*
import java.nio.ByteBuffer
import java.nio.ByteOrder

Device.create().use { device ->
    val function = device.makeLibrary(
        assets.open("shaders/double.comp.spv").use { it.readBytes() }
    ).makeFunction()
    val pipeline = device.makeComputePipelineState(function)
    val buffer = device.makeBuffer(length = 16)
    val queue = device.makeCommandQueue()

    val data = ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder())
    data.asFloatBuffer().put(floatArrayOf(1f, 2f, 3f, 4f))
    buffer.write(data)

    queue.makeCommandBuffer().use { command ->
        command.compute {
            setComputePipelineState(pipeline)
            setBuffer(buffer, index = 0)
            setBytes(ByteBuffer.allocate(4).order(ByteOrder.nativeOrder()).putInt(4).array())
            dispatchThreads(Size(4))
        }
        command.commit()
        command.waitUntilCompleted()
    }

    buffer.read(data)
    val result = List(4) { data.getFloat(it * 4) } // [2, 4, 6, 8]
}
```

BindingとPush Constantsの範囲、Workgroupの大きさはシェーダーから取得します。`bindings`や`pushConstantBytes`を明示した場合は、シェーダーの宣言との整合性も検査します。`setBytes`には`pipeline.pushConstantBytes`で確認できる範囲全体を渡してください。

## 描画

`SurfaceHolder.surfaceChanged`で有効な`Surface`とサイズを受け取り、`device.makeSurfaceLayer(surface, width, height)`を呼び出します。パイプラインのColor Formatは`layer.pixelFormat`に合わせます。

```kotlin
layer.nextDrawable()?.use { drawable ->
    queue.makeCommandBuffer().use { command ->
        command.render(RenderPassDescriptor(ColorAttachment(drawable.texture))) {
            setRenderPipelineState(pipeline)
            drawPrimitives(vertexCount = 3)
        }
        command.present(drawable)
        command.commit()
    }
}
```

サンプルは[MainActivity.kt](sample/src/main/kotlin/dev/vulkano/sample/MainActivity.kt)です。Computeの結果を画面に表示し、三角形を描きます。GPU操作をワーカースレッドに集約し、`surfaceDestroyed`が返る前に処理を終了します。画面サイズの変更、Surfaceの再作成、Vulkanを利用できない場合も扱っています。

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

詳細は[メモリと同期](docs/memory-and-synchronization.md)を参照してください。

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

自動同期は保守的なBarrierを使用します。Descriptor SetをCommand内で再利用し、Surfaceは同時に1枚だけ取得してPresentationの完了を待ちます。Mali・PowerVR・Adreno・Xclipseを対象としたDescriptor、Pipeline、メモリ、画像Layoutの最適化は[モバイルGPU最適化](docs/mobile-gpu-optimization.md)を参照してください。GPU固有の性能や熱・電力特性は計測していません。

## 検証

```sh
# Kotlinの契約テスト。通常はGPU統合テストをスキップ
./gradlew :vulkano:testDebugUnitTest

# 接続したAndroid端末でJNI経由のComputeを検証
./gradlew :vulkano:connectedDebugAndroidTest
```

LinuxのVulkan環境ではNative BackendとKotlin → JNI → Vulkanの両方を検証できます。

```sh
cmake -S vulkano/src/main/cpp -B build/host
cmake --build build/host
python3 tools/run-native-tests.py build/host/vulkano_tests
./gradlew :vulkano:testDebugUnitTest -Pvulkano.hostLibraryPath="$PWD/build/host"
```

Validation LayerとソフトウェアICDが必要です。複数ICDがある場合は`VK_ICD_FILENAMES`を設定してください。CIはシェーダーの検証、同期検証を有効にしたNative Test、Kotlin経由のGPU Test、AAR、難読化したサンプルAPK、Instrumentation APKのビルドを行います。

NDK r28とAGP 8.9を使用し、共有ライブラリは16 KBで配置します。[Androidの16 KBページ対応](https://developer.android.com/guide/practices/page-sizes)

端末で確認する項目は[Android実機検証](docs/android-validation.md)に記載しています。
