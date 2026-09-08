JNI_METHOD(jlong, createHeap)(JNIEnv *e, jobject, jlong id, jlong size, jint storage) {
    return guard(e, [&] {
        require(size > 0 && storage >= 0 && storage <= 1, "Invalid heap descriptor");
        return put(std::make_shared<Heap>(get<Device>(id), size, static_cast<Storage>(storage)));
    });
}
JNI_METHOD(jlong, createHeapBuffer)(JNIEnv *e, jobject, jlong id, jlong size, jint usage) {
    return guard(e, [&] {
        require(size > 0, "Invalid buffer size");
        auto h = get<Heap>(id);
        return put(std::make_shared<Buffer>(h->d, size, usage, h->storage, false, h));
    });
}
JNI_METHOD(jlong, createHeapTexture)
(JNIEnv *e, jobject, jlong id, jint width, jint height, jint format, jint usage, jintArray options) {
    return guard(e, [&] {
        require(width > 0 && height > 0, "Invalid texture size");
        auto h = get<Heap>(id);
        return put(std::make_shared<Texture>(h->d, width, height, static_cast<VkFormat>(format), usage,
                                             Storage::Private, textureOptions(e, options), h));
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
