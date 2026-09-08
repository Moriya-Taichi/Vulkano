// Included after the shared JNI registry and exception/array helpers.
JNI_METHOD(jlong, createTextureView)
(JNIEnv *e, jobject, jlong id, jint format, jint type, jint mip, jint levels, jint layer, jint layers, jint usage,
 jintArray components) {
    return guard(e, [&] {
        require(mip >= 0 && levels > 0 && layer >= 0 && layers > 0 && type >= 0 && type <= 6, "Invalid view range");
        auto swizzle = ints(e, components);
        require(swizzle.size() == 4 && usage > 0, "Invalid texture view options");
        return put(std::make_shared<Texture>(
            get<Texture>(id), static_cast<VkFormat>(format), static_cast<VkImageViewType>(type), mip, levels, layer,
            layers, usage,
            VkComponentMapping{VkComponentSwizzle(swizzle[0]), VkComponentSwizzle(swizzle[1]),
                               VkComponentSwizzle(swizzle[2]), VkComponentSwizzle(swizzle[3])}));
    });
}
JNI_METHOD(jlong, createGraphics)
(JNIEnv *e, jobject, jlong device, jobjectArray codes, jobjectArray entries, jobjectArray constants, jintArray options,
 jintArray schema, jint push, jboolean mesh, jintArray subpasses, jint subpass) {
    return guard(e, [&] {
        require(codes && entries && constants && push >= 0, "Invalid graphics descriptor");
        int count = e->GetArrayLength(codes);
        require((count == 2 || count == (mesh ? 3 : 4)) && e->GetArrayLength(entries) == count &&
                    e->GetArrayLength(constants) == count,
                "Invalid shader stages");
        std::vector<Shader> shaders;
        for (int n = 0; n < count; ++n) {
            auto code = static_cast<jbyteArray>(e->GetObjectArrayElement(codes, n));
            auto entry = static_cast<jstring>(e->GetObjectArrayElement(entries, n));
            auto spec = static_cast<jintArray>(e->GetObjectArrayElement(constants, n));
            auto s = shader(e, code, entry);
            specialize(e, s, spec);
            shaders.push_back(std::move(s));
            e->DeleteLocalRef(code);
            e->DeleteLocalRef(entry);
            e->DeleteLocalRef(spec);
        }
        auto v = ints(e, options);
        require(v.size() >= 49 && v[13] >= 0 && v[14] >= 0 && v[15] >= 0, "Invalid graphics options");
        require(v.size() == 49 + size_t(v[13]) * 9 + size_t(v[14]) * 3 + size_t(v[15]) * 4,
                "Invalid graphics option lengths");
        GraphicsOptions g;
        require(subpass >= 0, "Invalid subpass index");
        g.passLayout = parseSubpassLayout(ints(e, subpasses));
        g.subpass = subpass;
        g.mesh = mesh;
        g.samples = static_cast<VkSampleCountFlagBits>(v[1]);
        g.topology = static_cast<VkPrimitiveTopology>(v[2]);
        g.cull = v[3];
        g.frontFace = static_cast<VkFrontFace>(v[4]);
        g.polygon = static_cast<VkPolygonMode>(v[5]);
        g.depthWrite = v[6];
        g.depthTest = v[7];
        g.depthCompare = static_cast<VkCompareOp>(v[8]);
        g.depthClamp = v[9];
        g.alphaToCoverage = v[10];
        g.stencilTest = v[11];
        g.patchPoints = v[12];
        auto stencil = [&](int at) {
            return VkStencilOpState{static_cast<VkStencilOp>(v[at]),
                                    static_cast<VkStencilOp>(v[at + 1]),
                                    static_cast<VkStencilOp>(v[at + 2]),
                                    static_cast<VkCompareOp>(v[at + 3]),
                                    uint32_t(v[at + 4]),
                                    uint32_t(v[at + 5]),
                                    uint32_t(v[at + 6])};
        };
        g.front = stencil(16);
        g.back = stencil(23);
        require(v[30] > 0, "Invalid viewport count");
        g.viewportCount = v[30];
        g.viewMask = v[31];
        g.sampleShading = v[32];
        std::memcpy(&g.minSampleShading, &v[33], 4);
        g.alphaToOne = v[34];
        g.sampleMask = {uint32_t(v[35]), uint32_t(v[36])};
        g.primitiveRestart = v[37];
        g.rasterizationDisabled = v[38];
        g.logicEnabled = v[39] >= 0;
        g.logic = g.logicEnabled ? VkLogicOp(v[39]) : VK_LOGIC_OP_COPY;
        g.depthBounds = v[40];
        std::memcpy(&g.minDepthBounds, &v[41], 4);
        std::memcpy(&g.maxDepthBounds, &v[42], 4);
        require(v[43] > 0 && v[44] > 0 && v[45] >= 0, "Invalid fragment shading rate");
        g.fragmentSize = {uint32_t(v[43]), uint32_t(v[44])};
        require(v[46] >= 0 && v[47] >= 0, "Invalid rate map texel size");
        g.rateMapTexelSize = {uint32_t(v[46]), uint32_t(v[47])};
        g.attachmentRateCombiner = VkFragmentShadingRateCombinerOpKHR(v[48]);
        g.primitiveRateCombiner = VkFragmentShadingRateCombinerOpKHR(v[45]);
        size_t at = 49;
        for (int n = 0; n < v[13]; ++n, at += 9) {
            g.colors.push_back(static_cast<VkFormat>(v[at]));
            g.blends.push_back({VkBool32(v[at + 1]), static_cast<VkBlendFactor>(v[at + 2]),
                                static_cast<VkBlendFactor>(v[at + 3]), static_cast<VkBlendOp>(v[at + 4]),
                                static_cast<VkBlendFactor>(v[at + 5]), static_cast<VkBlendFactor>(v[at + 6]),
                                static_cast<VkBlendOp>(v[at + 7]), VkColorComponentFlags(v[at + 8])});
        }
        for (int n = 0; n < v[14]; ++n, at += 3) {
            require(v[at] >= 0 && v[at + 1] >= 0, "Invalid vertex layout");
            g.vertexBindings.push_back(
                {uint32_t(v[at]), uint32_t(v[at + 1]), static_cast<VkVertexInputRate>(v[at + 2])});
        }
        for (int n = 0; n < v[15]; ++n, at += 4) {
            require(v[at] >= 0 && v[at + 1] >= 0 && v[at + 3] >= 0, "Invalid attribute");
            g.attributes.push_back(
                {uint32_t(v[at]), uint32_t(v[at + 1]), static_cast<VkFormat>(v[at + 2]), uint32_t(v[at + 3])});
        }
        if (mesh && count == 3)
            g.task = std::make_shared<Shader>(shaders[2]);
        if (count == 4) {
            g.tessControl = std::make_shared<Shader>(shaders[2]);
            g.tessEvaluation = std::make_shared<Shader>(shaders[3]);
        }
        return put(std::make_shared<Pipeline>(get<Device>(device), layout(e, schema), push, shaders[0], shaders[1],
                                              g.colors.empty() ? VK_FORMAT_UNDEFINED : g.colors[0],
                                              static_cast<VkFormat>(v[0]), false, g));
    });
}
JNI_METHOD(jlong, beginRenderAdvanced)
(JNIEnv *e, jobject, jlong command, jlongArray handles, jintArray options, jfloatArray clear, jintArray subpasses) {
    return guard(e, [&] {
        auto h = longValues(e, handles);
        auto v = ints(e, options);
        auto f = floatValues(e, clear);
        require(h.size() >= 3 && h.size() % 2 == 1, "Invalid attachments");
        size_t count = (h.size() - 3) / 2;
        require(v.size() == 15 + count * 6 && f.size() == 1 + count * 4, "Invalid render descriptor");
        auto c = get<Command>(command);
        c->recording();
        auto pending = std::make_shared<PendingRender>(c);
        auto &p = pending->pass;
        p.passLayout = parseSubpassLayout(ints(e, subpasses));
        if (h[0])
            p.depth = get<Texture>(h[0]);
        p.depthLoad = static_cast<VkAttachmentLoadOp>(v[0]);
        p.depthStore = static_cast<VkAttachmentStoreOp>(v[1]);
        require(v[2] >= 0 && v[3] >= 0, "Invalid depth slice/level");
        p.depthMip = v[2];
        p.depthLayer = v[3];
        p.clearStencil = v[4];
        p.clearDepth = f[0];
        p.viewMask = uint32_t(v[5]);
        require(v[6] > 0, "Invalid attachment layer count");
        p.layers = v[6];
        if (h[1])
            p.depthResolve = get<Texture>(h[1]);
        require(v[9] >= 0 && v[10] >= 0, "Invalid depth resolve subresource");
        p.depthResolveMode = VkResolveModeFlagBits(v[7]);
        p.stencilResolveMode = VkResolveModeFlagBits(v[8]);
        p.depthResolveMip = v[9];
        p.depthResolveLayer = v[10];
        if (h[2])
            p.rateMap = get<Texture>(h[2]);
        require(v[11] >= 0 && v[12] >= 0 && v[13] >= 0 && v[14] >= 0, "Invalid rate map range");
        p.rateMapTexelSize = {uint32_t(v[11]), uint32_t(v[12])};
        p.rateMip = v[13];
        p.rateLayer = v[14];
        for (size_t i = 0; i < count; ++i) {
            size_t n = 15 + i * 6;
            Attachment a;
            a.texture = get<Texture>(h[3 + i * 2]);
            if (h[4 + i * 2])
                a.resolve = get<Texture>(h[4 + i * 2]);
            a.load = static_cast<VkAttachmentLoadOp>(v[n]);
            a.store = static_cast<VkAttachmentStoreOp>(v[n + 1]);
            for (size_t k = 2; k < 6; ++k)
                require(v[n + k] >= 0, "Invalid attachment subresource");
            a.mip = v[n + 2];
            a.layer = v[n + 3];
            a.resolveMip = v[n + 4];
            a.resolveLayer = v[n + 5];
            std::copy_n(f.begin() + 1 + i * 4, 4, a.clear.begin());
            p.colors.push_back(a);
        }
        return put(pending);
    });
}
JNI_METHOD(void, drawAdvanced)
(JNIEnv *e, jobject, jlong encoder, jlong pipeline, jlongArray bs, jbyteArray constants, jintArray counts,
 jlongArray resources, jfloatArray state) {
    return guard(e, [&] {
        auto pending = get<PendingRender>(encoder);
        pending->command->recording();
        auto v = ints(e, counts);
        auto h = longValues(e, resources);
        auto f = floatValues(e, state);
        require(v.size() >= 20 && v[18] >= 0 && v[19] >= 0 && v.size() == 20 + size_t(v[19]) * 4 && h.size() >= 5 &&
                    (h.size() - 5) % 3 == 0 && f.size() == 14 + size_t(v[18]) * 6 && v[0] > 0 && v[1] > 0 &&
                    v[2] >= 0 && v[3] >= 0 && h[1] >= 0 && h[3] >= 0 && v[6] > 0 && v[7] >= 0,
                "Invalid draw descriptor");
        Draw d{get<Pipeline>(pipeline), bindings(e, bs), bytes(e, constants), uint32_t(v[0]),
               uint32_t(v[1]),          uint32_t(v[2]),  uint32_t(v[3])};
        d.baseVertex = v[4];
        require(v[15] >= 0 && v[16] >= 0 && v[17] >= 0, "Invalid mesh group size");
        d.meshGroups = {uint32_t(v[15]), uint32_t(v[16]), uint32_t(v[17])};
        d.indexType = static_cast<VkIndexType>(v[5]);
        d.drawCount = v[6];
        d.stride = v[7];
        d.stencilReference = v[8];
        if (h[0])
            d.indexBuffer = get<Buffer>(h[0]);
        d.indexOffset = h[1];
        if (h[2])
            d.indirect = get<Buffer>(h[2]);
        d.indirectOffset = h[3];
        if (h[4]) {
            d.visibility = get<CounterPool>(h[4]);
            require(v[14] >= 0, "Invalid visibility index");
            d.visibilityIndex = v[14];
        }
        for (size_t i = 5; i < h.size(); i += 3) {
            require(h[i] >= 0 && h[i] <= UINT32_MAX && h[i + 2] >= 0, "Invalid vertex buffer");
            d.vertexBuffers.push_back({uint32_t(h[i]), get<Buffer>(h[i + 1]), VkDeviceSize(h[i + 2])});
        }
        for (int n = 0; n < v[18]; ++n) {
            auto at = 14 + n * 6;
            d.viewports.push_back({f[at], f[at + 1], f[at + 2], f[at + 3], f[at + 4], f[at + 5]});
        }
        for (int n = 0; n < v[19]; ++n) {
            auto at = 20 + n * 4;
            require(v[at] >= 0 && v[at + 1] >= 0 && v[at + 2] >= 0 && v[at + 3] >= 0, "Invalid scissor");
            d.scissors.push_back({{v[at], v[at + 1]}, {uint32_t(v[at + 2]), uint32_t(v[at + 3])}});
        }
        std::copy_n(f.begin() + 6, 4, d.blendColor.begin());
        d.depthBias = f[10];
        d.slopeBias = f[11];
        d.biasClamp = f[12];
        d.lineWidth = f[13];
        d.subpass = pending->subpass;
        pending->pass.draws.push_back(std::move(d));
    });
}
JNI_METHOD(void, dispatchIndirect)
(JNIEnv *e, jobject, jlong command, jlong pipeline, jlongArray bs, jbyteArray constants, jlong buffer, jlong offset) {
    return guard(e, [&] {
        require(offset >= 0, "Invalid indirect offset");
        Dispatch op{get<Pipeline>(pipeline), bindings(e, bs), bytes(e, constants), {1, 1, 1}};
        op.indirect = get<Buffer>(buffer);
        op.indirectOffset = offset;
        get<Command>(command)->dispatch(op);
    });
}
JNI_METHOD(void, copyTextureRegion)
(JNIEnv *e, jobject, jlong command, jlong buffer, jlong texture, jlong offset, jboolean toTexture, jintArray region,
 jint row, jint height) {
    return guard(e, [&] {
        require(offset >= 0 && row >= 0 && height >= 0, "Invalid texture copy");
        get<Command>(command)->copy(get<Buffer>(buffer), get<Texture>(texture), offset, toTexture,
                                    imageRegion(e, region), row, height);
    });
}
JNI_METHOD(void, copyImages)(JNIEnv *e, jobject, jlong command, jlong source, jlong dest, jintArray a, jintArray b) {
    return guard(e, [&] {
        get<Command>(command)->copy(get<Texture>(source), get<Texture>(dest), imageRegion(e, a), imageRegion(e, b));
    });
}
JNI_METHOD(void, generateMipmaps)(JNIEnv *e, jobject, jlong command, jlong texture, jint filter) {
    return guard(e,
                 [&] { get<Command>(command)->generateMipmaps(get<Texture>(texture), static_cast<VkFilter>(filter)); });
}
JNI_METHOD(void, fillBuffer)(JNIEnv *e, jobject, jlong command, jlong buffer, jlong offset, jlong length, jint value) {
    return guard(e, [&] {
        require(offset >= 0 && length > 0, "Invalid buffer fill");
        get<Command>(command)->fill(get<Buffer>(buffer), offset, length, value);
    });
}

