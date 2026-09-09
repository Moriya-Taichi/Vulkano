package dev.vulkano

/**
 * Image transfer alignment in texels (compressed formats use blocks). Zero means whole mip levels.
 */
data class TransferGranularity(val width: Int, val height: Int, val depth: Int) {
    val requiresWholeMipLevel: Boolean
        get() = width == 0 && height == 0 && depth == 0
}

/** Distinct indices identify distinct native queues, which can execute independently. */
data class CommandQueueCapabilities(
    val index: Int,
    val familyIndex: Int,
    val indexInFamily: Int,
    val supportsRendering: Boolean,
    val supportsCompute: Boolean,
    val supportsTransfer: Boolean,
    val timestampValidBits: Int,
    val imageTransferGranularity: TransferGranularity,
    val supportsMachineLearning: Boolean = false,
)
