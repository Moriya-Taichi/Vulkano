package dev.vulkano

import dev.vulkano.internal.Native

/** Enable tile shading for a render pass and its graphics pipelines. Apron sizes are in pixels. */
data class TileShadingDescriptor(val apronWidth: Int = 0, val apronHeight: Int = apronWidth) {
    init {
        require(apronWidth >= 0 && apronHeight >= 0)
    }

    internal fun pack() = intArrayOf(apronWidth, apronHeight)
}

data class TileShadingCapabilities(
    val fragmentStage: Boolean,
    val colorAttachments: Boolean,
    val depthAttachments: Boolean,
    val stencilAttachments: Boolean,
    val inputAttachments: Boolean,
    val sampledAttachments: Boolean,
    val perTileDraw: Boolean,
    val perTileDispatch: Boolean,
    val areaDispatch: Boolean,
    val apron: Boolean,
    val anisotropicApron: Boolean,
    val imageAtomics: Boolean,
    val maxApronSize: Int,
    val preferNonCoherentReads: Boolean,
    val tileGranularity: Size,
    val maxShadingRate: Size?,
)

/** Reports driver support before enabling Feature.TILE_SHADING. */
fun Device.tileShadingCapabilities(): TileShadingCapabilities? = access {
    if (Feature.TILE_SHADING !in capabilities.availableFeatures) return@access null
    val v = Native.tileCapabilities(nativeHandle)
    TileShadingCapabilities(
        v[0] != 0,
        v[1] != 0,
        v[2] != 0,
        v[3] != 0,
        v[4] != 0,
        v[5] != 0,
        v[6] != 0,
        v[7] != 0,
        v[8] != 0,
        v[9] != 0,
        v[10] != 0,
        v[11] != 0,
        v[12],
        v[13] != 0,
        Size(v[14], v[15]),
        if (v[8] == 0) null else Size(v[16], v[17]),
    )
}

/**
 * Non-null for TileShadingRateQCOM shaders; these require dispatchTile(), not a workgroup count.
 */
val ComputePipelineState.tileShadingRate: Size?
    get() = access {
        val v = Native.pipelineTileRate(it)
        if (v[0] == 0) null else Size(v[0], v[1], v[2])
    }
