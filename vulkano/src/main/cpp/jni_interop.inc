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
