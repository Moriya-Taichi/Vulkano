# Mali・PowerVR・Adreno向けの最適化

AndroidのモバイルGPUを対象に、CPU側のVulkanオブジェクト作成とDescriptor更新を減らします。GPU名でFeatureやメモリ特性を決め打ちせず、Vulkan 1.1で利用できる経路を共通実装しています。

## 実装

| 対象 | 処理 |
| --- | --- |
| Descriptor Pool | Command内でPipelineごとに64セット単位で確保。個別解放フラグを使わず、Command終了後にまとめて解放 |
| Descriptor Set | Pipeline、Binding番号、Buffer/Texture/Sampler、Offset、Rangeが同じなら更新済みのSetを再利用。Bindingの指定順には依存しない |
| Pipeline | Deviceごとの`VkPipelineCache`をCompute/Graphics両方で使用。同一Pipelineの連続Bindを省略 |
| Render Pass | FormatとLoad/Storeの組み合わせでDevice内にキャッシュ。FramebufferやAttachmentの寿命とは分離 |
| Shared Buffer | VMAで常時マッピング。小さなread/writeのたびにVulkanメモリをMap/Unmapしない。Flush/InvalidateとGPU使用中のCPUアクセス検査は継続 |
| Sampled Texture | Storage用途がない画像は`SHADER_READ_ONLY_OPTIMAL`。Storage用途もある画像は同時BindingのAliasに対応するため`GENERAL` |
| Image Barrier | 現在と同じLayoutへのBarrierを省略。各操作の前にあるMemory Barrierが依存関係を保証し、Layoutが変わる場合はImage Barrierを発行 |

Descriptorは一度更新した後は変更しません。後続DrawのBinding変更が先行Drawを壊さないよう、異なる内容には別のSetを割り当てます。Push Constantsはキャッシュキーに含めず毎回設定します。Commandに記録したリソースをGPU完了まで保持するため、キャッシュキーに含むオブジェクトのアドレスが途中で再利用されることもありません。

Render PassのキャッシュはLoad/Storeを区別するので、CLEARとLOAD、STOREとDONT_CAREの意味は保持します。Memoryless AttachmentのTransient usage、Lazy memory優先、DONT_CARE Storeは引き続き使用します。PrivateはGPU用途のメモリ選択であり、独立したVRAMやCPUとのCoherencyを仮定しません。

## 各GPUで重視する点

- **Mali:** DrawごとのDescriptor更新とPool作成を削減します。[ArmのDescriptor管理資料](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-descriptor-and-buffer-management)に沿い、個別Set解放を避けます。タイル内で完結するDepthはMemorylessにし、同じAttachmentへのDrawを一つのRender Encoderにまとめてください。
- **PowerVR:** メモリの細かな確保は既存のVMAサブアロケーションで処理します。[Imaginationのメモリ推奨](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/available-memory-types-in-vulkan.html)を踏まえ、今回Shared Bufferのマッピングを常時保持します。小さな可変パラメータは既存の`setBytes`によるPush Constantsを利用できます。
- **Adreno:** [Qualcommの推奨](https://docs.qualcomm.com/doc/80-78185-2/topic/mobile_best_practices.html)を踏まえ、画像の用途に適したLayoutと連続Pipeline Bindの省略を適用します。Texture usageには必要な用途だけを指定してください。Storage usageを不要に付けるとGENERAL経路を選びます。

シェーダーのWorkgroupサイズ、Float16、Subgroup幅は世代や処理に依存します。ベンダー単位で固定したり、精度を自動的に落としたりしません。端末のFeature/Limitsを確認し、Float16は必要な場合だけ明示的に有効化してください。

## 検証と適用範囲

Native回帰テストには、65種類のBindingを使う130 Dispatch、同じ画像を読む100 Draw、BufferのOffset/Range変更、Push Constants変更、Render Pass再利用、実際のGPU結果のReadbackを含めています。前者ではPool作成が130回から2回、Descriptor更新が130回から65回に、後者ではDescriptor更新が100回から1回になります。これはAPI呼び出し回数の削減であり、実機の実行時間や消費電力の改善率ではありません。

DescriptorキャッシュはCommand内、Pipeline/Render PassキャッシュはDevice内で有効です。ディスクへのPipeline Cache保存、Command PoolやFramebufferのフレーム間再利用は未実装です。各操作のMemory BarrierとSurfaceのQueue idle待機は保守的なままなので、独立処理の重なりや複数フレームの同時実行には制限があります。

Mali・PowerVR・Adrenoの実機では未計測です。[Android検証手順](android-validation.md)に加え、同じシーン・解像度・ReleaseビルドでCPUフレーム時間、GPU時間、外部メモリ帯域、温度を比較してください。Validation有効時の時間を性能比較には使用しないでください。GPUカウンターは[Androidの対応資料](https://developer.android.com/agi/sys-trace/counters)と各ベンダーのProfilerで確認できます。
