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
