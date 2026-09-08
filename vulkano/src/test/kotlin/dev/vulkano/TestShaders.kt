package dev.vulkano

internal object TestShaders {
    var read: (String) -> ByteArray = { name ->
        checkNotNull(TestShaders::class.java.classLoader!!.getResourceAsStream(name)).use {
            it.readBytes()
        }
    }
}
