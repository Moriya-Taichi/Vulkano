#include "engine.hpp"
#include "generated.hpp"
#include "graphs.hpp"
#include "heaps.hpp"
#include "interop.hpp"
#include "ray.hpp"
#include "sparse.hpp"
#include "synchronization.hpp"
#include "tensors.hpp"
#include "tiles.hpp"
#include <cstring>
#include <jni.h>
#include <mutex>
#include <set>
#include <type_traits>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/hardware_buffer_jni.h>
#include <android/native_window_jni.h>
#endif

using namespace vulkano;
namespace {
// Opaque monotonic IDs, never JVM-visible pointers. JNI calls (including host
// Vulkan queue/pool access and teardown) are externally synchronized here.
std::recursive_mutex mutex;
std::unordered_map<jlong, std::shared_ptr<Object>> objects;
jlong nextId = 1;
template <class T> std::shared_ptr<T> get(jlong id) {
    auto it = objects.find(id);
    require(it != objects.end(), "Native resource is closed or invalid");
    auto result = std::dynamic_pointer_cast<T>(it->second);
    require(bool(result), "Native resource has the wrong type");
    return result;
}
jlong put(std::shared_ptr<Object> object) {
    require(nextId < INT64_MAX, "Native handle space exhausted");
    const auto id = nextId++;
    objects.emplace(id, std::move(object));
    return id;
}
void exception(JNIEnv *env, const char *type, const char *message) {
    if (env->ExceptionCheck())
        return;
    auto cls = env->FindClass(type);
    if (cls) {
        env->ThrowNew(cls, message);
        env->DeleteLocalRef(cls);
    }
}
template <class F> auto guard(JNIEnv *env, F action) -> decltype(action()) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    try {
        return action();
    } catch (const std::invalid_argument &e) {
        exception(env, "java/lang/IllegalArgumentException", e.what());
    } catch (const std::bad_alloc &) {
        exception(env, "java/lang/OutOfMemoryError", "Vulkano allocation failed");
    } catch (const std::exception &e) {
        exception(env, "java/lang/IllegalStateException", e.what());
    } catch (...) {
        exception(env, "java/lang/IllegalStateException", "Unexpected Vulkano native error");
    }
    if constexpr (!std::is_void_v<decltype(action())>)
        return {};
}
std::string string(JNIEnv *env, jstring source) {
    require(source != nullptr, "String is required");
    const char *chars = env->GetStringUTFChars(source, nullptr);
    if (!chars)
        throw std::bad_alloc();
    try {
        std::string result(chars);
        env->ReleaseStringUTFChars(source, chars);
        return result;
    } catch (...) {
        env->ReleaseStringUTFChars(source, chars);
        throw;
    }
}
std::vector<uint8_t> bytes(JNIEnv *env, jbyteArray source) {
    require(source != nullptr, "Byte array is required");
    std::vector<uint8_t> result(env->GetArrayLength(source));
    env->GetByteArrayRegion(source, 0, static_cast<jsize>(result.size()), reinterpret_cast<jbyte *>(result.data()));
    if (env->ExceptionCheck())
        throw std::runtime_error("Cannot read JNI byte array");
    return result;
}
std::vector<jint> ints(JNIEnv *env, jintArray source) {
    require(source != nullptr, "Int array is required");
    std::vector<jint> result(env->GetArrayLength(source));
    env->GetIntArrayRegion(source, 0, static_cast<jsize>(result.size()), result.data());
    if (env->ExceptionCheck())
        throw std::runtime_error("Cannot read JNI int array");
    return result;
}
jlongArray longs(JNIEnv *env, const std::vector<jlong> &values) {
    auto result = env->NewLongArray(static_cast<jsize>(values.size()));
    if (!result)
        throw std::bad_alloc();
    env->SetLongArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    return result;
}
jintArray intResult(JNIEnv *env, const std::vector<jint> &values) {
    auto result = env->NewIntArray(static_cast<jsize>(values.size()));
    if (!result)
        throw std::bad_alloc();
    env->SetIntArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    return result;
}
Shader shader(JNIEnv *env, jbyteArray source, jstring entry) {
    auto data = bytes(env, source);
    require(data.size() >= 20 && data.size() % 4 == 0, "Invalid SPIR-V byte count");
    Shader result;
    result.code.resize(data.size() / 4);
    std::memcpy(result.code.data(), data.data(), data.size());
    result.entry = string(env, entry);
    return result;
}
std::vector<BindingLayout> layout(JNIEnv *env, jintArray source) {
    auto data = ints(env, source);
    require(data.size() % 6 == 0, "Invalid binding layout");
    std::vector<BindingLayout> result;
    for (size_t i = 0; i < data.size(); i += 6) {
        require(data[i] >= 0, "Negative binding");
        require(data[i + 2] > 0, "Invalid descriptor count");
        BindingLayout b{static_cast<uint32_t>(data[i]), static_cast<VkDescriptorType>(data[i + 1])};
        b.count = data[i + 2];
        require(data[i + 5] >= 0, "Negative descriptor set");
        b.set = data[i + 5];
        const auto sampler = uint64_t(uint32_t(data[i + 3])) | (uint64_t(uint32_t(data[i + 4])) << 32);
        if (sampler)
            b.immutableSampler = get<Sampler>(jlong(sampler));
        result.push_back(b);
    }
    return result;
}
std::vector<Binding> bindings(JNIEnv *env, jlongArray source) {
    require(source != nullptr, "Bindings are required");
    std::vector<jlong> data(env->GetArrayLength(source));
    require(data.size() % 11 == 0, "Invalid binding data");
    env->GetLongArrayRegion(source, 0, static_cast<jsize>(data.size()), data.data());
    if (env->ExceptionCheck())
        throw std::runtime_error("Cannot read JNI bindings");
    std::vector<Binding> result;
    for (size_t i = 0; i < data.size(); i += 11) {
        require(data[i] >= 0 && data[i] <= UINT32_MAX && data[i + 2] >= 0 && data[i + 3] >= 0, "Invalid binding range");
        Binding b{};
        b.index = static_cast<uint32_t>(data[i]);
        require(data[i + 10] >= 0 && data[i + 10] <= UINT32_MAX, "Invalid descriptor set");
        b.set = data[i + 10];
        if (data[i + 1])
            b.buffer = get<Buffer>(data[i + 1]);
        b.offset = data[i + 2];
        b.length = data[i + 3];
        if (data[i + 4])
            b.texture = get<Texture>(data[i + 4]);
        if (data[i + 5])
            b.sampler = get<Sampler>(data[i + 5]);
        require(data[i + 6] >= 0 && data[i + 6] <= UINT32_MAX, "Invalid descriptor array element");
        b.element = data[i + 6];
        if (data[i + 9])
            b.tensor = get<TensorView>(data[i + 9]);
        if (data[i + 8])
            b.texel = get<TextureBuffer>(data[i + 8]);
        if (data[i + 7])
            b.acceleration = get<AccelerationStructure>(data[i + 7]);
        result.push_back(std::move(b));
    }
    return result;
}
std::vector<jfloat> floatValues(JNIEnv *e, jfloatArray a) {
    require(a, "Float array required");
    std::vector<jfloat> v(e->GetArrayLength(a));
    e->GetFloatArrayRegion(a, 0, v.size(), v.data());
    if (e->ExceptionCheck())
        throw std::runtime_error("JNI float array read failed");
    return v;
}
std::vector<jlong> longValues(JNIEnv *e, jlongArray a) {
    require(a, "Long array required");
    std::vector<jlong> v(e->GetArrayLength(a));
    e->GetLongArrayRegion(a, 0, v.size(), v.data());
    if (e->ExceptionCheck())
        throw std::runtime_error("JNI long array read failed");
    return v;
}
TextureOptions textureOptions(JNIEnv *e, jintArray a) {
    auto v = ints(e, a);
    require(v.size() == 5 && v[0] > 0 && v[1] > 0 && v[2] > 0 && v[3] > 0 && v[4] >= 0 && v[4] <= 6,
            "Invalid texture options");
    return {uint32_t(v[0]), uint32_t(v[1]), uint32_t(v[2]), static_cast<VkSampleCountFlagBits>(v[3]),
            static_cast<VkImageViewType>(v[4])};
}
void specialize(JNIEnv *e, Shader &s, jintArray a) {
    auto v = ints(e, a);
    require(v.size() % 4 == 0, "Invalid function constants");
    for (size_t i = 0; i < v.size(); i += 4) {
        require(v[i] >= 0 && (v[i + 1] == 1 || v[i + 1] == 2 || v[i + 1] == 4 || v[i + 1] == 8),
                "Invalid function constant ID or size");
        const uint64_t bits = uint32_t(v[i + 2]) | (uint64_t(uint32_t(v[i + 3])) << 32);
        require(s.constants.emplace(uint32_t(v[i]), FunctionConstant(bits, uint32_t(v[i + 1]))).second,
                "Duplicate function constant ID");
    }
}
ImageRegion imageRegion(JNIEnv *e, jintArray a) {
    auto v = ints(e, a);
    require(v.size() == 10, "Invalid image region");
    for (auto n : v)
        require(n >= 0, "Negative image region");
    return {uint32_t(v[0]),
            uint32_t(v[1]),
            uint32_t(v[2]),
            {v[3], v[4], v[5]},
            {uint32_t(v[6]), uint32_t(v[7]), uint32_t(v[8])},
            uint32_t(v[9])};
}
struct PendingRender : Resource {
    std::shared_ptr<Command> command;
    Render pass;
    uint32_t subpass = 0;
    bool perTile = false;
    explicit PendingRender(std::shared_ptr<Command> c) : Resource(c->d), command(std::move(c)) {}
};
} // namespace
#define JNI_METHOD(returnType, name) extern "C" JNIEXPORT returnType JNICALL Java_dev_vulkano_internal_Native_##name

