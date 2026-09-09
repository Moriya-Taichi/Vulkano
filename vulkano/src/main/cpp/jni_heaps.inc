JNI_METHOD(jlong, createHeap)(JNIEnv *e, jobject, jlong id, jlong size, jint storage) {
    return guard(e, [&] {
        require(size > 0 && storage >= 0 && storage <= 1, "Invalid heap descriptor");
        return put(std::make_shared<Heap>(get<Device>(id), size, static_cast<Storage>(storage)));
    });
}
JNI_METHOD(jlong, createHeapBuffer)(JNIEnv *e, jobject, jlong id, jlong size, jint usage, jlong offset) {
    return guard(e, [&] {
        require(size > 0 && offset >= 0, "Invalid buffer size/offset");
        auto h = get<Heap>(id);
        return put(std::make_shared<Buffer>(h->d, size, usage, h->storage, false, h, offset));
    });
}
JNI_METHOD(jlong, createHeapTexture)
(JNIEnv *e, jobject, jlong id, jint width, jint height, jint format, jint usage, jintArray options, jlong offset) {
    return guard(e, [&] {
        require(width > 0 && height > 0 && offset >= 0, "Invalid texture size/offset");
        auto h = get<Heap>(id);
        return put(std::make_shared<Texture>(h->d, width, height, static_cast<VkFormat>(format), usage,
                                             Storage::Private, textureOptions(e, options), h, offset));
    });
}
JNI_METHOD(jlong, createTextureBuffer)
(JNIEnv *e, jobject, jlong id, jint format, jlong offset, jlong length, jboolean writable) {
    return guard(e, [&] {
        require(offset >= 0 && length > 0, "Invalid texture buffer range");
        return put(
            std::make_shared<TextureBuffer>(get<Buffer>(id), static_cast<VkFormat>(format), offset, length, writable));
    });
}

static jlongArray heapRequirements(JNIEnv *e, Device &d, VkMemoryRequirements r, bool dedicated) {
    r.alignment =
        std::max({r.alignment, d.properties.limits.bufferImageGranularity, d.properties.limits.nonCoherentAtomSize});
    require(r.size <= INT64_MAX - r.alignment + 1, "Heap requirement exceeds Kotlin size range");
    const jlong values[] = {jlong((r.size + r.alignment - 1) & ~(r.alignment - 1)), jlong(r.alignment),
                            jlong(r.memoryTypeBits), jlong(dedicated)};
    auto result = e->NewLongArray(4);
    if (result)
        e->SetLongArrayRegion(result, 0, 4, values);
    return result;
}
JNI_METHOD(jlongArray, heapBufferRequirements)(JNIEnv *e, jobject, jlong id, jlong size, jint usage) {
    return guard(e, [&] {
        require(size > 0, "Invalid buffer size");
        auto d = get<Device>(id);
        Buffer b(d, size, usage, Storage::Private, false, {}, 0, true);
        bool dedicated;
        const auto requirements = bufferRequirements(*d, b.buffer, &dedicated);
        return heapRequirements(e, *d, requirements, dedicated);
    });
}
JNI_METHOD(jlongArray, heapTextureRequirements)
(JNIEnv *e, jobject, jlong id, jint width, jint height, jint format, jint usage, jintArray options) {
    return guard(e, [&] {
        require(width > 0 && height > 0, "Invalid texture size");
        auto d = get<Device>(id);
        Texture t(d, width, height, static_cast<VkFormat>(format), usage, Storage::Private, textureOptions(e, options),
                  {}, 0, true);
        bool dedicated;
        const auto requirements = textureRequirements(*d, t.image, &dedicated);
        return heapRequirements(e, *d, requirements, dedicated);
    });
}
JNI_METHOD(jlong, createPlacementHeap)
(JNIEnv *e, jobject, jlong id, jlong size, jint storage, jint types, jlong alignment) {
    return guard(e, [&] {
        require(size > 0 && alignment > 0 && storage >= 0 && storage <= 1, "Invalid placement heap");
        return put(
            std::make_shared<Heap>(get<Device>(id), size, static_cast<Storage>(storage), uint32_t(types), alignment));
    });
}
JNI_METHOD(void, aliasResources)(JNIEnv *e, jobject, jlong id, jlong before, jlong after) {
    guard(e, [&] { get<Command>(id)->alias(get<Resource>(before), get<Resource>(after)); });
}
