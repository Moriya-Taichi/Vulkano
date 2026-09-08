JNI_METHOD(jlong, bufferAddress)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] { return jlong(bufferAddress(*get<Buffer>(id))); });
}
JNI_METHOD(jlong, createPrimitiveAcceleration)(JNIEnv *e, jobject, jlong device, jlongArray values, jboolean update) {
    return guard(e, [&] {
        auto v = longValues(e, values);
        require(!v.empty() && v.size() % 10 == 0, "Invalid geometry descriptor");
        std::vector<Geometry> geometries;
        for (size_t i = 0; i < v.size(); i += 10) {
            Geometry g;
            g.boxes = v[i];
            g.vertices = get<Buffer>(v[i + 1]);
            for (int n : {2, 3, 4, 5, 8})
                require(v[i + n] >= 0, "Invalid geometry range");
            require(v[i + 4] <= UINT32_MAX && v[i + 5] <= UINT32_MAX, "Geometry count overflow");
            g.vertexOffset = v[i + 2];
            g.stride = v[i + 3];
            g.vertexCount = v[i + 4];
            g.primitiveCount = v[i + 5];
            if (v[i + 6])
                g.indices = get<Buffer>(v[i + 6]);
            g.indexType = static_cast<VkIndexType>(v[i + 7]);
            g.indexOffset = v[i + 8];
            g.flags = v[i + 9];
            geometries.push_back(g);
        }
        return put(std::make_shared<AccelerationStructure>(get<Device>(device), geometries, update));
    });
}
JNI_METHOD(jlong, createInstanceAcceleration)
(JNIEnv *e, jobject, jlong device, jlongArray values, jfloatArray matrices, jboolean update) {
    return guard(e, [&] {
        auto v = longValues(e, values);
        auto m = floatValues(e, matrices);
        require(!v.empty() && v.size() % 5 == 0 && m.size() == v.size() / 5 * 12, "Invalid instances");
        std::vector<AccelerationInstance> instances;
        for (size_t i = 0; i < v.size(); i += 5) {
            AccelerationInstance a;
            a.structure = get<AccelerationStructure>(v[i]);
            for (int n = 1; n < 5; ++n)
                require(v[i + n] >= 0 && v[i + n] <= 0xffffff, "Invalid instance value");
            std::memcpy(a.transform.matrix, m.data() + i / 5 * 12, 48);
            a.customIndex = v[i + 1];
            a.mask = v[i + 2];
            a.recordOffset = v[i + 3];
            a.flags = v[i + 4];
            instances.push_back(a);
        }
        return put(std::make_shared<AccelerationStructure>(get<Device>(device), instances, update));
    });
}
JNI_METHOD(void, buildAcceleration)(JNIEnv *e, jobject, jlong command, jlong structure, jboolean update) {
    return guard(e, [&] { get<Command>(command)->build(get<AccelerationStructure>(structure), update); });
}
JNI_METHOD(jlong, createRayPipeline)
(JNIEnv *e, jobject, jlong device, jobjectArray codes, jobjectArray entries, jobjectArray constants, jintArray stages,
 jintArray groupArray, jint recursion, jintArray schema, jint push) {
    return guard(e, [&] {
        auto flags = ints(e, stages);
        auto gs = ints(e, groupArray);
        require(codes && entries && constants && e->GetArrayLength(codes) == jint(flags.size()) &&
                    e->GetArrayLength(entries) == jint(flags.size()) &&
                    e->GetArrayLength(constants) == jint(flags.size()) && gs.size() % 5 == 0 && recursion > 0 &&
                    push >= 0,
                "Invalid ray pipeline");
        std::vector<Shader> shaders;
        std::vector<VkShaderStageFlagBits> sf;
        for (size_t n = 0; n < flags.size(); ++n) {
            auto code = static_cast<jbyteArray>(e->GetObjectArrayElement(codes, n));
            auto entry = static_cast<jstring>(e->GetObjectArrayElement(entries, n));
            auto spec = static_cast<jintArray>(e->GetObjectArrayElement(constants, n));
            auto s = shader(e, code, entry);
            specialize(e, s, spec);
            shaders.push_back(s);
            sf.push_back(static_cast<VkShaderStageFlagBits>(flags[n]));
            e->DeleteLocalRef(code);
            e->DeleteLocalRef(entry);
            e->DeleteLocalRef(spec);
        }
        std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
        for (size_t n = 0; n < gs.size(); n += 5) {
            VkRayTracingShaderGroupCreateInfoKHR g{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
            g.type = static_cast<VkRayTracingShaderGroupTypeKHR>(gs[n]);
            g.generalShader = gs[n + 1];
            g.closestHitShader = gs[n + 2];
            g.anyHitShader = gs[n + 3];
            g.intersectionShader = gs[n + 4];
            groups.push_back(g);
        }
        return put(std::make_shared<RayTracingPipeline>(get<Device>(device), layout(e, schema), push, shaders, sf,
                                                        groups, recursion));
    });
}
JNI_METHOD(void, traceRays)
(JNIEnv *e, jobject, jlong command, jlong pipeline, jlongArray bs, jbyteArray constants, jintArray size) {
    return guard(e, [&] {
        auto s = ints(e, size);
        require(s.size() == 3 && s[0] > 0 && s[1] > 0 && s[2] > 0, "Invalid trace dimensions");
        get<Command>(command)->trace(get<RayTracingPipeline>(pipeline), bindings(e, bs), bytes(e, constants),
                                     {uint32_t(s[0]), uint32_t(s[1]), uint32_t(s[2])});
    });
}
