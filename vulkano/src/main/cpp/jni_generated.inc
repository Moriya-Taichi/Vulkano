std::shared_ptr<GeneratedExecution> generatedExecution(JNIEnv *e, jlongArray packed) {
    auto v = longValues(e, packed);
    if (v.empty())
        return {};
    require(v.size() == 7 && v[2] >= 0 && v[3] > 0 && v[3] <= UINT32_MAX && v[5] >= 0 && v[6] > 0 && v[6] <= UINT32_MAX,
            "Invalid generated execution descriptor");
    return std::make_shared<GeneratedExecution>(get<GeneratedLayout>(v[0]), get<Buffer>(v[1]), v[2], uint32_t(v[3]),
                                                v[4] ? get<Buffer>(v[4]) : nullptr, v[5], uint32_t(v[6]));
}
JNI_METHOD(jlong, createGeneratedLayout)
(JNIEnv *e, jobject, jlong device, jlongArray pipelines, jintArray tokens, jint stride, jboolean unordered) {
    return guard(e, [&] {
        auto t = ints(e, tokens);
        require(t.size() % 4 == 0 && stride > 0, "Invalid generated layout descriptor");
        std::vector<GeneratedToken> ts;
        for (size_t n = 0; n < t.size(); n += 4) {
            require(t[n + 1] >= 0 && t[n + 2] >= 0 && t[n + 3] >= 0, "Invalid generated token");
            ts.push_back(
                {VkIndirectCommandsTokenTypeEXT(t[n]), uint32_t(t[n + 1]), uint32_t(t[n + 2]), uint32_t(t[n + 3])});
        }
        std::vector<std::shared_ptr<Pipeline>> ps;
        for (auto id : longValues(e, pipelines))
            ps.push_back(get<Pipeline>(id));
        return put(std::make_shared<GeneratedLayout>(get<Device>(device), std::move(ps), ts, stride, unordered));
    });
}
JNI_METHOD(jlongArray, generatedLimits)(JNIEnv *e, jobject, jlong device) {
    return guard(e, [&] {
        auto d = get<Device>(device);
        const auto &p = d->extensions->generatedProperties;
        return longs(e,
                     {p.maxIndirectPipelineCount, p.maxIndirectSequenceCount, p.maxIndirectCommandsTokenCount,
                      p.maxIndirectCommandsTokenOffset, p.maxIndirectCommandsIndirectStride,
                      p.supportedIndirectCommandsShaderStages, p.supportedIndirectCommandsShaderStagesPipelineBinding,
                      p.deviceGeneratedCommandsMultiDrawIndirectCount, d->extensions->generatedVertexInput});
    });
}
JNI_METHOD(void, executeGenerated)
(JNIEnv *e, jobject, jlong command, jlongArray generated, jlongArray bs, jbyteArray constants, jint kind) {
    return guard(e, [&] {
        auto g = generatedExecution(e, generated);
        require(bool(g), "Missing generated commands");
        auto p = g->layout->pipelines.front();
        require((kind == 0 && p->compute) || (kind == 2 && p->rayTracing), "Generated pipeline does not match encoder");
        get<Command>(command)->executeGenerated(std::move(g), bindings(e, bs), bytes(e, constants));
    });
}
JNI_METHOD(jlongArray, rayTableRegions)(JNIEnv *e, jobject, jlong pipeline) {
    return guard(e, [&] {
        auto p = get<RayTracingPipeline>(pipeline);
        std::vector<jlong> data;
        for (const auto &r : {p->raygen, p->miss, p->hit, p->callable})
            data.insert(data.end(), {jlong(r.deviceAddress), jlong(r.size), jlong(r.stride)});
        return longs(e, data);
    });
}
