# 検証結果 — 2026-09-07

この変更では次を確認しました。

| 検証 | 結果 |
| --- | --- |
| Android AAR Release | arm64-v8a / x86_64のビルド成功 |
| Androidサンプル | Debug / R8で難読化したReleaseのAPKビルド成功 |
| Android Instrumentation | テストAPKのビルド成功。端末での実行は未実施 |
| Native Vulkan | llvmpipe（LLVM 20.1.2）で535件の検査に成功。画素・要素単位の比較を含む |
| Kotlin → JNI → Vulkan | GPU統合テスト5件成功、スキップなし |
| KotlinのAPI契約 | 2件成功 |
| Validation Layer | NativeとKotlinの検証でValidation Error・Synchronization Hazardなし |
| ELF Alignment | AAR・Release APKの両ABIで、全LOADセグメントが16,384バイト |
| APK Alignment | `zipalign -c -P 16 4`成功 |

NativeではShared/Private間の転送、複数Compute Dispatchの依存関係、記録後に解放したResourceの保持、Textureへの転送・Compute・Sampling・描画・読み戻し、Memoryless Depth、Attachment Load/Discard、失敗時のLayout状態の保全を確認しました。未有効化Feature・異なるDevice・Shader Layout不一致・範囲外の引数も拒否されることを確認しています。Float16は無効時の拒否と、対応Deviceで有効化した場合の演算結果を検証しました。

接続されたAndroid端末はありません。AndroidのPresentation経路、Surfaceの再作成、Adreno/Mali固有の挙動、Non-coherentな実メモリ、Lazy Allocationを備えた実機、16 KBカーネルでの実行は未検証です。性能・熱・電力についても測定結果はありません。

再実行方法は[README](../README.md)と[実機検証手順](android-validation.md)を参照してください。

## モバイルGPU最適化の回帰検証（2026-09-07）

- llvmpipe（LLVM 20.1.2）でNative検査611件成功。Validation Error / Synchronization Hazardなし。
- 130 Dispatch・65 BindingでDescriptor Pool 2個、Descriptor Set更新65回。100 Drawでは更新1回。GPU結果のReadbackも一致。
- Offset/RangeとPush Constantsの変更、Shared常時マッピング、Render Passキャッシュ、Sampled-onlyのOptimal Layout、Storage対応TextureのGENERAL経路を検証。
- Kotlin GPU統合テスト5件、APIテスト2件成功。スキップなし。
- arm64-v8a / x86_64のRelease AAR、サンプルDebug / Release APKのビルド成功。
- Mali・PowerVR・Adreno実機の速度、帯域、温度、電力は未計測。API呼び出し回数を実機の速度改善率として扱わない。

## Xclipse / RDNA対応の回帰検証（2026-09-07）

- llvmpipeとValidation / 同期検証でNative検査724件成功。Validation Error / Synchronization Hazardなし。
- 32回のUpload直接参照・Compute・Readbackが一致し、同一Poolと一次Command Bufferを31回再利用。使用中Poolの分離、保持上限8組、失敗Commandの破棄を検証。
- Kotlin/JNI GPU統合テスト6件、APIテスト2件成功。スキップなし。Upload CPU read拒否とGPU結果の一致を含む。
- 両ABIのRelease AAR、サンプルDebug / Release APKのビルド成功。
- Xclipse実機の性能とNon-coherentメモリは未検証。Wave幅や専用VRAMを仮定していない。

## 機能拡張の検証（2026-09-08）

