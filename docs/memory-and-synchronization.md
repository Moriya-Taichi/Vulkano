# メモリと同期

VulkanoはKotlinのResourceをNativeの参照カウント付きResourceへ接続します。公開Handleは単調増加する識別子であり、メモリアドレスではありません。型・Deviceの一致・解放済みHandleをJNI側でも検査します。

## CPUとGPUのアクセス

Shared BufferへのCPUアクセスは`write`と`read`を使用します。Direct ByteBufferとVulkanメモリの間でコピーするため、ByteBufferを保持しても解放済みGPUメモリへのポインタは残りません。ByteBuffer自体へのアクセスを別のCPUスレッドと競合させない責任は呼び出し側にあります。

GPUがBufferを使うCommand Bufferを送信すると、Native側で使用中として扱います。CPUからの読み書き時にはFenceを確認し、完了していなければ例外にします。タイムアウトした`waitUntilCompleted()`は完了を意味しません。完了が確認されるまでResourceは保持されます。

VMAは必要に応じてCPUキャッシュをFlush・Invalidateし、Non-coherent Atom Sizeの整列を処理します。HOST_COHERENTな端末でも、処理の完了待ちやGPU内の依存関係は省略しません。[Vulkanの同期仕様](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)

## コマンドの送信順

EncoderはNativeに操作とResource参照を記録します。実際のVkCommandBufferは`commit()`時に作り、送信順にTextureのLayoutを確定します。先に記録したCommand Bufferを後から送信しても、過去のLayoutを前提にしたBarrierは生成しません。

Compute Dispatch、Blit、Render Passの間にメモリ依存関係を入れます。Render Pass内のシェーダーResourceは読み取り専用です。Attachmentと同じTextureを同一Pass内でSamplingするFeedback Loopは禁止します。Textureは必要に応じてTransfer、General、Attachment、PresentationのLayoutへ移行します。

未初期化TextureのSampling・Readback・Attachment Loadは拒否します。Storage ImageへのDispatchは書き込みを含む操作として扱いますが、画像の全画素を初期化することを保証しません。Storage Imageの読み出し範囲やBufferの初期化、シェーダー内部のデータ競合・境界検査はシェーダーの責任です。

Layoutの変更はQueueへの送信が成功してから公開します。送信前に失敗したCommand Bufferは`FAILED`になり、Textureの状態は更新しません。送信後のPresentationエラーではGPUへの送信自体は成立しているため、Command BufferはGPU完了まで保持されます。

## 所有権と終了

`commit()`は一度だけ呼び出せます。記録したBufferやPipelineを先に`close()`しても、NativeのCommand BufferがGPU処理を終えるまで保持します。Command Buffer自体を送信後に`close()`すると完了を待ちます。

`Device.close()`は子Resourceを終了し、最後にDeviceを破棄します。`use`で例外経路も解放してください。長期間Deviceを保持するアプリでは、不要になったCommand BufferやResourceをその都度`close()`します。GCによる自動解放はありません。

`compute {}`、`render {}`、`blit {}`のブロックが例外で終了した場合、そのCommand Bufferを閉じます。閉じたEncoder、別DeviceのResource、Encoderが開いたままの`commit()`は受け付けません。

同じDeviceのAPI操作は直列化します。JNI内でもVulkanが求める外部同期を行います。複数のCommandQueueを作っても、同じVulkan Queueへ順に送信されます。

## Surfaceの寿命

SurfaceLayerごとにDrawableを1枚だけ取得できます。TextureをRender Passで使うCommand Bufferに、そのDrawableの`present()`も記録してください。`nextDrawable()`が`null`を返したら、次のフレームで再試行します。

提示済みDrawableのTextureは再利用できません。取得後に提示せず破棄した場合は、Acquireの完了を待ち、次回にSwapchainを作り直します。画像の解放用拡張やPresentation Fence拡張を必須にしないため、Swapchainの再利用・破棄前にはQueueの完了を待つ設計です。

サイズ変更前に取得中のDrawableを提示または破棄してください。Androidの`surfaceDestroyed`が返る前に、そのSurfaceを使う作業を終えます。DeviceをUIの各フレームで作り直す必要はありません。

現在はCompositorに画面回転を任せます。ShaderでのPre-rotation、複数フレームの同時進行、フレーム間のDescriptor Pool再利用、より狭いBarrierへの最適化は、実機での性能確認後に進める対象です。

DescriptorのCommand内再利用、Shared Bufferの常時マッピング、画像Layoutの選択は[モバイルGPU最適化](mobile-gpu-optimization.md)を参照してください。
