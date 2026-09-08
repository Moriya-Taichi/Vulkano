package dev.vulkano

/** Scale, shear, pivot, quaternion rotation, and translation at one point in time. */
class SrtTransform(
    scale: FloatArray = floatArrayOf(1f, 1f, 1f),
    shear: FloatArray = floatArrayOf(0f, 0f, 0f),
    pivot: FloatArray = floatArrayOf(0f, 0f, 0f),
    rotationQuaternion: FloatArray = floatArrayOf(0f, 0f, 0f, 1f),
    translation: FloatArray = floatArrayOf(0f, 0f, 0f),
) {
    internal val packed: FloatArray

    init {
        require(
            scale.size == 3 &&
                shear.size == 3 &&
                pivot.size == 3 &&
                rotationQuaternion.size == 4 &&
                translation.size == 3
        )
        require(
            listOf(scale, shear, pivot, rotationQuaternion, translation).all { values ->
                values.all { it.isFinite() }
            }
        )
        require(scale.all { it != 0f })
        require(kotlin.math.abs(rotationQuaternion.sumOf { it.toDouble() * it } - 1) <= 0.0001) {
            "Quaternion must be normalized"
        }
        packed =
            floatArrayOf(
                scale[0],
                shear[0],
                shear[1],
                pivot[0],
                scale[1],
                shear[2],
                pivot[1],
                scale[2],
                pivot[2],
                rotationQuaternion[0],
                rotationQuaternion[1],
                rotationQuaternion[2],
                rotationQuaternion[3],
                translation[0],
                translation[1],
                translation[2],
            )
    }
}

/** Motion endpoints correspond to Vulkan ray times 0 and 1. */
sealed class AccelerationMotionTransform protected constructor(internal val type: Int) {
    internal abstract val packed: FloatArray

    /** Two row-major 3 x 4 affine matrices, interpolated component by component. */
    class Matrix(start: FloatArray, end: FloatArray) : AccelerationMotionTransform(1) {
        internal override val packed: FloatArray = FloatArray(32)

        init {
            require(
                start.size == 12 &&
                    end.size == 12 &&
                    start.all { it.isFinite() } &&
                    end.all { it.isFinite() }
            )
            start.copyInto(packed)
            end.copyInto(packed, 12)
        }
    }

    class Srt(start: SrtTransform, end: SrtTransform) : AccelerationMotionTransform(2) {
        internal override val packed: FloatArray = start.packed + end.packed
    }
}
