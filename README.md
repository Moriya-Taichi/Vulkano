# Vulkano

Vulkanoは、AndroidでVulkanをKotlinから使うためのGPUライブラリです。Metalに近い`Device`、`CommandQueue`、`CommandBuffer`、EncoderのAPIで、GPUによる計算や描画を記述できます。

Vulkanのメモリ確保、リソースのBinding、画像レイアウトの遷移、コマンド間の同期をライブラリが管理します。アプリは、SPIR-V形式のシェーダーと、そのシェーダーで処理するデータを用意します。

## できること

| 用途 | 機能 |
| --- | --- |
| GPUで計算する | Compute ShaderでBuffer、Texture、Tensorを処理し、結果をCPUへ読み戻す。対応端末ではCooperative Matrixも利用する |
| 画像を描く | MSAA、Indexed/Indirect Draw、頂点属性、MRT、Depth/Stencil、Subpass、Multiview、Shading Rate、Tessellation、Mesh Shaderを使って描画する |
| データを転送する | Buffer、Textureの領域やMip、配列Sliceをコピーし、Mipを生成する |
| 光線を追跡する | 対応端末でBLAS/TLASの構築、Copy、Compaction、保存・復元を行い、Ray QueryまたはRay Tracing Pipelineを実行する |
| リソースを管理する | Texture View、Resource配列、Heap、Shared Event、Counter、Pipeline Cacheを使う |
| 端末に合わせる | 利用可能なFeature、画像形式、処理サイズの上限、メモリ情報を取得する |

Android 10（API 29）以上の`arm64-v8a`・`x86_64`に対応します。Vulkan 1.1以上とGraphics・Computeを扱えるGPUが必要です。実際に利用できるかは`Device.create()`で検査します。

## 導入

現在のバージョンは`0.1.0-SNAPSHOT`です。Maven Centralには未公開のため、ソースからビルドしてローカルのMavenリポジトリへ配置します。JDK 17と、このリポジトリで指定するAndroid SDK・NDK・CMakeを用意してください。バージョンと設定は[ビルド環境](docs/development.md#ビルド環境)に記載しています。

```sh
git clone https://github.com/Moriya-Taichi/Vulkano.git
cd Vulkano
./gradlew :vulkano:publishReleasePublicationToMavenLocal
```

利用するアプリの`settings.gradle.kts`で、依存関係のリポジトリに`mavenLocal()`を追加します。

```kotlin
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        mavenLocal()
    }
}
```

アプリモジュールの`build.gradle.kts`に依存関係を追加し、`minSdk`を29以上にします。

```kotlin
dependencies {
    implementation("io.github.moriya-taichi:vulkano:0.1.0-SNAPSHOT")
}
```

AARの生成やソースモジュールからの組み込みは[ビルドと検証](docs/development.md)を参照してください。

## 使い方

### GPUで配列を処理する

4個の数値をGPUで2倍にする例です。次のシェーダーを`double.comp`として用意します。`dispatchThreads`は実行するWorkgroup数を切り上げるため、シェーダーで配列の範囲を確認します。

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

NDK付属の`glslc`でSPIR-Vへ変換し、アプリの`app/src/main/assets/shaders/double.comp.spv`に配置します。

```sh
glslc --target-env=vulkan1.1 double.comp -o double.comp.spv
```

Kotlin側では、シェーダーを読み込んでPipelineを作成し、Bufferを渡して実行します。以下はワーカースレッドで実行してください。`assets`はAndroidの`AssetManager`です。

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
    val result = List(4) { data.getFloat(it * 4) } // [2.0, 4.0, 6.0, 8.0]
}
```

`compute {}`で処理を記録し、`commit()`でGPUへ送信します。CPUで結果を読み戻す前に`waitUntilCompleted()`で完了を待ちます。Binding、Push Constants、Workgroupの大きさはシェーダーから取得します。この例の`setBytes`は、処理する要素数を渡しています。

### 画面に描画する

有効なAndroidの`Surface`から`SurfaceLayer`を作成し、取得したDrawableを描画先にします。以下では、作成済みの`device`と`queue`、読み込んだ頂点・フラグメントシェーダーの`vertexFunction`・`fragmentFunction`を使用します。シェーダーとSurfaceの管理を含む実装は[サンプルアプリ](sample/src/main/kotlin/dev/vulkano/sample/MainActivity.kt)にあります。

```kotlin
val layer = device.makeSurfaceLayer(surface, width, height)
val pipeline = device.makeRenderPipelineState(
    vertexFunction = vertexFunction,
    fragmentFunction = fragmentFunction,
    colorFormat = layer.pixelFormat,
)

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

LayerとPipelineは初期化時に作成し、フレームごとにDrawableを取得します。`nextDrawable()`が`null`の場合は次のフレームで再試行します。Surfaceのサイズ変更は`layer.resize()`で反映し、`surfaceDestroyed`が返る前に、そのSurfaceを使うGPU処理を終了してください。

### メモリを選ぶ・リソースを解放する

| Storage Mode | 主な用途 |
| --- | --- |
| `SHARED` | CPUから更新・読み戻しするBuffer。`makeBuffer()`の既定値 |
| `PRIVATE` | GPU用のBufferやTexture。CPUとの受け渡しにはShared BufferとのBlitを使う |
| `MEMORYLESS` | Render Passの中だけで使うDepthなどのAttachment |

CPUから書き込むだけの用途には`makeUploadBuffer()`も利用できます。GPU処理中のBufferをCPUから読み書きする場合は、先に処理の完了を待ってください。

Resourceは`use`または`close()`で解放します。`Device.close()`は残っている子Resourceも解放するため、上のCompute例ではDeviceの`use`を抜けるとBufferやPipelineも解放されます。Deviceを長く保持するアプリでは、不要になったResourceをその都度閉じてください。送信済みCommand Bufferの`close()`はGPUの完了を待ちます。

## 詳しい使い方と対応範囲

MSAA、Mip生成、Indexed Drawは基本APIから利用できます。
Ray TracingやMesh Shaderなどの追加機能は、端末が提供するFeatureをDevice作成時に要求します。
利用手順は[拡張APIの使用例](docs/advanced-features.md)、Metalとの対応と残る機能は[機能対応表](docs/metal-coverage.md)を参照してください。

- [APIの詳細と対応範囲](docs/api-guide.md)：Featureの選択、画像形式、シェーダーや描画の制約
- [メモリと同期](docs/memory-and-synchronization.md)：CPU/GPUアクセス、送信順、リソースとSurfaceの寿命
- [モバイルGPU最適化](docs/mobile-gpu-optimization.md)：Mali・PowerVR・Adreno・Xclipse向けの実装
- [ビルドと検証](docs/development.md)：サンプルの実行、ライブラリのビルドとテスト
- [ライブラリの公開](docs/publishing.md)：Mavenリポジトリの生成とMaven Centralへの公開

GPUごとの実機性能は未計測です。実施済みの確認は[検証結果](docs/validation-results.md)、端末での確認手順は[Android実機検証](docs/android-validation.md)に記載しています。

## ライセンス

Vulkano本体は[Apache License 2.0](LICENSE)で提供します。Copyright 2026 Moriya-Taichi。

同梱するVMA・SPIRV-Reflect・SPIR-V Headersには、それぞれのライセンスが適用されます。著作権・ライセンス表示は[third_party](vulkano/src/main/cpp/third_party/)に保持し、配布するAARとSources JARにも同梱します。
