# メモリと同期

VulkanoはKotlinのResourceをNativeの参照カウント付きResourceへ接続します。公開Handleは単調増加する識別子であり、メモリアドレスではありません。型・Deviceの一致・解放済みHandleをJNI側でも検査します。

## CPUとGPUのアクセス

Shared BufferへのCPUアクセスは`write`と`read`を使用します。Direct ByteBufferとVulkanメモリの間でコピーするため、ByteBufferを保持しても解放済みGPUメモリへのポインタは残りません。ByteBuffer自体へのアクセスを別のCPUスレッドと競合させない責任は呼び出し側にあります。

GPUがBufferを使うCommand Bufferを送信すると、Native側で使用中として扱います。CPUからの読み書き時にはFenceを確認し、完了していなければ例外にします。タイムアウトした`waitUntilCompleted()`は完了を意味しません。完了が確認されるまでResourceは保持されます。

VMAは必要に応じてCPUキャッシュをFlush・Invalidateし、Non-coherent Atom Sizeの整列を処理します。HOST_COHERENTな端末でも、処理の完了待ちやGPU内の依存関係は省略しません。[Vulkanの同期仕様](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)

## コマンドの送信順

EncoderはNativeに操作とResource参照を記録します。実際のVkCommandBufferは`commit()`時に作り、送信順にTextureのLayoutを確定します。先に記録したCommand Bufferを後から送信しても、過去のLayoutを前提にしたBarrierは生成しません。

Compute Dispatch、Blit、Render Passの間にメモリ依存関係を入れます。GraphicsのStorage書き込みは対応Featureを要求します。
Draw間に一般のStorageの読み書き依存がある場合はRender Passを分けてください。
Attachmentの読み取りはSubpassとInput Attachmentで指定でき、BY_REGIONの依存関係を入れます。書き込むAttachment Aspectを同一Pass内でSamplingするFeedback Loopは禁止します。
Depth/Stencilの全Aspectを読み取り専用のLOAD / NONEにした場合、対応するViewを指定してSamplingできます。Textureは必要に応じてTransfer、General、Attachment、PresentationのLayoutへ移行します。

未初期化TextureのSampling・Readback・Attachment Loadは拒否します。Storage ImageへのDispatchは書き込みを含む操作として扱いますが、画像の全画素を初期化することを保証しません。Storage Imageの読み出し範囲やBufferの初期化、シェーダー内部のデータ競合・境界検査はシェーダーの責任です。

Layoutの変更はQueueへの送信が成功してから公開します。送信前に失敗したCommand Bufferは`FAILED`になり、Textureの状態は更新しません。送信後のPresentationエラーではGPUへの送信自体は成立しているため、Command BufferはGPU完了まで保持されます。

## 所有権と終了

`commit()`は一度だけ呼び出せます。記録したBufferやPipelineを先に`close()`しても、NativeのCommand BufferがGPU処理を終えるまで保持します。Command Buffer自体を送信後に`close()`すると完了を待ちます。

`Device.close()`は子Resourceを終了し、最後にDeviceを破棄します。`use`で例外経路も解放してください。長期間Deviceを保持するアプリでは、不要になったCommand BufferやResourceをその都度`close()`します。GCによる自動解放はありません。

`compute {}`、`render {}`、`blit {}`のブロックが例外で終了した場合、そのCommand Bufferを閉じます。閉じたEncoder、別DeviceのResource、Encoderが開いたままの`commit()`は受け付けません。

同じDeviceのAPI操作は直列化します。
JNI内でもVulkanが求める外部同期を行います。
引数なしの`makeCommandQueue()`は、同じ順序付きのVulkan Queueを使用します。

## 独立Queue

`INDEPENDENT_QUEUES`を有効にすると、`Device.commandQueues`から実際に作成されたQueueを選べます。
`makeCommandQueue(index)`の異なるIndexは異なるVulkan Queueを使い、並行実行が可能です。
同じIndexのCommandは送信順に実行します。
Queueの数やGraphics・Compute・Transferへの対応は端末に問い合わせます。
独立Queueがない端末ではこのFeatureを要求せず、Index 0を使います。

