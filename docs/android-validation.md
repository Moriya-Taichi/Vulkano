# Android実機検証

Nativeの数値・画像テストはLinuxのソフトウェアVulkanでも実行できますが、Adreno、Mali、PowerVR、Xclipseの実機ドライバ、Surfaceのライフサイクル、メモリ種別の違いを代替するものではありません。

## ビルド

```sh
./gradlew :vulkano:assembleRelease :sample:assembleDebug :sample:assembleRelease :vulkano:assembleDebugAndroidTest
./gradlew :sample:installDebug
./gradlew :vulkano:connectedDebugAndroidTest
```

サンプルでGPU名、Compute結果`[2.0, 4.0, 6.0, 8.0]`、色付き三角形が表示されることを確認します。ホーム画面への移動・復帰、画面回転、Activity再生成、画面の消灯と復帰も確認します。描画用ResourceはSurfaceの破棄前に解放される必要があります。

`GpuTestRunner`はJVMと共通の`GpuIntegrationTest`、`GraphicsIntegrationTest`、API契約テストを実機で実行します。
ShaderはテストAPKのAssetsから読み込みます。
追加Featureがない端末では、その成功経路をスキップし、サポートをメーカー名から仮定しません。

| 機能群 | 実機で確認する内容 |
| --- | --- |
| 描画 | MSAA、Color/Depth Resolve、Indexed/Indirect Draw、MRT、Stencil、整数Clear |
| Texture | Mip、Array、View、Swizzle、圧縮形式の対応問い合わせ、非重複領域のCopy |
| Subpass | Memoryless入力、途中のPassでの保持、Depth/MSAA入力、Color Resolve、Multiview |
| 追加Stage | Tessellation、Mesh Shader、Runtime Descriptor Array |
| Ray Tracing | BLAS/TLAS、Query/Pipeline、Copy/Compaction、ArchiveのBLAS再配置 |
| Shading Rate | PipelineのサイズとSample Count、R8_UINTのRate Map |
| Tensor | Viewの寿命とStride、対応GPUでのCooperative Matrixの積 |
| 同期と管理 | TimelineのHost Signal、Counter、Heap、Pipeline Cache |

Vulkan 1.1だけを備える端末でも、基本描画とComputeの結果を確認します。
Ray TracingやCooperative Matrixは、対応する端末の結果を別途記録します。
SurfaceのPresentationと再作成は、サンプルを使う手動検証も必要です。

## 検証する端末の差

| 条件 | 確認点 |
| --- | --- |
| Vulkan 1.1端末 | 1.2/1.3の必須シンボルに依存せず起動・Compute・描画できる |
| Mali / PowerVR / Adreno / Xclipse | 画像結果、Feature問い合わせ、WorkgroupとRangeの上限、Samplerの対応 |
| Non-coherentなShared Memory | CPU書き込み後のGPU読み取り、GPU書き込み後のCPU読み戻し |
| Lazy Allocation対応 / 非対応 | Memorylessの破棄規則と`isLazilyAllocated`の値が一致する |
| メモリ圧迫時 | Budgetの変化、確保失敗、不要Resourceの解放。Heapの和を空きRAMと扱わない |
| 16 KBページ端末 | インストール、起動、JNI、Compute、回転と復帰 |
| Vulkan非対応 | Device作成失敗をアプリで表示・処理できる |

## Validation Layer

Android用`VK_LAYER_KHRONOS_validation`をデバッグ環境に配置し、`Device.create(enableValidation = true)`を使います。Layerが配置されていなければ作成に失敗します。利用できるLayerではSynchronization Validationも有効にします。通常の配布AARにはLayerを含めません。[AndroidでのValidation Layerの利用](https://developer.android.com/ndk/guides/graphics/validation-layer)

## 16 KBページ

```sh
adb shell getconf PAGE_SIZE
"$ANDROID_HOME/build-tools/35.0.0/zipalign" -c -P 16 -v 4 sample/build/outputs/apk/release/sample-release.apk
```

16 KB端末では`PAGE_SIZE`が`16384`になります。AAR/APK内の各ABIの`libvulkano.so`について、`llvm-readelf -lW`でLOADセグメントのAlignmentが`0x4000`以上になっていることも確認します。[Androidのページサイズ対応](https://developer.android.com/guide/practices/page-sizes)

Sparse対応端末ではPage/TileのMap/Unmap、Mapping共有、Mip Tail、Shader Residencyのテストも実行します。
Placement HeapはAlignmentと領域の検査、Alias Barrier後の描画結果を確認します。
