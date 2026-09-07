package dev.vulkano.sample

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.FrameLayout
import android.widget.TextView
import dev.vulkano.*
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch

/** All GPU work belongs to one worker; surface teardown drains it before returning. */
class MainActivity : Activity(), SurfaceHolder.Callback {
    private val workerThread = HandlerThread("Vulkano renderer")
    private lateinit var worker: Handler
    private lateinit var status: TextView
    private lateinit var surfaceView: SurfaceView
    private var device: Device? = null
    private var layer: SurfaceLayer? = null
    private var queue: CommandQueue? = null
    private var pipeline: RenderPipelineState? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        workerThread.start()
        worker = Handler(workerThread.looper)
        val view = SurfaceView(this).apply { holder.addCallback(this@MainActivity) }
        surfaceView = view
        status = TextView(this).apply {
            text = "Initializing Vulkan…"
            setTextColor(0xffffffff.toInt())
            setBackgroundColor(0x99000000.toInt())
            setPadding(24, 48, 24, 24)
        }
        setContentView(FrameLayout(this).apply {
            addView(view)
            addView(status, FrameLayout.LayoutParams(-1, -2))
        })
    }
    override fun surfaceCreated(holder: SurfaceHolder) = Unit
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (width <= 0 || height <= 0) return
        worker.post {
            try {
                if (device == null) {
                    val gpu = Device.create()
                    device = gpu
                    queue = gpu.makeCommandQueue()
                    val computeResult = compute(gpu)
                    runOnUiThread { status.text = "${gpu.capabilities.name}\nCompute: [1, 2, 3, 4] × 2 = $computeResult" }
                }
                val gpu = checkNotNull(device)
                val target = layer?.also { it.resize(width, height) }
                    ?: gpu.makeSurfaceLayer(holder.surface, width, height).also { layer = it }
                pipeline?.close()
                pipeline = gpu.makeRenderPipelineState(
                    vertexFunction = gpu.makeLibrary(assets.open("shaders/triangle.vert.spv").use { it.readBytes() }).makeFunction(),
                    fragmentFunction = gpu.makeLibrary(assets.open("shaders/triangle.frag.spv").use { it.readBytes() }).makeFunction(),
                    colorFormat = target.pixelFormat,
                )
                renderFrame()
            } catch (error: Exception) {
                releaseGpu()
                runOnUiThread { status.text = "Vulkan unavailable: ${error.message}" }
            } catch (error: UnsatisfiedLinkError) {
                releaseGpu()
                runOnUiThread { status.text = "Vulkan loader unavailable: ${error.message}" }
            }
        }
    }
    private fun renderFrame() {
        try {
            val target = layer ?: return
            val drawable = target.nextDrawable()
            if (drawable == null) {
                worker.postDelayed({ renderFrame() }, 16)
                return
            }
            // A static sample redraws on surface changes and retries acquisition when needed.
            drawable.use {
                checkNotNull(queue).makeCommandBuffer().use { command ->
                    command.render(RenderPassDescriptor(ColorAttachment(it.texture, clearColor = ClearColor(0.035f, 0.045f, 0.07f)))) {
                        setRenderPipelineState(checkNotNull(pipeline))
                        drawPrimitives(vertexCount = 3)
                    }
                    command.present(it)
                    command.commit()
                    command.waitUntilCompleted()
                }
            }
        } catch (error: Exception) {
            releaseGpu()
            runOnUiThread { status.text = "Vulkan rendering failed: ${error.message}" }
        }
    }
    private fun compute(gpu: Device): String {
        val function = gpu.makeLibrary(assets.open("shaders/double.comp.spv").use { it.readBytes() }).makeFunction()
        gpu.makeComputePipelineState(function, listOf(BindingLayout(0, BindingType.STORAGE_BUFFER)), pushConstantBytes = 4).use { compute ->
            gpu.makeBuffer(16).use { buffer ->
                val data = ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder())
                data.asFloatBuffer().put(floatArrayOf(1f, 2f, 3f, 4f))
                buffer.write(data)
                checkNotNull(queue).makeCommandBuffer().use { command ->
                    command.compute {
                        setComputePipelineState(compute)
                        setBuffer(buffer, index = 0)
                        setBytes(ByteBuffer.allocate(4).order(ByteOrder.nativeOrder()).putInt(4).array())
                        dispatchThreads(Size(4))
                    }
                    command.commit()
                    command.waitUntilCompleted()
                }
                buffer.read(data)
                return List(4) { data.getFloat(it * 4) }.toString()
            }
        }
    }
    private fun releaseGpu() {
        pipeline?.close(); pipeline = null
        layer?.close(); layer = null
        queue?.close(); queue = null
        device?.close(); device = null
    }
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        drainGpu()
    }
    private fun drainGpu() {
        val drained = CountDownLatch(1)
        // SurfaceHolder requires the render thread to stop using the Surface before return.
        if (worker.post { try { releaseGpu() } finally { drained.countDown() } }) drained.await()
    }
    override fun onDestroy() {
        surfaceView.holder.removeCallback(this)
        drainGpu()
        workerThread.quitSafely()
        super.onDestroy()
    }
}