JNI_METHOD(jlong, createDevice)
(JNIEnv *e, jobject, jlong features, jboolean validation, jboolean software, jlong extra) {
    return guard(e, [&] { return put(Device::create(features, validation, software, extra)); });
}
JNI_METHOD(jstring, deviceName)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] { return e->NewStringUTF(get<Device>(id)->properties.deviceName); });
}
JNI_METHOD(jlongArray, deviceInfo)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto d = get<Device>(id);
        const auto &l = d->properties.limits;
        return longs(e, {d->properties.apiVersion,
                         d->properties.deviceType,
                         static_cast<jlong>(d->available),
                         static_cast<jlong>(d->enabled),
                         l.maxStorageBufferRange,
                         l.maxUniformBufferRange,
                         static_cast<jlong>(l.minStorageBufferOffsetAlignment),
                         static_cast<jlong>(l.minUniformBufferOffsetAlignment),
                         l.maxPushConstantsSize,
                         l.maxImageDimension2D,
                         l.maxComputeWorkGroupInvocations,
                         l.maxComputeWorkGroupSize[0],
                         l.maxComputeWorkGroupSize[1],
                         l.maxComputeWorkGroupSize[2],
                         l.maxComputeWorkGroupCount[0],
                         l.maxComputeWorkGroupCount[1],
                         l.maxComputeWorkGroupCount[2],
                         d->subgroup.subgroupSize,
                         d->subgroup.supportedStages,
                         d->subgroup.supportedOperations,
                         d->memoryBudget,
                         static_cast<jlong>(l.maxSamplerAnisotropy * 1000),
                         static_cast<jlong>(d->availableExtra),
                         static_cast<jlong>(d->enabledExtra), l.maxBoundDescriptorSets});
    });
}
JNI_METHOD(jlongArray, memoryHeaps)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto d = get<Device>(id);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(d->allocator, budgets);
        std::vector<jlong> result;
        for (uint32_t i = 0; i < d->memory.memoryHeapCount; ++i)
            result.insert(result.end(), {static_cast<jlong>(d->memory.memoryHeaps[i].size),
                                         (d->memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0,
                                         static_cast<jlong>(budgets[i].budget), static_cast<jlong>(budgets[i].usage),
                                         !d->memoryBudget});
        return longs(e, result);
    });
}
JNI_METHOD(void, closeDevice)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto d = get<Device>(id);
        // Destructors drain submitted work even on a lost device.
        for (auto it = objects.begin(); it != objects.end();) {
            if (it->first != id && it->second->owner() == d.get())
                it = objects.erase(it);
            else
                ++it;
        }
        objects.erase(id);
    });
}
JNI_METHOD(void, close)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] { objects.erase(id); });
}
JNI_METHOD(void, waitIdle)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] { get<Device>(id)->waitIdle(); });
}
JNI_METHOD(jlong, createBuffer)(JNIEnv *e, jobject, jlong device, jlong length, jint usage, jint storage) {
    return guard(e, [&] {
        require(length > 0 && storage >= 0 && storage <= 1, "Invalid buffer descriptor");
        return put(std::make_shared<Buffer>(get<Device>(device), length, usage, static_cast<Storage>(storage)));
    });
}
JNI_METHOD(jlong, createUploadBuffer)(JNIEnv *e, jobject, jlong device, jlong length, jint usage) {
    return guard(e, [&] {
        require(length > 0, "Invalid upload buffer length");
        return put(std::make_shared<Buffer>(get<Device>(device), length, usage, Storage::Shared, true));
    });
}
JNI_METHOD(void, writeBuffer)(JNIEnv *e, jobject, jlong id, jlong offset, jobject source) {
    return guard(e, [&] {
        require(source && offset >= 0, "Invalid buffer write");
        const auto count = e->GetDirectBufferCapacity(source);
        const auto data = e->GetDirectBufferAddress(source);
        require(count > 0 && data, "A non-empty direct ByteBuffer is required");
        get<Buffer>(id)->write(offset, data, static_cast<size_t>(count));
    });
}
JNI_METHOD(void, readBuffer)(JNIEnv *e, jobject, jlong id, jlong offset, jobject destination) {
    return guard(e, [&] {
        require(destination && offset >= 0, "Invalid buffer read");
        const auto count = e->GetDirectBufferCapacity(destination);
        const auto data = e->GetDirectBufferAddress(destination);
        require(count > 0 && data, "A non-empty direct ByteBuffer is required");
        get<Buffer>(id)->read(offset, data, static_cast<size_t>(count));
    });
}
JNI_METHOD(jlong, createTexture)
(JNIEnv *e, jobject, jlong device, jint w, jint h, jint format, jint usage, jint storage, jintArray options) {
    return guard(e, [&] {
        auto o = textureOptions(e, options);
        require(w > 0 && h > 0 && storage >= 1 && storage <= 2, "Invalid texture descriptor");
        return put(std::make_shared<Texture>(get<Device>(device), w, h, static_cast<VkFormat>(format), usage,
                                             static_cast<Storage>(storage), o));
    });
}
JNI_METHOD(jboolean, textureIsLazy)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] { return get<Texture>(id)->lazy; });
}
JNI_METHOD(jlongArray, textureFormatCapabilities)
(JNIEnv *e, jobject, jlong id, jint format, jint type, jint usage) {
    return guard(e, [&] {
        require(type >= 0 && type <= 6 && format != VK_FORMAT_UNDEFINED && usage > 0 &&
                    !(usage & ~(255u | VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)),
                "Invalid texture format query");
        auto d = get<Device>(id);
        if ((usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) && !(d->available & AttachmentRate))
            return longs(e, {});
        VkImageFormatProperties p{};
        auto viewType = VkImageViewType(type);
        auto r =
            vkGetPhysicalDeviceImageFormatProperties(d->physical, VkFormat(format), textureImageType(viewType),
                                                     VK_IMAGE_TILING_OPTIMAL, usage, textureImageFlags(viewType), &p);
        if (r == VK_ERROR_FORMAT_NOT_SUPPORTED)
            return longs(e, {});
        check(r, "query texture format capabilities");
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(d->physical, VkFormat(format), &fp);
        return longs(e, {jlong(p.maxExtent.width), jlong(p.maxExtent.height), jlong(p.maxExtent.depth),
                         jlong(p.maxMipLevels), jlong(p.maxArrayLayers), jlong(p.sampleCounts),
                         jlong(std::min(p.maxResourceSize, VkDeviceSize(INT64_MAX))), jlong(fp.optimalTilingFeatures)});
    });
}
JNI_METHOD(jlong, createSampler)
(JNIEnv *e, jobject, jlong id, jboolean linear, jboolean repeat, jfloat anisotropy, jintArray options,
 jfloatArray lod) {
    return guard(e, [&] {
        auto o = ints(e, options);
        auto l = floatValues(e, lod);
        require(o.size() == 6 && l.size() == 3, "Invalid sampler");
        return put(std::make_shared<Sampler>(
            get<Device>(id), linear, repeat, anisotropy, static_cast<VkSamplerMipmapMode>(o[0]), l[0], l[1], l[2],
            static_cast<VkCompareOp>(o[1]), o[2],
            o[3] == -1 ? VK_SAMPLER_ADDRESS_MODE_MAX_ENUM : static_cast<VkSamplerAddressMode>(o[3]),
            VkSamplerReductionMode(o[4]), VkBorderColor(o[5])));
    });
}
JNI_METHOD(jlong, createComputePipeline)
(JNIEnv *e, jobject, jlong device, jbyteArray code, jstring entry, jintArray schema, jint push, jintArray constants,
 jboolean indirect) {
    return guard(e, [&] {
        require(push >= 0, "Negative push constant size");
        auto function = shader(e, code, entry);
        specialize(e, function, constants);
        return put(std::make_shared<Pipeline>(get<Device>(device), layout(e, schema), push, function, indirect));
    });
}
JNI_METHOD(jlong, createRenderPipeline)
(JNIEnv *e, jobject, jlong device, jbyteArray vs, jstring ve, jbyteArray fs, jstring fe, jintArray schema, jint push,
 jint color, jint depth, jboolean blend) {
    return guard(e, [&] {
        require(push >= 0, "Negative push constant size");
        return put(std::make_shared<Pipeline>(get<Device>(device), layout(e, schema), push, shader(e, vs, ve),
                                              shader(e, fs, fe), static_cast<VkFormat>(color),
                                              static_cast<VkFormat>(depth), blend));
    });
}
JNI_METHOD(jintArray, pipelineLocalSize)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        const auto p = get<Pipeline>(id);
        return intResult(e, {static_cast<jint>(p->localSize[0]), static_cast<jint>(p->localSize[1]),
                             static_cast<jint>(p->localSize[2]), static_cast<jint>(p->pushBytes)});
    });
}
JNI_METHOD(jlong, createCommand)(JNIEnv *e, jobject, jlong id, jint index) {
    return guard(e, [&] {
        require(index >= 0, "Negative queue index");
        return put(std::make_shared<Command>(get<Device>(id), uint32_t(index)));
    });
}
JNI_METHOD(jintArray, queueInfo)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        std::vector<jint> values;
        const auto d = get<Device>(id);
        for (const auto &q : d->queues) {
            const auto &p = q.properties;
            auto flags = p.queueFlags;
            if (!(d->enabledExtra & DataGraph) || !d->extensions->graphQueues.count(q.family))
                flags &= ~VK_QUEUE_DATA_GRAPH_BIT_ARM;
            const auto timestampBits = flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)
                                           ? p.timestampValidBits
                                           : 0;
            values.insert(values.end(),
                          {jint(q.family), jint(q.index), jint(flags), jint(timestampBits),
                           jint(p.minImageTransferGranularity.width), jint(p.minImageTransferGranularity.height),
                           jint(p.minImageTransferGranularity.depth)});
        }
        return intResult(e, values);
    });
}
JNI_METHOD(void, dispatch)
(JNIEnv *e, jobject, jlong id, jlong pipeline, jlongArray bs, jbyteArray constants, jintArray groups) {
    return guard(e, [&] {
        auto g = ints(e, groups);
        require(g.size() == 3 && g[0] > 0 && g[1] > 0 && g[2] > 0, "Invalid dispatch size");
        get<Command>(id)->dispatch(
            {get<Pipeline>(pipeline),
             bindings(e, bs),
             bytes(e, constants),
             {static_cast<uint32_t>(g[0]), static_cast<uint32_t>(g[1]), static_cast<uint32_t>(g[2])}});
    });
}
JNI_METHOD(jlong, beginRender)
(JNIEnv *e, jobject, jlong command, jlong color, jlong depth, jintArray actions, jfloatArray clear) {
    return guard(e, [&] {
        auto c = get<Command>(command);
        c->recording();
        auto pending = std::make_shared<PendingRender>(c);
        auto a = ints(e, actions);
        require(a.size() == 4 && clear && e->GetArrayLength(clear) == 5, "Invalid render pass");
        jfloat values[5];
        e->GetFloatArrayRegion(clear, 0, 5, values);
        if (e->ExceptionCheck())
            throw std::runtime_error("Cannot read JNI clear values");
        auto &p = pending->pass;
        p.color = get<Texture>(color);
        if (depth)
            p.depth = get<Texture>(depth);
        p.colorLoad = static_cast<VkAttachmentLoadOp>(a[0]);
        p.colorStore = static_cast<VkAttachmentStoreOp>(a[1]);
        p.depthLoad = static_cast<VkAttachmentLoadOp>(a[2]);
        p.depthStore = static_cast<VkAttachmentStoreOp>(a[3]);
        p.clearColor = {values[0], values[1], values[2], values[3]};
        p.clearDepth = values[4];
        return put(pending);
    });
}
JNI_METHOD(void, draw)
(JNIEnv *e, jobject, jlong encoder, jlong pipeline, jlongArray bs, jbyteArray constants, jintArray counts) {
    return guard(e, [&] {
        auto pending = get<PendingRender>(encoder);
        pending->command->recording();
        auto n = ints(e, counts);
        require(n.size() == 4 && n[0] > 0 && n[1] > 0 && n[2] >= 0 && n[3] >= 0, "Invalid draw counts");
        pending->pass.draws.push_back({get<Pipeline>(pipeline), bindings(e, bs), bytes(e, constants),
                                       static_cast<uint32_t>(n[0]), static_cast<uint32_t>(n[1]),
                                       static_cast<uint32_t>(n[2]), static_cast<uint32_t>(n[3])});
    });
}
JNI_METHOD(void, endRender)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto p = get<PendingRender>(id);
        require(!p->pass.passLayout || p->subpass + 1 == p->pass.passLayout->subpasses.size(),
                "Encode every subpass before ending the render pass");
        require(!p->perTile, "End per-tile execution before ending the render pass");
        p->command->render(p->pass);
        objects.erase(id);
    });
}
JNI_METHOD(void, copyBuffers)
(JNIEnv *e, jobject, jlong command, jlong src, jlong dst, jlong so, jlong to, jlong length) {
    return guard(e, [&] {
        require(so >= 0 && to >= 0 && length > 0, "Invalid copy range");
        get<Command>(command)->copy(get<Buffer>(src), get<Buffer>(dst), so, to, length);
    });
}
JNI_METHOD(void, copyTexture)
(JNIEnv *e, jobject, jlong command, jlong buffer, jlong texture, jlong offset, jboolean toTexture) {
    return guard(e, [&] {
        require(offset >= 0, "Invalid copy offset");
        get<Command>(command)->copy(get<Buffer>(buffer), get<Texture>(texture), offset, toTexture);
    });
}
JNI_METHOD(void, commit)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] { get<Command>(id)->commit(); });
}
JNI_METHOD(jboolean, waitCommand)(JNIEnv *e, jobject, jlong id, jlong timeout) {
    return guard(e, [&] {
        require(timeout >= 0, "Invalid timeout");
        return get<Command>(id)->wait(timeout == INT64_MAX ? UINT64_MAX : static_cast<uint64_t>(timeout));
    });
}
JNI_METHOD(jint, commandState)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto c = get<Command>(id);
        if (c->state == Command::State::Submitted)
            c->wait(0);
        return static_cast<jint>(c->state);
    });
}
JNI_METHOD(jlong, createSurface)(JNIEnv *e, jobject, jlong device, jobject surface, jint w, jint h) {
    return guard(e, [&]() -> jlong {
#ifdef __ANDROID__
        require(surface && w > 0 && h > 0, "Invalid Android surface");
        auto window = ANativeWindow_fromSurface(e, surface);
        require(window != nullptr, "Android Surface has been released");
        std::unique_ptr<ANativeWindow, decltype(&ANativeWindow_release)> reference(window, ANativeWindow_release);
        return put(std::make_shared<Surface>(get<Device>(device), window, w, h));
#else
        (void)device; (void)surface; (void)w; (void)h;
        throw std::runtime_error("Surface presentation is available only on Android");
#endif
    });
}
JNI_METHOD(void, resizeSurface)(JNIEnv *e, jobject, jlong id, jint w, jint h) {
    return guard(e, [&] {
        require(w > 0 && h > 0, "Invalid surface size");
        get<Surface>(id)->resize(w, h);
    });
}
JNI_METHOD(jintArray, surfaceInfo)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto s = get<Surface>(id);
        return intResult(e, {static_cast<jint>(s->extent.width), static_cast<jint>(s->extent.height), s->format});
    });
}
JNI_METHOD(jlongArray, acquireDrawable)(JNIEnv *e, jobject, jlong id, jlong timeout) {
    return guard(e, [&] {
        require(timeout >= 0, "Invalid acquire timeout");
        auto drawable = get<Surface>(id)->acquire(timeout == INT64_MAX ? UINT64_MAX : static_cast<uint64_t>(timeout));
        if (!drawable)
            return longs(e, {});
        const auto drawableId = put(drawable);
        jlong textureId = 0;
        try {
            textureId = put(drawable->texture);
            return longs(e, {drawableId, textureId, drawable->texture->width, drawable->texture->height,
                             drawable->texture->format});
        } catch (...) {
            objects.erase(textureId);
            objects.erase(drawableId);
            throw;
        }
    });
}
JNI_METHOD(void, present)(JNIEnv *e, jobject, jlong command, jlong drawable) {
    return guard(e, [&] { get<Command>(command)->present(get<Drawable>(drawable)); });
}

#include "jni_generated.inc"
#include "jni_graphics.inc"
#include "jni_graphs.inc"
#include "jni_tensors.inc"
#include "jni_tiles.inc"

#include "jni_ray.inc"

#include "jni_synchronization.inc"

#include "jni_heaps.inc"
#include "jni_interop.inc"
#include "jni_sparse.inc"