JNI_METHOD(jintArray, depthResolveSupport)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto &p = get<Device>(id)->extensions->depthResolveProperties;
        jint values[] = {jint(p.supportedDepthResolveModes), jint(p.supportedStencilResolveModes),
                         jint(p.independentResolve)};
        auto array = e->NewIntArray(3);
        if (!array)
            throw std::bad_alloc();
        e->SetIntArrayRegion(array, 0, 3, values);
        return array;
    });
}

JNI_METHOD(jintArray, fragmentShadingRates)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        const auto &rates = get<Device>(id)->extensions->fragmentRates;
        std::vector<jint> values;
        for (const auto &r : rates) {
            values.push_back(r.fragmentSize.width);
            values.push_back(r.fragmentSize.height);
            values.push_back(r.sampleCounts);
        }
        auto a = e->NewIntArray(values.size());
        if (!a)
            throw std::bad_alloc();
        e->SetIntArrayRegion(a, 0, values.size(), values.data());
        return a;
    });
}

JNI_METHOD(jintArray, rateMapLimits)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto d = get<Device>(id);
        require(d->available & AttachmentRate, "Attachment shading rate is unavailable");
        const auto &p = d->extensions->fragmentRateProperties;
        std::vector<jint> values{int(p.minFragmentShadingRateAttachmentTexelSize.width),
                                 int(p.minFragmentShadingRateAttachmentTexelSize.height),
                                 int(p.maxFragmentShadingRateAttachmentTexelSize.width),
                                 int(p.maxFragmentShadingRateAttachmentTexelSize.height),
                                 int(p.maxFragmentShadingRateAttachmentTexelSizeAspectRatio),
                                 int(p.layeredShadingRateAttachments)};
        auto result = e->NewIntArray(values.size());
        e->SetIntArrayRegion(result, 0, values.size(), values.data());
        return result;
    });
}

JNI_METHOD(void, nextSubpass)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto p = get<PendingRender>(id);
        p->command->recording();
        require(p->pass.passLayout && p->subpass + 1 < p->pass.passLayout->subpasses.size(), "No next subpass");
        ++p->subpass;
    });
}

JNI_METHOD(jlong, referenceBuffer)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] { return put(get<Buffer>(id)); });
}
JNI_METHOD(jintArray, cooperativeMatrixConfigurations)(JNIEnv *e, jobject, jlong id) {
    return guard(e, [&] {
        auto d = get<Device>(id);
        std::vector<jint> values;
        for (const auto &p : d->extensions->matrixConfigurations) {
            // Workgroup-scope matrices require another extension/feature, so do not advertise them here.
            if (p.scope != VK_SCOPE_SUBGROUP_KHR)
                continue;
            values.insert(values.end(), {jint(p.MSize), jint(p.NSize), jint(p.KSize), jint(p.AType), jint(p.BType),
                                         jint(p.CType), jint(p.ResultType), jint(p.saturatingAccumulation)});
        }
        auto result = e->NewIntArray(values.size());
        if (!result)
            throw std::bad_alloc();
        e->SetIntArrayRegion(result, 0, values.size(), values.data());
        return result;
    });
}
