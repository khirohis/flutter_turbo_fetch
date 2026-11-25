package io.github.khirohis.flutter_turbo_fetch

import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.TimeUnit

internal interface CronetTimerHandle {
    fun cancel()
}

internal interface CronetTimerScheduler {
    fun nowMs(): Long = System.nanoTime() / 1_000_000L

    fun schedule(delayMs: Long, task: () -> Unit): CronetTimerHandle

    fun shutdown()
}

internal class ScheduledCronetTimerScheduler : CronetTimerScheduler {
    private var executor: ScheduledExecutorService? = null

    @Synchronized
    private fun ensureExecutor(): ScheduledExecutorService {
        val current = executor
        if (current != null && !current.isShutdown) {
            return current
        }
        val created = ScheduledThreadPoolExecutor(1) { runnable ->
            Thread(runnable, "turbo-fetch-cronet-timer").apply {
                isDaemon = true
            }
        }.apply {
            removeOnCancelPolicy = true
        }
        executor = created
        return created
    }

    override fun schedule(delayMs: Long, task: () -> Unit): CronetTimerHandle {
        val future: ScheduledFuture<*> = ensureExecutor().schedule(task, delayMs, TimeUnit.MILLISECONDS)
        return object : CronetTimerHandle {
            override fun cancel() {
                future.cancel(false)
            }
        }
    }

    @Synchronized
    override fun shutdown() {
        executor?.shutdownNow()
        executor = null
    }
}
