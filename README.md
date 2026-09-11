# Vulkano

English | [日本語](README.ja.md)

Vulkano is a Kotlin GPU library for Vulkan on Android. Its Metal-style `Device`,
`CommandQueue`, `CommandBuffer`, and encoder APIs let you write graphics and compute
workloads without managing Vulkan objects directly.

The library manages memory allocation, resource bindings, image layout transitions,
and synchronization between commands. Your application supplies SPIR-V shaders and
the data they process. Vulkano does not compile Metal Shading Language or provide
complete Metal API compatibility.

## What you can do

| Task | Features |
| --- | --- |
| GPU compute | Process buffers, textures, and tensors; read results on the CPU; use cooperative matrices on supported devices |
| Machine learning | Dedicated tensors, SPIR-V graphs, weights, function constants, graph sequencing, and cache restoration on supported devices |
| Rendering | MSAA, indexed and indirect draws, GPU-generated commands, vertex attributes, MRT, depth/stencil, subpasses, multiview, shading rates, tessellation, mesh shaders, and tile compute |
| Data transfer | Copy buffer and texture regions, individual depth/stencil aspects, mip levels, and array slices; generate mipmaps |
| Ray tracing | BLAS/TLAS build, refit, copy, compaction, serialization, ray queries, ray tracing pipelines, and motion blur on supported devices |
| Resource management | Texture views, resource arrays, heaps, placement and aliasing, sparse resources, independent queues, shared events, counters, and pipeline caches |
| Device adaptation | Query features, texture formats, workload limits, and memory information |

Requires Android 10 (API 29) or newer, `arm64-v8a` or `x86_64`, and Vulkan 1.1 with
graphics and compute support. `Device.create()` checks these requirements.
Optional features depend on the device and driver, not just the Android version or
GPU manufacturer.

## Installation

The current version is `0.1.0-SNAPSHOT`. It is not yet published to Maven Central.
Build from source and publish to your local Maven repository. Use JDK 17, Android
SDK 35, NDK 28.1.13356709, and CMake 3.22.1; the Gradle wrapper is included.

```sh
git clone https://github.com/Moriya-Taichi/Vulkano.git
cd Vulkano
./gradlew :vulkano:publishReleasePublicationToMavenLocal
```

Add `mavenLocal()` to your application's `settings.gradle.kts`:

```kotlin
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        mavenLocal()
    }
}
```

Set `minSdk` to at least 29 and add the dependency to the app module:

```kotlin
dependencies {
    implementation("io.github.moriya-taichi:vulkano:0.1.0-SNAPSHOT")
}
```

## Usage

### Process an array on the GPU

Save this shader as `double.comp`. It doubles four floating-point values.
`dispatchThreads` rounds up to whole workgroups, so the shader checks the array
bounds before accessing memory.

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

Compile with the NDK's `glslc` and place the output in
`app/src/main/assets/shaders/double.comp.spv`:

```sh
glslc --target-env=vulkan1.1 double.comp -o double.comp.spv
```

Run the following Kotlin code on a worker thread. `assets` is an Android
`AssetManager`.

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

`compute {}` records operations and `commit()` submits them asynchronously.
Wait for completion before reading the buffer on the CPU. The library reflects
bindings, push constants, and workgroup sizes from the shader. `setBytes` supplies
the element count in this example.

### Render to the screen

Create a `SurfaceLayer` from a valid Android `Surface`, then render into an acquired
drawable. This example assumes an existing device, queue, and vertex/fragment
functions. See the [sample application](sample/src/main/kotlin/dev/vulkano/sample/MainActivity.kt)
for shader loading and Android Surface lifecycle handling.

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

Create the layer and pipeline during initialization and acquire a drawable for each
frame. If `nextDrawable()` returns `null`, retry on a later frame. Call
`layer.resize()` when the Surface size changes. Finish GPU work using the Surface
before `surfaceDestroyed` returns.

### Choose memory and release resources

| Storage mode | Use |
| --- | --- |
| `SHARED` | Buffers updated or read by the CPU; the default for `makeBuffer()` |
| `PRIVATE` | GPU buffers and textures; transfer CPU data through a shared staging buffer |
| `MEMORYLESS` | Attachments used only within one render pass, such as temporary depth |

Use `makeUploadBuffer()` for CPU-write-only uploads. Wait for GPU completion before
accessing an in-use buffer from the CPU. Shared physical memory on Android does not
remove the need for synchronization or cache maintenance.

Release resources with `use` or `close()`. `Device.close()` also releases remaining
child resources. Applications that keep a device alive should close unused resources
as they go. Closing a submitted command buffer waits for GPU completion.

## Documentation and support

MSAA, mipmap generation, and indexed drawing are available through the basic APIs.
Request optional features such as ray tracing or mesh shaders when creating the
device. The following detailed guides are currently in Japanese:

- [Advanced examples](docs/advanced-features.md)
- [API guide and limitations](docs/api-guide.md)
- [Metal feature coverage](docs/metal-coverage.md)
- [Memory and synchronization](docs/memory-and-synchronization.md)
- [Mali, PowerVR, Adreno, and Xclipse optimizations](docs/mobile-gpu-optimization.md)
- [Building and testing](docs/development.md)
- [Publishing](docs/publishing.md)

See [validation results](docs/validation-results.md) for completed checks and
[Android device validation](docs/android-validation.md) for hardware test procedures.
GPU-specific performance, thermal behavior, and power consumption have not been
measured on physical devices.

## License

Vulkano is licensed under the [Apache License 2.0](LICENSE).
Copyright 2026 Moriya-Taichi.

Bundled VMA, SPIRV-Reflect, SPIR-V Headers, and Vulkan-Headers retain their respective
licenses. Notices are preserved in [third_party](vulkano/src/main/cpp/third_party/)
and included in the distributed AAR and sources JAR.
