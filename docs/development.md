# ビルドと検証

アプリへの導入は[README](../README.md#導入)を参照してください。以下は、このリポジトリのサンプルやライブラリ自体を開発するための手順です。コマンドはリポジトリのルートで実行します。

## ビルド環境

| 項目 | バージョン |
| --- | --- |
| JDK | 17 |
| Gradle | 8.11.1（Wrapperで指定） |
| Android Gradle Plugin | 8.9.2 |
| Kotlin | 2.1.20 |
| Android SDK | 35 |
| Android NDK | 28.1.13356709 |
| CMake | 3.22.1 |

Android StudioのSDK ManagerなどでSDK・NDK・CMakeを用意し、SDKのパスを`ANDROID_HOME`または`local.properties`の`sdk.dir`で指定します。

VMA、SPIRV-Reflect、Vulkan-HeadersのC APIは固定したバージョンを同梱しています。バージョンとライセンスは[third_partyの説明](../vulkano/src/main/cpp/third_party/README.md)に記載しています。

## AARとサンプル

```sh
./gradlew :vulkano:assembleRelease :sample:assembleDebug
./gradlew :sample:installDebug
```

AARは`vulkano/build/outputs/aar/vulkano-release.aar`に生成されます。ソースから組み込む場合はAndroidライブラリモジュールとして`:vulkano`を参照してください。

サンプルの[MainActivity.kt](../sample/src/main/kotlin/dev/vulkano/sample/MainActivity.kt)はCompute結果の表示と三角形の描画を行います。GPU操作をワーカースレッドに集約し、画面サイズの変更、Surfaceの再作成、Vulkanを利用できない場合を扱っています。

サンプルはAGPが`src/main/shaders`からシェーダーを自動コンパイルする設定です。[sample/build.gradle.kts](../sample/build.gradle.kts)を参照してください。サンプルのRelease APKは難読化の検証用にDebug Keyで署名します。

## 検証

```sh
# Kotlinの契約テスト。通常はGPU統合テストをスキップ
./gradlew :vulkano:testDebugUnitTest

# 接続したAndroid端末で共通の描画・Compute・Resourceテストを検証
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

CIのValidation Layerは同梱ヘッダーに合わせてVulkan-ValidationLayers v1.4.335を使用します。
初回に公式ソースと固定された依存関係からビルドし、以降はInstall済みファイルをキャッシュします。
古いLayerはDevice Generated Commandsなどの新しい構造体を認識せず、正しい問い合わせもエラーとして報告するため、実機検証でも新しいLayerを使用してください。

NDK r28とAGP 8.9を使用し、共有ライブラリは16 KBで配置します。[Androidの16 KBページ対応](https://developer.android.com/guide/practices/page-sizes)

端末で確認する項目は[Android実機検証](android-validation.md)に記載しています。

過去に実施した検証と実機未検証の項目は[検証結果](validation-results.md)に記載しています。

配布用のAAR・POM・Sources・APIドキュメントをまとめる手順は[ライブラリの公開](publishing.md)を参照してください。
