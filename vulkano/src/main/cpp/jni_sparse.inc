JNI_METHOD(jlongArray, sparseCapabilities)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto d = get<Device>(id);
        VkPhysicalDeviceFeatures f{};
        vkGetPhysicalDeviceFeatures(d->physical, &f);
        const jlong values[] = {jlong((d->available & SparseResources) != 0),
                                f.sparseResidencyBuffer,
                                f.sparseResidencyImage2D,
                                f.sparseResidencyImage3D,
                                f.shaderResourceResidency,
                                f.sparseResidencyAliased,
                                f.shaderResourceMinLod,
                                d->properties.sparseProperties.residencyNonResidentStrict,
                                jlong(d->properties.limits.sparseAddressSpaceSize)};
        auto result = e->NewLongArray(9);
        if (result)
            e->SetLongArrayRegion(result, 0, 9, values);
        return result;
    });
}
static std::shared_ptr<SparseState> sparseState(jlong id) {
    const auto resource = get<Resource>(id);
    std::shared_ptr<SparseState> s;
    if (auto b = std::dynamic_pointer_cast<Buffer>(resource))
        s = b->sparse;
    else if (auto t = std::dynamic_pointer_cast<Texture>(resource))
        s = t->sparse;
    require(bool(s), "An original sparse resource is required");
    return s;
}
JNI_METHOD(jlongArray, sparseInfo)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto s = sparseState(id);
        std::set<SparsePage *> unique;
        VkDeviceSize bytes = 0, meta = 0;
        for (const auto &[key, p] : s->pages) {
            (void)key;
            if (unique.insert(p.get()).second)
                bytes += p->info.size;
        }
        for (const auto &p : s->metadata)
            meta += p->info.size;
        const auto &g = s->tiles.formatProperties.imageGranularity;
        const jlong values[] = {
            jlong(s->requirements.size),
            jlong(s->requirements.alignment),
            jlong(bytes + meta),
            jlong(s->pages.size()),
            g.width,
            g.height,
            g.depth,
            s->tiles.imageMipTailFirstLod,
            jlong(s->tiles.imageMipTailSize),
            jlong((s->tiles.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0),
            jlong(meta)};
        auto result = e->NewLongArray(11);
        if (result)
            e->SetLongArrayRegion(result, 0, 11, values);
        return result;
    });
}
JNI_METHOD(jlong, createSparseBuffer)(JNIEnv *e, jobject, jlong id, jlong size, jint usage) {
    return guard(e, [&] {
        require(size > 0, "Invalid sparse buffer size");
        return put(std::make_shared<Buffer>(get<Device>(id), size, usage, Storage::Private, false,
                                            std::shared_ptr<Heap>{}, 0, false, true));
    });
}
JNI_METHOD(jlong, createSparseTexture)
(JNIEnv *e, jobject, jlong id, jint width, jint height, jint format, jint usage, jintArray options) {
    return guard(e, [&] {
        require(width > 0 && height > 0, "Invalid sparse texture size");
        return put(std::make_shared<Texture>(get<Device>(id), width, height, static_cast<VkFormat>(format), usage,
                                             Storage::Private, textureOptions(e, options), std::shared_ptr<Heap>{}, 0,
                                             false, true));
    });
}
JNI_METHOD(void, mapSparseBuffer)
(JNIEnv *e, jobject, jlong id, jlong page, jint count, jboolean resident, jlong source, jlong sourcePage) {
    guard(e, [&] {
        require(page >= 0 && count > 0 && sourcePage >= 0, "Invalid sparse page range");
        auto s = sparseState(id);
        auto from = source ? sparseState(source) : nullptr;
        s->mapBuffer(page, count, resident, from.get(), sourcePage);
    });
}
JNI_METHOD(void, mapSparseTexture)
(JNIEnv *e, jobject, jlong id, jintArray region, jboolean resident, jlong source, jintArray sourceRegion) {
    guard(e, [&] {
        auto t = get<Texture>(id);
        auto s = sparseState(id);
        auto from = source ? get<Texture>(source) : nullptr;
        s->mapTexture(*t, imageRegion(e, region), resident, from.get(),
                      source ? imageRegion(e, sourceRegion) : ImageRegion{});
    });
}
JNI_METHOD(void, mapSparseTail)(JNIEnv *e, jobject, jlong id, jint layer, jboolean resident) {
    guard(e, [&] {
        require(layer >= 0, "Invalid sparse tail layer");
        sparseState(id)->mapTail(*get<Texture>(id), layer, resident);
    });
}
JNI_METHOD(jboolean, sparseBufferResident)(JNIEnv *e, jobject, jlong id, jlong page, jint count) {
    return guard(e, [&] {
        auto s = sparseState(id);
        require(s->buffer && page >= 0 && count > 0 &&
                    uint64_t(page) < s->requirements.size / s->requirements.alignment &&
                    uint64_t(count) <= s->requirements.size / s->requirements.alignment - uint64_t(page),
                "Invalid sparse range");
        for (uint64_t n = 0; n < uint64_t(count); ++n)
            if (!s->pages.count({0, 0, 0, uint64_t(page) + n, 0, 0}))
                return false;
        return true;
    });
}

JNI_METHOD(jboolean, sparseTextureResident)(JNIEnv *e, jobject, jlong id, jintArray region) {
    return guard(e, [&] { return sparseState(id)->isResident(*get<Texture>(id), imageRegion(e, region)); });
}
