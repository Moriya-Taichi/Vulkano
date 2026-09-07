# Android実機検証

Nativeの数値・画像テストはLinuxのソフトウェアVulkanでも実行できますが、Adreno・Mali等の実機ドライバ、Surfaceのライフサイクル、メモリ種別の違いを代替するものではありません。

## ビルド

```sh
./gradlew :vulkano:assembleRelease :sample:assembleDebug :sample:assembleRelease :vulkano:assembleDebugAndroidTest
./gradlew :sample:installDebug
./gradlew :vulkano:connectedDebugAndroidTest
```

サンプルでGPU名、Compute結果`[2.0, 4.0, 6.0, 8.0]`、色付き三角形が表示されることを確認します。ホーム画面への移動・復帰、画面回転、Activity再生成、画面の消灯と復帰も確認します。描画用ResourceはSurfaceの破棄前に解放される必要があります。

## 検証する端末の差

| 条件 | 確認点 |
| --- | --- |
| Vulkan 1.1端末 | 1.2/1.3の必須シンボルに依存せず起動・Compute・描画できる |
| Adreno / Mali等 | 画像結果、Feature問い合わせ、WorkgroupとRangeの上限、Samplerの対応 |
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
