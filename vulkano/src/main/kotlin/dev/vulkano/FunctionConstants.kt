package dev.vulkano

/** Scalar function constants keyed by the SPIR-V constant_id decoration. */
class FunctionConstants {
    private data class Value(val bits: Long, val bytes: Int)

    private val values = sortedMapOf<Int, Value>()

    private fun set(index: Int, bits: Long, bytes: Int) = apply {
        require(index >= 0)
        values[index] = Value(bits, bytes)
    }

    fun setByte(index: Int, value: Byte) = set(index, value.toLong() and 0xff, 1)

    fun setUByte(index: Int, value: UByte) = setByte(index, value.toByte())

    fun setShort(index: Int, value: Short) = set(index, value.toLong() and 0xffff, 2)

    fun setUShort(index: Int, value: UShort) = setShort(index, value.toShort())

    fun setInt(index: Int, value: Int) = set(index, value.toLong() and 0xffffffffL, 4)

    fun setUInt(index: Int, value: UInt) = setInt(index, value.toInt())

    fun setLong(index: Int, value: Long) = set(index, value, 8)

    fun setULong(index: Int, value: ULong) = setLong(index, value.toLong())

    fun setFloat(index: Int, value: Float) = setInt(index, value.toRawBits())

    fun setDouble(index: Int, value: Double) = setLong(index, value.toRawBits())

    fun setBoolean(index: Int, value: Boolean) = setInt(index, if (value) 1 else 0)

    /** IEEE 754 binary16, rounded to nearest with ties to even. */
    fun setHalf(index: Int, value: Float) = setHalfBits(index, halfBits(value))

    /** Supplies the exact IEEE 754 binary16 bit pattern, including NaN payloads. */
    fun setHalfBits(index: Int, bits: Short) = setShort(index, bits)

    internal fun pack(): IntArray =
        values
            .flatMap { (id, v) -> listOf(id, v.bytes, v.bits.toInt(), (v.bits ushr 32).toInt()) }
            .toIntArray()

    private fun halfBits(value: Float): Short {
        val bits = value.toRawBits()
        val sign = (bits ushr 16) and 0x8000
        val exponent = (bits ushr 23) and 0xff
        val fraction = bits and 0x7fffff
        if (exponent == 255) {
            return (sign or 0x7c00 or (if (fraction == 0) 0 else (fraction ushr 13) or 0x200))
                .toShort()
        }
        if (exponent > 142) return (sign or 0x7c00).toShort()
        if (exponent < 102) return sign.toShort()
        val shift: Int
        val significand: Int
        var result: Int
        if (exponent < 113) {
            shift = 126 - exponent
            significand = fraction or 0x800000
            result = significand ushr shift
        } else {
            shift = 13
            significand = fraction
            result = ((exponent - 112) shl 10) or (fraction ushr shift)
        }
        val remainder = significand and ((1 shl shift) - 1)
        val halfway = 1 shl (shift - 1)
        if (remainder > halfway || (remainder == halfway && result and 1 != 0)) result++
        return (sign or result).toShort()
    }
}
