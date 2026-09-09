JNI_METHOD(jlong, createExternalSemaphore)(JNIEnv *e, jobject, jlong id, jint fd) {
    return guard(e, [&] { return put(std::make_shared<ExternalSemaphore>(get<Device>(id), fd)); });
}
JNI_METHOD(jint, exportSyncFd)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] { return get<ExternalSemaphore>(id)->exportFd(); });
}
JNI_METHOD(jint, duplicateSyncFd)(JNIEnv *e, jobject, jint fd) {
    return guard(e, [&] { return duplicateSyncFd(fd); });
}
JNI_METHOD(void, closeSyncFd)(JNIEnv *e, jobject, jint fd) {
    guard(e, [&] {
        require(fd >= -1, "Invalid sync fd");
        if (fd >= 0)
            close(fd);
    });
}
JNI_METHOD(void, waitExternalSemaphore)(JNIEnv *e, jobject, jlong command, jlong semaphore) {
    guard(e, [&] { get<Command>(command)->waitExternal(get<ExternalSemaphore>(semaphore)); });
}
JNI_METHOD(void, signalExternalSemaphore)(JNIEnv *e, jobject, jlong command, jlong semaphore) {
    guard(e, [&] { get<Command>(command)->signalExternal(get<ExternalSemaphore>(semaphore)); });
}
JNI_METHOD(jlong, importHardwareBuffer)
(JNIEnv *e, jobject, jlong id, jobject buffer, jint usage, jboolean externalFormat, jboolean linear, jint model,
 jint range, jint externalFamily) {
    return guard(e, [&]() -> jlong {
        auto device = get<Device>(id);
        require(device->enabledExtra & HardwareBufferInterop, "Android hardware buffer feature was not enabled");
#ifdef __ANDROID__
        require(buffer, "HardwareBuffer is required");
        const auto cls = e->FindClass("android/hardware/HardwareBuffer");
        const bool valid = cls && e->IsInstanceOf(buffer, cls);
        if (cls)
            e->DeleteLocalRef(cls);
        require(valid, "Expected an Android HardwareBuffer");
        auto native = AHardwareBuffer_fromHardwareBuffer(e, buffer);
        require(native && !e->ExceptionCheck(), "HardwareBuffer is closed or invalid");
        return put(importHardwareBuffer(device, native, uint32_t(usage), externalFormat, linear, model, range,
                                        uint32_t(externalFamily)));
#else
        (void)buffer;
        (void)usage;
        (void)externalFormat;
        (void)linear;
        (void)model;
        (void)range;
        (void)externalFamily;
        throw std::invalid_argument("HardwareBuffer import requires Android");
#endif
    });
}
JNI_METHOD(jintArray, hardwareBufferTextureInfo)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto t = get<Texture>(id);
        require(bool(t->external), "Texture is not an imported hardware buffer");
        return intResult(e, {jint(t->width), jint(t->height), jint(t->format), jint(t->usage),
                             jint(t->options.mipLevels), jint(t->options.layers), jint(t->options.type)});
    });
}
JNI_METHOD(jlong, hardwareBufferSampler)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&]() -> jlong {
        auto t = get<Texture>(id);
        require(bool(t->external), "Texture is not an imported hardware buffer");
        return t->external->conversionSampler ? put(t->external->conversionSampler) : 0;
    });
}
JNI_METHOD(void, acquireExternalTexture)(JNIEnv *e, jobject, jlong command, jlong texture, jboolean preserve) {
    guard(e, [&] { get<Command>(command)->acquireExternal(get<Texture>(texture), preserve); });
}
JNI_METHOD(void, releaseExternalTexture)(JNIEnv *e, jobject, jlong command, jlong texture) {
    guard(e, [&] { get<Command>(command)->releaseExternal(get<Texture>(texture)); });
}
