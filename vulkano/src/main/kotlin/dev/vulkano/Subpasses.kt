package dev.vulkano

/** Input indices address colorFormats followed by the optional depth attachment. */
data class RenderSubpass(
    val colorAttachments: List<Int>,
    val inputAttachments: List<Int> = emptyList(),
    val usesDepthAttachment: Boolean = false,
)

/** The same layout is supplied to every pipeline and the render pass that uses it. */
data class RenderPassLayout(
    val colorFormats: List<PixelFormat>,
    val subpasses: List<RenderSubpass>,
    val depthFormat: PixelFormat? = null,
    val sampleCount: Int = 1,
    val resolveColorAttachments: Set<Int> = emptySet(),
) {
    init {
        require(subpasses.isNotEmpty() && (colorFormats.isNotEmpty() || depthFormat != null))
        require(colorFormats.none { it.isDepth || it.isStencil })
        require(depthFormat == null || depthFormat.isDepth || depthFormat.isStencil)
        require(sampleCount in listOf(1, 2, 4, 8, 16, 32, 64))
        require(resolveColorAttachments.all { it in colorFormats.indices })
        require(resolveColorAttachments.isEmpty() || sampleCount > 1)
        val count =
            colorFormats.size + (if (depthFormat == null) 0 else 1) + resolveColorAttachments.size
        subpasses.forEach { sub ->
            require(sub.colorAttachments.distinct().size == sub.colorAttachments.size)
            require(sub.colorAttachments.all { it in colorFormats.indices })
            require(sub.inputAttachments.all { it in 0 until count && it !in sub.colorAttachments })
            require(
                !sub.usesDepthAttachment ||
                    (depthFormat != null && colorFormats.size !in sub.inputAttachments)
            )
        }
    }

    /** Resolve inputs follow the color attachments and optional depth attachment. */
    fun resolveAttachmentIndex(colorAttachment: Int): Int {
        require(colorAttachment in resolveColorAttachments)
        return colorFormats.size +
            (if (depthFormat == null) 0 else 1) +
            resolveColorAttachments.sorted().indexOf(colorAttachment)
    }

    internal fun pack(): IntArray =
        (listOf(colorFormats.size, depthFormat?.vk ?: 0, sampleCount, subpasses.size) +
                colorFormats.map { it.vk } +
                subpasses.flatMap { sub ->
                    listOf(
                        sub.colorAttachments.size,
                        sub.inputAttachments.size,
                        if (sub.usesDepthAttachment) 1 else 0,
                    ) + sub.colorAttachments + sub.inputAttachments
                } +
                listOf(resolveColorAttachments.size) +
                resolveColorAttachments.sorted())
            .toIntArray()
}
