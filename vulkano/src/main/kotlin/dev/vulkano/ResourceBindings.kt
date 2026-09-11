package dev.vulkano

import dev.vulkano.internal.Native

/** A reusable descriptor set. Updates affect future draws/dispatches, never recorded work. */
class ResourceBindings internal constructor(device: Device, id: Long, val set: Int) : Resource(device, id) {
    /** Atomically patch this group. An exception leaves all previous bindings intact. */
    fun update(block: ResourceBindingUpdate.() -> Unit): Unit = access { id ->
        val changes = ResourceBindingUpdate(device, set)
        try {
            changes.block()
            Native.updateResourceBindings(id, changes.data(), changes.removed(), changes.replaceAll)
        } finally { changes.finish() }
    }
}

fun Device.makeResourceBindings(set: Int = 0, block: ResourceBindingUpdate.() -> Unit = {}): ResourceBindings = access {
    require(set in 0 until capabilities.limits.maxBoundDescriptorSets)
    val changes = ResourceBindingUpdate(this, set)
    try {
        changes.block()
        ResourceBindings(this, Native.createResourceBindings(nativeHandle, set, changes.data()), set)
    } finally { changes.finish() }
}

/** A transaction for a single descriptor set; fixed arrays use arrayElement. */
class ResourceBindingUpdate internal constructor(private val device: Device, private val set: Int) {
    private val bindings = sortedMapOf<Triple<Int, Int, Int>, BoundResource>(
        compareBy<Triple<Int, Int, Int>> { it.first }.thenBy { it.second }.thenBy { it.third })
    private val removals = mutableSetOf<Pair<Int, Int>>()
    private var active = true
    internal var replaceAll = false
        private set
    private fun edit(block: () -> Unit) { check(active) { "Binding update has ended" }; block() }
    internal fun finish() { active = false }
    internal fun data(): LongArray = bindings.values.flatMap { it.pack() }.toLongArray()
    internal fun removed(): LongArray = removals.flatMap { listOf(it.first.toLong(), it.second.toLong()) }.toLongArray()

    fun clear(): Unit = edit { bindings.clear(); removals.clear(); replaceAll = true }
    fun remove(index: Int, arrayElement: Int = 0): Unit = edit {
        require(index >= 0 && arrayElement >= 0)
        bindings.remove(Triple(set, index, arrayElement))
        removals.add(index to arrayElement)
    }

    fun setBuffer(
        buffer: Buffer,
        index: Int,
        offset: Long = 0,
        length: Long = buffer.length - offset,
        arrayElement: Int = 0,
    ): Unit = edit {
        require(
            buffer.device === device &&
                index >= 0 &&
                offset >= 0 &&
                length > 0 &&
                offset <= buffer.length &&
                length <= buffer.length - offset
        )
        buffer.handle()
        require(arrayElement >= 0 && set >= 0)
        bindings[Triple(set, index, arrayElement)] =
            BoundResource(index, buffer = buffer, offset = offset, length = length, set = set, arrayElement = arrayElement)
    }

    fun setTexture(
        texture: Texture,
        index: Int,
        sampler: Sampler? = null,
        arrayElement: Int = 0,
    ): Unit = edit {
        require(
            texture.device === device &&
                index >= 0 &&
                (sampler == null || sampler.device === device)
        )
        texture.handle()
        sampler?.handle()
        require(arrayElement >= 0 && set >= 0)
        bindings[Triple(set, index, arrayElement)] =
            BoundResource(index, set = set, texture = texture, sampler = sampler, arrayElement = arrayElement)
    }

    fun setSampler(sampler: Sampler, index: Int, arrayElement: Int = 0): Unit = edit {
        require(sampler.device === device && index >= 0 && arrayElement >= 0 && set >= 0)
        sampler.handle()
        bindings[Triple(set, index, arrayElement)] =
            BoundResource(index, set = set, sampler = sampler, arrayElement = arrayElement)
    }

    fun setTextureBuffer(texture: TextureBuffer, index: Int, arrayElement: Int = 0): Unit = edit {
        require(texture.device === device && index >= 0 && arrayElement >= 0 && set >= 0)
        texture.handle()
        bindings[Triple(set, index, arrayElement)] =
            BoundResource(index, set = set, texel = texture, arrayElement = arrayElement)
    }

    fun setTensor(tensor: TensorView, index: Int, arrayElement: Int = 0): Unit = edit {
        require(tensor.device === device && index >= 0 && arrayElement >= 0 && set >= 0)
        tensor.handle()
        bindings[Triple(set, index, arrayElement)] =
            BoundResource(index, set = set, tensor = tensor, arrayElement = arrayElement)
    }

    fun setAccelerationStructure(
        structure: AccelerationStructure,
        index: Int,
        arrayElement: Int = 0,
    ): Unit = edit {
        require(structure.device === device && index >= 0 && arrayElement >= 0 && set >= 0)
        structure.handle()
        bindings[Triple(set, index, arrayElement)] =
            BoundResource(index, set = set, arrayElement = arrayElement, acceleration = structure)
    }

}
