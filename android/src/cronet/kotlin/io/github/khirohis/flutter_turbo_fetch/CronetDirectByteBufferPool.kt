package io.github.khirohis.flutter_turbo_fetch

import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.atomic.AtomicInteger

/**
 * Bounded, lock-free pool of fixed-size direct [ByteBuffer] instances shared across Cronet requests.
 */
internal class CronetDirectByteBufferPool(
    private val bufferSize: Int,
    private val maxPooled: Int
) {
    private val pool = ConcurrentLinkedQueue<ByteBuffer>()
    private val pooledCount = AtomicInteger(0)

    val size: Int
        get() = pooledCount.get()

    fun acquire(): ByteBuffer {
        val pooled = pool.poll()
        if (pooled != null) {
            pooledCount.decrementAndGet()
            pooled.clear()
            return pooled
        }
        return ByteBuffer.allocateDirect(bufferSize)
    }

    fun release(buffer: ByteBuffer) {
        buffer.clear()
        if (pooledCount.incrementAndGet() <= maxPooled) {
            pool.offer(buffer)
        } else {
            pooledCount.decrementAndGet()
        }
    }

    fun clear() {
        while (pool.poll() != null) {
            pooledCount.decrementAndGet()
        }
    }
}