```kotlin
val device = Device.create(
    requiredFeatures = setOf(Feature.INDEPENDENT_QUEUES, Feature.TIMELINE_SEMAPHORE),
)
val transfer = device.commandQueues.first { it.supportsTransfer && it.index != 0 }
val event = device.makeSharedEvent()
val upload = device.makeCommandQueue(transfer.index).makeCommandBuffer()
upload.blit { copy(staging, input) }
upload.signalEventOnCompletion(event, 1)
upload.commit()

val compute = device.makeCommandQueue().makeCommandBuffer()
compute.waitForEvent(event, 1)
compute.compute {
    setComputePipelineState(pipeline)
    setBuffer(input, 0)
    setBuffer(output, 1)
    dispatchThreadgroups(Size(1))
}
compute.commit()
compute.waitUntilCompleted()
upload.close()
compute.close()
```

Queueをまたぐ読み書き・Texture Layout変更・Alias切り替えにはSharedEventで依存関係を指定します。
書き込み元のCommandを先に`commit()`してから、待機するCommandを送信してください。
同じEventを繰り返しSignalする場合、値の増加順に完了するよう前回のSignalを待ちます。
並行に動かしたい処理には別々のEventを用意してください。

複数のQueue Familyを作成したDeviceでは、BufferとTextureをそれらのFamilyで共有できるように割り当てます。
この共有設定は単一Familyを使う場合には適用しません。
HardwareBufferを独立Queueと併用する場合は、外部所有権の移譲に必要な`SYNCHRONIZATION_2`も有効にします。
転送専用QueueではTexture領域の粒度制限を検査し、Depth/StencilのBuffer転送とMip生成にはGraphics Queueを使います。
Counterは`makeCounterSampleBuffer(queueIndex = index)`で使用先のQueueに割り当てます。
`Device.waitUntilIdle()`はすべてのQueueの送信済みCommandを待ちます。

## Surfaceの寿命

SurfaceLayerごとにDrawableを1枚だけ取得できます。TextureをRender Passで使うCommand Bufferに、そのDrawableの`present()`も記録してください。`nextDrawable()`が`null`を返したら、次のフレームで再試行します。

提示済みDrawableのTextureは再利用できません。取得後に提示せず破棄した場合は、Acquire完了後にNativeの同期オブジェクトを解放し、次回にSwapchainを作り直します。
解放待ちのNative HandleはDeviceが保持し、未完了のCommandがある間はQueue全体を待って破棄しません。
`nextDrawable()`はDevice/JNIのLockを解放しながら待つため、別ThreadからShared EventをSignalできます。画像の解放用拡張やPresentation Fence拡張を必須にしないため、Swapchainの再利用・破棄前にはQueueの完了を待つ設計です。

サイズ変更前に取得中のDrawableを提示または破棄してください。Androidの`surfaceDestroyed`が返る前に、そのSurfaceを使う作業を終えます。DeviceをUIの各フレームで作り直す必要はありません。

現在はCompositorに画面回転を任せます。ShaderでのPre-rotation、複数フレームの同時進行、フレーム間のDescriptor Pool再利用、より狭いBarrierへの最適化は、実機での性能確認後に進める対象です。

DescriptorのCommand内再利用、Shared Bufferの常時マッピング、画像Layoutの選択は[モバイルGPU最適化](mobile-gpu-optimization.md)を参照してください。

## HeapとShared Event

`makeHeap()`は容量を固定したVMA Poolを作成します。
Heapから作ったBufferとTextureはHeapのNative参照を保持し、GPU処理が終わるまで割り当てを解放しません。
Shared HeapはBuffer用、Private Heapは互換メモリ種別のBufferとTexture用です。
Placement Heapは`makePlacementHeap`で作成し、Resourceの配置を`offset`で指定します。
重なるResourceの切り替えには`aliasResources`を使い、切り替え後のTextureを全面的に初期化してください。
Sparse ResourceのPage/Tile/Mip Tailは独立したMapping APIで管理します。

`makeSharedEvent()`はTimeline Semaphoreを使います。
CommandのWaitは最初のGPU操作より前に記録し、SignalはCommand全体の完了時に発生します。
HostのSignal値は現在値を増やし、送信済みGPU Signalの値より小さくする必要があります。
満たされないWaitを残したままDeviceを閉じると、完了待ちは終了しません。
同じVulkan Queue内の将来のCommandだけにSignalを依存させると循環待ちになるため、CPUなど実行可能なSignal元を用意します。
