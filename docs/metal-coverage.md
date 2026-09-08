# Metalとの機能対応

VulkanoはAndroidのVulkanを使うKotlin APIです。
対応する機能を追加しており、Metal全体との完全互換はまだ達成していません。
「端末依存」は、FeatureとExtensionを問い合わせ、Device作成時に有効化した場合に利用できることを意味します。
GPUのメーカー名からFeatureの有無を推定しません。

## 実装した機能

| 機能 | Vulkano API | Vulkanでの実装と条件 |
| --- | --- | --- |
| Graphics / Compute | Render / Compute Encoder | Graphics / Compute Pipeline |
| MSAA | sampleCount / resolveTexture | FormatごとのSample Count検査、Color/Depth/Stencil Resolve、Transient Attachment |
| Mip生成 | generateMipmaps | Subresource別BarrierとBlit。BlitとFilterをサポートするFormat |
| Indexed / Instanced Draw | drawIndexedPrimitives | UINT16 / UINT32、Base Vertex、First Instance |
| Indirect Draw / Dispatch | Indirect Bufferを受けるEncoder API | Draw、Indexed Draw、Compute、Mesh。Multi Drawは端末依存 |
| Vertex Descriptor | vertexBuffers / vertexAttributes | Vertex Input、頂点ごとまたはInstanceごとの入力 |
| 複数Render Target | colorAttachments | DeviceのAttachment上限まで。Independent Blendは端末依存 |
| Depth / Stencil | DepthStencilDescriptor | Compare、Mask、Stencil操作、Depth Bias、Depth Bounds |
| Rasterization | RenderPipelineDescriptor | Primitive、Cull、Winding、Wireframe、Clip/Clamp、Sample Mask、Sample Shading |
| Viewport / Scissor | setViewports / setScissorRects | 複数Viewportは端末依存 |
| 可変Shading Rate | fragmentSize / fragmentShadingRates | Pipeline、Primitive、Rate Map Attachment。サイズとSample Countを照合 |
| Subpass / Input Attachment | RenderPassLayout / nextSubpass | Color/Depth/MSAA入力、Color Resolve、Attachment保持、BY_REGION依存関係 |
| Multiview / Layer出力 | viewMask / renderTargetArrayLength | Vertex/Fragment Multiview、配列Attachment、Layer出力Feature |
| Texture | TextureDescriptor | 1D、2D、3D、Array、Cube、Mip、MSAA、Float/Integer/圧縮Format |
| Texture View / Buffer | makeTextureView / makeTextureBuffer | Subresource、互換Format、Swizzle、Usage制限、VkBufferView |
| Sampler | SamplerDescriptor | LOD、Mip Filter、Compare、Anisotropy、Min/Max、Border Color、Immutable Sampler |
| 転送 | Blit Encoder | Buffer、Texture領域、Mip、Slice、Fill |
| Argument Bufferに相当する配列 | BindingLayout / arrayElement | 固定Descriptor Array、端末依存のRuntime Array、BDAによる間接参照 |
| Function Constants | FunctionConstants | 32ビットのSpecialization Constants |
| Tessellation | Control / Evaluation Function | 端末依存。Patch Primitive |
| Object / Mesh Shader | objectFunction / meshShader | VK_EXT_mesh_shader。Task Shaderは別Feature |
| Ray Query | AccelerationStructureとCompute/Graphics Shader | VK_KHR_ray_query、BLAS/TLAS、Triangle/AABB、Build/Refit、Copy/Compaction、SerializationとBLASアドレス再配置 |
| Ray Tracing Pipeline | RayTracingPipelineState / traceRays | Raygen、Miss、Hit、Intersection、Callable、SBT。端末依存 |
| Heap / Placement / Alias | makeHeap / makePlacementHeap / aliasResources | VMA Pool、Memory Type・Size・Alignmentの検査、明示配置、Alias Barrier |
| Sparse Resource | makeSparseBuffer / makeSparseTexture | Page/Tile/Mip TailのMapping、必要なMetadataの確保、Mapping共有、CPU/Shader Residency検査 |
| Event / 外部同期 | makeSharedEvent / makeExternalSemaphore | TimelineとBinary Semaphore、SYNC_FDのImport/Export。WaitはCommand開始前、Signalは完了時 |
| Android画像共有 / YCbCr | importHardwareBuffer / acquireExternalTexture / releaseExternalTexture | AHardwareBufferのMemory Import、RGB/Depth、外部Format変換、Foreign/External所有権移譲 |
| Counter / Visibility | CounterSampleBuffer | Timestamp / Occlusion Query。精密なSample数は端末依存 |
| Binary Archiveに相当するキャッシュ | serializePipelineCache / loadPipelineCache | DeviceとDriverに対応したPipeline Cache |
| SIMD / 数値型 / Atomic | SPIR-V Shader | Subgroup、8/16/64ビット型、64ビット整数Buffer Atomic、32ビットFloat Buffer Atomicは個別Feature |
| Raster Order Groupに相当する排他 | Fragment ShaderのPixel Interlock | VK_EXT_fragment_shader_interlock。Fragment Storage Featureも必要 |
| Tensor / 行列演算 | TensorDescriptor / setTensor | Bufferを使うShape/Stride/View、端末依存のVK_KHR_cooperative_matrix |
| メモリモデル | StorageMode / Vulkan Memory Model | AndroidのShared Memory、Flush/Invalidate、任意のVulkan Memory Model Feature |