[PR #7](https://github.com/Moriya-Taichi/Vulkano/pull/7)の初回コミット`65ecae7`はAndroid CIが成功しました。
両ABIのAAR、Debug/Releaseのサンプル、Instrumentation APK、JNIテスト、Maven公開用成果物を検証しています。
Instrumentation APKの端末での実行は含みません。

ローカルではLavapipe（Mesa 24.0.5、LLVM 17）とValidation Layerを使っています。
MSAAとColor/Depth Resolve、Indexed/Indirect Draw、MRT、Vertex Input、Mip生成、Array/View、Texture Buffer、Function Constants、Timeline Event、Counter、Heap、Pipeline Cache、整数ClearをGPU出力と照合しました。
Tessellation、Mesh、Multiview、Runtime Descriptor Array、Shading Rate、Ray TracingはFeatureに応じて実行するテストです。
Ray QueryとRay Tracing PipelineはこのローカルDriverでは非対応のため、成功経路を実行していません。

ローカルのKotlin/JNIテストは36件中30件成功、6件スキップです。
スキップはPipeline Shading Rate、Rate Map、Ray Tracing Pipeline、AS Copy/Compaction、AS Archive、Cooperative Matrixです。
Ray Queryの成功経路は、非対応時の拒否を確認する分岐に入るため実行していません。
Tensor View、SubpassのMemoryless/MSAA/Depth入力、Color Resolve、Multiview、Swizzleの結果を追加で照合しています。
新しいSurfaceの取得待ちとNative Handleの破棄経路は、Androidでの実行確認が必要です。

Nativeの既存724チェックも保持しています。
新機能のMali、PowerVR、Adreno、Xclipse上での性能、電力、Driver固有の挙動は実機未検証です。

### Placement Heap

Placement BufferのAlias、非重複領域へのCopy、TextureのAlias切り替えとRGB読み戻しを追加しました。
配置範囲、Alignment、重複Copy、未初期化Textureの読み取りも検査しています。
ローカル結果はKotlin/JNI 38件中32件成功、6件スキップ、失敗0件です。
Android CIで見つかったC++17のLambda Captureを修正し、再ビルドの対象にしています。

### Sparse Resourceと追加CI

Placement Heapまでを含むCommit `26205c84ad781aca67429af248e5f109298dd2b6`の[Android CI](https://github.com/Moriya-Taichi/Vulkano/actions/runs/34224756190)が成功しました。
AAR、Debug/Release Sample、Instrumentation APK、Kotlin/JNIテスト、Maven配布物の検証を含みます。
Android実機での実行結果は含みません。

その後、SparseのFeature未有効時の拒否、Buffer PageのMap/Unmapと共有、Texture Tile/Mip TailとShader Residencyのテストを追加しました。
ローカルではKotlin/JNI 41件中33件成功、8件スキップ、失敗0件です。
追加のスキップはSparse BufferとSparse Textureの2件で、使用したLavapipeがSparse Residencyに対応しないためです。
Sparse Shaderはglslcでコンパイルし、spirv-valで検証しました。
Native回帰テスト724項目も成功しました。

### 外部同期とImmutable Sampler

Sparseまでを含むCommit `8b96ae8832bb80512954ccdac762fff12e98fd37`の[Android CI](https://github.com/Moriya-Taichi/Vulkano/actions/runs/34226576729)が成功しました。
その後、外部SYNC_FDとImmutable Samplerを追加しました。
ローカルではNative 725項目、Kotlin/JNI 44件中35件成功・9件スキップ・失敗0件です。
Immutable SamplerはCombinedとSeparateの両方で、Samplerを閉じた後のGPU描画と読み戻しを確認しました。
この環境にSYNC_FD対応がないため、GPU Signal・Export・Import・Waitの成功経路はスキップしています。
Feature未有効時の拒否と完了済みFDの所有権操作は実行しました。


### HardwareBufferとYCbCr

外部同期までを含むCommit `979cc07ca412c09d39b97e13afe44f11f84fff29`の[Android CI](https://github.com/Moriya-Taichi/Vulkano/actions/runs/34228471696)が成功しました。
HardwareBufferのImport、Feature拒否、所有権検査、RGB描画・読み戻し、外部FormatとImmutable SamplerによるSamplingのテストを追加しました。
ローカルではKotlin/JNI 47件中36件成功・11件スキップ・失敗0件です。
追加の2件はAndroid専用のため、ローカルでは実行していません。
YUVのCamera/Codec画像、外部所有権BarrierのDriver上での挙動はAndroid実機での検証が必要です。


### GPU側Count Bufferによる間接描画

HardwareBufferまでを含むCommit `ecd71b3ecd0e033b194f9bae0eb9083dec60c2b4`の[Android CI](https://github.com/Moriya-Taichi/Vulkano/actions/runs/34230869015)が成功しました。
GPUがPrivate Bufferへ書いた引数・描画数を使い、通常・Indexed・Mesh Drawを検証するテストを追加しました。
ローカルDriverはMesh Shaderにも対応しており、3種類ともGPU出力の照合に成功しました。
0件、上限でのClamp、Count Offset、Stride、Buffer範囲とFeature拒否を確認しています。
ローカルではNative 725項目、Kotlin/JNI 49件中38件成功・11件スキップ・失敗0件です。
Validation LayerのエラーとSync Hazardは検出されませんでした。


### ASTC HDRとPVRTC

GPU側Count Bufferまでを含むCommit `b78e7c66326a8c18d24034ad0badd87398abb165`の[Android CI](https://github.com/Moriya-Taichi/Vulkano/actions/runs/34232325538)が成功しました。
ASTC HDRとPVRTCのFeature拒否、HDR Sampling、PVRTC Block転送のテストを追加しました。
ASTC HDRのFixtureはFloat16の定数色Blockで、赤成分2.0をFloat Render Targetへ保持できるかを検証します。
ローカルDriverは両方の圧縮Featureに非対応で、成功経路の2件はスキップしています。
Kotlin/JNIは52件中39件成功・13件スキップ・失敗0件です。


### Device Generated Commands

Vulkan-Headers 1.4.335導入までのCommit `8b3ecfb6225dc3f58bf5724e60699f47fd5bc58d`は[Android CI](https://github.com/Moriya-Taichi/Vulkano/actions/runs/34234101369)が成功しました。
新しいToken Layout、Pipeline選択、GPU引数/Count、Compute・Indexed/Mesh描画、PipelineとBufferの保持を検証するテストを追加しました。
Mesa 24.0.5のローカル環境ではNative 725項目、Kotlin/JNI 55件中40件成功・15件スキップ・失敗0件です。
このDriverにはDevice Generated Commandsがないため、追加した成功経路2件はスキップしています。
CIのMesa 25.2.8でもFeatureに応じて実行し、実行/スキップの内訳をJUnitレポートから検査します。


### Tile Shadingと検証レイヤーの更新

Vulkan-ValidationLayersをヘッダーと同じ1.4.335に固定しました。
Commit `d4438da`のCIではKotlin/JNI 55件中47件が実行に成功し、Device Generated Commands、Ray Tracing、Sparseの成功経路も動作しました。
ただしJNIの標準出力に4件のValidation Errorがあり、この結果を検証完了とは扱いません。
必要なMaintenance5の有効化、頂点入力がないPipelineのDynamic Stride、Generated MeshのTask Featureを修正しました。
JUnitのXMLだけではJNIの標準出力を取り込めないため、Gradle全体のログも検査するようにしました。

Tile AttachmentのSPIR-V ReflectionをNativeで検査し、Tile機能の拒否、通常/間接Tile Dispatch、Area Dispatch、Fragment Tile Readのテストを追加しました。
ローカルではNative 740項目、Kotlin/JNI 59件中41件成功・18件スキップ・失敗0件です。
Tile Shadingに非対応のDriverなので、TileのGPU実行3件はスキップしています。
TileのSPIR-VはCIで同じ固定版のSPIRV-Toolsを使って検証します。

Commit `10c8d10`の[Android CI](https://github.com/Moriya-Taichi/Vulkano/actions/runs/34243153938)は成功しました。
Native 740項目、Kotlin/JNI 59件中48件成功・11件スキップ・失敗0件です。
Device Generated Commands、Ray Tracing、Sparseの成功経路が動作し、Gradle全体のログにもValidation ErrorとSync Hazardはありませんでした。
3種類のTile Shaderも固定版のSPIRV-Toolsで検証に成功しました。
TileのGPU実行は対応Driverがないためスキップしています。

### 独立Queue

Queue選択、単一Queueの順序維持、QueueごとのCommand Pool、複数FamilyでのResource共有とTimeline依存関係を追加しました。
Queue間でのTexture転送、Counterの保持、Pool再利用、連続Signalの順序を検証するテストを用意しています。
ローカルではNative 740項目、Kotlin/JNI 63件中42件成功・21件スキップ・失敗0件です。
このDriverには独立Queueがないため、追加したQueue間実行の3件はスキップしています。
