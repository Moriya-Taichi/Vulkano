// Included after the shared JNI registry and array helpers.
JNI_METHOD(jintArray, tileCapabilities)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        const auto d = get<Device>(id);
        require(d->availableExtra & TileShading, "Tile shading is unavailable");
        const auto &f = d->extensions->tile;
        const auto &p = d->extensions->tileProperties;
        std::vector<jint> values{jint(f.tileShadingFragmentStage),
                                 jint(f.tileShadingColorAttachments),
                                 jint(f.tileShadingDepthAttachments),
                                 jint(f.tileShadingStencilAttachments),
                                 jint(f.tileShadingInputAttachments),
                                 jint(f.tileShadingSampledAttachments),
                                 jint(f.tileShadingPerTileDraw),
                                 jint(f.tileShadingPerTileDispatch),
                                 jint(f.tileShadingDispatchTile),
                                 jint(f.tileShadingApron),
                                 jint(f.tileShadingAnisotropicApron),
                                 jint(f.tileShadingAtomicOps),
                                 jint(p.maxApronSize),
                                 jint(p.preferNonCoherent),
                                 jint(p.tileGranularity.width),
                                 jint(p.tileGranularity.height),
                                 jint(p.maxTileShadingRate.width),
                                 jint(p.maxTileShadingRate.height)};
        auto out = e->NewIntArray(jint(values.size()));
        if (out)
            e->SetIntArrayRegion(out, 0, jint(values.size()), values.data());
        return out;
    });
}
JNI_METHOD(jintArray, pipelineTileRate)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        const auto p = get<Pipeline>(id);
        const jint values[]{jint(p->tileRate[0]), jint(p->tileRate[1]), jint(p->tileRate[2])};
        auto out = e->NewIntArray(3);
        if (out)
            e->SetIntArrayRegion(out, 0, 3, values);
        return out;
    });
}
JNI_METHOD(void, tileControl)(JNIEnv *e, jobject, jlong id, jint action) {
    return guard(e, [&] {
        auto p = get<PendingRender>(id);
        p->command->recording();
        require(p->pass.tileShading && action >= 3 && action <= 5, "Invalid tile command");
        require(action != 3 || p->d->extensions->tile.tileShadingPerTileDraw ||
                    p->d->extensions->tile.tileShadingPerTileDispatch,
                "Per-tile execution is unavailable");
        require(action != 3 || !p->perTile, "Per-tile execution is already active");
        require(action != 4 || p->perTile, "Per-tile execution is not active");
        Draw marker;
        marker.subpass = p->subpass;
        marker.tileAction = action;
        p->pass.draws.push_back(std::move(marker));
        if (action == 3)
            p->perTile = true;
        if (action == 4)
            p->perTile = false;
    });
}
JNI_METHOD(void, dispatchTile)
(JNIEnv *e, jobject, jlong id, jlong pipeline, jlongArray bs, jbyteArray constants, jintArray groups, jboolean area,
 jlong indirect, jlong offset) {
    return guard(e, [&] {
        auto p = get<PendingRender>(id);
        p->command->recording();
        require(p->pass.tileShading && p->perTile && offset >= 0, "Tile dispatch requires per-tile execution");
        auto v = ints(e, groups);
        require(v.size() == 3 && v[0] > 0 && v[1] > 0 && v[2] > 0, "Invalid tile workgroup count");
        Draw op;
        op.pipeline = get<Pipeline>(pipeline);
        require(op.pipeline->compute && op.pipeline->owner() == p->owner(), "Tile compute pipeline required");
        op.bindings = bindings(e, bs);
        op.constants = bytes(e, constants);
        op.tileAction = area ? 2 : 1;
        op.subpass = p->subpass;
        op.meshGroups = {uint32_t(v[0]), uint32_t(v[1]), uint32_t(v[2])};
        if (indirect)
            op.indirect = get<Buffer>(indirect);
        op.indirectOffset = offset;
        p->pass.draws.push_back(std::move(op));
    });
}