## Vulkanに手段があるものの残っている機能

次の項目はVulkan非対応という理由で除外していません。
現在のWrapperでの未実装項目として扱います。

| 機能 | 残る実装 |
| --- | --- |
| Tile Compute | 任意Tile Kernelを実行する専用拡張。Input AttachmentによるPixel内の読み取りは実装済み |
| GPUからのCommand生成 | PipelineやBindingもGPUで指定するDevice Generated Commands。現在はDraw/Dispatch引数の生成 |
| Acceleration StructureのMotion Blur | Motion Blur用拡張、復元済みStructureへのRefit |
| 複数Queue | 独立したQueueとQueue Family Ownership Transfer |
| ML実行 | 専用ML EncoderやGraph実行API。TensorとCompute Shaderによる演算は実装済み |

## 組み合わせの制約

SubpassはColor ResolveとMultiviewに対応します。
Depth/Stencil ResolveとRate Mapは単一Subpassで利用でき、明示的なSubpass Layoutとの組み合わせは未実装です。
Input Attachmentには描画先と同じImage、Mip、Layer、FormatのViewを指定します。
同じSubpassで同じAttachmentへ書き込みながらInput Attachmentとして読む構成は受け付けません。
一般のStorage Resourceについては、Draw間の任意の依存関係を自動で推定しません。

Acceleration Structure Archiveは互換Driver向けの不透明なデータです。
TLASの復元には、保存時のBLASアドレスから復元済みBLASへの対応表が必要です。
Sparse TextureはSingle Sampleの2D/3D Color ImageとそのArray/Cubeに対応します。
Sparse MappingはDeviceの先行Command完了を待ち、Sparse QueueへのBind完了後に戻ります。
未割り当て領域の値は端末のResidency規則に従い、新規に割り当てた領域は使用前に初期化します。
共有Mapping間のCopyには中間Buffer/Textureを使います。

HardwareBufferの外部FormatはSampled用途、単一Layer/Mipの2D画像に対応します。
変換SamplerはPipelineのImmutable Samplerとして指定します。
外部との受け渡しではSemaphoreまたはCPU待機による同期が必要です。
Android 29/30では同じメモリを別のHardwareBuffer Handleから重複してImportしないでください。
Android 31以降はBuffer IDによる重複検査も行います。

Cooperative MatrixはSubgroup ScopeのCompute Shaderに対応し、Shape、数値型、Saturationを端末の組み合わせと照合します。

## APIの直接の対応先がないもの

MSLソースのコンパイル、MetalのDynamic Library形式、XcodeのGPUキャプチャ形式はVulkanのAPIとして提供されません。
VulkanoはSPIR-VとVulkanの開発ツールを使用します。
MetalFXやMPSなどの上位ライブラリのアルゴリズムは、Vulkan APIの機能対応とは別の実装課題です。

比較の基準は[AppleのMetal Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)です。
Vulkan側の条件は[KhronosのFeature管理](https://docs.vulkan.org/guide/latest/features.html)と[Ray Tracingの対応関係](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html)を参照してください。
