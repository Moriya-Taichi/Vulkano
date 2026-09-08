namespace {
TensorOptions tensorOptions(JNIEnv *e, jint format, jint tiling, jlong usage, jlongArray shape, jlongArray strides) {
    TensorOptions result;
    result.format = VkFormat(format);
    result.tiling = VkTensorTilingARM(tiling);
    result.usage = uint64_t(usage);
    const auto dimensions = longValues(e, shape), steps = longValues(e, strides);
    result.dimensions.assign(dimensions.begin(), dimensions.end());
    result.strides.assign(steps.begin(), steps.end());
    return result;
}
} // namespace
JNI_METHOD(jlongArray, tensorCapabilities)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        const auto d = get<Device>(id);
        require(d->availableExtra & TensorResources, "Tensor resources are unavailable");
        const auto &f = d->extensions->tensor;
        const auto &p = d->extensions->tensorProperties;
        return longs(e, {p.maxTensorDimensionCount, jlong(p.maxTensorElements), jlong(p.maxPerDimensionTensorElements),
                         p.maxTensorStride, jlong(p.maxTensorSize), p.maxTensorShaderAccessArrayLength,
                         p.maxTensorShaderAccessSize, p.maxDescriptorSetStorageTensors,
                         p.maxPerStageDescriptorSetStorageTensors, p.shaderTensorSupportedStages, f.tensorNonPacked,
                         f.shaderTensorAccess, f.shaderStorageTensorArrayDynamicIndexing,
                         f.shaderStorageTensorArrayNonUniformIndexing});
    });
}
JNI_METHOD(jlong, createTensor)
(JNIEnv *e, jobject, jlong id, jint format, jint tiling, jlong usage, jlongArray shape, jlongArray strides,
 jint storage) {
    return guard(e, [&] {
        require(storage == 0 || storage == 1, "Invalid tensor storage mode");
        return put(std::make_shared<TensorResource>(
            get<Device>(id), tensorOptions(e, format, tiling, usage, shape, strides), Storage(storage)));
    });
}
JNI_METHOD(jboolean, supportsTensor)
(JNIEnv *e, jobject, jlong id, jint format, jint tiling, jlong usage, jlongArray shape, jlongArray strides,
 jint storage) {
    return guard(e, [&] {
        auto options = tensorOptions(e, format, tiling, usage, shape, strides);
        try {
            const auto d = get<Device>(id);
            options.validate(*d);
            if (storage != 0 && storage != 1)
                return false;
            if (storage == 0 && options.tiling != VK_TENSOR_TILING_LINEAR_ARM)
                return false;
            auto description = options.description();
            VkTensorCreateInfoARM create{VK_STRUCTURE_TYPE_TENSOR_CREATE_INFO_ARM};
            create.pDescription = &description;
            create.flags = VK_TENSOR_CREATE_MUTABLE_FORMAT_BIT_ARM;
            d->share(create);
            VkDeviceTensorMemoryRequirementsARM query{VK_STRUCTURE_TYPE_DEVICE_TENSOR_MEMORY_REQUIREMENTS_ARM};
            query.pCreateInfo = &create;
            VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
            d->extensions->deviceTensorMemoryRequirements(d->device, &query, &requirements);
            for (uint32_t i = 0; i < d->memory.memoryTypeCount; ++i) {
                auto flags = d->memory.memoryTypes[i].propertyFlags;
                if ((requirements.memoryRequirements.memoryTypeBits & (1u << i)) &&
                    !(flags & (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) &&
                    (storage == 1 || (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))
                    return true;
            }
            return false;
        } catch (const std::invalid_argument &) {
            return false;
        }
    });
}
JNI_METHOD(jlongArray, tensorInfo)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto tensor = get<TensorResource>(id);
        return longs(e, {jlong(tensor->allocationSize), jlong(tensor->linearSize)});
    });
}
JNI_METHOD(jlong, createTensorView)(JNIEnv *e, jobject, jlong id, jint format) {
    return guard(e, [&] { return put(std::make_shared<TensorView>(get<TensorResource>(id), VkFormat(format))); });
}
JNI_METHOD(void, accessTensorBytes)(JNIEnv *e, jobject, jlong id, jlong offset, jobject data, jboolean write) {
    return guard(e, [&] {
        require(data && offset >= 0, "Invalid tensor byte access");
        auto length = e->GetDirectBufferCapacity(data);
        auto pointer = e->GetDirectBufferAddress(data);
        require(length > 0 && pointer, "A non-empty direct ByteBuffer is required");
        get<TensorResource>(id)->accessBytes(uint64_t(offset), pointer, size_t(length), write);
    });
}
JNI_METHOD(void, copyTensor)(JNIEnv *e, jobject, jlong command, jlong src, jlong dst) {
    return guard(e, [&] { get<Command>(command)->copyTensor(get<TensorResource>(src), get<TensorResource>(dst)); });
}
