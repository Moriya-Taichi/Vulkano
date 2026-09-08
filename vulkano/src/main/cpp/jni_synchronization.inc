JNI_METHOD(jlong, createEvent)(JNIEnv *e, jobject, jlong device, jlong initial) {
    return guard(e, [&] {
        require(initial >= 0, "Negative event value");
        return put(std::make_shared<SharedEvent>(get<Device>(device), initial));
    });
}
JNI_METHOD(jlong, eventValue)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] { return jlong(get<SharedEvent>(id)->value()); });
}
JNI_METHOD(void, signalEvent)(JNIEnv *e, jobject, jlong id, jlong value) {
    return guard(e, [&] {
        require(value >= 0, "Negative event value");
        get<SharedEvent>(id)->signal(value);
    });
}
JNI_METHOD(void, waitEvent)(JNIEnv *e, jobject, jlong id, jlong event, jlong value) {
    return guard(e, [&] {
        require(value >= 0, "Negative event value");
        get<Command>(id)->waitEvent(get<SharedEvent>(event), value);
    });
}
JNI_METHOD(void, signalCommandEvent)(JNIEnv *e, jobject, jlong id, jlong event, jlong value) {
    return guard(e, [&] {
        require(value >= 0, "Negative event value");
        get<Command>(id)->signalEvent(get<SharedEvent>(event), value);
    });
}
JNI_METHOD(jlong, createCounters)(JNIEnv *e, jobject, jlong device, jint count, jboolean timestamp) {
    return guard(e, [&] {
        require(count > 0, "Invalid counter count");
        return put(std::make_shared<CounterPool>(get<Device>(device), count, timestamp));
    });
}
JNI_METHOD(jlongArray, readCounters)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto v = get<CounterPool>(id)->read();
        return longs(e, std::vector<jlong>(v.begin(), v.end()));
    });
}
JNI_METHOD(void, sampleCounter)(JNIEnv *e, jobject, jlong id, jlong pool, jint index) {
    return guard(e, [&] {
        require(index >= 0, "Negative counter index");
        get<Command>(id)->sample(get<CounterPool>(pool), index);
    });
}
JNI_METHOD(void, retainBuffer)(JNIEnv *e, jobject, jlong id, jlong buffer) {
    return guard(e, [&] {
        auto c = get<Command>(id);
        auto b = get<Buffer>(buffer);
        c->recording();
        require(c->owner() == b->owner(), "Buffer belongs to a different device");
        c->buffers.push_back(b);
    });
}
JNI_METHOD(jbyteArray, pipelineCacheData)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto d = get<Device>(id);
        size_t size = 0;
        check(vkGetPipelineCacheData(d->device, d->pipelineCache, &size, nullptr), "query pipeline cache size");
        require(size <= INT32_MAX, "Pipeline cache exceeds JVM array size");
        std::vector<uint8_t> data(size);
        check(vkGetPipelineCacheData(d->device, d->pipelineCache, &size, data.data()), "serialize pipeline cache");
        auto result = e->NewByteArray(size);
        if (!result)
            throw std::bad_alloc();
        e->SetByteArrayRegion(result, 0, size, reinterpret_cast<const jbyte *>(data.data()));
        return result;
    });
}
JNI_METHOD(void, loadPipelineCache)(JNIEnv *e, jobject, jlong id, jbyteArray values) {
    return guard(e, [&] {
        auto d = get<Device>(id);
        auto data = bytes(e, values);
        require(data.size() >= 32, "Truncated pipeline cache");
        uint32_t fields[4];
        std::memcpy(fields, data.data(), 16);
        require(fields[0] >= 32 && fields[0] <= data.size() && fields[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                    fields[2] == d->properties.vendorID && fields[3] == d->properties.deviceID &&
                    std::memcmp(data.data() + 16, d->properties.pipelineCacheUUID, VK_UUID_SIZE) == 0,
                "Pipeline cache is incompatible with this device/driver");
        VkPipelineCacheCreateInfo i{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        i.initialDataSize = data.size();
        i.pInitialData = data.data();
        VkPipelineCache cache;
        check(vkCreatePipelineCache(d->device, &i, nullptr, &cache), "load pipeline cache");
        auto result = vkMergePipelineCaches(d->device, d->pipelineCache, 1, &cache);
        vkDestroyPipelineCache(d->device, cache, nullptr);
        check(result, "merge pipeline cache");
    });
}

JNI_METHOD(jboolean, pollIdle)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto d = get<Device>(id);
        d->collect();
        if (!d->pending.empty())
            return false;
        check(vkQueueWaitIdle(d->queue), "wait for presentation idle");
        return true;
    });
}
