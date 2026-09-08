namespace {
TensorOptions graphTensorOptions(JNIEnv *env, jobjectArray descriptions, size_t index) {
    auto array = static_cast<jlongArray>(env->GetObjectArrayElement(descriptions, index));
    require(array, "Graph tensor description is required");
    std::vector<jlong> values;
    try {
        values = longValues(env, array);
    } catch (...) {
        env->DeleteLocalRef(array);
        throw;
    }
    env->DeleteLocalRef(array);
    require(values.size() >= 5 && values[3] > 0 && uint64_t(values[3]) <= values.size() - 4,
            "Invalid graph tensor shape");
    const size_t rank = size_t(values[3]);
    require(values.size() == 4 + rank || values.size() == 4 + rank * 2, "Invalid graph tensor strides");
    require(values[0] >= 0 && values[0] <= INT32_MAX && values[1] >= 0 && values[1] <= 1 && values[2] > 0,
            "Invalid graph tensor options");
    TensorOptions result;
    result.format = VkFormat(values[0]);
    result.tiling = VkTensorTilingARM(values[1]);
    result.usage = uint64_t(values[2]);
    result.dimensions.assign(values.begin() + 4, values.begin() + 4 + rank);
    result.strides.assign(values.begin() + 4 + rank, values.end());
    return result;
}
} // namespace
JNI_METHOD(jintArray, graphCapabilities)(JNIEnv *env, jobject, jlong device) {
    return guard(env, [&] {
        const auto d = get<Device>(device);
        require(d->availableExtra & DataGraph, "Machine learning graphs are unavailable");
        const auto &e = *d->extensions;
        return intResult(env, {int(e.graph.dataGraphShaderModule), int(e.graph.dataGraphSpecializationConstants),
                               int(e.cacheControl.pipelineCreationCacheControl)});
    });
}
JNI_METHOD(jobjectArray, graphOperations)(JNIEnv *env, jobject, jlong device, jint queue) {
    return guard(env, [&] {
        const auto d = get<Device>(device);
        require(queue >= 0 && size_t(queue) < d->queues.size() && (d->enabledExtra & DataGraph), "Invalid graph queue");
        auto it = d->extensions->graphQueues.find(d->queues[queue].family);
        require(it != d->extensions->graphQueues.end(), "Queue has no GPU graph engine");
        std::vector<std::string> entries;
        for (const auto &row : it->second)
            if (row.operation.operationType ==
                VK_PHYSICAL_DEVICE_DATA_GRAPH_OPERATION_TYPE_SPIRV_EXTENDED_INSTRUCTION_SET_ARM)
                entries.push_back(std::to_string(row.operation.version) + "\n" + row.operation.name);
        auto cls = env->FindClass("java/lang/String");
        if (!cls)
            throw std::bad_alloc();
        auto result = env->NewObjectArray(entries.size(), cls, nullptr);
        env->DeleteLocalRef(cls);
        if (!result)
            throw std::bad_alloc();
        for (size_t i = 0; i < entries.size(); ++i) {
            auto value = env->NewStringUTF(entries[i].c_str());
            if (!value)
                throw std::bad_alloc();
            env->SetObjectArrayElement(result, i, value);
            env->DeleteLocalRef(value);
        }
        return result;
    });
}
JNI_METHOD(jlong, createGraphPipeline)
(JNIEnv *env, jobject, jlong device, jint queue, jbyteArray code, jstring entry, jintArray specialization,
 jintArray keys, jobjectArray descriptions, jintArray constantIds, jobjectArray constantDescriptions,
 jobjectArray constantData, jstring compilerOptions, jboolean optimize, jbyteArray identifier) {
    return guard(env, [&] {
        auto cached = bytes(env, identifier);
        Shader source;
        if (cached.empty())
            source = shader(env, code, entry);
        specialize(env, source, specialization);
        require(queue >= 0 && descriptions && constantDescriptions && constantData, "Missing graph pipeline options");
        auto indices = ints(env, keys), ids = ints(env, constantIds);
        require(indices.size() % 2 == 0 && indices.size() / 2 == size_t(env->GetArrayLength(descriptions)),
                "Invalid graph binding descriptions");
        require(ids.size() == size_t(env->GetArrayLength(constantDescriptions)) &&
                    ids.size() == size_t(env->GetArrayLength(constantData)),
                "Invalid graph constants");
        std::vector<GraphBinding> resources;
        for (size_t i = 0; i < indices.size(); i += 2) {
            require(indices[i] >= 0 && indices[i + 1] > 0, "Invalid graph binding index or count");
            resources.push_back(
                {uint32_t(indices[i]), uint32_t(indices[i + 1]), graphTensorOptions(env, descriptions, i / 2)});
        }
        std::vector<GraphConstant> constants;
        for (size_t i = 0; i < ids.size(); ++i) {
            require(ids[i] >= 0, "Invalid graph constant ID");
            auto data = static_cast<jbyteArray>(env->GetObjectArrayElement(constantData, i));
            std::vector<uint8_t> value;
            try {
                value = bytes(env, data);
            } catch (...) {
                if (data)
                    env->DeleteLocalRef(data);
                throw;
            }
            env->DeleteLocalRef(data);
            constants.push_back({uint32_t(ids[i]), graphTensorOptions(env, constantDescriptions, i), std::move(value)});
        }
        try {
            return put(std::make_shared<GraphPipeline>(get<Device>(device), uint32_t(queue), std::move(source),
                                                       std::move(resources), std::move(constants),
                                                       string(env, compilerOptions), optimize, cached));
        } catch (const GraphCacheMiss &) {
            return jlong(0);
        }
    });
}
JNI_METHOD(jintArray, graphAvailableProperties)(JNIEnv *env, jobject, jlong pipeline) {
    return guard(env, [&] {
        const auto properties = get<GraphPipeline>(pipeline)->availableProperties();
        return intResult(env, std::vector<jint>(properties.begin(), properties.end()));
    });
}
JNI_METHOD(jbyteArray, graphProperty)(JNIEnv *env, jobject, jlong pipeline, jint property) {
    return guard(env, [&] {
        require(property == 0 || property == 1, "Unknown graph property");
        auto value = get<GraphPipeline>(pipeline)->property(VkDataGraphPipelinePropertyARM(property));
        require(value.size() <= INT32_MAX, "Graph property exceeds byte array limit");
        auto result = env->NewByteArray(value.size());
        if (!result)
            throw std::bad_alloc();
        env->SetByteArrayRegion(result, 0, value.size(), reinterpret_cast<const jbyte *>(value.data()));
        return result;
    });
}
JNI_METHOD(void, dispatchGraph)(JNIEnv *env, jobject, jlong command, jlong pipeline, jlongArray tensors) {
    return guard(env, [&] {
        const auto values = longValues(env, tensors);
        require(values.size() % 3 == 0, "Invalid graph tensor bindings");
        std::vector<Binding> bindings;
        for (size_t i = 0; i < values.size(); i += 3) {
            require(values[i] >= 0 && values[i] <= UINT32_MAX && values[i + 1] >= 0 && values[i + 1] <= UINT32_MAX,
                    "Invalid graph tensor binding index");
            Binding binding{uint32_t(values[i])};
            binding.element = uint32_t(values[i + 1]);
            binding.tensor = get<TensorView>(values[i + 2]);
            bindings.push_back(std::move(binding));
        }
        get<Command>(command)->dispatchGraph(get<GraphPipeline>(pipeline), std::move(bindings));
    });
}
