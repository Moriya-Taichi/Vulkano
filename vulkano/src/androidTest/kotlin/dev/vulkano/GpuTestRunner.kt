package dev.vulkano

import android.os.Bundle
import androidx.test.runner.AndroidJUnitRunner

/** Runs the same Kotlin/JNI graphics and compute suite on Android hardware. */
class GpuTestRunner : AndroidJUnitRunner() {
    override fun onCreate(arguments: Bundle?) {
        System.setProperty("vulkano.runNativeTests", "true")
        TestShaders.read = { name -> context.assets.open(name).use { it.readBytes() } }
        super.onCreate(arguments)
    }
}
